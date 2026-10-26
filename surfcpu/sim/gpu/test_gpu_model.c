/*
SurfGPU reference model self-test
--------------------
File: test_gpu_model.c Date: 10/18/26 (SurfGPU), 10/25/26 (GPU2)
--------------------
The C model (sim/models/gpu_model.c) against expectations written by hand from docs/GPU.md,
never from the RTL: so that when the RTL and the model agree, they agree with the document.
The frame CRCs below were computed independently from GPU.md's definitions with Python's zlib
(the test pattern, GPAL's RGB332, the graphics address rule with doubling and wrapping); the
engine's results are traced by hand: pixels, clips, Bresenham's points, overlapping copies,
the bitmap's bit order, the target's refusals, fences and errors.
GPU2 (section 16): TRIANGLE's coverage worked out pixel by pixel (a small triangle in both
windings, the top-left rule at a top, a bottom, a left and a right edge through pixel centres,
culling, zero areas, a huge triangle clipped), Gouraud values with exact, halved and inexact
gradients (the last one pins the floor of 16.4), the depth buffer's layout, the depth functions,
ZWRITE without ZTEST and ZTEST without ZWRITE, two triangles meeting in depth, SET_ZBUF's
refusals and SET_TARGET dropping the depth buffer, FLIP. Then properties, in the counting mode
(gpu_tri_setup, gpu_tri_inside): squares split along either diagonal, fans around a point and
jittered triangulated grids cover each pixel of their hull exactly once (the hull's pixels worked
out independently: a half-open box, or a convex polygon's edges), and coverage does not depend on
the vertex order; random triangles never need the clamp or a wider value than the RTL keeps.
*/
#include "gpu_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

static uint8_t vram[GPU_VRAM_BYTES];
static uint16_t frame[GPU_SCREEN_W * GPU_SCREEN_H];
static uint8_t font[256][16];

static uint32_t xy(int x, int y) { return (uint32_t)(uint16_t)x | (uint32_t)(uint16_t)y << 16; }
static uint32_t wh(unsigned w, unsigned h) { return w | h << 16; }
static uint32_t hdr(unsigned op, uint32_t bits) { return op << 24 | bits; }

static void cmd(struct gpu_engine *g, const uint32_t *w, int n)
{
    for (int i = 0; i < n; i++)
        gpu_engine_word(g, w[i]);
}

static void display(void)
{
    struct gpu_display d;
    gpu_display_reset(&d, 1);
    CHECK(gpu_gpal_initial(0) == 0x000 && gpu_gpal_initial(0xFF) == 0xFFF && gpu_gpal_initial(0xE0) == 0xF00 &&
          gpu_gpal_initial(0x1C) == 0x0F0 && gpu_gpal_initial(0x03) == 0x00F && gpu_gpal_initial(0x24) == 0x220 &&
          gpu_gpal_initial(0x49) == 0x445, "GPAL's RGB332");
    CHECK(gpu_crc32(0, "123456789", 9) == 0xCBF43926u, "CRC-32 check value");

    memset(frame, 0, sizeof frame);
    CHECK(gpu_frame_crc(frame) == 0xC656B350u, "black frame CRC %08x", gpu_frame_crc(frame));
    gpu_display_render(&d, NULL, NULL, (const uint8_t(*)[16])font, frame);
    CHECK(gpu_frame_crc(frame) == 0x2077DB89u, "test pattern CRC %08x", gpu_frame_crc(frame));
    CHECK(frame[0] == 0xFFF && frame[1 * 640 + 1] == 0xFFF && frame[1 * 640 + 81] == 0xFF0 &&
          frame[370 * 640 + 41] == 0x111 && frame[400 * 640 + 41] == 0x100, "test pattern pixels");

    for (uint32_t a = 0; a < GPU_VRAM_BYTES; a++)
        vram[a] = (uint8_t)(a * 7 + (a >> 9));
    d.mode = 2;
    gpu_display_render(&d, vram, NULL, (const uint8_t(*)[16])font, frame);
    CHECK(gpu_frame_crc(frame) == 0xCFD3A84Eu, "graphics 640x480 CRC %08x", gpu_frame_crc(frame));
    d.gfx_base = 4 | 3;                     /* bits 1:0 ignored */
    d.gfx_stride = 324 | 1;
    d.gfx_scale = 2;
    gpu_display_render(&d, vram, NULL, (const uint8_t(*)[16])font, frame);
    CHECK(gpu_frame_crc(frame) == 0x0A1E1539u, "graphics doubled CRC %08x", gpu_frame_crc(frame));
    d.gfx_base = 307200 - 640 * 100;        /* lines past the memory show entry 0 */
    d.gfx_stride = 640;
    d.gfx_scale = 3;                        /* acts as 1 */
    gpu_display_render(&d, vram, NULL, (const uint8_t(*)[16])font, frame);
    CHECK(gpu_frame_crc(frame) == 0x3E8EB9B9u, "graphics past the end CRC %08x", gpu_frame_crc(frame));
    CHECK(frame[99 * 640] == gpu_gpal_initial(vram[307200 - 640]) && frame[100 * 640] == 0x000, "past the end");

    /* the overlay: glyph pixels, then a non-zero background, then graphics */
    static uint32_t text[2400];
    memset(font, 0, sizeof font);
    font[0x41][0] = 0x80;                   /* 'A': one pixel at the top left */
    text[0] = 0x0000 | 0x0F00 | 0x41;       /* fg 15, bg 0 */
    text[1] = 0x1000 | 0x0F00 | 0x41;       /* fg 15, bg 1 */
    gpu_display_reset(&d, 1);
    d.mode = 3;
    gpu_display_render(&d, vram, text, (const uint8_t(*)[16])font, frame);
    CHECK(frame[0] == 0xFFF, "glyph pixel shows the foreground");
    CHECK(frame[1] == gpu_gpal_initial(vram[1]), "background 0 shows graphics");
    CHECK(frame[8] == 0xFFF && frame[9] == 0x00A, "background 1 shows the text background");
    d.cursor = 1;
    d.cur_col = 0;
    d.cur_row = 0;
    gpu_display_render(&d, vram, text, (const uint8_t(*)[16])font, frame);
    CHECK(frame[14 * 640 + 3] == 0xFFF && frame[13 * 640 + 3] == gpu_gpal_initial(vram[13 * 640 + 3]),
          "the cursor counts as a glyph pixel");
    d.has_gfx = 0;
    gpu_display_render(&d, vram, text, (const uint8_t(*)[16])font, frame);
    CHECK(gpu_frame_crc(frame) == 0xC656B350u, "mode 3 without graphics is black");
}

static void engine(void)
{
    struct gpu_engine g;
    memset(vram, 0, sizeof vram);
    gpu_engine_init(&g, vram);

    /* FILL_RECT, the target and clip after a reset (640 x 480 at 0) */
    uint32_t f1[] = {hdr(GPU_OP_FILL_RECT, 7), xy(2, 3), wh(4, 2)};
    cmd(&g, f1, 3);
    CHECK(vram[3 * 640 + 2] == 7 && vram[4 * 640 + 5] == 7 && vram[3 * 640 + 1] == 0 && vram[3 * 640 + 6] == 0 &&
          vram[5 * 640 + 2] == 0 && g.writes == 8, "FILL_RECT 4x2 at (2,3)");
    uint32_t f2[] = {hdr(GPU_OP_FILL_RECT, 9), xy(-3, -2), wh(5, 4)};
    cmd(&g, f2, 3);
    CHECK(vram[0] == 9 && vram[1] == 9 && vram[2] == 0 && vram[640 + 1] == 9 && vram[2 * 640] == 0,
          "negative coordinates clipped");
    uint32_t f3[] = {hdr(GPU_OP_FILL_RECT, 5), xy(638, 478), wh(10, 10)};
    cmd(&g, f3, 3);
    CHECK(vram[478 * 640 + 638] == 5 && vram[479 * 640 + 639] == 5 && vram[479 * 640 + 637] == 0,
          "the far corner clipped to the target");

    /* SET_CLIP */
    uint32_t c1[] = {hdr(GPU_OP_SET_CLIP, 0), xy(10, 20), xy(12, 21), hdr(GPU_OP_FILL_RECT, 4), xy(0, 0), wh(100, 100)};
    unsigned long long w0 = g.writes;
    cmd(&g, c1, 6);
    CHECK(g.writes - w0 == 2 && vram[20 * 640 + 10] == 4 && vram[20 * 640 + 11] == 4, "the clip's two pixels");

    /* SET_TARGET: refusals, then a small target */
    uint32_t t1[] = {hdr(GPU_OP_SET_TARGET, 0), 2, 64, wh(16, 16)};         /* unaligned base */
    cmd(&g, t1, 4);
    CHECK((g.sticky & GPU_ST_BAD_PARAM) && g.base == 0 && g.tw == 640 && g.cx0 == 10, "unaligned base refused");
    g.sticky = 0;
    uint32_t t2[] = {hdr(GPU_OP_SET_TARGET, 0), 307200 - 64 * 3, 64, wh(64, 4)};  /* one line too many */
    cmd(&g, t2, 4);
    CHECK(g.sticky & GPU_ST_BAD_PARAM, "a target past VRAM refused");
    g.sticky = 0;
    uint32_t t3[] = {hdr(GPU_OP_SET_TARGET, 0), 0, 64, wh(65, 2)};          /* wider than the stride */
    cmd(&g, t3, 4);
    CHECK(g.sticky & GPU_ST_BAD_PARAM, "width over stride refused");
    g.sticky = 0;
    uint32_t t4[] = {hdr(GPU_OP_SET_TARGET, 0), 0, 64, wh(100, 1)};         /* one line: allowed */
    cmd(&g, t4, 4);
    CHECK(!g.sticky && g.tw == 100 && g.th == 1, "a one-line target wider than its stride");
    uint32_t t5[] = {hdr(GPU_OP_SET_TARGET, 0), 307200 - 64 * 3, 64, wh(64, 3)};
    cmd(&g, t5, 4);
    CHECK(!g.sticky && g.base == 307200 - 192 && g.cx1 == 64 && g.cy1 == 3 && g.cx0 == 0, "the last 192 bytes");
    uint32_t t6[] = {hdr(GPU_OP_SET_TARGET, 0), 0x1000, 32, wh(8, 8)};
    cmd(&g, t6, 4);
    memset(vram, 0, sizeof vram);

    /* LINE: (0,0)-(3,1) is (0,0) (1,0) (2,1) (3,1); a point; a vertical line; clipping */
    uint32_t l1[] = {hdr(GPU_OP_LINE, 1), xy(0, 0), xy(3, 1)};
    cmd(&g, l1, 3);
    CHECK(vram[0x1000] == 1 && vram[0x1001] == 1 && vram[0x1000 + 32 + 2] == 1 && vram[0x1000 + 32 + 3] == 1 &&
          vram[0x1002] == 0 && vram[0x1000 + 32 + 1] == 0, "LINE (0,0)-(3,1)");
    w0 = g.writes;
    uint32_t l2[] = {hdr(GPU_OP_LINE, 2), xy(5, 5), xy(5, 5), hdr(GPU_OP_LINE, 3), xy(7, 0), xy(7, 3)};
    cmd(&g, l2, 6);
    CHECK(g.writes - w0 == 5 && vram[0x1000 + 5 * 32 + 5] == 2 && vram[0x1000 + 3 * 32 + 7] == 3, "a point, a column");
    w0 = g.writes;
    uint32_t l3[] = {hdr(GPU_OP_LINE, 4), xy(-4, -4), xy(11, 11)};          /* the diagonal through 8x8 */
    cmd(&g, l3, 3);
    CHECK(g.writes - w0 == 8 && vram[0x1000 + 7 * 32 + 7] == 4 && vram[0x1000] == 4, "a clipped diagonal");

    /* COPY_RECT: overlapping, to the right by one: A B C D -> A A B C D */
    memset(vram, 0, sizeof vram);
    for (int i = 0; i < 4; i++)
        vram[0x1000 + i] = (uint8_t)(0xA0 + i);
    uint32_t k1[] = {hdr(GPU_OP_COPY_RECT, 0), xy(0, 0), xy(1, 0), wh(4, 1)};
    cmd(&g, k1, 4);
    CHECK(vram[0x1000] == 0xA0 && vram[0x1001] == 0xA0 && vram[0x1002] == 0xA1 && vram[0x1003] == 0xA2 &&
          vram[0x1004] == 0xA3, "overlapping copy to the right");
    uint32_t k2[] = {hdr(GPU_OP_COPY_RECT, 0), xy(1, 0), xy(0, 0), wh(4, 1)};   /* and back to the left */
    cmd(&g, k2, 4);
    CHECK(vram[0x1000] == 0xA0 && vram[0x1001] == 0xA1 && vram[0x1002] == 0xA2 && vram[0x1003] == 0xA3 &&
          vram[0x1004] == 0xA3, "overlapping copy to the left");
    /* downward, overlapping rows: rows 0..2 to rows 1..3 */
    memset(vram, 0, sizeof vram);
    for (int r = 0; r < 3; r++)
        vram[0x1000 + r * 32] = (uint8_t)(r + 1);
    uint32_t k3[] = {hdr(GPU_OP_COPY_RECT, 0), xy(0, 0), xy(0, 1), wh(1, 3)};
    cmd(&g, k3, 4);
    CHECK(vram[0x1000] == 1 && vram[0x1020] == 1 && vram[0x1040] == 2 && vram[0x1060] == 3, "overlapping copy down");
    /* the key, and a source partly outside the target */
    memset(vram, 0, sizeof vram);
    vram[0x1000] = 5;
    vram[0x1001] = 6;
    uint32_t k4[] = {hdr(GPU_OP_COPY_RECT, GPU_COPY_KEY | 5), xy(0, 0), xy(0, 1), wh(2, 1)};
    cmd(&g, k4, 4);
    CHECK(vram[0x1020] == 0 && vram[0x1021] == 6, "the key colour is not copied");
    w0 = g.writes;
    uint32_t k5[] = {hdr(GPU_OP_COPY_RECT, 0), xy(-1, 0), xy(3, 3), wh(2, 1)};
    cmd(&g, k5, 4);
    CHECK(g.writes - w0 == 1 && vram[0x1000 + 3 * 32 + 4] == 5, "only pixels with a source in the target");

    /* EXPAND: 8x2 from one word, MSB first; transparent; 3x3 from 9 bits */
    memset(vram, 0, sizeof vram);
    uint32_t e1[] = {hdr(GPU_OP_EXPAND, 2 << 8 | 1), xy(0, 0), wh(8, 2), 0xF0A50000u};
    cmd(&g, e1, 4);
    const uint8_t want[16] = {1, 1, 1, 1, 2, 2, 2, 2, 1, 2, 1, 2, 2, 1, 2, 1};
    int bad = 0;
    for (int i = 0; i < 16; i++)
        bad += vram[0x1000 + (i / 8) * 32 + i % 8] != want[i];
    CHECK(bad == 0 && gpu_engine_idle(&g), "EXPAND's bit order");
    memset(vram, 9, 0x2000);
    uint32_t e2[] = {hdr(GPU_OP_EXPAND, GPU_EXPAND_TRANSP | 2 << 8 | 1), xy(1, 1), wh(3, 3), 0x157u << 23};
    cmd(&g, e2, 4);                         /* 101 010 111 */
    CHECK(vram[0x1021] == 1 && vram[0x1022] == 9 && vram[0x1023] == 1 && vram[0x1041] == 9 && vram[0x1042] == 1 &&
          vram[0x1061] == 1 && vram[0x1063] == 1 && gpu_engine_idle(&g), "EXPAND transparent, 9 bits");

    /* WRITE_RECT: 3x2 from two words, clipped at the left */
    memset(vram, 0, sizeof vram);
    uint32_t r1[] = {hdr(GPU_OP_WRITE_RECT, 0), xy(-1, 0), wh(3, 2), 0x04030201u, 0x00000605u};
    cmd(&g, r1, 5);
    CHECK(vram[0x1000] == 2 && vram[0x1001] == 3 && vram[0x1020] == 5 && vram[0x1021] == 6 && vram[0x1002] == 0 &&
          gpu_engine_idle(&g), "WRITE_RECT, clipped pixels consumed");

    /* FENCE, opcodes, NOP, ABORT */
    uint32_t n1[] = {hdr(GPU_OP_NOP, 0), hdr(GPU_OP_FENCE, 0), 0x1234, hdr(GPU_OP_FENCE, GPU_FENCE_IRQ), 0x5678};
    cmd(&g, n1, 5);
    CHECK(g.fence == 0x5678 && g.fences == 2 && (g.sticky & GPU_ST_FENCE_EV), "fences");
    g.sticky = 0;
    uint32_t n2[] = {hdr(0x0D, 0), hdr(GPU_OP_FENCE, 0), 0x99};
    cmd(&g, n2, 3);
    CHECK((g.sticky & GPU_ST_BAD_OPCODE) && g.fence == 0x99, "an unknown opcode is dropped, the next word is a command");
    uint32_t n3[] = {hdr(GPU_OP_WRITE_RECT, 0), xy(0, 0), wh(64, 64), 1, 2};
    cmd(&g, n3, 5);
    CHECK(!gpu_engine_idle(&g), "WRITE_RECT waits for its data");
    gpu_engine_abort(&g);
    uint32_t n4[] = {hdr(GPU_OP_FENCE, 0), 0x77};
    cmd(&g, n4, 2);
    CHECK(gpu_engine_idle(&g) && g.fence == 0x77, "ABORT ends it");
}

/* ------------------------------------------------------------------ GPU2: TRIANGLE */

#define FX(px) ((px) * 16)                  /* whole pixels in 12.4 */
static const uint32_t TB = 0x2000, TS = 64; /* the triangle tests' target: 64 x 64 at 0x2000 */

static void tri_target(struct gpu_engine *g)
{
    uint32_t t[] = {hdr(GPU_OP_SET_TARGET, 0), TB, TS, wh(64, 64)};
    cmd(g, t, 4);
}

/* a flat TRIANGLE of colour c (12.4 vertices), with header flags f */
static void tri(struct gpu_engine *g, uint32_t f, int x0, int y0, int x1, int y1, int x2, int y2)
{
    uint32_t w[] = {hdr(GPU_OP_TRIANGLE, f), xy(x0, y0), 0, xy(x1, y1), 0, xy(x2, y2), 0};
    cmd(g, w, 7);
}

/* a TRIANGLE with each vertex's depth and colour */
static void tri_zc(struct gpu_engine *g, uint32_t f, const int v[3][4])
{
    uint32_t w[7] = {hdr(GPU_OP_TRIANGLE, f)};
    for (int i = 0; i < 3; i++) {
        w[1 + 2 * i] = xy(v[i][0], v[i][1]);
        w[2 + 2 * i] = (uint32_t)v[i][2] | (uint32_t)v[i][3] << 16;
    }
    cmd(g, w, 7);
}

static uint8_t px(int x, int y) { return vram[TB + (uint32_t)y * TS + (uint32_t)x]; }

/* the 64 x 64 target against a rule: pixels where want() is true have colour c, the rest 0;
 * nothing written outside the target (its stride is its width, so check the bytes around it) */
static int check_cover(const char *what, uint8_t c, int (*want)(int x, int y))
{
    int bad = 0, n = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const int w = want(x, y);
            n += w;
            if (px(x, y) != (w ? c : 0)) {
                if (bad++ < 4)
                    printf("  %s: pixel (%d, %d) is %u, want %u\n", what, x, y, px(x, y), w ? c : 0);
            }
        }
    for (uint32_t a = TB - 256; a < TB; a++)
        bad += vram[a] != 0;
    for (uint32_t a = TB + 64 * 64; a < TB + 64 * 64 + 256; a++)
        bad += vram[a] != 0;
    return bad == 0 ? n : -1;
}

static int r_small(int x, int y) { return x + y <= 2; }
static int r_top(int x, int y) { return y <= 3 && x + y <= 3; }
static int r_bottom(int x, int y) { return y >= 1 && y <= 3 && x < y; }
static int r_left(int x, int y) { return y <= 3 && x <= y; }
static int r_right(int x, int y) { return y <= 3 && x <= 2 && x + y >= 3; }
static int r_all(int x, int y) { (void)x; (void)y; return 1; }
static int r_none(int x, int y) { (void)x; (void)y; return 0; }

static void clear_target(void) { memset(vram + TB - 4096, 0, 64 * 64 + 8192); }

static void triangles(void)
{
    struct gpu_engine g;
    memset(vram, 0, sizeof vram);
    gpu_engine_init(&g, vram);
    tri_target(&g);

    /* (0,0) (4,0) (0,4), clockwise: the hypotenuse x + y = 4 is a right edge, so centres on it
     * (x + y = 3) are not covered; the top edge y = 0 and the left edge x = 0 pass no centre */
    tri(&g, 7, 0, 0, FX(4), 0, 0, FX(4));
    CHECK(check_cover("small, clockwise", 7, r_small) == 6, "a 4x4 right triangle: the 6 pixels x + y <= 2");
    clear_target();
    tri(&g, 7, 0, 0, 0, FX(4), FX(4), 0);   /* the same, counter-clockwise */
    CHECK(check_cover("small, counter-clockwise", 7, r_small) == 6, "the other winding covers the same 6 pixels");
    /* culling: CW is the first winding (A > 0 with y down) */
    clear_target();
    tri(&g, 7 | GPU_TRI_CULL_CW, 0, 0, FX(4), 0, 0, FX(4));
    tri(&g, 7 | GPU_TRI_CULL_CCW, 0, 0, 0, FX(4), FX(4), 0);
    tri(&g, 7 | GPU_TRI_CULL_CW | GPU_TRI_CULL_CCW, 0, 0, 0, FX(4), FX(4), 0);
    CHECK(check_cover("culled", 7, r_none) == 0 && g.tri_culled == 3, "CULL_CW, CULL_CCW and both cull their windings");
    tri(&g, 7 | GPU_TRI_CULL_CCW, 0, 0, FX(4), 0, 0, FX(4));
    CHECK(check_cover("CULL_CCW keeps CW", 7, r_small) == 6, "CULL_CCW draws the clockwise one");
    clear_target();
    tri(&g, 7 | GPU_TRI_CULL_CW, 0, 0, 0, FX(4), FX(4), 0);
    CHECK(check_cover("CULL_CW keeps CCW", 7, r_small) == 6, "CULL_CW draws the counter-clockwise one");
    /* zero area: collinear, two vertices equal, all equal */
    clear_target();
    const unsigned long long z0 = g.tri_zero;
    tri(&g, 7, 0, 0, FX(2), FX(2), FX(4), FX(4));
    tri(&g, 7, FX(1), FX(1), FX(1), FX(1), FX(9), FX(3));
    tri(&g, 7, 40, 40, 40, 40, 40, 40);
    CHECK(check_cover("zero area", 7, r_none) == 0 && g.tri_zero - z0 == 3 && !g.sticky, "zero areas draw nothing, no error");
    /* a top edge through row 0's centres (y = 0.5): included */
    tri(&g, 3, 0, 8, FX(4), 8, 0, FX(4) + 8);
    CHECK(check_cover("top edge", 3, r_top) == 10, "a top edge through centres covers them");
    clear_target();
    /* a bottom edge through row 4's centres (y = 4.5): excluded */
    tri(&g, 3, 0, 8, FX(4), FX(4) + 8, 0, FX(4) + 8);
    CHECK(check_cover("bottom edge", 3, r_bottom) == 6, "a bottom edge through centres does not");
    clear_target();
    /* a left edge through column 0's centres (x = 0.5): included */
    tri(&g, 3, 8, 0, FX(4) + 8, FX(4), 8, FX(4));
    CHECK(check_cover("left edge", 3, r_left) == 10, "a left edge through centres covers them");
    clear_target();
    /* a right edge through column 3's centres (x = 3.5): excluded */
    tri(&g, 3, FX(3) + 8, 0, FX(3) + 8, FX(4), -8, FX(4));
    CHECK(check_cover("right edge", 3, r_right) == 6, "a right edge through centres does not");
    clear_target();
    /* huge: vertices at the ends of the 12.4 range (the half of the square below its diagonal
     * x + y = -1/16, so every centre of the target); the target clips it to its 64 x 64 */
    tri(&g, 9, 32767, -32768, 32767, 32767, -32768, 32767);
    CHECK(check_cover("huge", 9, r_all) == 4096, "a triangle across the whole 12.4 range fills the target and no more");
    /* the clip cuts it too */
    uint32_t c1[] = {hdr(GPU_OP_SET_CLIP, 0), xy(10, 20), xy(13, 22)};
    cmd(&g, c1, 3);
    clear_target();
    const unsigned long long w0 = g.writes;
    tri(&g, 9, 32767, -32768, 32767, 32767, -32768, 32767);
    CHECK(g.writes - w0 == 6 && px(10, 20) == 9 && px(12, 21) == 9 && px(13, 21) == 0 && px(9, 20) == 0,
          "the clip's 3 x 2 pixels only");
    tri_target(&g);
    clear_target();

    /* Gouraud: vertices on centres (0,0) 0, (16,0) 16, (0,16) 32: A = 65536, GX = 2^16, GY = 2^17,
     * so V = 2^20 (x + 2y) exactly: pixel (x, y) is x + 2y; the hypotenuse is a right edge */
    const int g1[3][4] = {{8, 8, 0, 0}, {FX(16) + 8, 8, 0, 16}, {8, FX(16) + 8, 0, 32}};
    tri_zc(&g, GPU_TRI_GOURAUD | 0x55, g1);
    int bad = 0, n = 0;
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < 20; x++) {
            const int in = x + y <= 15;
            n += in;
            bad += px(x, y) != (in ? x + 2 * y : 0);
        }
    CHECK(bad == 0 && n == 136, "Gouraud x + 2y: %d pixels wrong", bad);
    clear_target();
    /* half a step a pixel: x / 2 rounded to the nearest, halves up: 0 1 1 2 2 3 ... */
    const int g2[3][4] = {{8, 8, 0, 0}, {FX(16) + 8, 8, 0, 8}, {8, FX(16) + 8, 0, 0}};
    tri_zc(&g, GPU_TRI_GOURAUD, g2);
    CHECK(px(0, 0) == 0 && px(1, 0) == 1 && px(2, 0) == 1 && px(3, 0) == 2 && px(9, 0) == 5 && px(9, 3) == 5,
          "halves round up: %u %u %u %u", px(0, 0), px(1, 0), px(2, 0), px(3, 0));
    clear_target();
    /* down by half a step: 8 8 7 7 6 ... (floor((8 2^20 - 2^19 x + 2^19) / 2^20)) */
    const int g3[3][4] = {{8, 8, 0, 8}, {FX(16) + 8, 8, 0, 0}, {8, FX(16) + 8, 0, 8}};
    tri_zc(&g, GPU_TRI_GOURAUD, g3);
    CHECK(px(0, 0) == 8 && px(1, 0) == 8 && px(2, 0) == 7 && px(3, 0) == 7 && px(4, 0) == 6 && px(14, 1) == 1,
          "downhill halves: %u %u %u %u %u", px(0, 0), px(1, 0), px(2, 0), px(3, 0), px(4, 0));
    clear_target();
    /* an inexact gradient, the floor of 16.4: (16, 8) 1, (64, 8) 0, (16, 56) 1: Nx = -48, A = 2304,
     * GX = floor(-21845.33) = -21846; pixel (2, 0)'s centre is 24 right of vertex 0: V = 2^20 -
     * 24 * 21846 = 524272, just under 2^19, so 0 (the exact plane is 1/2 there, and a gradient cut
     * toward zero, -21845, would give 524296 and 1) */
    const int g4[3][4] = {{16, 8, 0, 1}, {64, 8, 0, 0}, {16, 56, 0, 1}};
    tri_zc(&g, GPU_TRI_GOURAUD, g4);
    CHECK(px(1, 0) == 1 && px(2, 0) == 0, "the gradient's floor: pixel (2, 0) is %u, want 0 (pixel (1, 0) %u, want 1)",
          px(2, 0), px(1, 0));
    struct gpu_plane pl;
    int32_t vx4[3] = {16, 64, 16}, vy4[3] = {8, 8, 56};
    uint32_t vv4[3] = {1, 0, 1};
    gpu_plane_setup(&pl, vx4, vy4, vv4);
    CHECK(pl.gx == -21846 && pl.gy == 0, "the gradients: GX %lld, GY %lld", (long long)pl.gx, (long long)pl.gy);
    clear_target();

    /* depth: no buffer yet, so a TRIANGLE with ZTEST or ZWRITE is refused */
    g.sticky = 0;
    tri(&g, 7 | GPU_TRI_ZWRITE, 0, 0, FX(4), 0, 0, FX(4));
    CHECK((g.sticky & GPU_ST_BAD_PARAM) && check_cover("no depth buffer", 7, r_none) == 0,
          "ZWRITE without a depth buffer: refused");
    g.sticky = 0;
    tri(&g, 7 | GPU_TRI_ZTEST, 0, 0, FX(4), 0, 0, FX(4));
    CHECK((g.sticky & GPU_ST_BAD_PARAM) && check_cover("no depth buffer", 7, r_none) == 0,
          "ZTEST without a depth buffer: refused");
    g.sticky = 0;
    /* SET_ZBUF's refusals against the 64 x 64 target (2W = 128) */
    const uint32_t zr[][2] = {{0x8002, 128}, {0x8000, 130}, {307200, 128}, {0x8000, 0x10000}, {0x8000, 124},
                              {307200 - 63 * 128 - 124, 128}};
    for (unsigned i = 0; i < sizeof zr / sizeof zr[0]; i++) {
        uint32_t w[] = {hdr(GPU_OP_SET_ZBUF, 0), zr[i][0], zr[i][1]};
        cmd(&g, w, 3);
        CHECK((g.sticky & GPU_ST_BAD_PARAM) && !g.zvalid, "SET_ZBUF %u, %u refused", zr[i][0], zr[i][1]);
        g.sticky = 0;
    }
    uint32_t zend[] = {hdr(GPU_OP_SET_ZBUF, 0), 307200 - 63 * 128 - 128, 128};   /* ends at VRAM's end */
    cmd(&g, zend, 3);
    CHECK(!g.sticky && g.zvalid && g.zbase == 307200 - 63 * 128 - 128, "SET_ZBUF ending at VRAM's end: taken");
    /* SET_TARGET drops it; a refused SET_TARGET does not */
    uint32_t tbad[] = {hdr(GPU_OP_SET_TARGET, 0), 2, 64, wh(16, 16)};
    cmd(&g, tbad, 4);
    CHECK(g.zvalid && (g.sticky & GPU_ST_BAD_PARAM), "a refused SET_TARGET keeps the depth buffer");
    g.sticky = 0;
    tri_target(&g);
    CHECK(!g.zvalid, "an accepted SET_TARGET leaves no depth buffer");
    /* a one-line target may be wider than the stride; an empty one takes any */
    uint32_t t1[] = {hdr(GPU_OP_SET_TARGET, 0), 0x1000, 64, wh(100, 1)}, z1[] = {hdr(GPU_OP_SET_ZBUF, 0), 0x8000, 64};
    cmd(&g, t1, 4);
    cmd(&g, z1, 3);
    CHECK(!g.sticky && g.zvalid, "a one-line target's depth wider than the stride: taken");
    uint32_t t2[] = {hdr(GPU_OP_SET_TARGET, 0), 0x1000, 64, wh(0, 5)}, z2[] = {hdr(GPU_OP_SET_ZBUF, 0), 307196, 4};
    cmd(&g, t2, 4);
    cmd(&g, z2, 3);
    CHECK(!g.sticky && g.zvalid, "an empty target takes any aligned depth buffer");

    /* the depth layout: the 64 x 64 target, depth at 0x8000 stride 128; ZWRITE alone writes the
     * depth of every covered pixel, little-endian at 0x8000 + 128 y + 2x */
    tri_target(&g);
    uint32_t zb[] = {hdr(GPU_OP_SET_ZBUF, 0), 0x8000, 128};
    cmd(&g, zb, 3);
    memset(vram + 0x8000, 0, 128 * 64 + 256);
    const int d1[3][4] = {{0, 0, 0x1234, 0}, {FX(4), 0, 0x1234, 0}, {0, FX(4), 0x1234, 0}};
    tri_zc(&g, GPU_TRI_ZWRITE | 5, d1);
    bad = 0;
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 6; x++) {
            const int in = x + y <= 2;
            bad += vram[0x8000 + 128 * y + 2 * x] != (in ? 0x34 : 0) || vram[0x8000 + 128 * y + 2 * x + 1] != (in ? 0x12 : 0);
            bad += px(x, y) != (in ? 5 : 0);
        }
    CHECK(bad == 0, "ZWRITE: the depth at 0x8000 + 128 y + 2x, little-endian, and the colour: %d wrong", bad);
    /* clearing depth: FILL_RECT 0xFF over its bytes, a target set to them (128 x 64 bytes) */
    uint32_t zc[] = {hdr(GPU_OP_SET_TARGET, 0), 0x8000, 128, wh(128, 64), hdr(GPU_OP_FILL_RECT, 0xFF), xy(0, 0), wh(128, 64)};
    cmd(&g, zc, 7);
    tri_target(&g);
    cmd(&g, zb, 3);
    clear_target();
    /* the depth functions on the same triangle: 1000 drawn; 1000 again with less: nothing; with
     * less or equal: drawn; 999 with less: drawn; 2000 tested without ZWRITE: nothing; 500 tested
     * without ZWRITE: drawn, the depth staying 999 */
    const uint32_t T = GPU_TRI_ZTEST | GPU_TRI_ZWRITE;
    const struct { uint32_t flags; int z; uint8_t c; uint8_t want; unsigned want_z; } steps[] = {
        {T | 1, 1000, 1, 1, 1000},
        {T | 2, 1000, 2, 1, 1000},
        {T | GPU_TRI_ZFUNC_LE | 3, 1000, 3, 3, 1000},
        {T | 4, 999, 4, 4, 999},
        {GPU_TRI_ZTEST | 5, 2000, 5, 4, 999},
        {GPU_TRI_ZTEST | 6, 500, 6, 6, 999},
        {GPU_TRI_ZTEST | GPU_TRI_ZFUNC_LE | 7, 999, 7, 7, 999},
    };
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        const int d[3][4] = {{0, 0, steps[i].z, 0}, {FX(4), 0, steps[i].z, 0}, {0, FX(4), steps[i].z, 0}};
        tri_zc(&g, steps[i].flags, d);
        const unsigned zgot = vram[0x8000] | vram[0x8001] << 8;
        CHECK(px(0, 0) == steps[i].want && px(2, 0) == steps[i].want && px(3, 0) == 0 && zgot == steps[i].want_z,
              "depth step %u: colour %u depth %u, want %u %u", i, px(0, 0), zgot, steps[i].want, steps[i].want_z);
    }
    /* two triangles meeting in depth: T1 rises 100 a pixel from 0 at column 0 (vertices on
     * centres), T2 covers it at 1000 with less: columns 0..10 keep T1 (100 x >= 1000 fails only
     * from 11: 1000 < 100 x), 11 and on show T2 */
    cmd(&g, zc, 7);
    tri_target(&g);
    cmd(&g, zb, 3);
    clear_target();
    const int t1v[3][4] = {{8, 8, 0, 0}, {FX(32) + 8, 8, 3200, 0}, {8, FX(32) + 8, 0, 0}};
    const int t2v[3][4] = {{-FX(100), -FX(100), 1000, 0}, {FX(300), -FX(100), 1000, 0}, {-FX(100), FX(300), 1000, 0}};
    tri_zc(&g, T | 0x11, t1v);
    tri_zc(&g, T | 0x22, t2v);
    CHECK(px(0, 0) == 0x11 && px(10, 0) == 0x11 && px(11, 0) == 0x22 && px(10, 21) == 0x11 && px(11, 20) == 0x22 &&
          px(40, 40) == 0x22 && px(5, 26) == 0x11 && px(6, 26) == 0x22,
          "depth meets at column 10/11: %02x %02x %02x %02x", px(10, 0), px(11, 0), px(10, 21), px(11, 20));
    /* FLIP records its base as GFX_BASE keeps it */
    uint32_t f1[] = {hdr(GPU_OP_FLIP, 0), 0xFFF12C03u};
    cmd(&g, f1, 2);
    CHECK(g.flips == 1 && g.flip_base == (0xFFF12C03u & 0x7FFFCu) && gpu_engine_idle(&g), "FLIP: base 0x%x", g.flip_base);
    CHECK(g.tri_clamped == 0 && g.tri_wide == 0, "the clamp changed %llu values, %llu were wide", g.tri_clamped, g.tri_wide);
}

/* ------------------------------------------------------------------ GPU2: properties */

static uint32_t rng_state = 12345;
static uint32_t rnd32(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static int rndi(int lo, int hi) { return lo + (int)(rnd32() % (uint32_t)(hi - lo + 1)); }

#define PW 96                               /* the counting area: pixels 0..95 squared */
static uint8_t cnt[PW][PW];

/* add a triangle's coverage to cnt (vertices in 12.4, the order as given) */
static void count_tri(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t cx, int32_t cy)
{
    struct gpu_tri t;
    int32_t vx[3] = {ax, bx, cx}, vy[3] = {ay, by, cy};
    if (!gpu_tri_setup(&t, vx, vy))
        return;
    for (int y = 0; y < PW; y++)
        for (int x = 0; x < PW; x++)
            cnt[y][x] += (uint8_t)gpu_tri_inside(&t, x, y);
}

/* the half-open box [x0, x1) x [y0, y1) in 12.4: the pixels whose centres are in it (an axis-
 * aligned rectangle's top and left edges take centres on them, its bottom and right ones not) */
static int box_has(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int x, int y)
{
    const int32_t cx = 16 * x + 8, cy = 16 * y + 8;
    return cx >= x0 && cx < x1 && cy >= y0 && cy < y1;
}

/* a convex polygon (clockwise on the screen, n vertices in 12.4) by its own edges, with the
 * top-left rule written out again here: a centre is in when it is inside every edge, or on one
 * whose direction goes up the screen, or right along it */
static int poly_has(const int32_t *px_, const int32_t *py_, int n, int x, int y)
{
    const int64_t cx = 16 * x + 8, cy = 16 * y + 8;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const int64_t dx = px_[j] - px_[i], dy = py_[j] - py_[i];
        const int64_t e = dx * (cy - py_[i]) - dy * (cx - px_[i]);
        if (e < 0 || (e == 0 && !(dy < 0 || (dy == 0 && dx > 0))))
            return 0;
    }
    return 1;
}

static int compare_counts(const char *what, int (*want)(int x, int y, void *ctx), void *ctx)
{
    int bad = 0;
    for (int y = 0; y < PW; y++)
        for (int x = 0; x < PW; x++) {
            const int w = want(x, y, ctx);
            if (cnt[y][x] != w) {
                if (bad++ < 3)
                    printf("  %s: pixel (%d, %d) covered %u times, want %d\n", what, x, y, cnt[y][x], w);
            }
        }
    return bad;
}

struct boxc { int32_t x0, y0, x1, y1; };
static int want_box(int x, int y, void *ctx)
{
    const struct boxc *b = (const struct boxc *)ctx;
    return box_has(b->x0, b->y0, b->x1, b->y1, x, y);
}
struct polyc { int32_t x[16], y[16]; int n; };
static int want_poly(int x, int y, void *ctx)
{
    const struct polyc *p = (const struct polyc *)ctx;
    return poly_has(p->x, p->y, p->n, x, y);
}

/* a triangle given in a random one of its six vertex orders */
static void count_any_order(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t cx, int32_t cy)
{
    int32_t x[3] = {ax, bx, cx}, y[3] = {ay, by, cy};
    const int r = rndi(0, 5), a = r % 3, rev = r / 3;
    const int i0 = a, i1 = rev ? (a + 2) % 3 : (a + 1) % 3, i2 = rev ? (a + 1) % 3 : (a + 2) % 3;
    count_tri(x[i0], y[i0], x[i1], y[i1], x[i2], y[i2]);
}

/* a quarter of a sine, 16384 sin(i pi / 128): only to place a polygon's points on a circle */
static const int16_t s64[65] = {0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370,
    4756, 5139, 5520, 5897, 6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765, 9102, 9434, 9760, 10080,
    10394, 10702, 11003, 11297, 11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395, 13623, 13842,
    14053, 14256, 14449, 14635, 14811, 14978, 15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986,
    16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379, 16384};

static void properties(void)
{
    int fails = 0;
    /* squares split along either diagonal, corners anywhere on the 1/16 grid */
    for (int k = 0; k < 400; k++) {
        struct boxc b;
        b.x0 = rndi(0, 16 * 40);
        b.y0 = rndi(0, 16 * 40);
        b.x1 = b.x0 + rndi(1, 16 * 50);
        b.y1 = b.y0 + rndi(1, 16 * 50);
        memset(cnt, 0, sizeof cnt);
        if (k & 1) {
            count_any_order(b.x0, b.y0, b.x1, b.y0, b.x1, b.y1);
            count_any_order(b.x0, b.y0, b.x1, b.y1, b.x0, b.y1);
        } else {
            count_any_order(b.x0, b.y0, b.x1, b.y0, b.x0, b.y1);
            count_any_order(b.x1, b.y0, b.x1, b.y1, b.x0, b.y1);
        }
        fails += compare_counts("square", want_box, &b) != 0;
    }
    CHECK(fails == 0, "squares split in two: %d of 400 not covered exactly once", fails);
    /* fans around a point inside a convex polygon (a circle's points on the 1/16 grid) */
    fails = 0;
    for (int k = 0; k < 300; k++) {
        struct polyc p;
        p.n = rndi(3, 16);
        const int cx = rndi(16 * 30, 16 * 66), cy = rndi(16 * 30, 16 * 66), r = rndi(16 * 3, 16 * 28);
        const int phase = rndi(0, 1023);
        for (int i = 0; i < p.n; i++) {     /* clockwise on the screen: the angle grows, y down */
            const int a = (phase + i * 1024 / p.n) & 1023, q = a >> 8, o = a & 255;
            const int i4 = o >> 2;          /* 0..63 within the quarter */
            int sn = q == 0 ? s64[i4] : q == 1 ? s64[64 - i4] : q == 2 ? -s64[i4] : -s64[64 - i4];
            int cs = q == 0 ? s64[64 - i4] : q == 1 ? -s64[i4] : q == 2 ? -s64[64 - i4] : s64[i4];
            p.x[i] = cx + (r * cs) / 16384;
            p.y[i] = cy + (r * sn) / 16384;
        }
        /* drop repeated points and keep it convex and clockwise (a circle's points are) */
        int m = 0;
        for (int i = 0; i < p.n; i++)
            if (m == 0 || p.x[i] != p.x[m - 1] || p.y[i] != p.y[m - 1]) {
                p.x[m] = p.x[i];
                p.y[m] = p.y[i];
                m++;
            }
        while (m > 1 && p.x[m - 1] == p.x[0] && p.y[m - 1] == p.y[0])
            m--;
        p.n = m;
        int convex = m >= 3;
        for (int i = 0; i < m && convex; i++) {
            const int j = (i + 1) % m, l = (i + 2) % m;
            const int64_t e = (int64_t)(p.x[j] - p.x[i]) * (p.y[l] - p.y[i]) - (int64_t)(p.y[j] - p.y[i]) * (p.x[l] - p.x[i]);
            convex = e > 0;
        }
        if (!convex)
            continue;
        const int32_t fx = cx + rndi(-8, 8), fy = cy + rndi(-8, 8);     /* the fan's centre, near the middle */
        memset(cnt, 0, sizeof cnt);
        for (int i = 0; i < m; i++)
            count_any_order(fx, fy, p.x[i], p.y[i], p.x[(i + 1) % m], p.y[(i + 1) % m]);
        fails += compare_counts("fan", want_poly, &p) != 0;
    }
    CHECK(fails == 0, "fans around a point: %d not covered exactly once", fails);
    /* jittered grids: (n + 1)^2 points over a box, the inner ones moved by up to a sixth of a cell
     * (so every cell stays a convex quadrilateral and both of its diagonals cut it into two
     * triangles of one winding), the points of the box's sides along their side, each cell cut
     * along a random diagonal, each triangle in a random order: the box exactly once */
    fails = 0;
    for (int k = 0; k < 120; k++) {
        const int n = rndi(2, 9), cell = rndi(16 * 3, 16 * 9);
        const int32_t ox = rndi(0, 16 * 4), oy = rndi(0, 16 * 4);
        int32_t gxp[10][10], gyp[10][10];
        for (int j = 0; j <= n; j++)
            for (int i = 0; i <= n; i++) {
                const int side_x = i > 0 && i < n, side_y = j > 0 && j < n;    /* may move in x, in y */
                gxp[j][i] = ox + i * cell + (side_x ? rndi(-cell / 6, cell / 6) : 0);
                gyp[j][i] = oy + j * cell + (side_y ? rndi(-cell / 6, cell / 6) : 0);
            }
        memset(cnt, 0, sizeof cnt);
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++) {
                const int32_t ax = gxp[j][i], ay = gyp[j][i], bx = gxp[j][i + 1], by = gyp[j][i + 1];
                const int32_t cx_ = gxp[j + 1][i + 1], cy_ = gyp[j + 1][i + 1], dx = gxp[j + 1][i], dy = gyp[j + 1][i];
                if (rnd32() & 1) {
                    count_any_order(ax, ay, bx, by, cx_, cy_);
                    count_any_order(ax, ay, cx_, cy_, dx, dy);
                } else {
                    count_any_order(ax, ay, bx, by, dx, dy);
                    count_any_order(bx, by, cx_, cy_, dx, dy);
                }
            }
        struct boxc b = {ox, oy, ox + n * cell, oy + n * cell};
        fails += compare_counts("grid", want_box, &b) != 0;
    }
    CHECK(fails == 0, "jittered triangulated grids: %d not covered exactly once", fails);
    /* random triangles: the same pixels in all six orders */
    fails = 0;
    for (int k = 0; k < 300; k++) {
        int32_t x[3], y[3];
        for (int i = 0; i < 3; i++) {
            x[i] = rndi(-16 * 20, 16 * 110);
            y[i] = rndi(-16 * 20, 16 * 110);
        }
        if (k % 5 == 0)
            y[2] = y[1];                    /* a horizontal edge now and then */
        if (k % 7 == 0)
            x[2] = x[0];                    /* a vertical one */
        static uint8_t ref[PW][PW];
        for (int o = 0; o < 6; o++) {
            const int a = o % 3, rev = o / 3;
            const int i0 = a, i1 = rev ? (a + 2) % 3 : (a + 1) % 3, i2 = rev ? (a + 1) % 3 : (a + 2) % 3;
            memset(cnt, 0, sizeof cnt);
            count_tri(x[i0], y[i0], x[i1], y[i1], x[i2], y[i2]);
            if (o == 0)
                memcpy(ref, cnt, sizeof cnt);
            else if (memcmp(ref, cnt, sizeof cnt))
                fails++;
        }
    }
    CHECK(fails == 0, "random triangles: %d vertex orders covered other pixels", fails);
    /* random Gouraud triangles with depth over the whole 12.4 range and every value: never a
     * clamp that changes a value, never a value wider than the RTL keeps (docs/GPU.md 16.4) */
    struct gpu_engine g;
    memset(vram, 0, sizeof vram);
    gpu_engine_init(&g, vram);
    uint32_t t[] = {hdr(GPU_OP_SET_TARGET, 0), 0, 320, wh(320, 240), hdr(GPU_OP_SET_ZBUF, 0), 153600, 640};
    cmd(&g, t, 7);
    for (int k = 0; k < 3000; k++) {
        uint32_t w[7] = {hdr(GPU_OP_TRIANGLE, GPU_TRI_GOURAUD | GPU_TRI_ZTEST | GPU_TRI_ZWRITE | (rnd32() & 0x4FF))};
        const int big = k % 3 == 0, thin = k % 3 == 1;
        for (int i = 0; i < 3; i++) {
            int x = big ? rndi(-32768, 32767) : rndi(-400, 16 * 330), y = big ? rndi(-32768, 32767) : rndi(-400, 16 * 250);
            if (thin && i == 2) {           /* slivers: the third vertex next to the line of the first two */
                const int ax = (int16_t)(w[1] & 0xFFFF), ay = (int16_t)(w[1] >> 16);
                const int bx = (int16_t)(w[3] & 0xFFFF), by = (int16_t)(w[3] >> 16), t16 = rndi(-8, 24);
                x = ax + (bx - ax) * t16 / 16 + rndi(-2, 2);
                y = ay + (by - ay) * t16 / 16 + rndi(-2, 2);
            }
            w[1 + 2 * i] = xy(x, y);
            w[2 + 2 * i] = (rnd32() % 4 == 0 ? (rnd32() & 1 ? 0xFFFFu : 0u) : rnd32() & 0xFFFFu) |
                           ((rnd32() % 4 == 0 ? (rnd32() & 1 ? 0xFFu : 0u) : rnd32() & 0xFFu) << 16);
        }
        cmd(&g, w, 7);
        if (k % 64 == 0) {
            uint32_t c[] = {hdr(GPU_OP_SET_TARGET, 0), 153600, 640, wh(640, 240), hdr(GPU_OP_FILL_RECT, 0xFF), xy(0, 0),
                            wh(640, 240), hdr(GPU_OP_SET_TARGET, 0), 0, 320, wh(320, 240), hdr(GPU_OP_SET_ZBUF, 0), 153600, 640};
            cmd(&g, c, 14);
        }
    }
    CHECK(g.tri_pixels > 1000000 && g.tri_clamped == 0 && g.tri_wide == 0 && !g.sticky,
          "3000 random triangles: %llu pixels, %llu values clamped, %llu wide, sticky %x", g.tri_pixels, g.tri_clamped,
          g.tri_wide, g.sticky);
    printf("properties: %llu random triangles drawn (%llu culled, %llu of zero area), %llu pixels, %llu failed the "
           "depth test\n", g.tri_drawn, g.tri_culled, g.tri_zero, g.tri_pixels, g.tri_zfail);
}

int main(void)
{
    display();
    engine();
    triangles();
    properties();
    if (failures) {
        printf("test_gpu_model: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("test_gpu_model: PASS (%d checks against docs/GPU.md: display path, CRCs, every command, TRIANGLE's "
           "rules and properties)\n", checks);
    return 0;
}
