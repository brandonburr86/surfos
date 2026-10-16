#!/usr/bin/env python3
"""
SurfCPU Arty A7 resource estimates
--------------------
File: estimate.py Date: 10/15/26 (SurfCPU)
--------------------
Synthesizes the board designs and the SoC blocks present on this branch with Yosys
`synth_xilinx -family xc7`, counts what they use, and sets it against the two Arty parts,
together with the block RAM budget the memory map implies (docs/BOARD.md section 10).

    python3 boards/arty_a7/estimate.py [OUT]     (default: print; make estimate-board writes
                                                  docs/evidence/board/resources.txt)

Yosys is not Vivado: its counts are estimates (Vivado usually needs fewer LUTs after its own
optimization, and packs differently). The device capacities are AMD DS180 v2.6.1 table 4:
XC7A35T 5,200 slices (20,800 LUTs, 41,600 flip-flops), 50 RAMB36 (1,800 Kb), 90 DSP48E1,
5 CMTs; XC7A100T 15,850 slices (63,400 LUTs, 126,800 flip-flops), 135 RAMB36 (4,860 Kb),
240 DSP48E1, 6 CMTs (each slice 4 LUTs and 8 flip-flops; each CMT one MMCM and one PLL).
Only the Python standard library is used.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent      # surfcpu/
BD = ROOT / "boards" / "arty_a7"

PARTS = {   # DS180 v2.6.1 table 4
    "XC7A35T (Arty A7-35T)": {"LUT": 20800, "FF": 41600, "RAMB36": 50, "DSP": 90, "CMT": 5},
    "XC7A100T (Arty A7-100T)": {"LUT": 63400, "FF": 126800, "RAMB36": 135, "DSP": 240, "CMT": 6},
}
# LUTs a distributed-RAM or shift-register cell occupies
LUTRAM = {"RAM32X1S": 1, "RAM32X1D": 2, "RAM64X1S": 1, "RAM64X1D": 2, "RAM128X1S": 2,
          "RAM128X1D": 4, "RAM256X1S": 4, "RAM32M": 4, "RAM64M": 4, "RAM32M16": 8, "RAM64M8": 8,
          "SRL16E": 1, "SRLC32E": 1, "SRLC16E": 1}


def design_files(design: str, kind: str) -> list[str]:
    out = []
    for line in (BD / "designs" / f"{design}.files").read_text().splitlines():
        line = line.split("#", 1)[0].split()
        if line and line[0] == kind:
            out += sorted(str(p.relative_to(ROOT)) for p in ROOT.glob(line[1]))
    return out


def synth(name: str, top: str, files: list[str], incs: list[str], pads: bool) -> dict:
    """Yosys synth_xilinx; returns {cell type: count} for the flattened top. pads = False for a
    block inside the chip (no I/O buffers or clock buffers on its ports)."""
    with tempfile.TemporaryDirectory() as tmp:
        js = Path(tmp) / "stat.json"
        inc = " ".join(f"-I{i}" for i in incs)
        opts = "" if pads else " -noiopad -noclkbuf"
        script = (f"read_verilog {inc} {' '.join(files)}; "
                  f"synth_xilinx -family xc7 -flatten{opts} -top {top}; tee -q -o {js} stat -json")
        r = subprocess.run(["yosys", "-q", "-p", script], cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"{name}: yosys failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
        data = json.loads(js.read_text())
    mod = data["modules"].get(f"\\{top}") or next(iter(data["modules"].values()))
    return {k.lstrip("\\$"): v for k, v in mod.get("num_cells_by_type", {}).items()}


def resources(cells: dict) -> dict:
    lut = sum(v for k, v in cells.items() if k.startswith("LUT")) + cells.get("INV", 0)
    lutram = sum(LUTRAM.get(k, 0) * v for k, v in cells.items())
    ff = sum(v for k, v in cells.items() if k.startswith("FD"))
    bram = cells.get("RAMB36E1", 0) + cells.get("RAMB18E1", 0) / 2
    return {"LUT": lut + lutram, "LUTRAM": lutram, "FF": ff, "RAMB36": bram,
            "DSP": cells.get("DSP48E1", 0), "CARRY4": cells.get("CARRY4", 0),
            "MUXF7/8": cells.get("MUXF7", 0) + cells.get("MUXF8", 0),
            "BUFG": cells.get("BUFG", 0) + cells.get("BUFGCTRL", 0),
            "MMCM": cells.get("MMCME2_BASE", 0) + cells.get("MMCME2_ADV", 0),
            "PLL": cells.get("PLLE2_BASE", 0) + cells.get("PLLE2_ADV", 0),
            "IO": sum(v for k, v in cells.items() if k in ("IBUF", "OBUF", "IOBUF", "OBUFT"))}


def bram_budget() -> list[tuple[str, int, str]]:
    """RAMB36 blocks the memory map implies (each holds 1024 x 32 bits of data)."""
    spec = tomllib.loads((ROOT / "spec" / "soc.toml").read_text())
    size = {r["name"]: r["size"] for r in spec["region"]}
    rows = []
    for name, why in (("bootrom", "boot ROM, true dual port (fetch + data)"),
                      ("scratch", "scratch RAM, true dual port"),
                      ("textram", "text RAM, dual clock (data port, pixel port)")):
        rows.append((f"{name} ({size[name] // 1024} KiB)", -(-size[name] // 4096), why))
    rows.append(("font ROM (256 glyphs x 16 rows x 8 bits = 4 KiB)", 1, "io track; one RAMB36 as 4K x 9"))
    rows.append(("CPU register file, FIFOs", 0, "small: distributed RAM (LUTs), not block RAM"))
    rows.append(("DDR3L controller (AMD MIG)", 0, "uses the PHY's own FIFOs; to be confirmed by the ddr track"))
    return rows


def main(argv: list[str]) -> int:
    incs = ["rtl/generated", "rtl/cpu", "rtl/lib", "boards/arty_a7/rtl"]
    real_soc = (ROOT / "rtl" / "soc" / "surfcpu_soc.v").exists()
    runs = [("arty_a7_baseline (milestone 1, whole design)", "arty_a7_baseline",
             design_files("arty_a7_baseline", "src"), True)]
    if real_soc:
        top_files = design_files("surfcpu_arty_a7_top", "src")
        runs.append(("surfcpu_arty_a7_top (whole SoC, HAS_DDR = 0)", "surfcpu_arty_a7_top", top_files, True))
        runs.append(("surfcpu_soc alone", "surfcpu_soc",
                     [f for f in top_files if not f.startswith("boards/")], False))
    runs.append(("arty_a7_clocks (the board's clocks and resets)", "arty_a7_clocks",
                 ["rtl/lib/surf_reset_sync.v", "boards/arty_a7/rtl/arty_a7_clocks.v"], False))
    for lib in sorted((ROOT / "rtl" / "lib").glob("*.v")):
        runs.append((f"rtl/lib/{lib.name} (default parameters)", lib.stem, [str(lib.relative_to(ROOT))], False))

    out = ["SurfCPU resource estimates: Yosys synth_xilinx -family xc7 (generated by boards/arty_a7/estimate.py)",
           f"SoC on this branch: {'rtl/soc/surfcpu_soc.v' if real_soc else 'not yet (the soc, cpu and io tracks); estimated once merged'}",
           "Yosys counts, not Vivado's: an estimate. LUT includes LUTRAM/SRL; RAMB36 counts RAMB18 as half.",
           ""]
    head = f"{'design':66s} {'LUT':>6s} {'FF':>6s} {'RAMB36':>6s} {'DSP':>4s} {'CARRY4':>6s} {'BUFG':>4s} {'MMCM':>4s} {'PLL':>3s} {'IO':>4s}"
    out.append(head)
    out.append("-" * len(head))
    results = {}
    for name, top, files, pads in runs:
        r = resources(synth(name, top, files, incs, pads))
        results[name] = r
        out.append(f"{name:66s} {r['LUT']:6d} {r['FF']:6d} {r['RAMB36']:6g} {r['DSP']:4d} {r['CARRY4']:6d} "
                   f"{r['BUFG']:4d} {r['MMCM']:4d} {r['PLL']:3d} {r['IO']:4d}")
    out.append("")
    out.append("Share of each part (DS180 v2.6.1 table 4):")
    for name in [n for n in results if n.startswith(("arty_a7_baseline", "surfcpu_"))]:
        r = results[name]
        for part, cap in PARTS.items():
            out.append(f"  {name[:48]:48s} {part:24s} LUT {100 * r['LUT'] / cap['LUT']:5.1f}%  "
                       f"FF {100 * r['FF'] / cap['FF']:5.1f}%  RAMB36 {100 * r['RAMB36'] / cap['RAMB36']:5.1f}%  "
                       f"DSP {100 * r['DSP'] / cap['DSP']:5.1f}%  {r['MMCM']} MMCM + {r['PLL']} PLL "
                       f"(of {cap['CMT']} CMTs, one of each per CMT)")
    out.append("")
    out.append("Block RAM budget from spec/soc.toml (RAMB36 = 36 Kb, used as 1024 x 32 data bits):")
    total = 0
    for what, n, why in bram_budget():
        total += n
        out.append(f"  {what:56s} {n:3d}  {why}")
    out.append(f"  {'total':56s} {total:3d}")
    for part, cap in PARTS.items():
        out.append(f"  {part:24s} {cap['RAMB36']:3d} RAMB36: {total} used ({100 * total / cap['RAMB36']:.0f}%), "
                   f"{cap['RAMB36'] - total} free")
    text = "\n".join(out) + "\n"
    if len(argv) > 1:
        Path(argv[1]).write_text(text)
        print(f"estimate: wrote {argv[1]}")
    print(text, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
