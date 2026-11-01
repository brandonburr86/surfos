/*
SurfCPU MIG user-interface model
--------------------
File: mig_ui_model.cpp Date: 10/15/26 (SurfCPU)
--------------------
The behaviour described in mig_ui_model.h.
*/
#include "mig_ui_model.h"

#include <algorithm>
#include <cstdio>

namespace surfddr {

MigUiModel::MigUiModel(const MigUiConfig &cfg)
    : cfg_(cfg), rng_(cfg.seed ^ 0x4d4947ULL), reset_left_(cfg.reset_cycles), calib_left_(cfg.calib_cycles) {
    out_.ui_clk_sync_rst = cfg.reset_cycles > 0;
    for (auto &b : out_.app_rd_data) b = static_cast<uint8_t>(rng_.next());
}

void MigUiModel::error(const std::string &what) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "mig ui cycle %llu: ", static_cast<unsigned long long>(cycle_));
    if (errors_.size() < 50) errors_.push_back(buf + what);
}

void MigUiModel::start_reset(uint32_t cycles) {
    pending_reset_ = std::max<uint32_t>(cycles, 1);
}

Beat MigUiModel::beat(uint32_t index) const {
    auto it = mem_.find(index);
    if (it != mem_.end()) return it->second;
    Beat b;
    for (int k = 0; k < kBeatBytes; ++k) b[k] = initial_byte(index * kBeatBytes + k);
    return b;
}

uint32_t MigUiModel::word(uint32_t offset) const {
    Beat b = beat(offset / kBeatBytes);
    uint32_t k = offset % kBeatBytes & ~3u, w = 0;
    for (int i = 3; i >= 0; --i) w = w << 8 | b[k + i];
    return w;
}

void MigUiModel::execute() {
    if (cmds_.empty()) return;
    Cmd &c = cmds_.front();
    uint32_t index = c.addr >> 3;                 // app_addr counts 16-bit words; 8 per beat
    if (c.write) {
        if (wdf_.empty()) {
            if (!c.late && cycle_ - c.when > cfg_.wdata_window) {
                error("write command without its data " + std::to_string(cfg_.wdata_window) +
                      " cycles after it was accepted (UG586 allows at most 2)");
                c.late = true;
            }
            return;
        }
        Wdf d = wdf_.front();
        wdf_.pop_front();
        auto it = mem_.find(index);
        if (it == mem_.end()) it = mem_.emplace(index, beat(index)).first;
        for (int k = 0; k < kBeatBytes; ++k)
            if (!(d.mask >> k & 1)) it->second[k] = d.data[k];
        ++stats_.writes;
        --outstanding_;
    } else {
        Rd r;
        r.data = beat(index);
        r.ready = std::max<uint64_t>(cycle_ + rng_.range(cfg_.rd_lat_min, cfg_.rd_lat_max), last_ready_ + 1);
        last_ready_ = r.ready;
        ++stats_.reads;
        if (drop_reads_) ++stats_.dropped_reads;  // never answered (and never finished)
        else rdq_.push_back(r);
    }
    cmds_.pop_front();
}

void MigUiModel::edge(const MigUiIn &in) {
    ++cycle_;
    ++stats_.cycles;
    const MigUiOut cur = out_;                    // what the user saw during the cycle just ended
    MigUiOut n;
    for (auto &b : n.app_rd_data) b = static_cast<uint8_t>(rng_.next());

    if (cur.ui_clk_sync_rst) {                    // in reset: the user side is ignored
        if (pending_reset_) { reset_left_ = std::max(reset_left_, pending_reset_); pending_reset_ = 0; }
        if (reset_left_ > 0) --reset_left_;
        n.ui_clk_sync_rst = reset_left_ > 0;
        if (!n.ui_clk_sync_rst) calib_left_ = cfg_.calib_cycles;
        prev_in_ = in;
        prev_out_ = cur;
        out_ = n;
        return;
    }

    // ---- the user's side of the interface
    if (in.app_sr_req || in.app_ref_req || in.app_zq_req)
        error("app_sr_req/app_ref_req/app_zq_req asserted");
    if (prev_in_.app_en && !prev_out_.app_rdy && !prev_out_.ui_clk_sync_rst &&
        (!in.app_en || in.app_cmd != prev_in_.app_cmd || in.app_addr != prev_in_.app_addr))
        error("app_en/app_cmd/app_addr changed before app_rdy took the command");
    if (prev_in_.app_wdf_wren && !prev_out_.app_wdf_rdy && !prev_out_.ui_clk_sync_rst &&
        (!in.app_wdf_wren || in.app_wdf_data != prev_in_.app_wdf_data ||
         in.app_wdf_mask != prev_in_.app_wdf_mask || in.app_wdf_end != prev_in_.app_wdf_end))
        error("app_wdf_* changed before app_wdf_rdy took the data");
    if (in.app_wdf_wren != in.app_wdf_end)
        error("app_wdf_end must come with every beat (one beat is one BL8 burst at 4:1)");
    if (in.app_en && !cur.init_calib_complete)
        error("app_en before init_calib_complete");

    const bool cmd_x = in.app_en && cur.app_rdy;
    const bool wdf_x = in.app_wdf_wren && cur.app_wdf_rdy;
    if (in.app_en && !cur.app_rdy) ++stats_.cmd_stalls;
    if (in.app_wdf_wren && !cur.app_wdf_rdy) ++stats_.wdf_stalls;
    if (wdf_x) wdf_.push_back({in.app_wdf_data, in.app_wdf_mask});
    if (cmd_x) {
        if (in.app_cmd != 0 && in.app_cmd != 1) error("app_cmd " + std::to_string(in.app_cmd) + " (only write 0, read 1)");
        if (in.app_addr & 7) error("app_addr not aligned to a BL8 burst");
        if (in.app_addr >> (cfg_.addr_width - 1)) error("app_addr rank bit set (one rank)");
        if (static_cast<uint64_t>(in.app_addr) * 2 >= cfg_.mem_bytes) error("app_addr beyond the memory");
        cmds_.push_back({in.app_cmd == 0, in.app_addr, cycle_, false});
        ++outstanding_;
        stats_.max_outstanding = std::max(stats_.max_outstanding, outstanding_);
    }

    execute();

    // ---- outputs for the next cycle
    if (calib_left_ > 0) --calib_left_;
    n.init_calib_complete = cur.init_calib_complete || (calib_left_ == 0 && !cfg_.calib_never);
    const bool cal = n.init_calib_complete;
    const bool refreshing = cfg_.refresh_period && cycle_ % cfg_.refresh_period < cfg_.refresh_len;
    n.app_rdy = cal && !stuck_rdy_ && !refreshing && cmds_.size() < cfg_.cmd_depth && rng_.percent(cfg_.rdy_pct);
    n.app_wdf_rdy = cal && !stuck_wdf_ && wdf_.size() < cfg_.wdf_depth && rng_.percent(cfg_.wdf_rdy_pct);
    if (!rdq_.empty() && rdq_.front().ready <= cycle_) {
        n.app_rd_data_valid = true;
        n.app_rd_data_end = true;
        n.app_rd_data = rdq_.front().data;
        rdq_.pop_front();
        --outstanding_;
    }
    if (pending_reset_) {
        // A MIG reset starts after this edge (whose transfers were honoured): everything still
        // in flight is lost, read data not yet returned included; the memory keeps its contents.
        for (const Cmd &c : cmds_) stats_.lost_writes += c.write;
        cmds_.clear();
        wdf_.clear();
        rdq_.clear();
        outstanding_ = 0;
        reset_left_ = pending_reset_;
        pending_reset_ = 0;
        ++stats_.resets;
        MigUiOut r;
        r.ui_clk_sync_rst = true;                 // everything else 0
        r.app_rd_data = n.app_rd_data;
        n = r;
    }
    prev_in_ = in;
    prev_out_ = cur;
    out_ = n;
}

}  // namespace surfddr
