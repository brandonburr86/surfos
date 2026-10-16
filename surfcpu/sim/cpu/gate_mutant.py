#!/usr/bin/env python3
"""
SurfCPU: is the gate-level check sensitive?
--------------------
File: gate_mutant.py Date: 10/15/26 (SurfCPU)
--------------------
Takes the post-synthesis netlist written by `make synth-cpu-gate`, makes a copy in which
the first DSP48E1 gets an output register it does not have (PREG 0 -> 1, so part of every
product arrives a cycle late), and runs the multiply test on the copy. The run must fail:
that shows the gate-level simulation really exercises the DSP mapping, so a multiplier that
synthesis broke or dropped would be caught. Exit 0 when the mutant fails as expected.

    gate_mutant.py [--netlist build/cpu/gate/surfcpu_core_netlist.v] [--elf build/cpu/tests/muldiv.elf]
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--netlist", default=str(ROOT / "build" / "cpu" / "gate" / "surfcpu_core_netlist.v"))
    ap.add_argument("--elf", default=str(ROOT / "build" / "cpu" / "tests" / "muldiv.elf"))
    a = ap.parse_args()
    text = Path(a.netlist).read_text()
    start = text.find("DSP48E1 #(")
    if start < 0:
        print("gate_mutant: the netlist has no DSP48E1 (was the multiplier mapped at all?)")
        return 1
    key = ".PREG(32'sd0)"
    at = text.find(key, start)
    if at < 0 or at - start > 4000:
        print("gate_mutant: the first DSP48E1 has no PREG(0) parameter to change")
        return 1
    mutant = Path(a.netlist).with_name("surfcpu_core_netlist_mutant.v")
    mutant.write_text(text[:at] + ".PREG(32'sd1)" + text[at + len(key):])
    r = subprocess.run([sys.executable, str(HERE / "run_icarus.py"), "--out", str(mutant.parent / "mutant"),
                        "--seeds", "1", "--netlist", str(mutant), a.elf], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode == 0:
        print("gate_mutant: the mutant PASSED: the gate-level check would not notice a broken multiplier")
        return 1
    print("gate_mutant: the mutant (first DSP48E1 with PREG = 1) fails, as it must: the check is sensitive")
    return 0


if __name__ == "__main__":
    sys.exit(main())
