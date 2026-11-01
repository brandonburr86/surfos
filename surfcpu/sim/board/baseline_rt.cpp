/*
SurfCPU Arty A7 baseline: real-time test of the milestone 1 logic
--------------------
File: baseline_rt.cpp Date: 10/15/26 (SurfCPU)
--------------------
Verilator harness for arty_a7_baseline_core with its real parameters (a second is
clocks.sys_hz cycles): sys_clk at SURF_SYS_HZ and pix_clk at SURF_PIX_HZ, with edges at exact
femtosecond times, for a little over two seconds. It decodes the UART output at 115200 8N1
(round(sys_hz/115200) cycles a bit) and checks:
  * the sys heartbeat (LD0 green) rises exactly every SURF_SYS_HZ cycles, high for half;
  * the pix heartbeat (LD1 green) rises every second within 3 sys cycles;
  * one line a second, exactly SURF_SYS_HZ cycles apart, each exactly the expected text
    (board, BOARD_ID, build identity), with the uptime counting 1, 2 and the measured pixel
    clock within 2 of SURF_PIX_HZ on the second line;
  * switches, buttons and a byte sent to the RX pin appear in the line and on the LEDs.
The model is built with -GBOARD="arty_a7_100t" -GBUILD_ID=32'h1a2b3c4d -GBUILD_FLAGS=3
-GBUILD_TIME=1760560000 -GRGB_DIM_SHIFT=0 (sim/board/board.mk). Exit 0 = pass.
*/
#include "Varty_a7_baseline_core.h"
#include "verilated.h"
#include "surfcpu/soc.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace {

const int kDiv = (SURF_SYS_HZ + 57600) / 115200;   // cycles per bit, as the RTL
int failures = 0;

void fail(const char *what) {
    std::printf("baseline_rt: FAIL: %s\n", what);
    failures++;
}

struct Line {
    uint64_t start;   // sys cycle of the first start bit
    std::string text;
};

// 8N1 receiver sampling the TX pin once per sys cycle.
struct UartDecoder {
    enum { IDLE, BITS } state = IDLE;
    int count = 0, bit = 0;
    unsigned shift = 0;
    bool last = true;
    uint64_t byte_start = 0, line_start = 0;
    std::string cur;
    std::vector<Line> lines;

    void sample(bool tx, uint64_t cycle) {
        if (state == IDLE) {
            if (last && !tx) {                       // start bit edge
                state = BITS;
                count = kDiv / 2;                    // to the middle of the start bit
                bit = -1;
                shift = 0;
                byte_start = cycle;
            }
        } else if (--count == 0) {
            count = kDiv;
            if (bit < 0) {
                if (tx) state = IDLE;                // glitch, not a start bit
            } else if (bit < 8) {
                shift |= (tx ? 1u : 0u) << bit;
            } else {
                state = IDLE;
                if (!tx) fail("UART stop bit is 0");
                if (cur.empty()) line_start = byte_start;
                cur.push_back(static_cast<char>(shift));
                if (shift == '\n') {
                    lines.push_back({line_start, cur});
                    cur.clear();
                }
            }
            bit++;
        }
        last = tx;
    }
};

// Sends bytes on the RX pin, 8N1, kDiv cycles a bit.
struct UartSender {
    std::vector<int> bits;   // pending pin levels, one per bit time
    int count = 0;
    bool level = true;
    void send(uint8_t b) {
        bits.push_back(0);
        for (int i = 0; i < 8; i++) bits.push_back((b >> i) & 1);
        bits.push_back(1);
    }
    bool tick() {            // once per sys cycle
        if (count == 0 && !bits.empty()) {
            level = bits.front();
            bits.erase(bits.begin());
            count = kDiv;
        }
        if (count > 0) count--;
        return level;
    }
};

}  // namespace

int main(int argc, char **argv) {
    double seconds = 2.05;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);

    auto ctx = std::make_unique<VerilatedContext>();
    ctx->commandArgs(argc, argv);
    auto top = std::make_unique<Varty_a7_baseline_core>(ctx.get());

    // Edge times in femtoseconds: sys half period exact, pix edge n at n * 1e15 / (2 * pix_hz).
    const __int128 kFs = 1000000000000000LL;
    const uint64_t sys_half = static_cast<uint64_t>(kFs / (2 * SURF_SYS_HZ));
    uint64_t sys_n = 1, pix_n = 1;
    const uint64_t end_fs = static_cast<uint64_t>(seconds * 1e15);

    top->clk = 0;
    top->pix_clk = 0;
    top->rst = 1;
    top->pix_rst = 1;
    top->sw = 0;
    top->btn = 0;
    top->uart_rx = 1;
    top->eval();

    UartDecoder rx;
    UartSender tx;
    uint64_t sys_cycle = 0;
    bool hb_last = false, hbp_last = false;
    uint64_t hb_rise_at = 0, hb_fall_at = 0;
    std::vector<uint64_t> hb_rises, hb_highs, hbp_rises;
    const uint64_t reset_cycles = 16;
    bool injected = false;

    for (;;) {
        uint64_t t_sys = sys_n * sys_half;
        uint64_t t_pix = static_cast<uint64_t>(pix_n * kFs / (2 * SURF_PIX_HZ));
        uint64_t t = t_sys < t_pix ? t_sys : t_pix;
        if (t > end_fs) break;
        if (t_pix == t) {
            top->pix_clk = !top->pix_clk;
            pix_n++;
        }
        bool sys_rise = false;
        if (t_sys == t) {
            top->clk = !top->clk;
            sys_rise = top->clk;
            sys_n++;
            if (sys_rise) {                          // inputs change just after the edge
                sys_cycle++;
                if (sys_cycle == reset_cycles) {
                    top->rst = 0;
                    top->pix_rst = 0;
                }
            }
        }
        top->eval();
        if (!sys_rise) continue;

        // stimulus: switches before the first line; after it, a button and a byte on RX
        if (sys_cycle == SURF_SYS_HZ / 2) top->sw = 0x5;
        if (!injected && rx.lines.size() == 1) {
            top->btn = 0x4;
            tx.send('A');
            injected = true;
        }
        top->uart_rx = tx.tick();

        rx.sample(top->uart_tx, sys_cycle);

        bool hb = (top->rgb >> 1) & 1, hbp = (top->rgb >> 4) & 1;
        if (hb && !hb_last) {
            hb_rises.push_back(sys_cycle);
            hb_rise_at = sys_cycle;
        }
        if (!hb && hb_last) {
            hb_fall_at = sys_cycle;
            hb_highs.push_back(hb_fall_at - hb_rise_at);
        }
        if (hbp && !hbp_last) hbp_rises.push_back(sys_cycle);
        hb_last = hb;
        hbp_last = hbp;

        if (sys_cycle == SURF_SYS_HZ + SURF_SYS_HZ / 2) {   // halfway through second 2
            if (top->led != 0x5) fail("LD4..LD7 do not show the switches 0101");
            if (((top->rgb >> 8) & 1) != 1 || ((top->rgb >> 2) & 1) != 0)
                fail("the blue LEDs do not show BTN2 alone");
        }
    }

    // ------------------------------------------------------------ results
    std::printf("baseline_rt: %.3f s simulated, %llu sys cycles, UART %d cycles a bit\n", seconds,
                static_cast<unsigned long long>(sys_cycle), kDiv);
    if (hb_rises.size() < 3) fail("fewer than 3 sys heartbeat rises");
    for (size_t i = 1; i < hb_rises.size(); i++) {
        uint64_t d = hb_rises[i] - hb_rises[i - 1];
        std::printf("baseline_rt: sys heartbeat period %llu cycles\n", static_cast<unsigned long long>(d));
        if (d != SURF_SYS_HZ) fail("sys heartbeat period is not SURF_SYS_HZ cycles");
    }
    for (uint64_t h : hb_highs)
        if (h != SURF_SYS_HZ / 2) fail("sys heartbeat not high for exactly half a second");
    if (hbp_rises.size() < 3) fail("fewer than 3 pix heartbeat rises");
    for (size_t i = 1; i < hbp_rises.size(); i++) {
        int64_t d = static_cast<int64_t>(hbp_rises[i] - hbp_rises[i - 1]);
        std::printf("baseline_rt: pix heartbeat period %lld sys cycles\n", static_cast<long long>(d));
        if (d < SURF_SYS_HZ - 3 || d > SURF_SYS_HZ + 3) fail("pix heartbeat period is not one second");
    }

    std::regex re("^SurfCPU baseline arty_a7_100t 0x00000100 build 1a2b3c4d dirty sim 1760560000 "
                  "\\| up (\\d{6}) s \\| pix (\\d{8}) Hz \\| sw ([01]{4}) btn ([01]{4}) rx (..)\r\n$");
    if (rx.lines.size() != 2) fail("expected exactly two lines in two seconds");
    for (size_t i = 0; i < rx.lines.size(); i++) {
        const Line &l = rx.lines[i];
        std::string shown = l.text.substr(0, l.text.size() - 2);
        std::printf("baseline_rt: line %zu at cycle %llu: %s\n", i + 1,
                    static_cast<unsigned long long>(l.start), shown.c_str());
        std::smatch m;
        if (!std::regex_match(l.text, m, re)) {
            fail("line does not match the expected text");
            continue;
        }
        if (std::stoi(m[1]) != static_cast<int>(i + 1)) fail("uptime does not count 1, 2");
        long pix = std::stol(m[2]);
        if (i == 1 && (pix < SURF_PIX_HZ - 2 || pix > SURF_PIX_HZ + 2)) fail("pix figure is not SURF_PIX_HZ +- 2");
        if (i == 0 && pix > SURF_PIX_HZ + 2) fail("first pix figure is too high");
        if (m[3] != "0101") fail("line does not show the switches 0101");
        if (i == 0 && (m[4] != "0000" || m[5] != "--")) fail("first line: buttons or rx wrong");
        if (i == 1 && (m[4] != "0100" || m[5] != "41")) fail("second line: BTN2 or rx 41 missing");
        if (i > 0 && l.start - rx.lines[i - 1].start != SURF_SYS_HZ) fail("lines are not one second apart");
    }
    if (!rx.lines.empty()) {
        long first = static_cast<long>(rx.lines[0].start) - static_cast<long>(reset_cycles);
        std::printf("baseline_rt: first line starts %ld cycles after reset\n", first);
        if (first < SURF_SYS_HZ || first > SURF_SYS_HZ + 300) fail("first line not one second after reset");
    }
    top->final();
    std::printf("baseline_rt: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
