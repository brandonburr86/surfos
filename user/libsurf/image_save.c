/*
SurfOS user library: pictures written
----------------------
File: image_save.c  Date: 11/05/26
----------------------
image.h's encoders, the decoders' other half: a picture as a BMP of 24 bits, or as a PNG of 8 bits a sample, RGB or
RGBA. The bytes go to the caller's put() as they are made, a buffer of OUTBUF at a time, so that a picture is never
held a second time as a file.

BMP, as Microsoft documents BITMAPFILEHEADER and BITMAPINFOHEADER: the file header of 14 bytes, the header of 40
with the height positive (rows from the bottom), one plane, 24 bits, BI_RGB, no resolution (0: none said); each
row's pixels as blue, green and red, padded with zeros to a multiple of 4 bytes, as image_bmp.c reads them.

PNG, as its standard has it (ISO/IEC 15948, the W3C's Portable Network Graphics second edition): the signature,
IHDR (8 bits, colour type 2 or 6, compression, filter method and interlace 0), the data in IDAT chunks of OUTBUF
bytes at most, IEND; each chunk's CRC by libsurf's crc32(). The data is a zlib stream (RFC 1950): two bytes of
header (deflate with a 32 KB window, the flag of the default level, its check), the rows by libsurf's deflate (RFC
1951) at level 6, and the Adler-32 of the rows, as RFC 1950 section 8.2 defines it. Each row is filtered as the
standard's section 12.8 suggests for pictures of true colour: all five filters are tried, and the one whose bytes,
read as signed numbers, have the least sum of absolute values is kept.

Memory: deflate's state (about 310 KB), four rows of the picture's width and OUTBUF, all asked for before anything
is written and each checked; a picture over wimg_alloc()'s limits is refused before that.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <crc32.h>
#include <deflate.h>
#include <image.h>

#define OUTBUF  32768                           /* the bytes handed to put() at a time; a PNG's largest IDAT */
#define LEVEL   6                               /* deflate's: RFC 1950's FLEVEL 2, the default */

static void le16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void le32(unsigned char *p, unsigned int v) {
    le16(p, v & 0xffff);
    le16(p + 2, v >> 16);
}

static void be32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* wimg_alloc()'s limits, so that whatever is written here is read here again */
static int fits(const struct wimg *m, char *err, size_t cap) {
    unsigned long long n;
    if(!m || !m->px || m->w <= 0 || m->h <= 0) return wimg_error(err, cap, "a picture of no size");
    n = (unsigned long long)(unsigned int)m->w * (unsigned int)m->h;
    if(m->w > WIMG_SIDE_MAX || m->h > WIMG_SIDE_MAX || n > WIMG_PIXELS_MAX)
        return wimg_error(err, cap, "a picture of %d x %d pixels: over the limit of %d a side and 16 megapixels", m->w,
                          m->h, WIMG_SIDE_MAX);
    return 0;
}

/* put() with what it says remembered: once it fails, nothing more is handed to it */
struct sink {
    wimg_put put;
    void *ctx;
    int failed;
};

static int give(struct sink *k, const void *p, size_t n) {
    if(!k->failed && n && k->put(k->ctx, p, n)) k->failed = 1;
    return k->failed ? -1 : 0;
}

/**** BMP ****/

int wimg_save_bmp(const struct wimg *m, wimg_put put, void *ctx, char *err, size_t cap) {
    struct sink k;
    unsigned char *buf;
    size_t row, fill;
    int x, y;
    if(fits(m, err, cap)) return -1;
    k.put = put;
    k.ctx = ctx;
    k.failed = 0;
    /* a row is 12288 bytes at most, and the file 54 + 12288 x 3906 at most: well within 32 bits */
    row = ((size_t)m->w * 3 + 3) & ~(size_t)3;
    buf = malloc(OUTBUF);
    if(!buf) return wimg_error(err, cap, "out of memory writing a BMP");
    memset(buf, 0, 54);
    buf[0] = 'B';
    buf[1] = 'M';
    le32(buf + 2, (unsigned int)(54 + row * (size_t)m->h));    /* the file's size */
    le32(buf + 10, 54);                                         /* where the pixels start */
    le32(buf + 14, 40);                                         /* the header's size */
    le32(buf + 18, (unsigned int)m->w);
    le32(buf + 22, (unsigned int)m->h);                         /* positive: the bottom row first */
    le16(buf + 26, 1);                                          /* planes */
    le16(buf + 28, 24);                                         /* bits a pixel; BI_RGB, the rest 0 */
    le32(buf + 34, (unsigned int)(row * (size_t)m->h));         /* the pixels' bytes */
    fill = 54;
    for(y = m->h - 1; y >= 0 && !k.failed; y--) {
        const unsigned int *s = m->px + (size_t)y * (size_t)m->w;
        unsigned char *d;
        if(fill + row > OUTBUF) {
            give(&k, buf, fill);
            fill = 0;
        }
        d = buf + fill;
        for(x = 0; x < m->w; x++, d += 3) {
            d[0] = (unsigned char)s[x];
            d[1] = (unsigned char)(s[x] >> 8);
            d[2] = (unsigned char)(s[x] >> 16);
        }
        memset(d, 0, row - (size_t)m->w * 3);
        fill += row;
    }
    give(&k, buf, fill);
    free(buf);
    if(k.failed) return wimg_error(err, cap, "the BMP could not be written");
    return 0;
}

/**** PNG ****/

struct png_out {
    struct sink k;
    struct deflate *z;
    unsigned char *data;                        /* an IDAT's data as it fills, OUTBUF bytes */
    size_t fill;
    unsigned int adler_a, adler_b;
};

/* A chunk, whole: its length, its type, its data and the CRC of the last two */
static int chunk(struct png_out *o, const char *type, const unsigned char *d, size_t n) {
    unsigned char head[8], tail[4];
    be32(head, (unsigned int)n);
    memcpy(head + 4, type, 4);
    be32(tail, crc32(crc32(0, type, 4), d, n));
    give(&o->k, head, 8);
    give(&o->k, d, n);
    return give(&o->k, tail, 4);
}

/* Bytes of the zlib stream: into the IDAT being filled, which goes out when it is full */
static void stream(struct png_out *o, const unsigned char *p, size_t n) {
    while(n && !o->k.failed) {
        size_t room = OUTBUF - o->fill;
        if(room > n) room = n;
        memcpy(o->data + o->fill, p, room);
        o->fill += room;
        p += room;
        n -= room;
        if(o->fill == OUTBUF) {
            chunk(o, "IDAT", o->data, o->fill);
            o->fill = 0;
        }
    }
}

/* Bytes of the rows: their Adler-32, then deflate, whose output goes straight into the IDAT being filled. finish:
   these are the last, and deflate ends its stream. */
static void rows(struct png_out *o, const unsigned char *p, size_t n, int finish) {
    struct deflate *z = o->z;
    size_t i;
    for(i = 0; i < n; i++) {
        o->adler_a += p[i];
        if(o->adler_a >= 65521) o->adler_a -= 65521;
        o->adler_b += o->adler_a;
        if(o->adler_b >= 65521) o->adler_b -= 65521;
    }
    z->in = p;
    z->in_n = n;
    while(!o->k.failed) {
        int r;
        z->out = o->data + o->fill;
        z->out_n = OUTBUF - o->fill;
        r = deflate_run(z, finish);
        o->fill = OUTBUF - z->out_n;
        if(!z->out_n) {                         /* the chunk is full: deflate may have more */
            chunk(o, "IDAT", o->data, o->fill);
            o->fill = 0;
            continue;
        }
        if(finish ? r == DEFLATE_END : !z->in_n) return;
    }
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Row raw (n bytes, bpp a pixel) filtered by type t against up, the row above it (zeros for the first), into f with
   the type first: the sum of its bytes' absolute values as signed numbers */
static unsigned long filtered(int t, const unsigned char *raw, const unsigned char *up, size_t n, size_t bpp,
                              unsigned char *f) {
    unsigned long sum = 0;
    size_t i;
    f[0] = (unsigned char)t;
    for(i = 0; i < n; i++) {
        int a = i >= bpp ? raw[i - bpp] : 0, b = up[i], c = i >= bpp ? up[i - bpp] : 0, pred;
        unsigned char v;
        switch(t) {
        case 0: pred = 0; break;
        case 1: pred = a; break;
        case 2: pred = b; break;
        case 3: pred = (a + b) >> 1; break;
        default: pred = paeth(a, b, c); break;
        }
        v = (unsigned char)(raw[i] - pred);
        f[i + 1] = v;
        sum += v < 128 ? v : 256u - v;
    }
    return sum;
}

int wimg_save_png(const struct wimg *m, int alpha, wimg_put put, void *ctx, char *err, size_t cap) {
    static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    struct png_out o;
    unsigned char ihdr[13], zhead[2], adler[4], *raw, *up, *best, *cand;
    size_t bpp = alpha ? 4 : 3, n;
    int x, y, t;
    if(fits(m, err, cap)) return -1;
    memset(&o, 0, sizeof(o));
    o.k.put = put;
    o.k.ctx = ctx;
    o.adler_a = 1;
    n = (size_t)m->w * bpp;                     /* 16384 bytes at most */
    o.z = malloc(sizeof(*o.z));
    o.data = malloc(OUTBUF);
    raw = malloc(n);
    up = calloc(1, n);                          /* the row above the first is zeros */
    best = malloc(n + 1);
    cand = malloc(n + 1);
    if(!o.z || !o.data || !raw || !up || !best || !cand) {
        free(o.z);
        free(o.data);
        free(raw);
        free(up);
        free(best);
        free(cand);
        return wimg_error(err, cap, "out of memory writing a PNG");
    }
    deflate_init(o.z, LEVEL);
    give(&o.k, signature, sizeof(signature));
    be32(ihdr, (unsigned int)m->w);
    be32(ihdr + 4, (unsigned int)m->h);
    ihdr[8] = 8;                                /* bits a sample */
    ihdr[9] = alpha ? 6 : 2;                    /* RGBA, RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;         /* deflate, the adaptive filters, not interlaced */
    chunk(&o, "IHDR", ihdr, sizeof(ihdr));
    zhead[0] = 0x78;                            /* CM 8 (deflate), CINFO 7 (a 32 KB window) */
    zhead[1] = 2 << 6;                          /* FLEVEL 2, no dictionary; then FCHECK makes it a multiple of 31 */
    zhead[1] = (unsigned char)(zhead[1] + (31 - (zhead[0] * 256u + zhead[1]) % 31) % 31);
    stream(&o, zhead, 2);
    for(y = 0; y < m->h && !o.k.failed; y++) {
        const unsigned int *s = m->px + (size_t)y * (size_t)m->w;
        unsigned char *d = raw, *swap;
        unsigned long least = 0;
        for(x = 0; x < m->w; x++) {
            unsigned int c = s[x];
            *d++ = (unsigned char)(c >> 16);
            *d++ = (unsigned char)(c >> 8);
            *d++ = (unsigned char)c;
            if(alpha) *d++ = (unsigned char)(255 - (c >> 24));
        }
        for(t = 0; t < 5; t++) {
            unsigned long sum = filtered(t, raw, up, n, bpp, cand);
            if(t && sum >= least) continue;
            least = sum;
            swap = best;
            best = cand;
            cand = swap;
        }
        rows(&o, best, n + 1, y == m->h - 1);
        swap = up;
        up = raw;
        raw = swap;
    }
    be32(adler, o.adler_b << 16 | o.adler_a);
    stream(&o, adler, 4);
    if(o.fill) chunk(&o, "IDAT", o.data, o.fill);
    chunk(&o, "IEND", NULL, 0);
    free(o.z);
    free(o.data);
    free(raw);
    free(up);
    free(best);
    free(cand);
    if(o.k.failed) return wimg_error(err, cap, "the PNG could not be written");
    return 0;
}
