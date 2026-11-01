#!/usr/bin/env python3
"""
SurfCPU open flow: no shift-register LUTs in the netlist
--------------------
File: srlcheck.py Date: 10/18/26 (SurfGPU)
--------------------
Yosys's synth_xilinx turns a chain of three or more registers without a reset into SRL16E or
SRLC32E cells (shift registers in a LUT). The LVS proof cannot follow them: its extractor's tile
model (fasm2netlist, the xcol module of lvs/tile_model.v) has no shift mode, and the values it
does not implement resolve to 0, so every signal an SRL feeds comes out different. Two of the
GPU's builds failed LVS on two SRL16E that way (docs/GPU.md section 12.2); whether those
bitstreams were right was never known. Turning SRL inference off (synth_xilinx -nosrl) changes the netlist of every
design, SRLs or not (more LUTs: 798 against 820 cells in the baseline), so the flow keeps
synthesis as it is and refuses an SRL here, right after synthesis, before anything is placed:
give the chain a reset (or otherwise keep it in flip-flops).

    python3 -I devenv/openxc7/srlcheck.py --json TOP.json --top TOP [--report FILE]
    python3 -I devenv/openxc7/srlcheck.py --self-test

Exit 0 when the netlist has no SRL cell, 1 when it has (each listed with the register its output
stands for, the name of the net it drives), 2 when the netlist cannot be read. Only the Python
standard library.
"""

import argparse
import json
import sys

# every shift-register LUT of the 7 series (UG953): the SRL16 family and SRLC32E, and CFGLUT5, a
# LUT whose contents shift in from a pin
SRL_TYPES = ("SRL16", "SRL16E", "SRLC16", "SRLC16E", "SRLC32E", "CFGLUT5")


def find(netlist: dict, top: str) -> list[tuple[str, str, str]]:
    """(cell, type, the net its output drives: the register it stands for) of every SRL cell of
    the top module"""
    mod = netlist.get("modules", {}).get(top)
    if mod is None:
        raise KeyError(f"no module {top} in the netlist")
    names: dict[int, str] = {}
    for net, info in mod.get("netnames", {}).items():
        for i, b in enumerate(info.get("bits", [])):
            if isinstance(b, int) and (b not in names or names[b].startswith("$")):
                names[b] = net if len(info["bits"]) == 1 else f"{net}[{i}]"
    out = []
    for name, cell in mod.get("cells", {}).items():
        if cell.get("type") in SRL_TYPES:
            q = [b for b in cell.get("connections", {}).get("Q", []) if isinstance(b, int)]
            out.append((name, cell["type"], ", ".join(names.get(b, "?") for b in q) or "?"))
    return sorted(out)


def report_lines(srls: list[tuple[str, str, str]], cells: int) -> list[str]:
    lines = [f"  {t} {n}: drives {net}" for n, t, net in srls]
    if srls:
        lines += [f"srlcheck: FAIL: {len(srls)} SRL cell(s) in a netlist of {cells} cells. The open flow's LVS "
                  "cannot check them (its extractor has no shift mode: every signal they feed would differ); give "
                  "each chain a reset, or keep it in flip-flops otherwise (docs/OPEN-FLOW.md 6.5)"]
    else:
        lines.append(f"srlcheck: PASS: no SRL cell among {cells} cells")
    return lines


def self_test() -> int:
    def net(types):
        return {"modules": {"t": {"cells": {f"c{i}": {"type": ty, "connections": {"Q": [10 + i]}}
                                            for i, ty in enumerate(types)},
                                  "netnames": {f"r{i}": {"bits": [10 + i]} for i in range(len(types))}}}}
    cases = [("flip-flops and LUTs only: nothing found", net(["FDRE", "LUT6", "FDSE", "RAM64M"]), 0),
             ("an SRL16E (the GPU's lane delay) is found", net(["FDRE", "SRL16E", "LUT2"]), 1),
             ("... and named by the net it drives", None, "r1"),
             ("every SRL kind is found", net(["SRL16", "SRLC16", "SRLC16E", "SRLC32E", "CFGLUT5", "FDRE"]), 5)]
    fails = 0
    for name, n, want in cases:
        if n is None:                               # the name of the previous case's SRL
            got = find(net(["FDRE", "SRL16E", "LUT2"]), "t")[0][2]
        else:
            got = len(find(n, "t"))
        ok = got == want
        print(("ok   " if ok else "FAIL ") + f"{name} ({got} found)")
        fails += not ok
    try:
        find(net([]), "nope")
        print("FAIL a missing top is refused")
        fails += 1
    except KeyError:
        print("ok   a missing top is refused")
    print(f"srlcheck self-test: {'PASS' if not fails else 'FAIL'} ({len(cases) + 1} cases)")
    return 1 if fails else 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="no shift-register LUTs in the netlist")
    ap.add_argument("--json")
    ap.add_argument("--top")
    ap.add_argument("--report")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.json or not a.top:
        ap.error("--json and --top are required")
    try:
        netlist = json.load(open(a.json))
        srls = find(netlist, a.top)
    except (OSError, ValueError, KeyError) as e:
        print(f"srlcheck: cannot read {a.json}: {e}")
        return 2
    lines = report_lines(srls, len(netlist["modules"][a.top].get("cells", {})))
    text = "\n".join(lines) + "\n"
    if a.report:
        open(a.report, "w").write(text)
    sys.stdout.write(text)
    return 1 if srls else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
