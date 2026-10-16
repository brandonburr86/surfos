/*
SurfCPU DDR testbench core
--------------------
File: surfbus_bench.h Date: 10/15/26 (SurfCPU)
--------------------
What the DDR testbenches share (tb_ddr_bridge.cpp against the MIG model, tb_ddr_litedram.cpp
against LiteDRAM's controller): a scheduler for two unrelated clocks (sys_clk and ui_clk, any
periods, optional jitter, the ui clock can stop), a SurfBus master that follows SURFBUS.md
(one outstanding request, no withdrawal, the next request offered as early as the cycle of
the previous response), a reference memory that also knows which writes may or may not have
happened (an access cut short by a reset or the watchdog), and the checks made in every test:
a response only for an accepted request, never during reset, never later than the watchdog
allows; calib_done and calib_fail never both 1; timeout sticky until rst.

Bench<Top, Side> works on any Verilated top with the bridge's sys-side ports (clk, rst, s_req_*,
s_rsp_*, calib_done, calib_fail, timeout) and a ui_clk input. Side looks after whatever sits on
the controller side: the C++ MIG model, or nothing when the controller is inside the top.
It provides: init(top), pre_ui_rise(top), post_ui_rise(top), done() (controller commands
finished so far), idle(), observable() (whether done()/idle() mean anything), ui_reset(len),
finish(bench) (end-of-test checks), reads(), writes(), tag().

The including file defines TB_TIMEOUT_CYCLES and TB_CALIB_TIMEOUT_CYCLES (the bridge's
parameters as built).
*/
#pragma once
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "verilated.h"

#include "ddr_rng.h"

#ifndef TB_TIMEOUT_CYCLES
#error "build with -DTB_TIMEOUT_CYCLES=<the bridge's TIMEOUT_CYCLES>"
#endif
#ifndef TB_CALIB_TIMEOUT_CYCLES
#error "build with -DTB_CALIB_TIMEOUT_CYCLES=<the bridge's CALIB_TIMEOUT_CYCLES>"
#endif

namespace surfddr {

constexpr uint64_t kTimeout = TB_TIMEOUT_CYCLES;
constexpr uint64_t kCalibTimeout = TB_CALIB_TIMEOUT_CYCLES;
constexpr uint32_t kDdrBase = 0x80000000u;
constexpr uint32_t kDdrMask = 0x0FFFFFFFu;
const uint8_t kBe[7] = {0x1, 0x2, 0x4, 0x8, 0x3, 0xC, 0xF};

inline uint32_t be_offset(uint8_t be) {         // SURFBUS.md section 4: addr[1:0] that goes with be
    switch (be) {
        case 0x2: return 1;
        case 0x4: case 0xC: return 2;
        case 0x8: return 3;
        default: return 0;
    }
}
inline uint32_t be_mask(uint8_t be) {
    uint32_t m = 0;
    for (int i = 0; i < 4; ++i)
        if (be >> i & 1) m |= 0xFFu << 8 * i;
    return m;
}
inline uint64_t name_hash(const std::string &s) {   // FNV-1a: the same on every machine
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
    return h;
}

struct Options {
    std::string test = "all";
    uint64_t seed = 1;
    uint64_t count = 0;                 // 0 = the test's default
    double sys_ns = 40.0;               // 25 MHz
    double ui_ns = 1000.0 / 81.25;      // 81.25 MHz: the Arty's MIG at 4:1
    double jitter = 0.0;                // fraction of a half period, random per edge
    uint32_t lat_min = 12, lat_max = 32;
    int rdy = 75, wdf = 75;
    bool refresh = true;
    bool verbose = false;
};

// Reference memory: per word, the values it may hold (one, unless a write may or may not have
// happened). A successful read must match one of them, and then that is the value. Never-written
// words hold initial_byte() (the MIG model) or zero (LiteDRAM's DRAM model).
class RefMem {
public:
    explicit RefMem(bool zero_init = false) : zero_(zero_init) {}
    uint32_t initial(uint32_t off) const {
        if (zero_) return 0;
        uint32_t w = 0;
        for (int i = 3; i >= 0; --i) w = w << 8 | initial_byte((off & ~3u) + i);
        return w;
    }
    void write(uint32_t off, uint8_t be, uint32_t data, bool certain) {
        std::vector<uint32_t> &c = slot(off), n;
        const uint32_t m = be_mask(be);
        for (uint32_t v : c) {
            add(n, (v & ~m) | (data & m));
            if (!certain) add(n, v);
        }
        c = n;
    }
    bool read(uint32_t off, uint32_t got, std::string &expected) {
        std::vector<uint32_t> &c = slot(off);
        for (uint32_t v : c)
            if (v == got) { c.assign(1, got); return true; }
        expected.clear();
        for (uint32_t v : c) {
            char b[16];
            std::snprintf(b, sizeof b, "%s%08x", expected.empty() ? "" : "|", v);
            expected += b;
        }
        return false;
    }
    bool certain(uint32_t off, uint32_t &v) {
        std::vector<uint32_t> &c = slot(off);
        if (c.size() != 1) return false;
        v = c[0];
        return true;
    }
    uint64_t maybes = 0;                // writes recorded as "may have happened"
private:
    static void add(std::vector<uint32_t> &v, uint32_t x) {
        if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
    }
    std::vector<uint32_t> &slot(uint32_t off) {
        off &= kDdrMask & ~3u;
        auto it = m_.find(off);
        if (it == m_.end()) it = m_.emplace(off, std::vector<uint32_t>(1, initial(off))).first;
        return it->second;
    }
    bool zero_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_;
};

struct Req {
    bool write = false;
    uint32_t addr = kDdrBase;
    uint8_t be = 0xF;
    uint32_t wdata = 0;
};
struct Rsp {
    bool got = false, err = false, aborted = false;
    uint32_t rdata = 0;
    uint64_t latency = 0;               // sys cycles from the accepting edge to the response cycle
};

// A random access: addresses from a small hot set, the whole window, near boundaries, or
// continuing a stream; byte enables as SURFBUS.md section 4 allows; reads and writes.
inline Req random_req(Rng &g, uint32_t &stream) {
    static const uint32_t bounds[] = {0x0, 0x10, 0x800, 0x2000, 0x10000, 0x1000000, 0x2000000,
                                      0x4000000, 0x8000000, 0xFFFFFF0};
    Req q;
    q.write = g.percent(50);
    q.be = kBe[g.range(0, 6)];
    uint32_t off;
    const int mode = static_cast<int>(g.range(0, 99));
    if (mode < 40) off = 0x123450u + static_cast<uint32_t>(g.range(0, 63)) * 4;            // hot set
    else if (mode < 70) off = g.u32();                                                    // anywhere
    else if (mode < 90) off = bounds[g.range(0, 9)] + static_cast<uint32_t>(g.range(0, 31)) * 4 - 64;
    else off = stream += 4;                                                               // stream
    q.addr = kDdrBase | (off & kDdrMask & ~3u) | be_offset(q.be);
    q.wdata = g.u32();
    return q;
}

// ------------------------------------------------------------------------------------------
template <class Top, class Side>
class Bench {
public:
    Bench(const Options &o, Side s, const std::string &name, bool zero_init = false, int rand_reset = 2)
        : o_(o), name_(name), rng_(o.seed * 7919 + name_hash(name)), side(std::move(s)), ref(zero_init) {
        ctx_ = std::make_unique<VerilatedContext>();
        ctx_->randReset(rand_reset);    // 2: registers without an initial value start random
        ctx_->randSeed(static_cast<int>(o.seed & 0x7fffffff));
        dut_ = std::make_unique<Top>(ctx_.get(), "top");
        sys_half_ = std::max<uint64_t>(1, static_cast<uint64_t>(o.sys_ns * 500.0));
        ui_half_ = std::max<uint64_t>(1, static_cast<uint64_t>(o.ui_ns * 500.0));
        next_sys_ = sys_half_;
        next_ui_ = ui_half_ / 2 + rng_.range(0, ui_half_);   // a random phase
        dut_->clk = 0;
        dut_->ui_clk = 0;
        dut_->rst = 1;
        dut_->s_req_valid = 0;
        side.init(*dut_);
        dut_->eval();
        rst_left_ = 4;
        ticks(4);
    }
    ~Bench() { dut_->final(); }

    // ---- time
    void tick() {                       // one sys_clk cycle: through the next rising edge
        const uint64_t c = sys_cycle_;
        while (sys_cycle_ == c) advance();
    }
    void ticks(uint64_t n) { for (uint64_t i = 0; i < n; ++i) tick(); }
    bool wait_calib(uint64_t max_cycles) {
        for (uint64_t i = 0; i < max_cycles && !dut_->calib_done; ++i) tick();
        return dut_->calib_done;
    }
    void wait_reset_done() { while (dut_->rst || rst_left_) tick(); }
    void stop_ui_clock() { ui_running_ = false; }
    void start_ui_clock() {
        if (!ui_running_) { ui_running_ = true; next_ui_ = t_ + ui_half_; }
    }
    // a controller reset (Side::ui_reset) after `after` more sys cycles
    void schedule_ui_reset(uint64_t after, uint32_t len) { ui_reset_at_ = sys_cycle_ + after + 1; ui_reset_len_ = len; }

    // ---- reset: rst for n sys cycles from the next one (runs them)
    void sys_reset(int n) {
        rst_left_ = n;
        ticks(n + 1);
    }

    // ---- one SurfBus access; with reset_after >= 0, rst comes that many cycles after the offer
    Rsp access(const Req &r, int gap = 0, int reset_after = -1, int reset_len = 1) {
        for (int i = 0; i < gap; ++i) tick();
        while (dut_->rst || rst_left_) tick();      // a master offers nothing during reset
        aborted_now_ = false;
        offering_ = true;
        offer_ = r;
        offer_aborted_ = false;
        have_rsp_ = false;
        drive_req();
        dut_->eval();
        const uint64_t start = sys_cycle_;
        for (;;) {
            if (reset_after >= 0 && sys_cycle_ - start == static_cast<uint64_t>(reset_after)) {
                rst_left_ = reset_len;
                ++resets_applied;
            }
            tick();
            if (have_rsp_) return rsp_;
            if (offer_aborted_ || aborted_now_) { Rsp a; a.aborted = true; aborted_now_ = false; return a; }
            if (sys_cycle_ - start > kTimeout + 64) {
                fail("no response %llu cycles after offering %s %08x", (unsigned long long)(sys_cycle_ - start),
                     r.write ? "write" : "read", r.addr);
                Rsp a; a.aborted = true;
                return a;
            }
        }
    }
    // checked helpers for the directed tests
    uint32_t read(uint32_t addr, uint8_t be = 0xF, int gap = 0) {
        Req q; q.write = false; q.addr = addr; q.be = be;
        Rsp s = access(q, gap);
        if (!s.got || s.err) fail("read %08x: %s", addr, s.got ? "error response" : "no response");
        return s.rdata;
    }
    void write(uint32_t addr, uint8_t be, uint32_t data, int gap = 0) {
        Req q; q.write = true; q.addr = addr; q.be = be; q.wdata = data;
        Rsp s = access(q, gap);
        if (!s.got || s.err) fail("write %08x: %s", addr, s.got ? "error response" : "no response");
    }

    // ---- results
    void fail(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        ++failures;
        if (failures > 20) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        std::printf("FAIL %s (sys cycle %llu): %s\n", name_.c_str(), (unsigned long long)sys_cycle_, buf);
    }
    bool finish(bool may_time_out = false) {
        ticks(8);
        side.finish(*this);
        if (!may_time_out && timeouts) fail("unexpected watchdog timeout");
        return failures == 0;
    }
    void summary(const char *extra = "") const {
        char ctrl[64] = "";
        if (side.observable())
            std::snprintf(ctrl, sizeof ctrl, " ctrl_rd=%llu ctrl_wr=%llu", (unsigned long long)side.reads(),
                          (unsigned long long)side.writes());
        std::printf("%s: %-15s %s seed=%llu sys=%.3fns ui=%.3fns jitter=%.2f accesses=%llu "
                    "reads=%llu writes=%llu err=%llu aborted=%llu timeouts=%llu lat=%llu/%.1f/%llu%s "
                    "cycles=%llu%s%s\n",
                    side.tag(), name_.c_str(), failures ? "FAIL" : "PASS", (unsigned long long)o_.seed, o_.sys_ns,
                    o_.ui_ns, o_.jitter, (unsigned long long)accesses, (unsigned long long)reads_ok,
                    (unsigned long long)writes_ok, (unsigned long long)errors, (unsigned long long)aborts,
                    (unsigned long long)timeouts, (unsigned long long)(lat_n ? lat_min : 0),
                    lat_n ? double(lat_sum) / lat_n : 0.0, (unsigned long long)lat_max, ctrl,
                    (unsigned long long)sys_cycle_, *extra ? " " : "", extra);
    }

    Top &dut() { return *dut_; }
    uint64_t cycle() const { return sys_cycle_; }
    Rng &rng() { return rng_; }
    bool calib_done() const { return dut_->calib_done; }
    bool calib_fail() const { return dut_->calib_fail; }
    bool timeout() const { return dut_->timeout; }

    const Options &o_;
    std::string name_;
    Rng rng_;
    Side side;
    RefMem ref;
    int failures = 0;
    uint64_t accesses = 0, reads_ok = 0, writes_ok = 0, errors = 0, aborts = 0, timeouts = 0;
    uint64_t lat_min = ~uint64_t(0), lat_max = 0, lat_sum = 0, lat_n = 0;
    // accesses cut short by rst: 0 not yet at the controller, 1 controller busy with it,
    // 2 controller finished (response on its way back); 3 offered, not accepted
    uint64_t abort_phase[4] = {0, 0, 0, 0};
    uint64_t resets_applied = 0, ui_resets_applied = 0;

private:
    struct PreSys { bool valid, ready; };

    void drive_req() {
        dut_->s_req_valid = offering_;
        dut_->s_req_addr = offer_.addr;
        dut_->s_req_write = offer_.write;
        dut_->s_req_be = offer_.be;
        dut_->s_req_wdata = offer_.wdata;
        dut_->s_req_fetch = 0;
    }
    uint64_t half(uint64_t h) {
        if (o_.jitter <= 0) return h;
        const double j = (double(rng_.range(0, 2000000)) / 1000000.0 - 1.0) * o_.jitter;
        return std::max<uint64_t>(1, static_cast<uint64_t>(double(h) * (1.0 + j)));
    }

    void advance() {                    // the next clock toggle (or two at the same time)
        const uint64_t t = std::min(next_sys_, ui_running_ ? next_ui_ : ~uint64_t(0));
        const bool sys_t = next_sys_ == t, ui_t = ui_running_ && next_ui_ == t;
        const bool sys_rise = sys_t && !dut_->clk, ui_rise = ui_t && !dut_->ui_clk;
        if (ui_rise) side.pre_ui_rise(*dut_);
        PreSys pre{dut_->s_req_valid != 0, dut_->s_req_ready != 0};
        t_ = t;
        if (sys_t) { dut_->clk = !dut_->clk; next_sys_ = t + half(sys_half_); }
        if (ui_t) { dut_->ui_clk = !dut_->ui_clk; next_ui_ = t + half(ui_half_); }
        dut_->eval();
        if (ui_rise) side.post_ui_rise(*dut_);
        if (sys_rise) sys_edge(pre);
        dut_->eval();
    }

    void abort_outstanding() {
        if (side.observable())
            ++abort_phase[side.done() == ctrl_at_accept_ ? 0 : (side.idle() ? 2 : 1)];
        if (out_req_.write) { ref.write(out_req_.addr, out_req_.be, out_req_.wdata, false); ++ref.maybes; }
        outstanding_ = false;
        aborted_now_ = true;
        ++aborts;
    }

    void sys_edge(const PreSys &pre) {
        ++sys_cycle_;
        if (ui_reset_at_ && sys_cycle_ >= ui_reset_at_) {
            side.ui_reset(ui_reset_len_);
            ui_reset_at_ = 0;
            ++ui_resets_applied;
        }
        if (pre.valid && pre.ready) {   // accepted at this edge
            outstanding_ = true;
            out_req_ = offer_;
            accept_cycle_ = sys_cycle_;
            ctrl_at_accept_ = side.done();
            offering_ = false;
            ++accesses;
        }
        const bool r = rst_left_ > 0;
        if (rst_left_) --rst_left_;
        dut_->rst = r;
        if (r) {
            if (outstanding_) abort_outstanding();
            if (offering_) { offering_ = false; offer_aborted_ = true; ++abort_phase[3]; }
        }
        drive_req();
        dut_->eval();

        if (dut_->s_rsp_valid) {
            if (r) {
                fail("s_rsp_valid during reset");
            } else if (!outstanding_) {
                fail("response with no request outstanding");
            } else {
                Rsp s;
                s.got = true;
                s.err = dut_->s_rsp_err;
                s.rdata = dut_->s_rsp_rdata;
                s.latency = sys_cycle_ - accept_cycle_;
                const uint32_t off = out_req_.addr & kDdrMask;
                if (s.err) {
                    ++errors;
                    if (s.latency == kTimeout) ++timeouts;
                    if (out_req_.write && s.latency >= kTimeout) {
                        ref.write(off, out_req_.be, out_req_.wdata, false);   // may have happened
                        ++ref.maybes;
                    }
                    if (!dut_->timeout && s.latency == kTimeout) fail("watchdog error without TIMEOUT");
                } else {
                    lat_min = std::min(lat_min, s.latency);
                    lat_max = std::max(lat_max, s.latency);
                    lat_sum += s.latency;
                    ++lat_n;
                    if (out_req_.write) {
                        ref.write(off, out_req_.be, out_req_.wdata, true);
                        ++writes_ok;
                    } else {
                        std::string exp;
                        if (!ref.read(off, s.rdata, exp))
                            fail("read %08x (be %x) returned %08x, expected %s", out_req_.addr, out_req_.be,
                                 s.rdata, exp.c_str());
                        ++reads_ok;
                    }
                }
                outstanding_ = false;
                have_rsp_ = true;
                rsp_ = s;
            }
        }
        if (dut_->calib_done && dut_->calib_fail) fail("calib_done and calib_fail both 1");
        if (outstanding_ && sys_cycle_ - accept_cycle_ > kTimeout)
            fail("no response %llu cycles after acceptance (watchdog is %llu)",
                 (unsigned long long)(sys_cycle_ - accept_cycle_), (unsigned long long)kTimeout);
        if (prev_timeout_ && !dut_->timeout && !r) fail("timeout cleared without a reset");
        prev_timeout_ = dut_->timeout && !r;
    }

    std::unique_ptr<VerilatedContext> ctx_;
    std::unique_ptr<Top> dut_;
    uint64_t t_ = 0, sys_half_, ui_half_, next_sys_, next_ui_;
    bool ui_running_ = true;
    uint64_t sys_cycle_ = 0;
    int rst_left_ = 0;
    bool offering_ = false, offer_aborted_ = false, outstanding_ = false, have_rsp_ = false;
    bool aborted_now_ = false, prev_timeout_ = false;
    Req offer_, out_req_;
    Rsp rsp_;
    uint64_t accept_cycle_ = 0, ctrl_at_accept_ = 0;
    uint64_t ui_reset_at_ = 0;
    uint32_t ui_reset_len_ = 1;
};

// Command-line options shared by the testbenches; returns false on a bad option.
inline bool parse_options(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--test") o.test = val();
        else if (a == "--seed") o.seed = std::strtoull(val(), nullptr, 0);
        else if (a == "--count") o.count = std::strtoull(val(), nullptr, 0);
        else if (a == "--sys-ns") o.sys_ns = std::atof(val());
        else if (a == "--ui-ns") o.ui_ns = std::atof(val());
        else if (a == "--jitter") o.jitter = std::atof(val());
        else if (a == "--lat") { if (std::sscanf(val(), "%u:%u", &o.lat_min, &o.lat_max) != 2) return false; }
        else if (a == "--rdy") o.rdy = std::atoi(val());
        else if (a == "--wdf") o.wdf = std::atoi(val());
        else if (a == "--no-refresh") o.refresh = false;
        else if (a == "-v") o.verbose = true;
        else if (a.rfind("+verilator", 0) == 0) continue;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    return true;
}

}  // namespace surfddr
