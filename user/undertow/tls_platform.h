/*
SurfOS Undertow: what Mbed TLS asks of the platform
----------------------
File: tls_platform.h  Date: 10/11/26
----------------------
tls_config.h points Mbed TLS's platform macros here (MBEDTLS_PLATFORM_STD_MEM_HDR
makes every Mbed TLS unit include this header), and tls_platform.c implements
them on SurfOS's user space: memory and formatting from libsurf, randomness
only from getentropy() (RDRAND; a failure is a failure, never a weaker source),
the monotonic clock from uptime(), and the UTC clock that certificates are
checked against from ut_clock_now() of the settings transport.c is connecting
with. The header stands alone: it is included before Mbed TLS's own types.
*/

#ifndef UNDERTOW_TLS_PLATFORM_H
#define UNDERTOW_TLS_PLATFORM_H

#include <stddef.h>
#include <stdarg.h>

struct ut_clock;
struct tm;

/* mbedtls_calloc and mbedtls_free: libsurf's calloc and free */
void *ut_tls_calloc(size_t n, size_t size);
void ut_tls_free(void *p);

/* mbedtls_snprintf and mbedtls_vsnprintf: C99's, libsurf's formatter */
int ut_tls_snprintf(char *s, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int ut_tls_vsnprintf(char *s, size_t n, const char *fmt, va_list ap) __attribute__((format(printf, 3, 0)));

/* mbedtls_time (MBEDTLS_PLATFORM_TIME_MACRO; long long is mbedtls_time_t): UTC from the clock
   ut_tls_set_clock() named, -1 when none is set or it says nothing plausible, so that a
   certificate is never checked against a time nobody vouched for */
long long ut_tls_time(long long *t);

/* The clock certificates are checked against while transport.c runs a handshake: its
   settings' clock (Mbed TLS's time callback has no context argument; a process has one
   browser). NULL afterwards. */
void ut_tls_set_clock(const struct ut_clock *c);

/* Mbed TLS's entropy source (MBEDTLS_ENTROPY_HARDWARE_ALT): getentropy() in pieces of at most
   256 bytes. ut_tls_entropy_errno() is errno of the last failure, 0 if none: ENOSYS when the CPU
   has no RDRAND, EIO when it failed. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
int ut_tls_entropy_errno(void);

/* MBEDTLS_PLATFORM_GMTIME_R_ALT: mbedtls_time_t (long long) as UTC; NULL outside 0..2^31-1,
   which is all ut_clock_now() ever gives */
struct tm *mbedtls_platform_gmtime_r(const long long *tt, struct tm *tm_buf);

/* The file transport.c reads its trust roots from: UT_CA_PATH; the host tests point it at
   their own certificates. */
extern const char *ut_tls_roots_path;

#endif
