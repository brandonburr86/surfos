/*
SurfGPU surf_video graphics testbench
--------------------
File: test_video_gfx.cpp Date: 10/18/26 (SurfGPU), 10/25/26 (GPU2: the FLIP port)
--------------------
Verilator harness for surf_video built with HAS_GFX = 1 (inside sim/io/tb_video.v, which adds
stand-ins for the text RAM and for surf_vram's pixel port), with VgaCapture on the pins and the
C model (sim/models/gpu_model.c) for what they should show. Checks (docs/GPU.md section 3):
  * the new registers: reset values, field masks, read-only ones, GPAL's RGB332 contents and
    mask, the offsets around them without a register, partial words and fetches refused;
  * FRAME_CRC in every mode: the test pattern (the model's 0x2077DB89), text, a black picture,
    graphics; each equal to the model's CRC and to the CRC of the frame captured at the pins;
  * MODE 2 with random VRAM and a random GPAL, pixel by pixel against the model, at scale 1
    and 2, with bases and strides that run past 300 KiB and wrap at 512 KiB;
  * MODE 3: random text over graphics (backgrounds 0 and not), the cursor;
  * GFX_BASE, GFX_STRIDE and GFX_SCALE written in mid-frame change the next frame wholly and
    never the current one; a GPAL write applies as it arrives;
  * FRAME_COUNT counts every frame, and a FRAME_CRC read after VBLANK_EV is that frame's;
  * FRAME_COUNT then FRAME_CRC is a consistent pair even with frames shown between the two reads;
  * CTRL.MODE and GFX_BASE written together in the vertical blanking change the same frame;
    written on either side of the frame latch (line 524) they split for one frame (GPU.md 4.1);
  * a reset of the pixel domain alone: the count restarts, the settings come back over the
    crossing;
  * GPU2's FLIP port (docs/GPU.md 16.7, tb_video built with FLIP_PORT = 1): a write through it reads
    back as GFX_BASE; in mid-frame it shows from the next frame, wholly; at the start of a vertical
    blanking, where FLIP writes, from the frame after that blanking; a CPU store to GFX_BASE in the
    same cycle wins, and a port write a cycle later wins over the store.
At the board's clocks or, by seed, other ratios for the crossings.

    Vtb_video --font driver/font8x16.c --out DIR [--seed N]
*/
#include "Vtb_video.h"
#include "verilated.h"

#include "gpu_model.h"
#include "surfbus_master.h"
#include "surfcpu/soc.h"
#include "vga_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
        }                                                                  \
    } while (0)

static const uint32_t B = SURF_VIDEO_BASE;
enum { CTRL_EN = 1, MODE_BARS = 0 << 1, MODE_TEXT = 1 << 1, MODE_GFX = 2 << 1, MODE_OVER = 3 << 1, CUR_EN = 1 << 3 };

struct Tb {
    Vtb_video *top;
    VgaCapture cap;
    uint64_t sys_half, pix_half, next_sys, next_pix;
    uint64_t sys_cycles = 0, pix_cycles = 0;
    // the FLIP port: a write at the next clk edge (flip_pending), or at the edge of the CPU's next
    // store to GFX_BASE (flip_with_store: the same cycle)
    bool flip_pending = false, flip_with_store = false;
    uint32_t flip_value = 0;
    uint64_t flip_writes = 0;
    Tb(Vtb_video *t, uint64_t sys_ps, uint64_t pix_ps, uint64_t phase_ps)
        : top(t), sys_half(sys_ps / 2), pix_half(pix_ps / 2), next_sys(sys_ps / 2), next_pix(phase_ps + pix_ps / 2) {}
    void posedge() {
        for (;;) {
            const uint64_t tn = std::min(next_sys, next_pix);
            bool sys_rise = false, pix_rise = false;
            if (next_sys == tn) {
                if (!top->clk) {                          // a clk edge comes: the port's input for it
                    const bool store = top->s_req_valid && top->s_req_write &&
                                       (top->s_req_addr & 0xFFFu) == SURF_VIDEO_GFX_BASE;
                    top->gfx_base_we = flip_pending || (flip_with_store && store);
                    top->gfx_base_in = flip_value;
                    if (top->gfx_base_we) flip_writes++;
                    if (store) flip_with_store = false;
                    flip_pending = false;
                }
                top->clk = !top->clk;
                sys_rise = top->clk;
                next_sys += sys_half;
            }
            if (next_pix == tn) {
                top->pix_clk = !top->pix_clk;
                pix_rise = top->pix_clk;
                next_pix += pix_half;
            }
            top->eval();
            if (pix_rise) {
                pix_cycles++;
                cap.tick(top->vga_r, top->vga_g, top->vga_b, top->vga_hs, top->vga_vs);
            }
            if (sys_rise) {
                sys_cycles++;
                return;
            }
        }
    }
};

using Bus = SurfBusMaster<Vtb_video>;

static VgaFont g_font;
static std::string g_out;
static std::vector<uint8_t> g_vram(GPU_VRAM_BYTES, 0);   // the harness's copy of the VRAM stand-in
static std::vector<uint32_t> g_text(2400, 0);            // ... and of text RAM cells 0..2399
static struct gpu_display g_d;                           // the settings as the registers hold them
static std::vector<uint16_t> g_frame(GPU_SCREEN_W * GPU_SCREEN_H);

static void vram_word(Tb &tb, Bus &bus, uint32_t w, uint32_t v) {
    bus.idle(0);
    tb.top->vram_we = 1;
    tb.top->vram_waddr = w;
    tb.top->vram_wdata = v;
    tb.posedge();
    tb.top->vram_we = 0;
    for (int k = 0; k < 4; k++) g_vram[4 * w + k] = (uint8_t)(v >> (8 * k));
}

static void text_cell(Tb &tb, Bus &bus, int i, uint32_t v) {
    bus.idle(0);
    tb.top->tram_we = 1;
    tb.top->tram_waddr = i;
    tb.top->tram_wdata = v;
    tb.posedge();
    tb.top->tram_we = 0;
    g_text[i] = v;
}

static void wait_frames(Tb &tb, Bus &bus, uint64_t n) {
    const uint64_t target = tb.cap.frames() + n;
    while (tb.cap.frames() < target) bus.idle(2000);
}

static void wait_frame_edge(Tb &tb, Bus &bus, uint64_t frames) {
    while (tb.cap.frames() < frames) bus.idle(64);
}

static void sync_to_line(Tb &tb, Bus &bus, int64_t line) {
    const uint64_t f = tb.cap.frames();
    while (tb.cap.frames() == f) bus.idle(16);
    while (tb.cap.line() < line) bus.idle(16);
}

static void render() {
    gpu_display_render(&g_d, g_vram.data(), g_text.data(), (const uint8_t(*)[16])g_font.rows, g_frame.data());
}

static uint32_t captured_crc(const Tb &tb) { return gpu_frame_crc(tb.cap.frame().data()); }

// the last captured frame against the model's: pixel by pixel
static int compare(Tb &tb, const char *what) {
    render();
    int bad = 0, fx = -1, fy = -1;
    for (int y = 0; y < GPU_SCREEN_H; y++)
        for (int x = 0; x < GPU_SCREEN_W; x++)
            if (tb.cap.pixel(x, y) != g_frame[y * GPU_SCREEN_W + x]) {
                if (!bad) { fx = x; fy = y; }
                bad++;
            }
    CHECK(bad == 0, "%s: %d pixels differ, first at (%d,%d): %03x, want %03x", what, bad, fx, fy,
          fx >= 0 ? tb.cap.pixel(fx, fy) : 0, fx >= 0 ? g_frame[fy * GPU_SCREEN_W + fx] : 0);
    return bad;
}

// FRAME_CRC after the next VBLANK_EV, the frame captured meanwhile, and the model's CRC
static void wait_vblank_ev(Bus &bus) {
    bus.wr(B + SURF_VIDEO_STATUS, SURF_VIDEO_STATUS_VBLANK_EV_MASK);
    while (!(bus.rd(B + SURF_VIDEO_STATUS) & SURF_VIDEO_STATUS_VBLANK_EV_MASK)) bus.idle(50);
}

static void check_crc(Tb &tb, Bus &bus, const char *what, uint32_t expect) {
    wait_vblank_ev(bus);                                  // just after a blanking start: far from the next count
    bus.wr(B + SURF_VIDEO_STATUS, SURF_VIDEO_STATUS_VBLANK_EV_MASK);
    const uint32_t c0 = bus.rd(B + SURF_VIDEO_FRAME_COUNT);
    while (!(bus.rd(B + SURF_VIDEO_STATUS) & SURF_VIDEO_STATUS_VBLANK_EV_MASK)) bus.idle(50);
    const uint32_t c1 = bus.rd(B + SURF_VIDEO_FRAME_COUNT);       // the pair: the count, then its CRC
    const uint32_t crc = bus.rd(B + SURF_VIDEO_FRAME_CRC);
    const uint64_t f = tb.cap.frames();                  // the capture closes the frame at its vsync
    while (tb.cap.frames() == f) bus.idle(64);
    const uint32_t cap = captured_crc(tb);
    render();
    const uint32_t model = gpu_frame_crc(g_frame.data());
    CHECK(crc == cap && crc == model && (!expect || crc == expect),
          "%s: FRAME_CRC %08x, captured %08x, model %08x%s", what, crc, cap, model, expect ? " (fixed value)" : "");
    CHECK(((c1 - c0) & 0xFFFF) == 1, "%s: FRAME_COUNT %u -> %u over one frame", what, c0, c1);
}

// ------------------------------------------------------------------ tests

static void test_registers(Tb &tb, Bus &bus) {
    (void)tb;
    CHECK(bus.rd(B + SURF_VIDEO_GFX_BASE) == 0 && bus.rd(B + SURF_VIDEO_GFX_STRIDE) == 640 &&
          bus.rd(B + SURF_VIDEO_GFX_SCALE) == 1, "GFX_ after reset: %x %x %x", bus.rd(B + SURF_VIDEO_GFX_BASE),
          bus.rd(B + SURF_VIDEO_GFX_STRIDE), bus.rd(B + SURF_VIDEO_GFX_SCALE));
    int bad = 0;
    for (unsigned n = 0; n < 256; n++) bad += bus.rd(B + SURF_VIDEO_GPAL + 4 * n) != gpu_gpal_initial(n);
    CHECK(bad == 0, "%d GPAL entries differ from RGB332 at start", bad);
    bus.wr(B + SURF_VIDEO_GFX_BASE, 0xFFFFFFFF);
    bus.wr(B + SURF_VIDEO_GFX_STRIDE, 0xFFFFFFFF);
    bus.wr(B + SURF_VIDEO_GFX_SCALE, 0xFFFFFFFF);
    CHECK(bus.rd(B + SURF_VIDEO_GFX_BASE) == 0x7FFFC && bus.rd(B + SURF_VIDEO_GFX_STRIDE) == 0xFFFC &&
          bus.rd(B + SURF_VIDEO_GFX_SCALE) == 3, "GFX_ masks: %x %x %x", bus.rd(B + SURF_VIDEO_GFX_BASE),
          bus.rd(B + SURF_VIDEO_GFX_STRIDE), bus.rd(B + SURF_VIDEO_GFX_SCALE));
    bus.wr(B + SURF_VIDEO_GFX_BASE, 0);
    bus.wr(B + SURF_VIDEO_GFX_STRIDE, 640);
    bus.wr(B + SURF_VIDEO_GFX_SCALE, 1);
    const uint32_t crc = bus.rd(B + SURF_VIDEO_FRAME_CRC), cnt = bus.rd(B + SURF_VIDEO_FRAME_COUNT);
    bus.wr(B + SURF_VIDEO_FRAME_CRC, ~crc);
    bus.wr(B + SURF_VIDEO_FRAME_COUNT, 0xFFFF);
    CHECK(bus.rd(B + SURF_VIDEO_FRAME_COUNT) == cnt && cnt <= 0xFFFF, "FRAME_COUNT is read only");
    bus.wr(B + SURF_VIDEO_GPAL + 4 * 200, 0xFFFFFABC);
    CHECK(bus.rd(B + SURF_VIDEO_GPAL + 4 * 200) == 0xABC, "GPAL mask");
    bus.wr(B + SURF_VIDEO_GPAL + 4 * 200, gpu_gpal_initial(200));
    const uint32_t holes[] = {0x094, 0x098, 0x0A0, 0x0FC, 0x3FC, 0x800, 0x804, 0xFFC};
    for (uint32_t off : holes)
        CHECK(bus.read(B + off).err && bus.write(B + off, 0).err, "offset %03x not an error", off);
    const uint32_t regs[] = {SURF_VIDEO_GFX_BASE, SURF_VIDEO_GFX_STRIDE, SURF_VIDEO_GFX_SCALE, SURF_VIDEO_FRAME_CRC,
                             SURF_VIDEO_FRAME_COUNT, SURF_VIDEO_GPAL, SURF_VIDEO_GPAL + 4 * 255};
    for (uint32_t r : regs) {
        CHECK(bus.write(B + r, 0x1234, 0x3).err && bus.read(B + r, 0x1).err && bus.fetch(B + r).err,
              "partial access or fetch to %03x not an error", r);
    }
    CHECK(bus.rd(B + SURF_VIDEO_GPAL + 4 * 255) == 0xFFF && bus.rd(B + SURF_VIDEO_GFX_STRIDE) == 640,
          "a refused access changed a register");
    // GPAL readable back to back with other registers (its answer comes from the block RAM)
    bad = 0;
    for (unsigned n = 0; n < 16; n++) {
        bad += bus.rd(B + SURF_VIDEO_GPAL + 4 * n) != gpu_gpal_initial(n);
        bad += bus.rd(B + SURF_VIDEO_PIXCLK) != SURF_PIX_HZ;
    }
    CHECK(bad == 0, "GPAL reads interleaved with PIXCLK: %d wrong", bad);
}

static void test_fixed_crcs(Tb &tb, Bus &bus, std::mt19937 &rng) {
    // the test pattern: the reset mode
    check_crc(tb, bus, "test pattern", 0x2077DB89u);
    // black
    bus.wr(B + SURF_VIDEO_CTRL, MODE_GFX);
    g_d.enable = 0;
    g_d.mode = 2;
    wait_frames(tb, bus, 1);
    check_crc(tb, bus, "ENABLE = 0", 0xC656B350u);
    // text: random cells, CGA
    for (int i = 0; i < 2400; i++)
        text_cell(tb, bus, i, (rng() & 0xFFFF0000u) | (rng() % 16) << 12 | (rng() % 16) << 8 | (i & 0xFF));
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
    g_d.enable = 1;
    g_d.mode = 1;
    wait_frames(tb, bus, 2);
    compare(tb, "text with HAS_GFX");
    check_crc(tb, bus, "text", 0);
}

static void random_gpal(Bus &bus, std::mt19937 &rng) {
    for (unsigned n = 0; n < 256; n++) {
        const uint16_t c = (uint16_t)(rng() & 0xFFF);
        bus.wr(B + SURF_VIDEO_GPAL + 4 * n, c);
        g_d.gpal[n] = c;
    }
}

static void set_gfx(Bus &bus, uint32_t base, uint32_t stride, uint32_t scale) {
    bus.wr(B + SURF_VIDEO_GFX_BASE, base);
    bus.wr(B + SURF_VIDEO_GFX_STRIDE, stride);
    bus.wr(B + SURF_VIDEO_GFX_SCALE, scale);
    g_d.gfx_base = base;
    g_d.gfx_stride = stride;
    g_d.gfx_scale = scale;
}

static void test_graphics(Tb &tb, Bus &bus, std::mt19937 &rng) {
    for (uint32_t w = 0; w < GPU_VRAM_BYTES / 4; w++) vram_word(tb, bus, w, rng());
    random_gpal(bus, rng);
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_GFX);
    g_d.enable = 1;
    g_d.mode = 2;
    struct Cfg { uint32_t base, stride, scale; const char *what; };
    const Cfg cfgs[] = {
        {0, 640, 1, "640x480 at 0"},
        {4 * (rng() % 1000), 640 + 4 * (rng() % 64), 1, "640x480, random base and stride"},
        {0, 320, 2, "320x240 doubled"},
        {153600, 320, 2, "the second 320x240 page"},
        {307200 - 640 * 50, 640, 1, "lines past 300 KiB show entry 0"},
        {0x7FFFC, 0xFFFC, 1, "base and stride at their maximum (wraps at 512 KiB)"},
        {1000, 0, 2, "stride 0: every line the same"},
        {8, 1288, 0, "scale 0 acts as 1"},
    };
    for (const Cfg &c : cfgs) {
        set_gfx(bus, c.base, c.stride, c.scale);
        wait_frames(tb, bus, 2);
        compare(tb, c.what);
    }
    set_gfx(bus, 4 * (rng() % 4096), 4 * (80 + rng() % 400), 2);
    check_crc(tb, bus, "graphics, doubled", 0);
    std::printf("graphics: %zu placements pixel by pixel against the model, FRAME_CRC equal to the model's and the "
                "pins'\n", sizeof cfgs / sizeof cfgs[0]);
}

static void test_overlay(Tb &tb, Bus &bus, std::mt19937 &rng) {
    for (int i = 0; i < 2400; i++) {
        const uint32_t bg = (rng() & 1) ? 0 : rng() % 16;
        text_cell(tb, bus, i, bg << 12 | (rng() % 16) << 8 | (rng() & 0xFF));
    }
    const int ccol = (int)(rng() % 80), crow = (int)(rng() % 30);
    bus.wr(B + SURF_VIDEO_CURSOR, (uint32_t)(crow << 8 | ccol));
    bus.wr(B + SURF_VIDEO_SCROLL, 7);
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_OVER | CUR_EN);
    set_gfx(bus, 0, 640, 1);
    g_d.mode = 3;
    g_d.cursor = 1;
    g_d.cur_col = ccol;
    g_d.cur_row = crow;
    g_d.scroll = 7;
    wait_frames(tb, bus, 2);
    compare(tb, "text over graphics, cursor, scroll");
    check_crc(tb, bus, "overlay", 0);
    bus.wr(B + SURF_VIDEO_SCROLL, 0);
    g_d.scroll = 0;
}

// a placement change in mid-frame: that frame old, the next new; GPAL at once
static void test_latch(Tb &tb, Bus &bus) {
    const int64_t mid = 35 + 240;
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_GFX);
    g_d.mode = 2;
    g_d.cursor = 0;
    set_gfx(bus, 0, 640, 1);
    wait_frames(tb, bus, 2);
    struct gpu_display old_d = g_d;
    sync_to_line(tb, bus, mid);
    bus.wr(B + SURF_VIDEO_GFX_BASE, 64000);
    bus.wr(B + SURF_VIDEO_GFX_STRIDE, 320);
    bus.wr(B + SURF_VIDEO_GFX_SCALE, 2);
    uint64_t f0 = tb.cap.frames();
    CHECK(tb.cap.line() < 35 + 480, "the writes did not land inside the picture");
    wait_frame_edge(tb, bus, f0 + 1);
    g_d = old_d;
    compare(tb, "the frame GFX_ was written in");
    wait_frame_edge(tb, bus, f0 + 2);
    g_d.gfx_base = 64000;
    g_d.gfx_stride = 320;
    g_d.gfx_scale = 2;
    compare(tb, "the next frame, with the new placement");
    // GPAL in mid-frame: the bottom half of that frame changes already
    sync_to_line(tb, bus, mid);
    const uint8_t v_top = g_vram[64000], v_bot = g_vram[64000 + 239 * 320 + 319];
    bus.wr(B + SURF_VIDEO_GPAL + 4 * v_bot, 0x0C0);
    f0 = tb.cap.frames();
    wait_frame_edge(tb, bus, f0 + 1);
    (void)v_top;
    CHECK(tb.cap.pixel(639, 479) == 0x0C0, "a GPAL write in mid-frame showed only later (%03x)",
          tb.cap.pixel(639, 479));
    g_d.gpal[v_bot] = 0x0C0;
    wait_frame_edge(tb, bus, f0 + 2);
    compare(tb, "the frame after a GPAL write");
    std::printf("latch: GFX_BASE, GFX_STRIDE and GFX_SCALE written at line 240 change the next frame only; GPAL at "
                "once\n");
}

// FRAME_COUNT, then FRAME_CRC: the CRC is that of the frame counted, even when frames have passed
// between the two reads (the first GPU review: two loads could pair a count with another frame's
// CRC); a pair read back to back is the newest frame's
static void test_pair(Tb &tb, Bus &bus) {
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_GFX);
    g_d.mode = 2;
    g_d.cursor = 0;
    set_gfx(bus, 0, 640, 1);
    wait_frames(tb, bus, 2);
    for (int t = 0; t < 3; t++) {
        wait_vblank_ev(bus);                              // frame c has ended and its CRC crossed
        const uint32_t c = bus.rd(B + SURF_VIDEO_FRAME_COUNT);
        const uint64_t f = tb.cap.frames();
        while (tb.cap.frames() == f) bus.idle(64);        // the capture closes frame c at its vsync
        const uint32_t crc_c = captured_crc(tb);
        const uint8_t v = g_vram[0];                      // from the next frame on, another picture
        const uint16_t col = (uint16_t)((g_d.gpal[v] + 0x111u * (unsigned)(t + 1)) & 0xFFF);
        bus.wr(B + SURF_VIDEO_GPAL + 4 * v, col);
        g_d.gpal[v] = col;
        render();
        const uint32_t model = gpu_frame_crc(g_frame.data());
        wait_frames(tb, bus, 2);
        const uint32_t crc = bus.rd(B + SURF_VIDEO_FRAME_CRC);
        CHECK(crc == crc_c && crc_c != model, "FRAME_CRC two frames after FRAME_COUNT %u: %08x, frame %u's %08x "
              "(the newer frames' %08x)", c, crc, c, crc_c, model);
        const uint32_t c2 = bus.rd(B + SURF_VIDEO_FRAME_COUNT);
        const uint32_t crc2 = bus.rd(B + SURF_VIDEO_FRAME_CRC);
        CHECK(crc2 == model && ((c2 - c) & 0xFFFF) >= 2, "the next pair, back to back: count %u -> %u, FRAME_CRC "
              "%08x, the newest frame's %08x", c, c2, crc2, model);
        // other reads between the two halves (a STATUS poll, a GFX_BASE read) keep the pair, and a
        // FRAME_CRC read after the pair's is the newest again (the second review: both halves)
        const uint32_t c3 = bus.rd(B + SURF_VIDEO_FRAME_COUNT);
        const uint16_t col2 = (uint16_t)((g_d.gpal[v] + 0x222u) & 0xFFF);
        bus.wr(B + SURF_VIDEO_GPAL + 4 * v, col2);
        g_d.gpal[v] = col2;
        render();
        const uint32_t model2 = gpu_frame_crc(g_frame.data());
        wait_frames(tb, bus, 2);
        (void)bus.rd(B + SURF_VIDEO_STATUS);
        (void)bus.rd(B + SURF_VIDEO_GFX_BASE);
        const uint32_t crc3 = bus.rd(B + SURF_VIDEO_FRAME_CRC);
        CHECK(crc3 == model && model != model2, "FRAME_COUNT %u, a STATUS and a GFX_BASE read, then FRAME_CRC: %08x, "
              "the counted frame's %08x (the newest %08x)", c3, crc3, model, model2);
        const uint32_t crc4 = bus.rd(B + SURF_VIDEO_FRAME_CRC);
        CHECK(crc4 == model2, "a FRAME_CRC read after the pair's: %08x, the newest frame's %08x", crc4, model2);
    }
    std::printf("pair: FRAME_COUNT then FRAME_CRC two frames later gives the counted frame's CRC, also with other "
                "reads between; a later lone FRAME_CRC read the newest; 3 times\n");
}

// CTRL.MODE and GFX_BASE written one after the other: in the vertical blanking, after VBLANK_EV
// and well before the frame latch (line 524), they change the same frame (GPU.md section 4.1's
// rule); written on either side of the latch they split for one frame, as the rule says
static void test_mode_and_placement(Tb &tb, Bus &bus, std::mt19937 &rng) {
    const uint64_t window = 1300000000ull / tb.sys_half / 2;      // 1.3 ms of the 1.4 ms to the latch
    for (int t = 0; t < 4; t++) {
        bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
        set_gfx(bus, 0, 640, 1);
        g_d.mode = 1;
        wait_frames(tb, bus, 2);
        const uint32_t base = 4 * (1 + rng() % 20000);
        wait_vblank_ev(bus);
        bus.idle((int)(rng() % window));
        bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_GFX);
        bus.wr(B + SURF_VIDEO_GFX_BASE, base);
        const uint64_t f0 = tb.cap.frames();
        CHECK(tb.cap.line() >= 515 || tb.cap.line() < 33, "the writes left the vertical blanking (line %lld)",
              (long long)tb.cap.line());
        wait_frame_edge(tb, bus, tb.cap.line() >= 515 ? f0 + 2 : f0 + 1);  // the first frame after the latch
        g_d.mode = 2;
        g_d.gfx_base = base;
        compare(tb, "MODE and GFX_BASE written in the blanking: the next frame has both");
    }
    // on either side of the latch: MODE at line 523, GFX_BASE at line 1 of the frame
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_TEXT);
    set_gfx(bus, 0, 640, 1);
    g_d.mode = 1;
    wait_frames(tb, bus, 2);
    sync_to_line(tb, bus, 33);                            // capture line 33 = vcnt 523
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_GFX);
    while (tb.cap.line() < 36) bus.idle(16);              // vcnt 1: past the latch
    bus.wr(B + SURF_VIDEO_GFX_BASE, 6400);
    const uint64_t f0 = tb.cap.frames();
    wait_frame_edge(tb, bus, f0 + 1);
    g_d.mode = 2;
    compare(tb, "MODE before the latch, GFX_BASE after: that frame has the new mode and the old placement");
    wait_frame_edge(tb, bus, f0 + 2);
    g_d.gfx_base = 6400;
    compare(tb, "... and the next frame both");
    std::printf("mode and placement: written in the blanking, the same frame (4 times); across the latch, split for "
                "one frame\n");
}

// GPU2's FLIP port (docs/GPU.md 16.7): the engine's write into GFX_BASE, one clk cycle
static void flip_write(Tb &tb, Bus &bus, uint32_t word) {
    bus.idle(0);
    tb.flip_pending = true;
    tb.flip_value = word;
    tb.posedge();
}

static void test_flip_port(Tb &tb, Bus &bus, std::mt19937 &rng) {
    bus.wr(B + SURF_VIDEO_CTRL, CTRL_EN | MODE_GFX);
    g_d.mode = 2;
    g_d.cursor = 0;
    set_gfx(bus, 0, 320, 2);
    wait_frames(tb, bus, 2);
    // in mid-frame: that frame keeps the old base, the next shows the FLIP's, wholly
    const struct gpu_display old_d = g_d;
    sync_to_line(tb, bus, 35 + 240);
    flip_write(tb, bus, 76800 / 4);
    CHECK(bus.rd(B + SURF_VIDEO_GFX_BASE) == 76800, "GFX_BASE after a FLIP port write: %x",
          bus.rd(B + SURF_VIDEO_GFX_BASE));
    uint64_t f0 = tb.cap.frames();
    wait_frame_edge(tb, bus, f0 + 1);
    g_d = old_d;
    compare(tb, "the frame the FLIP port wrote GFX_BASE in");
    wait_frame_edge(tb, bus, f0 + 2);
    g_d.gfx_base = 76800;
    compare(tb, "the next frame, from the FLIP's base");
    // at the start of a vertical blanking, where FLIP writes: the frame after that blanking
    for (int t = 0; t < 3; t++) {
        const uint32_t base = 4 * (uint32_t)(rng() % 19000);
        wait_vblank_ev(bus);
        bus.idle((uint64_t)(rng() % 200));
        flip_write(tb, bus, base / 4);
        const uint64_t f1 = tb.cap.frames();
        CHECK(tb.cap.line() >= 515, "the FLIP port write left the blanking's start (line %lld)", (long long)tb.cap.line());
        wait_frame_edge(tb, bus, f1 + 1);
        compare(tb, "the frame before the blanking the FLIP port wrote in");
        wait_frame_edge(tb, bus, f1 + 2);
        g_d.gfx_base = base;
        compare(tb, "the frame after it, from the FLIP's base");
    }
    // a CPU store to GFX_BASE in the same cycle as a port write wins; a port write after it wins
    const uint64_t w0 = tb.flip_writes;
    tb.flip_with_store = true;
    tb.flip_value = 0x1000;
    bus.wr(B + SURF_VIDEO_GFX_BASE, 0x8000);
    CHECK(tb.flip_writes == w0 + 1 && bus.rd(B + SURF_VIDEO_GFX_BASE) == 0x8000,
          "a CPU store and a FLIP port write in one cycle: %llu port writes, GFX_BASE %x (want the store's 8000)",
          (unsigned long long)(tb.flip_writes - w0), bus.rd(B + SURF_VIDEO_GFX_BASE));
    flip_write(tb, bus, 0x1000);
    CHECK(bus.rd(B + SURF_VIDEO_GFX_BASE) == 0x4000, "a FLIP port write after the store: GFX_BASE %x",
          bus.rd(B + SURF_VIDEO_GFX_BASE));
    wait_frames(tb, bus, 2);
    g_d.gfx_base = 0x4000;
    compare(tb, "after the same-cycle store and the port write");
    set_gfx(bus, 0, 640, 1);
    std::printf("flip port: GFX_BASE written by the port in mid-frame shows from the next frame, at a blanking's "
                "start from the frame after it (3 times); a CPU store in the same cycle wins\n");
}

static void test_count_and_reset(Tb &tb, Bus &bus) {
    // FRAME_COUNT: one per frame over 5 frames
    const uint64_t f0 = tb.cap.frames();
    const uint32_t c0 = bus.rd(B + SURF_VIDEO_FRAME_COUNT);
    wait_frame_edge(tb, bus, f0 + 5);
    const uint32_t c1 = bus.rd(B + SURF_VIDEO_FRAME_COUNT);
    CHECK(((c1 - c0) & 0xFFFF) >= 4 && ((c1 - c0) & 0xFFFF) <= 6, "FRAME_COUNT %u -> %u over 5 frames", c0, c1);
    // a reset of the pixel domain alone
    tb.top->pix_rst = 1;
    bus.idle(300);
    tb.top->pix_rst = 0;
    bus.idle(50);
    CHECK(bus.rd(B + SURF_VIDEO_FRAME_COUNT) == 0, "FRAME_COUNT after a pixel reset: %u",
          bus.rd(B + SURF_VIDEO_FRAME_COUNT));
    wait_frames(tb, bus, 3);
    compare(tb, "after a pixel reset (the placement came back)");
    check_crc(tb, bus, "after a pixel reset", 0);
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    unsigned seed = 1;
    std::string font_path;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) g_out = argv[++i];
    }
    if (font_path.empty() || g_out.empty() || !g_font.load_c(font_path)) {
        std::printf("usage: %s --font driver/font8x16.c --out DIR [--seed N]\n", argv[0]);
        return 2;
    }
    std::mt19937 rng(seed);
    const uint64_t pix_ps = (1000000000000ull + SURF_PIX_HZ / 2) / SURF_PIX_HZ;
    const uint64_t sys_choices[] = {40000, 40000, 20000, 100000, 33333};
    const uint64_t sys_ps = sys_choices[seed % 5];
    auto top = std::make_unique<Vtb_video>();
    Tb tb(top.get(), sys_ps, pix_ps, rng() % pix_ps);
    top->clk = 0;
    top->pix_clk = 0;
    top->rst = 1;
    top->pix_rst = 1;
    top->s_req_valid = 0;
    top->tram_we = 0;
    top->vram_we = 0;
    top->gfx_base_we = 0;
    top->gfx_base_in = 0;
    top->eval();
    for (int i = 0; i < 8; i++) tb.posedge();
    top->rst = 0;
    top->pix_rst = 0;
    tb.posedge();
    Bus bus(top.get(), [&] { tb.posedge(); }, seed);
    bus.max_gap = (int)(seed % 3);
    gpu_display_reset(&g_d, 1);

    std::printf("clocks: sys %.3f MHz, pix %.4f MHz\n", 1e6 / sys_ps, 1e6 / pix_ps);
    test_registers(tb, bus);
    test_fixed_crcs(tb, bus, rng);
    test_graphics(tb, bus, rng);
    tb.cap.write_ppm(g_out + "/graphics.ppm");
    test_overlay(tb, bus, rng);
    tb.cap.write_ppm(g_out + "/overlay.ppm");
    test_latch(tb, bus);
    test_pair(tb, bus);
    test_mode_and_placement(tb, bus, rng);
    test_flip_port(tb, bus, rng);
    test_count_and_reset(tb, bus);
    CHECK(bus.problems.empty(), "SurfBus protocol: %s", bus.problems.empty() ? "" : bus.problems[0].c_str());
    top->final();
    if (failures) {
        std::printf("test_video_gfx: FAIL (%d of %d checks failed, seed %u)\n", failures, checks, seed);
        return 1;
    }
    std::printf("test_video_gfx: PASS (%d checks, seed %u, %llu frames)\n", checks, seed,
                (unsigned long long)tb.cap.frames());
    return 0;
}
