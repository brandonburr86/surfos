#!/usr/bin/env python3
"""
SurfCPU DDR bridge mutation check
--------------------
File: ddr_mutants.py Date: 10/15/26 (SurfCPU)
--------------------
Measures whether the DDR bridge tests can tell a broken bridge from a working one: each
mutant is a copy of the RTL with one deliberate bug (a wrong lane, an inverted mask, a lost
address bit, no id check, a watchdog off by one, ...). The script builds the fast testbench
against each copy and runs every test with one seed; a mutant must make it fail ("killed").
Mutants of the mailbox are also run against its own testbench (tb_ddr_mailbox.v), because the
bridge never asks it to do some things (take an empty box) that it must still get right. Two
mutants remove a synchronizer stage: metastability is not simulated, so they are expected to
survive, and say so (the clock crossings rest on review and on the timing constraints of
docs/DDR.md instead). Equivalent mutants (no behaviour change, such as dropping the busy term
of the response handling, which only runs while busy, or rs_got, since rs_q always holds the
latest response and ids are not reused while one is outstanding) are left out.

    python3 sim/ddr/ddr_mutants.py [--only NAME ...] [--jobs N]

Run from surfcpu/ inside the toolchain container (make test-ddr-mutants). Writes
build/ddr/mutants/<name>/ and prints one line per mutant; exit 1 if a mutant that should be
killed survives.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent      # surfcpu/
BRIDGE = "rtl/soc/ddr/surf_ddr_bridge.v"
MAILBOX = "rtl/soc/ddr/surf_ddr_mailbox.v"
ADAPTER = "rtl/soc/ddr/surf_ddr_litedram_adapter.v"
LITEDRAM_SIM = "build/litedram/arty_sim/gateware/litedram_core.v"   # make litedram-gen

# name, file, old text, new text, expected to survive (simulation cannot see it)
MUTANTS = [
    ("lane-select", BRIDGE, "app_rd_data[{u_lane, 5'b00000} +: 32]", "app_rd_data[31:0]", False),
    ("mask-polarity", BRIDGE, "assign app_wdf_mask = ~(", "assign app_wdf_mask = (", False),
    ("mask-shift", BRIDGE, "u_be} << {u_lane, 2'b00});", "u_be} << {u_lane, 1'b0});", False),
    ("addr-shift", BRIDGE, "u_beat} << (BEAT_LSB - UNIT_LSB);", "u_beat} << (BEAT_LSB - UNIT_LSB - 1);", False),
    ("addr-bit27", BRIDGE, "s_req_addr[OFFSET_BITS-1:2], s_req_wdata}", "1'b0, s_req_addr[OFFSET_BITS-2:2], s_req_wdata}", False),
    ("cmd-swap", BRIDGE, "u_write ? 3'b000 : 3'b001", "u_write ? 3'b001 : 3'b000", False),
    ("no-id-check", BRIDGE, "rs_mine = rs_got && busy && sent && (rs_q[RS_W-1 -: 4] == cur_id);",
     "rs_mine = rs_got && busy && sent;", False),
    ("id-reset", BRIDGE, "        if (!rst && accept && usable)\n            next_id <= next_id + 1'b1;",
     "        if (rst) next_id <= 4'd0;\n        else if (accept && usable)\n            next_id <= next_id + 1'b1;", False),
    ("err-bit", BRIDGE, "rsp_err_q   <= rs_q[32];", "rsp_err_q   <= rs_q[31];", False),
    ("watchdog-early", BRIDGE, "TO_LAST32  = TIMEOUT_CYCLES - 1;", "TO_LAST32  = TIMEOUT_CYCLES - 2;", False),
    ("no-fail-stop", BRIDGE, "usable  = cal_done_q && !timeout_q;", "usable  = cal_done_q;", False),
    ("no-precal-refusal", BRIDGE, "usable  = cal_done_q && !timeout_q;", "usable  = !timeout_q;", False),
    ("calib-fail-ignored", BRIDGE, "cal_done_q <= calib_seen && !cal_fail_q;", "cal_done_q <= calib_seen;", False),
    ("calib-done-latched", BRIDGE, "cal_done_q <= calib_seen && !cal_fail_q;",
     "cal_done_q <= (calib_seen || cal_done_q) && !cal_fail_q;", False),
    ("no-calib-limit", BRIDGE, "if (cal_count == CAL_LAST) cal_fail_q <= 1'b1;", "if (1'b0) cal_fail_q <= 1'b1;", False),
    ("wdf-rdy-ignored", BRIDGE, "if (app_wdf_rdy) begin", "if (1'b1) begin", False),
    ("wcmd-rdy-ignored", BRIDGE, "U_WCMD:\n                    if (app_rdy) begin", "U_WCMD:\n                    if (1'b1) begin", False),
    ("rcmd-rdy-ignored", BRIDGE, "U_RCMD:\n                    if (app_rdy) begin", "U_RCMD:\n                    if (1'b1) begin", False),
    ("ui-no-refusal", BRIDGE, "if (!init_calib_complete) begin", "if (1'b0) begin", False),
    ("no-take", BRIDGE, "assign rq_take = !ui_rst && u_state == U_IDLE && rq_valid;", "assign rq_take = 1'b0;", False),
    ("rsp-in-reset", BRIDGE, "assign s_rsp_valid = rsp_valid_q && !rst;", "assign s_rsp_valid = rsp_valid_q;", False),
    ("box-send-unchecked", MAILBOX, "if (src_send && src_ready) begin", "if (src_send) begin", False),
    ("box-take-unchecked", MAILBOX, "if (dst_take && dst_valid)", "if (dst_take)", False),
    ("box-no-req-sync", MAILBOX, "assign dst_valid = (req_sync[1] != ack);", "assign dst_valid = (req != ack);", True),
    ("box-no-ack-sync", MAILBOX, "assign src_ready = (req == ack_sync[1]);", "assign src_ready = (req == ack);", True),
    # the LiteDRAM adapter, run against LiteDRAM's controller (tb_ddr_litedram)
    ("lite-we-polarity", ADAPTER, ".wr_data({~app_wdf_mask, app_wdf_data})", ".wr_data({app_wdf_mask, app_wdf_data})", False),
    ("lite-addr-shift", ADAPTER, "app_addr[BURST_BITS +: CMD_ADDR_WIDTH]", "app_addr[BURST_BITS-1 +: CMD_ADDR_WIDTH]", False),
    ("lite-cmd-we", ADAPTER, "assign cmd_we    = (app_cmd == 3'b000);", "assign cmd_we    = (app_cmd == 3'b001);", False),
    ("lite-no-buffer", ADAPTER, "assign app_wdf_rdy = !wf_full;", "assign app_wdf_rdy = !wf_full && wdata_ready;", False),
    ("lite-calib", ADAPTER, "assign init_calib_complete = init_done && !init_error;", "assign init_calib_complete = 1'b1;", False),
]

VFLAGS = ["--cc", "--exe", "--build", "-O2", "--x-assign", "unique", "--x-initial", "unique",
          "-Wno-fatal", "-Wno-lint", "-Wno-style", "-Irtl/generated", "-Irtl/lib",
          "--top-module", "surf_ddr_bridge", "-GTIMEOUT_CYCLES=512", "-GCALIB_TIMEOUT_CYCLES=20000"]
CFLAGS = "-std=c++17 -O1 -DTB_TIMEOUT_CYCLES=512 -DTB_CALIB_TIMEOUT_CYCLES=20000 -I{}".format(ROOT / "sim/ddr")
TB = ["sim/ddr/tb_ddr_bridge.cpp", "sim/ddr/mig_ui_model.cpp"]


def run_mutant(name: str, path: str, old: str, new: str, jobs: int) -> tuple[bool, str]:
    out = ROOT / "build/ddr/mutants" / name
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for f in (BRIDGE, MAILBOX, ADAPTER):
        text = (ROOT / f).read_text()
        if f == path:
            if text.count(old) != 1:
                return False, "mutation text found {} times in {}".format(text.count(old), f)
            text = text.replace(old, new)
        (out / Path(f).name).write_text(text)
    exe = out / "tb"
    if path == ADAPTER:                 # the LiteDRAM bench
        if not (ROOT / LITEDRAM_SIM).exists():
            return False, "not run: no LiteDRAM simulation core (make litedram-gen)"
        cmd = (["verilator", "--cc", "--exe", "--build", "-O2", "-Wno-fatal", "-Wno-lint", "-Wno-style",
                "-Irtl/generated", "-Irtl/lib", "--top-module", "tb_litedram_top", "-j", str(jobs),
                "--Mdir", str(out / "obj"), "-CFLAGS", CFLAGS, "-o", str(exe), "sim/ddr/tb_litedram_top.v",
                LITEDRAM_SIM, str(out / "surf_ddr_bridge.v"), str(out / "surf_ddr_mailbox.v"),
                str(out / "surf_ddr_litedram_adapter.v"), "rtl/lib/surf_sync.v", "rtl/lib/surf_fifo.v",
                "sim/ddr/tb_ddr_litedram.cpp"])
    else:
        cmd = (["verilator"] + VFLAGS + ["-j", str(jobs), "--Mdir", str(out / "obj"), "-CFLAGS", CFLAGS,
                                        "-o", str(exe), str(out / "surf_ddr_bridge.v"), str(out / "surf_ddr_mailbox.v"),
                                        "rtl/lib/surf_sync.v"] + TB)
    b = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if b.returncode != 0:
        return True, "killed (does not build)"
    if path == MAILBOX:                 # the mailbox's own test first
        vvp = out / "tb_ddr_mailbox.vvp"
        c = subprocess.run(["iverilog", "-g2005", "-o", str(vvp), "sim/ddr/tb_ddr_mailbox.v",
                            str(out / "surf_ddr_mailbox.v")], cwd=ROOT, capture_output=True, text=True)
        if c.returncode != 0:
            return True, "killed (mailbox test does not build)"
        for seed in (1, 2):
            try:
                m = subprocess.run(["vvp", "-n", str(vvp), "+seed={}".format(seed)], cwd=ROOT,
                                   capture_output=True, text=True, timeout=300)
            except subprocess.TimeoutExpired:
                return True, "killed by tb_ddr_mailbox (hung)"
            if m.returncode != 0 or "PASS" not in m.stdout:
                first = next((l for l in m.stdout.splitlines() if l.startswith("mailbox:")), "failed")
                return True, "killed by tb_ddr_mailbox: " + first[:120]
    try:
        r = subprocess.run([str(exe), "--test", "all", "--seed", "1"], cwd=ROOT, capture_output=True,
                           text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return True, "killed (testbench hung, timeout)"
    if r.returncode == 0:
        return False, "survived"
    if "FAIL" not in r.stdout and r.returncode != 1:
        return True, "killed (testbench exit {})".format(r.returncode)
    first = next((l for l in r.stdout.splitlines() if l.startswith("FAIL")), "exit {}".format(r.returncode))
    return True, "killed: " + first[:150]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()
    bad = 0
    killed = survived_expected = 0
    for name, path, old, new, may_survive in MUTANTS:
        if args.only and name not in args.only:
            continue
        was_killed, what = run_mutant(name, path, old, new, args.jobs)
        if was_killed:
            killed += 1
            verdict = "ok"
        elif what.startswith("not run"):
            verdict = "SKIPPED"
            bad += 1
        elif may_survive and what == "survived":
            survived_expected += 1
            verdict = "ok (expected: metastability is not simulated)"
        else:
            bad += 1
            verdict = "BAD"
        print("mutant {:20s} {:4s} {}  [{}]".format(name, "", what, verdict), flush=True)
    print("ddr_mutants: {} killed, {} survived as expected, {} not caught".format(killed, survived_expected, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
