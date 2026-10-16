#!/usr/bin/env python3
"""
SurfCPU directed test runner
--------------------
File: run_tests.py Date: 10/15/26 (SurfCPU)
--------------------
Runs each directed test ELF (sim/cpu/tests/*.S) on build/cpu/surfcpu_tb under several bus
timing configurations and seeds, in parallel, and prints one line per test and a summary.
Exit status 0 when every run passed.

    run_tests.py --tb build/cpu/surfcpu_tb --out build/cpu/runs [--jobs N] ELF...
    run_tests.py --spike --out build/cpu/spike ELF...      (the same ELFs on Spike)

A test can carry options in a comment line of its source:
    // surf-test: spike=no max-cycles=4000000
spike=no marks a test that needs the testbench's own devices (docs/CPU.md section 9) or
checks behaviour where docs/ISA.md and Spike legitimately differ, so it is not run on Spike.
Spike runs with Zicntr and Zihpm added to the ISA string, because surfcpu_core implements
the cycle/time/instret and hpmcounter CSRs (docs/ISA.md section 2). A failing run is repeated with --trace and its trace is kept in --out.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import time
from pathlib import Path

# (name, ilat, dlat, seeds, extra arguments)
CONFIGS = [
    ("zero",   "1:1",    "1:1",    [1],              []),
    ("short",  "1:3",    "1:3",    [2, 12],          []),
    ("mixed",  "1:12",   "1:12",   [3, 13, 23, 33],  []),
    ("dslow",  "1:2",    "4:40",   [4, 14],          []),
    ("islow",  "4:40",   "1:2",    [5, 15],          []),
    ("slow",   "6:30",   "6:30",   [6, 16],          ["--rand-reset"]),
    ("huge",   "20:100", "20:100", [7],              []),
]

SPIKE = ["spike", "--isa=rv32im_zicntr_zicsr_zifencei_zihpm", "--priv=m", "--pmpregions=0", "-m0x80000000:0x400000"]


def test_options(elf: Path) -> dict[str, str]:
    """The `surf-test:` options of the test's source (next to the ELF's name in sim/cpu/tests)."""
    src = Path(__file__).resolve().parent / "tests" / (elf.stem + ".S")
    opts: dict[str, str] = {}
    if src.exists():
        for line in src.read_text().splitlines():
            m = re.search(r"surf-test:\s*(.*)", line)
            if m:
                for item in m.group(1).split():
                    k, _, v = item.partition("=")
                    opts[k] = v
    return opts


def run_tb(tb: str, elf: Path, cfg: tuple, seed: int, out: Path, max_cycles: int) -> tuple[bool, str]:
    name, ilat, dlat, _, extra = cfg
    base = [tb, "--elf", str(elf), "--seed", str(seed), "--ilat", ilat, "--dlat", dlat,
            "--max-cycles", str(max_cycles), "--quiet"] + extra
    r = subprocess.run(base, capture_output=True, text=True, timeout=600)
    if r.returncode == 0:
        return True, ""
    # keep a trace of the failure for debugging
    tr = out / f"{elf.stem}.{name}.{seed}.trace"
    subprocess.run(base + ["--trace", str(tr)], capture_output=True, text=True, timeout=600)
    msg = (r.stderr.strip().splitlines() or ["(no output)"])[-1]
    return False, f"{name} seed {seed}: exit {r.returncode}: {msg} (trace {tr})"


def run_spike(elf: Path, timeout: int) -> tuple[bool, str]:
    try:
        r = subprocess.run(SPIKE + [str(elf)], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "spike: timeout"
    if r.returncode == 0:
        return True, ""
    return False, f"spike: exit {r.returncode} (test {r.returncode}) {r.stderr.strip()[-200:]}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tb", default="build/cpu/surfcpu_tb")
    ap.add_argument("--out", default="build/cpu/runs")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--spike", action="store_true", help="run the ELFs on Spike instead")
    ap.add_argument("elfs", nargs="+")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    elfs = [Path(e) for e in args.elfs]
    start = time.time()

    jobs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for elf in elfs:
            opts = test_options(elf)
            max_cycles = int(opts.get("max-cycles", "3000000"))
            if args.spike:
                if opts.get("spike", "yes") == "no":
                    continue
                jobs.append((elf, "spike", pool.submit(run_spike, elf, 120)))
            else:
                for cfg in CONFIGS:
                    for seed in cfg[3]:
                        jobs.append((elf, cfg[0], pool.submit(run_tb, args.tb, elf, cfg, seed, out, max_cycles)))

        per_test: dict[str, list[tuple[bool, str]]] = {}
        for elf, _, fut in jobs:
            try:
                ok, msg = fut.result()
            except subprocess.TimeoutExpired:
                ok, msg = False, "timeout"
            per_test.setdefault(elf.stem, []).append((ok, msg))

    total = passed = 0
    for name in sorted(per_test):
        res = per_test[name]
        good = sum(1 for ok, _ in res if ok)
        total += len(res)
        passed += good
        print(f"  {name:<22} {good}/{len(res)} {'PASS' if good == len(res) else 'FAIL'}")
        for ok, msg in res:
            if not ok:
                print(f"      {msg}")
    what = "spike" if args.spike else "surfcpu_tb"
    print(f"{what}: {len(per_test)} tests, {passed}/{total} runs passed ({time.time() - start:.1f} s)")
    if not args.spike:
        print("configurations: " + ", ".join(
            f"{c[0]} (ilat {c[1]}, dlat {c[2]}, seeds {' '.join(map(str, c[3]))}{', ' + ' '.join(c[4]) if c[4] else ''})"
            for c in CONFIGS))
    return 0 if passed == total and total > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
