/*
SurfOS user library: a failed assert()
----------------------
File: assert.c  Date: 10/11/26
----------------------
assert.h's assert() calls this when its expression is false: one line on
stderr (unbuffered), then abort(). The pieces come from the program's own
source, but they are printed as they are, so only printable ASCII is kept.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void put_safe(const char *s) {
    for(; s && *s; s++) fputc(*s >= 32 && *s < 127 ? *s : '?', stderr);
}

void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    put_safe(file);
    fprintf(stderr, ":%d: ", line);
    put_safe(func);
    fputs(": assertion \"", stderr);
    put_safe(expr);
    fputs("\" failed\n", stderr);
    abort();
}
