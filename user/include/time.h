/*
SurfOS user library: time
----------------------
File: time.h    Date: 10/5/26
----------------------
time() is the kernel's CMOS clock in seconds since 1970. The clock keeps local
time and SurfOS knows no time zone, so localtime() and gmtime() give the same
broken-down time, and mktime() is their inverse. File times (st_mtime) are on
the same scale.
*/

#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

struct tm {
    int tm_sec;                 /* 0-59 */
    int tm_min;                 /* 0-59 */
    int tm_hour;                /* 0-23 */
    int tm_mday;                /* 1-31 */
    int tm_mon;                 /* 0-11 */
    int tm_year;                /* years since 1900 */
    int tm_wday;                /* 0-6, Sunday = 0 */
    int tm_yday;                /* 0-365 */
    int tm_isdst;               /* always 0 */
};

time_t time(time_t *t);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t mktime(struct tm *tm);                   /* normalizes the fields, sets tm_wday and tm_yday */
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
    /* %Y %m %d %H %M %S %a %b, also %A %B %y %e %j %p %I %F %T %D %R %%; 0 when it does not fit */

#endif
