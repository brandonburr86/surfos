/*
SurfOS Undertow's window: JPEG pictures
----------------------
File: image_jpeg.c  Date: 11/1/26
----------------------
JPEG (ITU T.81): baseline, extended sequential and progressive, Huffman coded, 8 bits, of 1 component (gray), 3 (YCbCr,
or RGB by Adobe's marker or by the components' names) or 4 (CMYK, or YCCK, with Adobe's inversion), any sampling of 1
to 4, restart intervals, several scans. Arithmetic coding, 12 bits, lossless and hierarchical files are refused.
Chroma is widened by repeating it. The EXIF orientation is not looked at.

A sequential block goes through the inverse DCT as it is read, into its component's plane; a progressive one is kept
as coefficients (WIMG_COEFS_MAX bytes at most) until every scan is in. The planes are as large as the picture and
are checked against its limits first.

The file is hostile. The numbers its codes give are clamped before they are multiplied, so that no sum can overflow
in 32 bits (dequant(), idct()): the limits lie well above what any picture of 8 bits can hold. The input ending is
a corrupted file, which is found at the end of each block, so a scan's time is bounded by its bytes. Its scans are
WIMG_SCANS_MAX at most, so that a file of many empty scans cannot keep the helper for long.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "undertow.h"
#include "image.h"

#define FAST        9                           /* bits of a code that the quick table covers */
#define COEF_MAX    4095                        /* a coefficient, dequantized: 8-bit samples stay near 1200 */
#define PASS1_MAX   8191                        /* the first pass's output: 8-bit samples stay near 4100 */

struct table {                                  /* a Huffman table */
    unsigned short fast[1 << FAST];             /* the input's next bits: length << 8 | value; 0: a longer code */
    unsigned int limit[18];                     /* codes of s bits are below limit[s], left aligned to 16 bits */
    int delta[18];                              /* code + delta[s]: its place in value[] */
    unsigned char value[256];
    int n;                                      /* values; 0: not defined */
};

struct comp {
    int id, hs, vs, tq;
    int dc, ac;                                 /* its tables in this scan */
    int bw, bh;                                 /* its blocks across and down, to whole MCUs */
    int nw, nh;                                 /* those a scan of it alone covers */
    int pred;                                   /* the DC prediction */
    int scanned;
    unsigned char *plane;                       /* bw * 8 x bh * 8 samples */
    short *coef;                                /* progressive: bw * bh blocks of 64, in zigzag order */
};

struct jpeg {
    const unsigned char *in;
    size_t len, pos;
    /* the frame */
    int w, h, ncomp, hmax, vmax, mcux, mcuy;
    int sof, progressive;
    struct comp comp[4];
    unsigned short quant[4][64];                /* in zigzag order */
    unsigned char have_quant[4];
    struct table dc[4], ac[4];
    int restart;                                /* MCUs between restart markers, 0: none */
    int adobe, transform;
    /* the scan */
    int scans;
    int ss, se, ah, al;
    int eobrun;
    unsigned int buf;                           /* bits to come, at its top */
    int cnt;                                    /* how many */
    int pad;                                    /* of those, how many are zeros made up after the data ended */
    int marker;                                 /* the marker the data ran into, 0: none yet */
    int eoi;
    /* the output */
    unsigned char *rows[4];                     /* a row of the picture of each component, widened */
};

static const unsigned char zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63
};

static unsigned int be16(const unsigned char *p) { return (unsigned int)p[0] << 8 | p[1]; }

/**** the inverse DCT: libjpeg's accurate integer method, as the Loeffler, Ligtenberg and Moschytz paper has it ****/

#define F298   2446
#define F390   3196
#define F541   4433
#define F765   6270
#define F899   7373
#define F1175  9633
#define F1501  12299
#define F1847  15137
#define F1961  16069
#define F2053  16819
#define F2562  20995
#define F3072  25172

static int clamp_to(int v, int m) { return v > m ? m : v < -m ? -m : v; }

/* One column or row: s[0 .. 7 * step] in, each result before the descaling in out[8] */
static void idct1(const int *s, int step, int *out) {
    int z1, z2, z3, z4, z5, t0, t1, t2, t3, t10, t11, t12, t13;
    z2 = s[2 * step];
    z3 = s[6 * step];
    z1 = (z2 + z3) * F541;
    t2 = z1 - z3 * F1847;
    t3 = z1 + z2 * F765;
    t0 = (s[0] + s[4 * step]) * 8192;
    t1 = (s[0] - s[4 * step]) * 8192;
    t10 = t0 + t3;
    t13 = t0 - t3;
    t11 = t1 + t2;
    t12 = t1 - t2;
    t0 = s[7 * step];
    t1 = s[5 * step];
    t2 = s[3 * step];
    t3 = s[step];
    z1 = t0 + t3;
    z2 = t1 + t2;
    z3 = t0 + t2;
    z4 = t1 + t3;
    z5 = (z3 + z4) * F1175;
    t0 *= F298;
    t1 *= F2053;
    t2 *= F3072;
    t3 *= F1501;
    z1 *= -F899;
    z2 *= -F2562;
    z3 = z3 * -F1961 + z5;
    z4 = z4 * -F390 + z5;
    t0 += z1 + z3;
    t1 += z2 + z4;
    t2 += z2 + z3;
    t3 += z1 + z4;
    out[0] = t10 + t3;
    out[7] = t10 - t3;
    out[1] = t11 + t2;
    out[6] = t11 - t2;
    out[2] = t12 + t1;
    out[5] = t12 - t1;
    out[3] = t13 + t0;
    out[4] = t13 - t0;
}

/* c: 64 coefficients within COEF_MAX, in the order of the block; 8 x 8 samples into out, rows stride apart */
static void idct(const int *c, unsigned char *out, int stride) {
    int ws[64], r[8], x, y;
    for(x = 0; x < 8; x++) {                    /* columns: magnitudes up to 2^30 in 32 bits */
        if(!c[x + 8] && !c[x + 16] && !c[x + 24] && !c[x + 32] && !c[x + 40] && !c[x + 48] && !c[x + 56]) {
            int dc = clamp_to(c[x] * 4, PASS1_MAX);
            for(y = 0; y < 8; y++) ws[y * 8 + x] = dc;
            continue;
        }
        idct1(c + x, 8, r);
        for(y = 0; y < 8; y++) ws[y * 8 + x] = clamp_to((r[y] + 1024) >> 11, PASS1_MAX);
    }
    for(y = 0; y < 8; y++) {                    /* rows, of values within PASS1_MAX: again under 2^31 */
        idct1(ws + y * 8, 1, r);
        for(x = 0; x < 8; x++) {
            int v = ((r[x] + (1 << 17)) >> 18) + 128;
            out[x] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        out += stride;
    }
}

/* A coefficient as read, by its quantizer: within COEF_MAX whatever a file says */
static int dequant(int v, unsigned int q) {
    return clamp_to(clamp_to(v, COEF_MAX) * (int)q, COEF_MAX);
}

/**** bits ****/

static void fill(struct jpeg *j) {
    while(j->cnt <= 24) {
        unsigned int b = 0;
        if(j->marker) j->pad += 8;
        else if(j->pos >= j->len) {
            j->marker = 0x100;                  /* the file's end */
            j->pad += 8;
        } else {
            b = j->in[j->pos++];
            if(b == 0xff) {
                unsigned int c = 0xff;
                while(c == 0xff && j->pos < j->len) c = j->in[j->pos++];
                if(c == 0xff) {
                    j->marker = 0x100;
                    b = 0;
                    j->pad += 8;
                } else if(c) {                  /* a marker: the scan's data ends here */
                    j->marker = (int)c;
                    b = 0;
                    j->pad += 8;
                }
            }
        }
        j->buf |= b << (24 - j->cnt);
        j->cnt += 8;
    }
}

static unsigned int get_bits(struct jpeg *j, int n) {
    unsigned int v;
    if(!n) return 0;
    if(j->cnt < n) fill(j);
    v = j->buf >> (32 - n);
    j->buf <<= n;
    j->cnt -= n;
    return v;
}

/* s bits as the value they stand for (T.81 F.2.2.1) */
static int extend(struct jpeg *j, int s) {
    int v;
    if(!s) return 0;
    v = (int)get_bits(j, s);
    return v < 1 << (s - 1) ? v - (1 << s) + 1 : v;
}

/* The next Huffman code's value, or -1 for a code that is none */
static int code(struct jpeg *j, const struct table *t) {
    unsigned int e, k;
    int s, i;
    if(j->cnt < 16) fill(j);
    e = t->fast[j->buf >> (32 - FAST)];
    if(e) {
        s = (int)(e >> 8);
        j->buf <<= s;
        j->cnt -= s;
        return (int)(e & 255);
    }
    k = j->buf >> 16;
    for(s = FAST + 1; s <= 16; s++) if(k < t->limit[s]) break;
    if(s > 16) return -1;
    i = (int)(k >> (16 - s)) + t->delta[s];
    if(i < 0 || i >= t->n) return -1;
    j->buf <<= s;
    j->cnt -= s;
    return t->value[i];
}

/* Has the scan used bits that were not in the file? */
static int ran_out(const struct jpeg *j) { return j->cnt < j->pad; }

/**** the segments ****/

static const char *dqt(struct jpeg *j, const unsigned char *d, size_t n) {
    while(n) {
        unsigned int pq = d[0] >> 4, tq = d[0] & 15, i;
        size_t need = 1 + (pq ? 128 : 64);
        if(pq > 1 || tq > 3 || n < need) return "a JPEG with a bad quantization table";
        for(i = 0; i < 64; i++) j->quant[tq][i] = (unsigned short)(pq ? be16(d + 1 + i * 2) : d[1 + i]);
        j->have_quant[tq] = 1;
        d += need;
        n -= need;
    }
    return NULL;
}

static const char *dht(struct jpeg *j, const unsigned char *d, size_t n) {
    while(n) {
        unsigned int tc = d[0] >> 4, th = d[0] & 15, next = 0;
        struct table *t;
        int s, k = 0, total = 0;
        if(tc > 1 || th > 3 || n < 17) return "a JPEG with a bad Huffman table";
        for(s = 1; s <= 16; s++) total += d[s];
        if(total > 256 || n < (size_t)17 + (size_t)total) return "a JPEG with a bad Huffman table";
        t = tc ? &j->ac[th] : &j->dc[th];
        memset(t->fast, 0, sizeof(t->fast));
        memcpy(t->value, d + 17, (size_t)total);
        t->n = total;
        for(s = 1; s <= 16; s++) {
            int c = d[s], i;
            t->delta[s] = k - (int)next;
            for(i = 0; i < c; i++, k++, next++) {
                if(next >= 1u << s) return "a JPEG with a bad Huffman table";
                if(s <= FAST) {
                    unsigned int first = next << (FAST - s), e;
                    for(e = 0; e < 1u << (FAST - s); e++) t->fast[first + e] = (unsigned short)(s << 8 | t->value[k]);
                }
            }
            t->limit[s] = next << (16 - s);
            next <<= 1;
        }
        t->limit[17] = 0xffffffffu;
        d += 17 + total;
        n -= 17 + (size_t)total;
    }
    return NULL;
}

static const char *sof(struct jpeg *j, const unsigned char *d, size_t n, char *err, size_t cap, struct wimg *m, int *said) {
    int i;
    unsigned long long pixels;
    if(j->sof) return "a JPEG with two frames";
    if(n < 6) return "a JPEG with a bad frame header";
    if(d[0] != 8) return "a JPEG of other than 8 bits";
    j->h = (int)be16(d + 1);
    j->w = (int)be16(d + 3);
    j->ncomp = d[5];
    if(!j->h) return "a JPEG whose height comes after its data: not read";
    if(!j->w) return "a JPEG of no width";
    if(j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4) return "a JPEG of a number of components that is not read";
    if(n < (size_t)6 + (size_t)j->ncomp * 3) return "a JPEG with a bad frame header";
    for(i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        const unsigned char *e = d + 6 + i * 3;
        c->id = e[0];
        c->hs = e[1] >> 4;
        c->vs = e[1] & 15;
        c->tq = e[2];
        if(c->hs < 1 || c->hs > 4 || c->vs < 1 || c->vs > 4 || c->tq > 3) return "a JPEG with a bad frame header";
        if(c->hs > j->hmax) j->hmax = c->hs;
        if(c->vs > j->vmax) j->vmax = c->vs;
    }
    if(wimg_alloc(m, j->w, j->h, err, cap)) {
        *said = 1;
        return "";
    }
    j->mcux = (j->w + j->hmax * 8 - 1) / (j->hmax * 8);
    j->mcuy = (j->h + j->vmax * 8 - 1) / (j->vmax * 8);
    pixels = 0;
    for(i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        c->bw = j->mcux * c->hs;
        c->bh = j->mcuy * c->vs;
        c->nw = ((j->w * c->hs + j->hmax - 1) / j->hmax + 7) / 8;
        c->nh = ((j->h * c->vs + j->vmax - 1) / j->vmax + 7) / 8;
        pixels += (unsigned long long)(unsigned int)c->bw * (unsigned int)c->bh;
    }
    if(j->progressive && pixels * 64 * sizeof(short) > WIMG_COEFS_MAX) return "a progressive JPEG too large to hold";
    for(i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        size_t blocks = (size_t)c->bw * (size_t)c->bh;
        c->plane = calloc(blocks, 64);
        if(!c->plane) return "out of memory reading a JPEG";
        if(j->progressive) {
            c->coef = calloc(blocks, 64 * sizeof(short));
            if(!c->coef) return "out of memory reading a JPEG";
        }
        j->rows[i] = malloc((size_t)j->w);
        if(!j->rows[i]) return "out of memory reading a JPEG";
    }
    j->sof = 1;
    return NULL;
}

/**** blocks ****/

/* The DC difference, added to the component's prediction */
static int dc_value(struct jpeg *j, struct comp *c, int *value) {
    int t = code(j, &j->dc[c->dc]);
    if(t < 0 || t > 15) return -1;
    c->pred = clamp_to(c->pred + extend(j, t), 65535);
    *value = c->pred;
    return 0;
}

/* A block of a sequential file, into its plane */
static const char *sequential(struct jpeg *j, struct comp *c, int bx, int by) {
    static const char corrupt[] = "a JPEG whose data is corrupted";
    const unsigned short *q = j->quant[c->tq];
    const struct table *ac = &j->ac[c->ac];
    int f[64], k, dc;
    if(dc_value(j, c, &dc)) return corrupt;
    memset(f, 0, sizeof(f));
    f[0] = dequant(dc, q[0]);
    for(k = 1; k < 64; k++) {
        int rs = code(j, ac), r, s;
        if(rs < 0) return corrupt;
        r = rs >> 4;
        s = rs & 15;
        if(!s) {
            if(r < 15) break;
            k += 15;
            continue;
        }
        k += r;
        if(k > 63) return corrupt;
        f[zigzag[k]] = dequant(extend(j, s), q[k]);
    }
    idct(f, c->plane + ((size_t)by * c->bw * 64 + (size_t)bx * 8), c->bw * 8);
    return NULL;
}

/* The bits of a progressive block: DC scans */
static const char *dc_first(struct jpeg *j, struct comp *c, short *b) {
    int v;
    if(dc_value(j, c, &v)) return "a JPEG whose data is corrupted";
    b[0] = (short)clamp_to(v * (1 << j->al), 32767);
    return NULL;
}

static const char *dc_refine(struct jpeg *j, short *b) {
    if(get_bits(j, 1)) b[0] = (short)(b[0] | 1 << j->al);
    return NULL;
}

/* AC scans of one band: first pass and refinement, as T.81 G.1.2 has them */
static const char *ac_first(struct jpeg *j, const struct table *t, short *b) {
    int k;
    if(j->eobrun) {
        j->eobrun--;
        return NULL;
    }
    for(k = j->ss; k <= j->se; k++) {
        int rs = code(j, t), r, s;
        if(rs < 0) return "a JPEG whose data is corrupted";
        r = rs >> 4;
        s = rs & 15;
        if(!s) {
            if(r < 15) {
                j->eobrun = (1 << r) - 1 + (int)get_bits(j, r);
                break;
            }
            k += 15;
            continue;
        }
        k += r;
        if(k > 63) return "a JPEG whose data is corrupted";
        b[k] = (short)clamp_to(extend(j, s) * (1 << j->al), 32767);
    }
    return NULL;
}

static void correct(struct jpeg *j, short *c, int p1) {
    if(get_bits(j, 1) && !(*c & p1)) *c = (short)(*c >= 0 ? *c + p1 : *c - p1);
}

static const char *ac_refine(struct jpeg *j, const struct table *t, short *b) {
    int p1 = 1 << j->al, k = j->ss;
    if(!j->eobrun) {
        for(; k <= j->se; k++) {
            int rs = code(j, t), r, s, v = 0;
            if(rs < 0) return "a JPEG whose data is corrupted";
            r = rs >> 4;
            s = rs & 15;
            if(s) {
                v = get_bits(j, 1) ? p1 : -p1;
            } else if(r < 15) {
                j->eobrun = (1 << r) + (int)get_bits(j, r);
                break;
            }
            for(; k <= j->se; k++) {            /* past r zeros, correcting what is not zero */
                if(b[k]) correct(j, &b[k], p1);
                else {
                    if(!r) break;
                    r--;
                }
            }
            if(v) {
                if(k > 63) return "a JPEG whose data is corrupted";
                b[k] = (short)v;
            }
        }
    }
    if(j->eobrun) {
        for(; k <= j->se; k++) if(b[k]) correct(j, &b[k], p1);
        j->eobrun--;
    }
    return NULL;
}

static const char *block(struct jpeg *j, struct comp *c, int bx, int by) {
    short *b;
    if(!j->progressive) return sequential(j, c, bx, by);
    b = c->coef + ((size_t)by * c->bw + bx) * 64;
    if(!j->ss) return j->ah ? dc_refine(j, b) : dc_first(j, c, b);
    return j->ah ? ac_refine(j, &j->ac[c->ac], b) : ac_first(j, &j->ac[c->ac], b);
}

/**** scans ****/

/* At a restart: the marker, wherever it is from here; the bits and the predictions start again */
static const char *next_restart(struct jpeg *j) {
    int i;
    j->buf = 0;
    j->cnt = 0;
    j->pad = 0;
    j->eobrun = 0;
    for(i = 0; i < j->ncomp; i++) j->comp[i].pred = 0;
    if(!j->marker) {
        while(j->pos + 1 < j->len && !(j->in[j->pos] == 0xff && j->in[j->pos + 1] && j->in[j->pos + 1] != 0xff)) j->pos++;
        if(j->pos + 1 >= j->len) return "a JPEG that ends too early";
        j->marker = j->in[j->pos + 1];
        j->pos += 2;
    }
    if(j->marker < 0xd0 || j->marker > 0xd7) return "a JPEG whose data ends before a restart marker";
    j->marker = 0;
    return NULL;
}

static const char *scan_data(struct jpeg *j, const int *which, int n) {
    int interleaved = n > 1, total, count, left = j->restart;
    int across = interleaved ? j->mcux : j->comp[which[0]].nw;
    int down = interleaved ? j->mcuy : j->comp[which[0]].nh;
    int x, y;
    j->buf = 0;
    j->cnt = 0;
    j->pad = 0;
    j->marker = 0;
    j->eobrun = 0;
    for(x = 0; x < j->ncomp; x++) j->comp[x].pred = 0;
    total = across * down;
    count = 0;
    for(y = 0; y < down; y++)
        for(x = 0; x < across; x++) {
            const char *why;
            int i;
            for(i = 0; i < n; i++) {
                struct comp *c = &j->comp[which[i]];
                if(interleaved) {
                    int h, v;
                    for(v = 0; v < c->vs; v++)
                        for(h = 0; h < c->hs; h++) {
                            why = block(j, c, x * c->hs + h, y * c->vs + v);
                            if(why) return why;
                        }
                } else {
                    why = block(j, c, x, y);
                    if(why) return why;
                }
            }
            if(ran_out(j)) return "a JPEG that ends too early";
            count++;
            if(j->restart && !--left && count < total) {
                why = next_restart(j);
                if(why) return why;
                left = j->restart;
            }
        }
    return NULL;
}

/* SOS: its header, then the data; j->pos is then past what was read of it */
static const char *sos(struct jpeg *j, const unsigned char *d, size_t n) {
    int which[4], ns, i, k;
    if(!j->sof) return "a JPEG with a scan before its frame";
    if(++j->scans > WIMG_SCANS_MAX) return "a JPEG of too many scans";
    if(n < 1) return "a JPEG with a bad scan header";
    ns = d[0];
    if(ns < 1 || ns > j->ncomp || n < (size_t)1 + (size_t)ns * 2 + 3) return "a JPEG with a bad scan header";
    for(i = 0; i < ns; i++) {
        int id = d[1 + i * 2], tables = d[2 + i * 2];
        for(k = 0; k < j->ncomp; k++) if(j->comp[k].id == id) break;
        if(k == j->ncomp) return "a JPEG with a scan of a component it does not have";
        which[i] = k;
        j->comp[k].dc = tables >> 4;
        j->comp[k].ac = tables & 15;
        if(j->comp[k].dc > 3 || j->comp[k].ac > 3) return "a JPEG with a bad scan header";
    }
    j->ss = d[1 + ns * 2];
    j->se = d[2 + ns * 2];
    j->ah = d[3 + ns * 2] >> 4;
    j->al = d[3 + ns * 2] & 15;
    if(j->progressive) {
        if(j->ss > j->se || j->se > 63 || j->ah > 13 || j->al > 13) return "a JPEG with a bad scan header";
        if(!j->ss && j->se) return "a JPEG with a bad scan header";     /* DC comes alone */
        if(j->ss && ns != 1) return "a JPEG with a bad scan header";    /* AC comes one component at a time */
    } else {                                    /* what a sequential file says of these means nothing */
        j->ss = 0;
        j->se = 63;
        j->ah = j->al = 0;
    }
    for(i = 0; i < ns; i++) {
        struct comp *c = &j->comp[which[i]];
        if(!j->progressive || !j->ss) {
            if((!j->progressive || !j->ah) && !j->dc[c->dc].n) return "a JPEG with a Huffman table it does not define";
        }
        if((!j->progressive || j->ss) && !j->ac[c->ac].n) return "a JPEG with a Huffman table it does not define";
        if(!j->progressive && !j->have_quant[c->tq]) return "a JPEG with a quantization table it does not define";
        c->scanned = 1;
    }
    return scan_data(j, which, ns);
}

/* The next marker's code, from where the reading is; -1 at the file's end */
static int next_marker(struct jpeg *j) {
    if(j->marker && j->marker != 0x100) {
        int m = j->marker;
        j->marker = 0;
        return m;
    }
    for(;;) {
        while(j->pos < j->len && j->in[j->pos] != 0xff) j->pos++;
        while(j->pos < j->len && j->in[j->pos] == 0xff) j->pos++;
        if(j->pos >= j->len) return -1;
        if(j->in[j->pos]) return j->in[j->pos++];
        j->pos++;                               /* a stuffed FF: data that belongs to nobody */
    }
}

/* The segments, up to EOI or the file's end */
static const char *segments(struct jpeg *j, char *err, size_t cap, struct wimg *m, int *said) {
    j->pos = 2;
    for(;;) {
        int k = next_marker(j);
        size_t n;
        const unsigned char *d;
        const char *why = NULL;
        if(k < 0) return NULL;
        if(k == 0xd9) {
            j->eoi = 1;
            return NULL;
        }
        if(k == 0xd8 || k == 0x01 || (k >= 0xd0 && k <= 0xd7)) continue;   /* no segment; RST out of place */
        if(j->len - j->pos < 2) return "a JPEG that ends too early";
        n = be16(j->in + j->pos);
        if(n < 2 || n > j->len - j->pos) return "a JPEG that ends too early";
        d = j->in + j->pos + 2;
        n -= 2;
        j->pos += n + 2;
        switch(k) {
        case 0xc0: case 0xc1: case 0xc2:
            j->progressive = k == 0xc2;
            why = sof(j, d, n, err, cap, m, said);
            break;
        case 0xc3: case 0xc5: case 0xc6: case 0xc7: case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xcf:
            return "a JPEG of a kind that is not read (arithmetic, lossless or hierarchical)";
        case 0xc4:
            why = dht(j, d, n);
            break;
        case 0xcc:
            return "a JPEG of a kind that is not read (arithmetic)";
        case 0xda:
            why = sos(j, d, n);
            break;
        case 0xdb:
            why = dqt(j, d, n);
            break;
        case 0xdd:
            if(n < 2) return "a JPEG with a bad restart interval";
            j->restart = (int)be16(d);
            break;
        case 0xee:
            if(n >= 12 && !memcmp(d, "Adobe", 5)) {
                j->adobe = 1;
                j->transform = d[11];
            }
            break;
        }
        if(why) return why;
    }
}

/**** the picture ****/

/* The coefficients of a progressive file become samples */
static const char *progressive_planes(struct jpeg *j) {
    int i, x, y, k;
    for(i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        const unsigned short *q = j->quant[c->tq];
        if(!j->have_quant[c->tq]) return "a JPEG with a quantization table it does not define";
        for(y = 0; y < c->bh; y++)
            for(x = 0; x < c->bw; x++) {
                const short *b = c->coef + ((size_t)y * c->bw + x) * 64;
                int f[64];
                memset(f, 0, sizeof(f));
                for(k = 0; k < 64; k++) if(b[k]) f[zigzag[k]] = dequant(b[k], q[k]);
                idct(f, c->plane + ((size_t)y * c->bw * 64 + (size_t)x * 8), c->bw * 8);
            }
        free(c->coef);
        c->coef = NULL;
    }
    return NULL;
}

static unsigned char sat(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* Row y of the picture, each component widened to it */
static void widen(struct jpeg *j, int y) {
    int i, x;
    for(i = 0; i < j->ncomp; i++) {
        const struct comp *c = &j->comp[i];
        const unsigned char *s = c->plane + (size_t)(y * c->vs / j->vmax) * (size_t)c->bw * 8;
        unsigned char *r = j->rows[i];
        if(c->hs == j->hmax) memcpy(r, s, (size_t)j->w);
        else for(x = 0; x < j->w; x++) r[x] = s[x * c->hs / j->hmax];
    }
}

static void ycc(int y, int cb, int cr, int *r, int *g, int *b) {
    cb -= 128;
    cr -= 128;
    *r = y + ((91881 * cr + 32768) >> 16);
    *g = y - ((22554 * cb + 46802 * cr + 32768) >> 16);
    *b = y + ((116130 * cb + 32768) >> 16);
}

static const char *pixels(struct jpeg *j, struct wimg *m) {
    int rgb = j->ncomp == 3 && ((j->adobe && !j->transform) ||
              (!j->adobe && j->comp[0].id == 'R' && j->comp[1].id == 'G' && j->comp[2].id == 'B'));
    int ycck = j->ncomp == 4 && j->adobe && j->transform == 2, x, y;
    for(x = 0; x < j->ncomp; x++) if(!j->comp[x].scanned) return "a JPEG that ends too early";
    for(y = 0; y < j->h; y++) {
        unsigned int *out = m->px + (size_t)y * j->w;
        const unsigned char *a = j->rows[0], *b = j->rows[1], *c = j->rows[2], *d = j->rows[3];
        widen(j, y);
        for(x = 0; x < j->w; x++) {
            int r, g, e;
            if(j->ncomp == 1) out[x] = a[x] * 0x010101u;
            else if(j->ncomp == 3) {
                if(rgb) {
                    r = a[x];
                    g = b[x];
                    e = c[x];
                } else ycc(a[x], b[x], c[x], &r, &g, &e);
                out[x] = (unsigned int)sat(r) << 16 | (unsigned int)sat(g) << 8 | sat(e);
            } else {                            /* Adobe keeps CMYK inverted: each is how much of the paper shows */
                int k = j->adobe ? d[x] : 255 - d[x];
                if(ycck) ycc(a[x], b[x], c[x], &r, &g, &e);
                else {
                    r = j->adobe ? a[x] : 255 - a[x];
                    g = j->adobe ? b[x] : 255 - b[x];
                    e = j->adobe ? c[x] : 255 - c[x];
                }
                out[x] = (unsigned int)(sat(r) * k / 255) << 16 | (unsigned int)(sat(g) * k / 255) << 8 | (unsigned int)(sat(e) * k / 255);
            }
        }
    }
    return NULL;
}

static void release(struct jpeg *j) {
    int i;
    for(i = 0; i < 4; i++) {
        free(j->comp[i].plane);
        free(j->comp[i].coef);
        free(j->rows[i]);
    }
    free(j);
}

int wimg_jpeg(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap) {
    struct jpeg *j = calloc(1, sizeof(*j));
    const char *why;
    int said = 0;
    if(!j) return ut_error(err, cap, "out of memory reading a JPEG");
    j->in = in;
    j->len = len;
    why = segments(j, err, cap, out, &said);
    if(!said && !why && !j->scans) why = "a JPEG with no picture";
    if(!said && !why && j->progressive && !j->eoi) why = "a JPEG that ends too early";
    if(!said && !why && j->progressive) why = progressive_planes(j);
    if(!said && !why) why = pixels(j, out);
    release(j);
    if(said) return -1;
    if(why) return wimg_fail(out, err, cap, why);
    return 0;
}
