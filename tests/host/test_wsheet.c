/* Host tests for the Spreadsheet's model (user/wsheet/: cells.c, number.c, formula.c, eval.c, recalc.c, edit.c,
   files.c), built as they are with the host's C library (11/05/26, the desktop applications, track sheet). They
   check: the names of cells and ranges; numbers read as typed and as CSV writes them, the shortest text that gives a
   double back, General at every width, fixed decimals and percents, ROUND and equality to 15 digits; the formula
   language (precedence, text, errors, names, the messages of formulas that do not parse, the depth and length
   limits, references kept in capitals); conversions and comparisons; every function; recalculation in dependency
   order, cycles (one cell, two, a component reached by a cross edge, through a range) and the work limit; copy and
   paste with relative, absolute and mixed references and #REF!, pasting text; inserting and deleting rows and
   columns with references, ranges and widths moving; the history (every change undone and redone, the modified
   state, the limits); the sheet's own file format and CSV (RFC 4180's examples, line ends, quotes, a byte order
   mark, every limit, a file fed a byte at a time and cut at every byte); every allocation of each change, Undo,
   Redo, Copy and the readers refused in turn, with nothing changed and nothing leaked; and at the end nothing
   allocated. Built and run by `make unittest`.

   `test_wsheet long` adds the long runs: generated and damaged formulas, damaged CSV and sheet files fed in random
   pieces, and random changes each undone and redone, checked against the sheet read again from its own file. They
   run natively on the Mac under the sanitizers, never in `make unittest` (a 32-bit sanitizer binary hangs in the
   devenv image):

   clang -std=gnu99 -g -O1 -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=all -Iuser/wsheet \
       tests/host/test_wsheet.c user/wsheet/{cells,number,formula,eval,recalc,edit,files,view}.c -o /tmp/test_wsheet
   /tmp/test_wsheet long */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sheet.h"
#include "sheetpriv.h"
#include "view.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)
#define CHECK_STR(got, want) check_str(__LINE__, #got, (got), (want))

static void check_str(int line, const char *what, const char *got, const char *want) {
    checks++;
    if(strcmp(got, want)) {
        failed++;
        printf("FAIL line %d: %s is \"%.200s\", not \"%.200s\"\n", line, what, got, want);
    }
}

static struct sheet *S;
static unsigned long long rng = 0x9E3779B97F4A7C15ULL;

/* Marsaglia's xorshift */
static unsigned rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng >> 16);
}

static int below(int n) { return n > 0 ? (int)(rnd() % (unsigned)n) : 0; }

static void fresh(void) {
    sh_free(S);
    S = sh_new();
    if(!S) {
        printf("no sheet\n");
        exit(2);
    }
}

static void cell(const char *name, int *r, int *c) {
    if(sh_parse_cell(name, r, c) < 0) {
        printf("bad cell name in the test: %s\n", name);
        exit(2);
    }
}

static int set(const char *name, const char *text) {
    int r, c;
    cell(name, &r, &c);
    return sh_set(S, r, c, text, (int)strlen(text));
}

static const char *show(const char *name) {
    static char buf[4][SH_TEXT_MAX + 64];
    static int k;
    int r, c, align;
    cell(name, &r, &c);
    k = (k + 1) % 4;
    sh_display(S, r, c, 0, buf[k], sizeof(buf[k]), &align);
    return buf[k];
}

static const char *show_in(const char *name, int chars) {
    static char buf[64];
    int r, c, align;
    cell(name, &r, &c);
    sh_display(S, r, c, chars, buf, sizeof(buf), &align);
    return buf;
}

static const char *inp(const char *name) {
    static char buf[4][SH_INPUT_MAX + 64];
    static int k;
    int r, c;
    cell(name, &r, &c);
    k = (k + 1) % 4;
    sh_input(S, r, c, buf[k], sizeof(buf[k]));
    return buf[k];
}

static double num(const char *name) {
    struct sh_value v;
    int r, c;
    cell(name, &r, &c);
    sh_value(S, r, c, &v);
    return v.type == SH_NUMBER ? v.num : NAN;
}

static int vtype(const char *name) {
    struct sh_value v;
    int r, c;
    cell(name, &r, &c);
    sh_value(S, r, c, &v);
    return v.type;
}

static struct sh_range rg(const char *text) {
    struct sh_range r;
    if(sh_parse_range(text, &r) < 0) {
        printf("bad range in the test: %s\n", text);
        exit(2);
    }
    return r;
}

/* a formula's value as it shows, in Z99 */
static void ev_at(int line, const char *formula, const char *want) {
    const char *got;
    set("Z99", formula);
    got = show("Z99");
    checks++;
    if(strcmp(got, want)) {
        failed++;
        printf("FAIL line %d: %.120s gives \"%.120s\", not \"%s\"\n", line, formula, got, want);
    }
}
#define EV(f, w) ev_at(__LINE__, f, w)

/* writing to memory */
struct mbuf {
    char *s;
    int len, cap, fail_at;
};

static int mput(void *ctx, const char *b, int n) {
    struct mbuf *m = ctx;
    if(m->fail_at >= 0 && m->len + n > m->fail_at) return -1;
    if(m->len + n + 1 > m->cap) {
        m->cap = (m->len + n + 1) * 2;
        m->s = realloc(m->s, (size_t)m->cap);
        if(!m->s) exit(2);
    }
    memcpy(m->s + m->len, b, (size_t)n);
    m->len += n;
    m->s[m->len] = 0;
    return 0;
}

/* the sheet as a file of kind: malloc'd text */
static char *save(const struct sheet *s, int kind) {
    struct mbuf m = { NULL, 0, 0, -1 };
    mput(&m, "", 0);
    if(sh_write(s, kind, mput, &m) < 0) {
        printf("a write failed\n");
        exit(2);
    }
    return m.s;
}

static struct sheet *load(int kind, const char *text, int len, int chunk, char *why) {
    struct sh_reader *rd = sh_read_begin(kind);
    int i, n;
    why[0] = 0;
    if(!rd) {
        strcpy(why, "no reader");
        return NULL;
    }
    for(i = 0; i < len; i += n) {
        n = chunk > 0 ? chunk : len;
        if(n > len - i) n = len - i;
        if(sh_read(rd, text + i, n) < 0) break;
    }
    return sh_read_end(rd, why, SH_WHY);
}

/* the sheet's text once saved, read back and saved again: the same */
static int round_trips(const struct sheet *s) {
    char why[SH_WHY], *a = save(s, SH_NATIVE), *b;
    struct sheet *t = load(SH_NATIVE, a, (int)strlen(a), 0, why);
    int ok;
    if(!t) {
        printf("could not read back: %s\n%.400s\n", why, a);
        free(a);
        return 0;
    }
    b = save(t, SH_NATIVE);
    ok = !strcmp(a, b);
    if(!ok) printf("not the same:\n%.400s\n---\n%.400s\n", a, b);
    sh_free(t);
    free(a);
    free(b);
    return ok;
}

/* its values as CSV, and those of the sheet read again from its own file: the recalculation left nothing stale */
/* the values of the sheet read back from its own file are the same, cell by cell (compared directly: a CSV of a sheet
   whose cells reach IV65536 would be 17 MB of commas, 11/09/26) */
static int values_hold(const struct sheet *s) {
    char why[SH_WHY], *a = save(s, SH_NATIVE);
    struct sheet *t = load(SH_NATIVE, a, (int)strlen(a), 0, why);
    struct sh_value x, y;
    int ok = t != NULL, i, j, r, c;
    for(i = 0; ok && i < s->nrows; i++)
        for(j = 0; ok && j < s->rows[i].n; j++) {
            r = s->rows[i].r;
            c = s->rows[i].cells[j].col;
            sh_value(s, r, c, &x);
            sh_value(t, r, c, &y);
            ok = x.type == y.type && x.b == y.b && (x.type != SH_NUMBER || x.num == y.num) &&
                 (x.type != SH_TEXT || (x.len == y.len && !memcmp(x.text, y.text, (size_t)x.len)));
            if(!ok) printf("values differ at row %d, column %d\n", r + 1, c + 1);
        }
    if(ok && sh_cells(s) != sh_cells(t)) ok = 0;
    sh_free(t);
    free(a);
    return ok;
}

/**** names ****/

static void test_names(void) {
    char b[16];
    struct sh_range g;
    int r, c;
    sh_colname(0, b); CHECK_STR(b, "A");
    sh_colname(25, b); CHECK_STR(b, "Z");
    sh_colname(26, b); CHECK_STR(b, "AA");
    sh_colname(51, b); CHECK_STR(b, "AZ");
    sh_colname(52, b); CHECK_STR(b, "BA");
    sh_colname(255, b); CHECK_STR(b, "IV");
    sh_cellname(0, 0, b); CHECK_STR(b, "A1");
    sh_cellname(65535, 255, b); CHECK_STR(b, "IV65536");
    CHECK(sh_parse_cell("a1", &r, &c) == 0 && r == 0 && c == 0);
    CHECK(sh_parse_cell(" $B$12 ", &r, &c) == 0 && r == 11 && c == 1);
    CHECK(sh_parse_cell("IV65536", &r, &c) == 0 && r == 65535 && c == 255);
    CHECK(sh_parse_cell("iv1", &r, &c) == 0 && c == 255);
    CHECK(sh_parse_cell("IW1", &r, &c) < 0);
    CHECK(sh_parse_cell("A0", &r, &c) < 0);
    CHECK(sh_parse_cell("A65537", &r, &c) < 0);
    CHECK(sh_parse_cell("A01", &r, &c) < 0);
    CHECK(sh_parse_cell("", &r, &c) < 0);
    CHECK(sh_parse_cell("1A", &r, &c) < 0);
    CHECK(sh_parse_cell("AAAA1", &r, &c) < 0);
    CHECK(sh_parse_cell("A1B", &r, &c) < 0);
    CHECK(sh_parse_cell("A1:B2", &r, &c) < 0);
    CHECK(sh_parse_range("B5:A1", &g) == 0 && g.r1 == 0 && g.c1 == 0 && g.r2 == 4 && g.c2 == 1);
    CHECK(sh_parse_range("C3", &g) == 0 && g.r1 == 2 && g.r2 == 2 && g.c1 == 2 && g.c2 == 2);
    CHECK(sh_parse_range("A1:", &g) < 0 && sh_parse_range(":A1", &g) < 0 && sh_parse_range("A1:B2:C3", &g) < 0);
    CHECK_STR(sh_error_name(SH_DIV0), "#DIV/0!");
    CHECK_STR(sh_error_name(SH_VALUE), "#VALUE!");
    CHECK_STR(sh_error_name(SH_REF), "#REF!");
    CHECK_STR(sh_error_name(SH_NAME), "#NAME?");
    CHECK_STR(sh_error_name(SH_NUM), "#NUM!");
    CHECK_STR(sh_error_name(SH_NA), "#N/A");
    CHECK_STR(sh_error_name(SH_NULL), "#NULL!");
    CHECK_STR(sh_error_name(SH_CYCLE), "#CYCLE!");
    CHECK_STR(sh_error_name(SH_SYNTAX), "#ERROR!");
    CHECK_STR(sh_error_name(SH_LIMIT), "#LIMIT!");
}

/**** numbers ****/

static int rd(const char *s, double *v, int *pct) { return num_read(s, (int)strlen(s), v, pct); }
static int rdj(const char *s, double *v) { return num_read_json(s, (int)strlen(s), v); }

static const char *shortest(double x) {
    static char b[64];
    num_shortest(x, b, sizeof(b));
    return b;
}

static const char *general(double x, int chars) {
    static char b[64];
    num_general(x, chars, b, sizeof(b));
    return b;
}

static const char *fixed(double x, int d, int pct) {
    static char b[400];
    num_fixed(x, d, pct, b, sizeof(b));
    return b;
}

static double bits(unsigned long long u) {
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

static void test_numbers(void) {
    char b[64];
    double v, x;
    int pct, i, ok;
    CHECK(rd("12", &v, &pct) && v == 12 && pct == 0);
    CHECK(rd("-3.5", &v, &pct) && v == -3.5);
    CHECK(rd(".5", &v, &pct) && v == 0.5);
    CHECK(rd("5.", &v, &pct) && v == 5);
    CHECK(rd("+7", &v, &pct) && v == 7);
    CHECK(rd("1e3", &v, &pct) && v == 1000);
    CHECK(rd("2.5E-3", &v, &pct) && v == 0.0025);
    CHECK(rd("50%", &v, &pct) && v == 0.5 && pct == 1);
    CHECK(rd("12.5%", &v, &pct) && v == 0.125 && pct == 2);
    CHECK(rd("7%", &v, &pct) && v == 0.07);         /* the double nearest 0.07, not 7/100 rounded twice */
    CHECK(rd("1e1%", &v, &pct) && v == 0.1);
    CHECK(rd("-0", &v, &pct) && v == 0 && !signbit(v));
    CHECK(rd("007", &v, &pct) && v == 7);
    CHECK(!rd("", &v, &pct) && !rd("1e", &v, &pct) && !rd("e1", &v, &pct) && !rd("1.2.3", &v, &pct));
    CHECK(!rd(" 1", &v, &pct) && !rd("1 ", &v, &pct) && !rd("1e999", &v, &pct) && !rd("0x10", &v, &pct));
    CHECK(!rd("--1", &v, &pct) && !rd("1,000", &v, &pct) && !rd(".", &v, &pct) && !rd("%", &v, &pct));
    CHECK(!rd("inf", &v, &pct) && !rd("nan", &v, &pct) && !rd("1%%", &v, &pct) && !rd("+", &v, &pct));
    CHECK(rdj("0", &v) && v == 0);
    CHECK(rdj("-0", &v) && v == 0 && !signbit(v));
    CHECK(rdj("12", &v) && v == 12);
    CHECK(rdj("-1.5e3", &v) && v == -1500);
    CHECK(rdj("1E+2", &v) && v == 100);
    CHECK(!rdj("007", &v) && !rdj(".5", &v) && !rdj("+1", &v) && !rdj("1.", &v) && !rdj("-", &v));
    CHECK(!rdj("1e", &v) && !rdj("1 ", &v) && !rdj("50%", &v) && !rdj("1e400", &v) && !rdj("", &v));

    CHECK_STR(shortest(0.1), "0.1");
    CHECK_STR(shortest(0.1 + 0.2), "0.30000000000000004");
    CHECK_STR(shortest(1e21), "1E+21");
    CHECK_STR(shortest(1e20), "100000000000000000000");
    CHECK_STR(shortest(123.456), "123.456");
    CHECK_STR(shortest(-2.5), "-2.5");
    CHECK_STR(shortest(1e-7), "1E-7");
    CHECK_STR(shortest(1e-6), "0.000001");
    CHECK_STR(shortest(0), "0");
    CHECK_STR(shortest(-0.0), "0");
    CHECK_STR(shortest(1.7976931348623157e308), "1.7976931348623157E+308");
    CHECK_STR(shortest(9007199254740992.0), "9007199254740992");
    CHECK_STR(shortest(1.0 / 3), "0.3333333333333333");
    CHECK(strtod(shortest(5e-324), NULL) == 5e-324);
    for(i = 0, ok = 1; i < 20000; i++) {            /* any finite double reads back from its text */
        x = bits(((unsigned long long)rnd() << 32) ^ rnd() ^ ((unsigned long long)rnd() << 48));
        if(!isfinite(x)) continue;
        if(strtod(shortest(x), NULL) != x && x != 0) ok = 0;
        num_shortest_percent(x, b, sizeof(b));
        if(isfinite(x * 100) && (!rd(b, &v, &pct) || v != x)) ok = 0;
    }
    CHECK(ok);
    num_shortest_percent(0.5, b, sizeof(b)); CHECK_STR(b, "50%");
    num_shortest_percent(0.123, b, sizeof(b)); CHECK_STR(b, "12.3%");
    num_shortest_percent(1, b, sizeof(b)); CHECK_STR(b, "100%");
    num_shortest_percent(0.07, b, sizeof(b)); CHECK_STR(b, "7%");
    num_shortest_percent(0, b, sizeof(b)); CHECK_STR(b, "0%");

    CHECK_STR(general(1.0 / 3, 0), "0.333333333333333");
    CHECK_STR(general(2.0 / 3, 0), "0.666666666666667");
    CHECK_STR(general(0.1 + 0.2, 0), "0.3");
    CHECK_STR(general(1e15, 0), "1E+15");
    CHECK_STR(general(123456789012345.0, 0), "123456789012345");
    CHECK_STR(general(1234567890123456.0, 0), "1.23456789012346E+15");
    CHECK_STR(general(1e-9, 0), "0.000000001");
    CHECK_STR(general(1e-10, 0), "1E-10");
    CHECK_STR(general(-0.5, 0), "-0.5");
    CHECK_STR(general(2.5e-5, 0), "0.000025");
    CHECK_STR(general(1e100, 0), "1E+100");
    CHECK_STR(general(-1e-100, 0), "-1E-100");
    CHECK_STR(general(0, 0), "0");
    CHECK_STR(general(3.14159265358979, 10), "3.14159265");
    CHECK_STR(general(3.14159265358979, 5), "3.142");
    CHECK_STR(general(3.14159265358979, 1), "3");
    CHECK_STR(general(1234567, 7), "1234567");
    CHECK_STR(general(1234567, 5), "1E+06");
    CHECK_STR(general(0.000123456, 8), "0.000123");
    CHECK_STR(general(-0.000123456, 8), "-0.00012");
    CHECK_STR(general(999.96, 5), "1000");
    CHECK_STR(general(12345678901.0, 10), "1.2346E+10");
    CHECK_STR(general(1e100, 3), "###");
    CHECK_STR(general(1e100, 6), "1E+100");
    CHECK_STR(general(1e100, 5), "#####");
    CHECK_STR(general(-5, 1), "#");
    CHECK_STR(general(0.5, 1), "1");                /* 0.5 in one character: rounded, a half away from 0 */
    CHECK_STR(general(-0.5, 2), "-1");
    CHECK_STR(general(0.4, 2), "0");
    CHECK_STR(general(-0.4, 1), "0");
    CHECK_STR(general(1e-5, 5), "1E-05");           /* an exponent before a whole number */
    CHECK_STR(general(-0.5, 1), "#");
    CHECK_STR(general(9.99999999999999e14, 0), "999999999999999");
    for(i = 0, ok = 1; i < 5000; i++) {             /* whatever the width, it fits */
        int w = 1 + below(20);
        x = bits(((unsigned long long)rnd() << 32) ^ rnd() ^ ((unsigned long long)rnd() << 48));
        if(!isfinite(x)) continue;
        if((int)strlen(general(x, w)) > w) ok = 0;
    }
    CHECK(ok);

    CHECK_STR(fixed(2.675, 2, 0), "2.68");
    CHECK_STR(fixed(1.005, 2, 0), "1.01");
    CHECK_STR(fixed(-0.001, 2, 0), "0.00");
    CHECK_STR(fixed(-0.005, 2, 0), "-0.01");
    CHECK_STR(fixed(1234.5, 0, 0), "1235");
    CHECK_STR(fixed(-1.5, 0, 0), "-2");
    CHECK_STR(fixed(0.5, 0, 0), "1");
    CHECK_STR(fixed(0.4, 0, 0), "0");
    CHECK_STR(fixed(0, 2, 0), "0.00");
    CHECK_STR(fixed(1e20, 2, 0), "100000000000000000000.00");
    CHECK_STR(fixed(123.456, 10, 0), "123.4560000000");
    CHECK_STR(fixed(0.07, 0, 1), "7%");
    CHECK_STR(fixed(0.123456, 1, 1), "12.3%");
    CHECK_STR(fixed(1, 0, 1), "100%");
    CHECK_STR(fixed(-0.25, 2, 1), "-25.00%");
    CHECK_STR(fixed(0.0000001, 2, 1), "0.00%");

    CHECK(num_round(2.675, 2) == 2.68);
    CHECK(num_round(2.5, 0) == 3 && num_round(-2.5, 0) == -3 && num_round(0.5, 0) == 1);
    CHECK(num_round(1234.5678, -2) == 1200);
    CHECK(num_round(0.1 + 0.2, 15) == 0.3);
    CHECK(num_round(1.23456789, 20) == 1.23456789);
    CHECK(num_round(5, -1) == 10 && num_round(4.9, -1) == 0);
    CHECK(num_round(1e300, 2) == 1e300);
    CHECK(num_round(-0.4, 0) == 0 && !signbit(num_round(-0.4, 0)));
    CHECK(num_round(123.456, -400) == 0);
    CHECK(num_equal(0.1 + 0.2, 0.3) && num_equal(1, 1 + 1e-15) && !num_equal(1, 1 + 1e-14));
    CHECK(!num_equal(0, 1e-300) && !num_equal(-1, 1) && num_equal(3, 3) && num_equal(-0.0, 0));
}

/**** the formula language ****/

static void problem_is(int line, const char *formula, const char *want) {
    char why[SH_WHY];
    int r, c;
    set("Z99", formula);
    cell("Z99", &r, &c);
    checks++;
    if(!sh_problem(S, r, c, why, sizeof(why)) || strncmp(why, want, strlen(want))) {
        failed++;
        printf("FAIL line %d: %.80s says \"%s\", not \"%s\"\n", line, formula, why, want);
    }
}
#define PROBLEM(f, w) problem_is(__LINE__, f, w)

static void test_language(void) {
    static char f[SH_INPUT_MAX + 16];
    char why[SH_WHY];
    int i, n, r, c;
    fresh();
    EV("=1+2*3", "7");
    EV("=(1+2)*3", "9");
    EV("=-2^2", "4");                               /* the sign binds tighter than ^ */
    EV("=-3^2", "9");
    EV("=2-3^2", "-7");
    EV("=2^3^2", "64");                             /* from the left */
    EV("=2^-1", "0.5");
    EV("=-(2^2)", "-4");
    EV("=2*3^2", "18");
    EV("=10-2-3", "5");
    EV("=100/10/2", "5");
    EV("=50%", "0.5");
    EV("=3%", "0.03");
    EV("=2%%", "0.0002");
    EV("=200%*3", "6");
    EV("=-50%", "-0.5");
    EV("=1+2&\"x\"", "3x");
    EV("=\"a\"&1+1", "a2");
    EV("=1<2", "TRUE");
    EV("=1+1=2", "TRUE");
    EV("=1<2<3", "FALSE");                          /* TRUE < 3 is FALSE: numbers come before truth values */
    EV("=2*-3", "-6");
    EV("=--3", "3");
    EV("=+3", "3");
    EV("=+\"abc\"", "abc");
    EV("=\"say \"\"hi\"\"\"", "say \"hi\"");
    EV("=\"\"", "");
    EV("=#N/A", "#N/A");
    EV("=#div/0!+1", "#DIV/0!");
    EV("=1/0", "#DIV/0!");
    EV("=(1/0)+#N/A", "#DIV/0!");                   /* the left one first */
    EV("=#N/A+(1/0)", "#N/A");
    EV("=FOO", "#NAME?");
    EV("=FOO(1,2)", "#NAME?");
    EV("=foo()", "#NAME?");
    EV("=TRUE", "TRUE");
    EV("=false", "FALSE");
    EV("=SUM(1;2)", "3");
    EV("= SUM ( 1 , 2 )", "3");
    CHECK_STR(inp("Z99"), "= SUM ( 1 , 2 )");
    EV("=1.5E1+.5", "15.5");
    set("Z99", "=sum(a1:b2,$c$3)+iv65536");
    CHECK_STR(inp("Z99"), "=SUM(A1:B2,$C$3)+IV65536");
    set("Z99", "=SUM(B5:A1)");
    CHECK_STR(inp("Z99"), "=SUM(A1:B5)");
    set("Z99", "=SUM($B5:A$1)");
    CHECK_STR(inp("Z99"), "=SUM(A$1:$B5)");
    set("Z99", "=SUM(A1:$A$1)");                    /* corners alike: the absolute one first */
    CHECK_STR(inp("Z99"), "=SUM($A$1:A1)");
    set("Z99", "=\"a1\"&a1");
    CHECK_STR(inp("Z99"), "=\"a1\"&A1");            /* text keeps its case */
    set("Z99", "=#n/a");
    CHECK_STR(inp("Z99"), "=#N/A");

    PROBLEM("=1+", "the formula ends too soon");
    PROBLEM("=(1", "a ( has no )");
    PROBLEM("=1)", "a ) has no (");
    PROBLEM("=SUM(1,)", "something is missing before )");
    PROBLEM("=SUM(,1)", "an argument is missing");
    PROBLEM("=", "the formula is empty");
    PROBLEM("=  ", "the formula is empty");
    PROBLEM("=1 2", "an operator is missing");
    PROBLEM("=SUM(1)2", "an operator is missing");
    PROBLEM("=\"abc", "a text has no closing \"");
    PROBLEM("=A1:", "a range needs a cell after its :");
    PROBLEM("=A1:IW2", "a range needs a cell after its :");
    PROBLEM("=$", "$ belongs in a reference");
    PROBLEM("=$IW$1", "$ belongs in a reference");
    PROBLEM("=IF(1)", "IF takes at least 2 arguments");
    PROBLEM("=ABS(1,2)", "ABS takes 1 argument");
    PROBLEM("=IF(1,2,3,4)", "IF takes at most 3 arguments");
    PROBLEM("=SUM()", "SUM takes at least 1 argument");
    PROBLEM("=1,2", "a , outside a function's ( )");
    PROBLEM("=(1,2)", "a , outside a function's ( )");
    PROBLEM("=*2", "an operand is missing");
    PROBLEM("=%2", "% needs a number before it");
    PROBLEM("=()", "something is missing before )");
    PROBLEM("=1e999", "a number is too large");
    PROBLEM("=#FOO", "# begins no error that a sheet knows");
    PROBLEM("=@", "a character a formula does not use");
    PROBLEM("=1+(2", "a ( has no )");
    PROBLEM("=FOO+1", "FOO is no name of this sheet");
    PROBLEM("=BAR(1)", "BAR is no function of this sheet");
    PROBLEM("=A1 B1", "an operator is missing");
    PROBLEM("=2A5", "a number runs into a name");   /* a move must never make it 2E6 */
    PROBLEM("=1.5.3", "a number runs into a name");
    PROBLEM("=1e+", "a number runs into a name");
    CHECK(vtype("Z99") == SH_ERROR && !strcmp(show("Z99"), "#ERROR!"));
    set("Z99", "=1+");
    CHECK_STR(show("Z99"), "#ERROR!");
    CHECK_STR(inp("Z99"), "=1+");                   /* kept as typed */
    set("Z99", "=\"unclosed");
    CHECK_STR(inp("Z99"), "=\"unclosed");
    cell("Z99", &r, &c);
    set("Z99", "=1+1");
    CHECK(!sh_problem(S, r, c, why, sizeof(why)));

    /* depth: 64 parentheses or calls inside one another, not 65 */
    for(n = 64; n <= 65; n++) {
        strcpy(f, "=");
        for(i = 0; i < n; i++) strcat(f, "(");
        strcat(f, "1");
        for(i = 0; i < n; i++) strcat(f, ")");
        set("Z99", f);
        CHECK_STR(show("Z99"), n == 64 ? "1" : "#ERROR!");
        strcpy(f, "=");
        for(i = 0; i < n; i++) strcat(f, "ABS(");
        strcat(f, "-1");
        for(i = 0; i < n; i++) strcat(f, ")");
        set("Z99", f);
        CHECK_STR(show("Z99"), n == 64 ? "1" : "#ERROR!");
    }
    PROBLEM(f, "parentheses and calls go too deep");
    /* 255 arguments, not 256 */
    for(n = 255; n <= 256; n++) {
        strcpy(f, "=SUM(1");
        for(i = 1; i < n; i++) strcat(f, ",1");
        strcat(f, ")");
        set("Z99", f);
        CHECK_STR(show("Z99"), n == 255 ? "255" : "#ERROR!");
    }
    /* a cell holds SH_INPUT_MAX bytes */
    f[0] = '=';
    for(i = 1; i < SH_INPUT_MAX; i += 2) {
        f[i] = '1';
        f[i + 1] = '+';
    }
    f[SH_INPUT_MAX - 1] = '1';
    cell("Z99", &r, &c);
    CHECK(sh_set(S, r, c, f, SH_INPUT_MAX) == 0);
    CHECK_STR(show("Z99"), "4096");
    CHECK(sh_set(S, r, c, f, SH_INPUT_MAX + 1) < 0 && strstr(sh_why(S), "8192"));
    CHECK(sh_set(S, r, c, "a\0b", 3) < 0 && strstr(sh_why(S), "NUL"));
    /* a long chain of operators, deeper than any C stack would like if it recursed */
    f[0] = '=';
    for(i = 1; i < 8000; i++) f[i] = '-';
    f[8000] = '1';
    CHECK(sh_set(S, r, c, f, 8001) == 0);
    CHECK_STR(show("Z99"), "-1");                   /* 7999 minus signs */
}

/**** values and conversions ****/

static void test_values(void) {
    fresh();
    EV("=A1", "0");                                 /* an empty cell shows 0 */
    EV("=A1&\"\"", "");
    EV("=A1=0", "TRUE");
    EV("=A1=\"\"", "TRUE");
    EV("=A1=FALSE", "TRUE");
    EV("=0=\"\"", "FALSE");
    set("A1", "'5");
    CHECK(vtype("A1") == SH_TEXT && !strcmp(show("A1"), "5"));
    EV("=A1+1", "6");
    set("A1", " 12 ");
    EV("=A1*2", "24");
    set("A1", "abc");
    EV("=A1+1", "#VALUE!");
    EV("=\"50%\"*2", "1");
    EV("=\"\"+1", "#VALUE!");
    EV("=TRUE+1", "2");
    EV("=TRUE&\"\"", "TRUE");
    EV("=IF(\"TRUE\",1,2)", "1");
    EV("=IF(\"x\",1,2)", "#VALUE!");
    EV("=1/3&\"\"", "0.333333333333333");
    EV("=1E20&\"\"", "1E+20");
    EV("=1<\"a\"", "TRUE");
    EV("=\"a\"<TRUE", "TRUE");
    EV("=FALSE<TRUE", "TRUE");
    EV("=\"ABC\"=\"abc\"", "TRUE");
    EV("=\"a\"<\"B\"", "TRUE");
    EV("=\"b\"<\"A\"", "FALSE");
    EV("=\"\xc3\xa9t\xc3\xa9\"=\"\xc3\x89T\xc3\x89\"", "TRUE");    /* été, ÉTÉ */
    EV("=\"ab\"<\"abc\"", "TRUE");
    EV("=0.1+0.2=0.3", "TRUE");
    EV("=0.1+0.2-0.3", "5.55111512312578E-17");
    EV("=1E308*10", "#NUM!");
    EV("=-1E308*10", "#NUM!");
    EV("=SQRT(-1)", "#NUM!");
    EV("=(-8)^(1/3)", "#NUM!");
    EV("=(-8)^3", "-512");
    EV("=0^-1", "#DIV/0!");
    EV("=0^0", "1");
    EV("=-0", "0");
    EV("=0*-1", "0");
    set("A1", "1");
    set("A2", "2");
    EV("=A1:A2", "#VALUE!");
    EV("=A1:A1", "1");
    EV("=A1:A2+1", "#VALUE!");
    EV("=#N/A=1", "#N/A");
    EV("=1=#N/A", "#N/A");
    /* as a cell of some width shows them, and where they sit */
    set("B1", "1234.5678");
    sh_set_format(S, rg("B1"), SH_FIXED(2));
    CHECK_STR(show_in("B1", 7), "1234.57");
    CHECK_STR(show_in("B1", 6), "######");
    set("B2", "=1/3");
    CHECK_STR(show_in("B2", 6), "0.3333");
    CHECK_STR(show_in("B2", 0), "0.333333333333333");
    set("B3", "a long text");
    CHECK_STR(show_in("B3", 3), "a long text");     /* text is the window's to cut */
    {
        static const struct { const char *cell; int align; } where[] = {
            { "B1", SH_RIGHT }, { "B3", SH_LEFT }, { "A1", SH_RIGHT }, { "C9", SH_LEFT },
        };
        char buf[32];
        int i, r, c, align;
        set("A2", "=1=1");
        set("A3", "=1/0");
        for(i = 0; i < 4; i++) {
            cell(where[i].cell, &r, &c);
            sh_display(S, r, c, 10, buf, sizeof(buf), &align);
            CHECK(align == where[i].align);
        }
        cell("A2", &r, &c);
        sh_display(S, r, c, 10, buf, sizeof(buf), &align);
        CHECK(align == SH_CENTER);
        cell("A3", &r, &c);
        sh_display(S, r, c, 10, buf, sizeof(buf), &align);
        CHECK(align == SH_CENTER);
    }
}

/**** the functions ****/

static void test_functions(void) {
    static char f[SH_TEXT_MAX + 64];
    int i;
    fresh();
    set("A1", "1");
    set("A2", "2");
    set("A3", "3");
    set("A4", "text");
    set("A5", "TRUE");
    set("B1", "'7");
    EV("=SUM(A1:A5)", "6");                         /* text and truth values in a range are left out */
    EV("=SUM(1,\"2\",TRUE)", "4");                  /* in the call they count */
    EV("=SUM(\"x\")", "#VALUE!");
    EV("=SUM(B1)", "0");                            /* a reference is a range: its text is left out */
    EV("=SUM(A1:A3,10)", "16");
    EV("=SUM(A1,A1:A2)", "4");
    EV("=AVERAGE(1,2,3)", "2");
    EV("=AVERAGE(A1:A5)", "2");
    EV("=AVERAGE(C1:C9)", "#DIV/0!");
    EV("=AVERAGE(TRUE,3)", "2");
    EV("=MIN(3,1,2)", "1");
    EV("=MAX(3,1,2)", "3");
    EV("=MIN(C1:C9)", "0");
    EV("=MAX(A4:A5)", "0");
    EV("=MAX(-5,-2)", "-2");
    EV("=COUNT(A1:A5)", "3");
    EV("=COUNT(1,\"2\",\"x\",TRUE)", "3");
    EV("=COUNTA(A1:A5)", "5");
    EV("=COUNTA(1,\"\",#N/A)", "3");
    EV("=COUNTA(C1:C9)", "0");
    EV("=COUNT(#N/A,1)", "1");
    set("C1", "=\"\"");
    EV("=COUNTA(C1:C9)", "1");                      /* a formula's "" counts */
    set("C2", "=1/0");
    EV("=SUM(C1:C9)", "#DIV/0!");
    EV("=COUNTA(C1:C9)", "2");
    EV("=COUNT(C1:C9)", "0");
    for(i = 1; i <= 10; i++) {
        char name[8];
        snprintf(name, sizeof(name), "D%d", i);
        set(name, "0.1");
    }
    EV("=SUM(D1:D10)", "1");
    EV("=SUM(D1:D10)=1", "TRUE");
    set("Z99", "=SUM(D1:D10)");
    CHECK(num("Z99") == 1.0);                       /* Neumaier: exactly 1, where adding gives 0.9999999999999999 */
    EV("=SUM(1E100,1,-1E100)", "1");
    EV("=IF(1,\"y\",\"n\")", "y");
    EV("=IF(0,\"y\",\"n\")", "n");
    EV("=IF(0,\"y\")", "FALSE");
    EV("=IF(1/0,1,2)", "#DIV/0!");
    EV("=IF(1,C9)", "0");
    EV("=IF(TRUE,1,1/0)", "1");
    EV("=SUM(IF(1,A1:A3,0))", "6");
    EV("=AND(TRUE,1)", "TRUE");
    EV("=AND(TRUE,0)", "FALSE");
    EV("=OR(0,0)", "FALSE");
    EV("=OR(0,1)", "TRUE");
    EV("=AND(A4:A4)", "#VALUE!");                   /* only text in it: no truth at all */
    EV("=AND(A1:A5)", "TRUE");
    EV("=AND(\"x\")", "#VALUE!");
    EV("=OR(\"TRUE\")", "TRUE");
    EV("=AND(1/0)", "#DIV/0!");
    EV("=NOT(0)", "TRUE");
    EV("=NOT(C9)", "TRUE");
    EV("=NOT(\"x\")", "#VALUE!");
    EV("=ROUND(2.675,2)", "2.68");
    EV("=ROUND(-2.5,0)", "-3");
    EV("=ROUND(1234,-2)", "1200");
    EV("=ROUND(1.5)", "2");
    EV("=ROUND(1.25,1.9)", "1.3");                  /* places are whole: 1.9 is 1 */
    EV("=INT(-1.5)", "-2");
    EV("=INT(2.9)", "2");
    EV("=ABS(-3)", "3");
    EV("=SQRT(16)", "4");
    EV("=SQRT(2)", "1.4142135623731");
    EV("=POWER(2,10)", "1024");
    EV("=POWER(2,0.5)", "1.4142135623731");
    EV("=MOD(-1,3)", "2");
    EV("=MOD(1,-3)", "-2");
    EV("=MOD(5.5,2)", "1.5");
    EV("=MOD(6,3)", "0");
    EV("=MOD(1,0)", "#DIV/0!");
    EV("=LEN(\"h\xc3\xa9llo\")", "5");
    EV("=LEN(123.5)", "5");
    EV("=LEN(TRUE)", "4");
    EV("=LEN(\"\")", "0");
    EV("=LEN(\"\xff\xfe\")", "2");                  /* bytes that are no UTF-8: one character each */
    EV("=LEFT(\"abc\")", "a");
    EV("=LEFT(\"abc\",5)", "abc");
    EV("=LEFT(\"abc\",0)", "");
    EV("=LEFT(\"abc\",-1)", "#VALUE!");
    EV("=RIGHT(\"h\xc3\xa9llo\",4)", "\xc3\xa9llo");
    EV("=RIGHT(\"\xc3\xa9\",1)", "\xc3\xa9");
    EV("=RIGHT(\"abc\",9)", "abc");
    EV("=MID(\"abcdef\",2,3)", "bcd");
    EV("=MID(\"abc\",5,1)", "");
    EV("=MID(\"abc\",0,1)", "#VALUE!");
    EV("=MID(\"abc\",2,-1)", "#VALUE!");
    EV("=MID(\"h\xc3\xa9llo\",2,2)", "\xc3\xa9l");
    EV("=UPPER(\"a\xc3\xa9z\")", "A\xc3\x89Z");
    EV("=LOWER(\"\xc3\x80" "B\")", "\xc3\xa0" "b");
    EV("=UPPER(\"\xc3\xbf\")", "\xc5\xb8");         /* ÿ, Ÿ */
    EV("=LOWER(\"\xc5\xb8\")", "\xc3\xbf");
    EV("=UPPER(\"\xc3\x9f\xc3\xb7\")", "\xc3\x9f\xc3\xb7");    /* ß and ÷ stay */
    EV("=TRIM(\"  a   b  \")", "a b");
    EV("=TRIM(\"   \")", "");
    EV("=CONCAT(\"a\",1,TRUE)", "a1TRUE");
    set("E1", "x");
    set("F1", "y");
    set("E2", "1");
    EV("=CONCAT(E1:F2)", "xy1");
    EV("=CONCAT(E1:F2,1/0)", "#DIV/0!");
    EV("=UPPER(1/0)", "#DIV/0!");
    EV("=NOPE(1,2,3)", "#NAME?");
    memset(f, 'x', 8000);
    f[8000] = 0;
    set("G1", f);
    EV("=LEN(G1&G1)", "#VALUE!");                   /* 16,000 bytes: longer than a formula's text may be */
    EV("=LEN(G1&\"y\")", "8001");
    EV("=LEN(CONCAT(G1,G1))", "#VALUE!");
    EV("=LEN(CONCAT(G1,\"abc\"))", "8003");
}

/**** recalculation ****/

static void test_recalc(void) {
    static char text[65536 * 32 + 64];
    char name[16], formula[64], why[SH_WHY];
    struct sheet *t;
    int i, n;
    fresh();
    set("A1", "1");
    for(i = 2; i <= 100; i++) {
        snprintf(name, sizeof(name), "A%d", i);
        snprintf(formula, sizeof(formula), "=A%d+1", i - 1);
        set(name, formula);
    }
    CHECK(num("A100") == 100);
    set("A1", "11");
    CHECK(num("A100") == 110);
    /* made in the reverse order: still worked out after what each uses */
    fresh();
    for(i = 50; i >= 2; i--) {
        snprintf(name, sizeof(name), "B%d", i);
        snprintf(formula, sizeof(formula), "=B%d*2", i - 1);
        set(name, formula);
    }
    set("B1", "1");
    CHECK(num("B50") == 562949953421312.0);
    set("C1", "=SUM(D1:D3)");
    set("D2", "=E1*2");
    set("E1", "5");
    CHECK(num("C1") == 10);
    /* cycles */
    fresh();
    set("A1", "=A1");
    CHECK_STR(show("A1"), "#CYCLE!");
    set("A1", "=B1+1");
    set("B1", "=A1+1");
    CHECK_STR(show("A1"), "#CYCLE!");
    CHECK_STR(show("B1"), "#CYCLE!");
    set("C1", "=A1+1");
    CHECK_STR(show("C1"), "#CYCLE!");               /* passed on, not in the cycle */
    set("D1", "=COUNTA(A1)");
    CHECK_STR(show("D1"), "1");                     /* uses it without being part of it */
    set("B1", "5");
    CHECK_STR(show("A1"), "6");
    CHECK_STR(show("C1"), "7");
    fresh();                                        /* A -> B -> C -> A, and A -> D -> B: all four are one */
    set("A1", "=B1+D1");
    set("B1", "=C1");
    set("C1", "=A1");
    set("D1", "=COUNTA(B1)");
    CHECK_STR(show("A1"), "#CYCLE!");
    CHECK_STR(show("B1"), "#CYCLE!");
    CHECK_STR(show("C1"), "#CYCLE!");
    CHECK_STR(show("D1"), "#CYCLE!");
    fresh();
    set("A1", "1");
    set("A3", "=SUM(A1:A5)");                       /* inside its own range */
    CHECK_STR(show("A3"), "#CYCLE!");
    set("A3", "=SUM(A1:A2)");
    CHECK_STR(show("A3"), "1");
    set("B1", "=IF(TRUE,1,B1)");                    /* a reference not taken is still one: a cycle */
    CHECK_STR(show("B1"), "#CYCLE!");
    /* 65,536 formulas, each using the one above: no recursion anywhere */
    n = snprintf(text, sizeof(text), "SurfOS sheet 1\ncell A1 general 1\n");
    for(i = 2; i <= SH_ROWS; i++)
        n += snprintf(text + n, sizeof(text) - (size_t)n, "cell A%d general =A%d+1\n", i, i - 1);
    t = load(SH_NATIVE, text, n, 0, why);
    CHECK(t != NULL);
    if(t) {
        struct sh_value v;
        sh_value(t, SH_ROWS - 1, 0, &v);
        CHECK(v.type == SH_NUMBER && v.num == SH_ROWS);
        CHECK(!sh_limited(t));
        CHECK(sh_set(t, 0, 0, "=A65536", 7) == 0); /* and all of it one cycle */
        sh_value(t, 30000, 0, &v);
        CHECK(v.type == SH_ERROR && v.b == SH_CYCLE);
        sh_free(t);
    }
    /* the work limit */
    fresh();
    for(i = 1; i <= 200; i++) {
        snprintf(name, sizeof(name), "A%d", i);
        set(name, "1");
    }
    for(i = 1; i <= 50; i++) {
        snprintf(name, sizeof(name), "B%d", i);
        set(name, "=SUM(A1:A200)");
    }
    CHECK(!sh_limited(S) && num("B50") == 200);
    sh_steps_max = 3000;
    set("C1", "1");
    CHECK(sh_limited(S));
    CHECK_STR(show("B50"), "#LIMIT!");
    sh_steps_max = SH_STEPS;
    set("C1", "2");
    CHECK(!sh_limited(S) && num("B50") == 200);
}

/**** copy and paste ****/

static struct sh_clip *copy(const char *range) {
    struct sh_clip *c = sh_copy(S, rg(range));
    if(!c) {
        printf("copy failed: %s\n", sh_why(S));
        exit(2);
    }
    return c;
}

static int paste(struct sh_clip *c, const char *at) {
    int r, col;
    cell(at, &r, &col);
    return sh_paste(S, c, r, col, NULL);
}

static int paste_text(const char *text, const char *at) {
    int r, c;
    cell(at, &r, &c);
    return sh_paste_text(S, text, (int)strlen(text), r, c, NULL);
}

static void test_paste(void) {
    struct sh_clip *c;
    char *before;
    const char *t;
    int len;
    fresh();
    set("A1", "1");
    set("A2", "2");
    set("A3", "3");
    set("B1", "=A1*2");
    c = copy("B1");
    CHECK(paste(c, "B2") == 0);
    CHECK_STR(inp("B2"), "=A2*2");
    CHECK(num("B2") == 4);
    CHECK(paste(c, "D3") == 0);
    CHECK_STR(inp("D3"), "=C3*2");
    sh_clip_free(c);
    set("B1", "=$A$1+A1+A$1+$A1");
    c = copy("B1");
    CHECK(paste(c, "C3") == 0);
    CHECK_STR(inp("C3"), "=$A$1+B3+B$1+$A3");
    sh_clip_free(c);
    set("B5", "=A4");
    c = copy("B5");
    CHECK(paste(c, "A2") == 0);                     /* up 3, left 1: A4 would be @1, off the sheet */
    CHECK_STR(inp("A2"), "=#REF!");
    CHECK_STR(show("A2"), "#REF!");
    sh_clip_free(c);
    fresh();
    set("A1", "1");
    set("A2", "2");
    set("B2", "=SUM(A1:A2)");
    c = copy("B2");
    CHECK(paste(c, "B3") == 0);
    CHECK_STR(inp("B3"), "=SUM(A2:A3)");
    CHECK(paste(c, "B1") == 0);                     /* A0 is no cell: the whole range goes */
    CHECK_STR(inp("B1"), "=SUM(#REF!)");
    sh_clip_free(c);
    set("C1", "=SUM(A$5:A6)");
    c = copy("C1");
    CHECK(paste(c, "C10") == 0);                    /* a corner that is absolute stays: the range turns over */
    CHECK_STR(inp("C10"), "=SUM(A$5:A15)");
    sh_clip_free(c);
    /* what the copy has nothing for is emptied, formats too; formats come along */
    fresh();
    set("A1", "1");
    sh_set_format(S, rg("A1"), SH_FIXED(2));
    set("C1", "x");
    set("C2", "y");
    sh_set_format(S, rg("C2"), SH_PERCENT(0));
    c = copy("A1:A2");
    CHECK(paste(c, "C1") == 0);
    CHECK_STR(show("C1"), "1.00");
    CHECK(vtype("C2") == SH_EMPTY && sh_format(S, 1, 2) == SH_GENERAL);
    sh_clip_free(c);
    /* past the edge: refused, nothing changed */
    c = copy("A1:A2");
    before = save(S, SH_NATIVE);
    CHECK(paste(c, "A65536") < 0 && strstr(sh_why(S), "edge"));
    t = save(S, SH_NATIVE);
    CHECK(!strcmp(before, t));
    free((void *)t);
    free(before);
    sh_clip_free(c);
    /* the text for the desktop's clipboard: the values as they show */
    fresh();
    set("A1", "1");
    set("B1", "50%");
    set("C1", "3.14159");
    sh_set_format(S, rg("C1"), SH_FIXED(2));
    set("A2", "tab\there");
    set("B2", "=1/3");
    set("C2", "say \"hi\"");
    set("A3", "two\nlines");
    c = copy("A1:C3");
    t = sh_clip_text(c, &len);
    CHECK_STR(t, "1\t50%\t3.14\n\"tab\there\"\t0.333333333333333\t\"say \"\"hi\"\"\"\n\"two\nlines\"\t\t");
    CHECK(len == (int)strlen(t));
    sh_clip_free(c);
    c = sh_copy(S, rg("A1:Q65536"));                /* 17 whole columns: more than a copy takes */
    CHECK(c == NULL && strstr(sh_why(S), "Copy"));
    c = sh_copy(S, rg("A1:P65536"));                /* 16 do, but their text is too long for the desktop */
    CHECK(c != NULL && sh_clip_text(c, &len) == NULL);
    sh_clip_free(c);
    /* text pasted from elsewhere: as typed, a field a cell, into the cells' formats */
    fresh();
    sh_set_format(S, rg("C5"), SH_PERCENT(0));
    CHECK(paste_text("0.5\t2\n3\t=A1+B1\n", "C5") == 0);
    CHECK_STR(show("C5"), "50%");
    CHECK(num("D5") == 2 && num("C6") == 3);
    CHECK_STR(inp("D6"), "=A1+B1");                 /* as typed: not moved */
    CHECK(vtype("C7") == SH_EMPTY);                 /* the last line end makes no row */
    CHECK(paste_text("\"a\tb\"\t\"x\"\"y\"\r\n\"multi\nline\"\t", "A1") == 0);
    CHECK_STR(show("A1"), "a\tb");
    CHECK_STR(show("B1"), "x\"y");
    CHECK_STR(show("A2"), "multi\nline");
    CHECK(paste_text("\t\n", "C5") == 0);           /* empty fields empty the cells, and keep their formats */
    CHECK(vtype("C5") == SH_EMPTY && vtype("D5") == SH_EMPTY && sh_format(S, 4, 2) == SH_PERCENT(0));
    CHECK(paste_text("1\t2", "IV1") < 0 && strstr(sh_why(S), "edge"));
    CHECK(paste_text("a\0b", "A1") == 0);           /* strlen: "a" alone */
    CHECK(sh_paste_text(S, "a\0b", 3, 0, 0, NULL) < 0 && strstr(sh_why(S), "NUL"));
    CHECK(paste_text("", "A1") == 0);
}

/**** inserting and deleting rows and columns ****/

static void test_structure(void) {
    char *before, *after;
    fresh();
    set("A1", "1");
    set("A2", "2");
    set("A3", "3");
    set("A4", "=SUM(A1:A3)");
    set("B1", "=A2");
    set("B2", "=$A$3*2");
    before = save(S, SH_NATIVE);
    CHECK(sh_insert_rows(S, 1, 1) == 0);            /* a row before row 2 */
    CHECK_STR(inp("A5"), "=SUM(A1:A4)");
    CHECK(num("A5") == 6 && num("A3") == 2);
    CHECK_STR(inp("B1"), "=A3");
    CHECK_STR(inp("B3"), "=$A$4*2");
    CHECK(sh_delete_rows(S, 1, 1) == 0);
    after = save(S, SH_NATIVE);
    CHECK(!strcmp(before, after));                  /* back as it was */
    free(after);
    CHECK(sh_delete_rows(S, 1, 1) == 0);            /* row 2, which B1 refers to */
    CHECK_STR(inp("B1"), "=#REF!");
    CHECK_STR(inp("A3"), "=SUM(A1:A2)");
    CHECK(num("A3") == 4);
    CHECK(sh_undo(S, NULL) == 0);
    after = save(S, SH_NATIVE);
    CHECK(!strcmp(before, after));
    free(after);
    free(before);
    fresh();
    set("B1", "=SUM(A1:A5)");
    CHECK(sh_delete_rows(S, 1, 2) == 0);            /* rows 2 and 3: the range shrinks */
    CHECK_STR(inp("B1"), "=SUM(A1:A3)");
    CHECK(sh_delete_rows(S, 0, 3) == 0);            /* all of it, and the formula: nothing left */
    CHECK(vtype("B1") == SH_EMPTY);
    fresh();
    set("B1", "=SUM(A3:A6)");
    CHECK(sh_delete_rows(S, 1, 3) == 0);            /* rows 2 to 4: the range's start went */
    CHECK_STR(inp("B1"), "=SUM(A2:A3)");
    fresh();
    set("B1", "=SUM(A1:A3)+SUM(A2:A3)+A$5");
    CHECK(sh_insert_rows(S, 1, 2) == 0);            /* in the first range it grows; at the second's start it moves */
    CHECK_STR(inp("B1"), "=SUM(A1:A5)+SUM(A4:A5)+A$7");
    fresh();
    set("B1", "=SUM(A2:A65536)+A65536");
    CHECK(sh_insert_rows(S, 0, 1) == 0);            /* a range's end stays at the sheet's; a cell pushed off is #REF! */
    CHECK_STR(inp("B2"), "=SUM(A3:A65536)+#REF!");
    fresh();
    set("B1", "=SUM(A3:A4)");
    CHECK(sh_delete_rows(S, 2, 2) == 0);
    CHECK_STR(inp("B1"), "=SUM(#REF!)");
    /* cells at the end are not pushed off */
    fresh();
    set("A65536", "end");
    before = save(S, SH_NATIVE);
    CHECK(sh_insert_rows(S, 0, 1) < 0 && strstr(sh_why(S), "push"));
    after = save(S, SH_NATIVE);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    CHECK(sh_insert_rows(S, 65535, 2) < 0);
    CHECK(sh_delete_rows(S, 65535, 1) == 0 && vtype("A65536") == SH_EMPTY);
    CHECK(sh_insert_rows(S, 0, 0) < 0 && sh_delete_rows(S, -1, 1) < 0);
    /* columns, with their widths */
    fresh();
    set("A1", "1");
    set("B1", "2");
    set("C1", "=A1+B1");
    set("D1", "=SUM(A1:C1)");
    sh_set_width(S, 1, 1, 120);
    sh_set_width(S, 255, 255, 40);
    before = save(S, SH_NATIVE);
    CHECK(sh_insert_cols(S, 1, 2) == 0);            /* two columns before B */
    CHECK_STR(inp("E1"), "=A1+D1");
    CHECK_STR(inp("F1"), "=SUM(A1:E1)");
    CHECK(num("F1") == 6);
    CHECK(sh_width(S, 3) == 120 && sh_width(S, 1) == SH_WIDTH_DEFAULT && sh_width(S, 255) == SH_WIDTH_DEFAULT);
    CHECK(sh_undo(S, NULL) == 0);
    after = save(S, SH_NATIVE);
    CHECK(!strcmp(before, after) && sh_width(S, 255) == 40);
    free(after);
    CHECK(sh_delete_cols(S, 1, 1) == 0);            /* column B, which C1 refers to */
    CHECK_STR(inp("B1"), "=A1+#REF!");
    CHECK_STR(inp("C1"), "=SUM(A1:B1)");
    CHECK(sh_width(S, 1) == SH_WIDTH_DEFAULT);
    CHECK(sh_undo(S, NULL) == 0);
    after = save(S, SH_NATIVE);
    CHECK(!strcmp(before, after) && sh_width(S, 1) == 120);
    free(after);
    free(before);
    set("IV2", "edge");
    CHECK(sh_insert_cols(S, 0, 1) < 0);
    CHECK(sh_delete_cols(S, 255, 1) == 0 && sh_insert_cols(S, 0, 1) == 0);
    CHECK_STR(inp("B1"), "1");
}

/**** the history ****/

static void test_history(void) {
    struct sh_range w;
    char *a, *b;
    int i;
    fresh();
    CHECK(!sh_modified(S) && !sh_undo_label(S) && sh_undo(S, NULL) < 0 && sh_redo(S, NULL) < 0);
    set("A1", "1");
    CHECK(sh_modified(S));
    CHECK_STR(sh_undo_label(S), "typing");
    CHECK(sh_undo(S, &w) == 0 && w.r1 == 0 && w.c1 == 0);
    CHECK(vtype("A1") == SH_EMPTY && !sh_modified(S));
    CHECK_STR(sh_redo_label(S), "typing");
    CHECK(sh_redo(S, NULL) == 0 && num("A1") == 1 && sh_modified(S));
    sh_saved(S);
    CHECK(!sh_modified(S));
    set("A1", "2");
    CHECK(sh_modified(S));
    CHECK(sh_undo(S, NULL) == 0 && !sh_modified(S));    /* back to what the file holds */
    CHECK(sh_undo(S, NULL) == 0 && sh_modified(S));
    CHECK(sh_redo(S, NULL) == 0 && !sh_modified(S));
    set("A2", "x");                                 /* a new change after Undo: no Redo */
    CHECK(sh_redo(S, NULL) < 0 && !sh_redo_label(S));
    set("A2", "x");                                 /* the same again is no change */
    CHECK_STR(sh_undo_label(S), "typing");
    sh_undo(S, NULL);
    CHECK(vtype("A2") == SH_EMPTY);
    /* every kind of change, undone and redone */
    fresh();
    set("A1", "1");
    set("A2", "=A1*2");
    set("B1", "text");
    a = save(S, SH_NATIVE);
    CHECK(sh_clear(S, rg("A1:B1")) == 0 && vtype("A1") == SH_EMPTY && num("A2") == 0);
    CHECK_STR(sh_undo_label(S), "clearing");
    sh_undo(S, NULL);
    b = save(S, SH_NATIVE);
    CHECK(!strcmp(a, b) && num("A2") == 2);
    free(b);
    CHECK(sh_set_format(S, rg("A1:A3"), SH_FIXED(1)) == 0);
    CHECK_STR(show("A2"), "2.0");
    CHECK(sh_format(S, 2, 0) == SH_FIXED(1));
    sh_undo(S, NULL);
    b = save(S, SH_NATIVE);
    CHECK(!strcmp(a, b));
    free(b);
    CHECK(sh_set_width(S, 0, 2, 5) == 0 && sh_width(S, 1) == SH_WIDTH_MIN);
    CHECK(sh_set_width(S, 0, 0, 5000) == 0 && sh_width(S, 0) == SH_WIDTH_MAX);
    sh_undo(S, NULL);
    sh_undo(S, NULL);
    b = save(S, SH_NATIVE);
    CHECK(!strcmp(a, b));
    free(b);
    sh_redo(S, NULL);
    CHECK(sh_width(S, 2) == SH_WIDTH_MIN);
    sh_undo(S, NULL);
    free(a);
    /* 100 steps are kept */
    fresh();
    for(i = 0; i < 105; i++) {
        char t[16];
        snprintf(t, sizeof(t), "%d", i);
        set("A1", t);
    }
    for(i = 0; sh_undo(S, NULL) == 0; i++);
    CHECK(i == 100);
    CHECK_STR(show("A1"), "4");
    /* a step bigger than the history: made, and the history starts again */
    fresh();
    set("A1", "x");
    {
        static char big[8001];
        struct sh_clip *c;
        char name[16];
        memset(big, 'y', 8000);
        for(i = 1; i <= 128; i++) {
            snprintf(name, sizeof(name), "B%d", i);
            set(name, big);
        }
        c = copy("B1:B128");
        for(i = 0; i < 9; i++) CHECK(sh_paste(S, c, i * 128, 2, NULL) == 0);
        sh_clip_free(c);
        c = copy("C1:C1152");                       /* 1,152 cells of 8,000 bytes: 9 MB */
        CHECK(paste(c, "D1") == 0);
        CHECK(!sh_undo_label(S) && sh_modified(S));
        CHECK_STR(show("D1152"), big);
        sh_clip_free(c);
    }
    /* a whole column's format on 32,768 scattered rows: made in one merge, not a row at a time */
    {
        static char text[32768 * 24 + 32];
        char why[SH_WHY];
        struct sheet *t;
        int n = snprintf(text, sizeof(text), "SurfOS sheet 1\n");
        for(i = 0; i < 32768; i++) n += snprintf(text + n, sizeof(text) - (size_t)n, "cell B%d general 1\n", 2 * i + 2);
        t = load(SH_NATIVE, text, n, 0, why);
        CHECK(t != NULL);
        if(t) {
            struct sh_range col = { 0, 0, SH_ROWS - 1, 0 };
            CHECK(sh_set_format(t, col, SH_PERCENT(0)) == 0 && sh_cells(t) == 32768 + 65536);
            CHECK(sh_undo(t, NULL) == 0 && sh_cells(t) == 32768);
            CHECK(sh_redo(t, NULL) == 0 && sh_format(t, 65535, 0) == SH_PERCENT(0));
            sh_free(t);
        }
    }
    /* Undo shows where */
    fresh();
    set("C7", "1");
    set("A1", "2");
    sh_undo(S, &w);
    sh_undo(S, &w);
    CHECK(w.r1 == 6 && w.c1 == 2);
}

/**** files ****/

static void test_native(void) {
    static char text[4 * SH_INPUT_MAX + 256];
    char why[SH_WHY], *a, *b;
    struct sheet *t;
    int i, n, ok;
    fresh();
    set("A1", "Total");
    set("B1", "=SUM(B2:B3)");
    set("B2", "0.1");
    set("B3", "1E+21");
    set("C1", "'42");
    set("C2", "''x");
    set("C3", "'");
    set("D1", "line\none\ttab\\back\x01\x7f");
    set("D2", "=\"q\"\"uote\"&\"\\\"");
    set("D3", "TRUE");
    set("D4", "#N/A");
    set("E1", "50%");
    set("E2", "caf\xc3\xa9");
    set("E3", "=1/3");
    set("E4", "-0");
    set("E5", "=1+");
    sh_set_format(S, rg("F1:F2"), SH_FIXED(3));
    set("F1", "2.5");
    sh_set_width(S, 1, 1, 120);
    sh_set_width(S, 255, 255, 16);
    a = save(S, SH_NATIVE);
    CHECK_STR(a, "SurfOS sheet 1\nwidth B 120\nwidth IV 16\n"
                 "cell A1 general Total\ncell B1 general =SUM(B2:B3)\ncell C1 general '42\ncell D1 general "
                 "line\\none\\ttab\\\\back\\x01\\x7F\ncell E1 percent0 50%\ncell F1 fixed3 2.5\n"
                 "cell B2 general 0.1\ncell C2 general ''x\ncell D2 general =\"q\"\"uote\"&\"\\\\\"\n"
                 "cell E2 general caf\xc3\xa9\ncell F2 fixed3\ncell B3 general 1E+21\ncell C3 general '\n"
                 "cell D3 general TRUE\ncell E3 general =1/3\ncell D4 general #N/A\ncell E4 general 0\n"
                 "cell E5 general =1+\n");
    CHECK(round_trips(S));
    t = load(SH_NATIVE, a, (int)strlen(a), 1, why);    /* a byte at a time */
    CHECK(t != NULL);
    if(t) {
        b = save(t, SH_NATIVE);
        CHECK(!strcmp(a, b));
        free(b);
        b = save(t, SH_CSV);
        CHECK_STR(b, "Total,1E+21,42,\"line\none\ttab\\back\x01\x7f\",0.5,2.5\r\n"
                     ",0.1,'x,\"q\"\"uote\\\",caf\xc3\xa9,\r\n"
                     ",1E+21,,TRUE,0.3333333333333333,\r\n"
                     ",,,#N/A,0,\r\n"
                     ",,,,#ERROR!,\r\n");
        free(b);
        CHECK(sh_width(t, 1) == 120 && sh_width(t, 255) == 16 && !sh_modified(t));
        sh_free(t);
    }
    /* cut at every byte: a sheet or a refusal, never anything else */
    for(i = 0, ok = 1, n = (int)strlen(a); i <= n; i++) {
        t = load(SH_NATIVE, a, i, 7, why);
        if(t) sh_free(t);
        else if(!why[0]) ok = 0;
    }
    CHECK(ok);
    free(a);
    /* by hand: comments, blank lines, CRLF, small letters in names */
    a = "# made by hand\r\nSurfOS sheet 1\r\n\r\ncell b2 general 5\r\ncell A1 fixed10 =b2*2\r\n";
    t = load(SH_NATIVE, a, (int)strlen(a) - 1, 0, why);     /* the last line end left off too */
    CHECK(t != NULL);
    if(t) {
        struct sh_value v;
        sh_value(t, 0, 0, &v);
        CHECK(v.type == SH_NUMBER && v.num == 10);
        sh_free(t);
    }
    t = load(SH_NATIVE, "", 0, 0, why);
    CHECK(t != NULL && sh_cells(t) == 0);
    sh_free(t);
    /* refusals, with where */
    {
        static const struct { const char *text, *why; } bad[] = {
            { "hello\n", "This is not a SurfOS sheet" },
            { "SurfOS sheet 2\n", "Line 1: the file was made by a newer Spreadsheet" },
            { "SurfOS sheet 1\nwidth B 15\n", "Line 2: a width is 16 to 800 pixels" },
            { "SurfOS sheet 1\nwidth B 801\n", "Line 2: a width is 16 to 800 pixels" },
            { "SurfOS sheet 1\nwidth IW 80\n", "Line 2: no such column" },
            { "SurfOS sheet 1\nwidth B\n", "Line 2: a width needs a column and pixels" },
            { "SurfOS sheet 1\ncell A1 fixed11 1\n", "Line 2: no such format" },
            { "SurfOS sheet 1\ncell A1 fixed02 1\n", "Line 2: no such format" },
            { "SurfOS sheet 1\ncell A1 bold 1\n", "Line 2: no such format" },
            { "SurfOS sheet 1\ncell A0 general 1\n", "Line 2: no such cell" },
            { "SurfOS sheet 1\ncell $A$1 general 1\n", "Line 2: no such cell" },
            { "SurfOS sheet 1\ncell A65537 general 1\n", "Line 2: no such cell" },
            { "SurfOS sheet 1\ncell A1\n", "Line 2: a cell needs its name and format" },
            { "SurfOS sheet 1\n\ncell A1 general a\\qb\n", "Line 3: a \\ that begins no escape" },
            { "SurfOS sheet 1\ncell A1 general a\\x4\n", "Line 2: \\x needs two hexadecimal digits" },
            { "SurfOS sheet 1\ncell A1 general a\\x00\n", "Line 2: a cell cannot hold a NUL character" },
            { "SurfOS sheet 1\ncell A1 general a\\\n", "Line 2: a \\ ends the line" },
            { "SurfOS sheet 1\nrow 1\n", "Line 2: not a line of a sheet" },
            { "SurfOS sheet 1\ncell A1 general 1\ncell A1 general 2\n", "A1 is in the file twice" },
            { "SurfOS sheet 1\ncell B7 general 1\ncell A1 general 2\ncell B7 general 3\n", "B7 is in the file twice" },
        };
        for(i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
            t = load(SH_NATIVE, bad[i].text, (int)strlen(bad[i].text), 0, why);
            checks++;
            if(t || strncmp(why, bad[i].why, strlen(bad[i].why))) {
                failed++;
                printf("FAIL: %s read as \"%s\", not \"%s\"\n", bad[i].text, why, bad[i].why);
            }
            if(t) sh_free(t);
        }
    }
    t = load(SH_NATIVE, "SurfOS sheet 1\ncell A1 general a\0b\n", 33, 0, why);
    CHECK(!t && strstr(why, "NUL"));
    n = snprintf(text, sizeof(text), "SurfOS sheet 1\ncell A1 general ");
    memset(text + n, 'x', SH_INPUT_MAX + 1);
    t = load(SH_NATIVE, text, n + SH_INPUT_MAX + 1, 0, why);
    CHECK(!t && strstr(why, "8,192"));
    memset(text + n, 'x', SH_INPUT_MAX);
    t = load(SH_NATIVE, text, n + SH_INPUT_MAX, 0, why);
    CHECK(t != NULL);
    sh_free(t);
    memset(text + n, '\x01', SH_INPUT_MAX);         /* every byte escaped: the longest line there is */
    t = load(SH_NATIVE, text, n + SH_INPUT_MAX, 0, why);
    CHECK(t != NULL);
    if(t) {
        CHECK(round_trips(t));
        sh_free(t);
    }
    memset(text + n, 'x', sizeof(text) - (size_t)n);
    t = load(SH_NATIVE, text, (int)sizeof(text), 0, why);
    CHECK(!t && strstr(why, "too long"));
}

static void test_csv(void) {
    static char big[70000 * 4];
    char why[SH_WHY], *a;
    struct sheet *t, *keep = S;
    int n, i;
    /* RFC 4180's examples */
    t = load(SH_CSV, "aaa,bbb,ccc\r\nzzz,yyy,xxx\r\n", 26, 0, why);
    CHECK(t != NULL);
    if(t) {
        S = t;
        CHECK_STR(show("A1"), "aaa");
        CHECK_STR(show("C2"), "xxx");
        CHECK(vtype("A3") == SH_EMPTY);
        a = save(t, SH_CSV);
        CHECK_STR(a, "aaa,bbb,ccc\r\nzzz,yyy,xxx\r\n");
        free(a);
        sh_free(t);
    }
    t = load(SH_CSV, "aaa,bbb,ccc\r\nzzz,yyy,xxx", 24, 0, why);
    CHECK(t != NULL);
    if(t) {
        S = t;
        CHECK_STR(show("C2"), "xxx");
        sh_free(t);
    }
    t = load(SH_CSV, "\"aaa\",\"b\r\nbb\",\"ccc\"\r\nzzz,yyy,xxx\r\n", 34, 3, why);
    CHECK(t != NULL);
    if(t) {
        S = t;
        CHECK_STR(show("B1"), "b\r\nbb");
        CHECK_STR(show("A2"), "zzz");
        sh_free(t);
    }
    t = load(SH_CSV, "\"aaa\",\"b\"\"bb\",\"ccc\"", 19, 0, why);
    CHECK(t != NULL);
    if(t) {
        S = t;
        CHECK_STR(show("B1"), "b\"bb");
        sh_free(t);
    }
    /* line ends, empty fields and rows, a byte order mark, numbers and text */
    a = "\xef\xbb\xbf" "1,-2.5,007,.5,1e3,TRUE,true,=1+1,\"42\",1,000\nx\r\r\n\n,,last";
    t = load(SH_CSV, a, (int)strlen(a), 2, why);
    CHECK(t != NULL);
    if(t) {
        S = t;
        CHECK(num("A1") == 1 && num("B1") == -2.5);
        CHECK(vtype("C1") == SH_TEXT && vtype("D1") == SH_TEXT && num("E1") == 1000);
        CHECK(vtype("F1") == SH_BOOL && vtype("G1") == SH_TEXT && vtype("H1") == SH_TEXT);
        CHECK_STR(show("H1"), "=1+1");
        CHECK(num("I1") == 42 && num("J1") == 1 && vtype("K1") == SH_TEXT);    /* 000: no number as JSON writes one */
        CHECK_STR(show("A2"), "x");
        CHECK(vtype("A3") == SH_EMPTY && vtype("A4") == SH_EMPTY);
        CHECK_STR(show("C5"), "last");
        CHECK(vtype("A6") == SH_EMPTY);
        CHECK_STR(inp("C1"), "'007");
        a = save(t, SH_CSV);
        CHECK_STR(a, "1,-2.5,007,.5,1000,TRUE,true,=1+1,42,1,000\r\nx,,,,,,,,,,\r\n,,,,,,,,,,\r\n,,,,,,,,,,\r\n"
                     ",,last,,,,,,,,\r\n");
        free(a);
        sh_free(t);
    }
    t = load(SH_CSV, "\xef\xbb", 2, 1, why);       /* a byte order mark's start alone is text */
    CHECK(t != NULL);
    if(t) {
        S = t;
        CHECK_STR(show("A1"), "\xef\xbb");
        sh_free(t);
    }
    t = load(SH_CSV, "a\"b,\"c\"d\n", 9, 0, why);  /* lenient where RFC 4180 has no answer */
    CHECK(t != NULL);
    if(t) {
        S = t;
        CHECK_STR(show("A1"), "a\"b");
        CHECK_STR(show("B1"), "cd");
        sh_free(t);
    }
    t = load(SH_CSV, "\"open", 5, 0, why);
    CHECK(t != NULL);
    if(t) {
        S = t;
        CHECK_STR(show("A1"), "open");
        sh_free(t);
    }
    t = load(SH_CSV, "", 0, 0, why);
    CHECK(t != NULL && sh_cells(t) == 0);
    sh_free(t);
    /* what a sheet writes */
    S = keep;
    fresh();
    set("A1", "a,b");
    set("B1", "say \"x\"");
    set("C1", "=1/3");
    set("A2", "=1/0");
    set("B2", "=1=1");
    set("D3", "line\nend");
    set("E5", "");
    sh_set_format(S, rg("F9"), SH_FIXED(2));
    a = save(S, SH_CSV);
    CHECK_STR(a, "\"a,b\",\"say \"\"x\"\"\",0.3333333333333333,\r\n#DIV/0!,TRUE,,\r\n,,,\"line\nend\"\r\n");
    free(a);
    fresh();
    a = save(S, SH_CSV);
    CHECK_STR(a, "");
    free(a);
    /* limits */
    for(n = i = 0; i < 257; i++) n += snprintf(big + n, sizeof(big) - (size_t)n, "%d,", i);
    big[n - 1] = '\n';
    t = load(SH_CSV, big, n, 0, why);
    CHECK(!t && strstr(why, "256 fields") && strstr(why, "Row 1"));
    t = load(SH_CSV, big, n - 5, 0, why);           /* 256 fields */
    CHECK(t != NULL);
    sh_free(t);
    memset(big, 'x', SH_INPUT_MAX + 1);
    t = load(SH_CSV, big, SH_INPUT_MAX + 1, 0, why);
    CHECK(!t && strstr(why, "8,192"));
    t = load(SH_CSV, "a,b\nc\0d\n", 8, 0, why);
    CHECK(!t && strstr(why, "NUL") && strstr(why, "Row 2"));
    memset(big, '\n', 65537);
    t = load(SH_CSV, big, 65536, 0, why);
    CHECK(t != NULL);
    sh_free(t);
    t = load(SH_CSV, big, 65537, 0, why);
    CHECK(!t && strstr(why, "65,536 rows"));
    /* a round trip: what a CSV holds, saved and read again, is the same */
    a = "x,1.5,\"a\nb\",\r\n,,\"\"\"\",TRUE\r\n";
    t = load(SH_CSV, a, (int)strlen(a), 0, why);
    CHECK(t != NULL);
    if(t) {
        char *b;
        struct sheet *u;
        a = save(t, SH_CSV);
        u = load(SH_CSV, a, (int)strlen(a), 0, why);
        CHECK(u != NULL);
        if(u) {
            b = save(u, SH_CSV);
            CHECK(!strcmp(a, b));
            free(b);
            sh_free(u);
        }
        free(a);
        sh_free(t);
    }
    CHECK(sh_kind_of("/usb1p1/data.csv") == SH_CSV && sh_kind_of("A.CSV") == SH_CSV);
    CHECK(sh_kind_of("a.sheet") == SH_NATIVE && sh_kind_of("csv") == SH_NATIVE && sh_kind_of("a.csvx") == SH_NATIVE);
}

/**** the grid as the window shows it ****/

static int at_is(struct view *v, int x, int y, int what, int row, int col) {
    struct view_at a;
    view_at(v, x, y, &a);
    return a.what == what && a.row == row && a.col == col;
}

static int sel_is(struct view *v, const char *range) {
    struct sh_range r, want = rg(range);
    view_selection(v, &r);
    return r.r1 == want.r1 && r.c1 == want.c1 && r.r2 == want.r2 && r.c2 == want.c2;
}

static void test_view(void) {
    struct view v;
    struct sh_range r;
    char st[SH_WHY];
    int i;
    fresh();
    view_init(&v, S);
    view_place(&v, 0, 28, 600, 300);            /* a formula bar of 28 above it */
    CHECK(view_rows_shown(&v) == 14 && view_cols_shown(&v) == 6);
    CHECK(view_col_x(&v, 0) == 48 && view_col_x(&v, 1) == 128 && view_col_x(&v, 6) == 528 && view_col_x(&v, 7) == -1);
    CHECK(view_row_y(&v, 0) == 48 && view_row_y(&v, 13) == 308 && view_row_y(&v, 14) == -1);
    CHECK(view_chars(&v, 0) == 9);              /* 80 pixels, 3 each side: 9 characters */
    CHECK(at_is(&v, 50, 50, VA_CELL, 0, 0) && at_is(&v, 130 + 80, 70, VA_CELL, 1, 2));
    CHECK(at_is(&v, 10, 50, VA_ROWHEAD, 0, -1) && at_is(&v, 10, 30, VA_CORNER, -1, -1));
    CHECK(at_is(&v, 100, 30, VA_COLHEAD, -1, 0) && at_is(&v, 126, 30, VA_EDGE, -1, 0));
    CHECK(at_is(&v, 129, 30, VA_EDGE, -1, 0) && at_is(&v, 140, 30, VA_COLHEAD, -1, 1));
    CHECK(at_is(&v, 700, 50, VA_NONE, -1, -1) && at_is(&v, 50, 20, VA_NONE, -1, -1));
    /* keys */
    CHECK(view_key(&v, V_DOWN, 0) && v.row == 1 && v.col == 0);
    CHECK(view_key(&v, V_RIGHT, VM_SHIFT) && v.row == 1 && v.col == 0 && sel_is(&v, "A2:B2"));
    CHECK(view_key(&v, V_DOWN, VM_SHIFT) && sel_is(&v, "A2:B3"));
    CHECK(view_key(&v, V_PGDN, 0) && v.row == 15 && v.top == 14 && sel_is(&v, "A16"));
    CHECK(view_key(&v, V_HOME, VM_CTRL) && v.row == 0 && v.col == 0 && v.top == 0);
    CHECK(view_key(&v, V_UP, 0) && v.row == 0);     /* at the edge: stays */
    CHECK(!view_key(&v, 'x', 0) && !view_key(&v, '\r', 0));
    for(i = 1; i <= 5; i++) {
        char name[8];
        snprintf(name, sizeof(name), "A%d", i);
        set(name, "1");
    }
    set("A10", "1");
    set("D1", "x");
    view_key(&v, V_DOWN, VM_CTRL);
    CHECK(v.row == 4);                          /* along the filled cells to their last */
    view_key(&v, V_DOWN, VM_CTRL);
    CHECK(v.row == 9);                          /* across the empty ones to the next filled */
    view_key(&v, V_DOWN, VM_CTRL);
    CHECK(v.row == SH_ROWS - 1 && v.top == SH_ROWS - 14);   /* to the sheet's edge */
    view_key(&v, V_UP, VM_CTRL | VM_SHIFT);
    CHECK(v.row == SH_ROWS - 1 && v.erow == 9);
    view_go(&v, 0, 0, 0);
    view_key(&v, V_RIGHT, VM_CTRL);
    CHECK(v.col == 3);
    view_key(&v, V_RIGHT, VM_CTRL);
    CHECK(v.col == SH_COLS - 1);
    view_key(&v, V_HOME, 0);
    CHECK(v.col == 0 && v.left == 0);
    view_key(&v, V_END, 0);
    CHECK(v.col == 3 && v.row == 0);            /* the row's last filled cell */
    view_key(&v, V_END, VM_CTRL);
    CHECK(v.row == 9 && v.col == 3);            /* the last row and column that hold anything */
    /* Enter and Tab */
    view_go(&v, 0, 0, 0);
    view_tab(&v, 0);
    view_tab(&v, 0);
    CHECK(v.row == 0 && v.col == 2);
    view_enter(&v, 0);
    CHECK(v.row == 1 && v.col == 0);            /* back to the column the Tabs began in */
    view_enter(&v, 1);
    CHECK(v.row == 0 && v.col == 0);
    view_tab(&v, 1);
    CHECK(v.col == 0);
    /* the pointer */
    CHECK(view_press(&v, 48 + 2 * 80 + 5, 48 + 2 * 20 + 5, 0) && v.row == 2 && v.col == 2 && v.drag == VD_CELLS);
    CHECK(view_move(&v, 48 + 3 * 80 + 5, 48 + 4 * 20 + 5) && sel_is(&v, "C3:D5"));
    CHECK(view_release(&v) && v.drag == VD_NONE);
    view_press(&v, 48 + 5, 48 + 5, VM_SHIFT);
    CHECK(sel_is(&v, "A1:C3") && v.row == 2 && v.col == 2);
    view_release(&v);
    view_press(&v, 48 + 80 + 20, 30, 0);
    CHECK(sel_is(&v, "B1:B65536") && v.col == 1 && v.row == 0);
    view_move(&v, 48 + 3 * 80 + 20, 30);
    CHECK(sel_is(&v, "B1:D65536"));
    view_release(&v);
    view_press(&v, 10, 48 + 2 * 20 + 5, 0);
    CHECK(sel_is(&v, "A3:IV3"));
    view_release(&v);
    view_press(&v, 10, 30, 0);
    CHECK(sel_is(&v, "A1:IV65536"));
    view_release(&v);
    /* a column's width by its header's edge, set once when let go */
    view_go(&v, 0, 0, 0);
    CHECK(view_press(&v, 126, 30, 0) && v.drag == VD_WIDTH);
    CHECK(view_move(&v, 146, 30) && view_width(&v, 0) == 100 && view_col_x(&v, 1) == 148 && sh_width(S, 0) == 80);
    view_move(&v, -500, 30);
    CHECK(view_width(&v, 0) == SH_WIDTH_MIN);
    view_move(&v, 146, 30);
    CHECK(view_release(&v) && sh_width(S, 0) == 100);
    CHECK_STR(sh_undo_label(S), "column width");
    view_press(&v, 100, 30, 0);                 /* columns A to C selected whole: each takes the width */
    view_move(&v, 48 + 100 + 80 + 20, 30);
    view_release(&v);
    CHECK(sel_is(&v, "A1:C65536"));
    view_press(&v, 48 + 100 + 78, 30, 0);       /* B's right edge */
    CHECK(v.drag == VD_WIDTH && v.drag_col == 1);
    view_move(&v, 48 + 100 + 78 - 40, 30);
    view_release(&v);
    CHECK(sh_width(S, 0) == 40 && sh_width(S, 1) == 40 && sh_width(S, 2) == 40 && sh_width(S, 3) == 80);
    sh_undo(S, NULL);
    sh_undo(S, NULL);
    CHECK(sh_width(S, 0) == 80 && sh_width(S, 1) == 80);
    /* a drag past the bottom scrolls a row a move */
    view_go(&v, 0, 0, 0);
    view_press(&v, 60, 48 + 13 * 20 + 5, 0);
    CHECK(v.row == 13);
    view_move(&v, 60, 400);
    CHECK(v.erow == 14 && v.top == 1);
    view_move(&v, 60, 400);
    CHECK(v.erow == 15 && v.top == 2);
    view_move(&v, 60, 10);                      /* above: back up */
    CHECK(v.top == 1 && v.erow == 1);
    view_release(&v);
    /* the wheel */
    v.top = 0;
    CHECK(view_wheel(&v, 3, 0) && v.top == 3);
    CHECK(view_wheel(&v, -3, VM_SHIFT) == 0 && v.left == 0);
    CHECK(view_wheel(&v, 3, VM_SHIFT) && v.left == 1);
    CHECK(view_wheel(&v, -30, 0) && v.top == 0);
    /* a far cell scrolls into view whole */
    view_go(&v, 70000, 100, 0);
    CHECK(v.row == SH_ROWS - 1 && v.col == 100 && view_col_x(&v, 100) + view_width(&v, 100) <= 600);
    CHECK(view_col_x(&v, 100) >= 0 && v.top == SH_ROWS - 14);
    /* text runs on over empty cells to its right */
    fresh();
    view_init(&v, S);
    view_place(&v, 0, 28, 600, 300);
    set("B1", "a long text");
    set("D1", "1");
    CHECK(view_text_end(&v, 0, 1) == 48 + 3 * 80);
    CHECK(view_text_end(&v, 0, 4) == 600);      /* to the grid's edge */
    set("C1", "=\"\"");
    CHECK(view_text_end(&v, 0, 1) == 48 + 2 * 80);   /* "" is a value: it stops there */
    /* the status line */
    set("A1", "1");
    set("A2", "2");
    set("A3", "3");
    set("A4", "x");
    r = rg("A1:A4");
    view_select(&v, r);
    view_status(&v, st, sizeof(st));
    CHECK_STR(st, "Sum 6  Average 2  Count 4");
    view_select(&v, rg("A4:B4"));
    view_status(&v, st, sizeof(st));
    CHECK_STR(st, "Count 1");
    view_select(&v, rg("A1"));
    view_status(&v, st, sizeof(st));
    CHECK_STR(st, "");
    set("A1", "=SUM(1");
    view_status(&v, st, sizeof(st));
    CHECK_STR(st, "a ( has no ) (at character 5)");
    /* a window too small for anything still shows a row and a column */
    view_place(&v, 0, 0, 10, 10);
    CHECK(view_rows_shown(&v) == 1 && view_cols_shown(&v) == 1);
}

/**** allocations refused in turn ****/

static void base(void) {
    fresh();
    set("A1", "1");
    set("A2", "=A1*2");
    set("A3", "text");
    set("B1", "=SUM(A1:A2)&\"!\"");
    sh_set_format(S, rg("C1"), SH_PERCENT(1));
    sh_set_width(S, 2, 2, 100);
}

static struct sh_clip *the_clip;
static int op_set_formula(void) { return set("D5", "=A2+CONCAT(A3,\"x\")"); }
static int op_set_text(void) { return set("A1", "a new text"); }
static int op_clear(void) { return sh_clear(S, rg("A1:B3")); }
static int op_format(void) { return sh_set_format(S, rg("A1:D4"), SH_FIXED(2)); }
static int op_format_big(void) { return sh_set_format(S, rg("E1:E200"), SH_FIXED(1)); }
static int op_width(void) { return sh_set_width(S, 0, 5, 50); }
static int op_insert_rows(void) { return sh_insert_rows(S, 1, 2); }
static int op_delete_rows(void) { return sh_delete_rows(S, 0, 1); }
static int op_insert_cols(void) { return sh_insert_cols(S, 0, 1); }
static int op_delete_cols(void) { return sh_delete_cols(S, 0, 1); }
static int op_paste(void) { return sh_paste(S, the_clip, 3, 3, NULL); }
static int op_paste_text(void) { return paste_text("1\t=A1\nx\t\n", "B2"); }
static int op_undo(void) { return sh_undo(S, NULL); }
static int op_redo(void) { return sh_redo(S, NULL); }

static void faults(const char *name, int (*op)(void), int undone) {
    char *before, *after, *values, *values_after;
    long blocks;
    int k, r, hit, ok = 1, n = 0;
    for(k = 0; k < 100000; k++) {
        base();
        if(the_clip) sh_clip_free(the_clip);
        the_clip = sh_copy(S, rg("A1:B2"));
        if(undone == 1) sh_delete_rows(S, 0, 1);    /* Undo makes its cells again */
        if(undone == 2) {                           /* Redo makes a cell in a new row */
            set("H9", "new");
            sh_undo(S, NULL);
        }
        if(undone == 3) {                           /* Undo makes 100 rows again */
            int i;
            for(i = 0; i < 100; i++) sh_set(S, i * 2, 6, "x", 1);
            sh_delete_rows(S, 0, 200);
        }
        before = save(S, SH_NATIVE);
        values = save(S, SH_CSV);
        blocks = sh_mem_blocks;
        sh_mem_fail = k;
        r = op();
        hit = sh_mem_fail < 0;
        sh_mem_fail = -1;
        after = save(S, SH_NATIVE);
        values_after = save(S, SH_CSV);
        if(r < 0) {
            n++;
            if(strcmp(before, after) || strcmp(values, values_after) || sh_mem_blocks != blocks) ok = 0;
            if(strcmp(before, after)) printf("%s, allocation %d refused: the sheet changed\n", name, k);
            if(strcmp(values, values_after)) printf("%s, allocation %d refused: its values changed\n", name, k);
            if(sh_mem_blocks != blocks)
                printf("%s, allocation %d refused: %ld blocks leaked\n", name, k, sh_mem_blocks - blocks);
        } else {
            recalc(S);                              /* an allocation of the recalculation: #LIMIT! until the next */
            if(!values_hold(S)) {
                ok = 0;
                printf("%s, allocation %d refused: values do not hold\n", name, k);
            }
        }
        free(before);
        free(after);
        free(values);
        free(values_after);
        if(!hit) break;
    }
    checks++;
    if(!ok || !n) {
        failed++;
        printf("FAIL: %s with allocations refused (%d refusals)\n", name, n);
    }
}

static void test_faults(void) {
    char why[SH_WHY];
    struct sheet *t;
    struct sh_clip *c;
    long blocks;
    int k, ok, hit;
    faults("typing a formula", op_set_formula, 0);
    faults("typing text", op_set_text, 0);
    faults("clearing", op_clear, 0);
    faults("a format", op_format, 0);
    faults("a format of many rows", op_format_big, 0);
    faults("a width", op_width, 0);
    faults("inserting rows", op_insert_rows, 0);
    faults("deleting rows", op_delete_rows, 0);
    faults("inserting columns", op_insert_cols, 0);
    faults("deleting columns", op_delete_cols, 0);
    faults("a paste", op_paste, 0);
    faults("pasting text", op_paste_text, 0);
    faults("Undo", op_undo, 1);
    faults("Redo", op_redo, 2);
    faults("Undo of many rows", op_undo, 3);
    sh_clip_free(the_clip);
    the_clip = NULL;
    /* Copy and the readers: NULL with a reason, nothing kept */
    base();
    for(k = 0, ok = 1; k < 100000; k++) {
        blocks = sh_mem_blocks;
        sh_mem_fail = k;
        c = sh_copy(S, rg("A1:C3"));
        hit = sh_mem_fail < 0;
        sh_mem_fail = -1;
        if(!c && (sh_mem_blocks != blocks || !strstr(sh_why(S), "memory"))) ok = 0;
        sh_clip_free(c);
        if(!hit) break;
    }
    CHECK(ok && k > 3);
    {
        static const char *const files[] = {
            "SurfOS sheet 1\nwidth C 99\ncell A1 general 1\ncell B2 fixed2 =A1*2\ncell C3 general text\n",
            "1,2,x\r\n\"a\nb\",=c,TRUE\r\n",
        };
        int f;
        for(f = 0; f < 2; f++)
            for(k = 0, ok = 1; k < 100000; k++) {
                blocks = sh_mem_blocks;
                sh_mem_fail = k;
                t = load(f ? SH_CSV : SH_NATIVE, files[f], (int)strlen(files[f]), 5, why);
                hit = sh_mem_fail < 0;
                sh_mem_fail = -1;
                if(t) {
                    recalc(t);
                    sh_free(t);
                }
                if(sh_mem_blocks != blocks || (!t && !why[0])) ok = 0;
                if(!hit) break;
            }
        CHECK(ok && k > 5);
    }
}

/**** the long runs: generated and damaged input ****/

static const char *const atoms[] = {
    "1", "0", "2.5", ".5", "1e3", "1E-2", "\"a\"", "\"\"", "\"x\"\"y\"", "A1", "B2", "$A$1", "A$2", "$B1", "A1:B3",
    "B3:A1", "C1:C9", "$A1:B$4", "IV65536", "A65536", "IV1", "TRUE", "FALSE", "#N/A", "#DIV/0!", "#REF!", "FOO", "e1",
    "\"\xc3\xa9\"", "D4", "E5", "F6",
};
static const char *const binops[] = { "+", "-", "*", "/", "^", "&", "=", "<>", "<", "<=", ">", ">=" };
static const char *const fnames[] = {
    "SUM", "AVERAGE", "MIN", "MAX", "COUNT", "COUNTA", "IF", "AND", "OR", "NOT", "ROUND", "INT", "ABS", "SQRT", "POWER",
    "MOD", "LEN", "LEFT", "RIGHT", "MID", "UPPER", "LOWER", "TRIM", "CONCAT", "NOPE", "sum",
};
#define NOF(a) ((int)(sizeof(a) / sizeof(a[0])))

static void add(char *f, int size, const char *s) {
    size_t n = strlen(f);
    if(n + strlen(s) < (size_t)size) strcpy(f + n, s);
}

static void gen(char *f, int size, int depth) {
    int k = depth > 5 ? 0 : below(7), i, n;
    switch(k) {
    case 0: add(f, size, atoms[below(NOF(atoms))]); break;
    case 1: add(f, size, below(2) ? "-" : "+"); gen(f, size, depth + 1); break;
    case 2: case 3: gen(f, size, depth + 1); add(f, size, binops[below(NOF(binops))]); gen(f, size, depth + 1); break;
    case 4: add(f, size, "("); gen(f, size, depth + 1); add(f, size, ")"); break;
    case 5:
        add(f, size, fnames[below(NOF(fnames))]);
        add(f, size, "(");
        for(i = 0, n = below(4); i < n; i++) {
            if(i) add(f, size, below(8) ? "," : ";");
            gen(f, size, depth + 1);
        }
        add(f, size, ")");
        break;
    default: gen(f, size, depth + 1); add(f, size, "%"); break;
    }
    if(!below(10)) add(f, size, " ");
}

static void damage(char *f, int size) {
    static const char junk[] = "()+-*/^&%=<>,;\"$:#.Ee1A \t\xc3\xa9\x80\x01!?@_";
    int n = (int)strlen(f), i, k = 1 + below(3);
    for(i = 0; i < k; i++) {
        int at = below(n + 1), what = below(3);
        if(what == 0 && n + 1 < size) {
            memmove(f + at + 1, f + at, (size_t)(n - at + 1));
            f[at] = junk[below((int)sizeof(junk) - 1)];
            n++;
        } else if(what == 1 && at < n) {
            memmove(f + at, f + at + 1, (size_t)(n - at));
            n--;
        } else if(at < n) f[at] = junk[below((int)sizeof(junk) - 1)];
    }
}

static int contains(const char *s, const char *what) { return strstr(s, what) != NULL; }

static void fuzz_formulas(int rounds) {
    static char f[2048], a[SH_INPUT_MAX], b[SH_INPUT_MAX], c[SH_INPUT_MAX];
    int i, ok = 1, n, m, k, dr, dc, at, cnt;
    fresh();
    for(i = 0; i < 30; i++) {                       /* values for the formulas to use */
        char name[8], v[16];
        snprintf(name, sizeof(name), "%c%d", 'A' + below(6), 1 + below(9));
        snprintf(v, sizeof(v), below(3) ? "%d" : "x%d", below(100));
        set(name, v);
    }
    for(i = 0; i < rounds; i++) {
        strcpy(f, "=");
        gen(f, sizeof(f), 0);
        if(below(3) == 0) damage(f + 1, (int)sizeof(f) - 1);
        k = (int)strlen(f);
        sh_set(S, below(9), 6 + below(3), f, k);
        n = fx_rewrite(f + 1, k - 1, FX_NORMAL, 0, 0, a, sizeof(a));
        if(n >= 0) {                                /* in capitals once, the same twice */
            m = fx_rewrite(a, n, FX_NORMAL, 0, 0, b, sizeof(b));
            if(m != n || strcmp(a, b)) ok = 0;
            dr = below(9) - 4;
            dc = below(9) - 4;
            m = fx_rewrite(a, n, FX_MOVE, dr, dc, b, sizeof(b));
            if(m >= 0 && !contains(b, "#REF!") && !contains(a, "#REF!")) {      /* moved there and back */
                cnt = fx_rewrite(b, m, FX_MOVE, -dr, -dc, c, sizeof(c));
                if(cnt != n || strcmp(a, c)) {
                    ok = 0;
                    printf("moving %d,%d and back: %s -> %s -> %s\n", dr, dc, a, b, c);
                }
            }
            at = below(12);
            cnt = 1 + below(3);
            m = fx_rewrite(a, n, FX_ROWS, at, cnt, b, sizeof(b));
            if(m >= 0 && !contains(b, "#REF!") && !contains(a, "65536") && !contains(a, "#REF!")) {
                k = fx_rewrite(b, m, FX_ROWS, at, -cnt, c, sizeof(c));      /* inserted and deleted again */
                if(k != n || strcmp(a, c)) {
                    ok = 0;
                    printf("rows at %d, %d: %s -> %s -> %s\n", at, cnt, a, b, c);
                }
            }
            m = fx_rewrite(a, n, FX_COLS, at, cnt, b, sizeof(b));
            if(m >= 0 && !contains(b, "#REF!") && !contains(a, "IV") && !contains(a, "#REF!")) {
                k = fx_rewrite(b, m, FX_COLS, at, -cnt, c, sizeof(c));
                if(k != n || strcmp(a, c)) {
                    ok = 0;
                    printf("columns at %d, %d: %s -> %s -> %s\n", at, cnt, a, b, c);
                }
            }
        }
        if(i % 500 == 0 && !values_hold(S)) ok = 0;
    }
    CHECK(ok);
    CHECK(values_hold(S));
}

static void fuzz_csv(int rounds) {
    static const char alpha[] = "ab10.-e,\"\r\n \t\xc3\xa9=TRUE";
    static char text[600];
    char why[SH_WHY], *a, *b;
    struct sheet *t, *u;
    int i, n, j, ok = 1;
    for(i = 0; i < rounds; i++) {
        n = below(500);
        for(j = 0; j < n; j++) text[j] = alpha[below((int)sizeof(alpha) - 1)];
        if(!below(50) && n) text[below(n)] = 0;
        t = load(SH_CSV, text, n, 1 + below(40), why);
        if(!t) {
            if(!why[0]) ok = 0;
            continue;
        }
        a = save(t, SH_CSV);
        u = load(SH_CSV, a, (int)strlen(a), 0, why);
        if(!u) ok = 0;
        else {
            b = save(u, SH_CSV);
            if(strcmp(a, b)) {
                ok = 0;
                printf("CSV again differs:\n%s\n---\n%s\n", a, b);
            }
            free(b);
            sh_free(u);
        }
        free(a);
        sh_free(t);
    }
    CHECK(ok);
}

/* a random change to S: 0, or 1 when it was Undo or Redo */
/**** the review (11/09/26): one test for each of its findings in the model ****/

static int puts_seen;
static int count_put(void *ctx, const char *b, int n) {
    (void)ctx;
    (void)b;
    (void)n;
    puts_seen++;
    return 0;
}

static void test_review(void) {
    static char text[8300], csv[8300], got[8300];
    char why[SH_WHY], name[16], *a, *b, *files[2];
    struct sh_reader *rd;
    struct sh_value v;
    struct sheet *t, *u, *old;
    struct sh_clip *c;
    int len, k, n, ok;
    /* a text's ' is not counted: "'" and 8,192 bytes is a cell, and 8,193 bytes of anything else is not */
    fresh();
    text[0] = '\'';
    text[1] = '=';
    memset(text + 2, 'b', 8191);
    CHECK(sh_set(S, 0, 0, text, 8193) == 0 && vtype("A1") == SH_TEXT && strlen(inp("A1")) == 8193);
    CHECK(sh_set(S, 0, 1, text + 1, 8192) == 0 && vtype("B1") == SH_ERROR);   /* a formula: =bbb... is #NAME? */
    text[0] = 'b';
    CHECK(sh_set(S, 0, 2, text, 8193) < 0 && vtype("C1") == SH_EMPTY);
    /* anything wsheet saves, wsheet opens: CSV fields of 8,190 to 8,192 bytes, two kinds that need the ' as text
       (= in front, a number's look) and one that does not, saved as .sheet, opened, saved again */
    ok = 1;
    for(len = 8190; len <= 8192; len++)
        for(k = 0; k < 3; k++) {
            memset(csv, k == 1 ? '7' : 'a', (size_t)len);
            csv[0] = k == 0 ? '=' : k == 1 ? '\'' : 'a';
            memcpy(csv + len, "\r\n", 2);
            t = load(SH_CSV, csv, len + 2, 4096, why);
            if(!t) {
                printf("the CSV of a %d-byte field refused: %s\n", len, why);
                ok = 0;
                continue;
            }
            sh_input(t, 0, 0, got, sizeof(got));
            a = save(t, SH_NATIVE);
            u = load(SH_NATIVE, a, (int)strlen(a), 1000, why);
            if(!u) {
                printf("the .sheet of a %d-byte field refused: %s\n", len, why);
                ok = 0;
            } else {
                b = save(u, SH_NATIVE);
                sh_value(u, 0, 0, &v);
                if(strcmp(a, b) || (int)strlen(got) != len + (k < 2) || v.type != SH_TEXT || v.len != len ||
                   memcmp(v.text, csv, (size_t)len))
                    ok = 0;
                free(b);
            }
            sh_free(u);
            sh_free(t);
            free(a);
        }
    CHECK(ok);
    /* nothing is written that a reader would refuse: past sh_file_max nothing at all, the same bytes read */
    fresh();
    set("A1", "hello");
    set("B2", "=A1&\"!\"");
    a = save(S, SH_NATIVE);
    n = (int)strlen(a);
    CHECK(sh_write_size(S, SH_NATIVE) == n);
    sh_file_max = n - 1;
    puts_seen = 0;
    CHECK(sh_write(S, SH_NATIVE, count_put, NULL) < 0 && puts_seen == 0);
    t = load(SH_NATIVE, a, n, 0, why);
    CHECK(!t && !strcmp(why, "The file is bigger than 32 MB"));
    sh_file_max = n;
    CHECK(sh_write(S, SH_NATIVE, count_put, NULL) == 0 && puts_seen > 0);
    t = load(SH_NATIVE, a, n, 0, why);
    if(t) sh_value(t, 1, 1, &v);
    CHECK(t && v.type == SH_TEXT && !strcmp(v.text, "hello!"));
    sh_free(t);
    free(a);
    sh_file_max = SH_FILE_MAX;
    a = save(S, SH_CSV);
    n = (int)strlen(a);
    CHECK(sh_write_size(S, SH_CSV) == n);
    sh_file_max = n - 1;
    puts_seen = 0;
    CHECK(sh_write(S, SH_CSV, count_put, NULL) < 0 && puts_seen == 0);
    sh_file_max = SH_FILE_MAX;
    free(a);
    /* what a CSV save leaves out */
    fresh();
    CHECK(sh_csv_loss(S) == 0);
    set("A1", "5");
    set("A2", "text");
    CHECK(sh_csv_loss(S) == 0);
    set("A3", "50%");
    CHECK(sh_csv_loss(S) == SH_LOSE_FORMATS);
    sh_set_width(S, 2, 2, 120);
    CHECK(sh_csv_loss(S) == (SH_LOSE_FORMATS | SH_LOSE_WIDTHS));
    set("B1", "=A1*2");
    CHECK(sh_csv_loss(S) == (SH_LOSE_FORMATS | SH_LOSE_WIDTHS | SH_LOSE_FORMULAS));
    sh_steps_max = 1;
    set("B2", "=A1*3");
    CHECK((sh_csv_loss(S) & SH_LOSE_LIMIT) && sh_limited(S));
    sh_steps_max = SH_STEPS;
    set("B2", "=A1*4");
    CHECK(!(sh_csv_loss(S) & SH_LOSE_LIMIT) && num("B2") == 20);
    /* a Cut pasted moves its cells: references into them go along, $ and all, a range when all of it was cut;
       the rest stays where it is */
    fresh();
    set("A1", "4");
    set("C1", "=A1*2");
    c = sh_copy(S, rg("C1"));
    sh_clip_cut(c);
    sh_clear(S, rg("C1"));
    CHECK(c && sh_paste(S, c, 4, 2, NULL) == 0 && vtype("C1") == SH_EMPTY && num("C5") == 8);
    CHECK_STR(inp("C5"), "=A1*2");
    sh_clip_free(c);
    set("A1", "3");
    set("B1", "=A1*2+$A$1");
    set("C1", "=SUM(A1:B1)+SUM(A1:A2)");
    c = sh_copy(S, rg("A1:C1"));
    sh_clip_cut(c);
    sh_clear(S, rg("A1:C1"));
    CHECK(c && sh_paste(S, c, 9, 0, NULL) == 0 && num("B10") == 9 && num("C10") == 12);
    CHECK_STR(inp("B10"), "=A10*2+$A$10");
    CHECK_STR(inp("C10"), "=SUM(A10:B10)+SUM(A1:A2)");
    sh_clip_free(c);
    /* text work is counted: 8 bytes compared, scanned or made a step */
    fresh();
    memset(text, 'a', 8000);
    text[8000] = 0;
    set("A1", text);
    set("A2", text);
    sh_steps_max = 100000;
    for(n = 0; n < 40; n++) {
        snprintf(name, sizeof(name), "B%d", n + 1);
        set(name, "=A1=A2");
    }
    CHECK(!sh_limited(S) && vtype("B40") == SH_BOOL);       /* 40 of 2,000 steps */
    for(; n < 100; n++) {
        snprintf(name, sizeof(name), "B%d", n + 1);
        set(name, "=A1=A2");
    }
    CHECK(sh_limited(S) && vtype("B100") == SH_ERROR);      /* 100: past 100,000 */
    fresh();
    set("A1", text);
    for(n = 0; n < 150; n++) {
        snprintf(name, sizeof(name), "C%d", n + 1);
        set(name, n % 2 ? "=LEN(A1)" : "=UPPER(A1)");
    }
    CHECK(sh_limited(S));
    sh_steps_max = SH_STEPS;
    set("D1", "1");
    CHECK(!sh_limited(S) && num("C2") == 8000);
    /* the recalculation's arrays never run out: each formula keeps its share back */
    fresh();
    sh_mem_max = sh_mem_used + read_headroom + 150000;
    for(n = 0; n < 65536; n++) {
        snprintf(name, sizeof(name), "A%d", n + 1);
        if(set(name, "=1+1") < 0) break;
    }
    snprintf(name, sizeof(name), "A%d", n);
    CHECK(n > 100 && n < 65536 && !sh_limited(S) && num("A1") == 2 && num(name) == 2);
    sh_mem_max = SH_MEMORY;
    /* a sheet opened in place of another: the other goes before the new one is worked out; refused, it stays */
    fresh();
    set("A1", "1");
    set("A2", "=A1+1");
    a = save(S, SH_NATIVE);
    old = S;
    rd = sh_read_begin(SH_NATIVE);
    sh_read(rd, a, (int)strlen(a));
    t = sh_read_finish(rd, why, sizeof(why), &old);
    CHECK(t && !old);
    S = t;                                      /* the old one is freed */
    CHECK(num("A2") == 2);
    old = S;
    rd = sh_read_begin(SH_NATIVE);
    sh_read(rd, "not a sheet\n", 12);
    t = sh_read_finish(rd, why, sizeof(why), &old);
    CHECK(!t && old == S && num("A2") == 2);
    free(a);
    /* a formula whose text does not split into tokens is kept as typed when rows move; one that splits but does not
       parse has its references moved (the manual) */
    fresh();
    set("A1", "7");
    set("B5", "=A1*2+B9?");
    set("B6", "=A1+");
    sh_insert_rows(S, 0, 2);
    CHECK_STR(inp("B7"), "=A1*2+B9?");
    CHECK_STR(inp("B8"), "=A3+");
    /* the second review: a sheet grown until an edit is refused (texts of 200 bytes down 65,536 rows and on), saved
       as .sheet and as CSV, opens again: edits stop read_headroom short of the budget, all of which a reader may use */
    fresh();
    memset(text, 'x', 200);
    for(n = k = 0; sh_set(S, k % SH_ROWS, k / SH_ROWS, text, 200) == 0; k++) n++;
    CHECK(n > 100000 && sh_mem_used <= sh_mem_max - read_headroom && !strcmp(sh_why(S), "Not enough memory for the change"));
    files[SH_NATIVE] = save(S, SH_NATIVE);
    files[SH_CSV] = save(S, SH_CSV);
    fresh();                                    /* a window that holds nothing else: a fresh one, or Open's */
    for(k = SH_NATIVE; k <= SH_CSV; k++) {
        t = load(k, files[k], (int)strlen(files[k]), 65536, why);
        if(!t) printf("the %s of a sheet at its budget refused: %s\n", k == SH_CSV ? "CSV" : ".sheet", why);
        CHECK(t && sh_cells(t) == n && !sh_limited(t));
        sh_free(t);
        free(files[k]);
    }
}

/* a row or a column for a change: mostly among the first, sometimes at the sheet's far edge or in its middle
   (11/09/26, the review: the changes' fuzzing kept to A1:F10) */
static int pick(int near, int limit) {
    switch(below(8)) {
    case 0: return limit - 1 - below(near);
    case 1: return limit / 2 + below(near);
    default: return below(near);
    }
}

static int random_change(void) {
    static char f[512];
    struct sh_clip *c;
    char name[8];
    int k = below(16), r = pick(10, SH_ROWS), col = pick(6, SH_COLS);
    sh_cellname(r, col, name);
    switch(k) {
    case 0: case 1: snprintf(f, sizeof(f), "%d", below(1000) - 500); set(name, f); break;
    case 2: set(name, below(2) ? "text" : "'12"); break;
    case 3: case 4:
        strcpy(f, "=");
        gen(f, sizeof(f), 2);
        set(name, f);
        break;
    case 5: sh_clear(S, (struct sh_range){ r, col, r + below(3), col + below(3) }); break;
    case 6:
        sh_set_format(S, (struct sh_range){ r, col, r + below(3), col }, below(2) ? SH_FIXED(below(4)) : SH_PERCENT(1));
        break;
    case 7: sh_set_width(S, col, col + below(2), 16 + below(200)); break;
    case 8: sh_insert_rows(S, r, 1 + below(2)); break;
    case 9: sh_delete_rows(S, r, 1 + below(2)); break;
    case 10: sh_insert_cols(S, col, 1 + below(2)); break;
    case 11: sh_delete_cols(S, col, 1 + below(2)); break;
    case 12:
        c = sh_copy(S, (struct sh_range){ r, col, r + below(3), col + below(3) });
        if(c) {
            if(!below(3)) sh_clip_cut(c);       /* pasted as a Cut's: moved */
            sh_paste(S, c, pick(10, SH_ROWS), pick(6, SH_COLS), NULL);
            sh_clip_free(c);
        }
        break;
    case 13: snprintf(f, sizeof(f), "%d\t=A%d\n\tx", below(9), 1 + below(9)); paste_text(f, name); break;
    case 14: sh_undo(S, NULL); return 1;
    default: sh_redo(S, NULL); return 1;
    }
    return 0;
}

static void fuzz_changes(int rounds) {
    char *before, *after, *again;
    unsigned state;
    int i, ok = 1;
    fresh();
    for(i = 0; i < rounds; i++) {
        before = save(S, SH_NATIVE);
        state = S->state;
        if(!random_change() && S->state != state && sh_undo_label(S)) {
            after = save(S, SH_NATIVE);
            if(sh_undo(S, NULL) < 0) ok = 0;
            again = save(S, SH_NATIVE);
            if(strcmp(before, again)) {
                ok = 0;
                printf("Undo did not give back:\n%s\n---\n%s\n", before, again);
            }
            free(again);
            if(sh_redo(S, NULL) < 0) ok = 0;
            again = save(S, SH_NATIVE);
            if(strcmp(after, again)) {
                ok = 0;
                printf("Redo did not give back:\n%s\n---\n%s\n", after, again);
            }
            free(again);
            free(after);
        }
        free(before);
        if(i % 25 == 0 && !values_hold(S)) ok = 0;
        if(!ok) break;
    }
    CHECK(ok);
    CHECK(values_hold(S));
    CHECK(round_trips(S));
}

static void fuzz_native(int rounds) {
    char why[SH_WHY], *good, *b, *c;
    struct sheet *t, *u;
    int i, n, j, ok = 1;
    fresh();
    for(i = 0; i < 60; i++) random_change();
    good = save(S, SH_NATIVE);
    n = (int)strlen(good);
    for(i = 0; i < rounds; i++) {
        static char text[1 << 16];
        int len = n < (int)sizeof(text) - 64 ? n : (int)sizeof(text) - 64;
        memcpy(text, good, (size_t)len);
        for(j = 1 + below(4); j > 0; j--) {
            int at = below(len), what = below(4);
            if(what == 0) text[at] = (char)rnd();
            else if(what == 1) text[at] = "\\\n $=:#\"x1"[below(10)];
            else if(what == 2 && len > 1) {
                memmove(text + at, text + at + 1, (size_t)(len - at - 1));
                len--;
            } else if(len < (int)sizeof(text) - 2) {
                memmove(text + at + 1, text + at, (size_t)(len - at));
                text[at] = "\\\n $=:#\"x1"[below(10)];
                len++;
            }
        }
        t = load(SH_NATIVE, text, len, 1 + below(100), why);
        if(!t) {
            if(!why[0]) ok = 0;
            continue;
        }
        b = save(t, SH_NATIVE);
        u = load(SH_NATIVE, b, (int)strlen(b), 0, why);
        if(!u) {
            ok = 0;
            printf("written, not read: %s\n", why);
        } else {
            c = save(u, SH_NATIVE);
            if(strcmp(b, c)) ok = 0;
            free(c);
            sh_free(u);
        }
        free(b);
        sh_free(t);
    }
    free(good);
    CHECK(ok);
}

int main(int argc, char **argv) {
    int lng = argc > 1 && !strcmp(argv[1], "long");
    test_names();
    test_numbers();
    test_language();
    test_values();
    test_functions();
    test_recalc();
    test_paste();
    test_structure();
    test_history();
    test_native();
    test_csv();
    test_view();
    test_faults();
    test_review();
    fuzz_formulas(lng ? 200000 : 3000);
    fuzz_csv(lng ? 100000 : 1500);
    fuzz_changes(lng ? 30000 : 600);
    fuzz_native(lng ? 50000 : 600);
    sh_free(S);
    S = NULL;
    CHECK(sh_mem_blocks == 0 && sh_mem_used == 0);  /* nothing left allocated */
    printf("test_wsheet: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
