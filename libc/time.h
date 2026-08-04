#pragma once
/*
 * Minimal <time.h> for TAPP builds.
 *
 * TYPES ONLY. The firmware exports no time entry point at all — not time(), mktime(),
 * localtime(), gmtime(), difftime(), clock() or strftime() — so none of them are declared
 * here: calling one must be a compile error, not a load-time missing import.
 *
 * struct tm exists because portable emulator cores include <time.h> just to name it in an
 * RTC-setter prototype (e.g. Peanut-GB's gb_set_rtc). Declaring the type costs nothing and
 * imports nothing. If a tapp needs real wall-clock time, get it from the firmware API.
 */

#include <stddef.h>

struct tm {
    int tm_sec;   /* seconds after the minute [0,60] */
    int tm_min;   /* minutes after the hour [0,59] */
    int tm_hour;  /* hours since midnight [0,23] */
    int tm_mday;  /* day of the month [1,31] */
    int tm_mon;   /* months since January [0,11] */
    int tm_year;  /* years since 1900 */
    int tm_wday;  /* days since Sunday [0,6] */
    int tm_yday;  /* days since January 1 [0,365] */
    int tm_isdst; /* daylight saving flag */
};
