/*
SurfCPU firmware: text formatting
--------------------
File: fmt.h Date: 10/15/26 (SurfCPU)
--------------------
A small printf without a C library, shared by SurfBoot, the diagnostics and the host
tests (the same source compiles for RV32 and natively). Conversions: %c %s %d %i %u %x %X
%p %% with the flags '-' and '0', a width, a precision for %s, and the length modifiers
l (ignored, longs are 32 bits on RV32) and ll (64-bit values).

On RV32 uint32_t is `unsigned long`, on the host `unsigned int`; both print with %u and %x
here. Format checking is enabled on the host build only, where %u is exactly uint32_t.
*/
#ifndef SURFCPU_FMT_H
#define SURFCPU_FMT_H

#include <stdarg.h>
#include <stddef.h>

#ifdef SURF_HOST
#define FMT_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define FMT_PRINTF(f, a)
#endif

typedef void (*fmt_out_fn)(void *ctx, char c);

/* Formats into out(ctx, c); returns the number of characters produced. */
int fmt_vformat(fmt_out_fn out, void *ctx, const char *fmt, va_list ap);

/* Like snprintf: always NUL-terminates when size > 0; returns the full length. */
int fmt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int fmt_snprintf(char *buf, size_t size, const char *fmt, ...) FMT_PRINTF(3, 4);

#endif /* SURFCPU_FMT_H */
