#!/usr/bin/env python3
"""
SurfCPU LiteDRAM core report
--------------------
File: inspect_core.py Date: 10/16/26 (SurfCPU)
--------------------
Says what ip/litedram/generate.sh produced, for docs/DDR.md and the open flow: for each core
under build/litedram/<name>/ the top-level ports, the vendor primitives it instantiates (with
counts), the modules it needs from elsewhere, the PLL settings, the calibration BIOS's size, the
constraint file's contents and the files' SHA-256. It also checks the facts the bridge and the
adapter rely on: a 24-bit beat address, 128-bit data with 16 byte enables, init_done and
init_error, user_clk/user_rst, and no vendor primitive in the simulation core. Then it compares
the two cores: the simulation core's configuration must be the board's (ip/litedram/arty.yml)
but for "cpu" and "uart" (ip/litedram/sim_config.py), the DRAM initialization sequences (mode
registers) must be the same, the PHY settings that differ are listed, and the board top's lint
stand-in (sim/board/stubs/litedram_core.v) must have exactly the board core's ports.

    . build/third_party/litedram/env.sh; python3 ip/litedram/inspect_core.py build/litedram
    (make litedram-gen; PyYAML from the pinned generator environment)
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


def ports_of(text: str, module: str) -> list[tuple[str, str, str]]:
    """[(direction, range or '', name)] of an ANSI module header."""
    m = re.search(r"^module " + module + r" \((.*?)\);", text, re.S | re.M)
    return re.findall(r"(input|output|inout)\s+(?:wire|reg)\s*(\[\d+:\d+\])?\s*(\w+)", m.group(1)) if m else []


def report(core: Path, module: str) -> list[str]:
    v = core / "gateware" / f"{module}.v"
    if not v.exists():
        return [f"{core.name}: not generated ({v} missing)"]
    text = v.read_text()
    lines = [f"== {core.name}: {v.relative_to(core.parent.parent)} ({v.stat().st_size} bytes, sha256 {sha(v)})"]
    ports = ports_of(text, module)
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
    for extra in (f"{module}_rom.init", f"{module}.xdc"):
        f = core / "gateware" / extra
        if f.exists():
            lines.append(f"{extra}: {f.stat().st_size} bytes, sha256 {sha(f)}")
    xdc = core / "gateware" / f"{module}.xdc"
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


def phy_defines(core: Path) -> dict[str, str]:
    h = core / "software" / "include" / "generated" / "sdram_phy.h"
    if not h.exists():
        return {}
    return {m.group(1): m.group(2).strip() for m in re.finditer(r"^#define (SDRAM_PHY_\w+)(.*)$", h.read_text(), re.M)}


def init_sequence(core: Path) -> str:
    h = core / "software" / "include" / "generated" / "sdram_phy.h"
    m = re.search(r"static inline void init_sequence\(void\)\s*\{.*?^\}", h.read_text(), re.S | re.M) if h.exists() else None
    return m.group(0) if m else ""


def compare(out: Path) -> list[str]:
    """The simulation core against the bitstream's core: what is the same, what differs."""
    lines = ["== the simulation core (arty_sim) against the bitstream's core (arty)"]
    problems = []
    try:
        import yaml
    except ImportError:
        return lines + ["PROBLEM: PyYAML missing: run with the pinned environment (. build/third_party/litedram/env.sh)"]
    board_cfg = Path("ip/litedram/arty.yml")
    sim_cfg = out / "arty_sim" / "config.yml"
    if not sim_cfg.exists():
        return lines + [f"PROBLEM: {sim_cfg} missing (make litedram-gen)"]
    a = yaml.load(board_cfg.read_text(), Loader=yaml.Loader)
    b = yaml.load(sim_cfg.read_text(), Loader=yaml.Loader)
    differ = sorted(k for k in set(a) | set(b) if a.get(k, "<absent>") != b.get(k, "<absent>"))
    lines.append(f"configuration: {sim_cfg} is {board_cfg} but for: " +
                 ", ".join(f"{k} {a.get(k, '<absent>')!r} -> {b.get(k, '<absent>')!r}" for k in differ))
    if differ != ["cpu", "uart"]:
        problems.append(f"the simulation configuration differs from {board_cfg} in {differ}, not just cpu and uart")
    pa, pb = phy_defines(out / "arty"), phy_defines(out / "arty_sim")
    same = sorted(k for k in pa if pb.get(k) == pa[k])
    diffs = sorted(k for k in set(pa) | set(pb) if pa.get(k) != pb.get(k))
    lines.append("PHY settings the same (sdram_phy.h): " + ", ".join(f"{k[10:]}{'=' + pa[k] if pa[k] else ''}" for k in same))
    lines.append("PHY settings that differ (board -> simulation): " +
                 ", ".join(f"{k[10:]} {pa.get(k, '<absent>') or 'set'} -> {pb.get(k, '<absent>') or 'set'}" for k in diffs))
    sa, sb = init_sequence(out / "arty"), init_sequence(out / "arty_sim")
    if sa and sa == sb:
        n = len(re.findall(r"Load Mode Register", sa))
        lines.append(f"DRAM initialization sequence: identical ({n} mode register loads, the same values and waits)")
    else:
        problems.append("the DRAM initialization sequences (sdram_phy.h init_sequence) differ")
    # the board top's lint stand-in has exactly the board core's ports
    stub = Path("sim/board/stubs/litedram_core.v")
    core_v = out / "arty" / "gateware" / "litedram_core.v"
    if stub.exists() and core_v.exists():
        ps = ports_of(stub.read_text(), "litedram_core")
        pc = ports_of(core_v.read_text(), "litedram_core")
        if sorted(ps) == sorted(pc):
            lines.append(f"{stub}: the board core's {len(pc)} ports exactly")
        else:
            problems.append(f"{stub} and {core_v} have different ports: " + str(sorted(set(ps) ^ set(pc))))
    return lines + [f"PROBLEM: {p}" for p in problems]


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "build/litedram")
    allx = []
    allx += report(out / "arty", "litedram_core")
    allx += report(out / "arty_sim", "litedram_sim")
    vex = Path("build/third_party/litedram/src/pythondata-cpu-vexriscv/pythondata_cpu_vexriscv/verilog/VexRiscv_Lite.v")
    if vex.exists():
        allx.append(f"VexRiscv_Lite.v (the arty core's calibration CPU, the lite variant, read beside it): sha256 {sha(vex)}")
    allx += compare(out)
    print("\n".join(allx))
    return 1 if any(l.startswith("PROBLEM") for l in allx) else 0


if __name__ == "__main__":
    sys.exit(main())
