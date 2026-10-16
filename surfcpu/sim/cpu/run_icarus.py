#!/usr/bin/env python3
"""
SurfCPU core under Icarus Verilog
--------------------
File: run_icarus.py Date: 10/15/26 (SurfCPU)
--------------------
Builds sim/cpu/tb_core_icarus.v with surfcpu_core (without RVFI) in Icarus Verilog and runs
the directed tests that need only RAM and bus errors (no timer, no testbench registers)
with three seeds each, to show the RTL also simulates in Icarus (docs/MODULES.md section 1).
Exit 0 when every run prints PASS.

    run_icarus.py [--out build/cpu/icarus] [--jobs N] ELF...
"""

from __future__ import annotations

import argparse
import concurrent.futures
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent          # surfcpu/
SEEDS = [1, 2, 3]


def make_hex(elf: Path, out: Path) -> tuple[Path, str]:
    binf = out / (elf.stem + ".bin")
    hexf = out / (elf.stem + ".hex")
    subprocess.run(["riscv64-unknown-elf-objcopy", "-O", "binary", str(elf), str(binf)], check=True)
    data = binf.read_bytes()
    data += b"\0" * (-len(data) % 4)
    words = struct.unpack(f"<{len(data) // 4}I", data)
    hexf.write_text("".join(f"{w:08x}\n" for w in words))
    nm = subprocess.run(["riscv64-unknown-elf-nm", str(elf)], capture_output=True, text=True, check=True).stdout
    tohost = next(line.split()[0] for line in nm.splitlines() if line.split()[-1:] == ["tohost"])
    return hexf, tohost


def run(vvp: Path, hexf: Path, tohost: str, seed: int) -> tuple[bool, str]:
    r = subprocess.run(["vvp", "-n", str(vvp), f"+image={hexf}", f"+tohost={tohost}", f"+seed={seed}"],
                       capture_output=True, text=True, timeout=900)
    lines = [l for l in r.stdout.splitlines() if l.startswith("tb_core_icarus:")]
    last = lines[-1] if lines else "(no result)"
    return "PASS" in last, last


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "build" / "cpu" / "icarus"))
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("elfs", nargs="+")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    vvp = out / "tb_core_icarus.vvp"
    rtl = sorted(str(p) for p in (ROOT / "rtl" / "cpu").glob("surfcpu_*.v")) + [str(ROOT / "rtl" / "lib" / "surf_fifo.v")]
    subprocess.run(["iverilog", "-g2005", "-Wall", "-Wno-timescale", f"-I{ROOT / 'rtl' / 'cpu'}",
                    f"-I{ROOT / 'rtl' / 'lib'}", f"-I{ROOT / 'rtl' / 'generated'}", "-s", "tb_core_icarus",
                    "-o", str(vvp), str(ROOT / "sim" / "cpu" / "tb_core_icarus.v")] + rtl, check=True)
    jobs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        for e in a.elfs:
            hexf, tohost = make_hex(Path(e), out)
            for seed in SEEDS:
                jobs.append((Path(e).stem, seed, pool.submit(run, vvp, hexf, tohost, seed)))
        results: dict[str, list[tuple[int, bool, str]]] = {}
        for name, seed, fut in jobs:
            ok, msg = fut.result()
            results.setdefault(name, []).append((seed, ok, msg))
    passed = 0
    for name in sorted(results):
        good = all(ok for _, ok, _ in results[name])
        passed += good
        print(f"  {name:<22} {'PASS' if good else 'FAIL'}")
        for seed, ok, msg in results[name]:
            if not ok:
                print(f"      seed {seed}: {msg}")
    print(f"icarus: {passed}/{len(results)} tests pass with seeds {' '.join(map(str, SEEDS))}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
