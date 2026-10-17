#!/usr/bin/env python3
"""
SurfCPU open flow: no ordinary net on the global clock network
--------------------
File: gclkcheck.py Date: 10/16/26 (SurfCPU)
--------------------
The chip database of this image (openXC7/nextpnr 1.0.0, generated from Project X-Ray) lists each
BUFGCTRL's I0 -> O as a route-through pseudo pip, so nextpnr's router can carry an ordinary net
into an unused global buffer (CLK_BUFG_BUFGCTRLn_I0 <- CLK_BUFG_IMUXk), onto a global clock line
(CLK_BUFG_CK_GCLKm <- CLK_BUFG_BUFGCTRLn_O), through the clock rows and back into logic through an
interconnect GFAN wire (GFANn <- GCLK_*). The bitstream then configures no buffer site (no
IN_USE, no CE/S/IGNORE settings), and LVS, which reads the route-through with the same database,
sees a connection. On the board it is not one: round ddr-4 found two placements of the DDR
diagnostics whose only resources in common that none of five working placements used were such
routes, each into CPU logic (the EX stage's adders and ALU control in one, the ALU's shifter in
the other), and the faults on the board are what those nets stuck would do (docs/OPEN-FLOW.md 6.8).

The flow builds with a chip database without these pips (devenv/openxc7/chipdb-gen.py); this
check reads the routed FASM and fails on any trace of the pattern:
  * a global clock line driven by a BUFGCTRL whose site the FASM does not configure (IN_USE);
  * a fabric input (CLK_BUFG_IMUX) into a BUFGCTRL that is not configured;
  * a GFAN wire fed from a global clock leaf (a clock line used as data in an interconnect tile).
A real global buffer (a BUFGCTRL cell, configured, fed from its dedicated clock inputs) passes.

    python3 -I devenv/openxc7/gclkcheck.py --fasm TOP.fasm [--report FILE]
    python3 -I devenv/openxc7/gclkcheck.py --self-test

Exit 0 when nothing is found (the report's last line: "gclkcheck: N global buffer(s), 0 ..."),
1 when something is, 2 when the FASM cannot be read. Only the Python standard library.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SITE = re.compile(r"^(CLK_BUFG_(?:TOP|BOT)_R_X\d+Y\d+)\.BUFGCTRL\.BUFGCTRL_X0Y(\d+)\.IN_USE$")
GCLK_OUT = re.compile(r"^(CLK_BUFG_(?:TOP|BOT)_R_X\d+Y\d+)\.CLK_BUFG_CK_GCLK(\d+)\.CLK_BUFG_BUFGCTRL(\d+)_O$")
BUF_IN = re.compile(r"^(CLK_BUFG_(?:TOP|BOT)_R_X\d+Y\d+)\.CLK_BUFG_BUFGCTRL(\d+)_I([01])\.(\S+)$")
GFAN = re.compile(r"^(INT_[LR]_X\d+Y\d+)\.(GFAN[01])\.(GCLK\S*)$")


def check(lines: list[str]) -> tuple[list[str], int]:
    """(findings, global buffers configured) of the FASM's feature lines."""
    feats = []
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        feats.append(line.split("=")[0].strip())
    used = set()
    for f in feats:
        m = SITE.match(f)
        if m:
            used.add((m.group(1), int(m.group(2))))
    findings = []
    for f in feats:
        m = GCLK_OUT.match(f)
        if m and (m.group(1), int(m.group(3))) not in used:
            findings.append(f"route-through: {m.group(1)} drives GCLK{m.group(2)} from BUFGCTRL{m.group(3)}, "
                            f"whose site is not configured ({f})")
            continue
        m = BUF_IN.match(f)
        if m and (m.group(1), int(m.group(2))) not in used and "IMUX" in m.group(4):
            findings.append(f"fabric into an unconfigured buffer: {m.group(1)} BUFGCTRL{m.group(2)} I{m.group(3)} "
                            f"from {m.group(4)} ({f})")
            continue
        m = GFAN.match(f)
        if m:
            findings.append(f"clock line as data: {m.group(1)} {m.group(2)} from {m.group(3)} ({f})")
    return findings, len(used)


def report_lines(findings: list[str], used: int) -> list[str]:
    out = [f"FOUND  {x}" for x in findings]
    rt = sum(1 for x in findings if x.startswith("route-through"))
    cd = sum(1 for x in findings if x.startswith("clock line as data"))
    fi = len(findings) - rt - cd
    out.append(f"gclkcheck: {used} global buffer(s) configured, {rt} route-through(s), "
               f"{fi} fabric input(s) into unconfigured buffers, {cd} clock-to-data use(s)")
    return out


def self_test() -> int:
    real = ["CLK_BUFG_TOP_R_X78Y105.BUFGCTRL.BUFGCTRL_X0Y5.IN_USE",
            "CLK_BUFG_TOP_R_X78Y105.BUFGCTRL.BUFGCTRL_X0Y5.ZINV_CE0",
            "CLK_BUFG_TOP_R_X78Y105.CLK_BUFG_BUFGCTRL5_I0.CLK_BUFG_TOP_R_CK_MUXED10",
            "CLK_BUFG_TOP_R_X78Y105.CLK_BUFG_R_BUFGCTRL5_CE0.CLK_BUFG_IMUX22_1",
            "CLK_BUFG_TOP_R_X78Y105.CLK_BUFG_CK_GCLK21.CLK_BUFG_BUFGCTRL5_O",
            "INT_R_X39Y127.CLK0.GCLK_B0_EAST", "INT_R_X39Y127.GCLK_B0_EAST.GCLK_B0",
            "INT_R_X39Y127.IMUX41.GFAN0", "INT_R_X39Y127.GFAN0.GND_WIRE"]
    # round ddr-4, head 69ee491 seed 3: the one route into the ALU's shifter
    seed3 = ["CLK_BUFG_TOP_R_X78Y105.CLK_BUFG_BUFGCTRL13_I0.CLK_BUFG_IMUX29_3",
             "CLK_BUFG_TOP_R_X78Y105.CLK_BUFG_CK_GCLK29.CLK_BUFG_BUFGCTRL13_O",
             "CLK_HROW_TOP_R_X78Y130.CLK_HROW_CK_MUX_OUT_R11.CLK_HROW_R_CK_GCLK29",
             "INT_L_X38Y127.GCLK_L_B9_EAST.GCLK_L_B9", "INT_R_X39Y127.GFAN0.GCLK_B9",
             "INT_R_X39Y127.IMUX41.GFAN0"]
    cases = [("a configured global buffer and GFAN from GND: nothing found", real, 0, 1),
             ("the seed 3 route: the buffer output, its fabric input and the GFAN found", real[:5] + seed3, 3, 1),
             ("a GFAN fed from a leaf clock line alone is found", ["INT_L_X40Y106.GFAN0.GCLK_L_B9_WEST"], 1, 0),
             ("a route-through beside a configured buffer of another index is found",
              real[:5] + ["CLK_BUFG_TOP_R_X78Y105.CLK_BUFG_CK_GCLK27.CLK_BUFG_BUFGCTRL11_O"], 1, 1),
             ("a configured buffer in the bottom tile with a fabric input passes (a real BUFG fed from logic)",
              ["CLK_BUFG_BOT_R_X78Y100.BUFGCTRL.BUFGCTRL_X0Y2.IN_USE",
               "CLK_BUFG_BOT_R_X78Y100.CLK_BUFG_BUFGCTRL2_I0.CLK_BUFG_IMUX24_0",
               "CLK_BUFG_BOT_R_X78Y100.CLK_BUFG_CK_GCLK2.CLK_BUFG_BUFGCTRL2_O"], 0, 1)]
    fails = 0
    for name, lines, want, want_used in cases:
        found, used = check(lines)
        ok = len(found) == want and used == want_used
        print(("ok   " if ok else "FAIL ") + f"{name} ({len(found)} found, {used} configured)")
        if not ok:
            print("\n".join("     " + x for x in report_lines(found, used)))
            fails += 1
    print("gclkcheck self-test: " + ("PASS" if not fails else f"{fails} FAILED"))
    return 1 if fails else 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="no ordinary net on the global clock network")
    ap.add_argument("--fasm")
    ap.add_argument("--report")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv[1:])
    if a.self_test:
        return self_test()
    if not a.fasm:
        ap.error("--fasm is required")
    try:
        lines = Path(a.fasm).read_text().splitlines()
    except (OSError, UnicodeDecodeError) as e:
        print(f"gclkcheck: cannot read {a.fasm}: {e}", file=sys.stderr)
        return 2
    found, used = check(lines)
    rep = report_lines(found, used)
    if a.report:
        Path(a.report).write_text("\n".join(rep) + "\n")
    print("\n".join(rep))
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
