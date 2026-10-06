/*
SurfOS user library: calendar time
--------------------
File: time.c    Date: 10/5/26
--------------------
The CMOS clock keeps local time and there is no time zone, so seconds since
1970 are simply that clock's reading counted from 1970-01-01 00:00:00, the way
the kernel's time call and the FAT driver count them. Days and dates convert
with Howard Hinnant's civil calendar algorithms.
*/

#include <stddef.h>
#include <string.h>
#include <time.h>

static const char *const wday_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const month_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                           "August", "September", "October", "November", "December" };

static long days_from_civil(long y, int m, int d) {
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, long *y, int *m, int *d) {
    long era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if(*m <= 2) (*y)++;
}

struct tm *gmtime_r(const time_t *t, struct tm *tm) {
    long s = *t, days = s / 86400, rem = s % 86400, y;
    int m, d;
    if(rem < 0) { rem += 86400; days--; }
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem / 60 % 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7);           /* 1970-01-01 was a Thursday */
    civil_from_days(days, &y, &m, &d);
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm->tm_isdst = 0;
    return tm;
}

struct tm *gmtime(const time_t *t) {
    static struct tm tm;
    return gmtime_r(t, &tm);
}

struct tm *localtime_r(const time_t *t, struct tm *tm) {
    return gmtime_r(t, tm);
}

struct tm *localtime(const time_t *t) {
    return gmtime(t);
}

time_t mktime(struct tm *tm) {
    long y = tm->tm_year + 1900L, mon = tm->tm_mon;
    long long secs;
    time_t t;
    y += mon / 12;
    mon %= 12;
    if(mon < 0) { mon += 12; y--; }
    secs = (long long)(days_from_civil(y, (int)mon + 1, 1) + tm->tm_mday - 1) * 86400
         + (long long)tm->tm_hour * 3600 + (long long)tm->tm_min * 60 + tm->tm_sec;
    if(secs < -2147483647LL - 1 || secs > 2147483647LL) return (time_t)-1;
    t = (time_t)secs;
    gmtime_r(&t, tm);                                   /* normalized fields, weekday and day of the year */
    return t;
}

/* append to s, failing when it does not fit */
static int put(char *s, size_t max, size_t *n, const char *text) {
    size_t len = strlen(text);
    if(*n + len >= max) return -1;
    memcpy(s + *n, text, len);
    *n += len;
    return 0;
}

static void two(char *buf, int v, char lead) {
    buf[0] = v >= 10 ? (char)('0' + v / 10 % 10) : lead;
    buf[1] = (char)('0' + v % 10);
    buf[2] = 0;
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    char buf[16];
    for(; *fmt; fmt++) {
        const char *text = buf;
        if(*fmt != '%' || !fmt[1]) {
            buf[0] = *fmt;
            buf[1] = 0;
        } else {
            switch(*++fmt) {
            case 'Y': {
                long y = tm->tm_year + 1900L;
                int i = 0, k = 0;
                char rev[12];
                if(y < 0) { buf[k++] = '-'; y = -y; }
                do { rev[i++] = (char)('0' + y % 10); y /= 10; } while(y);
                while(i < 4) rev[i++] = '0';
                while(i) buf[k++] = rev[--i];
                buf[k] = 0;
                break;
            }
            case 'y': two(buf, (tm->tm_year % 100 + 100) % 100, '0'); break;
            case 'm': two(buf, tm->tm_mon + 1, '0'); break;
            case 'd': two(buf, tm->tm_mday, '0'); break;
            case 'e': two(buf, tm->tm_mday, ' '); break;
            case 'H': two(buf, tm->tm_hour, '0'); break;
            case 'I': two(buf, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, '0'); break;
            case 'M': two(buf, tm->tm_min, '0'); break;
            case 'S': two(buf, tm->tm_sec, '0'); break;
            case 'p': text = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'j':
                buf[0] = (char)('0' + (tm->tm_yday + 1) / 100 % 10);
                two(buf + 1, (tm->tm_yday + 1) % 100, '0');
                break;
            case 'a':
            case 'A':
                if(tm->tm_wday < 0 || tm->tm_wday > 6) { text = "?"; break; }
                strcpy(buf, wday_names[tm->tm_wday]);
                if(*fmt == 'a') buf[3] = 0;
                break;
            case 'b':
            case 'h':
            case 'B':
                if(tm->tm_mon < 0 || tm->tm_mon > 11) { text = "?"; break; }
                strcpy(buf, month_names[tm->tm_mon]);
                if(*fmt != 'B') buf[3] = 0;
                break;
            case 'F': {                                 /* %Y-%m-%d */
                char sub[16];
                size_t k = strftime(sub, sizeof(sub), "%Y-%m-%d", tm);
                if(!k) return 0;
                strcpy(buf, sub);
                break;
            }
            case 'T': strftime(buf, sizeof(buf), "%H:%M:%S", tm); break;
            case 'R': strftime(buf, sizeof(buf), "%H:%M", tm); break;
            case 'D': strftime(buf, sizeof(buf), "%m/%d/%y", tm); break;
            case 'n': text = "\n"; break;
            case 't': text = "\t"; break;
            case '%': text = "%"; break;
            default:
                buf[0] = '%';
                buf[1] = *fmt;
                buf[2] = 0;
                break;
            }
        }
        if(put(s, max, &n, text)) return 0;
    }
    if(n < max) s[n] = 0;
    return n;
}
