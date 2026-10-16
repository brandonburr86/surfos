#!/usr/bin/env python3
"""
SurfCPU MIG project files
--------------------
File: make_prj.py Date: 10/15/26 (SurfCPU)
--------------------
Makes ip/mig/<board>/mig_ddr3.prj, the MIG 7-series project file our recipe (mig_ddr3.tcl)
feeds to Vivado, from Digilent's file for that board (ip/mig/digilent/, kept unchanged, see
SOURCE.txt). Everything stays as Digilent configured it (memory part and timing, 3077 ps,
4:1, pins, I/O standards, termination, mode registers, internal Vref, XADC) except:

  1. ModuleName      design_1_mig_7series_0_0 -> mig_ddr3
  2. SystemClock     Single-Ended -> No Buffer, and the E3 pin entry goes: the board top
                     owns the 100 MHz oscillator's input buffer and gives the MIG the clock
                     from a global buffer, because the SoC's own clocks come from the same pin
                     and must not depend on the MIG
  3. Ordering        Normal -> Strict: commands are carried out in the order given (the bridge
                     issues one at a time, so this costs nothing and removes any doubt about a
                     read passing a write)
  4. PortInterface   AXI -> NATIVE, and the AXI parameters go: surf_ddr_bridge talks to the
                     native app_* interface

    python3 ip/mig/make_prj.py           write the derived files
    python3 ip/mig/make_prj.py --check   exit 1 if they are missing or out of date, or if the
                                         MIG configuration no longer fits the bridge's defaults

Each change must match exactly once, so a changed upstream file stops the script. The check
also reads the configuration back and compares it with surf_ddr_bridge's parameter defaults
(app_addr and app data widths) and the 81.25 MHz ui_clk that docs/DDR.md assumes.
"""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent                                  # surfcpu/
BRIDGE = ROOT / "rtl" / "soc" / "ddr" / "surf_ddr_bridge.v"
BOARDS = {
    "arty_a7_100t": ("digilent/arty-a7-100/mig.prj", "xc7a100t-csg324/-1"),
    "arty_a7_35t": ("digilent/arty-a7-35/mig.prj", "xc7a35ti-csg324/-1L"),
}
NOTE = ("<!-- SurfCPU: made from Digilent's vivado-boards file (ip/mig/digilent/SOURCE.txt) by "
        "ip/mig/make_prj.py; the four changes are listed there and in docs/DDR.md. -->")

CHANGES = [
    ("<ModuleName>design_1_mig_7series_0_0</ModuleName>", "<ModuleName>mig_ddr3</ModuleName>"),
    ("<SystemClock>Single-Ended</SystemClock>", "<SystemClock>No Buffer</SystemClock>"),
    ('    <System_Clock>\n      <Pin Bank="35" PADName="E3(MRCC_P)" name="sys_clk_i"/>\n    </System_Clock>\n', ""),
    ("<Ordering>Normal</Ordering>", "<Ordering>Strict</Ordering>"),
    (re.compile(r"<PortInterface>AXI</PortInterface>\n    <AXIParameters>\n.*?</AXIParameters>\n", re.S),
     "<PortInterface>NATIVE</PortInterface>\n"),
]


def derive(text: str) -> str:
    for old, new in CHANGES:
        if isinstance(old, str):
            n = text.count(old)
            if n != 1:
                raise SystemExit(f"make_prj: expected one {old[:50]!r}, found {n}")
            text = text.replace(old, new)
        else:
            text, n = old.subn(new, text)
            if n != 1:
                raise SystemExit(f"make_prj: expected one {old.pattern[:50]!r}, found {n}")
    anchor = "  <ModuleName>mig_ddr3</ModuleName>"
    if text.count(anchor) != 1:
        raise SystemExit("make_prj: cannot place the note")
    return text.replace(anchor, NOTE + "\n\n" + anchor)


def bridge_default(name: str) -> int:
    m = re.search(r"parameter integer\s+" + name + r"\s*=\s*(\d+)", BRIDGE.read_text())
    if not m:
        raise SystemExit(f"make_prj: no default for {name} in {BRIDGE}")
    return int(m.group(1))


def check_config(board: str, text: str, part: str) -> list[str]:
    """Read the derived file back and compare it with what the bridge and docs/DDR.md assume."""
    problems = []
    root = ET.fromstring(text.lstrip("﻿"))
    c = root.find("Controller")
    def g(e, tag): return (e.findtext(tag) or "").strip()
    expect = {
        (root, "TargetFPGA"): part, (root, "SystemClock"): "No Buffer", (root, "ReferenceClock"): "No Buffer",
        (root, "SysResetPolarity"): "ACTIVE LOW", (root, "Version"): "4.2", (c, "PHYRatio"): "4:1",
        (c, "TimePeriod"): "3077", (c, "DataWidth"): "16", (c, "DataMask"): "1", (c, "ECC"): "Disabled",
        (c, "Ordering"): "Strict", (c, "PortInterface"): "NATIVE", (c, "C0_MEM_SIZE"): "268435456",
        (c, "MemoryDevice"): "DDR3_SDRAM/Components/MT41K128M16XX-15E", (c, "MemoryVoltage"): "1.35V",
        (c, "mrBurstLength"): "8 - Fixed",
    }
    for (e, tag), want in expect.items():
        if g(e, tag) != want:
            problems.append(f"{board}: {tag} is {g(e, tag)!r}, expected {want!r}")
    if c.find("AXIParameters") is not None or c.find("System_Clock") is not None:
        problems.append(f"{board}: AXI parameters or a system clock pin left in")
    pins = c.find("PinSelection")
    if pins is None or len(pins) != 48:
        problems.append(f"{board}: expected the 48 DDR3 pins of Digilent's file")
    # MIG native UI widths: app_addr = rank + bank + row + column bits (one rank counts one bit),
    # app data = 2 x nCK_PER_CLK x DQ width (BL8 in one beat at 4:1)
    addr_w = 1 + int(g(c, "BankAddress")) + int(g(c, "RowAddress")) + int(g(c, "ColAddress"))
    data_w = 2 * 4 * int(g(c, "DataWidth"))
    for name, value in (("APP_ADDR_WIDTH", addr_w), ("APP_DATA_WIDTH", data_w), ("DQ_WIDTH", int(g(c, "DataWidth")))):
        if bridge_default(name) != value:
            problems.append(f"{board}: MIG gives {name} = {value}, surf_ddr_bridge defaults to {bridge_default(name)}")
    mem_bytes = (1 << (addr_w - 1)) * int(g(c, "DataWidth")) // 8
    if mem_bytes != int(g(c, "C0_MEM_SIZE")):
        problems.append(f"{board}: address bits give {mem_bytes} bytes, C0_MEM_SIZE says {g(c, 'C0_MEM_SIZE')}")
    ui_mhz = 1e6 / int(g(c, "TimePeriod")) / 4
    if abs(ui_mhz - 81.25) > 0.01:
        problems.append(f"{board}: ui_clk would be {ui_mhz:.3f} MHz, docs/DDR.md assumes 81.25")
    return problems


def main() -> int:
    check = "--check" in sys.argv[1:]
    problems = []
    for board, (src, part) in BOARDS.items():
        text = (HERE / src).read_text(encoding="utf-8")
        out = derive(text)
        path = HERE / board / "mig_ddr3.prj"
        problems += check_config(board, out, part)
        if check:
            if not path.exists() or path.read_text(encoding="utf-8") != out:
                problems.append(f"{path.relative_to(ROOT)} is missing or out of date (python3 ip/mig/make_prj.py)")
        else:
            path.parent.mkdir(exist_ok=True)
            path.write_text(out, encoding="utf-8")
            print(f"make_prj: wrote {path.relative_to(ROOT)}")
    for p in problems:
        print("make_prj: " + p)
    if not problems and check:
        print("make_prj: ip/mig/*/mig_ddr3.prj up to date; MIG native UI 28-bit app_addr, 128-bit data, "
              "ui_clk 81.25 MHz match surf_ddr_bridge")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
