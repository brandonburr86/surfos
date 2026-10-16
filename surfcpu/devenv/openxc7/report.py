#!/usr/bin/env python3
"""
SurfCPU openXC7 build report
--------------------
File: report.py Date: 10/15/26 (SurfCPU)
--------------------
Turns one openXC7 build's outputs into the two summaries tools/openxc7-build.sh keeps next
to the bitstream: timing.txt (each clock's constraint and the frequency nextpnr's timing
model gives after routing, the critical path, warnings) and utilization.txt (nextpnr's
placed resources against the die, Yosys's cell counts, and the documented capacity of the
device). Exit status 1 if a clock misses its constraint or the design exceeds the device's
documented capacity.

nextpnr knows dies, not parts: an xc7a35t is placed on the xc7a50t die and nextpnr counts
the 50T's resources as available. The 35T offers fewer (AMD DS180), so the capacity check
uses the device's own numbers.

    report.py --device xc7a35t --report report.json --log nextpnr.log --stat stat.json \\
              --timing timing.txt --utilization utilization.txt
"""

from __future__ import annotations

import argparse
import json
import re
import sys

# AMD DS180 (7 Series FPGAs Data Sheet: Overview), Artix-7 table: slices (4 LUT6 and
# 8 flip-flops each), 36 Kb block RAMs, DSP48E1 slices, clock management tiles.
CAPACITY = {
    "xc7a35t": {"slices": 5200, "ramb36": 50, "dsp": 90, "cmt": 5},
    "xc7a50t": {"slices": 8150, "ramb36": 75, "dsp": 120, "cmt": 5},
    "xc7a100t": {"slices": 15850, "ramb36": 135, "dsp": 240, "cmt": 6},
}
SHOWN = ["SLICE_LUTX", "SLICE_FFX", "CARRY4", "RAMB36E1_RAMB36E1", "RAMB18E1_RAMB18E1",
         "DSP48E1_DSP48E1", "BUFGCTRL", "MMCME2_ADV_MMCME2_ADV", "PLLE2_ADV_PLLE2_ADV", "PAD"]


def main() -> int:
    ap = argparse.ArgumentParser(description="openXC7 build report (see the file header)")
    ap.add_argument("--device", required=True, choices=sorted(CAPACITY))
    ap.add_argument("--report", required=True, help="nextpnr --report JSON")
    ap.add_argument("--log", required=True, help="nextpnr log")
    ap.add_argument("--stat", required=True, help="Yosys stat -json output")
    ap.add_argument("--timing", required=True)
    ap.add_argument("--utilization", required=True)
    args = ap.parse_args()

    with open(args.report) as f:
        rep = json.load(f)
    with open(args.log, errors="replace") as f:
        log = f.read().splitlines()
    with open(args.stat) as f:
        stat = json.load(f)
    problems = []

    # ------------------------------------------------------------ timing
    t = ["Timing (nextpnr's model after routing; an estimate, not Vivado signoff)", ""]
    for clk, v in sorted(rep.get("fmax", {}).items()):
        ok = v["achieved"] >= v["constraint"]
        t.append(f"clock {clk}: constraint {v['constraint']:.2f} MHz, achieved {v['achieved']:.2f} MHz, "
                 f"slack {1000 / v['constraint'] - 1000 / v['achieved']:+.3f} ns  {'PASS' if ok else 'FAIL'}")
        if not ok:
            problems.append(f"clock {clk} misses its {v['constraint']} MHz constraint")
    if not rep.get("fmax"):
        t.append("no constrained clocks (nothing was timed)")
        problems.append("no clock constraint reached the design")
    holdfix = [ln for ln in log if "Hold-fix" in ln]
    t += [""] + [ln.replace("Info: ", "") for ln in holdfix]
    warns = [ln for ln in log if ln.startswith("Warning:")]
    t += ["", f"nextpnr warnings: {len(warns)}"] + [f"  {w}" for w in warns]
    # Cross-domain paths: nextpnr reports their longest delay but checks nothing (its XDC reader
    # has no set_max_delay or set_clock_groups), so list them for a reader to judge against
    # the CDC design (rtl/lib's synchronizers).
    cross = {}
    for ln in log:
        if ln.startswith("Info: Max delay ") and "->" in ln:
            pair, delay = ln[len("Info: Max delay "):].rsplit(":", 1)
            cross[pair.strip()] = delay.strip()
    if cross:
        t += ["", "Cross-domain paths (reported by nextpnr, not checked against any constraint):"]
        t += [f"  {pair}: {delay}" for pair, delay in sorted(cross.items())]
    # The last report of each kind in the log is the one after routing.
    blocks = {}
    i = 0
    while i < len(log):
        if "Critical path report for" in log[i]:
            key = log[i].split("Critical path report for", 1)[1].strip()
            block = [log[i].replace("Info: ", "", 1)]
            i += 1
            while i < len(log) and log[i].startswith("Info: ") and "Critical path report for" not in log[i] \
                    and not log[i].startswith("Info: Max frequency") and not log[i].startswith("Info: Max delay") \
                    and not log[i].startswith("Info: Slack histogram"):
                block.append(log[i].replace("Info: ", "", 1))
                i += 1
            blocks[key] = block
            continue
        i += 1
    for key in sorted(blocks):
        t += [""] + blocks[key]
    with open(args.timing, "w") as f:
        f.write("\n".join(t) + "\n")

    # ------------------------------------------------------------ utilization
    util = rep.get("utilization", {})
    cap = CAPACITY[args.device]
    u = [f"Utilization ({args.device}; nextpnr counts the die it places on)", "",
         f"{'nextpnr resource':28s} {'used':>8s} {'die':>8s}"]
    for name in SHOWN + sorted(k for k, v in util.items() if v.get("used") and k not in SHOWN):
        if name in util:
            u.append(f"{name:28s} {util[name]['used']:8d} {util[name]['available']:8d}")
    cells = {}
    for mod in stat.get("modules", {}).values():
        for k, n in mod.get("num_cells_by_type", {}).items():
            cells[k] = cells.get(k, 0) + n
    luts = sum(n for k, n in cells.items() if re.fullmatch(r"LUT[1-6]|INV", k))
    ffs = sum(n for k, n in cells.items() if re.fullmatch(r"FD[A-Z]*|LD[A-Z]*", k))
    bram36 = cells.get("RAMB36E1", 0) + 0.5 * cells.get("RAMB18E1", 0)
    dsp = cells.get("DSP48E1", 0)
    u += ["", "Yosys cells: " + ", ".join(f"{k} {v}" for k, v in sorted(cells.items()) if not k.startswith("$")),
          "", f"Against the {args.device}'s documented capacity (AMD DS180):"]
    rows = [("LUTs (Yosys LUT1-6 and INV cells; packing can only reduce)", luts, cap["slices"] * 4),
            ("LUT positions (nextpnr SLICE_LUTX, two per LUT6)", util.get("SLICE_LUTX", {}).get("used", 0), cap["slices"] * 8),
            ("flip-flops", ffs, cap["slices"] * 8),
            ("carry chains (CARRY4, one per slice)", util.get("CARRY4", {}).get("used", 0), cap["slices"]),
            ("36 Kb block RAMs (RAMB18 = 1/2)", bram36, cap["ramb36"]),
            ("DSP48E1", dsp, cap["dsp"])]
    for what, used, limit in rows:
        pct = 100.0 * used / limit if limit else 0
        flag = "" if used <= limit else "  OVER"
        u.append(f"  {what:58s} {used:>8g} / {limit:<7d} {pct:5.1f}%{flag}")
        if used > limit:
            problems.append(f"{what}: {used:g} exceeds the {args.device}'s {limit}")
    if args.device == "xc7a35t":
        u += ["", "Note: the xc7a35t is placed on the xc7a50t die (the same silicon and configuration",
              "frames); nextpnr may use sites beyond the region Vivado offers for a 35T. AMD does not",
              "document those as usable on a 35T: see docs/OPEN-FLOW.md."]
    with open(args.utilization, "w") as f:
        f.write("\n".join(u) + "\n")

    print("\n".join(ln for ln in t if ln.startswith("clock ")))
    for p in problems:
        print(f"report: {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
