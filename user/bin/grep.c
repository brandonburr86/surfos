/*
SurfOS user program: grep
--------------------
File: grep.c    Date: 10/25/26
--------------------
grep [-i] [-v] [-n] [-c] [-l] [-q] [-F] [-e pattern] pattern [file...]: the lines of the files
(standard input without them) that the pattern matches. The pattern is a basic regular
expression: . any character, * the one before it any number of times, ^ and $ the start and
the end of the line, [abc] [a-z] one of those, [^abc] one not of them, \ the next character
as itself; -F takes it as a fixed string. -i ignores case, -v gives the lines that do not
match, -n puts each line's number before it, -c gives how many lines there are, -l the names
of the files that have one, -q nothing. With more than one file each line comes after its
file's name and a colon. Status 0 when a line was found, 1 when none was, 2 for a mistake
(a file that cannot be read; with -q a line found counts more).
*/

#include <ctype.h>
#include "lineutil.h"

static int icase, fixed;

static int same(char a, char b) {
    return icase ? tolower((unsigned char)a) == tolower((unsigned char)b) : a == b;
}

/* the length of the pattern's piece at re: a character, ., \c or [...] (an [ never closed is itself) */
static int atom_len(const char *re) {
    const char *p;
    if(!*re) return 0;
    if(*re == '\\' && re[1]) return 2;
    if(*re != '[') return 1;
    p = re + 1;
    if(*p == '^') p++;
    if(*p == ']') p++;
    while(*p && *p != ']') p++;
    return *p ? (int)(p - re + 1) : 1;
}

/* does the piece at re (len bytes) take the character c? */
static int takes(const char *re, int len, char c) {
    const char *p, *end;
    int neg = 0, hit = 0;
    if(len == 2 && re[0] == '\\') return same(re[1], c);
    if(len == 1) return re[0] == '.' || same(re[0], c);
    p = re + 1;
    end = re + len - 1;
    if(*p == '^') {
        neg = 1;
        p++;
    }
    for(; p < end; p++) {
        if(p + 2 < end && p[1] == '-') {        /* a range */
            unsigned char lo = (unsigned char)p[0], hi = (unsigned char)p[2], u = (unsigned char)c;
            if((u >= lo && u <= hi) || (icase && ((tolower(u) >= lo && tolower(u) <= hi) ||
                                                  (toupper(u) >= lo && toupper(u) <= hi))))
                hit = 1;
            p += 2;
        } else if(same(*p, c)) {
            hit = 1;
        }
    }
    return hit != neg;
}

static int here(const char *re, const char *text) {
    int len;
    if(!*re) return 1;
    if(re[0] == '$' && !re[1]) return !*text;
    len = atom_len(re);
    if(re[len] == '*') {                        /* the piece any number of times, the most first */
        const char *t = text;
        while(*t && takes(re, len, *t)) t++;
        for(;; t--) {
            if(here(re + len + 1, t)) return 1;
            if(t == text) return 0;
        }
    }
    return *text && takes(re, len, *text) && here(re + len, text + 1);
}

static int matches(const char *re, const char *line) {
    if(fixed) {
        size_t n = strlen(re);
        for(; *line; line++) {
            size_t k;
            for(k = 0; k < n && line[k] && same(line[k], re[k]); k++);
            if(k == n) return 1;
        }
        return !n;
    }
    if(re[0] == '^') return here(re + 1, line);
    do {
        if(here(re, line)) return 1;
    } while(*line++);
    return 0;
}

int main(int argc, char **argv) {
    const char *pattern = NULL;
    char *line = NULL;
    size_t cap = 0;
    int invert = 0, numbers = 0, counting = 0, names = 0, quiet = 0, i = 1, found = 0, bad = 0, files;
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
            else if(*o == 'F') fixed = 1;
            else if(*o == 'e' && !o[1] && i + 1 < argc) {
                pattern = argv[++i];
                break;
            } else {
                fprintf(stderr, "usage: grep [-ivnclqF] [-e pattern] pattern [file...]\n");
                return 2;
            }
        }
    }
    if(!pattern) {
        if(i == argc) {
            fprintf(stderr, "usage: grep [-ivnclqF] [-e pattern] pattern [file...]\n");
            return 2;
        }
        pattern = argv[i++];
    }
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
            lineno++;
            if(matches(pattern, line) == invert) continue;
            found = 1;
            count++;
            if(quiet) break;
            if(names) break;
            if(counting) continue;
            if(files > 1) printf("%s:", name);
            if(numbers) printf("%ld:", lineno);
            printf("%s\n", line);
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
    if(bad && !(quiet && found)) return 2;
    return found ? 0 : 1;
}
