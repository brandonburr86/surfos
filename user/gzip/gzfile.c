/*
SurfOS user programs: the gzip format
--------------------
File: gzfile.c  Date: 11/01/26
--------------------
What gzfile.h says. A header's optional fields are read and skipped as they come: nothing of them
is kept, so a name or a comment of any length costs only the time to read it. A cut anywhere is
"the data ends too early"; nothing here runs past what the source gave.
*/

#include <string.h>
#include <crc32.h>
#include "gzfile.h"

#define FHCRC    2
#define FEXTRA   4
#define FNAME    8
#define FCOMMENT 16
#define FLAGS_RESERVED 0xE0

enum { R_NEXT, R_BODY, R_TRAILER, R_DONE, R_BAD };

static const char CUT_SHORT[] = "the data ends too early";

static int bad(struct gz_reader *r, const char *why) {
    r->why = why;
    r->state = R_BAD;
    return -1;
}

/* more input in hand: 1, or 0 at the end or when the source failed */
static int refill(struct gz_reader *r) {
    int n;
    if(r->at_end) return 0;
    n = r->source(r->ctx, r->in, sizeof(r->in));
    if(n < 0) {
        r->source_failed = 1;
        return 0;
    }
    if(!n) {
        r->at_end = 1;
        return 0;
    }
    r->next = r->in;
    r->have = (unsigned int)n;
    return 1;
}

/* the next byte, -1 when there is none */
static int next_byte(struct gz_reader *r) {
    if(!r->have && !refill(r)) return -1;
    r->have--;
    return *r->next++;
}

/* -1 from next_byte(): what to say */
static int no_byte(struct gz_reader *r) {
    return bad(r, r->source_failed ? NULL : CUT_SHORT);
}

/* a header's byte, into its CRC: -1 said */
static int header_byte(struct gz_reader *r, uint32_t *crc) {
    unsigned char b;
    int c = next_byte(r);
    if(c < 0) return no_byte(r);
    b = (unsigned char)c;
    *crc = crc32(*crc, &b, 1);
    return c;
}

/* up to and including the terminating zero */
static int skip_string(struct gz_reader *r, uint32_t *crc) {
    int c;
    do {
        c = header_byte(r, crc);
    } while(c > 0);
    return c;
}

/* After the first byte, 0x1f: the rest of a member's header. The bytes before the data are all this reads. */
static int read_header(struct gz_reader *r, const char *not_gzip) {
    static const unsigned char id1 = 0x1f;
    uint32_t crc = crc32(0, &id1, 1);
    int c, flags, i;
    if((c = header_byte(r, &crc)) < 0) return r->why == CUT_SHORT ? bad(r, not_gzip) : -1;
    if(c != 0x8b) return bad(r, not_gzip);
    if((c = header_byte(r, &crc)) < 0) return -1;
    if(c != 8) return bad(r, "a compression method gzip does not know");
    if((flags = header_byte(r, &crc)) < 0) return -1;
    if(flags & FLAGS_RESERVED) return bad(r, "header flags gzip does not know");
    for(i = 0; i < 6; i++) if(header_byte(r, &crc) < 0) return -1;      /* the time, XFL, OS: not used */
    if(flags & FEXTRA) {
        int lo, hi;
        unsigned int n;
        if((lo = header_byte(r, &crc)) < 0 || (hi = header_byte(r, &crc)) < 0) return -1;
        for(n = (unsigned int)hi << 8 | (unsigned int)lo; n; n--) if(header_byte(r, &crc) < 0) return -1;
    }
    if((flags & FNAME) && skip_string(r, &crc) < 0) return -1;
    if((flags & FCOMMENT) && skip_string(r, &crc) < 0) return -1;
    if(flags & FHCRC) {
        int lo = next_byte(r), hi;
        if(lo < 0 || (hi = next_byte(r)) < 0) return no_byte(r);
        if((unsigned int)(hi << 8 | lo) != (crc & 0xffff)) return bad(r, "a header whose CRC is wrong");
    }
    inflate_init(&r->z);
    r->crc = 0;
    r->size = 0;
    r->state = R_BODY;
    return 0;
}

/* What follows a member, or the very start: the next member, or the end. */
static int next_member(struct gz_reader *r) {
    const char *why = r->members ? "something other than gzip data after the end" : "not in gzip format";
    int c = next_byte(r);
    if(c < 0) {
        if(r->source_failed) return no_byte(r);
        if(!r->members) return bad(r, why);
        r->state = R_DONE;
        return 0;
    }
    if(!c && r->members) {                              /* padding, if nothing but zeros is left */
        do {
            c = next_byte(r);
        } while(c == 0);
        if(c < 0 && r->source_failed) return no_byte(r);
        if(c < 0) {
            r->state = R_DONE;
            return 0;
        }
        return bad(r, why);
    }
    if(c != 0x1f) return bad(r, why);
    return read_header(r, why);
}

static int read_trailer(struct gz_reader *r) {
    uint32_t v[2] = { 0, 0 };
    int i;
    for(i = 0; i < 8; i++) {
        int c = next_byte(r);
        if(c < 0) return no_byte(r);
        v[i / 4] |= (uint32_t)c << (8 * (i % 4));
    }
    if(v[0] != r->crc) return bad(r, "the data's CRC is wrong");
    if(v[1] != r->size) return bad(r, "the data's length is wrong");
    r->members++;
    r->state = R_NEXT;
    return 0;
}

void gzr_start(struct gz_reader *r, gz_source source, void *ctx) {
    r->why = NULL;
    r->source = source;
    r->ctx = ctx;
    r->state = R_NEXT;
    r->at_end = 0;
    r->source_failed = 0;
    r->members = 0;
    r->next = r->in;
    r->have = 0;
}

int gzr_read(struct gz_reader *r, void *buf, unsigned int n) {
    for(;;) switch(r->state) {
    case R_NEXT:
        if(next_member(r) < 0) return -1;
        break;
    case R_BODY: {
        unsigned int made;
        int result;
        r->z.in = r->next;
        r->z.in_n = r->have;
        r->z.out = buf;
        r->z.out_n = n;
        result = inflate_run(&r->z);
        r->next = r->z.in;
        r->have = (unsigned int)r->z.in_n;
        made = n - (unsigned int)r->z.out_n;
        r->crc = crc32(r->crc, buf, made);
        r->size += made;
        if(result == INFLATE_BAD) return bad(r, r->z.msg);
        if(result == INFLATE_END) r->state = R_TRAILER;
        if(made) return (int)made;
        if(result == INFLATE_OK && !r->have && !refill(r)) return no_byte(r);
        break;
    }
    case R_TRAILER:
        if(read_trailer(r) < 0) return -1;
        break;
    case R_DONE:
        return 0;
    default:
        return -1;
    }
}

/**** writing ****/

static void put_le32(unsigned char *p, uint32_t v) {
    int i;
    for(i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

int gzw_start(struct gz_writer *w, gz_sink sink, void *ctx, int level, const char *name, unsigned int mtime) {
    unsigned char h[10];
    w->sink = sink;
    w->ctx = ctx;
    w->crc = 0;
    w->size = 0;
    deflate_init(&w->z, level);
    h[0] = 0x1f;
    h[1] = 0x8b;
    h[2] = 8;
    h[3] = name ? FNAME : 0;
    put_le32(h + 4, mtime);
    h[8] = level >= 9 ? 2 : level <= 1 ? 4 : 0;         /* XFL: the slowest and the fastest */
    h[9] = 255;                                         /* OS: not one the format lists */
    if(sink(ctx, h, sizeof(h)) < 0) return -1;
    if(name && sink(ctx, name, (unsigned int)strlen(name) + 1) < 0) return -1;
    return 0;
}

static int pump(struct gz_writer *w, const void *data, unsigned int n, int finish) {
    w->z.in = data;
    w->z.in_n = n;
    for(;;) {
        unsigned int made;
        int result;
        w->z.out = w->out;
        w->z.out_n = sizeof(w->out);
        result = deflate_run(&w->z, finish);
        made = (unsigned int)sizeof(w->out) - (unsigned int)w->z.out_n;
        if(made && w->sink(w->ctx, w->out, made) < 0) return -1;
        if(result == DEFLATE_END) return 0;
        if(!finish && !w->z.in_n) return 0;
    }
}

int gzw_write(struct gz_writer *w, const void *data, unsigned int n) {
    w->crc = crc32(w->crc, data, n);
    w->size += n;
    return pump(w, data, n, 0);
}

int gzw_finish(struct gz_writer *w) {
    unsigned char t[8];
    if(pump(w, NULL, 0, 1) < 0) return -1;
    put_le32(t, w->crc);
    put_le32(t + 4, w->size);
    return w->sink(w->ctx, t, sizeof(t));
}
