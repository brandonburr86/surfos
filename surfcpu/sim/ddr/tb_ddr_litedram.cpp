/*
SurfCPU DDR bridge with LiteDRAM testbench
--------------------
File: tb_ddr_litedram.cpp Date: 10/16/26 (SurfCPU)
--------------------
Verilator testbench for tb_litedram_top.v: surf_ddr_bridge and surf_ddr_litedram_adapter in
front of the LiteDRAM controller generated from the board's ip/litedram/arty.yml with --sim (its
real controller, crossbar and native port with the bitstream's settings; LiteDRAM's behavioural
DRAM model in place of the PHY, all memory zero at the start). The same bench as tb_ddr_bridge.cpp (surfbus_bench.h): every response is
checked against a reference memory, with the SURFBUS.md rules checked on every cycle. The
default clocks are the open flow's: sys_clk 25 MHz, LiteDRAM's clock 100 MHz.

usage: tb_ddr_litedram [--test NAME|all] [--seed N] [--count N] [--sys-ns P] [--ui-ns P] [--jitter F]
tests: basic lanes bounds alias random stream precal calerror sysreset all
*/
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Vtb_litedram_top.h"

#include "surfbus_bench.h"

using namespace surfddr;

namespace {

// The controller lives inside the top; the only inputs are "calibration finished" and whether
// it failed (the BIOS's report on the board).
struct LiteSide {
    bool init_go = true;
    bool init_fail = false;
    void init(Vtb_litedram_top &t) { t.init_go = init_go; t.init_fail = init_fail; }
    void pre_ui_rise(Vtb_litedram_top &) {}
    void post_ui_rise(Vtb_litedram_top &t) { t.init_go = init_go; t.init_fail = init_fail; }
    uint64_t done() const { return 0; }
    bool idle() const { return true; }
    bool observable() const { return false; }
    void ui_reset(uint32_t) {}
    template <class B> void finish(B &) const {}
    uint64_t reads() const { return 0; }
    uint64_t writes() const { return 0; }
    const char *tag() const { return "tb_ddr_litedram"; }
};

using LiteBench = Bench<Vtb_litedram_top, LiteSide>;

constexpr uint64_t kCalibWait = 2000;   // sys cycles for the init_done write to come through
constexpr uint64_t kCalibLimit = 20000; // tb_litedram_top's CALIB_TIMEOUT_CYCLES

bool t_basic(const Options &o) {
    LiteBench b(o, LiteSide(), "basic", true, 0);
    if (!b.wait_calib(kCalibWait)) b.fail("no calibration");
    b.read(0x80000000);                 // the DRAM model starts at zero
    b.write(0x80000000, 0xF, 0x11223344);
    if (b.read(0x80000000) != 0x11223344) b.fail("readback");
    b.write(0x8FFFFFFC, 0xF, 0xCAFEF00D);
    b.read(0x8FFFFFFC);
    bool ok = b.finish();
    b.summary();
    return ok;
}

bool t_lanes(const Options &o) {
    LiteBench b(o, LiteSide(), "lanes", true, 0);
    if (!b.wait_calib(kCalibWait)) b.fail("no calibration");
    const uint32_t beats[] = {0x80000000, 0x81234560, 0x8ABCDEF0, 0x8FFFFFF0};
    for (uint32_t base : beats)
        for (int lane = 0; lane < 4; ++lane)
            for (uint8_t be : kBe) {
                b.write(base + 4 * lane + be_offset(be), be, b.rng().u32(), static_cast<int>(b.rng().range(0, 2)));
                for (int w = 0; w < 4; ++w) b.read(base + 4 * w, kBe[(w + lane) % 7], 0);
            }
    bool ok = b.finish();
    b.summary();
    return ok;
}

bool t_bounds(const Options &o) {
    LiteBench b(o, LiteSide(), "bounds", true, 0);
    if (!b.wait_calib(kCalibWait)) b.fail("no calibration");
    const uint32_t edges[] = {0x0, 0x10, 0x800, 0x1000, 0x2000, 0x10000, 0x100000, 0x1000000,
                              0x2000000, 0x4000000, 0x6000000, 0x8000000, 0xE000000, 0xFFFFFF0};
    std::vector<uint32_t> addrs;
    for (uint32_t e : edges)
        for (int d = -16; d < 16; d += 4) {
            const int64_t a = int64_t(e) + d;
            if (a >= 0 && a <= int64_t(kDdrMask & ~3u)) addrs.push_back(kDdrBase | uint32_t(a));
        }
    for (uint32_t a : addrs) b.read(a);
    for (uint32_t a : addrs) b.write(a, 0xF, b.rng().u32());
    for (uint32_t a : addrs) b.read(a);
    bool ok = b.finish();
    b.summary();
    return ok;
}

bool t_alias(const Options &o) {
    LiteBench b(o, LiteSide(), "alias", true, 0);
    if (!b.wait_calib(kCalibWait)) b.fail("no calibration");
    std::vector<uint32_t> addrs;
    addrs.push_back(kDdrBase);
    for (int k = 2; k < 28; ++k) addrs.push_back(kDdrBase | 1u << k);
    addrs.push_back(kDdrBase | (kDdrMask & ~3u));
    for (int k = 2; k < 28; ++k) addrs.push_back(kDdrBase | ((kDdrMask & ~3u) ^ 1u << k));
    for (size_t i = 0; i < addrs.size(); ++i) b.write(addrs[i], 0xF, 0xA5000000u | uint32_t(i) << 8 | uint32_t(i));
    for (size_t i = 0; i < addrs.size(); ++i)
        if (b.read(addrs[i]) != (0xA5000000u | uint32_t(i) << 8 | uint32_t(i))) b.fail("alias at %08x", addrs[i]);
    bool ok = b.finish();
    b.summary();
    return ok;
}

bool t_random(const Options &o) {
    LiteBench b(o, LiteSide(), "random", true, 0);
    if (!b.wait_calib(kCalibWait)) b.fail("no calibration");
    const uint64_t n = o.count ? o.count : 10000;
    uint32_t stream = 0x0400000;
    for (uint64_t i = 0; i < n && b.failures == 0; ++i) {
        Req q = random_req(b.rng(), stream);
        Rsp s = b.access(q, b.rng().percent(50) ? 0 : static_cast<int>(b.rng().range(1, 4)));
        if (!s.got || s.err) b.fail("access %llu failed", (unsigned long long)i);
    }
    bool ok = b.finish();
    b.summary();
    return ok;
}

bool t_stream(const Options &o) {
    LiteBench b(o, LiteSide(), "stream", true, 0);
    if (!b.wait_calib(kCalibWait)) b.fail("no calibration");
    const uint32_t n = o.count ? static_cast<uint32_t>(o.count) : 1024;
    uint64_t c0 = b.cycle();
    for (uint32_t i = 0; i < n; ++i) b.write(0x80200000 + 4 * i, 0xF, 0x5A000000u ^ i * 2654435761u);
    const uint64_t wr = b.cycle() - c0;
    c0 = b.cycle();
    for (uint32_t i = 0; i < n; ++i)
        if (b.read(0x80200000 + 4 * i) != (0x5A000000u ^ i * 2654435761u)) b.fail("stream word %u", i);
    const uint64_t rd = b.cycle() - c0;
    bool ok = b.finish();
    char extra[96];
    std::snprintf(extra, sizeof extra, "sys_cycles_per_write=%.2f per_read=%.2f", double(wr) / n, double(rd) / n);
    b.summary(extra);
    return ok;
}

// Before init_done the bridge refuses every access at once; then the DDR becomes usable.
bool t_precal(const Options &o) {
    LiteSide side;
    side.init_go = false;
    LiteBench b(o, side, "precal", true, 0);
    uint32_t stream = 0;
    for (int i = 0; i < 100; ++i) {
        Rsp s = b.access(random_req(b.rng(), stream), static_cast<int>(b.rng().range(0, 3)));
        if (!s.got || !s.err || s.latency != 0) b.fail("access before init_done not refused at once");
        if (b.calib_done()) b.fail("calib_done before init_done");
    }
    b.side.init_go = true;
    if (!b.wait_calib(kCalibWait)) b.fail("no calibration after init_done");
    for (int i = 0; i < 200; ++i) {
        Rsp s = b.access(random_req(b.rng(), stream));
        if (!s.got || s.err) b.fail("access after init_done failed");
    }
    bool ok = b.finish();
    b.summary();
    return ok;
}

// The BIOS's failure report (init_error, then init_done, as liblitedram writes them when its
// memory test fails on the board): the bridge never calibrates and refuses every access at once,
// the controller's native port stays blocked, and calib_fail comes when the bridge's limit runs
// out (CALIB_TIMEOUT_CYCLES after reset), with calib_done 0 for good.
bool t_calerror(const Options &o) {
    LiteSide side;
    side.init_fail = true;
    LiteBench b(o, side, "calerror", true, 0);
    uint32_t stream = 0;
    for (int i = 0; i < 200; ++i) {
        Rsp s = b.access(random_req(b.rng(), stream), static_cast<int>(b.rng().range(0, 40)));
        if (!s.got || !s.err || s.latency != 0) b.fail("access after init_error not refused at once");
        if (b.calib_done()) b.fail("calib_done after init_error");
        if (b.failures) break;
    }
    while (!b.calib_fail() && b.cycle() < kCalibLimit + 100) b.tick();
    const uint64_t seen = b.cycle();
    if (!b.calib_fail()) b.fail("calib_fail not set %llu sys cycles after reset", (unsigned long long)seen);
    else if (seen < kCalibLimit) b.fail("calib_fail before the limit (sys cycle %llu)", (unsigned long long)seen);
    for (int i = 0; i < 50; ++i) {
        Rsp s = b.access(random_req(b.rng(), stream));
        if (!s.got || !s.err || s.latency != 0) b.fail("access after calib_fail not refused at once");
        if (b.calib_done() || !b.calib_fail()) b.fail("calib_done/calib_fail changed after the failure");
    }
    bool ok = b.finish();
    char extra[64];
    std::snprintf(extra, sizeof extra, "calib_fail_seen_at_cycle=%llu", (unsigned long long)seen);
    b.summary(extra);
    return ok;
}

// Random traffic with rst at random moments, in the middle of accesses too.
bool t_sysreset(const Options &o) {
    LiteBench b(o, LiteSide(), "sysreset", true, 0);
    if (!b.wait_calib(kCalibWait)) b.fail("no calibration");
    const uint64_t n = o.count ? o.count : 5000;
    uint32_t stream = 0x0800000;
    for (uint64_t i = 0; i < n && b.failures == 0; ++i) {
        Req q = random_req(b.rng(), stream);
        const int gap = b.rng().percent(60) ? 0 : static_cast<int>(b.rng().range(1, 3));
        if (b.rng().percent(8)) {
            const uint64_t typ = b.lat_n ? b.lat_sum / b.lat_n + 2 : 20;
            Rsp s = b.access(q, gap, static_cast<int>(b.rng().range(0, typ + typ / 2)),
                             static_cast<int>(b.rng().range(1, 5)));
            if (s.got && s.err) b.fail("error response around a reset");
            b.wait_reset_done();
            b.wait_calib(64);
        } else {
            Rsp s = b.access(q, gap);
            if (!s.got || s.err) b.fail("access %llu failed", (unsigned long long)i);
        }
    }
    for (int i = 0; i < 64; ++i) { Req r; r.addr = 0x80123450 + 4 * i; b.access(r); }
    bool ok = b.finish();
    char extra[64];
    std::snprintf(extra, sizeof extra, "resets=%llu maybes=%llu", (unsigned long long)b.resets_applied,
                  (unsigned long long)b.ref.maybes);
    b.summary(extra);
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
    if (t == "calerror") return t_calerror(o);
    if (t == "sysreset") return t_sysreset(o);
    std::printf("tb_ddr_litedram: unknown test %s\n", t.c_str());
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    Options o;
    o.ui_ns = 10.0;                     // LiteDRAM's 100 MHz clock unless told otherwise
    if (!parse_options(argc, argv, o)) return 2;
    const char *all[] = {"basic", "lanes", "bounds", "alias", "random", "stream", "precal", "calerror", "sysreset"};
    bool ok = true;
    if (o.test == "all") {
        for (const char *t : all) ok = run(t, o) && ok;
    } else {
        ok = run(o.test, o);
    }
    std::printf("tb_ddr_litedram: %s (LiteDRAM 2026.08 controller, ip/litedram/arty.yml, with its DRAM model)\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
