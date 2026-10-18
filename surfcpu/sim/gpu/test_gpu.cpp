/*
SurfGPU engine testbench
--------------------
File: test_gpu.cpp Date: 10/18/26 (SurfGPU)
--------------------
Verilator harness for surf_gpu with surf_vram (sim/gpu/tb_gpu.v) against the C model
(sim/models/gpu_model.c): every command word written to CMD also goes to the model, and at every
FENCE the harness waits for STATUS.BUSY = 0, then compares all 307,200 bytes of VRAM (through
VRAM's pixel port, so port A is left alone), FENCE and the sticky bits with the model's.
Directed tests (docs/GPU.md sections 4 and 5): the registers and their error rules, every
command at every x mod 4 alignment and size, clipping at each edge, zero and out-of-range sizes
and coordinates, SET_TARGET's refusals, copies overlapping in all eight directions with and
without the key, lines in every octant with their endpoints, EXPAND's bit order and
transparency, WRITE_RECT's byte order, a full FIFO and OVERFLOW, an unknown opcode, fences and
their interrupt, WAIT_VBLANK, ABORT. Then randomized command streams (targets, clips, every
command, coordinates near the edges and far outside) while the CPU keeps reading VRAM and
writing a part of it the engine never draws on, so the engine is stalled in every state.
A broken engine (sim/gpu/mutants.py) must fail here.

    Vtb_gpu [--seed N] [--chunks N]
*/
#include "Vtb_gpu.h"
#include "verilated.h"

#include "gpu_model.h"
#include "surfbus_master.h"
#include "surfcpu/soc.h"

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
    void posedge() {
        t->vblank = pulse || (vblank_every && cycles >= next_vblank);
        if (vblank_every && cycles >= next_vblank) next_vblank = cycles + vblank_every;
        pulse = false;
        t->clk = 0;
        t->eval();
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
    // an unknown opcode: dropped, the next word is a command
    push(bus, GPU_HDR(0x0A, 0x123456));
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

// ------------------------------------------------------------------ randomized streams

static int rnd(int lo, int hi) { return lo + (int)(g_rng() % (uint32_t)(hi - lo + 1)); }

static void random_chunk(Bus &bus, unsigned &tw, unsigned &th) {
    const int n = rnd(1, 24);
    for (int i = 0; i < n; i++) {
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
            if (!(base & 3)) { tw = w; th = h; }
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
            push(bus, GPU_HDR(0x0A + g_rng() % 0xF0, g_rng() & 0xFFFFFF));   // unknown: dropped
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
    test_fill(tb, bus);
    test_target(tb, bus);
    test_copy(tb, bus);
    test_line(tb, bus);
    test_expand(tb, bus);
    test_write_rect(tb, bus);
    test_errors_and_fences(tb, bus);
    const uint64_t directed = g_compares;

    // randomized streams, with the CPU on VRAM meanwhile
    g_noise = true;
    unsigned tw = 640, th = 480;
    set_target(bus, 0, 640, 640, 473);                      // the random part stays below SPARE
    th = 473;
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
    std::printf("test_gpu: PASS (seed %u: %d checks; %llu commands, %llu words, %llu pixel writes in the model; "
                "VRAM compared %llu times (%llu directed, %u random chunks); engine %llu grants, %llu cycles waiting "
                "for the CPU (%llu CPU accesses meanwhile); %llu cycles)\n",
                seed, checks, (unsigned long long)g_cmds, (unsigned long long)g_words, g_eng.writes,
                (unsigned long long)g_compares, (unsigned long long)directed, chunks,
                (unsigned long long)tb.engine_grants, (unsigned long long)tb.engine_waits,
                (unsigned long long)g_cpu_noise, (unsigned long long)tb.cycles);
    return 0;
}
