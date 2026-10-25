/*
SurfOS user programs: what the filters share
----------------------
File: lineutil.h  Date: 10/25/26
----------------------
grep, sort, uniq and tail read lines of any length: lu_getline() grows its buffer. Each
takes files or, without them (or for -), its standard input; lu_open() opens one and says
why it cannot.
*/

#ifndef _LINEUTIL_H
#define _LINEUTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define LU_UNUSED __attribute__((unused))

static LU_UNUSED const char *lu_prog = "?";     /* for the messages */

static LU_UNUSED void *lu_alloc(void *p, size_t n) {
    p = realloc(p, n);
    if(!p) {
        fprintf(stderr, "%s: out of memory\n", lu_prog);
        exit(2);
    }
    return p;
}

/* The next line of f in *buf (grown as needed, *cap its size), its '\n' taken off: its length,
   -1 at the end of the input. *nl: it ended in a '\n' (a last line may not). */
static LU_UNUSED long lu_getline(FILE *f, char **buf, size_t *cap, int *nl) {
    size_t n = 0;
    int c;
    *nl = 0;
    while((c = getc(f)) != EOF) {
        if(n + 2 > *cap) {
            *cap = *cap ? *cap * 2 : 128;
            *buf = lu_alloc(*buf, *cap);
        }
        if(c == '\n') {
            *nl = 1;
            break;
        }
        (*buf)[n++] = (char)c;
    }
    if(c == EOF && !n) return -1;
    if(!*cap) {
        *cap = 128;
        *buf = lu_alloc(*buf, *cap);
    }
    (*buf)[n] = 0;
    return (long)n;
}

/* a file to read, - for standard input; NULL said */
static LU_UNUSED FILE *lu_open(const char *name) {
    FILE *f;
    if(!strcmp(name, "-")) return stdin;
    f = fopen(name, "r");
    if(!f) fprintf(stderr, "%s: %s: %s\n", lu_prog, name, strerror(errno));
    return f;
}

static LU_UNUSED void lu_close(FILE *f) {
    if(f != stdin) fclose(f);
}

#endif
