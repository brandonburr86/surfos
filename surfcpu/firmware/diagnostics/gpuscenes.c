/*
SurfGPU diagnostics: the scenes
--------------------
File: gpuscenes.c Date: 10/18/26 (SurfGPU)
--------------------
gpudiag's pictures (gpuscenes.h). The first ones need only VRAM and the graphics scan-out (the
CPU draws through the VRAM window): the test pattern, text, 640x480 graphics through the default
and a grey palette, a 320x240 page shown doubled, text over graphics, a black picture. The
engine's scenes follow (docs/GPU.md section 5): every command, every alignment of x mod 4,
clipping at each edge, overlapping copies in every direction, the colour key, lines in every
octant, bitmaps and pixel rectangles, and a page drawn by the engine and flipped to.
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

/* ------------------------------------------------------------------ the scene list */

const struct gpu_scene gpu_scenes[] = {
    {"colour bars (MODE 0)", 0, scene_bars},
    {"text, 256 glyphs (MODE 1)", 0, scene_text},
    {"VRAM 640x480, RGB332", 0, scene_gfx640},
    {"GPAL greys", 0, scene_greys},
    {"320x240 page 2, doubled", 0, scene_page2},
    {"text over graphics (MODE 3)", 0, scene_overlay},
    {"ENABLE 0", 0, scene_black},
};
const unsigned gpu_scene_count = sizeof gpu_scenes / sizeof gpu_scenes[0];
