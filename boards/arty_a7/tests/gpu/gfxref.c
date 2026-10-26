/*
SurfOS host tool: test-gpuos's pictures from GPU.md alone
--------------------
File: gfxref.c      Date: 10/25/26 (SurfGPU G3)
--------------------
  gfxref [--ppm DIR]

What the gfx lines of test-gpuos (boards/arty_a7/tests/gpuos_test.py, whose first scene is GPU.md
section 15.6's) leave on the screen, worked out without SurfOS's driver or gfx command (gpu.c,
gfx.c), so that the CRCs SurfOS prints are checked against something that does not share their
code. Each line's command words are written here by hand from GPU.md section 5 and fed to the C
model's engine (sim/models/gpu_model.c); the same pixels are also computed directly from GPU.md's
rules (a fill's rectangle, section 5's LINE algorithm, a copy whose source is read whole before
anything is written, the console font's bits for text, one byte for a put), and the two VRAMs
must be equal. The frame is made as section 3 says (GFX_BASE, GFX_STRIDE, GFX_SCALE, GPAL: the
RGB332 that the configuration leaves, or section 15.1's ramps) and its FRAME_CRC computed as
section 3 says (zlib's CRC-32 of the pixels as little-endian 16-bit words); the model's display
path (gpu_display_render, gpu_frame_crc) must give the same. It prints

  line <gfx line>             every gfx line of the test, in order (test-gpuos checks they are its own)
  expect <n> <crc> mode 2     the n-th gfx crc, where the display shows graphics alone
  expect <n> - mode <m>       modes 1 and 3: the console's text is part of the frame

and with --ppm DIR the graphics of each gfx crc in mode 2 or 3 as DIR/crc-<n>.ppm (in mode 3 what
shows where no character is). The lines that leave the picture as it was (gfx, get, the demos,
which put VRAM, GPAL, the display, the target and the clip back) only take their place; after
gfx bench, which draws on the target, VRAM counts as unknown until a fill covers all of it.
Exit status 0 when the model's VRAM and the direct pixels, and the two CRCs, agreed throughout.

10/26/26 (GPU2, GPU.md section 16): the depth buffer (SET_ZBUF, and its clearing as 16.6 gives the
words), TRIANGLE and FLIP the same way: their words written here from 16.2 and 16.6 and fed to the
model, and the pixels and depths worked out directly from 16.3 (coverage: exact edge functions at
the pixel centres, the top-left rule, the box), 16.4 (the planes: floor gradients with 20
fractional bits, values relative to vertex 0, rounded to the nearest, clamped) and 16.5 (the depth
test, then the depth, then the colour); gfx cube puts everything back, so it only takes its
place, as the demos do. These have nothing of the model's code: a mistake in either shows as two
VRAMs that differ.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_model.h"

extern const uint8_t font8x16[256][16];

#define VRAM    307200u                         /* GPU.md section 2: 300 KiB */

/* GPU.md section 5: the opcode in header bits 31:24; coordinates and sizes as a | b << 16 */
#define OP_SET_TARGET   0x01u
#define OP_FILL_RECT    0x03u
#define OP_COPY_RECT    0x04u
#define OP_LINE         0x05u
#define OP_EXPAND       0x06u
#define COPY_KEY        (1u << 8)               /* COPY_RECT: 7:0 key, 8 KEY */
#define EXPAND_TRANSP   (1u << 16)              /* EXPAND: 7:0 fg, 15:8 bg, 16 TRANSPARENT */
/* GPU.md 16.2 */
#define OP_SET_ZBUF     0x0Au
#define OP_TRIANGLE     0x0Bu
#define OP_FLIP         0x0Cu
#define TRI_GOURAUD     (1u << 8)
#define TRI_ZTEST       (1u << 9)
#define TRI_ZFUNC_LE    (1u << 10)
#define TRI_ZWRITE      (1u << 11)
#define TRI_CULL_CW     (1u << 12)
#define TRI_CULL_CCW    (1u << 13)

static uint8_t vm[VRAM];                        /* the model engine's VRAM, fed the words */
static uint8_t vp[VRAM];                        /* the pixels computed directly */
static uint8_t src[VRAM];
static struct gpu_engine eng;
static uint32_t tb = 0, ts = 640, tw = 640, th = 480;          /* the target: the reset's */
static int mode = 1;                            /* SurfOS starts in text mode, */
static uint32_t gbase = 0, gstride = 640, gscale = 1;           /* GFX_ registers at their reset values */
static uint16_t gpal[256];
static int known, ncrc, bad;
static int zvalid;                              /* GPU2: the depth buffer (16.2) */
static uint32_t zbase, zstride;
static const char *ppm;
static uint16_t frame[640 * 480], mframe[640 * 480];

static uint32_t pair(int a, int b) { return ((uint32_t)a & 0xFFFFu) | ((uint32_t)b & 0xFFFFu) << 16; }
static void word(uint32_t w) { gpu_engine_word(&eng, w); }

/* a pixel of the target (no command here sets a clip: SET_TARGET's is the whole target) */
static void plot(int x, int y, uint8_t c) {
    if(x >= 0 && y >= 0 && (uint32_t)x < tw && (uint32_t)y < th) vp[tb + (uint32_t)y * ts + (uint32_t)x] = c;
}

static void target(uint32_t base, uint32_t stride, uint32_t w, uint32_t h) {
    word(OP_SET_TARGET << 24);
    word(base);
    word(stride);
    word(pair((int)w, (int)h));
    tb = base;
    ts = stride;
    tw = w;
    th = h;
    zvalid = 0;                                 /* 16.2: an accepted SET_TARGET leaves no depth buffer */
}

static void fill(int x, int y, int w, int h, uint8_t c) {
    int i, j;
    word(OP_FILL_RECT << 24 | c);
    word(pair(x, y));
    word(pair(w, h));
    for(j = y; j < y + h; j++)
        for(i = x; i < x + w; i++) plot(i, j, c);
    if(tb == 0 && ts == tw && tw * th == VRAM && x <= 0 && y <= 0 && x + w >= (int)tw && y + h >= (int)th) known = 1;
}

static void line(int x0, int y0, int x1, int y1, uint8_t c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy, e2;
    word(OP_LINE << 24 | c);
    word(pair(x0, y0));
    word(pair(x1, y1));
    for(;;) {                                   /* GPU.md section 5's algorithm, as it is written there */
        plot(x0, y0, c);
        if(x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if(e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if(e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* gfx rect: the outline of [x, x + w) x [y, y + h), its far corner (x + w - 1, y + h - 1)
   (GPU.md 15.2), as four LINEs along its edges */
static void outline(int x, int y, int w, int h, uint8_t c) {
    line(x, y, x + w - 1, y, c);
    line(x, y + h - 1, x + w - 1, y + h - 1, c);
    line(x, y, x, y + h - 1, c);
    line(x + w - 1, y, x + w - 1, y + h - 1, c);
}

/* COPY_RECT: destination (px, py) is written when its source is inside the target, with the
   source's value as it was before the command (and not when that is the key) */
static void copy(int sx, int sy, int dx, int dy, int w, int h, int key) {
    int i, j;
    word(OP_COPY_RECT << 24 | (key >= 0 ? COPY_KEY | (uint32_t)key : 0));
    word(pair(sx, sy));
    word(pair(dx, dy));
    word(pair(w, h));
    memcpy(src, vp, sizeof(src));
    for(j = 0; j < h; j++)
        for(i = 0; i < w; i++) {
            int qx = sx + i, qy = sy + j;
            uint8_t v;
            if(qx < 0 || qy < 0 || (uint32_t)qx >= tw || (uint32_t)qy >= th) continue;
            v = src[tb + (uint32_t)qy * ts + (uint32_t)qx];
            if(key < 0 || v != key) plot(dx + i, dy + j, v);
        }
}

/* gfx text: the words joined by one space, one cell of the console's font a character (these are
   ASCII, the same cells in code page 437); EXPAND of 8n x 16, the bitmap's rows in raster order,
   32 bits a word, the first pixel in bit 31 */
static void text(int x, int y, uint8_t fg, int bg, const char *s) {
    unsigned n = (unsigned)strlen(s), w = 8 * n, bits = w * 16, k, b, i, r;
    word(OP_EXPAND << 24 | fg | (bg < 0 ? EXPAND_TRANSP : (uint32_t)bg << 8));
    word(pair(x, y));
    word(pair((int)w, 16));
    for(k = 0; k < (bits + 31) / 32; k++) {
        uint32_t v = 0;
        for(b = 0; b < 32 && k * 32 + b < bits; b++) {
            unsigned p = k * 32 + b, px = p % w, py = p / w;
            v |= (uint32_t)(font8x16[(uint8_t)s[px / 8]][py] >> (7 - px % 8) & 1) << (31 - b);
        }
        word(v);
    }
    for(i = 0; i < n; i++)
        for(r = 0; r < 16; r++)
            for(b = 0; b < 8; b++) {
                if(font8x16[(uint8_t)s[i]][r] >> (7 - b) & 1) plot(x + (int)(8 * i + b), y + (int)r, fg);
                else if(bg >= 0) plot(x + (int)(8 * i + b), y + (int)r, (uint8_t)bg);
            }
}

/* gfx put: the target's pixel written through the CPU's VRAM window, as a byte */
static void put(int x, int y, uint8_t c) {
    uint32_t a = tb + (uint32_t)y * ts + (uint32_t)x;
    vm[a] = vp[a] = c;
}

/**** GPU2: GPU.md section 16 ****/

/* gfx zbuf base stride: SET_ZBUF (16.2) */
static void zbuf(uint32_t base, uint32_t stride) {
    word(OP_SET_ZBUF << 24);
    word(base);
    word(stride);
    zvalid = 1;
    zbase = base;
    zstride = stride;
}

/* gfx zclear [b]: 16.6's words, a FILL_RECT of b over the depth bytes (2W of them a line) with a
   target set to them, the target back, SET_ZBUF again; directly, every depth byte b */
static void zclear(uint8_t b) {
    uint32_t x, y;
    word(OP_SET_TARGET << 24); word(zbase); word(zstride); word(pair((int)(2 * tw), (int)th));
    word(OP_FILL_RECT << 24 | b); word(pair(0, 0)); word(pair((int)(2 * tw), (int)th));
    word(OP_SET_TARGET << 24); word(tb); word(ts); word(pair((int)tw, (int)th));
    word(OP_SET_ZBUF << 24); word(zbase); word(zstride);
    for(y = 0; y < th; y++)
        for(x = 0; x < 2 * tw; x++) vp[zbase + y * zstride + x] = b;
}

/* floor(n / d) for d > 0, whatever the sign of n */
static __int128 floordiv(__int128 n, int64_t d) {
    __int128 q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

/* 16.4: a vertex quantity's plane: GX, GY = floor(N * 2^20 / A), N's from the vertices in the order
   given; the value at a centre: V = v0 2^20 + GX (cx - X0) + GY (cy - Y0), rounded to the nearest
   (halves up), clamped to the vertices' smallest and largest */
struct plane { __int128 gx, gy; int64_t v0, lo, hi; };

static void plane_of(struct plane *p, const int32_t *X, const int32_t *Y, const int64_t *v, int64_t a) {
    __int128 nx = (__int128)(v[1] - v[0]) * (Y[2] - Y[0]) - (__int128)(v[2] - v[0]) * (Y[1] - Y[0]);
    __int128 ny = (__int128)(v[2] - v[0]) * (X[1] - X[0]) - (__int128)(v[1] - v[0]) * (X[2] - X[0]);
    int i;
    if(a < 0) {                                 /* the same floor for -N / -A */
        a = -a;
        nx = -nx;
        ny = -ny;
    }
    p->gx = floordiv(nx * ((__int128)1 << 20), a);
    p->gy = floordiv(ny * ((__int128)1 << 20), a);
    p->v0 = v[0];
    p->lo = p->hi = v[0];
    for(i = 1; i < 3; i++) {
        if(v[i] < p->lo) p->lo = v[i];
        if(v[i] > p->hi) p->hi = v[i];
    }
}

static int64_t value_at(const struct plane *p, const int32_t *X, const int32_t *Y, int64_t cx, int64_t cy) {
    __int128 V = (__int128)p->v0 * ((__int128)1 << 20) + p->gx * (cx - X[0]) + p->gy * (cy - Y[0]);
    int64_t t = (int64_t)floordiv(V + ((__int128)1 << 19), (int64_t)1 << 20);
    return t < p->lo ? p->lo : t > p->hi ? p->hi : t;
}

/* gfx tri: TRIANGLE's words (16.2), and the pixels: those whose centres the triangle covers (16.3),
   inside the box and the target (no clip here but SET_TARGET's), each with its depth tested, its
   depth written and its colour written (16.5; this test's depth buffer never overlaps the target,
   so a pixel at a time gives what a group at a time does) */
static void tri(uint32_t bits, const int32_t *X, const int32_t *Y, const uint32_t *Z, const uint32_t *C) {
    int64_t a = (int64_t)(X[1] - X[0]) * (Y[2] - Y[0]) - (int64_t)(Y[1] - Y[0]) * (X[2] - X[0]);
    int32_t ex[3], ey[3], ax[3], ay[3], lox, hix, loy, hiy;
    int64_t zv[3], cv[3], px, py, x0, x1, y0, y1;
    int tl[3], i, j;
    struct plane pz, pc;
    word(OP_TRIANGLE << 24 | bits);
    for(i = 0; i < 3; i++) {
        word(pair(X[i], Y[i]));
        word((Z[i] & 0xFFFFu) | (C[i] & 0xFFu) << 16);
    }
    if(!a || (a > 0 && (bits & TRI_CULL_CW)) || (a < 0 && (bits & TRI_CULL_CCW))) return;
    for(i = 0; i < 3; i++) {                    /* clockwise: v1 and v2 swapped when A < 0 */
        j = (a < 0 && i) ? 3 - i : i;
        ax[i] = X[j];
        ay[i] = Y[j];
        zv[i] = Z[i];
        cv[i] = C[i];
    }
    for(i = 0; i < 3; i++) {                    /* edge i, from vertex i to vertex i + 1 */
        ex[i] = ax[(i + 1) % 3] - ax[i];
        ey[i] = ay[(i + 1) % 3] - ay[i];
        tl[i] = ey[i] < 0 || (ey[i] == 0 && ex[i] > 0);
    }
    plane_of(&pz, X, Y, zv, a);
    plane_of(&pc, X, Y, cv, a);
    lox = hix = X[0];
    loy = hiy = Y[0];
    for(i = 1; i < 3; i++) {
        if(X[i] < lox) lox = X[i];
        if(X[i] > hix) hix = X[i];
        if(Y[i] < loy) loy = Y[i];
        if(Y[i] > hiy) hiy = Y[i];
    }
    x0 = (int64_t)floordiv(lox + 7, 16);       /* the centres the box can hold, cut by the target */
    x1 = (int64_t)floordiv(hix - 8, 16);
    y0 = (int64_t)floordiv(loy + 7, 16);
    y1 = (int64_t)floordiv(hiy - 8, 16);
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 > (int64_t)tw - 1) x1 = (int64_t)tw - 1;
    if(y1 > (int64_t)th - 1) y1 = (int64_t)th - 1;
    for(py = y0; py <= y1; py++)
        for(px = x0; px <= x1; px++) {
            int64_t cx = 16 * px + 8, cy = 16 * py + 8, z = 0, c;
            uint32_t za = zbase + (uint32_t)py * zstride + 2 * (uint32_t)px;
            int in = 1;
            for(i = 0; i < 3 && in; i++) {
                int64_t e = (int64_t)ex[i] * (cy - ay[i]) - (int64_t)ey[i] * (cx - ax[i]);
                in = e > 0 || (e == 0 && tl[i]);
            }
            if(!in) continue;
            if(bits & (TRI_ZTEST | TRI_ZWRITE)) z = value_at(&pz, X, Y, cx, cy);
            c = (bits & TRI_GOURAUD) ? value_at(&pc, X, Y, cx, cy) : (int64_t)(bits & 0xFFu);
            if(bits & TRI_ZTEST) {
                int64_t was = vp[za] | vp[za + 1] << 8;
                if((bits & TRI_ZFUNC_LE) ? z > was : z >= was) continue;
            }
            if(bits & TRI_ZWRITE) {
                vp[za] = (uint8_t)z;
                vp[za + 1] = (uint8_t)(z >> 8);
            }
            plot((int)px, (int)py, (uint8_t)c);
        }
}

/* gfx flip [base]: FLIP (16.2), GFX_BASE from the next frame (16.7) */
static void flip(uint32_t base) {
    word(OP_FLIP << 24);
    word(base);
    gbase = base;
}

/* a vertex in 12.4: pixels and sixteenths */
#define FX(p) ((int32_t)((p) * 16))

/* gfx tri's two forms: x0 y0 x1 y1 x2 y2 c (flat, depths 0), and each vertex with its depth and colour;
   the header's colour is vertex 0's */
static void tri7(uint32_t how, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t c) {
    const int32_t X[3] = { x0, x1, x2 }, Y[3] = { y0, y1, y2 };
    const uint32_t Z[3] = { 0, 0, 0 }, C[3] = { c, c, c };
    tri(how | c, X, Y, Z, C);
}

static void tri12(uint32_t how, int32_t x0, int32_t y0, uint32_t z0, uint32_t c0, int32_t x1, int32_t y1, uint32_t z1,
                  uint32_t c1, int32_t x2, int32_t y2, uint32_t z2, uint32_t c2) {
    const int32_t X[3] = { x0, x1, x2 }, Y[3] = { y0, y1, y2 };
    const uint32_t Z[3] = { z0, z1, z2 }, C[3] = { c0, c1, c2 };
    tri(how | c0, X, Y, Z, C);
}

/* GPU.md section 3: GPAL as the configuration leaves it: R = n[7:5], G = n[4:2], B = n[1:0], each
   widened by repeating its top bits (R4 = {R, R[2]}, G4 = {G, G[2]}, B4 = {B, B}) */
static void pal_rgb332(void) {
    unsigned n, r, g, b;
    for(n = 0; n < 256; n++) {
        r = n >> 5 & 7;
        g = n >> 2 & 7;
        b = n & 3;
        gpal[n] = (uint16_t)((r << 1 | r >> 2) << 8 | (g << 1 | g >> 2) << 4 | (b << 2 | b));
    }
}

/* GPU.md 15.1: ramps: hue n >> 5 of white, red, green, blue, yellow, cyan, magenta and orange
   (0xF80), shade i = n & 31, each component C of the hue becoming (C i + 15) / 31 */
static void pal_ramps(void) {
    static const uint16_t hue[8] = { 0xFFF, 0xF00, 0x0F0, 0x00F, 0xFF0, 0x0FF, 0xF0F, 0xF80 };
    unsigned n, k, i, c, v;
    for(n = 0; n < 256; n++) {
        i = n & 31;
        for(k = 0, v = 0; k < 3; k++) {
            c = (unsigned)hue[n >> 5] >> (4 * k) & 15;
            v |= (c * i + 15) / 31 << (4 * k);
        }
        gpal[n] = (uint16_t)v;
    }
}

/* gfx mode gfx|overlay <scale>: GFX_BASE 0, GFX_STRIDE 640 / scale, and the target is that picture */
static void show(int m, uint32_t scale) {
    mode = m;
    gbase = 0;
    gstride = 640 / scale;
    gscale = scale;
    target(0, 640 / scale, 640 / scale, 480 / scale);
}

/* GPU.md section 3: pixel (x, y) shows GPAL[v], v the VRAM byte at GFX_BASE + (y / s) GFX_STRIDE
   + x / s (s = 2 at GFX_SCALE 2, else 1; the address taken modulo 512 KiB; 0 at or past 300 KiB);
   FRAME_CRC is zlib's CRC-32 of the frame's pixels as little-endian 16-bit words, row by row */
static uint32_t picture(uint16_t *f) {
    uint32_t s = gscale == 2 ? 2 : 1, crc = 0xFFFFFFFFu, a, x, y, k, p;
    uint8_t byte[2];
    for(y = 0; y < 480; y++)
        for(x = 0; x < 640; x++) {
            a = (gbase + (y / s) * gstride + x / s) & 0x7FFFFu;
            p = f[y * 640 + x] = gpal[a < VRAM ? vp[a] : 0];
            byte[0] = (uint8_t)(p & 0xFF);
            byte[1] = (uint8_t)(p >> 8);
            for(k = 0; k < 2; k++) {
                int bit;
                crc ^= byte[k];
                for(bit = 0; bit < 8; bit++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
            }
        }
    return ~crc;
}

static int write_ppm(const char *path, const uint16_t *f) {
    FILE *o = fopen(path, "wb");
    int i;
    if(!o) return -1;
    fprintf(o, "P6\n640 480\n255\n");
    for(i = 0; i < 640 * 480; i++) {
        unsigned char rgb[3] = { (unsigned char)((f[i] >> 8 & 15) * 17), (unsigned char)((f[i] >> 4 & 15) * 17),
                                 (unsigned char)((f[i] & 15) * 17) };
        fwrite(rgb, 1, 3, o);
    }
    return fclose(o);
}

static void crc(void) {
    struct gpu_display d;
    uint32_t c, mc;
    unsigned i;
    char path[1024];
    ncrc++;
    for(i = 0; i < VRAM && vm[i] == vp[i]; i++) {}
    if(i < VRAM) {
        printf("gfxref: crc %d: the model's VRAM and the pixels computed directly differ, first at byte %u (%u, %u)\n",
               ncrc, i, vm[i], vp[i]);
        bad++;
    }
    if(mode != 2 && mode != 3) {
        printf("expect %d - mode %d\n", ncrc, mode);
        return;
    }
    if(!known) {
        printf("gfxref: crc %d: VRAM is not known here (nothing has covered all of it)\n", ncrc);
        bad++;
    }
    c = picture(frame);
    gpu_display_reset(&d, 1);                   /* the model's display path, graphics alone */
    d.mode = 2;
    d.gfx_base = gbase;
    d.gfx_stride = gstride;
    d.gfx_scale = gscale;
    memcpy(d.gpal, gpal, sizeof(d.gpal));
    gpu_display_render(&d, vm, NULL, font8x16, mframe);
    mc = gpu_frame_crc(mframe);
    if(mc != c || memcmp(frame, mframe, sizeof(frame))) {
        printf("gfxref: crc %d: the model's display path gives %08lx, GPU.md section 3's %08lx\n", ncrc, (unsigned long)mc,
               (unsigned long)c);
        bad++;
    }
    if(mode == 2) printf("expect %d %08lx mode 2\n", ncrc, (unsigned long)c);
    else printf("expect %d - mode 3\n", ncrc);
    if(ppm) {
        snprintf(path, sizeof(path), "%s/crc-%d.ppm", ppm, ncrc);
        if(write_ppm(path, frame)) {
            fprintf(stderr, "gfxref: cannot write %s\n", path);
            bad++;
        }
    }
}

static void say(const char *gfx_line) { printf("line %s\n", gfx_line); }

int main(int argc, char **argv) {
    int i;
    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else {
            fprintf(stderr, "usage: gfxref [--ppm DIR]\n");
            return 2;
        }
    }
    gpu_engine_init(&eng, vm);
    pal_rgb332();
    /* The scene of GPU.md 15.6 (crc 1, which the owner's board must print too) */
    say("gfx");
    say("gfx mode gfx 1");                                          show(2, 1);
    say("gfx pal rgb332");                                          pal_rgb332();
    say("gfx clear 0x25");                                          fill(0, 0, 640, 480, 0x25);
    say("gfx fill 16 16 200 120 0xe0");                             fill(16, 16, 200, 120, 0xe0);
    say("gfx rect 8 8 216 136 0xff");                               outline(8, 8, 216, 136, 0xff);
    say("gfx line 0 479 639 0 0x1c");                               line(0, 479, 639, 0, 0x1c);
    say("gfx line 0 0 639 479 0xfc");                               line(0, 0, 639, 479, 0xfc);
    say("gfx copy 16 16 400 300 200 120");                          copy(16, 16, 400, 300, 200, 120, -1);
    say("gfx copy 0 0 240 160 64 64 0x25");                         copy(0, 0, 240, 160, 64, 64, 0x25);
    say("gfx text 24 200 0xff - Hello from SurfOS on SurfGPU");     text(24, 200, 0xff, -1, "Hello from SurfOS on SurfGPU");
    say("gfx text 24 220 0x03 0xfc opaque text in the console font");
    text(24, 220, 0x03, 0xfc, "opaque text in the console font");
    say("gfx put 639 479 0x92");                                    put(639, 479, 0x92);
    say("gfx get 639 479");
    say("gfx crc");                                                 crc();
    /* 320x240 doubled, in the ramps (crc 2) and in RGB332 (crc 3) */
    say("gfx mode gfx 2");                                          show(2, 2);
    say("gfx clear 0x49");                                          fill(0, 0, 320, 240, 0x49);
    say("gfx fill 296 216 40 40 0xe3");                             fill(296, 216, 40, 40, 0xe3);
    say("gfx line 0 239 319 0 0xff");                               line(0, 239, 319, 0, 0xff);
    say("gfx text 8 8 0xff 0 320x240, doubled");                    text(8, 8, 0xff, 0, "320x240, doubled");
    say("gfx pal ramps");                                           pal_ramps();
    say("gfx crc");                                                 crc();
    say("gfx pal rgb332");                                          pal_rgb332();
    say("gfx crc");                                                 crc();
    /* the demos put everything back: the same picture (crc 4, crc 5) */
    say("gfx demo 1");
    say("gfx crc");                                                 crc();
    say("gfx demo 5");
    say("gfx crc");                                                 crc();
    /* GPU2 (GPU.md section 16): triangles in 320x240 with a depth buffer (crc 6): flat; one culled
       (clockwise: A > 0) and drawn the other way; a square as two triangles along a diagonal through
       pixel centres (the top-left rule); Gouraud with depth, two triangles through each other (ztest,
       then zle), vertices with sixteenths; Gouraud without depth. Then picture B drawn and shown by
       FLIP (crc 7), A again (crc 8); gfx cube puts everything back (crc 9) */
    say("gfx mode gfx 2");                                          show(2, 2);
    say("gfx pal ramps");                                           pal_ramps();
    say("gfx zbuf 153600 640");                                     zbuf(153600, 640);
    say("gfx zclear");                                              zclear(0xFF);
    say("gfx clear 0");                                             fill(0, 0, 320, 240, 0);
    say("gfx tri 8 8 120 16 40 100 0x3f");                          tri7(0, FX(8), FX(8), FX(120), FX(16), FX(40), FX(100), 0x3f);
    say("gfx tri cullcw 200 20 300 20 250 60 0xff");
    tri7(TRI_CULL_CW, FX(200), FX(20), FX(300), FX(20), FX(250), FX(60), 0xff);
    say("gfx tri cullccw 200 20 300 20 250 60 0x7f");
    tri7(TRI_CULL_CCW, FX(200), FX(20), FX(300), FX(20), FX(250), FX(60), 0x7f);
    say("gfx tri 20 140 60 140 60 180 0x5f");                       tri7(0, FX(20), FX(140), FX(60), FX(140), FX(60), FX(180), 0x5f);
    say("gfx tri 20 140 60 180 20 180 0x9f");                       tri7(0, FX(20), FX(140), FX(60), FX(180), FX(20), FX(180), 0x9f);
    say("gfx tri gouraud,ztest,zwrite 150.5 70.25 1000 0x20 300.0625 100 60000 0x3f 200 230.75 30000 0x28");
    tri12(TRI_GOURAUD | TRI_ZTEST | TRI_ZWRITE, 2408, 1124, 1000, 0x20, 4801, FX(100), 60000, 0x3f, FX(200), 3692, 30000, 0x28);
    say("gfx tri gouraud,zle,zwrite 140 230 30000 0x40 310 220 30000 0x5f 250 60 30000 0x50");
    tri12(TRI_GOURAUD | TRI_ZTEST | TRI_ZFUNC_LE | TRI_ZWRITE, FX(140), FX(230), 30000, 0x40, FX(310), FX(220), 30000, 0x5f,
          FX(250), FX(60), 30000, 0x50);
    say("gfx tri gouraud 80 200 0 0xc0 140 236 0 0xdf 70 238.5 0 0xd0");
    tri12(TRI_GOURAUD, FX(80), FX(200), 0, 0xc0, FX(140), FX(236), 0, 0xdf, FX(70), 3816, 0, 0xd0);
    say("gfx crc");                                                 crc();
    say("gfx target 76800 320 320 240");                            target(76800, 320, 320, 240);
    say("gfx clear 0x70");                                          fill(0, 0, 320, 240, 0x70);
    say("gfx tri 0 0 319.9375 0 0 239.9375 0x9f");                  tri7(0, 0, 0, 5119, 0, 0, 3839, 0x9f);
    say("gfx flip");                                                flip(76800);
    say("gfx crc");                                                 crc();
    say("gfx flip 0");                                              flip(0);
    say("gfx crc");                                                 crc();
    say("gfx cube 1");
    say("gfx crc");                                                 crc();
    say("gfx pal rgb332");                                          pal_rgb332();
    /* the bench draws on the target; the reset's target; text mode (crc 10: the console) */
    say("gfx bench");                                               known = 0;
    say("gfx reset");                                               target(0, 640, 640, 480);
    say("gfx");
    say("gfx mode text");                                           mode = 1;
    say("gfx crc");                                                 crc();
    /* the overlay: the console over a picture (crc 11; its graphics for the pixels between characters) */
    say("gfx mode overlay 1");                                      show(3, 1);
    say("gfx clear 0x49");                                          fill(0, 0, 640, 480, 0x49);
    say("gfx fill 320 240 320 240 0x92");                           fill(320, 240, 320, 240, 0x92);
    say("gfx crc");                                                 crc();
    printf("gfxref: %d gfx crc, %s\n", ncrc, bad ? "DISAGREEMENTS (above)" : "the model's VRAM, the direct pixels and the "
           "two frame CRCs agreed");
    return bad ? 1 : 0;
}
