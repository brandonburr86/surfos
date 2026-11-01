/*
SurfOS Undertow's window: pictures
----------------------
File: image.h  Date: 11/1/26
----------------------
A file's bytes become pixels: BMP, GIF (its first frame), PNG (with an inflate of its own) and JPEG (baseline,
extended sequential and progressive), each its own file (image_bmp.c, image_gif.c, image_png.c, image_jpeg.c), in
integers only, with no library. What kind a file is comes from its first bytes, never from what the server says.

The bytes are a server's, so they are hostile. Every field is checked against the bytes there are before it is
used, sizes are worked out in 64 bits, memory is bounded before it is asked for and every allocation is checked,
and each loop ends with the input: a decoder either gives a whole picture or says why not, and nothing in between
(a truncated or corrupted file is no picture; its frame stays on the page).

A pixel is 0xTTRRGGBB: TT is how transparent it is, 0 solid to 255 not there at all, so a solid pixel is plain
0x00RRGGBB. Rows run from the top, w pixels each, one after the other.

Limits: WIMG_SIDE_MAX pixels a side and WIMG_PIXELS_MAX pixels in all, before anything is allocated; a progressive
JPEG's coefficients WIMG_COEFS_MAX bytes; WIMG_SCANS_MAX scans. The page's own limit on what it keeps of them
(WB_PAGE_IMAGES) is browser.h's.
*/

#ifndef WUNDERTOW_IMAGE_H
#define WUNDERTOW_IMAGE_H

#include <stddef.h>

#define WIMG_SIDE_MAX    4096                   /* pixels a side */
#define WIMG_PIXELS_MAX  16000000u              /* 16 megapixels */
#define WIMG_COEFS_MAX   (64u << 20)            /* the bytes of a progressive JPEG's coefficients */
#define WIMG_SCANS_MAX   128                    /* a JPEG's scans (a real one has at most a dozen or so) */
#define WIMG_KEEP_SIDE   2048                   /* a picture larger than this a side is kept smaller (wimg_shrink()) */

#define WIMG_SOLID(c)    (((c) >> 24) == 0)
#define WIMG_GONE        0xff000000u            /* a pixel that is not there */

struct wimg {
    int w, h;
    unsigned int *px;                           /* w x h pixels, malloc'ed */
};

/* in[0 .. len) as a picture: 0 and *out (free it with wimg_free()), or -1 with why in err (cap bytes, safe text) and
   *out zeroed. */
int wimg_decode(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap);
void wimg_free(struct wimg *m);

/* Is in the start of a picture of a kind read here? */
int wimg_known(const unsigned char *in, size_t len);

/* Make a picture wider or taller than side smaller by a whole factor, each pixel the average of those it stands for;
   the rest are left as they are. 1 when it shrank, 0 when it had no need to. No allocation: it cannot fail. */
int wimg_shrink(struct wimg *m, int side);

/* The picture src (sw x sh) in dst (dw x dh, rows of dw pixels): where it shrinks, each pixel the average of those it
   stands for; where it grows, the nearest. Whole numbers only. */
void wimg_scale(const unsigned int *src, int sw, int sh, unsigned int *dst, int dw, int dh);

/* The decoders' common calls (image.c) */
int wimg_alloc(struct wimg *m, int w, int h, char *err, size_t cap);    /* limits, then the pixels: uninitialized */
int wimg_fail(struct wimg *m, char *err, size_t cap, const char *why);  /* frees m, says why: -1 */

/* The decoders: in starts with what wimg_known() found */
int wimg_bmp(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap);
int wimg_gif(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap);
int wimg_png(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap);
int wimg_jpeg(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap);

#endif
