/*
SurfOS user library: GIF pictures
----------------------
File: image_gif.c  Date: 11/1/26
----------------------
GIF87a and GIF89a, the first frame: its colour table (its own, else the file's), its transparent colour, interlacing,
its place on the logical screen. What the frame does not cover is not there; a screen smaller than the frame grows
to hold it, as browsers do. The background colour is not used, as browsers do not. The frame must be whole: data that
ends before its last pixel is no picture. After it, nothing of the file is read.

LZW (of 12 bits at most) goes into a table of 4096 strings, each a prefix code and its last byte. A string is written
backwards from its end, its length known, so nothing is held but the table.

11/05/26 (the desktop applications, track img): moved here from user/wundertow/ as it was.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <image.h>

#define CODES 4096

struct gif {
    const unsigned char *in;
    size_t len, pos;
    unsigned int colors[256];                       /* zeroed: what a table leaves out is black */
    int transparent;                                /* a colour index, or -1 */
    /* the frame */
    int fx, fy, fw, fh, interlaced;
    /* where the next pixel goes */
    int x, y, pass;
    size_t left;                                    /* pixels still to come */
    /* the codes' sub-blocks */
    size_t block;                                   /* bytes left in this one */
    int ended;                                      /* its end was reached */
    unsigned int bits;
    int nbits;
    /* LZW's table */
    unsigned short prefix[CODES];
    unsigned short length[CODES];
    unsigned char suffix[CODES];
    unsigned char string[CODES + 1];
};

static unsigned int le16(const unsigned char *p) { return (unsigned int)p[0] | (unsigned int)p[1] << 8; }

static const char short_file[] = "a GIF that ends too early";

/* n bytes of a table at the reading place into colors[]: 0, or -1 */
static int table(struct gif *g, unsigned int n) {
    unsigned int i;
    if(g->len - g->pos < (size_t)n * 3) return -1;
    memset(g->colors, 0, sizeof(g->colors));
    for(i = 0; i < n; i++) {
        const unsigned char *c = g->in + g->pos + (size_t)i * 3;
        g->colors[i] = (unsigned int)c[0] << 16 | (unsigned int)c[1] << 8 | c[2];
    }
    g->pos += (size_t)n * 3;
    return 0;
}

/* Past a chain of sub-blocks: 0, or -1 */
static int skip_blocks(struct gif *g) {
    for(;;) {
        size_t n;
        if(g->pos >= g->len) return -1;
        n = g->in[g->pos++];
        if(!n) return 0;
        if(g->len - g->pos < n) return -1;
        g->pos += n;
    }
}

/* One more byte of the codes, or -1 at the end of the sub-blocks */
static int code_byte(struct gif *g) {
    while(!g->block) {
        if(g->ended || g->pos >= g->len) return -1;
        g->block = g->in[g->pos++];
        if(!g->block) {
            g->ended = 1;
            return -1;
        }
    }
    if(g->pos >= g->len) return -1;
    g->block--;
    return g->in[g->pos++];
}

/* A code of size bits, or -1 at the end of the data */
static int next_code(struct gif *g, int size) {
    int code;
    while(g->nbits < size) {
        int c = code_byte(g);
        if(c < 0) return -1;
        g->bits |= (unsigned int)c << g->nbits;
        g->nbits += 8;
    }
    code = (int)(g->bits & ((1u << size) - 1));
    g->bits >>= size;
    g->nbits -= size;
    return code;
}

static void next_row(struct gif *g) {
    static const unsigned char step[4] = { 8, 8, 4, 2 }, start[4] = { 0, 4, 2, 1 };
    g->x = 0;
    if(!g->interlaced) {
        g->y++;
        return;
    }
    g->y += step[g->pass];
    while(g->y >= g->fh && g->pass < 3) {           /* a frame of few rows has passes with none */
        g->pass++;
        g->y = start[g->pass];
    }
}

/* n bytes of colour indices as the next pixels of the frame; a frame ends when it is full */
static void put(struct gif *g, struct wimg *m, const unsigned char *s, size_t n) {
    size_t i;
    for(i = 0; i < n && g->left; i++) {
        if(g->y < g->fh) {
            if(s[i] != g->transparent) m->px[(size_t)(g->fy + g->y) * m->w + g->fx + g->x] = g->colors[s[i]];
        }
        g->left--;
        if(++g->x == g->fw) next_row(g);
    }
}

/* The frame's LZW data */
static const char *lzw(struct gif *g, struct wimg *m) {
    int root, clear, stop, size, first, avail, prev = -1, i;
    unsigned char top = 0;
    if(g->pos >= g->len) return short_file;
    root = g->in[g->pos++];
    if(root < 2 || root > 8) return "a GIF of a code size that is none";
    clear = 1 << root;
    stop = clear + 1;
    first = stop + 1;
    size = root + 1;
    avail = first;
    for(i = 0; i < clear; i++) {
        g->suffix[i] = (unsigned char)i;
        g->length[i] = 1;
        g->prefix[i] = 0;
    }
    while(g->left) {
        int code = next_code(g, size), c, n;
        if(code < 0) return "a GIF whose picture data ends too early";
        if(code == stop) return "a GIF whose picture data ends too early";
        if(code == clear) {
            size = root + 1;
            avail = first;
            prev = -1;
            continue;
        }
        if(prev < 0) {                              /* the first code after a clear: one of the roots */
            if(code >= clear) return "a GIF with a code it has not made";
            top = g->suffix[code];
            put(g, m, g->suffix + code, 1);
            prev = code;
            continue;
        }
        if(code > avail) return "a GIF with a code it has not made";
        c = code == avail ? prev : code;            /* KwKwK: the string just made, not yet in the table */
        n = g->length[c] + (code == avail ? 1 : 0);
        i = n;
        if(code == avail) g->string[--i] = top;
        while(i > 0) {
            g->string[--i] = g->suffix[c];
            c = g->prefix[c];
        }
        top = g->string[0];
        put(g, m, g->string, (size_t)n);
        if(avail < CODES) {
            g->prefix[avail] = (unsigned short)prev;
            g->suffix[avail] = top;
            g->length[avail] = (unsigned short)(g->length[prev] + 1);
            avail++;
            if(avail == 1 << size && size < 12) size++;
        }
        prev = code;
    }
    return NULL;
}

/* The file's blocks up to the first frame, which is decoded */
static const char *frames(struct gif *g, struct wimg *m, int sw, int sh, char *err, size_t cap, int *said) {
    int global = 0;
    size_t i, n;
    g->transparent = -1;
    if(g->len < 13) return short_file;
    {
        unsigned int packed = g->in[10];
        g->pos = 13;
        if(packed & 0x80) {
            if(table(g, 2u << (packed & 7))) return short_file;
            global = 1;
        }
    }
    for(;;) {
        unsigned int kind, packed;
        if(g->pos >= g->len) return short_file;
        kind = g->in[g->pos++];
        if(kind == 0x3b) return "a GIF with no picture";
        if(kind == 0x21) {                          /* an extension: only the graphic control's is looked at */
            if(g->pos >= g->len) return short_file;
            kind = g->in[g->pos++];
            if(kind == 0xf9 && g->len - g->pos >= 6 && g->in[g->pos] == 4)
                g->transparent = g->in[g->pos + 1] & 1 ? g->in[g->pos + 4] : -1;
            if(skip_blocks(g)) return short_file;
            continue;
        }
        if(kind != 0x2c) return "a GIF with a block of no known kind";
        if(g->len - g->pos < 9) return short_file;
        g->fx = (int)le16(g->in + g->pos);
        g->fy = (int)le16(g->in + g->pos + 2);
        g->fw = (int)le16(g->in + g->pos + 4);
        g->fh = (int)le16(g->in + g->pos + 6);
        packed = g->in[g->pos + 8];
        g->pos += 9;
        if(!g->fw || !g->fh) return "a GIF frame of no size";
        g->interlaced = (packed & 0x40) != 0;
        if(packed & 0x80) {
            if(table(g, 2u << (packed & 7))) return short_file;
        } else if(!global) return "a GIF with no colours";
        if(g->fx + g->fw > sw) sw = g->fx + g->fw;
        if(g->fy + g->fh > sh) sh = g->fy + g->fh;
        if(wimg_alloc(m, sw, sh, err, cap)) {
            *said = 1;
            return "";
        }
        n = (size_t)sw * sh;
        for(i = 0; i < n; i++) m->px[i] = WIMG_GONE;
        g->left = (size_t)g->fw * g->fh;
        return lzw(g, m);
    }
}

int wimg_gif(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap) {
    struct gif *g = malloc(sizeof(*g));
    const char *why;
    int said = 0;
    if(!g) return wimg_error(err, cap, "out of memory reading a GIF");
    memset(g, 0, offsetof(struct gif, prefix));
    g->in = in;
    g->len = len;
    why = len < 13 ? short_file : frames(g, out, (int)le16(in + 6), (int)le16(in + 8), err, cap, &said);
    free(g);
    if(said) return -1;
    if(why) return wimg_fail(out, err, cap, why);
    return 0;
}
