/* Host tests for bc (user/bc/): num.c, parse.c, vm.c and lib.c as they are, on the host's C library, with their memory
   through tests/host/bc_alloc.h. Numbers against the host's arithmetic and against identities, at sizes around every
   limit of num.c; the language and POSIX's scale rules from tables; the library against known digits, and against
   itself further out, since truncating must give the digits of the true value; a seeded corpus against the system's bc
   where there is one (none in the Docker image; the Mac's is Howard's) and everywhere against the digest of what that bc
   printed; every allocation of a run refused in turn; limits on memory and steps; input cut, damaged and oversized.

   Built and run by `make unittest`. `test_bc long` runs ten times the damaged input. The sanitizers are never part
   of `make unittest` (a -fsanitize build at -m32 hangs under the Docker image's emulation); run them on the Mac by hand:

       cc="clang -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Iuser/bc"
       for f in num parse vm lib; do $cc -include tests/host/bc_alloc.h -c user/bc/$f.c -o /tmp/bc_$f.o; done
       $cc tests/host/test_bc.c /tmp/bc_*.o -o /tmp/test_bc && /tmp/test_bc long
*/
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bc.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/**** memory: bc's goes through these ****/

static size_t live_bytes, peak_bytes, mem_limit;
static long live_blocks, fail_after = -1, alloc_count;

struct hdr { size_t size; size_t pad; };

static int refuse(size_t n) {
    alloc_count++;
    if(fail_after >= 0 && fail_after-- == 0) return 1;
    return mem_limit && live_bytes + n > mem_limit;
}

void *t_malloc(size_t n) {
    struct hdr *h;
    if(refuse(n) || !(h = malloc(sizeof(*h) + n))) return NULL;
    h->size = n;
    live_bytes += n;
    if(live_bytes > peak_bytes) peak_bytes = live_bytes;
    live_blocks++;
    return h + 1;
}

void *t_calloc(size_t n, size_t size) {
    void *p = t_malloc(n * size);
    if(p) memset(p, 0, n * size);
    return p;
}

void t_free(void *p) {
    struct hdr *h;
    if(!p) return;
    h = (struct hdr *)p - 1;
    live_bytes -= h->size;
    live_blocks--;
    free(h);
}

void *t_realloc(void *p, size_t n) {
    struct hdr *h;
    void *q;
    if(!p) return t_malloc(n);
    if(!(q = t_malloc(n))) return NULL;
    h = (struct hdr *)p - 1;
    memcpy(q, p, h->size < n ? h->size : n);
    t_free(p);
    return q;
}

/**** running a program ****/

static char *out;
static size_t nout, capout;
static char errtext[512];
static int errors;

static void cap_put(void *ctx, int c) {
    (void)ctx;
    if(nout + 2 > capout) {
        capout = capout ? capout * 2 : 4096;
        if(!(out = realloc(out, capout))) abort();
    }
    out[nout++] = (char)c;
    out[nout] = 0;
}

static void cap_flush(void *ctx) { (void)ctx; }

static void cap_error(void *ctx, const char *text) {
    (void)ctx;
    if(!errors++) snprintf(errtext, sizeof(errtext), "%s", text);
}

struct bytes { const unsigned char *p, *end; };

static const unsigned char *watch_at;       /* when the reader gets here, how much had been printed is noted */
static size_t printed_then;

static int get_bytes(void *in) {
    struct bytes *b = in;
    if(b->p == watch_at) printed_then = nout;
    return b->p < b->end ? *b->p++ : -1;
}

static const struct bc_host host = { cap_put, cap_flush, cap_error, NULL };

static void clear_output(void) {
    if(!out) cap_put(NULL, 0);
    nout = 0;
    out[0] = 0;
    errtext[0] = 0;
    errors = 0;
}

/* a whole run, from nothing, with everything given back by the end */
static int run_bytes(const void *src, size_t n, int lib, int interactive, unsigned long steps) {
    struct bytes in;
    struct bc *b;
    int rc;
    clear_output();
    in.p = src;
    in.end = in.p + n;
    if(!(b = bc_new(&host))) return BC_TROUBLE;
    bc_set_budget(b, steps);
    rc = lib ? bc_library(b) : BC_END;
    if(rc == BC_END) rc = bc_run(b, get_bytes, &in, "t", interactive);
    bc_free(b);
    CHECK(!live_blocks);
    return rc;
}

static int run(const char *src, int lib, int interactive) {
    return run_bytes(src, strlen(src), lib, interactive, 0);
}

static void said(const char *src, const char *want, int lib) {
    checks++;
    if(strcmp(out, want)) {
        failed++;
        printf("FAIL%s: %.300s\n  got  [%.600s]\n  want [%.600s]\n  (%s)\n", lib ? " -l" : "", src, out, want, errtext);
    }
}

static void is(const char *src, const char *want) {
    run(src, 0, 0);
    said(src, want, 0);
}

static void is_lib(const char *src, const char *want) {
    run(src, 1, 0);
    said(src, want, 1);
}

/* and nothing was said */
static void clean(const char *src, const char *want) {
    int rc = run(src, 0, 0);
    said(src, want, 0);
    checks++;
    if(rc != BC_END && rc != BC_QUIT) {
        failed++;
        printf("FAIL: %.300s\n  ended with trouble: %s\n", src, errtext);
    }
}

/* the digits, wherever the lines broke */
static void digits_are(const char *src, const char *want) {
    size_t i, m = 0;
    run(src, 1, 0);
    for(i = 0; i < nout; i++) {
        if(out[i] == '\\' && out[i + 1] == '\n') i++;
        else out[m++] = out[i];
    }
    out[m] = 0;
    said(src, want, 1);
}

/* as at a terminal: stdout is want, and the run ends well whatever went wrong on the way */
static void at_terminal(const char *src, const char *want, int lib) {
    int rc = run(src, lib, 1);
    CHECK(rc == BC_END || rc == BC_QUIT);
    said(src, want, lib);
}

/* ends with trouble, and the first message has why in it; stdout is what came before */
static void fails_after(const char *src, const char *before, const char *why) {
    int rc = run(src, 0, 0);
    checks++;
    if(rc != BC_TROUBLE || !strstr(errtext, why) || strcmp(out, before)) {
        failed++;
        printf("FAIL: %.300s\n  status %d, out [%.200s], message [%s], wanted [%s]\n", src, rc, out, errtext, why);
    }
}

static void fails(const char *src, const char *why) {
    fails_after(src, "", why);
}

/**** numbers ****/

static unsigned long long prng_state = 88172645463325252ull;

static unsigned next_rand(void) {           /* the same on every host */
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 7;
    prng_state ^= prng_state << 17;
    return (unsigned)(prng_state >> 32);
}

static unsigned below(unsigned n) {
    return next_rand() % n;
}

static struct num *parse(const char *s) {
    int neg = *s == '-';
    struct num *n, *m;
    if(neg) s++;
    if(!(n = num_parse(s, (unsigned)strlen(s), 10))) abort();
    if(!neg) return n;
    m = num_neg(n);
    num_put(n);
    return m;
}

static const char *text_of(const struct num *n) {
    clear_output();
    num_print(n, 10, cap_put, NULL);
    return out;
}

static void text_is(const struct num *n, const char *want) {
    checks++;
    if(strcmp(text_of(n), want)) {
        failed++;
        printf("FAIL: got %.300s, want %.300s\n", out, want);
    }
}

/* value / 10^scale, as bc prints it */
static void fixed_text(char *t, size_t size, long long v, int scale) {
    unsigned long long u = v < 0 ? (unsigned long long)-v : (unsigned long long)v, p = 1;
    char frac[32];
    int i;
    for(i = 0; i < scale; i++) p *= 10;
    if(!v) snprintf(t, size, "0");
    else if(!scale) snprintf(t, size, "%s%llu", v < 0 ? "-" : "", u);
    else {
        snprintf(frac, sizeof(frac), "%0*llu", scale, u % p);
        if(u / p) snprintf(t, size, "%s%llu.%s", v < 0 ? "-" : "", u / p, frac);
        else snprintf(t, size, "%s.%s", v < 0 ? "-" : "", frac);
    }
}

static long long ipow10(int n) {
    long long p = 1;
    while(n-- > 0) p *= 10;
    return p;
}

/* a printed 0 has lost its scale */
static struct num *parse_at(const char *s, unsigned scale) {
    struct num *n = parse(s), *m = num_rescale(n, scale);
    num_put(n);
    return m;
}

/* the four operations and the comparison on numbers of a few digits, against long long */
static void small_numbers(void) {
    int i;
    for(i = 0; i < 4000; i++) {
        int sa = (int)below(5), sb = (int)below(5), s = (int)below(7), sm, ss = sa > sb ? sa : sb;
        long long a = (long long)below(200000000) - 100000000, b = (long long)below(20000) - 10000, want;
        char ta[40], tb[40], w[64];
        struct num *x, *y, *r;
        fixed_text(ta, sizeof(ta), a, sa);
        fixed_text(tb, sizeof(tb), b, sb);
        x = parse_at(ta, (unsigned)sa);
        y = parse_at(tb, (unsigned)sb);
        r = num_add(x, y);
        want = a * ipow10(ss - sa) + b * ipow10(ss - sb);
        fixed_text(w, sizeof(w), want, ss);
        text_is(r, w);
        CHECK(r->scale == (unsigned)ss);
        num_put(r);
        r = num_sub(x, y);
        want = a * ipow10(ss - sa) - b * ipow10(ss - sb);
        fixed_text(w, sizeof(w), want, ss);
        text_is(r, w);
        num_put(r);
        r = num_mul(x, y, (unsigned)s);
        sm = sa + sb;
        if(s > ss) ss = s;
        if(sm > ss) sm = ss;                    /* min(a+b, max(scale, a, b)) */
        want = a * b / ipow10(sa + sb - sm);
        fixed_text(w, sizeof(w), want, sm);
        text_is(r, w);
        CHECK(r->scale == (unsigned)sm);
        num_put(r);
        if(b) {
            r = num_div(x, y, (unsigned)s);
            want = a * ipow10(s + sb) / (b * ipow10(sa));
            fixed_text(w, sizeof(w), want, s);
            text_is(r, w);
            CHECK(r->scale == (unsigned)s);
            num_put(r);
        }
        want = a * ipow10(4 - sa) - b * ipow10(4 - sb);
        CHECK(num_cmp(x, y) == (want > 0 ? 1 : want < 0 ? -1 : 0));
        num_put(x);
        num_put(y);
    }
}

/* digits that find the corners: runs of 0 and of 9, limbs of 5000, or any at all */
static struct num *pattern_number(unsigned most) {
    unsigned n = 1 + below(most), f = below(3) ? 0 : below(n + 1), i = 0, at = 0;
    char *s = malloc(n + 3);
    struct num *r;
    if(!s) abort();
    s[0] = below(2) ? '-' : '+';
    while(i < n) {
        unsigned kind = below(4), run = 1 + below(40);
        while(run-- && i < n) {
            char d = (char)('0' + below(10));
            if(kind == 0) d = '0';
            else if(kind == 1) d = '9';
            else if(kind == 2) d = at++ % 4 ? '0' : '5';
            s[1 + i++] = d;
        }
    }
    s[1 + n] = 0;
    if(f) {
        memmove(s + 1 + (n - f) + 1, s + 1 + (n - f), f + 1);
        s[1 + (n - f)] = '.';
    }
    r = parse(s[0] == '+' ? s + 1 : s);
    free(s);
    return r;
}

static void same(const struct num *a, const struct num *b) {
    checks++;
    if(num_cmp(a, b)) {
        char *t = strdup(text_of(a));
        failed++;
        printf("FAIL: %.300s != %.300s\n", t, text_of(b));
        free(t);
    }
}

static struct num *ten_to_minus(unsigned k) {
    struct num *ten = num_uint(10), *e = num_uint(k), *m = num_neg(e), *r = num_pow(ten, m, k);
    num_put(ten);
    num_put(e);
    num_put(m);
    return r;
}

static struct num *size_of(const struct num *n) {
    return n->neg ? num_neg(n) : num_ref((struct num *)n);
}

/* a = q b + r, with r smaller than one unit of q times b, and the sign of a */
static void division_holds(const struct num *a, const struct num *b, unsigned sc) {
    struct num *q = num_div(a, b, sc), *r = num_mod(a, b, sc), *p = num_mul(q, b, NUM_EXACT), *back = num_add(p, r);
    struct num *unit = ten_to_minus(sc), *lim = num_mul(b, unit, NUM_EXACT), *rs = size_of(r), *ls = size_of(lim);
    same(back, a);
    CHECK(num_cmp(rs, ls) < 0);
    CHECK(!r->len || r->neg == a->neg);
    num_put(q);
    num_put(r);
    num_put(p);
    num_put(back);
    num_put(unit);
    num_put(lim);
    num_put(rs);
    num_put(ls);
}

/* the largest number of its scale whose square is not above a */
static void root_holds(const struct num *a, unsigned sc) {
    unsigned up = sc > a->scale ? sc : a->scale;
    struct num *root = num_sqrt(a, sc), *unit = ten_to_minus(up), *next = num_add(root, unit);
    struct num *low = num_mul(root, root, NUM_EXACT), *high = num_mul(next, next, NUM_EXACT);
    CHECK(root->scale == up);
    CHECK(num_cmp(low, a) <= 0);
    CHECK(num_cmp(high, a) > 0);
    num_put(root);
    num_put(unit);
    num_put(next);
    num_put(low);
    num_put(high);
}

/* identities, at sizes that pass the stack's columns (64 limbs), the rows between carries (40), and one limb and two */
static void big_numbers(void) {
    static const unsigned sizes[] = { 3, 9, 30, 90, 170, 400, 1300 };
    unsigned i;
    for(i = 0; i < 700; i++) {
        unsigned m = sizes[i % 7];
        struct num *a = pattern_number(m), *b = pattern_number(i % 2 ? m : 1 + m / 2), *c = pattern_number(m);
        struct num *s = num_add(a, b), *d = num_sub(s, b), *p = num_mul(a, b, NUM_EXACT), *q = num_mul(b, a, NUM_EXACT);
        struct num *t = num_add(b, c), *u = num_mul(a, t, NUM_EXACT), *v = num_mul(a, c, NUM_EXACT), *w = num_add(p, v);
        same(d, a);                             /* (a + b) - b */
        same(p, q);                             /* a b = b a */
        same(u, w);                             /* a (b + c) = a b + a c */
        if(b->len) {
            struct num *back = num_div(p, b, a->scale);
            same(back, a);                      /* (a b) / b */
            num_put(back);
            division_holds(a, b, below(30));
        }
        if(a->len && !a->neg) root_holds(a, below(30));
        num_put(a);
        num_put(b);
        num_put(c);
        num_put(s);
        num_put(d);
        num_put(p);
        num_put(q);
        num_put(t);
        num_put(u);
        num_put(v);
        num_put(w);
    }
}

/* every base from 2 to 16, whole numbers of up to 64 bits, there and back */
static void bases(void) {
    unsigned base, i;
    for(base = 2; base <= 16; base++) {
        for(i = 0; i < 30; i++) {
            unsigned long long v = ((unsigned long long)next_rand() << 32 | next_rand()) >> below(64), u = v;
            char r[80], t[80], src[200], want[200];
            int n = 0, k = 0;
            do {
                r[n++] = "0123456789ABCDEF"[u % base];
                u /= base;
            } while(u);
            while(n) t[k++] = r[--n];
            t[k] = 0;
            snprintf(src, sizeof(src), "obase=%u\n%llu\n", base, v);
            snprintf(want, sizeof(want), "%s\n", t);
            is(src, want);
            if(k > 1) {                         /* a single digit is its own value in any base */
                snprintf(src, sizeof(src), "ibase=%u\n%s\n", base, t);
                snprintf(want, sizeof(want), "%llu\n", v);
                is(src, want);
            }
        }
    }
}

/* pre characters of text, n digits: the lengths of up to three lines, and all but the last end in a backslash */
static void breaks(unsigned pre, unsigned n, unsigned first, unsigned second, unsigned third) {
    static char t[600];
    unsigned want[3], line = 0, i = 0, len = 0, k = 0;
    int good = 1;
    want[0] = first;
    want[1] = second;
    want[2] = third;
    t[k++] = '"';
    while(k <= pre) t[k++] = 'x';
    t[k++] = '"';
    t[k++] = ';';
    for(i = 0; i < n; i++) t[k++] = '7';
    t[k++] = '\n';
    t[k] = 0;
    run(t, 0, 0);
    for(i = 0; i < nout; i++) {
        if(out[i] != '\n') {
            len++;
            continue;
        }
        if(line > 2 || len != want[line] || (line < 2 && want[line + 1] && out[i - 1] != '\\')) good = 0;
        line++;
        len = 0;
    }
    if(line < 3 && want[line]) good = 0;
    checks++;
    if(!good) {
        failed++;
        printf("FAIL: %u characters and %u digits: [%.400s]\n", pre, n, out);
    }
}

static void parses_as(const char *s, unsigned base, const char *want) {
    struct num *n = num_parse(s, (unsigned)strlen(s), base);
    CHECK(n != NULL);
    if(n) {
        text_is(n, want);
        num_put(n);
    }
}

static void quotient_is(const char *a, const char *b, const char *want) {
    struct num *x = parse(a), *y = parse(b), *r = num_div(x, y, 0);
    text_is(r, want);
    division_holds(x, y, 0);
    num_put(x);
    num_put(y);
    num_put(r);
}

static void rescaled(const char *a, unsigned scale, const char *want) {
    struct num *x = parse(a), *r = num_rescale(x, scale);
    text_is(r, want);
    CHECK(r->scale == scale);
    num_put(x);
    num_put(r);
}

static void numbers(void) {
    struct num *n, *m, *r;
    unsigned i;
    text_is(n = parse("0"), "0");
    CHECK(!n->len && !n->neg);
    num_put(n);
    text_is(n = parse("000.000"), "0");
    CHECK(n->scale == 3 && !n->len);
    num_put(n);
    text_is(n = parse("00123.4500"), "123.4500");
    CHECK(n->scale == 4);
    num_put(n);
    text_is(n = parse(".5"), ".5");
    num_put(n);
    text_is(n = parse("5."), "5");
    CHECK(!n->scale);
    num_put(n);
    text_is(n = num_uint(4294967295u), "4294967295");
    num_put(n);
    n = parse("4294967295.9");
    CHECK(!num_whole(n, &i) && i == 4294967295u);
    num_put(n);
    n = parse("4294967296");
    CHECK(num_whole(n, &i) < 0);
    num_put(n);
    n = parse("1.50");
    CHECK(num_hasfrac(n));
    num_put(n);
    n = parse("12.00");
    CHECK(!num_hasfrac(n));
    num_put(n);
    /* a digit that the base does not have counts at its own value, as on the Mac's bc */
    parses_as("1F", 10, "25");
    parses_as("12", 2, "4");
    parses_as("FFFFFFFF", 10, "166666665");     /* across a limb: 15 of each */
    parses_as(".FF", 10, "1.65");
    /* digits past the scale go, towards zero, and a minus does not stay on zero */
    rescaled("-1.99999", 2, "-1.99");
    rescaled("-1.99999", 0, "-1");
    rescaled("-1.99999", 7, "-1.9999900");
    rescaled("1.99999", 4, "1.9999");
    rescaled("12345.6789", 1, "12345.6");
    rescaled("12345.6789", 5, "12345.67890");
    n = parse("-.009");
    r = num_rescale(n, 2);
    CHECK(!r->len && !r->neg);
    num_put(n);
    num_put(r);
    /* the columns at their fullest: every limb 9999, at sizes around the rows between carries */
    for(i = 30; i < 100; i += 7) {
        unsigned digits = i * 4;
        char *s = malloc(digits + 1);
        memset(s, '9', digits);
        s[digits] = 0;
        n = parse(s);
        r = num_mul(n, n, 0);
        m = num_div(r, n, 0);
        same(m, n);
        CHECK(num_length(r) == digits * 2);
        num_put(m);
        num_put(r);
        num_put(n);
        free(s);
    }
    /* Knuth's step D6, the rare one: the first guess of 9999 passes D3's test and is still one too many */
    quotient_is("4999500000000000", "500000000001", "9998");
    quotient_is("99999999999999999999", "10000000000", "9999999999");
    quotient_is("10000000000000000", "9999", "1000100010001");
    quotient_is("1", "99999999999999999999", "0");
    /* no size wraps */
    n = parse("1.5");
    m = num_uint(4000000000u);
    CHECK(!num_pow(n, m, 0) && num_err == NUM_TOOLONG);
    num_put(n);
    num_put(m);
    n = parse("10");
    m = parse("4294967296");
    CHECK(!num_pow(n, m, 0) && num_err == NUM_BIGEXP);
    num_put(n);
    num_put(m);
    n = parse("1");
    m = parse("3");
    CHECK(!num_div(n, m, NUM_MAX_SCALE) && num_err == NUM_TOOLONG);
    num_put(n);
    num_put(m);
    n = parse("0");
    m = num_rescale(n, 2000000000);
    CHECK(!num_mul(m, m, NUM_EXACT) && num_err == NUM_TOOLONG);         /* two scales, more than one can be */
    num_put(n);
    num_put(m);
    small_numbers();
    big_numbers();
    bases();
    CHECK(!live_blocks);
}

/**** the language ****/

#define P2_500 "32733906078961418700131896968275991522166420460430647894832913680961\\\n" \
               "33796404674554883270092325904157150886684127560071009217256545885393\\\n053328527589376\n"

static void arithmetic(void) {
    is("1+2\n", "3\n");
    is("2+3*4\n", "14\n");
    is("(2+3)*4\n", "20\n");
    is("2^3^2\n", "512\n");                     /* right to left */
    is("-2^2\n", "4\n");                        /* unary minus above ^ */
    is("2-3-4\n", "-5\n");
    is("20/2/5\n", "2\n");
    is("2^-2\n", "0\n");
    is("scale=4;2^-2\n", ".2500\n");
    is("(-2)^3\n", "-8\n");
    is("0^0\n", "1\n");
    is("7/2\n", "3\n");
    is("-7/2\n", "-3\n");
    is("7%3\n", "1\n");
    is("-7%3\n", "-1\n");
    is("7%-3\n", "1\n");
    is("scale=1;7/2\n", "3.5\n");
    is("scale=2;1/3\n", ".33\n");
    is("scale=2;-1/3\n", "-.33\n");
    /* the results' scales, POSIX's rules */
    is("scale=0;3.2/1\n", "3\n");
    is("scale=0;1.5*1.5\n", "2.2\n");           /* min(2, max(0, 1, 1)) */
    is("scale=5;1.5*1.5\n", "2.25\n");          /* min(2, 5) */
    is("scale=0;1.50*1.5\n", "2.25\n");         /* min(3, 2) */
    is("scale(1.50+1.5)\n", "2\n");
    is("scale=3;scale(1/3)\n", "3\n");
    is("scale=3;scale(1.5*2)\n", "1\n");
    is("scale(2.5^3)\n", "1\n");                /* min(3, max(0, 1)) */
    is("scale=2;scale(2.5^3)\n", "2\n");
    is("scale=9;scale(2.5^3)\n", "3\n");
    is("scale=3;scale(2.5^-3)\n", "3\n");
    is("scale=2;scale(0^5)\n", "0\n");
    is("scale(1.5^0)\n", "0\n");
    is("scale=1;10%3\n", ".1\n");               /* a - (a/b)*b to scale + scale(b) */
    is("scale=1;scale(10%3.00)\n", "3\n");
    is("scale=0;scale(10.55%3)\n", "2\n");
    is("scale=2;7.5%2\n", "0\n");
    is("scale=0;7.5%2\n", "1.5\n");
    is("x=1.50;x\n", "1.50\n");
    is("x=1.50;scale(x)\n", "2\n");
    is("x=1.50;x+=1;x\n", "2.50\n");
    is("x=1.5;x++;x\n", "1.5\n2.5\n");          /* ++ keeps the scale */
    is("scale(-1.50)\n", "2\n");
    is("1.500\n", "1.500\n");
    is("0.00\n", "0\n");
    is("-0.0\n", "0\n");
    is(".5\n", ".5\n");
    is("-.5\n", "-.5\n");
    is("00012\n", "12\n");
    is("1.\n", "1\n");
    /* numbers at the edges of limbs */
    is("9999+1\n", "10000\n");
    is("99999999+1\n", "100000000\n");
    is("1-.0001\n", ".9999\n");
    is("1-.00001\n", ".99999\n");
    is("10000-.00001\n", "9999.99999\n");
    is("99999999*99999999\n", "9999999800000001\n");
    is("scale=8;1/10000\n", ".00010000\n");
    is("scale=9;1/10000\n", ".000100000\n");
    is("scale=3;1/10000\n", "0\n");
    is("scale=4;12345.6789*1\n", "12345.6789\n");
    is("scale=2;12345.6789/1\n", "12345.67\n");
    /* sqrt */
    is("sqrt(16)\n", "4\n");
    is("sqrt(15)\n", "3\n");
    is("scale=3;sqrt(2)\n", "1.414\n");
    is("sqrt(2.00)\n", "1.41\n");
    is("scale=2;sqrt(16)\n", "4.00\n");
    is("sqrt(0)\n", "0\n");
    is("scale=4;scale(sqrt(0))\n", "4\n");
    is("sqrt(.25)\n", ".50\n");
    is("sqrt(15.99)\n", "3.99\n");
    is("scale=30;sqrt(2)\n", "1.414213562373095048801688724209\n");
    is("scale=2;sqrt(.0001)\n", ".0100\n");
    is("sqrt(10^40)\n", "100000000000000000000\n");
    is("sqrt(10^40-1)\n", "99999999999999999999\n");
    is("sqrt(1)\n", "1\n");
    is("sqrt(3)\n", "1\n");
    is("sqrt(99980001)\n", "9999\n");
    is("sqrt(99980000)\n", "9998\n");
    /* length: the digits from the first that is not 0 */
    is("length(100)\n", "3\n");
    is("length(1.50)\n", "3\n");
    is("length(0.0012)\n", "2\n");
    is("length(.5)\n", "1\n");
    is("length(0)\n", "1\n");
    is("length(0.00)\n", "2\n");
    is("length(-12.5)\n", "3\n");
    is("scale(length(1.5))\n", "0\n");
    is("length(2^1000)\n", "302\n");
    /* the relations, anywhere */
    is("1<2\n", "1\n");
    is("2<1\n", "0\n");
    is("1==1.00\n", "1\n");
    is("1.5!=1.50\n", "0\n");
    is("-1<1\n", "1\n");
    is("-2<-1\n", "1\n");
    is("0==-0\n", "1\n");
    is("2<=2;2>=2;2>2\n", "1\n1\n0\n");
    is("x=3<5;x\n", "1\n3\n");                  /* (x=3)<5 */
    is("1&&0;1&&2;0||0;0||3;!0;!5\n", "0\n1\n0\n1\n1\n0\n");
    is("x=0;1||(x=1);x\n", "1\n0\n");           /* the right side only when it counts */
    is("x=0;0&&(x=1);x\n", "0\n0\n");
    is("!1==0\n", "1\n");                       /* below ==: !(1==0) */
    /* what is printed */
    is("x=5\n", "");
    is("(x=5)\n", "5\n");
    is("-(x=5)\n", "-5\n");
    is("2*x=4;x\n", "8\n4\n");                  /* an assignment where the name stands */
    is("x=y=3;x;y\n", "3\n3\n");
    is("x=2;x^=3;x\n", "8\n");
    is("x=7;x%=4;x\n", "3\n");
    is("x=7;x/=2;x\n", "3\n");
    is("x=7;x*=2;x\n", "14\n");
    is("x=7;x-=2;x\n", "5\n");
    is("x=1;x++;x\n", "1\n2\n");
    is("x=1;++x;x\n", "2\n2\n");
    is("x=1;x--;--x\n", "1\n-1\n");
    is("x=5;x=-1;x\n", "-1\n");                 /* no =- of old */
    is("x=1;x+=x+=1;x\n", "3\n");                /* x = x + e: from the left, as GNU's (Howard's has 4) */
    is("x=5;y=x++ + ++x;y;x\n", "12\n7\n");
    is("y\n", "0\n");
    is("scale(y)\n", "0\n");
    is("2^500\n", P2_500);
    /* a string counts towards a number's line */
    is("\"abc\"; 2^500\n", "abc32733906078961418700131896968275991522166420460430647894832913680\\\n"
                           "96133796404674554883270092325904157150886684127560071009217256545885\\\n393053328527589376\n");
    /* 69 digits fit a line, the newline making 70; a 70th takes the 69th with it */
    is("10^67\n", "10000000000000000000000000000000000000000000000000000000000000000000\n");
    is("10^68\n", "100000000000000000000000000000000000000000000000000000000000000000000\n");
    is("10^69\n", "10000000000000000000000000000000000000000000000000000000000000000000\\\n00\n");
    fails("1/0\n", "divide by zero");
    fails("1%0\n", "divide by zero");
    fails("0^-1\n", "divide by zero");
    fails("sqrt(-1)\n", "square root of a negative");
    fails("2^.5\n", "not a whole number");
    is("2^2.0\n", "4\n");
    fails("2^4294967296\n", "exponent too large");
}

static void registers(void) {
    is("scale;ibase;obase\n", "0\n10\n10\n");
    is("scale=2.9;scale\n", "2\n");
    is("(scale=2.9)\n", "2\n");
    is("scale=2;scale++;scale\n", "2\n3\n");
    is("scale(scale)\n", "0\n");
    is("obase=16\n255\n", "FF\n");
    is("ibase=16\nFF\n", "255\n");
    is("ibase=16\nibase\n", "16\n");
    is("ibase=16\nA\n", "10\n");
    is("ibase=2\nA\n", "10\n");                 /* a single digit is its own value */
    is("ibase=2\nF\n", "15\n");
    is("ibase=2\n1\n", "1\n");
    is("ibase=2\nobase=A\n101\n", "5\n");
    is("ibase=16\nobase=A\n10\n", "16\n");
    is("ibase=16\nibase=A\n10\n", "10\n");
    is("ibase=2\n.11\n", ".75\n");
    is("ibase=2\n.111\n", ".875\n");
    is("ibase=2\n101.1\n", "5.5\n");
    is("ibase=16\n.FF\n", ".99\n");             /* as many decimal digits as it was written with */
    is("ibase=16\n.8\n", ".5\n");
    is("ibase=8\n777\n", "511\n");
    is("ibase=3\n1\\\n0\n", "3\n");
    is("ibase=8;10\n", "8\n");                  /* the base when it is run, even on one line */
    is("x=2;ibase=x;11\n", "3\n");
    is("obase=2\n.5\n", ".1000\n");
    is("obase=2\n.1\n", ".0001\n");
    is("obase=2\n5.25\n", "101.0100000\n");           /* 2^7 reaches 10^2 */
    is("obase=2\n-5.25\n", "-101.0100000\n");
    is("obase=2\n1.000\n", "1.0000000000\n");
    is("obase=16\n4095.75\n", "FFF.C0\n");
    is("obase=16\n-255\n", "-FF\n");
    is("obase=8\n.1\n", ".06\n");
    is("obase=2\n0\n", "0\n");
    is("obase=2\n0.000\n", "0\n");
    is("obase=11\n120\n", "AA\n");
    is("obase=16\n2^64\n", "10000000000000000\n");
    /* over 16, decimal groups: POSIX's own examples first */
    is("obase=25\n1024\n", " 01 15 24\n");
    is("obase=125\n1024\n", " 008 024\n");
    is("obase=100\n1024.5\n", " 10 24.50\n");
    is("obase=100\n.5\n", ".50\n");
    is("obase=100\n-.5\n", "-.50\n");
    is("obase=100\n0\n", "0\n");
    is("obase=1000\n1234567\n", " 001 234 567\n");
    is("obase=17\n16\n", " 16\n");
    is("obase=101\n100\n", " 100\n");
    is("obase=100000\n123456789\n", " 01234 56789\n");
    is("obase=100000\n100000\n", " 00001 00000\n");
    /* where lines break, every case as the Mac's bc breaks it: text, digits, and each line's length with its backslash */
    breaks(0, 68, 68, 0, 0);
    breaks(0, 69, 69, 0, 0);                    /* its last character may take the 69th place */
    breaks(0, 70, 69, 2, 0);
    breaks(0, 136, 69, 68, 0);
    breaks(0, 137, 69, 69, 0);
    breaks(0, 138, 69, 69, 2);
    breaks(1, 68, 69, 0, 0);
    breaks(1, 69, 69, 2, 0);
    breaks(2, 67, 69, 0, 0);
    breaks(2, 68, 69, 2, 0);
    breaks(66, 3, 69, 0, 0);
    breaks(66, 4, 69, 2, 0);
    breaks(67, 1, 68, 0, 0);
    breaks(67, 2, 69, 0, 0);
    breaks(67, 3, 69, 2, 0);
    breaks(68, 1, 69, 1, 0);                    /* and a number does not start there */
    breaks(68, 2, 69, 2, 0);
    breaks(69, 1, 70, 1, 0);
    breaks(69, 4, 70, 4, 0);
    breaks(100, 3, 101, 3, 0);
    /* a number starts its own line when text has passed the end of one, as on the Mac's bc */
    {
        static const char start[] = "\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
        char t[300], want[300];
        snprintf(t, sizeof(t), "%sxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"\n1\n", start);
        snprintf(want, sizeof(want), "%sxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\\\n1\n", start + 1);
        is(t, want);
        snprintf(t, sizeof(t), "%s\";1\n", start);
        snprintf(want, sizeof(want), "%s\\\n1\n", start + 1);
        is(t, want);
        snprintf(t, sizeof(t), "%.68s\";123\n", start);
        snprintf(want, sizeof(want), "%.67s1\\\n23\n", start + 1);
        is(t, want);
    }
    /* in any base a long number breaks at the character */
    is("obase=16\n2^300\n", "10000000000000000000000000000000000000000000000000000000000000000000\\\n00000000\n");
    fails("scale=-1\n", "scale must be");
    fails("scale=2147483648\n", "scale must be");
    fails("ibase=1\n", "ibase must be");
    fails("ibase=17\n", "ibase must be");
    fails("ibase=0\n", "ibase must be");
    fails("obase=1\n", "obase must be");
    fails("obase=100001\n", "obase must be");
    fails("obase=-5\n", "obase must be");
    /* a digit above the base is taken at its own value: a script from elsewhere goes on. Each was checked on the Mac's bc */
    is("ibase=2\n12\n", "4\n");
    is("ibase=2\n102\n", "6\n");
    is("ibase=2\n.2\n", "1.0\n");
    is("ibase=2\n.12\n", "1.00\n");
    is("ibase=2\n1A\n", "12\n");
    is("ibase=2\nFFFFFFFF\n", "3825\n");
    is("ibase=8\n19\n", "17\n");
    is("1E5\n", "245\n");
    is("1E\n", "24\n");
    is("AB\n", "111\n");
    is(".F\n", "1.5\n");
    is("1.F\n", "2.5\n");
    is("19A\n", "200\n");
    is("FFFFFFFF\n", "166666665\n");
    is("scale=-.5;scale\n", "0\n");             /* cut to 0 first */
    is("scale=2147483647;scale\n", "2147483647\n");
}

static void statements(void) {
    is("1;2;3\n", "1\n2\n3\n");
    is("1;;2\n", "1\n2\n");
    is(";\n\n;\n", "");
    is("", "");
    is("1", "1\n");                             /* a last line without its newline */
    is("/* a */ 1 /* b\nc */ +2 # d\n", "3\n");
    is("# only\n3\n", "3\n");
    is("1 + \\\n2\n", "3\n");
    is("123\\\n456\n", "123456\n");
    is("1\t+ 1\r\n2\r\n", "2\n2\n");            /* a file from elsewhere */
    /* strings, taken as they stand */
    is("\"a\\nb\"\n", "a\\nb");
    is("\"line\nline\"\n", "line\nline");
    is("\"\"\n", "");
    is("\"a\"; \"b\"\n", "ab");
    is("\"h\xc3\xa9llo\"\n", "h\xc3\xa9llo");
    is("\"# no /* comment */\"\n", "# no /* comment */");
    /* if, while, for */
    is("if(1) 2\n", "2\n");
    is("if(0) 2\n", "");
    is("if(1==2) 3 else 4\n", "4\n");
    is("if(1)\n2\n", "2\n");
    is("if(1) { 2; 3 }\n", "2\n3\n");
    is("if(0) { 2 } else { 3 }\n", "3\n");
    is("if(0) 1 else if(0) 2 else 3\n", "3\n");
    is("if(1) if(0) 1 else 2\n", "2\n");        /* else with the nearest */
    is("i=0;while(i<3) { i; i+=1 }\n", "0\n1\n2\n");
    is("i=0;while(i<9) { i+=1; if(i==2) break }; i\n", "2\n");
    is("for(i=0;i<3;i++) i\n", "0\n1\n2\n");
    is("for(i=0;i<3;i++) { if(i==1) continue; i }\n", "0\n2\n");
    is("i=0;while(i<3) { i+=1; if(i==2) continue; i }\n", "1\n3\n");
    is("for(i=1;;i++) { if(i==3) break; i }\n", "1\n2\n");
    is("i=2;for(;i>0;) i--\n", "2\n1\n");
    is("for(i=0;i<3;i++) for(j=0;j<2;j++) { if(j) break; i }\n", "0\n1\n2\n");
    is("for(1;0;2) 3\n", "");                   /* the first and last are not printed */
    is("for(i=0;i<2;i++)\n{\ni\n}\n", "0\n1\n");
    /* quit stops where it is read, whatever it stands in, and what went before on the line runs */
    is("1;quit\n2\n", "1\n");
    is("1;if(0)quit\n2\n", "1\n");
    is("quit\n2\n", "");
    is("{1;quit;2}\n3\n", "");
    is("while(0) quit\n3\n", "");
    is("define f() { quit }\n3\n", "");
    is("1\nquit", "1\n");
    /* arrays */
    is("a[0]=1;a[1]=2;a[0]+a[1]\n", "3\n");
    is("a[5]\n", "0\n");
    is("a[1.9]=7;a[1]\n", "7\n");               /* the index is cut */
    is("a[-.5]=7;a[0]\n", "7\n");
    is("a[1]=5;a=9;a[1];a\n", "5\n9\n");        /* one name, two things */
    is("a[1]=5;a[1]++;a[1]\n", "5\n6\n");
    is("a[1]=5;++a[1]\n", "6\n");
    is("a[1]=5;--a[1];a[1]--;a[1]\n", "4\n4\n3\n");
    is("a[1]=5;a[1]+=2;a[1]*=3;a[1]\n", "21\n");
    is("i=0;a[i++]=i;a[0];i\n", "1\n1\n");      /* the index first, and once */
    is("i=1;a[1]=5;a[i++]+=1;a[1];i\n", "6\n2\n");
    is("a[16777215]=3;a[16777215]\n", "3\n");
    fails("a[16777216]=3\n", "index too large");
    fails("a[16777216]\n", "index too large");
    fails("a[-1]=3\n", "negative array index");
    fails("a[-1]\n", "negative array index");
    /* functions */
    is("define f(x) { return(x*2) }\nf(4)\n", "8\n");
    is("define f(x) { return x*2 }\nf(4)\n", "8\n");
    is("define f(x) {\nreturn(x+1)\n}\nf(1)\n", "2\n");
    is("define f(x)\n{\nreturn(x+1)\n}\nf(1)\n", "2\n");
    is("define f() { }\nf()\n", "0\n");
    is("define f() { return }\nf()\n", "0\n");
    is("define f() { return() }\nf()\n", "0\n");
    is("define f() { return (2)+3 }\nf()\n", "5\n");
    is("define f() { return (2)^3*2 }\nf()\n", "16\n");
    is("define f() { return ((2)) }\nf()\n", "2\n");
    is("define f(x) { if(x) return else return 1 }\nf(1);f(0)\n", "0\n1\n");
    is("define f(x) { if(x) return 1; return 2 }\nf(0);f(1)\n", "2\n1\n");
    is("define foo_bar2(x) { return x+1 }\nfoo_bar2(1)\n", "2\n");
    is("define f(x) { return(x) }\nscale(f(1.50))\n", "2\n");
    is("define f(n) { if(n<2) return 1; return n*f(n-1) }\nf(30)\n", "265252859812191058636308480000000\n");
    is("define f(n) { if(n<2) return n; return f(n-1)+f(n-2) }\nf(15)\n", "610\n");
    is("define f(x) { x; \"s\"; return 1 }\nf(2)\n", "2\ns1\n");    /* a statement in a function prints */
    is("define f() { return 1 }\ndefine f() { return 2 }\nf()\n", "2\n");
    is("define f() { return 1 } f()\n", "1\n");
    is("define g() { return f() }\ndefine f() { return 7 }\ng()\n", "7\n");
    is("define f() { return 10 }\nibase=2\nf()\n", "2\n");          /* constants in the ibase of the call */
    is("define f(x, y) { return x-y }\nf(5, 3)\n", "2\n");
    is("define f(a, a[]) { a[1]=a; return a[1]+1 }\nf(4, b[])\n", "5\n");
    /* the names are dynamic: a function sees its callers' */
    is("define g() { return x }\ndefine f(x) { return g() }\nx=1\nf(2)\nx\n", "2\n1\n");
    is("define g() { return a }\ndefine f() { auto a; a=5; return g() }\na=1\nf()\na\n", "5\n1\n");
    is("define f() { auto a; a }\nf()\n", "0\n0\n");                /* an auto starts at 0 */
    is("define g() { x=9; return 0 }\ndefine f(x) { g(); return x }\nx=1\nf(2)\nx\n", "0\n9\n1\n");
    /* arrays go by value, and autos start empty */
    is("define f(a[]) { a[0]=9; return a[0] }\nb[0]=1\nf(b[])\nb[0]\n", "9\n1\n");
    is("define f(a[]) { return a[0]+a[1000] }\nb[0]=1;b[1000]=2\nf(b[])\n", "3\n");
    is("define f(a[], b[]) { return a[0]*10+b[0] }\na[0]=1;b[0]=2\nf(b[],a[])\n", "21\n");  /* each the other's old array */
    is("define f() { auto a[]; a[0]=7; return a[0] }\na[0]=1\nf()\na[0]\n", "7\n1\n");
    is("define f(n) { auto a[]; a[0]=n; if(n>0) f(n-1); return a[0] }\nf(5)\n", "0\n1\n2\n3\n4\n5\n");
    /* a function's recursion is on the heap */
    is("define f(n) { if(n==0) return 0; return 1+f(n-1) }\nf(100000)\n", "100000\n");
    /* POSIX's example */
    is("scale = 20\n"
       "define e(x) {\n"
       "    auto a, b, c, d, n\n"
       "    a = 1\n    b = 1\n    c = 1\n    d = 0\n    n = 1\n"
       "    while (1 == 1) {\n"
       "        a = a * x\n        b = b * n\n        c = c + a / b\n        n += 1\n"
       "        if (c == d) return(c)\n"
       "        d = c\n"
       "    }\n"
       "}\n"
       "for (i = 1; i <= 3; ++i) {\n    e(i)\n}\n",
       "2.71828182845904523526\n7.38905609893065022713\n20.08553692318766774083\n");
    /* mistakes */
    fails("1+\n", "unexpected end of line");
    fails("(1\n", "unexpected end of line");
    fails("1 2\n", "unexpected number");
    fails("1 \"a\"\n", "unexpected string");
    fails("x y\n", "syntax error near 'y'");
    fails("1 == == 2\n", "syntax error near '=='");
    fails("1 + ;\n", "syntax error near ';'");
    fails("5=1\n", "syntax error near '='");
    fails("x++=1\n", "syntax error near '='");
    fails("++1\n", "unexpected number");
    fails("++\n", "unexpected end of line");
    fails(")\n", "syntax error near ')'");
    fails("if\n", "unexpected end of line");
    fails("if(1\n", "unexpected end of line");
    fails("for(i=0;i<1)\n", "syntax error near ')'");
    fails("for(i=0;i<2;i++);i\n", "syntax error near ';'");    /* POSIX has no empty statement, and Howard's runs i as the body */
    fails("{\n", "unexpected end of input");
    fails("}\n", "syntax error near '}'");
    fails("else\n", "syntax error near 'else'");
    fails("auto a\n", "syntax error near 'auto'");
    fails("1;define f() { }\n", "syntax error near 'define'");
    fails("define\n", "unexpected end of line");
    fails("define f\n", "unexpected end of line");
    fails("define f(\n", "unexpected end of line");
    fails("define f(1) { }\n", "unexpected number");
    fails("define f(a,) { }\n", "syntax error near ')'");
    fails("define f(a, a) { }\n", "a name twice");
    fails("define f(a[], a[]) { }\n", "a name twice");
    fails("define f() { auto a, a; }\n", "a name twice");
    fails("define f() { 1; auto a; }\n", "syntax error near 'auto'");
    fails("define f() { define g() { } }\n", "syntax error near 'define'");
    fails("break\n", "break outside a loop");
    fails("continue\n", "continue outside a loop");
    fails("return\n", "return outside a function");
    fails("\"abc\n", "string without its closing");
    fails("/* abc\n", "comment without its closing");
    fails("@\n", "unexpected character '@'");
    fails("\x01\n", "unexpected character 0x01");
    fails("\xc3\xa9\n", "unexpected character 0xc3");
    fails("Z\n", "unexpected character 'Z'");
    fails("_x\n", "unexpected character '_'");
    fails("1 \\ 2\n", "a backslash outside");
    fails("1\\ 2\n", "a backslash outside");
    fails(".\n", "syntax error near '.'");
    fails(".\\\n\n", "a point without a digit");
    fails("1.2.3\n", "unexpected number");
    fails("x & y\n", "unexpected character '&'");
    fails("x | y\n", "unexpected character '|'");
    fails("f(1)\n", "function f is not defined");
    fails("a[]\n", "an array's name where a value");
    fails("-a[]\n", "an array's name where a value");
    fails("++a[]\n", "an array's name where a value");
    fails("f(a[]+1)\n", "syntax error near '+'");
    fails("length(a[])\n", "an array's name where a value");
    fails("define f(x,y) { }\nf(1)\n", "f takes 2 arguments, not 1");
    fails("define f() { }\nf(1)\n", "f takes 0 arguments, not 1");
    fails("define f(a[]) { }\nf(1)\n", "f: argument 1 should be an array");
    fails("define f(a) { }\nf(a[])\n", "f: argument 1 should not be an array");
    fails("define f() { return 1/0 }\nf()\n", "f: divide by zero");
    fails("define g() { return 1/0 }\ndefine f() { return g() }\nf()\n", "g: divide by zero");
    /* else after a ; and on a later line, as the Mac's bc takes it */
    clean("if(0)2;else 3\n", "3\n");
    clean("if(1)2;else 3\n", "2\n");
    clean("if(0)2; ; else 3\n", "3\n");
    clean("if(0)2;;else 3\n", "3\n");
    clean("if(1)4\nelse 5\n", "4\n");
    clean("if(0)4\nelse 5\n", "5\n");
    clean("if(0)4\n\nelse 5\n", "5\n");
    clean("if(0)4;\nelse 5\n", "5\n");
    clean("if(0)4 # why\n/* so\n */\n\nelse 5\n", "5\n");
    clean("if(0)4 \\\nelse 5\n", "5\n");
    clean("if(0){4}\nelse{5}\n", "5\n");
    clean("{if(0)6\nelse 7}\n", "7\n");
    clean("if(1)if(0)1;else 2;else 3\n", "2\n");
    clean("if(0)if(0)1;else 2;else 3\n", "3\n");
    clean("if(1)if(0)1\nelse 2\nelse 3\n", "2\n");
    clean("define f(x) { if(x) return 1\n else return 2 }\nf(0);f(1)\n", "2\n1\n");
    clean("define f(x) { if(x) return; else return 2 }\nf(0)\n", "2\n");
    clean("for(i=0;i<3;i++) if(i==1) 5; else 6\n", "6\n5\n6\n");
    /* and what is no else is left as it stood */
    clean("if(0)2;3\n", "3\n");
    clean("if(1)2\n3\n", "2\n3\n");
    clean("if(1)2;\n3\n", "2\n3\n");
    clean("if(1)2\n\n\n", "2\n");
    clean("if(1)2", "2\n");
    clean("if(1)2;", "2\n");
    clean("{if(1)2;}\n", "2\n");
    clean("{if(1)2\n}\n", "2\n");
    clean("elsewhere=1\nif(0)2\nelsewhere\n", "1\n");
    clean("e=1\nif(0)2\ne\n", "1\n");
    clean("els=1\nif(0)2\nels\n", "1\n");
    clean("if(1)2\ndefine f() { return 7 }\nf()\n", "2\n7\n");
    clean("if(1)2;\ndefine f() { return 7 }\nf()\n", "2\n7\n");
    clean("if(1)2\n# a comment\n3\n", "2\n3\n");
    clean("x=2\nif(1)x /* a */ /3\n", "0\n");
    clean("if(1)4\n/* a */ 5\n", "4\n5\n");
    clean("if(1)4\n5\nquit\n6\n", "4\n5\n");
    clean("if(1)4\nquit\n6\n", "4\n");
    fails_after("{if(0)6\n;else 7}\n", "", "syntax error near 'else'");
    fails_after("if(0)2;\n;else 3\n", "", "syntax error near 'else'");
    fails_after("if(0)2\n;else 3\n", "", "syntax error near 'else'");
    /* a mistake after it is its own line's, and the if has run */
    fails_after("if(1)4\n$\n", "4\n", "t: line 2:");
    fails_after("if(1)4\n\n\n)\n", "4\n", "t: line 4:");
    fails_after("1\nif(1)1/0\n\n2\n", "1\n", "t: line 2:");
    fails_after("if(0)4\nelse 1/0\n\n2\n", "", "t: line 2:");
    /* at a terminal it needs no line more than the one typed, or the answer would wait for the next */
    {
        static const char typed[] = "if(1)4\n5\n";
        watch_at = (const unsigned char *)typed + 7;
        at_terminal(typed, "4\n5\n", 0);
        CHECK(printed_then == 2);
        watch_at = (const unsigned char *)typed + 6;
        at_terminal(typed, "4\n5\n", 0);
        CHECK(!printed_then);           /* the line's newline is read first */
        watch_at = NULL;
    }
    {
        static const char typed[] = "if(1)4;\n5\n";
        watch_at = (const unsigned char *)typed + 8;
        at_terminal(typed, "4\n5\n", 0);
        CHECK(printed_then == 2);
        watch_at = NULL;
    }
    at_terminal("if(0)2;else 3\n", "3\n", 0);
    at_terminal("if(0)4\nelse 5\n6\n", "6\n", 0);   /* the else is a line of its own there, and a mistake */
    at_terminal("{if(0)4\nelse 5}\n6\n", "5\n6\n", 0);
    at_terminal("define f(x) { if(x) return 1\n else return 2 }\nf(0)\n", "2\n", 0);
    /* a mistake's line */
    fails_after("1\n2\n+\n", "1\n2\n", "t: line 3:");
    fails_after("1\n2\n1/0\n", "1\n2\n", "t: line 3:");
    fails("/* a\nb */\n\n)\n", "t: line 4:");
    fails_after("\"a\nb\"\n)\n", "a\nb", "t: line 3:");
    /* a file or a pipe stops at its first mistake */
    fails_after("1\n1/0\n2\n", "1\n", "divide by zero");
    fails_after("1\n)\n2\n", "1\n", "syntax error");
    /* a line with a mistake runs none of its statements */
    fails("1; 2; )\n3\n", "syntax error");
    /* at a terminal it costs that line and the run goes on */
    at_terminal("1\n)\n2\n", "1\n2\n", 0);
    at_terminal("1/0\n2\n", "2\n", 0);
    at_terminal("1; )\n2\n", "2\n", 0);
    at_terminal("define f() { ) }\n2\n", "2\n", 0);
    at_terminal("x=5\ndefine f(x) { return 1/0 }\nf(1)\nx\n", "5\n", 0);
    at_terminal("define f(x) { auto y; y=1; return g(x) }\ndefine g(y) { return 1/0 }\ny=7;x=6\nf(1)\nx;y\n", "6\n7\n", 0);
    at_terminal("a[0]=1\ndefine f(a[]) { return 1/0 }\nf(a[])\na[0]\n", "1\n", 0);
    at_terminal("1+\n", "", 0);
    /* its whole line, whatever token came before it */
    at_terminal("$ 5\n2\n", "2\n", 0);
    at_terminal("1\n$ 5\n2\n", "1\n2\n", 0);
    at_terminal("1\n$ quit\n2\n", "1\n2\n", 0);
    at_terminal("1\n\n$ 5\n2\n", "1\n2\n", 0);
    at_terminal("1;\n$ 5\n2\n", "1\n2\n", 0);
    at_terminal("1\n@\n2\n", "1\n2\n", 0);          /* the line ends right after the mistake */
    at_terminal("1\n$", "1\n", 0);
    at_terminal("1\n$ 5", "1\n", 0);
    at_terminal("1\n/* never closed\n", "1\n", 0);
    at_terminal("1\n\"never closed\n", "1\n", 0);
    at_terminal("1\n3 \\ 4\n2\n", "1\n2\n", 0);
    at_terminal("scale=5\n1/0\nscale\n", "5\n", 0);
    at_terminal("define f() { scale=9; return 1/0 }\nf()\nscale\n", "9\n", 0);      /* only the library's scale goes back */
}

/**** the library ****/

#define PI "3.141592653589793238462643383279502884197169399375105820974944592307816406286208998628034825342117067"
#define E  "2.718281828459045235360287471352662497757247093699959574966967627724076630353547594571382178525166427"    /* 99 places */

static void truncates(const char *call, const char *why) {
    static const unsigned scales[] = { 0, 1, 7, 20, 37 };
    unsigned i;
    for(i = 0; i < 5; i++) {
        char src[256];
        snprintf(src, sizeof(src), "scale=%u\nx=%s\nscale=%u\ny=%s\nx/1-y\nscale(y)\n", scales[i] + 25, call, scales[i], call);
        run(src, 1, 0);
        checks++;
        snprintf(src, sizeof(src), "0\n%u\n", scales[i]);
        if(strcmp(out, src)) {
            failed++;
            printf("FAIL %s: %s at scale %u: [%s] %s\n", why, call, scales[i], out, errtext);
        }
    }
}

/* What each plan might get wrong at its edges, each value from Python's decimal, which shares no code or method with this.
   e: where f is 0 or just under ln 2, and x below 0. l: far from 1 both ways, a power of 2. j: x much larger than n and equal to
   it, a tiny x, the signs of -n and -x, and an order cut to a whole number. And what needs no working out. */
static void library_edges(void) {
    digits_are("scale=20;l(10^100)\n", "230.25850929940456840179\n");
    digits_are("scale=60;l(10^-50)\n", "-115.129254649702284200899572734218210380055074431438648801666395\n");
    digits_are("scale=20;l(10^1000)\n", "2302.58509299404568401799\n");
    digits_are("scale=20;l(2^64)\n", "44.36141955583649980270\n");
    digits_are("scale=20;l(.5)+l(2)\n", "0\n");
    digits_are("scale=20;e(-50)\n", "0\n");
    digits_are("scale=30;e(.000000000001)\n", "1.000000000001000000000000500000\n");
    digits_are("scale=30;e(-.000000000001)\n", ".999999999999000000000000499999\n");
    digits_are("scale=0;e(100)\n", "26881171418161354484126255515800135873611118\n");
    digits_are("scale=20;e(693.14718)\n", "10715080072002167065095953624953945328001792053694574089957390750811231613753999359372782046692696596708477564721337532818666950736595423188794738140793605761642081592772646958328753003554607651999046408504108777856094212200392117999276216498956836747523642200995393236271475845722928878322778765633026.93484382529239391619\n");
    digits_are("scale=20;j(3,1000)\n", "-.00482742082520394789\n");
    digits_are("scale=20;j(1000,1000)\n", ".04473067294796404088\n");
    digits_are("scale=20;j(0,300.5)\n", "-.01394956002108059669\n");
    digits_are("scale=50;j(2,.001)\n", ".00000012499998958333365885416124131950095847760558\n");
    digits_are("scale=20;j(-3,-2)\n", ".12894324947440205109\n");
    digits_are("scale=20;j(-4,2)\n", ".03399571980756843414\n");
    digits_are("scale=20;j(2.9,1)\n", ".11490348493190048046\n");
    is_lib("j(0,0);j(5,0);j(-5,0);j(100000,1);e(-1000);e(0);l(1)\n",
           "1.00000000000000000000\n0\n0\n0\n0\n1.00000000000000000000\n0\n");
    /* a large x costs a recurrence and no digits for what would otherwise cancel: the value is from Hankel's expansion */
    is_lib("scale=10;j(0,5000)\n", "-.0066489842\n");
    /* ln 2 and pi are kept, and kept for the largest scale asked: a smaller one after it is cut from it, not worked out wrong */
    is_lib("scale=50;x=l(2);scale=10;l(2);scale=50;l(2)-x\n", ".6931471805\n0\n");
}

/* l of numbers of up to a million digits, where x / 2^k must still come near 1: log2 10 to four digits had it 75 bits out
   at a million. The true values from Python's decimal. Seconds each, so only in test_bc long. */
static void library_huge(void) {
    digits_are("x=10^500000; scale=20; l(x)\n", "1151292.54649702284200899572\n");
    digits_are("x=10^1000000; scale=20; l(x)\n", "2302585.09299404568401799145\n");
    digits_are("scale=1000000; x=10^-1000000; scale=20; l(x)\n", "-2302585.09299404568401799145\n");
    digits_are("x=3^2000000; scale=20; l(x)\n", "2197224.57733621938279049047\n");
}

static void library(void) {
    static const char *const es[] = { ".001", ".5", "1", "2.5", "10", "100", "-1", "-.5", "-20", "-46", "1000", ".00000000001" };
    static const char *const ls[] = { ".001", ".5", ".999", "1.001", "2", "10", "1000000", "1.0000001", "10^50",
                                      ".00000000000000000000000000000000000000000000000001", "1.99" };
    static const char *const ts[] = { ".001", ".5", "1", "3", "3.14159", "6.3", "100", "1000000", "-1", "-2.5", ".7854",
                                      "1.5", "355", "-710" };
    static const char *const as[] = { ".001", ".2", ".5", "1", "1.5", "10", "1000000", "-1", "-.7", "10^40", ".9999" };
    static const char *const bs[] = { "0,1", "1,1", "2,3.5", "5,10", "0,30", "-3,2", "3,-2", "10,.5", "20,20", "1,.001" };
    char call[64];
    unsigned i;
    is_lib("scale\n", "20\n");
    is_lib("e(1)\n", "2.71828182845904523536\n");
    is_lib("l(2)\n", ".69314718055994530941\n");
    is_lib("s(1)\n", ".84147098480789650665\n");
    is_lib("c(1)\n", ".54030230586813971740\n");
    is_lib("a(1)\n", ".78539816339744830961\n");
    is_lib("j(0,1)\n", ".76519768655796655144\n");
    is_lib("j(1,1)\n", ".44005058574493351595\n");
    is_lib("e(0)\n", "1.00000000000000000000\n");
    is_lib("c(0)\n", "1.00000000000000000000\n");
    is_lib("j(0,0)\n", "1.00000000000000000000\n");
    is_lib("l(1);s(0);a(0);j(1,0)\n", "0\n0\n0\n0\n");
    is_lib("scale=5\ne(1)\nl(10)\n", "2.71828\n2.30258\n");
    is_lib("scale=0\ne(1)\nl(10);a(1);s(1);c(1);j(0,1)\n", "2\n2\n0\n0\n0\n0\n");
    digits_are("scale=99\ne(1)\n", E "\n");
    digits_are("scale=102\nx=4*a(1)\nscale=99\nx/1\n", PI "\n");
    digits_are("scale=70\nl(10)\n", "2.3025850929940456840179914546843642076011014886287729760333279009675726\n");
    is_lib("scale=60\nl(2)\n", ".693147180559945309417232121458176568075500134360255254120680\n");
    is_lib("scale=60\ns(1)\n", ".841470984807896506652502321630298999622563060798371065672751\n");
    is_lib("scale=60\nc(1)\n", ".540302305868139717400936607442976603732310420617922227670097\n");
    is_lib("scale=40\nj(0,1)\n", ".7651976865579665514497175261026632209092\n");
    /* far from zero: pi is carried far enough. The Mac's bc gets these wrong; the digits are from exact arithmetic */
    is_lib("s(10^30)\n", "-.09011690191213805803\n");
    is_lib("c(10^30)\n", "-.99593119440539570239\n");
    is_lib("e(-100)\n", "0\n");
    is_lib("e(-50)\n", "0\n");
    is_lib("e(100)\n", "26881171418161354484126255515800135873611118.77374192241519160861\n");
    is_lib("a(10^30)\n", "1.57079632679489661923\n");
    is_lib("a(-1)\n", "-.78539816339744830961\n");
    is_lib("j(3,-2)\n", "-.12894324947440205109\n");
    is_lib("j(-3,2)\n", "-.12894324947440205109\n");
    is_lib("j(2.7,1)\n", ".11490348493190048046\n");    /* the order is cut to a whole number */
    is_lib("j(5,100)\n", "-.07419573696451392083\n");
    /* each works at its own scale and gives back the caller's */
    is_lib("scale=3\nl(2)\nscale\n", ".693\n3\n");
    is_lib("x=7;n=8;s=9\ne(1);l(2);s(1);c(1);a(1);j(0,1)\nx;n;s\n",
           "2.71828182845904523536\n.69314718055994530941\n.84147098480789650665\n.54030230586813971740\n"
           ".78539816339744830961\n.76519768655796655144\n7\n8\n9\n");
    /* its constants are in base 10 whatever ibase says, and the result is printed in obase */
    is_lib("ibase=16\ne(1)\n", "2.71828182845904523536\n");
    is_lib("ibase=2\nl(10)\n", ".69314718055994530941\n");
    is_lib("obase=2\nscale=2\ne(0)\n", "1.0000000\n");
    /* a program may take the names; the library does not lean on them */
    is_lib("define e(x) { return 5 }\ne(1)\nl(2)\na(2)\n", "5\n.69314718055994530941\n1.10714871779409050301\n");
    /* out of a person's reach */
    run("_pi()\n", 1, 0);
    CHECK(strstr(errtext, "unexpected character '_'") != NULL);
    run("_fail \"x\"\n", 1, 0);
    CHECK(strstr(errtext, "unexpected character '_'") != NULL);
    /* a mistake inside gives the caller's scale back too */
    run("scale=5\nl(0)\n2\n", 1, 0);
    CHECK(strstr(errtext, "l: not above zero") != NULL);
    at_terminal("scale=5\nl(0)\nscale\n", "5\n", 1);
    at_terminal("scale=5\nl(-1)\nscale\n", "5\n", 1);
    /* the digits are the true value's: the same worked out further */
    for(i = 0; i < sizeof(es) / sizeof(es[0]); i++) {
        snprintf(call, sizeof(call), "e(%s)", es[i]);
        truncates(call, "e");
    }
    for(i = 0; i < sizeof(ls) / sizeof(ls[0]); i++) {
        snprintf(call, sizeof(call), "l(%s)", ls[i]);
        truncates(call, "l");
    }
    for(i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
        snprintf(call, sizeof(call), "s(%s)", ts[i]);
        truncates(call, "s");
        snprintf(call, sizeof(call), "c(%s)", ts[i]);
        truncates(call, "c");
    }
    for(i = 0; i < sizeof(as) / sizeof(as[0]); i++) {
        snprintf(call, sizeof(call), "a(%s)", as[i]);
        truncates(call, "a");
    }
    for(i = 0; i < sizeof(bs) / sizeof(bs[0]); i++) {
        snprintf(call, sizeof(call), "j(%s)", bs[i]);
        truncates(call, "j");
    }
    /* inverses, to the last digits but a few */
    is_lib("scale=40\nx=e(l(7))-7\nif(x<0)x=-x\nx<10^-37\n", "1\n");
    is_lib("scale=40\nx=s(.6)^2+c(.6)^2-1\nif(x<0)x=-x\nx<10^-37\n", "1\n");
    is_lib("scale=40\nx=s(a(.5))/c(a(.5))-.5\nif(x<0)x=-x\nx<10^-37\n", "1\n");
}

/**** the corpus, against the system's bc ****/

static char *corpus;
static size_t ncorpus, capcorpus;

static void add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void add(const char *fmt, ...) {
    va_list ap;
    int n;
    if(ncorpus + 4096 > capcorpus) {
        capcorpus = capcorpus ? capcorpus * 2 : 65536;
        if(!(corpus = realloc(corpus, capcorpus))) abort();
    }
    va_start(ap, fmt);
    n = vsnprintf(corpus + ncorpus, capcorpus - ncorpus, fmt, ap);
    va_end(ap);
    ncorpus += (size_t)n;
}

static void add_number(unsigned most, unsigned base) {
    unsigned n = 1 + below(most), f = below(2) ? below(n + 1) : 0, i;
    for(i = 0; i < n; i++) {
        unsigned d = below(5) ? below(base) : base - 1;
        if(n == 1) d = below(base < 10 ? base : 10);    /* one digit means itself in any base */
        if(n - i == f) add(".");
        add("%c", "0123456789ABCDEF"[d]);
    }
}

static void add_nonzero(void) {
    add("(1+");
    add_number(12, 10);
    add(")");
}

/* an expression that makes no mistake: what it divides by is at least 1, a root's argument is not negative */
static void add_expr(unsigned depth) {
    unsigned k = below(depth ? 11 : 2);
    switch(k) {
    case 0: case 1: add_number(40, 10); break;
    case 2: add("("); add_expr(depth - 1); add("+"); add_expr(depth - 1); add(")"); break;
    case 3: add("("); add_expr(depth - 1); add("-"); add_expr(depth - 1); add(")"); break;
    case 4: add("("); add_expr(depth - 1); add("*"); add_expr(depth - 1); add(")"); break;
    case 5: add("("); add_expr(depth - 1); add("/"); add_nonzero(); add(")"); break;
    /* with 1 added: a remainder at a scale can be so small that its square is cut to a zero, and see comparable() */
    case 6: add("(1+("); add_expr(depth - 1); add("%%"); add_nonzero(); add("))"); break;
    case 7: add("("); add_expr(depth - 1); add("^%u)", below(6)); break;
    case 8: add("(-"); add_expr(depth - 1); add(")"); break;
    case 9: add("sqrt(1+("); add_expr(depth - 1); add(")^2)"); break;
    default: add("(1+"); add_number(30, 10); add(")^-%u", below(4)); break;
    }
}

/* What is printed depends on nothing but the seed. No lengths of fractions below 1 (GNU's bc counts their zeros). */
static void add_programs(void) {
    static const unsigned obases[] = { 2, 3, 7, 8, 16, 17, 25, 100, 125, 1000, 10 };
    unsigned i;
    for(i = 0; i < 500; i++) {
        add("scale=%u\n", below(5) ? below(30) : below(120));
        add_expr(4);
        add("\n");
    }
    for(i = 0; i < 80; i++) {
        unsigned ib = 2 + below(15);
        add("obase=%u\nscale=%u\n", obases[i % 11], below(25));
        add_expr(2);
        add("\n-");
        add_number(100, 10);
        add("\nobase=10\nibase=%u\n", ib);
        add_number(50, ib);
        add("\nibase=A\n");
    }
    add("obase=10\nscale=0\n");
    for(i = 0; i < 60; i++) {
        add("sqrt(");
        add_number(300, 10);
        add(")\n");
        add("length(1");
        add_number(300, 10);
        add(")\nscale(");
        add_number(100, 10);
        add(")\n");
    }
    /* long division, the digits long enough for algorithm D */
    for(i = 0; i < 60; i++) {
        add("scale=%u\n1", below(60));
        add_number(800, 10);
        add("/(1");
        add_number(400, 10);
        add(")\n1");
        add_number(800, 10);
        add("%%(1");
        add_number(400, 10);
        add(")\n");
    }
    add("scale=0\n");
    add("define f(n) { if(n<2) return n; return f(n-1)+f(n-2) }\nfor(i=0;i<20;i++) f(i)\n");
    add("define p(x,n) { auto r; r=1; while(n>0) { if(n%%2==1) r=r*x; x=x*x; n/=2 }; return r }\np(3,200);p(7,99)\n");
    add("define g(a[],n) { auto i,s; for(i=0;i<n;i++) s+=a[i]; a[0]=-1; return s }\n");
    add("for(i=0;i<100;i++) v[i]=i^2\ng(v[],100);v[0]\n");
    add("scale=20\nx=2;for(i=0;i<6;i++) x=(x+2/x)/2\nx\n");
    add("define r(n) { auto x; x=n; if(n>0) r(n-1); return x }\nr(3)\n");
    add("scale=5;\"a string\"; 1/7; \"and another\n\"\n");
}

/* From scale 20: below that Howard's library can be a digit out (c(18.17518) at scale 3 is .780; it is .78109...). And
   no sines of huge numbers: it loses them. */
static void add_library(void) {
    unsigned i;
    for(i = 0; i < 60; i++) {
        unsigned k = i % 6;
        add("scale=%u\n%c(", 20 + below(40), "sclaej"[k]);
        if(k == 5) add("%u,", below(6));
        if(k == 2) add("1+");
        else if(!below(3)) add("-");
        add("%u.", below(40));                  /* one each: the order of a call's arguments is the compiler's */
        add("%u)\n", below(100000));
    }
}

/* What can be compared: without the line breaks of long numbers, as Howard's bc does not break in other bases as GNU's
   does; and values, not scales, as in Howard's a zero loses its scale when it is added (1+.000 is 1 there, and 1.000 in
   POSIX, GNU's and here). The scales are checked above, from POSIX's rules. So trailing zeros go. */
static size_t comparable(const char *s, size_t n, char *clean) {
    size_t i, m = 0, start = 0;
    for(i = 0; i <= n; i++) {
        if(i < n && s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') {
            i++;
            continue;
        }
        if(i == n || s[i] == '\n') {
            if(memchr(clean + start, '.', m - start)) {
                while(clean[m - 1] == '0' || clean[m - 1] == ' ') m--;      /* a base over 16 has spaces */
                if(clean[m - 1] == '.') m--;
            }
            if(i == n) break;
            clean[m++] = '\n';
            start = m;
        } else {
            clean[m++] = s[i];
        }
    }
    clean[m] = 0;
    return m;
}

static unsigned long long digest(const char *s, size_t n) {
    unsigned long long h = 14695981039346656037ull;
    size_t i;
    for(i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h;
}

/* What Howard's bc 6.5.0 printed on the Mac, 11/01/26, as this tree's bc did. After a change to the generator above:
   run this test on the Mac, and look at any difference it shows, before taking the new digest it prints. */
#define CORPUS_DIGEST 0xe31529830835ed98ull
#define CORPUS_SEED 0x5bd1e9955bd1e995ull

static int have_bc(void) {
    return !system("command -v bc > /dev/null 2>&1");
}

static void compare(const char *name, const char *flags) {
    char path[] = "/tmp/test_bc_XXXXXX", cmd[256];
    int fd = mkstemp(path);
    FILE *p;
    char *ref = NULL, *mine, *refc;
    size_t n = 0, cap = 0, nm, nr, i;
    int c, line = 1;
    if(fd < 0 || write(fd, corpus, ncorpus) != (ssize_t)ncorpus) {
        printf("FAIL: cannot write %s\n", path);
        failed++;
        return;
    }
    close(fd);
    snprintf(cmd, sizeof(cmd), "bc %s %s < /dev/null 2>&1", flags, path);
    if(!(p = popen(cmd, "r"))) {
        printf("FAIL: cannot run %s\n", cmd);
        failed++;
        unlink(path);
        return;
    }
    while((c = getc(p)) != EOF) {
        if(n + 2 > cap) {
            cap = cap ? cap * 2 : 65536;
            if(!(ref = realloc(ref, cap))) abort();
        }
        ref[n++] = (char)c;
    }
    pclose(p);
    unlink(path);
    if(!ref) ref = strdup("");
    mine = malloc(nout + 1);
    refc = malloc(n + 1);
    nm = comparable(out, nout, mine);
    nr = comparable(ref, n, refc);
    checks++;
    for(i = 0; i < nm && i < nr && mine[i] == refc[i]; i++) if(mine[i] == '\n') line++;
    if(i < nm || i < nr) {
        size_t start = i;
        while(start && mine[start - 1] != '\n') start--;
        failed++;
        printf("FAIL: %s differs from the system's bc at line %d of the output:\n  mine %.200s\n  bc   %.200s\n", name, line,
               mine + start, start <= nr ? refc + start : "");
    } else {
        printf("test_bc: %s: the same as the system's bc, %lu bytes\n", name, (unsigned long)nm);
    }
    free(ref);
    free(mine);
    free(refc);
}

static void against_bc(void) {
    unsigned long long h1, h2;
    char *clean;
    int ref = have_bc();
    prng_state = CORPUS_SEED;
    ncorpus = 0;
    add_programs();
    run_bytes(corpus, ncorpus, 0, 0, 0);
    CHECK(!errors);
    if(errors) printf("  the corpus: %s\n", errtext);
    clean = malloc(nout + 1);
    h1 = digest(clean, comparable(out, nout, clean));
    free(clean);
    if(ref) compare("the corpus", "");
    ncorpus = 0;
    add_library();
    run_bytes(corpus, ncorpus, 1, 0, 0);
    CHECK(!errors);
    clean = malloc(nout + 1);
    h2 = digest(clean, comparable(out, nout, clean));
    free(clean);
    if(ref) compare("the library's corpus", "-l");
    else printf("test_bc: no bc here: the comparison is skipped, and the digest of the Mac's bc checked instead\n");
    checks++;
    if((h1 ^ (h2 * 31)) != CORPUS_DIGEST) {
        failed++;
        printf("FAIL: the corpus's digest is 0x%llxull\n", h1 ^ (h2 * 31));
    }
}

/**** memory running out ****/

/* every allocation of a run refused, one at a time: a message or a clean end, nothing left behind */
static void refuse_each(const char *src, int lib, int interactive) {
    long n;
    for(n = 0; n < 200000; n++) {
        long used;
        int rc;
        fail_after = n;
        alloc_count = 0;
        rc = run(src, lib, interactive);
        used = alloc_count;
        checks++;
        if(fail_after >= 0) {                   /* all it asked for was had */
            fail_after = -1;
            return;
        }
        if(!interactive && rc == BC_TROUBLE && errors && !strstr(errtext, "out of memory")) {
            failed++;
            printf("FAIL: allocation %ld of %ld: %s\n", n, used, errtext);
        }
    }
    fail_after = -1;
    failed++;
    printf("FAIL: refuse_each did not end: %s\n", src);
}

static void limited(const char *src, int lib, size_t limit, const char *want) {
    int rc;
    mem_limit = limit;
    rc = run(src, lib, 0);
    mem_limit = 0;
    checks++;
    if(rc != BC_TROUBLE || !strstr(errtext, want)) {
        failed++;
        printf("FAIL: under %lu bytes: %s\n  status %d, message [%s], wanted [%s]\n", (unsigned long)limit, src, rc, errtext, want);
    }
}

static void memory(void) {
    static const char *const programs[] = {
        "x=2^300;y=x*x;z=y/7;w=z%13;sqrt(y);obase=16;y;.5\n",
        "scale=30;1/7;sqrt(2);obase=25;2^100;3.5\n",
        "define f(n) { auto a[], b; a[n]=n; if(n>0) return f(n-1)+a[n]; return 0 }\nf(40)\n",
        "define g(a[]) { a[1]=2; return a[0]+a[1] }\nb[0]=1;b[5000]=2;g(b[])\n\"text\"\n",
        "ibase=16;FFFF.8;ibase=A;for(i=0;i<3;i++) { i^40; if(i==1) break }\n",
        "x[3]=1;x[3]++;x[3]+=2;--x[3];x[3]\n",
    };
    unsigned i;
    size_t before;
    for(i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
        refuse_each(programs[i], 0, 0);
        refuse_each(programs[i], 0, 1);         /* and the next line then runs */
    }
    refuse_each("e(1);l(2);s(1);c(1);a(2);j(1,1)\n", 1, 0);
    /* the limit, and a clear message: on a number, an array, the calls, the code */
    limited("x=2\nwhile(1) x=x*x\n", 0, 1u << 18, "out of memory");
    limited("x=10^100000000\n", 0, 1u << 20, "out of memory");  /* at once, not after the multiplying */
    limited("2^4000000000\n", 0, 1u << 20, "out of memory");
    limited("define f(n) { return f(n+1) }\nf(0)\n", 0, 1u << 20, "f: out of memory");
    limited("define f(n) { auto a; return f(n+1)+1 }\nf(0)\n", 0, 1u << 20, "f: out of memory");
    limited("for(i=0;;i++) a[i*1024]=i\n", 0, 1u << 20, "out of memory");
    limited("for(i=0;;i++) a[i]=i\n", 0, 1u << 20, "out of memory");
    limited("scale=10000000\n1/3\n", 0, 1u << 20, "out of memory");
    limited("scale=2147483647\n1/3\n", 0, 1u << 20, "number too long");
    limited("x=.1^2147483647\n", 0, 1u << 20, "number too long");
    limited("scale=2000000000\nsqrt(2)\n", 0, 1u << 20, "number too long");
    limited("scale=100000000\ne(1)\n", 1, 1u << 20, "out of memory");
    /* a sparse array costs little */
    peak_bytes = 0;
    before = live_bytes;
    is("a[16777215]=1;a[0]=2;a[8000000]=3;a[16777215]+a[0]+a[8000000]\n", "6\n");
    CHECK(peak_bytes - before < 256 * 1024);
}

/**** input cut, damaged and oversized ****/

static const char *const seeds[] = {
    "define f(x, y[]) {\n    auto a, b[]\n    a = x; b[1] = a\n    if(x > 0) { return(f(x-1, y[]) + b[1]) } else return 1\n}\n"
    "c[0]=5\nf(3, c[])\n\"str\"\n",
    "scale=9;for(i=0;i<5;i++) { if(i==3) continue; x+=i/7; if(x>1) break }\nx;sqrt(x);length(x);scale(x)\n",
    "ibase=16;obase=2;FF.8;ibase=A;obase=A;-.5^2;a[1]=2;a[1]++;--a[1];a[1]%=3;a[1]\n",
    "/* a comment */ 1 + \\\n2 # another\n1\\\n2.3\\\n4\nwhile(y<3) y+=1\ny\n!y||y&&0\n",
    "obase=25;2^99;obase=100;1024.5;obase=1000;1234567\n",
};

static void damaged(int rounds) {
    static const char tokens[][8] = { "(", ")", "[", "]", "{", "}", ";", ",", "\n", "\"", "\\\n", "/*", "*/", "#", "++", "--", "=",
                                      "^", "%", "==", "&&", "||", "!", ".", "9", "A", "F", "x", "f", "a[]", "quit", "break",
                                      "continue", "return", "define", "auto", "if", "else", "for", "while", "scale", "ibase",
                                      "obase", "length", "sqrt", "99999", "0", "-", "/", "\xff", "\x01" };
    char t[1024];
    int r, m;
    unsigned i;
    /* cut at every byte: any end of input is taken */
    for(i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        size_t n = strlen(seeds[i]), k;
        for(k = 0; k < n; k++) {
            run_bytes(seeds[i], k, 0, 0, 2000);
            run_bytes(seeds[i], k, 0, 1, 2000);
        }
    }
    /* any bytes at all */
    for(r = 0; r < rounds; r++) {
        unsigned n = below(sizeof(t));
        for(i = 0; i < n; i++) t[i] = (char)next_rand();
        run_bytes(t, n, r % 2, r % 3 == 0, 2000);
    }
    /* programs with their tokens changed: small limits, as a changed program may well loop or grow */
    mem_limit = 1u << 16;
    for(r = 0; r < rounds * 5; r++) {
        const char *s = seeds[below(sizeof(seeds) / sizeof(seeds[0]))];
        size_t n = strlen(s);
        memcpy(t, s, n);
        for(m = 1 + (int)below(4); m > 0 && n; m--) {
            size_t at = below((unsigned)n), len = 1 + below(4);
            const char *put = tokens[below(sizeof(tokens) / sizeof(tokens[0]))];
            size_t pl = strlen(put);
            if(at + len > n) len = n - at;
            if(n - len + pl >= sizeof(t)) continue;
            memmove(t + at + pl, t + at + len, n - at - len);
            memcpy(t + at, put, pl);
            n = n - len + pl;
        }
        if(!(r % 4)) fail_after = (long)below(120);     /* and some of them short of memory at any moment too */
        run_bytes(t, n, 0, r % 3 != 0, 2000);
        fail_after = -1;
    }
    mem_limit = 0;
}

static char *repeated(const char *a, unsigned n, const char *mid, const char *b, unsigned m, const char *end) {
    size_t la = strlen(a), lb = strlen(b), lm = strlen(mid);
    char *s = malloc(la * n + lm + lb * m + strlen(end) + 1), *p = s;
    unsigned i;
    if(!s) abort();
    for(i = 0; i < n; i++, p += la) memcpy(p, a, la);
    memcpy(p, mid, lm);
    p += lm;
    for(i = 0; i < m; i++, p += lb) memcpy(p, b, lb);
    strcpy(p, end);
    return s;
}

static void too_deep(const char *open, const char *mid, const char *close) {
    char *s = repeated(open, 100000, mid, close, 100000, "\n");
    run(s, 0, 0);
    checks++;
    if(!strstr(errtext, "nested too deeply")) {
        failed++;
        printf("FAIL: %s...: %s\n", open, errtext);
    }
    free(s);
}

static void oversized(void) {
    char *s;
    unsigned i;
    /* nesting stops with a message, not by running over the stack */
    too_deep("(", "1", ")");
    too_deep("{", "1", "}");
    too_deep("- ", "1", "");
    too_deep("!", "1", "");
    too_deep("if(1) ", "1", "");
    too_deep("x=", "1", "");
    too_deep("2^", "1", "");
    too_deep("a[", "1", "]");
    too_deep("f(", "1", ")");
    too_deep("length(", "1", ")");
    too_deep("sqrt(", "1", ")");
    too_deep("for(;;) ", "1", "");
    too_deep("while(1) ", "1", "");
    /* and the most that is allowed runs */
    s = repeated("(", 300, "7", ")", 300, "\n");
    is(s, "7\n");
    free(s);
    /* long lists are no nesting */
    s = repeated("1+", 100000, "1", "", 0, "\n");
    is(s, "100001\n");
    free(s);
    s = repeated("1;", 20000, "", "", 0, "\n");
    run(s, 0, 0);
    CHECK(nout == 40000);
    free(s);
    /* a number, a name, a string and a comment of a megabyte */
    s = repeated("1234567890", 100000, "", "", 0, "\n");
    run(s, 0, 0);
    CHECK(nout > 1000000 && !errors);
    free(s);
    s = repeated("length(", 1, "", "9", 1000000, ")\n");
    is(s, "1000000\n");
    free(s);
    s = repeated("xy", 500000, "=4\n", "xy", 500000, "\n");
    is(s, "4\n");
    free(s);
    s = repeated("\"", 1, "", "ab", 500000, "\"\n");
    run(s, 0, 0);
    CHECK(nout == 1000000 && !errors);
    free(s);
    s = repeated("/*", 1, "", " * / ", 200000, "*/ 5\n");
    is(s, "5\n");
    free(s);
    /* many names, and many arguments */
    {
        size_t cap = 3000 * 16 + 64, n = 0;
        s = malloc(cap);
        for(i = 0; i < 3000; i++) n += (size_t)snprintf(s + n, cap - n, "v%u=%u\n", i, i);
        memcpy(s + n, "v2999+v1\n", 10);
        is(s, "3000\n");
        free(s);
    }
    {
        size_t cap = 2000 * 32 + 256, n = 0;
        s = malloc(cap);
        n += (size_t)snprintf(s + n, cap - n, "define f(");
        for(i = 0; i < 1000; i++) n += (size_t)snprintf(s + n, cap - n, "%sp%u", i ? "," : "", i);
        n += (size_t)snprintf(s + n, cap - n, ") { return p999 }\nf(");
        for(i = 0; i < 1000; i++) n += (size_t)snprintf(s + n, cap - n, "%s%u", i ? "," : "", i);
        snprintf(s + n, cap - n, ")\n");
        is(s, "999\n");
        free(s);
    }
    /* big numbers, each way */
    is("length(2^100000)\n", "30103\n");
    is("length(10^5000000)\n", "5000001\n");           /* past the size where a power tries the memory first */
    is("(10^5000+1)/(10^4999)\n", "10\n");
    is("x=3^5000;y=7^3000;(x*y)/y==x\n", "1\n");
    is("x=3^5000;y=7^3000;(x*y)%y\n", "0\n");
    is("scale=5000;x=1/3;length(x);scale(x)\n", "5000\n5000\n");
    is("x=sqrt(10^4000);x==10^2000\n", "1\n");
    is("obase=2\n2^10\n", "10000000000\n");
    run("obase=16\n2^4000\n", 0, 0);
    CHECK(out[0] == '1' && nout > 1000);
    /* a constant of ten thousand digits in another base */
    s = repeated("ibase=16\n", 1, "", "F", 10000, "\n");
    run(s, 0, 0);
    CHECK(!errors && nout > 12000);
    free(s);
    /* the limit on steps, which the damaged input leans on */
    CHECK(run_bytes("while(1) { }\n", 13, 0, 0, 100) == BC_TROUBLE && strstr(errtext, "out of steps"));
    CHECK(run_bytes("while(1) { }\nwhile(1) { }\n2\n", 28, 0, 1, 100) == BC_END && !strcmp(out, "2\n"));
}

int main(int argc, char **argv) {
    int rounds = argc > 1 && !strcmp(argv[1], "long") ? 30000 : 3000;
    if(argc > 1 && !strcmp(argv[1], "corpus")) {        /* for a look at a difference: test_bc corpus [library] */
        prng_state = CORPUS_SEED;
        add_programs();
        if(argc > 2) {
            ncorpus = 0;
            add_library();
        }
        fwrite(corpus, 1, ncorpus, stdout);
        return 0;
    }
    numbers();
    arithmetic();
    registers();
    statements();
    library();
    library_edges();
    if(rounds > 3000) library_huge();
    against_bc();
    memory();
    damaged(rounds);
    oversized();
    free(out);
    free(corpus);
    printf("test_bc: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
