/*
SurfOS user program: tail
--------------------
File: tail.c    Date: 10/25/26
--------------------
tail [-n N | -N] [file...]: the last N lines (10) of each file or of standard input; with more
than one file each gets a "==> name <==" heading, as head does. The lines are kept in a ring
that grows with the input up to N of them, however big N is.
*/

#include "lineutil.h"

static int tail(FILE *f, long want) {
    char **ring = NULL, *line = NULL;
    size_t cap = 0;
    long n = 0, size = 0, k;                /* lines read; the ring's places, at most want */
    int nl;
    if(want <= 0) {
        while(lu_getline(f, &line, &cap, &nl) >= 0);
        free(line);
        return ferror(f) ? -1 : 0;
    }
    while(lu_getline(f, &line, &cap, &nl) >= 0) {
        if(n == size && size < want) {      /* full before it wrapped: twice the places (no more than want) */
            long more = size ? (size > want / 2 ? want : size * 2) : (want < 64 ? want : 64);
            ring = lu_alloc(ring, (size_t)more * sizeof(char *));
            memset(ring + size, 0, (size_t)(more - size) * sizeof(char *));
            size = more;
        }
        free(ring[n % size]);
        ring[n % size] = strdup(line);
        if(!ring[n % size]) lu_alloc(NULL, (size_t)-1);
        n++;
    }
    for(k = n > size ? n - size : 0; k < n; k++) {
        printf("%s\n", ring[k % size]);
        free(ring[k % size]);
    }
    free(ring);
    free(line);
    return ferror(f) ? -1 : 0;
}

int main(int argc, char **argv) {
    long lines = 10;
    int i = 1, rc = 0, files;
    char *end;
    lu_prog = "tail";
    if(i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) {
        lines = strtol(argv[i + 1], &end, 10);
        if(*end || lines < 0) {
            fprintf(stderr, "tail: %s: not a line count\n", argv[i + 1]);
            return 2;
        }
        i += 2;
    } else if(i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
        lines = strtol(argv[i] + 1, &end, 10);
        if(*end) {
            fprintf(stderr, "usage: tail [-n N] [file...]\n");
            return 2;
        }
        i++;
    }
    files = argc - i;
    if(!files) return tail(stdin, lines) ? 1 : 0;
    for(; i < argc; i++) {
        FILE *f = lu_open(argv[i]);
        if(!f) {
            rc = 1;
            continue;
        }
        if(files > 1) printf("%s==> %s <==\n", i > argc - files ? "\n" : "", argv[i]);
        if(tail(f, lines)) {
            fprintf(stderr, "tail: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
        lu_close(f);
    }
    return rc;
}
