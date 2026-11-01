/*
SurfOS Undertow's window: pictures
----------------------
File: image.c  Date: 11/1/26
----------------------
image.h's calls that do not depend on a file's format: which decoder a file goes to, the limits every decoder
meets before it allocates, and making pictures smaller or larger in whole numbers.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "undertow.h"
#include "image.h"

static int is_png(const unsigned char *in, size_t len) { return len >= 8 && !memcmp(in, "\x89PNG\r\n\x1a\n", 8); }
static int is_jpeg(const unsigned char *in, size_t len) { return len >= 3 && in[0] == 0xff && in[1] == 0xd8 && in[2] == 0xff; }
static int is_gif(const unsigned char *in, size_t len) { return len >= 6 && (!memcmp(in, "GIF87a", 6) || !memcmp(in, "GIF89a", 6)); }
static int is_bmp(const unsigned char *in, size_t len) { return len >= 2 && in[0] == 'B' && in[1] == 'M'; }

int wimg_known(const unsigned char *in, size_t len) {
    return in && (is_png(in, len) || is_jpeg(in, len) || is_gif(in, len) || is_bmp(in, len));
}

int wimg_decode(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap) {
    memset(out, 0, sizeof(*out));
    if(!in) return ut_error(err, cap, "there is no picture");
    if(is_png(in, len)) return wimg_png(in, len, out, err, cap);
    if(is_jpeg(in, len)) return wimg_jpeg(in, len, out, err, cap);
    if(is_gif(in, len)) return wimg_gif(in, len, out, err, cap);
    if(is_bmp(in, len)) return wimg_bmp(in, len, out, err, cap);
    return ut_error(err, cap, "not a picture this browser reads (BMP, GIF, PNG, JPEG)");
}

void wimg_free(struct wimg *m) {
    if(!m) return;
    free(m->px);
    memset(m, 0, sizeof(*m));
}

int wimg_alloc(struct wimg *m, int w, int h, char *err, size_t cap) {
    unsigned long long n = (unsigned long long)(unsigned int)w * (unsigned int)h;
    if(w <= 0 || h <= 0) return ut_error(err, cap, "a picture of no size");
    if(w > WIMG_SIDE_MAX || h > WIMG_SIDE_MAX || n > WIMG_PIXELS_MAX)
        return ut_error(err, cap, "a picture of %d x %d pixels: over the limit of %d a side and 16 megapixels", w, h,
                        WIMG_SIDE_MAX);
    m->px = malloc((size_t)n * sizeof(*m->px));
    if(!m->px) return ut_error(err, cap, "out of memory for a picture of %d x %d pixels", w, h);
    m->w = w;
    m->h = h;
    return 0;
}

int wimg_fail(struct wimg *m, char *err, size_t cap, const char *why) {
    wimg_free(m);
    return ut_error(err, cap, "%s", why);
}

/**** sizes ****/

/* The average of n pixels whose channels are summed: transparency counts as a channel. Colours of pixels that are not
   there at all do not count; a pixel half there counts in full, which is near enough for a picture's edge. */
static unsigned int average(unsigned long a, unsigned long r, unsigned long g, unsigned long b, unsigned long n,
                            unsigned long shown) {
    unsigned long t = (a + n / 2) / n;
    if(!shown) return WIMG_GONE;
    return (unsigned int)t << 24 | (unsigned int)((r + shown / 2) / shown) << 16 |
           (unsigned int)((g + shown / 2) / shown) << 8 | (unsigned int)((b + shown / 2) / shown);
}

/* the pixels x0 .. x1, y0 .. y1 of src, averaged */
static unsigned int block(const unsigned int *src, int sw, int x0, int x1, int y0, int y1) {
    unsigned long a = 0, r = 0, g = 0, b = 0, n = 0, shown = 0;
    int x, y;
    for(y = y0; y < y1; y++) {
        const unsigned int *row = src + (size_t)y * sw;
        for(x = x0; x < x1; x++) {
            unsigned int c = row[x];
            a += c >> 24;
            n++;
            if((c >> 24) == 255) continue;
            r += c >> 16 & 255;
            g += c >> 8 & 255;
            b += c & 255;
            shown++;
        }
    }
    return average(a, r, g, b, n, shown);
}

int wimg_shrink(struct wimg *m, int side) {
    int big = m->w > m->h ? m->w : m->h, f, x, y, w, h;
    unsigned int *p;
    if(side < 1 || big <= side || !m->px) return 0;
    f = (big + side - 1) / side;                    /* the smallest whole factor that fits */
    w = (m->w + f - 1) / f;
    h = (m->h + f - 1) / f;
    for(y = 0; y < h; y++) {                        /* each block's first pixel is at or after where its average goes */
        int y1 = (y + 1) * f < m->h ? (y + 1) * f : m->h;
        for(x = 0; x < w; x++) {
            int x1 = (x + 1) * f < m->w ? (x + 1) * f : m->w;
            m->px[(size_t)y * w + x] = block(m->px, m->w, x * f, x1, y * f, y1);
        }
    }
    p = realloc(m->px, (size_t)w * h * sizeof(*m->px));
    if(p) m->px = p;                                /* else it keeps the larger block */
    m->w = w;
    m->h = h;
    return 1;
}

void wimg_scale(const unsigned int *src, int sw, int sh, unsigned int *dst, int dw, int dh) {
    int x, y;
    for(y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * sh / dh), y1 = (int)((long long)(y + 1) * sh / dh);
        if(y1 <= y0) y1 = y0 + 1;
        if(y1 > sh) y1 = sh;
        for(x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * sw / dw), x1 = (int)((long long)(x + 1) * sw / dw);
            if(x1 <= x0) x1 = x0 + 1;
            if(x1 > sw) x1 = sw;
            dst[(size_t)y * dw + x] = x1 - x0 == 1 && y1 - y0 == 1 ? src[(size_t)y0 * sw + x0] : block(src, sw, x0, x1, y0, y1);
        }
    }
}
