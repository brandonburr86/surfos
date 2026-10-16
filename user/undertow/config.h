/*
SurfOS Undertow: settings and the clock that certificates are checked against
----------------------
File: config.h  Date: 10/11/26
----------------------
/initrd/etc/undertow.conf (UT_CONF_PATH) holds two kinds of line, besides blank
lines and # comments:

  rtc_offset_minutes=N   the hardware clock's offset from UTC, -840..840: a clock
                         that keeps US Central Daylight Time (UTC-5) is -300.
                         UTC = the clock's reading - N minutes. The default is 0:
                         the clock keeps UTC (as QEMU's does).
  host=NAME ADDRESS      a fixed IPv4 address for NAME, used instead of DNS (like
                         curl's --resolve; the test images map their fixture
                         names this way). At most UT_HOSTS_MAX.

SurfOS has no time zone: time() is the hardware clock (CMOS) counted as if it
were UTC. The browser converts it with the offset above, or uses the time the
user gave with "clock YYYY-MM-DD HH:MM:SS" for this session, advanced by the
uptime since. Neither changes the hardware clock or any file's time.
*/

#ifndef UNDERTOW_CONFIG_H
#define UNDERTOW_CONFIG_H

#include <stddef.h>
#include <time.h>

#define UT_HOSTS_MAX      16
#define UT_OFFSET_MAX     840               /* minutes: UTC+14 and UTC-14 */
#define UT_CLOCK_FLOOR    1790294400        /* 2026-09-25 00:00:00 UTC, the date of the CA bundle */
#define UT_CLOCK_TEXT     40                /* "2026-10-11 14:05:09 UTC" and room the compiler cannot rule out */

struct ut_clock {
    int rtc_offset_minutes;         /* the hardware clock's offset from UTC, -840..840 */
    int override_set;               /* 1: the user set the time for this session */
    time_t override_utc;            /* that time */
    unsigned int override_ticks;    /* ut_now() when it was set */
};

struct ut_host {
    char name[254];                 /* lowercase */
    unsigned int ip;                /* network byte order */
};

struct ut_config {
    struct ut_clock clock;
    struct ut_host hosts[UT_HOSTS_MAX];
    int host_count;
};

/* The defaults: offset 0, no override, no hosts. */
void ut_config_init(struct ut_config *c);

/* Read the settings from text (len bytes) or from the file path. A file that does not exist
   leaves the defaults and is no error. Anything else that is wrong (an unknown setting, a
   value out of range, a line over 512 bytes, a file over 16 KB) is: -1, a message naming the
   line, and c unchanged. On success c holds the defaults changed by what the text says. */
int ut_config_parse(struct ut_config *c, const char *text, size_t len, char *err, size_t cap);
int ut_config_load(struct ut_config *c, const char *path, char *err, size_t cap);

/* NAME's fixed address (any case): 0 and *ip, or -1 when the table has none. */
int ut_config_host(const struct ut_config *c, const char *name, unsigned int *ip);

/* UTC now: the session's time plus the uptime since it was set, or the hardware clock
   less the offset. (time_t)-1 when the result is outside 1970..2038-01-19 03:14:07. */
time_t ut_clock_now(const struct ut_clock *c);

/* Is t a time certificates can be checked against: not -1 and not before UT_CLOCK_FLOOR
   (the clock of a machine whose battery ran down says 2000 or 2004)? */
int ut_clock_plausible(time_t t);

/* Set this session's UTC from exactly "YYYY-MM-DD HH:MM:SS": a real date and time from
   1970-01-01 00:00:00 to 2038-01-19 03:14:07 (no leap second, no other form). 0, or -1
   with a message and c unchanged. */
int ut_clock_set(struct ut_clock *c, const char *utc, char *err, size_t cap);

/* t as "YYYY-MM-DD HH:MM:SS UTC" (out has UT_CLOCK_TEXT bytes); "unknown" for -1. */
void ut_clock_format(time_t t, char out[UT_CLOCK_TEXT]);

#endif
