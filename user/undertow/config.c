/*
SurfOS Undertow: settings and the clock that certificates are checked against
----------------------
File: config.c  Date: 10/11/26
----------------------
config.h describes the file and the clock. Dates convert with Howard Hinnant's
civil calendar algorithm, in 32-bit arithmetic plus 64-bit multiplication only,
so nothing here needs libgcc's 64-bit division.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <surf.h>
#include "undertow.h"
#include "config.h"

#define CONF_FILE_MAX 16384
#define CONF_LINE_MAX 512
#define TIME_MAX      2147483647LL      /* 2038-01-19 03:14:07 UTC, the end of a 32-bit time_t */

void ut_config_init(struct ut_config *c) {
    memset(c, 0, sizeof(*c));
}

/**** dates ****/

static long days_from_civil(long y, int m, int d) {
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int days_in_month(int y, int m) {
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if(m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return days[m - 1];
}

time_t ut_clock_now(const struct ut_clock *c) {
    long long t;
    if(c->override_set) {
        t = (long long)c->override_utc + (ut_now() - c->override_ticks) / 1000;
    } else {
        time_t rtc = time(NULL);
        if(rtc == (time_t)-1) return (time_t)-1;
        t = (long long)rtc - (long long)c->rtc_offset_minutes * 60;
    }
    if(t < 0 || t > TIME_MAX) return (time_t)-1;
    return (time_t)t;
}

int ut_clock_plausible(time_t t) {
    return t != (time_t)-1 && (long long)t >= UT_CLOCK_FLOOR;
}

static int digits(const char *s, int n) {
    int v = 0;
    while(n--) v = v * 10 + (*s++ - '0');
    return v;
}

int ut_clock_set(struct ut_clock *c, const char *s, char *err, size_t cap) {
    static const char form[] = "dddd-dd-dd dd:dd:dd";
    int i, year, month, day, hour, min, sec;
    long long t;
    if(!s || strnlen(s, sizeof(form)) != sizeof(form) - 1)
        return ut_error(err, cap, "give the time in UTC as clock YYYY-MM-DD HH:MM:SS");
    for(i = 0; form[i]; i++) {
        int digit = s[i] >= '0' && s[i] <= '9';
        if(form[i] == 'd' ? !digit : s[i] != form[i])
            return ut_error(err, cap, "give the time in UTC as clock YYYY-MM-DD HH:MM:SS");
    }
    year = digits(s, 4); month = digits(s + 5, 2); day = digits(s + 8, 2);
    hour = digits(s + 11, 2); min = digits(s + 14, 2); sec = digits(s + 17, 2);
    if(month < 1 || month > 12) return ut_error(err, cap, "%.19s: the month must be 01 to 12", s);
    if(day < 1 || day > days_in_month(year, month)) return ut_error(err, cap, "%.10s is not a date", s);
    if(hour > 23 || min > 59 || sec > 59)
        return ut_error(err, cap, "%.19s: the time of day must be 00:00:00 to 23:59:59", s);
    if(year < 1970) return ut_error(err, cap, "%.19s: the clock cannot go back before 1970", s);
    t = (long long)days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
    if(t > TIME_MAX) return ut_error(err, cap, "%.19s: the clock cannot go past 2038-01-19 03:14:07 UTC", s);
    c->override_utc = (time_t)t;
    c->override_ticks = ut_now();
    c->override_set = 1;
    return 0;
}

void ut_clock_format(time_t t, char out[UT_CLOCK_TEXT]) {
    long days, rem, y, era, doe, yoe, doy, mp;
    int m, d;
    if(t == (time_t)-1 || (long long)t < 0 || (long long)t > TIME_MAX) {
        strcpy(out, "unknown");
        return;
    }
    days = (long)(t / 86400);
    rem = (long)(t % 86400);
    era = (days + 719468) / 146097;                 /* civil_from_days, for days >= 0 */
    doe = days + 719468 - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if(m <= 2) y++;
    snprintf(out, UT_CLOCK_TEXT, "%04ld-%02d-%02d %02ld:%02ld:%02ld UTC",
             y, m, d, rem / 3600, rem / 60 % 60, rem % 60);
}

/**** the settings file ****/

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

static char *trim(char *s) {
    char *end;
    while(*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while(end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = 0;
    return s;
}

/* "a.b.c.d", each 0-255 without a sign: network byte order */
static int parse_ipv4(const char *s, unsigned int *ip) {
    unsigned char b[4];
    int i;
    for(i = 0; i < 4; i++) {
        int v = 0, n = 0;
        while(*s >= '0' && *s <= '9' && n < 3) { v = v * 10 + (*s++ - '0'); n++; }
        if(!n || v > 255 || (n > 1 && s[-n] == '0')) return -1;   /* no leading zeros: 010 is not 8 or 10 */
        b[i] = (unsigned char)v;
        if(i < 3 && *s++ != '.') return -1;
    }
    if(*s) return -1;
    memcpy(ip, b, 4);
    return 0;
}

static int valid_name(const char *s) {
    size_t n = strlen(s), label = 0, i;
    if(!n || n > 253) return 0;
    for(i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if(c == '.') {
            if(!label || s[i - 1] == '-') return 0;
            label = 0;
        } else if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') {
            if((!label && c == '-') || ++label > 63) return 0;
        } else return 0;
    }
    return s[n - 1] != '-';
}

static int set_offset(struct ut_config *c, const char *v, unsigned int line, char *err, size_t cap) {
    const char *p = v;
    int sign = 1, value = 0, n = 0;
    if(*p == '-' || *p == '+') sign = *p++ == '-' ? -1 : 1;
    while(*p >= '0' && *p <= '9' && n < 4) { value = value * 10 + (*p++ - '0'); n++; }
    if(!n || *p || value > UT_OFFSET_MAX)
        return ut_error(err, cap, "undertow.conf line %u: rtc_offset_minutes must be a whole number "
                        "from -%d to %d", line, UT_OFFSET_MAX, UT_OFFSET_MAX);
    c->clock.rtc_offset_minutes = sign * value;
    return 0;
}

static int add_host(struct ut_config *c, char *v, unsigned int line, char *err, size_t cap) {
    char *name = v, *addr = v;
    struct ut_host *h;
    unsigned int ip, dup;
    int i;
    while(*addr && *addr != ' ' && *addr != '\t') addr++;
    if(*addr) *addr++ = 0;
    addr = trim(addr);
    if(!valid_name(name) || parse_ipv4(addr, &ip))
        return ut_error(err, cap, "undertow.conf line %u: write host=NAME ADDRESS, "
                        "with a host name and an IPv4 address", line);
    if(!ut_config_host(c, name, &dup))
        return ut_error(err, cap, "undertow.conf line %u: %s is given twice", line, name);
    if(c->host_count == UT_HOSTS_MAX)
        return ut_error(err, cap, "undertow.conf line %u: more than %d host lines", line, UT_HOSTS_MAX);
    h = &c->hosts[c->host_count++];
    for(i = 0; name[i]; i++) h->name[i] = (char)lower((unsigned char)name[i]);
    h->name[i] = 0;
    h->ip = ip;
    return 0;
}

static int parse_line(struct ut_config *c, char *s, unsigned int line, char *err, size_t cap) {
    char *eq, *key, *value;
    s = trim(s);
    if(!*s || *s == '#') return 0;
    eq = strchr(s, '=');
    if(!eq) return ut_error(err, cap, "undertow.conf line %u: expected setting=value", line);
    *eq = 0;
    key = trim(s);
    value = trim(eq + 1);
    if(!strcmp(key, "rtc_offset_minutes")) return set_offset(c, value, line, err, cap);
    if(!strcmp(key, "host")) return add_host(c, value, line, err, cap);
    return ut_error(err, cap, "undertow.conf line %u: unknown setting \"%.40s\"", line, key);
}

int ut_config_parse(struct ut_config *c, const char *text, size_t len, char *err, size_t cap) {
    struct ut_config *n;
    char line[CONF_LINE_MAX + 1];
    size_t i = 0;
    unsigned int number = 0;
    if(len > CONF_FILE_MAX) return ut_error(err, cap, "undertow.conf is over %u bytes", CONF_FILE_MAX);
    n = malloc(sizeof(*n));
    if(!n) return ut_error(err, cap, "out of memory for undertow.conf");
    ut_config_init(n);
    while(i < len) {
        size_t start = i, end;
        number++;
        while(i < len && text[i] != '\n') i++;
        end = i;
        if(i < len) i++;
        if(end > start && text[end - 1] == '\r') end--;
        if(end - start > CONF_LINE_MAX) {
            free(n);
            return ut_error(err, cap, "undertow.conf line %u is over %u bytes", number, CONF_LINE_MAX);
        }
        memcpy(line, text + start, end - start);
        line[end - start] = 0;
        if(strlen(line) != end - start) {
            free(n);
            return ut_error(err, cap, "undertow.conf line %u has a NUL byte", number);
        }
        if(parse_line(n, line, number, err, cap)) {
            free(n);
            return -1;
        }
    }
    memcpy(c, n, sizeof(*c));
    free(n);
    return 0;
}

int ut_config_load(struct ut_config *c, const char *path, char *err, size_t cap) {
    FILE *f = fopen(path, "r");
    char *buf;
    size_t len;
    int rc, failed;
    if(!f) {
        if(errno == ENOENT) return 0;
        return ut_error(err, cap, "%s: %s", path, strerror(errno));
    }
    buf = malloc(CONF_FILE_MAX + 1);
    if(!buf) {
        fclose(f);
        return ut_error(err, cap, "out of memory for undertow.conf");
    }
    len = fread(buf, 1, CONF_FILE_MAX + 1, f);
    failed = ferror(f);
    fclose(f);
    if(failed) rc = ut_error(err, cap, "%s: cannot be read", path);
    else rc = ut_config_parse(c, buf, len, err, cap);
    free(buf);
    return rc;
}

int ut_config_host(const struct ut_config *c, const char *name, unsigned int *ip) {
    int i;
    for(i = 0; i < c->host_count; i++) {
        const char *a = c->hosts[i].name, *b = name;
        while(*a && *a == lower((unsigned char)*b)) { a++; b++; }
        if(!*a && !*b) {
            *ip = c->hosts[i].ip;
            return 0;
        }
    }
    return -1;
}
