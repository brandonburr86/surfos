/*
SurfGPU engine testbench
--------------------
File: test_gpu.cpp Date: 10/18/26 (SurfGPU), 10/25/26 (GPU2)
--------------------
Verilator harness for surf_gpu with surf_vram (sim/gpu/tb_gpu.v) against the C model
(sim/models/gpu_model.c): every command word written to CMD also goes to the model, and at every
FENCE the harness waits for STATUS.BUSY = 0, then compares all 307,200 bytes of VRAM (through
VRAM's pixel port, so port A is left alone), FENCE and the sticky bits with the model's.
Directed tests (docs/GPU.md sections 4 and 5): the registers and their error rules, the target
and clip a reset leaves, every command at every x mod 4 alignment and size, clipping at each edge, zero and out-of-range sizes
and coordinates, SET_TARGET's refusals, copies overlapping in all eight directions with and
without the key, lines in every octant with their endpoints, EXPAND's bit order and
transparency, WRITE_RECT's byte order, a full FIFO and OVERFLOW, an unknown opcode, fences and
their interrupt, WAIT_VBLANK, ABORT. Then randomized command streams (targets, clips, every
command, coordinates near the edges and far outside) while the CPU keeps reading VRAM and
writing a part of it the engine never draws on, so the engine is stalled in every state.
GPU2 (docs/GPU.md section 16): SET_ZBUF's refusals and SET_TARGET dropping the depth buffer,
depth without a buffer refused; TRIANGLE at every lane alignment and sub-pixel offset in both
windings, the four kinds of edges through pixel centres, culling, zero areas, the clip's edges,
huge and far triangles; Gouraud and every combination of the depth flags, depth buffers
overlapping the target; FLIP against the blanking pulse; ABORT inside a TRIANGLE at random
moments (what it drew must be a prefix of the walk, its last group possibly without its colour);
the walk's cycles; then triangles, depth buffers, depth clears and FLIPs in the random streams.
Every FLIP write the engine makes must come in the cycle after a blanking pulse, in every test.

    Vtb_gpu [--seed N] [--chunks N]
*/
#include "Vtb_gpu.h"
#include "verilated.h"

#include "gpu_model.h"
#include "surfbus_master.h"
#include "surfcpu/soc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <random>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            if (failures < 25) {                                           \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__);           \
                std::printf(__VA_ARGS__);                                  \
                std::printf("\n");                                         \
            }                                                              \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static const uint32_t G = SURF_GPU_BASE, V = SURF_VRAM_BASE;
static const uint32_t SPARE = 303104;          // the engine never draws above this in the random part

struct Tb {
    Vtb_gpu *t;
    uint64_t cycles = 0;
    uint64_t vblank_every = 0, next_vblank = 0;
    uint64_t engine_grants = 0, engine_waits = 0;
    uint64_t trace = 0;                 // --trace N: print the engine's first N granted accesses
    bool pulse = false;
    // FLIP's writes into video's GFX_BASE: each must come in the cycle after a blanking pulse
    uint64_t flips = 0, flip_early = 0;
    uint32_t flip_last = 0;
    bool prev_vblank = false;
    void posedge() {
        const bool vb = pulse || (vblank_every && cycles >= next_vblank);
        t->vblank = vb;
        if (vblank_every && cycles >= next_vblank) next_vblank = cycles + vblank_every;
        pulse = false;
        t->clk = 0;
        t->eval();
        if (t->gfx_base_we) {
            flips++;
            flip_last = t->gfx_base_out;
            if (!prev_vblank) flip_early++;
        }
        prev_vblank = vb;
        if (t->m_req) { if (t->m_gnt) engine_grants++; else engine_waits++; }
        if (trace && t->m_req && t->m_gnt) {
            trace--;
            std::printf("  cycle %llu: engine %s word %05x be %x data %08x\n", (unsigned long long)cycles,
                        t->m_we ? "writes" : "reads", t->m_addr, t->m_be, t->m_wdata);
        }
        t->clk = 1;
        t->eval();
        cycles++;
    }
};
using Bus = SurfBusMaster<Vtb_gpu>;

static std::vector<uint8_t> g_vram(GPU_VRAM_BYTES);
static struct gpu_engine g_eng;
static std::mt19937 g_rng;
static uint64_t g_words = 0, g_cmds = 0, g_compares = 0, g_cpu_noise = 0;
static bool g_noise = false;
static uint64_t g_trace = 0;
static bool g_only_fill = false;

static uint32_t rd(Bus &b, uint32_t off) { return b.rd(G + off); }
static void wr(Bus &b, uint32_t off, uint32_t v) { b.wr(G + off, v); }

// one CMD word: wait for room, write it, give it to the model too
static void push(Bus &bus, uint32_t w) {
    int spins = 0;
    while (rd(bus, SURF_GPU_FIFO_FREE) == 0) {
        bus.idle(4);
        if (++spins > 2000000) { std::printf("FIFO never freed\n"); std::exit(3); }
    }
    wr(bus, SURF_GPU_CMD, w);
    gpu_engine_word(&g_eng, w);
    g_words++;
}
static void cmd(Bus &bus, std::initializer_list<uint32_t> ws) {
    for (uint32_t w : ws) push(bus, w);
    g_cmds++;
}

// the CPU busy on VRAM meanwhile: reads anywhere, writes in the spare part (the model sees them)
static void cpu_noise(Bus &bus) {
    const uint32_t r = g_rng();
    if (r % 3 == 0) {
        bus.idle(1 + r % 5);
        return;
    }
    if (r % 7 == 0) {
        const uint32_t off = SPARE + 4 * (g_rng() % ((GPU_VRAM_BYTES - SPARE) / 4));
        const uint32_t v = g_rng();
        bus.wr(V + off, v);
        for (int k = 0; k < 4; k++) g_vram[off + k] = (uint8_t)(v >> (8 * k));
    } else {
        (void)bus.read(V + 4 * (g_rng() % (GPU_VRAM_BYTES / 4)));
    }
    g_cpu_noise++;
}

static bool wait_idle(Bus &bus, uint64_t limit = 40000000) {
    const uint64_t t0 = bus.requests;
    for (uint64_t n = 0; n < limit; n++) {
        if (!(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK)) return true;
        if (g_noise) cpu_noise(bus);
        else bus.idle(8);
        if (bus.requests - t0 > limit) break;
    }
    return false;
}

// all of VRAM through the pixel port (two edges of latency) against the model
static int compare_vram(Tb &tb, Bus &bus, const char *what) {
    bus.idle(0);
    int bad = 0;
    long first = -1;
    uint32_t got = 0, want = 0;
    const uint32_t words = GPU_VRAM_BYTES / 4;
    // the address goes in before an edge; its word is pix_data after the next edge
    for (uint32_t w = 0; w < words + 1; w++) {
        tb.t->pix_addr = w < words ? w : 0;
        tb.posedge();
        if (w >= 1) {
            const uint32_t a = 4 * (w - 1);
            const uint32_t m = (uint32_t)g_vram[a] | (uint32_t)g_vram[a + 1] << 8 | (uint32_t)g_vram[a + 2] << 16 |
                               (uint32_t)g_vram[a + 3] << 24;
            if (tb.t->pix_data != m) {
                if (first < 0) { first = a; got = tb.t->pix_data; want = m; }
                bad++;
            }
        }
    }
    g_compares++;
    CHECK(bad == 0, "%s: %d VRAM words differ from the model, first at byte %ld: %08x, model %08x", what, bad, first,
          got, want);
    return bad;
}

// a FENCE, the engine idle, then VRAM, FENCE and the sticky bits against the model
static void fence_check(Tb &tb, Bus &bus, const char *what) {
    static uint32_t n = 0x100;
    cmd(bus, {GPU_HDR(GPU_OP_FENCE, 0), ++n});
    const bool idle = wait_idle(bus);
    CHECK(idle, "%s: the engine stayed busy", what);
    if (!idle) {
        wr(bus, SURF_GPU_CTRL, SURF_GPU_CTRL_ABORT_MASK);
        gpu_engine_abort(&g_eng);
    }
    CHECK(rd(bus, SURF_GPU_FENCE) == g_eng.fence, "%s: FENCE %08x, model %08x", what, rd(bus, SURF_GPU_FENCE),
          g_eng.fence);
    const uint32_t st = rd(bus, SURF_GPU_STATUS);
    const uint32_t sticky = st & (SURF_GPU_STATUS_BAD_OPCODE_MASK | SURF_GPU_STATUS_BAD_PARAM_MASK |
                                  SURF_GPU_STATUS_FENCE_EV_MASK);
    CHECK(sticky == g_eng.sticky, "%s: sticky STATUS %02x, model %02x", what, sticky, g_eng.sticky);
    CHECK(gpu_engine_idle(&g_eng), "%s: the model is inside a command", what);
    CHECK(tb.flips == g_eng.flips && (g_eng.flips == 0 || tb.flip_last == g_eng.flip_base >> 2) && tb.flip_early == 0,
          "%s: FLIP: %llu writes (last %05x, %llu not after a blanking pulse), the model %u (last %05x)", what,
          (unsigned long long)tb.flips, tb.flip_last, (unsigned long long)tb.flip_early, g_eng.flips,
          g_eng.flip_base >> 2);
    wr(bus, SURF_GPU_STATUS, 0x1E);
    g_eng.sticky = 0;
    compare_vram(tb, bus, what);
}

static uint32_t xy(int x, int y) { return GPU_XY(x, y); }
static uint32_t wh(unsigned w, unsigned h) { return GPU_WH(w, h); }

static void set_target(Bus &bus, uint32_t base, uint32_t stride, unsigned w, unsigned h) {
    cmd(bus, {GPU_HDR(GPU_OP_SET_TARGET, 0), base, stride, wh(w, h)});
}

// ------------------------------------------------------------------ directed tests

static void test_registers(Tb &tb, Bus &bus) {
    (void)tb;
    CHECK(rd(bus, SURF_GPU_ID) == GPU_ID_VALUE, "ID %08x", rd(bus, SURF_GPU_ID));
    CHECK(rd(bus, SURF_GPU_INFO) == (300u << 16 | 64u), "INFO %08x", rd(bus, SURF_GPU_INFO));
    CHECK(rd(bus, SURF_GPU_FIFO_FREE) == 64 && rd(bus, SURF_GPU_STATUS) == 0 && rd(bus, SURF_GPU_FENCE) == 0 &&
          rd(bus, SURF_GPU_IE) == 0, "reset values: free %u status %x", rd(bus, SURF_GPU_FIFO_FREE),
          rd(bus, SURF_GPU_STATUS));
    CHECK(rd(bus, SURF_GPU_CMD) == 0 && rd(bus, SURF_GPU_CTRL) == 0, "write-only registers read 0");
    wr(bus, SURF_GPU_IE, 0xFFFFFFFF);
    CHECK(rd(bus, SURF_GPU_IE) == 3, "IE mask");
    wr(bus, SURF_GPU_IE, 0);
    const uint32_t holes[] = {0x020, 0x024, 0x100, 0xFFC};
    for (uint32_t off : holes) CHECK(bus.read(G + off).err && bus.write(G + off, 0).err, "offset %03x", off);
    const uint32_t regs[] = {SURF_GPU_ID, SURF_GPU_STATUS, SURF_GPU_CMD, SURF_GPU_FIFO_FREE, SURF_GPU_CTRL};
    for (uint32_t r : regs)
        CHECK(bus.write(G + r, 0xFFFFFFFF, 0x3).err && bus.read(G + r, 0x1).err && bus.fetch(G + r).err,
              "partial access or fetch at %03x", r);
    CHECK(rd(bus, SURF_GPU_FIFO_FREE) == 64 && !(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_OVERFLOW_MASK),
          "a refused CMD write went into the FIFO");
}

// the target and clip a reset leaves (base 0, stride 640, 640 x 480, the clip the whole of it), before
// any SET_TARGET or SET_CLIP; the model starts with them (gpu_engine_init). A fill from far out covers
// exactly the frame (the base, the stride, the clip's four edges); a fill across the bottom-right corner
// shows the right edge at 640, not past it (its pixels 640..643 of row 478 would land on row 479's
// first four); a copy whose source runs past the frame's right and bottom edges copies only what lies
// in the target, so its width and height are 640 and 480, not more (the clip edges past the target
// change nothing a program can see: the engine clips to both)
static void test_reset_target(Tb &tb, Bus &bus) {
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x5A), xy(-32768, -32768), wh(65535, 65535)});
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0xC3), xy(636, 478), wh(8, 2)});
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x96), xy(0, 4), wh(8, 2)});
    cmd(bus, {GPU_HDR(GPU_OP_COPY_RECT, 0), xy(636, 479), xy(0, 4), wh(8, 2)});
    fence_check(tb, bus, "the target and clip a reset leaves");
}

static void test_fill(Tb &tb, Bus &bus) {
    set_target(bus, 0, 640, 640, 480);
    for (int x = 0; x < 8; x++)                         // every x mod 4, widths 1..9
        for (unsigned w = 1; w <= 9; w++)
            cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, (x * 9 + w) & 0xFF), xy(4 + x + 16 * (int)w, 2 + x), wh(w, 3)});
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x11), xy(-5, -7), wh(9, 10)});         // the top-left corner
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x22), xy(636, 470), wh(10, 30)});      // the bottom-right corner
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x33), xy(-32768, 200), wh(65535, 1)}); // a whole line from far out
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x44), xy(300, 300), wh(0, 50)});       // zero sizes draw nothing
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x44), xy(300, 300), wh(50, 0)});
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x55), xy(640, 0), wh(5, 5)});          // just outside
    fence_check(tb, bus, "FILL_RECT alignments, corners, far, zero");
    // the clip at each edge
    cmd(bus, {GPU_HDR(GPU_OP_SET_CLIP, 0), xy(101, 51), xy(203, 77)});
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x66), xy(90, 40), wh(200, 100)});
    cmd(bus, {GPU_HDR(GPU_OP_SET_CLIP, 0), xy(10, 10), xy(9, 20)});              // empty clip
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x77), xy(0, 0), wh(640, 480)});
    cmd(bus, {GPU_HDR(GPU_OP_SET_CLIP, 0), xy(600, 470), xy(0xFFFF, 0xFFFF)});   // past the target
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x78), xy(0, 0), wh(640, 480)});
    fence_check(tb, bus, "FILL_RECT clipped at each edge");
    // full-screen fill (throughput)
    set_target(bus, 0, 640, 640, 480);
    const uint64_t c0 = tb.cycles;
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x01), xy(0, 0), wh(640, 480)});
    wait_idle(bus);
    std::printf("fill: 640x480 in %llu cycles\n", (unsigned long long)(tb.cycles - c0));
    fence_check(tb, bus, "FILL_RECT 640x480");
}

static void test_target(Tb &tb, Bus &bus) {
    set_target(bus, 2, 64, 16, 16);                         // refused: base not a multiple of 4
    set_target(bus, 0, 66, 16, 16);                         // stride not a multiple of 4
    set_target(bus, 307200, 64, 1, 1);                      // base past VRAM
    set_target(bus, 307200 - 192, 64, 64, 4);               // one line too many
    set_target(bus, 0, 64, 65, 2);                          // wider than the stride
    set_target(bus, 0, 0x10000, 16, 16);                    // stride too large
    set_target(bus, 0, 64, 0x8000, 1);                      // width too large
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x99), xy(0, 0), wh(2, 2)});              // still the old target
    fence_check(tb, bus, "SET_TARGET refusals");
    set_target(bus, 307200 - 192, 64, 64, 3);               // the last 192 bytes: allowed
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x98), xy(-1, -1), wh(100, 100)});
    set_target(bus, 4096, 64, 200, 1);                      // one line wider than its stride: allowed
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x97), xy(10, 0), wh(150, 5)});
    set_target(bus, 8192, 32, 0, 5);                        // empty target: nothing drawn
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x96), xy(0, 0), wh(8, 8)});
    fence_check(tb, bus, "SET_TARGET at VRAM's end, one line, empty");
}

static void seed_pattern(Bus &bus, uint32_t base, unsigned stride, unsigned w, unsigned h) {
    set_target(bus, base, stride, w, h);
    for (unsigned y = 0; y < h; y++)
        cmd(bus, {GPU_HDR(GPU_OP_LINE, (y * 37 + 5) & 0xFF), xy(0, (int)y), xy((int)w - 1, (int)y)});
    for (unsigned x = 0; x < w; x += 3)
        cmd(bus, {GPU_HDR(GPU_OP_LINE, (x * 11 + 1) & 0xFF), xy((int)x, 0), xy((int)x, (int)h - 1)});
}

static void test_copy(Tb &tb, Bus &bus) {
    seed_pattern(bus, 0x10000, 128, 120, 60);
    // all eight directions, by 1 and by 5 pixels, every source alignment
    const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
    for (int d = 0; d < 8; d++)
        for (int step = 1; step <= 5; step += 4)
            for (int a = 0; a < 4; a++) {
                const int sx = 20 + a, sy = 20;
                cmd(bus, {GPU_HDR(GPU_OP_COPY_RECT, 0), xy(sx, sy), xy(sx + dirs[d][0] * step, sy + dirs[d][1] * step),
                          wh(17 + (unsigned)a, 9)});
            }
    fence_check(tb, bus, "COPY_RECT overlapping in eight directions");
    // the key, sources outside the target, the clip, zero sizes
    cmd(bus, {GPU_HDR(GPU_OP_COPY_RECT, GPU_COPY_KEY | 0x05), xy(0, 0), xy(61, 31), wh(40, 20)});
    cmd(bus, {GPU_HDR(GPU_OP_COPY_RECT, 0), xy(-10, -5), xy(50, 40), wh(30, 30)});
    cmd(bus, {GPU_HDR(GPU_OP_COPY_RECT, 0), xy(100, 50), xy(3, 3), wh(40, 40)});
    cmd(bus, {GPU_HDR(GPU_OP_SET_CLIP, 0), xy(33, 7), xy(77, 41)});
    cmd(bus, {GPU_HDR(GPU_OP_COPY_RECT, 0), xy(0, 0), xy(31, 5), wh(60, 40)});
    cmd(bus, {GPU_HDR(GPU_OP_COPY_RECT, 0), xy(0, 0), xy(31, 5), wh(0, 40)});
    cmd(bus, {GPU_HDR(GPU_OP_COPY_RECT, 0), xy(-32768, -32768), xy(32767, 32767), wh(65535, 65535)});
    fence_check(tb, bus, "COPY_RECT key, outside sources, clip, extremes");
    // a big one across alignments (throughput)
    set_target(bus, 0, 640, 640, 480);
    const uint64_t c0 = tb.cycles;
    cmd(bus, {GPU_HDR(GPU_OP_COPY_RECT, 0), xy(1, 0), xy(0, 3), wh(639, 400)});
    wait_idle(bus);
    std::printf("copy: 639x400 (unaligned, overlapping) in %llu cycles\n", (unsigned long long)(tb.cycles - c0));
    fence_check(tb, bus, "COPY_RECT 639x400");
}

static void test_line(Tb &tb, Bus &bus) {
    set_target(bus, 0x20000, 256, 200, 150);
    const int cx = 100, cy = 75;
    for (int a = 0; a < 32; a++) {                          // every octant, steep and shallow
        const int ex[8] = {60, 60, 20, -20, -60, -60, -20, 20};
        const int ey[8] = {20, -20, -60, -60, -20, 20, 60, 60};
        const int o = a % 8, s = a / 8;
        cmd(bus, {GPU_HDR(GPU_OP_LINE, (a * 7 + 3) & 0xFF), xy(cx, cy), xy(cx + ex[o] + s, cy + ey[o] - s)});
    }
    cmd(bus, {GPU_HDR(GPU_OP_LINE, 0xA1), xy(5, 5), xy(5, 5)});                     // one point
    cmd(bus, {GPU_HDR(GPU_OP_LINE, 0xA2), xy(-300, 10), xy(500, 12)});              // clipped both ends
    cmd(bus, {GPU_HDR(GPU_OP_LINE, 0xA3), xy(199, 149), xy(0, 0)});                 // the endpoints
    cmd(bus, {GPU_HDR(GPU_OP_LINE, 0xA4), xy(-32768, -32768), xy(32767, 32767)});   // from far to far
    cmd(bus, {GPU_HDR(GPU_OP_SET_CLIP, 0), xy(50, 30), xy(150, 120)});
    cmd(bus, {GPU_HDR(GPU_OP_LINE, 0xA5), xy(0, 149), xy(199, 0)});
    fence_check(tb, bus, "LINE octants, endpoints, clipping, far");
}

static void test_expand(Tb &tb, Bus &bus) {
    set_target(bus, 0x30000, 128, 120, 64);
    // 8x16 glyph-like bitmaps at every alignment, opaque and transparent
    for (int x = 0; x < 8; x++) {
        const uint32_t hdrbits = (uint32_t)(0x40 + x) | (uint32_t)(0x80 + x) << 8 | (x & 1 ? GPU_EXPAND_TRANSP : 0);
        cmd(bus, {GPU_HDR(GPU_OP_EXPAND, hdrbits), xy(3 + x * 13, 2), wh(8, 16)});
        for (int k = 0; k < 4; k++) push(bus, 0x81C3E7FFu ^ (uint32_t)(k * 0x11111111u) ^ (uint32_t)x);
    }
    // odd sizes crossing words, clipped, negative
    const unsigned sizes[][2] = {{1, 1}, {3, 3}, {5, 7}, {33, 2}, {31, 5}, {64, 3}, {13, 13}};
    int y = 20;
    for (auto &sz : sizes) {
        for (int c = 0; c < 2; c++) {
            const int x = c ? -3 : 110;
            cmd(bus, {GPU_HDR(GPU_OP_EXPAND, 0x0102 | (c ? 0 : GPU_EXPAND_TRANSP)), xy(x, y), wh(sz[0], sz[1])});
            const unsigned words = (sz[0] * sz[1] + 31) / 32;
            for (unsigned k = 0; k < words; k++) push(bus, g_rng());
        }
        y += (int)sz[1] + 1;
    }
    cmd(bus, {GPU_HDR(GPU_OP_EXPAND, 0x0302), xy(0, 0), wh(0, 5)});              // no data words
    fence_check(tb, bus, "EXPAND bit order, transparency, sizes, clipping");
}

static void test_write_rect(Tb &tb, Bus &bus) {
    set_target(bus, 0x38000, 96, 90, 50);
    for (int x = 0; x < 8; x++) {
        const unsigned w = 3 + (unsigned)x, h = 4;
        cmd(bus, {GPU_HDR(GPU_OP_WRITE_RECT, 0), xy(x * 11 - 4, x), wh(w, h)});
        for (unsigned k = 0; k < (w * h + 3) / 4; k++) push(bus, g_rng());
    }
    cmd(bus, {GPU_HDR(GPU_OP_WRITE_RECT, 0), xy(80, 45), wh(17, 9)});             // past two edges
    for (unsigned k = 0; k < (17 * 9 + 3) / 4; k++) push(bus, g_rng());
    cmd(bus, {GPU_HDR(GPU_OP_WRITE_RECT, 0), xy(5, 5), wh(7, 0)});                // no data words
    fence_check(tb, bus, "WRITE_RECT byte order, alignments, clipping");
}

static void test_errors_and_fences(Tb &tb, Bus &bus) {
    // an unknown opcode (the first after GPU2's three): dropped, the next word is a command
    push(bus, GPU_HDR(0x0D, 0x123456));
    push(bus, GPU_HDR(0xFF, 0));
    fence_check(tb, bus, "unknown opcodes");
    // a full FIFO: WAIT_VBLANK holds the engine (no vblank comes), 64 words fill it, one more overflows
    tb.vblank_every = 0;
    push(bus, GPU_HDR(GPU_OP_WAIT_VBLANK, 0));
    bus.idle(10);
    CHECK(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK, "WAIT_VBLANK: not busy while waiting");
    for (int i = 0; i < 32; i++) {                          // 32 fences = 64 words
        wr(bus, SURF_GPU_CMD, GPU_HDR(GPU_OP_FENCE, 0));
        wr(bus, SURF_GPU_CMD, 0x500 + (uint32_t)i);
        gpu_engine_word(&g_eng, GPU_HDR(GPU_OP_FENCE, 0));
        gpu_engine_word(&g_eng, 0x500 + (uint32_t)i);
    }
    CHECK(rd(bus, SURF_GPU_FIFO_FREE) == 0 && !(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_OVERFLOW_MASK),
          "64 words: FIFO_FREE %u", rd(bus, SURF_GPU_FIFO_FREE));
    wr(bus, SURF_GPU_CMD, GPU_HDR(GPU_OP_FENCE, 0));        // dropped
    CHECK(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_OVERFLOW_MASK, "OVERFLOW not set by a write to a full FIFO");
    tb.pulse = true;                                        // the vertical blanking: the engine drains
    bus.idle(400);
    CHECK(rd(bus, SURF_GPU_FENCE) == 0x51F, "after the drain FENCE %08x, want 0x51f", rd(bus, SURF_GPU_FENCE));
    CHECK(rd(bus, SURF_GPU_FIFO_FREE) == 64 && !(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK),
          "after the drain: free %u", rd(bus, SURF_GPU_FIFO_FREE));
    wr(bus, SURF_GPU_STATUS, SURF_GPU_STATUS_OVERFLOW_MASK);
    CHECK(!(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_OVERFLOW_MASK), "OVERFLOW not cleared");
    tb.vblank_every = 5000;
    tb.next_vblank = tb.cycles + 5000;
    // the fence interrupt and the error interrupt
    wr(bus, SURF_GPU_IE, SURF_GPU_IE_FENCE_MASK);
    cmd(bus, {GPU_HDR(GPU_OP_FENCE, GPU_FENCE_IRQ), 0xFEED});
    wait_idle(bus);
    CHECK(tb.t->irq && (rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_FENCE_EV_MASK) && rd(bus, SURF_GPU_FENCE) == 0xFEED,
          "FENCE with IRQ: irq %d", tb.t->irq);
    wr(bus, SURF_GPU_STATUS, SURF_GPU_STATUS_FENCE_EV_MASK);
    bus.idle(1);
    CHECK(!tb.t->irq, "irq after clearing FENCE_EV");
    g_eng.sticky = 0;
    wr(bus, SURF_GPU_IE, SURF_GPU_IE_ERR_MASK);
    push(bus, GPU_HDR(0x77, 0));
    wait_idle(bus);
    CHECK(tb.t->irq, "an unknown opcode with IE.ERR: no irq");
    wr(bus, SURF_GPU_STATUS, 0x1E);
    wr(bus, SURF_GPU_IE, 0);
    g_eng.sticky = 0;
    // each interrupt needs its own enable: an event without it keeps irq low, the enable alone
    // raises it (the first GPU review: IE was only ever set before the event)
    cmd(bus, {GPU_HDR(GPU_OP_FENCE, GPU_FENCE_IRQ), 0xFE0});
    wait_idle(bus);
    bus.idle(1);
    CHECK((rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_FENCE_EV_MASK) && !tb.t->irq, "FENCE_EV with IE 0: irq %d",
          tb.t->irq);
    wr(bus, SURF_GPU_IE, SURF_GPU_IE_ERR_MASK);
    bus.idle(1);
    CHECK(!tb.t->irq, "FENCE_EV with IE.ERR alone: irq");
    wr(bus, SURF_GPU_IE, SURF_GPU_IE_FENCE_MASK);
    bus.idle(1);
    CHECK(tb.t->irq, "FENCE_EV, then IE.FENCE: no irq");
    wr(bus, SURF_GPU_IE, 0);
    wr(bus, SURF_GPU_STATUS, SURF_GPU_STATUS_FENCE_EV_MASK);
    g_eng.sticky = 0;
    push(bus, GPU_HDR(0x78, 0));                           // BAD_OPCODE
    wait_idle(bus);
    bus.idle(1);
    CHECK((rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BAD_OPCODE_MASK) && !tb.t->irq, "BAD_OPCODE with IE 0: irq %d",
          tb.t->irq);
    wr(bus, SURF_GPU_IE, SURF_GPU_IE_FENCE_MASK);
    bus.idle(1);
    CHECK(!tb.t->irq, "BAD_OPCODE with IE.FENCE alone: irq");
    wr(bus, SURF_GPU_IE, SURF_GPU_IE_ERR_MASK);
    bus.idle(1);
    CHECK(tb.t->irq, "BAD_OPCODE, then IE.ERR: no irq");
    wr(bus, SURF_GPU_IE, 0);
    wr(bus, SURF_GPU_STATUS, 0x1E);
    g_eng.sticky = 0;
    fence_check(tb, bus, "the interrupt enables");
    // ABORT empties the FIFO: two fills and a fence queued behind a WAIT_VBLANK that waits for good
    // are dropped and never run, even once the blanking comes (none of it is given to the model)
    tb.vblank_every = 0;
    wr(bus, SURF_GPU_CMD, GPU_HDR(GPU_OP_WAIT_VBLANK, 0));
    for (uint32_t w : {GPU_HDR(GPU_OP_FILL_RECT, 0x5A), xy(0, 0), wh(640, 480), GPU_HDR(GPU_OP_FILL_RECT, 0xA5),
                       xy(100, 100), wh(8, 8), GPU_HDR(GPU_OP_FENCE, GPU_FENCE_IRQ), 0xDEADu})
        wr(bus, SURF_GPU_CMD, w);
    bus.idle(20);
    const uint32_t queued = 64 - rd(bus, SURF_GPU_FIFO_FREE);
    CHECK((rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK) && queued == 8,
          "WAIT_VBLANK with 8 words behind it: busy %u, %u words queued",
          rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK, queued);
    wr(bus, SURF_GPU_CTRL, SURF_GPU_CTRL_ABORT_MASK);
    CHECK(!(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK) && rd(bus, SURF_GPU_FIFO_FREE) == 64,
          "ABORT with 8 words queued: busy, or FIFO_FREE %u", rd(bus, SURF_GPU_FIFO_FREE));
    tb.pulse = true;                                       // the blanking the WAIT_VBLANK waited for
    bus.idle(4000);
    CHECK(rd(bus, SURF_GPU_FENCE) != 0xDEADu && !(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_FENCE_EV_MASK),
          "a FENCE queued before the ABORT ran: FENCE %08x", rd(bus, SURF_GPU_FENCE));
    tb.vblank_every = 5000;
    tb.next_vblank = tb.cycles + 5000;
    fence_check(tb, bus, "ABORT with words queued");
    // BUSY until the last write: a WRITE_RECT whose last group waits for its data word stays busy
    // (the second review's mutant dropped BUSY in a rectangle's last group)
    set_target(bus, 0, 640, 640, 480);
    cmd(bus, {GPU_HDR(GPU_OP_WRITE_RECT, 0), xy(20, 420), wh(8, 1)});
    push(bus, 0x0D0C0B0Au);                                // the first group's word; the last one's is missing
    bus.idle(200);
    CHECK(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK, "WRITE_RECT waiting in its last group is not busy");
    push(bus, 0x14131211u);
    fence_check(tb, bus, "WRITE_RECT, its last data word late");
    // ABORT: a WRITE_RECT waits for data; the destination words it has every pixel of are drawn
    // (x 10..15), the rest never come. The model draws each data word as it arrives, so it also
    // drew x 16 and 17, whose destination word (x 16..19) the engine still waits to complete:
    // GPU.md section 5 says such pixels are not drawn, and the model is put right below.
    set_target(bus, 0, 640, 640, 480);
    const uint8_t keep16 = g_vram[400 * 640 + 16], keep17 = g_vram[400 * 640 + 17];
    cmd(bus, {GPU_HDR(GPU_OP_WRITE_RECT, 0), xy(10, 400), wh(40, 40)});
    push(bus, 0x11223344);
    push(bus, 0x55667788);
    wait_idle(bus, 2000);
    CHECK(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK, "WRITE_RECT short of data is not busy");
    wr(bus, SURF_GPU_CTRL, SURF_GPU_CTRL_ABORT_MASK);
    gpu_engine_abort(&g_eng);
    g_vram[400 * 640 + 16] = keep16;
    g_vram[400 * 640 + 17] = keep17;
    CHECK(!(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK) && rd(bus, SURF_GPU_FIFO_FREE) == 64,
          "after ABORT: busy or words left");
    fence_check(tb, bus, "ABORT in a WRITE_RECT");
    // WAIT_VBLANK holds what follows until the pulse
    tb.vblank_every = 0;
    cmd(bus, {GPU_HDR(GPU_OP_WAIT_VBLANK, 0)});
    cmd(bus, {GPU_HDR(GPU_OP_FENCE, 0), 0xB1A});
    bus.idle(300);
    CHECK(rd(bus, SURF_GPU_FENCE) != 0xB1A, "a FENCE after WAIT_VBLANK ran before the vertical blanking");
    tb.pulse = true;
    bus.idle(50);
    CHECK(rd(bus, SURF_GPU_FENCE) == 0xB1A, "the FENCE after WAIT_VBLANK did not run after the pulse");
    tb.vblank_every = 5000;
    tb.next_vblank = tb.cycles + 5000;
    fence_check(tb, bus, "WAIT_VBLANK");
}

// ------------------------------------------------------------------ GPU2: depth, triangles, flips

static uint32_t zcw(int z, int c) { return GPU_ZC((unsigned)z, (unsigned)c); }

// one TRIANGLE: its header bits, then (x, y in 12.4, depth, colour) for each vertex
static void tri(Bus &bus, uint32_t flags, const int v[3][4]) {
    cmd(bus, {GPU_HDR(GPU_OP_TRIANGLE, flags), xy(v[0][0], v[0][1]), zcw(v[0][2], v[0][3]), xy(v[1][0], v[1][1]),
              zcw(v[1][2], v[1][3]), xy(v[2][0], v[2][1]), zcw(v[2][2], v[2][3])});
}
static void tri3(Bus &bus, uint32_t flags, int x0, int y0, int x1, int y1, int x2, int y2, int z = 0) {
    const int v[3][4] = {{x0, y0, z, 0}, {x1, y1, z, 0}, {x2, y2, z, 0}};
    tri(bus, flags, v);
}
static void set_zbuf(Bus &bus, uint32_t base, uint32_t stride) { cmd(bus, {GPU_HDR(GPU_OP_SET_ZBUF, 0), base, stride}); }

// every depth 0xFFFF (docs/GPU.md 16.6): FILL_RECT 0xFF over the depth bytes of a w x h target, then
// that target again and its depth buffer (an accepted SET_TARGET leaves none)
static void clear_depth(Bus &bus, uint32_t tbase, uint32_t tstride, unsigned w, unsigned h, uint32_t zbase,
                        uint32_t zstride) {
    set_target(bus, zbase, zstride, 2 * w, h);
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0xFF), xy(0, 0), wh(2 * w, h)});
    set_target(bus, tbase, tstride, w, h);
    set_zbuf(bus, zbase, zstride);
}

static void test_zbuf(Tb &tb, Bus &bus) {
    set_target(bus, 0x10000, 128, 64, 64);                  // 2W = 128
    const uint32_t refused[][2] = {{0x20002, 128}, {0x20000, 130}, {307200, 128}, {0x20000, 0x10000}, {0x20000, 124},
                                   {307200 - 63 * 128 - 124, 128}};
    for (auto &z : refused) set_zbuf(bus, z[0], z[1]);
    tri3(bus, GPU_TRI_ZTEST | 0x41, 0, 0, GPU_FX(9), 0, 0, GPU_FX(9));      // depth without a buffer: refused
    tri3(bus, GPU_TRI_ZWRITE | 0x42, 0, 0, GPU_FX(9), 0, 0, GPU_FX(9));
    tri3(bus, 0x43, GPU_FX(20), 0, GPU_FX(29), 0, GPU_FX(20), GPU_FX(9));   // without depth: drawn
    fence_check(tb, bus, "SET_ZBUF's refusals, depth without a depth buffer");
    set_zbuf(bus, 307200 - 63 * 128 - 128, 128);            // ends at VRAM's end: taken
    tri3(bus, GPU_TRI_ZWRITE | 0x44, GPU_FX(30), GPU_FX(50), GPU_FX(64), GPU_FX(50), GPU_FX(64), GPU_FX(64), 0x2345);
    set_target(bus, 2, 64, 16, 16);                         // refused: the depth buffer stays
    tri3(bus, GPU_TRI_ZTEST | GPU_TRI_ZWRITE | 0x45, GPU_FX(40), GPU_FX(40), GPU_FX(64), GPU_FX(40), GPU_FX(40),
         GPU_FX(64), 0x1000);
    set_target(bus, 0x10000, 128, 64, 64);                  // accepted: no depth buffer any more
    tri3(bus, GPU_TRI_ZWRITE | 0x46, 0, GPU_FX(30), GPU_FX(9), GPU_FX(30), 0, GPU_FX(39));
    fence_check(tb, bus, "SET_ZBUF at VRAM's end; a refused SET_TARGET keeps it, an accepted one drops it");
    set_target(bus, 0x10000, 64, 100, 1);                   // one line wider than its stride
    set_zbuf(bus, 0x20000, 64);                             // and its depth: taken
    tri3(bus, GPU_TRI_ZWRITE | 0x47, 0, 0, GPU_FX(100), 0, 0, GPU_FX(1), 0x3456);
    set_target(bus, 0x10000, 64, 0, 5);                     // an empty target takes any aligned buffer
    set_zbuf(bus, 307196, 4);
    tri3(bus, GPU_TRI_ZWRITE | 0x48, 0, 0, GPU_FX(9), 0, 0, GPU_FX(9));
    fence_check(tb, bus, "SET_ZBUF for a one-line and an empty target");
}

static void test_tri_coverage(Tb &tb, Bus &bus) {
    set_target(bus, 0x8000, 256, 256, 120);
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0), xy(0, 0), wh(256, 120)});
    // small triangles at every lane alignment and several sub-pixel offsets, in both windings
    int k = 0;
    for (int a = 0; a < 4; a++)
        for (int sub = 0; sub < 16; sub += 5)
            for (int size = 1; size <= 7; size += 2) {
                const int x0 = GPU_FX(4 * (k % 24) + a) + sub, y0 = GPU_FX(40 * (k / 24)) + (15 - sub);
                const int sz = GPU_FX(size);
                if (k & 1) tri3(bus, (uint32_t)(k + 1) & 0xFF, x0, y0, x0 + sz, y0 + sub, x0 + (sub & 3), y0 + sz);
                else       tri3(bus, (uint32_t)(k + 1) & 0xFF, x0, y0, x0 + (sub & 3), y0 + sz, x0 + sz, y0 + sub);
                k++;
            }
    fence_check(tb, bus, "TRIANGLE at every lane and sub-pixel offset, both windings");
    // edges through pixel centres: a top and a left edge cover them, a bottom and a right one not;
    // a mesh of squares cut into triangles sharing those edges, alternating colours
    for (int q = 0; q < 6; q++) {
        const int x = GPU_FX(120 + 20 * q) + 8, y = GPU_FX(4) + 8, d = GPU_FX(3 + q);
        tri3(bus, 0x80 + (uint32_t)q, x, y, x + d, y, x, y + d);              // top and left through centres
        tri3(bus, 0x90 + (uint32_t)q, x + d, y, x + d, y + d, x, y + d);      // right and bottom through centres
    }
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 8; i++) {
            const int x = GPU_FX(130 + 12 * i) + (i * 5) % 16, y = GPU_FX(30 + 12 * j) + (j * 7) % 16;
            const int x2 = x + GPU_FX(12) + ((i + 1) * 5) % 16 - (i * 5) % 16, y2 = y + GPU_FX(12);
            const uint32_t c = 0xA0 + (uint32_t)(i + 8 * j) * 2;
            tri3(bus, c, x, y, x2, y, x2, y2);
            tri3(bus, c + 1, x, y, x2, y2, x, y2);
        }
    fence_check(tb, bus, "TRIANGLE: edges through pixel centres, a shared-edge mesh");
    // culling, zero areas
    tri3(bus, GPU_TRI_CULL_CW | 0x31, GPU_FX(10), GPU_FX(90), GPU_FX(30), GPU_FX(90), GPU_FX(10), GPU_FX(110));
    tri3(bus, GPU_TRI_CULL_CCW | 0x32, GPU_FX(40), GPU_FX(90), GPU_FX(60), GPU_FX(90), GPU_FX(40), GPU_FX(110));
    tri3(bus, GPU_TRI_CULL_CW | 0x33, GPU_FX(70), GPU_FX(90), GPU_FX(70), GPU_FX(110), GPU_FX(90), GPU_FX(90));
    tri3(bus, GPU_TRI_CULL_CCW | 0x34, GPU_FX(100), GPU_FX(90), GPU_FX(100), GPU_FX(110), GPU_FX(120), GPU_FX(90));
    tri3(bus, GPU_TRI_CULL_CW | GPU_TRI_CULL_CCW | 0x35, GPU_FX(130), GPU_FX(90), GPU_FX(150), GPU_FX(90), GPU_FX(130), GPU_FX(110));
    tri3(bus, 0x36, GPU_FX(160), GPU_FX(90), GPU_FX(170), GPU_FX(100), GPU_FX(180), GPU_FX(110));     // collinear
    tri3(bus, 0x37, GPU_FX(190), GPU_FX(90), GPU_FX(190), GPU_FX(90), GPU_FX(200), GPU_FX(110));     // two the same
    tri3(bus, 0x38, 3000, 1500, 3000, 1500, 3000, 1500);
    fence_check(tb, bus, "TRIANGLE: culling, zero areas");
    // the clip's four edges, huge triangles over the whole 12.4 range, far ones, slivers
    cmd(bus, {GPU_HDR(GPU_OP_SET_CLIP, 0), xy(13, 7), xy(203, 101)});
    tri3(bus, 0x51, GPU_FX(5), GPU_FX(2), GPU_FX(250), GPU_FX(60), GPU_FX(40), GPU_FX(118));
    tri3(bus, 0x52, 32767, -32768, 32767, 32767, -32768, 32767);
    cmd(bus, {GPU_HDR(GPU_OP_SET_CLIP, 0), xy(0, 0), xy(256, 120)});
    tri3(bus, 0x53, -32768, -32768, 32767, -32768, -32768, 32767);    // the other half: misses the target
    tri3(bus, 0x54, -GPU_FX(50), GPU_FX(10), -GPU_FX(1), GPU_FX(10), -GPU_FX(20), GPU_FX(50));    // left of it
    tri3(bus, 0x55, GPU_FX(10), GPU_FX(121), GPU_FX(90), GPU_FX(130), GPU_FX(30), GPU_FX(200));  // below it
    tri3(bus, 0x56, GPU_FX(2) + 3, GPU_FX(60), GPU_FX(250), GPU_FX(61) + 2, GPU_FX(2) + 5, GPU_FX(60) + 1);  // sliver
    tri3(bus, 0x57, GPU_FX(200) + 8, GPU_FX(100) + 8, GPU_FX(201) + 8, GPU_FX(100) + 8, GPU_FX(200) + 8, GPU_FX(101) + 8);
    fence_check(tb, bus, "TRIANGLE: the clip's edges, huge, far, slivers, one pixel");
}

static void test_tri_depth(Tb &tb, Bus &bus) {
    const uint32_t T = 0, TS = 320, Z = 0x20000, ZS = 640;
    set_target(bus, T, TS, 320, 200);
    cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x00), xy(0, 0), wh(320, 200)});
    clear_depth(bus, T, TS, 320, 200, Z, ZS);
    // every combination of GOURAUD, ZTEST, ZFUNC, ZWRITE and the culls, two triangles each, overlapping
    for (unsigned f = 0; f < 64; f++) {
        const uint32_t flags = (f & 1 ? GPU_TRI_GOURAUD : 0) | (f & 2 ? GPU_TRI_ZTEST : 0) |
                               (f & 4 ? GPU_TRI_ZFUNC_LE : 0) | (f & 8 ? GPU_TRI_ZWRITE : 0) |
                               (f & 16 ? GPU_TRI_CULL_CW : 0) | (f & 32 ? GPU_TRI_CULL_CCW : 0) | ((f * 3 + 1) & 0xFF);
        const int cx = 20 + (int)(f % 8) * 38, cy = 20 + (int)(f / 8) * 22;
        const int v[3][4] = {{GPU_FX(cx - 18) + 3, GPU_FX(cy - 15) + 11, (int)(f * 997 & 0xFFFF), (int)(f * 5 & 0xFF)},
                             {GPU_FX(cx + 21) + 7, GPU_FX(cy - 2), (int)(f * 1997 & 0xFFFF), (int)(f * 11 & 0xFF)},
                             {GPU_FX(cx - 5), GPU_FX(cy + 16) + 9, (int)(f * 2999 & 0xFFFF), (int)(f * 17 & 0xFF)}};
        tri(bus, flags, v);
        const int w[3][4] = {{v[0][0], v[0][1], 30000, 40}, {v[2][0], v[2][1], 32768, 47},
                             {v[1][0] + 40, v[1][1] + 30, 29000, 33}};
        tri(bus, flags ^ GPU_TRI_GOURAUD, w);                   // the other winding, over it
    }
    fence_check(tb, bus, "TRIANGLE: every combination of GOURAUD, ZTEST, ZFUNC, ZWRITE and the culls");
    // two triangles crossing in depth, each in front on one side; Gouraud ramps at full range
    clear_depth(bus, T, TS, 320, 200, Z, ZS);
    const uint32_t D = GPU_TRI_ZTEST | GPU_TRI_ZWRITE | GPU_TRI_GOURAUD;
    const int a[3][4] = {{GPU_FX(10), GPU_FX(10), 0, 32}, {GPU_FX(300), GPU_FX(30), 65535, 63}, {GPU_FX(40), GPU_FX(190), 30000, 40}};
    const int b[3][4] = {{GPU_FX(310), GPU_FX(15), 0, 64}, {GPU_FX(290), GPU_FX(195), 40000, 95}, {GPU_FX(5), GPU_FX(100), 65535, 80}};
    tri(bus, D, a);
    tri(bus, D, b);
    const int c[3][4] = {{0, 0, 65535, 0}, {GPU_FX(319) + 15, 0, 0, 255}, {0, GPU_FX(199) + 15, 32768, 128}};
    tri(bus, D | GPU_TRI_ZFUNC_LE, c);
    fence_check(tb, bus, "TRIANGLE: two triangles crossing in depth, full-range ramps");
    // a depth buffer that overlaps the target itself (section 16.5's order decides)
    set_target(bus, 0x30000, 256, 100, 50);
    set_zbuf(bus, 0x30000 + 64, 256);
    const int o[3][4] = {{GPU_FX(2), GPU_FX(1), 1000, 9}, {GPU_FX(97), GPU_FX(8), 50000, 200}, {GPU_FX(30), GPU_FX(49), 20000, 90}};
    tri(bus, D, o);
    tri(bus, D | GPU_TRI_ZFUNC_LE, o);
    fence_check(tb, bus, "TRIANGLE: a depth buffer overlapping the target");
}

static void test_flip(Tb &tb, Bus &bus) {
    tb.vblank_every = 0;
    const uint64_t f0 = tb.flips;
    cmd(bus, {GPU_HDR(GPU_OP_FLIP, 0), 0xFFF12C03u});       // GFX_BASE keeps bits 18:2: 0x12C00
    cmd(bus, {GPU_HDR(GPU_OP_FENCE, 0), 0xF11});
    bus.idle(300);
    CHECK(tb.flips == f0 && rd(bus, SURF_GPU_FENCE) != 0xF11 && (rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK),
          "FLIP did not wait for the vertical blanking: %llu writes", (unsigned long long)(tb.flips - f0));
    tb.pulse = true;
    bus.idle(30);
    CHECK(tb.flips == f0 + 1 && tb.flip_last == (0x12C00u >> 2) && rd(bus, SURF_GPU_FENCE) == 0xF11,
          "FLIP after the pulse: %llu writes, last %05x, FENCE %x", (unsigned long long)(tb.flips - f0), tb.flip_last,
          rd(bus, SURF_GPU_FENCE));
    cmd(bus, {GPU_HDR(GPU_OP_FLIP, 0), 0x100});            // two FLIPs: one each blanking
    cmd(bus, {GPU_HDR(GPU_OP_FLIP, 0), 0x204});
    bus.idle(50);
    tb.pulse = true;
    bus.idle(50);
    CHECK(tb.flips == f0 + 2 && tb.flip_last == 0x40, "the first of two FLIPs: %llu writes, last %05x",
          (unsigned long long)(tb.flips - f0), tb.flip_last);
    tb.pulse = true;
    bus.idle(50);
    CHECK(tb.flips == f0 + 3 && tb.flip_last == 0x81, "the second of two FLIPs: %llu writes, last %05x",
          (unsigned long long)(tb.flips - f0), tb.flip_last);
    // ABORT while a FLIP waits: it never writes (the words go to the engine only, not the model)
    wr(bus, SURF_GPU_CMD, GPU_HDR(GPU_OP_FLIP, 0));
    wr(bus, SURF_GPU_CMD, 0x300);
    bus.idle(20);
    wr(bus, SURF_GPU_CTRL, SURF_GPU_CTRL_ABORT_MASK);
    tb.pulse = true;
    bus.idle(50);
    CHECK(tb.flips == f0 + 3, "a FLIP aborted before the blanking wrote GFX_BASE");
    tb.vblank_every = 5000;
    tb.next_vblank = tb.cycles + 5000;
    fence_check(tb, bus, "FLIP");
}

// all of VRAM through the pixel port
static std::vector<uint8_t> dump_vram(Tb &tb, Bus &bus) {
    bus.idle(0);
    std::vector<uint8_t> v(GPU_VRAM_BYTES);
    const uint32_t words = GPU_VRAM_BYTES / 4;
    for (uint32_t w = 0; w < words + 1; w++) {
        tb.t->pix_addr = w < words ? w : 0;
        tb.posedge();
        if (w >= 1)
            for (int k = 0; k < 4; k++) v[4 * (w - 1) + k] = (uint8_t)(tb.t->pix_data >> (8 * k));
    }
    return v;
}

static int rnd(int lo, int hi);

// ABORT inside a TRIANGLE at a random moment: every byte is as before the triangle or as the whole
// triangle leaves it, and what was drawn is a prefix of the walk (docs/GPU.md 16.5: rows, then
// groups), whose last group may have some or all of its depth written but not its colour (ABORT
// stops between two accesses); the model then takes the engine's VRAM, as after any ABORT
static void test_abort_triangle(Tb &tb, Bus &bus) {
    const uint32_t T = 0, TS = 320, Z = 0x20000, ZS = 640;
    set_target(bus, T, TS, 320, 200);
    clear_depth(bus, T, TS, 320, 200, Z, ZS);
    int bad = 0, partial = 0, mid = 0;
    for (int n = 0; n < 24; n++) {
        if (n % 6 == 0) clear_depth(bus, T, TS, 320, 200, Z, ZS);
        fence_check(tb, bus, "before an aborted TRIANGLE");
        const uint32_t flags = (uint32_t)rnd(1, 255) | (n & 1 ? GPU_TRI_GOURAUD : 0) |
                               (n % 3 ? GPU_TRI_ZTEST | GPU_TRI_ZWRITE : 0) | (n % 5 == 0 ? GPU_TRI_ZWRITE : 0);
        int v[3][4];
        for (int i = 0; i < 3; i++) {
            v[i][0] = rnd(-GPU_FX(40), GPU_FX(360));
            v[i][1] = rnd(-GPU_FX(30), GPU_FX(230));
            v[i][2] = rnd(0, 65535);
            v[i][3] = rnd(0, 255);
        }
        const std::vector<uint8_t> before = g_vram;
        tri(bus, flags, v);
        const std::vector<uint8_t> after = g_vram;
        bus.idle((uint64_t)rnd(0, 40000));
        wr(bus, SURF_GPU_CTRL, SURF_GPU_CTRL_ABORT_MASK);
        gpu_engine_abort(&g_eng);
        CHECK(!(rd(bus, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK), "ABORT inside a TRIANGLE: still busy");
        const std::vector<uint8_t> got = dump_vram(tb, bus);
        for (uint32_t a = 0; a < GPU_VRAM_BYTES; a++)
            if (got[a] != before[a] && got[a] != after[a]) bad++;
        // the walk's groups in order (the box of 16.3 in the target, no clip)
        int32_t vx[3], vy[3];
        for (int i = 0; i < 3; i++) { vx[i] = (int16_t)v[i][0]; vy[i] = (int16_t)v[i][1]; }
        const int32_t mnx = std::min(vx[0], std::min(vx[1], vx[2])), mxx = std::max(vx[0], std::max(vx[1], vx[2]));
        const int32_t mny = std::min(vy[0], std::min(vy[1], vy[2])), mxy = std::max(vy[0], std::max(vy[1], vy[2]));
        auto fl16 = [](int32_t a) { return a >= 0 ? a / 16 : -((-a + 15) / 16); };
        const int x0 = std::max(fl16(mnx + 7), 0), x1 = std::min(fl16(mxx - 8), 319);
        const int y0 = std::max(fl16(mny + 7), 0), y1 = std::min(fl16(mxy - 8), 199);
        bool cut = false;                                   // a group not (wholly) drawn came
        for (int y = y0; y <= y1 && x0 <= x1; y++)
            for (int g = x0 / 4; g <= x1 / 4; g++) {
                bool c_after = true, c_before = true, d_after = true, d_before = true, d_each = true, any = false;
                for (int L = 0; L < 4; L++) {
                    const int x = 4 * g + L;
                    if (x < x0 || x > x1) continue;
                    const uint32_t ca = T + (uint32_t)y * TS + (uint32_t)x, za = Z + (uint32_t)y * ZS + 2 * (uint32_t)x;
                    any |= before[ca] != after[ca] || before[za] != after[za] || before[za + 1] != after[za + 1];
                    c_after &= got[ca] == after[ca];
                    c_before &= got[ca] == before[ca];
                    const bool la = got[za] == after[za] && got[za + 1] == after[za + 1];
                    const bool lb = got[za] == before[za] && got[za + 1] == before[za + 1];
                    d_after &= la;
                    d_before &= lb;
                    d_each &= la || lb;
                }
                if (!any) continue;                         // the triangle changes nothing here
                if (!cut) {
                    if (c_after && d_after) continue;        // drawn
                    cut = true;
                    mid++;
                    if (c_before && d_each && !d_before) { partial++; continue; }   // depth, not yet colour
                    if (c_before && d_before) continue;
                    bad++;
                } else if (!(c_before && d_before)) {
                    bad++;                                  // drawn after a group that was not
                }
            }
        g_vram = got;                                       // the model takes what the engine left
    }
    CHECK(bad == 0, "ABORT inside a TRIANGLE: %d bytes or groups outside the walk's prefix", bad);
    std::printf("abort: 24 TRIANGLEs aborted at random moments, %d stopped inside their walk (%d with a group's "
                "depth written, not its colour)\n", mid, partial);
    fence_check(tb, bus, "after the aborted TRIANGLEs");
}

// the walk's cycles (docs/GPU.md 16.5): one big triangle each way, with the groups of its box counted
static void test_tri_timing(Tb &tb, Bus &bus) {
    const uint32_t T = 0, TS = 320, Z = 0x20000, ZS = 640;
    set_target(bus, T, TS, 320, 200);
    const int v[3][4] = {{GPU_FX(4) + 5, GPU_FX(3) + 2, 1000, 3}, {GPU_FX(310) + 9, GPU_FX(20), 64000, 30},
                         {GPU_FX(30), GPU_FX(196) + 11, 30000, 17}};
    int32_t vx[3] = {v[0][0], v[1][0], v[2][0]}, vy[3] = {v[0][1], v[1][1], v[2][1]};
    struct gpu_tri t;
    gpu_tri_setup(&t, vx, vy);
    unsigned box = 0, covered = 0;
    for (int y = 0; y < 200; y++)
        for (int g = 0; g < 80; g++) {
            bool c = false, in = false;
            for (int L = 0; L < 4; L++) {
                const int x = 4 * g + L;
                c |= gpu_tri_inside(&t, x, y) != 0;
                in |= 16 * x + 8 >= 4 * 16 + 5 && 16 * x + 8 <= 310 * 16 + 9;
            }
            if (in && 16 * y + 8 >= 3 * 16 + 2 && 16 * y + 8 <= 196 * 16 + 11) box++;
            covered += c;
        }
    const struct { const char *what; uint32_t flags; } runs[] = {
        {"flat", 0x21}, {"Gouraud", GPU_TRI_GOURAUD}, {"depth", GPU_TRI_ZTEST | GPU_TRI_ZWRITE | 0x22},
        {"Gouraud and depth", GPU_TRI_GOURAUD | GPU_TRI_ZTEST | GPU_TRI_ZWRITE}, {"ZWRITE alone", GPU_TRI_ZWRITE | 0x23}};
    for (auto &r : runs) {
        clear_depth(bus, T, TS, 320, 200, Z, ZS);
        wait_idle(bus);
        const uint64_t c0 = tb.cycles;
        tri(bus, r.flags, v);
        wait_idle(bus);
        const uint64_t n = tb.cycles - c0;
        std::printf("tri timing: %s: %llu cycles; the box %u groups, %u of them covered: %.2f cycles a covered group "
                    "beyond the 1 every group takes\n", r.what, (unsigned long long)n, box, covered,
                    covered ? (double)((long long)n - (long long)box) / covered : 0.0);
    }
    fence_check(tb, bus, "TRIANGLE timing");
}

// ------------------------------------------------------------------ randomized streams

static int rnd(int lo, int hi) { return lo + (int)(g_rng() % (uint32_t)(hi - lo + 1)); }

static uint32_t g_rt_base = 0, g_rt_stride = 640;           // the random part's target

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

// three random vertices (12.4) for a target of tw x th: near it, tiny, huge, of zero area, far
// beyond one of its edges, or slivers
static void random_vertices(int v[3][4], unsigned tw, unsigned th) {
    const int mode = rnd(0, 99), W = (int)tw, H = (int)th;
    for (int i = 0; i < 3; i++) {
        v[i][0] = rnd(-GPU_FX(40), GPU_FX(W + 40));
        v[i][1] = rnd(-GPU_FX(40), GPU_FX(H + 40));
    }
    if (mode < 40) {
        // near the target, as drawn above
    } else if (mode < 58) {                             // tiny: within three pixels of a point
        const int cx = rnd(-GPU_FX(4), GPU_FX(W + 4)), cy = rnd(-GPU_FX(4), GPU_FX(H + 4));
        for (int i = 0; i < 3; i++) {
            v[i][0] = cx + rnd(-48, 48);
            v[i][1] = cy + rnd(-48, 48);
        }
    } else if (mode < 68) {                             // huge: anywhere in the 12.4 range
        for (int i = 0; i < 3; i++) {
            v[i][0] = rnd(-32768, 32767);
            v[i][1] = rnd(-32768, 32767);
        }
    } else if (mode < 78) {                             // zero area: on a line, or two the same
        const int k = rnd(-1, 2);
        v[2][0] = v[0][0] + k * (v[1][0] - v[0][0]);
        v[2][1] = v[0][1] + k * (v[1][1] - v[0][1]);
        if (rnd(0, 3) == 0) { v[1][0] = v[0][0]; v[1][1] = v[0][1]; }
    } else if (mode < 88) {                             // beyond one edge of the target
        const int side = rnd(0, 3);
        for (int i = 0; i < 3; i++) {
            if (side == 0) v[i][0] = rnd(-GPU_FX(300), -1);
            if (side == 1) v[i][1] = rnd(-GPU_FX(300), -1);
            if (side == 2) v[i][0] = rnd(GPU_FX(W) + 8, GPU_FX(W + 300));
            if (side == 3) v[i][1] = rnd(GPU_FX(H) + 8, GPU_FX(H + 300));
        }
    } else {                                            // a sliver along the first two
        const int t16 = rnd(-8, 24);
        v[2][0] = v[0][0] + (v[1][0] - v[0][0]) * t16 / 16 + rnd(-3, 3);
        v[2][1] = v[0][1] + (v[1][1] - v[0][1]) * t16 / 16 + rnd(-3, 3);
    }
    for (int i = 0; i < 3; i++) {
        v[i][0] = clamp16(v[i][0]);
        v[i][1] = clamp16(v[i][1]);
        v[i][2] = (g_rng() % 5 == 0) ? (g_rng() & 1 ? 0xFFFF : 0) : (int)(g_rng() & 0xFFFF);
        v[i][3] = (g_rng() % 5 == 0) ? (g_rng() & 1 ? 0xFF : 0) : (int)(g_rng() & 0xFF);
    }
}

// one GPU2 command (or a few): a TRIANGLE, a depth buffer, a depth clear, a FLIP, a new target
// with its depth buffer. Every depth buffer the engine takes lies below SPARE
static void random_gpu2(Bus &bus, unsigned &tw, unsigned &th) {
    const uint32_t r = g_rng() % 100;
    if (r < 72) {
        int v[3][4];
        random_vertices(v, tw, th);
        uint32_t f = g_rng() & 0xFF;
        if (g_rng() & 1) f |= GPU_TRI_GOURAUD;
        if (g_eng.zvalid ? (g_rng() % 4 != 0) : (g_rng() % 20 == 0)) {     // without a buffer: refused
            const uint32_t d = g_rng() % 3;
            f |= d == 0 ? GPU_TRI_ZTEST : d == 1 ? GPU_TRI_ZWRITE : GPU_TRI_ZTEST | GPU_TRI_ZWRITE;
            if (g_rng() & 1) f |= GPU_TRI_ZFUNC_LE;
        }
        if (g_rng() % 6 == 0) f |= GPU_TRI_CULL_CW;
        if (g_rng() % 6 == 0) f |= GPU_TRI_CULL_CCW;
        f |= (g_rng() & 0x3FFu) << 14;                  // header bits TRIANGLE ignores
        tri(bus, f, v);
    } else if (r < 82) {                                // a depth buffer for this target, or a refused one
        const unsigned zs = ((2 * tw + 3) & ~3u) + 4 * (unsigned)rnd(0, 6);
        const uint64_t need = (uint64_t)(th - 1) * zs + 2 * tw;
        if (zs < 65536 && need <= SPARE && g_rng() % 8 != 0)
            set_zbuf(bus, 4 * (uint32_t)(g_rng() % ((SPARE - need) / 4 + 1)), zs);
        else
            set_zbuf(bus, 4 * (g_rng() % 1000) + 2, zs);   // unaligned: refused
    } else if (r < 90) {
        if (g_eng.zvalid) clear_depth(bus, g_rt_base, g_rt_stride, tw, th, g_eng.zbase, g_eng.zstride);
    } else if (r < 93) {
        cmd(bus, {GPU_HDR(GPU_OP_FLIP, g_rng() & 0xFFFFFF), g_rng()});
    } else {                                            // a small target and its depth buffer, cleared
        const unsigned w = (unsigned)rnd(1, 200), h = (unsigned)rnd(1, 150);
        const unsigned st = ((w + 3) & ~3u) + 4 * (unsigned)rnd(0, 8), zs = ((2 * w + 3) & ~3u) + 4 * (unsigned)rnd(0, 8);
        const uint32_t tneed = (h - 1) * st + w, zneed = (h - 1) * zs + 2 * w;
        const uint32_t tbase = 4 * (g_rng() % ((SPARE - tneed) / 4 + 1)), zbase = 4 * (g_rng() % ((SPARE - zneed) / 4 + 1));
        set_target(bus, tbase, st, w, h);
        tw = w;
        th = h;
        g_rt_base = tbase;
        g_rt_stride = st;
        clear_depth(bus, tbase, st, w, h, zbase, zs);
    }
}

static void random_chunk(Bus &bus, unsigned &tw, unsigned &th) {
    const int n = rnd(1, 24);
    for (int i = 0; i < n; i++) {
        if (g_rng() % 100 < 40) {
            random_gpu2(bus, tw, th);
            continue;
        }
        const uint32_t r = g_rng() % 100;
        const int bx = (int)tw, by = (int)th;
        auto cx = [&] { return (g_rng() % 8 == 0) ? rnd(-32768, 32767) : rnd(-40, bx + 40); };
        auto cy = [&] { return (g_rng() % 8 == 0) ? rnd(-32768, 32767) : rnd(-40, by + 40); };
        auto sz = [&] { return (g_rng() % 16 == 0) ? (unsigned)rnd(0, 65535) : (unsigned)rnd(0, 90); };
        if (r < 4) {                                        // a new target (valid, or now and then not)
            const unsigned w = (unsigned)rnd(1, 300), h = (unsigned)rnd(1, 200);
            const unsigned stride = ((w + 3) & ~3u) + 4 * (unsigned)rnd(0, 20);
            const uint32_t room = SPARE - (h - 1) * stride - w;
            uint32_t base = (room > 0x7FFFFFF) ? 0 : 4 * (g_rng() % (room / 4 + 1));
            if (g_rng() % 10 == 0) base += 2;               // refused
            set_target(bus, base, stride, w, h);
            if (!(base & 3)) { tw = w; th = h; g_rt_base = base; g_rt_stride = stride; }
        } else if (r < 9) {
            cmd(bus, {GPU_HDR(GPU_OP_SET_CLIP, 0), xy(rnd(0, bx), rnd(0, by)), xy(rnd(0, bx + 20), rnd(0, by + 20))});
        } else if (r < 30) {
            cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, g_rng() & 0xFF), xy(cx(), cy()), wh(sz(), sz())});
        } else if (r < 50) {
            const uint32_t key = (g_rng() % 3 == 0) ? GPU_COPY_KEY : 0;
            int sxv = rnd(-10, bx), syv = rnd(-10, by);
            int dxv = (g_rng() % 2) ? sxv + rnd(-6, 6) : rnd(-10, bx), dyv = (g_rng() % 2) ? syv + rnd(-3, 3) : rnd(-10, by);
            cmd(bus, {GPU_HDR(GPU_OP_COPY_RECT, key | (g_rng() & 0xFF)), xy(sxv, syv), xy(dxv, dyv),
                      wh((unsigned)rnd(0, 120), (unsigned)rnd(0, 60))});
        } else if (r < 68) {
            const bool far = g_rng() % 10 == 0;
            cmd(bus, {GPU_HDR(GPU_OP_LINE, g_rng() & 0xFF), xy(far ? cx() : rnd(-20, bx + 20), far ? cy() : rnd(-20, by + 20)),
                      xy(rnd(-20, bx + 20), rnd(-20, by + 20))});
        } else if (r < 82) {
            const unsigned w = (unsigned)rnd(0, 70), h = (unsigned)rnd(0, 40);
            const uint32_t bits = (g_rng() & 0xFFFF) | (g_rng() % 2 ? GPU_EXPAND_TRANSP : 0);
            cmd(bus, {GPU_HDR(GPU_OP_EXPAND, bits), xy(rnd(-30, bx + 10), rnd(-30, by + 10)), wh(w, h)});
            for (unsigned k = 0; k < (w * h + 31) / 32; k++) push(bus, g_rng());
        } else if (r < 94) {
            const unsigned w = (unsigned)rnd(0, 60), h = (unsigned)rnd(0, 30);
            cmd(bus, {GPU_HDR(GPU_OP_WRITE_RECT, 0), xy(rnd(-30, bx + 10), rnd(-30, by + 10)), wh(w, h)});
            for (unsigned k = 0; k < (w * h + 3) / 4; k++) push(bus, g_rng());
        } else if (r < 96) {
            cmd(bus, {GPU_HDR(GPU_OP_NOP, g_rng() & 0xFFFFFF)});
        } else if (r < 97) {
            cmd(bus, {GPU_HDR(GPU_OP_WAIT_VBLANK, 0)});
        } else if (r < 98) {
            push(bus, GPU_HDR(0x0D + g_rng() % 0xF3, g_rng() & 0xFFFFFF));   // unknown: dropped
        } else {
            cmd(bus, {GPU_HDR(GPU_OP_FENCE, g_rng() & 1), g_rng()});
        }
    }
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    unsigned seed = 1, chunks = 120;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--chunks") && i + 1 < argc) chunks = (unsigned)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) g_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--only-fill")) g_only_fill = true;
    }
    g_rng.seed(seed);
    auto top = std::make_unique<Vtb_gpu>();
    Tb tb{top.get()};
    top->clk = 0;
    top->rst = 1;
    top->s_req_valid = 0;
    top->vblank = 0;
    top->pix_addr = 0;
    top->eval();
    for (int i = 0; i < 8; i++) tb.posedge();
    top->rst = 0;
    Bus bus(top.get(), [&] { tb.posedge(); }, seed);
    bus.max_gap = (int)(seed % 3);
    tb.vblank_every = 5000;
    tb.next_vblank = 5000;
    gpu_engine_init(&g_eng, g_vram.data());
    tb.trace = g_trace;
    if (g_only_fill) {                                      // a debugging aid: one small fill
        set_target(bus, 0, 640, 640, 480);
        cmd(bus, {GPU_HDR(GPU_OP_FILL_RECT, 0x99), xy(0, 0), wh(2, 2)});
        fence_check(tb, bus, "one 2x2 fill");
        std::printf("test_gpu: --only-fill: %d failures\n", failures);
        return failures ? 1 : 0;
    }

    test_registers(tb, bus);
    test_reset_target(tb, bus);                         // before anything sets a target
    test_fill(tb, bus);
    test_target(tb, bus);
    test_copy(tb, bus);
    test_line(tb, bus);
    test_expand(tb, bus);
    test_write_rect(tb, bus);
    test_errors_and_fences(tb, bus);
    test_zbuf(tb, bus);
    test_tri_coverage(tb, bus);
    test_tri_depth(tb, bus);
    test_flip(tb, bus);
    test_abort_triangle(tb, bus);
    test_tri_timing(tb, bus);
    const uint64_t directed = g_compares;

    // randomized streams, with the CPU on VRAM meanwhile
    g_noise = true;
    unsigned tw = 640, th = 480;
    set_target(bus, 0, 640, 640, 473);                      // the random part stays below SPARE
    th = 473;
    g_rt_base = 0;
    g_rt_stride = 640;
    for (unsigned c = 0; c < chunks; c++) {
        random_chunk(bus, tw, th);
        char what[64];
        std::snprintf(what, sizeof what, "random chunk %u (seed %u)", c, seed);
        fence_check(tb, bus, what);
        if (failures > 20) break;
    }
    CHECK(bus.problems.empty(), "SurfBus protocol: %s", bus.problems.empty() ? "" : bus.problems[0].c_str());
    top->final();
    if (failures) {
        std::printf("test_gpu: FAIL (%d of %d checks, seed %u)\n", failures, checks, seed);
        return 1;
    }
    CHECK(g_eng.tri_clamped == 0 && g_eng.tri_wide == 0, "the model's clamp changed %llu values, %llu were wide",
          g_eng.tri_clamped, g_eng.tri_wide);
    std::printf("test_gpu: PASS (seed %u: %d checks; %llu commands, %llu words, %llu pixel writes in the model; "
                "VRAM compared %llu times (%llu directed, %u random chunks); engine %llu grants, %llu cycles waiting "
                "for the CPU (%llu CPU accesses meanwhile); %llu cycles)\n",
                seed, checks, (unsigned long long)g_cmds, (unsigned long long)g_words, g_eng.writes,
                (unsigned long long)g_compares, (unsigned long long)directed, chunks,
                (unsigned long long)tb.engine_grants, (unsigned long long)tb.engine_waits,
                (unsigned long long)g_cpu_noise, (unsigned long long)tb.cycles);
    std::printf("test_gpu: GPU2 (seed %u): %llu TRIANGLEs (%llu walked, %llu culled, %llu of zero area), %llu pixels "
                "covered, %llu failing the depth test; %llu FLIPs\n", seed, g_eng.tri_cmds, g_eng.tri_drawn,
                g_eng.tri_culled, g_eng.tri_zero, g_eng.tri_pixels, g_eng.tri_zfail, (unsigned long long)tb.flips);
    return 0;
}
