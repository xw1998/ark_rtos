/**
* @file arkrtos_posix_types.h
* @brief Base POSIX types for the Ark RTOS compatibility layer.
* @details Kept in one place so every POSIX header agrees on the width of
*          ssize_t / off_t / time_t. The widths are fixed (int32 / uint32)
*          because Ark RTOS is a 32-bit only kernel and pretending otherwise
*          would only hide porting mistakes.
*
*          struct timespec: our definition may collide with a toolchain one,
*          because only the toolchain knows its own guard macro. If your
*          platform header already defines it, define ARKRTOS_POSIX_NO_TIMESPEC
*          before including this file. A collision is a compile error, never
*          a silent misread.
*
* @note  Dependency-free apart from arkrtos_types.h.
* @author xw
*/
#ifndef __ARKRTOS_POSIX_TYPES_H__
#define __ARKRTOS_POSIX_TYPES_H__

#include "arkrtos_types.h"

typedef int32  arkrtos_ssize_t;
typedef int32  arkrtos_off_t;
typedef int32  arkrtos_pid_t;
typedef uint32 arkrtos_mode_t;
typedef int32  arkrtos_clockid_t;
typedef uint32 arkrtos_time_t;
typedef int32  arkrtos_useconds_t;

/** POSIX clock ids. Only MONOTONIC is backed by anything here: Ark RTOS has a
 *  boot relative tick, there is no wall clock, so REALTIME is aliased to it
 *  rather than reported as a different (and wrong) time base. */
#define ARKRTOS_CLOCK_REALTIME   0
#define ARKRTOS_CLOCK_MONOTONIC  1

/* POSIX spellings of the same types, so ordinary POSIX source compiles with
 * them unchanged. Fixed width on purpose: Ark RTOS is 32-bit only. Define
 * ARKRTOS_POSIX_NO_STD_TYPES if your toolchain headers already provide any of
 * these - a collision is a compile error, never a silent misread. */
#ifndef ARKRTOS_POSIX_NO_STD_TYPES
typedef arkrtos_ssize_t    ssize_t;
typedef arkrtos_off_t      off_t;
typedef arkrtos_pid_t      pid_t;
typedef arkrtos_mode_t     mode_t;
typedef arkrtos_time_t     time_t;
typedef arkrtos_useconds_t useconds_t;
#endif

#ifndef ARKRTOS_POSIX_NO_TIMESPEC
struct timespec
{
    arkrtos_time_t tv_sec;
    int32        tv_nsec;
};
#endif

#endif /* __ARKRTOS_POSIX_TYPES_H__ */
