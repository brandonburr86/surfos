/*
SurfOS user program: sort
--------------------
File: sort.c    Date: 10/25/26
--------------------
sort [-r] [-n] [-f] [-u] [file...]: the lines of the files (standard input without them) in
order: byte by byte, -f with case left out, -n by the number each begins with (blanks before
it skipped, none counts as 0; lines with the same number byte by byte); -r the other way
round; -u only the first of lines that compare equal. Every line is held in memory.
*/

#include "lineutil.h"

static int reverse, numeric, fold;

static long number(const char *s) {
    while(*s == ' ' || *s == '\t') s++;
    return strtol(s, NULL, 10);
}

static int compare(const char *a, const char *b) {
    int c;
    if(numeric) {
        long x = number(a), y = number(b);
        if(x != y) return x < y ? -1 : 1;
    }
    c = fold ? strcasecmp(a, b) : strcmp(a, b);
    return c;
}

static int by_order(const void *pa, const void *pb) {
    int c = compare(*(char *const *)pa, *(char *const *)pb);
    return reverse ? -c : c;
}

int main(int argc, char **argv) {
    char **lines = NULL, *line = NULL;
    size_t cap = 0, n = 0, room = 0, k;
    int unique = 0, i = 1, files, bad = 0;
    lu_prog = "sort";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'r') reverse = 1;
            else if(*o == 'n') numeric = 1;
            else if(*o == 'f') fold = 1;
            else if(*o == 'u') unique = 1;
            else {
                fprintf(stderr, "usage: sort [-rnfu] [file...]\n");
                return 2;
            }
        }
    }
    files = argc - i;
    for(; i < argc || !files; i++) {
        FILE *f = lu_open(files ? argv[i] : "-");
        int nl;
        if(!f) {
            bad = 1;
            if(!files) break;
            continue;
        }
        while(lu_getline(f, &line, &cap, &nl) >= 0) {
            if(n == room) {
                room = room ? room * 2 : 256;
                lines = lu_alloc(lines, room * sizeof(char *));
            }
            lines[n] = strdup(line);
            if(!lines[n]) lu_alloc(NULL, (size_t)-1);
            n++;
        }
        lu_close(f);
        if(!files) break;
    }
    qsort(lines, n, sizeof(char *), by_order);
    for(k = 0; k < n; k++) {
        if(unique && k && !compare(lines[k - 1], lines[k])) continue;
        printf("%s\n", lines[k]);
    }
    return bad ? 2 : 0;
}
