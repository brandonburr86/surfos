/*
SurfOS Undertow: what Mbed TLS asks of the platform
----------------------
File: tls_platform.c  Date: 10/11/26
----------------------
tls_platform.h says what each hook is for. Nothing here keeps state that
matters beyond one process: the clock pointer is set only around a handshake,
and the monotonic clock's high half only counts uptime's wraps.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <surf.h>
#include "undertow.h"
#include "config.h"
#include "tls_platform.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/entropy.h"

/* tls_config.h promises a 64-bit mbedtls_time_t: certificates run past 2038 */
typedef char ut_tls_time_is_64_bits[sizeof(mbedtls_time_t) == 8 ? 1 : -1];

#define ENTROPY_CHUNK 256               /* getentropy()'s limit */

const char *ut_tls_roots_path = UT_CA_PATH;

static const struct ut_clock *active_clock;
static int entropy_errno;

void *ut_tls_calloc(size_t n, size_t size) {
    return calloc(n, size);
}

void ut_tls_free(void *p) {
    free(p);
}

int ut_tls_vsnprintf(char *s, size_t n, const char *fmt, va_list ap) {
    return vsnprintf(s, n, fmt, ap);
}

int ut_tls_snprintf(char *s, size_t n, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

void ut_tls_set_clock(const struct ut_clock *c) {
    active_clock = c;
}

long long ut_tls_time(long long *t) {
    long long now = -1;
    if(active_clock) {
        time_t utc = ut_clock_now(active_clock);
        if(ut_clock_plausible(utc)) now = (long long)utc;
    }
    if(t) *t = now;
    return now;
}

struct tm *mbedtls_platform_gmtime_r(const long long *tt, struct tm *tm_buf) {
    time_t t;
    if(*tt < 0 || *tt > 0x7fffffffLL) return NULL;
    t = (time_t)*tt;
    return gmtime_r(&t, tm_buf);
}

/* uptime() in milliseconds, made 64 bits by counting its wraps (it wraps after 49.7 days;
   Mbed TLS asks often enough that no wrap goes unseen) */
mbedtls_ms_time_t mbedtls_ms_time(void) {
    static unsigned int last;
    static unsigned long long wraps;
    unsigned int now = uptime();
    if(now < last) wraps += 0x100000000ULL;
    last = now;
    return (mbedtls_ms_time_t)(wraps + now);
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    size_t done = 0;
    (void)data;
    *olen = 0;
    while(done < len) {
        size_t n = len - done < ENTROPY_CHUNK ? len - done : ENTROPY_CHUNK;
        if(getentropy(output + done, n) != 0) {
            entropy_errno = errno ? errno : EIO;
            mbedtls_platform_zeroize(output, done);
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        done += n;
    }
    *olen = len;
    return 0;
}

int ut_tls_entropy_errno(void) {
    return entropy_errno;
}
