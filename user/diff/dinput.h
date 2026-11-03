/*
SurfOS user programs: what diff and patch read
----------------------
File: dinput.h  Date: 11/01/26
----------------------
A source is a file by offsets: a regular file on its descriptor, read again as often as needed, or
standard input and pipes kept in memory, since they can be read only once. A reader goes
forward through part of a source a block at a time. Neither holds more of a regular file than
a block, so a file may be nearly 4 GB; offsets are unsigned, and lseek's 32 signed bits are
kept to here alone. Host-tested by tests/host/test_diff.c, where sources are memory.
*/

#ifndef _DINPUT_H
#define _DINPUT_H

#include <stddef.h>
#include <stdio.h>

#define DR_BLOCK 32768
#define DSRC_ALL 0xFFFFFFFFu            /* a stream's size: not known */

struct dsrc {
    int fd;                             /* -1: the bytes are at mem */
    int stream;                         /* read once, forward, to its end: patch's input */
    int own;                            /* fd or mem is this source's, to close or free */
    const unsigned char *mem;
    unsigned size;
    unsigned pos;                       /* where fd's read position is */
};

struct dreader {
    struct dsrc *src;
    unsigned next;                      /* the offset of what comes after the buffer */
    unsigned end;                       /* it stops before this offset */
    unsigned pos, len;                  /* buf[pos..len) is not read yet */
    int failed;                         /* the errno of a read that failed; EIO: the file got shorter */
    unsigned char *buf;
};

/* name; - is standard input. 0, or -1 and errno */
int dsrc_open(struct dsrc *s, const char *name);
void dsrc_memory(struct dsrc *s, const void *p, unsigned n);    /* the bytes stay the caller's */
void dsrc_stream(struct dsrc *s, int fd);                       /* fd stays the caller's */
void dsrc_close(struct dsrc *s);
/* up to n bytes at off: how many, 0 at the end, -1 and errno */
long dsrc_read(struct dsrc *s, unsigned off, void *buf, unsigned n);
/* n bytes at off, or fewer only at the end: how many, -1 and errno */
long dsrc_read_full(struct dsrc *s, unsigned off, void *buf, unsigned n);

/* a reader of [from, to): 0, or -1 (ENOMEM) */
int dr_start(struct dreader *r, struct dsrc *s, unsigned from, unsigned to);
void dr_stop(struct dreader *r);
void dr_seek(struct dreader *r, unsigned off);
int dr_refill(struct dreader *r);                               /* the next byte, -1 at the end or on a failure */
#define dr_getc(r) ((r)->pos < (r)->len ? (int)(r)->buf[(r)->pos++] : dr_refill(r))
#define dr_tell(r) ((r)->next - ((r)->len - (r)->pos))
/* past the next line, or copied to out without its newline: 1 it ended in a newline, 0 in the end of the input,
   -1 there is no line */
int dr_skip_line(struct dreader *r);
int dr_copy_line(struct dreader *r, FILE *out);

/* the hash of a line: FNV-1a. Never trusted alone: diff and patch compare the bytes before they believe it */
#define DH_START 2166136261u
#define DH_ADD(h, c) (((h) ^ (unsigned char)(c)) * 16777619u)

#endif
