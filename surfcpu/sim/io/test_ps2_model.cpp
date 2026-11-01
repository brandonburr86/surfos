/*
SurfCPU PS/2 keyboard model tests
--------------------
File: test_ps2_model.cpp Date: 10/15/26 (SurfCPU)
--------------------
Ps2KeyboardModel on its own, against a software host on the two open-drain lines: scan
code set 2 tables (checked against values written out here), all 256 byte values framed
correctly with the right timing (bit period, half-period clock phases, data changing only
while the clock is high and a quarter period before the falling edge), each injected
fault seen as such, a frame abandoned on a host inhibit and sent again, the two commit
points (an inhibit after the parity clock: resent, or with IBM's rule delivered as it is),
host commands clocked in and answered, a keyboard that stops clocking a host byte, and the
model's checks of the host (short inhibit, data changed while the clock is high, no
acknowledge, no keyboard).

    test_ps2_model [seed]
*/
#include "../models/ps2_keyboard_model.h"

#include <cstdio>
#include <cstdlib>
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

static const uint32_t SYS = 25000000;
static const uint64_t US = SYS / 1000000;

struct SoftHost {
    Ps2KeyboardModel &kb;
    bool clk_oe = false, data_oe = false, clk = true, dat = true, p_clk = true, p_dat = true;
    bool sending = false;
    uint64_t now = 0;
    // receiver
    int n = 0;
    uint16_t sh = 0;
    std::vector<uint8_t> got;
    int parity_errs = 0, start_errs = 0, stop_errs = 0;
    // timing of the keyboard's frames
    uint64_t last_fall = 0, last_rise = 0, last_dat_change = 0;
    uint64_t per_min = ~0ull, per_max = 0, low_min = ~0ull, low_max = 0, setup_min = ~0ull;
    int dat_change_clock_low = 0;
    int send_falls = 0;                                    // keyboard clock edges in the last send()

    explicit SoftHost(Ps2KeyboardModel &k) : kb(k) {}

    void tick() {
        kb.tick(clk_oe, data_oe, clk, dat);
        now++;
        if (!sending && !clk_oe) {
            if (dat != p_dat) {
                if (!clk) dat_change_clock_low++;
                last_dat_change = now;
            }
            if (p_clk && !clk) {                          // falling edge: sample
                const uint64_t per = now - last_fall;
                if (last_fall && per < 2 * kb.ticks_per_bit()) {
                    if (per < per_min) per_min = per;
                    if (per > per_max) per_max = per;
                }
                if (now - last_dat_change < setup_min) setup_min = now - last_dat_change;
                last_fall = now;
                sh = static_cast<uint16_t>(sh | (dat ? 1u : 0u) << n);
                if (++n == 11) frame_done();
            }
            if (!p_clk && clk) {
                const uint64_t low = now - last_fall;
                if (low < low_min) low_min = low;
                if (low > low_max) low_max = low;
                last_rise = now;
            }
        }
        p_clk = clk;
        p_dat = dat;
    }
    void frame_done() {
        const int start = sh & 1, stop = (sh >> 10) & 1;
        int ones = 0;
        for (int i = 1; i <= 9; i++) ones += (sh >> i) & 1;
        if (start) start_errs++;
        else if (!stop) stop_errs++;
        else if (!(ones & 1)) parity_errs++;
        else got.push_back(static_cast<uint8_t>(sh >> 1));
        n = 0;
        sh = 0;
    }
    void run(uint64_t ticks) {
        for (uint64_t i = 0; i < ticks; i++) tick();
    }
    void run_until_idle(uint64_t limit = 2000000000) {
        uint64_t t = 0;
        while (!kb.idle() && t++ < limit) tick();
        run(400 * US);
    }
    // host to device; returns whether the keyboard acknowledged
    bool send(uint8_t b, uint64_t inhibit_us = 125, bool wiggle = false) {
        int ones = 0;
        for (int i = 0; i < 8; i++) ones += (b >> i) & 1;
        const int bits[10] = {b & 1, b >> 1 & 1, b >> 2 & 1, b >> 3 & 1, b >> 4 & 1, b >> 5 & 1, b >> 6 & 1, b >> 7 & 1,
                              (ones & 1) ? 0 : 1, 1};
        sending = true;
        n = 0;
        sh = 0;
        clk_oe = true;
        run(inhibit_us * US);
        data_oe = true;                                   // start bit
        run(5 * US);
        clk_oe = false;
        int i = 0;
        bool acked = false;
        const uint64_t end = now + 20000 * US;
        while (now < end) {
            const bool was = clk;
            tick();
            if (was && !clk) {                            // a falling edge of the keyboard's clock
                if (i < 10) {
                    data_oe = !bits[i++];
                } else {
                    acked = !dat;                         // the 11th: the keyboard holds data low
                    break;
                }
            }
            if (wiggle && i == 4 && clk) data_oe = !data_oe, wiggle = false;   // a mistake on purpose
        }
        send_falls = i + (acked ? 1 : 0);
        data_oe = false;
        for (uint64_t t = 0; t < 1000 * US && !(clk && dat); t++) tick();
        sending = false;
        p_clk = clk;
        p_dat = dat;
        return acked;
    }
};

static void test_tables() {
    using M = Ps2KeyboardModel;
    CHECK(M::text_codes("aA1!\n") == (std::vector<uint8_t>{0x1C, 0xF0, 0x1C, 0x12, 0x1C, 0xF0, 0x1C, 0xF0, 0x12, 0x16, 0xF0,
                                                           0x16, 0x12, 0x16, 0xF0, 0x16, 0xF0, 0x12, 0x5A, 0xF0, 0x5A}),
          "text_codes(aA1!\\n)");
    // the letters and digits of scan code set 2, written out independently
    const char *letters = "abcdefghijklmnopqrstuvwxyz1234567890";
    const uint8_t codes[] = {0x1C, 0x32, 0x21, 0x23, 0x24, 0x2B, 0x34, 0x33, 0x43, 0x3B, 0x42, 0x4B,
                             0x3A, 0x31, 0x44, 0x4D, 0x15, 0x2D, 0x1B, 0x2C, 0x3C, 0x2A, 0x1D, 0x22,
                             0x35, 0x1A, 0x16, 0x1E, 0x26, 0x25, 0x2E, 0x36, 0x3D, 0x3E, 0x46, 0x45};
    int bad = 0;
    for (int i = 0; letters[i]; i++) {
        const std::vector<uint8_t> c = M::text_codes(std::string(1, letters[i]));
        if (c != std::vector<uint8_t>{codes[i], 0xF0, codes[i]}) bad++;
    }
    CHECK(bad == 0, "%d letter/digit codes wrong", bad);
    std::vector<uint8_t> v;
    CHECK(M::make_codes("RCTRL", v) && v == (std::vector<uint8_t>{0xE0, 0x14}), "RCTRL make");
    v.clear();
    CHECK(M::break_codes("UP", v) && v == (std::vector<uint8_t>{0xE0, 0xF0, 0x75}), "UP break");
    v.clear();
    CHECK(M::make_codes("F7", v) && v == std::vector<uint8_t>{0x83}, "F7 make");
    v.clear();
    CHECK(M::break_codes("PAUSE", v) && v.empty(), "Pause has no break");
    v.clear();
    CHECK(!M::make_codes("NOSUCHKEY", v), "unknown key accepted");
}

static void test_frames(unsigned seed) {
    const uint32_t rates[] = {10000, 12500, 16700};
    for (uint32_t hz : rates) {
        Ps2KeyboardModel kb(SYS, hz);
        kb.gap_us = 100 + seed % 300;
        SoftHost h(kb);
        std::vector<uint8_t> all;
        for (int b = 0; b < 256; b++) all.push_back(static_cast<uint8_t>(b));
        kb.queue_bytes(all);
        h.run_until_idle();
        const uint32_t tpb = kb.ticks_per_bit();
        CHECK(h.got == all && h.parity_errs + h.start_errs + h.stop_errs == 0, "%u Hz: %zu bytes, errors %d/%d/%d", hz,
              h.got.size(), h.parity_errs, h.start_errs, h.stop_errs);
        CHECK(h.per_min + 2 >= tpb && h.per_max <= tpb + 2, "%u Hz: period %llu..%llu, want %u", hz,
              (unsigned long long)h.per_min, (unsigned long long)h.per_max, tpb);
        CHECK(h.low_min + 2 >= tpb / 2 && h.low_max <= tpb / 2 + 2, "%u Hz: clock low %llu..%llu", hz,
              (unsigned long long)h.low_min, (unsigned long long)h.low_max);
        CHECK(h.dat_change_clock_low == 0 && h.setup_min + 2 >= tpb / 4 && h.setup_min >= 5 * US,
              "%u Hz: data changed with the clock low %d times, setup %llu ticks", hz, h.dat_change_clock_low,
              (unsigned long long)h.setup_min);
        CHECK(kb.stats.frames_sent == 256, "frames_sent %llu", (unsigned long long)kb.stats.frames_sent);
    }
}

static void test_faults() {
    Ps2KeyboardModel kb(SYS, 12500);
    SoftHost h(kb);
    kb.queue_frame(0x11, Ps2KeyboardModel::Fault::PARITY);
    kb.queue_frame(0x22, Ps2KeyboardModel::Fault::STOP);
    kb.queue_frame(0x33, Ps2KeyboardModel::Fault::START);
    kb.queue_frame(0x44);
    h.run_until_idle();
    CHECK(h.parity_errs == 1 && h.stop_errs == 1 && h.start_errs == 1 && h.got == std::vector<uint8_t>{0x44},
          "faults seen: parity %d stop %d start %d, %zu good", h.parity_errs, h.stop_errs, h.start_errs, h.got.size());
    // a glitch: one extra low pulse on the clock, far shorter than a bit (this host has no filter)
    Ps2KeyboardModel kbg(SYS, 12500);
    SoftHost hg(kbg);
    kbg.queue_frame(0x44, Ps2KeyboardModel::Fault::GLITCH);
    uint64_t gfalls = 0, gfall_at = 0, shortest = ~0ull;
    bool gpc = true;
    while (!kbg.idle()) {
        hg.tick();
        if (gpc && !hg.clk) { gfalls++; gfall_at = hg.now; }
        if (!gpc && hg.clk && hg.now - gfall_at < shortest) shortest = hg.now - gfall_at;
        gpc = hg.clk;
    }
    CHECK(gfalls == 12 && shortest < 1 * US, "glitch frame: %llu falling edges, shortest low %llu ticks",
          (unsigned long long)gfalls, (unsigned long long)shortest);
    // a partial frame: 5 bits, then a pause longer than 2 ms before the next frame
    Ps2KeyboardModel kb2(SYS, 12500);
    SoftHost h2(kb2);
    kb2.queue_frame(0x55, Ps2KeyboardModel::Fault::PARTIAL, 5);
    kb2.queue_frame(0x66);
    uint64_t falls = 0, last = 0, gap = 0;
    bool pc = true;
    for (uint64_t t = 0; t < 10000 * US; t++) {
        h2.tick();
        if (pc && !h2.clk) {
            if (falls == 5) gap = h2.now - last;
            last = h2.now;
            falls++;
        }
        pc = h2.clk;
    }
    CHECK(falls == 16 && gap > 2000 * US, "partial frame: %llu falling edges, pause %llu us", (unsigned long long)falls,
          (unsigned long long)(gap / US));
}

static void test_inhibit_and_commands() {
    Ps2KeyboardModel kb(SYS, 12500);
    SoftHost h(kb);
    kb.queue_bytes({0xA5, 0x5A});
    while (kb.sending_bit() != 6) h.tick();
    h.clk_oe = true;                                      // inhibit in the middle of the frame
    h.run(200 * US);
    h.clk_oe = false;
    h.n = 0;
    h.sh = 0;
    h.run_until_idle();
    CHECK(kb.stats.frames_aborted == 1 && h.got == (std::vector<uint8_t>{0xA5, 0x5A}), "abandoned frame: %llu aborted, got %zu",
          (unsigned long long)kb.stats.frames_aborted, h.got.size());
    h.got.clear();
    CHECK(h.send(0xF2), "F2 not acknowledged");
    h.run_until_idle();
    CHECK(kb.host_bytes == std::vector<uint8_t>{0xF2} && h.got == (std::vector<uint8_t>{0xFA, 0xAB, 0x83}),
          "identify: keyboard got %zu bytes, answered %zu", kb.host_bytes.size(), h.got.size());
    h.got.clear();
    CHECK(h.send(0xED) && h.send(0x05), "ED 05 not acknowledged");
    h.run_until_idle();
    CHECK(kb.leds == 5 && h.got == (std::vector<uint8_t>{0xFA, 0xFA}), "LEDs %u", kb.leds);
    CHECK(kb.stats.violations == 0, "violations %llu: %s", (unsigned long long)kb.stats.violations,
          kb.log.empty() ? "" : kb.log.back().c_str());
    // the model's checks of the host
    h.got.clear();
    h.send(0xF4, 50);                                     // inhibit of 50 us: too short
    h.run_until_idle();
    CHECK(kb.stats.violations == 1, "short inhibit not reported (%llu)", (unsigned long long)kb.stats.violations);
    h.send(0xF4, 125, true);                              // data changed while the clock was high
    h.run_until_idle();
    CHECK(kb.stats.violations >= 2, "data change with the clock high not reported (%llu)",
          (unsigned long long)kb.stats.violations);
    kb.no_ack = 1;
    CHECK(!h.send(0xF4), "acknowledged although no_ack was set");
    h.run_until_idle();
    kb.present = false;
    CHECK(!h.send(0xF4), "a missing keyboard acknowledged");
    CHECK(kb.stats.host_clock_pulls >= 6 && kb.stats.host_clock_min <= 56 * US, "clock pulls %llu, shortest %llu",
          (unsigned long long)kb.stats.host_clock_pulls, (unsigned long long)kb.stats.host_clock_min);
}

// The commit point: an inhibit after the parity bit's clock rose makes the default keyboard
// abandon the frame and send it again, and an IBM-rule keyboard count it delivered.
static void test_commit_points() {
    for (int mode = 0; mode < 2; mode++) {
        Ps2KeyboardModel kb(SYS, 12500);
        kb.commit = mode ? Ps2KeyboardModel::Commit::AFTER_PARITY_CLOCK : Ps2KeyboardModel::Commit::AFTER_STOP_EDGE;
        SoftHost h(kb);
        kb.queue_bytes({0x3C});
        bool pc = h.clk;
        uint64_t t = 0;
        while (t++ < 100000 && !(kb.sending_bit() == 9 && !pc && h.clk)) {   // the parity clock rises
            pc = h.clk;
            h.tick();
        }
        h.run(100);
        h.clk_oe = true;
        h.run(200 * US);
        h.clk_oe = false;
        h.n = 0;
        h.sh = 0;
        h.run_until_idle();
        if (mode == 0)
            CHECK(kb.stats.frames_aborted == 1 && kb.stats.frames_cut_short == 0 && h.got == std::vector<uint8_t>{0x3C},
                  "stop-edge commit: aborted %llu, cut %llu, got %zu bytes", (unsigned long long)kb.stats.frames_aborted,
                  (unsigned long long)kb.stats.frames_cut_short, h.got.size());
        else
            CHECK(kb.stats.frames_aborted == 0 && kb.stats.frames_cut_short == 1 && kb.stats.frames_sent == 1 &&
                      h.got.empty(),
                  "IBM commit: aborted %llu, cut %llu, sent %llu, got %zu bytes", (unsigned long long)kb.stats.frames_aborted,
                  (unsigned long long)kb.stats.frames_cut_short, (unsigned long long)kb.stats.frames_sent, h.got.size());
    }
    // a keyboard that stops clocking a host byte after 3 bits, then recovers
    Ps2KeyboardModel kb(SYS, 12500);
    SoftHost h(kb);
    kb.host_stall_bits = 3;
    CHECK(!h.send(0xF4) && h.send_falls == 3 && kb.host_bytes.empty(), "stalled host byte: %d clock edges, %zu bytes",
          h.send_falls, kb.host_bytes.size());
    h.run_until_idle();
    CHECK(h.send(0xF4) && kb.host_bytes == std::vector<uint8_t>{0xF4}, "send after the stall");
    CHECK(kb.stats.violations == 0, "violations %llu", (unsigned long long)kb.stats.violations);
}

int main(int argc, char **argv) {
    const unsigned seed = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 0)) : 1;
    test_tables();
    test_frames(seed);
    test_faults();
    test_inhibit_and_commands();
    test_commit_points();
    if (failures) {
        std::printf("test_ps2_model: FAIL (%d of %d checks failed, seed %u)\n", failures, checks, seed);
        return 1;
    }
    std::printf("test_ps2_model: PASS (%d checks, seed %u)\n", checks, seed);
    return 0;
}
