/*
SurfOS user library: inflate
----------------------
File: inflate.h Date: 11/01/26
----------------------
RFC 1951's decoder, as a stream: the caller points in and out at buffers and calls inflate_run(),
which uses what it can of both and returns. Either may run out in the middle of anything, a code
included; the next call goes on from there. It never reads a byte past the end of the deflate
data, so what follows it (gzip's trailer, the next file of a zip) is where in points.

    struct inflate *z = malloc(sizeof(*z));     about 46 KB: it holds the 32 KB window
    inflate_init(z);
    z->in = data; z->in_n = n;
    z->out = buf; z->out_n = sizeof(buf);
    r = inflate_run(z);

INFLATE_OK: more input, or more room, is wanted: in_n or out_n has reached 0. With no input left to
give and room to spare, the data was cut short: the library cannot know that, the caller does.
INFLATE_END: the last block is finished. INFLATE_BAD: the data is not deflate; msg says why, and
every later call answers the same.

Nothing is allocated, nothing is read but what in holds, and no length in the data is believed: a
stream may make as much as it likes, but only as far as out goes, so it is the caller who bounds it.
*/

#ifndef _INFLATE_H
#define _INFLATE_H

#include <stddef.h>

#define INFLATE_OK    0
#define INFLATE_END   1
#define INFLATE_BAD (-1)

#define INFLATE_WINDOW  32768
#define INFLATE_ROOT    9               /* a code of up to this many bits is found by one lookup */
#define INFLATE_MORE    2600            /* entries for all longer codes: inflate.c says why this is enough */
#define INFLATE_SYMBOLS 288

/* a Huffman code as a table: inflate.c says how it is laid out */
struct inflate_table {
    unsigned short first[1 << INFLATE_ROOT];
    unsigned short more[INFLATE_MORE];
};

struct inflate {
    const unsigned char *in;            /* the input: moves past what is used */
    size_t in_n;
    unsigned char *out;                 /* where the output goes: moves past what is made */
    size_t out_n;
    const char *msg;                    /* after INFLATE_BAD: why */

    /* the rest is inflate.c's */
    unsigned int bits;                  /* bits taken from in, the next one lowest */
    int nbits;
    int state;
    int last;                           /* this is the last block */
    int fixed_ready;                    /* lit and dist hold the fixed codes */
    unsigned int left;                  /* a stored block's bytes to go; a copy's */
    unsigned int dist;                  /* a copy's distance */
    int extra;                          /* the extra bits being waited for */
    int nlit, ndist, nlen, have;        /* the code lengths of a dynamic block */
    int repeat, repeat_value;
    unsigned char literal;              /* one that waits for room */
    unsigned int wpos, whave;           /* the window: where the next byte goes, how many are good */
    struct inflate_table lit, dcode;
    unsigned char lens[286 + 30];
    unsigned char window[INFLATE_WINDOW];
};

void inflate_init(struct inflate *z);   /* ready for a stream; in and out are the caller's */
int inflate_run(struct inflate *z);

#endif
