#define _GNU_SOURCE
#include "multiwd.h"

#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>

#define MULTIWD_DEBUG

#define inline_macro __attribute__((always_inline))

// ---- internal state ----

#if (MULTIWD_MAX_TIMERS % 64) != 0
    #define MULTIWD_MAX_TIMERS_FINAL (((MULTIWD_MAX_TIMERS) + 63ul) & ~63ul)
    #warning ("MULTIWD_MAX_TIMERS was not a multiple of 64. Rounding up.")
#else
    #define MULTIWD_MAX_TIMERS_FINAL MULTIWD_MAX_TIMERS
#endif

#if (MULTIWD_MAX_CHILDREN % 64) != 0
    #define MULTIWD_MAX_CHILDREN_FINAL (((MULTIWD_MAX_CHILDREN) + 63ul) & ~63ul)
    #warning ("MULTIWD_MAX_CHILDREN was not a multiple of 64. Rounding up.")
#else 
    #define MULTIWD_MAX_CHILDREN_FINAL MULTIWD_MAX_CHILDREN
#endif

typedef atomic_uint_fast64_t bitmap_type;

#define MULTIWD_TIMERS_BITMAP_SLOTS MULTIWD_MAX_TIMERS_FINAL / (sizeof(bitmap_type) * 8)
#define MULTIWD_CHILDREN_BITMAP_SLOTS MULTIWD_MAX_CHILDREN_FINAL / (sizeof(bitmap_type) * 8)

#if MULTIWD_MAX_TIMERS_FINAL == 1024
_Static_assert(sizeof(bitmap_type) == 64 / 8, "Sanity failed");
_Static_assert(MULTIWD_MAX_TIMERS_FINAL == 1024, "Sanity failed");
_Static_assert(MULTIWD_TIMERS_BITMAP_SLOTS == 16, "Sanity failed");
#endif

#define READ_END 0
#define WRITE_END 1

typedef struct {
    int                 timer_fd;
    typeof(SIGABRT)     trigger_type;
} wd_timer;

typedef struct {
    pid_t               pid;
    int                 pidfd;
    typeof(SIGABRT)     trigger_type;
} wd_child;

#define bitmap(name, bit_cnt) bitmap_type name[bit_cnt / (sizeof(bitmap_type) * 8)]

typedef struct {
    pthread_mutex_t lock;
    wd_timer timers[MULTIWD_MAX_TIMERS_FINAL];
    bitmap(used, MULTIWD_MAX_TIMERS_FINAL);
    bitmap(kicked, MULTIWD_MAX_TIMERS_FINAL);
} wd_timers;
_Static_assert((sizeof(((wd_timers *)0)->used)) == MULTIWD_MAX_TIMERS_FINAL / 8, "wd_timers used bitmap incorrect size");
_Static_assert((sizeof(((wd_timers *)0)->kicked)) == MULTIWD_MAX_TIMERS_FINAL / 8, "wd_timers kicked bitmap incorrect size");

typedef struct {
    pthread_mutex_t lock;
    wd_child children[MULTIWD_MAX_CHILDREN_FINAL];
    bitmap(used, MULTIWD_MAX_CHILDREN_FINAL);
} wd_children;
_Static_assert((sizeof(((wd_children *)0)->used)) == MULTIWD_MAX_CHILDREN_FINAL / 8, "wd_children used bitmap incorrect size");


static wd_timers g_timers = { .lock = PTHREAD_MUTEX_INITIALIZER, .timers = { 0 }, .used = { 0 }, .kicked = { 0 } };
static wd_children g_children = { .lock = PTHREAD_MUTEX_INITIALIZER, .children = { 0 }, .used = { 0 }, };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static atomic_int g_eventfd_timers = -1;
static atomic_int g_epollfd_timers = -1;
static atomic_int g_epollfd_children = -1;

static atomic_int g_children_comm_pipe[2] = { -1, -1 };

static atomic_bool atfork_installed = false;

static atomic_bool g_is_parent = true;

static atomic_bool g_initialised = false;
static atomic_bool g_finalizing = false;

static pthread_t g_watchdog_thread;
static atomic_bool g_watchdog_thread_started = false;

static pthread_t g_children_thread;
static atomic_bool g_children_thread_started = false;

static atomic_int g_coupling_flags = MULTIWD_COUPLE_BOTH | MULTIWD_AUTOINIT_IN_CHILD | MULTIWD_AUTOREGISTER_CHILDREN;
static typeof(SIGABRT) g_trigger_type = SIGABRT;

static const struct timespec zero_spec = { .tv_sec = 0, .tv_nsec = 0 };
static const struct itimerspec zero_timer = { .it_interval = zero_spec, .it_value = zero_spec };

#ifdef MULTIWD_DEBUG
static FILE *g_tty = NULL;

#include <stdarg.h>

#include <execinfo.h>

static void dump_trace(void);
static void dump_trace(void)
{
    return;
    void *buffer[64];
    int nptrs = backtrace(buffer, 64);
    backtrace_symbols_fd(buffer, nptrs, fileno(stderr));
}

void debug_print_setup();
void debug_print_setup()
{
        g_tty = fopen(TARGET_TTY, "w");
        if(g_tty == NULL){
            printf("NO TTY");
        }
}

void __attribute__ ((constructor)) debug_print_setup_construct(void);
void __attribute__ ((constructor)) debug_print_setup_construct(void) {
    if(g_tty == NULL)
    {
        debug_print_setup();
    }
}

static void debug_print(const char *restrict fmt, ...) {
    puts(fmt);
    if(!g_tty){
        debug_print_setup();
    }
    if (g_tty) {
        va_list args;
        va_start(args, fmt);
        vfprintf(g_tty, fmt, args);
        va_end(args);
    }
}
#endif

// ---- declarations ----

static void write_stderr(const char *str);

static void trigger(typeof(SIGABRT) trigger_type);
static void trigger_watchdog(uint64_t id);
static void trigger_child(uint64_t id);

static inline int lock_timers(void) inline_macro;
static inline int unlock_timers(void) inline_macro;
static inline int lock_children(void) inline_macro;
static inline int unlock_children(void) inline_macro;
static inline int lock_global(void) inline_macro;
static inline int unlock_global(void) inline_macro;
static inline void lock_global_fork_handler(void) inline_macro;
static inline void unlock_global_fork_handler(void) inline_macro;
static inline int reset_mutexes(void) inline_macro;

static int wakeup_thread(atomic_int fd);
static int wakeup_timers_thread(void);
static int wakeup_children_thread(void);

static inline void calculate_bitmap(uint64_t id, off64_t *offset, bitmap_type *bits) inline_macro;
static inline uint64_t calculate_id(size_t len, bitmap_type *bitmap) inline_macro;
static inline uint64_t find_empty_id(size_t len, bitmap_type *bitmap) inline_macro;

static int create_timer(uint64_t id, const struct timespec *timeout);
static int reset_timer(uint64_t id, struct timespec *_Nullable remaining);
static int destroy_timer(uint64_t id, struct timespec *_Nullable interval);

static int remove_child(uint64_t id);

static int kill_watcher_threads_locked(void);
static int close_comm_fds(bool include_comm_pipe, bool include_eventfd);
static int cleanup_timers(void);
static int cleanup_children(void);

static void *watchdog_thread_runner(void *arg);
static void *children_thread_runner(void *arg);

static int start_thread(atomic_bool *started, pthread_t *thread, void *(*fun) (void *));
static int start_watchdog_thread(void);
static int start_children_thread(void);

static int sys_pidfd_open(pid_t pid, unsigned int flags);
static void child_fork_handler(void);

static int multiwd_init_locked(uint64_t flags, typeof(SIGABRT) trigger_type);
static int multiwd_add3_locked(uint64_t id, const struct timespec *timeout, typeof(SIGABRT) trigger_type);
static int multiwd_kick_locked(uint64_t id, struct timespec *_Nullable remaining);
static int multiwd_kick_multiple_locked(uint64_t offset, uint64_t bitmap);
static int multiwd_remove_locked(uint64_t id, struct timespec *_Nullable timeout);
static int multiwd_register_child_locked2(pid_t child_pid, typeof(SIGABRT) trigger_type);
static int multiwd_shutdown_locked(bool total);

__attribute__((destructor)) static void multiwd_destructor(void);
__attribute__((destructor)) static void multiwd_destructor(void) {
    if(!g_initialised || g_finalizing){
        return;
    }
#ifdef MULTIWD_DEBUG
    debug_print("Calling shutdown from destructor\n");
#endif
    //multiwd_shutdown();
}

// ---- fork wrap shenenigans ----

#if MULTIWD_WRAP_FORK == 1

#include <dlfcn.h>

static pthread_once_t resolve_symbols_once = PTHREAD_ONCE_INIT;

//guard is implemented for recursion if we ever decide to wrap more than fork
static __thread int fork_guard = 0;

static pid_t (*real_fork)(void);

static void resolve_symbols(void)
{
    if (!real_fork){
        real_fork = (typeof(real_fork))dlsym(RTLD_NEXT, "fork");
    }
}

pid_t fork(void)
{
    pthread_once(&resolve_symbols_once, resolve_symbols);

    if (fork_guard){
        return real_fork();
    }

    fork_guard = 1;

    pid_t pid = real_fork();

    if(g_coupling_flags & MULTIWD_AUTOREGISTER_CHILDREN){
        multiwd_register_child(pid);

    }

    fork_guard = 0;
    return pid;
}

#endif

pid_t multiwd_fork()
{
#if MULTIWD_WRAP_FORK == 1
    return fork();
#else
    pid_t pid = fork();
    multiwd_register_child(pid);
    return pid;
#endif
}

// ---- helper functions ----
// all of these functions assume locked state or are not concerned with state

static void write_stderr(const char *str)
{
    constexpr size_t buffsize = 512 + 21;
    char buf[buffsize];

    int n = snprintf(buf, buffsize, "\x1b[1;31m[multiwd] %s\x1b[0m\n", str);
    if(n < 0){
        return;
    }

    size_t len = (size_t)n;

    if(len > buffsize){
        len = buffsize;
    }

    (void) write(2, str, len);
}

static void trigger(typeof(SIGABRT) trigger_type)
{
    if(trigger_type == 0){
        trigger_type = g_trigger_type;
    }

    if(trigger_type == SIGABRT){
        abort();
    }
    raise(trigger_type);
}

static void trigger_watchdog(uint64_t id)
{
    constexpr int buffsize = 64;
    char buf[buffsize];
    
    snprintf(buf, buffsize, "Watchdog %lu triggered\n", id);

    write_stderr(buf);
    trigger(g_timers.timers[id].trigger_type);
}

static void trigger_child(uint64_t id)
{ 
    constexpr int buffsize = 64;
    char buf[buffsize];
    
    snprintf(buf, buffsize, "Child pid %ld triggered\n", g_children.children[id].pid);

    write_stderr(buf);
    trigger(g_children.children[id].trigger_type);
}

static inline int lock_children(void)
{
    int ret = 0;
#ifdef MULTIWD_DEBUG
        debug_print("Acquiring children lock\n");
        dump_trace();
#endif
    if(pthread_mutex_lock(&(g_children.lock)) != 0 && errno != EDEADLK){
        ret |= 1 << 1;
    }
#ifdef MULTIWD_DEBUG
    if(ret != 0){
        debug_print("Failed acquiring children lock\n");
    } else {
        debug_print("Acquired children lock\n");
        dump_trace();
    }
#endif
    return ret * -1;
}

static inline int unlock_children(void)
{
    int ret = 0;
#ifdef MULTIWD_DEBUG
        debug_print("Releasing children lock\n");
#endif
    if(pthread_mutex_unlock(&(g_children.lock)) != 0){
        ret |= 1 << 1;
    }
#ifdef MULTIWD_DEBUG
    if(ret != 0){
        debug_print("Failed releasing children lock\n");
    } else {
        debug_print("Released children lock\n");
    }
#endif
    return ret * -1;
}

static inline int lock_timers(void)
{
    int ret = 0;
#ifdef MULTIWD_DEBUG
        debug_print("Acquiring timer lock\n");
#endif
    if(pthread_mutex_lock(&(g_timers.lock)) != 0 && errno != EDEADLK){
        ret |= 1 << 0;
    }
#ifdef MULTIWD_DEBUG
    if(ret != 0){
        debug_print("Failed acquiring timers lock\n");
    } else {
        debug_print("Acquired timers lock\n");
    }
#endif
    return ret * -1;
}

static inline int unlock_timers(void)
{
    int ret = 0;
#ifdef MULTIWD_DEBUG
        debug_print("Releasing timers lock\n");
#endif
    if(pthread_mutex_unlock(&(g_timers.lock)) != 0){
        ret |= 1 << 0;
    }
#ifdef MULTIWD_DEBUG
    if(ret != 0){
        debug_print("Failed releasing timers lock\n");
    } else {
        debug_print("Released timers lock\n");
    }
#endif
    return ret * -1;
}

static inline int lock_global(void)
{
    int ret = 0;
#ifdef MULTIWD_DEBUG
        debug_print("Acquiring global lock\n");
#endif
    if(pthread_mutex_lock(&g_lock) != 0 && errno != EDEADLK){
#ifdef MULTIWD_DEBUG
        debug_print("Failed acquiring global lock\n");
#endif
        ret |= 1 << 2;
    }
    if(ret == 0 && (ret = ((lock_timers() * -1) | (lock_children() * -1))) != 0){
        if((ret & (1 << 0)) == 0){
            if(unlock_timers() != 0){
                ret |= 1 << 3;
            }
        }
        if((ret & (1 << 1)) == 0){
            if(unlock_children() != 0){
                ret |= 1 << 4;
            }
        }
    }
    if(ret != 0 && pthread_mutex_unlock(&g_lock) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Failed releasing global lock\n");
#endif
        ret |= 1 << 5;
    }
#ifdef MULTIWD_DEBUG
    if(ret != 0){
        debug_print("Failed acquiring global lock %d\n", ret);
    } else {
        debug_print("Acquired global lock\n");
    }
#endif
    return ret * -1;
}

static inline int unlock_global(void)
{
    int ret = 0;
#ifdef MULTIWD_DEBUG
        debug_print("Releasing global lock\n");
#endif
    if(pthread_mutex_unlock(&g_lock) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Failed releasing global lock\n");
#endif
        return -1;
    }
    if(unlock_timers() != 0){
        return -1;
    }
    if(unlock_children() != 0){
        return -1;
    }
    return ret * -1;
}

static inline void lock_global_fork_handler(void)
{
    lock_global();
}

static inline void unlock_global_fork_handler(void)
{
    unlock_global();
}

static inline int reset_mutexes(void)
{
#ifdef MULTIWD_DEBUG
    debug_print("Reseting mutexes\n");
#endif
    int ret = 0;
    if(pthread_mutex_destroy(&g_lock) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Destroying mutex failed %d\n", errno);
#endif
        ret |= 1 << 0;
    }
    if(pthread_mutex_destroy(&g_timers.lock) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Destroying mutex failed %d\n", errno);
#endif
        ret |= 1 << 1;
    }
    if(pthread_mutex_destroy(&g_children.lock) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Destroying mutex failed %d\n", errno);
#endif
        ret |= 1 << 2;
    }

    ret = 0; //Assume failure to destroy mutex is non fatal as errno 11 does not make sense
    
    if(pthread_mutex_init(&g_lock, NULL) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Initing mutex failed %d\n", errno);
#endif
        ret |= 1 << 3;
    }
    if(pthread_mutex_init(&g_timers.lock, NULL) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Initing mutex failed %d\n", errno);
#endif
        ret |= 1 << 5;
    }
    if(pthread_mutex_init(&g_children.lock, NULL) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Initing mutex failed %d\n", errno);
#endif
        ret |= 1 << 4;
    }
#ifdef MULTIWD_DEBUG
    if(ret != 0){
        debug_print("Failed reseting mutexes %d\n", ret);
    }
#endif

    return ret * -1;
}

static int wakeup_thread(atomic_int fd)
{
    uint64_t val = 1ul;
    if(write(fd, &val, 8) != 8){
        return -1;
    }
    return 0;
}

static int wakeup_timers_thread(void)
{
    return wakeup_thread(g_eventfd_timers);
}

static int wakeup_children_thread(void)
{
    return wakeup_thread(g_children_comm_pipe[WRITE_END]);
}

static inline void calculate_bitmap(uint64_t id, off64_t *offset, bitmap_type *bits)
{
    constexpr uint64_t width = sizeof(bitmap_type) * 8;
    *offset = id / width;
#ifdef MULTIWD_DEBUG
    debug_print("Calculating bitmap id: %lu, offset %lu, width: %d\n", id, *offset, width);
#endif
    *bits = UINT64_C(1) << (id % width);
}

static inline uint64_t calculate_id(size_t len, bitmap_type *bitmap)
{
    for(size_t off = 0; off < len; off++){
        if(bitmap[off] == 0){
            continue;
        }
        for(uint64_t bit = 0; bit < sizeof(uint64_t) * 8; bit++){
            if(bitmap[off] & (UINT64_C(1) << bit)){
                return off * sizeof(uint64_t) * 8 + bit;
            }
        }
    }
    return UINT64_MAX;
}

static inline uint64_t find_empty_id(size_t len, bitmap_type *bitmap)
{
    for(size_t off = 0; off < len; off++){
        if(bitmap[off] == UINT64_MAX){
            continue;
        }
        for(uint64_t bit = 0; bit < sizeof(uint64_t) * 8; bit++){
            if((bitmap[off] & (UINT64_C(1) << bit)) == 0){
                return off * sizeof(uint64_t) * 8 + bit;
            }
        }
    }
    return UINT64_MAX;
}

static int create_timer(uint64_t id, const struct timespec *timeout)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if(fd == -1){
        return -1;
    }

    g_timers.timers[id].timer_fd = fd;

    const struct itimerspec t = { .it_interval = *timeout, .it_value = *timeout };

    if(timerfd_settime(fd, 0, &t, NULL) != 0){
        close(fd);
        return -2;
    }
    return 0;
}

static int reset_timer(uint64_t id, struct timespec *_Nullable remaining)
{
    struct itimerspec t;
    int fd = g_timers.timers[id].timer_fd;

    if(timerfd_gettime(fd, &t) != 0)
    {
        return -1;
    }

    if(remaining != NULL){
        *remaining = t.it_value;
    }

    t.it_value = t.it_interval;

    if(timerfd_settime(fd, 0, &t, NULL) != 0){
        return -1;
    }
    return 0;
}

static int destroy_timer(uint64_t id, struct timespec *_Nullable interval)
{
    int fd = g_timers.timers[id].timer_fd;

    struct itimerspec t;

    if(timerfd_settime(fd, 0, &zero_timer, &t) != 0){
        return -1;
    }

    if(close(fd) != 0){
        return -1;
    }

    if(interval != NULL){
        *interval = t.it_interval;
    }

    return 0;
}

static int remove_child(uint64_t id)
{
    off64_t offset;
    bitmap_type bits;
    calculate_bitmap(id, &offset, &bits);

    if((g_children.used[offset] & bits) == 0){
        return -1;
    }

    int fd = g_children.children[id].pidfd;

    if(close(fd) != 0){
        return -1;
    }

    g_children.used[offset] &= ~bits;

    return 0;
}

static int kill_watcher_threads_locked(void)
{
    if(!g_watchdog_thread_started && !g_children_thread_started){
        return 0;
    }

    bool timers_woken = false;
    bool children_woken = false;

    int ret = 0;

    if(g_watchdog_thread_started){
        if(wakeup_timers_thread() != 0){
            ret = 1 << 0;
        } else {
            timers_woken = true;
        }
    }

    if(g_children_thread_started){
        if(wakeup_children_thread() != 0){
            ret = 1 << 1;
        } else {
            children_woken = true;
        }
    }


#ifdef MULTIWD_DEBUG
        debug_print("sleeping kill_watcher_threads_locked\n");
#endif
    sleep(1);
#ifdef MULTIWD_DEBUG
        debug_print("slept kill_watcher_threads_locked\n");
#endif


    if(unlock_global() != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Failed releasing global lock for kill_watcher_threads_locked\n");
#endif

        return (1 << 20) * -1;
    }

    if(timers_woken && pthread_join(g_watchdog_thread, NULL) != 0){
        ret = 1 << 2;
    } else {
        g_watchdog_thread_started = false;

    }

    if(children_woken && pthread_join(g_children_thread, NULL) != 0){
        ret = 1 << 3;
    } else {
        g_children_thread_started = false;
    }

    if(lock_global() != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Failed acquiring global lock for kill_watcher_threads_locked\n");
#endif
        return (1 << 21) * -1;
    }

    return ret * -1;
}

static int close_comm_fds(bool include_comm_pipe, bool include_eventfd)
{
    int ret = 0;

    if(include_eventfd && g_eventfd_timers != -1){
        if(close(g_eventfd_timers) != 0){
            ret = 1 << 5;
        } else {
            g_eventfd_timers = -1;
        }
    }

    
    if(g_epollfd_timers != -1){
        if(close(g_epollfd_timers) != 0){
            ret = 1 << 6;
        } else {
            g_epollfd_timers = -1;
        }
    }

    if(g_epollfd_children != -1){
        if(close(g_epollfd_children) != 0){
            ret = 1 << 7;
        } else {
            g_epollfd_children = -1;
        }
    }


    if(include_comm_pipe){
        if(g_children_comm_pipe[READ_END] != -1){
            if(close(g_children_comm_pipe[READ_END]) != 0){
                ret = 1 << 8;
            } else {
                g_children_comm_pipe[READ_END] = -1;
            }
        }

        if(g_children_comm_pipe[WRITE_END] != -1){
            if(close(g_children_comm_pipe[WRITE_END]) != 0){
                ret = 1 << 9;
            } else {
                g_children_comm_pipe[WRITE_END] = -1;
            }
        }
    }
    
    return ret * -1;
}

static int cleanup_timers(void)
{
    uint64_t id;
    while((id = calculate_id(MULTIWD_TIMERS_BITMAP_SLOTS, g_timers.used)) != UINT64_MAX){
        if(multiwd_remove_locked(id, NULL) != 0){
            return -1 * (1 << 10);
        }
    }
    
    return 0;
}

static int cleanup_children(void)
{
    uint64_t id;
    while((id = calculate_id(MULTIWD_CHILDREN_BITMAP_SLOTS, g_children.used)) != UINT64_MAX){
        if(remove_child(id) != 0){
            return -1 * (1 << 11);
        }
    }
    
    return 0;
}

static void *watchdog_thread_runner(void *arg)
{
    (void) arg;
    constexpr uint64_t event_cnt = MULTIWD_MAX_TIMERS_FINAL + 1;
    struct epoll_event events[event_cnt];
    int epoll_count;
    off64_t offset;
    bitmap_type bits;
    uint64_t id;

    while(true){
        if((epoll_count = epoll_wait(g_epollfd_timers, events, event_cnt, -1)) <= 0){
            write_stderr("Epoll failed in watchdog thread! Disabling it!");
            return NULL;
        }

#ifdef MULTIWD_DEBUG
        debug_print("Watchdog thread waken!!!!!!!!!!!!!!!!!!!!!!!!\n");
#endif

        bool finalize = false;
#ifdef MULTIWD_DEBUG
            debug_print("WD!!! finalizing????? %d\n", g_finalizing);
            printf("WD!!! finalizing????? %d\n", g_finalizing);
#endif
        if(lock_global() != 0){
            write_stderr("Watchdog thread couldn't acquire lock! Disabling it!");
            return NULL;
        }
        finalize = g_finalizing;
        if(unlock_global() != 0){
            write_stderr("Watchdog thread failed releasing lock!");
            abort();
        }
        if(finalize){
#ifdef MULTIWD_DEBUG
            debug_print("Watchdog finalizing\n");
#endif
            return NULL;
        }
#ifdef MULTIWD_DEBUG
            debug_print("Watchdog!!! finalizing????? %d\n", g_finalizing);
#endif
#ifdef MULTIWD_DEBUG
        debug_print("Watchdog work %d\n", epoll_count);
#endif

        if(lock_timers() != 0){
            write_stderr("Watchdog thread couldn't acquire lock! Disabling it!");
            return NULL;
        }
        for(int i = 0; i < epoll_count; i++){
            if((events[i].events & EPOLLIN) == 0){
                continue;
            }

            if(events[i].data.u64 == UINT64_MAX){ //eventfd
                while((id = calculate_id(MULTIWD_TIMERS_BITMAP_SLOTS, g_timers.kicked)) != UINT64_MAX){
                    multiwd_kick_locked(id, NULL);
                }
            } else { //timerfd
                id = events[i].data.u64;
#ifdef MULTIWD_DEBUG
        debug_print("Watchdog processing timer %lu\n", id);
#endif
                calculate_bitmap(id, &offset, &bits);
                if((g_timers.kicked[id] & bits) != 0){
                    g_timers.kicked[id] &= ~bits;
                    continue;
                }

                if(!g_is_parent){
                    uint64_t pid = (uint64_t) getpid();
                    (void) write(g_children_comm_pipe[WRITE_END], &pid, 8);
                }

                trigger_watchdog(id);
            }
        }
        if(unlock_timers() != 0){
            write_stderr("Watchdog thread failed releasing lock!");
            abort();

        }
    }
}

static void *children_thread_runner(void *arg)
{
    (void) arg;
    constexpr uint64_t event_cnt = MULTIWD_MAX_CHILDREN_FINAL + 1;
    struct epoll_event events[event_cnt];
    int epoll_count;
    uint64_t id;

    while(true){
        if((epoll_count = epoll_wait(g_epollfd_children, events, event_cnt, -1)) <= 0){
            write_stderr("Epoll failed in children thread! Disabling it!");
            return NULL;
        }
#ifdef MULTIWD_DEBUG
        debug_print("Children thread waken\n");
#endif

        bool finalize = false;
#ifdef MULTIWD_DEBUG
            debug_print("Children!!! finalizing????? %d\n", g_finalizing);
            printf("Children!!! finalizing????? %d\n", g_finalizing);
#endif
        if(lock_global() != 0){
            write_stderr("Children thread couldn't acquire lock! Disabling it!");
            return NULL;
        }
        finalize = g_finalizing;
        if(unlock_global() != 0){
            write_stderr("Children thread failed releasing lock!");
            abort();
        }
        if(finalize){
#ifdef MULTIWD_DEBUG
            debug_print("Children finalizing\n");
#endif
            return NULL;
        }


        if(lock_children() != 0){
            write_stderr("Children thread couldn't acquire lock! Disabling it!");
            return NULL;
        }
        for(int i = 0; i < epoll_count; i++){
            if((events[i].events & EPOLLIN) == 0){
                continue;
            }

            if(events[i].data.u64 == UINT64_MAX){ //comm pipe
                uint64_t pid_u;
                ssize_t read_cnt;

                if(unlock_children() != 0){
                    write_stderr("Children thread failed releasing lock!");
                    abort();
                }
                read_cnt = read(g_children_comm_pipe[READ_END], &pid_u, 8);
                if(lock_children() != 0){
                    write_stderr("Children thread couldn't acquire lock! Disabling it!");
                    return NULL;
                }

                if( read_cnt != 8){
                    continue;
                }

                pid_t pid = (pid_t) pid_u;

                while((id = calculate_id(MULTIWD_CHILDREN_BITMAP_SLOTS, g_children.used)) != UINT64_MAX){
                    if(g_children.children[id].pid != pid){
                        continue;
                    }

                    trigger_child(id);
                }

            } else { //pidfd
                id = events[i].data.u64;
                siginfo_t info = { 0 };

                if(waitid(P_PIDFD, (id_t) g_children.children[id].pidfd, &info, WNOHANG | WNOWAIT) == -1){
                    if (errno == ECHILD) {
                        constexpr int buffsize = 64;
                        char buf[buffsize];
                        snprintf(buf, buffsize, "Child pid %ld already reaped, ignoring\n", g_children.children[id].pid);
                        write_stderr(buf);
                    }
                    remove_child(id);
                    continue;
                }

                if (info.si_pid == 0) {
                    //no child
                    continue;
                }

                if(info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED){
                    trigger_child(id);
                }
            }
        }
        if(unlock_children() != 0){
            write_stderr("Children thread failed releasing lock!");
            abort();
        }
    }

}

static int start_thread(atomic_bool *started, pthread_t *thread, void *(*fun) (void *))
{
    if(*started){
        return -1;
    }

    *started = false;

    if(pthread_create(thread, NULL, fun, NULL) != 0){
        return -1;
    }

    *started = true;
    return 0;
}

static int start_watchdog_thread(void)
{
    return start_thread(&g_watchdog_thread_started, &g_watchdog_thread, &watchdog_thread_runner);
}

static int start_children_thread(void)
{
    return start_thread(&g_children_thread_started, &g_children_thread, &children_thread_runner);
}

static int sys_pidfd_open(pid_t pid, unsigned int flags)
{
    return (int)syscall(SYS_pidfd_open, pid, flags);
}

static void child_fork_handler(void)
{
    //TODO: what does this imply
    g_is_parent = false;

    g_watchdog_thread_started = false;
    g_children_thread_started = false;


    if(multiwd_shutdown_locked(false) != 0){
        write_stderr("Cannot shutdown in child after fork");
        abort();
    }

    if (g_coupling_flags & MULTIWD_COUPLE_CHILD_WITH_PARENT) {
        if (prctl(PR_SET_PDEATHSIG, g_trigger_type) != 0) {
            write_stderr("PR_SET_PDEATHSIG failed; coupling may not work\n");
        }
        if (getppid() == 1) {
            write_stderr("Parent already dead after fork\n");
            trigger(g_trigger_type);
        }
    }


    if(g_coupling_flags & MULTIWD_AUTOINIT_IN_CHILD){
        multiwd_init(0, 0);
    }
}

// ---- public API - locked variants ----

static int multiwd_init_locked(uint64_t flags, typeof(SIGABRT) trigger_type)
{
    if(g_initialised == 1){
        errno = EEXIST;
        return -1;
    }

    if(reset_mutexes() != 0){
        printf("Could not initiaize mutexes");
        return -1;
    }

    if(lock_global() != 0){
#ifdef MULTIWD_DEBUG
    debug_print("Failed acquiring global lock for init\n");
#endif
        return -205;
    }

#ifdef MULTIWD_DEBUG
    if(g_tty == NULL)
    {
        g_tty = fopen(TARGET_TTY, "w");
        if(g_tty == NULL){
            printf("NO TTY");
            return -1;
        }
    }

    debug_print("INIT\n");
#endif

    g_finalizing = false;

    if(flags != 0){
        g_coupling_flags = flags;
    }
    if(trigger_type != 0){
        g_trigger_type = trigger_type;
    }

    if(g_eventfd_timers == -1){
        g_eventfd_timers = eventfd(0, EFD_CLOEXEC);
        if(g_eventfd_timers == -1){
            return -1;
        }
    }

    if(g_children_comm_pipe[READ_END] == -1 && g_children_comm_pipe[WRITE_END] == -1){
        int temp[2];
        if(pipe(temp) != 0){
            return -1;
        }
        g_children_comm_pipe[READ_END] = temp[READ_END];
        g_children_comm_pipe[WRITE_END] = temp[WRITE_END];
    }

    struct epoll_event event = { .events = EPOLLIN, .data.u64 = UINT64_MAX };

    if(g_epollfd_timers == -1){
        g_epollfd_timers = epoll_create1(EPOLL_CLOEXEC);
        if(g_epollfd_timers == -1){
            return -1;
        }

        if(epoll_ctl(g_epollfd_timers, EPOLL_CTL_ADD, g_eventfd_timers, &event) != 0){
            return -1;
        }
    }

    if(g_epollfd_children == -1){
        g_epollfd_children = epoll_create1(EPOLL_CLOEXEC);
        if(g_epollfd_children == -1){
            return -1;
        }

        if(epoll_ctl(g_epollfd_children, EPOLL_CTL_ADD, g_children_comm_pipe[READ_END], &event) != 0){
            return -1;
        }
    }

    if(!g_watchdog_thread_started && start_watchdog_thread() != 0){
        return -1;
    }

    if(!g_children_thread_started && start_children_thread() != 0){
        return -1;
    }

    if (!atfork_installed) {
        if (pthread_atfork(&lock_global_fork_handler, &unlock_global_fork_handler, &child_fork_handler) != 0) {
            return -1;
        }
        atfork_installed = true;
    }

    g_initialised = 1;
#ifdef MULTIWD_DEBUG
    debug_print("inited\n");
#endif

    return 0;
}

static int multiwd_add3_locked(uint64_t id, const struct timespec *timeout, typeof(SIGABRT) trigger_type)
{
    off64_t offset;
    bitmap_type bits;
    int ret;
    calculate_bitmap(id, &offset, &bits);

#ifdef MULTIWD_DEBUG
    debug_print("Adding timer %lu\n", id);
#endif


    if(g_timers.used[offset] & bits){
        errno = EBADSLT;
        return -100;
    }

    g_timers.used[offset] |= bits;

    if((ret = create_timer(id, timeout)) != 0){
        g_timers.used[offset] &= ~bits;

#ifdef MULTIWD_DEBUG
        debug_print("Create_timer failed with errno = %lu", errno);

#endif
        return ret;
    }

    g_timers.timers[id].trigger_type = trigger_type;

    int fd = g_timers.timers[id].timer_fd;

    struct epoll_event event = { .events = EPOLLIN, .data.u64 = id };

    if(epoll_ctl(g_epollfd_timers, EPOLL_CTL_ADD, fd, &event) != 0){
        if(destroy_timer(id, NULL) == 0){
            g_timers.used[offset] &= ~bits;
        }
        return -101;
    }

#ifdef MULTIWD_DEBUG
    debug_print("Added timer %lu\n", id);
#endif
    
    return 0;
}

static int multiwd_kick_locked(uint64_t id, struct timespec *_Nullable remaining)
{
    off64_t offset;
    bitmap_type bits;
    calculate_bitmap(id, &offset, &bits);

    g_timers.kicked[offset] |= bits;

    if((g_timers.used[offset] & bits) == 0){
        errno = EBADSLT;
        g_timers.kicked[offset] &= ~bits;
        return -1;
    }

    if(reset_timer(id, remaining) != 0){
        return -1;
    }

    g_timers.kicked[offset] &= ~bits;
    return 0;
}

static int multiwd_kick_multiple_locked(uint64_t offset, uint64_t bitmap)
{
    bitmap_type bitmap_actual = bitmap;

    g_timers.kicked[offset] |= bitmap_actual;
    
    uint64_t id = 0;
    int ret = 0;
    while((id = calculate_id(1, &bitmap_actual)) != UINT64_MAX){
        if(multiwd_kick_locked(id, NULL) != 0){
            ret = -1;
        }
    }

    return ret;
}

static int multiwd_remove_locked(uint64_t id, struct timespec *_Nullable timeout)
{
    off64_t offset;
    bitmap_type bits;
    calculate_bitmap(id, &offset, &bits);

    if((g_timers.used[offset] & bits) == 0){
        errno = EBADSLT;
        return -1;
    }

    int fd = g_timers.timers[id].timer_fd;
    int epoll_ret = epoll_ctl(g_epollfd_timers, EPOLL_CTL_DEL, fd, NULL);

    if(destroy_timer(id, timeout) != 0){
        return -1;
    }

    if(epoll_ret != 0){
        g_timers.used[offset] &= ~bits;
    }

    return 0;
}

static int multiwd_register_child_locked2(pid_t child_pid, typeof(SIGABRT) trigger_type)
{
    int fd = sys_pidfd_open(child_pid, 0);
    if(fd == -1){
        return -1;
    }

    uint64_t id = find_empty_id(MULTIWD_CHILDREN_BITMAP_SLOTS, g_children.used);
    if(id == UINT64_MAX){
        errno = ENOSPC;
        return -2;
    }

    off64_t offset;
    bitmap_type bits;
    calculate_bitmap(id, &offset, &bits);

    g_children.used[offset] |= bits;

    wd_child child = { .pid = child_pid, .pidfd = fd, .trigger_type = trigger_type };
    g_children.children[id] = child;

    struct epoll_event event = { .events = EPOLLIN, .data.u64 = id };

    if(epoll_ctl(g_epollfd_children, EPOLL_CTL_ADD, fd, &event) != 0){
        g_children.used[offset] &= ~bits;
        return -3;
    }

    return 0;
}

static int multiwd_shutdown_locked(bool total)
{
    if(!g_initialised || g_finalizing){
        errno = ENOTSUP;
        return -1;
    }

    g_finalizing = true;

    int ret = 0;
    int tmp;

    if(total && (tmp = kill_watcher_threads_locked()) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Shutdown kill_threads failed ret: %d\n", tmp * -1);
#endif
        ret |= tmp * -1;
    }

    if((tmp = close_comm_fds(total && g_is_parent, ret == 0)) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Shutdown close_comm_fds failed ret: %d\n", tmp * -1);
#endif
        ret |= tmp * -1;
    }

    if((tmp = cleanup_timers()) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Shutdown cleanup_timers failed ret: %d\n", tmp * -1);
#endif
        ret |= tmp * -1;
    }

    if((tmp = cleanup_children()) != 0){
#ifdef MULTIWD_DEBUG
        debug_print("Shutdown cleanup_children failed ret: %d\n", tmp * -1);
#endif
        ret |= tmp * -1;
    }

    g_finalizing = false;
    g_initialised = false;

    if(unlock_global() != 0) {
        ret |= 1 << 20;
    }
    sleep(1);

    if((tmp = reset_mutexes()) != 0){
        printf("Could not reset mutexes");
        if(!total){
            abort();
        }
    }

#ifdef MULTIWD_DEBUG
    debug_print("Closing debug print and exiting with status %d\n", ret * -1);

    if(g_tty != NULL){
        if(fclose(g_tty) == 0){
            g_tty = NULL;
        }
    }
#endif
    return ret * -1;
}

// ---- public API ----

int multiwd_init(uint64_t flags, typeof(SIGABRT) trigger_type)
{
    //no lock as init creates mutexes
    int ret = multiwd_init_locked(flags, trigger_type);
    if(unlock_global() != 0){
        write_stderr("Init couldn't release lock!");
        multiwd_shutdown_locked(true);
        return -205;
    }
#ifdef MULTIWD_DEBUG
    debug_print("Releasing global lock for init\n");
#endif
    return ret;
}

int multiwd_add(uint64_t id, const struct timespec *timeout)
{
    return multiwd_add3(id, timeout, 0);
}

int multiwd_add3(uint64_t id, const struct timespec *timeout, int trigger_type)
{
    if(id >= MULTIWD_MAX_TIMERS_FINAL){
        errno = EOVERFLOW;
        return -200;
    }

#ifdef MULTIWD_DEBUG
    debug_print("Acquiring timer lock for add3 id: %lu\n", id);
#endif
    if(lock_timers() != 0){
#ifdef MULTIWD_DEBUG
    debug_print("Failed acquiring timer lock for add3 id: %lu\n", id);
#endif
        return -205;
    }
    int ret = multiwd_add3_locked(id, timeout, trigger_type);
    if(unlock_timers() != 0){
        write_stderr("Add couldn't release lock! Shutting down!");
        multiwd_shutdown_locked(true);
        return -205;
    }
#ifdef MULTIWD_DEBUG
    debug_print("Releasing timers lock for add3 id: %lu\n", id);
#endif
    return ret;
}

int multiwd_kick(uint64_t id, struct timespec *_Nullable remaining)
{
    if(id >= MULTIWD_MAX_TIMERS_FINAL){
        errno = EOVERFLOW;
        return -1;
    }

#ifdef MULTIWD_DEBUG
    debug_print("Acquiring timers lock for kick\n");
#endif
    if(lock_timers() != 0){
#ifdef MULTIWD_DEBUG
    debug_print("Failed acquiring timers lock for kick\n");
#endif
        return -205;
    }
    int ret = multiwd_kick_locked(id, remaining);
#ifdef MULTIWD_DEBUG
    debug_print("Releasing timers lock for kick\n");
#endif
    if(unlock_timers() != 0){
        write_stderr("Kick couldn't release lock! Shutting down!");
        multiwd_shutdown_locked(true);
        return -205;
    }
    return ret;
}

int multiwd_kick_minimal(uint64_t id)
{
    if(id >= MULTIWD_MAX_TIMERS_FINAL){
        errno = EOVERFLOW;
        return -1;
    }

    off64_t offset;
    bitmap_type bits;
    calculate_bitmap(id, &offset, &bits);

    g_timers.kicked[offset] |= bits;

    if((g_timers.used[offset] & bits) == 0){
        errno = EBADSLT;
        g_timers.kicked[offset] &= ~bits;
        return -1;
    }

    if(wakeup_timers_thread() != 0){
        return -1;
    }

    return 0;
}

int multiwd_kick_multiple(uint64_t offset, uint64_t bitmap)
{
    if(offset >= MULTIWD_TIMERS_BITMAP_SLOTS){
        errno = EOVERFLOW;
        return -1;
    }

#ifdef MULTIWD_DEBUG
    debug_print("Acquiring timers lock for kick_multiple\n");
#endif
    if(lock_timers() != 0){
#ifdef MULTIWD_DEBUG
    debug_print("Failed acquiring children lock for kick_multiple\n");
#endif
        return -205;
    }
    int ret = multiwd_kick_multiple_locked(offset, bitmap);
    if(unlock_timers() != 0){
        write_stderr("kick_multiple couldn't release lock! Shutting down!");
        multiwd_shutdown_locked(true);
        return -205;
    }
#ifdef MULTIWD_DEBUG
    debug_print("Releasing timers lock for kick_multipled\n");
#endif
    return ret;
}

int multiwd_kick_multiple_minimal(uint64_t offset, uint64_t bitmap)
{
    if(offset >= MULTIWD_TIMERS_BITMAP_SLOTS){
        errno = EOVERFLOW;
        return -1;
    }

    if((g_timers.used[offset] & bitmap) != bitmap){
        errno = EBADSLT;
        return -1;
    }

    g_timers.kicked[offset] |= bitmap;
    
    if(wakeup_timers_thread() != 0){
        return -1;
    }

    return 0;
}

int multiwd_remove(uint64_t id, struct timespec *_Nullable timeout)
{
    if(id >= MULTIWD_MAX_TIMERS_FINAL){
        errno = EOVERFLOW;
        return -1;
    }

#ifdef MULTIWD_DEBUG
    debug_print("Acquiring timers lock for remove\n");
#endif
    if(lock_timers() != 0){
#ifdef MULTIWD_DEBUG
    debug_print("Failed acquiring children lock for remove\n");
#endif
        return -205;
    }
    int ret = multiwd_remove_locked(id, timeout);
    if(unlock_timers() != 0){
        write_stderr("Remove couldn't release lock! Shutting down!");
        multiwd_shutdown_locked(true);
        return -205;
    }
#ifdef MULTIWD_DEBUG
    debug_print("Releasing timers lock for remove\n");
#endif
    return ret;
}

int multiwd_register_child(pid_t child_pid)
{
    return multiwd_register_child2(child_pid, 0);
}

int multiwd_register_child2(pid_t child_pid, typeof(SIGABRT) trigger_type)
{
#ifdef MULTIWD_DEBUG
    debug_print("Acquiring children lock for register_child2\n");
#endif
    if(lock_children() != 0){
#ifdef MULTIWD_DEBUG
    debug_print("Failed acquiring children lock for register_child2\n");
#endif
        return -205;
    }
    int ret = multiwd_register_child_locked2(child_pid, trigger_type);
    if(unlock_children() != 0){
        write_stderr("Register_child2 couldn't release lock! Shutting down!");
        multiwd_shutdown_locked(true);
        return -205;

    }
#ifdef MULTIWD_DEBUG
    debug_print("Releasing children lock for register_child2\n");
#endif
    return ret;
}

int multiwd_shutdown(void)
{
    if(!g_initialised || g_finalizing){
        errno = ENOTSUP;
        return -1;
    }
#ifdef MULTIWD_DEBUG
    debug_print("Acquiring global lock for shutdown\n");
#endif
    if(lock_global() != 0){
#ifdef MULTIWD_DEBUG
    debug_print("Failed acquiring global lock for shutdown\n");
#endif
        return -205;
    }
    return multiwd_shutdown_locked(true);
    //no unlock as shutdown destroys mutexes
}


