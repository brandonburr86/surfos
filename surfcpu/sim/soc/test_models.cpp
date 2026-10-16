/*
SurfCPU model tests
--------------------
File: test_models.cpp Date: 10/15/26 (SurfCPU)
--------------------
Host tests of the soc track's C++ models (make test-soc-models). UartModel: two models wired
back to back exchange random bytes both ways; the line is checked bit by bit against the
expected waveform (every bit exactly clks_per_bit cycles); framing errors, breaks and
glitches are detected and recovered from; +-3 % baud is tolerated. DdrModel: calibration
time and failure; a SurfBus master in C++ (one request at a time, held until accepted)
drives random byte/halfword/word traffic across the whole 256 MiB window against a reference
map, with latency bounds, the window's first and last words, accesses outside it, stalls
answered by the watchdog with a timeout pulse, injected errors, and the model's own checks
for early and withdrawn requests. Usage: test_models [seed]. Prints PASS or the failures.
*/
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "ddr_model.h"
#include "uart_model.h"

static int failures = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);     \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
            failures++;                                                     \
        }                                                                   \
    } while (0)

// ------------------------------------------------------------------------------------ UART
static void uart_tests(std::mt19937_64 &rng) {
    const unsigned cpb = (SURF_SYS_HZ + 115200 / 2) / 115200;     // 217

    // the waveform of one frame, cycle by cycle
    {
        UartModel a(cpb);
        a.send(std::string(1, char(0x35)));                       // 0b00110101
        std::vector<bool> line;
        while (a.sending()) {
            line.push_back(a.rx_pin());
            a.tick(true);
        }
        CHECK(line.size() == 10 * cpb, "frame of %zu cycles", line.size());
        const int bits[10] = {0, 1, 0, 1, 0, 1, 1, 0, 0, 1};      // start, LSB first, stop
        int bad = 0;
        for (size_t i = 0; i < line.size(); i++)
            if (line[i] != bool(bits[i / cpb])) bad++;
        CHECK(bad == 0, "%d cycles off the bit grid", bad);
        CHECK(a.rx_pin() == true, "line not idle after the frame");
    }

    // two models back to back, random bytes both ways
    {
        UartModel a(cpb), b(cpb);
        std::string ab, ba;
        for (int i = 0; i < 400; i++) ab.push_back(char(rng()));
        for (int i = 0; i < 300; i++) ba.push_back(char(rng()));
        a.send(ab);
        b.send(ba);
        std::string got_b, got_a;
        for (uint64_t c = 0; c < uint64_t(cpb) * 10 * 410; c++) {
            bool la = a.rx_pin(), lb = b.rx_pin();
            a.tick(lb);
            b.tick(la);
        }
        got_b = b.take_output();
        got_a = a.take_output();
        CHECK(got_b == ab, "b received %zu of %zu bytes correctly", got_b.size(), ab.size());
        CHECK(got_a == ba, "a received %zu of %zu bytes", got_a.size(), ba.size());
        CHECK(a.framing_errors() == 0 && b.framing_errors() == 0, "framing errors");
        CHECK(b.take_output().empty(), "take_output did not clear");
    }

    // framing error, break, glitch, recovery
    {
        UartModel a(cpb), b(cpb);
        a.send("A");
        a.send_frame(0x5A, false);                                // bad stop bit
        a.send_idle(2);
        a.send("B");
        a.send_break(25);
        a.send_idle(1);
        a.send("C");
        while (a.sending()) {
            bool l = a.rx_pin();
            a.tick(true);
            b.tick(l);
        }
        for (unsigned i = 0; i < 2 * cpb; i++) b.tick(true);
        // a glitch shorter than half a bit
        for (unsigned i = 0; i < cpb / 4; i++) b.tick(false);
        for (unsigned i = 0; i < 12 * cpb; i++) b.tick(true);
        CHECK(b.take_output() == "ABC", "the good bytes around the errors");
        CHECK(b.framing_errors() == 2, "%llu framing errors, expected 2",
              (unsigned long long)b.framing_errors());
        CHECK(b.breaks() == 1, "%llu breaks", (unsigned long long)b.breaks());
    }

    // +-3 % baud
    for (unsigned other : {cpb * 97 / 100, cpb * 103 / 100}) {
        UartModel a(other), b(cpb);
        std::string s;
        for (int i = 0; i < 100; i++) s.push_back(char(rng()));
        a.send(s);
        while (a.sending()) {
            bool l = a.rx_pin();
            a.tick(true);
            b.tick(l);
        }
        for (unsigned i = 0; i < 2 * cpb; i++) b.tick(true);
        CHECK(b.take_output() == s, "bytes at %u cycles per bit", other);
    }
}

// ------------------------------------------------------------------------------------ DDR
struct Req {
    uint32_t addr = 0, wdata = 0;
    bool write = false, fetch = false;
    uint8_t be = 0xF;
};

// A SurfBus master for the model: offers one request, holds it until accepted, waits for
// the response. Returns the response; counts the cycles from acceptance to response.
struct Master {
    DdrModel &m;
    unsigned lat = 0;
    bool err = false;
    uint32_t rdata = 0;
    bool saw_timeout = false;
    explicit Master(DdrModel &model) : m(model) {}
    void idle(unsigned n) {
        for (unsigned i = 0; i < n; i++) m.tick(false, 0, false, 0, 0, false);
    }
    void xfer(const Req &r) {
        // offer until the edge where req_ready is 1
        for (;;) {
            bool ready = m.req_ready();
            m.tick(true, r.addr, r.write, r.be, r.wdata, r.fetch);
            if (ready) break;
        }
        lat = 1;
        while (!m.rsp_valid()) {
            m.tick(false, 0, false, 0, 0, false);
            lat++;
        }
        err = m.rsp_err();
        rdata = m.rsp_rdata();
        saw_timeout = m.timeout();
    }
};

static uint32_t merge(uint32_t old, uint32_t d, uint8_t be) {
    for (int k = 0; k < 4; k++)
        if (be >> k & 1) old = (old & ~(0xFFu << (8 * k))) | (d & (0xFFu << (8 * k)));
    return old;
}

static void ddr_tests(std::mt19937_64 &rng, uint64_t seed) {
    // calibration time and failure
    {
        DdrModelOptions o;
        o.calib_cycles = 500;
        DdrModel m(o);
        Master ms(m);
        ms.idle(499);
        CHECK(!m.calib_done(), "calibrated too early");
        ms.idle(1);
        CHECK(m.calib_done() && !m.calib_fail(), "not calibrated after 500 cycles");
        o.calib_fails = true;
        DdrModel f(o);
        Master mf(f);
        mf.idle(600);
        CHECK(!f.calib_done() && f.calib_fail(), "calib_fail");
        o.calib_fails = false;
        o.calib_cycles = -1;
        DdrModel n(o);
        Master mn(n);
        mn.idle(5000);
        CHECK(!n.calib_done() && !n.calib_fail(), "calibration with calib_cycles < 0");
    }

    // random traffic over the whole window against a reference
    {
        DdrModelOptions o;
        o.seed = seed;
        o.calib_cycles = 10;
        o.latency_min = 1;
        o.latency_max = 9;
        o.ready_percent = 60;
        DdrModel m(o);
        Master ms(m);
        ms.idle(12);
        std::map<uint32_t, uint32_t> ref;
        std::vector<uint32_t> used;
        unsigned lat_min = ~0u, lat_max = 0;
        int bad = 0;
        for (int i = 0; i < 20000; i++) {
            Req r;
            if (!used.empty() && rng() % 2) r.addr = used[rng() % used.size()];
            else r.addr = SURF_DDR_BASE + uint32_t(rng() % SURF_DDR_SIZE);
            switch (rng() % 3) {
            case 0: r.be = uint8_t(1u << (r.addr & 3)); break;
            case 1: r.addr &= ~1u; r.be = (r.addr & 2) ? 0xC : 0x3; break;
            default: r.addr &= ~3u; r.be = 0xF; break;
            }
            uint32_t w = r.addr & ~3u;
            r.write = rng() % 2;
            r.fetch = !r.write && rng() % 4 == 0;
            r.wdata = uint32_t(rng());
            ms.xfer(r);
            lat_min = std::min(lat_min, ms.lat);
            lat_max = std::max(lat_max, ms.lat);
            if (ms.err) bad++;
            if (r.write) {
                ref[w] = merge(ref.count(w) ? ref[w] : 0, r.wdata, r.be);
                used.push_back(r.addr);
            } else if (ms.rdata != (ref.count(w) ? ref[w] : 0)) {
                bad++;
            }
            if (rng() % 8 == 0) ms.idle(rng() % 4);
        }
        CHECK(bad == 0, "%d wrong or failed accesses", bad);
        CHECK(lat_min == 1 && lat_max == 9, "latencies %u..%u, expected 1..9", lat_min, lat_max);
        CHECK(m.early_requests == 0 && m.protocol_errors == 0 && m.outside == 0, "model checks");
        CHECK(m.pages() < 12000, "%zu pages for a sparse memory", m.pages());
        for (auto &kv : ref)
            if (m.peek(kv.first) != kv.second) { bad++; break; }
        CHECK(bad == 0, "the back door disagrees");
    }

    // the window's edges, outside, stalls, injected errors, the model's own checks
    {
        DdrModelOptions o;
        o.calib_cycles = 50;
        o.watchdog_cycles = 250;
        DdrModel m(o);
        Master ms(m);
        // a request before calibration is counted (the SoC's gate must prevent it)
        ms.xfer(Req{SURF_DDR_BASE, 0, false, false, 0xF});
        CHECK(m.early_requests == 1, "early request not seen");
        ms.idle(60);
        Req first{SURF_DDR_BASE, 0x11223344, true, false, 0xF};
        Req last{SURF_DDR_BASE + SURF_DDR_SIZE - 4, 0x55667788, true, false, 0xF};
        ms.xfer(first);
        ms.xfer(last);
        first.write = last.write = false;
        ms.xfer(first);
        CHECK(!ms.err && ms.rdata == 0x11223344, "first word %08x", ms.rdata);
        ms.xfer(last);
        CHECK(!ms.err && ms.rdata == 0x55667788, "last word %08x", ms.rdata);
        ms.xfer(Req{SURF_DDR_BASE + SURF_DDR_SIZE, 0, false, false, 0xF});
        CHECK(ms.err && m.outside == 1, "outside the window");
        ms.xfer(Req{SURF_DDR_BASE - 4, 0, false, false, 0xF});
        CHECK(ms.err && m.outside == 2, "below the window");
        // a stall: the watchdog answers with an error and a timeout pulse in that cycle
        m.stall_at(SURF_DDR_BASE + 0x80);
        ms.xfer(Req{SURF_DDR_BASE + 0x80, 0xFFFFFFFF, true, false, 0xF});
        CHECK(ms.err && ms.saw_timeout && ms.lat == 250, "stall: err %d timeout %d after %u",
              ms.err, ms.saw_timeout, ms.lat);
        ms.idle(1);
        CHECK(!m.timeout(), "timeout is a pulse");
        CHECK(m.peek(SURF_DDR_BASE + 0x80) == 0, "a stalled write must not land");
        // an injected error, answered at once
        m.error_at(SURF_DDR_BASE + 0x100);
        ms.xfer(Req{SURF_DDR_BASE + 0x100, 0, false, false, 0xF});
        CHECK(ms.err && !ms.saw_timeout && ms.lat <= o.latency_max, "injected error");
        // a withdrawn request is a protocol error
        uint64_t pe = m.protocol_errors;
        for (int i = 0; i < 1000 && m.req_ready(); i++) ms.idle(1);   // find a not-ready cycle
        if (!m.req_ready()) {
            m.tick(true, SURF_DDR_BASE + 0x200, false, 0xF, 0, false);   // offered, not taken
            m.tick(false, 0, false, 0, 0, false);                        // and withdrawn
            CHECK(m.protocol_errors == pe + 1, "withdrawal not seen");
        }
    }
}

int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 1;
    std::mt19937_64 rng(seed);
    uart_tests(rng);
    ddr_tests(rng, seed);
    if (failures) {
        std::printf("test_models: %d failures (seed %llu)\n", failures, (unsigned long long)seed);
        return 1;
    }
    std::printf("test_models: PASS (seed %llu: UART waveform, 700 bytes back to back, errors, "
                "+-3%% baud; DDR calibration, 20000 random accesses, edges, stalls, errors)\n",
                (unsigned long long)seed);
    return 0;
}
