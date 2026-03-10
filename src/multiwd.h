#ifndef MULTIWD_H
#define MULTIWD_H

#define _GNU_SOURCE

#include <stdint.h>
#include <time.h>
#include <signal.h>

#include <sys/types.h>

#include "multiwd_config.h"

#if defined(__GNUC__)
#define _Nullable
#define _Nonnull
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MULTIWD_DEFAULT                     = 0,
    MULTIWD_COUPLE_NONE                 = 1 << 0,
    MULTIWD_COUPLE_CHILD_WITH_PARENT    = 1 << 1,   // child trigers if parent trigered
    MULTIWD_COUPLE_PARENT_WITH_CHILD    = 1 << 2,   // parent trigers if child is trigered
    MULTIWD_COUPLE_BOTH                 = MULTIWD_COUPLE_PARENT_WITH_CHILD | MULTIWD_COUPLE_CHILD_WITH_PARENT,

    MULTIWD_AUTOINIT_IN_CHILD           = 1 << 3,

    MULTIWD_AUTOREGISTER_CHILDREN       = 1 << 4,
};

/*
 * Setups multiwd's internal structure.
 * flags consist from enum above, defaults to MULTIWD_COUPLE_BOTH | MULTIWD_AUTOREGISTER_CHILDREN | MULTIWD_AUTOINIT_IN_CHILD
 * trigger_type represents signal type that is sent when watchdog expires, defaults to SIGABRT.
 * use zero for default behaviour
 * Secondary calls before shutdown result in error return value.
 */
int multiwd_init(uint64_t flags, typeof(SIGABRT) trigger_type);

/*
 * Adds watchdog under id.
 * Watchdog starts immediatly.
 * id must be >= 0 && <= MULTIWD_MAX_TIMERS.
 * Secondary calls before remove result in error return value.
 * id value of UINT64_MAX is reserved.
 */
int multiwd_add(uint64_t id, const struct timespec *timeout);

/*
 * Expands add by specifying signal to be sent when watchdog triggers.
 * Use 0 global behaviour.
 */
int multiwd_add3(uint64_t id, const struct timespec *timeout, typeof(SIGABRT) trigger_type);

/*
 * Resets watchdog under it to timeout specified when adding.
 * Optionally returs remaining timeout in remaining.
 * id must be >= 0 && <= MULTIWD_MAX_TIMERS.
 */
int multiwd_kick(uint64_t id, struct timespec *_Nullable remaining);

/*
 * Functionaly identical to kick, but suitable for calls in signal handlers.
 * Differentiates in implementation.
 * May have slower response time.
 * id must be >= 0 && <= MULTIWD_MAX_TIMERS.
 */
int multiwd_kick_minimal(uint64_t id);

/*
 * Kicks multiple watchdogs based on bitmap.
 * Bitmap is represented as array of uint64_t values large enough to fit MULTIWD_MAX_TIMERS,
 * each bit representing sequentially increasing watchdog id starting from 0 at offset = 0
 * and most significant bit.
 */
int multiwd_kick_multiple(uint64_t offset, uint64_t bitmap);

/*
 * Combination of kick_multiple and kick_minimal.
 */
int multiwd_kick_multiple_minimal(uint64_t offset, uint64_t bitmap);

/*
 * Stops and removes watchdog under id.
 * Optionally returns configured timeout
 * id must be >= 0 && <= MULTIWD_MAX_TIMERS.
 */
int multiwd_remove(uint64_t id, struct timespec *_Nullable timeout);

/*
 * Registers child for coupling.
 */
int multiwd_register_child(pid_t child_pid);

/*
 *  Expands multiwd_register_child functionality by specifying signal to be sent
 */
int multiwd_register_child2(pid_t child_pid, typeof(SIGABRT) trigger_type);

/*
 * Wraps fork calls to autoregister children;
 * Assumes MULTIWD_AUTOREGISTER_CHILDREN
 * Compatible but independed with MULTIWD_WRAP_FORK
 */
pid_t multiwd_fork();

/*
 * Stops and removes all watchdogs and releases resources
 */
int multiwd_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif //MULTIWD_H
