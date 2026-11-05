/*
SurfOS user library: pictures
----------------------
File: image.c  Date: 11/1/26
----------------------
image.h's calls that do not depend on a file's format: which decoder a file goes to, and the limits every decoder
meets before it allocates.

11/05/26 (the desktop applications, track img): moved here from user/wundertow/ with the decoders. Their messages
were Undertow's ut_error(); wimg_error() makes them the same way (formatted apart, then cut to the caller's buffer,
anything but printable ASCII as '?'), so that the decoders need nothing of the browser.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <image.h>

static int is_png(const unsigned char *in, size_t len) { return len >= 8 && !memcmp(in, "\x89PNG\r\n\x1a\n", 8); }
static int is_jpeg(const unsigned char *in, size_t len) { return len >= 3 && in[0] == 0xff && in[1] == 0xd8 && in[2] == 0xff; }
static int is_gif(const unsigned char *in, size_t len) { return len >= 6 && (!memcmp(in, "GIF87a", 6) || !memcmp(in, "GIF89a", 6)); }
static int is_bmp(const unsigned char *in, size_t len) { return len >= 2 && in[0] == 'B' && in[1] == 'M'; }

int wimg_error(char *err, size_t cap, const char *fmt, ...) {
    char tmp[WIMG_ERR_MAX];                     /* an argument may be err itself: format apart first */
    va_list ap;
    size_t i;
    if(!err || !cap) return -1;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    for(i = 0; tmp[i] && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)tmp[i];
        err[i] = c < 32 || c >= 127 ? '?' : (char)c;
    }
    err[i] = 0;
    return -1;
}

int wimg_known(const unsigned char *in, size_t len) {
    return in && (is_png(in, len) || is_jpeg(in, len) || is_gif(in, len) || is_bmp(in, len));
}

int wimg_decode(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap) {
    memset(out, 0, sizeof(*out));
    if(!in) return wimg_error(err, cap, "there is no picture");
    if(is_png(in, len)) return wimg_png(in, len, out, err, cap);
    if(is_jpeg(in, len)) return wimg_jpeg(in, len, out, err, cap);
    if(is_gif(in, len)) return wimg_gif(in, len, out, err, cap);
    if(is_bmp(in, len)) return wimg_bmp(in, len, out, err, cap);
    return wimg_error(err, cap, "not a picture of a kind read here (BMP, GIF, PNG, JPEG)");
}

void wimg_free(struct wimg *m) {
    if(!m) return;
    free(m->px);
    memset(m, 0, sizeof(*m));
}

int wimg_alloc(struct wimg *m, int w, int h, char *err, size_t cap) {
    unsigned long long n = (unsigned long long)(unsigned int)w * (unsigned int)h;
    if(w <= 0 || h <= 0) return wimg_error(err, cap, "a picture of no size");
    if(w > WIMG_SIDE_MAX || h > WIMG_SIDE_MAX || n > WIMG_PIXELS_MAX)
        return wimg_error(err, cap, "a picture of %d x %d pixels: over the limit of %d a side and 16 megapixels", w, h,
                          WIMG_SIDE_MAX);
    m->px = malloc((size_t)n * sizeof(*m->px));
    if(!m->px) return wimg_error(err, cap, "out of memory for a picture of %d x %d pixels", w, h);
    m->w = w;
    m->h = h;
    return 0;
}

int wimg_fail(struct wimg *m, char *err, size_t cap, const char *why) {
    wimg_free(m);
    return wimg_error(err, cap, "%s", why);
}
