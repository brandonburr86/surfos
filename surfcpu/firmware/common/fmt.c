/*
SurfCPU firmware: text formatting
--------------------
File: fmt.c Date: 10/15/26 (SurfCPU)
--------------------
See fmt.h. No floating point, no allocation; 64-bit conversions use libgcc's division
helpers on RV32 (the rv32im/ilp32 libgcc).
*/
#include <stdint.h>
#include "fmt.h"

static void emit(fmt_out_fn out, void *ctx, char c, int *n)
{
    out(ctx, c);
    (*n)++;
}

static void pad(fmt_out_fn out, void *ctx, char c, int count, int *n)
{
    while (count-- > 0)
        emit(out, ctx, c, n);
}

/* v / 10 with 32-bit divisions only (no libgcc 64-bit helpers in the boot ROM). */
static unsigned long long div10(unsigned long long v, unsigned *rem)
{
    uint32_t hi = (uint32_t)(v >> 32), lo = (uint32_t)v;
    uint32_t qh = hi / 10u, r = hi % 10u;
    uint32_t x = (r << 16) | (lo >> 16);
    uint32_t q1 = x / 10u;
    uint32_t y = ((x % 10u) << 16) | (lo & 0xFFFFu);
    *rem = y % 10u;
    return ((unsigned long long)qh << 32) | ((unsigned long long)q1 << 16) | (y / 10u);
}

/* Digits of v in base 10 or 16, least significant first; returns the count. */
static int digits(char *buf, unsigned long long v, unsigned base, int upper)
{
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int len = 0;
    if (base == 16) {
        do { buf[len++] = set[v & 15u]; v >>= 4; } while (v);
    } else if (v <= 0xFFFFFFFFull) {
        uint32_t w = (uint32_t)v;         /* one DIVU/REMU per digit */
        do { buf[len++] = set[w % 10u]; w /= 10u; } while (w);
    } else {
        unsigned r;
        do { v = div10(v, &r); buf[len++] = set[r]; } while (v);
    }
    return len;
}

int fmt_vformat(fmt_out_fn out, void *ctx, const char *f, va_list ap)
{
    int n = 0;
    for (; *f; f++) {
        if (*f != '%') {
            emit(out, ctx, *f, &n);
            continue;
        }
        f++;
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else break;
        }
        if (*f == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            f++;
        } else {
            while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        }
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'l') { lng++; f++; }
        if (*f == 'z') { lng = sizeof(size_t) > 4 ? 2 : 0; f++; }

        char buf[24];
        const char *s = buf;
        int len = 0, neg = 0;
        unsigned long long v;
        switch (*f) {
        case 'c':
            buf[0] = (char)va_arg(ap, int);
            len = 1;
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (s[len] && (prec < 0 || len < prec)) len++;
            zero = 0;
            break;
        case 'd':
        case 'i': {
            long long sv = lng >= 2 ? va_arg(ap, long long)
                         : lng == 1 ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
            neg = sv < 0;
            v = neg ? 0ull - (unsigned long long)sv : (unsigned long long)sv;
            len = digits(buf, v, 10, 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
            v = lng >= 2 ? va_arg(ap, unsigned long long)
              : lng == 1 ? (unsigned long long)va_arg(ap, unsigned long)
              : (unsigned long long)va_arg(ap, unsigned);
            len = digits(buf, v, *f == 'u' ? 10 : 16, *f == 'X');
            break;
        case 'p':
            v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            len = digits(buf, v, 16, 0);
            emit(out, ctx, '0', &n);
            emit(out, ctx, 'x', &n);
            break;
        case '%':
            buf[0] = '%';
            len = 1;
            break;
        case '\0':
            return n;
        default:                       /* unknown conversion: print it as it was */
            emit(out, ctx, '%', &n);
            buf[0] = *f;
            len = 1;
            break;
        }

        int numeric = (*f == 'd' || *f == 'i' || *f == 'u' || *f == 'x' || *f == 'X' || *f == 'p');
        int total = len + neg;
        if (!left && !(zero && numeric))
            pad(out, ctx, ' ', width - total, &n);
        if (neg)
            emit(out, ctx, '-', &n);
        if (!left && zero && numeric)
            pad(out, ctx, '0', width - total, &n);
        if (numeric) {
            while (len > 0) emit(out, ctx, buf[--len], &n);     /* digits are reversed */
        } else {
            for (int i = 0; i < len; i++) emit(out, ctx, s[i], &n);
        }
        if (left)
            pad(out, ctx, ' ', width - total, &n);
    }
    return n;
}

struct sn_ctx {
    char *buf;
    size_t size, pos;
};

static void sn_out(void *p, char c)
{
    struct sn_ctx *x = p;
    if (x->pos + 1 < x->size)
        x->buf[x->pos] = c;
    x->pos++;
}

int fmt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct sn_ctx x = { buf, size, 0 };
    int n = fmt_vformat(sn_out, &x, fmt, ap);
    if (size > 0)
        buf[x.pos < size ? x.pos : size - 1] = '\0';
    return n;
}

int fmt_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = fmt_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
