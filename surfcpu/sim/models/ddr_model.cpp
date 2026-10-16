/*
SurfCPU DDR model
--------------------
File: ddr_model.cpp Date: 10/15/26 (SurfCPU)
--------------------
See ddr_model.h.
*/
#include "ddr_model.h"

#include <stdexcept>

DdrModel::DdrModel(const DdrModelOptions &opt) : opt_(opt), rng_(opt.seed) {
    if (opt_.latency_min < 1 || opt_.latency_max < opt_.latency_min)
        throw std::invalid_argument("DdrModel: need 1 <= latency_min <= latency_max");
    if (opt_.ready_percent > 100) throw std::invalid_argument("DdrModel: ready_percent > 100");
    reset();
}

void DdrModel::reset() {
    cycle_ = 0;
    ready_ = rsp_valid_ = rsp_err_ = false;
    rsp_rdata_ = 0;
    calib_done_ = calib_fail_ = timeout_ = false;
    busy_ = false;
    held_ = false;
}

bool DdrModel::draw_ready() {
    return std::uniform_int_distribution<unsigned>(0, 99)(rng_) < opt_.ready_percent;
}

unsigned DdrModel::draw_latency() {
    return std::uniform_int_distribution<unsigned>(opt_.latency_min, opt_.latency_max)(rng_);
}

uint32_t DdrModel::peek(uint32_t addr) const {
    auto it = mem_.find(addr >> 12);
    return it == mem_.end() ? 0 : (*it->second)[(addr >> 2) & 1023];
}

void DdrModel::poke(uint32_t addr, uint32_t value, uint8_t be) {
    auto &page = mem_[addr >> 12];
    if (!page) {
        page = std::make_unique<Page>();
        page->fill(0);
    }
    uint32_t &w = (*page)[(addr >> 2) & 1023];
    for (int k = 0; k < 4; k++)
        if (be >> k & 1) w = (w & ~(0xFFu << (8 * k))) | (value & (0xFFu << (8 * k)));
}

void DdrModel::tick(bool req_valid, uint32_t req_addr, bool req_write, uint8_t req_be,
                    uint32_t req_wdata, bool req_fetch) {
    cycle_++;
    const bool was_done = calib_done_;     // what the SoC saw in the cycle that ends here

    // rule 2: a request offered and not accepted must come back unchanged
    if (held_ && (!req_valid || req_addr != held_addr_ || req_write != held_write_ ||
                  req_be != held_be_ || req_wdata != held_wdata_ || req_fetch != held_fetch_))
        protocol_errors++;
    held_ = req_valid && !ready_;
    held_addr_ = req_addr;
    held_write_ = req_write;
    held_be_ = req_be;
    held_wdata_ = req_wdata;
    held_fetch_ = req_fetch;

    // calibration ends with the calib_cycles-th edge after reset
    if (!calib_done_ && !calib_fail_ && opt_.calib_cycles >= 0 &&
        cycle_ >= uint64_t(opt_.calib_cycles)) {
        if (opt_.calib_fails) calib_fail_ = true;
        else calib_done_ = true;
    }

    timeout_ = false;
    rsp_valid_ = false;
    bool deliver = false;
    if (busy_) {
        if (--left_ == 0) deliver = true;
    } else if (req_valid && ready_) {
        // accepted at this edge
        if (!was_done) early_requests++;
        uint32_t a = req_addr & ~3u;
        bool inside = req_addr - opt_.base < opt_.size;
        pend_stall_ = inside && stalls_.count(a);
        pend_err_ = !inside || pend_stall_ || faults_.count(a);
        if (!inside) outside++;
        pend_rdata_ = pend_err_ ? 0 : peek(a);
        if (!pend_err_ && req_write) poke(a, req_wdata, req_be);
        if (req_fetch) fetches++;
        else if (req_write) writes++;
        else reads++;
        if (pend_stall_) stalls++;
        lat_ = pend_stall_ ? opt_.watchdog_cycles : draw_latency();
        if (lat_ <= 1) {
            deliver = true;
        } else {
            busy_ = true;
            left_ = lat_ - 1;
        }
    }

    if (deliver) {
        rsp_valid_ = true;
        rsp_rdata_ = pend_rdata_;
        rsp_err_ = pend_err_;
        timeout_ = pend_stall_;
        if (pend_err_) error_responses++;
        if (lat_ < latency_seen_min) latency_seen_min = lat_;
        if (lat_ > latency_seen_max) latency_seen_max = lat_;
        busy_ = false;
    }
    ready_ = !busy_ && draw_ready();
}
