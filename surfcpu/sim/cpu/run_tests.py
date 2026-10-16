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

A test can carry options in a comment line of its source (.S, or .c for a C test):
    // surf-test: spike=no max-cycles=4000000
spike=no marks a test that needs the testbench's own devices (docs/CPU.md section 9) or
checks behaviour where docs/ISA.md and Spike legitimately differ, so it is not run on Spike.
For each test that fails, its first failing run is repeated once with a trace of only the
last TRACE_TAIL records (--trace-tail), so a runaway program cannot fill the disk.
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

TRACE_TAIL = 5000          # records kept in the trace of a failing run

SPIKE = ["spike", "--isa=rv32im_zicntr_zicsr_zifencei_zihpm", "--priv=m", "--pmpregions=0", "-m0x80000000:0x400000"]


def test_options(elf: Path) -> dict[str, str]:
    """The `surf-test:` options of the test's source: sim/cpu/tests/<name>.S, or
    sim/cpu/tests/c/<name>.c for a C test (c_<name>.elf)."""
    tests = Path(__file__).resolve().parent / "tests"
    src = tests / (elf.stem + ".S")
    if elf.stem.startswith("c_"):
        src = tests / "c" / (elf.stem[2:] + ".c")
    opts: dict[str, str] = {}
    if src.exists():
        for line in src.read_text().splitlines():
            m = re.search(r"surf-test:\s*(.*)", line)
            if m:
                for item in m.group(1).split():
                    k, _, v = item.partition("=")
                    opts[k] = v
    return opts


def tb_command(tb: str, elf: Path, cfg: tuple, seed: int, max_cycles: int) -> list[str]:
    _, ilat, dlat, _, extra = cfg
    return [tb, "--elf", str(elf), "--seed", str(seed), "--ilat", ilat, "--dlat", dlat,
            "--max-cycles", str(max_cycles), "--quiet"] + extra


def run_tb(cmd: list[str], label: str) -> tuple[bool, str]:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return False, f"{label}: no result within 600 s"
    if r.returncode == 0:
        return True, ""
    msg = (r.stderr.strip().splitlines() or ["(no output)"])[-1]
    return False, f"{label}: exit {r.returncode}: {msg}"


def trace_failure(cmd: list[str], trace: Path) -> None:
    """Repeat a failing run once, keeping only the last TRACE_TAIL records of its trace."""
    try:
        subprocess.run(cmd + ["--trace", str(trace), "--trace-tail", str(TRACE_TAIL)],
                       capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        pass


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
                jobs.append((elf, [], "spike", pool.submit(run_spike, elf, 120)))
            else:
                for cfg in CONFIGS:
                    for seed in cfg[3]:
                        cmd = tb_command(args.tb, elf, cfg, seed, max_cycles)
                        label = f"{cfg[0]} seed {seed}"
                        jobs.append((elf, cmd, label, pool.submit(run_tb, cmd, label)))

        per_test: dict[str, list[tuple[bool, str]]] = {}
        first_failure: dict[str, tuple[list[str], str]] = {}
        for elf, cmd, label, fut in jobs:
            ok, msg = fut.result()
            per_test.setdefault(elf.stem, []).append((ok, msg))
            if not ok and elf.stem not in first_failure and cmd:
                first_failure[elf.stem] = (cmd, label)
        # one capped trace per failing test, for debugging
        traces = {}
        for stem, (cmd, label) in first_failure.items():
            tr = out / f"{stem}.{label.replace(' seed ', '.')}.trace"
            trace_failure(cmd, tr)
            traces[stem] = tr

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
        if name in traces:
            print(f"      (last {TRACE_TAIL} records of the first failing run: {traces[name]})")
    what = "spike" if args.spike else "surfcpu_tb"
    print(f"{what}: {len(per_test)} tests, {passed}/{total} runs passed ({time.time() - start:.1f} s)")
    if not args.spike:
        print("configurations: " + ", ".join(
            f"{c[0]} (ilat {c[1]}, dlat {c[2]}, seeds {' '.join(map(str, c[3]))}{', ' + ' '.join(c[4]) if c[4] else ''})"
            for c in CONFIGS))
    return 0 if passed == total and total > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
