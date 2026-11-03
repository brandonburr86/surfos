/*
SurfOS user library: deflate
----------------------
File: deflate.h Date: 11/01/26
----------------------
RFC 1951's encoder, as a stream: LZ77 with hash chains, in dynamic Huffman blocks (or fixed or
stored ones, where those are smaller). The caller points in and out at buffers and calls
deflate_run(), which takes what it can and makes what it can.

    struct deflate *z = malloc(sizeof(*z));     about 310 KB
    deflate_init(z, 6);                         level 1 (fastest) to 9 (smallest)
    z->in = data; z->in_n = n;
    z->out = buf; z->out_n = sizeof(buf);
    r = deflate_run(z, finish);

finish 0: there is more input to come, and it may be cut anywhere. DEFLATE_OK: in_n has reached 0 or
out_n has. finish 1: this is the end of the input: call again, with more room, until DEFLATE_END.
Nothing is allocated and nothing can fail: all input is valid.
*/

#ifndef _DEFLATE_H
#define _DEFLATE_H

#include <stddef.h>

#define DEFLATE_OK   0
#define DEFLATE_END  1

#define DEFLATE_WSIZE   32768
#define DEFLATE_HASH    (1 << 15)
#define DEFLATE_SYMBOLS 16384           /* a block ends at this many literals and matches */
#define DEFLATE_PENDING (66 * 1024)     /* holds a whole block: deflate.c says why this is enough */
#define DEFLATE_ALPHABET 286

struct deflate {
    const unsigned char *in;            /* the input: moves past what was taken */
    size_t in_n;
    unsigned char *out;                 /* where the output goes: moves past what was made */
    size_t out_n;

    /* the rest is deflate.c's */
    int chain, lazy, nice, good;        /* what the level means */
    unsigned int pos, fill;             /* the window: the next byte to code, the end of the input in it */
    int block_start;                    /* where the block begins in the window; below 0: slid out of it */
    unsigned int block_bytes;           /* the input the block's symbols cover */
    int have_literal;                   /* the byte before pos is coded yet */
    unsigned int prev_len, prev_dist;   /* the match found there */
    int nsym;
    int done;
    unsigned int pend_at, pend_n;
    unsigned int bitbuf;
    int bitcnt;
    unsigned short freq_lit[DEFLATE_ALPHABET], freq_dist[30];
    unsigned short code_lit[DEFLATE_ALPHABET + 2], code_dist[30];
    unsigned char len_lit[DEFLATE_ALPHABET + 2], len_dist[30];
    unsigned char window[2 * DEFLATE_WSIZE];
    unsigned short head[DEFLATE_HASH];
    unsigned short prev[DEFLATE_WSIZE];
    unsigned short sym_dist[DEFLATE_SYMBOLS];   /* 0: a literal */
    unsigned char sym_val[DEFLATE_SYMBOLS];     /* the literal, or the length - 3 */
    unsigned char pending[DEFLATE_PENDING];
};

void deflate_init(struct deflate *z, int level);
int deflate_run(struct deflate *z, int finish);

#endif
