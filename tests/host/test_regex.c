/* Host-side tests of the regular expressions, user/libsurf/regex.c (11/01/26, the command-line suite):

   - a table of patterns with what POSIX says they give, most of them the classic cases of Glenn Fowler's test suite,
     each also run through the host's regexec(). A host need not agree where it is not POSIX's: glibc keeps a
     subexpression from an earlier iteration, and so do others. Those are counted and said, not failures.
   - the errors, the limits, the flags, UTF-8, back-references
   - random patterns and strings against the host, whole match only, where glibc is reliable
   - random patterns and strings against an oracle in this file, which is POSIX's rule written down the slow way
     (does this node match this span? then divide the span from the left), subexpressions too
   - patterns that make a backtracking engine take forever, on long text

   The library is compiled with surf_ names. Its header comes in under them first, then the host's. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <locale.h>

#define regex_t surf_regex_t
#define regmatch_t surf_regmatch_t
#define regoff_t surf_regoff_t
#define regcomp surf_regcomp
#define regexec surf_regexec
#define regerror surf_regerror
#define regfree surf_regfree
#include "../../user/include/regex.h"
enum {
    S_EXTENDED = REG_EXTENDED, S_ICASE = REG_ICASE, S_NOSUB = REG_NOSUB, S_NEWLINE = REG_NEWLINE, S_NOTBOL = REG_NOTBOL,
    S_NOTEOL = REG_NOTEOL, S_STARTEND = REG_STARTEND, S_NOMATCH = REG_NOMATCH, S_BADPAT = REG_BADPAT,
    S_ECOLLATE = REG_ECOLLATE, S_ECTYPE = REG_ECTYPE, S_EESCAPE = REG_EESCAPE, S_ESUBREG = REG_ESUBREG,
    S_EBRACK = REG_EBRACK, S_EPAREN = REG_EPAREN, S_EBRACE = REG_EBRACE, S_BADBR = REG_BADBR, S_ERANGE = REG_ERANGE,
    S_ESPACE = REG_ESPACE, S_BADRPT = REG_BADRPT, S_ESIZE = REG_ESIZE, S_ESTEPS = REG_ESTEPS
};
#undef regex_t
#undef regmatch_t
#undef regoff_t
#undef regcomp
#undef regexec
#undef regerror
#undef regfree
#undef _REGEX_H
#undef RE_DUP_MAX
#undef REG_EXTENDED
#undef REG_ICASE
#undef REG_NOSUB
#undef REG_NEWLINE
#undef REG_NOTBOL
#undef REG_NOTEOL
#undef REG_STARTEND
#undef REG_NOMATCH
#undef REG_BADPAT
#undef REG_ECOLLATE
#undef REG_ECTYPE
#undef REG_EESCAPE
#undef REG_ESUBREG
#undef REG_EBRACK
#undef REG_EPAREN
#undef REG_EBRACE
#undef REG_BADBR
#undef REG_ERANGE
#undef REG_ESPACE
#undef REG_BADRPT
#undef REG_ESIZE
#undef REG_ESTEPS
#include <regex.h>

/* what the library expects of the rest of libsurf: tests/host/libsurf_names.h's names over the host's */
int surf_errno;
void *surf_malloc(size_t n) { return malloc(n); }
void surf_free(void *p) { free(p); }
void *surf_calloc(size_t n, size_t k) { return calloc(n, k); }
void *surf_realloc(void *p, size_t n) { return realloc(p, n); }

static int failures, checks, host_runs, host_differs;
static unsigned rng = 12345;

static unsigned rnd(unsigned n) {
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 16) & 0x7fff) % n;
}

static void fail(const char *what, const char *pat, const char *str, const char *got, const char *want) {
    failures++;
    printf("FAIL: %s /%s/ on [%s]: got %s, want %s\n", what, pat, str, got, want);
}

/* (so,eo)(so,eo)..., with the groups nobody took as (?,?) and those at the end left out; NOMATCH, or E<n> for an error */
static void show(char *buf, size_t size, int rc, const int *so, const int *eo, int n) {
    size_t len = 0;
    int i;
    if(rc == 1) {
        snprintf(buf, size, "NOMATCH");
        return;
    }
    if(rc) {
        snprintf(buf, size, "E%d", rc);
        return;
    }
    while(n > 1 && so[n - 1] < 0) n--;
    buf[0] = 0;
    for(i = 0; i < n; i++) {
        if(so[i] < 0) len += (size_t)snprintf(buf + len, size - len, "(?,?)");
        else len += (size_t)snprintf(buf + len, size - len, "(%d,%d)", so[i], eo[i]);
    }
}

#define MAXSUB 24

static void run_surf(const char *pat, int flags, const char *str, int eflags, char *out, size_t size) {
    surf_regex_t re;
    surf_regmatch_t m[MAXSUB];
    int so[MAXSUB], eo[MAXSUB], i, rc = surf_regcomp(&re, pat, flags);
    if(rc) {
        snprintf(out, size, "C%d", rc);
        return;
    }
    rc = surf_regexec(&re, str, MAXSUB, m, eflags);
    for(i = 0; i < MAXSUB; i++) {
        so[i] = m[i].rm_so;
        eo[i] = m[i].rm_eo;
    }
    show(out, size, rc, so, eo, MAXSUB);
    surf_regfree(&re);
}

static void run_host(const char *pat, int flags, const char *str, int eflags, char *out, size_t size) {
    regex_t re;
    regmatch_t m[MAXSUB];
    int so[MAXSUB], eo[MAXSUB], i, rc;
    rc = regcomp(&re, pat, (flags & S_EXTENDED ? REG_EXTENDED : 0) | (flags & S_ICASE ? REG_ICASE : 0) |
                           (flags & S_NEWLINE ? REG_NEWLINE : 0));
    if(rc) {
        snprintf(out, size, "C%d", rc);
        return;
    }
    rc = regexec(&re, str, MAXSUB, m, (eflags & S_NOTBOL ? REG_NOTBOL : 0) | (eflags & S_NOTEOL ? REG_NOTEOL : 0));
    for(i = 0; i < MAXSUB; i++) {
        so[i] = (int)m[i].rm_so;
        eo[i] = (int)m[i].rm_eo;
    }
    show(out, size, rc == REG_NOMATCH ? 1 : rc, so, eo, MAXSUB);
    regfree(&re);
}

/* the library against what POSIX says, and the host against the same */
static void expect(int flags, const char *pat, const char *str, const char *want) {
    char got[256], host[256];
    checks++;
    run_surf(pat, flags, str, 0, got, sizeof(got));
    if(strcmp(got, want)) fail("table", pat, str, got, want);
    run_host(pat, flags, str, 0, host, sizeof(host));
    host_runs++;
    if(strcmp(host, want)) {
        host_differs++;
        if(getenv("REGEX_VERBOSE")) printf("host differs: /%s/ on [%s]: host %s, POSIX %s\n", pat, str, host, want);
    }
}

#define E S_EXTENDED
#define B 0
#define I S_ICASE
#define BE(pat, str, want) do { expect(B, pat, str, want); expect(E, pat, str, want); } while(0)

static void table(void) {
    /* Fowler's basic.dat */
    expect(E, "abracadabra$", "abracadabracadabra", "(7,18)");
    expect(E, "a...b", "abababbb", "(2,7)");
    expect(E, "XXXXXX", "..XXXXXX", "(2,8)");
    expect(E, "\\)", "()", "(1,2)");
    expect(E, "a]", "a]a", "(0,2)");
    expect(E, "}", "}", "(0,1)");
    expect(E, "\\}", "}", "(0,1)");
    BE("\\]", "]", "(0,1)");
    expect(B, "]", "]", "(0,1)");
    expect(E, "]", "]", "(0,1)");
    expect(B, "{", "{", "(0,1)");
    expect(B, "}", "}", "(0,1)");
    BE("^a", "ab", "(0,1)");
    BE("\\^a", "a^a", "(1,3)");
    BE("a\\^", "a^", "(0,2)");
    BE("a$", "aa", "(1,2)");
    BE("a\\$", "a$", "(0,2)");
    BE("^$", "", "(0,0)");
    expect(E, "$^", "", "(0,0)");
    expect(E, "a($)", "aa", "(1,2)(2,2)");
    expect(E, "a*(^a)", "aa", "(0,1)(0,1)");
    expect(E, "(..)*(...)*", "a", "(0,0)");
    expect(E, "(..)*(...)*", "abcd", "(0,4)(2,4)");
    expect(E, "(ab|a)(bc|c)", "abc", "(0,3)(0,2)(2,3)");
    expect(E, "(ab)c|abc", "abc", "(0,3)(0,2)");
    expect(E, "a{0}b", "ab", "(1,2)");
    expect(E, "(a*)(b?)(b+)b{3}", "aaabbbbbbb", "(0,10)(0,3)(3,4)(4,7)");
    expect(E, "(a*)(b{0,1})(b{1,})b{3}", "aaabbbbbbb", "(0,10)(0,3)(3,4)(4,7)");
    expect(E, "((a|a)|a)", "a", "(0,1)(0,1)(0,1)");
    expect(E, "(a*)(a|aa)", "aaaa", "(0,4)(0,3)(3,4)");
    expect(E, "a*(a.|aa)", "aaaa", "(0,4)(2,4)");
    expect(E, "a(b)|c(d)|a(e)f", "aef", "(0,3)(?,?)(?,?)(1,2)");
    expect(E, "(a|b)?.*", "b", "(0,1)(0,1)");
    expect(E, "(a|b)c|a(b|c)", "ac", "(0,2)(0,1)");
    expect(E, "(a|b)c|a(b|c)", "ab", "(0,2)(?,?)(1,2)");
    expect(E, "(a|b)*c|(a|ab)*c", "abc", "(0,3)(1,2)");
    expect(E, "(a|b)*c|(a|ab)*c", "xc", "(1,2)");
    expect(E, "(.a|.b).*|.*(.a|.b)", "xa", "(0,2)(0,2)");
    expect(E, "a?(ab|ba)ab", "abab", "(0,4)(0,2)");
    expect(E, "a?(ac{0}b|ba)ab", "abab", "(0,4)(0,2)");
    expect(E, "ab|abab", "abbabab", "(0,2)");
    expect(E, "aba|bab|bba", "baaabbbaba", "(5,8)");
    expect(E, "aba|bab", "baaabbbaba", "(6,9)");
    expect(E, "(aa|aaa)*|(a|aaaaa)", "aa", "(0,2)(0,2)");
    expect(E, "(a.|.a.)*|(a|.a...)", "aa", "(0,2)(0,2)");
    expect(E, "ab|a", "xabc", "(1,3)");
    expect(E, "ab|a", "xxabc", "(2,4)");
    expect(E | I, "(Ab|cD)*", "aBcD", "(0,4)(2,4)");
    BE("[^-]", "--Z--", "(2,3)");
    BE("[a-]*", "--Z--", "(0,2)");
    BE("[a-m-]*", "--Z--", "(0,2)");
    expect(E, "a|b|c|d|e", "e", "(0,1)");
    expect(E, "(a|b|c|d|e)f", "ef", "(0,2)(0,1)");
    expect(E, "((a*|b))*", "-", "(0,0)(0,0)(0,0)");
    BE("abcd*efg", "abcdefg", "(0,7)");
    BE("ab*", "xabyabbbz", "(1,3)");
    BE("ab*", "xayabbbz", "(1,2)");
    expect(E, "(ab|cd)e", "abcde", "(2,5)(2,4)");
    BE("[abhgefdc]ij", "hij", "(0,3)");
    expect(E, "(a|b)c*d", "abcd", "(1,4)(1,2)");
    expect(E, "(ab|ab*)bc", "abc", "(0,3)(0,1)");
    expect(E, "a([bc]*)c*", "abc", "(0,3)(1,3)");
    expect(E, "a([bc]*)(c*d)", "abcd", "(0,4)(1,3)(3,4)");
    expect(E, "a([bc]+)(c*d)", "abcd", "(0,4)(1,3)(3,4)");
    expect(E, "a([bc]*)(c+d)", "abcd", "(0,4)(1,2)(2,4)");
    expect(E, "a[bcd]*dcdcde", "adcdcde", "(0,7)");
    expect(E, "(ab|a)b*c", "abc", "(0,3)(0,2)");
    expect(E, "((a)(b)c)(d)", "abcd", "(0,4)(0,3)(0,1)(1,2)(3,4)");
    BE("[A-Za-z_][A-Za-z0-9_]*", "alpha", "(0,5)");
    expect(E, "^a(bc+|b[eh])g|.h$", "abh", "(1,3)");
    expect(E, "(bc+d$|ef*g.|h?i(j|k))", "effgz", "(0,5)(0,5)");
    expect(E, "(bc+d$|ef*g.|h?i(j|k))", "ij", "(0,2)(0,2)(1,2)");
    expect(E, "(bc+d$|ef*g.|h?i(j|k))", "effg", "NOMATCH");
    expect(E, "(bc+d$|ef*g.|h?i(j|k))", "bcdd", "NOMATCH");
    expect(E, "(bc+d$|ef*g.|h?i(j|k))", "reffgz", "(1,6)(1,6)");
    expect(E, "((((((((((a))))))))))", "a", "(0,1)(0,1)(0,1)(0,1)(0,1)(0,1)(0,1)(0,1)(0,1)(0,1)(0,1)");
    BE("multiple words", "multiple words, yeah", "(0,14)");
    expect(E, "(.*)c(.*)", "abcde", "(0,5)(0,2)(3,5)");
    BE("abcd", "abcd", "(0,4)");
    expect(E, "a(bc)d", "abcd", "(0,4)(1,3)");
    expect(E, "(a+|b)*", "ab", "(0,2)(1,2)");
    expect(E, "(a+|b){0,}", "ab", "(0,2)(1,2)");
    expect(E, "(a+|b)+", "ab", "(0,2)(1,2)");
    expect(E, "(a+|b){1,}", "ab", "(0,2)(1,2)");
    expect(E, "(a+|b)?", "ab", "(0,1)(0,1)");
    expect(E, "(a+|b){0,1}", "ab", "(0,1)(0,1)");
    expect(B, "\\(ab*\\)c", "xabbc", "(1,5)(1,4)");
    expect(B, "a\\{2\\}", "caaa", "(1,3)");
    expect(B, "a\\{1,2\\}", "aaa", "(0,2)");
    expect(B, "\\(a\\)\\{2\\}", "aaa", "(0,2)(1,2)");
    /* nullsubexpr.dat */
    expect(E, "(a*)*", "a", "(0,1)(0,1)");
    expect(E, "(a*)*", "x", "(0,0)(0,0)");
    expect(E, "(a*)*", "aaaaaa", "(0,6)(0,6)");
    expect(E, "(a*)*", "aaaaaax", "(0,6)(0,6)");
    expect(E, "(a*)+", "a", "(0,1)(0,1)");
    expect(E, "(a*)+", "x", "(0,0)(0,0)");
    expect(E, "(a+)*", "a", "(0,1)(0,1)");
    expect(E, "(a+)*", "x", "(0,0)");
    expect(E, "(a+)*", "aaaaaa", "(0,6)(0,6)");
    expect(E, "(a+)+", "a", "(0,1)(0,1)");
    expect(E, "(a+)+", "x", "NOMATCH");
    expect(E, "([a]*)*", "a", "(0,1)(0,1)");
    expect(E, "([^b]*)*", "a", "(0,1)(0,1)");
    expect(E, "([^b]*)*", "b", "(0,0)(0,0)");
    expect(E, "([^b]*)*", "aaaaaab", "(0,6)(0,6)");
    expect(E, "([ab]*)*", "ababab", "(0,6)(0,6)");
    expect(E, "([ab]*)*", "aaaabcde", "(0,5)(0,5)");
    expect(E, "([^a]*)*", "aaaaaa", "(0,0)(0,0)");
    expect(E, "([^ab]*)*", "ccccxx", "(0,6)(0,6)");
    expect(E, "((z)+|a)*", "zabcde", "(0,2)(1,2)");
    expect(E, "(a*)*(x)", "x", "(0,1)(0,0)(0,1)");
    expect(E, "(a*)*(x)", "ax", "(0,2)(0,1)(1,2)");
    expect(E, "(a*)*(x)", "axa", "(0,2)(0,1)(1,2)");
    expect(E, "(a*)+(x)", "x", "(0,1)(0,0)(0,1)");
    expect(E, "(a*)+(x)", "ax", "(0,2)(0,1)(1,2)");
    expect(E, "(a*){2}(x)", "x", "(0,1)(0,0)(0,1)");
    expect(E, "(a*){2}(x)", "ax", "(0,2)(1,1)(1,2)");
    expect(E, "(a*){2}(x)", "axa", "(0,2)(1,1)(1,2)");
    /* repetition.dat, with Kuklewicz's corrections: only the last iteration counts, inside a group too */
    expect(E, "((..)|(.))", "", "NOMATCH");
    expect(E, "((..)|(.))*", "", "(0,0)");
    expect(E, "((..)|(.))", "a", "(0,1)(0,1)(?,?)(0,1)");
    expect(E, "((..)|(.)){2}", "a", "NOMATCH");
    expect(E, "((..)|(.))*", "a", "(0,1)(0,1)(?,?)(0,1)");
    expect(E, "((..)|(.))", "aa", "(0,2)(0,2)(0,2)");
    expect(E, "((..)|(.))((..)|(.))", "aa", "(0,2)(0,1)(?,?)(0,1)(1,2)(?,?)(1,2)");
    expect(E, "((..)|(.)){2}", "aa", "(0,2)(1,2)(?,?)(1,2)");
    expect(E, "((..)|(.))*", "aa", "(0,2)(0,2)(0,2)");
    expect(E, "((..)|(.)){3}", "aaa", "(0,3)(2,3)(?,?)(2,3)");
    expect(E, "((..)|(.))*", "aaa", "(0,3)(2,3)(?,?)(2,3)");
    expect(E, "((..)|(.)){2}", "aaa", "(0,3)(2,3)(?,?)(2,3)");
    expect(E, "(a|ab|c|bcd){0,}(d*)", "ababcd", "(0,6)(3,6)(6,6)");
    expect(E, "(a|ab|c|bcd){1,}(d*)", "ababcd", "(0,6)(3,6)(6,6)");
    expect(E, "(a|ab|c|bcd){2,}(d*)", "ababcd", "(0,6)(3,6)(6,6)");
    expect(E, "(a|ab|c|bcd){3,}(d*)", "ababcd", "(0,6)(3,6)(6,6)");
    expect(E, "(a|ab|c|bcd){4,}(d*)", "ababcd", "NOMATCH");
    expect(E, "(a|ab|c|bcd)*(d*)", "ababcd", "(0,6)(3,6)(6,6)");
    expect(E, "(a|ab|c|bcd){0,10}(d*)", "ababcd", "(0,6)(3,6)(6,6)");
    expect(E, "(a|ab|c|bcd){2,3}(d*)", "ababcd", "(0,6)(3,6)(6,6)");
    expect(E, "(ab|a|c|bcd)*(d*)", "ababcd", "(0,6)(3,6)(6,6)");
    expect(E, "(.)\\1", "abccd", "(2,4)(2,3)");
    /* the alternatives' longest, not the first */
    expect(E, "(a|ab)(c|bcd)(d*)", "abcd", "(0,4)(0,2)(2,3)(3,4)");
    expect(E, "(foo|foobar)(barfoo|foo)?", "foobarfoo", "(0,9)(0,6)(6,9)");
    expect(E, "x*", "aaa", "(0,0)");
    expect(E, "", "abc", "(0,0)");
    expect(B, "", "abc", "(0,0)");
    expect(E, "()", "abc", "(0,0)(0,0)");
    expect(E, "(|a)", "abc", "(0,1)(0,1)");
    expect(E, "(a|)", "abc", "(0,1)(0,1)");
    expect(E, "a||b", "b", "(0,1)");
    expect(B, "a\\|b", "b", "(0,1)");
    /* a star that is itself in basic syntax */
    expect(B, "*a", "*a", "(0,2)");
    expect(B, "\\(*a\\)", "*a", "(0,2)(0,2)");
    expect(B, "^*", "*", "(0,1)");
    expect(B, "a^b", "a^b", "(0,3)");
    expect(B, "a$b", "a$b", "(0,3)");
    expect(B, "\\(^a\\)", "a", "(0,1)(0,1)");
    expect(B, "\\(a$\\)", "a", "(0,1)(0,1)");
    expect(B, "a\\|^b", "b", "(0,1)");
    /* only the first ^ is an anchor, a star after any anchor is itself: as glibc reads them */
    expect(B, "^^a", "^a", "(0,2)");
    expect(B, "^^a", "a", "NOMATCH");
    expect(B, "^^", "^", "(0,1)");
    expect(B, "\\(^^a\\)", "^a", "(0,2)(0,2)");
    expect(B, "a\\|^^b", "^b", "(0,2)");
    expect(B, "^^*a", "^*a", "NOMATCH");
    expect(B, "^**a", "**a", "(0,3)");
    expect(B, "\\<^a", "a", "NOMATCH");
    expect(B, "\\<*a", "*a", "NOMATCH");
    /* braces that are no interval */
    expect(E, "{", "{", "(0,1)");
    expect(E, "a{", "a{", "(0,2)");
    expect(E, "a{x}", "a{x}", "(0,4)");
    expect(E, "a{1", "a{1", "(0,3)");
    expect(E, "a{,2}", "aaa", "(0,2)");
    expect(B, "a\\{,2\\}", "aaa", "(0,2)");
    /* brackets */
    expect(E, "[]a]+", "b]a]", "(1,4)");
    expect(E, "[^]a]+", "]ab]", "(2,3)");
    expect(E, "[a\\]+", "a\\", "(0,2)");
    expect(E, "[[:alpha:]]+", "12abC3", "(2,5)");
    expect(E, "[[:digit:]]+", "ab12c", "(2,4)");
    expect(E, "[[:alnum:]_]+", "-a1_-", "(1,4)");
    expect(E, "[[:upper:]]+", "abCDe", "(2,4)");
    expect(E, "[[:lower:]]+", "ABcdE", "(2,4)");
    expect(E, "[[:space:]]+", "a \t\nb", "(1,4)");
    expect(E, "[[:blank:]]+", "a \t\nb", "(1,3)");
    expect(E, "[[:punct:]]+", "a,.b", "(1,3)");
    expect(E, "[[:xdigit:]]+", "xyzAf09g", "(3,7)");
    expect(E, "[^[:space:]]+", "  ab ", "(2,4)");
    expect(E, "[[.a.]]", "a", "(0,1)");
    expect(E, "[[=a=]]", "a", "(0,1)");
    expect(E, "[[.-.]b]+", "a-b", "(1,3)");
    expect(E, "[%--]+", "a%,-b", "(1,4)");
    expect(E | I, "[a-c]+", "xABCx", "(1,4)");
    expect(E | I, "[[:upper:]]+", "1aB2", "(1,3)");
    expect(E | I, "[^a]", "Ab", "(1,2)");
    expect(E | I, "abc", "xAbC", "(1,4)");
    expect(B, "\\(a\\)*", "aa", "(0,2)(1,2)");
}

static void errors_case(int flags, const char *pat, int want) {
    surf_regex_t re;
    int rc = surf_regcomp(&re, pat, flags);
    checks++;
    if(rc != want) {
        char g[16], w[16];
        snprintf(g, sizeof(g), "C%d", rc);
        snprintf(w, sizeof(w), "C%d", want);
        fail("compile", pat, "", g, w);
    }
    if(!rc) surf_regfree(&re);
}

static void errors(void) {
    char big[4096];
    int i, n;
    errors_case(E, "(", S_EPAREN);
    errors_case(E, ")", S_EPAREN);
    errors_case(E, "a)", S_EPAREN);
    errors_case(E, "(a", S_EPAREN);
    errors_case(B, "\\(", S_EPAREN);
    errors_case(B, "\\)", S_EPAREN);
    errors_case(B, "a\\)", S_EPAREN);
    errors_case(E, "[", S_EBRACK);
    errors_case(E, "[a", S_EBRACK);
    errors_case(E, "[]", S_EBRACK);
    errors_case(E, "[[:alpha:]", S_EBRACK);
    errors_case(E, "[[:alpha", S_EBRACK);
    errors_case(E, "[[.a", S_EBRACK);
    errors_case(E, "[[:nosuch:]]", S_ECTYPE);
    errors_case(E, "[[.ab.]]", S_ECOLLATE);
    errors_case(E, "[z-a]", S_ERANGE);
    errors_case(E, "[[:alpha:]-z]", S_ERANGE);
    errors_case(E, "[a-[:alpha:]]", S_ERANGE);
    errors_case(E, "\\", S_EESCAPE);
    errors_case(B, "a\\", S_EESCAPE);
    errors_case(E, "*a", S_BADRPT);
    errors_case(E, "+a", S_BADRPT);
    errors_case(E, "?a", S_BADRPT);
    errors_case(E, "a|*b", S_BADRPT);
    errors_case(E, "(*a)", S_BADRPT);
    errors_case(E, "^*", S_BADRPT);
    errors_case(B, "\\{1\\}", S_BADRPT);
    errors_case(E, "a{2,1}", S_BADBR);
    errors_case(E, "a{32768}", S_BADBR);
    errors_case(E, "a{1,99999999999}", S_BADBR);
    errors_case(B, "a\\{2,1\\}", S_BADBR);
    errors_case(B, "a\\{x\\}", S_BADBR);
    errors_case(B, "a\\{1", S_EBRACE);
    errors_case(B, "\\1", S_ESUBREG);
    errors_case(B, "\\(a\\)\\2", S_ESUBREG);
    errors_case(B, "\\(a\\1\\)", S_ESUBREG);
    errors_case(E, "(a)\\2", S_ESUBREG);
    /* the limits: nesting of 64 goes, 65 is refused, and neither reaches the stack's end */
    for(n = 64; n <= 65; n++) {
        for(i = 0; i < n; i++) big[i] = '(';
        big[n] = 'a';
        for(i = 0; i < n; i++) big[n + 1 + i] = ')';
        big[2 * n + 1] = 0;
        errors_case(E, big, n == 64 ? 0 : S_ESIZE);
    }
    memset(big, '(', sizeof(big) - 1);          /* deep and never closed */
    big[sizeof(big) - 1] = 0;
    errors_case(E, big, S_ESIZE);
    errors_case(E, "(a{1000}){1000}", S_ESIZE);
    errors_case(E, "((a{100}){100}){100}", S_ESIZE);
    errors_case(E, "(()()()()()()()(){1000}){1000}", 0);    /* nothing to emit, so little to do */
    errors_case(E, "(()()()()()()()a){32767}", 0);
    errors_case(E, "(()()()()()()()()()()()()()()()()()()()()()()()()()()()()()()()()a){32767}", S_ESIZE);
    errors_case(E, "a{32767}", 0);
    errors_case(E, "a*****************", S_ESIZE);
    {
        /* 64 levels of 16 operators around 40000 states: what walk() would pay for is refused, not 41 s per 100 bytes */
        char *p = malloc(4096), *q = malloc(4096);
        strcpy(p, "(a*){20000}");
        for(i = 0; i < 63; i++) {
            snprintf(q, 4096, "(%s)****************", p);
            strcpy(p, q);
        }
        errors_case(E, p, S_ESIZE);
        /* and without groups there is nothing to report: no such limit */
        errors_case(E | S_NOSUB, p, 0);
        /* one level of it is fine */
        errors_case(E, "((a*){20000})*", 0);
        free(p);
        free(q);
    }
    {
        char *p = malloc(200001);               /* a pattern of more than the states there may be */
        memset(p, 'a', 200000);
        p[200000] = 0;
        errors_case(E, p, S_ESIZE);
        free(p);
    }
    {
        char msg[64];
        size_t k = surf_regerror(S_NOMATCH, NULL, msg, sizeof(msg));
        checks++;
        if(k != strlen("no match") + 1 || strcmp(msg, "no match")) fail("regerror", "", "", msg, "no match");
        k = surf_regerror(S_EBRACK, NULL, msg, 4);
        if(k != strlen("[ without its ]") + 1 || strcmp(msg, "[ w")) fail("regerror short", "", "", msg, "[ w");
        if(surf_regerror(S_EBRACK, NULL, NULL, 0) != k) fail("regerror none", "", "", "", "");
    }
}

static void flags_case(int flags, const char *pat, const char *str, int eflags, const char *want) {
    char got[256];
    checks++;
    run_surf(pat, flags, str, eflags, got, sizeof(got));
    if(strcmp(got, want)) fail("flags", pat, str, got, want);
}

static void flags(void) {
    surf_regex_t re;
    surf_regmatch_t m[4];
    int rc;
    flags_case(E | S_NEWLINE, "^b$", "a\nb\nc", 0, "(2,3)");
    flags_case(E | S_NEWLINE, "a.b", "a\nb", 0, "NOMATCH");
    flags_case(E, "a.b", "a\nb", 0, "(0,3)");
    flags_case(E | S_NEWLINE, "a[^x]b", "a\nb", 0, "NOMATCH");
    flags_case(E, "a[^x]b", "a\nb", 0, "(0,3)");
    flags_case(E | S_NEWLINE, "a[\n]b", "a\nb", 0, "(0,3)");
    flags_case(E, "^b", "a\nb", 0, "NOMATCH");
    flags_case(E, "a$", "a\nb", 0, "NOMATCH");
    flags_case(E | S_NEWLINE, "\\`b", "a\nb", 0, "NOMATCH");
    flags_case(E | S_NEWLINE, "a\\'", "a\nb", 0, "NOMATCH");
    flags_case(E, "^a", "a", S_NOTBOL, "NOMATCH");
    flags_case(E, "a$", "a", S_NOTEOL, "NOMATCH");
    flags_case(E | S_NEWLINE, "^b", "\nb", S_NOTBOL, "(1,2)");
    flags_case(E | S_NEWLINE, "a$", "a\n", S_NOTEOL, "(0,1)");
    flags_case(E, "\\bfoo\\b", "a foo b", 0, "(2,5)");
    flags_case(E, "\\Boo\\b", "a foo b", 0, "(3,5)");
    flags_case(E, "\\<b", "ab b", 0, "(3,4)");
    flags_case(E, "a\\>", "ab a", 0, "(3,4)");
    flags_case(E, "\\w+", "- ab_1 -", 0, "(2,6)");
    flags_case(E, "\\W+", "ab -c", 0, "(2,4)");
    flags_case(E, "\\s+", "a \tb", 0, "(1,3)");
    flags_case(E, "\\S+", "  ab ", 0, "(2,4)");
    flags_case(B, "a\\+", "caab", 0, "(1,3)");
    flags_case(B, "ca\\?b", "cb", 0, "(0,2)");
    flags_case(E, "\\(a\\)", "(a)", 0, "(0,3)");
    /* no room for all: the rest is simply not reported; more room than groups: -1 */
    rc = surf_regcomp(&re, "(a)(b)(c)", S_EXTENDED);
    checks++;
    if(rc || re.re_nsub != 3) fail("re_nsub", "(a)(b)(c)", "", "?", "3");
    memset(m, 0x55, sizeof(m));
    if(surf_regexec(&re, "abc", 2, m, 0) || m[1].rm_so != 0 || m[1].rm_eo != 1 || m[2].rm_so != 0x55555555)
        fail("two rooms", "(a)(b)(c)", "abc", "?", "(0,3)(0,1)");
    if(surf_regexec(&re, "abc", 0, m, 0)) fail("no room", "(a)(b)(c)", "abc", "?", "0");
    surf_regfree(&re);
    rc = surf_regcomp(&re, "(a)", S_EXTENDED);
    if(rc || surf_regexec(&re, "a", 4, m, 0) || m[2].rm_so != -1 || m[2].rm_eo != -1 || m[3].rm_so != -1)
        fail("more rooms", "(a)", "a", "?", "-1");
    surf_regfree(&re);
    /* REG_NOSUB: only the answer, and pmatch untouched */
    rc = surf_regcomp(&re, "a(b*)c", S_EXTENDED | S_NOSUB);
    m[0].rm_so = 77;
    if(rc || surf_regexec(&re, "xabbc", 4, m, 0) || m[0].rm_so != 77) fail("nosub", "a(b*)c", "xabbc", "?", "untouched");
    if(surf_regexec(&re, "xbbc", 4, m, 0) != S_NOMATCH) fail("nosub", "a(b*)c", "xbbc", "match", "NOMATCH");
    surf_regfree(&re);
    /* REG_STARTEND: bounds, NUL bytes inside, and what is before the start still counts */
    rc = surf_regcomp(&re, "b+", S_EXTENDED);
    m[0].rm_so = 2;
    m[0].rm_eo = 5;
    if(rc || surf_regexec(&re, "abbbbb", 1, m, S_STARTEND) || m[0].rm_so != 2 || m[0].rm_eo != 5)
        fail("startend", "b+", "abbbbb", "?", "(2,5)");
    m[0].rm_so = 1;
    m[0].rm_eo = 5;
    if(surf_regexec(&re, "a\0bb\0", 1, m, S_STARTEND) || m[0].rm_so != 2 || m[0].rm_eo != 4)
        fail("startend NUL", "b+", "a b b", "?", "(2,4)");
    surf_regfree(&re);
    rc = surf_regcomp(&re, "a.c", S_EXTENDED);
    m[0].rm_so = 0;
    m[0].rm_eo = 3;
    if(rc || surf_regexec(&re, "a\0c", 1, m, S_STARTEND) || m[0].rm_eo != 3) fail("dot NUL", "a.c", "a c", "?", "(0,3)");
    surf_regfree(&re);
    rc = surf_regcomp(&re, "^a", S_EXTENDED);
    m[0].rm_so = 1;
    m[0].rm_eo = 2;
    if(rc || surf_regexec(&re, "aa", 1, m, S_STARTEND) != S_NOMATCH) fail("startend ^", "^a", "aa", "match", "NOMATCH");
    surf_regfree(&re);
    rc = surf_regcomp(&re, "\\<a", S_EXTENDED);
    m[0].rm_so = 1;
    m[0].rm_eo = 2;
    if(rc || surf_regexec(&re, "aa", 1, m, S_STARTEND) != S_NOMATCH) fail("startend \\<", "\\<a", "aa", "match", "NOMATCH");
    surf_regfree(&re);
}

static void utf8(void) {
    /* a character is a whole sequence; é is 2 bytes, € 3, 𝄞 4 */
    flags_case(E, ".", "\xc3\xa9", 0, "(0,2)");
    flags_case(E, "^.$", "\xe2\x82\xac", 0, "(0,3)");
    flags_case(E, "^.$", "\xf0\x9d\x84\x9e", 0, "(0,4)");
    flags_case(E, "^..$", "a\xc3\xa9", 0, "(0,3)");
    flags_case(E, "^.$", "ab", 0, "NOMATCH");
    flags_case(E, "[\xc3\xa9]", "x\xc3\xa9", 0, "(1,3)");
    flags_case(E, "[^a]", "a\xc3\xa9", 0, "(1,3)");
    flags_case(E, "[\xc3\xa0-\xc3\xbf]+", "a\xc3\xa9\xc3\xa8z", 0, "(1,5)");
    flags_case(E, "\xc3\xa9*", "\xc3\xa9\xc3\xa9x", 0, "(0,4)");
    flags_case(E, "\xc3\xa9+x", "\xc3\xa9\xc3\xa9x", 0, "(0,5)");
    flags_case(E, "(.)(.)", "\xe2\x82\xac\xc3\xa9", 0, "(0,5)(0,3)(3,5)");
    flags_case(E, ".*", "\xe2\x82\xac\xc3\xa9", 0, "(0,5)");
    flags_case(E, "^(.*)(.)$", "\xe2\x82\xac\xc3\xa9", 0, "(0,5)(0,3)(3,5)");
    /* never inside a sequence */
    flags_case(E, "\xa9", "\xc3\xa9", 0, "NOMATCH");
    flags_case(E, "\xc3", "\xc3\xa9", 0, "NOMATCH");
    flags_case(E, "\x82\xac", "\xe2\x82\xac", 0, "NOMATCH");
    /* a byte in no sequence is a character of its own, and passes everything but a positive set */
    flags_case(E, "^.$", "\xff", 0, "(0,1)");
    flags_case(E, "^.$", "\xc3", 0, "(0,1)");
    flags_case(E, "^.$", "\x80", 0, "(0,1)");
    flags_case(E, "^..$", "\xe2\x82", 0, "(0,2)");
    flags_case(E, "^...$", "\xc0\x80z", 0, "(0,3)");            /* overlong */
    flags_case(E, "^...$", "\xed\xa0\x80", 0, "(0,3)");         /* a surrogate */
    flags_case(E, "^....$", "\xf4\x90\x80\x80", 0, "(0,4)");    /* above U+10FFFF */
    flags_case(E, "[^a]", "\xff", 0, "(0,1)");
    flags_case(E, "[b]", "\xff", 0, "NOMATCH");
    flags_case(E, "\xff", "a\xff", 0, "(1,2)");
    flags_case(E, "[\xff]", "a\xff", 0, "(1,2)");
    flags_case(E, "[\xfe-\xff]+", "a\xfe\xff", 0, "(1,3)");
    flags_case(E, "^(.)(.)$", "\xff\xc3\xa9", 0, "(0,3)(0,1)(1,3)");
    flags_case(E, "^(.*)\xff$", "\xc3\xa9\xff", 0, "(0,3)(0,2)");
    /* backward through a bad byte after a good sequence */
    flags_case(E, "^(.*)(.)(.)$", "\xe2\x82\xac\x80\xc3\xa9", 0, "(0,6)(0,3)(3,4)(4,6)");
    /* classes are ASCII */
    flags_case(E, "[[:alpha:]]", "\xc3\xa9", 0, "NOMATCH");
    flags_case(E, "[^[:alpha:]]", "\xc3\xa9", 0, "(0,2)");
    flags_case(E | S_ICASE, "\xc3\xa9", "\xc3\x89", 0, "NOMATCH");
    flags_case(E, "\\w", "\xc3\xa9", 0, "NOMATCH");
    /* the search for the first byte with UTF-8 */
    flags_case(E, "\xe2\x82\xac", "abc\xc3\xa9\xe2\x82\xac", 0, "(5,8)");
    flags_case(E, "\xc3\xa9|z", "abcz", 0, "(3,4)");
}

static void backrefs(void) {
    surf_regex_t re;
    surf_regmatch_t m[4];
    char *s;
    int rc, i;
    flags_case(B, "\\(a*\\)\\1", "aaaa", 0, "(0,4)(0,2)");
    flags_case(B, "\\(.\\)\\1", "abccd", 0, "(2,4)(2,3)");
    flags_case(B, "^\\(.*\\)\n\\1$", "abc\nabc", 0, "(0,7)(0,3)");
    flags_case(B, "^\\(.*\\)\n\\1$", "abc\nabd", 0, "NOMATCH");
    flags_case(B, "\\(a\\)\\(b\\)\\2\\1", "abba", 0, "(0,4)(0,1)(1,2)");
    flags_case(B, "\\(a*\\)b\\1", "aabaa", 0, "(0,5)(0,2)");
    flags_case(B, "\\(a*\\)b\\1", "aabaaa", 0, "(0,5)(0,2)");
    flags_case(E, "(a)\\1", "xaa", 0, "(1,3)(1,2)");
    flags_case(E, "(a|ab)\\1", "abab", 0, "(0,4)(0,2)");
    flags_case(E | S_ICASE, "(a)\\1", "aA", 0, "(0,2)(0,1)");
    flags_case(E, "(a*)*\\1", "aa", 0, "(0,2)(2,2)");     /* with these the first path is reported: aa, nothing, nothing */
    flags_case(E, "(a)|\\1b", "b", 0, "NOMATCH");
    flags_case(E, "((a)|b)*\\2", "ab", 0, "NOMATCH");        /* the last iteration has no \2 */
    flags_case(E, "(a*)+$\\1", "b", 0, "(1,1)(1,1)");
    flags_case(E, "(\xc3\xa9)\\1", "\xc3\xa9\xc3\xa9", 0, "(0,4)(0,2)");
    /* a back-reference is a character's bytes, not just bytes: the second \xC3 starts a sequence */
    flags_case(B, "\\(.\\)\\1", "\xc3\xc3\xa9", 0, "NOMATCH");
    flags_case(B, "\\(\xc3\\)\\1", "\xc3\xc3\xa9", 0, "NOMATCH");
    flags_case(B, "\\(.\\)\\1", "\xc3\xc3", 0, "(0,2)(0,1)");
    flags_case(B, "\\(.\\)\\1", "\xc3\xa9\xc3\xa9", 0, "(0,4)(0,2)");
    /* a star whose body can be empty ends */
    flags_case(E, "(a*)*b\\1", "aab", 0, "(0,3)(2,2)");
    flags_case(E, "(|a)*b\\1", "b", 0, "(0,1)(0,0)");
    /* the step limit: an error, not a hang, and the pattern works again afterwards */
    rc = surf_regcomp(&re, "\\(a\\|aa\\)*b\\1c", 0);
    s = malloc(64);
    memset(s, 'a', 50);
    strcpy(s + 50, "baaac");
    checks++;
    if(rc) fail("compile", "steps", "", "?", "0");
    rc = surf_regexec(&re, s, 4, m, 0);
    if(rc != S_ESTEPS) {
        char g[16];
        snprintf(g, sizeof(g), "%d", rc);
        fail("steps", "\\(a\\|aa\\)*b\\1c", "a..baaac", g, "REG_ESTEPS");
    }
    if(surf_regexec(&re, "aabac", 4, m, 0) || m[1].rm_so != 1 || m[1].rm_eo != 2)
        fail("after the limit", "\\(a\\|aa\\)*b\\1c", "aabac", "?", "(0,5)(1,2)");
    surf_regfree(&re);
    free(s);
    /* nothing like it matches: no steps at all */
    rc = surf_regcomp(&re, "\\(a\\|aa\\)*b\\1c", 0);
    s = malloc(100001);
    memset(s, 'a', 100000);
    s[100000] = 0;
    for(i = 0; i < 1000; i++)
        if(surf_regexec(&re, s + 99000, 1, m, 0) != S_NOMATCH) fail("no steps", "", "", "?", "NOMATCH");
    checks++;
    surf_regfree(&re);
    free(s);
}

/**** the oracle ****/

enum { O_EMPTY, O_CHAR, O_ANY, O_NOTA, O_AB, O_BOL, O_EOL, O_GROUP, O_CAT, O_ALT, O_REP };

#define ONODES 64
#define OKIDS 4
#define OLEN 8

struct onode {
    int type, c, min, max, group, ngroups;
    int kid[OKIDS], kids;
};

static struct onode on[ONODES];
static int onodes, ogroups;
static const char *otext;
static int olen;
static signed char memo[ONODES][OLEN + 1][OLEN + 1];
static int ocap[MAXSUB][2];

static int o_new(int type) {
    struct onode *n = &on[onodes];
    memset(n, 0, sizeof(*n));
    n->type = type;
    return onodes++;
}

/* a random pattern of at most about budget nodes */
static int o_gen(int depth, int budget, int anchors) {
    int n, i, k = rnd(depth > 3 || budget < 3 ? 6 : 12);
    if(onodes > ONODES - 8) k = 0;
    switch(k) {
    case 0: case 1:
        n = o_new(O_CHAR);
        on[n].c = 'a' + (int)rnd(3);
        return n;
    case 2:
        return o_new(O_ANY);
    case 3:
        return o_new(rnd(2) ? O_NOTA : O_AB);
    case 4:
        if(anchors) return o_new(rnd(2) ? O_BOL : O_EOL);
        n = o_new(O_CHAR);
        on[n].c = 'a';
        return n;
    case 5:
        if(!rnd(4)) return o_new(O_EMPTY);
        n = o_new(O_CHAR);
        on[n].c = 'b';
        return n;
    case 6: case 7:
        if(ogroups < MAXSUB - 2) {
            n = o_new(O_GROUP);
            on[n].group = ++ogroups;
            on[n].kid[on[n].kids++] = o_gen(depth + 1, budget - 1, anchors);
            return n;
        }
        /* fall through */
    case 8: case 9:
        n = o_new(O_CAT);
        i = 2 + (int)rnd(3);
        while(i--) {
            int c;
            do {
                c = o_gen(depth + 1, budget / 3, anchors);
            } while(on[c].type == O_CAT || on[c].type == O_ALT);    /* flat, as the parser makes it */
            on[n].kid[on[n].kids++] = c;
        }
        return n;
    case 10:
        n = o_new(O_ALT);
        i = 2 + (int)rnd(3);
        while(i--) {
            int c;
            do {
                c = o_gen(depth + 1, budget / 3, anchors);
            } while(on[c].type == O_ALT);
            on[n].kid[on[n].kids++] = c;
        }
        return n;
    default: {
        static const int counts[][2] = { { 0, -1 }, { 0, -1 }, { 1, -1 }, { 0, 1 }, { 2, 2 }, { 1, 2 }, { 2, -1 }, { 0, 2 }, { 0, 3 } };
        int w = (int)rnd(9), c;
        do {
            c = o_gen(depth + 1, budget / 2, anchors);
        } while(on[c].type != O_GROUP && on[c].type != O_CHAR && on[c].type != O_ANY && on[c].type != O_AB &&
                on[c].type != O_NOTA);                  /* what a repetition can follow */
        n = o_new(O_REP);
        on[n].min = counts[w][0];
        on[n].max = counts[w][1];
        on[n].kid[on[n].kids++] = c;
        return n;
    }
    }
}

/* groups count from the left, whatever order they were made in, and rejected ones are not there */
static void o_number(int n) {
    int k;
    if(on[n].type == O_GROUP) on[n].group = ++ogroups;
    for(k = 0; k < on[n].kids; k++) o_number(on[n].kid[k]);
}

/* the pattern in extended (ere) or basic syntax */
static size_t o_str(int n, char *b, size_t at, int ere) {
    const struct onode *o = &on[n];
    int i;
    switch(o->type) {
    case O_EMPTY: break;
    case O_CHAR: b[at++] = (char)o->c; break;
    case O_ANY: b[at++] = '.'; break;
    case O_NOTA: at += (size_t)snprintf(b + at, 64, "[^a]"); break;
    case O_AB: at += (size_t)snprintf(b + at, 64, "[ab]"); break;
    case O_BOL: b[at++] = '^'; break;
    case O_EOL: b[at++] = '$'; break;
    case O_GROUP:
        at += (size_t)snprintf(b + at, 64, ere ? "(" : "\\(");
        at = o_str(o->kid[0], b, at, ere);
        at += (size_t)snprintf(b + at, 64, ere ? ")" : "\\)");
        break;
    case O_CAT:
        for(i = 0; i < o->kids; i++) at = o_str(o->kid[i], b, at, ere);
        break;
    case O_ALT:
        for(i = 0; i < o->kids; i++) {
            if(i) at += (size_t)snprintf(b + at, 64, ere ? "|" : "\\|");
            at = o_str(o->kid[i], b, at, ere);
        }
        break;
    case O_REP:
        at = o_str(o->kid[0], b, at, ere);
        if(!o->min && o->max < 0) b[at++] = '*';
        else if(o->max < 0) at += (size_t)snprintf(b + at, 64, ere ? "{%d,}" : "\\{%d,\\}", o->min);
        else if(o->min == o->max) at += (size_t)snprintf(b + at, 64, ere ? "{%d}" : "\\{%d\\}", o->min);
        else at += (size_t)snprintf(b + at, 64, ere ? "{%d,%d}" : "\\{%d,%d\\}", o->min, o->max);
        break;
    }
    b[at] = 0;
    return at;
}

static int o_m(int n, int i, int e);

static int o_cat(int n, int kid, int i, int e) {
    int k;
    if(kid == on[n].kids) return i == e;
    for(k = e; k >= i; k--)
        if(o_m(on[n].kid[kid], i, k) && o_cat(n, kid + 1, k, e)) return 1;
    return 0;
}

static int o_rep(int n, int done, int i, int e) {
    int k, x = on[n].kid[0];
    if(i == e && done >= on[n].min) return 1;
    if(on[n].max >= 0 && done >= on[n].max) return 0;
    for(k = e; k >= i; k--) {
        if(k == i && done >= on[n].min) continue;
        if(o_m(x, i, k) && o_rep(n, done + 1, k, e)) return 1;
    }
    return 0;
}

/* does node n match exactly [i, e)? */
static int o_m(int n, int i, int e) {
    int r = 0, k;
    if(memo[n][i][e] >= 0) return memo[n][i][e];
    switch(on[n].type) {
    case O_EMPTY: r = i == e; break;
    case O_CHAR: r = e == i + 1 && otext[i] == on[n].c; break;
    case O_ANY: r = e == i + 1; break;
    case O_NOTA: r = e == i + 1 && otext[i] != 'a'; break;
    case O_AB: r = e == i + 1 && (otext[i] == 'a' || otext[i] == 'b'); break;
    case O_BOL: r = i == e && i == 0; break;
    case O_EOL: r = i == e && i == olen; break;
    case O_GROUP: r = o_m(on[n].kid[0], i, e); break;
    case O_CAT: r = o_cat(n, 0, i, e); break;
    case O_ALT:
        for(k = 0; k < on[n].kids && !r; k++) r = o_m(on[n].kid[k], i, e);
        break;
    case O_REP: r = o_rep(n, 0, i, e); break;
    }
    memo[n][i][e] = (signed char)r;
    return r;
}

static void o_groups_clear(int n) {
    int k;
    if(on[n].type == O_GROUP) ocap[on[n].group][0] = ocap[on[n].group][1] = -1;
    for(k = 0; k < on[n].kids; k++) o_groups_clear(on[n].kid[k]);
}

/* POSIX's rule: divide [i, e) from the left, each part the longest that leaves a match for the rest */
static void o_cap(int n, int i, int e) {
    const struct onode *o = &on[n];
    int k, p, q;
    switch(o->type) {
    case O_GROUP:
        o_groups_clear(o->kid[0]);
        ocap[o->group][0] = i;
        ocap[o->group][1] = e;
        o_cap(o->kid[0], i, e);
        break;
    case O_CAT:
        p = i;
        for(k = 0; k < o->kids; k++) {
            for(q = e; q >= p; q--)
                if(o_m(o->kid[k], p, q) && o_cat(n, k + 1, q, e)) break;
            o_cap(o->kid[k], p, q);
            p = q;
        }
        break;
    case O_ALT:
        for(k = 0; k < o->kids; k++) {
            if(o_m(o->kid[k], i, e)) {
                o_cap(o->kid[k], i, e);
                return;
            }
        }
        break;
    case O_REP: {
        int done = 0, ls = -1, le = -1;
        p = i;
        for(;;) {
            if(p == e && done >= o->min) break;
            if(o->max >= 0 && done >= o->max) break;
            for(q = e; q >= p; q--) {
                if(q == p && done >= o->min) continue;
                if(o_m(o->kid[0], p, q) && o_rep(n, done + 1, q, e)) break;
            }
            if(q < p) break;
            ls = p;
            le = q;
            p = q;
            done++;
        }
        if(!done && o->max && i == e && o_m(o->kid[0], i, i)) ls = le = i;
        if(ls >= 0) o_cap(o->kid[0], ls, le);
        break;
    }
    }
}

static void o_solve(int root, char *want, size_t size) {
    int s, e, so[MAXSUB], eo[MAXSUB], g;
    memset(memo, -1, sizeof(memo));
    for(s = 0; s <= olen; s++) {
        for(e = olen; e >= s; e--) {
            if(o_m(root, s, e)) {
                for(g = 0; g < MAXSUB; g++) ocap[g][0] = ocap[g][1] = -1;
                o_cap(root, s, e);
                ocap[0][0] = s;
                ocap[0][1] = e;
                for(g = 0; g < MAXSUB; g++) {
                    so[g] = ocap[g][0];
                    eo[g] = ocap[g][1];
                }
                show(want, size, 0, so, eo, MAXSUB);
                return;
            }
        }
    }
    snprintf(want, size, "NOMATCH");
}

static void random_cases(int rounds) {
    char pat[2048], bre[2048], text[OLEN + 1], want[512], got[512], host[512], whole[64];
    int i, k, hostbad = 0;
    for(i = 0; i < rounds; i++) {
        int root, anchors = (int)rnd(3) == 0, n;
        onodes = 0;
        ogroups = 0;
        root = o_gen(0, 10 + (int)rnd(10), anchors);
        ogroups = 0;
        o_number(root);
        if(ogroups >= MAXSUB) continue;
        o_str(root, pat, 0, 1);
        if(strlen(pat) > 300) continue;
        olen = (int)rnd(OLEN + 1);
        for(k = 0; k < olen; k++) text[k] = (char)('a' + rnd(4));
        text[olen] = 0;
        otext = text;
        o_solve(root, want, sizeof(want));
        run_surf(pat, S_EXTENDED, text, 0, got, sizeof(got));
        checks++;
        if(strcmp(got, want)) fail("oracle", pat, text, got, want);
        if(!anchors) {                          /* in basic syntax ^ and $ depend on where they are */
            o_str(root, bre, 0, 0);
            run_surf(bre, 0, text, 0, got, sizeof(got));
            checks++;
            if(strcmp(got, want)) fail("oracle, basic", bre, text, got, want);
        }
        /* the whole match, and that there is one, against the host */
        run_host(pat, S_EXTENDED, text, 0, host, sizeof(host));
        n = (int)(strchr(want, ')') ? strchr(want, ')') - want + 1 : 0);
        snprintf(whole, sizeof(whole), "%.*s", n ? n : 7, want);
        if(host[0] != 'C' && strncmp(host, whole, strlen(whole))) {     /* C: some hosts refuse what is empty, ( ) or a|| */
            hostbad++;
            if(hostbad < 4 || getenv("REGEX_VERBOSE")) printf("host differs: /%s/ on [%s]: host %s, here %s\n", pat, text, host, want);
        }
        host_runs++;
    }
    /* glibc's whole matches are reliable, so a difference from them is ours; the BSDs' are wrong about $ inside a group */
#ifdef __GLIBC__
    checks++;
    if(hostbad) {
        char g[16];
        snprintf(g, sizeof(g), "%d", hostbad);
        fail("whole match against the host", "", "", g, "0");
    }
#endif
    printf("test_regex: %d random patterns: %d whole matches differ from the host's\n", rounds, hostbad);
}

/* is p where a character starts or ends in s? The characters counted from the start, on their own, not by the library's code */
static int at_boundary(const char *s, int n, int p) {
    int i = 0;
    while(i < p && i < n) {
        const unsigned char *u = (const unsigned char *)s + i;
        int k = 1;
        if(u[0] >= 0xC2 && u[0] <= 0xDF && i + 1 < n && (u[1] & 0xC0) == 0x80) k = 2;
        else if(u[0] >= 0xE0 && u[0] <= 0xEF && i + 2 < n && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 &&
                !(u[0] == 0xE0 && u[1] < 0xA0) && !(u[0] == 0xED && u[1] > 0x9F)) k = 3;
        else if(u[0] >= 0xF0 && u[0] <= 0xF4 && i + 3 < n && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80 &&
                !(u[0] == 0xF0 && u[1] < 0x90) && !(u[0] == 0xF4 && u[1] > 0x8F)) k = 4;
        i += k;
    }
    return i == p;
}

static const char *printable(const char *s, char *b, size_t size) {
    size_t n = 0;
    for(; *s && n + 5 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if(c >= 0x20 && c < 0x7f) b[n++] = (char)c;
        else n += (size_t)snprintf(b + n, size - n, "\\x%02x", c);
    }
    b[n] = 0;
    return b;
}

/* Patterns of nothing in particular, valid or not, on text with newlines, bad bytes and half sequences: on the Mac this runs
   under the sanitizers. Whatever it compiles to, offsets stay in the string, in order, and on character boundaries. */
static void fuzz(int rounds) {
    static const char pieces[] = "ab()[]{}|*+?.^$\\-:,=0129<>wWsSbB`'";
    static const char *const bytes[] = { "\xc3\xa9", "\xe2\x82\xac", "\xff", "\xc3", "\x80", "[[:", ":]]", "[.", "=]", "\\(", "\\)", "\\{", "\\}",
                                         "\\|", "\\1", "\\2" };
    static const char *const text_parts[] = { "a", "b", "c", "\n", "\xc3\xa9", "\xff", "\xe2\x82", " ", "_", "\xc3" };
    char pat[64], text[64];
    int i, k, compiled = 0;
    for(i = 0; i < rounds; i++) {
        int n = (int)rnd(20), flags = ((rnd(2) ? S_EXTENDED : 0) | (rnd(4) ? 0 : S_ICASE) | (rnd(4) ? 0 : S_NEWLINE) |
                                       (rnd(8) ? 0 : S_NOSUB));
        size_t at = 0;
        surf_regex_t re;
        surf_regmatch_t m[MAXSUB];
        pat[0] = 0;
        for(k = 0; k < n; k++) {
            if(rnd(5)) pat[at++] = pieces[rnd(sizeof(pieces) - 1)];
            else at += (size_t)snprintf(pat + at, sizeof(pat) - at, "%s", bytes[rnd(sizeof(bytes) / sizeof(bytes[0]))]);
            pat[at] = 0;
        }
        checks++;
        if(surf_regcomp(&re, pat, flags)) continue;
        compiled++;
        for(k = 0; k < 3; k++) {
            int parts = (int)rnd(12), j, rc, len, ef;
            size_t tl = 0;
            text[0] = 0;
            for(j = 0; j < parts; j++)
                tl += (size_t)snprintf(text + tl, sizeof(text) - tl, "%s", text_parts[rnd(sizeof(text_parts) / sizeof(text_parts[0]))]);
            len = (int)tl;
            memset(m, 0x7f, sizeof(m));
            ef = rnd(4) ? 0 : (int)(1 + rnd(3));
            rc = surf_regexec(&re, text, MAXSUB, m, ef);
            if(getenv("REGEX_FUZZ_HOST") && !(flags & S_NOSUB)) {       /* by hand: where the two syntaxes' readings differ */
                char hs[256];
                run_host(pat, flags, text, ef, hs, sizeof(hs));
                if(hs[0] != 'C' && (rc == S_NOMATCH ? strcmp(hs, "NOMATCH") :
                                    rc || atoi(hs + 1) != m[0].rm_so || atoi(strchr(hs, ',') ? strchr(hs, ',') + 1 : hs) != m[0].rm_eo)) {
                    char a[256], b[256];
                    printf("fuzz: %c%c /%s/ on [%s] %d: host %s, here %d (%d,%d)\n", flags & S_EXTENDED ? 'E' : 'B',
                           flags & S_NEWLINE ? 'N' : '-', printable(pat, a, sizeof(a)), printable(text, b, sizeof(b)), ef, hs, rc,
                           m[0].rm_so, m[0].rm_eo);
                }
            }
            if(rc == S_NOMATCH || rc == S_ESTEPS) continue;
            if(rc) {
                fail("fuzz", pat, text, "an error", "an answer");
                continue;
            }
            if(flags & S_NOSUB) continue;
            for(j = 0; j < MAXSUB; j++) {
                int so = m[j].rm_so, eo = m[j].rm_eo;
                if(j > (int)re.re_nsub ? so != -1 || eo != -1 : so == -1 ? eo != -1 :
                   so < m[0].rm_so || eo > m[0].rm_eo || so > eo || eo > len || !at_boundary(text, len, so) || !at_boundary(text, len, eo))
                    fail("fuzz offsets", pat, text, "bad", "in the string");
            }
        }
        surf_regfree(&re);
    }
    if(compiled < rounds / 20) fail("fuzz", "", "", "few patterns", "more");
}

/* what makes a backtracking engine take years takes a moment here, subexpressions too */
static void long_case(const char *pat, int flags, const char *fill, size_t n, const char *tail, int wantrc) {
    surf_regex_t re;
    surf_regmatch_t m[6];
    size_t k = strlen(fill), t = strlen(tail), i;
    char *s = malloc(n * k + t + 1);
    clock_t c0 = clock();
    double ms;
    int rc;
    for(i = 0; i < n; i++) memcpy(s + i * k, fill, k);
    memcpy(s + n * k, tail, t + 1);
    checks++;
    if(surf_regcomp(&re, pat, flags)) {
        fail("compile", pat, "long", "error", "0");
        free(s);
        return;
    }
    rc = surf_regexec(&re, s, 6, m, 0);
    if(rc != wantrc) {
        char g[16], w[16];
        snprintf(g, sizeof(g), "%d", rc);
        snprintf(w, sizeof(w), "%d", wantrc);
        fail("long text", pat, "...", g, w);
    }
    surf_regfree(&re);
    free(s);
    ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    if(getenv("REGEX_VERBOSE")) printf("%.1f ms: /%s/ on %zu bytes\n", ms, pat, n * k + t);
    /* a very wide margin: what this guards against takes centuries, and this may run under an emulator */
    if(ms > 120000) fail("time", pat, "long", "too slow", "fast");
}

/* CPU milliseconds of one regexec(), and its status */
static double one_run(const char *pat, int flags, const char *text, size_t nmatch, int want, int repeat) {
    surf_regex_t re;
    surf_regmatch_t m[3];
    clock_t c0;
    size_t len = strlen(text);
    int i, rc = 0;
    if(surf_regcomp(&re, pat, flags)) {
        fail("compile", pat, "timing", "error", "0");
        return 0;
    }
    c0 = clock();
    for(i = 0; i < repeat; i++) {               /* the length given, or the time would be strlen()'s */
        m[0].rm_so = 0;
        m[0].rm_eo = (int)len;
        rc = surf_regexec(&re, text, nmatch, m, S_STARTEND);
    }
    {
        double ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
        surf_regfree(&re);
        checks++;
        if(rc != want) {
            char g[16], w[16];
            snprintf(g, sizeof(g), "%d", rc);
            snprintf(w, sizeof(w), "%d", want);
            fail("timing's status", pat, "...", g, w);
        }
        if(getenv("REGEX_VERBOSE")) printf("%.1f ms: /%.60s/\n", ms, pat);
        return ms;
    }
}

static char *repeated(const char *fill, size_t times, const char *tail) {
    size_t k = strlen(fill), t = strlen(tail), i;
    char *s = malloc(times * k + t + 1);
    for(i = 0; i < times; i++) memcpy(s + i * k, fill, k);
    memcpy(s + times * k, tail, t + 1);
    return s;
}

/* The limits hold in time, not only in counts. Nothing here depends on the machine's speed: each run is measured against a run
   that must cost the same order, in CPU time. */
static void time_bounds(void) {
    char *s, *p;
    double plain, ms;
    int i, n;
    size_t k = 40000;
    /* REG_ESTEPS after its 20,000,000 steps, the yardstick */
    s = malloc(64);
    memset(s, 'a', 50);
    strcpy(s + 50, "baaac");
    plain = one_run("\\(a\\|aa\\)*b\\1c", 0, s, 2, S_ESTEPS, 1);
    free(s);
    if(plain < 1) plain = 1;
    /* a comparison that fails costs what it reads: \1 is 40 KB here, and it is tried at every place */
    s = repeated("a", k, "b");
    p = repeated("a", 2 * k, "");
    {
        char *text = malloc(3 * k + 2);
        strcpy(text, s);
        strcat(text, p);
        ms = one_run("\\(a*b\\).*\\1", 0, text, 2, S_ESTEPS, 1);
        checks++;
        if(ms > 60 * plain) {
            char g[64];
            snprintf(g, sizeof(g), "%.0f ms against %.0f", ms, plain);
            fail("a failed back-reference is charged", "\\(a*b\\).*\\1", "...", g, "the same order");
        }
        free(text);
    }
    free(s);
    free(p);
    /* clearing 1000 groups is 1000 steps' work, and so is each new start */
    p = malloc(4096);
    n = snprintf(p, 4096, "((z");
    for(i = 0; i < 1000; i++) n += snprintf(p + n, (size_t)(4096 - n), "()");
    snprintf(p + n, (size_t)(4096 - n), ")|a|a)*\\2");
    s = repeated("a", 40, "");
    ms = one_run(p, S_EXTENDED, s, 2, S_ESTEPS, 1);
    checks++;
    if(ms > 40 * plain) {
        char g[64];
        snprintf(g, sizeof(g), "%.0f ms against %.0f", ms, plain);
        fail("clearing groups is charged", "((z()...)|a|a)*\\2", "...", g, "the same order");
    }
    free(s);
    free(p);
    /* a bracket of 1000 ranges costs little more than one of 10 */
    {
        double few = 1, many = 0;
        int members;
        for(members = 10; members <= 1000; members *= 100) {
            size_t at = 0;
            p = malloc(16000);
            at = (size_t)snprintf(p, 16000, "([");
            for(i = 0; i < members; i++) {              /* every second character of a stretch of CJK: none touch */
                unsigned c = 0x5000 + 2u * (unsigned)i;
                p[at++] = (char)(0xE0 | c >> 12);
                p[at++] = (char)(0x80 | (c >> 6 & 0x3F));
                p[at++] = (char)(0x80 | (c & 0x3F));
            }
            snprintf(p + at, 16000 - at, "]*){5000}z");
            s = repeated("\xe4\xb8\x80", 700, "");    /* a character above all of them or below: the whole search */
            ms = one_run(p, S_EXTENDED, s, 1, S_NOMATCH, 1);
            if(members == 10) few = ms;
            else many = ms;
            free(s);
            free(p);
        }
        checks++;
        if(many > 12 * (few > 1 ? few : 1)) {
            char g[64];
            snprintf(g, sizeof(g), "%.0f ms against %.0f", many, few);
            fail("a big bracket", "", "...", g, "a logarithm's worth");
        }
    }
    /* ^ at the start: one look, however long the text */
    {
        double shorter, longer;
        s = repeated("x", 1 << 20, "");
        shorter = one_run("^foo", S_EXTENDED, "xxxxxxxx", 1, S_NOMATCH, 2000);
        longer = one_run("^foo", S_EXTENDED, s, 1, S_NOMATCH, 2000);
        checks++;
        if(longer > 20 * (shorter > 5 ? shorter : 5)) {
            char g[64];
            snprintf(g, sizeof(g), "%.0f ms against %.0f", longer, shorter);
            fail("an anchored pattern reads the whole text", "^foo", "...", g, "the same");
        }
        free(s);
    }
}

static void long_text(void) {
    size_t n = 20000;
    long_case("(a*)*b", S_EXTENDED, "a", n, "", S_NOMATCH);
    long_case("(a*)*", S_EXTENDED, "a", n, "", 0);
    long_case("(a|a)*b", S_EXTENDED, "a", n, "", S_NOMATCH);
    long_case("(a|aa)*$", S_EXTENDED, "a", n, "", 0);
    long_case("(x+x+)+y", S_EXTENDED, "x", n, "", S_NOMATCH);
    long_case("(x+x+)+y", S_EXTENDED, "x", n, "y", 0);
    long_case("^(([a-z])+.)+[A-Z]([a-z])+$", S_EXTENDED, "a", n, "!", S_NOMATCH);
    long_case("(.*)(.*)(.*)(.*)(.*)", S_EXTENDED, "ab", n / 4, "", 0);
    long_case("((a|ab|abc)*)(c*)", S_EXTENDED, "abc", n / 4, "", 0);
    long_case("(((((a*)*)*)*)*)*", S_EXTENDED, "a", n / 2, "", 0);
    long_case("(a{1,100}){1,100}", S_EXTENDED, "a", n / 20, "", 0);
    long_case("(a?){30}a{30}", S_EXTENDED, "a", 30, "", 0);
    long_case("\\(a*\\)*b", 0, "a", n, "", S_NOMATCH);
    long_case("x", 0, "a", n * 10, "", S_NOMATCH);
    long_case("foo\\|bar", 0, "ab ", n, "bar", 0);
}

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 30000;
    if(getenv("REGEX_FUZZ_HOST")) setlocale(LC_ALL, "C.UTF-8");
    table();
    printf("test_regex: the table against the host: %d of %d differ (REGEX_VERBOSE=1 says which)\n", host_differs, host_runs);
    errors();
    flags();
    utf8();
    backrefs();
    random_cases(rounds);
    fuzz(rounds);
    long_text();
    time_bounds();
    printf("test_regex: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
