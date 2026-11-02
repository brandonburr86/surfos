/*
SurfOS user program: bc
--------------------
File: main.c    Date: 11/01/26
--------------------
bc [-lq] [file...]: an arbitrary precision calculator, and the language POSIX gives it. It reads
the files and then standard input (- in the list is standard input at that place), and runs each
line or function as it is complete. -l defines s, c, e, l, a and j, and starts scale at 20. -q is
taken and does nothing, as bc says nothing when it starts. A file that cannot be read ends it
before anything runs, as POSIX asks.

A mistake at a terminal costs its line and bc goes on; in a file or a pipe it ends bc with status
2, as what follows a mistake would mean something else. Status 0 otherwise. Memory is the only
limit on a number's length and on how deep functions call, and reaching it is such a mistake.
The rest is in user/bc/bc.h.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "bc.h"

/* A disk that is full would otherwise have a loop print into nothing for ever. A reader that went away ends the
   program in libsurf, status 141, before this sees it. */
static void write_failed(void) {
    fprintf(stderr, "bc: standard output: %s\n", strerror(errno));
    _exit(2);
}

static void host_put(void *ctx, int c) {
    (void)ctx;
    if(putchar(c) == EOF) write_failed();
}

static void host_flush(void *ctx) {
    (void)ctx;
    if(fflush(stdout) == EOF) write_failed();
}

static void host_error(void *ctx, const char *text) {
    (void)ctx;
    fprintf(stderr, "bc: %s\n", text);
}

static int host_get(void *in) {
    return getc((FILE *)in);
}

/* The end of a file that could not be read is no end: a directory, say. */
static int read_failed(const char *name) {
    host_flush(NULL);
    fprintf(stderr, "bc: %s: %s\n", name, strerror(errno));
    return BC_TROUBLE;
}

static int usage(void) {
    fprintf(stderr, "usage: bc [-lq] [file...]\n");
    return 2;
}

int main(int argc, char **argv) {
    static const struct bc_host host = { host_put, host_flush, host_error, NULL };
    struct bc *b;
    int library = 0, i = 1, k, rc = BC_END, status = 0;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'l') library = 1;
            else if(*o != 'q') return usage();
        }
    }
    for(k = i; k < argc; k++) {         /* opened and closed: 16 descriptors would not hold every name at once */
        FILE *f;
        if(!strcmp(argv[k], "-")) continue;
        if(!(f = fopen(argv[k], "r"))) {
            fprintf(stderr, "bc: %s: %s\n", argv[k], strerror(errno));
            return 2;
        }
        fclose(f);
    }
    if(!(b = bc_new(&host))) {
        fprintf(stderr, "bc: out of memory\n");
        return 2;
    }
    if(library && bc_library(b) != BC_END) return 2;
    for(; i < argc && rc == BC_END; i++) {
        FILE *f = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if(!f) {
            fprintf(stderr, "bc: %s: %s\n", argv[i], strerror(errno));
            return 2;
        }
        rc = bc_run(b, host_get, f, f == stdin ? "(standard input)" : argv[i], 0);
        if(ferror(f)) rc = read_failed(f == stdin ? "standard input" : argv[i]);
        if(f != stdin) fclose(f);
    }
    if(rc == BC_END) {
        rc = bc_run(b, host_get, stdin, "(standard input)", isatty(0));
        if(ferror(stdin)) rc = read_failed("standard input");
    }
    if(rc == BC_TROUBLE) status = 2;
    host_flush(NULL);
    return status;
}
