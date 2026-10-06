/*
SurfOS user library: what the library's own files share
----------------------
File: libsurf.h Date: 10/5/26
----------------------
Not a public header: the formatter behind every printf (printf.c) writes into
a sink, which stdio.c points at a stream and printf.c at a string.
*/

#ifndef _SURF_LIBSURF_H
#define _SURF_LIBSURF_H

#include <stddef.h>
#include <stdarg.h>

struct sink {
    void (*put)(struct sink *s, const char *p, size_t n);
    size_t total;               /* characters produced so far */
    char *buf;                  /* string sinks: where the next character goes */
    size_t left;                /* and the room left there, terminator included */
    void *ctx;                  /* stream sinks: the FILE or descriptor */
    int fail;                   /* set by a sink that could not write */
};

int __format(struct sink *s, const char *fmt, va_list ap);

#endif
