#!/usr/bin/env python3
"""
SurfCPU Arty A7: the synthesized netlist still holds the design's memories and multiplier
--------------------
File: netlist_check.py Date: 10/16/26 (SurfCPU)
--------------------
tools/fpga-build.sh runs this after the open flow (FLOW=openxc7), next to the flow's own
synthesis check (devenv/openxc7/synthcheck.py), which compares flip-flops instance by
instance and the total of large memories. This one names the parts that check cannot see
one by one:

  memories   every memory of the elaborated RTL (pre.json: $mem_v2 cells after memory
             -nomap) must be in the netlist as block RAM or distributed RAM cells named
             after it (Yosys names them <memory>.<n>...), with at least its bits of storage:
             a register file or a FIFO that synthesis had dropped, or turned into something
             else, fails;
  dsp        with --dsp INSTANCE=N, exactly N DSP48E1 cells take their operands (A or B)
             from nets of INSTANCE: the core's multiplier stays in its DSPs (--dsp safe).

    netlist_check.py --top surfcpu_arty_a7_top --pre pre.json --post top.json \
        [--dsp u_soc.u_core.muldiv=4]

Exit status 0 when every rule holds. Only the standard library is used.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict

# storage bits per cell (data and parity for block RAM; distributed RAM counted once per cell)
STORAGE = {"RAMB36E1": 36 * 1024, "RAMB18E1": 18 * 1024,
           "RAM32M": 256, "RAM32M16": 512, "RAM64M": 256, "RAM64M8": 512,
           "RAM32X1D": 32, "RAM64X1D": 64, "RAM128X1D": 128, "RAM256X1D": 256,
           "RAM32X1S": 32, "RAM64X1S": 64, "RAM128X1S": 128, "RAM256X1S": 256}


def module(path: str, top: str) -> dict:
    with open(path) as f:
        design = json.load(f)
    if top not in design["modules"]:
        raise SystemExit(f"netlist_check: no module {top} in {path}")
    return design["modules"][top]


def as_int(v) -> int:
    v = str(v)
    return int(v, 2) if v and set(v) <= {"0", "1"} else int(v)


def main() -> int:
    ap = argparse.ArgumentParser(description="netlist expectations (see the file header)")
    ap.add_argument("--top", required=True)
    ap.add_argument("--pre", required=True, help="the elaborated RTL as Yosys JSON (memory -nomap)")
    ap.add_argument("--post", required=True, help="the synthesized netlist as Yosys JSON")
    ap.add_argument("--dsp", action="append", default=[], metavar="INSTANCE=N")
    args = ap.parse_args()
    pre, post = module(args.pre, args.top), module(args.post, args.top)
    problems: list[str] = []

    # ---- memories, one by one
    mems = {}
    for cell in pre.get("cells", {}).values():
        if cell["type"] in ("$mem", "$mem_v2"):
            p = cell.get("parameters", {})
            name = str(p.get("MEMID", "")).lstrip("\\")
            mems[name] = as_int(p.get("WIDTH", 0)) * as_int(p.get("SIZE", 0))
    found = defaultdict(lambda: defaultdict(int))
    for cname, cell in post.get("cells", {}).items():
        if cell["type"] in STORAGE:
            for name in mems:
                if cname == name or cname.startswith(name + "."):
                    found[name][cell["type"]] += 1
                    break
    print(f"{'memory (RTL)':42s} {'bits':>8s}   netlist cells")
    for name, bits in sorted(mems.items()):
        cells = found.get(name, {})
        have = sum(STORAGE[t] * n for t, n in cells.items())
        desc = ", ".join(f"{n} {t}" for t, n in sorted(cells.items())) or "none"
        flag = ""
        if not cells:
            flag = "  MISSING"
            problems.append(f"memory {name} ({bits} bits) has no RAM cell in the netlist")
        elif have < bits:
            flag = "  TOO SMALL"
            problems.append(f"memory {name}: {bits} bits in the RTL, {have} bits of RAM cells in the netlist")
        print(f"{name:42s} {bits:8d}   {desc}{flag}")
    if not mems:
        print("(no memories in the RTL)")

    # ---- DSP48E1 operands by instance
    names = defaultdict(set)
    for n, net in post.get("netnames", {}).items():
        if n.startswith("$"):
            continue
        for b in net["bits"]:
            if isinstance(b, int):
                names[b].add(n)
    dsps = [c for c in post.get("cells", {}).values() if c["type"] == "DSP48E1"]
    print(f"DSP48E1 in the netlist: {len(dsps)}")
    for spec in args.dsp:
        m = re.fullmatch(r"([^=]+)=(\d+)", spec)
        if not m:
            raise SystemExit(f"netlist_check: --dsp {spec}: expected INSTANCE=N")
        inst, want = m.group(1), int(m.group(2))
        got = 0
        for c in dsps:
            nets = {n for port in ("A", "B") for b in c["connections"].get(port, []) if isinstance(b, int)
                    for n in names[b]}
            if any(n.startswith(inst + ".") for n in nets):
                got += 1
        print(f"DSP48E1 taking operands from {inst}: {got} (expected {want})")
        if got != want:
            problems.append(f"{got} DSP48E1 take operands from {inst}, expected {want}")

    for p in problems:
        print(f"PROBLEM: {p}")
    print("netlist_check: " + ("PASS" if not problems else f"FAIL ({len(problems)} problems)"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
