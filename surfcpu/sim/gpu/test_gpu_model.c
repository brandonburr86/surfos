/*
SurfGPU reference model self-test
--------------------
File: test_gpu_model.c Date: 10/18/26 (SurfGPU)
--------------------
The C model (sim/models/gpu_model.c) against expectations written by hand from docs/GPU.md,
never from the RTL: so that when the RTL and the model agree, they agree with the document.
The frame CRCs below were computed independently from GPU.md's definitions with Python's zlib
(the test pattern, GPAL's RGB332, the graphics address rule with doubling and wrapping); the
engine's results are traced by hand: pixels, clips, Bresenham's points, overlapping copies,
the bitmap's bit order, the target's refusals, fences and errors.
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
    uint32_t n2[] = {hdr(0x0A, 0), hdr(GPU_OP_FENCE, 0), 0x99};
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

int main(void)
{
    display();
    engine();
    if (failures) {
        printf("test_gpu_model: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("test_gpu_model: PASS (%d checks against docs/GPU.md: display path, CRCs, every command)\n", checks);
    return 0;
}
