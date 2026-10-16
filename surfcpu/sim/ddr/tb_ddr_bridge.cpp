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
       abandon sysreset uireset all (calfail-stopped is not part of all)
Exit status 0 when every selected test passes.
*/
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Vsurf_ddr_bridge.h"

#include "mig_ui_model.h"
#include "surfbus_bench.h"

using namespace surfddr;

namespace {

// The controller side: the C++ MIG user-interface model on ui_clk.
struct MigSide {
    explicit MigSide(const MigUiConfig &c) : mig(c) {}
    MigUiModel mig;
    MigUiIn in;
    uint64_t cmd_cycles = 0;            // ui cycles with app_en or app_wdf_wren high

    void init(Vsurf_ddr_bridge &t) { drive(t); }
    void pre_ui_rise(Vsurf_ddr_bridge &t) {
        in.app_en = t.app_en;
        in.app_cmd = t.app_cmd;
        in.app_addr = t.app_addr;
        in.app_wdf_wren = t.app_wdf_wren;
        in.app_wdf_end = t.app_wdf_end;
        in.app_wdf_mask = t.app_wdf_mask;
        for (int w = 0; w < 4; ++w)
            for (int b = 0; b < 4; ++b) in.app_wdf_data[4 * w + b] = t.app_wdf_data[w] >> 8 * b;
        in.app_sr_req = t.app_sr_req;
        in.app_ref_req = t.app_ref_req;
        in.app_zq_req = t.app_zq_req;
    }
    void post_ui_rise(Vsurf_ddr_bridge &t) {
        if (in.app_en || in.app_wdf_wren) ++cmd_cycles;
        mig.edge(in);
        drive(t);
    }
    uint64_t done() const { return mig.stats().reads + mig.stats().writes; }
    bool idle() const { return mig.idle(); }
    bool observable() const { return true; }
    void ui_reset(uint32_t len) { mig.start_reset(len); }
    template <class B> void finish(B &b) const {
        for (const std::string &e : mig.errors()) b.fail("%s", e.c_str());
        if (mig.stats().max_outstanding > 1)
            b.fail("%llu commands outstanding in the controller at once (the bridge issues one)",
                   (unsigned long long)mig.stats().max_outstanding);
        if (mig.stats().lost_writes) b.fail("a controller reset lost an accepted write command");
    }
    uint64_t reads() const { return mig.stats().reads; }
    uint64_t writes() const { return mig.stats().writes; }
    const char *tag() const { return "tb_ddr_bridge"; }

private:
    void drive(Vsurf_ddr_bridge &t) const {
        const MigUiOut &u = mig.out();
        t.ui_rst = u.ui_clk_sync_rst;
        t.init_calib_complete = u.init_calib_complete;
        t.app_rdy = u.app_rdy;
        t.app_wdf_rdy = u.app_wdf_rdy;
        t.app_rd_data_valid = u.app_rd_data_valid;
        t.app_rd_data_end = u.app_rd_data_end;
        for (int w = 0; w < 4; ++w)
            t.app_rd_data[w] = u.app_rd_data[4 * w] | u.app_rd_data[4 * w + 1] << 8 |
                               u.app_rd_data[4 * w + 2] << 16 | uint32_t(u.app_rd_data[4 * w + 3]) << 24;
    }
};

using MigBench = Bench<Vsurf_ddr_bridge, MigSide>;

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

// ------------------------------------------------------------------------------------------
bool t_basic(const Options &o) {
    MigUiConfig m = model_config(o);
    MigBench b(o, MigSide(m), "basic");
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
    MigBench b(o, MigSide(m), "lanes");
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    const uint32_t beats[] = {0x80000000, 0x81234560, 0x8ABCDEF0, 0x8FFFFFF0};
    uint64_t checks = 0;
    for (uint32_t base : beats) {
        for (int lane = 0; lane < 4; ++lane) {
            for (uint8_t be : kBe) {
                const uint32_t addr = base + 4 * lane + be_offset(be);
                b.write(addr, be, b.rng().u32(), static_cast<int>(b.rng().range(0, 2)));
                // the controller's beat must equal the reference, byte for byte
                const Beat bt = b.side.mig.beat((base & kDdrMask) / 16);
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
    MigBench b(o, MigSide(m), "bounds");
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
    for (uint32_t a : addrs) if (b.side.mig.word(a & kDdrMask) != b.read(a)) b.fail("controller memory differs at %08x", a);
    bool ok = b.finish();
    char extra[64];
    std::snprintf(extra, sizeof extra, "addresses=%zu", addrs.size());
    b.summary(extra);
    return ok;
}

// Every address bit is distinct: walking ones and walking zeros over the 28-bit offset.
bool t_alias(const Options &o) {
    MigUiConfig m = model_config(o);
    MigBench b(o, MigSide(m), "alias");
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
        if (b.side.mig.word(addrs[i] & kDdrMask) != v) b.fail("controller memory differs at %08x", addrs[i]);
    }
    bool ok = b.finish();
    char extra[64];
    std::snprintf(extra, sizeof extra, "addresses=%zu", addrs.size());
    b.summary(extra);
    return ok;
}

bool t_random(const Options &o) {
    MigUiConfig m = model_config(o);
    MigBench b(o, MigSide(m), "random");
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
    MigBench b(o, MigSide(m), "stream");
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
    MigBench b(o, MigSide(m), "precal");
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
        if (b.side.cmd_cycles || b.side.mig.stats().reads || b.side.mig.stats().writes) {
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
    MigBench b(o, MigSide(m), stopped ? "calfail-stopped" : "calfail");
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
    b.side.mig.set_calib_never(false);
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
            MigBench b(oo, MigSide(m), std::string("watchdog-") + kinds[kind]);
            if (!b.wait_calib(calib_wait(oo, m))) b.fail("no calibration");
            uint32_t st = 0x50000;
            for (int i = 0; i < 50; ++i) b.access(random_req(b.rng(), st), 0);
            // the fault, at a random moment around the offer
            const bool write = kind == 1 || kind == 2 || kind == 5;
            Req q; q.write = write; q.addr = 0x80123450; q.be = 0xF; q.wdata = 0xDEADBEEF;
            switch (kind) {
                case 0: case 1: b.side.mig.set_stuck_rdy(true); break;
                case 2: b.side.mig.set_stuck_wdf(true); break;
                case 3: b.side.mig.set_drop_reads(true); break;
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
            b.side.mig.set_stuck_rdy(false);
            b.side.mig.set_stuck_wdf(false);
            b.start_ui_clock();
            if (kind == 3) {            // a read that never returns needs a controller reset
                b.side.mig.set_drop_reads(false);
                b.side.mig.start_reset(8);
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

// An access abandoned by the watchdog before it could enter the request mailbox (still
// occupied by an earlier request that the stopped controller clock never took) is never
// carried out; the earlier one is, once the clock runs again.
bool t_abandon(const Options &o) {
    MigUiConfig m = model_config(o);
    MigBench b(o, MigSide(m), "abandon");
    if (!b.wait_calib(calib_wait(o, m))) b.fail("no calibration");
    uint32_t st = 0x90000;
    for (int i = 0; i < 20; ++i) b.access(random_req(b.rng(), st));
    const uint32_t a_addr = 0x80400000, b_addr = 0x80400040;
    const uint32_t b_before = b.side.mig.word(b_addr & kDdrMask);
    b.stop_ui_clock();
    Req qa; qa.write = true; qa.addr = a_addr; qa.wdata = 0xAAAA5555;
    Rsp sa = b.access(qa);              // into the mailbox, never taken: watchdog
    if (!sa.got || !sa.err || sa.latency != kTimeout) b.fail("first stuck access not timed out");
    b.sys_reset(2);
    if (!b.wait_calib(16)) b.fail("calib_done did not return after reset");
    const uint64_t writes_before = b.side.mig.stats().writes;
    Req qb; qb.write = true; qb.addr = b_addr; qb.wdata = 0xBBBB6666;
    Rsp sb = b.access(qb);              // waits behind the first: watchdog again
    if (!sb.got || !sb.err || sb.latency != kTimeout) b.fail("second stuck access not timed out");
    b.start_ui_clock();
    b.ticks(200);
    if (b.side.mig.stats().writes != writes_before + 1)
        b.fail("controller carried out %llu writes, expected 1 (the first only)",
               (unsigned long long)(b.side.mig.stats().writes - writes_before));
    if (b.side.mig.word(a_addr & kDdrMask) != 0xAAAA5555) b.fail("the first write did not happen");
    if (b.side.mig.word(b_addr & kDdrMask) != b_before) b.fail("the abandoned second write happened");
    b.sys_reset(2);
    if (!b.wait_calib(16)) b.fail("no calibration after recovery");
    for (int i = 0; i < 100; ++i) b.access(random_req(b.rng(), st));
    if (b.timeouts != 2) b.fail("%llu timeouts, expected 2", (unsigned long long)b.timeouts);
    bool ok = b.finish(true);
    b.summary();
    return ok;
}

// Random traffic with rst at random moments, also in the middle of accesses at every stage.
bool t_sysreset(const Options &o) {
    MigUiConfig m = model_config(o);
    MigBench b(o, MigSide(m), "sysreset");
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
    MigBench b(o, MigSide(m), "uireset");
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
    if (t == "abandon") return t_abandon(o);
    if (t == "sysreset") return t_sysreset(o);
    if (t == "uireset") return t_uireset(o);
    std::printf("tb_ddr_bridge: unknown test %s\n", t.c_str());
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    Options o;
    if (!parse_options(argc, argv, o)) return 2;
    const char *all[] = {"basic", "lanes", "bounds", "alias", "random", "stream", "precal", "calfail",
                         "watchdog", "abandon", "sysreset", "uireset"};
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
