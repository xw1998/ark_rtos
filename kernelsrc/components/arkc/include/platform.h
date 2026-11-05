/* Ark C Compiler - platform.h
 * Platform layer: logging and abort.
 * The minimal API that hides host / freestanding differences.
 */
#ifndef ARK_PLATFORM_H
#define ARK_PLATFORM_H

#include "common.h"
#include <stdarg.h>

/* Debug log output, printf-style */
void ark_log(const char *fmt, ...);

/* Fatal error: report and stop */
void ark_abort(const char *msg);

/* Date/time strings for the C99 __DATE__/__TIME__ macros:
 *   date_buf receives "Mmm dd yyyy" (11 chars, buffer must hold 12)
 *   time_buf receives "hh:mm:ss" (8 chars, buffer must hold 9)
 * Kept here so freestanding builds need no <time.h>
 * (permitted by C99 7.1.1). */
void ark_get_date_time(char *date_buf, char *time_buf);

#endif /* ARK_PLATFORM_H */
