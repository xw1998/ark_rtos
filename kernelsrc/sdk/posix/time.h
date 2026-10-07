/**
* @file time.h
* @brief Minimal <time.h> for Ark RTOS Apps.
* @details Only the calls that make sense without a wall clock: the kernel
*          keeps a boot relative millisecond tick, so time() returns whole
*          seconds since boot and clock_gettime() reports that same base.
*          There is no calendar, so no localtime / strftime here - a program
*          needing those should bring its own.
* @note  If your toolchain's <time.h> is pulled in as well, define
*        ARKRTOS_POSIX_NO_TIMESPEC so struct timespec is not defined twice.
* @author xw
*/
#ifndef __ARKRTOS_TIME_H__
#define __ARKRTOS_TIME_H__

#include "arkrtos_posix_types.h"
#include "unistd.h"

#define CLOCK_REALTIME   ARKRTOS_CLOCK_REALTIME
#define CLOCK_MONOTONIC  ARKRTOS_CLOCK_MONOTONIC

arkrtos_time_t arkrtos_posix_time(arkrtos_time_t *tloc);
int          arkrtos_posix_clock_gettime(arkrtos_clockid_t clk, struct timespec *ts);
/* Kept for source compatibility; Ark RTOS has nothing to yield to but the
 * scheduler, so this is a plain thread wait of 0 ms. */
int          arkrtos_posix_sched_yield(void);

#define time(t)              arkrtos_posix_time(t)
#define clock_gettime(c, ts) arkrtos_posix_clock_gettime((c), (ts))
#define sched_yield()        arkrtos_posix_sched_yield()

#endif /* __ARKRTOS_TIME_H__ */
