/*
SurfOS user library: tests of the picture encoders
----------------------
File: test_image_save.c  Date: 11/05/26
----------------------
user/libsurf/image_save.c on the host, with libsurf's decoders, inflate, deflate and CRC-32, compiled with Undertow's
test allocator (tests/host/undertow/alloc.c) so that the test counts what they hold and fails any allocation:

- pictures of many sizes and contents (smooth, noisy, striped, solid, of every transparency) written as a BMP and as
  a PNG, RGB and RGBA, then read again by the decoders here: pixel for pixel, but for the transparency a BMP and an
  RGB PNG have no room for;
- the files' structure by hand: BMP's two headers; PNG's signature, chunks in order, every CRC (a bitwise CRC of
  this test's own), the IHDR, the zlib header, the rows inflated and their Adler-32; every row filter chosen
  somewhere, and data long enough for many IDAT chunks;
- every allocation failing in turn, and put() failing at each of its calls: -1 and nothing held, and put() never
  called again once it failed;
- pictures over wimg_alloc()'s limits, of no size or with no pixels: refused before anything is asked for; one at
  the limits begins.

"test_image_save" runs it all; "test_image_save --write DIR" also leaves each picture it checks in DIR, as NAME.bmp,
NAME.png and NAME-rgba.png, and its pixels as NAME.rgba (red, green, blue and 255 - TT, rows from the top), for
tests/test_image_files.py, which reads them with Python's zlib and with sips where there is one (the Mac).
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <inflate.h>
#include <image.h>
#include "alloc.h"

static int failures, checks;
static const char *outdir;
static int filters_seen[5];

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

/**** put() into memory ****/

struct mem {
    unsigned char *p;
    size_t n, cap;
    long calls;                                 /* put()'s calls so far */
    long fail_at;                               /* >= 0: that call fails */
    int after;                                  /* calls after a failure */
};

static int to_mem(void *ctx, const void *p, size_t n) {
    struct mem *m = ctx;
    long call = m->calls++;
    if(m->fail_at >= 0 && call > m->fail_at) m->after++;
    if(call == m->fail_at) return -1;
    if(!n) return 0;
    if(m->n + n > m->cap) {
        m->cap = (m->n + n) * 2;
        m->p = realloc(m->p, m->cap);
        if(!m->p) {
            printf("the test is out of memory\n");
            exit(2);
        }
    }
    memcpy(m->p + m->n, p, n);
    m->n += n;
    return 0;
}

static void mem_start(struct mem *m, long fail_at) {
    memset(m, 0, sizeof(*m));
    m->fail_at = fail_at;
}

/**** pictures ****/

static unsigned int seed = 4242;
static unsigned int rnd(void) {
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

enum { SMOOTH, NOISE, STRIPES, SOLID, ALPHA, CHECKER };
static const char *const kinds[] = { "smooth", "noise", "stripes", "solid", "alpha", "checker" };

static void make(struct wimg *m, int w, int h, int kind) {
    int x, y;
    m->w = w;
    m->h = h;
    m->px = malloc((size_t)w * h * sizeof(*m->px));
    for(y = 0; y < h; y++)
        for(x = 0; x < w; x++) {
            unsigned int r = (unsigned int)(x * 255 / (w > 1 ? w - 1 : 1)), g = (unsigned int)(y * 255 / (h > 1 ? h - 1 : 1)),
                         b = (unsigned int)((x + y) * 255 / (w + h > 2 ? w + h - 2 : 1)), t = 0, c;
            switch(kind) {
            case NOISE: c = rnd() & 0xffffff; break;
            case STRIPES: c = y & 1 ? 0x204060 : 0xe0c0a0; break;
            case SOLID: c = 0x3366cc; break;
            case ALPHA:
                t = (unsigned int)(x * 7 + y * 13) & 255;
                if(x == 0 && y == 0) t = 255;   /* not there at all, and solid, and everything between */
                if(x == 1 && y == 0) t = 0;
                c = r << 16 | g << 8 | b;
                break;
            case CHECKER: c = (x ^ y) & 1 ? 0xffffff : 0; break;
            default: c = r << 16 | g << 8 | b; break;
            }
            m->px[(size_t)y * w + x] = t << 24 | c;
        }
}

/**** reading what was written, by hand ****/

static unsigned int le16(const unsigned char *p) { return (unsigned int)p[0] | (unsigned int)p[1] << 8; }
static unsigned int le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }
static unsigned int be32(const unsigned char *p) {
    return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | (unsigned int)p[2] << 8 | p[3];
}

/* A CRC-32 bit by bit, not libsurf's table */
static unsigned int crc_bits(const unsigned char *p, size_t n) {
    unsigned int c = 0xffffffffu;
    int k;
    while(n--) {
        c ^= *p++;
        for(k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static void bmp_structure(const struct mem *f, const struct wimg *m, const char *what) {
    size_t row = ((size_t)m->w * 3 + 3) & ~(size_t)3;
    const unsigned char *p = f->p;
    checkf(f->n == 54 + row * (size_t)m->h, "%s: a file of its rows and headers", what);
    if(f->n < 54) return;
    checkf(p[0] == 'B' && p[1] == 'M' && le32(p + 2) == f->n && le32(p + 6) == 0 && le32(p + 10) == 54, "%s: the file header", what);
    checkf(le32(p + 14) == 40 && le32(p + 18) == (unsigned int)m->w && le32(p + 22) == (unsigned int)m->h &&
           le16(p + 26) == 1 && le16(p + 28) == 24 && le32(p + 30) == 0 && le32(p + 34) == row * (size_t)m->h &&
           le32(p + 46) == 0, "%s: the 40-byte header", what);
    if(f->n == 54 + row * (size_t)m->h) {
        int y, zeros = 1;
        size_t i;
        for(y = 0; y < m->h; y++)
            for(i = (size_t)m->w * 3; i < row; i++) if(p[54 + (size_t)y * row + i]) zeros = 0;
        checkf(zeros, "%s: rows padded with zeros", what);
    }
}

/* Row raw (n bytes, bpp a pixel, up the one above it or NULL) filtered by type t, as the standard's section 9.2
   defines the filters: the sum of the bytes' absolute values as signed numbers */
static unsigned long filter_sum(int t, const unsigned char *raw, const unsigned char *up, size_t n, int bpp) {
    unsigned long sum = 0;
    size_t i;
    for(i = 0; i < n; i++) {
        int left = i >= (size_t)bpp ? raw[i - bpp] : 0, above = up ? up[i] : 0, corner = up && i >= (size_t)bpp ? up[i - bpp] : 0, pred;
        int p = left + above - corner, pa = abs(p - left), pb = abs(p - above), pc = abs(p - corner);
        signed char v;
        switch(t) {
        case 0: pred = 0; break;
        case 1: pred = left; break;
        case 2: pred = above; break;
        case 3: pred = (left + above) / 2; break;
        default: pred = pa <= pb && pa <= pc ? left : pb <= pc ? above : corner; break;
        }
        v = (signed char)(unsigned char)(raw[i] - pred);
        sum += (unsigned long)abs(v);
    }
    return sum;
}

/* The rows of a PNG's data, inflated by libsurf's inflate, unfiltered by hand into RGB(A) bytes */
static int png_rows(const unsigned char *z, size_t zn, int w, int h, int bpp, unsigned char *out, const char *what) {
    static struct inflate inf;
    size_t n = (size_t)w * bpp, raw = (n + 1) * (size_t)h, i;
    unsigned char *d = malloc(raw + 1);
    unsigned int a = 1, b = 0;
    int r, y, least_misses = 0;
    inflate_init(&inf);
    inf.in = z + 2;
    inf.in_n = zn - 2;
    inf.out = d;
    inf.out_n = raw + 1;
    r = inflate_run(&inf);
    checkf(r == INFLATE_END && inf.out_n == 1, "%s: the data inflates to its rows exactly", what);
    checkf(inf.in_n == 4, "%s: the Adler-32 follows the deflate data", what);
    for(i = 0; i < raw; i++) {
        a = (a + d[i]) % 65521;
        b = (b + a) % 65521;
    }
    checkf(inf.in_n >= 4 && be32(inf.in) == (b << 16 | a), "%s: its Adler-32", what);
    for(y = 0; y < h; y++) {
        const unsigned char *f = d + (size_t)y * (n + 1);
        unsigned char *o = out + (size_t)y * n, *up = y ? o - n : NULL;
        int t = f[0];
        checkf(t <= 4, "%s: a filter there is", what);
        if(t > 4) break;
        filters_seen[t]++;
        for(i = 0; i < n; i++) {
            int left = i >= (size_t)bpp ? o[i - bpp] : 0, above = up ? up[i] : 0, corner = up && i >= (size_t)bpp ? up[i - bpp] : 0, pred;
            int p = left + above - corner, pa = abs(p - left), pb = abs(p - above), pc = abs(p - corner);
            switch(t) {
            case 0: pred = 0; break;
            case 1: pred = left; break;
            case 2: pred = above; break;
            case 3: pred = (left + above) / 2; break;
            default: pred = pa <= pb && pa <= pc ? left : pb <= pc ? above : corner; break;
            }
            o[i] = (unsigned char)(f[i + 1] + pred);
        }
        {                                       /* the filter whose sum is least, the first of those that tie */
            unsigned long least = filter_sum(0, o, up, n, bpp);
            int k, want = 0;
            for(k = 1; k < 5; k++) {
                unsigned long sum = filter_sum(k, o, up, n, bpp);
                if(sum < least) {
                    least = sum;
                    want = k;
                }
            }
            if(t != want) least_misses++;
        }
    }
    checkf(!least_misses, "%s: each row's filter is the one of the least sum (%d rows not)", what, least_misses);
    free(d);
    return r == INFLATE_END ? 0 : -1;
}

static void png_structure(const struct mem *f, const struct wimg *m, int alpha, const char *what) {
    const unsigned char *p = f->p;
    size_t at = 8, zn = 0;
    unsigned char *z = malloc(f->n), *rgb;
    int n = 0, idats = 0, ended = 0, bpp = alpha ? 4 : 3, x, y, same = 1;
    checkf(f->n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8), "%s: the signature", what);
    while(f->n - at >= 12 && !ended) {
        unsigned int len = be32(p + at);
        const unsigned char *type = p + at + 4, *d = p + at + 8;
        if(len > f->n - at - 12) break;
        checkf(crc_bits(type, len + 4) == be32(d + len), "%s: chunk %d's CRC", what, n);
        if(!n) {
            checkf(!memcmp(type, "IHDR", 4) && len == 13, "%s: IHDR first", what);
            checkf(be32(d) == (unsigned int)m->w && be32(d + 4) == (unsigned int)m->h && d[8] == 8 &&
                   d[9] == (alpha ? 6 : 2) && !d[10] && !d[11] && !d[12], "%s: the IHDR's fields", what);
        } else if(!memcmp(type, "IDAT", 4)) {
            checkf(len > 0 && len <= 32768, "%s: an IDAT of 1 to 32768 bytes", what);
            memcpy(z + zn, d, len);
            zn += len;
            idats++;
        } else {
            checkf(!memcmp(type, "IEND", 4) && !len, "%s: IEND, empty, after the IDATs", what);
            ended = 1;
        }
        n++;
        at += (size_t)len + 12;
    }
    checkf(ended && at == f->n, "%s: IEND ends the file", what);
    checkf(idats > 0 && zn >= 6, "%s: data", what);
    checkf(zn >= 2 && z[0] == 0x78 && z[1] == 0x9c && (z[0] * 256 + z[1]) % 31 == 0, "%s: zlib's header", what);
    rgb = malloc((size_t)m->w * m->h * bpp);
    if(zn >= 6 && !png_rows(z, zn, m->w, m->h, bpp, rgb, what)) {
        for(y = 0; y < m->h; y++)
            for(x = 0; x < m->w; x++) {
                const unsigned char *q = rgb + ((size_t)y * m->w + x) * bpp;
                unsigned int c = m->px[(size_t)y * m->w + x];
                if(q[0] != (c >> 16 & 255) || q[1] != (c >> 8 & 255) || q[2] != (c & 255) || (alpha && q[3] != 255 - (c >> 24)))
                    same = 0;
            }
        checkf(same, "%s: the rows are the picture's, filters undone", what);
    }
    free(rgb);
    free(z);
}

/**** writing and reading again ****/

static void write_file(const char *name, const char *ext, const void *p, size_t n) {
    char path[600];
    FILE *f;
    if(!outdir) return;
    snprintf(path, sizeof(path), "%s/%s%s", outdir, name, ext);
    f = fopen(path, "wb");
    if(!f || fwrite(p, 1, n, f) != n) {
        printf("cannot write %s\n", path);
        exit(2);
    }
    fclose(f);
}

static void write_rgba(const char *name, const struct wimg *m) {
    size_t i, n = (size_t)m->w * m->h;
    unsigned char *q = malloc(n * 4);
    for(i = 0; i < n; i++) {
        unsigned int c = m->px[i];
        q[i * 4] = (unsigned char)(c >> 16);
        q[i * 4 + 1] = (unsigned char)(c >> 8);
        q[i * 4 + 2] = (unsigned char)c;
        q[i * 4 + 3] = (unsigned char)(255 - (c >> 24));
    }
    write_file(name, ".rgba", q, n * 4);
    free(q);
}

/* The picture back from the file: as it was, its transparency gone unless keep */
static void again(const struct mem *f, const struct wimg *m, int keep, const char *what) {
    struct wimg r;
    char err[WIMG_ERR_MAX];
    long before = ut_alloc_live;
    size_t i, n = (size_t)m->w * m->h, bad = 0;
    if(wimg_decode(f->p, f->n, &r, err, sizeof(err))) {
        checkf(0, "%s reads again: %s", what, err);
        return;
    }
    checkf(r.w == m->w && r.h == m->h, "%s reads again at its size", what);
    if(r.w == m->w && r.h == m->h)
        for(i = 0; i < n; i++) if(r.px[i] != (keep ? m->px[i] : m->px[i] & 0xffffff)) bad++;
    checkf(!bad, "%s reads again pixel for pixel (%zu differ)", what, bad);
    wimg_free(&r);
    checkf(ut_alloc_live == before, "%s: nothing held", what);
}

static void round_trip(int w, int h, int kind) {
    struct wimg m;
    struct mem f;
    char name[64], what[96], err[WIMG_ERR_MAX];
    long before;
    int alpha;
    make(&m, w, h, kind);
    snprintf(name, sizeof(name), "%s-%dx%d", kinds[kind], w, h);
    if(outdir) write_rgba(name, &m);
    /* BMP */
    snprintf(what, sizeof(what), "%s.bmp", name);
    mem_start(&f, -1);
    before = ut_alloc_live;
    checkf(!wimg_save_bmp(&m, to_mem, &f, err, sizeof(err)), "%s is written", what);
    checkf(ut_alloc_live == before, "%s: nothing held after", what);
    bmp_structure(&f, &m, what);
    again(&f, &m, 0, what);
    write_file(name, ".bmp", f.p, f.n);
    free(f.p);
    /* PNG, RGB and RGBA */
    for(alpha = 0; alpha < 2; alpha++) {
        snprintf(what, sizeof(what), "%s%s.png", name, alpha ? "-rgba" : "");
        mem_start(&f, -1);
        before = ut_alloc_live;
        checkf(!wimg_save_png(&m, alpha, to_mem, &f, err, sizeof(err)), "%s is written", what);
        checkf(ut_alloc_live == before, "%s: nothing held after", what);
        png_structure(&f, &m, alpha, what);
        again(&f, &m, alpha, what);
        write_file(name, alpha ? "-rgba.png" : ".png", f.p, f.n);
        free(f.p);
    }
    free(m.px);
}

/**** failures ****/

typedef int (*saver)(const struct wimg *m, int alpha, wimg_put put, void *ctx, char *err, size_t cap);
static int save_bmp(const struct wimg *m, int alpha, wimg_put put, void *ctx, char *err, size_t cap) {
    (void)alpha;
    return wimg_save_bmp(m, put, ctx, err, cap);
}
static int save_png(const struct wimg *m, int alpha, wimg_put put, void *ctx, char *err, size_t cap) {
    return wimg_save_png(m, alpha, put, ctx, err, cap);
}

static void failing(saver save, int alpha, const char *what) {
    struct wimg m;
    struct mem f;
    char err[WIMG_ERR_MAX];
    long calls, puts, n;
    make(&m, 300, 120, NOISE);                  /* several IDATs, and several of BMP's buffers */
    mem_start(&f, -1);
    ut_alloc_calls = 0;
    check(!save(&m, alpha, to_mem, &f, err, sizeof(err)), what);
    calls = ut_alloc_calls;
    puts = f.calls;
    free(f.p);
    checkf(calls > 0 && puts > 2, "%s: %ld allocations, %ld calls of put()", what, calls, puts);
    for(n = 0; n < calls; n++) {
        long before = ut_alloc_live;
        mem_start(&f, -1);
        err[0] = 0;
        ut_alloc_fail_after = n;
        checkf(save(&m, alpha, to_mem, &f, err, sizeof(err)) == -1 && err[0], "%s: allocation %ld failing fails it", what, n);
        ut_alloc_fail_after = -1;
        checkf(ut_alloc_live == before && f.calls == 0, "%s: allocation %ld failing: nothing held, nothing written", what, n);
        free(f.p);
    }
    for(n = 0; n < puts; n++) {
        long before = ut_alloc_live;
        mem_start(&f, n);
        err[0] = 0;
        checkf(save(&m, alpha, to_mem, &f, err, sizeof(err)) == -1 && err[0], "%s: put() %ld failing fails it", what, n);
        checkf(ut_alloc_live == before, "%s: put() %ld failing: nothing held", what, n);
        checkf(f.after == 0, "%s: put() %ld failing: no call after it", what, n);
        free(f.p);
    }
    free(m.px);
}

static void limits(void) {
    static const struct { int w, h; const char *what; } t[] = {
        { 0, 1, "width 0" }, { 1, 0, "height 0" }, { -1, 5, "width -1" }, { 4097, 1, "4097 wide" }, { 1, 4097, "4097 high" },
        { 4096, 3907, "16,003,072 pixels" }, { 0x7fffffff, 0x7fffffff, "the largest" }
    };
    static unsigned int one;
    struct wimg m;
    struct mem f;
    char err[WIMG_ERR_MAX];
    size_t i;
    int k;
    for(i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        for(k = 0; k < 3; k++) {
            m.w = t[i].w;
            m.h = t[i].h;
            m.px = &one;
            mem_start(&f, -1);
            ut_alloc_calls = 0;
            err[0] = 0;
            checkf((k ? wimg_save_png(&m, k - 1, to_mem, &f, err, sizeof(err)) : wimg_save_bmp(&m, to_mem, &f, err, sizeof(err))) == -1 &&
                   err[0] && !ut_alloc_calls && !f.calls, "%s refused before anything", t[i].what);
        }
    m.w = m.h = 1;
    m.px = NULL;
    mem_start(&f, -1);
    check(wimg_save_bmp(&m, to_mem, &f, err, sizeof(err)) == -1 && wimg_save_png(&m, 1, to_mem, &f, err, sizeof(err)) == -1 &&
          !f.calls, "no pixels: refused");
    check(wimg_save_bmp(NULL, to_mem, &f, err, sizeof(err)) == -1 && wimg_save_png(NULL, 0, to_mem, &f, err, sizeof(err)) == -1,
          "no picture: refused");
    /* at the limits: 4000 x 4000, 4096 x 3906; each begins (put() refuses its first call, so no more is done) */
    {
        static const int sides[2][2] = { { 4000, 4000 }, { 4096, 3906 } };
        for(k = 0; k < 2; k++) {
            m.w = sides[k][0];
            m.h = sides[k][1];
            m.px = calloc((size_t)m.w * m.h, sizeof(*m.px));
            mem_start(&f, 0);
            checkf(wimg_save_bmp(&m, to_mem, &f, err, sizeof(err)) == -1 && f.calls == 1 && strstr(err, "written"),
                   "%dx%d: a BMP begins", m.w, m.h);
            mem_start(&f, 0);
            checkf(wimg_save_png(&m, 1, to_mem, &f, err, sizeof(err)) == -1 && f.calls == 1 && strstr(err, "written"),
                   "%dx%d: a PNG begins", m.w, m.h);
            free(m.px);
        }
    }
}

int main(int argc, char **argv) {
    static const int sizes[][2] = {
        { 1, 1 }, { 1, 7 }, { 7, 1 }, { 2, 2 }, { 3, 5 }, { 5, 3 }, { 4, 4 }, { 17, 13 }, { 64, 64 }, { 255, 2 }, { 333, 77 },
        { 1000, 1 }, { 4096, 1 }, { 1, 4096 }, { 4096, 3 }
    };
    size_t i;
    int k;
    if(argc == 3 && !strcmp(argv[1], "--write")) outdir = argv[2];
    else if(argc != 1) {
        printf("usage: test_image_save [--write DIR]\n");
        return 2;
    }
    for(i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        for(k = SMOOTH; k <= CHECKER; k++) round_trip(sizes[i][0], sizes[i][1], k);
    round_trip(640, 480, NOISE);               /* about 900 KB that will not compress: 28 IDATs or so */
    round_trip(640, 480, SMOOTH);
    for(k = 0; k < 5; k++) checkf(filters_seen[k] > 0, "filter %d chosen somewhere (%d rows)", k, filters_seen[k]);
    failing(save_bmp, 0, "a BMP");
    failing(save_png, 0, "an RGB PNG");
    failing(save_png, 1, "an RGBA PNG");
    limits();
    check(ut_alloc_live == 0, "nothing held at the end");
    if(failures) {
        printf("IMAGE SAVE TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("IMAGE SAVE TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
