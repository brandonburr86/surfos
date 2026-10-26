/*
SurfGPU diagnostics: the scenes
--------------------
File: gpuscenes.c Date: 10/18/26 (SurfGPU), 10/25/26 (GPU2)
--------------------
gpudiag's pictures (gpuscenes.h). The first ones need only VRAM and the graphics scan-out (the
CPU draws through the VRAM window): the test pattern, text, 640x480 graphics through the default
and a grey palette, a 320x240 page shown doubled, text over graphics, a black picture. The
engine's scenes follow (docs/GPU.md section 5): every command, every alignment of x mod 4,
clipping at each edge, overlapping copies in every direction, the colour key, lines in every
octant, bitmaps and pixel rectangles, and a page drawn by the engine and flipped to. GPU2's
(section 16): flat triangles (a fan and a mesh whose shared edges show a gap or a double draw
as a wrong colour, culling, zero areas, the clip), Gouraud triangles in the palette's ramps,
depth (triangles crossing, ZFUNC, a test without a write), and a page drawn behind the one shown,
then shown by FLIP.
Plain C for both RV32 and the host: no library calls, no division in the hot loops.
*/
#include "gpuscenes.h"
#include "surfcpu/gpu.h"
#include "surfcpu/soc.h"

#define CTRL_ON(mode) (SURF_VIDEO_CTRL_ENABLE_MASK | ((uint32_t)(mode) << SURF_VIDEO_CTRL_MODE_LSB))

/* the 16 CGA colours (the text palette's reset values) */
static const uint16_t cga[16] = {0x000, 0x00A, 0x0A0, 0x0AA, 0xA00, 0xA0A, 0xA50, 0xAAA,
                                 0x555, 0x55F, 0x5F5, 0x5FF, 0xF55, 0xF5F, 0xFF5, 0xFFF};

static void text_palette(const struct gpu_sink *s)
{
    for (uint32_t i = 0; i < 16; i++)
        s->video(SURF_VIDEO_PALETTE + 4 * i, cga[i]);
}

static uint32_t rgb332(uint32_t n)
{
    uint32_t r = (n >> 5) & 7, g = (n >> 2) & 7, b = n & 3;
    return ((r << 1 | r >> 2) << 8) | ((g << 1 | g >> 2) << 4) | (b << 2 | b);
}

static void gpal_rgb332(const struct gpu_sink *s)
{
    for (uint32_t n = 0; n < 256; n++)
        s->video(SURF_VIDEO_GPAL + 4 * n, rgb332(n));
}

static void placement(const struct gpu_sink *s, uint32_t base, uint32_t stride, uint32_t scale)
{
    s->video(SURF_VIDEO_GFX_BASE, base);
    s->video(SURF_VIDEO_GFX_STRIDE, stride);
    s->video(SURF_VIDEO_GFX_SCALE, scale);
}

/* a 640 x 480 picture for RGB332: red down, green across, blue in a fine check */
static uint32_t pat640(uint32_t x, uint32_t y)
{
    return ((y >> 1) & 0xE0) | ((x >> 4) & 0x1C) | (((x ^ y) >> 3) & 3);
}

/* ------------------------------------------------------------------ scenes without the engine */

static void scene_bars(const struct gpu_sink *s)
{
    s->video(SURF_VIDEO_CTRL, CTRL_ON(0));
}

static void scene_text(const struct gpu_sink *s)
{
    text_palette(s);
    s->video(SURF_VIDEO_SCROLL, 0);
    s->video(SURF_VIDEO_CURSOR, 0);
    for (uint32_t i = 0; i < 2400; i++) {
        uint32_t fg = 1 + (i >> 8) % 15, bg = (i >> 4) & 7;
        s->text(i, bg << 12 | fg << 8 | (i & 0xFF));
    }
    s->video(SURF_VIDEO_CTRL, CTRL_ON(1));
}

static void scene_gfx640(const struct gpu_sink *s)
{
    placement(s, 0, 640, 1);
    gpal_rgb332(s);
    for (uint32_t y = 0; y < 480; y++)
        for (uint32_t x = 0; x < 640; x += 4)
            s->vram32(y * 640 + x, pat640(x, y) | pat640(x + 1, y) << 8 | pat640(x + 2, y) << 16 |
                                   pat640(x + 3, y) << 24);
    s->video(SURF_VIDEO_CTRL, CTRL_ON(2));
}

static void scene_greys(const struct gpu_sink *s)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t v = n >> 4;
        s->video(SURF_VIDEO_GPAL + 4 * n, v << 8 | v << 4 | v);
    }
    s->video(SURF_VIDEO_CTRL, CTRL_ON(2));
}

/* the second 320 x 240 page (VRAM 153,600 .. 230,399), shown doubled */
static void scene_page2(const struct gpu_sink *s)
{
    gpal_rgb332(s);
    for (uint32_t y = 0; y < 240; y++)
        for (uint32_t x = 0; x < 320; x += 4) {
            uint32_t v = 0;
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t px = x + k;
                uint32_t c = ((px ^ y) & 0x10) ? 0xFF : (((px + y) >> 3) & 0xFF);
                v |= c << (8 * k);
            }
            s->vram32(153600 + y * 320 + x, v);
        }
    placement(s, 153600, 320, 2);
    s->video(SURF_VIDEO_CTRL, CTRL_ON(2));
}

/* text over graphics: a blue box with white text, yellow text straight on the picture, and the
 * cursor (no blink) */
static void put_str(const struct gpu_sink *s, uint32_t row, uint32_t col, const char *t, uint32_t attr)
{
    while (*t)
        s->text(row * 80 + col++, attr | (uint8_t)*t++);
}

static void scene_overlay(const struct gpu_sink *s)
{
    text_palette(s);
    placement(s, 0, 640, 1);
    gpal_rgb332(s);
    for (uint32_t i = 0; i < 2400; i++)
        s->text(i, 0);
    for (uint32_t r = 10; r < 16; r++)
        for (uint32_t c = 20; c < 60; c++)
            s->text(r * 80 + c, 1u << 12 | 15u << 8 | ' ');
    put_str(s, 12, 24, "SurfGPU: text over graphics", 1u << 12 | 15u << 8);
    put_str(s, 13, 24, "background 1 is opaque", 1u << 12 | 14u << 8);
    put_str(s, 2, 2, "background 0 lets the picture through", 14u << 8);
    s->video(SURF_VIDEO_SCROLL, 0);
    s->video(SURF_VIDEO_CURSOR, 13u << SURF_VIDEO_CURSOR_ROW_LSB | 47u << SURF_VIDEO_CURSOR_COL_LSB);
    s->video(SURF_VIDEO_CTRL, CTRL_ON(3) | SURF_VIDEO_CTRL_CURSOR_EN_MASK);
}

static void scene_black(const struct gpu_sink *s)
{
    s->video(SURF_VIDEO_CTRL, 3u << SURF_VIDEO_CTRL_MODE_LSB);      /* ENABLE 0 */
}

/* ------------------------------------------------------------------ scenes with the engine */

#define HDR(op, bits) GPU_HDR(GPU_OP_##op, bits)

static void c2(const struct gpu_sink *s, uint32_t a, uint32_t b) { s->cmd(a); s->cmd(b); }
static void c3(const struct gpu_sink *s, uint32_t a, uint32_t b, uint32_t c) { s->cmd(a); s->cmd(b); s->cmd(c); }
static void c4(const struct gpu_sink *s, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    s->cmd(a); s->cmd(b); s->cmd(c); s->cmd(d);
}

static void target(const struct gpu_sink *s, uint32_t base, uint32_t stride, uint32_t w, uint32_t h)
{
    c4(s, HDR(SET_TARGET, 0), base, stride, GPU_WH(w, h));
}

static void fill(const struct gpu_sink *s, uint32_t colour, int x, int y, unsigned w, unsigned h)
{
    c3(s, HDR(FILL_RECT, colour & 0xFF), GPU_XY(x, y), GPU_WH(w, h));
}

static void line(const struct gpu_sink *s, uint32_t colour, int x0, int y0, int x1, int y1)
{
    c3(s, HDR(LINE, colour & 0xFF), GPU_XY(x0, y0), GPU_XY(x1, y1));
}

static void show640(const struct gpu_sink *s)
{
    placement(s, 0, 640, 1);
    s->video(SURF_VIDEO_CTRL, CTRL_ON(2));
}

/* fills: every x mod 4 and width 1..8, the four edges, a clip, the whole screen first */
static void scene_fills(const struct gpu_sink *s)
{
    gpal_rgb332(s);
    target(s, 0, 640, 640, 480);
    fill(s, 0x02, 0, 0, 640, 480);                                  /* dark blue */
    for (int x = 0; x < 4; x++)
        for (unsigned w = 1; w <= 8; w++)
            fill(s, 0x1C + 0x20 * x + w, 16 + 72 * (int)w + 3 * x, 16 + 24 * x, w * 7, 16);
    fill(s, 0xE0, -20, 200, 60, 40);                                /* past the left edge */
    fill(s, 0xFC, 610, 200, 60, 40);                                /* past the right edge */
    fill(s, 0x1F, 300, -15, 40, 40);                                /* past the top */
    fill(s, 0xE3, 300, 460, 40, 40);                                /* past the bottom */
    c3(s, HDR(SET_CLIP, 0), GPU_XY(100, 300), GPU_XY(540, 420));
    for (int k = 0; k < 12; k++)
        fill(s, 0x24 * k + 3, 60 + 40 * k, 280 + 9 * k, 37, 50);
    c3(s, HDR(SET_CLIP, 0), GPU_XY(0, 0), GPU_XY(640, 480));
    show640(s);
}

/* lines: a star in every octant from the centre, a frame, lines clipped at every edge */
static void scene_lines(const struct gpu_sink *s)
{
    target(s, 0, 640, 640, 480);
    fill(s, 0x00, 0, 0, 640, 480);
    for (int a = 0; a < 64; a++) {
        int ex = (a & 15) * 40, ey = (a & 15) * 30;
        int x1 = a < 16 ? ex : a < 32 ? 639 : a < 48 ? 639 - ex : 0;
        int y1 = a < 16 ? 0 : a < 32 ? ey : a < 48 ? 479 : 479 - ey;
        line(s, 0x20 + 3 * a, 320, 240, x1, y1);
    }
    line(s, 0xFF, 0, 0, 639, 0);
    line(s, 0xFF, 639, 0, 639, 479);
    line(s, 0xFF, 639, 479, 0, 479);
    line(s, 0xFF, 0, 479, 0, 0);
    line(s, 0xE0, -500, 100, 1200, 140);                            /* clipped at both ends */
    line(s, 0x1C, 100, -300, 140, 900);
    line(s, 0x03, -32768, -32768, 32767, 32767);
    show640(s);
}

/* copies: a pattern block, copied overlapping in eight directions, and with a colour key */
static void scene_copies(const struct gpu_sink *s)
{
    target(s, 0, 640, 640, 480);
    fill(s, 0x49, 0, 0, 640, 480);
    for (int y = 0; y < 48; y += 4)
        for (int x = 0; x < 64; x += 4)
            fill(s, (uint32_t)(x * 4 + y), 20 + x, 20 + y, 4, 4);
    fill(s, 0x00, 36, 32, 30, 10);                                  /* a hole of colour 0 for the key */
    static const int d[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {3, 2}, {-3, -2}, {5, -1}, {-2, 3}};
    for (int k = 0; k < 8; k++) {
        int bx = 120 + (k % 4) * 128, by = 40 + (k / 4) * 120;
        c4(s, HDR(COPY_RECT, 0), GPU_XY(20, 20), GPU_XY(bx, by), GPU_WH(64, 48));
        for (int n = 0; n < 12; n++)                                /* smear it, overlapping */
            c4(s, HDR(COPY_RECT, 0), GPU_XY(bx + n * d[k][0], by + n * d[k][1]),
               GPU_XY(bx + (n + 1) * d[k][0], by + (n + 1) * d[k][1]), GPU_WH(64, 48));
    }
    for (int k = 0; k < 6; k++)                                     /* the key: colour 0 not copied */
        c4(s, HDR(COPY_RECT, GPU_COPY_KEY | 0x00), GPU_XY(20, 20), GPU_XY(40 + 97 * k, 340 + 11 * k),
           GPU_WH(64, 48));
    c4(s, HDR(COPY_RECT, 0), GPU_XY(600, 400), GPU_XY(620, 440), GPU_WH(80, 80));     /* clipped */
    show640(s);
}

/* 8x8 letters, MSB the leftmost pixel */
static const uint8_t glyphs[6][8] = {
    {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00},               /* S */
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00},               /* U */
    {0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00},               /* R */
    {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00},               /* F */
    {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00},               /* G */
    {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00},               /* P */
};
static const uint8_t word_surfgpu[7] = {0, 1, 2, 3, 4, 5, 1};       /* S U R F G P U */

/* one letter scaled by n (1..4) as an EXPAND bitmap: (8n)^2 bits, 32 to a word, MSB first */
static void letter(const struct gpu_sink *s, unsigned g, int x, int y, unsigned n, uint32_t fgbg)
{
    s->cmd(HDR(EXPAND, fgbg));
    s->cmd(GPU_XY(x, y));
    s->cmd(GPU_WH(8 * n, 8 * n));
    uint32_t w = 0, bits = 0;
    for (unsigned r = 0; r < 8 * n; r++)
        for (unsigned c = 0; c < 8 * n; c++) {
            w = w << 1 | ((glyphs[g][r / n] >> (7 - c / n)) & 1u);
            if (++bits == 32) {
                s->cmd(w);
                w = bits = 0;
            }
        }
    if (bits)
        s->cmd(w << (32 - bits));
}

/* EXPAND: letters at every alignment and size, opaque and transparent; WRITE_RECT sprites */
static void scene_bitmaps(const struct gpu_sink *s)
{
    target(s, 0, 640, 640, 480);
    fill(s, 0x25, 0, 0, 640, 480);
    for (unsigned k = 0; k < 7; k++) {
        letter(s, word_surfgpu[k], 20 + 9 * (int)k, 20 + (int)k, 1, 0xFF | 0x00u << 8);
        letter(s, word_surfgpu[k], 100 + 17 * (int)k, 60, 2, 0xFC | 0x80u << 8 | GPU_EXPAND_TRANSP);
        letter(s, word_surfgpu[k], 30 + 85 * (int)k, 120 + 3 * (int)(k & 3), 4, 0x1F | 0x00u << 8 | GPU_EXPAND_TRANSP);
        letter(s, word_surfgpu[k], 610 + 9 * (int)k, 470, 3, 0xE0 | 0x1Cu << 8);    /* past the corner */
    }
    for (unsigned k = 0; k < 8; k++) {                              /* 37 x 23 sprites, every x mod 4 */
        int x = 40 + 70 * (int)k + (int)k, y = 300 + (int)(k & 1) * 30;
        s->cmd(HDR(WRITE_RECT, 0));
        s->cmd(GPU_XY(x, y));
        s->cmd(GPU_WH(37, 23));
        uint32_t w = 0, n = 0;
        for (uint32_t i = 0; i < 37u * 23u; i++) {
            uint32_t px = i % 37, py = i / 37;
            uint32_t c = ((px ^ py) & 4) ? 0xFF : (uint32_t)((px * 7 + py * 3 + k * 16) & 0xFF);
            w |= c << (8 * n);
            if (++n == 4) {
                s->cmd(w);
                w = n = 0;
            }
        }
        if (n)
            s->cmd(w);
    }
    show640(s);
}

/* the 320 x 240 page at 153,600, drawn by the engine, then shown doubled after a vertical blanking */
static void scene_page_flip(const struct gpu_sink *s)
{
    target(s, 153600, 320, 320, 240);
    fill(s, 0x00, 0, 0, 320, 240);
    for (int k = 0; k < 16; k++)
        fill(s, 0x11 * k, 10 + 19 * k, 10, 17, 220);
    for (int k = 0; k < 20; k++)
        line(s, 0xFF, 0, 12 * k, 319, 239 - 12 * k);
    for (unsigned k = 0; k < 7; k++)
        letter(s, word_surfgpu[k], 20 + 40 * (int)k, 100, 4, 0xE0 | GPU_EXPAND_TRANSP);
    s->cmd(HDR(WAIT_VBLANK, 0));
    c2(s, HDR(FENCE, 0), 0x0F11Bu);
    s->sync();
    placement(s, 153600, 320, 2);
    s->video(SURF_VIDEO_CTRL, CTRL_ON(2));
}

/* the last picture: bands, boxes, the letters and a frame (gpudiag leaves it on the screen) */
static void scene_demo(const struct gpu_sink *s)
{
    target(s, 0, 640, 640, 480);
    for (int y = 0; y < 480; y += 15)
        fill(s, (uint32_t)((y / 15) & 7) << 5 | 0x03, 0, y, 640, 15);
    for (int k = 0; k < 10; k++)
        fill(s, 0x1C + k * 0x20, 40 + 55 * k, 330 - 20 * k, 45, 100 + 20 * k);
    for (int a = 0; a < 32; a++)
        line(s, 0xFF, 320, 470, a * 20, 300);
    for (unsigned k = 0; k < 7; k++)
        letter(s, word_surfgpu[k], 60 + 76 * (int)k, 60, 8, 0xFF | GPU_EXPAND_TRANSP);
    c4(s, HDR(COPY_RECT, GPU_COPY_KEY | 0x03), GPU_XY(60, 60), GPU_XY(64, 66), GPU_WH(520, 64));
    line(s, 0xFC, 0, 0, 639, 0);
    line(s, 0xFC, 639, 0, 639, 479);
    line(s, 0xFC, 639, 479, 0, 479);
    line(s, 0xFC, 0, 479, 0, 0);
    gpal_rgb332(s);
    show640(s);
}

/* ------------------------------------------------------------------ GPU2's scenes */

/* 16384 sin(2 pi i / 64), i = 0 .. 16: the fan's points */
static const int16_t s16[17] = {0, 1606, 3196, 4756, 6270, 7723, 9102, 10394, 11585, 12665, 13623, 14449, 15137,
                                15679, 16069, 16305, 16384};
static int32_t sin64(uint32_t a)
{
    a &= 63u;
    if (a <= 16u) return s16[a];
    if (a <= 32u) return s16[32u - a];
    if (a <= 48u) return -s16[a - 32u];
    return -s16[64u - a];
}

/* one TRIANGLE, flat (colour in the header) or with each vertex's depth and colour */
static void tri(const struct gpu_sink *s, uint32_t flags, int x0, int y0, uint32_t zc0, int x1, int y1, uint32_t zc1,
                int x2, int y2, uint32_t zc2)
{
    s->cmd(HDR(TRIANGLE, flags));
    s->cmd(GPU_XY(x0, y0));
    s->cmd(zc0);
    s->cmd(GPU_XY(x1, y1));
    s->cmd(zc1);
    s->cmd(GPU_XY(x2, y2));
    s->cmd(zc2);
}

static void flat(const struct gpu_sink *s, uint32_t colour, int x0, int y0, int x1, int y1, int x2, int y2)
{
    tri(s, colour & 0xFFu, x0, y0, 0, x1, y1, 0, x2, y2, 0);
}

/* eight ramps of 32 shades (black to a colour), the SurfOS driver's "ramps" (GPU.md section 16) */
static void gpal_ramps(const struct gpu_sink *s)
{
    static const uint8_t rgb[8][3] = {{1, 1, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}, {1, 1, 0}};
    for (uint32_t n = 0; n < 256; n++) {
        const uint32_t r = n >> 5, lv = ((n & 31u) * 15u + 15u) / 31u, w = r == 7 ? lv / 2u : 0u;
        s->video(SURF_VIDEO_GPAL + 4 * n, (rgb[r][0] * lv | w) << 8 | (rgb[r][1] * lv | w) << 4 | (rgb[r][2] * lv | w));
    }
}

/* flat triangles: a fan of 32 around the centre and a jittered mesh, every triangle its own
 * colour, so a gap or a pixel drawn twice on a shared edge changes the picture; both windings
 * with the other winding culled, the culled ones (nothing), zero areas (nothing), a clip */
static void scene_flat(const struct gpu_sink *s)
{
    gpal_rgb332(s);
    target(s, 0, 640, 640, 480);
    fill(s, 0x01, 0, 0, 640, 480);
    const int cx = GPU_FX(470) + 5, cy = GPU_FX(250) + 11, r = GPU_FX(150);
    for (uint32_t i = 0; i < 32; i++) {                 /* the fan: points on a circle, in 12.4 */
        const uint32_t a = 2 * i, b = 2 * i + 2;
        const int ax = cx + r * sin64(a + 16) / 16384, ay = cy + r * sin64(a) / 16384;
        const int bx = cx + r * sin64(b + 16) / 16384, by = cy + r * sin64(b) / 16384;
        if (i & 1)
            flat(s, 0x20 + 7 * i, cx, cy, ax, ay, bx, by);
        else
            flat(s, 0x20 + 7 * i, cx, cy, bx, by, ax, ay);              /* the other winding */
    }
    for (int j = 0; j < 8; j++)                         /* the mesh: 10 x 8 cells, the inner points moved */
        for (int i = 0; i < 10; i++) {
            int px[2][2], py[2][2];
            for (int dj = 0; dj < 2; dj++)
                for (int di = 0; di < 2; di++) {
                    const int gi = i + di, gj = j + dj, inner = gi > 0 && gi < 10 && gj > 0 && gj < 8;
                    px[dj][di] = GPU_FX(20 + 26 * gi) + (inner ? (gi * 7 + gj * 3) % 11 - 5 : 0);
                    py[dj][di] = GPU_FX(30 + 26 * gj) + (inner ? (gi * 5 + gj * 9) % 13 - 6 : 0);
                }
            const uint32_t c = (uint32_t)(0x40 + 3 * (i + 10 * j));
            if ((i + j) & 1) {
                flat(s, c, px[0][0], py[0][0], px[0][1], py[0][1], px[1][1], py[1][1]);
                flat(s, c + 1, px[0][0], py[0][0], px[1][1], py[1][1], px[1][0], py[1][0]);
            } else {
                flat(s, c, px[0][0], py[0][0], px[0][1], py[0][1], px[1][0], py[1][0]);
                flat(s, c + 1, px[1][0], py[1][0], px[0][1], py[0][1], px[1][1], py[1][1]);
            }
        }
    /* culling: each pair drawn in both windings, one culled; then two culled outright. The pairs
     * stand half a pixel down, so the drawn clockwise triangle's first edge, horizontal and to the
     * right, is a top edge through the centres of row 260: the top-left rule's top case decides
     * whether that row is drawn (a scene without such an edge never tests it) */
    for (int k = 0; k < 4; k++) {
        const int x = GPU_FX(30 + 70 * k), y = GPU_FX(260) + 8;
        flat(s, 0xE0, x, y, x + GPU_FX(50), y, x, y + GPU_FX(40));                      /* clockwise */
        tri(s, GPU_TRI_CULL_CW | 0x1C, x, y, 0, x + GPU_FX(50), y, 0, x + GPU_FX(25), y + GPU_FX(60), 0);
        tri(s, GPU_TRI_CULL_CW | 0x1F, x + GPU_FX(50), y + GPU_FX(60), 0, x + GPU_FX(50), y + GPU_FX(10), 0,
            x + GPU_FX(10), y + GPU_FX(60), 0);                                       /* counter-clockwise: drawn */
    }
    tri(s, GPU_TRI_CULL_CW | GPU_TRI_CULL_CCW | 0xFF, GPU_FX(300), GPU_FX(300), 0, GPU_FX(340), GPU_FX(300), 0,
        GPU_FX(300), GPU_FX(340), 0);
    flat(s, 0xFF, GPU_FX(300), GPU_FX(360), GPU_FX(320), GPU_FX(380), GPU_FX(340), GPU_FX(400));   /* zero area */
    /* a clip; a huge triangle across it, and one past the screen's corner */
    c3(s, HDR(SET_CLIP, 0), GPU_XY(40, 360), GPU_XY(260, 470));
    flat(s, 0x92, -32768, 32767, GPU_FX(150), -GPU_FX(500), 32767, 32767);
    c3(s, HDR(SET_CLIP, 0), GPU_XY(0, 0), GPU_XY(640, 480));
    flat(s, 0x6D, GPU_FX(600) + 9, GPU_FX(440) + 3, GPU_FX(700), GPU_FX(460), GPU_FX(610), GPU_FX(530));
    show640(s);
}

/* Gouraud: in each of the eight ramps a triangle from its dark end to its bright one, and one
 * across ramps (0 .. 255); vertices at sub-pixel positions */
static void scene_gouraud(const struct gpu_sink *s)
{
    gpal_ramps(s);
    target(s, 0, 640, 640, 480);
    fill(s, 0x00, 0, 0, 640, 480);
    for (uint32_t r = 0; r < 8; r++) {
        const int x = GPU_FX(20 + 76 * (int)r), y = GPU_FX(20);
        tri(s, GPU_TRI_GOURAUD, x + 3, y + 5, GPU_ZC(0, 32 * r), x + GPU_FX(70) + 9, y + GPU_FX(30), GPU_ZC(0, 32 * r + 31),
            x + GPU_FX(20), y + GPU_FX(200) + 7, GPU_ZC(0, 32 * r + 15));
    }
    tri(s, GPU_TRI_GOURAUD, GPU_FX(30), GPU_FX(240), GPU_ZC(0, 0), GPU_FX(610), GPU_FX(260) + 8, GPU_ZC(0, 255),
        GPU_FX(320) + 4, GPU_FX(470) + 12, GPU_ZC(0, 128));
    show640(s);
}

/* depth at 320 x 240 (page A, the depth buffer of section 16.6): two Gouraud triangles crossing,
 * each in front on one side; a third tested but not written; then the first again in another
 * ramp with less or equal, which passes exactly where the first is in front (the third did not
 * write its depth); then shown doubled */
static void scene_depth(const struct gpu_sink *s)
{
    target(s, 153600, 640, 640, 240);                   /* every depth 0xFFFF */
    fill(s, 0xFF, 0, 0, 640, 240);
    target(s, 0, 320, 320, 240);
    fill(s, 0x00, 0, 0, 320, 240);
    c3(s, HDR(SET_ZBUF, 0), 153600, 640);
    const uint32_t D = GPU_TRI_GOURAUD | GPU_TRI_ZTEST | GPU_TRI_ZWRITE;
    tri(s, D, GPU_FX(10), GPU_FX(20), GPU_ZC(1000, 32 + 31), GPU_FX(310), GPU_FX(60), GPU_ZC(60000, 32 + 4),
        GPU_FX(30), GPU_FX(220), GPU_ZC(30000, 32 + 16));
    tri(s, D, GPU_FX(300), GPU_FX(10), GPU_ZC(62000, 96 + 2), GPU_FX(290), GPU_FX(230), GPU_ZC(2000, 96 + 31),
        GPU_FX(15), GPU_FX(120), GPU_ZC(31000, 96 + 14));
    tri(s, GPU_TRI_ZTEST | 0xA8, GPU_FX(100), GPU_FX(100), GPU_ZC(20000, 0), GPU_FX(220), GPU_FX(100), GPU_ZC(20000, 0),
        GPU_FX(160), GPU_FX(200), GPU_ZC(20000, 0));                               /* tested, not written */
    tri(s, GPU_TRI_GOURAUD | GPU_TRI_ZTEST | GPU_TRI_ZFUNC_LE, GPU_FX(10), GPU_FX(20), GPU_ZC(1000, 128 + 31),
        GPU_FX(310), GPU_FX(60), GPU_ZC(60000, 128 + 4), GPU_FX(30), GPU_FX(220), GPU_ZC(30000, 128 + 16));
    placement(s, 0, 320, 2);
    s->video(SURF_VIDEO_CTRL, CTRL_ON(2));
}

/* page B drawn by the engine behind page A (shown), then FLIP to it: the CPU never writes GFX_BASE */
static void scene_flip(const struct gpu_sink *s)
{
    target(s, 76800, 320, 320, 240);
    fill(s, 0x08, 0, 0, 320, 240);
    for (int k = 0; k < 12; k++)
        tri(s, GPU_TRI_GOURAUD, GPU_FX(160), GPU_FX(120), GPU_ZC(0, 32 * (k & 7)),
            GPU_FX(160) + 150 * sin64(4u * (uint32_t)k + 16u) / 1024, GPU_FX(120) + 110 * sin64(4u * (uint32_t)k) / 1024,
            GPU_ZC(0, 32 * (k & 7) + 31), GPU_FX(160) + 150 * sin64(4u * (uint32_t)k + 20u) / 1024,
            GPU_FX(120) + 110 * sin64(4u * (uint32_t)k + 4u) / 1024, GPU_ZC(0, 32 * (k & 7) + 20));
    for (unsigned k = 0; k < 7; k++)
        letter(s, word_surfgpu[k], 30 + 38 * (int)k, 200, 4, 0xFF | GPU_EXPAND_TRANSP);
    c2(s, HDR(FLIP, 0), 76800);
    c2(s, HDR(FENCE, 0), 0xF11Bu);
    s->sync();
}

/* ------------------------------------------------------------------ the scene list */

const struct gpu_scene gpu_scenes[] = {
    {"colour bars (MODE 0)", 0, scene_bars},
    {"text, 256 glyphs (MODE 1)", 0, scene_text},
    {"VRAM 640x480, RGB332", 0, scene_gfx640},
    {"GPAL greys", 0, scene_greys},
    {"320x240 page 2, doubled", 0, scene_page2},
    {"text over graphics (MODE 3)", 0, scene_overlay},
    {"ENABLE 0", 0, scene_black},
    {"engine: fills, alignments, clips", 1, scene_fills},
    {"engine: lines in every octant", 1, scene_lines},
    {"engine: overlapping copies, key", 1, scene_copies},
    {"engine: EXPAND letters, sprites", 1, scene_bitmaps},
    {"engine: 320x240 page, flipped", 1, scene_page_flip},
    {"engine: demo", 1, scene_demo},
    {"engine: flat triangles, culling", 1, scene_flat},
    {"engine: Gouraud triangles, ramps", 1, scene_gouraud},
    {"engine: depth, 320x240", 1, scene_depth},
    {"engine: page B drawn, FLIP", 1, scene_flip},
};
const unsigned gpu_scene_count = sizeof gpu_scenes / sizeof gpu_scenes[0];
