#!/usr/bin/env python3
"""SurfGPU: where an open-flow build put the GPU (docs/GPU.md section 12.3).

    placement.py --build DIR --tilegrid TILEGRID.json [--out FILE]

DIR is an open-flow build directory (build/fpga/<board>/<design>/openxc7) with the Yosys netlist
(surfcpu_arty_a7_top.json) and nextpnr's placement (placement.json); TILEGRID is Project X-Ray's
tile grid of the part (tilegrid.json: each tile's grid position and clock region). Every placed
cell is given to a part of the design by the RTL file Yosys recorded for it (or, for a cell
nextpnr made, by the hierarchical name it carries): VRAM (surf_vram*.v), the engine
(surf_gpu.v, surf_fifo.v under it), the video block (surf_video*.v), the CPU (rtl/cpu), the
rest. For each part it prints the cells by kind, the clock regions they sit in, and the
bounding box and centre of their tiles on the grid. Only the Python standard library.
"""

import argparse
import collections
import json
import re
import sys

PARTS = [("vram", ("rtl/soc/surf_vram",), ("u_vram",)),
         ("engine", ("rtl/peripherals/surf_gpu",), ("u_gpu",)),
         ("video", ("rtl/peripherals/surf_video",), ("u_video",)),
         ("cpu", ("rtl/cpu/",), ("u_core",)),
         ("ddr", ("rtl/soc/ddr/", "litedram"), ("u_dram", "u_ddr")),
         ("other", ("",), ())]


def part_of(src: str, name: str) -> str:
    files = [p.split(":")[0] for p in (src or "").split("|")]
    for part, prefixes, _ in PARTS[:-1]:
        if any(f.startswith(px) or px in f for f in files for px in prefixes):
            return part
    for part, _, names in PARTS[:-1]:
        if any("." + n + "." in "." + name + "." or name.startswith(n + ".") or ("." + n) in name for n in names):
            return part
    return "other"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--tilegrid", required=True)
    ap.add_argument("--out")
    a = ap.parse_args()
    net = json.load(open(a.build + "/surfcpu_arty_a7_top.json"))
    top = net["modules"]["surfcpu_arty_a7_top"]
    cells = top["cells"]
    place = json.load(open(a.build + "/placement.json"))
    grid = json.load(open(a.tilegrid))

    kinds = collections.defaultdict(collections.Counter)      # part -> kind -> count
    regions = collections.defaultdict(collections.Counter)    # part -> clock region -> count
    boxes = {}
    sums = collections.defaultdict(lambda: [0, 0, 0])
    for pk, v in place.items():
        tile = v.get("tile")
        t = grid.get(tile)
        if not t:
            continue
        base = pk.split("/")[0]
        c = cells.get(base) or cells.get(re.sub(r"\$split\$.*$", "", base))
        src = c.get("attributes", {}).get("src", "") if c else ""
        part = part_of(src, base)
        ty = v.get("type", "?")
        kind = ("RAMB36" if "RAMB36" in ty else "RAMB18" if "RAMB18" in ty else "LUT" if "LUT" in ty
                else "FF" if "FF" in ty or ty.startswith("FD") else "CARRY" if "CARRY" in ty else
                "DSP" if "DSP" in ty else "IO" if "IOB" in ty or "PAD" in ty else ty)
        kinds[part][kind] += 1
        regions[part][t.get("clock_region", "?")] += 1
        x, y = t["grid_x"], t["grid_y"]
        b = boxes.get(part, [x, y, x, y])
        boxes[part] = [min(b[0], x), min(b[1], y), max(b[2], x), max(b[3], y)]
        s = sums[part]
        s[0] += x; s[1] += y; s[2] += 1

    W = 1 + max(t["grid_x"] for t in grid.values())
    H = 1 + max(t["grid_y"] for t in grid.values())
    lines = ["# placement of %s (grid %d x %d tiles; grid_y 0 is the top of the die)" % (a.build, W, H)]
    for part, _, _ in PARTS:
        if part not in kinds:
            continue
        k = kinds[part]
        b = boxes[part]
        s = sums[part]
        regs = ", ".join("%s %d" % (r, n) for r, n in sorted(regions[part].items()))
        lines.append("%-6s %s" % (part, ", ".join("%s %d" % kv for kv in sorted(k.items()))))
        lines.append("       clock regions: %s" % regs)
        lines.append("       tiles x %d..%d, y %d..%d (centre %.0f, %.0f)" % (b[0], b[2], b[1], b[3], s[0] / s[2], s[1] / s[2]))
    text = "\n".join(lines) + "\n"
    if a.out:
        open(a.out, "w").write(text)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
