#!/usr/bin/env python3
"""
SurfCPU synthesis sanity check
--------------------
File: synthcheck.py Date: 10/16/26 (SurfCPU)
--------------------
Yosys can silently delete working logic: its xilinx_dsp pass (0.52, 0.69 and 0.70 alike)
packs a counter that is also multiplied into a DSP48E1 and leaves the fabric without the
counter (devenv/openxc7/test/dsp_counter.v; the io track lost surf_video's whole scan-out
that way). Nothing after synthesis can notice, because LVS proves the bitstream equal to the
synthesized netlist, not to the RTL. So this compares the synthesized netlist with the
elaborated RTL (after proc, flatten, opt -full, wreduce and memory -nomap, before any
mapping) by three rules, any of which fails the check:

  flip-flops   per instance (hierarchical name prefix, from the flip-flops' output nets), the
               RTL's flip-flop bits against the netlist's flip-flops plus what DSP48E1, block
               RAM output and SRL cells can hold; an instance with at least --min-ffs bits
               that keeps less than --ratio of them fails, as does the whole design below
               --global-ratio
  memories     the bits of RTL memories of 16 Kbit or more against the netlist's block RAM
  outputs      a top-level output bit that is constant after synthesis but not in the RTL

The RTL's flip-flop bits leave out those Yosys has already merged into a memory's
synchronous read port when the elaborated RTL is written (memory_dff moves a read-data
register into the port and reconnects the register's outputs to $ffmerge_disconnected wires,
keeping the cell when some of its bits are still used): they are the memory's read port from
then on, which a block RAM holds in its output latch, not flip-flops that synthesis could
lose. They are counted and printed separately. Until round ddr-3 they were counted as RTL
flip-flops, so LiteX's VexRiscv (lite) appeared to lose its 22 instruction-cache tag bits.

With --expect FILE, lines "<instance prefix> <minimum flip-flops>" ('.' for the whole
design, '#' starts a comment) must hold as well. Only the standard library is used.

    synthcheck.py --top flow_demo --pre pre.json --post flow_demo.json [--expect file]
    synthcheck.py --self-test
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict

RTL_FF = {"$dff", "$dffe", "$adff", "$adffe", "$sdff", "$sdffe", "$sdffce", "$dffsr", "$dffsre",
          "$aldff", "$aldffe", "$dlatch", "$adlatch", "$dlatchsr", "$sr", "$ff"}
NETLIST_FF = {"FDRE", "FDSE", "FDCE", "FDPE", "LDCE", "LDPE", "FDRE_1", "FDSE_1", "FDCE_1", "FDPE_1"}
SRL_BITS = {"SRL16E": 16, "SRLC16E": 16, "SRLC32E": 32}
RAM_BITS = {"RAMB18E1": 18 * 1024, "RAMB36E1": 36 * 1024}
BIG_MEMORY = 16 * 1024


def load(path: str, top: str) -> dict:
    with open(path) as f:
        design = json.load(f)
    mods = design["modules"]
    if top not in mods:
        raise SystemExit(f"synthcheck: no module {top} in {path}")
    return mods[top]


def namer(module: dict):
    """bit -> the shortest public net name carrying it (hierarchical names keep the path).

    A top-level port's own name counts only when nothing else names the bit. In the RTL, a
    flip-flop inside an instance that drives an output port carries the port's name as
    well, the shortest; in the netlist an OBUF stands between them and the port's name
    moves to the pad side. Preferring the port name would credit the flip-flop to (top) in
    the RTL and to its instance in the netlist, and an instance that keeps every flip-flop
    would show as having lost them (the whole SoC: 37 output registers)."""
    ports = set(module.get("ports", {}))
    best: dict = {}
    for name, net in module.get("netnames", {}).items():
        if net.get("hide_name"):
            continue
        rank = (name in ports, len(name))
        for b in net["bits"]:
            if isinstance(b, int) and (b not in best or rank < best[b][0]):
                best[b] = (rank, name)
    return {b: name for b, (_, name) in best.items()}


def instance_of(name: str | None) -> str:
    if not name:
        return "(unnamed)"
    return name.rsplit(".", 1)[0] if "." in name else "(top)"


def nonconst(bits: list) -> int:
    return sum(1 for b in bits if isinstance(b, int))


def param_int(cell: dict, name: str, default: int = 0) -> int:
    v = cell.get("parameters", {}).get(name, default)
    if isinstance(v, int):
        return v
    v = str(v)
    try:
        return int(v, 2) if set(v) <= {"0", "1"} else int(v)
    except ValueError:
        return default


def merged_into_memories(module: dict) -> set:
    """Bits memory_dff took out of a register into a memory's read port: they are named only
    $ffmerge_disconnected... and nothing reads them."""
    marked: set = set()
    other: set = set()
    for name, net in module.get("netnames", {}).items():
        for b in net["bits"]:
            if isinstance(b, int):
                (marked if name.startswith("$ffmerge_disconnected") else other).add(b)
    used: set = set()
    for cell in module.get("cells", {}).values():
        dirs = cell.get("port_directions", {})
        for p, bits in cell.get("connections", {}).items():
            if dirs.get(p) != "output":
                used.update(b for b in bits if isinstance(b, int))
    for port in module.get("ports", {}).values():
        if port["direction"] != "input":
            used.update(b for b in port["bits"] if isinstance(b, int))
    return marked - other - used


def count(module: dict, rtl: bool) -> tuple[dict, dict, int, int, int]:
    """(flip-flop bits per instance, absorbable bits per instance, memory bits, RAM bits,
    register bits merged into memory read ports)."""
    names = namer(module)
    merged = merged_into_memories(module) if rtl else set()
    ffs: dict = defaultdict(int)
    absorbed: dict = defaultdict(int)
    mem_bits = ram_bits = merged_bits = 0
    for cell in module.get("cells", {}).values():
        t, conns = cell["type"], cell.get("connections", {})
        if rtl and t in RTL_FF:
            for b in conns.get("Q", []):
                if b in merged:
                    merged_bits += 1
                    continue
                ffs[instance_of(names.get(b))] += 1
        elif rtl and t.startswith("$mem"):
            bits = param_int(cell, "WIDTH") * param_int(cell, "SIZE")
            if bits >= BIG_MEMORY:
                mem_bits += bits
        elif not rtl and t in NETLIST_FF:
            ffs[instance_of(names.get(conns.get("Q", [None])[0]))] += 1
        elif not rtl and t in SRL_BITS:
            q = conns.get("Q", [None])[0]
            absorbed[instance_of(names.get(q))] += SRL_BITS[t]
        elif not rtl and t == "DSP48E1":
            a, b, c, d = (nonconst(conns.get(p, [])) for p in ("A", "B", "C", "D"))
            p_bits = [x for x in conns.get("P", []) if isinstance(x, int)]
            held = (param_int(cell, "AREG") * a + param_int(cell, "BREG") * b + param_int(cell, "CREG") * c
                    + param_int(cell, "DREG") * d + param_int(cell, "ADREG") * max(a, d)
                    + param_int(cell, "MREG") * min(43, a + b) + param_int(cell, "PREG") * len(p_bits))
            absorbed[instance_of(names.get(p_bits[0]) if p_bits else None)] += held
        elif not rtl and t in RAM_BITS:
            ram_bits += RAM_BITS[t]
            outs = [x for p in ("DOADO", "DOBDO") for x in conns.get(p, []) if isinstance(x, int)]
            regs = param_int(cell, "DOA_REG") + param_int(cell, "DOB_REG")
            if regs:
                absorbed[instance_of(names.get(outs[0]) if outs else None)] += len(outs)
    return ffs, absorbed, mem_bits, ram_bits, merged_bits


def constant_outputs(module: dict, rtl: bool) -> set:
    """Top-level output bits that are constants (through an OBUF in the netlist)."""
    drivers = {}
    if not rtl:
        for cell in module.get("cells", {}).values():
            if cell["type"] in ("OBUF", "OBUFT"):
                o = cell["connections"].get("O", [None])[0]
                i = cell["connections"].get("I", [None])[0]
                t = cell["connections"].get("T", ["0"])[0] if cell["type"] == "OBUFT" else "0"
                if isinstance(o, int):
                    drivers[o] = (i, t)
    out = set()
    for pname, port in module.get("ports", {}).items():
        if port["direction"] != "output":
            continue
        for k, b in enumerate(port["bits"]):
            if isinstance(b, str):
                out.add(f"{pname}[{k}]")
            elif b in drivers and isinstance(drivers[b][0], str) and drivers[b][1] in ("0", 0):
                out.add(f"{pname}[{k}]")
    return out


def self_test() -> int:
    """A register memory_dff half merged into a memory read port (the VexRiscv tag case), and a
    register synthesis really lost."""
    def rtl(lost: bool) -> dict:
        nets = {"clk": {"bits": [2]}, "o": {"bits": [3]}, "u.tag_err": {"bits": [10]},
                "u.cnt": {"bits": list(range(40, 72))}}
        for k in range(22):
            nets[f"$ffmerge_disconnected${100 + k}"] = {"bits": [11 + k], "hide_name": 1}
        cells = {
            "tagreg": {"type": "$dffe", "port_directions": {"CLK": "input", "D": "input", "EN": "input", "Q": "output"},
                       "connections": {"CLK": [2], "D": list(range(80, 103)), "EN": [4], "Q": [10] + list(range(11, 33))}},
            "use": {"type": "$and", "port_directions": {"A": "input", "B": "input", "Y": "output"},
                    "connections": {"A": [10], "B": [40], "Y": [3]}},
            "cnt": {"type": "$dff", "port_directions": {"CLK": "input", "D": "input", "Q": "output"},
                    "connections": {"CLK": [2], "D": list(range(110, 142)), "Q": list(range(40, 72))}},
            "add": {"type": "$add", "port_directions": {"A": "input", "B": "input", "Y": "output"},
                    "connections": {"A": list(range(40, 72)), "B": ["1"], "Y": list(range(110, 142))}},
        }
        return {"ports": {"clk": {"direction": "input", "bits": [2]}, "o": {"direction": "output", "bits": [3]}},
                "netnames": nets, "cells": cells}
    def netlist(lost: bool) -> dict:
        nets = {"clk": {"bits": [2]}, "o": {"bits": [3]}, "u.tag_err": {"bits": [10]},
                "u.cnt": {"bits": list(range(40, 72))}, "u.tags_q": {"bits": list(range(200, 222))}}
        cells = {"tag_err_ff": {"type": "FDRE", "connections": {"Q": [10]}},
                 "tags": {"type": "RAMB18E1", "parameters": {"DOA_REG": "0", "DOB_REG": "0"},
                          "connections": {"DOADO": list(range(200, 216)), "DOBDO": list(range(216, 222))}}}
        if not lost:
            for k in range(32):
                cells[f"cnt{k}"] = {"type": "FDRE", "connections": {"Q": [40 + k]}}
        return {"ports": {}, "netnames": nets, "cells": cells}
    bad = 0
    for lost in (False, True):
        pre_ffs, _, _, _, merged = count(rtl(lost), rtl=True)
        post_ffs, absorbed, _, _, _ = count(netlist(lost), rtl=False)
        r, p = pre_ffs.get("u", 0), post_ffs.get("u", 0) + absorbed.get("u", 0)
        flagged = [i for i in set(pre_ffs) | set(post_ffs)
                   if pre_ffs.get(i, 0) >= 16 and post_ffs.get(i, 0) + absorbed.get(i, 0) < 0.5 * pre_ffs.get(i, 0)]
        ok = merged == 22 and r == 33 and flagged == (["u"] if lost else [])
        print(("ok   " if ok else "FAIL ") + ("a counter lost in synthesis: flagged" if lost else
              "a tag register merged into the memory read port: not counted, nothing flagged")
              + f" (RTL {r}, netlist {p}, merged {merged})")
        bad += not ok
    print("synthcheck self-test: " + ("PASS" if not bad else f"{bad} FAILED"))
    return 1 if bad else 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return self_test()
    ap = argparse.ArgumentParser(description="synthesis sanity check (see the file header)")
    ap.add_argument("--top", required=True)
    ap.add_argument("--pre", required=True, help="the elaborated RTL as Yosys JSON")
    ap.add_argument("--post", required=True, help="the synthesized netlist as Yosys JSON")
    ap.add_argument("--expect", help="minimum flip-flops per instance prefix")
    ap.add_argument("--min-ffs", type=int, default=16)
    ap.add_argument("--ratio", type=float, default=0.5)
    ap.add_argument("--global-ratio", type=float, default=0.8)
    args = ap.parse_args()

    pre, post = load(args.pre, args.top), load(args.post, args.top)
    pre_ffs, _, mem_bits, _, merged_bits = count(pre, rtl=True)
    post_ffs, absorbed, _, ram_bits, _ = count(post, rtl=False)
    problems = []

    print(f"{'instance':40s} {'RTL FFs':>8s} {'netlist FFs':>12s} {'+ held in DSP/RAM/SRL':>22s}")
    for inst in sorted(set(pre_ffs) | set(post_ffs) | set(absorbed)):
        r, p, a = pre_ffs.get(inst, 0), post_ffs.get(inst, 0), absorbed.get(inst, 0)
        flag = ""
        if r >= args.min_ffs and p + a < args.ratio * r:
            flag = "  LOST"
            problems.append(f"instance {inst}: {r} flip-flop bits in the RTL, {p} (+{a} held) after synthesis")
        print(f"{inst:40s} {r:8d} {p:12d} {a:22d}{flag}")
    tr, tp, ta = sum(pre_ffs.values()), sum(post_ffs.values()), sum(absorbed.values())
    print(f"{'whole design':40s} {tr:8d} {tp:12d} {ta:22d}")
    if tr and tp + ta < args.global_ratio * tr:
        problems.append(f"the design keeps {tp} (+{ta} held) of its {tr} RTL flip-flop bits")

    print(f"register bits already merged into memory read ports in the RTL (memory_dff; not counted): {merged_bits}")
    print(f"memories of 16 Kbit or more in the RTL: {mem_bits} bits; block RAM after synthesis: {ram_bits} bits")
    if mem_bits and ram_bits < 0.9 * mem_bits:
        problems.append(f"{mem_bits} bits of large RTL memories, but only {ram_bits} bits of block RAM")

    became = sorted(constant_outputs(post, rtl=False) - constant_outputs(pre, rtl=True))
    print(f"outputs constant after synthesis but not in the RTL: {len(became)}" + (": " + " ".join(became[:16]) if became else ""))
    if became:
        problems.append(f"{len(became)} output bits became constant in synthesis ({' '.join(became[:8])})")

    if args.expect:
        with open(args.expect) as f:
            for line in f:
                line = line.split("#", 1)[0].split()
                if len(line) != 2:
                    continue
                prefix, minimum = line[0], int(line[1])
                got = tp + ta if prefix == "." else sum(v for k, v in post_ffs.items() if k == prefix or k.startswith(prefix + "."))
                print(f"expected at least {minimum} flip-flops in {prefix}: {got}")
                if got < minimum:
                    problems.append(f"{prefix} has {got} flip-flops, expected at least {minimum}")

    for p in problems:
        print(f"PROBLEM: {p}")
    print("synthcheck: " + ("PASS" if not problems else f"FAIL ({len(problems)} problems)"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
