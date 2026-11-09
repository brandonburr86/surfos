/*
SurfOS user library: PNG pictures
----------------------
File: image_png.c  Date: 11/1/26
----------------------
PNG: every colour type and depth of the standard, a palette and tRNS, Adam7 interlacing, 16 bits per sample kept to
their high 8. An animated PNG shows its default image. A chunk is checked whole by its CRC when it is reached, if it
is one that is read: IHDR first, PLTE, IDAT; an unknown critical chunk stops the picture, an ancillary one is passed
over. Three things a real file may do are let by, as browsers let them by, with nothing at risk: a palette longer
than the depth holds is cut, a tRNS that makes no sense or fails its CRC is ignored, and inflate stops at the first
byte past the last row. A stream that ends badly before that byte is refused, though: its blocks' ends and its
Adler-32 are checked when rows and stream end together, as they do in any file that is whole.

Nothing is held of the file's data but what the format needs: the inflate's 32 KB window, a few KB of what it made,
and two rows. Bytes go from inflate into the row being made; each whole row is unfiltered and turned into pixels. So
a small file that says it inflates to gigabytes costs nothing: the bytes a picture of its size holds are known from
its header, and inflate is given room for one byte past them and no more. Its input is the IDAT chunks one after
the other, however they are cut, each checked by its CRC when inflate needs its first byte.

11/05/26 (the desktop applications, track img): moved here from user/wundertow/, and its inflate is libsurf's
(inflate.c, RFC 1951), so that the tree has one; the CRC is libsurf's too (crc32.c). What it reads is as before,
but for a deflate stream whose Huffman codes have room left over (an incomplete code, RFC 1951 section 3.2.7) or no
code for a block's end: the old inflate went on with such a stream until a code fell into the hole, libsurf's
refuses it at once, as zlib does, so libpng's readers never show such a file either.
11/05/26 (the review of track img): the data's input is bounded too. A stream that makes nothing for long (empty
blocks, each with its tables) made a small picture cost seconds; now the IDAT bytes inflate is given stop at twice the
rows' bytes, 8 more a row and 4 KB (data_budget()): deflate's worst, stored blocks, adds 5 bytes to 64 KB, and an
encoder that flushes at every row 6 a row, so no real file comes near it.
11/09/26 (the review's second reading): and the input that makes nothing. A picture large enough has a budget no file
of 32 MiB reaches, so empty blocks behind its header still cost seconds; now inflate is refused once QUIET_MAX bytes
of the data have gone by without its making a byte. Between two bytes a real stream has a block's header (a dynamic
one's tables are under 300 bytes) or a flush (5), so 64 KB is far past anything real.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <crc32.h>
#include <inflate.h>
#include <image.h>

#define MADE     4096                           /* inflate's output at a time, on its way into rows */
#define QUIET_MAX 65536                         /* the data's bytes inflate may take without making one */
#define WORDS(x) #x
#define WORD(x)  WORDS(x)
#define OVER_SIDE "a PNG over the limit of " WORD(WIMG_SIDE_MAX) " pixels a side"

struct png {
    const unsigned char *in;
    size_t len, pos;                            /* the place in the file */
    size_t left;                                /* bytes left of this IDAT */
    int ended;                                  /* the IDAT chunks ended, or the next was not whole */
    int w, h, depth, color, interlaced;
    int channels;
    unsigned int bytes;                         /* the distance of the filters: a pixel's bytes, at least 1 */
    unsigned int palette[256];                  /* 0xTTRRGGBB; zeroed: an index past the palette is black */
    int colors;
    int keyed;                                  /* tRNS for gray or RGB: this sample's colour is not there */
    unsigned int key[3];
    struct wimg *m;
    /* the rows */
    unsigned char *row, *above;
    size_t rowbytes;                            /* of the row being made */
    size_t fill;                                /* its bytes so far, the filter's type byte first */
    int filter;
    int pass, y, pw, ph;                        /* the Adam7 pass (0-6, or 0), the row in it, its size */
    int done;                                   /* the last row was made */
    int over;                                   /* a byte past it was made: the picture is whole */
    unsigned long long ahead;                   /* the bytes inflate may still make: the rows' and one more */
    unsigned long long budget, taken;           /* the IDAT bytes inflate may be given, and has been */
    size_t quiet;                               /* those it has taken since it last made a byte */
    unsigned int adler_a, adler_b;
    unsigned char made[MADE];
    struct inflate z;
};

static const unsigned char pass_x0[7] = { 0, 4, 0, 2, 0, 1, 0 }, pass_y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const unsigned char pass_dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, pass_dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

static unsigned int be32(const unsigned char *p) {
    return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | (unsigned int)p[2] << 8 | p[3];
}

/**** the chunks ****/

/* The chunk at pos: its length and type; pos moves to its data. 0, or -1 (the file ends first) */
static int chunk(struct png *p, unsigned int *length, const unsigned char **type) {
    unsigned int n;
    if(p->len - p->pos < 12) return -1;
    n = be32(p->in + p->pos);
    if(n > 0x7fffffffu || n > p->len - p->pos - 12) return -1;
    *length = n;
    *type = p->in + p->pos + 4;
    p->pos += 8;
    return 0;
}

/* Is the CRC of the chunk just reached (of n bytes at pos) right? It covers the chunk's type and data */
static int crc_right(const struct png *p, unsigned int n) {
    return crc32(0, p->in + p->pos - 4, (size_t)n + 4) == be32(p->in + p->pos + n);
}

/* The next IDAT chunk once this one's bytes are used (one of no bytes is passed over): 0, or -1 where they end */
static int next_chunk(struct png *p) {
    while(!p->left) {
        unsigned int n;
        const unsigned char *t;
        if(p->ended) return -1;
        p->pos += 4;                            /* the CRC of the one before: head() and chunk() saw it is there */
        if(chunk(p, &n, &t) || memcmp(t, "IDAT", 4) || !crc_right(p, n)) {
            p->ended = 1;
            return -1;
        }
        p->left = n;
    }
    return 0;
}

/* The next byte of the IDAT chunks, or -1 where they end */
static int next_byte(struct png *p) {
    if(next_chunk(p)) return -1;
    p->left--;
    return p->in[p->pos++];
}

/**** rows into pixels ****/

/* Sample number x of a row of depth bits */
static unsigned int sample(const unsigned char *r, unsigned int x, int depth) {
    unsigned int at;
    switch(depth) {
    case 16: return (unsigned int)r[(size_t)x * 2] << 8 | r[(size_t)x * 2 + 1];
    case 8: return r[x];
    }
    at = x * (unsigned int)depth;
    return r[at >> 3] >> (8 - depth - (at & 7)) & ((1u << depth) - 1);
}

/* A sample in 8 bits */
static unsigned int level(unsigned int v, int depth) {
    return depth == 16 ? v >> 8 : depth == 8 ? v : v * 255 / ((1u << depth) - 1);
}

static void row_pixels(struct png *p) {
    const unsigned char *r = p->row;
    int i = p->interlaced ? p->pass : 6, step = p->interlaced ? pass_dx[i] : 1, x;
    unsigned int *out = p->m->px + (size_t)(p->interlaced ? pass_y0[i] + p->y * pass_dy[i] : p->y) * p->w;
    int d = p->depth, x0 = p->interlaced ? pass_x0[i] : 0;
    for(x = 0; x < p->pw; x++) {
        unsigned int c, n = (unsigned int)x * (unsigned int)p->channels, v;
        switch(p->color) {
        case 0:
            v = sample(r, n, d);
            c = level(v, d) * 0x010101u;
            if(p->keyed && v == p->key[0]) c = WIMG_GONE | c;
            break;
        case 2: {
            unsigned int a = sample(r, n, d), b = sample(r, n + 1, d), e = sample(r, n + 2, d);
            c = level(a, d) << 16 | level(b, d) << 8 | level(e, d);
            if(p->keyed && a == p->key[0] && b == p->key[1] && e == p->key[2]) c |= WIMG_GONE;
            break;
        }
        case 3:
            c = p->palette[sample(r, n, d)];
            break;
        case 4:
            c = level(sample(r, n, d), d) * 0x010101u | (255 - level(sample(r, n + 1, d), d)) << 24;
            break;
        default:
            c = level(sample(r, n, d), d) << 16 | level(sample(r, n + 1, d), d) << 8 | level(sample(r, n + 2, d), d) |
                (255 - level(sample(r, n + 3, d), d)) << 24;
            break;
        }
        out[x0 + x * step] = c;
    }
}

/* The rows the next pass has (a small picture has passes with none), or the end */
static void start_pass(struct png *p) {
    size_t bits;
    p->y = 0;
    p->fill = 0;
    if(p->interlaced) {
        for(;;) {
            if(++p->pass == 7) {
                p->done = 1;
                return;
            }
            p->pw = (p->w - pass_x0[p->pass] + pass_dx[p->pass] - 1) / pass_dx[p->pass];
            p->ph = (p->h - pass_y0[p->pass] + pass_dy[p->pass] - 1) / pass_dy[p->pass];
            if(p->pw > 0 && p->ph > 0) break;
        }
    } else {
        p->pw = p->w;
        p->ph = p->h;
    }
    bits = (size_t)p->pw * (size_t)p->channels * (size_t)p->depth;
    p->rowbytes = (bits + 7) / 8;
    memset(p->above, 0, p->rowbytes);
}

static int paeth(int a, int b, int c) {
    int e = a + b - c, pa = e > a ? e - a : a - e, pb = e > b ? e - b : b - e, pc = e > c ? e - c : c - e;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static void row_done(struct png *p) {
    unsigned char *r = p->row, *up = p->above, *t;
    size_t i, n = p->rowbytes, d = p->bytes;
    switch(p->filter) {
    case 1:
        for(i = d; i < n; i++) r[i] = (unsigned char)(r[i] + r[i - d]);
        break;
    case 2:
        for(i = 0; i < n; i++) r[i] = (unsigned char)(r[i] + up[i]);
        break;
    case 3:
        for(i = 0; i < n; i++) r[i] = (unsigned char)(r[i] + (((i >= d ? r[i - d] : 0) + up[i]) >> 1));
        break;
    case 4:
        for(i = 0; i < n; i++)
            r[i] = (unsigned char)(r[i] + paeth(i >= d ? r[i - d] : 0, up[i], i >= d ? up[i - d] : 0));
        break;
    }
    row_pixels(p);
    t = p->row;
    p->row = p->above;
    p->above = t;
    p->fill = 0;
    if(++p->y == p->ph) {
        if(p->interlaced) start_pass(p);
        else p->done = 1;
    }
}

/* One byte of the data inflate makes: 0, or -1 for a row whose filter is none */
static int emit(struct png *p, unsigned int c) {
    p->adler_a += c;
    if(p->adler_a >= 65521) p->adler_a -= 65521;
    p->adler_b += p->adler_a;
    if(p->adler_b >= 65521) p->adler_b -= 65521;
    if(p->done) {                               /* more than the picture holds */
        p->over = 1;
        return 0;
    }
    if(!p->fill) {
        if(c > 4) return -1;
        p->filter = (int)c;
        p->fill = 1;
        return 0;
    }
    p->row[p->fill++ - 1] = (unsigned char)c;
    if(p->fill == p->rowbytes + 1) row_done(p);
    return 0;
}

/**** the data ****/

/* The bytes of every row with its filter's: those of the passes that have pixels, or of the picture */
static unsigned long long data_bytes(const struct png *p) {
    unsigned long long n = 0;
    int i;
    for(i = p->interlaced ? 0 : 6; i < 7; i++) {
        int pw = p->interlaced ? (p->w - pass_x0[i] + pass_dx[i] - 1) / pass_dx[i] : p->w;
        int ph = p->interlaced ? (p->h - pass_y0[i] + pass_dy[i] - 1) / pass_dy[i] : p->h;
        if(pw > 0 && ph > 0)
            n += (unsigned long long)ph * (1 + ((unsigned long long)pw * (unsigned int)p->channels * (unsigned int)p->depth + 7) / 8);
    }
    return n;
}

/* The IDAT bytes the rows may come from: twice theirs, 8 a row (the passes' rows, for Adam7) and 4 KB */
static unsigned long long data_budget(const struct png *p) {
    unsigned long long rows = 0;
    int i;
    for(i = p->interlaced ? 0 : 6; i < 7; i++) {
        int pw = p->interlaced ? (p->w - pass_x0[i] + pass_dx[i] - 1) / pass_dx[i] : p->w;
        int ph = p->interlaced ? (p->h - pass_y0[i] + pass_dy[i] - 1) / pass_dy[i] : p->h;
        if(pw > 0 && ph > 0) rows += (unsigned long long)ph;
    }
    return 2 * data_bytes(p) + 8 * rows + 4096;
}

/* The zlib stream in the IDAT chunks (RFC 1950): its header, the deflate data by libsurf's inflate, as far as one byte
   past the rows, and its Adler-32 when the rows and the data end together. Inflate is given no more of the chunks
   than the budget, and no more than QUIET_MAX at a time while it makes nothing: a stream that wants more is refused */
static const char *data(struct png *p) {
    struct inflate *z = &p->z;
    int cmf = next_byte(p), flg = next_byte(p), i;
    unsigned int adler;
    if(cmf < 0 || flg < 0 || (cmf & 15) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flg) % 31 || (flg & 32))
        return "a PNG whose data is not zlib's";
    inflate_init(z);
    p->ahead = data_bytes(p) + 1;
    p->budget = data_budget(p);
    p->taken = 2;                               /* the zlib header */
    for(;;) {
        size_t room = p->ahead < MADE ? (size_t)p->ahead : MADE, made, k, give = p->left, used;
        int r;
        if(p->taken > p->budget) return "a PNG whose data is far longer than its rows";
        if(p->quiet > QUIET_MAX) return "a PNG whose data makes nothing for too long";
        if(give > p->budget - p->taken + 1) give = (size_t)(p->budget - p->taken + 1);
        if(give > QUIET_MAX - p->quiet + 1) give = QUIET_MAX - p->quiet + 1;
        z->in = p->in + p->pos;
        z->in_n = give;
        z->out = p->made;
        z->out_n = room;
        r = inflate_run(z);
        used = give - z->in_n;
        p->pos += used;
        p->left -= used;
        p->taken += used;
        made = room - z->out_n;
        p->quiet = made ? 0 : p->quiet + used;
        p->ahead -= made;
        for(k = 0; k < made; k++) if(emit(p, p->made[k])) return "a PNG whose data is corrupted";
        if(p->over) return NULL;                /* the picture is whole: what follows is not read */
        if(r == INFLATE_BAD) return "a PNG whose data is corrupted";
        if(r == INFLATE_END) break;
        if(!z->out_n) continue;                 /* the room was used: more of it */
        if(p->left) continue;                   /* what it was given was used: more of this chunk, within the budget */
        if(next_chunk(p)) return "a PNG that ends too early";
    }
    if(!p->done) return "a PNG that ends too early";
    for(adler = 0, i = 0; i < 4; i++) {
        int c = next_byte(p);
        if(c < 0) return "a PNG that ends too early";
        adler = adler << 8 | (unsigned int)c;
    }
    if(adler != (p->adler_b << 16 | p->adler_a)) return "a PNG whose data is corrupted";
    return NULL;
}

/**** the file ****/

static const char *ihdr(struct png *p, const unsigned char *d, unsigned int n) {
    unsigned int w, h;
    int color, depth;
    if(n != 13) return "a PNG with a bad header";
    w = be32(d);
    h = be32(d + 4);
    depth = d[8];
    color = d[9];
    if(!w || !h || w > 0x7fffffffu || h > 0x7fffffffu) return "a PNG of no size";
    if(d[10] || d[11] || d[12] > 1) return "a PNG of an unknown kind";
    switch(color) {
    case 0: if(depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return "a PNG of a depth that is none"; p->channels = 1; break;
    case 2: if(depth != 8 && depth != 16) return "a PNG of a depth that is none"; p->channels = 3; break;
    case 3: if(depth != 1 && depth != 2 && depth != 4 && depth != 8) return "a PNG of a depth that is none"; p->channels = 1; break;
    case 4: if(depth != 8 && depth != 16) return "a PNG of a depth that is none"; p->channels = 2; break;
    case 6: if(depth != 8 && depth != 16) return "a PNG of a depth that is none"; p->channels = 4; break;
    default: return "a PNG of a colour type that is none";
    }
    if(w > WIMG_SIDE_MAX || h > WIMG_SIDE_MAX) return OVER_SIDE;
    p->w = (int)w;
    p->h = (int)h;
    p->depth = depth;
    p->color = color;
    p->interlaced = d[12];
    p->bytes = (unsigned int)(p->channels * depth + 7) / 8;
    return NULL;
}

static const char *plte(struct png *p, const unsigned char *d, unsigned int n) {
    unsigned int i;
    if(p->color != 3) return NULL;              /* a suggestion for screens of few colours */
    if(n % 3 || !n) return "a PNG with a bad palette";
    n /= 3;
    if(n > 1u << p->depth) n = 1u << p->depth;  /* the rest no pixel can name */
    for(i = 0; i < n; i++) p->palette[i] = (unsigned int)d[i * 3] << 16 | (unsigned int)d[i * 3 + 1] << 8 | d[i * 3 + 2];
    p->colors = (int)n;
    return NULL;
}

/* Transparency is a nicety: a tRNS that makes no sense is passed over */
static void trns(struct png *p, const unsigned char *d, unsigned int n) {
    unsigned int i;
    switch(p->color) {
    case 0:
        if(n < 2) return;
        p->key[0] = (unsigned int)d[0] << 8 | d[1];
        p->keyed = 1;
        break;
    case 2:
        if(n < 6) return;
        for(i = 0; i < 3; i++) p->key[i] = (unsigned int)d[i * 2] << 8 | d[i * 2 + 1];
        p->keyed = 1;
        break;
    case 3:
        if(!p->colors || n > (unsigned int)p->colors) return;
        for(i = 0; i < n; i++) p->palette[i] |= (unsigned int)(255 - d[i]) << 24;
        break;
    }
}

/* The chunks up to the first IDAT; p->left is then its length */
static const char *head(struct png *p) {
    unsigned int n;
    const unsigned char *t;
    int first = 1;
    p->pos = 8;
    for(;;) {
        const char *why = NULL;
        if(chunk(p, &n, &t)) return "a PNG that ends too early";
        if(first != !memcmp(t, "IHDR", 4)) return "a PNG whose first chunk is not IHDR";
        if(!(t[0] & 0x20) && !crc_right(p, n)) return "a corrupted PNG";
        if(!memcmp(t, "IHDR", 4)) why = ihdr(p, p->in + p->pos, n);
        else if(!memcmp(t, "PLTE", 4)) why = plte(p, p->in + p->pos, n);
        else if(!memcmp(t, "tRNS", 4)) {
            if(crc_right(p, n)) trns(p, p->in + p->pos, n);
        } else if(!memcmp(t, "IDAT", 4)) {
            if(p->color == 3 && !p->colors) return "a PNG of colours with no palette";
            p->left = n;
            return NULL;
        } else if(!memcmp(t, "IEND", 4)) return "a PNG with no picture";
        else if(!(t[0] & 0x20)) return "a PNG with a chunk it must not be read without";
        if(why) return why;
        first = 0;
        p->pos += (size_t)n + 4;
    }
}

int wimg_png(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap) {
    struct png *p = calloc(1, sizeof(*p));
    const char *why;
    size_t row;
    if(!p) return wimg_error(err, cap, "out of memory reading a PNG");
    p->in = in;
    p->len = len;
    p->m = out;
    p->adler_a = 1;
    p->pass = -1;
    why = head(p);
    if(why) {
        free(p);
        return wimg_error(err, cap, "%s", why);
    }
    if(wimg_alloc(out, p->w, p->h, err, cap)) {
        free(p);
        return -1;
    }
    row = ((size_t)p->w * (size_t)p->channels * (size_t)p->depth + 7) / 8;
    p->row = malloc(row);
    p->above = malloc(row);
    if(!p->row || !p->above) {
        free(p->row);
        free(p->above);
        free(p);
        return wimg_fail(out, err, cap, "out of memory reading a PNG");
    }
    start_pass(p);
    why = data(p);
    free(p->row);
    free(p->above);
    free(p);
    if(why) return wimg_fail(out, err, cap, why);
    return 0;
}
