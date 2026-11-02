/*
SurfOS user programs: what the small utilities share
----------------------
File: cli.h     Date: 11/01/26
----------------------
cmp, comm, join, paste, fold, nl, rev, od, base64, strings, tree and diff and patch's main programs
sit on lineutil.h. This adds what they want beyond it: UTF-8 sequences (a character is one, and
is never split; a byte that starts none counts one and passes through as it is), input a
character at a time, a whole number from an option, and the end of standard output, whose
failure a program must say (a full disk, for one).
*/

#ifndef _CLI_H
#define _CLI_H

#include "../bin/lineutil.h"
#include <limits.h>
#include <unistd.h>

#define CLI_UNUSED __attribute__((unused))

/* the bytes of the UTF-8 sequence at s, of the n there: 2 to 4 for a whole and valid one, 1 for anything
   else (ASCII, a stray byte, a sequence the end cuts off). Valid as RFC 3629 says: the shortest form, no
   surrogates, nothing past U+10FFFF. */
static CLI_UNUSED int cli_seq(const unsigned char *s, size_t n) {
    unsigned c = *s, lo = 0x80, hi = 0xBF;
    int len, i;
    if(c < 0xC2 || c > 0xF4) return 1;
    len = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if((size_t)len > n) return 1;
    if(c == 0xE0) lo = 0xA0;
    else if(c == 0xED) hi = 0x9F;
    else if(c == 0xF0) lo = 0x90;
    else if(c == 0xF4) hi = 0x8F;
    for(i = 1; i < len; i++) {
        if(s[i] < lo || s[i] > hi) return 1;
        lo = 0x80;
        hi = 0xBF;
    }
    return len;
}

/* input a character at a time. read() and not stdio, so that what a slow pipe has sent is worked on at
   once; the buffer only ever waits for the rest of a sequence that has begun. */
struct cli_in {
    int fd;
    int eof;                            /* read() said the end, or failed: err then says why */
    int err;
    size_t pos, end;
    unsigned char buf[4096];
};

static CLI_UNUSED void cli_in_start(struct cli_in *in, FILE *f) {
    in->fd = fileno(f);
    in->eof = in->err = 0;
    in->pos = in->end = 0;
}

static CLI_UNUSED void cli_in_fill(struct cli_in *in) {
    size_t left = in->end - in->pos;
    ssize_t r;
    memmove(in->buf, in->buf + in->pos, left);
    in->pos = 0;
    in->end = left;
    r = read(in->fd, in->buf + left, sizeof(in->buf) - left);
    if(r > 0) in->end += (size_t)r;
    else {
        in->eof = 1;
        if(r < 0) in->err = errno;
    }
}

/* the next character: its bytes in *s, their count as the result; 0 at the end of the input (in->err: a read
   failed). What *s points to lasts until the next call. */
static CLI_UNUSED int cli_in_next(struct cli_in *in, const unsigned char **s) {
    unsigned c;
    size_t want;
    int n;
    while(in->pos == in->end) {
        if(in->eof) return 0;
        cli_in_fill(in);
    }
    c = in->buf[in->pos];
    want = c < 0xC2 || c > 0xF4 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    while(in->end - in->pos < want && !in->eof) cli_in_fill(in);
    n = cli_seq(in->buf + in->pos, in->end - in->pos);
    *s = in->buf + in->pos;
    in->pos += (size_t)n;
    return n;
}

/* a whole number: 0, or -1 when s is none or does not fit. By constants: 64-bit division needs libgcc,
   which programs do not link. */
static CLI_UNUSED int cli_number(const char *s, unsigned long long *v) {
    unsigned long long n = 0;
    if(*s < '0' || *s > '9') return -1;
    for(; *s >= '0' && *s <= '9'; s++) {
        unsigned d = (unsigned)(*s - '0');
        if(n > 1844674407370955161ULL || (n == 1844674407370955161ULL && d > 5)) return -1;
        n = n * 10 + d;
    }
    if(*s) return -1;
    *v = n;
    return 0;
}

/* the same for what must fit an int */
static CLI_UNUSED int cli_int(const char *s, int *v) {
    unsigned long long n;
    if(cli_number(s, &n) < 0 || n > INT_MAX) return -1;
    *v = (int)n;
    return 0;
}

/* the end of a program's output: 0, or 2 and a message when it could not all be written */
static CLI_UNUSED int cli_flush(const char *prog) {
    if(fflush(stdout) == EOF || ferror(stdout)) {
        fprintf(stderr, "%s: standard output: %s\n", prog, strerror(errno ? errno : EIO));
        return 2;
    }
    return 0;
}

#endif
