#!/usr/bin/env python3
"""
SurfCPU core synthesis summary
--------------------
File: synth_summary.py Date: 10/15/26 (SurfCPU)
--------------------
Reads the `stat` report Yosys writes after `synth_xilinx -flatten` of surfcpu_core alone
(make synth-cpu) and prints the resource estimate on one line: logic LUTs, LUTs used as
distributed RAM, flip-flops, carry chains, wide muxes, DSP48E1 and block RAM. These are
Yosys's mapping estimates for a 7-series part, not Vivado post-route utilisation.

    synth_summary.py build/cpu/synth-stat.txt [LABEL]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    label = sys.argv[2] if len(sys.argv) == 3 else "synth-cpu: surfcpu_core"
    text = Path(sys.argv[1]).read_text()
    # with -flatten there is one module; take the last block of cell counts
    block = text.split("=== ")[-1]
    cells: dict[str, int] = {}
    for m in re.finditer(r"^\s+([A-Z][A-Z0-9_]+)\s+(\d+)\s*$", block, re.M):
        cells[m.group(1)] = cells.get(m.group(1), 0) + int(m.group(2))
    luts = sum(cells.get(f"LUT{i}", 0) for i in range(1, 7))
    lutram_cells = {"RAM32M": 4, "RAM64M": 4, "RAM32X1D": 2, "RAM64X1D": 2, "RAM32X1S": 1, "RAM64X1S": 1,
                    "RAM128X1D": 4, "RAM256X1S": 4}
    lutram = sum(cells.get(k, 0) * v for k, v in lutram_cells.items())
    ffs = sum(cells.get(k, 0) for k in ("FDRE", "FDSE", "FDCE", "FDPE"))
    bram36 = cells.get("RAMB36E1", 0) + cells.get("RAMB18E1", 0) / 2
    print(f"{label} (Yosys synth_xilinx -family xc7, estimate): "
          f"{luts} LUTs logic + {lutram} LUTs as distributed RAM, {ffs} FFs, "
          f"{cells.get('CARRY4', 0)} CARRY4, {cells.get('MUXF7', 0)} MUXF7 + {cells.get('MUXF8', 0)} MUXF8, "
          f"{cells.get('DSP48E1', 0)} DSP48E1, {bram36:g} BRAM36")
    return 0


if __name__ == "__main__":
    sys.exit(main())
