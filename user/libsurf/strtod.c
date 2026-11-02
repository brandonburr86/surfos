/*
SurfOS user library: numbers from text
--------------------
File: strtod.c  Date: 11/01/26
--------------------
strtod() and atof(), and sscanf()'s %f %g %e through __strtoflt(). Correctly rounded,
ties to even, in integers only: x87 keeps doubles in 64-bit registers, so a calculation
in floating point would round twice, and differently at -O0 and at -O2. No floating
point also means that the same digits come out on every machine this runs on.

The number is D * 10^E with D the digits. For E >= 0 that is an integer, whose top 64
bits are the result and whose other bits say whether anything is left over. For E < 0 it
is a fraction, and a long division gives 64 bits of it, with its remainder saying the
same. Only the first KEEP_DIGITS digits are kept: a number halfway between two doubles
has fewer, so what is dropped only needs to be known as zero or not, and becomes a
last digit 1. Numbers far out of range never reach the arithmetic, which is what bounds
struct big.

Also read: a sign, inf, infinity, nan, nan(chars), and 0x hexadecimal with a p exponent.
ERANGE when the result overflows, or underflows to something inexact (as glibc does).
*/

#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include "libsurf.h"

typedef unsigned int u32;
typedef unsigned long long u64;

#define KEEP_DIGITS 800
#define BIG_LIMBS   128                 /* 10^1141 shifted left 64 bits fits: see decimal() */

struct big {
    int n;
    u32 d[BIG_LIMBS];                   /* the least significant first, no zero at the top */
};

/* b * m + add */
static void big_mul(struct big *b, u32 m, u32 add) {
    u64 c = add;
    int i;
    for(i = 0; i < b->n; i++) {
        c += (u64)b->d[i] * m;
        b->d[i] = (u32)c;
        c >>= 32;
    }
    if(c) b->d[b->n++] = (u32)c;
}

static void big_shl(struct big *b, int bits) {
    int w = bits / 32, s = bits % 32, i;
    if(!b->n) return;
    if(s) {
        u32 c = 0;
        for(i = 0; i < b->n; i++) {
            u32 v = b->d[i];
            b->d[i] = (v << s) | c;
            c = v >> (32 - s);
        }
        if(c) b->d[b->n++] = c;
    }
    if(w) {
        for(i = b->n - 1; i >= 0; i--) b->d[i + w] = b->d[i];
        for(i = 0; i < w; i++) b->d[i] = 0;
        b->n += w;
    }
}

static int big_bits(const struct big *b) {
    u32 top;
    int n;
    if(!b->n) return 0;
    top = b->d[b->n - 1];
    n = 32 * (b->n - 1);
    while(top) {
        n++;
        top >>= 1;
    }
    return n;
}

static int big_cmp(const struct big *a, const struct big *b) {
    int i;
    if(a->n != b->n) return a->n < b->n ? -1 : 1;
    for(i = a->n - 1; i >= 0; i--)
        if(a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

/* a -= b, with a >= b */
static void big_sub(struct big *a, const struct big *b) {
    u32 borrow = 0;
    int i;
    for(i = 0; i < a->n; i++) {
        u32 s = i < b->n ? b->d[i] : 0, v = a->d[i], r = v - s - borrow;
        borrow = v < s || (v == s && borrow);
        a->d[i] = r;
    }
    while(a->n && !a->d[a->n - 1]) a->n--;
}

/* the top 64 bits of b (not zero), its top bit at bit 63; *sticky: bits were left below them */
static u64 big_top(const struct big *b, int *sticky) {
    int bits = big_bits(b), low = bits - 64, i;
    u64 v = 0;
    for(i = bits - 1; i >= 0 && i >= low; i--) {
        v = (v << 1) | ((b->d[i / 32] >> (i % 32)) & 1);
    }
    if(low < 0) v <<= -low;
    for(i = 0; i < low / 32; i++) if(b->d[i]) *sticky = 1;
    if(low > 0 && low % 32 && (b->d[low / 32] << (32 - low % 32))) *sticky = 1;
    return v;
}

/* big = big * 10^e */
static void big_pow10(struct big *b, long e) {
    for(; e >= 9; e -= 9) big_mul(b, 1000000000u, 0);
    for(; e > 0; e--) big_mul(b, 10, 0);
}

u64 __flt_pack(int neg, u64 m, int sticky, int e, int mant, int emax, int width) {
    int emin = 1 - emax, shift = 64 - mant;
    u64 bits, rest, half, out;
    u64 sign = (u64)(neg != 0) << (width - 1);
    if(e > emax) {
        errno = ERANGE;
        return sign | ((u64)(2 * emax + 1) << (mant - 1));
    }
    if(e < emin) shift += emin - e;
    if(shift > 64) {                            /* below half of the smallest number */
        errno = ERANGE;
        return sign;
    }
    if(shift == 64) {
        rest = m;
        half = 1ULL << 63;
        out = 0;
    } else {
        rest = m & ((1ULL << shift) - 1);
        half = 1ULL << (shift - 1);
        out = m >> shift;
    }
    if(rest > half || (rest == half && (sticky || (out & 1)))) out++;
    if(e < emin) {
        bits = out;                             /* a carry here makes the smallest normal number by itself */
        if(rest || sticky) errno = ERANGE;
    } else {
        bits = ((u64)(e + emax - 1) << (mant - 1)) + out;      /* out's top bit adds the 1 back */
        if((bits >> (mant - 1)) >= (u64)(2 * emax + 1)) errno = ERANGE;     /* rounded up into infinity */
    }
    return sign | bits;
}

/* a double or, with single, a float, from a number's top 64 bits */
static u64 pack(int neg, u64 m, int sticky, int e, int single) {
    return single ? __flt_pack(neg, m, sticky, e, 24, 127, 32) : __flt_pack(neg, m, sticky, e, 53, 1023, 64);
}

static u64 infinity(int neg, int single) {
    return single ? ((u64)(neg != 0) << 31) | 0x7F800000u : ((u64)(neg != 0) << 63) | (0x7FFULL << 52);
}

static u64 not_a_number(int neg, int single) {
    return single ? ((u64)(neg != 0) << 31) | 0x7FC00000u : ((u64)(neg != 0) << 63) | (0xFFFULL << 51);
}

static u64 zero(int neg, int single) {
    return (u64)(neg != 0) << (single ? 31 : 63);
}

/* D * 10^e, as above. nd digits in D: 10^(nd-1) <= D < 10^nd */
static u64 decimal(int neg, struct big *d, int nd, long long e, int single) {
    struct big den, r;
    long long mag = nd + e;
    u64 q = 0;
    int sticky = 0, bits, ld, ln, s, i;
    if(!nd) return zero(neg, single);
    if(mag - 1 >= 310) {                /* at least 1e309: more than any double */
        errno = ERANGE;
        return infinity(neg, single);
    }
    if(mag <= -340) {                   /* less than 1e-340: less than half of the smallest double */
        errno = ERANGE;
        return zero(neg, single);
    }
    if(e >= 0) {                        /* at most 10^310: 1030 bits */
        big_pow10(d, (long)e);
        bits = big_bits(d);
        q = big_top(d, &sticky);
        return pack(neg, q, sticky, bits - 1, single);
    }
    /* -e < nd + 340 <= 801 + 340: den is below 2^3791. N = D shifted so that it is 63 bits longer than den (or den
       shifted when D is that long already), so the quotient has 63 or 64 bits */
    den.n = 1;
    den.d[0] = 1;
    big_pow10(&den, (long)-e);
    ld = big_bits(&den);
    ln = big_bits(d);
    s = ld + 63 - ln;
    if(s >= 0) big_shl(d, s);
    else big_shl(&den, -s);
    big_shl(&den, 63);
    r = *d;
    for(i = 63; i >= 0; i--) {
        if(big_cmp(&r, &den) >= 0) {
            big_sub(&r, &den);
            q |= 1ULL << i;
        }
        big_shl(&r, 1);
    }
    if(r.n) sticky = 1;
    /* the value is q * 2^-s */
    if(!(q >> 63)) {
        q <<= 1;
        s++;
    }
    return pack(neg, q, sticky, 63 - s, single);
}

static int same_word(const char *s, size_t max, const char *lower) {
    size_t i;
    for(i = 0; lower[i]; i++)
        if(i >= max || tolower((unsigned char)s[i]) != lower[i]) return 0;
    return 1;
}

/* 0x: top 64 bits of the digits, and how far the point moved them */
static u64 hexadecimal(int neg, const char *s, size_t max, size_t *used, int single) {
    size_t i = 0;
    u64 v = 0;
    long long e = 0;
    int sticky = 0, point = 0;
    for(; i < max; i++) {
        int c = (unsigned char)s[i], h;
        if(c == '.' && !point) {
            point = 1;
            continue;
        }
        if(!isxdigit(c)) break;
        h = isdigit(c) ? c - '0' : tolower(c) - 'a' + 10;
        if(v >> 60) {
            if(h) sticky = 1;
            if(!point) e += 4;
        } else {
            v = (v << 4) | (u64)h;
            if(point) e -= 4;
        }
    }
    *used = i;
    if(i < max && (s[i] == 'p' || s[i] == 'P')) {
        size_t k = i + 1;
        int eneg = 0;
        if(k < max && (s[k] == '-' || s[k] == '+')) eneg = s[k++] == '-';
        if(k < max && isdigit((unsigned char)s[k])) {
            long long x = 0;
            for(; k < max && isdigit((unsigned char)s[k]); k++)
                if(x < 100000000) x = x * 10 + (s[k] - '0');
            e += eneg ? -x : x;
            *used = k;
        }
    }
    if(!v) return zero(neg, single);
    while(!(v >> 63)) {
        v <<= 1;
        e--;
    }
    e += 63;
    if(e > 100000) e = 100000;
    if(e < -100000) e = -100000;
    return pack(neg, v, sticky, (int)e, single);
}

/* The number at s, reading at most max bytes (sscanf's width): its bits, as a float's with single. *end: where it
   stops, s when there is no number. Space before it is skipped, as strtod does. */
u64 __strtoflt(const char *s, size_t max, char **end, int single) {
    struct big d;
    const char *p = s;
    size_t left = max, used;
    long long e = 0;
    int neg = 0, nd = 0, any = 0, point = 0, sticky = 0, pending = 0;
    u32 chunk = 0, unit = 1;
    if(end) *end = (char *)s;
    while(left && isspace((unsigned char)*p)) {
        p++;
        left--;
    }
    if(left && (*p == '-' || *p == '+')) {
        neg = *p == '-';
        p++;
        left--;
    }
    if(same_word(p, left, "inf")) {
        used = same_word(p, left, "infinity") ? 8 : 3;
        if(end) *end = (char *)p + used;
        return infinity(neg, single);
    }
    if(same_word(p, left, "nan")) {
        used = 3;
        if(left > 3 && p[3] == '(') {           /* nan(chars): the chars say nothing here */
            size_t k = 4;
            while(k < left && (isalnum((unsigned char)p[k]) || p[k] == '_')) k++;
            if(k < left && p[k] == ')') used = k + 1;
        }
        if(end) *end = (char *)p + used;
        return not_a_number(neg, single);
    }
    if(left >= 3 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
       (isxdigit((unsigned char)p[2]) || (left >= 4 && p[2] == '.' && isxdigit((unsigned char)p[3])))) {
        u64 bits = hexadecimal(neg, p + 2, left - 2, &used, single);
        if(end) *end = (char *)p + 2 + used;
        return bits;
    }
    d.n = 0;
    for(; left; p++, left--) {
        int c = (unsigned char)*p;
        if(c == '.' && !point) {
            /* a point counts only with a digit on one side of it */
            if(!any && !(left > 1 && isdigit((unsigned char)p[1]))) break;
            point = 1;
            continue;
        }
        if(!isdigit(c)) break;
        any = 1;
        if(!nd && c == '0') {                   /* a zero before the first digit only moves the point */
            if(point) e--;
            continue;
        }
        if(nd < KEEP_DIGITS) {
            chunk = chunk * 10 + (u32)(c - '0');
            unit *= 10;
            pending = 1;
            if(unit == 1000000000u) {
                big_mul(&d, unit, chunk);
                chunk = 0;
                unit = 1;
                pending = 0;
            }
            nd++;
            if(point) e--;
        } else {
            if(c != '0') sticky = 1;
            if(!point) e++;
        }
    }
    if(!any) return 0;                          /* no number: *end stays at s */
    if(pending) big_mul(&d, unit, chunk);
    if(sticky) {                                /* see the top of this file */
        big_mul(&d, 10, 1);
        nd++;
        e--;
    }
    if(left && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        size_t k = left - 1;
        int eneg = 0;
        if(k && (*q == '-' || *q == '+')) {
            eneg = *q++ == '-';
            k--;
        }
        if(k && isdigit((unsigned char)*q)) {
            long long x = 0;
            for(; k && isdigit((unsigned char)*q); q++, k--)
                if(x < 1000000000) x = x * 10 + (*q - '0');
            e += eneg ? -x : x;
            p = q;
        }
    }
    if(end) *end = (char *)p;
    return decimal(neg, &d, nd, e, single);
}

double strtod(const char *s, char **end) {
    union { double d; u64 u; } v;
    v.u = __strtoflt(s, (size_t)-1, end, 0);
    return v.d;
}

double atof(const char *s) {
    return strtod(s, NULL);
}
