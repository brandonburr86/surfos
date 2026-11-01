/*
SurfCPU surf_ps2 testbench
--------------------
File: test_ps2.cpp Date: 10/15/26 (SurfCPU)
--------------------
Verilator harness for surf_ps2 (top module): a SurfBus master in place of the CPU, and
Ps2KeyboardModel on the open-drain lines (both sides pull down, the lines are the AND).
Checks:
  * reset values and the register rules (errors for unknown offsets, partial words and
    fetches, with no FIFO pop; TXDATA reads 0; write-1-to-clear);
  * typing at 10, 12.5 and 16.7 kHz keyboard clocks: every scan code byte arrives in
    order, and a scan code set 2 decoder here turns them back into the typed text;
    extended keys (E0, Print Screen, Pause), modifiers, typematic repeat;
  * a parity error, bad start and stop bits, a partial frame (abandoned after 2 ms), a
    clock glitch shorter than the filter: the bad frame is dropped and counted, the next
    one arrives; the counters saturate at 255;
  * FIFO overflow (16 kept, the rest counted lost), RX_EN, both interrupt sources;
  * host commands with the keyboard's acknowledge and answers (LEDs, ID, echo, reset),
    TXDATA ignored while busy, a missing acknowledge, a keyboard that stops clocking and a
    missing keyboard (TX_NAK), a command written in the middle of a keyboard frame (it waits
    for the gap: no resend), and TXDATA at every cycle around the parity and stop edges with
    the keyboard committing at either point (no byte ever lost);
  * refused accesses (partial words, fetches) with live state: no pop, no clear;
  * the open-drain discipline at the pins: the clock is pulled low only for the inhibit of
    a send (125 + 5 us, never toggled), data changes only while the clock line is low,
    and the keyboard model reports no protocol violation by the host.

    Vsurf_ps2 [--seed N]
*/
#include "Vsurf_ps2.h"
#include "verilated.h"

#include "ps2_keyboard_model.h"
#include "surfbus_master.h"
#include "surfcpu/soc.h"

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

// A port that is stuck (a send that never ends, a keyboard that never gets its lines back)
// makes everything after it meaningless: report it and stop at once.
#define REQUIRE(cond, ...)                                                 \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\ntest_ps2: FAIL (stopped: the port is stuck)\n");  \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

static const uint32_t B = SURF_PS2_BASE;
static const uint32_t SYS_HZ = SURF_SYS_HZ;
static const uint64_t US = SYS_HZ / 1000000;        // clk cycles per microsecond

struct Tb {
    Vsurf_ps2 *top;
    Ps2KeyboardModel *kb;
    uint64_t cycle = 0;
    bool clk_line = true, data_line = true;
    // pin discipline
    bool p_clk_oe = false, p_dat_oe = false, p_clk_line = true;
    uint64_t clk_oe_since = 0, clk_oe_pulses = 0, clk_oe_min = ~0ull, clk_oe_max = 0;
    uint64_t dat_oe_changes = 0, dat_oe_bad = 0;
    // the keyboard's own frames, from the lines (a low pulse under 10 us is a glitch)
    uint64_t cand_fall = 0, last_fall = 0, dev_falls = 0, glitches = 0, period_min = ~0ull, period_max = 0;

    Tb(Vsurf_ps2 *t, Ps2KeyboardModel *k) : top(t), kb(k) {}

    void posedge() {
        top->clk = 1;
        top->eval();
        cycle++;
        kb->tick(top->ps2_clk_oe, top->ps2_data_oe, clk_line, data_line);
        top->ps2_clk_i = clk_line;
        top->ps2_data_i = data_line;
        monitor();
        top->clk = 0;
        top->eval();
    }

    void monitor() {
        const bool clk_oe = top->ps2_clk_oe, dat_oe = top->ps2_data_oe;
        if (clk_oe && !p_clk_oe) clk_oe_since = cycle;
        if (!clk_oe && p_clk_oe) {
            const uint64_t len = cycle - clk_oe_since;
            clk_oe_pulses++;
            if (len < clk_oe_min) clk_oe_min = len;
            if (len > clk_oe_max) clk_oe_max = len;
        }
        if (dat_oe != p_dat_oe) {
            dat_oe_changes++;
            if (p_clk_line) dat_oe_bad++;            // must change only while the clock is low
        }
        // falling edges the keyboard makes on its own (no host pull): the bit period
        if (!clk_line && p_clk_line && !clk_oe) cand_fall = cycle;
        if (clk_line && !p_clk_line && cand_fall) {
            if (cycle - cand_fall >= 10 * US) {
                const uint64_t d = cand_fall - last_fall;
                if (last_fall && d < 3 * kb->ticks_per_bit() / 2) {
                    if (d < period_min) period_min = d;
                    if (d > period_max) period_max = d;
                }
                last_fall = cand_fall;
                dev_falls++;
            } else {
                glitches++;
            }
            cand_fall = 0;
        }
        p_clk_oe = clk_oe;
        p_dat_oe = dat_oe;
        p_clk_line = clk_line;
    }
};

using Bus = SurfBusMaster<Vsurf_ps2>;

struct Host {
    Tb &tb;
    Bus &bus;
    std::vector<uint8_t> got;
    Host(Tb &t, Bus &b) : tb(t), bus(b) {}

    // Collect bytes until the keyboard is idle and the FIFO empty. The timeouts (2 s simulated)
    // are several times the longest burst any test sends.
    void drain(uint64_t timeout = 50000000) {
        const uint64_t end = tb.cycle + timeout;
        int quiet = 0;
        for (;;) {
            REQUIRE(tb.cycle < end, "the keyboard never went idle while draining");
            const uint32_t d = bus.rd(B + SURF_PS2_DATA);
            if (!(d & SURF_PS2_DATA_EMPTY_MASK)) {
                CHECK((d & ~0xFFu) == 0, "DATA with EMPTY clear has stray bits: %08x", d);
                got.push_back(static_cast<uint8_t>(d));
                quiet = 0;
                continue;
            }
            CHECK((d & 0xFF) == 0, "DATA with EMPTY set has data bits: %08x", d);
            if (tb.kb->idle() && ++quiet > 3) break;
            bus.idle(300);
        }
    }
    void wait_kb_idle(uint64_t timeout = 50000000) {
        const uint64_t end = tb.cycle + timeout;
        while (!tb.kb->idle()) {
            REQUIRE(tb.cycle < end, "the keyboard never went idle");
            bus.idle(500);
        }
        bus.idle(400 * US);                           // the last frame's stop bit and the filter
    }
    // write TXDATA, follow it cycle by cycle: when the clock was first pulled (0: never), when
    // TX_BUSY dropped; false if it never did
    bool send_traced(uint8_t b, uint64_t &oe_at, uint64_t &done_at, uint64_t timeout = 30000 * US) {
        bus.wr(B + SURF_PS2_TXDATA, b);
        const uint64_t t0 = tb.cycle, end = tb.cycle + timeout;
        oe_at = 0;
        while (tb.cycle < end) {
            if (!oe_at && tb.top->ps2_clk_oe) oe_at = tb.cycle;
            if (((tb.cycle - t0) & 63) == 0 && !(bus.rd(B + SURF_PS2_STATUS) & SURF_PS2_STATUS_TX_BUSY_MASK)) {
                done_at = tb.cycle;
                return true;
            }
            bus.idle(1);
        }
        REQUIRE(false, "TX_BUSY still set %llu us after writing TXDATA %02x", (unsigned long long)(timeout / US), b);
        return false;
    }
    bool send(uint8_t b, uint64_t timeout = 30000 * US) {   // write TXDATA, wait for TX_BUSY to drop
        bus.wr(B + SURF_PS2_TXDATA, b);
        const uint64_t end = tb.cycle + timeout;
        while (tb.cycle < end) {
            if (!(bus.rd(B + SURF_PS2_STATUS) & SURF_PS2_STATUS_TX_BUSY_MASK)) return true;
            bus.idle(200);
        }
        REQUIRE(false, "TX_BUSY still set %llu us after writing TXDATA %02x", (unsigned long long)(timeout / US), b);
        return false;
    }
};

// scan code set 2 back to text, as a keyboard driver would (US layout)
static std::string decode_set2(const std::vector<uint8_t> &codes) {
    struct M { uint8_t code; char lo, hi; };
    static const M map[] = {
        {0x1C, 'a', 'A'}, {0x32, 'b', 'B'}, {0x21, 'c', 'C'}, {0x23, 'd', 'D'}, {0x24, 'e', 'E'}, {0x2B, 'f', 'F'},
        {0x34, 'g', 'G'}, {0x33, 'h', 'H'}, {0x43, 'i', 'I'}, {0x3B, 'j', 'J'}, {0x42, 'k', 'K'}, {0x4B, 'l', 'L'},
        {0x3A, 'm', 'M'}, {0x31, 'n', 'N'}, {0x44, 'o', 'O'}, {0x4D, 'p', 'P'}, {0x15, 'q', 'Q'}, {0x2D, 'r', 'R'},
        {0x1B, 's', 'S'}, {0x2C, 't', 'T'}, {0x3C, 'u', 'U'}, {0x2A, 'v', 'V'}, {0x1D, 'w', 'W'}, {0x22, 'x', 'X'},
        {0x35, 'y', 'Y'}, {0x1A, 'z', 'Z'}, {0x16, '1', '!'}, {0x1E, '2', '@'}, {0x26, '3', '#'}, {0x25, '4', '$'},
        {0x2E, '5', '%'}, {0x36, '6', '^'}, {0x3D, '7', '&'}, {0x3E, '8', '*'}, {0x46, '9', '('}, {0x45, '0', ')'},
        {0x0E, '`', '~'}, {0x4E, '-', '_'}, {0x55, '=', '+'}, {0x54, '[', '{'}, {0x5B, ']', '}'}, {0x5D, '\\', '|'},
        {0x4C, ';', ':'}, {0x52, '\'', '"'}, {0x41, ',', '<'}, {0x49, '.', '>'}, {0x4A, '/', '?'}, {0x29, ' ', ' '},
        {0x5A, '\n', '\n'}, {0x66, '\b', '\b'}, {0x0D, '\t', '\t'}, {0x76, '\x1b', '\x1b'},
    };
    std::string out;
    bool ext = false, brk = false, shift = false;
    for (uint8_t c : codes) {
        if (c == 0xE0) { ext = true; continue; }
        if (c == 0xF0) { brk = true; continue; }
        if (!ext && (c == 0x12 || c == 0x59)) shift = !brk;
        else if (!brk && !ext) {
            for (const M &m : map)
                if (m.code == c) { out += shift ? m.hi : m.lo; break; }
        }
        ext = brk = false;
    }
    return out;
}

static std::string hexs(const std::vector<uint8_t> &v, size_t max = 24) {
    std::string s;
    char b[8];
    for (size_t i = 0; i < v.size() && i < max; i++) {
        std::snprintf(b, sizeof b, "%02x ", v[i]);
        s += b;
    }
    if (v.size() > max) s += "...";
    return s;
}

static std::string random_text(std::mt19937 &rng, int n) {
    static const char chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 `~!@#$%^&*()-_=+[]{}\\|;:'\",.<>/?";
    std::string s;
    for (int i = 0; i < n; i++) s += chars[rng() % (sizeof chars - 1)];
    return s;
}

// ------------------------------------------------------------------ tests

static void test_registers(Tb &tb, Bus &bus) {
    CHECK(tb.top->ps2_clk_oe == 0 && tb.top->ps2_data_oe == 0, "lines pulled after reset");
    CHECK(bus.rd(B + SURF_PS2_CTRL) == SURF_PS2_CTRL_RX_EN_MASK, "CTRL after reset");
    CHECK(bus.rd(B + SURF_PS2_STATUS) == 0, "STATUS after reset");
    CHECK(bus.rd(B + SURF_PS2_ERRCOUNT) == 0, "ERRCOUNT after reset");
    CHECK(bus.rd(B + SURF_PS2_DATA) == SURF_PS2_DATA_EMPTY_MASK, "DATA after reset");
    CHECK(bus.rd(B + SURF_PS2_TXDATA) == 0, "TXDATA reads 0");
    CHECK(tb.top->irq == 0, "irq after reset");
    const uint32_t bad[] = {0x014, 0x018, 0x020, 0x800, 0x003};
    for (uint32_t off : bad) {
        CHECK(bus.read(B + off).err && bus.write(B + off, 0).err, "offset %03x not an error", off);
    }
    CHECK(bus.read(B + SURF_PS2_DATA, 0x1).err && bus.read(B + SURF_PS2_DATA, 0x3).err, "partial DATA read not an error");
    CHECK(bus.write(B + SURF_PS2_TXDATA, 0xEE, 0x1).err, "partial TXDATA write not an error");
    CHECK(bus.fetch(B + SURF_PS2_DATA).err, "fetch not an error");
    bus.idle(50);
    CHECK(tb.top->ps2_clk_oe == 0 && tb.kb->host_bytes.empty(), "a refused TXDATA write started a send");
    // writes to the read-only registers are ignored, not errors
    CHECK(!bus.write(B + SURF_PS2_ERRCOUNT, 0xFFFFFFFF).err && bus.rd(B + SURF_PS2_ERRCOUNT) == 0, "ERRCOUNT write");
    CHECK(!bus.write(B + SURF_PS2_DATA, 0x12).err && bus.rd(B + SURF_PS2_DATA) == SURF_PS2_DATA_EMPTY_MASK, "DATA write");
}

static void test_typing(Tb &tb, Bus &bus, Host &host, std::mt19937 &rng) {
    const std::string text = "Hello, SurfOS! " + random_text(rng, 24) + "\n";
    host.got.clear();
    tb.kb->type_text(text);
    host.drain();
    const std::vector<uint8_t> want = Ps2KeyboardModel::text_codes(text);
    CHECK(host.got == want, "typing: %zu bytes, want %zu\n  got  %s\n  want %s", host.got.size(), want.size(),
          hexs(host.got).c_str(), hexs(want).c_str());
    const std::string back = decode_set2(host.got);
    CHECK(back == text, "decoded text differs: '%s'", back.c_str());
    CHECK(bus.rd(B + SURF_PS2_STATUS) == 0, "STATUS after clean typing: %08x", bus.rd(B + SURF_PS2_STATUS));
    std::printf("typing at %u ticks/bit: %zu chars, %zu bytes, decoded back exactly\n", tb.kb->ticks_per_bit(),
                text.size(), host.got.size());
}

static void test_keys(Tb &tb, Bus &, Host &host) {
    host.got.clear();
    Ps2KeyboardModel &kb = *tb.kb;
    kb.key_tap("UP");
    kb.key_tap("PRTSC");
    kb.key_tap("PAUSE");
    kb.key_make("LCTRL");
    kb.key_tap("C");
    kb.key_break("LCTRL");
    kb.key_make("RALT");
    kb.key_tap("TAB");
    kb.key_break("RALT");
    kb.key_tap("KP_ENTER");
    kb.key_hold("A", 5);
    kb.key_tap("DELETE");
    host.drain();
    const std::vector<uint8_t> want = {
        0xE0, 0x75, 0xE0, 0xF0, 0x75,                                    // up
        0xE0, 0x12, 0xE0, 0x7C, 0xE0, 0xF0, 0x7C, 0xE0, 0xF0, 0x12,      // print screen
        0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77,                  // pause
        0x14, 0x21, 0xF0, 0x21, 0xF0, 0x14,                              // ctrl-c
        0xE0, 0x11, 0x0D, 0xF0, 0x0D, 0xE0, 0xF0, 0x11,                  // altgr-tab
        0xE0, 0x5A, 0xE0, 0xF0, 0x5A,                                    // keypad enter
        0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0xF0, 0x1C,                  // a held: typematic
        0xE0, 0x71, 0xE0, 0xF0, 0x71,                                    // delete
    };
    CHECK(host.got == want, "keys:\n  got  %s\n  want %s", hexs(host.got, 64).c_str(), hexs(want, 64).c_str());
}

static void test_errors(Tb &tb, Bus &bus, Host &host) {
    Ps2KeyboardModel &kb = *tb.kb;
    bus.wr(B + SURF_PS2_STATUS, 0xFF);
    host.got.clear();
    kb.queue_frame(0x1C, Ps2KeyboardModel::Fault::PARITY);
    kb.queue_frame(0x32);
    host.drain();
    uint32_t st = bus.rd(B + SURF_PS2_STATUS);
    uint32_t ec = bus.rd(B + SURF_PS2_ERRCOUNT);
    CHECK(host.got == std::vector<uint8_t>{0x32}, "after a parity error: %s", hexs(host.got).c_str());
    CHECK(st == SURF_PS2_STATUS_PARITY_MASK && ec == 0x00000001, "parity: STATUS %08x ERRCOUNT %08x", st, ec);
    bus.wr(B + SURF_PS2_STATUS, SURF_PS2_STATUS_PARITY_MASK);
    CHECK(bus.rd(B + SURF_PS2_STATUS) == 0, "PARITY not cleared");

    host.got.clear();
    kb.queue_frame(0x24, Ps2KeyboardModel::Fault::STOP);
    kb.queue_frame(0x2C);
    host.drain();
    st = bus.rd(B + SURF_PS2_STATUS);
    ec = bus.rd(B + SURF_PS2_ERRCOUNT);
    CHECK(host.got == std::vector<uint8_t>{0x2C}, "after a bad stop bit: %s", hexs(host.got).c_str());
    CHECK(st == SURF_PS2_STATUS_FRAME_MASK && ec == 0x00000101, "bad stop bit: STATUS %08x ERRCOUNT %08x", st, ec);
    bus.wr(B + SURF_PS2_STATUS, 0xFF);
    // A start bit of 1: frame 0x2D is 1 1 0 1 1 0 1 0 0 1 1 on the line. The first two edges
    // are refused (FRAME), the 0 of d1 looks like a start bit and the frame ends one edge
    // short; a typing pause longer than 2 ms abandons it (TIMEOUT) and the next byte is clean.
    host.got.clear();
    kb.queue_frame(0x2D, Ps2KeyboardModel::Fault::START);
    kb.queue_pause(3000);
    kb.queue_frame(0x2C);
    host.drain();
    st = bus.rd(B + SURF_PS2_STATUS);
    ec = bus.rd(B + SURF_PS2_ERRCOUNT);
    CHECK(host.got == std::vector<uint8_t>{0x2C}, "after a bad start bit: %s", hexs(host.got).c_str());
    CHECK(st == (SURF_PS2_STATUS_FRAME_MASK | SURF_PS2_STATUS_TIMEOUT_MASK) && ec == 0x00010301,
          "bad start bit: STATUS %08x ERRCOUNT %08x", st, ec);
    bus.wr(B + SURF_PS2_STATUS, 0xFF);

    host.got.clear();
    const uint32_t ec0 = bus.rd(B + SURF_PS2_ERRCOUNT);
    kb.queue_frame(0x1B, Ps2KeyboardModel::Fault::PARTIAL, 6);   // start and d0..d4, then 3 ms of nothing
    kb.queue_frame(0x23);
    host.drain();
    st = bus.rd(B + SURF_PS2_STATUS);
    ec = bus.rd(B + SURF_PS2_ERRCOUNT);
    CHECK(host.got == std::vector<uint8_t>{0x23}, "after a partial frame: %s", hexs(host.got).c_str());
    CHECK(st == SURF_PS2_STATUS_TIMEOUT_MASK && ((ec >> 16) & 0xFF) == ((ec0 >> 16) & 0xFF) + 1,
          "partial frame: STATUS %08x ERRCOUNT %08x", st, ec);
    bus.wr(B + SURF_PS2_STATUS, 0xFF);

    host.got.clear();
    kb.queue_frame(0x4D, Ps2KeyboardModel::Fault::GLITCH);
    kb.queue_frame(0x44, Ps2KeyboardModel::Fault::GLITCH);
    host.drain();
    st = bus.rd(B + SURF_PS2_STATUS);
    CHECK(host.got == (std::vector<uint8_t>{0x4D, 0x44}) && st == 0, "glitched frames: %s, STATUS %08x",
          hexs(host.got).c_str(), st);
    CHECK(tb.glitches == 2, "%llu glitches on the clock line, want 2", (unsigned long long)tb.glitches);
    CHECK(kb.stats.faults_sent >= 6, "faults sent %llu", (unsigned long long)kb.stats.faults_sent);
}

static void test_overflow_and_irq(Tb &tb, Bus &bus, Host &host) {
    Ps2KeyboardModel &kb = *tb.kb;
    bus.wr(B + SURF_PS2_STATUS, 0xFF);
    const uint32_t ec0 = bus.rd(B + SURF_PS2_ERRCOUNT);
    bus.wr(B + SURF_PS2_CTRL, SURF_PS2_CTRL_RX_EN_MASK | SURF_PS2_CTRL_RX_IE_MASK);
    bus.idle(2);
    CHECK(tb.top->irq == 0, "RX irq with an empty FIFO");
    std::vector<uint8_t> sent;
    for (int i = 0; i < 20; i++) sent.push_back(static_cast<uint8_t>(0x10 + i));
    kb.queue_bytes(sent);
    // the first byte raises the interrupt
    uint64_t n = 0;
    while (!tb.top->irq && n++ < 100000) bus.idle(10);
    CHECK(tb.top->irq == 1, "no RX interrupt");
    host.wait_kb_idle();
    uint32_t st = bus.rd(B + SURF_PS2_STATUS);
    uint32_t ec = bus.rd(B + SURF_PS2_ERRCOUNT);
    CHECK((st & (SURF_PS2_STATUS_RX_FULL_MASK | SURF_PS2_STATUS_OVF_MASK | SURF_PS2_STATUS_RX_AVAIL_MASK)) ==
              (SURF_PS2_STATUS_RX_FULL_MASK | SURF_PS2_STATUS_OVF_MASK | SURF_PS2_STATUS_RX_AVAIL_MASK),
          "overflow: STATUS %08x", st);
    CHECK((ec >> 24) == (ec0 >> 24) + 4, "overflow: ERRCOUNT.OVF %u, want +4", ec >> 24);
    // live state for the error rules: the FIFO full, OVF and now PARITY set. Refused accesses
    // (partial words, fetches) must not pop, clear or change anything.
    kb.queue_frame(0x99, Ps2KeyboardModel::Fault::PARITY);
    host.wait_kb_idle();
    st = bus.rd(B + SURF_PS2_STATUS);
    ec = bus.rd(B + SURF_PS2_ERRCOUNT);
    const uint32_t ctrl = bus.rd(B + SURF_PS2_CTRL);
    CHECK(st & SURF_PS2_STATUS_PARITY_MASK, "no PARITY for the live-state checks: %08x", st);
    const uint8_t bes[] = {0x1, 0x3, 0x7, 0xE, 0x8, 0x0};
    for (uint8_t be : bes) {
        CHECK(bus.read(B + SURF_PS2_DATA, be).err, "partial DATA read (be %x) not an error", be);
        CHECK(bus.write(B + SURF_PS2_STATUS, 0xFFFFFFFF, be).err, "partial STATUS write (be %x) not an error", be);
        CHECK(bus.write(B + SURF_PS2_CTRL, 0, be).err, "partial CTRL write (be %x) not an error", be);
    }
    CHECK(bus.fetch(B + SURF_PS2_DATA).err && bus.fetch(B + SURF_PS2_STATUS).err, "fetches not errors");
    CHECK(bus.rd(B + SURF_PS2_STATUS) == st && bus.rd(B + SURF_PS2_ERRCOUNT) == ec && bus.rd(B + SURF_PS2_CTRL) == ctrl,
          "refused accesses changed STATUS %08x (was %08x), ERRCOUNT or CTRL", bus.rd(B + SURF_PS2_STATUS), st);
    bus.wr(B + SURF_PS2_STATUS, SURF_PS2_STATUS_PARITY_MASK);
    std::vector<uint8_t> got;
    for (int i = 0; i < 17; i++) {
        uint32_t d = bus.rd(B + SURF_PS2_DATA);
        if (d & SURF_PS2_DATA_EMPTY_MASK) break;
        got.push_back(static_cast<uint8_t>(d));
    }
    CHECK(got == std::vector<uint8_t>(sent.begin(), sent.begin() + 16), "after overflow got %s", hexs(got).c_str());
    bus.idle(1);
    CHECK(tb.top->irq == 0, "RX irq with the FIFO emptied");
    // ERR_IE: an error raises the interrupt until cleared
    bus.wr(B + SURF_PS2_CTRL, SURF_PS2_CTRL_RX_EN_MASK | SURF_PS2_CTRL_ERR_IE_MASK);
    bus.idle(1);
    CHECK(tb.top->irq == 1, "ERR irq with OVF set");
    bus.wr(B + SURF_PS2_STATUS, SURF_PS2_STATUS_OVF_MASK);
    bus.idle(1);
    CHECK(tb.top->irq == 0, "ERR irq after clearing OVF");
    // RX_EN = 0: frames are ignored
    bus.wr(B + SURF_PS2_CTRL, 0);
    kb.queue_bytes({0x55, 0x66});
    host.wait_kb_idle();
    CHECK(bus.rd(B + SURF_PS2_DATA) == SURF_PS2_DATA_EMPTY_MASK && bus.rd(B + SURF_PS2_STATUS) == 0,
          "bytes received with RX_EN = 0");
    bus.wr(B + SURF_PS2_CTRL, SURF_PS2_CTRL_RX_EN_MASK);
    host.got.clear();
    kb.queue_bytes({0x77});
    host.drain();
    CHECK(host.got == std::vector<uint8_t>{0x77}, "after RX_EN again: %s", hexs(host.got).c_str());
}

static void test_commands(Tb &tb, Bus &bus, Host &host) {
    Ps2KeyboardModel &kb = *tb.kb;
    bus.wr(B + SURF_PS2_STATUS, 0xFF);
    const uint64_t pulls0 = tb.clk_oe_pulses;
    // each step: a byte to send and the answer bytes it brings, waited for as a driver does
    struct Step { uint8_t out; std::vector<uint8_t> answer; };
    const std::vector<std::vector<Step>> cmds = {
        {{0xED, {0xFA}}, {0x02, {0xFA}}},      // LEDs: caps lock
        {{0xF2, {0xFA, 0xAB, 0x83}}},          // identify
        {{0xEE, {0xEE}}},                      // echo
        {{0xF5, {0xFA}}, {0xF4, {0xFA}}},      // disable, enable
        {{0xFF, {0xFA, 0xAA}}},                // reset and self test
    };
    size_t total = 0;
    for (const auto &steps : cmds) {
        for (const Step &st1 : steps) {
            const size_t before = kb.host_bytes.size();
            host.got.clear();
            CHECK(host.send(st1.out), "send %02x: TX_BUSY stuck", st1.out);
            const uint64_t end = tb.cycle + 20000 * US;
            while (host.got.size() < st1.answer.size() && tb.cycle < end) {
                const uint32_t d = bus.rd(B + SURF_PS2_DATA);
                if (!(d & SURF_PS2_DATA_EMPTY_MASK)) host.got.push_back(static_cast<uint8_t>(d));
                else bus.idle(500);
            }
            CHECK(host.got == st1.answer, "command byte %02x: answer %s", st1.out, hexs(host.got).c_str());
            CHECK(kb.host_bytes.size() == before + 1 && kb.host_bytes.back() == st1.out,
                  "command byte %02x: the keyboard received %s", st1.out, hexs(kb.host_bytes).c_str());
            if (st1.out == 0x02) CHECK(kb.leds == 2, "LEDs %u after ED 02", kb.leds);
            total++;
        }
    }
    CHECK(kb.leds == 0, "LEDs after reset %u", kb.leds);    // the reset at the end cleared them
    uint32_t st = bus.rd(B + SURF_PS2_STATUS);
    CHECK(st == 0, "STATUS after commands %08x (TX_NAK?)", st);
    CHECK(tb.clk_oe_pulses - pulls0 == total, "%llu clock pulls for %zu bytes sent",
          (unsigned long long)(tb.clk_oe_pulses - pulls0), total);
    // LEDs really set: send ED 07 and look at the model
    host.got.clear();
    CHECK(host.send(0xED) && host.send(0x07), "ED 07");
    host.drain();
    CHECK(kb.leds == 7 && host.got == (std::vector<uint8_t>{0xFA, 0xFA}), "LEDs %u, answer %s", kb.leds,
          hexs(host.got).c_str());
    // TXDATA while busy is ignored
    host.got.clear();
    const size_t before = kb.host_bytes.size();
    bus.wr(B + SURF_PS2_TXDATA, 0xF4);
    bus.wr(B + SURF_PS2_TXDATA, 0xF5);              // ignored: TX_BUSY
    CHECK(bus.rd(B + SURF_PS2_STATUS) & SURF_PS2_STATUS_TX_BUSY_MASK, "TX_BUSY not set during a send");
    const uint64_t busy_end = tb.cycle + 30000 * US;
    while (bus.rd(B + SURF_PS2_STATUS) & SURF_PS2_STATUS_TX_BUSY_MASK) {
        REQUIRE(tb.cycle < busy_end, "TX_BUSY stuck after a write during a send");
        bus.idle(200);
    }
    host.drain();
    CHECK(kb.host_bytes.size() == before + 1 && kb.host_bytes.back() == 0xF4, "write during TX_BUSY was sent");
    // a missing acknowledge
    kb.no_ack = 1;
    host.got.clear();
    CHECK(host.send(0xF4), "send without acknowledge");
    st = bus.rd(B + SURF_PS2_STATUS);
    CHECK(st & SURF_PS2_STATUS_TX_NAK_MASK, "no TX_NAK for a missing acknowledge: %08x", st);
    host.drain();
    bus.wr(B + SURF_PS2_STATUS, SURF_PS2_STATUS_TX_NAK_MASK);
    CHECK(bus.rd(B + SURF_PS2_STATUS) == 0, "TX_NAK not cleared");
    // the keyboard stops clocking after 4 bits of a host byte: the 2 ms frame limit gives
    // TX_NAK (the clock was pulled 130 us, then 100 us until the keyboard's first clock)
    kb.host_stall_bits = 4;
    uint64_t oe_at = 0, done_at = 0;
    const uint64_t ts = tb.cycle, bad_stall0 = tb.dat_oe_bad;
    const bool stall_done = host.send_traced(0xF4, oe_at, done_at, 40000 * US);
    st = bus.rd(B + SURF_PS2_STATUS);
    const uint64_t stall_us = (done_at - ts) / US;
    CHECK(stall_done && (st & SURF_PS2_STATUS_TX_NAK_MASK) && stall_us > 2200 && stall_us < 2400,
          "keyboard stopped clocking: done %d after %llu us, STATUS %08x", stall_done, (unsigned long long)stall_us, st);
    CHECK(tb.top->ps2_clk_oe == 0 && tb.top->ps2_data_oe == 0, "lines still pulled after the 2 ms limit");
    // the one data change outside a low clock: letting data go when the send is given up
    CHECK(tb.dat_oe_bad - bad_stall0 <= 1, "%llu data_oe changes with the clock high in the stalled send",
          (unsigned long long)(tb.dat_oe_bad - bad_stall0));
    tb.dat_oe_bad = bad_stall0;
    bus.wr(B + SURF_PS2_STATUS, SURF_PS2_STATUS_TX_NAK_MASK);
    host.wait_kb_idle();
    host.got.clear();
    CHECK(host.send(0xF4) && !(bus.rd(B + SURF_PS2_STATUS) & SURF_PS2_STATUS_TX_NAK_MASK), "send after the stall");
    host.drain();
    CHECK(host.got == std::vector<uint8_t>{0xFA}, "answer after the stall: %s", hexs(host.got).c_str());
    // a keyboard that does not answer: TX_NAK after 15 ms, lines released (data is let go with
    // the clock high then: the one change of data_oe allowed outside a low clock)
    const uint64_t bad0 = tb.dat_oe_bad;
    kb.present = false;
    const uint64_t t0 = tb.cycle;
    CHECK(host.send(0xF4, 40000 * US), "send to a missing keyboard never finished");
    const uint64_t took = tb.cycle - t0;
    st = bus.rd(B + SURF_PS2_STATUS);
    CHECK((st & SURF_PS2_STATUS_TX_NAK_MASK) && took > 15000 * US && took < 16000 * US,
          "missing keyboard: STATUS %08x after %llu us", st, (unsigned long long)(took / US));
    CHECK(tb.top->ps2_clk_oe == 0 && tb.top->ps2_data_oe == 0, "lines still pulled after a failed send");
    CHECK(tb.dat_oe_bad == bad0 + 1, "%llu data_oe changes with the clock high during the failed send",
          (unsigned long long)(tb.dat_oe_bad - bad0));
    tb.dat_oe_bad = bad0;
    kb.present = true;
    bus.wr(B + SURF_PS2_STATUS, 0xFF);
    host.wait_kb_idle();
    std::printf("commands: %zu host bytes acknowledged, clock pulls %llu..%llu us, no-ack and missing keyboard give "
                "TX_NAK (%llu us)\n",
                kb.host_bytes.size(), (unsigned long long)(tb.clk_oe_min / US), (unsigned long long)(tb.clk_oe_max / US),
                (unsigned long long)(took / US));
}

static void test_command_while_typing(Tb &tb, Bus &bus, Host &host) {
    // A command written in the middle of a keyboard frame waits for the gap after it: TX_BUSY
    // at once, the inhibit only after the frame; the keyboard never has to send it again.
    Ps2KeyboardModel &kb = *tb.kb;
    bus.wr(B + SURF_PS2_STATUS, 0xFF);
    host.got.clear();
    const std::string text = "a command waits for the frame";
    kb.type_text(text);
    const uint64_t aborted0 = kb.stats.frames_aborted;
    uint64_t n = 0;
    while (kb.sending_bit() != 5 && n++ < 1000000) bus.idle(1);
    const uint64_t t0 = tb.cycle;
    bus.wr(B + SURF_PS2_TXDATA, 0xED);
    CHECK(bus.rd(B + SURF_PS2_STATUS) & SURF_PS2_STATUS_TX_BUSY_MASK, "TX_BUSY not set while the command waits");
    uint64_t oe_at = 0;
    const uint64_t busy_end = tb.cycle + 30000 * US;
    while (bus.rd(B + SURF_PS2_STATUS) & SURF_PS2_STATUS_TX_BUSY_MASK) {
        REQUIRE(tb.cycle < busy_end, "TX_BUSY stuck after a command written in mid-frame");
        for (int i = 0; i < 32; i++) {
            if (!oe_at && tb.top->ps2_clk_oe) oe_at = tb.cycle;
            bus.idle(1);
        }
    }
    const uint64_t waited = oe_at - t0;
    // the rest of the frame (bits 5..10, then the stop bit's clock high) and the filter
    CHECK(oe_at && waited > 5 * kb.ticks_per_bit() && waited < 7 * kb.ticks_per_bit(),
          "the inhibit came %llu cycles after the write, want after the frame (%u per bit)",
          (unsigned long long)waited, kb.ticks_per_bit());
    CHECK(host.send(0x01), "LED argument");
    host.drain();
    CHECK(kb.stats.frames_aborted == aborted0, "the host interrupted a keyboard frame (%llu resends)",
          (unsigned long long)(kb.stats.frames_aborted - aborted0));
    // the answers come in between, the scan codes complete and in order
    std::vector<uint8_t> codes;
    int fa = 0;
    for (uint8_t b : host.got) {
        if (b == 0xFA) fa++;
        else codes.push_back(b);
    }
    CHECK(fa == 2 && codes == Ps2KeyboardModel::text_codes(text), "typing around a command: %d FA, %zu codes", fa,
          codes.size());
    CHECK(decode_set2(codes) == text, "decoded '%s'", decode_set2(codes).c_str());
    CHECK(kb.leds == 1, "LEDs %u", kb.leds);
    CHECK(bus.rd(B + SURF_PS2_STATUS) == 0, "STATUS %08x", bus.rd(B + SURF_PS2_STATUS));
    std::printf("command in mid-frame: waited %llu us for the gap, no resend\n", (unsigned long long)(waited / US));
}

// TXDATA written at every clock cycle around the end of a keyboard frame: around the rising
// edge of the parity bit's clock (where an IBM keyboard commits the frame) and the falling
// edge of the stop bit's clock (where this model commits by default), and every 16 cycles in
// between, with the keyboard in both commit modes. The send must wait for the gap: the byte
// arrives exactly once, the keyboard never resends or cuts the frame short, the command gets
// through, no error.
static void test_txdata_race(Tb &tb, Bus &bus, Host &host) {
    Ps2KeyboardModel &kb = *tb.kb;
    const uint32_t gap0 = kb.gap_us;
    const int max_gap0 = bus.max_gap;
    bus.max_gap = 0;                       // the write lands exactly where the trial puts it
    kb.answer_commands = false;            // only the byte matters: shorter trials
    kb.gap_us = 100;
    bus.wr(B + SURF_PS2_STATUS, 0xFF);
    host.wait_kb_idle();
    // where the edges are, from the parity bit's falling edge
    auto find_parity_fall = [&]() -> uint64_t {
        bool prev = tb.clk_line;
        for (uint64_t n = 0; n < 1000000; n++) {
            bus.idle(1);
            if (kb.sending_bit() == 9 && prev && !tb.clk_line) return tb.cycle;
            prev = tb.clk_line;
        }
        return 0;
    };
    kb.queue_bytes({0x1C});
    const uint64_t f = find_parity_fall();
    uint64_t rise = 0, stop = 0;
    bool prev = tb.clk_line;
    while (!stop && tb.cycle < f + 10 * kb.ticks_per_bit()) {
        bus.idle(1);
        if (!rise && !prev && tb.clk_line) rise = tb.cycle;
        if (rise && prev && !tb.clk_line) stop = tb.cycle;
        prev = tb.clk_line;
    }
    CHECK(f && rise && stop, "could not find the parity and stop edges");
    host.wait_kb_idle();
    host.drain();
    const int64_t d_rise = static_cast<int64_t>(rise - f), d_stop = static_cast<int64_t>(stop - f);
    struct Win { int64_t base; int from, to, step; };
    const Win wins[] = {{d_rise, -64, 64, 1}, {d_rise, 80, static_cast<int>(d_stop - d_rise) - 80, 16},
                        {d_stop, -64, 160, 1}};
    const char *modes[] = {"stop-edge commit", "IBM parity-clock commit"};
    for (int mode = 0; mode < 2; mode++) {
        kb.commit = mode ? Ps2KeyboardModel::Commit::AFTER_PARITY_CLOCK : Ps2KeyboardModel::Commit::AFTER_STOP_EDGE;
        int trials = 0, bad = 0, lost = 0, dup = 0, resent = 0, cut = 0, cmd_lost = 0, errs = 0;
        uint64_t max_wait = 0;
        for (const Win &w : wins) {
            for (int off = w.from; off <= w.to; off += w.step) {
                const uint64_t aborted0 = kb.stats.frames_aborted, cut0 = kb.stats.frames_cut_short;
                const size_t hb0 = kb.host_bytes.size();
                kb.queue_bytes({0x1C});
                const uint64_t pf = find_parity_fall();
                REQUIRE(pf != 0, "the keyboard's frame never reached its parity bit");
                const int64_t at = w.base + off;      // cycles after the parity bit's falling edge
                while (static_cast<int64_t>(tb.cycle - pf) < at) bus.idle(1);
                uint64_t oe_at = 0, done_at = 0;
                const uint64_t t0 = tb.cycle;
                const bool done = host.send_traced(0xF4, oe_at, done_at);
                if (oe_at > t0 && oe_at - t0 > max_wait) max_wait = oe_at - t0;
                host.wait_kb_idle();
                host.got.clear();
                host.drain();
                const uint32_t st = bus.rd(B + SURF_PS2_STATUS);
                int copies = 0;
                for (uint8_t b : host.got) copies += b == 0x1C;
                const bool t_lost = copies == 0, t_dup = copies > 1 || host.got.size() != static_cast<size_t>(copies);
                const bool t_resent = kb.stats.frames_aborted != aborted0, t_cut = kb.stats.frames_cut_short != cut0;
                const bool t_cmd = !(done && kb.host_bytes.size() == hb0 + 1 && kb.host_bytes.back() == 0xF4);
                lost += t_lost;
                dup += t_dup;
                resent += t_resent;
                cut += t_cut;
                cmd_lost += t_cmd;
                errs += st != 0;
                const bool ok = !t_lost && !t_dup && !t_resent && !t_cut && !t_cmd && st == 0;
                if (!ok) {
                    if (bad < 4)
                        std::printf("  %s: TXDATA %+lld cycles from the %s edge: got %s, resent %llu, cut %llu, "
                                    "command %s, STATUS %08x\n", modes[mode], static_cast<long long>(off),
                                    w.base == d_stop ? "stop" : "parity", hexs(host.got).c_str(),
                                    (unsigned long long)(kb.stats.frames_aborted - aborted0),
                                    (unsigned long long)(kb.stats.frames_cut_short - cut0),
                                    kb.host_bytes.size() == hb0 + 1 ? "through" : "lost", st);
                    bad++;
                    bus.wr(B + SURF_PS2_STATUS, 0xFF);
                }
                trials++;
            }
        }
        CHECK(bad == 0, "%s: %d of %d TXDATA timings lost or disturbed the keyboard's byte", modes[mode], bad, trials);
        std::printf("txdata race, %s: %d timings around the parity and stop edges: %d bytes lost, %d duplicated, "
                    "%d resent, %d cut short, %d commands lost, %d with errors; longest wait for the gap %llu us\n",
                    modes[mode], trials, lost, dup, resent, cut, cmd_lost, errs, (unsigned long long)(max_wait / US));
    }
    kb.commit = Ps2KeyboardModel::Commit::AFTER_STOP_EDGE;
    kb.answer_commands = true;
    kb.gap_us = gap0;
    bus.max_gap = max_gap0;
}

static void test_saturation(Tb &tb, Bus &bus, Host &host) {
    Ps2KeyboardModel &kb = *tb.kb;
    for (int i = 0; i < 260; i++) kb.queue_frame(static_cast<uint8_t>(i), Ps2KeyboardModel::Fault::PARITY);
    host.wait_kb_idle();
    const uint32_t ec = bus.rd(B + SURF_PS2_ERRCOUNT);
    CHECK((ec & 0xFF) == 0xFF, "parity counter %u after 260+ errors, want 255", ec & 0xFF);
    CHECK(bus.rd(B + SURF_PS2_DATA) == SURF_PS2_DATA_EMPTY_MASK, "a parity-error byte reached the FIFO");
    bus.wr(B + SURF_PS2_STATUS, 0xFF);
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    unsigned seed = 1;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
    std::mt19937 rng(seed);
    auto top = std::make_unique<Vsurf_ps2>();
    const uint32_t rates[] = {12500, 10000, 16700};
    std::unique_ptr<Ps2KeyboardModel> kb(new Ps2KeyboardModel(SYS_HZ, rates[0]));
    kb->gap_us = 100 + rng() % 400;
    Tb tb(top.get(), kb.get());
    top->clk = 0;
    top->rst = 1;
    top->ps2_clk_i = 1;
    top->ps2_data_i = 1;
    top->s_req_valid = 0;
    top->eval();
    for (int i = 0; i < 5; i++) tb.posedge();
    top->rst = 0;
    tb.posedge();
    Bus bus(top.get(), [&] { tb.posedge(); }, seed);
    bus.max_gap = static_cast<int>(seed % 3);
    Host host(tb, bus);

    test_registers(tb, bus);
    test_typing(tb, bus, host, rng);
    test_keys(tb, bus, host);
    test_errors(tb, bus, host);
    test_overflow_and_irq(tb, bus, host);
    test_commands(tb, bus, host);
    test_command_while_typing(tb, bus, host);
    test_txdata_race(tb, bus, host);
    const uint64_t period_min = tb.period_min, period_max = tb.period_max;
    CHECK(period_min + 2 >= kb->ticks_per_bit() && period_max <= kb->ticks_per_bit() + 2,
          "keyboard bit period %llu..%llu ticks, want %u", (unsigned long long)period_min,
          (unsigned long long)period_max, kb->ticks_per_bit());
    // other keyboard clock rates: a fresh keyboard on the same port
    for (int r = 1; r < 3; r++) {
        Ps2KeyboardModel *old = kb.release();
        kb.reset(new Ps2KeyboardModel(SYS_HZ, rates[r]));
        kb->gap_us = 100 + rng() % 400;
        tb.kb = kb.get();
        CHECK(old->stats.violations == 0, "keyboard saw %llu host protocol violations: %s",
              (unsigned long long)old->stats.violations, old->log.empty() ? "" : old->log.back().c_str());
        delete old;
        bus.idle(100);
        test_typing(tb, bus, host, rng);
        if (r == 2) test_saturation(tb, bus, host);
    }
    CHECK(kb->stats.violations == 0, "keyboard saw %llu host protocol violations", (unsigned long long)kb->stats.violations);
    CHECK(tb.dat_oe_bad == 0, "data_oe changed %llu times while the clock line was high",
          (unsigned long long)tb.dat_oe_bad);
    CHECK(tb.clk_oe_min >= 125 * US && tb.clk_oe_max <= 131 * US, "clock pulled for %llu..%llu cycles",
          (unsigned long long)tb.clk_oe_min, (unsigned long long)tb.clk_oe_max);
    CHECK(bus.problems.empty(), "SurfBus protocol: %s", bus.problems.empty() ? "" : bus.problems[0].c_str());
    std::printf("pins: %llu clock pulls of %llu..%llu us, %llu data_oe changes all with the clock low; "
                "%llu keyboard falling edges\n",
                (unsigned long long)tb.clk_oe_pulses, (unsigned long long)(tb.clk_oe_min / US),
                (unsigned long long)(tb.clk_oe_max / US), (unsigned long long)tb.dat_oe_changes,
                (unsigned long long)tb.dev_falls);
    std::printf("bus: %llu requests, %llu error responses\n", (unsigned long long)bus.requests,
                (unsigned long long)bus.error_responses);
    top->final();
    if (failures) {
        std::printf("test_ps2: FAIL (%d of %d checks failed, seed %u, %llu cycles)\n", failures, checks, seed,
                    (unsigned long long)tb.cycle);
        return 1;
    }
    std::printf("test_ps2: PASS (%d checks, seed %u, %llu cycles = %.1f ms simulated)\n", checks, seed,
                (unsigned long long)tb.cycle, tb.cycle / 25000.0);
    return 0;
}
