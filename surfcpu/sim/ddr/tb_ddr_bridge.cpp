/*
SurfCPU DDR bridge testbench
--------------------
File: tb_ddr_bridge.cpp Date: 10/15/26 (SurfCPU)
--------------------
Verilator testbench for surf_ddr_bridge (rtl/soc/ddr/) against the MIG user-interface model
(mig_ui_model.h), each on its own clock: sys_clk drives the bridge's SurfBus side, ui_clk the
model and the bridge's controller side, with any periods and optional jitter, and the ui
clock can be stopped. A SurfBus master issues accesses following SURFBUS.md (one
outstanding, no withdrawal, the next request offered as early as the cycle of the previous
response); every response is checked against a reference memory that also knows which
writes may or may not have happened (an access cut short by a reset or the watchdog).

Always checked, in every test: the model's interface checks; a response only for an
accepted request, never during reset, and never later than the watchdog allows; calib_done
and calib_fail never both 1; timeout sticky until rst; at most one command at a time in the
controller; no write command ever lost by a controller reset (its data always came first).

usage: tb_ddr_bridge [--test NAME|all] [--seed N] [--count N] [--sys-ns P] [--ui-ns P]
                     [--jitter F] [--lat MIN:MAX] [--rdy PCT] [--wdf PCT] [--no-refresh] [-v]
tests: basic lanes bounds alias random stream precal calfail calfail-stopped watchdog
       sysreset uireset all (calfail-stopped is not part of all)
Exit status 0 when every selected test passes.
*/
#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Vsurf_ddr_bridge.h"
#include "verilated.h"

#include "ddr_rng.h"
#include "mig_ui_model.h"

#ifndef TB_TIMEOUT_CYCLES
#error "build with -DTB_TIMEOUT_CYCLES=<the bridge's TIMEOUT_CYCLES>"
#endif
#ifndef TB_CALIB_TIMEOUT_CYCLES
#error "build with -DTB_CALIB_TIMEOUT_CYCLES=<the bridge's CALIB_TIMEOUT_CYCLES>"
#endif

using namespace surfddr;

namespace {

constexpr uint64_t kTimeout = TB_TIMEOUT_CYCLES;
constexpr uint64_t kCalibTimeout = TB_CALIB_TIMEOUT_CYCLES;
constexpr uint32_t kDdrBase = 0x80000000u;
constexpr uint32_t kDdrMask = 0x0FFFFFFFu;
const uint8_t kBe[7] = {0x1, 0x2, 0x4, 0x8, 0x3, 0xC, 0xF};

uint32_t be_offset(uint8_t be) {        // SURFBUS.md section 4: addr[1:0] that goes with be
    switch (be) {
        case 0x2: return 1;
        case 0x4: case 0xC: return 2;
        case 0x8: return 3;
        default: return 0;
    }
}
uint64_t name_hash(const std::string &s) {     // FNV-1a: the same on every machine
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
    return h;
}
uint32_t be_mask(uint8_t be) {
    uint32_t m = 0;
    for (int i = 0; i < 4; ++i)
        if (be >> i & 1) m |= 0xFFu << 8 * i;
    return m;
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

// ------------------------------------------------------------------------------------------
// Reference memory: per word, the values it may hold (one, unless a write may or may not have
// happened). A successful read must match one of them, and then that is the value.
class RefMem {
public:
    static uint32_t initial(uint32_t off) {
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

// ------------------------------------------------------------------------------------------
class Bench {
public:
    Bench(const Options &o, const MigUiConfig &mc, const std::string &name)
        : o_(o), name_(name), rng_(o.seed * 7919 + name_hash(name)), mig(mc) {
        ctx_ = std::make_unique<VerilatedContext>();
        ctx_->randReset(2);             // registers without an initial value start random
        ctx_->randSeed(static_cast<int>(o.seed & 0x7fffffff));
        dut_ = std::make_unique<Vsurf_ddr_bridge>(ctx_.get(), "bridge");
        sys_half_ = std::max<uint64_t>(1, static_cast<uint64_t>(o.sys_ns * 500.0));
        ui_half_ = std::max<uint64_t>(1, static_cast<uint64_t>(o.ui_ns * 500.0));
        next_sys_ = sys_half_;
        next_ui_ = ui_half_ / 2 + rng_.range(0, ui_half_);   // a random phase
        dut_->clk = 0;
        dut_->ui_clk = 0;
        dut_->rst = 1;
        dut_->s_req_valid = 0;
        drive_ui(mig.out());
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
        for (uint64_t i = 0; i < max_cycles && !dut_->calib_done; ++i) {
            tick();
            if (o_.verbose && i % 16 == 0)
                std::printf("  wait_calib %llu: ui_rst=%d calib=%d mig_cycles=%llu bridge calib_done=%d fail=%d\n",
                            (unsigned long long)i, mig.out().ui_clk_sync_rst, mig.out().init_calib_complete,
                            (unsigned long long)mig.stats().cycles, dut_->calib_done, dut_->calib_fail);
        }
        return dut_->calib_done;
    }
    void wait_reset_done() { while (dut_->rst || rst_left_) tick(); }
    void stop_ui_clock() { ui_running_ = false; }
    // a controller reset (ui_clk_sync_rst for len ui cycles) after `after` more sys cycles
    void schedule_ui_reset(uint64_t after, uint32_t len) { ui_reset_at_ = sys_cycle_ + after + 1; ui_reset_len_ = len; }
    void start_ui_clock() {
        if (!ui_running_) { ui_running_ = true; next_ui_ = t_ + ui_half_; }
    }

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
        for (const std::string &e : mig.errors()) fail("%s", e.c_str());
        if (mig.stats().max_outstanding > 1)
            fail("%llu commands outstanding in the controller at once (the bridge issues one)",
                 (unsigned long long)mig.stats().max_outstanding);
        if (mig.stats().lost_writes) fail("a controller reset lost an accepted write command");
        if (!may_time_out && timeouts) fail("unexpected watchdog timeout");
        return failures == 0;
    }
    void summary(const char *extra = "") const {
        std::printf("tb_ddr_bridge: %-15s %s seed=%llu sys=%.3fns ui=%.3fns jitter=%.2f accesses=%llu "
                    "reads=%llu writes=%llu err=%llu aborted=%llu timeouts=%llu lat=%llu/%.1f/%llu "
                    "ctrl_rd=%llu ctrl_wr=%llu cycles=%llu%s%s\n",
                    name_.c_str(), failures ? "FAIL" : "PASS", (unsigned long long)o_.seed, o_.sys_ns,
                    o_.ui_ns, o_.jitter, (unsigned long long)accesses, (unsigned long long)reads_ok,
                    (unsigned long long)writes_ok, (unsigned long long)errors, (unsigned long long)aborts,
                    (unsigned long long)timeouts, (unsigned long long)(lat_n ? lat_min : 0),
                    lat_n ? double(lat_sum) / lat_n : 0.0, (unsigned long long)lat_max,
                    (unsigned long long)mig.stats().reads, (unsigned long long)mig.stats().writes,
                    (unsigned long long)sys_cycle_, *extra ? " " : "", extra);
    }

    Vsurf_ddr_bridge &dut() { return *dut_; }
    uint64_t cycle() const { return sys_cycle_; }
    Rng &rng() { return rng_; }
    bool calib_done() const { return dut_->calib_done; }
    bool calib_fail() const { return dut_->calib_fail; }
    bool timeout() const { return dut_->timeout; }

    const Options &o_;
    std::string name_;
    Rng rng_;
    MigUiModel mig;
    RefMem ref;
    int failures = 0;
    uint64_t accesses = 0, reads_ok = 0, writes_ok = 0, errors = 0, aborts = 0, timeouts = 0;
    uint64_t lat_min = ~uint64_t(0), lat_max = 0, lat_sum = 0, lat_n = 0;
    uint64_t ctrl_cmd_cycles = 0;       // ui cycles with app_en or app_wdf_wren high
    uint64_t abort_phase[4] = {0, 0, 0, 0};   // see abort_outstanding(); [3]: offer not yet accepted
    uint64_t resets_applied = 0, ui_resets_applied = 0;

private:
    struct PreSys { bool valid, ready; };

    void drive_ui(const MigUiOut &u) {
        dut_->ui_rst = u.ui_clk_sync_rst;
        dut_->init_calib_complete = u.init_calib_complete;
        dut_->app_rdy = u.app_rdy;
        dut_->app_wdf_rdy = u.app_wdf_rdy;
        dut_->app_rd_data_valid = u.app_rd_data_valid;
        dut_->app_rd_data_end = u.app_rd_data_end;
        for (int w = 0; w < 4; ++w)
            dut_->app_rd_data[w] = u.app_rd_data[4 * w] | u.app_rd_data[4 * w + 1] << 8 |
                                   u.app_rd_data[4 * w + 2] << 16 | uint32_t(u.app_rd_data[4 * w + 3]) << 24;
    }
    MigUiIn sample_ui() const {
        MigUiIn in;
        in.app_en = dut_->app_en;
        in.app_cmd = dut_->app_cmd;
        in.app_addr = dut_->app_addr;
        in.app_wdf_wren = dut_->app_wdf_wren;
        in.app_wdf_end = dut_->app_wdf_end;
        in.app_wdf_mask = dut_->app_wdf_mask;
        for (int w = 0; w < 4; ++w)
            for (int b = 0; b < 4; ++b) in.app_wdf_data[4 * w + b] = dut_->app_wdf_data[w] >> 8 * b;
        in.app_sr_req = dut_->app_sr_req;
        in.app_ref_req = dut_->app_ref_req;
        in.app_zq_req = dut_->app_zq_req;
        return in;
    }
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
        MigUiIn uin;
        if (ui_rise) uin = sample_ui();
        PreSys pre{dut_->s_req_valid != 0, dut_->s_req_ready != 0};
        t_ = t;
        if (sys_t) { dut_->clk = !dut_->clk; next_sys_ = t + half(sys_half_); }
        if (ui_t) { dut_->ui_clk = !dut_->ui_clk; next_ui_ = t + half(ui_half_); }
        dut_->eval();
        if (ui_rise) {
            if (uin.app_en || uin.app_wdf_wren) ++ctrl_cmd_cycles;
            mig.edge(uin);
            drive_ui(mig.out());
        }
        if (sys_rise) sys_edge(pre);
        dut_->eval();
    }

    void abort_outstanding() {
        // phase of the cut-short access: 0 not yet at the controller, 1 controller busy with it,
        // 2 controller finished (response on its way back)
        const uint64_t done_now = mig.stats().reads + mig.stats().writes;
        ++abort_phase[done_now == ctrl_at_accept_ ? 0 : (mig.idle() ? 2 : 1)];
        if (out_req_.write) { ref.write(out_req_.addr, out_req_.be, out_req_.wdata, false); ++ref.maybes; }
        outstanding_ = false;
        aborted_now_ = true;
        ++aborts;
    }

    void sys_edge(const PreSys &pre) {
        ++sys_cycle_;
        if (ui_reset_at_ && sys_cycle_ >= ui_reset_at_) {
            mig.start_reset(ui_reset_len_);
            ui_reset_at_ = 0;
            ++ui_resets_applied;
        }
        if (pre.valid && pre.ready) {   // accepted at this edge
            outstanding_ = true;
            out_req_ = offer_;
            accept_cycle_ = sys_cycle_;
            ctrl_at_accept_ = mig.stats().reads + mig.stats().writes;
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
    std::unique_ptr<Vsurf_ddr_bridge> dut_;
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

MigUiConfig model_config(const Options &o) {
    MigUiConfig m;
    m.seed = o.seed;
    m.rd_lat_min = o.lat_min;
    m.rd_lat_max = o.lat_max;
    m.rdy_pct = o.rdy;
    m.wdf_rdy_pct = o.wdf;
    if (!o.refresh) m.refresh_period = 0;
    return m;
}

uint64_t calib_wait(const Options &o, const MigUiConfig &m) {   // generous, in sys cycles
    return static_cast<uint64_t>((m.reset_cycles + m.calib_cycles + 16) * o.ui_ns / o.sys_ns) + 64;
}

// A random access: addresses from a small hot set, the whole window, near boundaries, or
// continuing a stream; byte enables as SURFBUS.md section 4 allows; reads and writes.
Req random_req(Rng &g, uint32_t &stream) {
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
bool t_basic(const Options &o) {
    MigUiConfig m = model_config(o);
    Bench b(o, m, "basic");
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    if (b.calib_fail() || b.timeout()) b.fail("calib_fail or timeout set");
    b.write(0x80000000, 0xF, 0x11223344);
    if (b.read(0x80000000) != 0x11223344) b.fail("readback");
    b.read(0x80000100);                 // untouched: the initial contents
    b.write(0x8FFFFFFC, 0xF, 0xCAFEF00D);
    b.read(0x8FFFFFFC);
    bool ok = b.finish();
    b.summary();
    return ok;
}

// Every byte lane of the 128-bit beat with every byte-enable pattern, checked through the bus
// and, byte by byte, in the controller's memory.
bool t_lanes(const Options &o) {
    MigUiConfig m = model_config(o);
    Bench b(o, m, "lanes");
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    const uint32_t beats[] = {0x80000000, 0x81234560, 0x8ABCDEF0, 0x8FFFFFF0};
    uint64_t checks = 0;
    for (uint32_t base : beats) {
        for (int lane = 0; lane < 4; ++lane) {
            for (uint8_t be : kBe) {
                const uint32_t addr = base + 4 * lane + be_offset(be);
                b.write(addr, be, b.rng().u32(), static_cast<int>(b.rng().range(0, 2)));
                // the controller's beat must equal the reference, byte for byte
                const Beat bt = b.mig.beat((base & kDdrMask) / 16);
                for (int w = 0; w < 4; ++w) {
                    uint32_t v;
                    if (!b.ref.certain((base & kDdrMask) + 4 * w, v)) b.fail("reference uncertain");
                    for (int k = 0; k < 4; ++k)
                        if (bt[4 * w + k] != static_cast<uint8_t>(v >> 8 * k))
                            b.fail("beat %08x byte %d is %02x, expected %02x after be %x lane %d", base,
                                   4 * w + k, bt[4 * w + k], static_cast<uint8_t>(v >> 8 * k), be, lane);
                    ++checks;
                }
                for (int w = 0; w < 4; ++w) b.read(base + 4 * w, kBe[(w + lane) % 7], 0);
            }
        }
    }
    bool ok = b.finish();
    char extra[64];
    std::snprintf(extra, sizeof extra, "beat_checks=%llu", (unsigned long long)checks);
    b.summary(extra);
    return ok;
}

// Region, row, bank and window boundaries, read before and after writing.
bool t_bounds(const Options &o) {
    MigUiConfig m = model_config(o);
    Bench b(o, m, "bounds");
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    const uint32_t edges[] = {0x0, 0x10, 0x800, 0x1000, 0x2000, 0x10000, 0x100000, 0x1000000,
                              0x2000000, 0x4000000, 0x6000000, 0x8000000, 0xE000000, 0xFFFFFF0};
    std::vector<uint32_t> addrs;
    for (uint32_t e : edges)
        for (int d = -16; d < 16; d += 4) {
            const int64_t a = int64_t(e) + d;
            if (a >= 0 && a <= int64_t(kDdrMask & ~3u)) addrs.push_back(kDdrBase | uint32_t(a));
        }
    for (uint32_t a : addrs) b.read(a);                 // initial contents
    for (uint32_t a : addrs) b.write(a, 0xF, b.rng().u32());
    for (uint32_t a : addrs) b.read(a);
    for (uint32_t a : addrs) if (b.mig.word(a & kDdrMask) != b.read(a)) b.fail("controller memory differs at %08x", a);
    bool ok = b.finish();
    char extra[64];
    std::snprintf(extra, sizeof extra, "addresses=%zu", addrs.size());
    b.summary(extra);
    return ok;
}

// Every address bit is distinct: walking ones and walking zeros over the 28-bit offset.
bool t_alias(const Options &o) {
    MigUiConfig m = model_config(o);
    Bench b(o, m, "alias");
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    std::vector<uint32_t> addrs;
    addrs.push_back(kDdrBase);
    for (int k = 2; k < 28; ++k) addrs.push_back(kDdrBase | 1u << k);
    addrs.push_back(kDdrBase | (kDdrMask & ~3u));
    for (int k = 2; k < 28; ++k) addrs.push_back(kDdrBase | ((kDdrMask & ~3u) ^ 1u << k));
    for (size_t i = 0; i < addrs.size(); ++i) b.write(addrs[i], 0xF, 0xA5000000u | uint32_t(i) << 8 | uint32_t(i));
    for (size_t i = 0; i < addrs.size(); ++i) {
        const uint32_t v = b.read(addrs[i]);
        if (v != (0xA5000000u | uint32_t(i) << 8 | uint32_t(i))) b.fail("alias at %08x: %08x", addrs[i], v);
        if (b.mig.word(addrs[i] & kDdrMask) != v) b.fail("controller memory differs at %08x", addrs[i]);
    }
    bool ok = b.finish();
    char extra[64];
    std::snprintf(extra, sizeof extra, "addresses=%zu", addrs.size());
    b.summary(extra);
    return ok;
}

bool t_random(const Options &o) {
    MigUiConfig m = model_config(o);
    Bench b(o, m, "random");
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    const uint64_t n = o.count ? o.count : 20000;
    uint32_t stream = 0x0400000;
    for (uint64_t i = 0; i < n && b.failures == 0; ++i) {
        Req q = random_req(b.rng(), stream);
        const int gap = b.rng().percent(50) ? 0 : static_cast<int>(b.rng().range(1, 4));
        Rsp s = b.access(q, gap);
        if (!s.got || s.err) b.fail("access %llu %s %08x: %s", (unsigned long long)i, q.write ? "write" : "read",
                                    q.addr, s.got ? "error" : "no response");
    }
    bool ok = b.finish();
    b.summary();
    return ok;
}

// Sustained back-to-back traffic: each request offered in the cycle of the previous response.
bool t_stream(const Options &o) {
    MigUiConfig m = model_config(o);
    Bench b(o, m, "stream");
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    const uint32_t n = o.count ? static_cast<uint32_t>(o.count) : 2048;
    const uint32_t base = 0x80200000;
    uint64_t c0 = b.cycle();
    for (uint32_t i = 0; i < n; ++i) b.write(base + 4 * i, 0xF, 0x5A000000u ^ i * 2654435761u);
    const uint64_t wr_cycles = b.cycle() - c0;
    c0 = b.cycle();
    for (uint32_t i = 0; i < n; ++i)
        if (b.read(base + 4 * i) != (0x5A000000u ^ i * 2654435761u)) b.fail("stream word %u", i);
    const uint64_t rd_cycles = b.cycle() - c0;
    c0 = b.cycle();
    for (uint32_t i = 0; i < n / 4; ++i) {    // read after write to one word, alternating lanes
        const uint32_t a = 0x80300000 + 4 * (i & 3);
        b.write(a, 0xF, i);
        if (b.read(a) != i) b.fail("RAW %u", i);
    }
    const uint64_t raw_cycles = b.cycle() - c0;
    bool ok = b.finish();
    char extra[160];
    std::snprintf(extra, sizeof extra, "sys_cycles_per_write=%.2f per_read=%.2f per_raw_pair=%.2f",
                  double(wr_cycles) / n, double(rd_cycles) / n, double(raw_cycles) / (n / 4));
    b.summary(extra);
    return ok;
}

// Before calibration every access is refused at once and nothing reaches the controller.
bool t_precal(const Options &o) {
    MigUiConfig m = model_config(o);
    m.calib_cycles = 3000;
    Bench b(o, m, "precal");
    uint32_t stream = 0;
    uint64_t refused = 0;
    for (;;) {
        if (b.calib_fail()) { b.fail("calib_fail during a normal calibration"); break; }
        if (b.calib_done()) break;
        Req q = random_req(b.rng(), stream);
        Rsp s = b.access(q, static_cast<int>(b.rng().range(0, 3)));
        if (s.got && !s.err) break;     // accepted in the cycle calib_done rose
        if (!s.got || s.latency != 0)
            b.fail("access before calibration: got=%d err=%d latency=%llu", s.got, s.err,
                   (unsigned long long)s.latency);
        ++refused;
        if (b.ctrl_cmd_cycles || b.mig.stats().reads || b.mig.stats().writes) {
            b.fail("the controller saw activity before calibration");
            break;
        }
        if (b.cycle() > 2 * calib_wait(o, m) + 1000) { b.fail("calibration never came"); break; }
    }
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    for (int i = 0; i < 200; ++i) {
        uint32_t st = 0x777000;
        Rsp s = b.access(random_req(b.rng(), st));
        if (!s.got || s.err) b.fail("access after calibration failed");
    }
    if (refused <= 10) b.fail("too few accesses before calibration (%llu)", (unsigned long long)refused);
    bool ok = b.finish();
    char extra[64];
    std::snprintf(extra, sizeof extra, "refused_before_calib=%llu", (unsigned long long)refused);
    b.summary(extra);
    return ok;
}

// Calibration never finishes: calib_fail exactly at the limit; a late calibration does not
// make the DDR usable; a reset tries again. stopped = the MIG's clock never runs at all.
bool t_calfail(const Options &o, bool stopped) {
    MigUiConfig m = model_config(o);
    m.calib_never = true;
    Bench b(o, m, stopped ? "calfail-stopped" : "calfail");
    if (stopped) b.stop_ui_clock();
    const uint64_t c0 = b.cycle();      // the cycle after rst went low
    while (!b.calib_fail() && b.cycle() - c0 < kCalibTimeout + 100) {
        if (b.calib_done()) b.fail("calib_done without calibration");
        b.tick();
    }
    const uint64_t when = b.cycle() - c0;
    if (!b.calib_fail()) b.fail("calib_fail not set");
    if (when + 4 < kCalibTimeout || when > kCalibTimeout + 4)
        b.fail("calib_fail after %llu cycles, limit %llu", (unsigned long long)when, (unsigned long long)kCalibTimeout);
    for (int i = 0; i < 20; ++i) {
        Req q; q.write = i & 1; q.addr = 0x80000000 + 4 * i; q.wdata = i;
        Rsp s = b.access(q);
        if (!s.got || !s.err || s.latency != 0) b.fail("access after calib_fail not refused at once");
    }
    // a late calibration: still failed until reset
    if (stopped) b.start_ui_clock();
    b.mig.set_calib_never(false);
    b.ticks(calib_wait(o, m));
    if (b.calib_done() || !b.calib_fail()) b.fail("late calibration changed the failed state");
    b.sys_reset(3);
    if (!b.wait_calib(64)) b.fail("no calib_done after reset with the controller calibrated");
    if (b.calib_fail()) b.fail("calib_fail survived the reset");
    b.write(0x80000040, 0xF, 0x600DCA1B);
    if (b.read(0x80000040) != 0x600DCA1B) b.fail("readback after recovery");
    bool ok = b.finish();
    char extra[64];
    std::snprintf(extra, sizeof extra, "calib_fail_after=%llu", (unsigned long long)when);
    b.summary(extra);
    return ok;
}

// A stuck controller: the access is answered with an error exactly TIMEOUT_CYCLES after
// acceptance, TIMEOUT is set and stays, later accesses are refused at once, the late answer
// is dropped, and after a reset everything works again.
bool t_watchdog(const Options &o) {
    const char *kinds[] = {"rdy-read", "rdy-write", "wdf-write", "drop-read", "uiclk-read", "uiclk-write"};
    bool all = true;
    uint64_t cases = 0;
    for (int kind = 0; kind < 6; ++kind) {
        for (int rep = 0; rep < 3; ++rep) {
            Options oo = o;
            oo.seed = o.seed * 31 + kind * 7 + rep;
            MigUiConfig m = model_config(oo);
            Bench b(oo, m, std::string("watchdog-") + kinds[kind]);
            if (!b.wait_calib(calib_wait(oo, m))) b.fail("no calibration");
            uint32_t st = 0x50000;
            for (int i = 0; i < 50; ++i) b.access(random_req(b.rng(), st), 0);
            // the fault, at a random moment around the offer
            const bool write = kind == 1 || kind == 2 || kind == 5;
            Req q; q.write = write; q.addr = 0x80123450; q.be = 0xF; q.wdata = 0xDEADBEEF;
            switch (kind) {
                case 0: case 1: b.mig.set_stuck_rdy(true); break;
                case 2: b.mig.set_stuck_wdf(true); break;
                case 3: b.mig.set_drop_reads(true); break;
                default: b.stop_ui_clock(); break;
            }
            b.ticks(b.rng().range(0, 3));
            Rsp s = b.access(q);
            if (!s.got || !s.err || s.latency != kTimeout)
                b.fail("stuck access: got=%d err=%d latency=%llu (want error at %llu)", s.got, s.err,
                       (unsigned long long)s.latency, (unsigned long long)kTimeout);
            if (!b.timeout()) b.fail("TIMEOUT not set");
            for (int i = 0; i < 10; ++i) {
                Rsp t = b.access(random_req(b.rng(), st), static_cast<int>(b.rng().range(0, 2)));
                if (!t.got || !t.err || t.latency != 0) b.fail("access after a timeout not refused at once");
            }
            // the controller recovers; its late answer must be dropped
            b.mig.set_stuck_rdy(false);
            b.mig.set_stuck_wdf(false);
            b.start_ui_clock();
            if (kind == 3) {            // a read that never returns needs a controller reset
                b.mig.set_drop_reads(false);
                b.mig.start_reset(8);
            }
            b.ticks(b.rng().range(0, 200));
            if (!b.timeout()) b.fail("TIMEOUT cleared before reset");
            b.sys_reset(static_cast<int>(b.rng().range(1, 4)));
            if (b.timeout()) b.fail("TIMEOUT survived the reset");
            if (!b.wait_calib(calib_wait(oo, m) + 64)) b.fail("no calibration after recovery");
            for (int i = 0; i < 300; ++i) {
                Rsp t = b.access(random_req(b.rng(), st), static_cast<int>(b.rng().range(0, 2)));
                if (!t.got || t.err) b.fail("access after recovery failed");
            }
            {                           // the abandoned write may have happened or not; say which
                Req r; r.addr = q.addr;
                Rsp t = b.access(r);
                if (!t.got || t.err) b.fail("read of the abandoned address failed");
            }
            all = b.finish(true) && all;
            if (b.timeouts != 1) b.fail("%llu timeouts, expected 1", (unsigned long long)b.timeouts);
            all = all && b.failures == 0;
            if (rep == 0 || b.failures) b.summary();
            ++cases;
        }
    }
    std::printf("tb_ddr_bridge: %-15s %s cases=%llu\n", "watchdog", all ? "PASS" : "FAIL", (unsigned long long)cases);
    return all;
}

// Random traffic with rst at random moments, also in the middle of accesses at every stage.
bool t_sysreset(const Options &o) {
    MigUiConfig m = model_config(o);
    Bench b(o, m, "sysreset");
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    const uint64_t n = o.count ? o.count : 10000;
    uint32_t stream = 0x0800000;
    for (uint64_t i = 0; i < n && b.failures == 0; ++i) {
        Req q = random_req(b.rng(), stream);
        const int gap = b.rng().percent(60) ? 0 : static_cast<int>(b.rng().range(1, 3));
        if (b.rng().percent(8)) {       // rst somewhere inside this access (or just after it)
            const uint64_t typ = b.lat_n ? b.lat_sum / b.lat_n + 2 : 20;
            const int after = static_cast<int>(b.rng().range(0, typ + typ / 2));
            Rsp s = b.access(q, gap, after, static_cast<int>(b.rng().range(1, 5)));
            if (s.got && s.err) b.fail("error response around a reset");
            b.wait_reset_done();        // calib_done drops during rst and returns after it
            b.wait_calib(64);
        } else {
            Rsp s = b.access(q, gap);
            if (!s.got || s.err) b.fail("access %llu failed", (unsigned long long)i);
        }
    }
    for (int i = 0; i < 64; ++i) {      // read back the hot set: settles what resets left open
        Req r; r.addr = 0x80123450 + 4 * i;
        b.access(r);
    }
    bool ok = b.finish();
    char extra[160];
    std::snprintf(extra, sizeof extra,
                  "resets=%llu cut_before_ctrl=%llu cut_in_ctrl=%llu cut_after_ctrl=%llu maybes=%llu",
                  (unsigned long long)b.resets_applied,
                  (unsigned long long)b.abort_phase[0], (unsigned long long)b.abort_phase[1],
                  (unsigned long long)b.abort_phase[2], (unsigned long long)b.ref.maybes);
    b.summary(extra);
    if (b.abort_phase[0] == 0 || b.abort_phase[1] + b.abort_phase[2] == 0) {
        std::printf("FAIL sysreset: resets did not hit accesses at every stage\n");
        ok = false;
    }
    return ok;
}

// Random traffic while the controller itself resets (calibration lost and regained), at
// random moments, also in the middle of accesses.
bool t_uireset(const Options &o) {
    MigUiConfig m = model_config(o);
    m.calib_cycles = 100;
    Bench b(o, m, "uireset");
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    const uint64_t n = o.count ? o.count : 6000;
    uint32_t stream = 0x0C00000;
    uint64_t at_once = 0, by_ui = 0, by_watchdog = 0;
    for (uint64_t i = 0; i < n && b.failures == 0; ++i) {
        Req q = random_req(b.rng(), stream);
        if (b.rng().percent(5)) {
            const uint64_t typ = b.lat_n ? b.lat_sum / b.lat_n + 2 : 20;
            b.schedule_ui_reset(b.rng().range(0, 2 * typ), static_cast<uint32_t>(b.rng().range(1, 20)));
        }
        Rsp s = b.access(q, b.rng().percent(50) ? 0 : 1);
        if (!s.got) {
            b.fail("no response");
        } else if (s.err) {
            if (s.latency == 0) ++at_once;               // calib_done already low
            else if (s.latency < kTimeout) ++by_ui;      // the ui side refused it untouched
            else ++by_watchdog;                          // a reset lost it inside the ui side
            if (b.timeout()) b.sys_reset(2);
            b.wait_calib(calib_wait(o, m) + 64);
        }
    }
    for (int i = 0; i < 64; ++i) { Req r; r.addr = 0x80123450 + 4 * i; b.access(r); }
    bool ok = b.finish(true);
    char extra[160];
    std::snprintf(extra, sizeof extra, "ui_resets=%llu refused_at_once=%llu refused_by_ui=%llu watchdog=%llu maybes=%llu",
                  (unsigned long long)b.ui_resets_applied, (unsigned long long)at_once, (unsigned long long)by_ui,
                  (unsigned long long)by_watchdog, (unsigned long long)b.ref.maybes);
    b.summary(extra);
    if (at_once == 0 || by_ui == 0 || by_watchdog == 0) {
        std::printf("FAIL uireset: controller resets did not produce every kind of refusal\n");
        ok = false;
    }
    return ok;
}

bool run(const std::string &t, const Options &o) {
    if (t == "basic") return t_basic(o);
    if (t == "lanes") return t_lanes(o);
    if (t == "bounds") return t_bounds(o);
    if (t == "alias") return t_alias(o);
    if (t == "random") return t_random(o);
    if (t == "stream") return t_stream(o);
    if (t == "precal") return t_precal(o);
    if (t == "calfail") return t_calfail(o, false);
    if (t == "calfail-stopped") return t_calfail(o, true);
    if (t == "watchdog") return t_watchdog(o);
    if (t == "sysreset") return t_sysreset(o);
    if (t == "uireset") return t_uireset(o);
    std::printf("tb_ddr_bridge: unknown test %s\n", t.c_str());
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](void) -> const char * {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--test") o.test = val();
        else if (a == "--seed") o.seed = std::strtoull(val(), nullptr, 0);
        else if (a == "--count") o.count = std::strtoull(val(), nullptr, 0);
        else if (a == "--sys-ns") o.sys_ns = std::atof(val());
        else if (a == "--ui-ns") o.ui_ns = std::atof(val());
        else if (a == "--jitter") o.jitter = std::atof(val());
        else if (a == "--lat") { if (std::sscanf(val(), "%u:%u", &o.lat_min, &o.lat_max) != 2) return 2; }
        else if (a == "--rdy") o.rdy = std::atoi(val());
        else if (a == "--wdf") o.wdf = std::atoi(val());
        else if (a == "--no-refresh") o.refresh = false;
        else if (a == "-v") o.verbose = true;
        else if (a == "-h" || a == "--help") {
            std::printf("see the header of sim/ddr/tb_ddr_bridge.cpp\n");
            return 0;
        } else if (a.rfind("+verilator", 0) == 0) {
            continue;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    const char *all[] = {"basic", "lanes", "bounds", "alias", "random", "stream", "precal", "calfail",
                         "watchdog", "sysreset", "uireset"};
    bool ok = true;
    if (o.test == "all") {
        for (const char *t : all) ok = run(t, o) && ok;
    } else {
        ok = run(o.test, o);
    }
    std::printf("tb_ddr_bridge: %s (watchdog %llu, calibration limit %llu cycles)\n", ok ? "PASS" : "FAIL",
                (unsigned long long)kTimeout, (unsigned long long)kCalibTimeout);
    return ok ? 0 : 1;
}
