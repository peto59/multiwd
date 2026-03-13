#define _GNU_SOURCE

#include <criterion/criterion.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>

#include <linux/prctl.h>  /* Definition of PR_* constants */
#include <sys/prctl.h>

#include "../src/multiwd_config.h"
#include "../src/multiwd.h"

void init_fun(void)
{
    int r = multiwd_init(MULTIWD_DEFAULT, SIGTERM);
    cr_assert_eq(r, 0, "init failed");
}

void fini_fun(void)
{
    int r = multiwd_shutdown();
    cr_assert_eq(r, -1, "shutdown succeeded when it shouldnt");
    cr_assert_eq(errno, ENOTSUP, "shutdown incorrect errno");
}

/* LLVM coverage flush function */
extern int __llvm_profile_write_file(void);

/* Global storage for the original SIGTERM handler */
static struct sigaction old_sigterm_sa;

/* Coverage + Criterion-safe SIGTERM handler */
static void coverage_sigterm_handler(int sig)
{
    /* 1. Flush LLVM coverage */
    __llvm_profile_write_file();

    /* 2. Call Criterion’s original handler, if any */
    if (old_sigterm_sa.sa_handler &&
        old_sigterm_sa.sa_handler != SIG_DFL &&
        old_sigterm_sa.sa_handler != SIG_IGN)
    {
        old_sigterm_sa.sa_handler(sig);
    }
    else
    {
        /* 3. Default termination if no handler */
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

/* Install handler while preserving Criterion’s original SIGTERM handler */
static void setup_coverage_signal_handler(void)
{
    return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = coverage_sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    /* Save the old handler (Criterion or default) */
    sigaction(SIGTERM, &sa, &old_sigterm_sa);
}


Test(multiwd, cleanup_multiple, .init = init_fun, .fini = fini_fun, .disabled = false)
{
    const struct timespec t = { .tv_sec = 30, .tv_nsec = 3 };
    int r;
    for(int i = 0; i < 10; i++){
        int r = multiwd_add(i, &t);
        cr_assert_eq(r, 0, "ret=%d", r);
    }
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, cleanup_max, .init = init_fun, .fini = fini_fun, .disabled = false)
{
    int r;
    const struct timespec t = { .tv_sec = 300, .tv_nsec = 3 };
    for(uint64_t i = 0; i < MULTIWD_MAX_TIMERS; i++){
        r = multiwd_add(i, &t);
        cr_assert_eq(r, 0, "ret=%d", r);
    }
    r = multiwd_add(MULTIWD_MAX_TIMERS, &t);
    cr_assert_lt(r, 0, "ret=%d", r);
    r = multiwd_add(MULTIWD_MAX_TIMERS + 1, &t);
    cr_assert_lt(r, 0, "ret=%d", r);
    r = multiwd_add(0, &t);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, timer_expiration, .init = init_fun, .fini = fini_fun, .signal = SIGTERM, .disabled = false )
{
    setup_coverage_signal_handler();
    const struct timespec t = { .tv_sec = 3, .tv_nsec = 3 };
    int r = multiwd_add(0, &t);
    cr_assert_eq(r, 0, "ret=%d", r);

    sleep(5);
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, timer_expiration_multiple, .init = init_fun, .fini = fini_fun, .signal = SIGTERM, .disabled = false )
{
    setup_coverage_signal_handler();
    const struct timespec t = { .tv_sec = 3, .tv_nsec = 3 };
    int r;
    for(uint64_t i = 0; i < 10; i++){
        int r = multiwd_add(i, &t);
        cr_assert_eq(r, 0, "ret=%d", r);
    }

    sleep(5);
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, timer_invalid, .init = init_fun, .fini = fini_fun, .disabled = false)
{
    const struct timespec t = { .tv_sec = 3, .tv_nsec = 3 };
    int r;
    r = multiwd_add(-1, &t);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_add(2, NULL);
    cr_assert_neq(r, 0, "ret=%d", r);

    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, kick_invalid, .init = init_fun, .fini = fini_fun, .disabled = false)
{
    int r;
    //out of range
    r = multiwd_kick(1024, NULL);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_kick_minimal(1024);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_kick_multiple(0, 100);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_kick_multiple(5, 2);
    cr_assert_neq(r, 0, "ret=%d", r); 
    r = multiwd_kick_multiple(0, 0);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_kick_multiple_minimal(0, 100);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_kick_multiple_minimal(0, 0);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_kick_multiple_minimal(5, 2);
    cr_assert_neq(r, 0, "ret=%d", r);

    //nonexistent
    r = multiwd_kick_multiple(0, 1 << 3);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_kick_multiple_minimal(0, 1 << 3);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_kick_minimal(0);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_kick(5, NULL);
    cr_assert_neq(r, 0, "ret=%d", r);

    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}


Test(multiwd, remove_invalid, .init = init_fun, .fini = fini_fun, .disabled = false)
{
    int r;
    r = multiwd_remove(1024, NULL);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_remove(3, NULL);
    cr_assert_neq(r, 0, "ret=%d", r);
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, read_kick_and_remove, .init = init_fun, .fini = fini_fun, .disabled = false)
{
    const struct timespec t = { .tv_sec = 30, .tv_nsec = 3 };
    struct timespec t2, t3;
    int r;
    r = multiwd_add(1, &t);
    cr_assert_eq(r, 0, "ret=%d", r);
    sleep(5);
    r = multiwd_kick(1, &t2);
    cr_assert_eq(r, 0, "ret=%d", r);
    sleep(5);
    r = multiwd_remove(1, &t3);
    cr_assert_eq(r, 0, "ret=%d", r);
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
    cr_assert_eq(memcmp(&t, &t3, sizeof(t3)), 0, "wrong timespec");
    cr_assert_lt(t2.tv_sec, t.tv_sec, "wrong timespec");
}

Test(multiwd, kicks_normal, .init = init_fun, .fini = fini_fun, .timeout = 30, .disabled = false)
{

    const struct timespec t = { .tv_sec = 4, .tv_nsec = 3 };
    int r;
    r = multiwd_add(1, &t);
    cr_assert_eq(r, 0, "ret=%d", r);
    sleep(2);
    r = multiwd_kick(1, NULL);
    cr_assert_eq(r, 0, "ret=%d", r);
    sleep(2);
    r = multiwd_kick_minimal(1);
    cr_assert_eq(r, 0, "ret=%d", r);
    sleep(2);
    r = multiwd_kick_multiple(0, 2);
    cr_assert_eq(r, 0, "ret=%d", r);
    sleep(2);
    r = multiwd_kick_multiple_minimal(0, 2);
    cr_assert_eq(r, 0, "ret=%d", r);
    sleep(2);
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, uninitialised, .fini = fini_fun, .disabled = false)
{
    int r;
    const struct timespec t = { .tv_sec = 30, .tv_nsec = 3 };

    multiwd_add(0, &t);
    cr_assert_neq(r, 0, "ret=%d", r);
    multiwd_add3(0, &t, SIGTERM);
    cr_assert_neq(r, 0, "ret=%d", r);
    multiwd_kick(0, NULL);
    cr_assert_neq(r, 0, "ret=%d", r);
    multiwd_kick_minimal(0);
    cr_assert_neq(r, 0, "ret=%d", r);
    multiwd_kick_multiple(0, 0);
    cr_assert_neq(r, 0, "ret=%d", r);
    multiwd_kick_multiple_minimal(0, 0);
    cr_assert_neq(r, 0, "ret=%d", r);
    multiwd_remove(0, NULL);
    cr_assert_neq(r, 0, "ret=%d", r);
    multiwd_register_child(getppid());
    cr_assert_neq(r, 0, "ret=%d", r);
    multiwd_register_child2(getppid(), SIGTERM);
    cr_assert_neq(r, 0, "ret=%d", r);
    pid_t pid = multiwd_fork();
    cr_assert_geq(pid, 0);
    if(pid == 0){
        exit(0);
    }
    waitpid(pid, NULL, 0);
    multiwd_shutdown();
    cr_assert_neq(r, 0, "ret=%d", r);
}

Test(multiwd, register_child_invalid, .init = init_fun, .fini = fini_fun, .disabled = false)
{
    int r;
    r = multiwd_register_child(0);
    cr_assert_lt(r, 0, "pid 0 is not valid");
    r = multiwd_register_child(-1);
    cr_assert_lt(r, 0, "pid -1 is not valid");
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, register_child_max, .init = init_fun, .fini = fini_fun, .disabled = false)
{
    for(int i = 0; i < MULTIWD_MAX_CHILDREN; i++){
        pid_t pid = multiwd_fork();
        cr_assert_geq(pid, 0, "fork failed");

        if(pid == 0){
            sleep(5);
            exit(i);
        }
    }

    pid_t pid = multiwd_fork();
    cr_assert_lt(pid, 0, "fork failed");

    for(int i = 0; i < MULTIWD_MAX_CHILDREN; i++){
        wait(NULL);
    }
    int r = multiwd_shutdown();
    cr_assert_eq(r, 0);
}

Test(multiwd, register_child, .init = init_fun, .fini = fini_fun, .timeout = 30, .disabled = false)
{
    pid_t pid = multiwd_fork();
    int r;
    cr_assert_geq(pid, 0, "fork failed");

    if(pid == 0){
        sleep(5);
        r = multiwd_shutdown();
        cr_assert_eq(r, 0, "ret=%d", r);
        exit(0);
    }

    waitpid(pid, NULL, 0);
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "shutdown failed");
}

Test(multiwd, register_child_multiple, .init = init_fun, .fini = fini_fun, .timeout = 30, .disabled = false)
{
    constexpr int test_cnt = 2;
    int r;

    for(int i = 0; i < test_cnt; i++){
        pid_t pid = multiwd_fork();
        cr_assert_geq(pid, 0, "fork failed");

        if(pid == 0){
            sleep(5);
            r = multiwd_shutdown();
            cr_assert_eq(r, 0, "ret=%d", r);
            exit(i);
        }
    }
    for(int i = 0; i < test_cnt; i++){
        wait(NULL);
    }
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, child_work_after_parent_exit, .init = init_fun, .fini = fini_fun, .timeout = 30, .disabled = false)
{
    constexpr int test_cnt = 10;
    int r;
    const struct timespec t = { .tv_sec = 10, .tv_nsec = 3 };

    for(int i = 0; i < test_cnt; i++){
        pid_t pid = multiwd_fork();
        cr_assert_geq(pid, 0, "fork failed");

        if(pid == 0){
            multiwd_add(0, &t);
            sleep(3);
            multiwd_kick(0, NULL);
            sleep(3);
            multiwd_kick(0, NULL);
            sleep(3);
            multiwd_kick(0, NULL);
            sleep(3);
            multiwd_kick(0, NULL);
            sleep(3);
            multiwd_kick(0, NULL);
            sleep(3);
            multiwd_kick(0, NULL);
            r = multiwd_shutdown();
            cr_assert_eq(r, 0, "ret=%d", r);
            exit(i);
        }
    }
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, register_child_early_exit, .init = init_fun, .fini = fini_fun, .timeout = 30, .disabled = false)
{
    constexpr int test_cnt = 10;
    int r;

    for(int i = 0; i < test_cnt; i++){
        pid_t pid = multiwd_fork();
        cr_assert_geq(pid, 0, "fork failed");

        if(pid == 0){
            r = multiwd_shutdown();
            cr_assert_eq(r, 0, "ret=%d", r);
            exit(i);
        }
    }
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}


Test(multiwd, register_child_branching, .init = init_fun, .fini = fini_fun, .timeout = 30, .disabled = false)
{
    constexpr int test_cnt = 2;
    int r;

    for(int i = 0; i < test_cnt; i++){
        pid_t pid = multiwd_fork();
        cr_assert_geq(pid, 0, "fork failed");

        if(pid == 0){
            pid_t pid2 = multiwd_fork();
            if(pid == 0){
                sleep(5);
                r = multiwd_shutdown();
                cr_assert_eq(r, 0, "ret=%d", r);
                exit(-1 * i);
            }
            sleep(5);
            r = multiwd_shutdown();
            cr_assert_eq(r, 0, "ret=%d", r);
            exit(i);
        }
    }
    for(int i = 0; i < test_cnt; i++){
        wait(NULL);
    }
    r = multiwd_shutdown();
    cr_assert_eq(r, 0, "ret=%d", r);
}

Test(multiwd, child_signal_kills_parent, .init = init_fun, .fini = fini_fun, .timeout = 60, .signal = SIGTERM, .disabled = false)
{
    setup_coverage_signal_handler();
    pid_t pid = multiwd_fork();
    printf("!!!!!!!!!!!!!!!!!!!TEST PID %d!!!!!!!!!!!!!!!!!!", pid);
    cr_assert_geq(pid, 0);

    if(pid == 0){
        const struct timespec t = { .tv_sec = 3, .tv_nsec = 0 };
        multiwd_add(0, &t);
        sleep(10);
        cr_assert_neq(0, 0, "we shouldnt get here child");
        exit(-1);
    }
    sleep(30);
    multiwd_shutdown();
    cr_assert_neq(0, 0, "we shouldnt get here parent");
}

Test(multiwd, parent_signal_kills_child, .fini = fini_fun, .timeout = 60, .disabled = false)
{
    setup_coverage_signal_handler();
    int pr = prctl(PR_SET_CHILD_SUBREAPER, 1);
    cr_assert_eq(pr,0);
    pid_t parent_pid = fork();
    cr_assert_geq(parent_pid, 0, "Initial fork failed");

    if (parent_pid == 0) {
        /* --- MOCK PARENT PROCESS --- */
        int r = multiwd_init(0, SIGTERM);
        cr_assert_eq(r, 0);

        pid_t child_pid = multiwd_fork();
        printf("!!!!!!!!!!!!!!!!!!!TEST PID %d!!!!!!!!!!!!!!!!!!", child_pid);
        cr_assert_geq(child_pid, 0);
        
        if (child_pid == 0) {
            /* --- CHILD PROCESS --- */
            sleep(100);
            cr_assert_neq(0,0, "sanity failed");
            exit(0); // Should not reach here
        } else {
            /* --- MOCK PARENT CONTINUES --- */
            const struct timespec t = { .tv_sec = 3, .tv_nsec = 0};
            multiwd_add(0, &t);
            sleep(5);
            cr_assert_neq(0,0, "sanity failed");
        }
    } else {
        /* --- TEST SUPERVISOR (CRITERION) --- */
        int status_parent;
        int status_child;

        // 1. Wait for the Mock Parent (it should have aborted)
        waitpid(parent_pid, &status_parent, 0);
        cr_assert(WIFSIGNALED(status_parent) && WTERMSIG(status_parent) == SIGTERM, "Mock Parent did not abort as expected");

        // 2. Wait for the Orphaned Child
        // We use -1 or specifically track the grandchild PID if passed back.
        // For simplicity, we wait for the next available child process.
        pid_t cleaned_child = waitpid(-1, &status_child, 0);
        
        // 3. THE VERIFICATION
        cr_assert(WIFSIGNALED(status_child), "Child exited normally, but should have been signaled.");
        cr_assert(WTERMSIG(status_child) == SIGTERM, "Child died, but not via term().");
    }
}
