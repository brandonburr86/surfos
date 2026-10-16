/*
SurfCPU MIG user-interface model
--------------------
File: mig_ui_model.h Date: 10/15/26 (SurfCPU)
--------------------
A behavioural model of the AMD MIG 7-series native user interface (app_*) as the Arty A7's
DDR3L controller presents it (UG586; 4:1 PHY, x16 memory, BL8): one 128-bit beat per
command, app_addr in 16-bit words with the rank as its top bit. It is clocked by its own
ui_clk (the caller calls edge() at every rising edge, with the user's signals as they were
just before it) and has:

* ui_clk_sync_rst after power-up or start_reset(), then init_calib_complete after a
  calibration delay (or never), with app_rdy and app_wdf_rdy low until then;
* back-pressure: app_rdy and app_wdf_rdy drop at random (seeded) and when the command queue
  or the write-data FIFO is full, and app_rdy also during periodic refresh stalls;
* writes and reads carried out in command order (strict ordering), each write with the
  oldest write data; read data after a random latency, in order, one beat per read, with
  random junk on app_rd_data while app_rd_data_valid is low;
* 256 MiB of sparse memory whose never-written bytes are initial_byte(offset);
* fault injection for watchdog tests: app_rdy stuck low, app_wdf_rdy stuck low, read data
  that never returns;
* checks of the user's side of the interface (errors()): app_en, app_cmd and app_addr held
  until app_rdy, write data and mask held until app_wdf_rdy, app_wdf_end with every beat,
  only read and write commands, aligned in-range addresses with rank 0, no command before
  calibration, write data no later than wdata_window cycles after its command, the
  self-refresh/refresh/ZQ requests left at 0.

Plain C++17; no Verilator types (the testbench converts).
*/
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "ddr_rng.h"

namespace surfddr {

constexpr int kBeatBytes = 16;                 // 128 bits
using Beat = std::array<uint8_t, kBeatBytes>;

struct MigUiConfig {
    uint64_t seed            = 1;
    uint32_t reset_cycles    = 16;     // ui_clk_sync_rst at power-up
    uint32_t calib_cycles    = 300;    // from leaving reset to init_calib_complete
    bool     calib_never     = false;
    int      rdy_pct         = 75;     // chance that app_rdy is 1 in a cycle where it may be
    int      wdf_rdy_pct     = 75;
    uint32_t rd_lat_min      = 12;     // read command accepted -> app_rd_data_valid, ui cycles
    uint32_t rd_lat_max      = 32;
    uint32_t cmd_depth       = 4;      // accepted commands not yet carried out
    uint32_t wdf_depth       = 4;      // write-data FIFO entries
    uint32_t wdata_window    = 2;      // UG586: write data at most 2 cycles after its command
    uint32_t refresh_period  = 634;    // tREFI 7.8 us at 81.25 MHz; 0 = no refresh stalls
    uint32_t refresh_len     = 14;     // tRFC 160 ns, rounded up
    uint32_t addr_width      = 28;     // app_addr bits (rank + bank + row + column)
    uint64_t mem_bytes       = 256ull << 20;
};

struct MigUiIn {                       // the user's signals just before a ui_clk rising edge
    bool     app_en       = false;
    uint8_t  app_cmd      = 0;
    uint32_t app_addr     = 0;
    bool     app_wdf_wren = false;
    bool     app_wdf_end  = false;
    Beat     app_wdf_data{};
    uint16_t app_wdf_mask = 0;
    bool     app_sr_req   = false;
    bool     app_ref_req  = false;
    bool     app_zq_req   = false;
};

struct MigUiOut {                      // the model's signals after the edge
    bool ui_clk_sync_rst     = false;
    bool init_calib_complete = false;
    bool app_rdy             = false;
    bool app_wdf_rdy         = false;
    bool app_rd_data_valid   = false;
    bool app_rd_data_end     = false;
    Beat app_rd_data{};
};

struct MigUiStats {
    uint64_t cycles = 0, reads = 0, writes = 0, cmd_stalls = 0, wdf_stalls = 0;
    uint64_t max_outstanding = 0;      // commands accepted but not yet finished (reads: data out)
    uint64_t resets = 0, dropped_reads = 0, lost_writes = 0;
};

class MigUiModel {
public:
    explicit MigUiModel(const MigUiConfig &cfg);

    void edge(const MigUiIn &in);                 // one rising edge of ui_clk
    const MigUiOut &out() const { return out_; }

    // Controls (take effect from the next edge).
    void start_reset(uint32_t cycles);            // MIG reset: queues flushed, memory kept
    void set_calib_never(bool never) { cfg_.calib_never = never; }
    void set_stuck_rdy(bool on) { stuck_rdy_ = on; }
    void set_stuck_wdf(bool on) { stuck_wdf_ = on; }
    void set_drop_reads(bool on) { drop_reads_ = on; }

    // Back door and results.
    Beat beat(uint32_t index) const;              // index = byte offset / 16
    uint32_t word(uint32_t offset) const;         // little-endian word at a byte offset
    bool idle() const { return cmds_.empty() && wdf_.empty() && rdq_.empty(); }
    const MigUiStats &stats() const { return stats_; }
    const std::vector<std::string> &errors() const { return errors_; }

private:
    struct Cmd { bool write; uint32_t addr; uint64_t when; bool late; };
    struct Wdf { Beat data; uint16_t mask; };
    struct Rd  { Beat data; uint64_t ready; };

    void error(const std::string &what);
    void execute();                               // carry out the oldest command if it can

    MigUiConfig cfg_;
    Rng rng_;
    MigUiOut out_;
    MigUiStats stats_;
    std::vector<std::string> errors_;

    uint64_t cycle_ = 0;
    uint32_t reset_left_;
    uint32_t pending_reset_ = 0;
    uint32_t calib_left_;
    bool stuck_rdy_ = false, stuck_wdf_ = false, drop_reads_ = false;

    std::deque<Cmd> cmds_;
    std::deque<Wdf> wdf_;
    std::deque<Rd>  rdq_;
    uint64_t last_ready_ = 0;
    uint64_t outstanding_ = 0;

    MigUiIn prev_in_{};                           // for the hold checks
    MigUiOut prev_out_{};

    std::unordered_map<uint32_t, Beat> mem_;
};

}  // namespace surfddr
