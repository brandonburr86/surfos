/*
SurfGPU surf_vram testbench
--------------------
File: test_vram.cpp Date: 10/18/26 (SurfGPU)
--------------------
Verilator harness for surf_vram alone (docs/GPU.md section 2), with its two clocks unrelated
(sys_clk edges on even picoseconds, pix_clk edges on odd ones, so the simulator never has to
order a write and a read of the same instant). Three users at once, all checked against a
reference copy of the 307,200 bytes:
  * the CPU window through the SurfBus master of sim/io (one-cycle answers, never a wait;
    bytes, halfwords and words; errors at and above 307,200 bytes and for fetches, with no
    effect);
  * the engine side of port A driven with a random request in most cycles: g_gnt exactly when
    the CPU offers nothing, a granted read's word with g_rvalid in the next cycle and only
    then, words at or above 76,800 read 0 and are never written;
  * the pixel port read at a random address every pix_clk: each word two edges later, 0 above
    the memory.
Then every word is read back through the window. A broken block (tools: sim/gpu/mutants.py)
must fail here.

    Vsurf_vram [--seed N] [--cycles N]
*/
#include "Vsurf_vram.h"
#include "verilated.h"

#include "surfbus_master.h"
#include "surfcpu/soc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            if (failures < 20) {                                           \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__);           \
                std::printf(__VA_ARGS__);                                  \
                std::printf("\n");                                         \
            }                                                              \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static const uint32_t BYTES = 307200, WORDS = BYTES / 4;

struct Expect {                     // what the CPU's outstanding access must return
    bool err;
    bool read;
    uint32_t data;
};

struct Tb {
    Vsurf_vram *t;
    std::mt19937 rng;
    uint64_t now = 0, next_sys, next_pix;
    const uint64_t sys_half, pix_half;
    std::vector<uint8_t> mem;
    std::deque<Expect> cpu;         // at most one entry: the CPU has one access outstanding
    // the engine's stimulus
    int engine_pct = 70;            // chance of a request in a cycle
    bool g_read_pending = false;    // a read was granted at the last edge
    uint32_t g_read_expect = 0;
    uint64_t g_grants = 0, g_reads = 0, g_writes = 0, g_oob = 0, g_denied = 0;
    // the pixel port
    uint32_t pix_prev_expect = 0;
    bool pix_prev_valid = false;
    uint64_t pix_checks = 0, pix_oob = 0;
    uint64_t sys_cycles = 0;

    Tb(Vsurf_vram *top, unsigned seed)
        : t(top), rng(seed), sys_half(20000), pix_half(19862), mem(BYTES, 0) {
        next_sys = sys_half;
        next_pix = 1 + pix_half;
    }

    uint32_t word(uint32_t w) const {
        return w < WORDS ? (uint32_t)mem[4 * w] | (uint32_t)mem[4 * w + 1] << 8 |
                               (uint32_t)mem[4 * w + 2] << 16 | (uint32_t)mem[4 * w + 3] << 24
                         : 0u;
    }
    void put(uint32_t w, uint8_t be, uint32_t v) {
        for (int k = 0; k < 4; k++)
            if (be >> k & 1) mem[4 * w + k] = (uint8_t)(v >> (8 * k));
    }

    void drive_engine() {
        const bool req = (int)(rng() % 100) < engine_pct;
        t->g_req = req;
        t->g_we = rng() & 1;
        t->g_be = rng() & 0xF;
        t->g_addr = (rng() % 100 < 4) ? (WORDS + rng() % (131072 - WORDS)) : (rng() % WORDS);
        t->g_wdata = rng();
    }

    // the edge of clk: what port A does there, by the rules; then the edge itself
    void sys_edge() {
        drive_engine();
        t->eval();
        const bool cpu_req = t->s_req_valid;
        CHECK(t->g_gnt == (t->g_req && !cpu_req), "g_gnt %d with g_req %d, CPU request %d", t->g_gnt, t->g_req,
              cpu_req);
        bool g_read = false;
        uint32_t g_expect = 0;
        if (cpu_req) {
            const uint32_t off = t->s_req_addr & 0x7FFFF;
            const bool ok = !t->s_req_fetch && off < BYTES;
            Expect e{!ok, !t->s_req_write, 0};
            if (ok && t->s_req_write) put(off >> 2, t->s_req_be, t->s_req_wdata);
            if (ok && !t->s_req_write) e.data = word(off >> 2);
            cpu.push_back(e);
            if (t->g_req) g_denied++;
        } else if (t->g_gnt) {
            g_grants++;
            const uint32_t w = t->g_addr;
            if (w >= WORDS) g_oob++;
            if (t->g_we) {
                g_writes++;
                if (w < WORDS) put(w, t->g_be, t->g_wdata);
            } else {
                g_reads++;
                g_read = true;
                g_expect = word(w);
            }
        }
        t->clk = 1;
        t->eval();
        sys_cycles++;
        CHECK(t->g_rvalid == g_read, "g_rvalid %d after a cycle with%s a granted read", t->g_rvalid,
              g_read ? "" : "out");
        if (g_read)
            CHECK(t->g_rdata == g_expect, "engine read: %08x, want %08x", t->g_rdata, g_expect);
    }

    void pix_edge() {
        const uint32_t a = (rng() % 100 < 5) ? (WORDS + rng() % (131072 - WORDS)) : (rng() % WORDS);
        t->pix_addr = a;
        t->eval();
        const uint32_t expect_now = word(a);       // the memory as it is at this edge
        t->pix_clk = 1;
        t->eval();
        if (pix_prev_valid) {
            pix_checks++;
            CHECK(t->pix_data == pix_prev_expect, "pixel port: %08x, want %08x", t->pix_data, pix_prev_expect);
        }
        if (a >= WORDS) pix_oob++;
        // pix_data now shows the word read at the previous edge; this edge's word comes next
        pix_prev_expect = expect_now;
        pix_prev_valid = true;
    }

    // run to the next rising edge of clk (the SurfBus master's posedge callback)
    void posedge() {
        for (;;) {
            const uint64_t tn = std::min(next_sys, next_pix);
            now = tn;
            if (next_pix == tn) {
                if (!t->pix_clk) pix_edge();
                else { t->pix_clk = 0; t->eval(); }
                next_pix += pix_half;
                continue;
            }
            next_sys += sys_half;
            if (!t->clk) {
                sys_edge();
                return;
            }
            t->clk = 0;
            t->eval();
        }
    }
};

using Bus = SurfBusMaster<Vsurf_vram>;

static void cpu_check(Tb &tb, const Bus::Rsp &r, const char *what, uint32_t addr) {
    CHECK(!tb.cpu.empty(), "%s %08x: no access was seen at the edge", what, addr);
    if (tb.cpu.empty()) return;
    const Expect e = tb.cpu.front();
    tb.cpu.pop_front();
    CHECK(r.err == e.err, "%s %08x: err %d, want %d", what, addr, r.err, e.err);
    if (!e.err && e.read) CHECK(r.data == e.data, "%s %08x: %08x, want %08x", what, addr, r.data, e.data);
    CHECK(r.latency == 1 && r.wait == 0, "%s %08x: wait %llu, latency %llu (want 0 and 1)", what, addr,
          (unsigned long long)r.wait, (unsigned long long)r.latency);
}

static Bus::Rsp cpu_read(Tb &tb, Bus &bus, uint32_t off, uint8_t be) {
    Bus::Rsp r = bus.read(SURF_VRAM_BASE + off, be);
    cpu_check(tb, r, "read", off);
    return r;
}
static Bus::Rsp cpu_write(Tb &tb, Bus &bus, uint32_t off, uint32_t v, uint8_t be) {
    Bus::Rsp r = bus.write(SURF_VRAM_BASE + off, v, be);
    cpu_check(tb, r, "write", off);
    return r;
}

// one random CPU access: sizes, lanes, and now and then an address past the memory or a fetch
static void cpu_random(Tb &tb, Bus &bus, std::mt19937 &rng) {
    uint32_t off = (rng() % 100 < 6) ? BYTES + rng() % (0x80000 - BYTES) : rng() % BYTES;
    const int kind = rng() % 3;
    uint8_t be;
    if (kind == 0) { be = (uint8_t)(1u << (off & 3)); }
    else if (kind == 1) { off &= ~1u; be = (off & 2) ? 0xC : 0x3; }
    else { off &= ~3u; be = 0xF; }
    if (rng() % 50 == 0) {
        Bus::Rsp r = bus.fetch(SURF_VRAM_BASE + (off & ~3u));
        cpu_check(tb, r, "fetch", off);
        return;
    }
    if (rng() & 1) cpu_write(tb, bus, off, rng(), be);
    else cpu_read(tb, bus, off, be);
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    unsigned seed = 1;
    uint64_t cycles = 400000;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = std::strtoull(argv[++i], nullptr, 0);
    }
    auto top = std::make_unique<Vsurf_vram>();
    Tb tb(top.get(), seed);
    std::mt19937 rng(seed * 7919u + 1);
    top->clk = 0;
    top->pix_clk = 0;
    top->rst = 1;
    top->s_req_valid = 0;
    top->g_req = 0;
    top->eval();
    tb.engine_pct = 0;
    for (int i = 0; i < 6; i++) tb.posedge();
    top->rst = 0;
    Bus bus(top.get(), [&] { tb.posedge(); }, seed);
    bus.max_gap = 0;

    // ---- directed: lanes, the ends, errors (the engine quiet)
    cpu_write(tb, bus, 0x100, 0x11223344, 0xF);
    cpu_write(tb, bus, 0x101, 0x0000AA00, 0x2);
    cpu_write(tb, bus, 0x102, 0xBBCC0000, 0xC);
    CHECK(cpu_read(tb, bus, 0x100, 0xF).data == 0xBBCCAA44, "lanes");
    cpu_write(tb, bus, BYTES - 4, 0xCAFEF00D, 0xF);
    CHECK(cpu_read(tb, bus, BYTES - 4, 0xF).data == 0xCAFEF00D, "the last word");
    CHECK(cpu_write(tb, bus, BYTES, 0xFFFFFFFF, 0xF).err && cpu_read(tb, bus, BYTES, 0xF).err, "307,200 is an error");
    CHECK(cpu_read(tb, bus, 0x7FFFC, 0xF).err && cpu_write(tb, bus, BYTES + 1, 0xFF00, 0x2).err,
          "the window's top is an error");
    {
        Bus::Rsp r = bus.fetch(SURF_VRAM_BASE + 0x100);
        cpu_check(tb, r, "fetch", 0x100);
        CHECK(r.err, "a fetch is an error");
    }
    // a write past the end changed nothing (the bank multiplexer wraps 4,096-word banks)
    CHECK(cpu_read(tb, bus, BYTES - 4, 0xF).data == 0xCAFEF00D && cpu_read(tb, bus, 0, 0xF).data == 0,
          "an erroring write changed memory");

    // ---- random: all three at once
    tb.engine_pct = 70;
    while (tb.sys_cycles < cycles) {
        if (rng() % 8 == 0) bus.idle(1 + rng() % 4);      // gaps for the engine
        cpu_random(tb, bus, rng);
    }
    // the engine alone for a while (it gets every cycle)
    const uint64_t g0 = tb.g_grants;
    bus.idle(2000);
    CHECK(tb.g_grants - g0 > 1000, "the engine alone got %llu grants in 2000 cycles",
          (unsigned long long)(tb.g_grants - g0));

    // ---- every word back through the window
    tb.engine_pct = 0;
    bus.idle(4);
    int bad = 0;
    for (uint32_t w = 0; w < WORDS; w++) {
        Bus::Rsp r = cpu_read(tb, bus, 4 * w, 0xF);
        if (r.data != tb.word(w)) bad++;
    }
    CHECK(bad == 0, "%d words differ in the final read-back", bad);
    CHECK(bus.problems.empty(), "SurfBus protocol: %s", bus.problems.empty() ? "" : bus.problems[0].c_str());

    if (failures) {
        std::printf("test_vram: FAIL (%d of %d checks, seed %u)\n", failures, checks, seed);
        return 1;
    }
    std::printf("test_vram: PASS (seed %u: %d checks; %llu CPU accesses, %llu errors, all answered in one cycle; "
                "engine %llu grants (%llu reads, %llu writes, %llu past the end), %llu waits for the CPU; "
                "%llu pixel-port reads (%llu past the end); %u words read back)\n",
                seed, checks, (unsigned long long)bus.responses, (unsigned long long)bus.error_responses,
                (unsigned long long)tb.g_grants, (unsigned long long)tb.g_reads, (unsigned long long)tb.g_writes,
                (unsigned long long)tb.g_oob, (unsigned long long)tb.g_denied, (unsigned long long)tb.pix_checks,
                (unsigned long long)tb.pix_oob, WORDS);
    return 0;
}
