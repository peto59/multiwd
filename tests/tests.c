#include <criterion/criterion.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>

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
    cr_assert_eq(r, 0, "shutdown failed");
}

/*Test(process, child_dies_when_parent_aborts)
{
    pid_t test = fork();
    cr_assert(test >= 0);

    if (test == 0) {

        mylib_init();

        pid_t pid = mylib_fork();
        if (pid == 0) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);

            while (1)
                pause();
        }

        usleep(100000);
        abort();
    }

    // Criterion side: observe child

    int status;
    time_t start = time(NULL);

    while (1) {

        pid_t r = waitpid(-1, &status, WNOHANG);

        if (r > 0)
            break;

        if (time(NULL) - start > CHILD_TIMEOUT_SEC)
            cr_assert_fail("child did not terminate");

        usleep(10000);
    }

    cr_assert(WIFSIGNALED(status));
}*/

/*Test(multiwd, plain_cleanup, .init = init_fun, .fini = fini_fun)
{
    cr_assert_eq(0, 0);
}

Test(multiwd, cleanup_multiple, .init = init_fun, .fini = fini_fun)
{
    const struct timespec t = { .tv_sec = 30, .tv_nsec = 3 };
    for(int i = 0; i < 10; i++){
        int r = multiwd_add(i, &t);
        cr_assert_eq(r, 0, "ret=%d", r);
    }
}

Test(multiwd, cleanup_max, .init = init_fun, .fini = fini_fun)
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
}

Test(multiwd, timer_expiration, .init = init_fun, .fini = fini_fun, .signal = SIGTERM )
{
    const struct timespec t = { .tv_sec = 3, .tv_nsec = 3 };
    int r = multiwd_add(0, &t);
    cr_assert_eq(r, 0, "ret=%d", r);

    sleep(5);
}

Test(multiwd, timer_expiration_multiple, .init = init_fun, .fini = fini_fun, .signal = SIGTERM )
{
    const struct timespec t = { .tv_sec = 3, .tv_nsec = 3 };
    for(uint64_t i = 0; i < 10; i++){
        int r = multiwd_add(i, &t);
        cr_assert_eq(r, 0, "ret=%d", r);
    }

    sleep(5);
}

Test(multiwd, register_child_invalid, .init = init_fun, .fini = fini_fun)
{
    int r;
    r = multiwd_register_child(0);
    cr_assert_lt(r, 0, "pid 0 is not valid");
    r = multiwd_register_child(-1);
    cr_assert_lt(r, 0, "pid -1 is not valid");
}

Test(multiwd, register_child_max, .init = init_fun, .fini = fini_fun)
{
    int r;

    for(int i = 0; i < MULTIWD_MAX_CHILDREN; i++){
        pid_t pid = multiwd_fork();
        cr_assert_geq(pid, 0, "fork failed");

        if(pid == 0){
            sleep(5);
            exit(i);
        }
    }
    for(int i = 0; i < MULTIWD_MAX_CHILDREN; i++){
        wait(NULL);
    }
}*/

Test(multiwd, register_child, .init = init_fun, .fini = fini_fun, .timeout = 30)
{
        pid_t pid = multiwd_fork();
        cr_assert_geq(pid, 0, "fork failed");

        if(pid == 0){
            sleep(5);
            exit(0);
        }

        waitpid(pid, NULL, 0);
}

Test(multiwd, register_child_multiple, .init = init_fun, .fini = fini_fun, .timeout = 30, .disabled = true)
{
    constexpr int test_cnt = 2;
    int r;

    for(int i = 0; i < test_cnt; i++){
        pid_t pid = multiwd_fork();
        cr_assert_geq(pid, 0, "fork failed");

        if(pid == 0){
            sleep(5);
            exit(i);
        }
    }
    for(int i = 0; i < test_cnt; i++){
        wait(NULL);
    }
}
