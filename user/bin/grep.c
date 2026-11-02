/*
SurfOS user program: grep
--------------------
File: grep.c    Date: 10/25/26
--------------------
grep [-i] [-v] [-n] [-c] [-l] [-q] [-E] [-F] [-e pattern] pattern [file...]: the lines of the files
(standard input without them) that the pattern matches. The pattern is a basic regular expression,
with -E an extended one, and -F takes it as a fixed string; of -E and -F the last one counts. -i ignores
case, -v gives the lines that do not match, -n puts each line's number before it, -c gives how many
lines there are, -l the names of the files that have one, -q nothing. With more than one file each line
comes after its file's name and a colon. Status 0 when a line was found, 1 when none was, 2 for a
mistake (a file that cannot be read; with -q a line found counts more) or a line the search gave up on.
11/01/26 (the command-line suite): the patterns are user/libsurf/regex.c's, all that man regex says: \( \)
and \1, \{m,n\}, [:classes:], a character being a UTF-8 sequence, and no pattern that takes for ever. A \
before those characters used to mean the character itself. A line may hold NUL bytes. An [ never closed is
still itself in a basic pattern (and a mistake with -E, which is new); other mistakes in a pattern are said,
with status 2. A line on which the search gave up (too many steps) neither matches nor fails to match, with
-v too: it is not shown or counted, the others go on, and the status is 2 whatever else was found.
*/

#include <ctype.h>
#include <regex.h>
#include "lineutil.h"

static int icase, fixed, extended;
static int steps_said;
static regex_t compiled;

static int same(char a, char b) {
    return icase ? tolower((unsigned char)a) == tolower((unsigned char)b) : a == b;
}

/* the length of the [ at re, up to its ], as the old grep saw it; 0 when no ] comes */
static size_t bracket_len(const char *re) {
    const char *p = re + 1;
    if(*p == '^') p++;
    if(*p == ']') p++;
    while(*p && *p != ']') p++;
    return *p ? (size_t)(p - re + 1) : 0;
}

/* the pattern with each [ never closed made \[: a new string */
static char *close_brackets(const char *re) {
    char *out = lu_alloc(NULL, 2 * strlen(re) + 1), *q = out;
    while(*re) {
        size_t n = 1;
        if(*re == '\\' && re[1]) n = 2;
        else if(*re == '[') {
            n = bracket_len(re);
            if(!n) {
                *q++ = '\\';
                n = 1;
            }
        }
        memcpy(q, re, n);
        q += n;
        re += n;
    }
    *q = 0;
    return out;
}

static int compile(const char *pattern) {
    char msg[128];
    int flags = REG_NOSUB | (extended ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0);
    int rc = regcomp(&compiled, pattern, flags);
    if(rc == REG_EBRACK && !extended) {
        char *closed = close_brackets(pattern);
        rc = regcomp(&compiled, closed, flags);
        free(closed);
    }
    if(!rc) return 0;
    regerror(rc, &compiled, msg, sizeof(msg));
    fprintf(stderr, "grep: %s: %s\n", pattern, msg);
    return -1;
}

/* does the line, n bytes, match? -1 when the search gave up */
static int matches(const char *pattern, const char *line, size_t n) {
    regmatch_t m[1];
    int rc;
    if(fixed) {
        size_t len = strlen(pattern), i, k;
        for(i = 0; i + len <= n; i++) {
            for(k = 0; k < len && same(line[i + k], pattern[k]); k++);
            if(k == len) return 1;
        }
        return 0;
    }
    m[0].rm_so = 0;
    m[0].rm_eo = (regoff_t)n;
    rc = regexec(&compiled, line, 0, m, REG_STARTEND);
    if(!rc) return 1;
    if(rc == REG_NOMATCH) return 0;
    if(!steps_said) {
        char msg[128];
        regerror(rc, &compiled, msg, sizeof(msg));
        fprintf(stderr, "grep: %s: %s\n", pattern, msg);
        steps_said = 1;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *pattern = NULL;
    char *line = NULL;
    size_t cap = 0;
    int invert = 0, numbers = 0, counting = 0, names = 0, quiet = 0, i = 1, found = 0, bad = 0, undecided = 0, files;
    lu_prog = "grep";
    for(; i < argc && argv[i][0] == '-' && argv[i][1] && !pattern; i++) {
        const char *o;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'i') icase = 1;
            else if(*o == 'v') invert = 1;
            else if(*o == 'n') numbers = 1;
            else if(*o == 'c') counting = 1;
            else if(*o == 'l') names = 1;
            else if(*o == 'q') quiet = 1;
            else if(*o == 'F') {
                fixed = 1;
                extended = 0;
            } else if(*o == 'E') {
                extended = 1;
                fixed = 0;
            } else if(*o == 'e' && !o[1] && i + 1 < argc) {
                pattern = argv[++i];
                break;
            } else {
                fprintf(stderr, "usage: grep [-ivnclqEF] [-e pattern] pattern [file...]\n");
                return 2;
            }
        }
    }
    if(!pattern) {
        if(i == argc) {
            fprintf(stderr, "usage: grep [-ivnclqEF] [-e pattern] pattern [file...]\n");
            return 2;
        }
        pattern = argv[i++];
    }
    if(!fixed && compile(pattern)) return 2;
    files = argc - i;
    for(; i < argc || files == 0; i++) {
        const char *name = files ? argv[i] : "-";
        FILE *f = lu_open(name);
        long n, lineno = 0, count = 0;
        int nl;
        if(!f) {
            bad = 1;
            if(!files) break;
            continue;
        }
        while((n = lu_getline(f, &line, &cap, &nl)) >= 0) {
            int hit = matches(pattern, line, (size_t)n);
            lineno++;
            if(hit < 0) {
                undecided = 1;
                continue;
            }
            if((hit > 0) == invert) continue;
            found = 1;
            count++;
            if(quiet) break;
            if(names) break;
            if(counting) continue;
            if(files > 1) printf("%s:", name);
            if(numbers) printf("%ld:", lineno);
            fwrite(line, 1, (size_t)n, stdout);
            putchar('\n');
        }
        if(ferror(f)) {
            fprintf(stderr, "grep: %s: %s\n", name, strerror(errno));
            bad = 1;
        }
        lu_close(f);
        if(names && count && !quiet) printf("%s\n", files ? name : "(standard input)");
        else if(counting && !quiet) {
            if(files > 1) printf("%s:", name);
            printf("%ld\n", count);
        }
        if(quiet && found) break;
        if(!files) break;
    }
    free(line);
    if(undecided || (bad && !(quiet && found))) return 2;
    return found ? 0 : 1;
}
