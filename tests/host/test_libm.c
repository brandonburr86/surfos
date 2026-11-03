/* Host-side tests of user/libsurf/math.c (11/01/26, the command-line suite). Every function against the host's libm: within 2
   ulp over a grid of each function's domain, random doubles of every size and the awkward places (multiples of pi/2, around
   1, the edges of overflow); bit for bit where the answer is exact (floor, fmod, sqrt, ...). The special cases C's Annex F
   names, which are checked against this file's own table, not the host's. And the fingerprint of user/bin/libm_grid.h,
   which libtest math repeats inside SurfOS. test_libm -v says the worst seen for each function. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>

#define S1(f) double surf_##f(double);
#define S2(f) double surf_##f(double, double);
S1(sin) S1(cos) S1(tan) S1(asin) S1(acos) S1(atan) S1(sinh) S1(cosh) S1(tanh) S1(exp) S1(log) S1(log2) S1(log10)
S1(sqrt) S1(cbrt) S1(floor) S1(ceil) S1(round) S1(trunc) S1(fabs)
S2(atan2) S2(pow) S2(hypot) S2(fmod)
double surf_modf(double, double *);
double surf_frexp(double, int *);
double surf_ldexp(double, int);
int surf_errno;

static int failures, checks, verbose;
static uint64_t rng = 0x2545F4914F6CDD1DULL;

static uint64_t rnd64(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static uint64_t bits(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static double of(uint64_t u) {
    double d;
    memcpy(&d, &u, 8);
    return d;
}

/* how many doubles apart */
static uint64_t ulps(double a, double b) {
    int64_t x = (int64_t)bits(a), y = (int64_t)bits(b);
    if(a != a || b != b) return a != a && b != b ? 0 : UINT64_MAX;
    if(a == b) return 0;                        /* the two zeros are one number here; the table is strict about them */
    if(x < 0) x = INT64_MIN - x;
    if(y < 0) y = INT64_MIN - y;
    return x > y ? (uint64_t)x - (uint64_t)y : (uint64_t)y - (uint64_t)x;
}

struct fn1 {
    const char *name;
    double (*s)(double), (*h)(double);
    double lo, hi;                              /* the grid */
    int tol;                                    /* ulps allowed */
    uint64_t worst;
    double at;
};

#define F1(f, lo, hi, tol) { #f, surf_##f, f, lo, hi, tol, 0, 0 }
static struct fn1 ones[] = {
    F1(sin, -30, 30, 2), F1(cos, -30, 30, 2), F1(tan, -30, 30, 2), F1(asin, -1, 1, 2), F1(acos, -1, 1, 2), F1(atan, -20, 20, 2),
    F1(sinh, -30, 30, 2), F1(cosh, -30, 30, 2), F1(tanh, -30, 30, 2), F1(exp, -750, 720, 2), F1(log, 0, 1000, 2),
    F1(log2, 0, 1000, 2), F1(log10, 0, 1000, 2), F1(sqrt, 0, 1000, 0), F1(cbrt, -1000, 1000, 2), F1(floor, -100, 100, 0),
    F1(ceil, -100, 100, 0), F1(round, -100, 100, 0), F1(trunc, -100, 100, 0), F1(fabs, -100, 100, 0),
};

struct fn2 {
    const char *name;
    double (*s)(double, double), (*h)(double, double);
    double xlo, xhi, ylo, yhi;
    int tol;
    uint64_t worst;
    double atx, aty;
};

#define F2(f, xlo, xhi, ylo, yhi, tol) { #f, surf_##f, f, xlo, xhi, ylo, yhi, tol, 0, 0, 0 }
static struct fn2 twos[] = {
    F2(atan2, -10, 10, -10, 10, 2), F2(pow, 0, 20, -20, 20, 2), F2(hypot, -1000, 1000, -1000, 1000, 2), F2(fmod, -100, 100, -10, 10, 0),
};

static void try1(struct fn1 *f, double x) {
    double h, s;
    uint64_t u;
    errno = 0;
    h = f->h(x);
    s = f->s(x);
    u = ulps(h, s);
    checks++;
    if(u > (uint64_t)f->tol) {
        failures++;
        if(failures < 40) printf("FAIL: %s(%.17g) = %.17g, host %.17g: %llu ulp\n", f->name, x, s, h, (unsigned long long)u);
    }
    if(u > f->worst) {
        f->worst = u;
        f->at = x;
    }
}

static void try2(struct fn2 *f, double x, double y) {
    double h = f->h(x, y), s = f->s(x, y);
    uint64_t u = ulps(h, s);
    checks++;
    if(u > (uint64_t)f->tol) {
        failures++;
        if(failures < 40)
            printf("FAIL: %s(%.17g, %.17g) = %.17g, host %.17g: %llu ulp\n", f->name, x, y, s, h, (unsigned long long)u);
    }
    if(u > f->worst) {
        f->worst = u;
        f->atx = x;
        f->aty = y;
    }
}

static void grids(int n) {
    size_t i;
    int k, j;
    for(i = 0; i < sizeof(ones) / sizeof(ones[0]); i++) {
        struct fn1 *f = &ones[i];
        for(k = 0; k <= n; k++) try1(f, f->lo + (f->hi - f->lo) * k / n);
        for(k = 0; k < n; k++) {                /* random doubles of every size, with the exponent where it matters */
            double x = of(rnd64());
            if(x != x) continue;
            if(f->s == surf_asin || f->s == surf_acos) x = ldexp(frexp(x, &j), -(int)(rnd64() % 60));
            try1(f, x);
            try1(f, f->lo + (f->hi - f->lo) * (double)(rnd64() >> 11) / 9007199254740992.0);
            try1(f, ldexp(1.0 + (double)(rnd64() >> 12) / 4503599627370496.0, (int)(rnd64() % 64) - 32));
        }
    }
    for(i = 0; i < sizeof(twos) / sizeof(twos[0]); i++) {
        struct fn2 *f = &twos[i];
        for(k = 0; k <= n / 4; k++)
            for(j = 0; j < 4; j++)
                try2(f, f->xlo + (f->xhi - f->xlo) * k / (n / 4), f->ylo + (f->yhi - f->ylo) * (double)(rnd64() >> 11) / 9007199254740992.0);
        for(k = 0; k < n; k++) {
            double x = of(rnd64()), y = of(rnd64());
            if(x != x || y != y) continue;
            if(f->s == surf_pow) {
                x = fabs(ldexp(frexp(x, &j), (int)(rnd64() % 40) - 20));
                y = ldexp(frexp(y, &j), (int)(rnd64() % 14) - 4);
            }
            try2(f, x, y);
        }
    }
}

static void places(void) {
    static const double awkward[] = {
        1.5707963267948966, 3.1415926535897931, 4.7123889803846897, 6.2831853071795862, 0.78539816339744828, 1e22, 1e100, 1e300,
        6381956970095103.0 * 1073741824.0, 5.0e-324, 2.2250738585072014e-308, 1e-300, 1e-20, 1e-10, 1e-5, 0.001, 0.1, 0.25, 0.5,
        0.75, 0.9999999999999999, 1, 1.0000000000000002, 1.5, 2, 2.5, 10, 100, 355, 710, 22, 19, 709.78271289338397,
        709.78271289338409, 710.47586007394386, 710.47586007394398, -708.39641853226408, -745.13321910194111, -745.13321910194122,
    };
    size_t i, k, f;
    /* each awkward place, either side of it, with either sign */
    for(i = 0; i < sizeof(awkward) / sizeof(awkward[0]); i++) {
        for(k = 0; k < 6; k++) {
            double x = k < 2 ? awkward[i] : k < 4 ? nextafter(awkward[i], 1e308) : nextafter(awkward[i], -1e308);
            if(k & 1) x = -x;
            for(f = 0; f < sizeof(ones) / sizeof(ones[0]); f++) try1(&ones[f], x);
            for(f = 0; f < sizeof(twos) / sizeof(twos[0]); f++) {
                size_t j;
                for(j = 0; j < sizeof(awkward) / sizeof(awkward[0]); j += 5) {
                    try2(&twos[f], x, awkward[j]);
                    try2(&twos[f], awkward[j], x);
                }
            }
        }
    }
    /* pow near 1, where log(x) is tiny and y huge: all the precision is in the log */
    for(i = 1; i < 53; i++) {
        double up = ldexp(1.0, -(int)i), x = 1 + up, x2 = 1 - up / 2;
        for(k = 0; k < 16; k++) {
            double y = ldexp(1.0 + (double)k / 16, (int)i - 8 + (int)k / 2);
            try2(&twos[1], x, y);
            try2(&twos[1], x, -y);
            try2(&twos[1], x2, y);
            try2(&twos[1], x2, -y);
        }
    }
    /* near multiples of pi/2, large and small */
    for(i = 1; i < 2000; i++) {
        double x = (double)i * 1.5707963267948966;
        try1(&ones[0], x);
        try1(&ones[1], x);
        try1(&ones[2], x);
    }
    /* whole numbers to whole powers come out right to the last bit, with no host to ask */
    for(i = 2; i < 40; i++) {
        double p = 1;
        for(k = 1; k < 40; k++) {
            p *= (double)i;
            if(p > 9007199254740992.0) break;   /* from here on the double is not the exact power */
            checks++;
            if(surf_pow((double)i, (double)k) != p) {
                failures++;
                printf("FAIL: pow(%d, %d)\n", (int)i, (int)k);
            }
            if(i == 10 && (surf_log10(p) != (double)k)) {
                failures++;
                printf("FAIL: log10(%g)\n", p);
            }
            if(k == 3 && surf_cbrt(p) != (double)i) {
                failures++;
                printf("FAIL: cbrt(%g)\n", p);
            }
        }
    }
    for(k = 0; k < 1000; k++) {
        double p = ldexp(1.0, (int)k - 500);
        checks++;
        if(surf_log2(p) != (double)k - 500) {
            failures++;
            printf("FAIL: log2(2^%d)\n", (int)k - 500);
        }
    }
}

/* the bits of the answer: +0 and -0 are different here */
#define PZ 0x0000000000000000ULL
#define NZ 0x8000000000000000ULL
#define PI_ 0x7FF0000000000000ULL
#define NI 0xFFF0000000000000ULL
#define ONE 0x3FF0000000000000ULL
#define NONE 0xBFF0000000000000ULL
#define NAN_ 0x7FF8000000000000ULL
#define PIB 0x400921FB54442D18ULL
#define NPIB 0xC00921FB54442D18ULL
#define PIO2 0x3FF921FB54442D18ULL
#define NPIO2 0xBFF921FB54442D18ULL
#define PIO4 0x3FE921FB54442D18ULL
#define NPIO4 0xBFE921FB54442D18ULL
#define P3O4 0x4002D97C7F3321D2ULL
#define N3O4 0xC002D97C7F3321D2ULL

static void want(const char *what, double x, double y, double got, uint64_t expected) {
    int nan_wanted = expected == NAN_;
    checks++;
    if(nan_wanted ? got == got : bits(got) != expected) {
        failures++;
        printf("FAIL: %s(%g, %g) is %g (%016llx), Annex F says %016llx\n", what, x, y, got, (unsigned long long)bits(got),
               (unsigned long long)expected);
    }
}

#define W1(f, x, e) want(#f, x, 0, surf_##f(x), e)
#define W2(f, x, y, e) want(#f, x, y, surf_##f(x, y), e)

static void annex_f(void) {
    double inf = of(PI_), nan_ = of(NAN_), pz = 0.0, nz = -0.0;
    /* what returns its argument at +-0 */
    W1(sin, pz, PZ); W1(sin, nz, NZ); W1(tan, pz, PZ); W1(tan, nz, NZ); W1(asin, pz, PZ); W1(asin, nz, NZ); W1(atan, pz, PZ);
    W1(atan, nz, NZ); W1(sinh, pz, PZ); W1(sinh, nz, NZ); W1(tanh, pz, PZ); W1(tanh, nz, NZ); W1(sqrt, pz, PZ); W1(sqrt, nz, NZ);
    W1(cbrt, pz, PZ); W1(cbrt, nz, NZ); W1(floor, nz, NZ); W1(ceil, nz, NZ); W1(round, nz, NZ); W1(trunc, nz, NZ);
    W1(floor, pz, PZ); W1(ceil, pz, PZ); W1(round, pz, PZ); W1(trunc, pz, PZ);
    /* infinities */
    W1(sin, inf, NAN_); W1(sin, -inf, NAN_); W1(cos, inf, NAN_); W1(cos, -inf, NAN_); W1(tan, inf, NAN_); W1(tan, -inf, NAN_);
    W1(atan, inf, PIO2); W1(atan, -inf, NPIO2); W1(sinh, inf, PI_); W1(sinh, -inf, NI); W1(cosh, inf, PI_); W1(cosh, -inf, PI_);
    W1(tanh, inf, ONE); W1(tanh, -inf, NONE); W1(exp, inf, PI_); W1(exp, -inf, PZ); W1(log, inf, PI_); W1(log2, inf, PI_);
    W1(log10, inf, PI_); W1(sqrt, inf, PI_); W1(sqrt, -inf, NAN_); W1(cbrt, inf, PI_); W1(cbrt, -inf, NI); W1(floor, inf, PI_);
    W1(ceil, -inf, NI); W1(round, inf, PI_); W1(trunc, -inf, NI); W1(fabs, -inf, PI_); W1(fabs, nz, PZ);
    W1(asin, inf, NAN_); W1(acos, -inf, NAN_);
    /* zero and one */
    W1(cos, pz, ONE); W1(cos, nz, ONE); W1(cosh, pz, ONE); W1(cosh, nz, ONE); W1(exp, pz, ONE); W1(exp, nz, ONE); W1(acos, 1.0, PZ);
    W1(log, 1.0, PZ); W1(log2, 1.0, PZ); W1(log10, 1.0, PZ); W1(log, pz, NI); W1(log, nz, NI); W1(log2, pz, NI); W1(log10, nz, NI);
    W1(log, -1.0, NAN_); W1(log2, -1.0, NAN_); W1(log10, -1.0, NAN_); W1(sqrt, -1.0, NAN_); W1(asin, 1.5, NAN_); W1(acos, -1.5, NAN_);
    W1(asin, 1.0, PIO2); W1(asin, -1.0, NPIO2); W1(acos, -1.0, PIB);
    /* NaN goes through */
    W1(sin, nan_, NAN_); W1(cos, nan_, NAN_); W1(tan, nan_, NAN_); W1(asin, nan_, NAN_); W1(acos, nan_, NAN_); W1(atan, nan_, NAN_);
    W1(sinh, nan_, NAN_); W1(cosh, nan_, NAN_); W1(tanh, nan_, NAN_); W1(exp, nan_, NAN_); W1(log, nan_, NAN_); W1(log2, nan_, NAN_);
    W1(log10, nan_, NAN_); W1(sqrt, nan_, NAN_); W1(cbrt, nan_, NAN_); W1(floor, nan_, NAN_); W1(ceil, nan_, NAN_); W1(round, nan_, NAN_);
    W1(trunc, nan_, NAN_); W1(fabs, nan_, NAN_);
    /* floor and friends either side of what they round to */
    W1(floor, -0.5, NONE); W1(ceil, -0.5, NZ); W1(round, -0.4, NZ); W1(round, -0.5, NONE); W1(round, 0.5, ONE); W1(round, 0.4, PZ);
    W1(trunc, -0.5, NZ); W1(trunc, 0.5, PZ); W1(floor, 0.5, PZ); W1(ceil, 0.5, ONE);
    W1(round, 0.49999999999999994, PZ); W1(round, 4503599627370497.0, bits(4503599627370497.0)); W1(round, 2.5, bits(3.0)); W1(round, -2.5, bits(-3.0));
    /* atan2 */
    W2(atan2, pz, -1.0, PIB); W2(atan2, nz, -1.0, NPIB); W2(atan2, pz, 1.0, PZ); W2(atan2, nz, 1.0, NZ); W2(atan2, pz, nz, PIB);
    W2(atan2, nz, nz, NPIB); W2(atan2, pz, pz, PZ); W2(atan2, nz, pz, NZ); W2(atan2, 1.0, pz, PIO2); W2(atan2, 1.0, nz, PIO2);
    W2(atan2, -1.0, pz, NPIO2); W2(atan2, -1.0, nz, NPIO2); W2(atan2, 1.0, -inf, PIB); W2(atan2, -1.0, -inf, NPIB);
    W2(atan2, 1.0, inf, PZ); W2(atan2, -1.0, inf, NZ); W2(atan2, inf, 1.0, PIO2); W2(atan2, -inf, 1.0, NPIO2); W2(atan2, inf, -1.0, PIO2);
    W2(atan2, inf, -inf, P3O4); W2(atan2, -inf, -inf, N3O4); W2(atan2, inf, inf, PIO4); W2(atan2, -inf, inf, NPIO4);
    W2(atan2, nan_, 1.0, NAN_); W2(atan2, 1.0, nan_, NAN_);
    /* pow */
    W2(pow, nan_, pz, ONE); W2(pow, nan_, nz, ONE); W2(pow, 1.0, nan_, ONE); W2(pow, 1.0, inf, ONE); W2(pow, 1.0, -inf, ONE);
    W2(pow, 3.0, pz, ONE); W2(pow, -3.0, nz, ONE); W2(pow, inf, pz, ONE); W2(pow, nan_, 1.0, NAN_); W2(pow, 2.0, nan_, NAN_);
    W2(pow, pz, -3.0, PI_); W2(pow, nz, -3.0, NI); W2(pow, pz, -2.0, PI_); W2(pow, nz, -2.0, PI_); W2(pow, pz, -0.5, PI_);
    W2(pow, nz, -0.5, PI_); W2(pow, pz, -inf, PI_); W2(pow, nz, -inf, PI_); W2(pow, pz, 3.0, PZ); W2(pow, nz, 3.0, NZ);
    W2(pow, pz, 2.0, PZ); W2(pow, nz, 2.0, PZ); W2(pow, pz, 0.5, PZ); W2(pow, nz, 0.5, PZ); W2(pow, pz, inf, PZ); W2(pow, nz, inf, PZ);
    W2(pow, -1.0, inf, ONE); W2(pow, -1.0, -inf, ONE); W2(pow, 0.5, -inf, PI_); W2(pow, -0.5, -inf, PI_); W2(pow, 2.0, -inf, PZ);
    W2(pow, -2.0, -inf, PZ); W2(pow, 0.5, inf, PZ); W2(pow, -0.5, inf, PZ); W2(pow, 2.0, inf, PI_); W2(pow, -2.0, inf, PI_);
    W2(pow, -inf, -3.0, NZ); W2(pow, -inf, -2.0, PZ); W2(pow, -inf, -0.5, PZ); W2(pow, -inf, 3.0, NI); W2(pow, -inf, 2.0, PI_);
    W2(pow, -inf, 0.5, PI_); W2(pow, inf, -1.0, PZ); W2(pow, inf, 1.0, PI_); W2(pow, -8.0, 1.0 / 3.0, NAN_); W2(pow, -2.0, 0.5, NAN_);
    W2(pow, -2.0, 3.0, bits(-8.0)); W2(pow, -2.0, 2.0, bits(4.0)); W2(pow, -2.0, -1.0, bits(-0.5)); W2(pow, 2.0, 1024.0, PI_);
    W2(pow, -2.0, 1025.0, NI); W2(pow, 2.0, -1080.0, PZ); W2(pow, -2.0, -1081.0, NZ); W2(pow, 2.0, -1074.0, 1); W2(pow, 10.0, 308.0, bits(1e308));
    W2(pow, -1.0, 9007199254740993.0, ONE);     /* huge odd-looking: that double is even */
    W2(pow, -3.0, 9007199254740991.0, NI);
    /* hypot */
    W2(hypot, inf, nan_, PI_); W2(hypot, nan_, -inf, PI_); W2(hypot, nan_, 1.0, NAN_); W2(hypot, 1.0, nan_, NAN_); W2(hypot, -3.0, pz, bits(3.0));
    W2(hypot, nz, 3.0, bits(3.0)); W2(hypot, 3.0, 4.0, bits(5.0)); W2(hypot, nz, nz, PZ);
    W2(hypot, 3e-320, 4e-320, bits(5e-320));
    /* fmod */
    W2(fmod, pz, 2.0, PZ); W2(fmod, nz, 2.0, NZ); W2(fmod, inf, 2.0, NAN_); W2(fmod, 2.0, pz, NAN_); W2(fmod, 2.0, inf, bits(2.0));
    W2(fmod, -2.0, inf, bits(-2.0)); W2(fmod, nan_, 2.0, NAN_); W2(fmod, 2.0, nan_, NAN_); W2(fmod, 6.0, 3.0, PZ); W2(fmod, -6.0, 3.0, NZ);
    W2(fmod, 5.5, 2.0, bits(1.5)); W2(fmod, -5.5, 2.0, bits(-1.5)); W2(fmod, 5.5, -2.0, bits(1.5)); W2(fmod, 1e308, 3.0, bits(fmod(1e308, 3.0)));
    W2(fmod, 1e308, 5e-324, PZ); W2(fmod, 5e-324, 1e308, 1);
    W2(fmod, 2.2250738585072014e-308, 3e-320, bits(fmod(2.2250738585072014e-308, 3e-320)));
}

static void rest(void) {
    double r, ip;
    int e;
    size_t i;
    static const double xs[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 1.5, -1.5, 3.75, 1e300, -1e300, 1e-300, 5e-324, 2.2250738585072009e-308,
                                 123456.789, 4503599627370496.5, 9007199254740992.0 };
    for(i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        double h, hi;
        int he;
        r = surf_modf(xs[i], &ip);
        h = modf(xs[i], &hi);
        checks++;
        if(bits(r) != bits(h) || bits(ip) != bits(hi)) {
            failures++;
            printf("FAIL: modf(%g) = %g, %g; host %g, %g\n", xs[i], r, ip, h, hi);
        }
        r = surf_frexp(xs[i], &e);
        h = frexp(xs[i], &he);
        checks++;
        if(bits(r) != bits(h) || e != he) {
            failures++;
            printf("FAIL: frexp(%g) = %g, %d; host %g, %d\n", xs[i], r, e, h, he);
        }
    }
    r = surf_modf(of(PI_), &ip);
    checks++;
    if(bits(r) != PZ || bits(ip) != PI_) { failures++; printf("FAIL: modf(inf)\n"); }
    r = surf_modf(of(NI), &ip);
    checks++;
    if(bits(r) != NZ || bits(ip) != NI) { failures++; printf("FAIL: modf(-inf)\n"); }
    r = surf_modf(of(NAN_), &ip);
    checks++;
    if(r == r || ip == ip) { failures++; printf("FAIL: modf(nan)\n"); }
    e = 99;
    r = surf_frexp(of(PI_), &e);
    checks++;
    if(bits(r) != PI_) { failures++; printf("FAIL: frexp(inf)\n"); }
    /* ldexp: exact, rounds correctly into the subnormals, and both ends of int */
    {
        static const int ns[] = { 0, 1, -1, 10, -10, 52, 1023, -1022, -1074, -1075, 1024, 2000, -2000, 2147483647, -2147483647 - 1 };
        size_t k;
        for(i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
            for(k = 0; k < sizeof(ns) / sizeof(ns[0]); k++) {
                double h = ldexp(xs[i], ns[k]), s = surf_ldexp(xs[i], ns[k]);
                checks++;
                if(bits(h) != bits(s)) {
                    failures++;
                    printf("FAIL: ldexp(%g, %d) = %g, host %g\n", xs[i], ns[k], s, h);
                }
            }
        }
    }
    /* errno */
    surf_errno = 0;
    surf_log(-1.0);
    checks++;
    if(surf_errno != EDOM) { failures++; printf("FAIL: log(-1): errno %d\n", surf_errno); }
    surf_errno = 0;
    surf_log(0.0);
    checks++;
    if(surf_errno != ERANGE) { failures++; printf("FAIL: log(0): errno %d\n", surf_errno); }
    surf_errno = 0;
    surf_exp(1000.0);
    checks++;
    if(surf_errno != ERANGE) { failures++; printf("FAIL: exp(1000): errno %d\n", surf_errno); }
    surf_errno = 0;
    surf_pow(10.0, 400.0);
    checks++;
    if(surf_errno != ERANGE) { failures++; printf("FAIL: pow(10, 400): errno %d\n", surf_errno); }
    surf_errno = 0;
    surf_pow(-2.0, 0.5);
    checks++;
    if(surf_errno != EDOM) { failures++; printf("FAIL: pow(-2, 0.5): errno %d\n", surf_errno); }
    surf_errno = 0;
    surf_sqrt(-1.0);
    checks++;
    if(surf_errno != EDOM) { failures++; printf("FAIL: sqrt(-1): errno %d\n", surf_errno); }
    surf_errno = 0;
    surf_sin(1.0);
    surf_exp(1.0);
    surf_pow(2.0, 0.5);
    checks++;
    if(surf_errno) { failures++; printf("FAIL: errno set without a reason: %d\n", surf_errno); }
}

/* the fingerprint, with the library's names */
#define sin surf_sin
#define cos surf_cos
#define tan surf_tan
#define asin surf_asin
#define acos surf_acos
#define atan surf_atan
#define sinh surf_sinh
#define cosh surf_cosh
#define tanh surf_tanh
#define exp surf_exp
#define log surf_log
#define log2 surf_log2
#define log10 surf_log10
#define sqrt surf_sqrt
#define cbrt surf_cbrt
#define floor surf_floor
#define ceil surf_ceil
#define round surf_round
#define trunc surf_trunc
#define fabs surf_fabs
#define modf surf_modf
#define frexp surf_frexp
#define ldexp surf_ldexp
#define pow surf_pow
#define atan2 surf_atan2
#define hypot surf_hypot
#define fmod surf_fmod
#include "../../user/bin/libm_grid.h"

int main(int argc, char **argv) {
    size_t i;
    int n = 3000;
    unsigned long long sum;
    if(argc > 1 && !strcmp(argv[1], "-v")) {
        verbose = 1;
        if(argc > 2) n = atoi(argv[2]);
    }
    grids(n);
    places();
    annex_f();
    rest();
    if(verbose) {
        for(i = 0; i < sizeof(ones) / sizeof(ones[0]); i++)
            printf("%-8s worst %llu ulp at %.17g\n", ones[i].name, (unsigned long long)ones[i].worst, ones[i].at);
        for(i = 0; i < sizeof(twos) / sizeof(twos[0]); i++)
            printf("%-8s worst %llu ulp at %.17g, %.17g\n", twos[i].name, (unsigned long long)twos[i].worst, twos[i].atx, twos[i].aty);
    }
    sum = libm_grid();
    checks++;
    if(sum != LIBM_GRID_SUM) {
        failures++;
        printf("FAIL: the fingerprint is 0x%llXULL, user/bin/libm_grid.h says 0x%llXULL\n", sum, LIBM_GRID_SUM);
    }
    printf("test_libm: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
