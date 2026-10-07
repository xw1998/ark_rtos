/**
* @file sys/time.h
* @brief struct timeval for Ark RTOS Apps - the select() timeout.
* @details Ark RTOS has no wall clock: the kernel keeps one boot relative
*          millisecond tick. So there is no gettimeofday() here, because a
*          "time of day" would have to be invented and a program would then
*          believe it. clock_gettime() in <time.h> reports that same tick,
*          and that is the honest call. This header exists so <sys/socket.h>
*          can give select() the POSIX timeout type.
* @note  Define ARKRTOS_POSIX_NO_TIMEVAL when the toolchain headers already
*        define struct timeval; a collision is a compile error, never a
*        silent misread.
* @author xw
*/
#ifndef __ARKRTOS_SYS_TIME_H__
#define __ARKRTOS_SYS_TIME_H__

#include "arkrtos_posix_types.h"

#ifndef ARKRTOS_POSIX_NO_TIMEVAL
struct timeval
{
    arkrtos_time_t     tv_sec;    /**< whole seconds                */
    arkrtos_useconds_t tv_usec;   /**< microseconds, 0..999999      */
};
#endif

#endif /* __ARKRTOS_SYS_TIME_H__ */
