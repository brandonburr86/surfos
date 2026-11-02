/*
SurfOS user program: bc
--------------------
File: num.c     Date: 11/01/26
--------------------
Decimal numbers of any length. A number is sign and magnitude: limbs in base 10000, kept in an
array of 16-bit words, with the decimal point on the edge of a limb. A scale that is no multiple
of 4 leaves the lowest limb's last digits zero, and they stay zero. So limbs line up in addition
without any shifting, which is why the point is kept there: scale is a number of decimal digits,
and bc's rules cut results at exactly that digit.

Base 10000 because a user program links no libgcc, so on 32-bit x86 C has no 64-bit division.
Every product and every carry here fits 32 bits, on a 486 and one day on SurfCPU.

Nothing is rounded: bc truncates, as POSIX says, so the digits are always those of the exact
result. num_pow() therefore works out the exact power before it cuts, as GNU's and Howard's bc do,
and num_mod() is a - (a / b) * b without a cut. Division is Knuth's algorithm D, the square root
Newton's iteration on whole numbers, which stops at exactly the floor of the root.

Nothing recurses. A size is worked out in 64 bits before anything is allocated.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "num.h"

#define FRAC(scale) (((scale) + NUM_DIGITS - 1) / NUM_DIGITS)   /* the fraction's limbs */
#define MUL_SMALL 64            /* columns of a product that go on the stack */
#define MUL_ROWS  40            /* rows that may add up before a column can pass 32 bits: 40 * 9999^2 < 2^32 */
#define CHUNK_MAX 429496u       /* the largest c for which (c - 1) * 10000 + 9999 fits 32 bits */

int num_err;

static const unsigned pow10[NUM_DIGITS] = { 1, 10, 100, 1000 };

const char *num_strerror(int err) {
    switch(err) {
    case NUM_NOMEM: return "out of memory";
    case NUM_TOOLONG: return "number too long";
    case NUM_DIVZERO: return "divide by zero";
    case NUM_NEGROOT: return "square root of a negative number";
    case NUM_BADEXP: return "exponent is not a whole number";
    case NUM_BIGEXP: return "exponent too large";
    }
    return "no error";
}

static struct num *alloc(uint64_t limbs, unsigned scale) {
    struct num *n;
    if(limbs > NUM_MAX_LIMBS || scale > NUM_MAX_SCALE) {
        num_err = NUM_TOOLONG;
        return NULL;
    }
    n = malloc(sizeof(*n) + (size_t)limbs * sizeof(limb_t));
    if(!n) {
        num_err = NUM_NOMEM;
        return NULL;
    }
    n->refs = 1;
    n->len = (unsigned)limbs;
    n->scale = scale;
    n->neg = 0;
    return n;
}

struct num *num_ref(struct num *n) {
    n->refs++;
    return n;
}

void num_put(struct num *n) {
    if(n && !--n->refs) free(n);
}

/* n's top limbs may be 0, and n may have become zero: the form every number leaves in */
static struct num *finish(struct num *n) {
    while(n->len && !n->d[n->len - 1]) n->len--;
    if(!n->len) n->neg = 0;
    return n;
}

/* zero the digits past the scale in the lowest limb, the fraction's last */
static void cut_digits(struct num *n) {
    unsigned part = n->scale % NUM_DIGITS;
    if(part && n->len) n->d[0] -= n->d[0] % pow10[NUM_DIGITS - part];
}

static struct num *zero(unsigned scale) {
    return alloc(0, scale);
}

struct num *num_uint(unsigned v) {
    struct num *n = alloc(3, 0);
    unsigned i;
    if(!n) return NULL;
    for(i = 0; i < 3; i++) {
        n->d[i] = v % NUM_BASE;
        v /= NUM_BASE;
    }
    return finish(n);
}

static int digit_value(char c) {
    return c >= 'A' ? c - 'A' + 10 : c - '0';
}

/* The digits s[0..n) as a whole number: base 10 only when one of them is above 9, the way below being the quick one.
   A digit the base does not have counts at its own value, so 12 in base 2 is 4: POSIX leaves it open, and a script
   should not stop for it, as it does not on the Mac's bc. Fifteen in a place adds at most one more digit to the whole. */
static struct num *whole_in_base(const char *s, unsigned n, unsigned base) {
    /* a digit of base 16 is 1.21 decimal digits, under 5/16 of a limb */
    struct num *r = alloc((uint64_t)n * 5 / 16 + 2, 0);
    unsigned i, k, len = 0;
    if(!r) return NULL;
    for(i = 0; i < n; i++) {
        uint32_t carry = (uint32_t)digit_value(s[i]);
        for(k = 0; k < len; k++) {
            uint32_t x = r->d[k] * base + carry;
            r->d[k] = x % NUM_BASE;
            carry = x / NUM_BASE;
        }
        if(carry) r->d[len++] = (limb_t)carry;
    }
    r->len = len;
    return r;
}

/* In another base a fraction is worked out and cut to as many decimal digits as it was written
   with, the scale POSIX gives a constant: .F in base 16 is .9 */
static struct num *parse_in_base(const char *s, unsigned nint, const char *f, unsigned nfrac, unsigned base) {
    struct num *in = whole_in_base(s, nint, base), *up, *down, *q, *b, *e, *r;
    if(!in) return NULL;
    if(!nfrac) return in;
    up = whole_in_base(f, nfrac, base);
    b = num_uint(base);
    e = num_uint(nfrac);
    down = b && e ? num_pow(b, e, 0) : NULL;
    q = up && down ? num_div(up, down, nfrac) : NULL;
    r = q ? num_add(in, q) : NULL;
    num_put(in);
    num_put(up);
    num_put(b);
    num_put(e);
    num_put(down);
    num_put(q);
    return r;
}

static int above_nine(const char *s, unsigned n) {
    while(n--) if(*s++ >= 'A') return 1;
    return 0;
}

struct num *num_parse(const char *s, unsigned n, unsigned base) {
    const char *dot = memchr(s, '.', n);
    unsigned nint = dot ? (unsigned)(dot - s) : n, nfrac = dot ? n - nint - 1 : 0, i, flimbs;
    struct num *r;
    while(nint && *s == '0') {          /* leading zeros say nothing, and cost a limb each */
        s++;
        nint--;
    }
    if(base != 10 || above_nine(s, nint + (dot ? 1 + nfrac : 0))) return parse_in_base(s, nint, s + nint + 1, nfrac, base);
    flimbs = FRAC(nfrac);
    r = alloc((uint64_t)flimbs + (nint + NUM_DIGITS - 1) / NUM_DIGITS, nfrac);
    if(!r) return NULL;
    memset(r->d, 0, r->len * sizeof(limb_t));
    for(i = 0; i < nint + nfrac; i++) {
        char c = i < nint ? s[i] : s[i + 1];    /* s[nint] is the point */
        unsigned v = (unsigned)digit_value(c);
        if(i < nint) {
            unsigned up = nint - 1 - i;
            r->d[flimbs + up / NUM_DIGITS] += (limb_t)(v * pow10[up % NUM_DIGITS]);
        } else {
            unsigned down = i - nint;
            r->d[flimbs - 1 - down / NUM_DIGITS] += (limb_t)(v * pow10[NUM_DIGITS - 1 - down % NUM_DIGITS]);
        }
    }
    return finish(r);
}

/**** comparing ****/

/* the sizes, lined up on the point: compare the top limbs' places, then the limbs from the top */
static int cmp_size(const struct num *a, const struct num *b) {
    unsigned fa = FRAC(a->scale), fb = FRAC(b->scale), f = fa > fb ? fa : fb, i, k;
    uint64_t top_a = (uint64_t)a->len + (f - fa), top_b = (uint64_t)b->len + (f - fb);
    if(!a->len || !b->len) return (int)!!a->len - (int)!!b->len;
    if(top_a != top_b) return top_a > top_b ? 1 : -1;
    i = a->len;
    k = b->len;
    while(i && k) {
        i--;
        k--;
        if(a->d[i] != b->d[k]) return a->d[i] > b->d[k] ? 1 : -1;
    }
    while(i) if(a->d[--i]) return 1;
    while(k) if(b->d[--k]) return -1;
    return 0;
}

int num_cmp(const struct num *a, const struct num *b) {
    if(a->neg != b->neg) return a->neg ? -1 : 1;
    return a->neg ? -cmp_size(a, b) : cmp_size(a, b);
}

/**** adding ****/

/* the limb of n at place i, where n's limb 0 is at place shift */
static inline unsigned limb_at(const struct num *n, unsigned shift, unsigned i) {
    return i >= shift && i - shift < n->len ? n->d[i - shift] : 0;
}

/* a + b, or a - b for a at least as large as b: sizes only, the larger scale */
static struct num *add_sizes(const struct num *a, const struct num *b, int subtract) {
    unsigned fa = FRAC(a->scale), fb = FRAC(b->scale), f = fa > fb ? fa : fb;
    unsigned sa = f - fa, sb = f - fb, i;
    uint64_t na = (uint64_t)a->len + sa, nb = (uint64_t)b->len + sb;
    struct num *r = alloc((na > nb ? na : nb) + 1, a->scale > b->scale ? a->scale : b->scale);
    unsigned carry = 0;
    if(!r) return NULL;
    for(i = 0; i < r->len; i++) {
        unsigned x = limb_at(a, sa, i), y = limb_at(b, sb, i) + carry;
        if(subtract) {
            carry = x < y;
            r->d[i] = (limb_t)(x + (carry ? NUM_BASE : 0) - y);
        } else {
            x += y;
            carry = x >= NUM_BASE;
            r->d[i] = (limb_t)(x - (carry ? NUM_BASE : 0));
        }
    }
    return r;
}

static struct num *signed_sum(const struct num *a, const struct num *b, int b_neg) {
    struct num *r;
    if(a->neg == b_neg) {
        if(!(r = add_sizes(a, b, 0))) return NULL;
        r->neg = a->neg;
    } else if(cmp_size(a, b) >= 0) {
        if(!(r = add_sizes(a, b, 1))) return NULL;
        r->neg = a->neg;
    } else {
        if(!(r = add_sizes(b, a, 1))) return NULL;
        r->neg = b_neg;
    }
    return finish(r);
}

struct num *num_add(const struct num *a, const struct num *b) {
    return signed_sum(a, b, b->neg);
}

struct num *num_sub(const struct num *a, const struct num *b) {
    return signed_sum(a, b, b->len ? !b->neg : 0);
}

struct num *num_neg(const struct num *a) {
    struct num *r = alloc(a->len, a->scale);
    if(!r) return NULL;
    memcpy(r->d, a->d, a->len * sizeof(limb_t));
    r->neg = a->len ? !a->neg : 0;
    return r;
}

/* a at another scale: limbs dropped or zeros below; the digits past the scale go */
struct num *num_rescale(const struct num *a, unsigned scale) {
    unsigned fa = FRAC(a->scale), fs;
    struct num *r;
    if(a->scale == scale) return num_ref((struct num *)a);
    fs = FRAC(scale);
    if(fs >= fa) {
        if(!(r = alloc((uint64_t)a->len + (fs - fa), scale))) return NULL;
        memset(r->d, 0, (fs - fa) * sizeof(limb_t));
        memcpy(r->d + (fs - fa), a->d, a->len * sizeof(limb_t));
    } else {
        unsigned drop = fa - fs;
        if(!(r = alloc(a->len > drop ? a->len - drop : 0, scale))) return NULL;
        if(r->len) memcpy(r->d, a->d + drop, r->len * sizeof(limb_t));
    }
    r->neg = a->neg;
    cut_digits(r);
    return finish(r);
}

/**** multiplying ****/

/* Columns lo up to hi back to limbs. A carry may run on past hi, into columns that hold limbs. */
static void carry_columns(uint32_t *t, unsigned lo, unsigned hi) {
    uint32_t carry = 0;
    unsigned k;
    for(k = lo; k < hi || carry; k++) {
        uint32_t x = t[k] + carry;
        t[k] = x % NUM_BASE;
        carry = x / NUM_BASE;
    }
}

struct num *num_mul(const struct num *a, const struct num *b, unsigned scale) {
    uint64_t full = (uint64_t)a->scale + b->scale;
    unsigned big = a->scale > b->scale ? a->scale : b->scale, keep, drop, cols, i, j, lo = 0, rows = 0;
    uint32_t small[MUL_SMALL], *t = small;
    struct num *r;
    if(scale > big) big = scale;
    if(full > big) full = big;
    if(full > NUM_MAX_SCALE) {
        num_err = NUM_TOOLONG;
        return NULL;
    }
    scale = (unsigned)full;
    keep = FRAC(scale);
    drop = FRAC(a->scale) + FRAC(b->scale) - keep;
    if(!a->len || !b->len) return zero(scale);
    if((uint64_t)a->len + b->len > NUM_MAX_LIMBS) {
        num_err = NUM_TOOLONG;
        return NULL;
    }
    cols = a->len + b->len;
    if(cols <= drop) return zero(scale);
    if(!(r = alloc(cols - drop, scale))) return NULL;
    if(cols > MUL_SMALL && !(t = malloc(cols * sizeof(*t)))) {
        num_err = NUM_NOMEM;
        num_put(r);
        return NULL;
    }
    memset(t, 0, cols * sizeof(*t));
    if(a->len > b->len) {               /* the fewer rows, the fewer times the columns need carrying */
        const struct num *x = a;
        a = b;
        b = x;
    }
    for(i = 0; i < a->len; i++) {
        uint32_t x = a->d[i], *col = t + i;
        if(!x) continue;                /* only rows that add count: a power of ten is nearly all such */
        for(j = 0; j < b->len; j++) col[j] += x * b->d[j];
        if(++rows == MUL_ROWS) {
            carry_columns(t, lo, i + b->len);
            lo = i + 1;                 /* the columns below take no more */
            rows = 0;
        }
    }
    if(rows) carry_columns(t, lo, cols);
    for(i = drop; i < cols; i++) r->d[i - drop] = (limb_t)t[i];
    if(t != small) free(t);
    r->neg = a->neg != b->neg;
    cut_digits(r);
    return finish(r);
}

/**** dividing ****/

/* limb i of the dividend: a's limbs moved up by shift places, or down when it is below 0 */
static inline unsigned dividend_limb(const struct num *a, int64_t shift, unsigned i) {
    int64_t k = (int64_t)i - shift;
    return k >= 0 && k < a->len ? a->d[k] : 0;
}

/* r gets the top limbs: Knuth's algorithm D (The Art of Computer Programming, 4.3.1) */
static int divide_long(struct num *r, const struct num *a, int64_t shift, unsigned ulen, const limb_t *v, unsigned vlen) {
    limb_t *u = malloc(((size_t)ulen + 1 + vlen) * sizeof(limb_t)), *w;
    uint32_t scale_up, carry = 0, v1, v2, x;
    unsigned i, j = ulen - vlen;
    if(!u) {
        num_err = NUM_NOMEM;
        return -1;
    }
    w = u + ulen + 1;
    /* The top limb of the divisor at least half the base makes the guess below nearly right. */
    scale_up = NUM_BASE / (v[vlen - 1] + 1u);
    for(i = 0; i < ulen; i++) {
        x = dividend_limb(a, shift, i) * scale_up + carry;
        u[i] = x % NUM_BASE;
        carry = x / NUM_BASE;
    }
    u[ulen] = (limb_t)carry;
    carry = 0;
    for(i = 0; i < vlen; i++) {
        x = v[i] * scale_up + carry;
        w[i] = x % NUM_BASE;
        carry = x / NUM_BASE;
    }
    v1 = w[vlen - 1];
    v2 = w[vlen - 2];
    for(;;) {
        uint32_t q, rest, borrow = 0;
        int t;
        x = u[j + vlen] * NUM_BASE + u[j + vlen - 1];
        q = x / v1;
        rest = x % v1;
        while(q >= NUM_BASE || q * v2 > rest * NUM_BASE + u[j + vlen - 2]) {
            q--;
            rest += v1;
            if(rest >= NUM_BASE) break;
        }
        carry = 0;
        for(i = 0; i < vlen; i++) {
            x = q * w[i] + carry;
            carry = x / NUM_BASE;
            t = (int)u[i + j] - (int)(x % NUM_BASE) - (int)borrow;
            borrow = t < 0;
            u[i + j] = (limb_t)(t + (borrow ? (int)NUM_BASE : 0));
        }
        t = (int)u[j + vlen] - (int)carry - (int)borrow;
        if(t < 0) {                     /* rarely, the guess was still one too many: add one back */
            q--;
            carry = 0;
            for(i = 0; i < vlen; i++) {
                x = u[i + j] + (uint32_t)w[i] + carry;
                carry = x >= NUM_BASE;
                u[i + j] = (limb_t)(x - (carry ? NUM_BASE : 0));
            }
        }
        u[j + vlen] = 0;
        r->d[j] = (limb_t)q;
        if(!j) break;
        j--;
    }
    free(u);
    return 0;
}

struct num *num_div(const struct num *a, const struct num *b, unsigned scale) {
    const limb_t *v = b->d;
    unsigned vlen = b->len, low = 0;
    int64_t shift, ulen;
    struct num *r;
    if(!b->len) {
        num_err = NUM_DIVZERO;
        return NULL;
    }
    if(!a->len) return zero(scale);
    /* The quotient is wanted as a whole number: |a / b| * BASE^FRAC(scale), rounded down. The
       divisor's lowest limbs that are 0 divide nothing, and dropping them also from the
       dividend gives the same floor. */
    while(!v[low]) low++;
    v += low;
    vlen -= low;
    shift = (int64_t)FRAC(scale) + FRAC(b->scale) - FRAC(a->scale) - low;
    ulen = (int64_t)a->len + shift;
    if(ulen < vlen) return zero(scale);
    if(!(r = alloc((uint64_t)(ulen - vlen) + 1, scale))) return NULL;
    if(vlen == 1) {
        uint32_t rest = 0, d = v[0];
        unsigned i = (unsigned)ulen;
        while(i--) {
            uint32_t x = rest * NUM_BASE + dividend_limb(a, shift, i);
            r->d[i] = (limb_t)(x / d);
            rest = x % d;
        }
    } else if(divide_long(r, a, shift, (unsigned)ulen, v, vlen) < 0) {
        num_put(r);
        return NULL;
    }
    r->neg = a->neg != b->neg;
    cut_digits(r);
    return finish(r);
}

/* a - (a / b) * b, with a / b to the scale and the rest exact, as POSIX has it */
struct num *num_mod(const struct num *a, const struct num *b, unsigned scale) {
    struct num *q = num_div(a, b, scale), *p, *r;
    if(!q) return NULL;
    p = num_mul(q, b, NUM_EXACT);
    num_put(q);
    if(!p) return NULL;
    r = num_sub(a, p);
    num_put(p);
    return r;
}

/**** powers ****/

/* What the exact power will take is known to be at least this much, so that a number the
   machine cannot hold is refused at once, not after hours of multiplying towards it. The last
   square needs its operand, its result and columns of 32 bits. */
static int power_fits(const struct num *a, unsigned e) {
    uint64_t limbs;
    void *p;
    if(a->len > 1) {
        limbs = (uint64_t)(a->len - 1) * e;
    } else {                            /* d^e has at least e digits for each digit of d after its first */
        unsigned d = a->d[0];
        limbs = d >= 1000 ? (uint64_t)e * 3 / 4 : d >= 100 ? e / 2 : d >= 10 ? e / 4 :
                d >= 2 ? (uint64_t)(e / 40) * 3 : 0;        /* e / 40 first: no 64-bit division here */
    }
    if((uint64_t)a->scale * e > NUM_MAX_SCALE || limbs > NUM_MAX_LIMBS) {
        num_err = NUM_TOOLONG;
        return 0;
    }
    if(limbs < (1u << 20)) return 1;
    if(limbs * 7 >= (1u << 31) || !(p = malloc((size_t)limbs * 7))) {
        num_err = NUM_NOMEM;
        return 0;
    }
    free(p);
    return 1;
}

struct num *num_pow(const struct num *a, const struct num *b, unsigned scale) {
    unsigned e, bit, want;
    struct num *r, *t;
    if(num_hasfrac(b)) {
        num_err = NUM_BADEXP;
        return NULL;
    }
    if(num_whole(b, &e) < 0) {
        num_err = NUM_BIGEXP;
        return NULL;
    }
    if(!e) return num_uint(1);
    want = scale;
    if(!b->neg) {
        uint64_t all = (uint64_t)a->scale * e;
        if(a->scale > want) want = a->scale;
        if(all < want) want = (unsigned)all;
    }
    if(!a->len) {
        if(b->neg) {
            num_err = NUM_DIVZERO;
            return NULL;
        }
        return zero(want);
    }
    if(!power_fits(a, e)) return NULL;
    /* from the top bit down, so that the last multiplications are by the small a */
    for(bit = 31; !(e >> bit & 1); bit--);
    r = num_ref((struct num *)a);
    while(bit--) {
        t = num_mul(r, r, NUM_EXACT);
        num_put(r);
        if(!t) return NULL;
        r = t;
        if(e >> bit & 1) {
            t = num_mul(r, a, NUM_EXACT);
            num_put(r);
            if(!t) return NULL;
            r = t;
        }
    }
    if(b->neg) {
        struct num *one = num_uint(1);
        t = one ? num_div(one, r, scale) : NULL;
        num_put(one);
    } else {
        t = num_rescale(r, want);
    }
    num_put(r);
    return t;
}

/**** square roots ****/

/* |n| * 10^k as a whole number, for a k of at least n's scale */
static struct num *times_ten_to(const struct num *n, uint64_t k) {
    unsigned part = (unsigned)(k % NUM_DIGITS), f = FRAC(n->scale), i;
    uint64_t q = k / NUM_DIGITS;
    uint32_t carry = 0;
    /* The digits below the scale are zero, so a place lost to rounding FRAC up holds nothing. */
    int64_t up = (int64_t)q - f;
    struct num *r;
    if(n->len + up + 1 > (int64_t)NUM_MAX_LIMBS) {
        num_err = NUM_TOOLONG;
        return NULL;
    }
    if(!(r = alloc((uint64_t)(n->len + up + 1), 0))) return NULL;
    if(up > 0) memset(r->d, 0, (size_t)up * sizeof(limb_t));
    for(i = 0; i < n->len; i++) {
        uint32_t x = n->d[i] * pow10[part] + carry;
        carry = x / NUM_BASE;
        if((int64_t)i + up >= 0) r->d[(int64_t)i + up] = x % NUM_BASE;
    }
    r->d[n->len + up] = (limb_t)carry;
    return finish(r);
}

/* a whole number n as n / 10^scale */
static struct num *over_ten_to(const struct num *n, unsigned scale) {
    unsigned part = (NUM_DIGITS - scale % NUM_DIGITS) % NUM_DIGITS, i;
    uint32_t carry = 0;
    struct num *r = alloc((uint64_t)n->len + 1, scale);
    if(!r) return NULL;
    for(i = 0; i < n->len; i++) {
        uint32_t x = n->d[i] * pow10[part] + carry;
        carry = x / NUM_BASE;
        r->d[i] = x % NUM_BASE;
    }
    r->d[n->len] = (limb_t)carry;
    return finish(r);
}

static uint32_t isqrt64(uint64_t x) {
    uint64_t bit = (uint64_t)1 << 62, root = 0;
    while(bit > x) bit >>= 2;
    while(bit) {
        if(x >= root + bit) {
            x -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

/* floor(sqrt(n)) for a whole n above 0: from above, Newton's steps fall to it and then stop */
static struct num *whole_root(const struct num *n) {
    unsigned top = n->len % 2 ? 3 : 4, i, up;
    uint64_t lead = 0;
    uint32_t s;
    struct num *u, *two, *q, *sum, *v;
    if(top > n->len) top = n->len;
    for(i = 0; i < top; i++) lead = lead * NUM_BASE + n->d[n->len - 1 - i];
    /* (s + 1)^2 is above lead, so (s + 1) * BASE^up is above the root */
    s = isqrt64(lead) + 1;
    up = (n->len - top) / 2;
    if(!(u = alloc((uint64_t)up + 4, 0))) return NULL;
    memset(u->d, 0, u->len * sizeof(limb_t));
    u->d[up] = s % NUM_BASE;
    u->d[up + 1] = (s / NUM_BASE) % NUM_BASE;
    u->d[up + 2] = (limb_t)(s / NUM_BASE / NUM_BASE);
    finish(u);
    if(!(two = num_uint(2))) {
        num_put(u);
        return NULL;
    }
    for(;;) {
        q = num_div(n, u, 0);
        sum = q ? num_add(u, q) : NULL;
        v = sum ? num_div(sum, two, 0) : NULL;
        num_put(q);
        num_put(sum);
        if(!v || cmp_size(v, u) >= 0) break;
        num_put(u);
        u = v;
    }
    num_put(two);
    if(!v) {
        num_put(u);
        return NULL;
    }
    num_put(v);
    return u;
}

struct num *num_sqrt(const struct num *a, unsigned scale) {
    unsigned want = scale > a->scale ? scale : a->scale;
    struct num *n, *root, *r;
    if(a->neg) {
        num_err = NUM_NEGROOT;
        return NULL;
    }
    if(!a->len) return zero(want);
    if(!(n = times_ten_to(a, (uint64_t)want * 2))) return NULL;
    root = whole_root(n);
    num_put(n);
    if(!root) return NULL;
    r = over_ten_to(root, want);
    num_put(root);
    return r;
}

/**** looking at a number ****/

int num_whole(const struct num *n, unsigned *v) {
    unsigned f = FRAC(n->scale), i = n->len;
    uint64_t x = 0;
    while(i > f) {
        x = x * NUM_BASE + n->d[--i];
        if(x > 0xffffffffu) return -1;
    }
    *v = (unsigned)x;
    return 0;
}

int num_hasfrac(const struct num *n) {
    unsigned f = FRAC(n->scale), i;
    for(i = 0; i < f && i < n->len; i++) if(n->d[i]) return 1;
    return 0;
}

static unsigned limb_digits(unsigned x) {
    return x >= 1000 ? 4 : x >= 100 ? 3 : x >= 10 ? 2 : 1;
}

/* From the first digit that is not 0 to the end of the scale. Zero has its scale's digits, or the one. */
unsigned num_length(const struct num *n) {
    if(!n->len) return n->scale ? n->scale : 1;
    return (n->len - 1) * NUM_DIGITS + limb_digits(n->d[n->len - 1]) - (FRAC(n->scale) * NUM_DIGITS - n->scale);
}

/**** printing ****/

static void put_decimal(unsigned v, unsigned width, void (*put)(void *, int), void *ctx) {
    char t[10];
    unsigned n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while(v || n < width);
    while(n) put(ctx, t[--n]);
}

static void print_ten(const struct num *n, void (*put)(void *, int), void *ctx) {
    unsigned f = FRAC(n->scale), i = n->len, left = n->scale;
    if(i > f) {
        put_decimal(n->d[--i], 1, put, ctx);
        while(i > f) put_decimal(n->d[--i], NUM_DIGITS, put, ctx);
    }
    if(!left) return;
    put(ctx, '.');
    for(i = f; i--; ) {                 /* a limb past len is zero */
        unsigned v = i < n->len ? n->d[i] : 0, k;
        for(k = 0; k < NUM_DIGITS && left; k++, left--) put(ctx, '0' + (int)(v / pow10[NUM_DIGITS - 1 - k] % 10));
    }
}

/* one digit of a base other than ten: a character up to 16, then a decimal number of a fixed width */
static void put_digit(unsigned v, unsigned base, unsigned width, int space, void (*put)(void *, int), void *ctx) {
    if(base <= 16) {
        put(ctx, "0123456789ABCDEF"[v]);
    } else {
        if(space) put(ctx, ' ');
        put_decimal(v, width, put, ctx);
    }
}

/* The whole part by short division, as many digits at a time as fit 32 bits, which makes
   base 2 and base 16 fast. A group is worth at least 656 (no base's square fits below that,
   and a smaller base has more digits in it), a limb under 14 bits: two groups for each limb
   are enough. */
static int print_whole(const struct num *n, unsigned base, unsigned width, void (*put)(void *, int), void *ctx) {
    unsigned f = FRAC(n->scale), w = n->len > f ? n->len - f : 0, chunk = base, per = 1, count = 0, first, i;
    uint64_t room = ((uint64_t)w * 2 + 2) * sizeof(uint32_t) + (uint64_t)w * sizeof(limb_t);
    uint32_t *groups;
    limb_t *t;
    if(!w) return 0;
    while(chunk * (uint64_t)base <= CHUNK_MAX) {
        chunk *= base;
        per++;
    }
    groups = room < 0x7ff00000u ? malloc((size_t)room) : NULL;
    if(!groups) {
        num_err = NUM_NOMEM;
        return -1;
    }
    t = (limb_t *)(groups + (size_t)w * 2 + 2);
    memcpy(t, n->d + f, w * sizeof(limb_t));
    while(w) {
        uint32_t rest = 0;
        for(i = w; i--; ) {
            uint32_t x = rest * NUM_BASE + t[i];
            t[i] = (limb_t)(x / chunk);
            rest = x % chunk;
        }
        groups[count++] = rest;
        while(w && !t[w - 1]) w--;
    }
    for(first = 1; count--; first = 0) {
        unsigned digits[32], g = groups[count], k = per;
        for(i = 0; i < per; i++) {
            digits[i] = g % base;
            g /= base;
        }
        if(first) while(k > 1 && !digits[k - 1]) k--;       /* no zeros ahead of the first digit */
        while(k--) put_digit(digits[k], base, width, 1, put, ctx);
    }
    free(groups);
    return 0;
}

/* a fraction in limbs, times base: what passes the point */
static uint32_t times_base(limb_t *x, unsigned limbs, unsigned base) {
    uint32_t carry = 0;
    unsigned i;
    for(i = 0; i < limbs; i++) {
        uint32_t v = x[i] * base + carry;
        x[i] = v % NUM_BASE;
        carry = v / NUM_BASE;
    }
    return carry;
}

/* Times the base, and what passes the point is the next digit. A number is known to one unit of its
   last decimal place, so that unit is multiplied along with it, and the digit at which it passes the
   point too is the last that says anything: the Mac's bc prints exactly that many in every base. */
static int print_fraction(const struct num *n, unsigned base, unsigned width, void (*put)(void *, int), void *ctx) {
    unsigned f = FRAC(n->scale), i;
    limb_t *x = malloc((size_t)f * 2 * sizeof(limb_t)), *unit;
    int space = 0;
    if(!x) {
        num_err = NUM_NOMEM;
        return -1;
    }
    unit = x + f;
    for(i = 0; i < f; i++) {
        x[i] = i < n->len ? n->d[i] : 0;
        unit[i] = 0;
    }
    unit[0] = (limb_t)pow10[f * NUM_DIGITS - n->scale];
    put(ctx, '.');
    do {
        put_digit(times_base(x, f, base), base, width, space, put, ctx);
        space = 1;
    } while(!times_base(unit, f, base));
    free(x);
    return 0;
}

int num_print(const struct num *n, unsigned base, void (*put)(void *, int), void *ctx) {
    unsigned width = 0, b;
    if(!n->len) {
        put(ctx, '0');
        return 0;
    }
    if(n->neg) put(ctx, '-');
    if(base == 10) {
        print_ten(n, put, ctx);
        return 0;
    }
    for(b = base - 1; b; b /= 10) width++;
    if(print_whole(n, base, width, put, ctx) < 0) return -1;
    return n->scale ? print_fraction(n, base, width, put, ctx) : 0;
}
