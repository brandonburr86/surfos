/* Host-side tests of user/libsurf/strtod.c and of sscanf()'s floating point (11/01/26, the command-line suite): strtod() and
   atof() bit for bit against the host's: its value, where it stops, whether it says ERANGE. Numbers of every shape, the
   ends of the range, and, where long double has the bits to hold one, the exact decimal expansions of the points
   halfway between two doubles, and the digits on either side of them. printf.c has x86 assembly, so sscanf() is
   tested on x86 only; on the Mac, without it, the rest of this file is what runs under the sanitizers. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <float.h>
#include <errno.h>
#include <math.h>

#if defined(__i386__) || defined(__x86_64__)
#define HAVE_SCANF 1
#endif

double surf_strtod(const char *s, char **end);
double surf_atof(const char *s);
int surf_sscanf(const char *s, const char *fmt, ...);
int surf_errno;

/* what printf.c and string.c need */
ssize_t surf_write(int fd, const void *buf, size_t n) { (void)fd; (void)buf; return (ssize_t)n; }
void *surf_malloc(size_t n) { return malloc(n); }

static int failures, checks;
static uint64_t rng = 0x9E3779B97F4A7C15ULL;

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

static int same(double a, double b) {
    if(a != a || b != b) return a != a && b != b;
    return bits(a) == bits(b);
}

/* one string through both */
static void one(const char *s) {
    char *he, *se;
    double h, m;
    int herr, serr;
    errno = 0;
    h = strtod(s, &he);
    herr = errno == ERANGE;
    surf_errno = 0;
    m = surf_strtod(s, &se);
    serr = surf_errno == ERANGE;
    checks++;
    if(!same(h, m) || he != se || (herr != serr && h == h)) {
        failures++;
        if(failures < 40)
            printf("FAIL: [%.100s]: host %.17g (%016llx) stops at %d %s; here %.17g (%016llx) stops at %d %s\n", s, h,
                   (unsigned long long)bits(h), (int)(he - s), herr ? "ERANGE" : "", m, (unsigned long long)bits(m),
                   (int)(se - s), serr ? "ERANGE" : "");
    }
}

static void fixed(void) {
    static const char *const cases[] = {
        "0", "-0", "+0", "0.0", ".0", "0.", "1", "-1", "+1", "1.", ".1", "1.5", "-1.5", "3.14159", "1e0", "1E0", "1e+0", "1e-0",
        "1e1", "1e22", "1e23", "1e-1", "1e-22", "1e-23", "123456789", "12345678901234567890", "0.1", "0.2", "0.3", "1.1", "2.2",
        "1.7976931348623157e308", "1.7976931348623158e308", "1.797693134862315807e308", "1.7976931348623159e308", "1.8e308",
        "1e308", "1e309", "1e400", "1e4000", "1e99999", "1e999999999999999999999", "-1e400",
        "2.2250738585072014e-308", "2.2250738585072011e-308", "2.225073858507201e-308", "4.9406564584124654e-324", "5e-324",
        "2.4703282292062328e-324", "2.4703282292062327e-324", "2.47e-324", "3e-324", "1e-323", "1e-320", "1e-400", "1e-4000",
        "1e-99999", "1e-999999999999999999999", "-1e-400", "0e999999999", "0e-999999999", "0.0e99999", "0e", "0e+", "0x", "0x.",
        "0xg", "0xp1", "0x.p1", "0x1", "0X1", "0x1p0", "0x1P+4", "0x1p-4", "0x1.8p1", "0x.8p1", "0x1.", "0x1.p1", "0xA", "0xa.bp0",
        "0x1.fffffffffffffp1023", "0x1.fffffffffffff8p1023", "0x1p1024", "0x1p-1074", "0x1p-1075", "0x1.8p-1074", "0x1p-1076",
        "0x0.0000000000001p-1022", "0x1.00000000000008p0", "0x1.00000000000018p0", "0x1.00000000000008000000001p0", "0x0p0",
        "0x0p99999999999", "0x1p99999999999", "0x1p-99999999999", "0x123456789abcdef0123456789", "0x.0000000000000000000000001p100",
        "inf", "INF", "Inf", "-inf", "+inf", "infinity", "INFINITY", "Infinity", "-infinity", "infin", "infx", "in", "i",
        "nan", "NAN", "NaN", "-nan", "+nan", "nanx", "nan(", "nan()", "nan(abc)", "nan(a_1)", "nan(a b)", "nan)", "na", "n",
        "", " ", "-", "+", ".", "-.", "+.", "e", "e1", ".e1", "-e1", "1e", "1e+", "1e-", "1ex", "1e+x", "1.5e", "1.e1", "1.e", ".5e1", ".e",
        "--1", "+-1", "1.2.3", "1..2", "  12", "\t12", "\n12", " \t\n\r\f\v12", "12  ", "12abc", "1,5", "1_000", "١٢",
        "00000000000000000000000000000000000001", "0.00000000000000000000000000000000000001", "1000000000000000000000000000000000000",
        "9007199254740993", "9007199254740992.5", "9007199254740993.000000000000000000000000000000001", "18014398509481985",
        "1.00000000000000011102230246251565404236316680908203125", "1.00000000000000011102230246251565404236316680908203124",
        "1.00000000000000011102230246251565404236316680908203126", "1.00000000000000033306690738754696212708950042724609375",
    };
    size_t i;
    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) one(cases[i]);
}

static void long_digits(void) {
    static char b[12000];
    int i, n;
    /* far more digits than matter, with one far out deciding a tie */
    strcpy(b, "1.00000000000000011102230246251565404236316680908203125");
    n = (int)strlen(b);
    memset(b + n, '0', 5000);
    b[n + 5000] = 0;
    one(b);
    b[n + 4999] = '1';
    one(b);
    /* a thousand digits before the point, with an exponent that brings them back */
    for(i = 0; i < 1100; i++) b[i] = (char)('1' + i % 9);
    strcpy(b + 1100, "e-1095");
    one(b);
    strcpy(b + 1100, "e-1100");
    one(b);
    strcpy(b + 1100, "e-800");
    one(b);
    b[1100] = 0;
    one(b);
    /* zeros before and after */
    memset(b, '0', 9000);
    strcpy(b + 9000, "1.5");
    one(b);
    b[0] = '.';
    one(b);
    strcpy(b + 9000, "1e9000");
    one(b);
    strcpy(b + 9000, "1e8990");
    one(b);
    memset(b, '0', 9000);
    b[0] = '0';
    b[1] = '.';
    strcpy(b + 9000, "1");
    one(b);
    /* the digits of a subnormal, in full */
    snprintf(b, sizeof(b), "%.1100e", 5e-324);
    one(b);
    snprintf(b, sizeof(b), "%.1100e", 2.2250738585072014e-308);
    one(b);
    snprintf(b, sizeof(b), "%.1100f", DBL_MAX);
    one(b);
}

static void randoms(int n) {
    char b[64];
    int i;
    for(i = 0; i < n; i++) {
        uint64_t u = rnd64();
        double d = of(u);
        if(d != d) continue;
        snprintf(b, sizeof(b), "%.17g", d);
        one(b);
        snprintf(b, sizeof(b), "%.16e", d);
        one(b);
        snprintf(b, sizeof(b), "%.15g", d);
        one(b);
        snprintf(b, sizeof(b), "%a", d);
        one(b);
        /* a short number at a random power of ten */
        snprintf(b, sizeof(b), "%llue%d", (unsigned long long)(u % 100000000000000000ULL), (int)((u >> 40) % 660) - 345);
        one(b);
        /* a subnormal's neighbourhood */
        snprintf(b, sizeof(b), "%.17g", of(u >> 13));
        one(b);
        snprintf(b, sizeof(b), "%a", of(u >> 13));
        one(b);
    }
}

/* the exact decimal digits of a point halfway between two doubles, and the same plus and minus a little */
static void halfway(int n) {
#if LDBL_MANT_DIG >= 64
    static char b[1500];
    int i;
    for(i = 0; i < n; i++) {
        uint64_t u = rnd64() & ~(1ULL << 63);
        double d;
        long double m;
        size_t len;
        char *e;
        if((u >> 52) == 0x7FF || (u >> 52) == 0x7FE) continue;
        if(i % 4 == 0) u &= (1ULL << 53) - 1;           /* the subnormals too */
        d = of(u);
        m = (long double)d + ((long double)of(u + 1) - (long double)d) / 2;
        snprintf(b, sizeof(b), "%.1100Le", m);
        one(b);
        /* just above: a 1 after the digits, which are all there are */
        e = strchr(b, 'e');
        len = (size_t)(e - b);
        memmove(b + len + 1, e, strlen(e) + 1);
        b[len] = '1';
        one(b);
        /* just below: the last digit that is not 0, one less */
        memmove(b + len, b + len + 1, strlen(b + len + 1) + 1);
        while(len > 2 && (b[len - 1] == '0' || b[len - 1] == '.')) len--;
        if(len > 2) {
            b[len - 1]--;
            one(b);
        }
    }
#else
    (void)n;
    printf("test_strtod: no long double wide enough here: no exact halfway points\n");
#endif
}

/* text that is partly a number, or nearly one */
static void noise(int n) {
    static const char pieces[] = "0123456789..eeEE+-xXpPinfatyINFATY()_ \t";
    char b[48];
    int i, k;
    for(i = 0; i < n; i++) {
        int len = (int)(rnd64() % 32);
        for(k = 0; k < len; k++) b[k] = pieces[rnd64() % (sizeof(pieces) - 1)];
        b[len] = 0;
#ifndef __GLIBC__
        if(strstr(b, "(")) continue;            /* others read more of nan(...) */
#endif
        one(b);
    }
}

#ifdef HAVE_SCANF
static void scan_case(const char *s, const char *fmt) {
    double hd = -7, sd = -7;
    float hf = -7, sf = -7;
    long double hl = -7, sl = -7;
    int hn = -9, sn = -9, hr, sr, width = fmt[1] >= '0' && fmt[1] <= '9';  /* a width that ends inside 1e3 is read differently */
    char f2[32];
    snprintf(f2, sizeof(f2), "%s%%n", fmt);
    checks++;
    if(strchr(fmt, 'L')) {
        hr = sscanf(s, f2, &hl, &hn);
        sr = surf_sscanf(s, f2, &sl, &sn);
        /* the value is the nearest double, as printf's %Lf has it */
        hd = (double)hl;
        sd = (double)sl;
        if(hr != sr || hn != sn || (hr == 1 && !same((double)strtod(s, NULL), sd))) {
            failures++;
            printf("FAIL: sscanf [%s] with %s: host %d %.17g, %d; here %d %.17g, %d\n", s, fmt, hr, hd, hn, sr, sd, sn);
        }
        return;
    }
    if(strchr(fmt, 'l')) {
        hr = sscanf(s, f2, &hd, &hn);
        sr = surf_sscanf(s, f2, &sd, &sn);
        if(hr != sr || (!width && hn != sn) || !same(hd, sd)) {
            failures++;
            printf("FAIL: sscanf [%s] with %s: host %d %.17g, %d; here %d %.17g, %d\n", s, fmt, hr, hd, hn, sr, sd, sn);
        }
    } else {
        hr = sscanf(s, f2, &hf, &hn);
        sr = surf_sscanf(s, f2, &sf, &sn);
        if(hr != sr || (!width && hn != sn) || memcmp(&hf, &sf, 4)) {
            failures++;
            printf("FAIL: sscanf [%s] with %s: host %d %.9g, %d; here %d %.9g, %d\n", s, fmt, hr, (double)hf, hn, sr, (double)sf, sn);
        }
    }
}

static void scans(void) {
    static const char *const nums[] = {
        "3.14", "-2.5", "+7", "1e3", "1.5e-3", ".5", "5.", "0x10", "0x1.8p1", "inf", "-inf", "nan", "1e400", "1e-400", "abc", "",
        "  12.5", "1.1754942e-38", "1.17549435e-38", "1.4e-45", "7e-46", "3.4028235e38", "3.4028237e38", "1.00000017881393432617187499",
        "1.00000017881393432617187501", "16777217", "16777219", "1e39", "0.1", "0.2", "1.5x", "2.5 3.5",
    };
    static const char *const fmts[] = { "%f", "%g", "%e", "%lf", "%lg", "%le", "%a", "%la", "%Lf", "%5f", "%3lf", "%1f", "%2lf", "%F", "%E", "%G" };
    size_t i, k;
    double a = 0, b = 0;
    float f = 0;
    char word[16];
    int n;
    for(i = 0; i < sizeof(nums) / sizeof(nums[0]); i++)
        for(k = 0; k < sizeof(fmts) / sizeof(fmts[0]); k++) scan_case(nums[i], fmts[k]);
    checks++;
    n = surf_sscanf("1.5 2.5", "%lf %lf", &a, &b);
    if(n != 2 || a != 1.5 || b != 2.5) { failures++; printf("FAIL: two doubles: %d %g %g\n", n, a, b); }
    checks++;
    n = surf_sscanf("1.5 2.5", "%*f %f", &f);
    if(n != 1 || f != 2.5f) { failures++; printf("FAIL: a suppressed one: %d %g\n", n, (double)f); }
    checks++;
    n = surf_sscanf("x=1.25;y", "x=%lf;%s", &a, word);
    if(n != 2 || a != 1.25 || strcmp(word, "y")) { failures++; printf("FAIL: among text: %d %g %s\n", n, a, word); }
    checks++;
    n = surf_sscanf("12", "%2lf%lf", &a, &b);          /* the input ends: the count so far */
    if(n != 1 || a != 12) { failures++; printf("FAIL: at the end: %d %g\n", n, a); }
    checks++;
    n = surf_sscanf("", "%lf", &a);
    if(n != EOF) { failures++; printf("FAIL: empty: %d\n", n); }
    /* a width stops the reading at the end of a buffer with no NUL in it; the sanitizers see that here */
    {
        char *end5 = malloc(5);
        memcpy(end5, "12.50", 5);
        checks++;
        n = surf_sscanf(end5, "%5lf", &a);
        if(n != 1 || a != 12.5) { failures++; printf("FAIL: at a buffer's end: %d %g\n", n, a); }
        memcpy(end5, "infin", 5);
        checks++;
        n = surf_sscanf(end5, "%5lf", &a);
        if(n != 1 || a != HUGE_VAL) { failures++; printf("FAIL: inf at a buffer's end: %d %g\n", n, a); }
        memcpy(end5, "nan(a", 5);
        checks++;
        n = surf_sscanf(end5, "%5lf", &a);
        if(n != 1 || a == a) { failures++; printf("FAIL: nan at a buffer's end: %d %g\n", n, a); }
        memcpy(end5, "1e+99", 5);
        checks++;
        n = surf_sscanf(end5, "%5lf", &a);
        if(n != 1 || a != 1e99) { failures++; printf("FAIL: an exponent at a buffer's end: %d %g\n", n, a); }
        memcpy(end5, "0x1p2", 5);
        checks++;
        n = surf_sscanf(end5, "%5lf", &a);
        if(n != 1 || a != 4) { failures++; printf("FAIL: hexadecimal at a buffer's end: %d %g\n", n, a); }
        free(end5);
    }
    /* "100ergs": strtod's answer, as the BSDs; glibc takes the e too, and C says it fails */
    checks++;
    n = surf_sscanf("100ergs", "%lf%15s", &a, word);
    if(n != 2 || a != 100 || strcmp(word, "ergs")) { failures++; printf("FAIL: 100ergs: %d %g %s\n", n, a, word); }
}
#endif

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 40000;
    fixed();
    long_digits();
    randoms(rounds);
    halfway(rounds / 40);
    noise(rounds);
    checks++;
    if(surf_atof("2.5e1x") != 25 || surf_atof("x") != 0) {
        failures++;
        printf("FAIL: atof\n");
    }
    checks++;
    surf_errno = 0;
    surf_strtod("1.5", NULL);
    surf_strtod("nothing", NULL);
    if(surf_errno) {
        failures++;
        printf("FAIL: errno set without a reason\n");
    }
#ifdef HAVE_SCANF
    scans();
#endif
    printf("test_strtod: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
