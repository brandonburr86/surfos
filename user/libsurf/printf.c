/*
SurfOS user library: formatted output and input
--------------------
File: printf.c  Date: 10/5/26
--------------------
__format() is the kernel's vsnprintf (lib/blibc/vsnprintf.c) turned to write
into a sink, so one formatter serves strings, streams and descriptors: flags
- 0 + space #, width and precision (numbers or *), lengths hh h l ll z j t,
conversions d i u x X o c s p %. sscanf() reads %d %i %u %x %o %s %c %n %%,
with widths, * (no assignment) and the length letters; 11/01/26 %f %e %g %a too,
into a float (l: a double, L: a long double, which gets the nearest double), read
through __strtoflt() in strtod.c. Where the text stops being a number is where strtod()
stops: "100ergs" gives 100 and leaves "ergs", as the BSDs do.
10/25/26 (robustness, L14): and floating point, f F e E g G (L: a long double,
printed as the double nearest to it), now that every program's FPU registers
are its own (kernel/fpu.c). The digits are the exact value's, as glibc's are.
*/

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include "libsurf.h"

#define FL_LEFT  0x01
#define FL_ZERO  0x02
#define FL_PLUS  0x04
#define FL_SPACE 0x08
#define FL_ALT   0x10
#define FL_PTR   0x20           /* %p: 0x even for 0 (# leaves 0 bare) */

typedef unsigned int u32;
typedef unsigned long long u64;

static void emit(struct sink *s, const char *p, size_t n) {
    if(!n) return;
    s->total += n;
    s->put(s, p, n);
}

static void pad(struct sink *s, int n, char c) {
    static const char spaces[] = "                ", zeros[] = "0000000000000000";
    while(n > 0) {
        int k = n > 16 ? 16 : n;
        emit(s, c == '0' ? zeros : spaces, (size_t)k);
        n -= k;
    }
}

/* edx:eax / d with the quotient known to fit 32 bits (hi < d). No libgcc needed. */
static inline u32 div64_32(u32 hi, u32 lo, u32 d, u32 *rem) {
    u32 q, r;
    asm("divl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "r"(d));
    *rem = r;
    return q;
}

/* 64-bit value divided by a small base, as two 32-bit steps */
static u64 udiv64_small(u64 n, u32 base, u32 *rem) {
    u32 hi = (u32)(n >> 32), lo = (u32)n, r, qhi, qlo;
    qhi = hi / base;
    r = hi % base;
    qlo = div64_32(r, lo, base, &r);
    *rem = r;
    return ((u64)qhi << 32) | qlo;
}

static void fmt_num(struct sink *s, u64 val, int neg, unsigned base, int upper, int width, int prec, int flags) {
    char tmp[24];
    int len = 0, zeros, total, pfxlen = 0;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    const char *pfx = "";
    char sign = 0;
    int zero = val == 0;

    if(!(val == 0 && prec == 0)) {
        do {
            u32 r;
            val = udiv64_small(val, base, &r);
            tmp[len++] = digs[r];
        } while(val);
    }
    if(neg) sign = '-';
    else if(flags & FL_PLUS) sign = '+';
    else if(flags & FL_SPACE) sign = ' ';
    if(((flags & FL_ALT) && base == 16 && !zero) || (flags & FL_PTR)) { pfx = upper ? "0X" : "0x"; pfxlen = 2; }
    if((flags & FL_ALT) && base == 8 && (len == 0 || tmp[len - 1] != '0')) { pfx = "0"; pfxlen = 1; }

    zeros = (prec > len) ? prec - len : 0;
    if(prec < 0 && (flags & FL_ZERO) && !(flags & FL_LEFT)) {
        int body = len + pfxlen + (sign ? 1 : 0);
        if(width > body) zeros = width - body;
    }
    total = len + zeros + pfxlen + (sign ? 1 : 0);

    if(!(flags & FL_LEFT)) pad(s, width - total, ' ');
    if(sign) emit(s, &sign, 1);
    emit(s, pfx, (size_t)pfxlen);
    pad(s, zeros, '0');
    while(len) {                        /* the digits are in reverse */
        char rev[24];
        int k = 0;
        while(len) rev[k++] = tmp[--len];
        emit(s, rev, (size_t)k);
    }
    if(flags & FL_LEFT) pad(s, width - total, ' ');
}

/**** 10/25/26 (L14): floating point ****/

/* A double is mant * 2^e2 exactly, which is mant * 5^-e2 / 10^-e2 when e2 < 0: an integer, and a place for the
   point. That integer's decimal digits come from a number in base 10^9 (767 digits at most, for the smallest
   denormal), so every digit printed is the exact value's, rounded to nearest with a tie to even: what glibc prints.
   Integer arithmetic only (div64_32() above, no libgcc): of the FPU, a conversion uses no more than it takes to
   pass the double, so a program that prints no floating point never touches it. */
#define FLT_LIMBS 90                    /* 9 digits each */
#define FLT_BASE 1000000000u

/* big (n limbs, the least significant first) times f; the new n */
static int big_mul(u32 *big, int n, u32 f) {
    u32 carry = 0, r;
    int i;
    for(i = 0; i < n; i++) {
        u64 t = (u64)big[i] * f + carry;        /* below 10^9 * 2^32: the quotient fits 32 bits */
        carry = div64_32((u32)(t >> 32), (u32)t, FLT_BASE, &r);
        big[i] = r;
    }
    while(carry) {
        big[n++] = carry % FLT_BASE;
        carry /= FLT_BASE;
    }
    return n;
}

/* the exact decimal digits of v (finite, > 0) in dig[] (FLT_LIMBS * 9 bytes; the first is not 0): how many, and in
   *exp10 the power of ten of the first */
static int flt_exact(double v, char *dig, int *exp10) {
    static const u32 pow5[14] = { 1, 5, 25, 125, 625, 3125, 15625, 78125, 390625, 1953125, 9765625, 48828125,
                                  244140625, 1220703125 };
    union { double d; u64 u; } x;
    u32 big[FLT_LIMBS], r, w;
    u64 mant;
    int e2, n = 0, k, len = 0;
    char tmp[10];
    x.d = v;
    mant = x.u & 0xFFFFFFFFFFFFFULL;
    e2 = (int)(x.u >> 52) & 0x7FF;
    if(e2) mant |= 1ULL << 52;
    else e2 = 1;                                /* a denormal */
    e2 -= 1075;
    while(!(mant & 1)) {
        mant >>= 1;
        e2++;
    }
    while(mant) {
        mant = udiv64_small(mant, FLT_BASE, &r);
        big[n++] = r;
    }
    for(k = e2; k > 0; k -= 31) n = big_mul(big, n, k >= 31 ? 0x80000000u : 1u << k);
    for(k = -e2; k > 0; k -= 13) n = big_mul(big, n, pow5[k >= 13 ? 13 : k]);
    w = big[n - 1];                             /* the first limb without its zeros, then 9 digits a limb */
    k = 0;
    do { tmp[k++] = (char)('0' + w % 10); w /= 10; } while(w);
    while(k) dig[len++] = tmp[--k];
    while(--n > 0) {
        w = big[n - 1];
        for(k = 8; k >= 0; k--, w /= 10) dig[len + k] = (char)('0' + w % 10);
        len += 9;
    }
    *exp10 = len - 1 + (e2 < 0 ? e2 : 0);
    return len;
}

/* dig[0..n) rounded to ndig digits, a tie to even (the digits after the n are 0): how many are left; *exp10 one more
   when 9s carried into a new first digit. ndig 0 rounds to the place just above the first digit (to nothing, or to
   a 1 there), less than 0 to nothing */
static int flt_round(char *dig, int n, int ndig, int *exp10) {
    int i, up, rest = 0;
    if(ndig >= n) return n;
    if(ndig < 0) return 0;
    for(i = ndig + 1; i < n && !rest; i++) rest = dig[i] != '0';
    up = dig[ndig] > '5' || (dig[ndig] == '5' && (rest || (ndig > 0 && ((dig[ndig - 1] - '0') & 1))));
    if(!up) return ndig;
    for(i = ndig - 1; i >= 0; i--) {
        if(dig[i] != '9') {
            dig[i]++;
            return ndig;
        }
        dig[i] = '0';
    }
    dig[0] = '1';                               /* 9s carried over, or nothing was kept: a 1 at the place above */
    ++*exp10;
    return ndig ? ndig : 1;
}

/* count digits from index j of dig[0..n), 0 outside them */
static void put_digits(struct sink *s, const char *dig, int n, int j, int count) {
    char buf[32];
    int k = 0;
    for(; count > 0; count--, j++) {
        buf[k++] = j >= 0 && j < n ? dig[j] : '0';
        if(k == (int)sizeof(buf)) {
            emit(s, buf, (size_t)k);
            k = 0;
        }
    }
    emit(s, buf, (size_t)k);
}

static void fmt_float(struct sink *s, double v, char conv, int width, int prec, int flags) {
    union { double d; u64 u; } x;
    char dig[FLT_LIMBS * 9], sign = 0, ebuf[8], ex[2];
    int upper = conv == 'F' || conv == 'E' || conv == 'G', n = 0, e = 0, fixed, frac, len, elen = 0, point, k;
    x.d = v;
    if(x.u >> 63) sign = '-';                   /* -0.0 too */
    else if(flags & FL_PLUS) sign = '+';
    else if(flags & FL_SPACE) sign = ' ';
    x.u &= ~(1ULL << 63);
    if((x.u >> 52) == 0x7FF) {                  /* an infinity or a NaN: no 0 padding */
        len = 3 + (sign ? 1 : 0);
        if(!(flags & FL_LEFT)) pad(s, width - len, ' ');
        if(sign) emit(s, &sign, 1);
        emit(s, x.u << 12 ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"), 3);
        if(flags & FL_LEFT) pad(s, width - len, ' ');
        return;
    }
    if(prec < 0) prec = 6;
    if(x.u) n = flt_exact(x.d, dig, &e);        /* 0: no digits, the power 0 */
    if(conv == 'f' || conv == 'F') {
        fixed = 1;
        frac = prec;
        n = flt_round(dig, n, e + 1 + prec, &e);
    } else if(conv == 'e' || conv == 'E') {
        fixed = 0;
        frac = prec;
        n = flt_round(dig, n, prec + 1, &e);
    } else {                                    /* g: P significant digits, %e's way when the power is not in [-4, P) */
        int p = prec ? prec : 1;
        n = flt_round(dig, n, p, &e);
        fixed = e >= -4 && e < p;
        frac = fixed ? p - 1 - e : p - 1;
        if(!(flags & FL_ALT)) {                 /* no 0s at the end of the fraction, nor a point with none */
            for(k = n - 1; k >= 0 && dig[k] == '0'; k--);
            k = fixed ? k - e : k;              /* the fraction's digits up to its last one that is not 0 */
            if(k < frac) frac = k > 0 ? k : 0;
        }
    }
    if(!n && fixed) e = -1;                     /* rounded to 0: the integer part is the 0 alone */
    point = frac || (flags & FL_ALT);
    if(fixed) len = (e >= 0 ? e + 1 : 1) + (point ? 1 + frac : 0);
    else {
        k = e < 0 ? -e : e;
        do { ebuf[elen++] = (char)('0' + k % 10); k /= 10; } while(k);
        if(elen < 2) ebuf[elen++] = '0';
        len = 1 + (point ? 1 + frac : 0) + 2 + elen;
    }
    len += sign ? 1 : 0;
    if(!(flags & FL_LEFT) && !(flags & FL_ZERO)) pad(s, width - len, ' ');
    if(sign) emit(s, &sign, 1);
    if(!(flags & FL_LEFT) && (flags & FL_ZERO)) pad(s, width - len, '0');
    if(fixed) {
        if(e >= 0) put_digits(s, dig, n, 0, e + 1);
        else emit(s, "0", 1);
        if(point) emit(s, ".", 1);
        put_digits(s, dig, n, e + 1, frac);
    } else {
        put_digits(s, dig, n, 0, 1);
        if(point) emit(s, ".", 1);
        put_digits(s, dig, n, 1, frac);
        ex[0] = upper ? 'E' : 'e';
        ex[1] = e < 0 ? '-' : '+';
        emit(s, ex, 2);
        while(elen) emit(s, &ebuf[--elen], 1);
    }
    if(flags & FL_LEFT) pad(s, width - len, ' ');
}

int __format(struct sink *s, const char *fmt, va_list ap) {
    while(*fmt) {
        int flags = 0, width = 0, prec = -1, length = 0;    /* length: -2 hh, -1 h, 0 int, 1 l, 2 ll, 3 L */
        char c;
        if(*fmt != '%') {
            const char *start = fmt;
            while(*fmt && *fmt != '%') fmt++;
            emit(s, start, (size_t)(fmt - start));
            continue;
        }
        fmt++;
        for(;;) {
            if(*fmt == '-') flags |= FL_LEFT;
            else if(*fmt == '0') flags |= FL_ZERO;
            else if(*fmt == '+') flags |= FL_PLUS;
            else if(*fmt == ' ') flags |= FL_SPACE;
            else if(*fmt == '#') flags |= FL_ALT;
            else break;
            fmt++;
        }
        if(*fmt == '*') {
            width = va_arg(ap, int);
            if(width < 0) { flags |= FL_LEFT; width = -width; }
            fmt++;
        } else {
            while(*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        if(*fmt == '.') {
            fmt++;
            prec = 0;
            if(*fmt == '*') {
                prec = va_arg(ap, int);
                if(prec < 0) prec = -1;
                fmt++;
            } else {
                while(*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            }
        }
        if(*fmt == 'h') { length = -1; fmt++; if(*fmt == 'h') { length = -2; fmt++; } }
        else if(*fmt == 'l') { length = 1; fmt++; if(*fmt == 'l') { length = 2; fmt++; } }
        else if(*fmt == 'j') { length = 2; fmt++; }
        else if(*fmt == 'z' || *fmt == 't') { length = 1; fmt++; }
        else if(*fmt == 'L') { length = 3; fmt++; }

        c = *fmt++;
        switch(c) {
        case 'd':
        case 'i': {
            long long v;
            if(length == 2) v = va_arg(ap, long long);
            else if(length == 1) v = va_arg(ap, long);
            else if(length == -1) v = (short)va_arg(ap, int);
            else if(length == -2) v = (signed char)va_arg(ap, int);
            else v = va_arg(ap, int);
            fmt_num(s, v < 0 ? (u64)0 - (u64)v : (u64)v, v < 0, 10, 0, width, prec, flags);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            u64 v;
            if(length == 2) v = va_arg(ap, unsigned long long);
            else if(length == 1) v = va_arg(ap, unsigned long);
            else if(length == -1) v = (unsigned short)va_arg(ap, unsigned int);
            else if(length == -2) v = (unsigned char)va_arg(ap, unsigned int);
            else v = va_arg(ap, unsigned int);
            fmt_num(s, v, 0, c == 'u' ? 10 : (c == 'o' ? 8 : 16), c == 'X', width, prec, flags);
            break;
        }
        case 'p':
            fmt_num(s, (u64)(unsigned long)va_arg(ap, void *), 0, 16, 0, width, -1, flags | FL_PTR);
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':      /* 10/25/26 (L14) */
            fmt_float(s, length == 3 ? (double)va_arg(ap, long double) : va_arg(ap, double), c, width, prec, flags);
            break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            if(!(flags & FL_LEFT)) pad(s, width - 1, ' ');
            emit(s, &ch, 1);
            if(flags & FL_LEFT) pad(s, width - 1, ' ');
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            size_t len;
            if(!str) str = "(null)";
            len = prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str);
            if(!(flags & FL_LEFT)) pad(s, width - (int)len, ' ');
            emit(s, str, len);
            if(flags & FL_LEFT) pad(s, width - (int)len, ' ');
            break;
        }
        case '%':
            emit(s, "%", 1);
            break;
        case 0:
            fmt--;                      /* "%" at the very end: print it and stop */
            emit(s, "%", 1);
            break;
        default: {
            char unknown[2] = { '%', c };
            emit(s, unknown, 2);
            break;
        }
        }
    }
    return (int)s->total;
}

/**** strings ****/

static void string_put(struct sink *s, const char *p, size_t n) {
    while(n && s->left > 1) {
        *s->buf++ = *p++;
        s->left--;
        n--;
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct sink s;
    int n;
    memset(&s, 0, sizeof(s));
    s.put = string_put;
    s.buf = buf;
    s.left = size;
    n = __format(&s, fmt, ap);
    if(size) *s.buf = 0;
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)INT_MAX, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, (size_t)INT_MAX, fmt, ap);
    va_end(ap);
    return n;
}

/**** straight to a descriptor, in pieces of up to 256 bytes ****/

struct fd_sink {
    struct sink s;
    int fd;
    size_t used;
    char chunk[256];
};

static void fd_flush(struct fd_sink *d) {
    size_t done = 0;
    while(done < d->used) {
        ssize_t n = write(d->fd, d->chunk + done, d->used - done);
        if(n <= 0) { d->s.fail = 1; break; }
        done += (size_t)n;
    }
    d->used = 0;
}

static void fd_put(struct sink *s, const char *p, size_t n) {
    struct fd_sink *d = (struct fd_sink *)s;
    while(n) {
        size_t k = sizeof(d->chunk) - d->used;
        if(k > n) k = n;
        memcpy(d->chunk + d->used, p, k);
        d->used += k;
        p += k;
        n -= k;
        if(d->used == sizeof(d->chunk)) fd_flush(d);
    }
}

int vdprintf(int fd, const char *fmt, va_list ap) {
    struct fd_sink d;
    int n;
    memset(&d.s, 0, sizeof(d.s));
    d.s.put = fd_put;
    d.fd = fd;
    d.used = 0;
    n = __format(&d.s, fmt, ap);
    fd_flush(&d);
    return d.s.fail ? -1 : n;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

/**** sscanf ****/

/* an integer of at most width characters at *sp; 0 when there is none */
static int scan_int(const char **sp, int width, int base, int is_signed, unsigned long long *out) {
    const char *p = *sp;
    unsigned long long v = 0;
    int neg = 0, any = 0;
    if(width <= 0) width = INT_MAX;
    (void)is_signed;                                    /* %u of "-1" wraps around, as in C */
    if(*p == '-' || *p == '+') {
        neg = (*p == '-');
        p++;
        width--;
    }
    if((base == 0 || base == 16) && width >= 3 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        width -= 2;
        base = 16;
    } else if(base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }
    while(width > 0) {
        int c = (unsigned char)*p, d;
        if(isdigit(c)) d = c - '0';
        else if(isalpha(c)) d = tolower(c) - 'a' + 10;
        else break;
        if(d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
        p++;
        width--;
        any = 1;
    }
    if(!any) return 0;
    *out = neg ? 0 - v : v;
    *sp = p;
    return 1;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
    const char *s = str;
    int assigned = 0, converted = 0;
    while(*fmt) {
        int skip = 0, width = 0, length = 0;
        char c;
        if(isspace((unsigned char)*fmt)) {
            while(isspace((unsigned char)*fmt)) fmt++;
            while(isspace((unsigned char)*s)) s++;
            continue;
        }
        if(*fmt != '%' || fmt[1] == '%') {
            if(*fmt == '%') fmt++;
            if(*s != *fmt) break;
            s++;
            fmt++;
            continue;
        }
        fmt++;
        if(*fmt == '*') { skip = 1; fmt++; }
        while(isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        if(*fmt == 'h') { length = -1; fmt++; if(*fmt == 'h') { length = -2; fmt++; } }
        else if(*fmt == 'l') { length = 1; fmt++; if(*fmt == 'l') { length = 2; fmt++; } }
        else if(*fmt == 'j') { length = 2; fmt++; }
        else if(*fmt == 'z' || *fmt == 't') { length = 1; fmt++; }
        else if(*fmt == 'L') { length = 3; fmt++; }
        c = *fmt++;
        if(c == 'n') {
            if(!skip) *va_arg(ap, int *) = (int)(s - str);
            continue;
        }
        if(c != 'c') while(isspace((unsigned char)*s)) s++;
        if(!*s) return converted ? assigned : EOF;      /* the input ended before this conversion */
        switch(c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v;
            int base = c == 'd' || c == 'u' ? 10 : c == 'i' ? 0 : c == 'o' ? 8 : 16;
            if(!scan_int(&s, width, base, c == 'd' || c == 'i', &v)) return assigned;
            if(!skip) {
                if(length == 2) *va_arg(ap, unsigned long long *) = v;
                else if(length == 1) *va_arg(ap, unsigned long *) = (unsigned long)v;
                else if(length == -1) *va_arg(ap, unsigned short *) = (unsigned short)v;
                else if(length == -2) *va_arg(ap, unsigned char *) = (unsigned char)v;
                else *va_arg(ap, unsigned int *) = (unsigned int)v;
                assigned++;
            }
            break;
        }
        case 's': {
            char *out = skip ? NULL : va_arg(ap, char *);
            if(width <= 0) width = INT_MAX;
            while(*s && !isspace((unsigned char)*s) && width-- > 0) {
                if(out) *out++ = *s;
                s++;
            }
            if(out) { *out = 0; assigned++; }
            break;
        }
        case 'c': {
            char *out = skip ? NULL : va_arg(ap, char *);
            if(width <= 0) width = 1;
            while(width-- > 0) {
                if(!*s) return assigned;
                if(out) *out++ = *s;
                s++;
            }
            if(out) assigned++;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            char *end;
            union { double d; u64 u; } v;
            v.u = __strtoflt(s, width > 0 ? (size_t)width : (size_t)-1, &end, length <= 0);
            if(end == s) return assigned;
            s = end;
            if(!skip) {
                if(length == 3) *va_arg(ap, long double *) = v.d;
                else if(length > 0) *va_arg(ap, u64 *) = v.u;
                else *va_arg(ap, u32 *) = (u32)v.u;
                assigned++;
            }
            break;
        }
        default:
            return assigned;            /* not a conversion this sscanf knows */
        }
        converted++;
    }
    return assigned;
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsscanf(s, fmt, ap);
    va_end(ap);
    return n;
}
