#!/usr/bin/env python3
"""
SurfCPU LiteDRAM core report
--------------------
File: inspect_core.py Date: 10/15/26 (SurfCPU)
--------------------
Says what ip/litedram/generate.sh produced, for docs/DDR.md and the open flow: for each core
under build/litedram/<name>/ the top-level ports, the vendor primitives it instantiates (with
counts), the modules it needs from elsewhere, the PLL settings, the calibration BIOS's size, the
constraint file's contents and the files' SHA-256. It also checks the facts the bridge and the
adapter rely on: a 24-bit beat address, 128-bit data with 16 byte enables, init_done and
init_error, user_clk/user_rst, and no vendor primitive in the simulation core.

    python3 ip/litedram/inspect_core.py build/litedram     (make litedram-gen)
"""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

PRIMS = ["ISERDESE2", "OSERDESE2", "IDELAYE2", "ODELAYE2", "IDELAYCTRL", "IOBUF", "IOBUFDS", "OBUFDS",
         "OBUFTDS", "OBUF", "IBUF", "IBUFDS", "IBUFG", "BUFG", "BUFGCE", "BUFIO", "BUFR", "PLLE2_ADV",
         "PLLE2_BASE", "MMCME2_ADV", "MMCME2_BASE", "FDCE", "FDPE", "FDRE", "FDSE", "RAMB18E1",
         "RAMB36E1", "SRL16E", "XADC", "STARTUPE2"]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def report(core: Path) -> list[str]:
    v = core / "gateware" / "litedram_core.v"
    if not v.exists():
        return [f"{core.name}: not generated ({v} missing)"]
    text = v.read_text()
    lines = [f"== {core.name}: {v.relative_to(core.parent.parent)} ({v.stat().st_size} bytes, sha256 {sha(v)})"]
    m = re.search(r"^module litedram_core \((.*?)\);", text, re.S | re.M)
    ports = re.findall(r"(input|output|inout)\s+(?:wire|reg)\s*(\[\d+:\d+\])?\s*(\w+)", m.group(1)) if m else []
    lines.append("ports: " + ", ".join(f"{d} {w or ''}{n}".replace("  ", " ") for d, w, n in ports))
    counts = {p: len(re.findall(r"^\s*" + p + r"\b", text, re.M)) for p in PRIMS}
    used = {p: c for p, c in counts.items() if c}
    lines.append("vendor primitives: " + (", ".join(f"{p} x{c}" for p, c in used.items()) or "none"))
    defined = set(re.findall(r"^module (\w+)", text, re.M))
    inst = set(re.findall(r"^(\w+)\s+(?:#\(|\w+\s*\()", text, re.M)) - defined - set(PRIMS) - {"module", "assign"}
    if inst:
        lines.append("modules needed from elsewhere: " + ", ".join(sorted(inst)))
    pll = re.search(r"PLLE2_ADV #\((.*?)\)\s*PLLE2_ADV", text, re.S)
    if pll:
        params = dict(re.findall(r"\.(\w+)\s*\(([^)]*)\)", pll.group(1)))
        def num(x): return float(re.sub(r"^\d+'d", "", params[x].strip()))
        vco = 1000.0 / num("CLKIN1_PERIOD") * num("CLKFBOUT_MULT") / num("DIVCLK_DIVIDE")
        outs = []
        for i in range(6):
            if f"CLKOUT{i}_DIVIDE" in params:
                outs.append(f"CLKOUT{i} {vco / num(f'CLKOUT{i}_DIVIDE'):.1f} MHz @{num(f'CLKOUT{i}_PHASE'):g} deg")
        lines.append(f"PLL: input {1000.0 / num('CLKIN1_PERIOD'):.1f} MHz, VCO {vco:.0f} MHz; " + "; ".join(outs))
    log = core / "generate.log"
    if log.exists():
        rom = re.search(r"ROM usage: ([^\n]+)", log.read_text())
        if rom:
            lines.append("calibration BIOS: ROM usage " + " ".join(rom.group(1).split()))
    for extra in ("litedram_core_rom.init", "litedram_core.xdc"):
        f = core / "gateware" / extra
        if f.exists():
            lines.append(f"{extra}: {f.stat().st_size} bytes, sha256 {sha(f)}")
    xdc = core / "gateware" / "litedram_core.xdc"
    if xdc.exists():
        cmds = [l.split()[0] for l in xdc.read_text().splitlines() if l.strip() and not l.startswith("#")]
        lines.append("constraints shipped: " + (", ".join(f"{c} x{cmds.count(c)}" for c in sorted(set(cmds))) or "none") +
                     " (no pins, no I/O standards, no clocks: the board's XDC supplies them)")
    # what the bridge and the adapter rely on
    names = {n: w for _, w, n in ports}
    problems = []
    for name, width in (("user_port_native_0_cmd_addr", "[23:0]"), ("user_port_native_0_wdata_data", "[127:0]"),
                        ("user_port_native_0_wdata_we", "[15:0]"), ("user_port_native_0_rdata_data", "[127:0]"),
                        ("init_done", ""), ("init_error", ""), ("user_clk", ""), ("user_rst", "")):
        if names.get(name) != width:
            problems.append(f"{name} is {names.get(name, 'missing')!r}, the adapter expects {width or 'one bit'}")
    if "sim" in core.name and used:
        problems.append("the simulation core instantiates vendor primitives")
    lines += [f"PROBLEM: {p}" for p in problems] or ["adapter assumptions: hold (24-bit beat address, 128-bit data, 16 byte enables)"]
    return lines


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "build/litedram")
    allx = []
    for name in ("arty", "arty_sim"):
        allx += report(out / name)
    vex = Path("build/third_party/litedram/src/pythondata-cpu-vexriscv/pythondata_cpu_vexriscv/verilog/VexRiscv.v")
    if vex.exists():
        allx.append(f"VexRiscv.v (the arty core's calibration CPU, read beside it): sha256 {sha(vex)}")
    print("\n".join(allx))
    return 1 if any(l.startswith("PROBLEM") for l in allx) else 0


if __name__ == "__main__":
    sys.exit(main())
