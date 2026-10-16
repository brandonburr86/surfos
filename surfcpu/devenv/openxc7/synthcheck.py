#!/usr/bin/env python3
"""
SurfCPU synthesis sanity check
--------------------
File: synthcheck.py Date: 10/15/26 (SurfCPU)
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

With --expect FILE, lines "<instance prefix> <minimum flip-flops>" ('.' for the whole
design, '#' starts a comment) must hold as well. Only the standard library is used.

    synthcheck.py --top flow_demo --pre pre.json --post flow_demo.json [--expect file]
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
    """bit -> the shortest public net name carrying it (hierarchical names keep the path)."""
    names: dict = {}
    for name, net in module.get("netnames", {}).items():
        if net.get("hide_name"):
            continue
        for b in net["bits"]:
            if isinstance(b, int) and (b not in names or len(name) < len(names[b])):
                names[b] = name
    return names


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


def count(module: dict, rtl: bool) -> tuple[dict, dict, int, int]:
    """(flip-flop bits per instance, absorbable bits per instance, memory bits, RAM bits)."""
    names = namer(module)
    ffs: dict = defaultdict(int)
    absorbed: dict = defaultdict(int)
    mem_bits = ram_bits = 0
    for cell in module.get("cells", {}).values():
        t, conns = cell["type"], cell.get("connections", {})
        if rtl and t in RTL_FF:
            for b in conns.get("Q", []):
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
    return ffs, absorbed, mem_bits, ram_bits


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


def main() -> int:
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
    pre_ffs, _, mem_bits, _ = count(pre, rtl=True)
    post_ffs, absorbed, _, ram_bits = count(post, rtl=False)
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
