/*
SurfCPU DDR model
--------------------
File: ddr_model.h Date: 10/15/26 (SurfCPU)
--------------------
A SurfBus slave behaving like surf_ddr_bridge (docs/MODULES.md section 7), for the SoC's DDR
master port in surfsim: sparse memory over the whole DDR window (256 MiB, zero until written,
4 KiB pages allocated on first write), byte enables, one access at a time. While idle it
raises req_ready on ready_percent of the cycles; an access is answered latency_min..
latency_max cycles after acceptance (at least 1), from a seeded generator. Calibration ends
calib_cycles after reset (calib_done, or calib_fail with calib_fails); a negative value never
ends. Error injection: stall_at(addr) makes accesses to that word hang until the watchdog
answers them with an error and pulses timeout (as the bridge's watchdog does, with
watchdog_cycles); error_at(addr) answers them with an error at once. An access outside the
window is answered with an error. The model also checks its master: a request before
calibration (the SoC must keep them away) or a request withdrawn or changed before
acceptance (docs/SURFBUS.md rule 2) is counted in early_requests / protocol_errors.
Plain C++17, no Verilator inside: call tick() at every rising edge of clk with the request
signals as they were just before it; the outputs then hold for the next cycle.
*/
#ifndef SURFCPU_DDR_MODEL_H
#define SURFCPU_DDR_MODEL_H

#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <set>
#include <unordered_map>

#include "surfcpu/soc.h"

struct DdrModelOptions {
    uint64_t seed = 1;
    unsigned latency_min = 2;        // cycles from acceptance to the response (>= 1)
    unsigned latency_max = 12;
    unsigned ready_percent = 70;     // chance req_ready is 1 in an idle cycle
    int64_t calib_cycles = 3000;     // cycles after reset until calibration ends (< 0: never)
    bool calib_fails = false;        // calibration ends with calib_fail instead of calib_done
    unsigned watchdog_cycles = 300;  // a stalled access is abandoned after this many cycles
    uint32_t base = SURF_DDR_BASE;   // the window the SoC sends here
    uint32_t size = SURF_DDR_SIZE;
};

class DdrModel {
public:
    explicit DdrModel(const DdrModelOptions &opt = DdrModelOptions());

    void reset();                    // reset: calibration starts again, the memory is kept
    // One rising edge of clk, with the request signals of the cycle that ends there.
    void tick(bool req_valid, uint32_t req_addr, bool req_write, uint8_t req_be,
              uint32_t req_wdata, bool req_fetch);

    // Outputs for the cycle after the last tick()
    bool req_ready() const { return ready_; }
    bool rsp_valid() const { return rsp_valid_; }
    uint32_t rsp_rdata() const { return rsp_rdata_; }
    bool rsp_err() const { return rsp_err_; }
    bool calib_done() const { return calib_done_; }
    bool calib_fail() const { return calib_fail_; }
    bool timeout() const { return timeout_; }

    // Back door and error injection
    uint32_t peek(uint32_t addr) const;
    void poke(uint32_t addr, uint32_t value, uint8_t be = 0xF);
    void stall_at(uint32_t addr) { stalls_.insert(addr & ~3u); }
    void error_at(uint32_t addr) { faults_.insert(addr & ~3u); }

    // Statistics and checks
    uint64_t cycles() const { return cycle_; }
    uint64_t reads = 0, writes = 0, fetches = 0, error_responses = 0, stalls = 0;
    uint64_t early_requests = 0, protocol_errors = 0, outside = 0;
    unsigned latency_seen_min = ~0u, latency_seen_max = 0;
    size_t pages() const { return mem_.size(); }

private:
    using Page = std::array<uint32_t, 1024>;
    bool draw_ready();
    unsigned draw_latency();

    DdrModelOptions opt_;
    std::mt19937_64 rng_;
    std::unordered_map<uint32_t, std::unique_ptr<Page>> mem_;
    std::set<uint32_t> stalls_, faults_;
    uint64_t cycle_ = 0;
    bool ready_ = false, rsp_valid_ = false, rsp_err_ = false;
    uint32_t rsp_rdata_ = 0;
    bool calib_done_ = false, calib_fail_ = false, timeout_ = false;
    // the access in progress
    bool busy_ = false;
    unsigned left_ = 0, lat_ = 0;
    uint32_t pend_rdata_ = 0;
    bool pend_err_ = false, pend_stall_ = false;
    // the request offered and not accepted at the last edge (rule 2)
    bool held_ = false;
    uint32_t held_addr_ = 0, held_wdata_ = 0;
    bool held_write_ = false, held_fetch_ = false;
    uint8_t held_be_ = 0;
};

#endif
