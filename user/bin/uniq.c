/*
SurfOS user program: uniq
--------------------
File: uniq.c    Date: 10/25/26
--------------------
uniq [-c] [-d] [-u] [-i] [file]: the lines of the file (standard input without one), each run
of equal lines next to each other once; -c with how many there were before each, -d only the
runs of more than one, -u only the lines alone, -i with case left out.
*/

#include "lineutil.h"

static int show_count, only_dup, only_single;

static void put(const char *line, long count) {
    if((only_dup && count < 2) || (only_single && count > 1)) return;
    if(show_count) printf("%7ld %s\n", count, line);
    else printf("%s\n", line);
}

int main(int argc, char **argv) {
    char *line = NULL, *prev = NULL;
    size_t cap = 0;
    long count = 0;
    int fold = 0, i = 1, nl;
    FILE *f;
    lu_prog = "uniq";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'c') show_count = 1;
            else if(*o == 'd') only_dup = 1;
            else if(*o == 'u') only_single = 1;
            else if(*o == 'i') fold = 1;
            else {
                fprintf(stderr, "usage: uniq [-cdui] [file]\n");
                return 2;
            }
        }
    }
    if(argc - i > 1) {
        fprintf(stderr, "usage: uniq [-cdui] [file]\n");
        return 2;
    }
    if(!(f = lu_open(i < argc ? argv[i] : "-"))) return 2;
    while(lu_getline(f, &line, &cap, &nl) >= 0) {
        if(prev && !(fold ? strcasecmp(prev, line) : strcmp(prev, line))) {
            count++;
            continue;
        }
        if(prev) put(prev, count);
        free(prev);
        prev = strdup(line);
        if(!prev) lu_alloc(NULL, (size_t)-1);
        count = 1;
    }
    if(prev) put(prev, count);
    lu_close(f);
    return 0;
}
