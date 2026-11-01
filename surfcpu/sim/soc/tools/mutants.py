#!/usr/bin/env python3
"""SurfCPU soc mutation check
--------------------
File: mutants.py Date: 10/16/26 (SurfCPU)
--------------------
Checks the soc testbenches catch bugs (make test-soc-mutants): each mutant changes one line of
an RTL file the way a plausible bug would, the unit testbench that covers it is compiled with
the mutated copy (Icarus, under build/soc/mut/) and run on seed 1, and the run must fail. Exit
status 1 if any mutant survives. The RTL in the tree is never changed.

    python3 sim/soc/tools/mutants.py [--jobs N]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]          # surfcpu/
OUT = ROOT / "build" / "soc" / "mut"
BFM = ["sim/soc/bfm/surfbus_bfm.v", "sim/soc/bfm/surfbus_monitor.v"]
LIB = ["rtl/lib/surf_fifo.v", "rtl/lib/surf_sync.v"]
BUS = ["rtl/soc/surf_bus_err.v", "rtl/soc/surf_bus_demux.v", "rtl/soc/surf_ibus_decoder.v",
       "rtl/soc/surf_dbus_decoder.v", "rtl/soc/surf_bus_arbiter.v", "rtl/soc/surf_ddr_gate.v",
       "rtl/soc/surf_interconnect.v"]
MEM = ["rtl/soc/surf_bootrom.v", "rtl/soc/surf_scratch.v", "rtl/soc/surf_textram.v"]

# testbench name -> (top, testbench file, other sources, extra iverilog arguments)
BENCH = {
    "intc": ("tb_intc", "sim/soc/tb/tb_intc.v", ["rtl/peripherals/surf_intc.v"], []),
    "uart": ("tb_uart", "sim/soc/tb/tb_uart.v", ["rtl/peripherals/surf_uart.v"] + LIB, []),
    "gpio": ("tb_gpio", "sim/soc/tb/tb_gpio.v", ["rtl/peripherals/surf_gpio.v", LIB[1]], []),
    "timer": ("tb_timer", "sim/soc/tb/tb_timer.v", ["rtl/peripherals/surf_timer.v"], []),
    "sysinfo": ("tb_sysinfo", "sim/soc/tb/tb_sysinfo.v", ["rtl/peripherals/surf_sysinfo.v"], []),
    "mem": ("tb_mem", "sim/soc/tb/tb_mem.v", MEM, ['-Ptb_mem.ROMFILE="build/soc/rom_a.mem"']),
    "bus": ("tb_interconnect", "sim/soc/tb/tb_interconnect.v",
            BUS + ["sim/soc/bfm/surfbus_slave_model.v"], []),
    "bus_ddrc": ("tb_interconnect", "sim/soc/tb/tb_interconnect.v",
                 BUS + ["sim/soc/bfm/surfbus_slave_model.v"], ["-Ptb_interconnect.HAS_DDRC=1"]),
    "sysinfo_swinit": ("tb_sysinfo", "sim/soc/tb/tb_sysinfo.v", ["rtl/peripherals/surf_sysinfo.v"],
                       ["-Ptb_sysinfo.DDR=0", "-Ptb_sysinfo.SWINIT=1"]),
}

# (bench, file, original text, mutated text): the original must occur exactly once
MUTANTS = [
    ("intc", "rtl/peripherals/surf_intc.v",
     "for (i = 31; i >= 0; i = i - 1)", "for (i = 0; i <= 31; i = i + 1)"),
    ("intc", "rtl/peripherals/surf_intc.v",
     "rsp_err_q   <= s_req_valid && !(word_acc && known);", "rsp_err_q   <= s_req_valid && !known;"),
    ("uart", "rtl/peripherals/surf_uart.v",
     "wire rx_break_set = rx_bad && (rx_shift == 8'h00);", "wire rx_break_set = rx_bad;"),
    ("uart", "rtl/peripherals/surf_uart.v",
     "rx_state <= rx_s ? R_IDLE : R_WAIT;", "rx_state <= R_IDLE;"),
    ("uart", "rtl/peripherals/surf_uart.v",
     "wire rx_ovf_set   = rx_good && rx_full;", "wire rx_ovf_set   = 1'b0;"),
    ("uart", "rtl/peripherals/surf_uart.v",
     "wire       rx_pop = do_rd && hit_rx && !rx_empty;",
     "wire       rx_pop = s_req_valid && !s_req_write && hit_rx && !rx_empty;"),
    ("uart", "rtl/peripherals/surf_uart.v", "tx_left  <= 4'd9;", "tx_left  <= 4'd10;"),
    ("uart", "rtl/peripherals/surf_uart.v",
     "if (wr_div) div_q <= (wdiv < DIV_MIN) ? DIV_MIN : wdiv;", "if (wr_div) div_q <= wdiv;"),
    ("uart", "rtl/peripherals/surf_uart.v",
     "wire [15:0] rx_half    = {1'b0, div_q[15:1]} - 16'd2;", "wire [15:0] rx_half    = 16'd4;"),
    ("uart", "rtl/peripherals/surf_uart.v", "(tx_ie_q & tx_empty)", "(tx_ie_q & tx_idle)"),
    ("uart", "rtl/peripherals/surf_uart.v",
     "assign tx_pop   = (!tx_busy || tx_last) && !tx_empty;",
     "assign tx_pop   = !tx_busy && !tx_empty;"),
    ("gpio", "rtl/peripherals/surf_gpio.v",
     "edge_q   <= (edge_q & ~edge_clr) | btn_rise;", "edge_q   <= (edge_q | btn_rise) & ~edge_clr;"),
    ("gpio", "rtl/peripherals/surf_gpio.v",
     "end else if (btn_s[g] == state) begin\n                    cnt <= 4'd0;",
     "end else if (btn_s[g] == state) begin\n                    cnt <= cnt;"),
    ("gpio", "rtl/peripherals/surf_gpio.v",
     "wire [3:0] btn_rise = btn_db & ~btn_db_d;", "wire [3:0] btn_rise = btn_db ^ btn_db_d;"),
    ("gpio", "rtl/peripherals/surf_gpio.v",
     "rgb_out <= (pwm_cnt[10:8] == 3'b000) ? rgb_q : 12'h0;",
     "rgb_out <= (pwm_cnt[10:9] == 2'b00) ? rgb_q : 12'h0;"),
    ("gpio", "rtl/peripherals/surf_gpio.v",
     "localparam [3:0]   DB_LAST   = 4'd9;", "localparam [3:0]   DB_LAST   = 4'd8;"),
    ("gpio", "rtl/peripherals/surf_gpio.v",
     "assign irq         = |(edge_q & ie_q);", "assign irq         = |edge_q;"),
    ("timer", "rtl/peripherals/surf_timer.v",
     "assign irq_timer    = (mtime_q >= mtimecmp_q);", "assign irq_timer    = (mtime_q > mtimecmp_q);"),
    ("timer", "rtl/peripherals/surf_timer.v",
     "assign irq_timer    = (mtime_q >= mtimecmp_q);",
     "assign irq_timer    = ($signed(mtime_q) >= $signed(mtimecmp_q));"),
    ("timer", "rtl/peripherals/surf_timer.v",
     "else if (wr_thi) mtime_q[63:32] <= s_req_wdata;", "else if (wr_thi) mtime_q[31:0] <= s_req_wdata;"),
    ("sysinfo", "rtl/peripherals/surf_sysinfo.v",
     "ddr_status[SURF_SYSINFO_DDR_STATUS_TIMEOUT_LSB]    = timeout_q | ddr_timeout;",
     "ddr_status[SURF_SYSINFO_DDR_STATUS_TIMEOUT_LSB]    = ddr_timeout;"),
    ("sysinfo", "rtl/peripherals/surf_sysinfo.v",
     "SURF_SYSINFO_BUILD_FLAGS[11:2]: rd_mux = BUILD_FLAGS & FLAGS_DEFINED;",
     "SURF_SYSINFO_BUILD_FLAGS[11:2]: rd_mux = BUILD_FLAGS;"),
    ("mem", "rtl/soc/surf_bootrom.v", "b_e <= b_req_valid && b_req_write;", "b_e <= 1'b0;"),
    ("mem", "rtl/soc/surf_scratch.v",
     "if (b_req_be[2]) ram[bi][23:16] <= b_req_wdata[23:16];",
     "if (b_req_be[1]) ram[bi][23:16] <= b_req_wdata[23:16];"),
    ("mem", "rtl/soc/surf_scratch.v",
     "always @(posedge clk) if (a_req_valid) a_q <= ram[ai];",
     "always @(posedge clk) if (a_req_valid) a_q <= ram[bi];"),
    ("mem", "rtl/soc/surf_textram.v",
     "wire          we = s_req_valid && s_req_write && !s_req_fetch;",
     "wire          we = s_req_valid && s_req_write;"),
    ("mem", "rtl/soc/surf_textram.v",
     "always @(posedge pix_clk) b_q <= mem[pix_addr];",
     "always @(posedge pix_clk) b_q <= mem[pix_addr + 12'd1];"),
    ("bus", "rtl/soc/surf_dbus_decoder.v",
     "assign hit[`SURF_DT_DDR]     = (HAS_DDR != 0) && ((a & ~SURF_DDR_MASK) == SURF_DDR_BASE);",
     "assign hit[`SURF_DT_DDR]     = (HAS_DDR != 0) && ((a & ~(SURF_DDR_MASK >> 1)) == SURF_DDR_BASE);"),
    ("bus", "rtl/soc/surf_dbus_decoder.v",
     "assign hit[`SURF_DT_ROM]     = ((a & ~SURF_BOOTROM_MASK) == SURF_BOOTROM_BASE) && !wr;",
     "assign hit[`SURF_DT_ROM]     = ((a & ~SURF_BOOTROM_MASK) == SURF_BOOTROM_BASE);"),
    ("bus", "rtl/soc/surf_dbus_decoder.v",
     "assign hit[`SURF_DT_TEXTRAM] = ((a & ~SURF_TEXTRAM_MASK) == SURF_TEXTRAM_BASE) && nf;",
     "assign hit[`SURF_DT_TEXTRAM] = ((a & ~SURF_TEXTRAM_MASK) == SURF_TEXTRAM_BASE);"),
    ("bus", "rtl/soc/surf_ibus_decoder.v",
     "wire        rd     = !s_req_write;", "wire        rd     = 1'b1;"),
    ("bus", "rtl/soc/surf_bus_demux.v",
     "        else if (s_req_valid && s_req_ready) pend <= sel;\n"
     "        else if (s_rsp_valid)                pend <= {N{1'b0}};",
     "        else if (s_rsp_valid)                pend <= {N{1'b0}};\n"
     "        else if (s_req_valid && s_req_ready) pend <= sel;"),
    ("bus", "rtl/soc/surf_bus_arbiter.v",
     "            lock   <= m_req_valid;", "            lock   <= 1'b0;"),
    ("bus", "rtl/soc/surf_bus_arbiter.v",
     "    wire gnt_b = !busy && (lock ? lock_b  : b_req_valid);\n"
     "    wire gnt_a = !busy && (lock ? !lock_b : (!b_req_valid && a_req_valid));",
     "    wire gnt_b = !busy && (lock ? lock_b  : (!a_req_valid && b_req_valid));\n"
     "    wire gnt_a = !busy && (lock ? !lock_b : a_req_valid);"),
    ("bus", "rtl/soc/surf_ddr_gate.v", "    wire pass = ddr_ok || lock;", "    wire pass = ddr_ok;"),
    ("bus", "rtl/soc/surf_ddr_gate.v",
     "            err_q <= s_req_valid && !pass;", "            err_q <= 1'b0;"),
    ("bus", "rtl/soc/surf_interconnect.v",
     "                .clk(clk), .rst(rst), .ddr_ok(ddr_calib_done),",
     "                .clk(clk), .rst(rst), .ddr_ok(1'b1),"),
    ("bus", "rtl/soc/surf_dbus_decoder.v",
     "assign hit[`SURF_DT_DDRC]    = (HAS_DDRC != 0) && ((a & ~SURF_DDRC_MASK) == SURF_DDRC_BASE) && nf;",
     "assign hit[`SURF_DT_DDRC]    = ((a & ~SURF_DDRC_MASK) == SURF_DDRC_BASE) && nf;"),
    ("bus_ddrc", "rtl/soc/surf_dbus_decoder.v",
     "assign hit[`SURF_DT_DDRC]    = (HAS_DDRC != 0) && ((a & ~SURF_DDRC_MASK) == SURF_DDRC_BASE) && nf;",
     "assign hit[`SURF_DT_DDRC]    = (HAS_DDRC != 0) && ((a & ~SURF_DDRC_MASK) == SURF_DDRC_BASE);"),
    ("bus_ddrc", "rtl/soc/surf_dbus_decoder.v",
     "assign hit[`SURF_DT_DDRC]    = (HAS_DDRC != 0) && ((a & ~SURF_DDRC_MASK) == SURF_DDRC_BASE) && nf;",
     "assign hit[`SURF_DT_DDRC]    = (HAS_DDRC != 0) && ((a & ~(SURF_DDRC_MASK >> 1)) == SURF_DDRC_BASE) && nf;"),
    ("sysinfo_swinit", "rtl/peripherals/surf_sysinfo.v",
     "ddr_status[SURF_SYSINFO_DDR_STATUS_SWINIT_LSB]     = (DDR_SWINIT != 0) &&\n"
     "                                                             FEATURES[SURF_SYSINFO_FEATURES_DDR_LSB];",
     "ddr_status[SURF_SYSINFO_DDR_STATUS_SWINIT_LSB]     = (DDR_SWINIT != 0);"),
]


def run(i: int, mutant: tuple) -> tuple[int, bool, str]:
    bench, path, old, new = mutant
    top, tb, srcs, extra = BENCH[bench]
    text = (ROOT / path).read_text()
    if text.count(old) != 1:
        return i, False, f"pattern found {text.count(old)} times in {path}"
    d = OUT / f"m{i:02d}"
    d.mkdir(parents=True, exist_ok=True)
    mutated = d / Path(path).name
    mutated.write_text(text.replace(old, new))
    files = [str(mutated) if s == path else s for s in srcs]
    vvp = d / "tb.vvp"
    cmd = ["iverilog", "-g2005", "-Wno-timescale", "-Irtl/generated", "-Irtl/soc", "-s", top,
           "-o", str(vvp), *extra, tb, *files, *BFM]
    c = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if c.returncode != 0:
        return i, False, "does not compile: " + c.stderr.strip().splitlines()[0]
    r = subprocess.run(["vvp", "-n", str(vvp), "+seed=1"], cwd=ROOT, capture_output=True,
                       text=True, timeout=1800)
    caught = "PASS" not in r.stdout
    if caught:
        for f in d.iterdir():
            f.unlink()
        d.rmdir()
    return i, caught, "caught" if caught else "SURVIVED"


def main() -> int:
    ap = argparse.ArgumentParser(description="soc mutation check")
    ap.add_argument("--jobs", type=int, default=2)
    args = ap.parse_args()
    results = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for i, ok, what in ex.map(lambda p: run(*p), enumerate(MUTANTS)):
            results[i] = (ok, what)
    bad = 0
    for i, m in enumerate(MUTANTS):
        ok, what = results[i]
        first = m[3].splitlines()[0].strip()
        print(f"  mutant {i:2d} {m[0]:7s} {Path(m[1]).name:22s} {what:9s} {first[:60]}")
        bad += not ok
    per = {}
    for m in MUTANTS:
        per[m[0]] = per.get(m[0], 0) + 1
    summary = ", ".join(f"{k} {v}" for k, v in per.items())
    if bad:
        print(f"test-soc-mutants: {bad} of {len(MUTANTS)} mutants not caught")
        return 1
    print(f"test-soc-mutants: PASS (all {len(MUTANTS)} mutants caught: {summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
