/*
SurfOS user program: nl
--------------------
File: nl.c      Date: 11/01/26
--------------------
nl [-b a|t|n|pBRE] [-w width] [-s sep] [-v first] [-i step] [file...]: the lines of the files
(standard input without them, - for it) with numbers before them, counting on from file to
file. -b says which lines get one: t those with text (the default), a all, n none, pBRE those
a basic regular expression matches. A line not numbered is indented as far as the numbers. -w
is the number's width (6), -s what comes after it (a TAB), -v the first number (1), -i the step
(1). The numbers are at the right of their width. A last line without a newline gets one.

Other systems' pages of \: lines are not known here: those are lines like any other.

-b p takes the C library's regcomp(), which is built in only where libsurf has <regex.h>; a build
without it says so. Otherwise lines go through a byte at a time and may be any length.
*/

#include "../diff/cli.h"

#if defined(__has_include)
#if __has_include(<regex.h>)
#include <regex.h>
#define NL_REGEX 1
#endif
#endif

static int style = 't', width = 6;
static const char *sep = "\t";
static size_t sep_cells;                /* the columns sep takes */
static long long number = 1, step = 1;
static int spent;
#ifdef NL_REGEX
static regex_t pattern;
#endif

static void put_prefix(int numbered) {
    if(numbered) {
        if(spent) {                             /* the number before this was the last there is */
            cli_flush("nl");
            fprintf(stderr, "nl: the line number goes past what 64 bits hold\n");
            exit(2);
        }
        printf("%*lld%s", width, number, sep);
        if(step > 0 ? number > LLONG_MAX - step : number < LLONG_MIN - step) spent = 1;
        else number += step;
    } else printf("%*s", (int)((size_t)width + sep_cells), "");
}

/* -b p: a line whole, to match it */
static void matched_lines(FILE *f) {
#ifdef NL_REGEX
    char *line = NULL;
    size_t cap = 0;
    long n;
    int nl;
    while((n = lu_getline(f, &line, &cap, &nl)) >= 0) {
        put_prefix(regexec(&pattern, line, 0, NULL, 0) == 0);
        fwrite(line, 1, (size_t)n, stdout);
        putchar('\n');
    }
    free(line);
#else
    (void)f;
#endif
}

static void plain_lines(FILE *f) {
    int c;
    while((c = getc(f)) != EOF) {
        put_prefix(style == 'a' || (style == 't' && c != '\n'));
        while(c != EOF && c != '\n') {
            putchar(c);
            c = getc(f);
        }
        putchar('\n');
    }
}

static int usage(void) {
    fprintf(stderr, "usage: nl [-b a|t|n|pBRE] [-w width] [-s sep] [-v first] [-i step] [file...]\n");
    return 2;
}

/* a whole number that may be negative: 0, or -1 */
static int signed_number(const char *s, long long *v) {
    unsigned long long n;
    int minus = *s == '-';
    if(minus || *s == '+') s++;
    if(cli_number(s, &n) < 0 || n > LLONG_MAX) return -1;
    *v = minus ? -(long long)n : (long long)n;
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    size_t k, len;
    lu_prog = "nl";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        char o = argv[i][1];
        const char *arg = argv[i][2] ? argv[i] + 2 : NULL;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if(!strchr("bwsvi", o)) return usage();
        if(!arg) {
            if(++i >= argc) return usage();
            arg = argv[i];
        }
        switch(o) {
        case 'b':
            if(arg[0] == 'p') {
#ifdef NL_REGEX
                if(regcomp(&pattern, arg + 1, REG_NOSUB)) {
                    fprintf(stderr, "nl: %s: not a regular expression\n", arg + 1);
                    return 2;
                }
#else
                fprintf(stderr, "nl: -b p: this build has no regular expressions\n");
                return 2;
#endif
            } else if((arg[0] != 'a' && arg[0] != 't' && arg[0] != 'n') || arg[1]) return usage();
            style = arg[0];
            break;
        case 'w':
            if(cli_int(arg, &width) < 0 || !width) {
                fprintf(stderr, "nl: %s: not a width\n", arg);
                return 2;
            }
            break;
        case 's':
            sep = arg;
            break;
        case 'v':
        case 'i':
            if(signed_number(arg, o == 'v' ? &number : &step) < 0) {
                fprintf(stderr, "nl: %s: not a whole number\n", arg);
                return 2;
            }
            break;
        }
    }
    len = strlen(sep);
    for(k = 0; k < len; k += (size_t)cli_seq((const unsigned char *)sep + k, len - k)) sep_cells++;
    if(sep_cells > (size_t)(INT_MAX - width)) {  /* the indent of a line with no number must fit an int */
        fprintf(stderr, "nl: %d: too wide\n", width);
        return 2;
    }
    do {
        const char *name = i < argc ? argv[i] : "-";
        FILE *f = lu_open(name);
        if(!f) {
            rc = 2;
            continue;
        }
        if(style == 'p') matched_lines(f);
        else plain_lines(f);
        if(ferror(f)) {
            fprintf(stderr, "nl: %s: %s\n", name, strerror(errno));
            rc = 2;
        }
        lu_close(f);
    } while(++i < argc);
    return cli_flush("nl") ? 2 : rc;
}
