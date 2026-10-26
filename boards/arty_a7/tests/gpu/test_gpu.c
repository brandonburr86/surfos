/*
SurfOS host test: SurfGPU's driver and the gfx command against the C model
--------------------
File: test_gpu.c    Date: 10/25/26 (SurfGPU G3)
--------------------
boards/arty_a7/gpu.c and gfx.c built for the host, on the stand-in of gpustub.c, whose engine is
sim/models/gpu_model.c. What each gfx drawing command leaves in VRAM must be the model's picture
of the same scene: a second model fed the command words GPU.md section 5 gives for it (written
here from the document, not taken from the driver), or the pixels themselves where GPU.md says
which (an outline, a glyph, one pixel). Then the driver's rules: the FIFO_FREE rule (a FIFO that
stays full, a stopped engine), the sticky errors said in words, cleared, ABORTed and the target
and clip sent again, the fence waits (interrupt and sleeps), two claimants, the frame latch rule
of a display change, FRAME_CRC of a still picture, the palettes, the demo (never drawing into
the page on the screen; Ctrl+C; the display, palette, target and VRAM back as they were), the
bench, and every usage error refused before a word is sent. Every line printed, in every test,
is measured: none may pass the console's 80 columns (t_columns has each message at its widest).

  make -C surfcpu test-gpuos-host     (seeds 1 7 123; GPU_TEST_VERBOSE=1 prints what gfx says)
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "surfcpu/soc.h"
#include "harness.h"
#include "gpustub.h"
#include "../../../../shell/shell.h"
#include "gpucube.h"
#include "gfxcube_expect.h"             /* gfx cube's table, made at build time (boards/arty_a7/gen/) */
#include "gpuscenes_expect.h"           /* gpudiag's (build/fw/gen/): its first 16 cube frames to compare */

extern const uint8_t font8x16[256][16];

#define FEAT (SURF_SYSINFO_FEATURES_UART_MASK | SURF_SYSINFO_FEATURES_TIMER_MASK | SURF_SYSINFO_FEATURES_IRQ_MASK | \
              SURF_SYSINFO_FEATURES_VIDEO_MASK | SURF_SYSINFO_FEATURES_GPU_MASK)

static struct gpu G;

static struct stub *fresh(int irq) {
    struct stub *s = calloc(1, sizeof(*s));
    stub_init(s, irq);
    check(gpu_probe(&G, &s->port, FEAT) == GPU_OK && G.present, "probe: the stand-in is a GPU1");
    return s;
}

/* a GPU2 (G4's engine): SET_ZBUF, TRIANGLE, FLIP too */
static struct stub *fresh2(int irq) {
    struct stub *s = calloc(1, sizeof(*s));
    stub_init(s, irq);
    s->id = GPUDRV_ID_GPU2;
    check(gpu_probe(&G, &s->port, FEAT) == GPU_OK && G.present && G.gpu2, "probe: the stand-in is a GPU2");
    return s;
}

/* every test ends here: what it printed was measured line by line (GPU.md 15.2's 80 columns) */
static void done(struct stub *s) {
    const char *wide;
    unsigned n = stub_too_wide(&wide);
    if(n) printf("    (%u lines past 80 columns, the first: \"%s\")\n", n, wide);
    check(!n, "every line printed fits the console's 80 columns");
    stub_free(s);
    free(s);
}

static int gfx(struct stub *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int gfx(struct stub *s, const char *fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return stub_gfx(s, &G, line);
}

static int said(struct stub *s, const char *text) { return strstr(s->out, text) != NULL; }

static uint32_t p_status(struct stub *s) { return s->port.rd(s, SURF_GPU_STATUS); }

/**** the reference: a second model fed GPU.md's words ****/

struct ref {
    struct gpu_engine e;
    uint8_t vram[GPU_VRAM_BYTES];
};

static struct ref *ref_of(struct stub *s) {
    struct ref *r = malloc(sizeof(*r));
    memcpy(r->vram, s->vram, sizeof(r->vram));
    gpu_engine_init(&r->e, r->vram);
    r->e.base = s->eng.base;                    /* the target and clip the stand-in's engine has */
    r->e.stride = s->eng.stride;
    r->e.tw = s->eng.tw;
    r->e.th = s->eng.th;
    r->e.cx0 = s->eng.cx0;
    r->e.cy0 = s->eng.cy0;
    r->e.cx1 = s->eng.cx1;
    r->e.cy1 = s->eng.cy1;
    r->e.zvalid = s->eng.zvalid;                /* GPU2: and its depth buffer */
    r->e.zbase = s->eng.zbase;
    r->e.zstride = s->eng.zstride;
    return r;
}

static void w(struct ref *r, uint32_t v) { gpu_engine_word(&r->e, v); }

static void ref_text(struct ref *r, int x, int y, int fg, int bg, const uint8_t *cells, unsigned n) {
    uint32_t wd = 8 * n, nbits = wd * 16, k, b;
    w(r, GPU_HDR(GPU_OP_EXPAND, (unsigned)fg | (bg < 0 ? GPU_EXPAND_TRANSP : (unsigned)bg << 8)));
    w(r, GPU_XY(x, y));
    w(r, GPU_WH(wd, 16));
    for(k = 0; k < (nbits + 31) / 32; k++) {            /* the bitmap in raster order, the first pixel in bit 31 */
        uint32_t word = 0;
        for(b = 0; b < 32 && k * 32 + b < nbits; b++) {
            uint32_t p = k * 32 + b, px = p % wd, py = p / wd;
            word |= (uint32_t)((font8x16[cells[px / 8]][py] >> (7 - px % 8)) & 1) << (31 - b);
        }
        w(r, word);
    }
}

static int same(struct stub *s, struct ref *r) {
    stub_drain(s, 3000000);
    return !memcmp(s->vram, r->vram, GPU_VRAM_BYTES);
}

static uint8_t px(struct stub *s, int x, int y) {
    return s->vram[s->eng.base + (uint32_t)y * s->eng.stride + (uint32_t)x];
}

/**** finding the GPU ****/

static void t_probe(unsigned seed) {
    struct stub *s = calloc(1, sizeof(*s));
    unsigned a0;
    (void)seed;
    stub_init(s, 1);
    check(gpu_probe(&G, &s->port, FEAT & ~SURF_SYSINFO_FEATURES_GPU_MASK) == GPU_EABSENT && !G.present &&
          strstr(G.why, "FEATURES has no GPU"), "probe: FEATURES without the GPU is no GPU, said why");
    check(s->fifo_free_reads == 0 && s->cmd_writes == 0, "probe: without FEATURES.GPU the gpu window is not touched");
    check(gfx(s, "gfx") == CMD_FAIL && said(s, "gfx: no GPU: sysinfo.FEATURES has no GPU"), "gfx without a GPU says so");
    check(gfx(s, "gfx fill 0 0 1 1 1") == CMD_FAIL && said(s, "no GPU"), "gfx fill without a GPU fails, saying so");
    check(gfx(s, "gfx help") == CMD_OK && said(s, "gfx demo [seconds]"), "gfx help works without a GPU");
    check(gpu_probe(&G, &s->port, FEAT & ~SURF_SYSINFO_FEATURES_VIDEO_MASK) == GPU_EABSENT, "probe: no video block, no GPU");
    s->id = 0x31555048u;
    check(gpu_probe(&G, &s->port, FEAT) == GPU_EABSENT && strstr(G.why, "gpu.ID"), "probe: another gpu.ID is no GPU");
    check(gfx(s, "gfx") == CMD_FAIL && said(s, "gfx: no GPU: its gpu.ID reads 0x31555048, neither GPU1 nor GPU2"),
          "gfx names the ID it found");
    s->id = GPUDRV_ID_GPU1;
    s->info = 300u << 16;
    check(gpu_probe(&G, &s->port, FEAT) == GPU_EABSENT && gfx(s, "gfx") == CMD_FAIL &&
          said(s, "gfx: no GPU: gpu.INFO reads 0x012c0000: no FIFO, or not 300 KiB of VRAM"),
          "probe: an INFO without a FIFO is no GPU, with INFO's value");
    s->info = 64u | 300u << 16;
    s->id = GPUDRV_ID_GPU2;
    check(gpu_probe(&G, &s->port, FEAT) == GPU_OK && G.present && !strcmp(gpu_id_name(G.id), "GPU2"),
          "probe: GPU2 (G4's superset) is a GPU");
    check(gfx(s, "gfx fill 0 0 4 4 9") == CMD_OK && s->vram[0] == 9 && s->vram[3 * 640 + 3] == 9 &&
          gfx(s, "gfx") == CMD_OK && said(s, "SurfGPU GPU2 (gpu.ID 0x32555047)"), "  and gfx draws on it with GPU1's commands");
    s->id = GPUDRV_ID_GPU1;
    a0 = s->aborts;
    s->abort_at = s->fence_read_at = 0;
    check(gpu_probe(&G, &s->port, FEAT) == GPU_OK && G.fifo_depth == 64 && G.vram_bytes == 307200 && G.irq,
          "probe: GPU1, a 64-word FIFO, 300 KiB, fences on the interrupt");
    check(s->ie == SURF_GPU_IE_FENCE_MASK, "probe: IE.FENCE set (and IE.ERR not)");
    check(s->eng.base == 0 && s->eng.stride == 640 && s->eng.tw == 640 && s->eng.th == 480 && s->eng.cx1 == 640 &&
          s->eng.cy1 == 480 && G.t_w == 640 && G.c_y1 == 480, "probe: the engine at the reset target and clip, as the driver knows them");
    check(s->aborts == a0 + 2 && !(p_status(s) & 0x1E), "probe: two ABORTs (before FENCE is read, the reset), the sticky bits clear");
    check(s->abort_at && s->fence_read_at && s->abort_at < s->fence_read_at,
          "probe: ABORT before FENCE is read (a FENCE a warm restart left queued cannot complete after it)");
    stub_init(s, 0);
    check(gpu_probe(&G, &s->port, FEAT) == GPU_OK && !G.irq && s->ie == 0,
          "probe: a port without the interrupt: fences with sleeps, IE 0");
    check(gfx(s, "gfx") == CMD_OK && said(s, "SurfGPU GPU1 (gpu.ID 0x31555047)") && said(s, "interrupts: none (no FEATURES.IRQ); fences waited for with sleeps\n"),
          "gfx says what it found");
    done(s);
}
TEST(t_probe)

/**** the drawing commands ****/

static void t_commands(unsigned seed) {
    struct stub *s = fresh(1);
    struct ref *r;
    static const uint8_t hello[] = "Hello, SurfGPU";
    uint8_t cells[1];
    int x, y, ok;
    (void)seed;
    check(gfx(s, "gfx mode gfx 1") == CMD_OK && said(s, "640x480 at VRAM 0, stride 640"), "gfx mode gfx 1");
    r = ref_of(s);
    check(gfx(s, "gfx clear 0x25") == CMD_OK && !s->nout, "gfx clear 0x25: done, nothing said");
    w(r, GPU_HDR(GPU_OP_FILL_RECT, 0x25));
    w(r, GPU_XY(0, 0));
    w(r, GPU_WH(640, 480));
    check(same(s, r) && px(s, 0, 0) == 0x25 && px(s, 639, 479) == 0x25, "gfx clear: the whole target, as FILL_RECT");
    check(gfx(s, "gfx fill 10 20 30 40 200") == CMD_OK, "gfx fill");
    w(r, GPU_HDR(GPU_OP_FILL_RECT, 200));
    w(r, GPU_XY(10, 20));
    w(r, GPU_WH(30, 40));
    check(same(s, r) && px(s, 10, 20) == 200 && px(s, 39, 59) == 200 && px(s, 40, 59) == 0x25,
          "gfx fill 10 20 30 40 200: FILL_RECT, [10, 40) x [20, 60)");
    check(gfx(s, "gfx fill -5 -5 10 10 7") == CMD_OK && px(s, 0, 0) == 7 && px(s, 4, 4) == 7 && px(s, 5, 5) == 0x25,
          "gfx fill from (-5, -5): the clip cuts it");
    w(r, GPU_HDR(GPU_OP_FILL_RECT, 7));
    w(r, GPU_XY(-5, -5));
    w(r, GPU_WH(10, 10));
    check(same(s, r), "gfx fill clipped: as the model");
    check(gfx(s, "gfx line 0 479 639 0 0xff") == CMD_OK, "gfx line");
    w(r, GPU_HDR(GPU_OP_LINE, 0xFF));
    w(r, GPU_XY(0, 479));
    w(r, GPU_XY(639, 0));
    check(same(s, r) && px(s, 0, 479) == 0xFF && px(s, 639, 0) == 0xFF, "gfx line: LINE, both ends drawn");
    check(gfx(s, "gfx copy 0 0 100 100 50 60") == CMD_OK, "gfx copy");
    w(r, GPU_HDR(GPU_OP_COPY_RECT, 0));
    w(r, GPU_XY(0, 0));
    w(r, GPU_XY(100, 100));
    w(r, GPU_WH(50, 60));
    check(same(s, r), "gfx copy 0 0 100 100 50 60: COPY_RECT");
    check(gfx(s, "gfx copy 5 5 8 9 60 60 0x25") == CMD_OK, "gfx copy with a key, overlapping");
    w(r, GPU_HDR(GPU_OP_COPY_RECT, GPU_COPY_KEY | 0x25));
    w(r, GPU_XY(5, 5));
    w(r, GPU_XY(8, 9));
    w(r, GPU_WH(60, 60));
    check(same(s, r), "gfx copy ... 0x25: COPY_RECT with KEY 0x25 (overlapping: as if read first)");
    check(gfx(s, "gfx text 300 200 0xfc 0x03 Hello, SurfGPU") == CMD_OK, "gfx text opaque");
    ref_text(r, 300, 200, 0xFC, 0x03, hello, 14);
    check(same(s, r), "gfx text: EXPAND of the console font, 112x16, row after row");
    ok = 1;
    for(y = 0; y < 16; y++)
        for(x = 0; x < 8 * 14; x++)
            ok &= px(s, 300 + x, 200 + y) == (((font8x16[hello[x / 8]][y] >> (7 - x % 8)) & 1) ? 0xFC : 0x03);
    check(ok, "gfx text: every pixel the font's (fg where a bit is set, bg elsewhere)");
    check(gfx(s, "gfx text -4 470 9 - a b") == CMD_OK, "gfx text transparent, clipped");
    ref_text(r, -4, 470, 9, -1, (const uint8_t *)"a b", 3);
    check(same(s, r), "gfx text -: TRANSPARENT, words joined by one space, cut by the clip");
    check(gfx(s, "gfx text 0 0 1 2 \xE2\x89\x8B") == CMD_OK, "gfx text with UTF-8");
    cells[0] = 0xF7;                                    /* ≋ in code page 437 (driver/cp437.c) */
    ref_text(r, 0, 0, 1, 2, cells, 1);
    check(same(s, r), "gfx text: UTF-8 as the console decodes it (U+224B is cell 0xF7)");
    /* an outline: exactly the edge pixels of [50, 70) x [300, 310) */
    check(gfx(s, "gfx fill 40 290 40 30 0") == CMD_OK && gfx(s, "gfx rect 50 300 20 10 0x77") == CMD_OK, "gfx rect");
    ok = 1;
    for(y = 290; y < 320; y++)
        for(x = 40; x < 80; x++) {
            int edge = (x == 50 || x == 69) ? (y >= 300 && y <= 309) : (y == 300 || y == 309) ? (x >= 50 && x <= 69) : 0;
            ok &= px(s, x, y) == (edge ? 0x77 : 0);
        }
    check(ok, "gfx rect 50 300 20 10: the four edges and nothing else");
    check(gfx(s, "gfx rect 600 470 100 100 5") == CMD_OK && px(s, 600, 470) == 5 && px(s, 600, 479) == 5 &&
          px(s, 639, 470) == 5 && px(s, 601, 471) != 5, "gfx rect past the edges: clipped, nothing wraps");
    check(gfx(s, "gfx rect 9 9 0 5 5") == CMD_OK, "gfx rect of width 0 draws nothing");
    check(gfx(s, "gfx put 3 4 99") == CMD_OK && px(s, 3, 4) == 99, "gfx put 3 4 99: one byte through the window");
    check(gfx(s, "gfx get 3 4") == CMD_OK && said(s, "(3, 4) = 99 (0x63), VRAM byte 2563 (0x30000a03)"), "gfx get 3 4");
    check(gfx(s, "gfx get 639 479") == CMD_OK && said(s, "VRAM byte 307199"), "gfx get at the last pixel");
    /* at scale 2 the target is the 320x240 picture */
    check(gfx(s, "gfx mode gfx 2") == CMD_OK && G.t_w == 320 && G.t_stride == 320 && s->eng.tw == 320 && s->eng.stride == 320,
          "gfx mode gfx 2: the target 320x240, stride 320");
    free(r);
    r = ref_of(s);
    check(gfx(s, "gfx clear 1") == CMD_OK && gfx(s, "gfx fill 310 230 20 20 2") == CMD_OK, "gfx clear and fill at 320x240");
    w(r, GPU_HDR(GPU_OP_FILL_RECT, 1));
    w(r, GPU_XY(0, 0));
    w(r, GPU_WH(320, 240));
    w(r, GPU_HDR(GPU_OP_FILL_RECT, 2));
    w(r, GPU_XY(310, 230));
    w(r, GPU_WH(20, 20));
    check(same(s, r) && s->vram[76800] == 0x25 && s->vram[76799] == 2 && s->vram[76480] == 1,
          "at 320x240: 76,800 bytes drawn, the rest untouched");
    check(gfx(s, "gfx put 319 239 4") == CMD_OK && s->vram[76799] == 4, "gfx put at the last pixel of 320x240");
    check(s->writes_full == 0 && s->writes_blind == 0 && s->bad_vram == 0, "no CMD word into a full FIFO or without room said");
    free(r);
    done(s);
}
TEST(t_commands)

/* random commands with random arguments, at both scales, each against the reference */
static void t_random(unsigned seed) {
    struct stub *s = fresh(1);
    struct ref *r;
    int i, scale = 1 + (int)(seed & 1), bad = 0;
    char words[200];
    gfx(s, "gfx mode gfx %d", scale);
    r = ref_of(s);
    for(i = 0; i < 120; i++) {
        int k = (int)(rnd() % 6), tw = (int)G.t_w, th = (int)G.t_h;
        int x = (int)(rnd() % (unsigned)(tw + 100)) - 50, y = (int)(rnd() % (unsigned)(th + 100)) - 50;
        int x1 = (int)(rnd() % (unsigned)(tw + 100)) - 50, y1 = (int)(rnd() % (unsigned)(th + 100)) - 50;
        unsigned ww = rnd() % 120, hh = rnd() % 90, c = rnd() & 0xFF;
        if(rnd() % 10 == 0) { ww = rnd() % 2000; hh = rnd() % 2000; }
        switch(k) {
        case 0:
            gfx(s, "gfx fill %d %d %u %u %u", x, y, ww, hh, c);
            w(r, GPU_HDR(GPU_OP_FILL_RECT, c)); w(r, GPU_XY(x, y)); w(r, GPU_WH(ww, hh));
            break;
        case 1:
            gfx(s, "gfx line %d %d %d %d %u", x, y, x1, y1, c);
            w(r, GPU_HDR(GPU_OP_LINE, c)); w(r, GPU_XY(x, y)); w(r, GPU_XY(x1, y1));
            break;
        case 2:
            if(rnd() & 1) {
                gfx(s, "gfx copy %d %d %d %d %u %u %u", x, y, x1, y1, ww, hh, c);
                w(r, GPU_HDR(GPU_OP_COPY_RECT, GPU_COPY_KEY | c));
            } else {
                gfx(s, "gfx copy %d %d %d %d %u %u", x, y, x1, y1, ww, hh);
                w(r, GPU_HDR(GPU_OP_COPY_RECT, 0));
            }
            w(r, GPU_XY(x, y)); w(r, GPU_XY(x1, y1)); w(r, GPU_WH(ww, hh));
            break;
        case 3: {
            unsigned n = 1 + rnd() % 30, j;
            uint8_t cells[64];
            int bg = (rnd() & 1) ? -1 : (int)(rnd() & 0xFF);
            for(j = 0; j < n; j++) words[j] = cells[j] = (uint8_t)(0x21 + rnd() % 94);
            words[n] = 0;
            if(bg < 0) gfx(s, "gfx text %d %d %u - %s", x, y, c, words);
            else gfx(s, "gfx text %d %d %u %d %s", x, y, c, bg, words);
            ref_text(r, x, y, (int)c, bg, cells, n);
            break;
        }
        case 4:
            if(x >= 0 && y >= 0 && x < tw && y < th) {
                gfx(s, "gfx put %d %d %u", x, y, c);
                r->vram[r->e.base + (uint32_t)y * r->e.stride + (uint32_t)x] = (uint8_t)c;
            }
            break;
        default:
            gfx(s, "gfx clear %u", c);
            w(r, GPU_HDR(GPU_OP_FILL_RECT, c)); w(r, GPU_XY(0, 0)); w(r, GPU_WH(tw, th));
            break;
        }
        if(!same(s, r) && !bad++) printf("    (seed %u, command %d: %s)\n", seed, i, s->out);
    }
    check(!bad, "120 random commands (fill, line, copy with and without a key, text, put, clear): VRAM is the model's");
    check(s->writes_full == 0 && s->writes_blind == 0 && G.errors == 0 && G.timeouts == 0,
          "random commands: the FIFO rule kept, no error, no timeout");
    free(r);
    done(s);
}
TEST(t_random)

/**** GPU2's commands (GPU.md section 16; 10/26/26) ****/

/* a vertex coordinate in 12.4 as gfx tri takes it: pixels with sixteenths in decimal */
static void fx_str(char *b, size_t n, int v16) {
    unsigned a = (unsigned)(v16 < 0 ? -v16 : v16);
    if(a % 16) snprintf(b, n, "%s%u.%04u", v16 < 0 ? "-" : "", a / 16, (a % 16) * 625);
    else snprintf(b, n, "%s%u", v16 < 0 ? "-" : "", a / 16);
}

/* GPU.md 16.2's words of a TRIANGLE: the header, then each vertex's X | Y << 16 (12.4) and Z | C << 16 */
static void ref_tri(struct ref *r, uint32_t hdr_bits, const int *x16, const int *y16, const unsigned *z, const unsigned *c) {
    int i;
    w(r, GPU_HDR(GPU_OP_TRIANGLE, hdr_bits));
    for(i = 0; i < 3; i++) {
        w(r, ((uint32_t)x16[i] & 0xFFFFu) | ((uint32_t)y16[i] & 0xFFFFu) << 16);
        w(r, (z[i] & 0xFFFFu) | (c[i] & 0xFFu) << 16);
    }
}

/* GPU.md 16.6: the depth bytes as a target, FILL_RECT of b, the target back, SET_ZBUF again */
static void ref_zclear(struct ref *r, uint32_t zb, uint32_t zs, uint32_t tb, uint32_t ts, uint32_t tw, uint32_t th, uint8_t b) {
    w(r, GPU_HDR(GPU_OP_SET_TARGET, 0)); w(r, zb); w(r, zs); w(r, GPU_WH(2 * tw, th));
    w(r, GPU_HDR(GPU_OP_FILL_RECT, b)); w(r, GPU_XY(0, 0)); w(r, GPU_WH(2 * tw, th));
    w(r, GPU_HDR(GPU_OP_SET_TARGET, 0)); w(r, tb); w(r, ts); w(r, GPU_WH(tw, th));
    w(r, GPU_HDR(GPU_OP_SET_ZBUF, 0)); w(r, zb); w(r, zs);
}

static int depth_bytes_are(struct stub *s, uint8_t b) {
    unsigned i;
    for(i = 153600; i < GPU_VRAM_BYTES; i++)
        if(s->vram[i] != b) return 0;
    return 1;
}

static void t_gpu2(unsigned seed) {
    static const unsigned c200[3] = { 200, 200, 200 };
    struct stub *s = fresh2(1);
    struct ref *r;
    uint8_t *was = malloc(GPU_VRAM_BYTES);
    int x[3], y[3], i, ok;
    unsigned z[3], c[3], nw0, culled;
    unsigned long long zfail, covered;
    uint64_t t0, vb;
    (void)seed;
    check(gfx(s, "gfx mode gfx 2") == CMD_OK && G.t_w == 320 && G.t_h == 240, "gfx mode gfx 2: the target 320x240 at VRAM 0");
    check(gfx(s, "gfx") == CMD_OK && said(s, "    depth buffer: none\n"), "gfx on a GPU2: no depth buffer yet");
    r = ref_of(s);
    check(gfx(s, "gfx zbuf 153600 640") == CMD_OK && said(s, "depth buffer: VRAM 153600, stride 640, for the target's 320x240") &&
          s->eng.zvalid && s->eng.zbase == 153600 && s->eng.zstride == 640 && G.z_valid,
          "gfx zbuf 153600 640: SET_ZBUF, GPU.md 16.6's layout (the depth after pictures A and B)");
    w(r, GPU_HDR(GPU_OP_SET_ZBUF, 0)); w(r, 153600); w(r, 640);
    check(gfx(s, "gfx") == CMD_OK && said(s, "    depth buffer: VRAM 153600, stride 640\n"), "  and gfx says so");
    check(gfx(s, "gfx zclear") == CMD_OK && !s->nout, "gfx zclear: done, nothing said");
    ref_zclear(r, 153600, 640, 0, 320, 320, 240, 0xFF);
    check(same(s, r) && depth_bytes_are(s, 0xFF) && s->eng.zvalid && s->eng.base == 0 && s->eng.tw == 320 && s->eng.cx1 == 320,
          "  every depth 0xFFFF: FILL_RECT 0xFF over the depth bytes, then the target and the depth buffer again (16.6)");
    check(gfx(s, "gfx clear 0") == CMD_OK, "gfx clear 0");
    w(r, GPU_HDR(GPU_OP_FILL_RECT, 0)); w(r, GPU_XY(0, 0)); w(r, GPU_WH(320, 240));
    /* a flat triangle, clockwise on the screen (16.3: A > 0), drawn, then culled */
    x[0] = GPU_FX(10); y[0] = GPU_FX(10); x[1] = GPU_FX(100); y[1] = GPU_FX(20); x[2] = GPU_FX(50); y[2] = GPU_FX(90);
    z[0] = z[1] = z[2] = 0;
    c[0] = c[1] = c[2] = 77;
    check(gfx(s, "gfx tri 10 10 100 20 50 90 77") == CMD_OK && !s->nout, "gfx tri 10 10 100 20 50 90 77: a flat triangle");
    ref_tri(r, 77, x, y, z, c);
    check(same(s, r) && px(s, 50, 40) == 77 && px(s, 5, 5) == 0 && px(s, 100, 90) == 0, "  TRIANGLE (16.2), as the model draws it");
    memcpy(was, s->vram, GPU_VRAM_BYTES);
    culled = (unsigned)s->eng.tri_culled;
    check(gfx(s, "gfx tri cullcw 10 10 100 20 50 90 200") == CMD_OK && s->eng.tri_culled == culled + 1, "gfx tri cullcw on it: culled");
    ref_tri(r, 200 | GPU_TRI_CULL_CW, x, y, z, c200);
    check(same(s, r) && !memcmp(s->vram, was, GPU_VRAM_BYTES), "  nothing drawn");
    check(gfx(s, "gfx tri cullccw 10 10 100 20 50 90 200") == CMD_OK, "gfx tri cullccw on it: drawn");
    ref_tri(r, 200 | GPU_TRI_CULL_CCW, x, y, z, c200);
    check(same(s, r) && px(s, 50, 40) == 200, "  in colour 200");
    /* two triangles that share an edge through pixel centres (the top-left rule): every pixel of
       their square drawn exactly once (16.3) */
    memset(s->op_px, 0, sizeof(s->op_px));
    check(gfx(s, "gfx tri 20 100 60 100 60 140 1") == CMD_OK && gfx(s, "gfx tri 20 100 60 140 20 140 2") == CMD_OK,
          "a square of 40x40 as two triangles along its diagonal");
    x[0] = GPU_FX(20); y[0] = GPU_FX(100); x[1] = GPU_FX(60); y[1] = GPU_FX(100); x[2] = GPU_FX(60); y[2] = GPU_FX(140);
    c[0] = c[1] = c[2] = 1;
    ref_tri(r, 1, x, y, z, c);
    x[1] = GPU_FX(60); y[1] = GPU_FX(140); x[2] = GPU_FX(20); y[2] = GPU_FX(140);
    c[0] = c[1] = c[2] = 2;
    ref_tri(r, 2, x, y, z, c);
    for(ok = 1, i = 0; i < 1600; i++) ok &= px(s, 20 + i % 40, 100 + i / 40) == 1 || px(s, 20 + i % 40, 100 + i / 40) == 2;
    check(same(s, r) && ok && s->op_px[GPU_OP_TRIANGLE] == 1600 && px(s, 59, 100) == 1 && px(s, 20, 139) == 2,
          "  each of its 1,600 pixels drawn once (no gap, no pixel twice), as the model");
    /* Gouraud with depth, vertices with sixteenths: two triangles crossing in depth */
    check(gfx(s, "gfx tri gouraud,ztest,zwrite 150.5 30.25 1000 32 300.0625 60 60000 63 200 200.75 30000 40") == CMD_OK,
          "gfx tri gouraud,ztest,zwrite with sixteenths: drawn");
    x[0] = 2408; y[0] = 484; x[1] = 4801; y[1] = GPU_FX(60); x[2] = GPU_FX(200); y[2] = 3212;
    z[0] = 1000; z[1] = 60000; z[2] = 30000;
    c[0] = 32; c[1] = 63; c[2] = 40;
    ref_tri(r, 32 | GPU_TRI_GOURAUD | GPU_TRI_ZTEST | GPU_TRI_ZWRITE, x, y, z, c);
    check(same(s, r) && s->vram[153600 + 100 * 640 + 2 * 200] != 0xFF, "  as the model, its depths written (16.4)");
    zfail = s->eng.tri_zfail;
    covered = s->eng.tri_pixels;
    check(gfx(s, "gfx tri gouraud,zle,zwrite 140 200 30000 64 310 190 30000 95 250 20 30000 80") == CMD_OK,
          "gfx tri gouraud,zle,zwrite: a second one at depth 30000, through the first");
    x[0] = GPU_FX(140); y[0] = GPU_FX(200); x[1] = GPU_FX(310); y[1] = GPU_FX(190); x[2] = GPU_FX(250); y[2] = GPU_FX(20);
    z[0] = z[1] = z[2] = 30000;
    c[0] = 64; c[1] = 95; c[2] = 80;
    ref_tri(r, 64 | GPU_TRI_GOURAUD | GPU_TRI_ZTEST | GPU_TRI_ZFUNC_LE | GPU_TRI_ZWRITE, x, y, z, c);
    zfail = s->eng.tri_zfail - zfail;
    covered = s->eng.tri_pixels - covered;
    check(same(s, r) && zfail > 0 && covered > zfail && s->eng.tri_clamped == 0 && s->eng.tri_wide == 0,
          "  as the model: in front of the first in part, behind it (dropped by the depth test) in part");
    /* zclear with another byte, the depth buffer off, another target, FLIP */
    check(gfx(s, "gfx zclear 0x80") == CMD_OK, "gfx zclear 0x80");
    ref_zclear(r, 153600, 640, 0, 320, 320, 240, 0x80);
    check(same(s, r) && depth_bytes_are(s, 0x80), "  every depth 0x8080");
    check(gfx(s, "gfx zbuf off") == CMD_OK && said(s, "depth buffer: none") && !s->eng.zvalid && !G.z_valid &&
          s->eng.base == 0 && s->eng.tw == 320 && s->eng.cx1 == 320, "gfx zbuf off: SET_TARGET again, the depth buffer gone");
    w(r, GPU_HDR(GPU_OP_SET_TARGET, 0)); w(r, 0); w(r, 320); w(r, GPU_WH(320, 240));
    check(gfx(s, "gfx tri ztest 0 0 0 1 10 0 0 1 0 10 0 1") == CMD_FAIL && said(s, "need a depth buffer"),
          "  then ztest is refused (nothing sent), saying why");
    check(gfx(s, "gfx target 76800 320 320 240") == CMD_OK && said(s, "target: VRAM 76800, stride 320, 320x240") &&
          s->eng.base == 76800 && G.t_base == 76800, "gfx target 76800 320 320 240: picture B");
    w(r, GPU_HDR(GPU_OP_SET_TARGET, 0)); w(r, 76800); w(r, 320); w(r, GPU_WH(320, 240));
    check(gfx(s, "gfx clear 0x55") == CMD_OK && gfx(s, "gfx tri 0 0 319 0 0 239 9") == CMD_OK, "  drawn into");
    w(r, GPU_HDR(GPU_OP_FILL_RECT, 0x55)); w(r, GPU_XY(0, 0)); w(r, GPU_WH(320, 240));
    x[0] = 0; y[0] = 0; x[1] = GPU_FX(319); y[1] = 0; x[2] = 0; y[2] = GPU_FX(239);
    z[0] = z[1] = z[2] = 0;
    c[0] = c[1] = c[2] = 9;
    ref_tri(r, 9, x, y, z, c);
    check(same(s, r) && s->vram[76800] == 9 && s->vram[76800 + 239 * 320 + 319] == 0x55, "  as the model, in B");
    nw0 = s->nw;
    t0 = s->now;
    check(gfx(s, "gfx flip") == CMD_OK && said(s, "display: GFX_BASE 76800, written by FLIP at a vertical blanking") &&
          s->reg.base == 76800 && s->nw == nw0 + 1 && s->wlog[nw0].reg == SURF_VIDEO_GFX_BASE,
          "gfx flip: FLIP to the target's base, GFX_BASE written once");
    vb = s->wlog[nw0].t;
    check(vb % STUB_FRAME_US == STUB_VBLANK_US && vb > t0 && vb - t0 <= STUB_FRAME_US + 100 && s->flips_done == 1,
          "  at the next vertical blanking (16.7), the command done once it was written");
    check(gfx(s, "gfx flip 0") == CMD_OK && s->reg.base == 0 && s->flips_done == 2, "gfx flip 0: back to picture A");
    check(s->writes_full == 0 && s->writes_blind == 0 && G.errors == 0 && G.timeouts == 0 && s->gpu2_on_gpu1 == 0,
          "GPU2's commands: the FIFO rule kept, no error, no timeout");
    free(was);
    free(r);
    done(s);
}
TEST(t_gpu2)

/* random triangles with every flag, vertices on the target, past it and over the whole 12.4 range,
   with the depth buffer, depth clears and fills between: each against the reference */
static void t_gpu2_random(unsigned seed) {
    struct stub *s = fresh2(1);
    struct ref *r;
    int i, bad = 0, drawn = 0;
    gfx(s, "gfx mode gfx 2");
    gfx(s, "gfx zbuf 153600 640");
    gfx(s, "gfx zclear");
    r = ref_of(s);
    for(i = 0; i < 120; i++) {
        unsigned k = rnd() % 20;
        if(k == 0) {
            unsigned b = rnd() & 0xFF;
            gfx(s, "gfx zclear %u", b);
            ref_zclear(r, 153600, 640, 0, 320, 320, 240, (uint8_t)b);
        } else if(k == 1) {
            unsigned c = rnd() & 0xFF;
            gfx(s, "gfx clear %u", c);
            w(r, GPU_HDR(GPU_OP_FILL_RECT, c)); w(r, GPU_XY(0, 0)); w(r, GPU_WH(320, 240));
        } else {
            char how[64] = "", xs[3][16], ys[3][16];
            uint32_t bits = 0;
            int x[3], y[3], v;
            unsigned z[3], c[3];
            for(v = 0; v < 3; v++) {
                if(rnd() % 12 == 0) {                   /* anywhere in the 12.4 range */
                    x[v] = (int)(rnd() % 65536) - 32768;
                    y[v] = (int)(rnd() % 65536) - 32768;
                } else {                                /* on the target or a little past it */
                    x[v] = (int)(rnd() % (360 * 16)) - 20 * 16;
                    y[v] = (int)(rnd() % (280 * 16)) - 20 * 16;
                }
                z[v] = rnd() & 0xFFFF;
                c[v] = rnd() & 0xFF;
                fx_str(xs[v], sizeof(xs[v]), x[v]);
                fx_str(ys[v], sizeof(ys[v]), y[v]);
            }
            if(rnd() & 1) bits |= GPU_TRI_GOURAUD;
            switch(rnd() % 3) {
            case 1: bits |= GPU_TRI_ZTEST; break;
            case 2: bits |= GPU_TRI_ZTEST | GPU_TRI_ZFUNC_LE; break;
            }
            if(rnd() & 1) bits |= GPU_TRI_ZWRITE;
            if(rnd() % 10 == 0) bits |= GPU_TRI_CULL_CW;
            if(rnd() % 10 == 0) bits |= GPU_TRI_CULL_CCW;
            if(bits & GPU_TRI_GOURAUD) strcat(how, "gouraud,");
            if(bits & GPU_TRI_ZFUNC_LE) strcat(how, "zle,");
            else if(bits & GPU_TRI_ZTEST) strcat(how, "ztest,");
            if(bits & GPU_TRI_ZWRITE) strcat(how, "zwrite,");
            if(bits & GPU_TRI_CULL_CW) strcat(how, "cullcw,");
            if(bits & GPU_TRI_CULL_CCW) strcat(how, "cullccw,");
            if(how[0]) how[strlen(how) - 1] = ' ';
            if(gfx(s, "gfx tri %s%s %s %u %u %s %s %u %u %s %s %u %u", how, xs[0], ys[0], z[0], c[0], xs[1], ys[1], z[1], c[1],
                   xs[2], ys[2], z[2], c[2]) != CMD_OK && !bad++)
                printf("    (seed %u, triangle %d: %s)\n", seed, i, s->out);
            ref_tri(r, bits | c[0], x, y, z, c);
            drawn++;
        }
        if(!same(s, r) && !bad++) printf("    (seed %u, command %d: %s)\n", seed, i, s->out);
    }
    check(!bad && drawn > 90, "random triangles (every flag, sixteenths, past the edges, the whole 12.4 range) with "
          "depth clears and fills: VRAM and the depth buffer the model's");
    check(s->eng.tri_drawn > 20 && s->eng.tri_zfail > 0 && s->eng.tri_clamped == 0 && s->eng.tri_wide == 0 &&
          s->writes_full == 0 && s->writes_blind == 0 && G.errors == 0 && G.timeouts == 0,
          "  triangles walked and depths that failed among them; the FIFO rule kept, no error");
    free(r);
    done(s);
}
TEST(t_gpu2_random)

/* the driver's rules with GPU2's commands: the FIFO, a stopped engine (the depth buffer sent again
   after the ABORT), a FLIP that never comes (no pixel clock), Ctrl+C while a FLIP waits, an error,
   and a GPU1, on which each says that it needs GPU2 and sends nothing */
static void t_gpu2_rules(unsigned seed) {
    struct stub *s = fresh2(1);
    unsigned i, n0, f0;
    uint64_t t0;
    char line[160];
    int ok;
    (void)seed;
    gfx(s, "gfx mode gfx 2");
    gfx(s, "gfx zbuf 153600 640");
    s->slow = 300;                              /* 12 us a word: the FIFO fills */
    ok = gpu_claim(&G, "gfx tri") == GPU_OK;    /* one submission of 630 words, as gfx cube's frames are long */
    for(i = 0; i < 30 && ok; i++) {
        int x[3] = { (int)i * 16, 300 * 16, 0 }, y[3] = { 0, (int)i * 160, 239 * 16 };
        unsigned z[3] = { 100 * i, 9, 400 }, c[3] = { 1, 2, 3 };
        ok = gpu_tri(&G, 1 | GPU_TRI_GOURAUD | GPU_TRI_ZTEST | GPU_TRI_ZWRITE, x, y, z, c, "gfx tri") == GPU_OK &&
             gpu_zbuf_clear(&G, 0xFF, "gfx tri") == GPU_OK;
    }
    ok = ok && gpu_sync(&G, "gfx tri") == GPU_OK;
    gpu_release(&G);
    check(ok && s->full_seen > 0 && s->writes_full == 0 && s->writes_blind == 0 && !s->overflow && G.errors == 0,
          "a slow engine, 30 triangles and 30 depth clears in one go: no word into a full FIFO or past the room said");
    s->slow = 0;
    /* a stopped engine: said, ABORT, and the target, clip and depth buffer sent again */
    s->stopped = 1;
    t0 = s->now;
    check(gfx(s, "gfx tri ztest 0 0 0 1 100 0 0 1 0 100 0 1") == CMD_FAIL && said(s, "gfx tri: the GPU finished nothing for 2 s"),
          "a stopped engine under gfx tri: said, ABORT, failed");
    check(s->now - t0 >= 2000000 && s->now - t0 < 2400000, "  after 2 seconds, not much more");
    s->stopped = 0;
    stub_drain(s, 1000);
    check(s->eng.zvalid && s->eng.zbase == 153600 && s->eng.zstride == 640 && s->eng.base == 0 && s->eng.tw == 320 &&
          G.z_valid, "  the engine back: its target, clip and depth buffer the driver's (sent again after the ABORT)");
    check(gfx(s, "gfx tri ztest,zwrite 0 0 0 1 100 0 0 1 0 100 0 1") == CMD_OK, "  and a triangle with depth works");
    /* a FLIP that never comes: no pixel clock, no blanking */
    s->frames_stop = 1;
    f0 = s->flips_done;
    t0 = s->now;
    check(gfx(s, "gfx flip 76800") == CMD_FAIL && said(s, "gfx flip: the GPU finished nothing for 2 s: a stopped engine; ABORT"),
          "a FLIP with no blanking to wait for: a stopped engine after 2 s, ABORT, failed");
    snprintf(line, sizeof(line), "  a bounded wait: %llu ms, the FLIP's write dropped by the ABORT",
             (unsigned long long)((s->now - t0) / 1000));
    check(s->now - t0 >= 2000000 && s->now - t0 < 2400000 && s->flips_done == f0 && s->reg.base == 0, line);
    s->intr_at = s->now + 100000;
    t0 = s->now;
    check(gfx(s, "gfx flip 76800") == CMD_FAIL && said(s, "gfx flip: interrupted: the engine stopped (ABORT)") &&
          s->now - t0 < 200000 && s->flips_done == f0 && s->reg.base == 0, "Ctrl+C while a FLIP waits: ABORT, said, at once");
    s->intr_at = 0;
    s->frames_stop = 0;
    check(gfx(s, "gfx flip 76800") == CMD_OK && s->reg.base == 76800, "  with blankings again, the FLIP works");
    /* an error: SET_ZBUF's base arrives unaligned */
    s->corrupt_value_at = s->cmd_writes + 2;
    s->corrupt_value = 153602;
    check(gfx(s, "gfx zbuf 153600 640") == CMD_FAIL && said(s, "BAD_PARAM"), "BAD_PARAM (a refused SET_ZBUF): said in words");
    stub_drain(s, 1000);
    check(!(s->eng.sticky & GPU_ST_BAD_PARAM) && s->eng.zvalid && s->eng.zbase == 153600 && G.z_valid,
          "  cleared, ABORT, and the depth buffer sent again with the target: the engine's is the driver's");
    done(s);
    /* a GPU1 */
    s = fresh(1);
    gfx(s, "gfx mode gfx 2");
    n0 = s->ncmd;
    check(gfx(s, "gfx zbuf 153600 640") == CMD_FAIL && said(s, "gfx zbuf: needs GPU2 (SET_ZBUF, TRIANGLE, FLIP); this engine is GPU1") &&
          gfx(s, "gfx zclear") == CMD_FAIL && said(s, "needs GPU2") && gfx(s, "gfx tri 0 0 9 0 0 9 1") == CMD_FAIL &&
          said(s, "needs GPU2") && gfx(s, "gfx flip") == CMD_FAIL && said(s, "needs GPU2"),
          "on a GPU1, zbuf, zclear, tri and flip say they need GPU2");
    check(s->ncmd == n0 && s->gpu2_on_gpu1 == 0 && !s->locked, "  and send nothing");
    check(gfx(s, "gfx target 76800 320 320 240") == CMD_OK && s->eng.base == 76800, "  gfx target is GPU1's too");
    done(s);
}
TEST(t_gpu2_rules)

/**** the FIFO rule, a stopped engine ****/

static void t_fifo(unsigned seed) {
    struct stub *s = fresh(1);
    char words[260];
    uint64_t t0;
    int i;
    (void)seed;
    gfx(s, "gfx mode gfx 1");
    s->slow = 300;                              /* 12 us a word: the FIFO fills */
    for(i = 0; i < 200; i++) words[i] = (char)('A' + i % 26);
    words[200] = 0;
    check(gfx(s, "gfx text 0 0 1 2 %s", words) == CMD_OK && gfx(s, "gfx fill 0 0 640 480 3") == CMD_OK,
          "a slow engine: 803 words of text, then a fill");
    check(s->full_seen > 0 && s->writes_full == 0 && s->writes_blind == 0 && !s->overflow,
          "FIFO_FREE said 0 at times; no word went into a full FIFO, none beyond the room said: no OVERFLOW");
    s->slow = 0;
    /* the engine stops: the words fit, the fence never comes */
    s->stopped = 1;
    t0 = s->now;
    check(gfx(s, "gfx fill 0 0 4 4 1") == CMD_FAIL && said(s, "gfx fill: the GPU finished nothing for 2 s") &&
          said(s, "a stopped engine; ABORT"), "a stopped engine: said, ABORT, failed");
    check(s->now - t0 >= 2000000 && s->now - t0 < 2400000, "  after 2 seconds (simulated), not much more");
    check(G.timeouts >= 1 && s->aborts >= 2, "  counted, and the engine ABORTed");
    /* a FIFO that stays full: more words than fit */
    t0 = s->now;
    check(gfx(s, "gfx text 0 0 1 2 %s", words) == CMD_FAIL && said(s, "took no command word for 2 s"),
          "a FIFO that stays full: 'took no command word for 2 s'");
    check(s->now - t0 >= 2000000 && s->now - t0 < 2400000 && s->writes_full == 0, "  2 seconds; still no word into a full FIFO");
    s->stopped = 0;
    check(gfx(s, "gfx fill 1 1 2 2 9") == CMD_OK && s->vram[641] == 9, "the engine back: drawing works again");
    check(s->eng.tw == G.t_w && s->eng.th == G.t_h && s->eng.base == G.t_base && s->eng.cx1 == G.c_x1,
          "  the engine's target and clip are the driver's (sent again after the ABORT)");
    /* Ctrl+C while the FIFO is full */
    s->stopped = 1;
    s->intr_at = s->now + 100000;
    t0 = s->now;
    check(gfx(s, "gfx text 0 0 1 2 %s", words) == CMD_FAIL && said(s, "interrupted: the engine stopped (ABORT)") &&
          s->now - t0 < 200000, "Ctrl+C while waiting for room: ABORT, said, at once");
    s->intr_at = 0;
    s->stopped = 0;
    check(gfx(s, "gfx fill 0 0 1 1 1") == CMD_OK, "  and the next command works");
    /* a word lost (OVERFLOW), then the FIFO stays full: the error said and cleared with the stopped
       engine, not left for the next command (the review's finding 7) */
    s->stopped = 1;
    s->drop_word = s->cmd_writes + 1;
    check(gfx(s, "gfx text 0 0 1 2 %s", words) == CMD_FAIL && said(s, "took no command word for 2 s") &&
          said(s, "OVERFLOW: a command word found the FIFO full and was dropped") && !s->overflow,
          "a lost word and a FIFO that stays full: the stopped engine and the OVERFLOW said, OVERFLOW cleared");
    s->stopped = 0;
    check(gfx(s, "gfx fill 0 0 1 1 1") == CMD_OK && !s->nout, "  the next command works and finds no old error");
    /* Ctrl+C where nothing was ABORTed (waiting for frames) says so without an ABORT */
    s->intr_at = s->now + 10000;
    check(gfx(s, "gfx crc") == CMD_FAIL && said(s, "gfx crc: interrupted (Ctrl+C)") && !said(s, "ABORT"),
          "Ctrl+C while gfx crc waits for frames: interrupted, no ABORT claimed");
    s->intr_at = 0;
    done(s);
}
TEST(t_fifo)

/**** errors ****/

static uint32_t p_status_bits(struct stub *s) {
    return s->overflow | (s->eng.sticky & (GPU_ST_BAD_OPCODE | GPU_ST_BAD_PARAM));
}

static void t_errors(unsigned seed) {
    struct stub *s = fresh(1);
    struct ref *r;
    unsigned a0;
    (void)seed;
    gfx(s, "gfx mode gfx 1");
    a0 = s->aborts;
    s->corrupt_word = s->cmd_writes + 1;        /* the fill's header arrives with opcode 0x0F */
    check(gfx(s, "gfx fill 0 0 10 10 5") == CMD_FAIL && said(s, "gfx fill: the GPU reports:\n"
          "      BAD_OPCODE: a command word had an unknown opcode\n"
          "    gfx fill: cleared; the engine ABORTed, its target and clip sent again\n"),
          "BAD_OPCODE: said in words, cleared, ABORT, the target and clip again");
    check(!p_status_bits(s) && s->aborts == a0 + 1 && G.bad_opcodes == 1 && G.errors == 1, "  STATUS clean, one ABORT, counted");
    r = ref_of(s);
    check(gfx(s, "gfx fill 0 0 10 10 5") == CMD_OK, "  the next command works");
    w(r, GPU_HDR(GPU_OP_FILL_RECT, 5)); w(r, GPU_XY(0, 0)); w(r, GPU_WH(10, 10));
    check(same(s, r), "  and draws what it should");
    free(r);
    /* BAD_PARAM: SET_TARGET's base arrives unaligned */
    s->corrupt_value_at = s->cmd_writes + 2;
    s->corrupt_value = 2;
    check(gfx(s, "gfx mode gfx 2") == CMD_FAIL && said(s, "BAD_PARAM: a command's parameters were refused (SET_TARGET)"),
          "BAD_PARAM (a refused SET_TARGET): said in words");
    stub_drain(s, 1000);
    check(!p_status_bits(s) && s->eng.tw == 320 && s->eng.stride == 320 && G.t_w == 320,
          "  cleared, and the target sent again: the engine's is the driver's");
    check(gfx(s, "gfx mode gfx 2") == CMD_OK && s->reg.scale == 2, "  gfx mode gfx 2 again works");
    /* OVERFLOW: a word lost on its way in */
    s->drop_word = s->cmd_writes + 1;
    check(gfx(s, "gfx fill 0 0 10 10 5") == CMD_FAIL && said(s, "OVERFLOW: a command word found the FIFO full and was dropped"),
          "OVERFLOW: said in words");
    check(!p_status_bits(s) && G.overflows == 1, "  cleared, counted");
    /* a misread stream: the header lost, its parameters read as commands */
    s->drop_word = s->cmd_writes + 1;
    gfx(s, "gfx fill 0 0 300 0x2000 7");
    check(!p_status_bits(s) && gfx(s, "gfx fill 0 0 1 1 1") == CMD_OK, "  whatever followed, the next command works");
    check(s->writes_full == 0 && s->writes_blind == 0, "  the driver itself never overflowed the FIFO");
    done(s);
}
TEST(t_errors)

/**** fences: the interrupt, sleeps, the short poll ****/

static void t_fences(unsigned seed) {
    int irq;
    (void)seed;
    for(irq = 1; irq >= 0; irq--) {
        struct stub *s = fresh(irq);
        unsigned waits, sleeps, relax;
        uint64_t t0;
        gfx(s, "gfx mode gfx 1");
        waits = s->irq_waits;
        sleeps = s->sleeps;
        relax = s->relaxes;
        check(gfx(s, "gfx fill 0 0 4 4 1") == CMD_OK && s->irq_waits == waits && s->sleeps == sleeps && s->relaxes >= relax,
              irq ? "a short command: its fence polled, no sleep (interrupt port)" : "a short command: polled (sleep port)");
        t0 = s->now;
        check(gfx(s, "gfx fill 0 0 640 480 1") == CMD_OK && gfx(s, "gfx fill 0 0 640 480 2") == CMD_OK &&
              gfx(s, "gfx fill 0 0 640 480 3") == CMD_OK, "three full-screen fills");
        if(irq) check(s->irq_waits > waits && s->interrupts_sent > 0 && s->sleeps == sleeps,
                      "long commands: the waits sleep on the GPU's interrupt (FENCE with IRQ, IE.FENCE)");
        else check(s->sleeps > sleeps && s->irq_waits == 0 && s->interrupts_sent == 0 && !(s->ie & 1),
                   "long commands without the interrupt: sleeps, IE.FENCE off");
        check(s->now - t0 < 40000, "  done in time (3 x 3.1 ms of engine time, under 40 ms in all)");
        if(irq) {
            /* the re-review's N3: the interrupt must end the waits, not only reach its handler */
            unsigned long w0 = G.irq_waits, k0 = G.irq_wakes;
            check(G.irq_waits == 3 && G.irq_wakes == 3 && gfx(s, "gfx") == CMD_OK &&
                  said(s, "    fence waits that slept on it: 3, woken by it: 3\n"),
                  "  each of those waits woken by the interrupt, counted, and gfx says so");
            s->isr_no_wake = 1;                 /* a handler that runs and wakes nobody */
            t0 = s->now;
            check(gfx(s, "gfx fill 0 0 640 480 4") == CMD_OK && gfx(s, "gfx fill 0 0 640 480 5") == CMD_OK &&
                  s->interrupts_sent > 0 && G.irq_wakes == k0 && G.irq_waits == w0 + 2 && s->now - t0 >= 20000,
                  "an interrupt that wakes nobody: the fences come all the same, each wait ended by its 10 ms");
            check(gfx(s, "gfx") == CMD_OK && said(s, "    fence waits that slept on it: 5, woken by it: 3\n"),
                  "  and gfx shows the waits it did not wake");
            /* a wait of more than one 10 ms step (a FLIP waits up to a frame), woken: one wait, one woken */
            s->isr_no_wake = 0;
            s->slow = 7500;                     /* 300 us a word: a full FIFO is 19 ms of work at the wait */
            w0 = G.irq_waits;
            k0 = G.irq_wakes;
            check(gfx(s, "gfx text 0 0 1 2 ABCDEFGHIJKLMNOPQRSTUVWXYZ") == CMD_OK && G.irq_waits == w0 + 1 && G.irq_wakes == k0 + 1 &&
                  s->irq_waits >= 2, "  a wait of two 10 ms steps that the interrupt ends: one wait, woken");
            s->slow = 0;
            s->isr_no_wake = 0;
        }
        done(s);
    }
}
TEST(t_fences)

/**** two claimants ****/

static void t_claims(unsigned seed) {
    struct stub *s = fresh(1);
    static uint32_t other[3];
    unsigned i, mine0, first_mine;
    uint64_t t0;
    (void)seed;
    gfx(s, "gfx mode gfx 1");
    other[0] = GPU_HDR(GPU_OP_FILL_RECT, 0x44);
    other[1] = GPU_XY(0, 0);
    other[2] = GPU_WH(8, 8);
    s->other_holds = 1;
    s->other_until = s->now + 100000;
    s->other_words = other;
    s->other_n = 3;
    mine0 = s->ncmd;
    t0 = s->now;
    check(gfx(s, "gfx fill 4 4 8 8 0x55") == CMD_OK && said(s, "gfx fill: the GPU is in use: waiting (Ctrl+C gives up)\n"
          "    held by 'Shell 2', pid 9\n"), "a second claimant waits, and says who has it");
    check(s->now - t0 >= 100000 && s->other_done, "  until the first lets go (100 ms)");
    first_mine = s->ncmd;
    for(i = mine0; i < s->ncmd; i++)
        if(s->cmd[i] == GPU_HDR(GPU_OP_FILL_RECT, 0x55)) { first_mine = i; break; }
    check(s->cmd_at_other >= mine0 && first_mine >= s->cmd_at_other + 3, "  its words all after the first claimant's: no interleaving");
    check(s->vram[0] == 0x44 && s->vram[4 * 640 + 4] == 0x55 && s->vram[11 * 640 + 11] == 0x55, "  both drew, in that order");
    check(!s->locked && s->claims >= 2 && !s->claimed_twice && !s->unlocked_free, "  the claim given back once, never twice");
    /* Ctrl+C while waiting */
    s->other_holds = 1;
    s->other_done = 0;
    s->other_n = 0;
    s->other_until = s->now + 500000;
    s->intr_at = s->now + 50000;
    mine0 = s->ncmd;
    t0 = s->now;
    check(gfx(s, "gfx fill 4 4 8 8 0x66") == CMD_FAIL && said(s, "gfx fill: gave up waiting for the GPU") && s->ncmd == mine0 &&
          s->now - t0 < 100000, "Ctrl+C while waiting for the claim: gave up, nothing sent");
    s->intr_at = 0;
    s->other_holds = 0;
    check(gfx(s, "gfx fill 0 0 1 1 1") == CMD_OK && !s->locked, "  then the GPU is free");
    done(s);
}
TEST(t_claims)

/* The re-review's N2: what a run's Ctrl+C did (an ABORT or not) was a flag of the driver's struct,
   cleared by every claim before its trylock and read after the release, so another console's claim
   in between changed what this run said. Here the other console claims while this run's Ctrl+C is
   handled (at its ABORT: the claim fails, this run holds the GPU) and as soon as this run lets go
   (the claim taken; in the second case its own Ctrl+C ABORTs the engine). Now each run's result
   says it. */
static int other_aborts;
static void other_console(struct stub *s, int where) {
    if(where == STUB_AT_ABORT) {
        gpu_claim(&G, "gfx fill");                      /* this run holds it: Ctrl+C gives up at once */
        return;
    }
    if(gpu_claim(&G, "gfx fill") != GPU_OK) return;
    if(other_aborts) {                                  /* its fence never comes: its Ctrl+C ABORTs */
        s->stopped = 1;
        if(gpu_fill(&G, 0, 0, 4, 4, 9, "gfx fill") == GPU_OK) gpu_sync(&G, "gfx fill");
        s->stopped = 0;
    }
    gpu_release(&G);
}

static void t_ctrlc_said(unsigned seed) {
    struct stub *s = fresh(1);
    (void)seed;
    gfx(s, "gfx mode gfx 1");
    s->hook = other_console;
    other_aborts = 0;
    s->stopped = 1;
    s->intr_at = s->now + 100000;
    check(gfx(s, "gfx fill 0 0 4 4 1") == CMD_FAIL && said(s, "gfx fill: interrupted: the engine stopped (ABORT)\n") &&
          !said(s, "gfx fill: interrupted (Ctrl+C)") && s->hooks_run >= 2,
          "Ctrl+C ABORTs a stopped engine while another console claims: this run says it ABORTed");
    s->stopped = 0;
    s->intr_at = 0;
    s->hooks_run = 0;
    other_aborts = 1;
    s->intr_at = s->now + 10000;
    check(gfx(s, "gfx crc") == CMD_FAIL && said(s, "gfx crc: interrupted (Ctrl+C)\n") &&
          !said(s, "gfx crc: interrupted: the engine stopped") && s->hooks_run >= 1 && G.ctrl_c >= 2,
          "Ctrl+C with nothing to ABORT, the next console's Ctrl+C ABORTing: this run says it ABORTed nothing");
    s->intr_at = 0;
    s->hook = NULL;
    check(gfx(s, "gfx fill 0 0 1 1 1") == CMD_OK && !s->locked, "  then the GPU is free and works");
    done(s);
}
TEST(t_ctrlc_said)

/**** the frame latch rule ****/

/* the display writes of the last display change: GFX_BASE, GFX_STRIDE, GFX_SCALE, the mode */
static int latched_together(struct stub *s, unsigned from, uint64_t *when) {
    unsigned i, n = 0;
    uint64_t k = 0, vb = 0;
    int ok = 1;
    for(i = from; i < s->nw; i++) {
        uint64_t t = s->wlog[i].t, f = stub_frame_of(t);
        uint64_t since = t % STUB_FRAME_US;
        if(!n) {
            k = f;
            vb = f * STUB_FRAME_US + STUB_VBLANK_US;
        }
        ok &= f == k && since >= STUB_VBLANK_US && since + 1 <= STUB_LATCH_US && t - vb <= 1300;
        n++;
    }
    *when = vb;
    return ok && n == 4;
}

static void t_latch(unsigned seed) {
    struct stub *s = fresh(1);
    unsigned from;
    uint64_t vb = 0, next;
    (void)seed;
    from = s->nw;
    check(gfx(s, "gfx mode gfx 2") == CMD_OK && G.latched && latched_together(s, from, &vb),
          "gfx mode: GFX_BASE, GFX_STRIDE, GFX_SCALE and the mode written within 1.3 ms after a blanking began");
    check(s->mode_unlocked == 0 && s->text_locks == 0 && s->in_atomic == 0,
          "  the mode set with the console lock held and interrupts off, both given back");
    check(stub_shown(s, stub_frame_of(vb) + 1).scale == 2 && stub_shown(s, stub_frame_of(vb)).scale == 1 &&
          stub_shown(s, stub_frame_of(vb) + 1).mode == 2 && stub_shown(s, stub_frame_of(vb) + 1).stride == 320,
          "  the frame after that latch shows all of it, the frame before none");
    /* the CPU stalled for 2 ms (an interrupt, another task: between two samples, never inside one)
       just before the next blanking begins: the pair across it is not used */
    next = (stub_frame_of(s->now) + 1) * STUB_FRAME_US + STUB_VBLANK_US;
    if(next < s->now + 3000) next += STUB_FRAME_US;
    s->stall_at = next - 10;
    s->stall_us = 2000;
    from = s->nw;
    check(gfx(s, "gfx mode gfx 1") == CMD_OK && !s->stall_at && latched_together(s, from, &vb) && vb > next,
          "a stall across a blanking: the next blanking caught instead");
    /* no frames at all */
    s->frames_stop = 1;
    from = s->nw;
    check(gfx(s, "gfx mode overlay") == CMD_OK && !G.latched && G.latch_misses == 1 &&
          said(s, "no vertical blanking in 250 ms: the registers written at once"),
          "a frame counter that stays: written after 250 ms, and said");
    check(s->reg.mode == 3 && s->nw == from + 4, "  the mode and the registers written all the same");
    done(s);
}
TEST(t_latch)

/**** FRAME_CRC ****/

static void t_crc(unsigned seed) {
    struct stub *s = fresh(1);
    unsigned long got = 0;
    uint64_t t0;
    const char *p;
    struct stub_disp d;
    (void)seed;
    gfx(s, "gfx mode gfx 1");
    gfx(s, "gfx clear 0x49");
    gfx(s, "gfx fill 100 100 200 100 0xe0");
    gfx(s, "gfx text 120 140 0xff - SurfGPU");
    t0 = s->now;
    check(gfx(s, "gfx crc") == CMD_OK && (p = strstr(s->out, "gfx crc: frame ")) != NULL &&
          sscanf(p, "gfx crc: frame %*u, crc %lx", &got) == 1, "gfx crc");
    d.mode = 2; d.base = 0; d.stride = 640; d.scale = 1;
    check(got == stub_crc(s, &d), "  its CRC is the model's of the picture (gpu_display_render, gpu_frame_crc)");
    check(s->crc_frame * STUB_FRAME_US > t0, "  of a frame that began after the command did");
    gfx(s, "gfx mode gfx 2");
    gfx(s, "gfx pal grey");
    got = 0;
    check(gfx(s, "gfx crc") == CMD_OK && (p = strstr(s->out, "gfx crc: frame ")) != NULL &&
          sscanf(p, "gfx crc: frame %*u, crc %lx", &got) == 1, "gfx crc at 320x240 in greys");
    d.stride = 320; d.scale = 2;
    check(got == stub_crc(s, &d), "  the model's again");
    s->frames_stop = 1;
    check(gfx(s, "gfx crc") == CMD_FAIL && said(s, "FRAME_COUNT stayed at"), "no frames: gfx crc says so and fails");
    done(s);
}
TEST(t_crc)

/**** palettes ****/

static void t_pal(unsigned seed) {
    struct stub *s = fresh(1);
    unsigned n, ok;
    static const unsigned hue[8][3] = { {15,15,15}, {15,0,0}, {0,15,0}, {0,0,15}, {15,15,0}, {0,15,15}, {15,0,15}, {15,8,0} };
    (void)seed;
    check(gfx(s, "gfx pal grey") == CMD_OK && said(s, "palette: grey (16 levels)"), "gfx pal grey");
    for(n = 0, ok = 1; n < 256; n++) ok &= s->gpal[n] == (n >> 4) * 0x111;
    check(ok, "  GPAL[n] = (n >> 4) in red, green and blue");
    check(gfx(s, "gfx pal ramps") == CMD_OK && said(s, "palette: ramps (8 hues of 32 shades)"), "gfx pal ramps");
    for(n = 0, ok = 1; n < 256; n++) {
        unsigned i = n & 31, h = n >> 5;
        ok &= s->gpal[n] == (((hue[h][0] * i + 15) / 31) << 8 | ((hue[h][1] * i + 15) / 31) << 4 | (hue[h][2] * i + 15) / 31);
    }
    check(ok && s->gpal[0] == 0 && s->gpal[31] == 0xFFF && s->gpal[32] == 0 && s->gpal[63] == 0xF00 && s->gpal[255] == 0xF80 &&
          s->gpal[16] == 0x888, "  ramp h shade i: each of the hue's components times i / 31, rounded");
    check(gfx(s, "gfx pal rgb332") == CMD_OK && said(s, "rgb332"), "gfx pal rgb332");
    for(n = 0, ok = 1; n < 256; n++) ok &= s->gpal[n] == gpu_gpal_initial(n);
    check(ok, "  GPAL is what the configuration leaves (the model's RGB332)");
    check(gfx(s, "gfx pal 5 0xf80") == CMD_OK && s->gpal[5] == 0xF80 && said(s, "GPAL[5] = 0xf80"), "gfx pal 5 0xf80");
    check(gfx(s, "gfx") == CMD_OK && said(s, "palette: its own (1 of 256 entries differ from rgb332)"), "gfx names the palette");
    done(s);
}
TEST(t_pal)

/**** the demo ****/

static void t_demo(unsigned seed) {
    struct stub *s = fresh(1);
    uint8_t *before = malloc(GPU_VRAM_BYTES);
    uint16_t pal[256];
    struct stub_disp reg;
    unsigned nw0, flips, i;
    uint64_t t0;
    (void)seed;
    gfx(s, "gfx mode gfx 1");
    gfx(s, "gfx clear 0x92");
    gfx(s, "gfx fill 0 0 320 240 0x1c");
    gfx(s, "gfx text 10 300 0xff 0 a picture before the demo");
    gfx(s, "gfx pal grey");
    stub_drain(s, 100000);
    memcpy(before, s->vram, GPU_VRAM_BYTES);
    memcpy(pal, s->gpal, sizeof(pal));
    reg = s->reg;
    s->check_tearing = 1;
    nw0 = s->nw;
    t0 = s->now;
    check(gfx(s, "gfx demo 1") == CMD_OK && said(s, "gfx demo: 1 s at 320x240, two pages flipped") &&
          said(s, "display back as it was"), "gfx demo 1: runs and ends");
    for(i = nw0, flips = 0; i < s->nw; i++) flips += s->wlog[i].reg == SURF_VIDEO_GFX_BASE && (s->wlog[i].value == 0 ||
                                                                                                 s->wlog[i].value == 76800);
    check(flips >= 50 && strstr(s->out, " frames in 1."), "  about 60 page flips in the second");
    check(s->shown_draws == 0, "  no engine word drew into the page on the screen while it was shown");
    check(!memcmp(s->vram, before, GPU_VRAM_BYTES), "  VRAM back as it was");
    check(!memcmp(s->gpal, pal, sizeof(pal)), "  the palette back (the demo drew in RGB332)");
    check(s->reg.mode == reg.mode && s->reg.base == reg.base && s->reg.stride == reg.stride && s->reg.scale == reg.scale,
          "  the display registers back");
    check(G.t_w == 640 && G.t_h == 480 && G.t_stride == 640 && s->eng.tw == 640 && s->eng.th == 480 && s->eng.cy1 == 480 &&
          s->eng.cx1 == 640, "  the target and clip back, in the driver and the engine");
    check(!(p_status(s) & 1) && s->now - t0 < 1500000, "  the engine idle; about a second");
    /* Ctrl+C */
    s->intr_at = s->now + 300000;
    t0 = s->now;
    check(gfx(s, "gfx demo 5") == CMD_FAIL && said(s, "gfx demo: ended by Ctrl+C") && said(s, "display back as it was"),
          "gfx demo 5, Ctrl+C after 0.3 s: ends, says so");
    s->intr_at = 0;
    check(!memcmp(s->vram, before, GPU_VRAM_BYTES) && !memcmp(s->gpal, pal, sizeof(pal)) && s->reg.mode == reg.mode &&
          s->reg.stride == reg.stride && s->reg.scale == reg.scale && !(p_status(s) & 1) && s->now - t0 < 800000,
          "  VRAM, palette, display back, the engine idle, at once");
    check(s->shown_draws == 0, "  still no drawing into the page shown");
    /* text mode, and no memory for the copy */
    gfx(s, "gfx mode text");
    s->no_memory = 1;
    check(gfx(s, "gfx demo 1") == CMD_OK && said(s, "no memory to keep the 158720 bytes of VRAM") && s->reg.mode == 1,
          "no memory: said, run all the same, back to text mode");
    check(gfx(s, "gfx demo 0") == CMD_USAGE && gfx(s, "gfx demo 4000") == CMD_USAGE, "gfx demo 0 and 4000: usage");
    free(before);
    done(s);
    /* GPU2: the depth buffer kept too (the demo's SET_TARGET drops the engine's; 10/26/26) */
    s = fresh2(1);
    gfx(s, "gfx mode gfx 2");
    gfx(s, "gfx zbuf 153600 640");
    gfx(s, "gfx zclear 0x12");
    check(gfx(s, "gfx demo 1") == CMD_OK && G.z_valid && G.z_base == 153600 && G.z_stride == 640 && s->eng.zvalid &&
          s->eng.zbase == 153600 && s->eng.zstride == 640 && s->eng.tw == 320 && s->vram[153600] == 0x12 &&
          s->vram[GPU_VRAM_BYTES - 1] == 0x12, "gfx demo on a GPU2 with a depth buffer: it and its depths back after");
    done(s);
}
TEST(t_demo)

/* The demo's way in and out (the G3 review's finding 2): whenever the CPU writes VRAM or GPAL during
   it the screen shows text, and whenever the engine draws, text or the demo's own pages; so no frame
   shows the picture from before over the demo's pages or palette, or the demo's pages being
   overwritten. "Shows" is what a frame's lines show, a change in a blanking counting for the frame
   after it (stub_visible()). */
static int demo_layout(const struct stub_disp *d) {
    return d->mode == 2 && d->stride == 320 && d->scale == 2 && (d->base == 0 || d->base == 76800);
}

static void demo_order(struct stub *s, const char *what, unsigned gpal_writes, unsigned words) {
    unsigned i, cpu = 0, gpal = 0, eng = 0, bad_cpu = 0, bad_eng = 0;
    char line[160];
    for(i = 0; i < s->nev; i++) {
        struct stub_disp d = stub_visible(s, s->ev[i].t);
        if(s->ev[i].kind == STUB_EV_ENGINE_DRAW) {
            eng++;
            bad_eng += d.mode != 1 && !demo_layout(&d);
        } else {
            gpal += s->ev[i].kind == STUB_EV_GPAL;
            cpu += s->ev[i].kind == STUB_EV_CPU_VRAM;
            bad_cpu += d.mode != 1;
        }
    }
    snprintf(line, sizeof(line), "  %s: VRAM put back (%u words), GPAL written %u times (%u expected), the engine drew", what,
             cpu, gpal, gpal_writes);
    check(cpu >= words && gpal == gpal_writes && eng > 0, line);
    check(!bad_cpu, "  every CPU write of VRAM or GPAL while the screen showed text");
    check(!bad_eng, "  the engine drew while the screen showed text or the demo's own pages");
    s->nev = 0;
}

static void t_demo_order(unsigned seed) {
    struct stub *s = fresh(1);
    (void)seed;
    gfx(s, "gfx mode gfx 1");
    gfx(s, "gfx clear 0x92");
    gfx(s, "gfx pal grey");
    s->watch = 1;
    check(gfx(s, "gfx demo 1") == CMD_OK && s->reg.mode == 2 && s->reg.stride == 640, "gfx demo 1 from a 640x480 picture");
    s->watch = 0;
    demo_order(s, "gfx demo 1", 512, 39680);
    gfx(s, "gfx mode overlay 2");
    s->watch = 1;
    s->intr_at = s->now + 400000;
    check(gfx(s, "gfx demo 3") == CMD_FAIL && s->reg.mode == 3 && s->reg.scale == 2, "gfx demo 3 from the overlay, Ctrl+C");
    s->intr_at = 0;
    s->watch = 0;
    demo_order(s, "Ctrl+C", 512, 39680);
    done(s);
}
TEST(t_demo_order)

/* The engine stops in the middle of a demo (the G3 review's finding 3): said once, no second
   timeout, and the display, the driver's target and clip, VRAM and GPAL all put back. */
static unsigned count(const char *hay, const char *needle) {
    unsigned n = 0;
    for(; (hay = strstr(hay, needle)) != NULL; hay++) n++;
    return n;
}

static void t_demo_stop(unsigned seed) {
    int from_gfx;
    (void)seed;
    for(from_gfx = 0; from_gfx < 2; from_gfx++) {
        struct stub *s = fresh(1);
        uint8_t *before = malloc(GPU_VRAM_BYTES);
        uint16_t pal[256];
        struct stub_disp reg;
        uint64_t t0;
        if(from_gfx) {
            gfx(s, "gfx mode gfx 1");
            gfx(s, "gfx clear 0x92");
            gfx(s, "gfx pal grey");
        } else {
            gfx(s, "gfx mode text");
        }
        stub_drain(s, 100000);
        memcpy(before, s->vram, GPU_VRAM_BYTES);
        memcpy(pal, s->gpal, sizeof(pal));
        reg = s->reg;
        s->watch = 1;
        s->stop_at = s->now + 500000;
        t0 = s->now;
        check(gfx(s, "gfx demo 3") == CMD_FAIL && count(s->out, "a stopped engine") == 1 && said(s, "display back as it was"),
              from_gfx ? "the engine stops 0.5 s into a demo begun on a 640x480 picture: said once" :
                         "the engine stops 0.5 s into a demo begun in text mode: said once");
        check(s->now - t0 < 3000000, "  no second timeout (0.5 s, then one 2 s wait, and the put-back)");
        check(s->reg.mode == reg.mode && s->reg.base == reg.base && s->reg.stride == reg.stride && s->reg.scale == reg.scale,
              "  the display registers back");
        check(G.t_base == 0 && G.t_stride == 640 && G.t_w == 640 && G.t_h == 480 && G.c_x1 == 640 && G.c_y1 == 480,
              "  the driver's target and clip back");
        check(!memcmp(s->vram, before, GPU_VRAM_BYTES) && !memcmp(s->gpal, pal, sizeof(pal)), "  VRAM and GPAL back");
        s->watch = 0;
        demo_order(s, "a stopped engine", from_gfx ? 512 : 0, 39680);
        s->stopped = 0;
        check(gfx(s, "gfx fill 0 0 1 1 1") == CMD_OK && s->eng.tw == 640 && s->eng.th == 480 && s->eng.stride == 640,
              "  the engine back: its target is the driver's, and drawing works");
        free(before);
        done(s);
    }
}
TEST(t_demo_stop)

/* The re-review's N1: Ctrl+C on an engine that has stopped, before two seconds could say so. The
   demo's way out waits through Ctrl+C (keep_going) and gave the dead engine two seconds that a
   second Ctrl+C could not cut short. Now, once Ctrl+C has come, a run putting things back gives a
   stopped engine 50 ms (its ABORT emptied the FIFO; what is left takes a live engine microseconds). */
static void t_ctrlc_dead(unsigned seed) {
    struct stub *s = fresh(1);
    uint8_t *before = malloc(GPU_VRAM_BYTES);
    uint16_t pal[256];
    struct stub_disp reg;
    uint64_t intr;
    char line[120];
    (void)seed;
    gfx(s, "gfx mode gfx 1");
    gfx(s, "gfx clear 0x92");
    gfx(s, "gfx pal grey");
    stub_drain(s, 100000);
    memcpy(before, s->vram, GPU_VRAM_BYTES);
    memcpy(pal, s->gpal, sizeof(pal));
    reg = s->reg;
    s->stop_at = s->now + 500000;
    s->intr_at = intr = s->now + 600000;
    check(gfx(s, "gfx demo 5") == CMD_FAIL && said(s, "gfx demo: ended by Ctrl+C") && count(s->out, "a stopped engine") == 1 &&
          said(s, "gfx demo: the GPU finished nothing for 50 ms: a stopped engine; ABORT\n") &&
          said(s, "display back as it was"),
          "Ctrl+C 0.1 s after the engine stopped: the demo ends, the stopped engine said once, after 50 ms");
    snprintf(line, sizeof(line), "  all over %llu ms after the Ctrl+C: within 0.3 s, not another 2 s",
             (unsigned long long)((s->now - intr) / 1000));
    check(s->now - intr < 300000, line);
    if(getenv("GPU_TEST_VERBOSE")) printf("%s\n", line);
    check(s->reg.mode == reg.mode && s->reg.base == reg.base && s->reg.stride == reg.stride && s->reg.scale == reg.scale &&
          !memcmp(s->vram, before, GPU_VRAM_BYTES) && !memcmp(s->gpal, pal, sizeof(pal)) && G.t_w == 640 && G.t_h == 480 &&
          G.c_x1 == 640 && G.c_y1 == 480, "  the display, VRAM, GPAL, the target and the clip back");
    s->stopped = 0;
    s->intr_at = 0;
    check(gfx(s, "gfx fill 0 0 1 1 1") == CMD_OK && s->eng.tw == 640 && s->eng.th == 480, "  the engine back: drawing works");
    free(before);
    done(s);
}
TEST(t_ctrlc_dead)

/**** bench, reset, the state, usage ****/

/* the words of the command at c[i]: header, parameters, data (GPU.md section 5) */
static unsigned cmd_len(const uint32_t *c, unsigned i) {
    uint32_t wh = c[i + 2];
    switch(c[i] >> 24) {
    case GPU_OP_SET_TARGET: case GPU_OP_COPY_RECT: return 4;
    case GPU_OP_SET_CLIP: case GPU_OP_FILL_RECT: case GPU_OP_LINE: return 3;
    case GPU_OP_FENCE: return 2;
    case GPU_OP_EXPAND: return 3 + ((wh & 0xFFFF) * (wh >> 16) + 31) / 32;
    case GPU_OP_WRITE_RECT: return 3 + ((wh & 0xFFFF) * (wh >> 16) + 3) / 4;
    }
    return 1;
}

/* the pixels a rate line of gfx bench counts */
static unsigned long long rate_px(struct stub *s, const char *name) {
    char key[16];
    const char *p;
    unsigned long long n;
    snprintf(key, sizeof(key), "\n    %-5s ", name);
    if(!(p = strstr(s->out, key)) || !(p = strstr(p, ": ")) || sscanf(p + 2, "%llu pixels", &n) != 1) return ~0ull;
    return n;
}

static uint32_t bench_next(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* gfx bench on the target the driver has: what each rate counts is what the engine wrote (the
   review's finding 10: strings past the target's right edge were counted whole), and its strings
   are the generator's next numbers in the order x, y, fg, bg, after the lines' 1,024 */
static void bench_on(struct stub *s, const char *name) {
    uint32_t gen = 2026, w = G.t_w, h = G.t_h, n = w / 8 < 32 ? w / 8 : 32, x, y, fg, bg;
    unsigned i, from, strings = 0, wrong = 0;
    char line[300];
    memset(s->op_px, 0, sizeof(s->op_px));
    from = s->ncmd;
    check(gfx(s, "gfx bench") == CMD_OK, name);
    stub_drain(s, 1000000);
    snprintf(line, sizeof(line), "  %s: pixels counted = written: fill %llu = %llu, copy %llu = %llu, line %llu = %llu, "
             "text %llu = %llu", name, rate_px(s, "fill"), s->op_px[GPU_OP_FILL_RECT], rate_px(s, "copy"),
             s->op_px[GPU_OP_COPY_RECT], rate_px(s, "line"), s->op_px[GPU_OP_LINE], rate_px(s, "text"),
             s->op_px[GPU_OP_EXPAND]);
    check(rate_px(s, "fill") == s->op_px[GPU_OP_FILL_RECT] && rate_px(s, "copy") == s->op_px[GPU_OP_COPY_RECT] &&
          rate_px(s, "line") == s->op_px[GPU_OP_LINE] && rate_px(s, "text") == s->op_px[GPU_OP_EXPAND] &&
          s->op_px[GPU_OP_EXPAND] == 64ull * n * 128, line);
    for(i = 0; i < 1024; i++) bench_next(&gen);
    for(i = from; i < s->ncmd; i += cmd_len(s->cmd, i)) {
        if(s->cmd[i] >> 24 != GPU_OP_EXPAND) continue;
        x = bench_next(&gen) % (w - 8 * n + 1);
        y = bench_next(&gen) % (h - 16 + 1);
        fg = bench_next(&gen) & 0xFF;
        bg = bench_next(&gen) & 0xFF;
        if(s->cmd[i] != GPU_HDR(GPU_OP_EXPAND, fg | bg << 8) || s->cmd[i + 1] != GPU_XY(x, y) || s->cmd[i + 2] != GPU_WH(8 * n, 16))
            wrong++;
        strings++;
    }
    snprintf(line, sizeof(line), "  %s: 64 strings of %lu cells, each the generator's x, y, fg, bg (%u strings, %u not)", name,
             (unsigned long)n, strings, wrong);
    check(strings == 64 && !wrong, line);
}

static void t_bench(unsigned seed) {
    struct stub *s = fresh(1);
    (void)seed;
    gfx(s, "gfx mode gfx 1");
    check(gfx(s, "gfx bench") == CMD_OK && said(s, "fill  8 x 640x480: 2457600 pixels") && said(s, "copy  8 x 320x480") &&
          said(s, "line  256 lines") && said(s, "text  64 x 32 cells: 262144 pixels") && said(s, "million/s") &&
          G.spin_us == GPU_SPIN_US,
          "gfx bench: fills, copies, lines and text, each in pixels a second");
    bench_on(s, "gfx bench at 640x480");
    gfx(s, "gfx mode gfx 2");
    bench_on(s, "gfx bench at 320x240");
    check(gpu_set_target(&G, 0, 200, 200, 100, "gfx") == GPU_OK, "a target of 200x100");
    bench_on(s, "gfx bench at 200x100 (25 cells a string)");
    check(gpu_set_target(&G, 0, 64, 64, 32, "gfx") == GPU_OK, "a target of 64x32, the smallest the bench takes");
    bench_on(s, "gfx bench at 64x32 (8 cells a string)");
    gfx(s, "gfx mode gfx 1");
    s->intr_at = s->now + 5000;
    check(gfx(s, "gfx bench") == CMD_FAIL && said(s, "interrupted"), "gfx bench ends on Ctrl+C");
    s->intr_at = 0;
    check(gfx(s, "gfx fill 0 0 1 1 1") == CMD_OK, "  and the engine works after it");
    done(s);
}
TEST(t_bench)

static void t_reset(unsigned seed) {
    struct stub *s = fresh(1);
    unsigned a0;
    (void)seed;
    gfx(s, "gfx mode gfx 2");
    s->eng.sticky |= GPU_ST_BAD_OPCODE;
    a0 = s->aborts;
    check(gfx(s, "gfx reset") == CMD_OK && said(s, "gfx reset: ABORT, errors cleared"), "gfx reset");
    check(s->aborts > a0 && !(s->eng.sticky & (GPU_ST_BAD_OPCODE | GPU_ST_FENCE_EV)) && s->eng.tw == 640 && s->eng.th == 480 &&
          s->eng.stride == 640 && s->eng.base == 0 && s->eng.cx1 == 640 && s->eng.cy1 == 480 && G.t_w == 640,
          "  ABORT, the sticky bits cleared, the reset target and clip in the engine and the driver");
    done(s);
}
TEST(t_reset)

static void t_state(unsigned seed) {
    struct stub *s = fresh(1);
    (void)seed;
    gfx(s, "gfx mode overlay 2");
    check(gfx(s, "gfx") == CMD_OK && said(s, "SurfGPU GPU1 (gpu.ID 0x31555047), VRAM 300 KiB at 0x30000000\n") &&
          said(s, "engine idle, STATUS 0x0, FIFO 64 words, 64 free\n") &&
          said(s, "display: mode 3, graphics with the text over it\n") &&
          said(s, "GFX_BASE 0, GFX_STRIDE 320, GFX_SCALE 2 (320x240 doubled)\n") &&
          said(s, "target: VRAM 0, stride 320, 320x240\n    clip: (0, 0)-(320, 240)\n") &&
          said(s, "interrupts: 0 (intc source 5, IE.FENCE)\n    fences that asked for it: ") &&
          said(s, " (gfx cube's frames' do not)\n    fence waits that slept on it: 0, woken by it: 0\n") &&
          said(s, "errors 0, timeouts 0, ABORTs 0\n") &&
          said(s, "by kind: OVERFLOW 0, BAD_OPCODE 0, BAD_PARAM 0\n") && said(s, "frames: FRAME_COUNT"),
          "gfx: ID, VRAM, mode, base/stride/scale, FIFO, fences, errors, frames");
    done(s);
}
TEST(t_state)

/* Every line gfx and the driver print fits the console's 80 columns (GPU.md 15.2; the review's
   finding 8), here with each message at its widest: the counts at 10 digits (32 bits on the
   board), the registers and the target at their widest, the longest names. done() measures each
   line printed, as it does after every test. */
static void t_columns(unsigned seed) {
    static const char *const lines[] = {
        "gfx", "gfx help", "gfx frobnicate", "gfx clear x", "gfx fill", "gfx rect", "gfx rect 32000 0 1000 1 1", "gfx line",
        "gfx copy", "gfx text", "gfx pal", "gfx pal x", "gfx put", "gfx get", "gfx crc x", "gfx bench x", "gfx demo x",
        "gfx reset x", "gfx mode", "gfx mode foo", "gfx a b c d e f g h i j k l m n o p q r",
        "gfx put 32767 32767 1", "gfx get 32767 32767", "gfx bench", "gfx pal 255 0xfff",
        "gfx get 9999 29", NULL
    };
    struct stub *s = fresh(1);
    char word[220], buf[160];
    unsigned i;
    (void)seed;
    s->reg.mode = 3;                                    /* the longest mode's name, the widest GFX_ registers */
    s->reg.base = SURF_VIDEO_GFX_BASE_BASE_MASK & ~3u;
    s->reg.stride = SURF_VIDEO_GFX_STRIDE_STRIDE_MASK & ~3u;
    s->reg.scale = 3;
    check(gpu_set_target(&G, 307196, 65532, 4, 1, "gfx") == GPU_OK &&
          gpu_set_clip(&G, 65535, 65535, 65535, 65535, "gfx") == GPU_OK && gpu_sync(&G, "gfx") == GPU_OK,
          "the widest target (VRAM 307196, stride 65532) and clip");
    G.words = G.fences = G.irqs = G.irq_waits = G.irq_wakes = G.timeouts = G.aborts = G.errors = 4294967295ul;
    G.irq_fences = 4294967295ul;
    G.overflows = G.bad_opcodes = G.bad_params = G.display_changes = G.latch_misses = 4294967295ul;
    G.fifo_depth = 65535;
    G.fence_sent = s->eng.fence = 4294967290u;          /* FENCE at 10 digits (the driver's and the engine's) */
    s->frames_stop = 1;
    s->frames_frozen = 65535;                           /* FRAME_COUNT's 16 bits */
    check(gfx(s, "gfx") == CMD_OK && said(s, "    sent: 4294967295 words, 4294967295 fences; FENCE 4294967290\n") &&
          said(s, "    fences that asked for it: 4294967295 (gfx cube's frames' do not)\n") &&
          said(s, "    clip: (65535, 65535)-(65535, 65535)\n") && said(s, "FRAME_COUNT 65535"),
          "gfx with every count at 10 digits");
    snprintf(buf, sizeof(buf), "    gpu       ");
    gpu_describe(&G, buf + strlen(buf), (u_int)(sizeof(buf) - strlen(buf)));
    check(strlen(buf) <= STUB_COLUMNS && strstr(buf, "SurfGPU GPU1, 300 KiB VRAM; 4294967295 fences, 4294967295 errors"),
          "drivers' gpu line with the counts at 10 digits");
    check(gfx(s, "gfx crc") == CMD_FAIL && said(s, "FRAME_COUNT stayed at 65535"), "  no frames, FRAME_COUNT at 65535");
    s->frames_stop = 0;
    for(i = 0; i < 200; i++) word[i] = (char)('a' + i % 26);
    word[200] = 0;
    check(gfx(s, "gfx %s", word) == CMD_USAGE && said(s, "no such subcommand: abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv\n"),
          "a subcommand of 200 letters: its first 48 said");
    check(gpu_set_target(&G, 0, 10000, 10000, 30, "gfx") == GPU_OK && gpu_sync(&G, "gfx") == GPU_OK,
          "the target 10000x30: the widest pair of sizes a target can have");
    for(i = 0; lines[i]; i++) gfx(s, "%s", lines[i]);
    /* the claim's wait with the longest name and pid, all three errors at once, a stopped engine with
       a 9-letter command and FENCE at 10 digits */
    gfx(s, "gfx mode gfx 1");
    s->holder = "'abcdefghijklmnopqrstuvwxyz01234', pid 4294967295";
    s->other_holds = 1;
    s->other_until = s->now + 50000;
    check(gfx(s, "gfx clear 1") == CMD_OK && said(s, "    held by 'abcdefghijklmnopqrstuvwxyz01234', pid 4294967295\n"),
          "the claim's wait names the longest holder");
    s->holder = NULL;
    s->overflow = GPU_ST_OVERFLOW;
    s->eng.sticky |= GPU_ST_BAD_OPCODE | GPU_ST_BAD_PARAM;
    check(gfx(s, "gfx clear 2") == CMD_FAIL && said(s, "      BAD_PARAM:") && said(s, "      OVERFLOW:"), "all three errors said");
    stub_drain(s, 1000);
    G.fence_sent = s->eng.fence = 4294967290u;
    s->stopped = 1;
    check(gfx(s, "gfx bench") == CMD_FAIL && said(s, "gfx bench: the GPU finished nothing for 2 s: a stopped engine; ABORT\n") &&
          said(s, ", FENCE 4294967290, waiting for 4294967291\n"), "a stopped engine, its FENCE at 10 digits");
    s->stopped = 0;
    s->now += 50000ull * STUB_FRAME_US;                 /* FRAME_COUNT at 5 digits */
    check(gfx(s, "gfx crc") == CMD_OK && said(s, "gfx crc: frame 5"), "gfx crc with FRAME_COUNT at 5 digits");
    done(s);
}
TEST(t_columns)

static void t_usage(unsigned seed) {
    static const char *const bad[] = {
        "gfx fill 1 2 3", "gfx fill a b c d e", "gfx fill 0 0 70000 1 1", "gfx fill 0 0 1 1 256", "gfx fill 40000 0 1 1 1",
        "gfx line 0 0 0 -32769 1", "gfx rect 32000 0 1000 1 1", "gfx copy 0 0 0 0 1", "gfx copy 0 0 0 0 1 1 300",
        "gfx text 0 0 1", "gfx text 0 0 1 x hello", "gfx mode", "gfx mode foo", "gfx mode text 2", "gfx mode gfx 3",
        "gfx pal", "gfx pal blue", "gfx pal 256 0", "gfx pal 0 0x1000", "gfx put 640 0 1", "gfx put 0 0", "gfx get 0 480",
        "gfx clear 1 2", "gfx clear 0x100", "gfx crc 1", "gfx reset now", "gfx bench 3", "gfx demo x", "gfx frobnicate",
        "gfx fill 0x 0 1 1 1", "gfx fill 99999999999999 0 1 1 1",
        "gfx text 0 0 255 0 one two three four five six seven eight nine ten eleven twelve",
        "gfx text 0 0 1 2 a b c d e f g h i j", NULL
    };
    struct stub *s = fresh(1);
    unsigned i, n0, bad_ones = 0;
    (void)seed;
    gfx(s, "gfx mode gfx 1");
    n0 = s->ncmd;
    for(i = 0; bad[i]; i++) {
        if(gfx(s, "%s", bad[i]) != CMD_USAGE || s->ncmd != n0 || !said(s, "usage")) {
            if(!bad_ones++) printf("    (%s: %s)\n", bad[i], s->out);
        }
    }
    check(!bad_ones, "usage errors: CMD_USAGE, a usage line, and not a word sent");
    check(!s->locked, "  the claim never left held");
    /* numbers: decimal (a leading zero too) or 0x hex (the review's finding 9) */
    check(gfx(s, "gfx clear 0") == CMD_OK && gfx(s, "gfx fill 010 0 1 1 7") == CMD_OK && px(s, 10, 0) == 7 && px(s, 8, 0) == 0 &&
          gfx(s, "gfx fill 08 1 1 1 0x1F") == CMD_OK && px(s, 8, 1) == 0x1F && gfx(s, "gfx fill 0 2 1 1 012") == CMD_OK &&
          px(s, 0, 2) == 12 && gfx(s, "gfx fill -0x2 3 3 1 0XfF") == CMD_OK && px(s, 0, 3) == 255 && px(s, 1, 3) == 0,
          "  numbers: 010 is ten, 08 eight, 012 twelve, -0x2 and 0XfF hex");
    check(gfx(s, "gfx fill 0 0 4294967297 1 1") == CMD_USAGE && gfx(s, "gfx fill 0 0 1 1 -") == CMD_USAGE &&
          gfx(s, "gfx fill 0 0 0x 1 1") == CMD_USAGE && gfx(s, "gfx fill 0 0 1 1 1x") == CMD_USAGE,
          "  a number past 32 bits, a sign alone, 0x alone and a stray letter are refused");
    check(gfx(s, "gfx text 0 0 255 0 one two three four five six seven eight nine ten eleven twelve") == CMD_USAGE &&
          said(s, "a gfx line holds 15 words at most, gfx text 9 of text"),
          "  a line the shell may have cut (it keeps 16 words) is refused, saying why");
    check(gfx(s, "gfx clear 0x25") == CMD_OK && gfx(s, "gfx text 0 0 1 2 a b c d e f g h i") == CMD_OK &&
          px(s, 8 * 16, 0) == 2 && px(s, 8 * 17, 0) == 0x25, "  15 words (9 of text) draw all 17 cells");
    done(s);
}
TEST(t_usage)

/* GPU2's commands: every argument checked before a word is sent (a usage line, CMD_USAGE); x and
   y in pixels with sixteenths; how's words */
static void t_usage2(unsigned seed) {
    static const char *const bad[] = {
        "gfx tri", "gfx tri 1 2 3", "gfx tri 0 0 10 0 0 10", "gfx tri 0 0 10 0 0 10 256", "gfx tri 0.3 0 10 0 0 10 1",
        "gfx tri 2048 0 10 0 0 10 1", "gfx tri -2048.0625 0 10 0 0 10 1", "gfx tri gouraud 0 0 10 0 0 10 1",
        "gfx tri zwrite 0 0 10 0 0 10 1", "gfx tri zle 0 0 10 0 0 10 1", "gfx tri frob 0 0 10 0 0 10 1",
        "gfx tri flat,gouraud 0 0 0 1 9 0 0 1 0 9 0 1", "gfx tri ztest,zle 0 0 0 1 9 0 0 1 0 9 0 1",
        "gfx tri gouraud,,zwrite 0 0 0 1 9 0 0 1 0 9 0 1", "gfx tri gouraud, 0 0 0 1 9 0 0 1 0 9 0 1",
        "gfx tri ,gouraud 0 0 0 1 9 0 0 1 0 9 0 1", "gfx tri cullcw,cullcw 0 0 9 0 0 9 1",
        "gfx tri 0 0 70000 1 9 0 0 1 0 9 0 1", "gfx tri 0 0 0 1 9 0 0 1 0 9 0 256", "gfx tri 0 0 0 1 9 0 0 1 0 9 0",
        "gfx tri 0x 0 9 0 0 9 1", "gfx tri 1.2.3 0 9 0 0 9 1", "gfx tri 12. 0 9 0 0 9 1", "gfx tri 0x10.5 0 9 0 0 9 1",
        "gfx tri 1.000000001 0 9 0 0 9 1", "gfx tri - 0 9 0 0 9 1", "gfx tri 1e3 0 9 0 0 9 1",
        "gfx zbuf", "gfx zbuf 1 640", "gfx zbuf 153600 320", "gfx zbuf 300000 640", "gfx zbuf 0 x", "gfx zbuf on",
        "gfx zbuf 153600 640 1", "gfx zclear 256", "gfx zclear 1 2", "gfx zclear x", "gfx flip 2", "gfx flip 0x80000",
        "gfx flip 1 2", "gfx flip x", "gfx target 1 320 320 240", "gfx target 0 320 321 240", "gfx target 0 640 640 481",
        "gfx target 0 320 320", "gfx target 0 32768 32768 1", NULL
    };
    struct stub *s = fresh2(1);
    unsigned i, n0, bad_ones = 0;
    (void)seed;
    gfx(s, "gfx mode gfx 2");
    n0 = s->ncmd;
    for(i = 0; bad[i]; i++) {
        if(gfx(s, "%s", bad[i]) != CMD_USAGE || s->ncmd != n0 || !said(s, "usage")) {
            if(!bad_ones++) printf("    (%s: %s)\n", bad[i], s->out);
        }
    }
    check(!bad_ones, "GPU2's usage errors (tri, zbuf, zclear, flip) and gfx target's: CMD_USAGE, a usage line, not a word sent");
    check(!s->locked && !G.z_valid && G.t_w == 320, "  the claim never left held, nothing changed");
    /* what is taken: sixteenths in decimal, hex pixels, a fraction alone, the extremes of 12.4 */
    gfx(s, "gfx zbuf 153600 640");
    n0 = s->ncmd;
    check(gfx(s, "gfx tri 0x10 .5 -2048 2047.9375 +3.0625 -0.0625 7") == CMD_OK && s->ncmd == n0 + 7 + 2 &&
          s->cmd[n0] == GPU_HDR(GPU_OP_TRIANGLE, 7) && s->cmd[n0 + 1] == GPU_XY(256, 8) && s->cmd[n0 + 2] == GPU_ZC(0, 7) &&
          s->cmd[n0 + 3] == GPU_XY(-32768, 32767) && s->cmd[n0 + 5] == GPU_XY(49, -1),
          "  0x10 is 16 pixels, .5 half of one, -2048 and 2047.9375 the ends of 12.4, +3.0625 and -0.0625 in sixteenths");
    n0 = s->ncmd;
    check(gfx(s, "gfx tri zle,cullccw,gouraud,zwrite 1 2 3 4 5 6 65535 255 7 8 0 0") == CMD_OK &&
          s->cmd[n0] == GPU_HDR(GPU_OP_TRIANGLE, 4 | GPU_TRI_ZTEST | GPU_TRI_ZFUNC_LE | GPU_TRI_CULL_CCW | GPU_TRI_GOURAUD |
                                                 GPU_TRI_ZWRITE) && s->cmd[n0 + 4] == GPU_ZC(65535, 255),
          "  how's words in any order: zle is ZTEST and ZFUNC; vertex 0's colour the header's (flat)");
    check(gfx(s, "gfx tri flat,cullcw,cullccw 0 0 9 0 0 9 1") == CMD_OK, "  flat, and both culls (nothing is drawn)");
    /* and gfx help and gfx tri's usage say the same of how (the review of 58ac7b3's L4: the help said
       cullcw|cullccw, one or the other, where both are taken) */
    check(gfx(s, "gfx help") == CMD_OK && said(s, "how: flat|gouraud,ztest|zle,zwrite,cullcw,cullccw (commas between)") &&
          gfx(s, "gfx tri") == CMD_USAGE && said(s, "how: flat|gouraud,ztest|zle,zwrite,cullcw,cullccw joined by commas"),
          "  gfx help and gfx tri's usage give how's words alike: flat or gouraud, ztest or zle, the rest each or not");
    done(s);
}
TEST(t_usage2)

/* GPU2's lines at their widest (80 columns): a refused depth buffer for the widest target, the
   target, the depth buffer, a FLIP at the widest base, the usage of tri */
static void t_columns2(unsigned seed) {
    struct stub *s = fresh2(1);
    (void)seed;
    check(gpu_set_target(&G, 0, 10000, 10000, 30, "gfx") == GPU_OK && gpu_sync(&G, "gfx") == GPU_OK,
          "a target of 10000x30 (the widest pair of sizes a target can have)");
    check(gfx(s, "gfx zbuf 4 4") == CMD_USAGE && said(s, "refused for the target (10000x30)"), "gfx zbuf refused for it");
    check(gfx(s, "gfx zclear") == CMD_FAIL && said(s, "no depth buffer"), "gfx zclear without one");
    check(gfx(s, "gfx target 307196 65532 4 1") == CMD_OK && said(s, "target: VRAM 307196, stride 65532, 4x1"),
          "gfx target at the widest base and stride");
    check(gfx(s, "gfx target 0 16384 16384 2") == CMD_OK && gfx(s, "gfx zbuf 0 32768") == CMD_OK &&
          gfx(s, "gfx zclear") == CMD_FAIL && said(s, "16384 or more wide"), "gfx zclear for a target 16384 wide: refused, said");
    check(gfx(s, "gfx target 0 9996 9996 30") == CMD_OK && gfx(s, "gfx zbuf 0 19992") == CMD_USAGE &&
          gfx(s, "gfx target 0 9996 9996 15") == CMD_OK && gfx(s, "gfx zbuf 0 19992") == CMD_OK &&
          said(s, "depth buffer: VRAM 0, stride 19992, for the target's 9996x15"), "gfx zbuf, said with a wide target");
    check(gfx(s, "gfx flip 0x7fffc") == CMD_OK && said(s, "GFX_BASE 524284"), "gfx flip at the widest base");
    gfx(s, "gfx tri");
    gfx(s, "gfx zbuf");
    gfx(s, "gfx zbuf off");
    gfx(s, "gfx help");
    done(s);
}
TEST(t_columns2)

/**** gfx cube (boards/arty_a7/gfxcube.c; 10/26/26) ****/

/* the turn's frames a CRC is (several when pictures repeat): the one that follows prev by 0 or 1,
   else any, else -1 */
static int cube_frame_of(uint32_t crc, int prev) {
    int f, any = -1;
    for(f = 0; f < (int)GFX_CUBE_FRAMES; f++) {
        if(gfx_cube_expect[f] != crc) continue;
        if(prev >= 0 && ((f - prev + (int)GFX_CUBE_FRAMES) % (int)GFX_CUBE_FRAMES) <= 1) return f;
        if(any < 0) any = f;
    }
    return any;
}

/* the frames the stand-in logged as they ended: from the first of the cube's to the last, every
   one a whole frame of the turn, each the same as the one before it or the next (a FLIP a
   blanking), and the cube turning (the review of 58ac7b3's M2): `pictures` of the turn's frames at
   least, each new one at most two video frames after the one before from frame 2 on (30 a second
   or more: gpudiag's rule; frame 1 follows frame 0's still picture, whose wait is no rate) */
static void cube_at_the_pins(struct stub *s, const char *what, unsigned want, unsigned pictures) {
    unsigned i, first = 0, last = 0, cube = 0, not_cube = 0, bad_steps = 0, distinct = 0, slow = 0, run = 0;
    unsigned char seen[GFX_CUBE_FRAMES];
    int prev = -1, f;
    char line[240];
    memset(seen, 0, sizeof(seen));
    for(i = 0; i < s->nflog; i++)
        if(cube_frame_of(s->flog[i].crc, -1) >= 0) {
            if(!cube++) first = i;
            last = i;
        }
    for(i = first; cube && i <= last; i++) {
        f = cube_frame_of(s->flog[i].crc, prev);
        if(f < 0) {
            not_cube++;
            continue;
        }
        if(!seen[f]) {
            seen[f] = 1;
            distinct++;
        }
        if(prev >= 0 && f != prev) {
            if((f - prev + (int)GFX_CUBE_FRAMES) % (int)GFX_CUBE_FRAMES > 1) bad_steps++;
            if(distinct > 2 && run > 2) slow++;         /* the frame before shown for three video frames or more */
            run = 0;
        }
        run++;
        prev = f;
    }
    snprintf(line, sizeof(line), "  %s: %u frames shown of the cube (%u wanted), %u pictures (%u wanted), %u between them not "
             "the turn's, %u out of step, %u after more than two video frames", what, cube, want, distinct, pictures, not_cube,
             bad_steps, slow);
    check(cube >= want && distinct >= pictures && !not_cube && !bad_steps && !slow, line);
}

/* gfx cube's summary: the frames drawn, shown and missed (shown, never looked at); %n says that
   the whole pattern matched (sscanf counts the conversions only: "N shown" passed for "N drawn") */
static int cube_summary(struct stub *s, unsigned long *drawn, unsigned long *shown, unsigned long *missed) {
    const char *p;
    unsigned long a, b;
    int n = 0, end;
    for(p = s->out; (p = strstr(p, "    gfx cube: ")) != NULL; p++) {
        end = 0;
        if(sscanf(p, "    gfx cube: %lu drawn in%n", &a, &end) == 1 && end) {
            *drawn = a;
            n |= 1;
        }
        end = 0;
        if(sscanf(p, "    gfx cube: %lu shown, %lu missed:%n", &a, &b, &end) == 2 && end) {
            *shown = a;
            *missed = b;
            n |= 2;
        }
    }
    return n == 3;
}

/* gfx cube's rate, in tenths of a frame a second (-1: not said) */
static long cube_rate(struct stub *s) {
    const char *p = strstr(s->out, "    gfx cube: ");
    unsigned long d, sec, hund, whole, tenth;
    for(; p; p = strstr(p + 1, "    gfx cube: "))
        if(sscanf(p, "    gfx cube: %lu drawn in %lu.%lu s, %lu.%lu a second", &d, &sec, &hund, &whole, &tenth) == 5)
            return (long)(whole * 10 + tenth);
    return -1;
}

/* the expected table itself: its 128 frames, GFX_CUBE_PICTURES of them distinct, and its first 16
   gpudiag's own MODE 2 frames (gpuscenes_expect.h's gpu_cube_expect_check: the same display) */
static int once_t_cube_table;
static void t_cube_table(unsigned seed) {
    unsigned k, j, pictures = 0, same16 = 1;
    (void)seed;
    if(once_t_cube_table++) return;     /* nothing here depends on the seed: once */
    for(k = 0; k < GFX_CUBE_FRAMES; k++) {
        for(j = 0; j < k && gfx_cube_expect[j] != gfx_cube_expect[k]; j++) {}
        pictures += j == k;
    }
    for(k = 0; k < GPU_CUBE_CHECK_FRAMES; k++) same16 &= gfx_cube_expect[k] == gpu_cube_expect_check[k];
    check(GFX_CUBE_FRAMES == GPU_CUBE_PERIOD && pictures == GFX_CUBE_PICTURES,
          "gfx cube's table: a turn's 128 frames, GFX_CUBE_PICTURES of them pictures of their own");
    check(same16, "  its first 16 are gpudiag's checked frames in MODE 2 (gpuscenes_expect.h): the same display");
}
TEST(t_cube_table)

/* the check of the frames shown, by itself: in order, the same again, frames passed unseen, one too
   far for the frames counted, backwards, torn, FRAME_COUNT's wrap, a turn's wrap, repeated pictures;
   and in step with the FLIPs (the review of 58ac7b3's M2): each look's FENCE before and after it, as
   FLIPs done after frame 0's (fence0 + k is frame k's), here 1000 + the frame FLIPped last */
static void t_cube_check(unsigned seed) {
    static const u32 t[8] = { 10, 11, 12, 13, 14, 15, 16, 17 };
    static const u32 dup[8] = { 10, 11, 12, 10, 14, 15, 16, 17 };
    struct gfx_cube_check c;
    u32 k;
    (void)seed;
    gfx_cube_check_start(&c, t, 8, true, 100, 0, 1000);
    gfx_cube_check_frame(&c, 101, 10, 1000, 1000);  /* frame 0 again, nothing FLIPped since */
    gfx_cube_check_frame(&c, 102, 11, 1001, 1001);  /* 1 */
    gfx_cube_check_frame(&c, 104, 13, 1003, 1003);  /* 3, two frames on: one passed unseen */
    gfx_cube_check_frame(&c, 104, 99, 1003, 1003);  /* FRAME_COUNT did not move: nothing */
    check(c.shown == 4 && c.checked == 3 && !c.torn && !c.disorder && c.last == 3 && c.advance == 3,
          "the same frame again, the next, two on in two frames: whole, in order; 4 shown, 3 checked, 3 on");
    gfx_cube_check_frame(&c, 105, 15, 1005, 1005);  /* 5: two on in one frame (a FLIP a blanking at most) */
    gfx_cube_check_frame(&c, 106, 14, 1005, 1005);  /* 4: backwards */
    check(c.disorder == 2 && !c.torn, "two on in one frame, and backwards: out of order");
    gfx_cube_check_frame(&c, 107, 99, 1006, 1006);  /* none of them */
    check(c.torn == 1 && c.torn_count == 107 && c.torn_crc == 99 && c.last == 4, "a CRC of none of them: torn, the first kept");
    gfx_cube_check_frame(&c, 109, 16, 1006, 1006);  /* 6, two on in two frames (from 4) */
    gfx_cube_check_frame(&c, 110, 17, 1007, 1007);
    gfx_cube_check_frame(&c, 111, 10, 1008, 1008);  /* the turn wraps: 7, then 0 (frame 8) */
    check(c.disorder == 2 && c.torn == 1 && c.last == 0, "after it, in order again across the turn's wrap");
    gfx_cube_check_start(&c, t, 8, true, 0xFFFE, 2, 998);
    gfx_cube_check_frame(&c, 0xFFFF, 13, 1001, 1001);
    gfx_cube_check_frame(&c, 0x10001, 14, 1002, 1002);  /* FRAME_COUNT is 16 bits: 0xFFFF to 1 is two */
    check(c.shown == 3 && !c.disorder && !c.torn, "FRAME_COUNT's 16-bit wrap counted as the frames it is");
    gfx_cube_check_start(&c, dup, 8, false, 0, 2, 1000);
    gfx_cube_check_frame(&c, 1, 10, 1000, 1000);    /* 0 or 3: the order is not checked */
    gfx_cube_check_frame(&c, 2, 17, 1000, 1000);
    gfx_cube_check_frame(&c, 3, 11, 1000, 1000);
    check(!c.disorder && !c.torn && c.checked == 3, "a turn with repeated pictures: only whole or torn, no order");
    /* the cube standing still while its FLIPs go on (frame 0 drawn every time, the review's M2):
       frame 0 again is in order while at most one FLIP has come since, not after */
    gfx_cube_check_start(&c, t, 8, true, 200, 0, 1000);
    for(k = 1; k <= 6; k++) gfx_cube_check_frame(&c, 200 + k, 10, 1000 + k - 1, 1000 + k - 1);
    check(c.checked == 6 && c.disorder == 4 && !c.advance, "frame 0 shown again and again as FLIPs 0 to 5 are done: "
          "in order for two looks, then out of order");
    /* behind its FLIPs by two, though a frame a frame: out of order; one behind (the frame FLIPped
       last not yet shown) and one ahead (a FLIP done whose FENCE is still the CPU's) are in step */
    gfx_cube_check_start(&c, t, 8, true, 300, 0, 1000);
    gfx_cube_check_frame(&c, 301, 11, 1002, 1002);  /* 1 shown, 2 FLIPped last: in step */
    gfx_cube_check_frame(&c, 302, 12, 1004, 1004);  /* 2 shown, 4 FLIPped: behind by two */
    gfx_cube_check_frame(&c, 303, 13, 1002, 1002);  /* 3 shown, FENCE says 2: its FENCE not yet sent */
    check(c.disorder == 1 && c.advance == 3, "a frame two behind the FLIPs: out of order; one behind, one ahead: in step");
    gfx_cube_check_frame(&c, 304, 14, 1002, 1002);  /* 4 shown, FENCE says 2: two ahead */
    check(c.disorder == 2, "a frame two ahead of the FENCEs read: out of order");
    /* a blanking between the two reads of FENCE: the window is fb - 1 to fa + 1 */
    gfx_cube_check_start(&c, t, 8, true, 400, 0, 1000);
    gfx_cube_check_frame(&c, 401, 11, 1002, 1003);  /* 1 shown, FLIPs 2 then 3 done: in step (fb - 1) */
    gfx_cube_check_frame(&c, 402, 12, 1002, 1003);
    check(!c.disorder, "a FLIP between the two FENCE reads: the frame shown may be fb - 1 to fa + 1");
}
TEST(t_cube_check)

static int once_t_cube;
static void t_cube(unsigned seed) {
    struct stub *s;
    uint8_t *before;
    uint16_t pal[256];
    struct stub_disp reg;
    unsigned long drawn, shown, missed, waits0, wakes0, flips0;
    long rate;
    char want[64], line[160];
    (void)seed;
    if(once_t_cube++) return;     /* nothing here depends on the seed: once */
    s = fresh2(1);
    before = malloc(GPU_VRAM_BYTES);
    gfx(s, "gfx mode gfx 1");
    gfx(s, "gfx clear 0x92");
    gfx(s, "gfx fill 0 0 320 240 0x1c");
    gfx(s, "gfx pal grey");
    stub_drain(s, 100000);
    memcpy(before, s->vram, GPU_VRAM_BYTES);
    memcpy(pal, s->gpal, sizeof(pal));
    reg = s->reg;
    s->check_tearing = 1;
    s->log_frames = 1;
    s->nflog = 0;
    snprintf(want, sizeof(want), " 0 missed: %s\n", GFX_CUBE_PICTURES == GFX_CUBE_FRAMES ? "all whole, in order" : "all whole");
    waits0 = G.irq_waits;
    wakes0 = G.irq_wakes;
    flips0 = s->flips_done;
    check(gfx(s, "gfx cube 2") == CMD_OK && said(s, "gfx cube: 2 s at 320x240 with depth, a FLIP a frame") &&
          said(s, "\n    cube 1 s: ") && said(s, "\n    cube 2 s: ") && said(s, want) && !said(s, "torn,") &&
          said(s, "display back as it was"), "gfx cube 2: a line a second, every frame checked whole, everything back");
    check(GFX_CUBE_PICTURES == GFX_CUBE_FRAMES || said(s, "of a turn's 128 frames are pictures of their own"),
          "  with repeated pictures in the turn, it says that their order is not checked");
    check(cube_summary(s, &drawn, &shown, &missed) && drawn >= 115 && drawn < shown && !missed,
          "  about 60 frames drawn a second (one a blanking), each shown and checked (none missed)");
    /* drawn: the FLIPs done after frame 0's (the review of 58ac7b3's M1: the FENCE frame 0's wait
       sent was counted as one); the video frames shown are more (the first shows frame 0); and the
       rate, timed from the blanking at which the cube's display began, at most the display's 59.94 */
    rate = cube_rate(s);
    snprintf(line, sizeof(line), "  %lu drawn: the FLIPs the engine did after frame 0's (%lu); %lu shown; %ld.%ld a second",
             drawn, s->flips_done - flips0 - 1, shown, rate / 10, rate % 10);
    check(drawn == s->flips_done - flips0 - 1 && rate >= 590 && rate <= 599, line);
    cube_at_the_pins(s, "the frames as they ended", 110, 110);
    check(s->shown_draws == 0, "  no engine word drew into the picture on the screen while it was shown");
    check(!memcmp(s->vram, before, GPU_VRAM_BYTES) && !memcmp(s->gpal, pal, sizeof(pal)) && s->reg.mode == reg.mode &&
          s->reg.base == reg.base && s->reg.stride == reg.stride && s->reg.scale == reg.scale,
          "  VRAM (all of it), GPAL and the display back as they were");
    check(G.t_w == 640 && G.t_h == 480 && G.t_stride == 640 && G.c_x1 == 640 && !G.z_valid && s->eng.tw == 640 &&
          s->eng.th == 480 && s->eng.cy1 == 480 && !s->eng.zvalid && !(p_status(s) & 1),
          "  the target and clip back in the driver and the engine, no depth buffer, the engine idle");
    check(s->writes_full == 0 && s->writes_blind == 0 && G.errors == 0 && G.timeouts == 0, "  the FIFO rule kept, no error");
    check(G.irq_waits > waits0 && G.irq_wakes - wakes0 == G.irq_waits - waits0,
          "  each fence wait it slept on the interrupt woken by it (frame 0's FLIP waits for a blanking)");
    /* from a 320x240 picture with a depth buffer: it comes back too */
    gfx(s, "gfx mode gfx 2");
    gfx(s, "gfx zbuf 153600 640");
    gfx(s, "gfx zclear 0x33");
    s->log_frames = 0;
    check(gfx(s, "gfx cube 1") == CMD_OK && G.z_valid && G.z_base == 153600 && s->eng.zvalid && s->eng.zbase == 153600 &&
          s->vram[153600] == 0x33 && s->vram[GPU_VRAM_BYTES - 1] == 0x33 && s->reg.scale == 2 && s->eng.tw == 320,
          "gfx cube 1 from a 320x240 picture with a depth buffer: its depths and the depth buffer back");
    check(gfx(s, "gfx cube 0") == CMD_USAGE && gfx(s, "gfx cube 4000") == CMD_USAGE && gfx(s, "gfx cube x") == CMD_USAGE &&
          gfx(s, "gfx cube 1 2") == CMD_USAGE, "gfx cube 0, 4000, x, 1 2: usage");
    free(before);
    done(s);
}
TEST(t_cube)

/* how it ends: Ctrl+C, an engine that stops (and Ctrl+C before 2 s could say so, N1), the pixel
   clock gone (a FLIP that never comes), a torn frame, no memory for VRAM, an engine error (L2): each
   with everything put back and the engine idle */
static void cube_end(struct stub *s, const char *what, const uint8_t *before, const uint16_t *pal, const struct stub_disp *reg) {
    char line[160];
    snprintf(line, sizeof(line), "  %s: VRAM, GPAL, the display, the target and the clip back, the engine idle", what);
    check(!memcmp(s->vram, before, GPU_VRAM_BYTES) && !memcmp(s->gpal, pal, 512) && s->reg.mode == reg->mode &&
          s->reg.base == reg->base && s->reg.stride == reg->stride && s->reg.scale == reg->scale && G.t_w == 640 &&
          G.c_y1 == 480 && (s->stopped || (s->eng.tw == 640 && s->eng.th == 480 && !(p_status(s) & 1))), line);
    if(s->stopped) {                                    /* the engine back: the target and clip queued for it */
        s->stopped = 0;
        stub_drain(s, 1000);
        check(s->eng.tw == 640 && s->eng.th == 480 && s->eng.cy1 == 480 && !(p_status(s) & 1),
              "  the engine back: the target and clip it was sent are the driver's, and it is idle");
    }
}

/* the error ending (the review of 58ac7b3's L2: with the STATUS check after each frame gone, the
   host test still passed): frame 30's SET_ZBUF stride arriving as 642, which the engine refuses
   (BAD_PARAM). The word is found in one run and spoiled in a second, the same up to it */
static struct stub *cube_error_setup(void) {
    struct stub *s = fresh2(1);
    gfx(s, "gfx mode gfx 1");
    gfx(s, "gfx clear 0x92");
    gfx(s, "gfx pal grey");
    stub_drain(s, 100000);
    return s;
}

static void cube_error(void) {
    struct stub *s = cube_error_setup();
    unsigned i, n0 = s->ncmd, w0 = s->cmd_writes, seen = 0, at = 0;
    unsigned long drawn = 0, shown = 0, missed = 0;
    uint8_t *before = malloc(GPU_VRAM_BYTES);
    uint16_t pal[256];
    struct stub_disp reg;
    char line[200];
    int r, sum;
    gfx(s, "gfx cube 1");
    for(i = n0; i + 2 < s->ncmd; i++)
        if(s->cmd[i] == GPU_HDR(GPU_OP_SET_ZBUF, 0) && s->cmd[i + 1] == 153600u && s->cmd[i + 2] == 640u && ++seen == 31) {
            at = i + 2 - n0 + 1;                        /* frame 30's stride: the run's at-th CMD word */
            break;
        }
    check(at && s->cmd_writes - w0 == s->ncmd - n0, "gfx cube 1: frame 30's SET_ZBUF found among its words");
    done(s);
    s = cube_error_setup();
    memcpy(before, s->vram, GPU_VRAM_BYTES);
    memcpy(pal, s->gpal, sizeof(pal));
    reg = s->reg;
    s->corrupt_value_at = s->cmd_writes + at;
    s->corrupt_value = 642;
    r = gfx(s, "gfx cube 1");
    sum = cube_summary(s, &drawn, &shown, &missed);
    check(r == CMD_FAIL && count(s->out, "the GPU reports:") == 1 && said(s, "BAD_PARAM: parameters refused") &&
          said(s, "the engine ABORTed") && said(s, "display back as it was") && G.errors == 1,
          "frame 30's SET_ZBUF refused (BAD_PARAM): said once, the run fails");
    snprintf(line, sizeof(line), "  it ends at that frame: %lu drawn (the FLIPs after frame 0's), none after, no frame torn", drawn);
    check(sum && drawn >= 29 && drawn <= 31 && !said(s, "torn,") && !said(s, "cube 1 s:"), line);
    cube_end(s, "an engine error", before, pal, &reg);
    check(!G.z_valid && !s->eng.zvalid && !(s->eng.sticky & (GPU_ST_BAD_OPCODE | GPU_ST_BAD_PARAM)),
          "  no depth buffer left, the sticky error cleared");
    free(before);
    done(s);
}

static int once_t_cube_end;
static void t_cube_end(unsigned seed) {
    struct stub *s;
    uint8_t *before;
    uint16_t pal[256];
    struct stub_disp reg;
    uint64_t t0, intr;
    char line[160];
    (void)seed;
    if(once_t_cube_end++) return;     /* nothing here depends on the seed: once */
    s = fresh2(1);
    before = malloc(GPU_VRAM_BYTES);
    gfx(s, "gfx mode gfx 1");
    gfx(s, "gfx clear 0x92");
    gfx(s, "gfx pal grey");
    stub_drain(s, 100000);
    memcpy(before, s->vram, GPU_VRAM_BYTES);
    memcpy(pal, s->gpal, sizeof(pal));
    reg = s->reg;
    /* Ctrl+C */
    s->intr_at = intr = s->now + 700000;
    check(gfx(s, "gfx cube 5") == CMD_FAIL && said(s, "gfx cube: ended by Ctrl+C") && said(s, "display back as it was") &&
          !said(s, "a stopped engine"), "gfx cube 5, Ctrl+C after 0.7 s: ends, says so");
    snprintf(line, sizeof(line), "  over %llu ms after the Ctrl+C", (unsigned long long)((s->now - intr) / 1000));
    check(s->now - intr < 200000, line);
    cube_end(s, "Ctrl+C", before, pal, &reg);
    s->intr_at = 0;
    /* the engine stops */
    s->stop_at = s->now + 800000;
    t0 = s->now;
    check(gfx(s, "gfx cube 5") == CMD_FAIL && count(s->out, "a stopped engine") == 1 &&
          said(s, "took no command word for 2 s: a stopped engine") && said(s, "display back as it was"),
          "the engine stops 0.8 s into gfx cube 5: said once (its FIFO stays full), it ends");
    check(s->now - t0 < 3500000, "  no second timeout");
    cube_end(s, "a stopped engine", before, pal, &reg);
    s->stopped = 0;
    /* the engine stops, Ctrl+C 0.1 s later (N1) */
    s->stop_at = s->now + 800000;
    s->intr_at = intr = s->now + 900000;
    check(gfx(s, "gfx cube 5") == CMD_FAIL && said(s, "ended by Ctrl+C") && count(s->out, "a stopped engine") == 1 &&
          said(s, "for 50 ms: a stopped engine"), "the engine stops, Ctrl+C 0.1 s later: the stopped engine said after 50 ms");
    snprintf(line, sizeof(line), "  over %llu ms after the Ctrl+C, not 2 s", (unsigned long long)((s->now - intr) / 1000));
    check(s->now - intr < 300000, line);
    cube_end(s, "Ctrl+C on a stopped engine", before, pal, &reg);
    s->stopped = 0;
    s->intr_at = 0;
    /* the pixel clock stops: no blanking, no FLIP done, FRAME_COUNT stays */
    s->frames_stop_at = s->now + 800000;
    t0 = s->now;
    check(gfx(s, "gfx cube 5") == CMD_FAIL && said(s, "a stopped engine") && said(s, "no vertical blanking in 250 ms"),
          "the pixel clock stops 0.8 s in: the FLIP never comes, a stopped engine, the display put back without a blanking");
    snprintf(line, sizeof(line), "  a bounded wait: over %llu ms after it began", (unsigned long long)((s->now - t0) / 1000));
    check(s->now - t0 < 4000000, line);
    cube_end(s, "no pixel clock", before, pal, &reg);
    s->frames_stop = 0;
    check(gfx(s, "gfx fill 0 0 1 1 0x92") == CMD_OK, "  the engine back afterwards");
    stub_drain(s, 100000);
    memcpy(before, s->vram, GPU_VRAM_BYTES);
    /* a byte of the picture on the screen spoiled 1 s in: a torn frame */
    s->poke_at = s->now + 700000;
    s->poke_off = 120 * 320 + 160;
    check(gfx(s, "gfx cube 1") == CMD_FAIL && said(s, "1 torn, 0 out of order") && said(s, "gfx cube: the first torn: frame ") &&
          said(s, "display back as it was"), "a byte of the picture shown spoiled: one frame torn, said, the run fails");
    cube_end(s, "a torn frame", before, pal, &reg);
    /* no memory to keep VRAM */
    s->no_memory = 1;
    check(gfx(s, "gfx cube 1") == CMD_OK && said(s, "no memory to keep the 307200 bytes of VRAM it draws on") &&
          s->reg.mode == reg.mode && s->reg.base == reg.base && s->reg.scale == reg.scale && !memcmp(s->gpal, pal, sizeof(pal)),
          "no memory to keep VRAM: said, it runs, the display and palette back");
    free(before);
    done(s);
    cube_error();
}
TEST(t_cube_end)

/* the way in and out (as t_demo_order for the demo): every CPU write of VRAM or GPAL while the
   screen shows text, and every engine word that draws while it shows text or the cube's own pages */
static int once_t_cube_order;
static void t_cube_order(unsigned seed) {
    struct stub *s;
    (void)seed;
    if(once_t_cube_order++) return;     /* nothing here depends on the seed: once */
    s = fresh2(1);
    gfx(s, "gfx mode gfx 1");
    gfx(s, "gfx clear 0x92");
    gfx(s, "gfx pal grey");
    s->watch = 1;
    check(gfx(s, "gfx cube 1") == CMD_OK && s->reg.mode == 2 && s->reg.stride == 640, "gfx cube 1 from a 640x480 picture");
    s->watch = 0;
    demo_order(s, "gfx cube 1", 512, 76800);
    gfx(s, "gfx mode overlay 2");
    s->watch = 1;
    s->intr_at = s->now + 500000;
    check(gfx(s, "gfx cube 3") == CMD_FAIL && s->reg.mode == 3 && s->reg.scale == 2, "gfx cube 3 from the overlay, Ctrl+C");
    s->intr_at = 0;
    s->watch = 0;
    demo_order(s, "Ctrl+C", 512, 76800);
    done(s);
}
TEST(t_cube_order)

/* Each line printed costing the CPU 80 ms (on SurfCPU a console line costs about three frames, up
   to five: the console compares its whole screen, the UART takes its bytes): with the looks from
   the timer's tick every frame shown is checked all the same, and with the frames recorded ahead
   the tick keeps the engine fed meanwhile, so the cube turns on at a frame a frame (the re-review of
   e655c39's low 1: a line held the picture four to five frames once a second). On a port without
   the tick the run looks from its loop, and frames pass unseen while it prints, which its lines say
   (missed), and then it fails: a frame not looked at was not checked (the review of 58ac7b3's L1).
   And test-gpuos's rule of the interrupt holds for a run as long (the review's L3): interrupts for
   half the fences that asked for one at least; the cube's frames' FENCEs, which ask for none, are
   counted in fences alone */
static int once_t_cube_look;
static void t_cube_look(unsigned seed) {
    struct stub *s;
    unsigned long drawn = 0, shown = 0, missed = 0, asked, all;
    unsigned irqs;
    long rate = 0;
    char line[200], want[80];
    int r, ok;
    (void)seed;
    if(once_t_cube_look++) return;     /* nothing here depends on the seed: once */
    s = fresh2(1);
    s->line_us = 80000;
    s->log_frames = 1;
    irqs = s->interrupts_sent;
    asked = G.irq_fences;
    all = G.fences;
    check(gfx(s, "gfx cube 3") == CMD_OK && cube_summary(s, &drawn, &shown, &missed) && s->ticks_run > 250,
          "gfx cube 3 with a slow console and the timer's tick");
    snprintf(line, sizeof(line), "  %lu drawn, %lu shown, %lu missed: every frame looked at, a frame a frame drawn", drawn,
             shown, missed);
    check(!missed && shown >= 170 && drawn >= 177, line);
    cube_at_the_pins(s, "and at the pins", 170, 120);
    irqs = s->interrupts_sent - irqs;
    asked = G.irq_fences - asked;
    all = G.fences - all;
    snprintf(line, sizeof(line), "  the interrupt: %u taken for the %lu fences that asked for one (of %lu sent: the cube's "
             "%lu frames' FENCEs ask for none)", irqs, asked, all, drawn + 1);
    check(asked > 0 && 2 * irqs >= asked && all >= asked + drawn + 1, line);
    done(s);
    /* the rate with the slow console: its time ends at the look that saw the last FLIP done, not
       after the line the run was printing then (on the RTL SoC the second's line ends a 1 s run,
       and the rate read 56.8 for a FLIP every blanking) */
    s = fresh2(1);
    s->line_us = 40000;
    r = gfx(s, "gfx cube 1");
    rate = cube_rate(s);
    ok = cube_summary(s, &drawn, &shown, &missed);
    snprintf(line, sizeof(line), "gfx cube 1 with the slow console: %lu drawn, %ld.%ld a second (59.0 to 59.9 wanted)", drawn,
             rate / 10, rate % 10);
    check(r == CMD_OK && ok && drawn >= 58 && rate >= 590 && rate <= 599, line);
    done(s);
    s = fresh2(-1);
    s->line_us = 40000;
    r = gfx(s, "gfx cube 3");
    ok = cube_summary(s, &drawn, &shown, &missed);
    check(r == CMD_FAIL && ok && !s->ticks_run, "the same on a port without the tick: the run looks from its loop, and fails");
    snprintf(want, sizeof(want), "    gfx cube: %lu shown, %lu missed: 0 torn, 0 out of order\n", shown, missed);
    snprintf(line, sizeof(line), "  %lu shown, %lu missed: frames passed unseen while its lines printed, said, no whole verdict",
             shown, missed);
    check(missed >= 2 && said(s, want), line);
    done(s);
}
TEST(t_cube_look)
