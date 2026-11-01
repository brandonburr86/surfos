/*
SurfOS Undertow's window: PNG pictures
----------------------
File: image_png.c  Date: 11/1/26
----------------------
PNG, with an inflate of its own: every colour type and depth of the standard, a palette and tRNS, Adam7 interlacing,
16 bits per sample kept to their high 8. An animated PNG shows its default image. A chunk is checked whole by its CRC
when it is reached, if it is one that is read: IHDR first, PLTE, IDAT; an unknown critical chunk stops the picture, an
ancillary one is passed over. Three things a real file may do are let by, as browsers let them by, with nothing at risk:
a palette longer than the depth holds is cut, a tRNS that makes no sense or fails its CRC is ignored, and inflate stops
at the first byte past the last row. A stream that ends badly before that byte is refused, though: its blocks' ends
and its Adler-32 are checked when rows and stream end together, as they do in any file that is whole.

Nothing is held of the file's data but what the format needs: the inflate's 32 KB window, and two rows. Bytes come
out of inflate one at a time into the row being made; each whole row is unfiltered and turned into pixels. So a
small file that says it inflates to gigabytes costs nothing: the bytes a picture of its size can hold are known from
its header, and inflate stops at the first one past them. Inflate's input is the IDAT chunks one after the other, a
byte at a time, however they are cut.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "undertow.h"
#include "image.h"

#define WINDOW   32768u
#define FAST     9                              /* bits of a code that the quick table covers */
#define LITS     288
#define DISTS    32
#define WORDS(x) #x
#define WORD(x)  WORDS(x)
#define OVER_SIDE "a PNG over the limit of " WORD(WIMG_SIDE_MAX) " pixels a side"

struct huff {
    unsigned short fast[1 << FAST];             /* the low bits of the input: length << 9 | symbol; 0: a longer code */
    unsigned int limit[17];                     /* codes of s bits are below limit[s], left aligned to 16 bits */
    unsigned short first[16];                   /* the first code of each length */
    unsigned short index[16];                   /* its place in sym[] */
    unsigned short sym[LITS];
    unsigned char len[LITS];
};

struct png {
    const unsigned char *in;
    size_t len, pos;                            /* the place in the file */
    size_t left;                                /* bytes left of this IDAT */
    int ended;                                  /* the IDAT chunks ended, or the next was not whole */
    unsigned int crc[256];
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
    /* inflate */
    unsigned int bits;
    int nbits;
    int bad;                                    /* the data ended, or a row's filter is none */
    int made_up;                                /* of nbits, how many are zeros made up past the data's end */
    int over;                                   /* data past the last row: inflate stops */
    unsigned int wpos;
    unsigned long long out;                     /* bytes made */
    unsigned int adler_a, adler_b;
    int fixed_tables;                           /* lit and dist hold the fixed code (many short blocks may use it) */
    unsigned char window[WINDOW];
    struct huff lit, dist;
};

static const unsigned char pass_x0[7] = { 0, 4, 0, 2, 0, 1, 0 }, pass_y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const unsigned char pass_dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, pass_dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

static unsigned int be32(const unsigned char *p) {
    return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | (unsigned int)p[2] << 8 | p[3];
}

/**** the chunks ****/

static void crc_table(struct png *p) {
    unsigned int c, i;
    int k;
    for(i = 0; i < 256; i++) {
        c = i;
        for(k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        p->crc[i] = c;
    }
}

/* CRC-32 of a chunk's type and data */
static unsigned int crc_of(const struct png *p, const unsigned char *s, size_t n) {
    unsigned int c = 0xffffffffu;
    while(n--) c = p->crc[(c ^ *s++) & 255] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

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

/* Is the CRC of the chunk just reached (of n bytes at pos) right? */
static int crc_right(const struct png *p, unsigned int n) {
    return crc_of(p, p->in + p->pos - 4, (size_t)n + 4) == be32(p->in + p->pos + n);
}

/* The next byte of the IDAT chunks, or -1 where they end */
static int next_byte(struct png *p) {
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

/* One byte of the data inflate makes */
static void emit(struct png *p, unsigned int c) {
    p->window[p->wpos] = (unsigned char)c;
    p->wpos = (p->wpos + 1) & (WINDOW - 1);
    p->out++;
    p->adler_a += c;
    if(p->adler_a >= 65521) p->adler_a -= 65521;
    p->adler_b += p->adler_a;
    if(p->adler_b >= 65521) p->adler_b -= 65521;
    if(p->done) {                               /* more than the picture holds */
        p->over = 1;
        return;
    }
    if(!p->fill) {
        if(c > 4) {
            p->bad = 1;
            return;
        }
        p->filter = (int)c;
        p->fill = 1;
        return;
    }
    p->row[p->fill++ - 1] = (unsigned char)c;
    if(p->fill == p->rowbytes + 1) row_done(p);
}

/**** inflate ****/

static void refill(struct png *p, int n) {
    while(p->nbits < n) {
        int c = next_byte(p);
        if(c < 0) {
            p->made_up += 8;
            c = 0;
        }
        p->bits |= (unsigned int)c << p->nbits;
        p->nbits += 8;
    }
}

/* n bits used: a code looks further ahead than it is long, so it is the bits used that may run out, not those looked at */
static void used(struct png *p, int n) {
    p->bits >>= n;
    p->nbits -= n;
    if(p->nbits < p->made_up) p->bad = 1;
}

static unsigned int take(struct png *p, int n) {
    unsigned int v;
    if(!n) return 0;
    refill(p, n);
    v = p->bits & ((1u << n) - 1);
    used(p, n);
    return v;
}

static unsigned int reverse(unsigned int v, int n) {
    unsigned int r = 0;
    while(n--) {
        r = r << 1 | (v & 1);
        v >>= 1;
    }
    return r;
}

/* The table of a code given by each symbol's length: 0, or -1 for a code that is over-full */
static int build(struct huff *h, const unsigned char *lens, int n) {
    int count[16], next[16], i, s, k = 0;
    unsigned int code = 0;
    memset(count, 0, sizeof(count));
    memset(h->fast, 0, sizeof(h->fast));
    for(i = 0; i < n; i++) count[lens[i]]++;
    count[0] = 0;
    for(s = 1; s < 16; s++) {
        next[s] = (int)code;
        h->first[s] = (unsigned short)code;
        h->index[s] = (unsigned short)k;
        code += (unsigned int)count[s];
        if(code > 1u << s) return -1;
        h->limit[s] = code << (16 - s);
        code <<= 1;
        k += count[s];
    }
    h->limit[16] = 0x10000;
    for(i = 0; i < n; i++) {
        s = lens[i];
        if(!s) continue;
        k = next[s] - h->first[s] + h->index[s];
        h->len[k] = (unsigned char)s;
        h->sym[k] = (unsigned short)i;
        if(s <= FAST) {
            unsigned int j = reverse((unsigned int)next[s], s);
            for(; j < 1u << FAST; j += 1u << s) h->fast[j] = (unsigned short)(s << 9 | i);
        }
        next[s]++;
    }
    return 0;
}

/* The next symbol, or -1 for a code that is none */
static int symbol(struct png *p, const struct huff *h) {
    unsigned int e, k, i;
    int s;
    refill(p, 15);
    e = h->fast[p->bits & ((1u << FAST) - 1)];
    if(e) {
        used(p, (int)(e >> 9));
        return (int)(e & 511);
    }
    k = reverse(p->bits & 0xffff, 16);
    for(s = FAST + 1; s < 16; s++) if(k < h->limit[s]) break;
    if(s >= 16) return -1;
    i = (k >> (16 - s)) - h->first[s] + h->index[s];
    if(i >= LITS || h->len[i] != s) return -1;
    used(p, s);
    return h->sym[i];
}

static const char *stored(struct png *p) {
    unsigned int n, nn;
    take(p, p->nbits & 7);
    n = take(p, 16);
    nn = take(p, 16);
    if((n ^ nn) != 0xffff) return "a PNG whose data is corrupted";
    while(n-- && !p->bad && !p->over) emit(p, take(p, 8));
    return NULL;
}

static const char *codes(struct png *p) {
    static const unsigned short base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                             67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const unsigned char more[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
                                            5, 5, 5, 0 };
    static const unsigned short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                              769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const unsigned char dmore[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                             11, 11, 12, 12, 13, 13 };
    for(;;) {
        unsigned int n, d;
        int s;
        if(p->over) return NULL;
        s = symbol(p, &p->lit);
        if(s < 0 || p->bad) return "a PNG whose data is corrupted";
        if(s < 256) {
            emit(p, (unsigned int)s);
            continue;
        }
        if(s == 256) return NULL;
        if(s > 285) return "a PNG whose data is corrupted";
        n = base[s - 257] + take(p, more[s - 257]);
        s = symbol(p, &p->dist);
        if(s < 0 || s > 29) return "a PNG whose data is corrupted";
        d = dbase[s] + take(p, dmore[s]);
        if(d > p->out || p->bad) return "a PNG whose data is corrupted";
        while(n-- && !p->bad && !p->over) emit(p, p->window[(p->wpos - d) & (WINDOW - 1)]);
    }
}

static const char *fixed(struct png *p) {
    unsigned char lens[LITS];
    if(p->fixed_tables) return codes(p);
    p->fixed_tables = 1;
    memset(lens, 8, 144);
    memset(lens + 144, 9, 112);
    memset(lens + 256, 7, 24);
    memset(lens + 280, 8, 8);
    build(&p->lit, lens, LITS);
    memset(lens, 5, DISTS);
    build(&p->dist, lens, DISTS);
    return codes(p);
}

static const char *dynamic(struct png *p) {
    static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    unsigned char lens[LITS + DISTS];
    int nlit = (int)take(p, 5) + 257, ndist = (int)take(p, 5) + 1, nlen = (int)take(p, 4) + 4, i, n;
    p->fixed_tables = 0;
    if(nlit > 286 || ndist > 30 || p->bad) return "a PNG whose data is corrupted";
    memset(lens, 0, 19);
    for(i = 0; i < nlen; i++) lens[order[i]] = (unsigned char)take(p, 3);
    if(build(&p->lit, lens, 19)) return "a PNG whose data is corrupted";
    for(n = 0; n < nlit + ndist; ) {
        int s = symbol(p, &p->lit), times, v = 0;
        if(s < 0 || p->bad) return "a PNG whose data is corrupted";
        if(s < 16) {
            lens[n++] = (unsigned char)s;
            continue;
        }
        if(s == 16) {
            if(!n) return "a PNG whose data is corrupted";
            v = lens[n - 1];
            times = 3 + (int)take(p, 2);
        } else if(s == 17) times = 3 + (int)take(p, 3);
        else times = 11 + (int)take(p, 7);
        if(n + times > nlit + ndist) return "a PNG whose data is corrupted";
        while(times--) lens[n++] = (unsigned char)v;
    }
    if(build(&p->lit, lens, nlit) || build(&p->dist, lens + nlit, ndist)) return "a PNG whose data is corrupted";
    return codes(p);
}

static const char *inflate(struct png *p) {
    unsigned int cmf, flg, adler;
    int final, i;
    cmf = take(p, 8);
    flg = take(p, 8);
    if(p->bad || (cmf & 15) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flg) % 31 || (flg & 32))
        return "a PNG whose data is not zlib's";
    do {
        const char *why;
        unsigned int type;
        final = (int)take(p, 1);
        type = take(p, 2);
        if(p->bad) return "a PNG that ends too early";
        why = type == 0 ? stored(p) : type == 1 ? fixed(p) : type == 2 ? dynamic(p) : "a PNG whose data is corrupted";
        if(why) return why;
        if(p->over) return NULL;
        if(p->bad) return "a PNG whose data is corrupted";
    } while(!final);
    if(!p->done) return "a PNG that ends too early";
    take(p, p->nbits & 7);
    for(adler = 0, i = 0; i < 4; i++) adler = adler << 8 | take(p, 8);
    if(p->bad) return "a PNG that ends too early";
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
    if(!p) return ut_error(err, cap, "out of memory reading a PNG");
    p->in = in;
    p->len = len;
    p->m = out;
    p->adler_a = 1;
    p->pass = -1;
    crc_table(p);
    why = head(p);
    if(why) {
        free(p);
        return ut_error(err, cap, "%s", why);
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
    why = inflate(p);
    free(p->row);
    free(p->above);
    free(p);
    if(why) return wimg_fail(out, err, cap, why);
    return 0;
}
