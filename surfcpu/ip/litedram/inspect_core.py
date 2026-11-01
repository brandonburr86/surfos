#!/usr/bin/env python3
"""
SurfCPU LiteDRAM core report
--------------------
File: inspect_core.py Date: 10/16/26 (SurfCPU)
--------------------
Says what ip/litedram/generate.sh produced, for docs/DDR.md and the open flow: for each core
under build/litedram/<name>/ the top-level ports, the vendor primitives it instantiates (with
counts), the modules it needs from elsewhere, the PLL settings, the constraint file's contents
and the files' SHA-256. It also checks the facts the RTL and the firmware rely on:
  * the bridge and the adapter: a 24-bit beat address, 128-bit data with 16 byte enables,
    init_done and init_error, user_clk/user_rst, and no vendor primitive in the simulation core;
  * surf_ddr_csr: the Wishbone control port (wb_ctrl_*, 30-bit word address, 32-bit data), and
    no CPU in either core;
  * the firmware's DDR initialization (firmware/common/ddrinit.c): every register of
    spec/soc.toml's ddrc block at the offset and with the access the generator's csr.csv gives
    it, and every register of csr.csv in the block; the PHY settings and the mode register values
    of firmware/common/ddrinit_phy.h equal to the generated sdram_phy.h's (SDRAM_PHY_* and
    init_sequence());
  * the simulation core: generated from the same configuration, the same DRAM initialization
    sequence and the same DFII and init registers (its PHY model has no PHY registers), the PHY
    settings that differ listed; and the board top's lint stand-in (sim/board/stubs/
    litedram_core.v) with exactly the board core's ports.

    . build/third_party/litedram/env.sh; python3 ip/litedram/inspect_core.py build/litedram
    (make litedram-gen, from surfcpu/)
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
    for extra in (core / "gateware" / f"{module}.xdc", core / "csr.csv",
                  core / "software" / "include" / "generated" / "sdram_phy.h"):
        if extra.exists():
            lines.append(f"{extra.relative_to(core)}: {extra.stat().st_size} bytes, sha256 {sha(extra)}")
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
    for name, width in (("wb_ctrl_adr", "[29:0]"), ("wb_ctrl_dat_w", "[31:0]"), ("wb_ctrl_dat_r", "[31:0]"),
                        ("wb_ctrl_sel", "[3:0]"), ("wb_ctrl_cyc", ""), ("wb_ctrl_stb", ""), ("wb_ctrl_we", ""),
                        ("wb_ctrl_cti", "[2:0]"), ("wb_ctrl_bte", "[1:0]"), ("wb_ctrl_ack", ""), ("wb_ctrl_err", "")):
        if names.get(name) != width:
            problems.append(f"{name} is {names.get(name, 'missing')!r}, surf_ddr_csr expects {width or 'one bit'}")
    if re.search(r"VexRiscv|uart_tx|\bcpu_", text):
        problems.append("the core has a CPU or a console: ip/litedram/arty.yml must say cpu None (docs/DDR.md)")
    lines += [f"PROBLEM: {p}" for p in problems] or [
        "RTL assumptions: hold (24-bit beat address, 128-bit data, 16 byte enables; wb_ctrl control port, no CPU)"]
    return lines


def csr_registers(core: Path) -> dict[str, tuple[int, str]]:
    """csr.csv's registers: name -> (byte offset, access)."""
    f = core / "csr.csv"
    regs = {}
    if f.exists():
        for line in f.read_text().splitlines():
            parts = line.split(",")
            if parts[0] == "csr_register":
                regs[parts[1]] = (int(parts[2], 16), parts[4])
    return regs


def soc_ddrc() -> list[tuple[str, int, str, str]]:
    """spec/soc.toml's ddrc registers: (name, offset, access, LiteDRAM register as csr.csv names it)."""
    import tomllib
    soc = tomllib.loads(Path("spec/soc.toml").read_text())
    out = []
    for periph in soc.get("periph", []):
        if periph["name"] == "ddrc":
            for reg in periph["reg"]:
                m = re.match(r"(\w+)\.(\w+):", reg["desc"])
                out.append((reg["name"], reg["offset"], reg["access"], f"{m.group(1)}_{m.group(2)}" if m else "?"))
    return out


# ddrinit_phy.h's names for the generated sdram_phy.h's settings
FIRMWARE_PHY = {"PHASES": "PHASES", "MODULES": "MODULES", "DELAYS": "DELAYS", "BITSLIPS": "BITSLIPS",
                "DFI_DATABITS": "DFI_DATABITS", "XDR": "XDR", "DQ_DQS_RATIO": "DQ_DQS_RATIO",
                "RDPHASE": "RDPHASE", "WRPHASE": "WRPHASE", "CL": "CL", "CWL": "CWL"}


def firmware(out: Path) -> list[str]:
    """What the firmware's DDR initialization assumes, against the board core."""
    core = out / "arty"
    lines = ["== the firmware's DDR initialization (firmware/common/ddrinit*.[ch]) against the board core"]
    problems = []
    regs = csr_registers(core)
    if not regs:
        return lines + [f"PROBLEM: {core / 'csr.csv'} missing (make litedram-gen)"]
    listed = soc_ddrc()
    for name, off, acc, csr in listed:
        if csr not in regs:
            problems.append(f"spec/soc.toml ddrc.{name} is {csr}, which the core does not have")
        elif regs[csr] != (off, acc):
            problems.append(f"spec/soc.toml ddrc.{name} is at 0x{off:x} ({acc}); the core has {csr} at "
                            f"0x{regs[csr][0]:x} ({regs[csr][1]})")
    missing = sorted(set(regs) - {csr for _, _, _, csr in listed})
    if missing:
        problems.append("csr.csv registers spec/soc.toml's ddrc block does not list: " + ", ".join(missing))
    if not problems:
        lines.append(f"register map: spec/soc.toml's ddrc block lists the core's {len(regs)} registers at their "
                     "offsets and with their access (csr.csv)")
    hdr = Path("firmware/common/ddrinit_phy.h").read_text()
    fw = {m.group(1): int(m.group(2), 0) for m in re.finditer(r"^#define DDRINIT_(\w+)\s+(0x[0-9a-fA-F]+|\d+)", hdr, re.M)}
    gen = phy_defines(core)
    bad = []
    for fwname, genname in FIRMWARE_PHY.items():
        g = gen.get("SDRAM_PHY_" + genname, "")
        if fwname not in fw or not re.fullmatch(r"\d+", g) or fw[fwname] != int(g):
            bad.append(f"{fwname} {fw.get(fwname, 'missing')} (generated: {g or 'missing'})")
    seq = init_sequence(core)
    # init_sequence(): each mode register load is an address write, a bank write and command_p0(RAS|CAS|WE|CS)
    loads = re.findall(r"sdram_dfii_pi0_address_write\((0x[0-9a-fA-F]+)\);\s*sdram_dfii_pi0_baddress_write\((\d+)\);"
                       r"\s*command_p0\(([A-Z_|]+)\);", seq)
    mrs = [(int(a, 16), int(b)) for a, b, c in loads if c == "DFII_COMMAND_RAS|DFII_COMMAND_CAS|DFII_COMMAND_WE|DFII_COMMAND_CS"]
    zq = [(int(a, 16), int(b)) for a, b, c in loads if c == "DFII_COMMAND_WE|DFII_COMMAND_CS"]
    want = [(fw.get(f"MR{n}"), n) for n in (2, 3, 1, 0)]
    if mrs != want:
        bad.append(f"mode registers (value, register) in order {want} (generated: {mrs})")
    if zq != [(fw.get("ZQCL"), 0)]:
        bad.append(f"ZQ calibration address {fw.get('ZQCL')} (generated: {zq})")
    if len(loads) != 5:
        bad.append(f"init_sequence() has {len(loads)} commands, ddrinit.c sends 5 (four mode registers, ZQCL)")
    if bad:
        problems.append("firmware/common/ddrinit_phy.h differs from the generated sdram_phy.h: " + "; ".join(bad))
    else:
        lines.append(f"PHY settings and init_sequence: ddrinit_phy.h equals sdram_phy.h ({len(FIRMWARE_PHY)} settings; "
                     "MR2, MR3, MR1, MR0 and ZQCL in that order)")
    return lines + [f"PROBLEM: {p}" for p in problems]


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
    lines = ["== the simulation core (arty_sim) against the bitstream's core (arty)",
             "configuration: both generated from ip/litedram/arty.yml as it is (generate.sh, --sim for the model)"]
    problems = []
    ra, rb = csr_registers(out / "arty"), csr_registers(out / "arty_sim")
    phy_only = sorted(k for k in ra if k.startswith("ddrphy_"))
    shared = sorted(k for k in ra if not k.startswith("ddrphy_"))
    if any(rb.get(k) != ra[k] for k in shared):
        problems.append("the simulation core's DFII and init registers differ from the board core's: " +
                        ", ".join(k for k in shared if rb.get(k) != ra[k]))
    else:
        lines.append(f"registers: the simulation core has the board core's {len(shared)} DFII and init registers at the "
                     f"same offsets; of its {len(phy_only)} PHY registers it has none (the model has no delays)")
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
    allx += firmware(out)
    allx += compare(out)
    print("\n".join(allx))
    return 1 if any(l.startswith("PROBLEM") for l in allx) else 0


if __name__ == "__main__":
    sys.exit(main())
