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

static void demo_order(struct stub *s, const char *what, unsigned gpal_writes) {
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
    check(cpu >= 39680 && gpal == gpal_writes && eng > 0, line);
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
    demo_order(s, "gfx demo 1", 512);
    gfx(s, "gfx mode overlay 2");
    s->watch = 1;
    s->intr_at = s->now + 400000;
    check(gfx(s, "gfx demo 3") == CMD_FAIL && s->reg.mode == 3 && s->reg.scale == 2, "gfx demo 3 from the overlay, Ctrl+C");
    s->intr_at = 0;
    s->watch = 0;
    demo_order(s, "Ctrl+C", 512);
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
        demo_order(s, "a stopped engine", from_gfx ? 512 : 0);
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
          said(s, "interrupts: 0 (intc source 5, IE.FENCE)\n") && said(s, "errors 0, timeouts 0, ABORTs 0\n") &&
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
    G.words = G.fences = G.irqs = G.timeouts = G.aborts = G.errors = 4294967295ul;
    G.overflows = G.bad_opcodes = G.bad_params = G.display_changes = G.latch_misses = 4294967295ul;
    G.fifo_depth = 65535;
    G.fence_sent = s->eng.fence = 4294967290u;          /* FENCE at 10 digits (the driver's and the engine's) */
    s->frames_stop = 1;
    s->frames_frozen = 65535;                           /* FRAME_COUNT's 16 bits */
    check(gfx(s, "gfx") == CMD_OK && said(s, "    sent: 4294967295 words, 4294967295 fences; FENCE 4294967290\n") &&
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
