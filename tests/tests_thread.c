#define _GNU_SOURCE

#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>

#include <linux/prctl.h>
#include <sys/prctl.h>

#include "../src/multiwd_config.h"
#include "../src/multiwd.h"

static const char *current_test_name = NULL;

static void fail_impl(const char *file, int line, const char *fmt, ...)
{
    va_list args;

    fprintf(stderr, "[FAIL] %s:%d", file, line);
    if(current_test_name != NULL){
        fprintf(stderr, " [%s]", current_test_name);
    }
    fprintf(stderr, ": ");

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
    fflush(stderr);
    abort();
}

#define TEST_ASSERT(cond, fmt, ...) \
    do { \
        if(!(cond)){ \
            fail_impl(__FILE__, __LINE__, fmt, ##__VA_ARGS__); \
        } \
    } while(0)

static void flush_coverage(void)
{
    return;
}

static void reraising_signal_handler(int sig)
{
    flush_coverage();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void setup_signal_passthrough(int sig)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reraising_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    TEST_ASSERT(sigaction(sig, &sa, NULL) == 0, "sigaction failed: errno=%d", errno);
}

void init_fun(void)
{
    int r = multiwd_init(MULTIWD_DEFAULT, SIGTERM);
    TEST_ASSERT(r == 0, "init failed: ret=%d errno=%d", r, errno);
}

void fini_fun(void)
{
    errno = 0;
    int r = multiwd_shutdown();
    TEST_ASSERT(r == -1, "shutdown succeeded when it shouldnt: ret=%d", r);
    TEST_ASSERT(errno == ENOTSUP, "shutdown incorrect errno: errno=%d", errno);
}

static void wait_for_pid(pid_t pid, int *status, const char *what)
{
    pid_t waited;

    do {
        waited = waitpid(pid, status, 0);
    } while(waited == -1 && errno == EINTR);

    TEST_ASSERT(waited == pid, "%s waitpid failed: pid=%jd ret=%jd errno=%d", what, (intmax_t)pid, (intmax_t)waited, errno);
}

static pid_t wait_for_any_child(int *status, const char *what)
{
    pid_t waited;

    do {
        waited = waitpid(-1, status, 0);
    } while(waited == -1 && errno == EINTR);

    TEST_ASSERT(waited >= 0, "%s waitpid(-1) failed: ret=%jd errno=%d", what, (intmax_t)waited, errno);
    return waited;
}

static void assert_exit_status(int status, const char *what)
{
    TEST_ASSERT(WIFEXITED(status), "%s did not exit normally: status=%d", what, status);
}

static void assert_signal_status(int status, int expected_signal, const char *what)
{
    TEST_ASSERT(WIFSIGNALED(status), "%s was not terminated by signal: status=%d", what, status);
    TEST_ASSERT(WTERMSIG(status) == expected_signal,
        "%s terminated with signal %d instead of %d",
        what,
        WTERMSIG(status),
        expected_signal);
}

typedef void (*test_fn_t)(void);

static void run_test_case(const char *name, bool call_init, bool call_fini, test_fn_t fn)
{
    current_test_name = name;
    printf("[RUN ] %s\n", name);
    fflush(stdout);

    if(call_init){
        init_fun();
    }

    fn();

    if(call_fini){
        fini_fun();
    }

    flush_coverage();
    printf("[ OK ] %s\n", name);
    fflush(stdout);
}

static void run_signaled_test_case(const char *name, bool call_init, int expected_signal, test_fn_t fn)
{
    fflush(stdout);
    fflush(stderr);

    current_test_name = name;
    printf("[RUN ] %s\n", name);
    fflush(stdout);

    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork failed: errno=%d", errno);

    if(pid == 0){
        current_test_name = name;
        setup_signal_passthrough(expected_signal);

        if(call_init){
            init_fun();
        }

        fn();
        fail_impl(__FILE__, __LINE__, "expected signal %d but test returned", expected_signal);
    }

    int status = 0;
    wait_for_pid(pid, &status, name);
    assert_signal_status(status, expected_signal, name);

    flush_coverage();
    printf("[ OK ] %s (signal %d)\n", name, expected_signal);
    fflush(stdout);
}

/* cleanup_multiple */
static void test_cleanup_multiple(void)
{
    const struct timespec t = { .tv_sec = 30, .tv_nsec = 3 };

    for(int i = 0; i < 10; ++i){
        int r = multiwd_add((uint64_t)i, &t);
        TEST_ASSERT(r == 0, "multiwd_add failed: id=%d ret=%d errno=%d", i, r, errno);
    }

    int r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);
}

/* cleanup_max */
static void test_cleanup_max(void)
{
    const struct timespec t = { .tv_sec = 300, .tv_nsec = 3 };

    for(uint64_t i = 0; i < MULTIWD_MAX_TIMERS; ++i){
        int r = multiwd_add(i, &t);
        TEST_ASSERT(r == 0, "multiwd_add failed: id=%" PRIu64 " ret=%d errno=%d", i, r, errno);
    }

    int r = multiwd_add(MULTIWD_MAX_TIMERS, &t);
    TEST_ASSERT(r < 0, "multiwd_add should fail for max id: ret=%d errno=%d", r, errno);

    r = multiwd_add(MULTIWD_MAX_TIMERS + 1u, &t);
    TEST_ASSERT(r < 0, "multiwd_add should fail past max id: ret=%d errno=%d", r, errno);

    r = multiwd_add(0, &t);
    TEST_ASSERT(r != 0, "duplicate multiwd_add unexpectedly succeeded: ret=%d", r);

    r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);
}

/* timer_expiration */
static void test_timer_expiration(void)
{
    const struct timespec t = { .tv_sec = 3, .tv_nsec = 3 };

    int r = multiwd_add(0, &t);
    TEST_ASSERT(r == 0, "multiwd_add failed: ret=%d errno=%d", r, errno);

    sleep(5);
    TEST_ASSERT(false, "timer did not expire");
}

/* timer_expiration_multiple */
static void test_timer_expiration_multiple(void)
{
    const struct timespec t = { .tv_sec = 3, .tv_nsec = 3 };

    for(uint64_t i = 0; i < 10; ++i){
        int r = multiwd_add(i, &t);
        TEST_ASSERT(r == 0, "multiwd_add failed: id=%" PRIu64 " ret=%d errno=%d", i, r, errno);
    }

    sleep(5);
    TEST_ASSERT(false, "timers did not expire");
}

/* timer_invalid */
static void test_timer_invalid(void)
{
    const struct timespec t = { .tv_sec = 3, .tv_nsec = 3 };

    int r = multiwd_add(UINT64_MAX, &t);
    TEST_ASSERT(r != 0, "multiwd_add should fail for UINT64_MAX: ret=%d", r);

    r = multiwd_add(2, NULL);
    TEST_ASSERT(r != 0, "multiwd_add should fail for NULL timeout: ret=%d", r);

    r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);
}

/* kick_invalid */
static void test_kick_invalid(void)
{
    int r = multiwd_kick(1024, NULL);
    TEST_ASSERT(r != 0, "multiwd_kick should fail for out of range id: ret=%d", r);

    r = multiwd_kick_minimal(1024);
    TEST_ASSERT(r != 0, "multiwd_kick_minimal should fail for out of range id: ret=%d", r);

    r = multiwd_kick_multiple(0, 100);
    TEST_ASSERT(r != 0, "multiwd_kick_multiple should fail for nonexistent timers: ret=%d", r);

    r = multiwd_kick_multiple(5, 2);
    TEST_ASSERT(r != 0, "multiwd_kick_multiple should fail for invalid offset: ret=%d", r);

    r = multiwd_kick_multiple(0, 0);
    TEST_ASSERT(r != 0, "multiwd_kick_multiple should fail for empty bitmap: ret=%d", r);

    r = multiwd_kick_multiple_minimal(0, 100);
    TEST_ASSERT(r != 0, "multiwd_kick_multiple_minimal should fail for nonexistent timers: ret=%d", r);

    r = multiwd_kick_multiple_minimal(0, 0);
    TEST_ASSERT(r != 0, "multiwd_kick_multiple_minimal should fail for empty bitmap: ret=%d", r);

    r = multiwd_kick_multiple_minimal(5, 2);
    TEST_ASSERT(r != 0, "multiwd_kick_multiple_minimal should fail for invalid offset: ret=%d", r);

    r = multiwd_kick_multiple(0, 1u << 3);
    TEST_ASSERT(r != 0, "multiwd_kick_multiple should fail for nonexistent timer: ret=%d", r);

    r = multiwd_kick_multiple_minimal(0, 1u << 3);
    TEST_ASSERT(r != 0, "multiwd_kick_multiple_minimal should fail for nonexistent timer: ret=%d", r);

    r = multiwd_kick_minimal(0);
    TEST_ASSERT(r != 0, "multiwd_kick_minimal should fail for nonexistent timer: ret=%d", r);

    r = multiwd_kick(5, NULL);
    TEST_ASSERT(r != 0, "multiwd_kick should fail for nonexistent timer: ret=%d", r);

    r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);
}

/* remove_invalid */
static void test_remove_invalid(void)
{
    int r = multiwd_remove(1024, NULL);
    TEST_ASSERT(r != 0, "multiwd_remove should fail for out of range id: ret=%d", r);

    r = multiwd_remove(3, NULL);
    TEST_ASSERT(r != 0, "multiwd_remove should fail for nonexistent timer: ret=%d", r);

    r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);
}

/* read_kick_and_remove */
static void test_read_kick_and_remove(void)
{
    const struct timespec t = { .tv_sec = 30, .tv_nsec = 3 };
    struct timespec t2;
    struct timespec t3;

    int r = multiwd_add(1, &t);
    TEST_ASSERT(r == 0, "multiwd_add failed: ret=%d errno=%d", r, errno);

    sleep(5);
    r = multiwd_kick(1, &t2);
    TEST_ASSERT(r == 0, "multiwd_kick failed: ret=%d errno=%d", r, errno);

    sleep(5);
    r = multiwd_remove(1, &t3);
    TEST_ASSERT(r == 0, "multiwd_remove failed: ret=%d errno=%d", r, errno);

    r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);

    TEST_ASSERT(memcmp(&t, &t3, sizeof(t3)) == 0, "removed timeout does not match configured timeout");
    TEST_ASSERT(t2.tv_sec < t.tv_sec, "remaining timeout was not reduced: remaining=%jd configured=%jd",
        (intmax_t)t2.tv_sec,
        (intmax_t)t.tv_sec);
}

/* kicks_normal */
static void test_kicks_normal(void)
{
    const struct timespec t = { .tv_sec = 4, .tv_nsec = 3 };

    int r = multiwd_add(1, &t);
    TEST_ASSERT(r == 0, "multiwd_add failed: ret=%d errno=%d", r, errno);

    sleep(2);
    r = multiwd_kick(1, NULL);
    TEST_ASSERT(r == 0, "multiwd_kick failed: ret=%d errno=%d", r, errno);

    sleep(2);
    r = multiwd_kick_minimal(1);
    TEST_ASSERT(r == 0, "multiwd_kick_minimal failed: ret=%d errno=%d", r, errno);

    sleep(2);
    r = multiwd_kick_multiple(0, 2);
    TEST_ASSERT(r == 0, "multiwd_kick_multiple failed: ret=%d errno=%d", r, errno);

    sleep(2);
    r = multiwd_kick_multiple_minimal(0, 2);
    TEST_ASSERT(r == 0, "multiwd_kick_multiple_minimal failed: ret=%d errno=%d", r, errno);

    sleep(2);
    r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);
}

/* uninitialised */
static void test_uninitialised(void)
{
    const struct timespec t = { .tv_sec = 30, .tv_nsec = 3 };

    int r = multiwd_add(0, &t);
    TEST_ASSERT(r != 0, "multiwd_add should fail before init: ret=%d", r);

    r = multiwd_add3(0, &t, SIGTERM);
    TEST_ASSERT(r != 0, "multiwd_add3 should fail before init: ret=%d", r);

    r = multiwd_kick(0, NULL);
    TEST_ASSERT(r != 0, "multiwd_kick should fail before init: ret=%d", r);

    r = multiwd_kick_minimal(0);
    TEST_ASSERT(r != 0, "multiwd_kick_minimal should fail before init: ret=%d", r);

    r = multiwd_kick_multiple(0, 0);
    TEST_ASSERT(r != 0, "multiwd_kick_multiple should fail before init: ret=%d", r);

    r = multiwd_kick_multiple_minimal(0, 0);
    TEST_ASSERT(r != 0, "multiwd_kick_multiple_minimal should fail before init: ret=%d", r);

    r = multiwd_remove(0, NULL);
    TEST_ASSERT(r != 0, "multiwd_remove should fail before init: ret=%d", r);

    r = multiwd_register_child(getppid());
    TEST_ASSERT(r != 0, "multiwd_register_child should fail before init: ret=%d", r);

    r = multiwd_register_child2(getppid(), SIGTERM);
    TEST_ASSERT(r != 0, "multiwd_register_child2 should fail before init: ret=%d", r);

    pid_t pid = multiwd_fork();
    TEST_ASSERT(pid >= 0, "multiwd_fork failed unexpectedly: pid=%jd errno=%d", (intmax_t)pid, errno);

    if(pid == 0){
        _exit(0);
    }

    int status = 0;
    wait_for_pid(pid, &status, "uninitialised child");
    assert_exit_status(status, "uninitialised child");

    r = multiwd_shutdown();
    TEST_ASSERT(r != 0, "multiwd_shutdown should fail before init: ret=%d", r);
}

/* register_child_invalid */
static void test_register_child_invalid(void)
{
    int r = multiwd_register_child(0);
    TEST_ASSERT(r < 0, "pid 0 should be invalid: ret=%d", r);

    r = multiwd_register_child(-1);
    TEST_ASSERT(r < 0, "pid -1 should be invalid: ret=%d", r);

    r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);
}

/* register_child_max */
static void test_register_child_max(void)
{
    for(int i = 0; i < MULTIWD_MAX_CHILDREN; ++i){
        pid_t pid = multiwd_fork();
        TEST_ASSERT(pid >= 0, "multiwd_fork failed: errno=%d", errno);

        if(pid == 0){
            sleep(5);
            _exit(i & 0xff);
        }
    }

    pid_t pid = multiwd_fork();
    TEST_ASSERT(pid < 0, "multiwd_fork should fail after reaching child limit: pid=%jd", (intmax_t)pid);

    for(int i = 0; i < MULTIWD_MAX_CHILDREN; ++i){
        int status = 0;
        pid_t waited = wait_for_any_child(&status, "register_child_max");
        (void)waited;
        assert_exit_status(status, "register_child_max child");
    }

    int r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);
}

/* register_child */
static void test_register_child(void)
{
    pid_t pid = multiwd_fork();
    TEST_ASSERT(pid >= 0, "multiwd_fork failed: errno=%d", errno);

    if(pid == 0){
        sleep(5);
        int r = multiwd_shutdown();
        TEST_ASSERT(r == 0, "child multiwd_shutdown failed: ret=%d errno=%d", r, errno);
        _exit(0);
    }

    int status = 0;
    wait_for_pid(pid, &status, "register_child child");
    assert_exit_status(status, "register_child child");

    int r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "shutdown failed: ret=%d errno=%d", r, errno);
}

/* register_child_multiple */
static void test_register_child_multiple(void)
{
    enum { test_cnt = 2 };

    for(int i = 0; i < test_cnt; ++i){
        pid_t pid = multiwd_fork();
        TEST_ASSERT(pid >= 0, "multiwd_fork failed: errno=%d", errno);

        if(pid == 0){
            sleep(5);
            int r = multiwd_shutdown();
            TEST_ASSERT(r == 0, "child multiwd_shutdown failed: ret=%d errno=%d", r, errno);
            _exit(i & 0xff);
        }
    }

    for(int i = 0; i < test_cnt; ++i){
        int status = 0;
        pid_t waited = wait_for_any_child(&status, "register_child_multiple");
        (void)waited;
        assert_exit_status(status, "register_child_multiple child");
    }

    int r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);
}

/* child_work_after_parent_exit */
static void test_child_work_after_parent_exit(void)
{
    enum { test_cnt = 10 };
    const struct timespec t = { .tv_sec = 10, .tv_nsec = 3 };

    for(int i = 0; i < test_cnt; ++i){
        pid_t pid = multiwd_fork();
        TEST_ASSERT(pid >= 0, "multiwd_fork failed: errno=%d", errno);

        if(pid == 0){
            int r = multiwd_add(0, &t);
            TEST_ASSERT(r == 0, "child multiwd_add failed: ret=%d errno=%d", r, errno);

            for(int kick = 0; kick < 6; ++kick){
                sleep(3);
                r = multiwd_kick(0, NULL);
                TEST_ASSERT(r == 0, "child multiwd_kick failed: ret=%d errno=%d", r, errno);
            }

            r = multiwd_shutdown();
            TEST_ASSERT(r == 0, "child multiwd_shutdown failed: ret=%d errno=%d", r, errno);
            _exit(i & 0xff);
        }
    }

    int r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "parent multiwd_shutdown failed: ret=%d errno=%d", r, errno);

    for(int i = 0; i < test_cnt; ++i){
        int status = 0;
        pid_t waited = wait_for_any_child(&status, "child_work_after_parent_exit");
        (void)waited;
        assert_exit_status(status, "child_work_after_parent_exit child");
    }
}

/* register_child_early_exit */
static void test_register_child_early_exit(void)
{
    enum { test_cnt = 10 };

    for(int i = 0; i < test_cnt; ++i){
        pid_t pid = multiwd_fork();
        TEST_ASSERT(pid >= 0, "multiwd_fork failed: errno=%d", errno);

        if(pid == 0){
            int r = multiwd_shutdown();
            TEST_ASSERT(r == 0, "child multiwd_shutdown failed: ret=%d errno=%d", r, errno);
            _exit(i & 0xff);
        }
    }

    int r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "parent multiwd_shutdown failed: ret=%d errno=%d", r, errno);

    for(int i = 0; i < test_cnt; ++i){
        int status = 0;
        pid_t waited = wait_for_any_child(&status, "register_child_early_exit");
        (void)waited;
        assert_exit_status(status, "register_child_early_exit child");
    }
}

/* register_child_branching */
static void test_register_child_branching(void)
{
    enum { test_cnt = 2 };

    for(int i = 0; i < test_cnt; ++i){
        pid_t pid = multiwd_fork();
        TEST_ASSERT(pid >= 0, "multiwd_fork failed: errno=%d", errno);

        if(pid == 0){
            pid_t pid2 = multiwd_fork();
            TEST_ASSERT(pid2 >= 0, "nested multiwd_fork failed: errno=%d", errno);

            if(pid2 == 0){
                sleep(5);
                int r = multiwd_shutdown();
                TEST_ASSERT(r == 0, "grandchild multiwd_shutdown failed: ret=%d errno=%d", r, errno);
                _exit((-i) & 0xff);
            }

            sleep(5);
            int r = multiwd_shutdown();
            TEST_ASSERT(r == 0, "child multiwd_shutdown failed: ret=%d errno=%d", r, errno);

            int status = 0;
            wait_for_pid(pid2, &status, "register_child_branching grandchild");
            assert_exit_status(status, "register_child_branching grandchild");
            _exit(i & 0xff);
        }
    }

    for(int i = 0; i < test_cnt; ++i){
        int status = 0;
        pid_t waited = wait_for_any_child(&status, "register_child_branching");
        (void)waited;
        assert_exit_status(status, "register_child_branching child");
    }

    int r = multiwd_shutdown();
    TEST_ASSERT(r == 0, "multiwd_shutdown failed: ret=%d errno=%d", r, errno);
}

/* child_signal_kills_parent */
static void test_child_signal_kills_parent(void)
{
    pid_t pid = multiwd_fork();
    TEST_ASSERT(pid >= 0, "multiwd_fork failed: errno=%d", errno);

    if(pid == 0){
        const struct timespec t = { .tv_sec = 3, .tv_nsec = 0 };
        int r = multiwd_add(0, &t);
        TEST_ASSERT(r == 0, "child multiwd_add failed: ret=%d errno=%d", r, errno);
        sleep(10);
        TEST_ASSERT(false, "child survived watchdog expiration");
    }

    sleep(30);
    TEST_ASSERT(false, "parent survived child watchdog signal propagation");
}

/* parent_signal_kills_child */
static void test_parent_signal_kills_child(void)
{
    int pr = prctl(PR_SET_CHILD_SUBREAPER, 1);
    TEST_ASSERT(pr == 0, "prctl(PR_SET_CHILD_SUBREAPER) failed: ret=%d errno=%d", pr, errno);

    pid_t parent_pid = fork();
    TEST_ASSERT(parent_pid >= 0, "Initial fork failed: errno=%d", errno);

    if(parent_pid == 0){
        int r = multiwd_init(0, SIGTERM);
        TEST_ASSERT(r == 0, "mock parent init failed: ret=%d errno=%d", r, errno);

        pid_t child_pid = multiwd_fork();
        TEST_ASSERT(child_pid >= 0, "mock parent multiwd_fork failed: errno=%d", errno);

        if(child_pid == 0){
            sleep(100);
            TEST_ASSERT(false, "child survived parent watchdog signal");
        }

        const struct timespec t = { .tv_sec = 3, .tv_nsec = 0 };
        r = multiwd_add(0, &t);
        TEST_ASSERT(r == 0, "mock parent multiwd_add failed: ret=%d errno=%d", r, errno);
        sleep(5);
        TEST_ASSERT(false, "mock parent survived watchdog expiration");
    }

    int status_parent = 0;
    wait_for_pid(parent_pid, &status_parent, "mock parent");
    assert_signal_status(status_parent, SIGTERM, "mock parent");

    int status_child = 0;
    pid_t cleaned_child = wait_for_any_child(&status_child, "orphaned child");
    (void)cleaned_child;
    assert_signal_status(status_child, SIGTERM, "orphaned child");
}

int main(void)
{
    run_test_case("cleanup_multiple", true, true, test_cleanup_multiple);
    run_test_case("cleanup_max", true, true, test_cleanup_max);
    run_signaled_test_case("timer_expiration", true, SIGTERM, test_timer_expiration);
    run_signaled_test_case("timer_expiration_multiple", true, SIGTERM, test_timer_expiration_multiple);
    run_test_case("timer_invalid", true, true, test_timer_invalid);
    run_test_case("kick_invalid", true, true, test_kick_invalid);
    run_test_case("remove_invalid", true, true, test_remove_invalid);
    run_test_case("read_kick_and_remove", true, true, test_read_kick_and_remove);
    run_test_case("kicks_normal", true, true, test_kicks_normal);
    run_test_case("uninitialised", false, true, test_uninitialised);
    run_test_case("register_child_invalid", true, true, test_register_child_invalid);
    run_test_case("register_child_max", true, true, test_register_child_max);
    run_test_case("register_child", true, true, test_register_child);
    run_test_case("register_child_multiple", true, true, test_register_child_multiple);
    run_test_case("child_work_after_parent_exit", true, true, test_child_work_after_parent_exit);
    run_test_case("register_child_early_exit", true, true, test_register_child_early_exit);
    run_test_case("register_child_branching", true, true, test_register_child_branching);
    run_signaled_test_case("child_signal_kills_parent", true, SIGTERM, test_child_signal_kills_parent);
    run_test_case("parent_signal_kills_child", false, true, test_parent_signal_kills_child);

    printf("[DONE] all thread tests passed\n");
    return 0;
}


