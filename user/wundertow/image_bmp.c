/*
SurfOS Undertow's window: BMP pictures
----------------------
File: image_bmp.c  Date: 11/1/26
----------------------
Windows and OS/2 bitmaps: the 12-byte header and the 40-byte one with its extensions (52, 56, 64, 108, 124 bytes);
1, 2, 4, 8 bits with a palette, 16, 24 and 32 bits; plain rows, RLE4, RLE8 and bit fields; rows from the bottom or
from the top. A JPEG or a PNG inside a BMP, and OS/2's own compressions, are refused. Every offset and size is
checked against the file's real length in 64 bits: what the header says of the file's size is not looked at.
A palette index past the palette is black. What RLE skips is not there (a transparent pixel).
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "undertow.h"
#include "image.h"

#define BI_RGB        0
#define BI_RLE8       1
#define BI_RLE4       2
#define BI_BITFIELDS  3
#define BI_ALPHAFIELDS 6

typedef unsigned long long u64;

static unsigned int le16(const unsigned char *p) { return (unsigned int)p[0] | (unsigned int)p[1] << 8; }
static unsigned int le32(const unsigned char *p) {
    return (unsigned int)p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}

struct field {                                      /* a channel of a pixel of 16 or 32 bits */
    unsigned int mask;
    int shift, bits;
};

struct bmp {
    const unsigned char *in;
    size_t len;
    int w, h, topdown, bpp;
    unsigned int comp;
    unsigned int palette[256];                      /* 0x00RRGGBB; zeroed: what a file leaves out is black */
    struct field f[4];                              /* r, g, b, a */
    int alpha;                                      /* a mask for it */
    size_t pixels;                                  /* where they start */
};

/* A mask's place: it must be one run of bits */
static int field_of(struct field *f, unsigned int mask) {
    f->mask = mask;
    f->shift = f->bits = 0;
    if(!mask) return 0;
    while(!(mask & 1)) {
        mask >>= 1;
        f->shift++;
    }
    while(mask & 1) {
        mask >>= 1;
        f->bits++;
    }
    return mask ? -1 : 0;
}

/* The channel of pixel v, in 8 bits */
static unsigned int channel(const struct field *f, unsigned int v) {
    unsigned int c;
    if(!f->bits) return 0;
    c = (v & f->mask) >> f->shift;
    if(f->bits >= 8) return c >> (f->bits - 8);
    return c * 255 / ((1u << f->bits) - 1);
}

static unsigned int pixel_of(const struct bmp *b, unsigned int v) {
    unsigned int t = b->alpha ? 255 - channel(&b->f[3], v) : 0;
    return t << 24 | channel(&b->f[0], v) << 16 | channel(&b->f[1], v) << 8 | channel(&b->f[2], v);
}

/* The header at offset 14: b's fields, or why not */
static const char *header(struct bmp *b) {
    const unsigned char *h = b->in + 14;
    unsigned int hsize, planes, used = 0, psize = 4, entries, i, masks[4] = { 0, 0, 0, 0 };
    size_t palette;
    u64 end, off;
    long long height;
    if(b->len < 18) return "a BMP that ends inside its headers";
    hsize = le32(h);
    if(hsize != 12 && hsize != 40 && hsize != 52 && hsize != 56 && hsize != 64 && hsize != 108 && hsize != 124)
        return "a BMP with a header of an unknown size";
    if((u64)14 + hsize > b->len) return "a BMP that ends inside its headers";
    if(hsize == 12) {
        b->w = (int)le16(h + 4);
        height = le16(h + 6);
        planes = le16(h + 8);
        b->bpp = (int)le16(h + 10);
        psize = 3;
        b->comp = BI_RGB;
    } else {
        unsigned int w = le32(h + 4), hh = le32(h + 8);
        if(w > 0x7fffffffu) return "a BMP of a negative width";
        if(hh == 0x80000000u) return "a BMP of no possible height";
        b->w = (int)w;
        height = (int)hh;
        planes = le16(h + 12);
        b->bpp = (int)le16(h + 14);
        b->comp = le32(h + 16);
        used = le32(h + 32);
    }
    if(height < 0) {
        b->topdown = 1;
        height = -height;
    }
    if(height > 0x7fffffff) return "a BMP of no possible height";
    b->h = (int)height;
    if(planes != 1) return "a BMP that is not of one plane";
    if(b->w <= 0 || b->h <= 0) return "a BMP of no size";
    switch(b->bpp) {
    case 1: case 2: case 4: case 8: case 16: case 24: case 32: break;
    default: return "a BMP of an unknown pixel size";
    }
    palette = 14 + (size_t)hsize;
    if(hsize == 64 && b->comp > BI_RLE4) return "a BMP in an OS/2 compression";
    switch(b->comp) {
    case BI_RGB:
        break;
    case BI_RLE8:
        if(b->bpp != 8) return "an RLE8 BMP that is not of 8 bits";
        if(b->topdown) return "an RLE BMP with its rows from the top";
        break;
    case BI_RLE4:
        if(b->bpp != 4) return "an RLE4 BMP that is not of 4 bits";
        if(b->topdown) return "an RLE BMP with its rows from the top";
        break;
    case BI_BITFIELDS: case BI_ALPHAFIELDS: {
        unsigned int n = b->comp == BI_ALPHAFIELDS ? 4 : 3;
        const unsigned char *m = h + 40;            /* in the header from V2 on, else right after it */
        if(b->bpp != 16 && b->bpp != 32) return "a BMP of bit fields that is not of 16 or 32 bits";
        if(hsize == 40) {
            if(palette + (u64)n * 4 > b->len) return "a BMP that ends inside its headers";
            palette += (size_t)n * 4;
        } else if(hsize < 52 + (n == 4 ? 4u : 0u)) return "a BMP whose masks do not fit its header";
        for(i = 0; i < n; i++) masks[i] = le32(m + i * 4);
        if(hsize >= 56 && b->comp == BI_BITFIELDS) masks[3] = le32(m + 12);
        break;
    }
    case 4: case 5:
        return "a BMP with a JPEG or a PNG inside: not read";
    default:
        return "a BMP in an unknown compression";
    }
    if(b->comp == BI_RGB || b->comp == BI_RLE8 || b->comp == BI_RLE4) {
        if(b->bpp == 16) {
            masks[0] = 0x7c00;
            masks[1] = 0x03e0;
            masks[2] = 0x001f;
        } else if(b->bpp >= 24) {
            masks[0] = 0xff0000;
            masks[1] = 0xff00;
            masks[2] = 0xff;
        }
    }
    for(i = 0; i < 4; i++) if(field_of(&b->f[i], masks[i])) return "a BMP whose mask is not one run of bits";
    b->alpha = masks[3] != 0;
    /* the palette, when the pixels have one */
    if(b->bpp <= 8) {
        entries = used ? used : 1u << b->bpp;
        if(entries > 256 || entries > 1u << b->bpp) entries = 1u << b->bpp;
        end = palette + (u64)entries * psize;
        if(end > b->len) return "a BMP that ends inside its palette";
        for(i = 0; i < entries; i++) {
            const unsigned char *e = b->in + palette + (size_t)i * psize;
            b->palette[i] = (unsigned int)e[2] << 16 | (unsigned int)e[1] << 8 | e[0];
        }
        palette = (size_t)end;
    }
    off = le32(b->in + 10);
    if(!off) off = palette;                         /* a writer that left it out */
    if(off < palette || off > b->len) return "a BMP whose pixels are not in the file";
    b->pixels = (size_t)off;
    return NULL;
}

/* Plain rows: each row's pixels must be in the file; the last row's padding may be missing */
static const char *plain(struct bmp *b, struct wimg *m) {
    u64 rowbits = (u64)(unsigned int)b->w * (unsigned int)b->bpp, rowlen = (rowbits + 31) / 32 * 4;
    u64 need = rowlen * (unsigned int)(b->h - 1) + (rowbits + 7) / 8;
    int x, y;
    if(need > b->len - b->pixels) return "a BMP that ends inside its pixels";
    for(y = 0; y < b->h; y++) {
        const unsigned char *p = b->in + b->pixels + (size_t)rowlen * (size_t)y;
        unsigned int *row = m->px + (size_t)(b->topdown ? y : b->h - 1 - y) * b->w;
        switch(b->bpp) {
        case 1: case 2: case 4: case 8: {
            int bits = b->bpp;
            unsigned int mask = (1u << bits) - 1;
            for(x = 0; x < b->w; x++) {
                unsigned int at = (unsigned int)x * (unsigned int)bits;
                row[x] = b->palette[p[at >> 3] >> (8 - bits - (at & 7)) & mask];
            }
            break;
        }
        case 16:
            for(x = 0; x < b->w; x++) row[x] = pixel_of(b, le16(p + (size_t)x * 2));
            break;
        case 24:
            for(x = 0; x < b->w; x++, p += 3) row[x] = (unsigned int)p[2] << 16 | (unsigned int)p[1] << 8 | p[0];
            break;
        default:
            for(x = 0; x < b->w; x++) row[x] = pixel_of(b, le32(p + (size_t)x * 4));
            break;
        }
    }
    return NULL;
}

/* A file with an alpha mask in which every pixel says 0 was not written with alpha in mind */
static void alpha_or_none(const struct bmp *b, struct wimg *m) {
    size_t i, n = (size_t)m->w * m->h;
    if(!b->alpha) return;
    for(i = 0; i < n; i++) if((m->px[i] >> 24) != 255) return;
    for(i = 0; i < n; i++) m->px[i] &= 0xffffff;
}

/* RLE4 and RLE8: the file's codes in pairs, the pixels from the bottom row up; pixels past a row's end are dropped */
static const char *rle(struct bmp *b, struct wimg *m) {
    const unsigned char *p = b->in + b->pixels, *end = b->in + b->len;
    int four = b->comp == BI_RLE4, x = 0, y = 0;
    size_t i, n = (size_t)m->w * m->h;
    for(i = 0; i < n; i++) m->px[i] = WIMG_GONE;
#define PUT(v) do { if(x < b->w && y < b->h) m->px[(size_t)(b->h - 1 - y) * b->w + x] = b->palette[v]; x++; } while(0)
    for(;;) {
        unsigned int a, c;
        int k;
        if(end - p < 2) return "a BMP whose RLE codes end too early";
        a = p[0];
        c = p[1];
        p += 2;
        if(a) {                                     /* a run */
            for(k = 0; k < (int)a; k++) {
                if(four) PUT(k & 1 ? c & 15 : c >> 4);
                else PUT(c);
            }
            continue;
        }
        if(c == 0) {                                /* the row's end */
            x = 0;
            y++;
        } else if(c == 1) return NULL;              /* the picture's end */
        else if(c == 2) {                           /* a step */
            if(end - p < 2) return "a BMP whose RLE codes end too early";
            x += p[0];
            y += p[1];
            p += 2;
        } else {                                    /* c pixels as they are, kept to whole words */
            size_t bytes = four ? ((size_t)c + 1) / 2 : c, pad = bytes & 1;
            if((size_t)(end - p) < bytes + pad) return "a BMP whose RLE codes end too early";
            for(k = 0; k < (int)c; k++) {
                if(four) PUT(k & 1 ? p[k / 2] & 15 : p[k / 2] >> 4);
                else PUT(p[k]);
            }
            p += bytes + pad;
        }
        if(y > b->h) return NULL;                   /* past the top: nothing more can be drawn */
    }
#undef PUT
}

int wimg_bmp(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap) {
    struct bmp *b = calloc(1, sizeof(*b));
    const char *why;
    if(!b) return ut_error(err, cap, "out of memory reading a BMP");
    b->in = in;
    b->len = len;
    why = header(b);
    if(why) {
        free(b);
        return ut_error(err, cap, "%s", why);
    }
    if(wimg_alloc(out, b->w, b->h, err, cap)) {
        free(b);
        return -1;
    }
    why = b->comp == BI_RLE4 || b->comp == BI_RLE8 ? rle(b, out) : plain(b, out);
    if(why) {
        free(b);
        return wimg_fail(out, err, cap, why);
    }
    alpha_or_none(b, out);
    free(b);
    return 0;
}
