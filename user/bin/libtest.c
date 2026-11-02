/*
SurfOS user program: libtest
--------------------
File: libtest.c  Date: 11/01/26
--------------------
Checks of what libsurf gained for the command-line suite, inside SurfOS, for the smoke test
(tools/qemu-run.py) and for trying them by hand. The depth is on the host (tests/host/test_regex.c,
test_strtod.c, test_libm.c, test_getopt.c); these show that it all links without libgcc, runs on x87
at either optimisation level, and that what the host cannot test works: the commands.
Each mode ends with "LIBTEST <mode> PASS", or says what failed and exits with status 1.

libtest regex    leftmost longest, subexpressions, basic and extended, UTF-8, back-references,
                 an error's message
libtest numbers  strtod and atof to the bit, inf and nan, ERANGE; sscanf's %f %lf %g
libtest math     the fingerprint of user/bin/libm_grid.h, the same bits as on the host; math.h's
                 macros
libtest getopt   an option, a cluster, an argument, --, an unknown one
libtest system   the exit status; a kernel command; a pipeline; a command too long for one
                 argument and one too long for a pipe; the environment reaching the command
libtest popen    reading and writing; two at once; fflush(NULL) losing nothing that was read
                 ahead; a reader that stops early; no descriptor left behind in 40 rounds
libtest closed   the same with descriptor 0, then 1, closed first: a command must not be given the far end
                 of its own pipe as that descriptor
libtest order    this program's output comes out before its commands' (run it through a pipe)
libtest all      all of them but order and closed
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <regex.h>
#include <math.h>
#include <surf.h>
#include "libm_grid.h"

static int bad;

static void check(int ok, const char *what) {
    if(ok) return;
    bad = 1;
    printf("libtest: failed: %s\n", what);
}

static int finish(const char *mode) {
    if(bad) {
        printf("LIBTEST %s FAIL\n", mode);
        return 1;
    }
    printf("LIBTEST %s PASS\n", mode);
    return 0;
}

/**** regex ****/

/* does pattern find [so, eo) in text, and group g [gso, geo)? */
static int finds(const char *pattern, int flags, const char *text, int so, int eo, int g, int gso, int geo) {
    regex_t re;
    regmatch_t m[10];
    int rc = regcomp(&re, pattern, flags), ok;
    if(rc) return 0;
    rc = regexec(&re, text, 10, m, 0);
    if(so < 0) ok = rc == REG_NOMATCH;
    else ok = !rc && m[0].rm_so == so && m[0].rm_eo == eo && (!g || (m[g].rm_so == gso && m[g].rm_eo == geo));
    regfree(&re);
    return ok;
}

static int mode_regex(void) {
    regex_t re;
    char msg[64];
    int rc;
    check(finds("hello", 0, "say hello", 4, 9, 0, 0, 0), "a plain word");
    check(finds("a|ab|abc", REG_EXTENDED, "xabcd", 1, 4, 0, 0, 0), "the longest of the alternatives");
    check(finds("(a|ab)(c|bcd)(d*)", REG_EXTENDED, "abcd", 0, 4, 1, 0, 2), "a subexpression takes the longest it can");
    check(finds("\\(foo\\).*\\(bar\\)", 0, "x foo y bar", 2, 11, 2, 8, 11), "basic syntax");
    check(finds("[[:digit:]]+", REG_EXTENDED, "ab123c", 2, 5, 0, 0, 0), "a class");
    check(finds("a{2,3}", REG_EXTENDED, "caaaa", 1, 4, 0, 0, 0), "an interval");
    check(finds("^.$", REG_EXTENDED, "\xe2\x82\xac", 0, 3, 0, 0, 0), "a UTF-8 character is one");
    check(finds("\\(.\\)\\1", 0, "abccd", 2, 4, 1, 2, 3), "a back-reference");
    check(finds("HELLO", REG_ICASE, "hello", 0, 5, 0, 0, 0), "case");
    check(finds("x", 0, "abc", -1, 0, 0, 0, 0), "no match");
    rc = regcomp(&re, "a(", REG_EXTENDED);
    check(rc == REG_EPAREN, "an error");
    regerror(rc, &re, msg, sizeof(msg));
    check(!strcmp(msg, "( without its ) or ) without its ("), "its message");
    /* (x+x+)+y: years for a backtracking engine */
    {
        static char text[4001];
        memset(text, 'x', 4000);
        check(finds("(x+x+)+y", REG_EXTENDED, text, -1, 0, 0, 0, 0), "no catastrophe");
    }
    return finish("regex");
}

/**** numbers ****/

static int same_bits(double d, unsigned long long want) {
    union { double d; unsigned long long u; } v;
    v.d = d;
    return v.u == want;
}

static int mode_numbers(void) {
    char *end;
    double d = 0, e = 0;
    float f = 0;
    char word[8];
    check(same_bits(strtod("0.1", NULL), 0x3FB999999999999AULL), "0.1");
    check(same_bits(strtod("1e23", NULL), 0x44B52D02C7E14AF6ULL), "1e23");
    check(same_bits(strtod("2.2250738585072011e-308", NULL), 0x000FFFFFFFFFFFFFULL), "a subnormal");
    check(same_bits(strtod("5e-324", NULL), 1), "the smallest number");
    check(same_bits(strtod("1.7976931348623157e308", NULL), 0x7FEFFFFFFFFFFFFFULL), "the largest");
    check(same_bits(strtod("0x1.8p1", NULL), 0x4008000000000000ULL), "hexadecimal");
    check(same_bits(strtod("-inf", NULL), 0xFFF0000000000000ULL), "inf");
    check(isnan(strtod("nan", NULL)), "nan");
    check(same_bits(atof("2.5 and more"), 0x4004000000000000ULL), "atof");
    errno = 0;
    check(same_bits(strtod("1e999", NULL), 0x7FF0000000000000ULL) && errno == ERANGE, "overflow");
    check(strtod("12abc", &end) == 12 && !strcmp(end, "abc"), "where it stops");
    check(sscanf("1.5 2.25 x", "%f %lf %7s", &f, &d, word) == 3 && f == 1.5f && d == 2.25 && !strcmp(word, "x"), "sscanf");
    check(sscanf("1e2", "%lg", &e) == 1 && e == 100, "sscanf's %lg");
    return finish("numbers");
}

/**** math ****/

static int mode_math(void) {
    unsigned long long sum = libm_grid();
    if(sum != LIBM_GRID_SUM) printf("libtest: the fingerprint is 0x%llX, the host's 0x%llX\n", sum, LIBM_GRID_SUM);
    check(sum == LIBM_GRID_SUM, "the fingerprint");
    check(isinf(HUGE_VAL) && isinf(INFINITY) && isnan(NAN) && isfinite(1.0) && !isfinite(NAN) && !isfinite(INFINITY), "the macros");
    check(M_PI > 3.14 && M_PI < 3.15 && M_E > 2.71 && M_E < 2.72, "the constants");
    check(fabs(sin(M_PI / 6) - 0.5) < 1e-15 && pow(2, 10) == 1024 && sqrt(2) * sqrt(2) > 1.99 && floor(-1.5) == -2, "a few values");
    return finish("math");
}

/**** getopt ****/

static int mode_getopt(void) {
    char *argv[] = { "prog", "-ab", "-c", "value", "-x", "--", "-a", "file", NULL };
    char seen[8];
    int n = 0, c;
    const char *arg = NULL;
    opterr = 0;
    optind = 1;
    while((c = getopt(8, argv, "abc:")) != -1) {
        seen[n++] = (char)c;
        if(c == 'c') arg = optarg;
    }
    seen[n] = 0;
    check(!strcmp(seen, "abc?"), "the letters");
    check(arg && !strcmp(arg, "value"), "the argument");
    check(optopt == 'x', "the unknown one");
    check(optind == 6, "where it stopped");
    return finish("getopt");
}

/**** commands ****/

#define PADDING 400                     /* more than one argument may hold */

static int mode_system(void) {
    char *cmd = malloc(10000);
    check(system(NULL) == 1, "there is a shell");
    check(system("exit 7") == 7, "an exit status");
    check(system("true") == 0, "success");
    check(system("nosuchcommand 2> /dev/null") == 127, "a command that is nowhere");
    check(system("seq 3 | grep -c 2 > /dev/null") == 0, "a pipeline");
    check(system("uptime > /dev/null") == 0, "a kernel command");
    setenv("LIBTEST_WORD", "seen by a child", 1);
    check(system("echo $LIBTEST_WORD") == 0, "the environment");
    /* too long for an argument: through a descriptor, as a surfsh starts a surfsh */
    strcpy(cmd, "true");
    memset(cmd + 4, ' ', PADDING);
    strcpy(cmd + 4 + PADDING, "; exit 9");
    check(system(cmd) == 9, "a long command");
    /* too long for a pipe: refused, and nothing has run */
    memset(cmd, ' ', 9000);
    strcpy(cmd + 9000, "exit 5");
    errno = 0;
    check(system(cmd) == -1 && errno == E2BIG, "a command too long");
    free(cmd);
    return finish("system");
}

static int mode_popen(void) {
    char line[64];
    FILE *a, *b;
    long sum = 0;
    int i;
    a = popen("seq 1 1000", "r");
    check(a != NULL, "popen");
    if(a) {
        while(fgets(line, sizeof(line), a)) sum += atol(line);
        check(sum == 500500, "what it read");
        check(pclose(a) == 0, "pclose");
    }
    a = popen("exit 3", "r");
    check(a && !fgets(line, sizeof(line), a), "nothing to read");
    if(a) check(pclose(a) == 3, "pclose's status");
    /* writing, and reading the result back */
    a = popen("tr a-z A-Z > /hda1/LIBTEST.TXT", "w");
    check(a != NULL, "popen for writing");
    if(a) {
        fputs("hello\n", a);
        check(pclose(a) == 0, "its pclose");
        b = fopen("/hda1/LIBTEST.TXT", "r");
        check(b && fgets(line, sizeof(line), b) && !strcmp(line, "HELLO\n"), "what the command wrote");
        if(b) fclose(b);
        remove("/hda1/LIBTEST.TXT");
    }
    /* two at once, each ending by itself, and fflush(NULL) leaving what was read ahead */
    a = popen("seq 1 100", "r");
    b = popen("seq 101 200", "r");
    check(a && b, "two at once");
    if(a && b) {
        sum = 0;
        for(i = 0; i < 100; i++) {
            check(fgets(line, sizeof(line), a) != NULL, "the first's line");
            sum += atol(line);
            fflush(NULL);
            check(fgets(line, sizeof(line), b) != NULL, "the second's line");
            sum += atol(line);
        }
        check(sum == 20100, "both whole");
        check(pclose(b) == 0 && pclose(a) == 0, "both ends");
    }
    /* and so is what it keeps of a terminal's: lseek() does not refuse there, as it does on a pipe, but the bytes are gone all the same */
    {
        char *const argv[] = { "seq", "1", "50", NULL };
        int master = openpty(), pid = master < 0 ? -1 : spawnpty("/initrd/bin/seq", argv, master);
        check(pid >= 0, "a command on a pty");
        if(pid >= 0) {
            msleep(300);                        /* all its lines in the pty, so that one read takes many */
            a = fdopen(master, "r");
            check(a != NULL, "a stream on the pty");
            if(a) {
                check(fgets(line, sizeof(line), a) && atol(line) == 1, "the pty's first line");
                fflush(NULL);
                check(fgets(line, sizeof(line), a) && atol(line) == 2, "the pty's next line");
                fclose(a);
            }
            wait(pid, NULL);
        }
    }
    /* a reader that stops early: the command ends, nobody waits for ever */
    a = popen("seq 1 100000", "r");
    if(a) {
        check(fgets(line, sizeof(line), a) != NULL, "a first line");
        check(pclose(a) >= 0, "the end of a command nobody reads");
    }
    check(pclose(stdout) == -1 && errno == EINVAL, "a stream popen did not make");
    check(pclose(NULL) == -1 && errno == EINVAL, "no stream at all");
    /* what fflush() keeps of a pipe's input is safe from setvbuf() */
    a = popen("seq 1 100", "r");
    if(a) {
        check(fgets(line, sizeof(line), a) && !strcmp(line, "1\n"), "the first line");
        fflush(NULL);
        check(setvbuf(a, NULL, _IONBF, 0) != 0, "setvbuf refused with input in the buffer");
        check(fgets(line, sizeof(line), a) && !strcmp(line, "2\n"), "the next line");
        while(fgets(line, sizeof(line), a));
        check(pclose(a) == 0, "the end of it");
    }
    check(!popen("true", "x") && errno == EINVAL, "a bad mode");
    for(i = 0; i < 40; i++) {
        a = popen("true", "r");
        check(a && pclose(a) == 0, "again and again");
    }
    return finish("popen");
}

/* one that reads its standard input would otherwise wait for its own output for ever */
static int mode_closed(void) {
    char line[64];
    FILE *a;
    close(0);
    a = popen("cat; echo done", "r");
    check(a != NULL, "popen with 0 closed");
    if(a) {
        check(fgets(line, sizeof(line), a) && !strcmp(line, "done\n"), "the command ends, with no input");
        check(pclose(a) == 0, "its pclose");
    }
    check(system("true") == 0, "system with 0 closed");
    fflush(stdout);
    close(1);
    a = popen("cat > /hda1/LIBTEST.TXT", "w");
    if(!a) dprintf(2, "libtest: popen with 1 closed failed\n");
    if(a) {
        fputs("hello\n", a);
        if(pclose(a) != 0) dprintf(2, "libtest: pclose with 1 closed failed\n");
    }
    a = fopen("/hda1/LIBTEST.TXT", "r");
    bad |= !a || !fgets(line, sizeof(line), a) || strcmp(line, "hello\n");
    if(a) fclose(a);
    remove("/hda1/LIBTEST.TXT");
    dprintf(2, "LIBTEST closed %s\n", bad ? "FAIL" : "PASS");
    return bad;
}

static int mode_order(void) {
    printf("first\n");
    system("echo second");
    printf("third\n");
    return 0;
}

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "";
    if(!strcmp(m, "regex")) return mode_regex();
    if(!strcmp(m, "numbers")) return mode_numbers();
    if(!strcmp(m, "math")) return mode_math();
    if(!strcmp(m, "getopt")) return mode_getopt();
    if(!strcmp(m, "system")) return mode_system();
    if(!strcmp(m, "popen")) return mode_popen();
    if(!strcmp(m, "closed")) return mode_closed();
    if(!strcmp(m, "order")) return mode_order();
    if(!strcmp(m, "all")) {
        int r = mode_regex() | mode_numbers() | mode_math() | mode_getopt() | mode_system() | mode_popen();
        return r;
    }
    fprintf(stderr, "usage: libtest regex | numbers | math | getopt | system | popen | closed | order | all\n");
    return 2;
}
