#!/usr/bin/env python3
"""SurfCPU: Yosys cell models without the block RAM shells
--------------------
File: strip_cells.py Date: 10/15/26 (SurfCPU)
--------------------
Copies Yosys's techlibs/xilinx/cells_sim.v without the RAMB18E1 and RAMB36E1 modules, which in
Yosys 0.52 are timing shells without behaviour, so that sim/soc/synth/surf_ramb36e1_model.v can
stand in for RAMB36E1 in the post-synthesis simulations (make test-soc-gate).

    python3 sim/soc/synth/strip_cells.py CELLS_SIM.v OUT.v
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
drop = ("RAMB18E1", "RAMB36E1")
out = []
skipping = False
for line in text.splitlines(keepends=True):
    m = re.match(r"\s*module\s+(\w+)", line)
    if m and m.group(1) in drop:
        skipping = True
        # attributes written on the lines just before the module belong to it too
        while out and out[-1].lstrip().startswith("(*"):
            out.pop()
    if not skipping:
        out.append(line)
    if skipping and re.match(r"\s*endmodule\b", line):
        skipping = False
open(dst, "w").write("".join(out))
