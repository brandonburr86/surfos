#!/usr/bin/env python3
"""
SurfCPU: which register bits survive synthesis
--------------------
File: synth_regs.py Date: 10/15/26 (SurfCPU)
--------------------
Reads a netlist Yosys wrote (write_verilog -noattr) after synth_xilinx without -flatten,
and prints, for each module of the core, how many flip-flops (FDRE/FDSE/FDCE/FDPE) remain
for each register, by the name of the net on their Q output. Comparing this with the RTL's
declarations shows whether synthesis dropped anything: a register narrower than declared
must be explained by bits that are provably constant. `make synth-cpu-regs` runs it.

    synth_regs.py NETLIST
"""

from __future__ import annotations

import collections
import re
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    text = Path(sys.argv[1]).read_text()
    total = 0
    for chunk in re.split(r"\nmodule ", "\n" + text)[1:]:
        name = chunk.split("(")[0].strip().lstrip("\\")
        ffs = re.findall(r"\b(FD[RSCP]E)\s+(?:#\(.*?\)\s+)?\S+\s*\((.*?)\);", chunk, re.S)
        if not ffs:
            continue
        widths: collections.Counter[str] = collections.Counter()
        kinds: collections.Counter[str] = collections.Counter()
        for kind, body in ffs:
            q = re.search(r"\.Q\(([^)]*)\)", body)
            net = re.sub(r"\s*\[\d+\]$", "", q.group(1).strip()) if q else "?"
            widths[net] += 1
            kinds[kind] += 1
        total += len(ffs)
        print(f"{name}: {len(ffs)} flip-flops ({', '.join(f'{k} {v}' for k, v in sorted(kinds.items()))})")
        for net, n in sorted(widths.items()):
            print(f"    {net:<24} {n}")
    print(f"total: {total} flip-flops")
    return 0


if __name__ == "__main__":
    sys.exit(main())
