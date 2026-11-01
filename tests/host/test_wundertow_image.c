/*
SurfOS Undertow's window: tests of the picture decoders
----------------------
File: test_wundertow_image.c  Date: 11/1/26
----------------------
user/wundertow/image*.c on the host, with Undertow's util.c and the test's allocator:

- every picture of tests/fixtures/wundertow/images/ (tools/wundertow-images.py made them; its manifest says what each
  holds): the lossless ones pixel for pixel, the JPEGs within a tolerance;
- each of them cut short at every length up to 2 KB and at 200 lengths past that, and with bytes changed at random
  (a fixed sequence): the decoder says no or gives a picture, never reads or writes outside, and holds nothing after;
- every allocation failing in turn;
- files made here of every format: over the limits, sizes that overflow 32 bits, headers that lie, tables that do not
  add up, a small PNG that inflates to far more than its size holds, a JPEG of many scans, coefficients at their
  largest (the same test built with UndefinedBehaviorSanitizer at SurfOS's ABI stops at any overflow);
- making pictures smaller.

"test_wundertow_image FIXTURES" runs it all; "--dump FILE [RAW]" says what a file decodes to, and writes its pixels.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "undertow.h"
#include "alloc.h"
#include "image.h"

static int failures, checks;
static const char *dir;

static void check(int ok, const char *what) {
    checks++;
    if(ok) return;
    failures++;
    printf("FAIL %s\n", what);
}

static void checkf(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void checkf(int ok, const char *fmt, ...) {
    char s[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof(s), fmt, ap);
    va_end(ap);
    check(ok, s);
}

unsigned int uptime(void) { return 0; }
time_t ut_sim_time(time_t *t) { if(t) *t = 0; return 0; }

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    unsigned char *s;
    long n;
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s = malloc((size_t)n + 1);
    if(s && fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s);
        s = NULL;
    }
    fclose(f);
    if(s) *len = (size_t)n;
    return s;
}

static unsigned int fnv(const unsigned int *px, size_t n) {
    unsigned int h = 2166136261u;
    size_t i;
    int k;
    for(i = 0; i < n; i++)
        for(k = 0; k < 4; k++) h = (h ^ (px[i] >> (8 * k) & 255)) * 16777619u;
    return h;
}

/* tools/wundertow-images.py's pattern() */
static void pattern(int x, int y, int w, int h, int *r, int *g, int *b) {
    *r = x * 255 / (w > 1 ? w - 1 : 1);
    *g = y * 255 / (h > 1 ? h - 1 : 1);
    *b = (x + y) * 255 / (w + h - 2 > 1 ? w + h - 2 : 1);
}

static int diff(int a, int b) { return a > b ? a - b : b - a; }

/* what a decode must leave: a picture it says it gives, or nothing; nothing held but the picture */
static int decode(const unsigned char *in, size_t len, struct wimg *m, const char *what) {
    char err[UT_ERR_MAX];
    long before = ut_alloc_live;
    int rc;
    err[0] = 0;
    rc = wimg_decode(in, len, m, err, sizeof(err));
    if(rc) {
        checkf(!m->px && !m->w && !m->h, "%s: no picture after a failure", what);
        checkf(ut_alloc_live == before, "%s: nothing held after a failure (%ld)", what, ut_alloc_live - before);
        checkf(err[0] != 0, "%s: a failure says why", what);
        return -1;
    }
    checkf(m->px && m->w > 0 && m->h > 0 && m->w <= WIMG_SIDE_MAX && m->h <= WIMG_SIDE_MAX, "%s: a picture of a size", what);
    checkf(ut_alloc_live == before + 1, "%s: only the picture is held (%ld)", what, ut_alloc_live - before);
    return 0;
}

/**** the fixtures ****/

struct entry {
    char name[64], kind[8], value[16];
    int w, h;
};

static struct entry entries[256];
static int nentries;

static void load_manifest(void) {
    char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/wundertow/images/manifest", dir);
    f = fopen(path, "r");
    check(f != NULL, "the manifest is there");
    if(!f) return;
    while(nentries < 256 && fscanf(f, "%63s %d %d %7s %15s", entries[nentries].name, &entries[nentries].w, &entries[nentries].h,
                                   entries[nentries].kind, entries[nentries].value) == 5) nentries++;
    fclose(f);
    checkf(nentries > 100, "the manifest has its pictures (%d)", nentries);
}

static unsigned char *fixture(const struct entry *e, size_t *len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/wundertow/images/%s", dir, e->name);
    return read_file(path, len);
}

static void whole(const struct entry *e) {
    size_t len = 0;
    unsigned char *s = fixture(e, &len);
    struct wimg m;
    int worst = 0;
    checkf(s != NULL, "%s is there", e->name);
    if(!s) return;
    if(decode(s, len, &m, e->name)) {
        char err[UT_ERR_MAX];
        wimg_decode(s, len, &m, err, sizeof(err));
        checkf(0, "%s decodes: %s", e->name, err);
        free(s);
        return;
    }
    checkf(m.w == e->w && m.h == e->h, "%s is %dx%d (not %dx%d)", e->name, e->w, e->h, m.w, m.h);
    if(m.w == e->w && m.h == e->h) {
        if(!strcmp(e->kind, "exact")) {
            unsigned int want = (unsigned int)strtoul(e->value, NULL, 16);
            checkf(fnv(m.px, (size_t)m.w * m.h) == want, "%s: every pixel as made", e->name);
        } else {
            int tol = atoi(e->value), gray = !strcmp(e->kind, "gray"), x, y;
            for(y = 0; y < m.h; y++)
                for(x = 0; x < m.w; x++) {
                    unsigned int c = m.px[(size_t)y * m.w + x];
                    int r, g, b, d;
                    pattern(x, y, m.w, m.h, &r, &g, &b);
                    if(gray) g = b = r;
                    d = diff((int)(c >> 16 & 255), r);
                    if(diff((int)(c >> 8 & 255), g) > d) d = diff((int)(c >> 8 & 255), g);
                    if(diff((int)(c & 255), b) > d) d = diff((int)(c & 255), b);
                    if((c >> 24) != 0) d = 999;
                    if(d > worst) worst = d;
                }
            checkf(worst <= tol, "%s: every pixel within %d of the pattern (the worst %d off)", e->name, tol, worst);
        }
    }
    wimg_free(&m);
    free(s);
}

/* cut short: no picture, or for formats that need nothing after the pixels, the picture */
static void truncated(const struct entry *e) {
    size_t len = 0, n;
    unsigned char *s = fixture(e, &len);
    char what[96];
    size_t step;
    if(!s) return;
    step = len <= 2048 ? 1 : len / 200 + 1;
    snprintf(what, sizeof(what), "%.63s cut short", e->name);
    for(n = 0; n < len; n += step) {
        struct wimg m;
        unsigned char *c = malloc(n ? n : 1);           /* so that a read past its end meets nothing of the file */
        memcpy(c, s, n);
        if(!decode(c, n, &m, what)) wimg_free(&m);
        free(c);
    }
    free(s);
}

static unsigned int seed = 12345;
static unsigned int rnd(void) {
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

static void flipped(const struct entry *e, int times) {
    size_t len = 0;
    unsigned char *s = fixture(e, &len);
    char what[96];
    int t;
    if(!s) return;
    snprintf(what, sizeof(what), "%.63s with bytes changed", e->name);
    for(t = 0; t < times; t++) {
        struct wimg m;
        int k, n = 1 + (int)(rnd() % 8);
        unsigned char *c = malloc(len);
        memcpy(c, s, len);
        for(k = 0; k < n; k++) {
            size_t at = t & 1 ? rnd() % (len < 64 ? len : 64) : rnd() % len;   /* the headers often */
            c[at] = (unsigned char)(t & 2 ? rnd() : c[at] ^ (1u << (rnd() % 8)));
        }
        if(!decode(c, len, &m, what)) wimg_free(&m);
        free(c);
    }
    free(s);
}

static void out_of_memory(const char *name) {
    struct entry e;
    size_t len = 0;
    unsigned char *s;
    long ok, n;
    char what[96];
    struct wimg m;
    memset(&e, 0, sizeof(e));
    snprintf(e.name, sizeof(e.name), "%s", name);
    s = fixture(&e, &len);
    checkf(s != NULL, "%s is there", name);
    if(!s) return;
    ut_alloc_calls = 0;
    if(!decode(s, len, &m, name)) wimg_free(&m);
    ok = ut_alloc_calls;
    snprintf(what, sizeof(what), "%.63s without memory", name);
    for(n = 0; n < ok; n++) {
        ut_alloc_fail_after = n;
        checkf(decode(s, len, &m, what) == -1, "%s: allocation %ld failing fails it", name, n);
        ut_alloc_fail_after = -1;
    }
    free(s);
}

/**** making files ****/

struct buf {
    unsigned char *p;
    size_t n, cap;
};

static void put(struct buf *b, const void *s, size_t n) {
    if(b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void put8(struct buf *b, unsigned int v) {
    unsigned char c = (unsigned char)v;
    put(b, &c, 1);
}

static void put16le(struct buf *b, unsigned int v) { put8(b, v); put8(b, v >> 8); }
static void put32le(struct buf *b, unsigned int v) { put16le(b, v); put16le(b, v >> 16); }
static void put16be(struct buf *b, unsigned int v) { put8(b, v >> 8); put8(b, v); }
static void put32be(struct buf *b, unsigned int v) { put16be(b, v >> 16); put16be(b, v); }

static void refused(struct buf *b, const char *what) {
    struct wimg m;
    char err[UT_ERR_MAX];
    long before = ut_alloc_live;
    int rc = wimg_decode(b->p, b->n, &m, err, sizeof(err));
    checkf(rc == -1, "refused: %s", what);
    checkf(ut_alloc_live == before, "%s: nothing held", what);
    if(!rc) wimg_free(&m);
    free(b->p);
    memset(b, 0, sizeof(*b));
}

/**** PNG ****/

static unsigned int crc32_of(const unsigned char *p, size_t n) {
    unsigned int c = 0xffffffffu;
    int k;
    while(n--) {
        c ^= *p++;
        for(k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static void png_chunk(struct buf *b, const char *type, const void *d, size_t n) {
    size_t at;
    put32be(b, (unsigned int)n);
    at = b->n;
    put(b, type, 4);
    if(n) put(b, d, n);
    put32be(b, crc32_of(b->p + at, n + 4));
}

static void png_start(struct buf *b, unsigned int w, unsigned int h, int depth, int color, int comp, int filter, int interlace) {
    unsigned char d[13];
    put(b, "\x89PNG\r\n\x1a\n", 8);
    d[0] = (unsigned char)(w >> 24); d[1] = (unsigned char)(w >> 16); d[2] = (unsigned char)(w >> 8); d[3] = (unsigned char)w;
    d[4] = (unsigned char)(h >> 24); d[5] = (unsigned char)(h >> 16); d[6] = (unsigned char)(h >> 8); d[7] = (unsigned char)h;
    d[8] = (unsigned char)depth;
    d[9] = (unsigned char)color;
    d[10] = (unsigned char)comp;
    d[11] = (unsigned char)filter;
    d[12] = (unsigned char)interlace;
    png_chunk(b, "IHDR", d, sizeof(d));
}

static unsigned int adler_of(const unsigned char *p, size_t n) {
    unsigned int a = 1, b = 0;
    while(n--) {
        a = (a + *p++) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}

/* A zlib stream of stored blocks */
static void zlib_stored(struct buf *z, const unsigned char *p, size_t n) {
    put8(z, 0x78);
    put8(z, 0x01);
    do {
        size_t k = n > 65535 ? 65535 : n;
        put8(z, n == k);
        put16le(z, (unsigned int)k);
        put16le(z, (unsigned int)~k & 0xffff);
        put(z, p, k);
        p += k;
        n -= k;
    } while(n);
}

static void finish_png(struct buf *b, struct buf *z) {
    png_chunk(b, "IDAT", z->p, z->n);
    png_chunk(b, "IEND", NULL, 0);
    free(z->p);
    memset(z, 0, sizeof(*z));
}

/* deflate's bits by hand: a Huffman code goes in from its top bit, anything else from its low bit */
struct bits {
    struct buf *b;
    unsigned int acc;
    int n;
};

static void put_bits(struct bits *s, unsigned int v, int n) {
    while(n--) {
        s->acc |= (v & 1) << s->n;
        v >>= 1;
        if(++s->n == 8) {
            put8(s->b, s->acc);
            s->acc = 0;
            s->n = 0;
        }
    }
}

static void put_code(struct bits *s, unsigned int v, int n) {
    while(n--) put_bits(s, v >> n & 1, 1);
}

/* fixed Huffman's literal or length symbol */
static void put_lit(struct bits *s, int sym) {
    if(sym < 144) put_code(s, 0x30 + (unsigned int)sym, 8);
    else if(sym < 256) put_code(s, 0x190 + (unsigned int)(sym - 144), 9);
    else if(sym < 280) put_code(s, (unsigned int)(sym - 256), 7);
    else put_code(s, 0xc0 + (unsigned int)(sym - 280), 8);
}

static void flush_bits(struct bits *s) { while(s->n) put_bits(s, 0, 1); }

/* rows of gray bytes: 40 rows of 1024 bytes with their filter bytes, the last 7 made of matches 32768 back */
static void far_match(void) {
    static unsigned char raw[1024 * 33];
    struct buf b = { NULL, 0, 0 }, z = { NULL, 0, 0 };
    struct bits s;
    struct wimg m;
    unsigned int a = 1, c = 0;
    size_t i;
    int r, k, ok = 1;
    for(r = 0; r < 33; r++)
        for(k = 0; k < 1024; k++) raw[r * 1024 + k] = k ? (unsigned char)(r * 7 + k * 3) : 0;
    put8(&z, 0x78);
    put8(&z, 0x01);
    put8(&z, 0);
    put16le(&z, 1024 * 33);
    put16le(&z, ~(1024u * 33) & 0xffff);
    put(&z, raw, sizeof(raw));
    s.b = &z;
    s.acc = 0;
    s.n = 0;
    put_bits(&s, 1, 1);                             /* the last block */
    put_bits(&s, 1, 2);                             /* of the fixed code */
    for(k = 0; k < 27; k++) {
        put_lit(&s, 285);                           /* a length of 258 */
        put_code(&s, 29, 5);                        /* a distance of 24577 and 13 more bits */
        put_bits(&s, 8191, 13);
    }
    put_lit(&s, 283);                               /* a length of 195 and 5 more bits: 202 */
    put_bits(&s, 7, 5);
    put_code(&s, 29, 5);
    put_bits(&s, 8191, 13);
    put_lit(&s, 256);
    flush_bits(&s);
    for(i = 0; i < 40 * 1024; i++) {                /* the Adler-32 of all of it */
        a = (a + raw[i < 33 * 1024 ? i : i - 32 * 1024 * ((i - 1024) / (32 * 1024))]) % 65521;
        c = (c + a) % 65521;
    }
    put32be(&z, c << 16 | a);
    png_start(&b, 1023, 40, 8, 0, 0, 0, 0);
    finish_png(&b, &z);
    check(!decode(b.p, b.n, &m, "a match 32768 back"), "a match 32768 bytes back reads");
    if(m.px) {
        for(r = 33; r < 40; r++)
            for(k = 1; k < 1024; k++) {
                unsigned int v = raw[(r - 32) * 1024 + k];
                if(m.px[(size_t)r * 1023 + k - 1] != v * 0x010101u) ok = 0;
            }
        check(ok, "the bytes it reads are those 32768 back");
        wimg_free(&m);
    }
    free(b.p);
}

/* A small file that says it is 1 x 1 and inflates to megabytes: stops at the sixth byte */
static void png_bomb(void) {
    struct buf b = { NULL, 0, 0 }, z = { NULL, 0, 0 };
    struct bits s;
    int i;
    put8(&z, 0x78);
    put8(&z, 0x01);
    s.b = &z;
    s.acc = 0;
    s.n = 0;
    put_bits(&s, 1, 1);
    put_bits(&s, 1, 2);
    put_lit(&s, 0);
    for(i = 0; i < 100000; i++) {
        put_lit(&s, 285);
        put_code(&s, 0, 5);                         /* a distance of 1 */
    }
    put_lit(&s, 256);
    flush_bits(&s);
    put32be(&z, 1);
    png_start(&b, 1, 1, 8, 2, 0, 0, 0);
    finish_png(&b, &z);
    refused(&b, "a PNG that inflates to more than its picture holds");
}

static void png_files(void) {
    struct buf b = { NULL, 0, 0 }, z = { NULL, 0, 0 };
    static unsigned char raw[64];
    struct bits s;
    static const struct { unsigned int w, h; int depth, color, comp, filter, lace; const char *what; } heads[] = {
        { 0, 1, 8, 2, 0, 0, 0, "width 0" }, { 1, 0, 8, 2, 0, 0, 0, "height 0" },
        { 0x80000000u, 1, 8, 2, 0, 0, 0, "width 2^31" }, { 1, 0xffffffffu, 8, 2, 0, 0, 0, "height 2^32 - 1" },
        { 4097, 1, 8, 2, 0, 0, 0, "4097 wide" }, { 1, 4097, 8, 2, 0, 0, 0, "4097 high" },
        { 4096, 4096, 8, 2, 0, 0, 0, "over 16 megapixels" }, { 0x7fffffffu, 0x7fffffffu, 16, 6, 0, 0, 0, "the largest" },
        { 1, 1, 3, 0, 0, 0, 0, "gray of 3 bits" }, { 1, 1, 16, 3, 0, 0, 0, "16-bit colours" },
        { 1, 1, 4, 2, 0, 0, 0, "RGB of 4 bits" }, { 1, 1, 8, 1, 0, 0, 0, "colour type 1" }, { 1, 1, 8, 7, 0, 0, 0, "colour type 7" },
        { 1, 1, 8, 2, 1, 0, 0, "compression 1" }, { 1, 1, 8, 2, 0, 1, 0, "filter method 1" },
        { 1, 1, 8, 2, 0, 0, 2, "interlace 2" }
    };
    size_t i;
    for(i = 0; i < sizeof(heads) / sizeof(heads[0]); i++) {
        png_start(&b, heads[i].w, heads[i].h, heads[i].depth, heads[i].color, heads[i].comp, heads[i].filter, heads[i].lace);
        zlib_stored(&z, raw, 4);
        put32be(&z, adler_of(raw, 4));
        finish_png(&b, &z);
        refused(&b, heads[i].what);
    }
    /* chunks */
    put(&b, "\x89PNG\r\n\x1a\n", 8);
    png_chunk(&b, "IDAT", raw, 4);
    refused(&b, "a PNG without IHDR first");
    png_start(&b, 1, 1, 8, 2, 0, 0, 0);
    png_start(&b, 1, 1, 8, 2, 0, 0, 0);
    refused(&b, "two signatures");
    put(&b, "\x89PNG\r\n\x1a\n", 8);
    png_chunk(&b, "IHDR", raw, 12);
    refused(&b, "an IHDR of 12 bytes");
    png_start(&b, 1, 1, 8, 2, 0, 0, 0);
    b.p[20] ^= 1;
    zlib_stored(&z, raw, 4);
    put32be(&z, adler_of(raw, 4));
    finish_png(&b, &z);
    refused(&b, "a bad CRC on IHDR");
    png_start(&b, 1, 1, 8, 2, 0, 0, 0);
    png_chunk(&b, "IEND", NULL, 0);
    refused(&b, "no IDAT");
    png_start(&b, 1, 1, 8, 2, 0, 0, 0);
    png_chunk(&b, "ABCD", raw, 4);
    refused(&b, "an unknown critical chunk");
    png_start(&b, 1, 1, 8, 3, 0, 0, 0);
    zlib_stored(&z, raw, 2);
    put32be(&z, adler_of(raw, 2));
    finish_png(&b, &z);
    refused(&b, "colours with no palette");
    png_start(&b, 1, 1, 1, 3, 0, 0, 0);
    png_chunk(&b, "PLTE", raw, 9);
    refused(&b, "three colours at 1 bit");
    png_start(&b, 1, 1, 8, 3, 0, 0, 0);
    png_chunk(&b, "PLTE", raw, 8);
    refused(&b, "a palette of 8 bytes");
    png_start(&b, 1, 1, 8, 3, 0, 0, 0);
    png_chunk(&b, "PLTE", raw, 3);
    png_chunk(&b, "tRNS", raw, 2);
    refused(&b, "a tRNS longer than the palette");
    png_start(&b, 1, 1, 8, 2, 0, 0, 0);
    png_chunk(&b, "tRNS", raw, 2);
    refused(&b, "a tRNS too short for RGB");
    /* a chunk whose length lies */
    png_start(&b, 1, 1, 8, 2, 0, 0, 0);
    put32be(&b, 0xfffffff0u);
    put(&b, "IDAT", 4);
    refused(&b, "a chunk longer than the file");
    /* the data */
    raw[0] = 5;
    png_start(&b, 1, 1, 8, 0, 0, 0, 0);
    zlib_stored(&z, raw, 2);
    put32be(&z, adler_of(raw, 2));
    finish_png(&b, &z);
    refused(&b, "a filter of 5");
    raw[0] = 0;
    png_start(&b, 2, 2, 8, 0, 0, 0, 0);
    zlib_stored(&z, raw, 5);
    put32be(&z, adler_of(raw, 5));
    finish_png(&b, &z);
    refused(&b, "data that ends a byte short");
    png_start(&b, 1, 1, 8, 0, 0, 0, 0);
    zlib_stored(&z, raw, 3);
    put32be(&z, adler_of(raw, 3));
    finish_png(&b, &z);
    refused(&b, "data a byte too long");
    png_start(&b, 1, 1, 8, 0, 0, 0, 0);
    zlib_stored(&z, raw, 2);
    put32be(&z, adler_of(raw, 2) ^ 1);
    finish_png(&b, &z);
    refused(&b, "a bad Adler-32");
    {
        static const unsigned char heads2[][2] = { { 0x79, 0x01 }, { 0x88, 0x1c }, { 0x78, 0x20 }, { 0x78, 0x00 } };
        size_t k;
        for(k = 0; k < 4; k++) {
            png_start(&b, 1, 1, 8, 0, 0, 0, 0);
            zlib_stored(&z, raw, 2);
            z.p[0] = heads2[k][0];
            z.p[1] = heads2[k][1];
            put32be(&z, adler_of(raw, 2));
            finish_png(&b, &z);
            refused(&b, "a zlib header that is none");
        }
    }
    png_start(&b, 1, 1, 8, 0, 0, 0, 0);
    zlib_stored(&z, raw, 2);
    z.p[5] ^= 1;
    put32be(&z, adler_of(raw, 2));
    finish_png(&b, &z);
    refused(&b, "a stored block whose NLEN is not LEN's");
    /* a block of type 3 */
    png_start(&b, 1, 1, 8, 0, 0, 0, 0);
    put8(&z, 0x78);
    put8(&z, 0x01);
    put8(&z, 7);
    put32be(&z, 0);
    finish_png(&b, &z);
    refused(&b, "a block of type 3");
    /* a match before the start */
    png_start(&b, 4, 4, 8, 0, 0, 0, 0);
    put8(&z, 0x78);
    put8(&z, 0x01);
    s.b = &z;
    s.acc = 0;
    s.n = 0;
    put_bits(&s, 1, 1);
    put_bits(&s, 1, 2);
    put_lit(&s, 0);
    put_lit(&s, 285);
    put_code(&s, 4, 5);                             /* a distance of 5, one byte made */
    put_lit(&s, 256);
    flush_bits(&s);
    put32be(&z, 1);
    finish_png(&b, &z);
    refused(&b, "a match before the data's start");
    /* symbols of the fixed code that mean nothing */
    png_start(&b, 4, 4, 8, 0, 0, 0, 0);
    put8(&z, 0x78);
    put8(&z, 0x01);
    s.b = &z;
    s.acc = 0;
    s.n = 0;
    put_bits(&s, 1, 1);
    put_bits(&s, 1, 2);
    put_lit(&s, 0);
    put_lit(&s, 286);
    put_lit(&s, 256);
    flush_bits(&s);
    put32be(&z, 1);
    finish_png(&b, &z);
    refused(&b, "a length code of 286");
    png_start(&b, 4, 4, 8, 0, 0, 0, 0);
    put8(&z, 0x78);
    put8(&z, 0x01);
    s.b = &z;
    s.acc = 0;
    s.n = 0;
    put_bits(&s, 1, 1);
    put_bits(&s, 1, 2);
    put_lit(&s, 0);
    put_lit(&s, 257);
    put_code(&s, 30, 5);
    flush_bits(&s);
    put32be(&z, 1);
    finish_png(&b, &z);
    refused(&b, "a distance code of 30");
    /* dynamic blocks: more codes than there are; a code too full; a repeat with nothing before it, or past the end */
    {
        static const struct { unsigned int hlit, hdist, hclen; const char *what; } dyn[] = {
            { 30, 0, 0, "287 literal codes" }, { 0, 30, 0, "31 distance codes" }
        };
        size_t k;
        for(k = 0; k < 2; k++) {
            png_start(&b, 4, 4, 8, 0, 0, 0, 0);
            put8(&z, 0x78);
            put8(&z, 0x01);
            s.b = &z;
            s.acc = 0;
            s.n = 0;
            put_bits(&s, 1, 1);
            put_bits(&s, 2, 2);
            put_bits(&s, dyn[k].hlit, 5);
            put_bits(&s, dyn[k].hdist, 5);
            put_bits(&s, dyn[k].hclen, 4);
            put_bits(&s, 0, 12);
            flush_bits(&s);
            put32be(&z, 1);
            finish_png(&b, &z);
            refused(&b, dyn[k].what);
        }
        /* 19 code-length codes of 1 bit each: too full */
        png_start(&b, 4, 4, 8, 0, 0, 0, 0);
        put8(&z, 0x78);
        put8(&z, 0x01);
        s.b = &z;
        s.acc = 0;
        s.n = 0;
        put_bits(&s, 1, 1);
        put_bits(&s, 2, 2);
        put_bits(&s, 0, 5);
        put_bits(&s, 0, 5);
        put_bits(&s, 15, 4);
        for(k = 0; k < 19; k++) put_bits(&s, 1, 3);
        flush_bits(&s);
        put32be(&z, 1);
        finish_png(&b, &z);
        refused(&b, "a code-length code too full");
        /* symbol 16 (repeat) first: two codes of 1 bit, for 16 and 0 */
        png_start(&b, 4, 4, 8, 0, 0, 0, 0);
        put8(&z, 0x78);
        put8(&z, 0x01);
        s.b = &z;
        s.acc = 0;
        s.n = 0;
        put_bits(&s, 1, 1);
        put_bits(&s, 2, 2);
        put_bits(&s, 0, 5);
        put_bits(&s, 0, 5);
        put_bits(&s, 0, 4);                         /* 4 lengths: for 16, 17, 18, 0 */
        put_bits(&s, 1, 3);
        put_bits(&s, 0, 3);
        put_bits(&s, 0, 3);
        put_bits(&s, 1, 3);
        put_bits(&s, 1, 1);                         /* the code of 16: after 0's, which is 0 */
        put_bits(&s, 0, 2);
        flush_bits(&s);
        put32be(&z, 1);
        finish_png(&b, &z);
        refused(&b, "a repeat of nothing");
    }
    far_match();
    png_bomb();
}

/**** GIF ****/

static void gif_start(struct buf *b, unsigned int w, unsigned int h, int table) {
    int i;
    put(b, "GIF89a", 6);
    put16le(b, w);
    put16le(b, h);
    put8(b, table ? 0xf1 : 0x70);
    put8(b, 0);
    put8(b, 0);
    if(table) for(i = 0; i < 12; i++) put8(b, (unsigned int)i * 20);
}

static void gif_image(struct buf *b, unsigned int x, unsigned int y, unsigned int w, unsigned int h, int packed) {
    put8(b, 0x2c);
    put16le(b, x);
    put16le(b, y);
    put16le(b, w);
    put16le(b, h);
    put8(b, (unsigned int)packed);
}

static void gif_codes(struct buf *b, int root, const unsigned char *d, size_t n) {
    put8(b, (unsigned int)root);
    while(n) {
        size_t k = n > 255 ? 255 : n;
        put8(b, (unsigned int)k);
        put(b, d, k);
        d += k;
        n -= k;
    }
    put8(b, 0);
}

static void gif_files(void) {
    struct buf b = { NULL, 0, 0 };
    static const unsigned char some[4] = { 0x44, 0x44, 0x44, 0x44 };
    /* codes of 3 bits: clear (4), then 5, the end: */
    static const unsigned char ends[1] = { 0x2c };     /* 100 101 00: clear, end */
    static const unsigned char past[2] = { 0xbc, 0x01 };    /* clear, 7 (nothing has made it) */
    struct wimg m;
    put(&b, "GIF89a", 6);
    refused(&b, "a GIF that ends in its header");
    gif_start(&b, 1, 1, 0);
    gif_image(&b, 0, 0, 1, 1, 0);
    gif_codes(&b, 2, some, 4);
    refused(&b, "a GIF with no colours");
    gif_start(&b, 1, 1, 1);
    put8(&b, 0x3b);
    refused(&b, "a GIF with no frame");
    gif_start(&b, 1, 1, 1);
    put8(&b, 0x77);
    refused(&b, "a block of no kind");
    gif_start(&b, 1, 1, 1);
    gif_image(&b, 0, 0, 0, 1, 0);
    refused(&b, "a frame of no width");
    gif_start(&b, 1, 1, 1);
    gif_image(&b, 0, 0, 1, 0, 0);
    refused(&b, "a frame of no height");
    gif_start(&b, 5000, 1, 1);
    gif_image(&b, 0, 0, 1, 1, 0);
    gif_codes(&b, 2, some, 4);
    refused(&b, "a screen 5000 wide");
    gif_start(&b, 1, 1, 1);
    gif_image(&b, 65000, 0, 535, 1, 0);
    gif_codes(&b, 2, some, 4);
    refused(&b, "a frame far off the screen");
    gif_start(&b, 4096, 4096, 1);
    gif_image(&b, 0, 0, 1, 1, 0);
    gif_codes(&b, 2, some, 4);
    refused(&b, "a screen over 16 megapixels");
    gif_start(&b, 1, 1, 1);
    gif_image(&b, 0, 0, 1, 1, 0);
    gif_codes(&b, 0, some, 4);
    refused(&b, "a code size of 0");
    gif_start(&b, 1, 1, 1);
    gif_image(&b, 0, 0, 1, 1, 0);
    gif_codes(&b, 9, some, 4);
    refused(&b, "a code size of 9");
    gif_start(&b, 4, 4, 1);
    gif_image(&b, 0, 0, 4, 4, 0);
    gif_codes(&b, 2, ends, 1);
    refused(&b, "the end code before the last pixel");
    gif_start(&b, 4, 4, 1);
    gif_image(&b, 0, 0, 4, 4, 0);
    gif_codes(&b, 2, past, 2);
    refused(&b, "a code nothing has made");
    gif_start(&b, 4, 4, 1);
    gif_image(&b, 0, 0, 4, 4, 0);
    put8(&b, 2);
    put8(&b, 200);                                  /* a sub-block that says 200 bytes and has 1 */
    put8(&b, 0);
    refused(&b, "a sub-block past the file");
    /* a local table that ends the file */
    gif_start(&b, 1, 1, 0);
    gif_image(&b, 0, 0, 1, 1, 0x87);
    put8(&b, 1);
    refused(&b, "a colour table past the file");
    /* a whole one, with a frame that fills the file's own pixels past a tiny screen */
    gif_start(&b, 1, 1, 1);
    gif_image(&b, 0, 0, 1, 1, 0);
    gif_codes(&b, 2, some, 4);
    check(!decode(b.p, b.n, &m, "a 1 x 1 GIF"), "a GIF of one pixel");
    if(m.px) {
        check(m.w == 1 && m.h == 1, "its size");
        wimg_free(&m);
    }
    free(b.p);
}

/**** BMP ****/

static void bmp_start(struct buf *b, unsigned int off, unsigned int hsize, unsigned int w, unsigned int h, unsigned int planes,
                      unsigned int bpp, unsigned int comp, unsigned int used) {
    unsigned int i;
    put(b, "BM", 2);
    put32le(b, 0);
    put32le(b, 0);
    put32le(b, off);
    put32le(b, hsize);
    put32le(b, w);
    put32le(b, h);
    put16le(b, planes);
    put16le(b, bpp);
    put32le(b, comp);
    put32le(b, 0);
    put32le(b, 0);
    put32le(b, 0);
    put32le(b, used);
    put32le(b, 0);
    for(i = 40; i < hsize; i++) put8(b, 0);
}

static void bmp_files(void) {
    struct buf b = { NULL, 0, 0 };
    static unsigned char zero[4096];
    static const struct { unsigned int off, hsize, w, h, planes, bpp, comp, used; const char *what; } t[] = {
        { 54, 40, 0, 1, 1, 24, 0, 0, "width 0" }, { 54, 40, 1, 0, 1, 24, 0, 0, "height 0" },
        { 54, 40, 0xffffffffu, 1, 1, 24, 0, 0, "width -1" }, { 54, 40, 1, 0x80000000u, 1, 24, 0, 0, "the lowest height" },
        { 54, 40, 0x7fffffffu, 1, 1, 32, 0, 0, "the widest" }, { 54, 40, 1, 0x7fffffffu, 1, 32, 0, 0, "the highest" },
        { 54, 40, 0x40000000u, 4, 1, 32, 0, 0, "a row of 2^32 bytes" }, { 54, 40, 4097, 1, 1, 24, 0, 0, "4097 wide" },
        { 54, 40, 4096, 4096, 1, 32, 0, 0, "over 16 megapixels" }, { 54, 40, 1, 1, 2, 24, 0, 0, "two planes" },
        { 54, 40, 1, 1, 1, 3, 0, 0, "3 bits" }, { 54, 40, 1, 1, 1, 0, 0, 0, "0 bits" }, { 54, 40, 1, 1, 1, 64, 0, 0, "64 bits" },
        { 54, 40, 1, 1, 1, 24, 4, 0, "a JPEG inside" }, { 54, 40, 1, 1, 1, 24, 5, 0, "a PNG inside" },
        { 54, 40, 1, 1, 1, 24, 9, 0, "compression 9" }, { 54, 20, 1, 1, 1, 24, 0, 0, "a header of 20 bytes" },
        { 54, 40, 1, 1, 1, 24, 1, 0, "RLE8 of 24 bits" }, { 54, 40, 1, 1, 1, 8, 2, 0, "RLE4 of 8 bits" },
        { 54, 40, 1, 0xffffffffu, 1, 8, 1, 0, "RLE from the top" }, { 54, 40, 1, 1, 1, 24, 3, 0, "bit fields of 24 bits" },
        { 90000, 40, 1, 1, 1, 24, 0, 0, "pixels past the file" }, { 10, 40, 1, 1, 1, 24, 0, 0, "pixels in the header" },
        { 54, 40, 1, 1, 1, 8, 0, 256, "a palette past the file" },
        { 54, 64, 1, 1, 1, 24, 3, 0, "OS/2's Huffman" }
    };
    size_t i;
    for(i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        bmp_start(&b, t[i].off, t[i].hsize, t[i].w, t[i].h, t[i].planes, t[i].bpp, t[i].comp, t[i].used);
        put(&b, zero, 8);
        refused(&b, t[i].what);
    }
    put(&b, "BM", 2);
    refused(&b, "a BMP of 2 bytes");
    bmp_start(&b, 54, 124, 1, 1, 1, 24, 0, 0);
    b.n = 40;
    refused(&b, "a header past the file");
    /* rows past the file: a row short */
    bmp_start(&b, 54, 40, 10, 10, 1, 24, 0, 0);
    put(&b, zero, 32 * 9 + 29);
    refused(&b, "a row short of pixels");
    /* masks */
    bmp_start(&b, 66, 40, 1, 1, 1, 32, 3, 0);
    put32le(&b, 0x00ff00ff);
    put32le(&b, 0xff00);
    put32le(&b, 0xff0000);
    put(&b, zero, 4);
    refused(&b, "a mask in two pieces");
    bmp_start(&b, 54, 40, 1, 1, 1, 32, 3, 0);
    put32le(&b, 0xff);
    refused(&b, "masks past the file");
    /* RLE: codes that stop in a pair, in a step, in a literal run */
    bmp_start(&b, 54, 40, 4, 4, 1, 8, 1, 1);
    put32le(&b, 0);
    put8(&b, 2);
    refused(&b, "RLE ending in a pair");
    bmp_start(&b, 58, 40, 4, 4, 1, 8, 1, 1);
    put32le(&b, 0);
    put8(&b, 0);
    put8(&b, 2);
    put8(&b, 1);
    refused(&b, "RLE ending in a step");
    bmp_start(&b, 58, 40, 4, 4, 1, 8, 1, 1);
    put32le(&b, 0);
    put8(&b, 0);
    put8(&b, 200);
    put8(&b, 1);
    refused(&b, "RLE ending in a literal run");
    /* runs and steps far past the picture write nothing */
    {
        struct wimg m;
        size_t k;
        bmp_start(&b, 58, 40, 2, 2, 1, 8, 1, 1);
        put32le(&b, 0xff0000);
        put8(&b, 255);                              /* a run past the row's end */
        put8(&b, 0);
        put8(&b, 0);                                /* a step far off */
        put8(&b, 2);
        put8(&b, 255);
        put8(&b, 255);
        put8(&b, 255);
        put8(&b, 0);
        put8(&b, 0);
        put8(&b, 0);
        put8(&b, 0);
        put8(&b, 1);
        check(!decode(b.p, b.n, &m, "RLE past the picture"), "RLE past the picture is drawn as far as it goes");
        if(m.px) {
            check(m.px[2] == 0xff0000 && m.px[3] == 0xff0000 && m.px[0] == WIMG_GONE, "only its bottom row");
            for(k = 0; k < 4; k++) check((m.px[k] & 0xffffff) == 0xff0000 || m.px[k] == WIMG_GONE, "its pixels");
            wimg_free(&m);
        }
        free(b.p);
        memset(&b, 0, sizeof(b));
    }
}

/**** JPEG ****/

static void seg(struct buf *b, unsigned int marker, const void *d, size_t n) {
    put8(b, 0xff);
    put8(b, marker);
    put16be(b, (unsigned int)n + 2);
    if(n) put(b, d, n);
}

static void jpeg_sof(struct buf *b, unsigned int marker, unsigned int precision, unsigned int h, unsigned int w, int comps,
                     unsigned int sampling, unsigned int tq) {
    unsigned char d[6 + 12];
    int i;
    d[0] = (unsigned char)precision;
    d[1] = (unsigned char)(h >> 8);
    d[2] = (unsigned char)h;
    d[3] = (unsigned char)(w >> 8);
    d[4] = (unsigned char)w;
    d[5] = (unsigned char)comps;
    for(i = 0; i < comps && i < 4; i++) {
        d[6 + i * 3] = (unsigned char)(i + 1);
        d[7 + i * 3] = (unsigned char)sampling;
        d[8 + i * 3] = (unsigned char)tq;
    }
    seg(b, marker, d, (size_t)6 + (size_t)(comps > 4 ? 4 : comps) * 3);
}

static void jpeg_quant(struct buf *b, unsigned int v16) {           /* table 0, every value the same */
    unsigned char d[129];
    int i;
    d[0] = v16 > 255 ? 0x10 : 0;
    for(i = 0; i < 64; i++) {
        if(v16 > 255) {
            d[1 + i * 2] = (unsigned char)(v16 >> 8);
            d[2 + i * 2] = (unsigned char)v16;
        } else d[1 + i] = (unsigned char)v16;
    }
    seg(b, 0xdb, d, v16 > 255 ? 129 : 65);
}

/* Table tc 0 of one code of 1 bit (0) for a value */
static void jpeg_huff(struct buf *b, unsigned int tc, unsigned int value) {
    unsigned char d[18];
    memset(d, 0, sizeof(d));
    d[0] = (unsigned char)(tc << 4);
    d[1] = 1;
    d[17] = (unsigned char)value;
    seg(b, 0xc4, d, sizeof(d));
}

static void jpeg_sos(struct buf *b, int comps, unsigned int ss, unsigned int se, unsigned int ahal) {
    unsigned char d[12];
    int i;
    d[0] = (unsigned char)comps;
    for(i = 0; i < comps && i < 4; i++) {
        d[1 + i * 2] = (unsigned char)(i + 1);
        d[2 + i * 2] = 0;
    }
    d[1 + comps * 2] = (unsigned char)ss;
    d[2 + comps * 2] = (unsigned char)se;
    d[3 + comps * 2] = (unsigned char)ahal;
    seg(b, 0xda, d, (size_t)4 + (size_t)comps * 2);
}

static void soi(struct buf *b) { put8(b, 0xff); put8(b, 0xd8); }
static void eoi(struct buf *b) { put8(b, 0xff); put8(b, 0xd9); }

/* A one-block file: table entries say what the only code means */
static void small_jpeg(struct buf *b, unsigned int quant, unsigned int dc_value, unsigned int ac_value) {
    soi(b);
    jpeg_quant(b, quant);
    jpeg_sof(b, 0xc0, 8, 8, 8, 1, 0x11, 0);
    jpeg_huff(b, 0, dc_value);
    jpeg_huff(b, 1, ac_value);
}

static void jpeg_files(void) {
    struct buf b = { NULL, 0, 0 };
    struct wimg m;
    int i, k;
    static const struct { unsigned int marker, precision, h, w; int comps; unsigned int sampling, tq; const char *what; } t[] = {
        { 0xc0, 12, 8, 8, 1, 0x11, 0, "12 bits" }, { 0xc0, 8, 0, 8, 1, 0x11, 0, "a height given later" },
        { 0xc0, 8, 8, 0, 1, 0x11, 0, "width 0" }, { 0xc0, 8, 8, 8, 2, 0x11, 0, "two components" },
        { 0xc0, 8, 8, 8, 1, 0x00, 0, "sampling 0" }, { 0xc0, 8, 8, 8, 1, 0x51, 0, "sampling 5" }, { 0xc0, 8, 8, 8, 1, 0x15, 0, "sampling 5 down" },
        { 0xc0, 8, 8, 8, 1, 0x11, 4, "quantization table 4" }, { 0xc0, 8, 4097, 8, 1, 0x11, 0, "4097 high" },
        { 0xc0, 8, 8, 4097, 1, 0x11, 0, "4097 wide" }, { 0xc0, 8, 4096, 4096, 1, 0x11, 0, "over 16 megapixels" },
        { 0xc0, 8, 65535, 65535, 3, 0x44, 0, "the largest" }, { 0xc3, 8, 8, 8, 1, 0x11, 0, "lossless" },
        { 0xc5, 8, 8, 8, 1, 0x11, 0, "hierarchical" }, { 0xc9, 8, 8, 8, 1, 0x11, 0, "arithmetic" },
        { 0xca, 8, 8, 8, 1, 0x11, 0, "progressive arithmetic" }
    };
    for(i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
        soi(&b);
        jpeg_sof(&b, t[i].marker, t[i].precision, t[i].h, t[i].w, t[i].comps, t[i].sampling, t[i].tq);
        eoi(&b);
        refused(&b, t[i].what);
    }
    soi(&b);
    refused(&b, "only SOI");
    soi(&b);
    put8(&b, 0xff);
    refused(&b, "SOI and FF");
    soi(&b);
    eoi(&b);
    refused(&b, "SOI and EOI");
    /* a segment that lies */
    soi(&b);
    put8(&b, 0xff);
    put8(&b, 0xdb);
    put16be(&b, 65535);
    refused(&b, "a segment past the file");
    soi(&b);
    put8(&b, 0xff);
    put8(&b, 0xdb);
    put16be(&b, 1);
    refused(&b, "a segment of length 1");
    /* the tables */
    soi(&b);
    {
        unsigned char d[65];
        memset(d, 1, sizeof(d));
        d[0] = 0x20;
        seg(&b, 0xdb, d, sizeof(d));
    }
    refused(&b, "quantization of 24 bits");
    soi(&b);
    {
        unsigned char d[65];
        memset(d, 1, sizeof(d));
        d[0] = 0x04;
        seg(&b, 0xdb, d, sizeof(d));
    }
    refused(&b, "quantization table 4");
    soi(&b);
    {
        unsigned char d[10];
        memset(d, 1, sizeof(d));
        d[0] = 0;
        seg(&b, 0xdb, d, sizeof(d));
    }
    refused(&b, "a short quantization table");
    soi(&b);
    {
        unsigned char d[17 + 3];
        memset(d, 0, sizeof(d));
        d[1] = 3;                                   /* three codes of 1 bit */
        seg(&b, 0xc4, d, sizeof(d));
    }
    refused(&b, "a Huffman table too full");
    soi(&b);
    {
        unsigned char d[17 + 300];
        memset(d, 0, sizeof(d));
        d[8] = 255;
        d[9] = 45;
        seg(&b, 0xc4, d, sizeof(d));
    }
    refused(&b, "a Huffman table of 300 values");
    soi(&b);
    {
        unsigned char d[17];
        memset(d, 0, sizeof(d));
        d[0] = 0x24;
        seg(&b, 0xc4, d, sizeof(d));
    }
    refused(&b, "a Huffman table of class 2");
    soi(&b);
    {
        unsigned char d[17];
        memset(d, 0, sizeof(d));
        d[1] = 5;                                   /* five values, none of them there */
        seg(&b, 0xc4, d, sizeof(d));
    }
    refused(&b, "a Huffman table shorter than it says");
    soi(&b);
    seg(&b, 0xdd, "\x01", 1);
    refused(&b, "a restart interval of 1 byte");
    /* the frame and the scans */
    soi(&b);
    jpeg_sof(&b, 0xc0, 8, 8, 8, 1, 0x11, 0);
    jpeg_sof(&b, 0xc0, 8, 8, 8, 1, 0x11, 0);
    refused(&b, "two frames");
    soi(&b);
    jpeg_sos(&b, 1, 0, 63, 0);
    eoi(&b);
    refused(&b, "a scan before its frame");
    small_jpeg(&b, 1, 0, 0);
    eoi(&b);
    refused(&b, "no scan");
    small_jpeg(&b, 1, 0, 0);
    jpeg_sos(&b, 2, 0, 63, 0);
    eoi(&b);
    refused(&b, "a scan of more components than the frame's");
    small_jpeg(&b, 1, 0, 0);
    {
        unsigned char d[6] = { 1, 9, 0, 0, 63, 0 };
        seg(&b, 0xda, d, sizeof(d));
    }
    eoi(&b);
    refused(&b, "a scan of a component it does not have");
    small_jpeg(&b, 1, 0, 0);
    {
        unsigned char d[6] = { 1, 1, 0x44, 0, 63, 0 };
        seg(&b, 0xda, d, sizeof(d));
    }
    eoi(&b);
    refused(&b, "Huffman table 4");
    small_jpeg(&b, 1, 0, 0);
    {
        unsigned char d[2] = { 1, 1 };
        seg(&b, 0xda, d, sizeof(d));
    }
    refused(&b, "a short scan header");
    soi(&b);
    jpeg_quant(&b, 1);
    jpeg_sof(&b, 0xc0, 8, 8, 8, 1, 0x11, 0);
    jpeg_sos(&b, 1, 0, 63, 0);
    put8(&b, 0);
    eoi(&b);
    refused(&b, "a Huffman table it does not define");
    soi(&b);
    jpeg_sof(&b, 0xc0, 8, 8, 8, 1, 0x11, 0);
    jpeg_huff(&b, 0, 0);
    jpeg_huff(&b, 1, 0);
    jpeg_sos(&b, 1, 0, 63, 0);
    put8(&b, 0);
    eoi(&b);
    refused(&b, "a quantization table it does not define");
    /* progressive scans that are none */
    {
        static const struct { int comps; unsigned int ss, se, ahal; const char *what; } s[] = {
            { 1, 5, 2, 0, "a band that runs backwards" }, { 1, 1, 64, 0, "a band past 63" }, { 1, 0, 5, 0, "DC with AC" },
            { 1, 1, 5, 0xe0, "Ah of 14" }, { 1, 1, 5, 0x0e, "Al of 14" }
        };
        for(i = 0; i < 5; i++) {
            soi(&b);
            jpeg_quant(&b, 1);
            jpeg_sof(&b, 0xc2, 8, 8, 8, 1, 0x11, 0);
            jpeg_huff(&b, 0, 0);
            jpeg_huff(&b, 1, 0);
            jpeg_sos(&b, s[i].comps, s[i].ss, s[i].se, s[i].ahal);
            put8(&b, 0);
            eoi(&b);
            refused(&b, s[i].what);
        }
        soi(&b);
        jpeg_quant(&b, 1);
        jpeg_sof(&b, 0xc2, 8, 8, 8, 3, 0x11, 0);
        jpeg_huff(&b, 0, 0);
        jpeg_huff(&b, 1, 0);
        jpeg_sos(&b, 2, 1, 5, 0);
        put8(&b, 0);
        eoi(&b);
        refused(&b, "an AC scan of two components");
    }
    /* a progressive file without its end */
    soi(&b);
    jpeg_quant(&b, 1);
    jpeg_sof(&b, 0xc2, 8, 8, 8, 1, 0x11, 0);
    jpeg_huff(&b, 0, 0);
    jpeg_sos(&b, 1, 0, 0, 0);
    put8(&b, 0);
    refused(&b, "a progressive file without EOI");
    /* many scans */
    soi(&b);
    jpeg_quant(&b, 1);
    jpeg_sof(&b, 0xc2, 8, 8, 8, 1, 0x11, 0);
    jpeg_huff(&b, 0, 0);
    for(i = 0; i < WIMG_SCANS_MAX + 2; i++) {
        jpeg_sos(&b, 1, 0, 0, 0);
        put8(&b, 0);
    }
    eoi(&b);
    refused(&b, "more scans than are kept");
    /* the data: an AC run past the block; a code that is none */
    small_jpeg(&b, 1, 0, 0xf1);
    jpeg_sos(&b, 1, 0, 63, 0);
    for(i = 0; i < 16; i++) put8(&b, 0);
    eoi(&b);
    refused(&b, "AC coefficients past the block");
    small_jpeg(&b, 1, 0, 0);
    jpeg_sos(&b, 1, 0, 63, 0);
    put8(&b, 0xfe);                                 /* 1 is no code of a table with only 0 */
    put8(&b, 0xff);
    put8(&b, 0x00);
    eoi(&b);
    refused(&b, "a code that is none");
    small_jpeg(&b, 1, 16, 0);
    jpeg_sos(&b, 1, 0, 63, 0);
    put8(&b, 0);
    put8(&b, 0);
    eoi(&b);
    refused(&b, "a DC category of 16");
    /* the data ends at once */
    small_jpeg(&b, 1, 0, 0);
    jpeg_sos(&b, 1, 0, 63, 0);
    eoi(&b);
    refused(&b, "a scan with no data");
    /* restart: a marker that is not one */
    soi(&b);
    jpeg_quant(&b, 1);
    jpeg_sof(&b, 0xc0, 8, 8, 16, 1, 0x11, 0);
    jpeg_huff(&b, 0, 0);
    jpeg_huff(&b, 1, 0);
    seg(&b, 0xdd, "\0\1", 2);
    jpeg_sos(&b, 1, 0, 63, 0);
    put8(&b, 0);
    eoi(&b);
    refused(&b, "EOI where a restart marker belongs");
    /* the numbers at their largest: the largest quantizer, the largest coefficients, every cell of the block, at
       every sign. A file of no sense decodes to whatever it decodes to; nothing overflows on the way */
    for(k = 0; k < 2; k++) {
        soi(&b);
        jpeg_quant(&b, 65535);
        jpeg_sof(&b, 0xc0, 8, 8, 8, 1, 0x11, 0);
        jpeg_huff(&b, 0, 15);
        jpeg_huff(&b, 1, 15);                       /* a run of 0 and 15 bits */
        jpeg_sos(&b, 1, 0, 63, 0);
        for(i = 0; i < 64; i++) {                   /* a 0 bit for the code, 15 of k: */
            put8(&b, k ? 0x7f : 0x00);
            put8(&b, k ? 0xff : 0x00);
            if(k) put8(&b, 0x00);                   /* stuffing */
        }
        eoi(&b);
        if(!decode(b.p, b.n, &m, "the largest numbers")) wimg_free(&m);
        free(b.p);
        memset(&b, 0, sizeof(b));
    }
    /* progressive: DC at its largest, Al at its largest, refinements over them; a whole picture */
    soi(&b);
    jpeg_quant(&b, 65535);
    jpeg_sof(&b, 0xc2, 8, 8, 8, 1, 0x11, 0);
    jpeg_huff(&b, 0, 15);
    jpeg_huff(&b, 1, 15);
    jpeg_sos(&b, 1, 0, 0, 13);
    put8(&b, 0x7f);
    put8(&b, 0xff);
    put8(&b, 0x00);
    jpeg_sos(&b, 1, 1, 63, 13);
    for(i = 0; i < 64; i++) {
        put8(&b, 0x7f);
        put8(&b, 0xff);
        put8(&b, 0x00);
    }
    jpeg_sos(&b, 1, 0, 0, 0xd0);
    put8(&b, 0xff);
    put8(&b, 0x00);
    jpeg_sos(&b, 1, 1, 63, 0xd0);
    for(i = 0; i < 64; i++) {
        put8(&b, 0xff);
        put8(&b, 0x00);
    }
    eoi(&b);
    if(!decode(b.p, b.n, &m, "the largest progressive numbers")) wimg_free(&m);
    free(b.p);
    memset(&b, 0, sizeof(b));
}

/**** kinds, sizes ****/

static void kinds(void) {
    static const unsigned char junk[] = "<html>not a picture</html>";
    struct wimg m;
    char err[UT_ERR_MAX];
    check(wimg_decode(junk, sizeof(junk) - 1, &m, err, sizeof(err)) == -1 && strstr(err, "not a picture") != NULL, "text is no picture");
    check(wimg_decode(junk, 0, &m, err, sizeof(err)) == -1, "no bytes are no picture");
    check(wimg_decode(NULL, 0, &m, err, sizeof(err)) == -1, "no pointer is no picture");
    check(wimg_known((const unsigned char *)"GIF89a", 6) && wimg_known((const unsigned char *)"BM", 2) &&
          wimg_known((const unsigned char *)"\xff\xd8\xff", 3) && wimg_known((const unsigned char *)"\x89PNG\r\n\x1a\n", 8), "the kinds");
    check(!wimg_known((const unsigned char *)"GIF88a", 6) && !wimg_known((const unsigned char *)"\xff\xd8", 2) &&
          !wimg_known(NULL, 0) && !wimg_known(junk, 4), "what is none of them");
}

static void sizes(void) {
    struct wimg m;
    unsigned int src[6] = { 0x000000, 0x202020, 0x404040, 0x606060, 0x808080, 0xa0a0a0 }, dst[64];
    char err[UT_ERR_MAX];
    int i;
    check(wimg_alloc(&m, 0, 1, err, sizeof(err)) == -1, "no pixels");
    check(wimg_alloc(&m, 1, 0, err, sizeof(err)) == -1, "no rows");
    check(wimg_alloc(&m, WIMG_SIDE_MAX + 1, 1, err, sizeof(err)) == -1, "wider than the side's limit");
    check(wimg_alloc(&m, 4096, 3907, err, sizeof(err)) == -1, "16,003,072 pixels");
    memset(&m, 0, sizeof(m));
    check(!wimg_alloc(&m, 4000, 4000, err, sizeof(err)) && m.px, "16 megapixels exactly");
    wimg_free(&m);
    /* whole factors: 4 x 2 becomes 2 x 1 */
    memset(&m, 0, sizeof(m));
    check(!wimg_alloc(&m, 4, 2, err, sizeof(err)), "a small picture");
    memcpy(m.px, (const unsigned int[]){ 0, 0x10, 0x40, 0x50, 0x20, 0x30, 0x60, 0x70 }, 8 * sizeof(unsigned int));
    check(!wimg_shrink(&m, 4) && m.w == 4, "no smaller than it needs");
    check(wimg_shrink(&m, 2) && m.w == 2 && m.h == 1, "by 2");
    check(m.px[0] == 0x18 && m.px[1] == 0x58, "each the average of four");
    wimg_free(&m);
    /* a side that does not divide, and what is not there */
    memset(&m, 0, sizeof(m));
    wimg_alloc(&m, 5, 1, err, sizeof(err));
    memcpy(m.px, (const unsigned int[]){ WIMG_GONE, 0x101010, WIMG_GONE, WIMG_GONE, 0x303030 }, 5 * sizeof(unsigned int));
    check(wimg_shrink(&m, 3) && m.w == 3 && m.h == 1, "5 by 2: 3");
    check(m.px[0] >> 24 == 128 && (m.px[0] & 255) == 0x10, "half there: the colour of what is");
    check(m.px[1] == WIMG_GONE, "none there: not there");
    check(m.px[2] == 0x303030, "the last, of one pixel");
    wimg_free(&m);
    /* to any size */
    wimg_scale(src, 3, 2, dst, 3, 2);
    check(!memcmp(src, dst, sizeof(src)), "the same size: the same");
    wimg_scale(src, 3, 2, dst, 6, 4);
    check(dst[0] == src[0] && dst[1] == src[0] && dst[2] == src[1] && dst[18] == src[3] && dst[23] == src[5], "twice the size: the nearest");
    wimg_scale(src, 3, 2, dst, 1, 1);
    check(dst[0] == 0x505050, "one pixel: the average of them all");
    wimg_scale(src, 3, 2, dst, 2, 1);
    check(dst[0] == (0x303030 & 0xffffff) || (dst[0] & 255) >= 0x20, "narrower: averages");
    for(i = 0; i < 6; i++) dst[i] = 0;
}

static void dump(const char *path, const char *raw) {
    size_t len = 0;
    unsigned char *s = read_file(path, &len);
    struct wimg m;
    char err[UT_ERR_MAX];
    if(!s || wimg_decode(s, len, &m, err, sizeof(err))) {
        printf("%s: %s\n", path, s ? err : "cannot read it");
        return;
    }
    printf("%s: %dx%d, hash %08x\n", path, m.w, m.h, fnv(m.px, (size_t)m.w * m.h));
    if(raw) {
        FILE *f = fopen(raw, "wb");
        if(f) {
            fwrite(m.px, sizeof(*m.px), (size_t)m.w * m.h, f);
            fclose(f);
        }
    }
    wimg_free(&m);
    free(s);
}

int main(int argc, char **argv) {
    int i;
    if(argc >= 3 && !strcmp(argv[1], "--dump")) {
        dump(argv[2], argc > 3 ? argv[3] : NULL);
        return 0;
    }
    dir = argc > 1 ? argv[1] : "tests/fixtures";
    load_manifest();
    for(i = 0; i < nentries; i++) whole(&entries[i]);
    for(i = 0; i < nentries; i++) {
        truncated(&entries[i]);
        flipped(&entries[i], 60);
    }
    out_of_memory("rgba8.png");
    out_of_memory("rgba8-adam7.png");
    out_of_memory("plain.gif");
    out_of_memory("rgb24.bmp");
    out_of_memory("rle8.bmp");
    out_of_memory("base420.jpg");
    out_of_memory("prog420.jpg");
    out_of_memory("own444.jpg");
    kinds();
    sizes();
    png_files();
    gif_files();
    bmp_files();
    jpeg_files();
    check(ut_alloc_live == 0, "nothing held at the end");
    if(failures) {
        printf("WUNDERTOW IMAGE TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW IMAGE TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
