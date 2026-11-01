#!/usr/bin/env python3
"""
SurfCPU Arty A7: the boot ROM in an open-flow bitstream holds exactly its .mem file
--------------------
File: romcheck.py Date: 10/16/26 (SurfCPU)
--------------------
The open flow's bramcheck proves that the block RAM contents decoded from the bitstream are
the synthesized netlist's, block for block. This proves the netlist's boot ROM blocks hold the
.mem file the design was built with, so the chain reaches from the bitstream to the file:

  1. the open flow's Yosys (in its image) synthesizes surf_bootrom alone with the .mem, with
     the flow's commands (synth_xilinx -flatten -abc9 -arch xc7, setundef -zero);
  2. its block RAM cells must be those of the ROM instance in the design's netlist: the same
     contents (INIT, INITP), widths and modes, as multisets;
  3. Icarus (the toolchain image) reads every word of that ROM netlist on both ports, through
     Yosys's cell models and the block RAM models of sim/board/prims (Yosys has none), and
     compares each with the .mem (sim/board/tb_rom_read.v).

Run on the Mac from surfcpu/, by tools/fpga-build.sh after an open-flow build:

    romcheck.py --rom build/fw/bramdiag.mem --netlist DIR/top.json --top surfcpu_arty_a7_top \\
        --instance u_soc.u_bootrom --cells-sim DIR/cells_sim.v --out DIR/romcheck

Writes OUT/check-rom.txt and exits 0 only if all three hold. Paths must lie inside surfcpu/
(the containers see only the repository).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter

IMAGE_DEFAULT = "surfcpu-openxc7"
KEYS = re.compile(r"^(INITP?_[0-9A-F]{2}|READ_WIDTH_[AB]|WRITE_WIDTH_[AB]|RAM_MODE|WRITE_MODE_[AB]|DO[AB]_REG)$")


def blocks(path: str, top: str, prefix: str) -> Counter:
    with open(path) as f:
        mod = json.load(f)["modules"][top]
    out: Counter = Counter()
    for name, cell in mod.get("cells", {}).items():
        if cell["type"] in ("RAMB36E1", "RAMB18E1") and (name == prefix or name.startswith(prefix + ".")):
            params = cell.get("parameters", {})
            out[(cell["type"],) + tuple(sorted((k, str(v)) for k, v in params.items() if KEYS.match(k)))] += 1
    return out


def run(cmd: list[str], log: str, env: dict | None = None) -> int:
    with open(log, "w") as f:
        return subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, env=env)


def main() -> int:
    ap = argparse.ArgumentParser(description="boot ROM contents check (see the file header)")
    ap.add_argument("--rom", required=True)
    ap.add_argument("--netlist", required=True)
    ap.add_argument("--top", required=True)
    ap.add_argument("--instance", required=True, help="the ROM's instance in the netlist (u_soc.u_bootrom)")
    ap.add_argument("--cells-sim", required=True, help="the cells_sim.v the open flow copied")
    ap.add_argument("--out", required=True)
    ap.add_argument("--image", default=os.environ.get("SURFCPU_OPENXC7_IMAGE", ""))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    report = []
    ok = True

    def say(line: str) -> None:
        report.append(line)
        print(line)

    # 1. the ROM alone, with the open flow's Yosys
    ys = os.path.join(args.out, "rom.ys")
    with open(ys, "w") as f:
        f.write(f"read_verilog -Irtl/generated rtl/soc/surf_bootrom.v\n"
                f"chparam -set INIT_FILE \"{args.rom}\" surf_bootrom\n"
                f"scratchpad -set xilinx_dsp.multonly 1\n"
                f"synth_xilinx -flatten -abc9 -arch xc7 -top surf_bootrom -noiopad\n"
                f"setundef -zero\n"
                f"write_json {args.out}/rom.json\n"
                f"write_verilog -noattr {args.out}/rom.netlist.v\n")
    image = args.image
    if not image:
        tag = ""
        with open("tools/openxc7-build.sh") as f:
            for line in f:
                if line.startswith("IMAGE_TAG="):
                    tag = line.split("=", 1)[1].strip()
        image = f"{IMAGE_DEFAULT}:{tag}"
    env = dict(os.environ, SURFCPU_IMAGE=image)
    if run(["devenv/run.sh", "yosys", "-q", "-s", ys], os.path.join(args.out, "rom-yosys.log"), env) != 0:
        say(f"romcheck: Yosys could not synthesize the ROM alone (see {args.out}/rom-yosys.log)")
        ok = False
    else:
        # 2. the same blocks as in the design
        alone = blocks(os.path.join(args.out, "rom.json"), "surf_bootrom", "rom")
        design = blocks(args.netlist, args.top, args.instance + ".rom")
        same = sum((alone & design).values())
        say(f"blocks: {sum(alone.values())} when the ROM is synthesized alone, {sum(design.values())} in "
            f"{args.instance} of the design, {same} identical (contents, widths, modes)")
        if not alone or alone != design:
            ok = False
            say("romcheck: the design's ROM blocks are not those of the ROM synthesized alone")
        # 3. every word read through the netlist
        with open(args.cells_sim) as f:
            text = f.read()
        kept, skip = [], False
        for line in text.splitlines(keepends=True):
            if re.match(r"module (RAMB18E1|RAMB36E1)\b", line):
                skip = True
            if not skip:
                kept.append(line)
            elif line.startswith("endmodule"):
                skip = False
        cells = os.path.join(args.out, "cells_sim_noramb.v")
        with open(cells, "w") as f:
            f.write("".join(kept))
        vvp = os.path.join(args.out, "tb_rom_read.vvp")
        if run(["devenv/run.sh", "iverilog", "-g2012", "-Irtl/generated", "-o", vvp, "sim/board/tb_rom_read.v",
                os.path.join(args.out, "rom.netlist.v"), cells, "sim/board/prims/RAMB36E1.v",
                "sim/board/prims/RAMB18E1.v"], os.path.join(args.out, "iverilog.log")) != 0:
            say(f"romcheck: Icarus could not compile the ROM netlist (see {args.out}/iverilog.log)")
            ok = False
        else:
            log = os.path.join(args.out, "tb_rom_read.log")
            run(["devenv/run.sh", "vvp", "-n", vvp, f"+mem={args.rom}"], log)
            with open(log) as f:
                sim = f.read()
            m = re.search(r"romcheck: (\d+) words on each port, (\d+) mismatches", sim)
            for line in sim.splitlines():
                if line.startswith("tb_rom_read:") or line.startswith("romcheck:"):
                    say(line)
            if not m or m.group(2) != "0":
                ok = False
    say(f"romcheck: {'PASS' if ok else 'FAIL'} ({args.rom} in {args.instance})")
    with open(os.path.join(args.out, "check-rom.txt"), "w") as f:
        f.write("\n".join(report) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
