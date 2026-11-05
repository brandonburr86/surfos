/*
SurfOS user library: pictures at other sizes
----------------------
File: image_scale.c  Date: 11/1/26
----------------------
image.h's making pictures smaller or larger, in whole numbers: Wundertow's helper's, for what is large, and its
window's, for what is shown smaller than it is kept. Apart from the decoders, which a window has no use for.

11/05/26 (the desktop applications, track img): moved here from user/wundertow/ as it was.
*/

#include <stddef.h>
#include <stdlib.h>
#include <image.h>

/* The average of n pixels whose channels are summed: transparency counts as a channel. Colours of pixels that are not
   there at all do not count; a pixel half there counts in full, which is near enough for a picture's edge. Each sum is
   within an unsigned long of 32 bits: 255 x WIMG_PIXELS_MAX is 4.08e9 (16.8 million pixels would not be). */
static unsigned int average(unsigned long a, unsigned long r, unsigned long g, unsigned long b, unsigned long n,
                            unsigned long shown) {
    unsigned long t;
    if(!shown) return WIMG_GONE;
    t = (a + n / 2) / n;
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
