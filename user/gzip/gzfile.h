/*
SurfOS user programs: the gzip format
----------------------
File: gzfile.h  Date: 11/01/26
----------------------
RFC 1952 over libsurf's inflate and deflate, shared by gzip and tar. Bytes come from and go to
functions, so this file makes no system calls and tests/host/ can run it over memory.

    struct gz_reader *r = malloc(sizeof(*r));   about 70 KB
    gzr_start(r, source, ctx);
    while((n = gzr_read(r, buf, sizeof(buf))) > 0) ...

gzr_read() gives the data of every member in turn. It returns 0 only after the last member's CRC
and length have been checked and nothing but the end of the input (or zero bytes, below) follows.
-1: r->why says what is wrong with the data; NULL there means the source itself failed, and errno
says how. Data already given is not taken back, so a caller that writes a file keeps it only when
gzr_read() returns 0.

GNU tar pads what `tar czf - ...` writes with zero bytes up to its record size, and gzip takes
those silently. So zeros at the very end are the one thing allowed after the last member; anything
else there is refused, and so is any member that follows zeros.

    struct gz_writer *w = malloc(sizeof(*w));   about 340 KB
    gzw_start(w, sink, ctx, level, name, mtime);    name may be NULL
    gzw_write(w, data, n);
    gzw_finish(w);                              the last bytes and the trailer

Each returns 0, or -1 when the sink failed, with errno as the sink left it.
*/

#ifndef _GZFILE_H
#define _GZFILE_H

#include <stdint.h>
#include <inflate.h>
#include <deflate.h>

#define GZ_BUF 32768

typedef int (*gz_source)(void *ctx, void *buf, unsigned int n);         /* bytes read, 0 at the end, -1 and errno */
typedef int (*gz_sink)(void *ctx, const void *buf, unsigned int n);     /* 0 when all were written, -1 and errno */

struct gz_reader {
    const char *why;                    /* after -1: what is wrong with the data, NULL: the source failed */

    /* the rest is gzfile.c's */
    gz_source source;
    void *ctx;
    int state;
    int at_end;                         /* the source said 0 */
    int source_failed;
    unsigned int members;
    uint32_t crc, size;
    const unsigned char *next;          /* the input in hand */
    unsigned int have;
    unsigned char in[GZ_BUF];
    struct inflate z;
};

void gzr_start(struct gz_reader *r, gz_source source, void *ctx);
int gzr_read(struct gz_reader *r, void *buf, unsigned int n);

struct gz_writer {
    /* gzfile.c's */
    gz_sink sink;
    void *ctx;
    uint32_t crc, size;
    unsigned char out[GZ_BUF];
    struct deflate z;
};

int gzw_start(struct gz_writer *w, gz_sink sink, void *ctx, int level, const char *name, unsigned int mtime);
int gzw_write(struct gz_writer *w, const void *data, unsigned int n);
int gzw_finish(struct gz_writer *w);

#endif
