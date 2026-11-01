/*
SurfOS user library: the mathematical functions
--------------------
File: math.c    Date: 11/01/26
--------------------
Portable C with no floating point arithmetic in it (modf()'s one subtraction, which is
exact, is the exception). On x87 a double lives in a 64-bit register until it is stored, so
what a calculation in doubles gives depends on the compiler's optimisation level, and
the tricks that make such a calculation accurate do not work. SurfCPU, where these
functions are meant to run one day, has no floating point at all.

So a function takes its argument apart and calculates in struct xf, a number of 96 bits
with an exponent that cannot overflow, far more than a double holds. Each operation
loses at most a unit or two of the last of the 96 bits, so what comes out is the
true value to about 90 bits, and rounding that to 53 gives the double nearest to the
true value but for an error of 2^-37 ulp. The same bits come out on every machine.
The exact cases (log10(1000), pow(10, 22), cbrt(27)) come out right by the same margin.
It is slower than a table-driven libm: about a microsecond for sin(), exp() or log() and twice
that for pow() on the Mac, against some tens of nanoseconds.

Argument reduction: exp() by multiples of ln 2, log() to a mantissa near 1, sin() and cos()
by multiples of pi/2 with as many bits of 2/pi as the argument's size asks for (1280 for
1e308), atan() around the nearest 16th. Then Taylor series, with no fitted coefficients:
those need the arithmetic this file avoids. The tables were calculated in integers
to 3000 bits. floor(), fmod(), sqrt() and the like work on the bits and are exact.
*/

#include <stddef.h>
#include <math.h>
#include <errno.h>
#include "libsurf.h"

typedef unsigned int u32;
typedef unsigned long long u64;

#define SIGN_BIT  (1ULL << 63)
#define EXP_MASK  (0x7FFULL << 52)
#define FRAC_MASK ((1ULL << 52) - 1)
#define ONE_BITS  (0x3FFULL << 52)

#define XN 3                            /* limbs of struct xf */

/* (-1)^neg * 0.m * 2^e, the top bit of m[0] set; m[0] zero: the number 0 */
struct xf {
    int neg;
    int e;
    u32 m[XN];
};

static const struct xf X_ONE = { 0, 1, { 0x80000000u, 0, 0 } };
static const struct xf X_LN2 = { 0, 0, { 0xB17217F7u, 0xD1CF79ABu, 0xC9E3B398u } };
static const struct xf X_LN2INV = { 0, 1, { 0xB8AA3B29u, 0x5C17F0BBu, 0xBE87FED0u } };
static const struct xf X_LOG10E = { 0, -1, { 0xDE5BD8A9u, 0x37287195u, 0x355BAAB0u } };
static const struct xf X_PIO2 = { 0, 1, { 0xC90FDAA2u, 0x2168C234u, 0xC4C6628Cu } };
static const struct xf X_PI = { 0, 2, { 0xC90FDAA2u, 0x2168C234u, 0xC4C6628Cu } };

/* atan(j / 16) */
static const struct xf X_ATAN[17] = {
    { 0, 0, { 0, 0, 0 } },
    { 0, -4, { 0xFFAADDB9u, 0x67EF4E36u, 0xCB2792DCu } },
    { 0, -3, { 0xFEADD4D5u, 0x617B6E32u, 0xC897989Fu } },
    { 0, -2, { 0xBDCBDA5Eu, 0x72D81134u, 0x7B0B4F88u } },
    { 0, -2, { 0xFADBAFC9u, 0x6406EB15u, 0x6DC79EF6u } },
    { 0, -1, { 0x9B13B9B8u, 0x3F5E5E69u, 0xC5ABB499u } },
    { 0, -1, { 0xB7B0CA0Fu, 0x26F78473u, 0x8AA32123u } },
    { 0, -1, { 0xD327761Eu, 0x611FE5B6u, 0x427C95E9u } },
    { 0, -1, { 0xED63382Bu, 0x0DDA7B45u, 0x6FE445EDu } },
    { 0, 0, { 0x832BF4A6u, 0xD9867E2Au, 0x4B6A09CBu } },
    { 0, 0, { 0x8F005D5Eu, 0xF7F59F9Bu, 0x5C835E16u } },
    { 0, 0, { 0x9A2F80E6u, 0x71BDDA20u, 0x4226F8E2u } },
    { 0, 0, { 0xA4BC7D19u, 0x34F70924u, 0x19A87F2Au } },
    { 0, 0, { 0xAEAC4C38u, 0xB4D8C080u, 0x14725E2Fu } },
    { 0, 0, { 0xB8053E2Bu, 0xC2319E73u, 0xCB2DA552u } },
    { 0, 0, { 0xC0CE85B8u, 0xAC526640u, 0x89DD62C4u } },
    { 0, 0, { 0xC90FDAA2u, 0x2168C234u, 0xC4C6628Cu } },
};

/* 2/pi in binary, from the point on: enough for the largest double, its size in bits plus 300 */
#define TWO_OVER_PI_LIMBS 40
static const u32 TWO_OVER_PI[TWO_OVER_PI_LIMBS] = {
    0xA2F9836Eu, 0x4E441529u, 0xFC2757D1u, 0xF534DDC0u, 0xDB629599u, 0x3C439041u,
    0xFE5163ABu, 0xDEBBC561u, 0xB7246E3Au, 0x424DD2E0u, 0x06492EEAu, 0x09D1921Cu,
    0xFE1DEB1Cu, 0xB129A73Eu, 0xE88235F5u, 0x2EBB4484u, 0xE99C7026u, 0xB45F7E41u,
    0x3991D639u, 0x835339F4u, 0x9C845F8Bu, 0xBDF9283Bu, 0x1FF897FFu, 0xDE05980Fu,
    0xEF2F118Bu, 0x5A0A6D1Fu, 0x6D367ECFu, 0x27CB09B7u, 0x4F463F66u, 0x9E5FEA2Du,
    0x7527BAC7u, 0xEBE5F17Bu, 0x3D0739F7u, 0x8A5292EAu, 0x6BFB5FB1u, 0x1F8D5D08u,
    0x56033046u, 0xFC7B6BABu, 0xF0CFBC20u, 0x9AF4361Du,
};

/**** a double's bits ****/

static u64 bits_of(double d) {
    union { double d; u64 u; } v;
    v.d = d;
    return v.u;
}

static double of_bits(u64 u) {
    union { double d; u64 u; } v;
    v.u = u;
    return v.d;
}

static int is_nan(u64 b) {
    return (b & ~SIGN_BIT) > EXP_MASK;
}

/* a finite number other than zero: its integer mantissa with bit 52 set, and the power of 2 that goes with it */
static int unpack(u64 abs, u64 *m) {
    int e = (int)(abs >> 52);
    *m = abs & FRAC_MASK;
    if(e) *m |= 1ULL << 52;
    else {
        e = 1;
        while(!(*m >> 52)) {
            *m <<= 1;
            e--;
        }
    }
    return e - 1075;
}

static double exact_pack(int neg, u64 m, int e) {
    return of_bits(__flt_pack(neg, m << 11, 0, e + 52, 53, 1023, 64));
}

static double domain_error(void) {
    errno = EDOM;
    return of_bits(0x7FFULL << 52 | 1ULL << 51);
}

static double range_inf(int neg) {
    errno = ERANGE;
    return of_bits((neg ? SIGN_BIT : 0) | EXP_MASK);
}

static double range_zero(int neg) {
    errno = ERANGE;
    return of_bits(neg ? SIGN_BIT : 0);
}

/**** struct xf ****/

static int clz32(u32 v) {
    int n = 0;
    if(!(v >> 16)) { n += 16; v <<= 16; }
    if(!(v >> 24)) { n += 8; v <<= 8; }
    if(!(v >> 28)) { n += 4; v <<= 4; }
    if(!(v >> 30)) { n += 2; v <<= 2; }
    if(!(v >> 31)) n++;
    return n;
}

/* n limbs, the most significant first, moved right by d bits */
static void limbs_shr(u32 *x, int n, int d) {
    int w = d / 32, s = d % 32, i;
    for(i = n - 1; i >= 0; i--) {
        u32 v = i - w >= 0 ? x[i - w] : 0;
        if(s) {
            v >>= s;
            if(i - w - 1 >= 0) v |= x[i - w - 1] << (32 - s);
        }
        x[i] = v;
    }
}

/* n limbs moved left until the top bit is set: how far, -1 when all are zero */
static int limbs_norm(u32 *x, int n) {
    int w = 0, s, i;
    while(w < n && !x[w]) w++;
    if(w == n) return -1;
    s = clz32(x[w]);
    for(i = 0; i + w < n; i++) {
        u32 v = x[i + w];
        if(s) {
            v <<= s;
            if(i + w + 1 < n) v |= x[i + w + 1] >> (32 - s);
        }
        x[i] = v;
    }
    for(; i < n; i++) x[i] = 0;
    return 32 * w + s;
}

static void x_from_double(struct xf *r, double d) {
    u64 b = bits_of(d), m;
    r->neg = (int)(b >> 63);
    r->m[2] = 0;
    b &= ~SIGN_BIT;
    if(!b) {
        r->e = r->m[0] = r->m[1] = 0;
        return;
    }
    r->e = unpack(b, &m) + 53;
    m <<= 11;
    r->m[0] = (u32)(m >> 32);
    r->m[1] = (u32)m;
}

static double x_to_double(const struct xf *a) {
    if(!a->m[0]) return of_bits(a->neg ? SIGN_BIT : 0);
    return of_bits(__flt_pack(a->neg, ((u64)a->m[0] << 32) | a->m[1], a->m[2] != 0, a->e - 1, 53, 1023, 64));
}

static void x_from_int(struct xf *r, int v) {
    u32 u = v < 0 ? 0u - (u32)v : (u32)v;
    int n = u ? clz32(u) : 0;
    r->neg = v < 0;
    r->e = u ? 32 - n : 0;
    r->m[0] = u << n;
    r->m[1] = r->m[2] = 0;
}

/* r = a + b, whatever their signs. Below the last limb there is one more, so that a subtraction that
   cancels the top bits still has good ones to bring up */
static void x_add(struct xf *r, const struct xf *a, const struct xf *b) {
    u32 p[XN + 1], q[XN + 1];
    const struct xf *t;
    int d, e, neg, i, s;
    if(!a->m[0]) { *r = *b; return; }
    if(!b->m[0]) { *r = *a; return; }
    d = a->e - b->e;
    for(i = 0; !d && i < XN; i++) if(a->m[i] != b->m[i]) d = a->m[i] > b->m[i] ? 1 : -1;
    if(d < 0) {
        t = a;
        a = b;
        b = t;
    }
    d = a->e - b->e;                            /* |a| >= |b| now */
    if(d > 32 * (XN + 1)) { *r = *a; return; }
    for(i = 0; i < XN; i++) {
        p[i] = a->m[i];
        q[i] = b->m[i];
    }
    p[XN] = q[XN] = 0;
    limbs_shr(q, XN + 1, d);
    e = a->e;
    neg = a->neg;
    if(a->neg == b->neg) {
        u32 carry = 0;
        for(i = XN; i >= 0; i--) {
            u64 w = (u64)p[i] + q[i] + carry;
            p[i] = (u32)w;
            carry = (u32)(w >> 32);
        }
        if(carry) {
            limbs_shr(p, XN + 1, 1);
            p[0] |= 0x80000000u;
            e++;
        }
    } else {
        u32 borrow = 0;
        for(i = XN; i >= 0; i--) {
            u64 w = (u64)p[i] - q[i] - borrow;
            p[i] = (u32)w;
            borrow = (u32)(w >> 32) & 1;
        }
        s = limbs_norm(p, XN + 1);
        if(s < 0) {
            neg = 0;
            e = 0;
        } else {
            e -= s;
        }
    }
    r->neg = neg;
    r->e = e;
    for(i = 0; i < XN; i++) r->m[i] = p[i];
}

static void x_sub(struct xf *r, const struct xf *a, const struct xf *b) {
    struct xf t = *b;
    t.neg = !t.neg;
    x_add(r, a, &t);
}

static void x_mul(struct xf *r, const struct xf *a, const struct xf *b) {
    u32 t[2 * XN];
    int i, j, e, neg;
    if(!a->m[0] || !b->m[0]) {
        r->neg = r->e = 0;
        for(i = 0; i < XN; i++) r->m[i] = 0;
        return;
    }
    for(i = 0; i < 2 * XN; i++) t[i] = 0;
    for(i = XN - 1; i >= 0; i--) {
        u32 carry = 0;
        for(j = XN - 1; j >= 0; j--) {
            u64 w = (u64)a->m[i] * b->m[j] + t[i + j + 1] + carry;
            t[i + j + 1] = (u32)w;
            carry = (u32)(w >> 32);
        }
        t[i] = carry;
    }
    e = a->e + b->e;
    neg = a->neg != b->neg;
    if(!(t[0] >> 31)) {                         /* two numbers from 0.5 to 1 make one from 0.25 to 1 */
        for(i = 0; i < XN; i++) t[i] = (t[i] << 1) | (t[i + 1] >> 31);
        e--;
    }
    r->neg = neg;
    r->e = e;
    for(i = 0; i < XN; i++) r->m[i] = t[i];
}

/* r = a / k, for k below 65536: limbs of 16 bits, so that no 64-bit division is needed. libgcc is not linked, and the
   remainder fits a 32-bit one */
static void x_div_small(struct xf *r, const struct xf *a, u32 k) {
    u32 q[XN + 1], rem = 0;
    int i, s;
    for(i = 0; i <= XN; i++) {
        u32 limb = i < XN ? a->m[i] : 0, cur, hi, lo;
        cur = (rem << 16) | (limb >> 16);
        hi = cur / k;
        rem = cur % k;
        cur = (rem << 16) | (limb & 0xFFFF);
        lo = cur / k;
        rem = cur % k;
        q[i] = (hi << 16) | lo;
    }
    s = limbs_norm(q, XN + 1);
    r->neg = a->neg;
    r->e = s < 0 ? 0 : a->e - s;
    if(s < 0) r->neg = 0;
    for(i = 0; i < XN; i++) r->m[i] = q[i];
}

/* r = 1 / b, b not 0: Newton's iteration from 15 good bits, each step doubling them */
static void x_inverse(struct xf *r, const struct xf *b) {
    struct xf a = *b, x, t;
    int i;
    a.neg = 0;
    x_from_int(&x, (int)(0xFFFFFFFFu / ((a.m[0] >> 16) + 1)));
    x.e -= 16 + a.e;
    for(i = 0; i < 3; i++) {
        x_mul(&t, &a, &x);
        x_sub(&t, &X_ONE, &t);
        x_mul(&t, &x, &t);
        x_add(&x, &x, &t);
    }
    x.neg = b->neg;
    *r = x;
}

static void x_div(struct xf *r, const struct xf *a, const struct xf *b) {
    struct xf t;
    x_inverse(&t, b);
    x_mul(r, a, &t);
}

/* the square root of a >= 0: sqrt() of the leading bits, then one step of Heron's rule */
static void x_sqrt(struct xf *r, const struct xf *a) {
    struct xf g = *a, s, t;
    int even = (a->e & 1) ? a->e + 1 : a->e;
    if(!a->m[0]) {
        *r = *a;
        return;
    }
    g.e = a->e - even;                          /* a number from 0.25 to 1, and an even power of 2 */
    x_from_double(&s, sqrt(x_to_double(&g)));
    s.e += even / 2;
    x_div(&t, a, &s);
    x_add(&t, &s, &t);
    t.e--;
    *r = t;
}

/* the integer nearest to t, which is far below 2^31 */
static int x_nearest(const struct xf *t) {
    u32 v = t->m[0], whole;
    int n;
    if(!v || t->e < 0) return 0;
    whole = t->e ? v >> (32 - t->e) : 0;
    whole += (v >> (31 - t->e)) & 1;
    n = (int)whole;
    return t->neg ? -n : n;
}

/* e^x = (1 + *p) * 2^k, the k returned; |x| below 2^20. Keeping the 1 apart keeps every bit of a small p. By multiples of
   ln 2 to |r| <= 0.35, then r/64, whose series needs 10 terms, and 6 squarings, (1 + p)^2 - 1 = 2p + p^2 */
static int x_expm1(struct xf *p, const struct xf *x) {
    struct xf t, r, q;
    int i, k;
    x_mul(&t, x, &X_LN2INV);
    k = x_nearest(&t);
    r = *x;
    if(k) {
        x_from_int(&t, k);
        x_mul(&t, &t, &X_LN2);
        x_sub(&r, x, &t);
    }
    if(r.m[0]) r.e -= 6;
    q = X_ONE;
    for(i = 10; i >= 2; i--) {
        x_mul(&q, &q, &r);
        x_div_small(&q, &q, (u32)i);
        x_add(&q, &q, &X_ONE);
    }
    x_mul(p, &q, &r);
    for(i = 0; i < 6; i++) {
        x_mul(&t, p, p);
        q = *p;
        if(q.m[0]) q.e++;
        x_add(p, &q, &t);
    }
    return k;
}

static void x_exp(struct xf *r, const struct xf *x) {
    struct xf p;
    int k = x_expm1(&p, x);
    x_add(r, &p, &X_ONE);
    r->e += k;
}

/* a > 0 is 2^*k * m with m from sqrt(1/2) to sqrt(2); ln m = 2 atanh(z), z = (m - 1) / (m + 1), |z| <= 0.172 */
static void x_log_parts(const struct xf *a, int *k, struct xf *s) {
    struct xf m = *a, f, t, w, q, c;
    int i;
    *k = a->e;
    m.neg = 0;
    m.e = 0;
    if(m.m[0] < 0xB504F334u) {
        m.e = 1;
        --*k;
    }
    x_sub(&f, &m, &X_ONE);
    if(!f.m[0]) {
        *s = f;
        return;
    }
    x_add(&t, &m, &X_ONE);
    x_div(&t, &f, &t);
    x_mul(&w, &t, &t);
    q.neg = q.e = q.m[0] = q.m[1] = q.m[2] = 0;
    for(i = 19; i >= 0; i--) {                  /* 1 + w/3 + w^2/5 + ... */
        x_div_small(&c, &X_ONE, (u32)(2 * i + 1));
        x_mul(&q, &q, &w);
        x_add(&q, &q, &c);
    }
    x_mul(s, &q, &t);
    s->e++;
}

/* the natural logarithm of a > 0 */
static void x_log(struct xf *r, const struct xf *a) {
    struct xf s, t;
    int k;
    x_log_parts(a, &k, &s);
    x_from_int(&t, k);
    x_mul(&t, &t, &X_LN2);
    x_add(r, &t, &s);
}

/* sin a and cos a for |a| <= pi/4, w = a^2 */
static void x_sin(struct xf *r, const struct xf *a, const struct xf *w) {
    struct xf q = X_ONE;
    int i;
    for(i = 13; i >= 1; i--) {
        x_mul(&q, &q, w);
        x_div_small(&q, &q, (u32)(2 * i * (2 * i + 1)));
        x_sub(&q, &X_ONE, &q);
    }
    x_mul(r, &q, a);
}

static void x_cos(struct xf *r, const struct xf *w) {
    struct xf q = X_ONE;
    int i;
    for(i = 13; i >= 1; i--) {
        x_mul(&q, &q, w);
        x_div_small(&q, &q, (u32)((2 * i - 1) * 2 * i));
        x_sub(&q, &X_ONE, &q);
    }
    *r = q;
}

/* |x| >= pi/4: the quadrant, x = quadrant * pi/2 + r mod 2 pi, |r| <= pi/4. x * 2/pi in integers, only as much of 2/pi as
   matters: its bits far before the ones that meet x's make whole multiples of 4, and those after the window below
   fall off. With 9 limbs in the window, the 62 zeros that a double may have at most after its point when divided by
   pi/2 leave 96 good bits. */
#define WINDOW 9
static int x_reduce(u64 abs, struct xf *r) {
    u32 a[3], p[WINDOW + 3], *f = p + 3;
    u64 m;
    int e = unpack(abs, &m), first = 0, s, i, k, n, up, lead;
    if(e >= 2) first = (e - 2) / 32;
    s = e - 32 * first;                         /* x = m * 2^s, in units of 2^(32 * first) */
    a[0] = 0;
    a[1] = (u32)(m >> 32);
    a[2] = (u32)m;
    if(s > 0) {
        int sh = s;
        for(; sh >= 32; sh -= 32) {
            a[0] = a[1];
            a[1] = a[2];
            a[2] = 0;
        }
        if(sh) {
            a[0] = (a[0] << sh) | (a[1] >> (32 - sh));
            a[1] = (a[1] << sh) | (a[2] >> (32 - sh));
            a[2] <<= sh;
        }
    }
    for(i = 0; i < WINDOW + 3; i++) p[i] = 0;
    for(i = 2; i >= 0; i--) {
        u32 carry = 0;
        for(k = WINDOW - 1; k >= 0; k--) {
            u64 w = (u64)a[i] * TWO_OVER_PI[first + k] + p[i + k + 1] + carry;
            p[i + k + 1] = (u32)w;
            carry = (u32)(w >> 32);
        }
        p[i] = carry;
    }
    if(s < 0) limbs_shr(p, WINDOW + 3, -s);
    n = (int)(p[2] & 3);
    up = (int)(f[0] >> 31);                     /* the nearer multiple is the next one: r is below 0 */
    if(up) {
        u32 carry = 1;
        for(i = WINDOW - 1; i >= 0; i--) {
            f[i] = ~f[i] + carry;
            carry = carry && !f[i];
        }
        n = (n + 1) & 3;
    }
    lead = limbs_norm(f, WINDOW);
    r->neg = up;
    r->e = -lead;
    for(i = 0; i < XN; i++) r->m[i] = f[i];
    x_mul(r, r, &X_PIO2);
    return n;
}

/* atan of a >= 0. Above 1, by atan(a) = pi/2 - atan(1/a); then atan(a) = atan(c) + atan((a - c) / (1 + a c)), c the nearest
   16th, which leaves at most about 1/32 for the series */
static void x_atan(struct xf *r, const struct xf *a) {
    struct xf x = *a, t, w, q, c;
    int inverted = 0, j, i;
    if(!a->m[0]) {
        *r = *a;
        return;
    }
    if(a->e > 1 || (a->e == 1 && (a->m[0] != 0x80000000u || a->m[1] || a->m[2]))) {
        x_inverse(&x, a);
        inverted = 1;
    }
    t = x;
    t.e += 4;
    j = x_nearest(&t);
    t = x;
    if(j) {
        x_from_int(&c, j);
        c.e -= 4;
        x_sub(&t, &x, &c);
        x_mul(&c, &x, &c);
        x_add(&c, &c, &X_ONE);
        x_div(&t, &t, &c);
    }
    x_mul(&w, &t, &t);
    q.neg = q.e = q.m[0] = q.m[1] = q.m[2] = 0;
    for(i = 10; i >= 0; i--) {                  /* 1 - w/3 + w^2/5 - ... */
        x_div_small(&c, &X_ONE, (u32)(2 * i + 1));
        x_mul(&q, &q, &w);
        x_sub(&q, &c, &q);
    }
    x_mul(&q, &q, &t);
    x_add(r, &q, &X_ATAN[j]);
    if(inverted) x_sub(r, &X_PIO2, r);
}

/**** exact: the bits ****/

double fabs(double x) {
    return of_bits(bits_of(x) & ~SIGN_BIT);
}

#define TO_ZERO  0
#define DOWN     1
#define UP       2
#define NEAREST  3

static double whole(double x, int mode) {
    u64 b = bits_of(x), mask, unit;
    int e = (int)((b >> 52) & 0x7FF) - 1023, neg = (int)(b >> 63);
    if(e >= 52 || !(b << 1)) return x;          /* whole already, an infinity, a NaN, a zero */
    if(e < 0) {                                 /* below 1: 0 or 1, with x's sign */
        int one = (mode == DOWN && neg) || (mode == UP && !neg) || (mode == NEAREST && e == -1);
        return of_bits((neg ? SIGN_BIT : 0) | (one ? ONE_BITS : 0));
    }
    unit = 1ULL << (52 - e);
    mask = unit - 1;
    if(!(b & mask)) return x;
    if((mode == DOWN && neg) || (mode == UP && !neg) || (mode == NEAREST && (b & (unit >> 1)))) b += unit;
    return of_bits(b & ~mask);
}

double trunc(double x) { return whole(x, TO_ZERO); }
double floor(double x) { return whole(x, DOWN); }
double ceil(double x) { return whole(x, UP); }
double round(double x) { return whole(x, NEAREST); }

double modf(double x, double *ip) {
    u64 b = bits_of(x);
    double t;
    if(is_nan(b)) {
        *ip = x;
        return x;
    }
    if((b & ~SIGN_BIT) == EXP_MASK) {
        *ip = x;
        return of_bits(b & SIGN_BIT);
    }
    t = trunc(x);
    *ip = t;
    return of_bits((bits_of(x - t) & ~SIGN_BIT) | (b & SIGN_BIT));
}

double frexp(double x, int *e) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT, m;
    if(!abs || abs >= EXP_MASK) {
        *e = 0;
        return x;
    }
    *e = unpack(abs, &m) + 53;
    return of_bits((b & SIGN_BIT) | (1022ULL << 52) | (m & FRAC_MASK));
}

double ldexp(double x, int n) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT, m;
    int e;
    if(!abs || abs >= EXP_MASK) return x;
    e = unpack(abs, &m);
    if(n > 5000) n = 5000;                      /* past both ends of the range whatever x is */
    if(n < -5000) n = -5000;
    return exact_pack((int)(b >> 63), m, e + n);
}

double fmod(double x, double y) {
    u64 bx = bits_of(x), by = bits_of(y), ax = bx & ~SIGN_BIT, ay = by & ~SIGN_BIT, mx, my;
    int ex, ey, neg = (int)(bx >> 63);
    if(ax > EXP_MASK) return x;
    if(ay > EXP_MASK) return y;
    if(ax == EXP_MASK || !ay) return domain_error();
    if(ay == EXP_MASK || ax < ay) return x;
    if(ax == ay) return of_bits(bx & SIGN_BIT);
    ex = unpack(ax, &mx);
    ey = unpack(ay, &my);
    for(; ex > ey; ex--) {                      /* a long division, one bit at a time, of at most 2100 */
        if(mx >= my) mx -= my;
        mx <<= 1;
        if(!mx) return of_bits(bx & SIGN_BIT);
    }
    if(mx >= my) mx -= my;
    if(!mx) return of_bits(bx & SIGN_BIT);
    while(!(mx >> 52)) {
        mx <<= 1;
        ey--;
    }
    return exact_pack(neg, mx, ey);
}

/* Digit by digit, the way a square root is done on paper, in base 2: 54 bits of the root, and what remains of the
   remainder says that it is not a tie. A double's root is never exactly halfway between two doubles. */
double sqrt(double x) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT, m, rem = 0, root = 0;
    int e, i;
    if(!abs || abs == EXP_MASK || is_nan(b)) {
        if(b >> 63 && abs && !is_nan(b)) return domain_error();
        return x;
    }
    if(b >> 63) return domain_error();
    e = unpack(abs, &m) + 52;                   /* x = m * 2^(e - 52): m has 53 bits */
    if(e & 1) {
        m <<= 1;
        e--;
    }
    m <<= 10;                                   /* its first two bits at the top */
    for(i = 0; i < 54; i++) {
        u64 trial;
        rem = (rem << 2) | (m >> 62);
        m <<= 2;
        trial = (root << 2) | 1;
        root <<= 1;
        if(rem >= trial) {
            rem -= trial;
            root |= 1;
        }
    }
    return of_bits(__flt_pack(0, root << 10, rem != 0, e / 2, 53, 1023, 64));
}

/**** exponentials and logarithms ****/

/* large enough that exp() and its kin overflow or vanish, small enough for x_expm1(): 800 */
#define EXP_LIMIT 0x4089000000000000ULL

double exp(double x) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT;
    struct xf a, r;
    if(abs > EXP_MASK) return x;
    if(abs == EXP_MASK) return b >> 63 ? 0 : x;
    if(abs >= EXP_LIMIT) return b >> 63 ? range_zero(0) : range_inf(0);
    x_from_double(&a, x);
    x_exp(&r, &a);
    return x_to_double(&r);
}

/* what log() and its kin do without calculating: 1 when *r holds the answer */
static int log_special(double x, double *r) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT;
    if(abs > EXP_MASK) *r = x;
    else if(!abs) *r = range_inf(1);
    else if(b >> 63) *r = domain_error();
    else if(abs == EXP_MASK) *r = x;
    else return 0;
    return 1;
}

double log(double x) {
    struct xf a, r;
    double d;
    if(log_special(x, &d)) return d;
    x_from_double(&a, x);
    x_log(&r, &a);
    return x_to_double(&r);
}

double log2(double x) {
    struct xf a, s, t;
    double d;
    int k;
    if(log_special(x, &d)) return d;
    x_from_double(&a, x);
    x_log_parts(&a, &k, &s);
    x_mul(&s, &s, &X_LN2INV);
    x_from_int(&t, k);
    x_add(&s, &t, &s);
    return x_to_double(&s);
}

double log10(double x) {
    struct xf a, r;
    double d;
    if(log_special(x, &d)) return d;
    x_from_double(&a, x);
    x_log(&r, &a);
    x_mul(&r, &r, &X_LOG10E);
    return x_to_double(&r);
}

/* 0: y is not an integer, 1: an odd one, 2: an even one */
static int integer_kind(u64 y) {
    u64 abs = y & ~SIGN_BIT, m;
    int e = (int)(abs >> 52) - 1023;
    if(!abs) return 2;
    if(e < 0) return 0;
    if(e >= 53) return 2;
    m = (abs & FRAC_MASK) | (1ULL << 52);
    if(e == 52) return (m & 1) ? 1 : 2;
    if(m & ((1ULL << (52 - e)) - 1)) return 0;
    return ((m >> (52 - e)) & 1) ? 1 : 2;
}

double pow(double x, double y) {
    u64 bx = bits_of(x), by = bits_of(y), ax = bx & ~SIGN_BIT, ay = by & ~SIGN_BIT;
    int ykind, neg = 0, yneg = (int)(by >> 63);
    struct xf a, b, r;
    if(!ay || bx == ONE_BITS) return of_bits(ONE_BITS);     /* x^0 and 1^y, for a NaN too */
    if(ax > EXP_MASK) return x;
    if(ay > EXP_MASK) return y;
    ykind = integer_kind(by);
    if(ay == EXP_MASK) {
        if(ax == ONE_BITS) return of_bits(ONE_BITS);        /* (-1)^inf */
        return (ax > ONE_BITS) != yneg ? of_bits(EXP_MASK) : 0;
    }
    if((bx >> 63) && ykind == 1) neg = 1;
    if(!ax) {
        if(yneg) return range_inf(neg);
        return of_bits(neg ? SIGN_BIT : 0);
    }
    if(ax == EXP_MASK) return of_bits((neg ? SIGN_BIT : 0) | (yneg ? 0 : EXP_MASK));
    if((bx >> 63) && !ykind) return domain_error();
    x_from_double(&a, x);
    a.neg = 0;
    x_log(&r, &a);
    x_from_double(&b, y);
    x_mul(&b, &r, &b);
    if(b.m[0] && b.e > 12) return b.neg ? range_zero(neg) : range_inf(neg);     /* e^4096 is out of any range */
    x_exp(&r, &b);
    r.neg = neg;
    return x_to_double(&r);
}

double cbrt(double x) {
    u64 b = bits_of(x);
    struct xf a, r;
    if(!(b << 1) || (b & ~SIGN_BIT) >= EXP_MASK) return x;
    x_from_double(&a, x);
    a.neg = 0;
    x_log(&r, &a);
    x_div_small(&r, &r, 3);
    x_exp(&a, &r);
    a.neg = (int)(b >> 63);
    return x_to_double(&a);
}

double hypot(double x, double y) {
    u64 ax = bits_of(x) & ~SIGN_BIT, ay = bits_of(y) & ~SIGN_BIT;
    struct xf a, b;
    if(ax == EXP_MASK || ay == EXP_MASK) return of_bits(EXP_MASK);  /* even with a NaN */
    if(ax > EXP_MASK) return x;
    if(ay > EXP_MASK) return y;
    if(!ax) return of_bits(ay);
    if(!ay) return of_bits(ax);
    x_from_double(&a, x);
    x_from_double(&b, y);
    x_mul(&a, &a, &a);
    x_mul(&b, &b, &b);
    x_add(&a, &a, &b);
    x_sqrt(&a, &a);
    return x_to_double(&a);
}

/* e^|x| as x_expm1() gives it: 1 + *p times 2^k in *e */
static int hyper_start(double x, struct xf *p, struct xf *e) {
    struct xf a;
    int k;
    x_from_double(&a, x);
    a.neg = 0;
    k = x_expm1(p, &a);
    x_add(e, p, &X_ONE);
    e->e += k;
    return k;
}

/* sinh |x|. Around 0 the two exponentials cancel, so it is (p + p / (1 + p)) / 2, which has no subtraction in it */
static void x_sinh(struct xf *s, const struct xf *p, const struct xf *e, int k) {
    struct xf t;
    if(!k) {
        x_div(&t, p, e);
        x_add(s, p, &t);
    } else {
        x_inverse(&t, e);
        x_sub(s, e, &t);
    }
    s->e--;
}

double sinh(double x) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT;
    struct xf p, e, s;
    int k;
    if(!abs || abs >= EXP_MASK) return x;
    if(abs >= EXP_LIMIT) return range_inf((int)(b >> 63));
    k = hyper_start(x, &p, &e);
    x_sinh(&s, &p, &e, k);
    s.neg = (int)(b >> 63);
    return x_to_double(&s);
}

double cosh(double x) {
    u64 abs = bits_of(x) & ~SIGN_BIT;
    struct xf p, e, t;
    if(abs > EXP_MASK) return x;
    if(abs == EXP_MASK) return of_bits(EXP_MASK);
    if(abs >= EXP_LIMIT) return range_inf(0);
    hyper_start(x, &p, &e);
    x_inverse(&t, &e);
    x_add(&t, &e, &t);
    t.e--;
    return x_to_double(&t);
}

double tanh(double x) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT;
    struct xf p, e, s, c;
    int k;
    if(abs > EXP_MASK || !abs) return x;
    if(abs >= 0x4036000000000000ULL)            /* from 22 on 1 - tanh x is far below an ulp of 1, and an infinity is the same */
        return of_bits((b & SIGN_BIT) | ONE_BITS);
    k = hyper_start(x, &p, &e);
    x_sinh(&s, &p, &e, k);
    x_inverse(&c, &e);
    x_add(&c, &e, &c);
    c.e--;
    x_div(&s, &s, &c);
    s.neg = (int)(b >> 63);
    return x_to_double(&s);
}

/**** circles ****/

/* below pi/4, as bits, nothing needs reducing */
#define PIO4_BITS 0x3FE921FB54442D18ULL

/* the reduced argument of |x| (finite), a, its square, w: the quadrant */
static int trig_start(u64 abs, struct xf *a, struct xf *w) {
    int n = 0;
    if(abs < PIO4_BITS) x_from_double(a, of_bits(abs));
    else n = x_reduce(abs, a);
    x_mul(w, a, a);
    return n;
}

double sin(double x) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT;
    struct xf a, w, r;
    int n;
    if(abs > EXP_MASK || !abs) return x;
    if(abs == EXP_MASK) return domain_error();
    n = trig_start(abs, &a, &w);
    if(n & 1) x_cos(&r, &w);
    else x_sin(&r, &a, &w);
    if(n & 2) r.neg = !r.neg;
    if(b >> 63) r.neg = !r.neg;
    return x_to_double(&r);
}

double cos(double x) {
    u64 abs = bits_of(x) & ~SIGN_BIT;
    struct xf a, w, r;
    int n;
    if(abs > EXP_MASK) return x;
    if(abs == EXP_MASK) return domain_error();
    n = trig_start(abs, &a, &w);
    if(n & 1) x_sin(&r, &a, &w);
    else x_cos(&r, &w);
    if(n == 1 || n == 2) r.neg = !r.neg;
    return x_to_double(&r);
}

double tan(double x) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT;
    struct xf a, w, s, c, r;
    int n;
    if(abs > EXP_MASK || !abs) return x;
    if(abs == EXP_MASK) return domain_error();
    n = trig_start(abs, &a, &w);
    x_sin(&s, &a, &w);
    x_cos(&c, &w);
    if(n & 1) {
        x_div(&r, &c, &s);
        r.neg = !r.neg;
    } else {
        x_div(&r, &s, &c);
    }
    if(b >> 63) r.neg = !r.neg;
    return x_to_double(&r);
}

double atan(double x) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT;
    struct xf a, r;
    if(abs > EXP_MASK || !abs) return x;
    if(abs == EXP_MASK) r = X_PIO2;
    else {
        x_from_double(&a, x);
        a.neg = 0;
        x_atan(&r, &a);
    }
    r.neg = (int)(b >> 63);
    return x_to_double(&r);
}

double atan2(double y, double x) {
    u64 bx = bits_of(x), by = bits_of(y), ax = bx & ~SIGN_BIT, ay = by & ~SIGN_BIT;
    int xneg = (int)(bx >> 63), yneg = (int)(by >> 63);
    struct xf a, b, r;
    if(ax > EXP_MASK) return x;
    if(ay > EXP_MASK) return y;
    if(ay == EXP_MASK || !ax) {                 /* straight up or down, with the corners */
        if(!ay) r = xneg ? X_PI : (struct xf){ 0, 0, { 0, 0, 0 } };
        else if(ax == EXP_MASK) {
            r = X_PIO2;
            r.e--;                              /* pi/4 */
            if(xneg) {
                x_add(&r, &r, &X_PIO2);         /* 3 pi/4 */
            }
        } else r = X_PIO2;
    } else if(!ay || ax == EXP_MASK) {          /* along the axis */
        r = xneg ? X_PI : (struct xf){ 0, 0, { 0, 0, 0 } };
    } else {
        x_from_double(&a, x);
        x_from_double(&b, y);
        a.neg = b.neg = 0;
        x_div(&a, &b, &a);
        x_atan(&r, &a);
        if(xneg) x_sub(&r, &X_PI, &r);
    }
    r.neg = yneg;
    return x_to_double(&r);
}

/* sqrt((1 - s)(1 + s)) for |s| < 1; the factors lose nothing to the 1 */
static void cofunction(struct xf *c, const struct xf *s) {
    struct xf p, q;
    x_sub(&p, &X_ONE, s);
    x_add(&q, &X_ONE, s);
    x_mul(&p, &p, &q);
    x_sqrt(c, &p);
}

double asin(double x) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT;
    struct xf s, c, r;
    if(abs > EXP_MASK || !abs) return x;
    if(abs > ONE_BITS) return domain_error();
    if(abs == ONE_BITS) r = X_PIO2;
    else {
        x_from_double(&s, x);
        s.neg = 0;
        cofunction(&c, &s);
        x_div(&s, &s, &c);
        x_atan(&r, &s);
    }
    r.neg = (int)(b >> 63);
    return x_to_double(&r);
}

double acos(double x) {
    u64 b = bits_of(x), abs = b & ~SIGN_BIT;
    struct xf s, c, r;
    if(abs > EXP_MASK) return x;
    if(abs > ONE_BITS) return domain_error();
    if(abs == ONE_BITS) {
        if(!(b >> 63)) return 0;
        return x_to_double(&X_PI);
    }
    if(!abs) return x_to_double(&X_PIO2);
    x_from_double(&s, x);
    s.neg = 0;
    cofunction(&c, &s);
    x_div(&c, &c, &s);
    x_atan(&r, &c);
    if(b >> 63) x_sub(&r, &X_PI, &r);
    return x_to_double(&r);
}
