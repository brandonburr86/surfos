/*
SurfOS user library: pictures
----------------------
File: image.h  Date: 11/1/26
----------------------
A file's bytes become pixels: BMP, GIF (its first frame), PNG (through libsurf's inflate) and JPEG (baseline,
extended sequential and progressive), each its own file (image_bmp.c, image_gif.c, image_png.c, image_jpeg.c), in
integers only, with no library. What kind a file is comes from its first bytes, never from what a server or a
file's name says.

The bytes are hostile: a server's, or a file's that anyone may have made. Every field is checked against the bytes
there are before it is used, sizes are worked out in 64 bits, memory is bounded before it is asked for and every
allocation is checked, and each loop ends with the input: a decoder either gives a whole picture or says why not,
and nothing in between (a truncated or corrupted file is no picture).

A pixel is 0xTTRRGGBB: TT is how transparent it is, 0 solid to 255 not there at all, so a solid pixel is plain
0x00RRGGBB. Rows run from the top, w pixels each, one after the other.

Limits: WIMG_SIDE_MAX pixels a side and WIMG_PIXELS_MAX pixels in all, before anything is allocated; a progressive
JPEG's coefficients WIMG_COEFS_MAX bytes; WIMG_SCANS_MAX scans. What a program keeps of them is its own limit
(Wundertow's page: WB_PAGE_IMAGES, browser.h).

11/05/26 (the desktop applications, track img): moved here from user/wundertow/, so that every program reads
pictures the same way; the decoders are as they were, and the PNG decoder's inflate is libsurf's (inflate.h). New:
pictures written (image_save.c), as a BMP of 24 bits or a PNG of 8 bits a sample, RGB or RGBA, compressed by
libsurf's deflate. The bytes go to the caller's put() as they are made, so a picture is never held twice: a program
saves one through user/bin/saveutil.h by writing them to its file. Only a picture within wimg_alloc()'s limits is
written, so that whatever is written here is read here again.
*/

#ifndef _IMAGE_H
#define _IMAGE_H

#include <stddef.h>

#define WIMG_SIDE_MAX    4096                   /* pixels a side */
#define WIMG_PIXELS_MAX  16000000u              /* 16 megapixels */
#define WIMG_COEFS_MAX   (64u << 20)            /* the bytes of a progressive JPEG's coefficients */
#define WIMG_SCANS_MAX   128                    /* a JPEG's scans (a real one has at most a dozen or so) */
#define WIMG_KEEP_SIDE   2048                   /* a picture larger than this a side is kept smaller (wimg_shrink()) */
#define WIMG_ERR_MAX     160                    /* an error buffer that holds any of these calls' messages */

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

/* Writing a picture (image_save.c): its bytes go to put(ctx, p, n) in pieces of any size as they are made. put
   returns 0, or -1 to stop (a write that failed: errno is the caller's to keep), and the save then returns -1.
   0, or -1 with why in err: a picture over wimg_alloc()'s limits, no memory, or put's -1. Nothing is held after. */
typedef int (*wimg_put)(void *ctx, const void *p, size_t n);

/* A BMP of 24 bits: the 40-byte header, rows from the bottom. BMP has no transparency here: each pixel's colour is
   written as it is, whatever its TT. */
int wimg_save_bmp(const struct wimg *m, wimg_put put, void *ctx, char *err, size_t cap);

/* A PNG of 8 bits a sample, not interlaced: RGB (alpha 0), each pixel's colour as it is; or RGBA (alpha 1), its TT
   as the alpha 255 - TT. Each row's filter is the one the PNG standard suggests choosing (the least sum of its bytes
   as signed numbers), and the rows are compressed by libsurf's deflate at level 6. */
int wimg_save_png(const struct wimg *m, int alpha, wimg_put put, void *ctx, char *err, size_t cap);

/* The decoders' common calls (image.c) */
int wimg_alloc(struct wimg *m, int w, int h, char *err, size_t cap);    /* limits, then the pixels: uninitialized */
int wimg_fail(struct wimg *m, char *err, size_t cap, const char *why);  /* frees m, says why: -1 */
int wimg_error(char *err, size_t cap, const char *fmt, ...)             /* says why in err as safe text: -1 */
    __attribute__((format(printf, 3, 4)));

/* The decoders: in starts with what wimg_known() found */
int wimg_bmp(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap);
int wimg_gif(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap);
int wimg_png(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap);
int wimg_jpeg(const unsigned char *in, size_t len, struct wimg *out, char *err, size_t cap);

#endif
