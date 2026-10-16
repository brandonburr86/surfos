#!/usr/bin/env python3
"""
SurfCPU: riscv-tests on the core
--------------------
File: riscv_tests.py Date: 10/15/26 (SurfCPU)
--------------------
Fetches riscv-tests and its riscv-test-env submodule at pinned commits (GitHub archive
tarballs, SHA-256 checked) into build/third_party/riscv-tests/, builds the rv32ui, rv32um
and rv32mi tests of the "p" environment (machine mode, no virtual memory) for
rv32im_zicsr_zifencei with this toolchain, and runs each on build/cpu/surfcpu_tb under
several bus timings. A test listed in NOT_APPLICABLE needs something docs/ISA.md leaves
out; it is built and run too, and must fail the way the note says (so the list stays
honest). Exit 0 when every applicable test passes every timing and every listed one
fails as expected. This is extra evidence next to the directed tests; the official
architecture-test flow (riscv-arch-test) belongs to the verif track.

    riscv_tests.py [--tb build/cpu/surfcpu_tb] [--jobs N]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

TESTS_COMMIT = "d7e86f701b929b5d801f8cda1a4871ab05f452bb"     # riscv-software-src/riscv-tests, 2026-10-14
TESTS_SHA256 = "15b7acd84303600676f01206a63dcd446c7d76bca197dea42a76286d89490aaa"
ENV_COMMIT = "6de71edb142be36319e380ce782c3d1830c65d68"       # riscv/riscv-test-env (its env submodule)
ENV_SHA256 = "c261a872c3d118e07d8a5d6a09ceff7f666dec29030e150089f6fde5eec66edf"
TESTS_URL = f"https://codeload.github.com/riscv-software-src/riscv-tests/tar.gz/{TESTS_COMMIT}"
ENV_URL = f"https://codeload.github.com/riscv/riscv-test-env/tar.gz/{ENV_COMMIT}"

ROOT = Path(__file__).resolve().parent.parent.parent          # surfcpu/
THIRD = ROOT / "build" / "third_party" / "riscv-tests"
OUT = ROOT / "build" / "cpu" / "riscv-tests"
GCC = ["riscv64-unknown-elf-gcc", "-march=rv32im_zicsr_zifencei", "-mabi=ilp32", "-static", "-mcmodel=medany",
       "-fvisibility=hidden", "-nostdlib", "-nostartfiles", "-Wl,--no-warn-rwx-segments"]
TIMINGS = [("zero", ["--ilat", "1:1", "--dlat", "1:1", "--seed", "1"]),
           ("mixed", ["--ilat", "1:12", "--dlat", "1:12", "--seed", "3"]),
           ("slow", ["--ilat", "6:30", "--dlat", "6:30", "--seed", "6"])]

# Tests that need what docs/ISA.md leaves out, with the failure they must show.
NOT_APPLICABLE = {
    "rv32mi-p-breakpoint": "needs debug triggers (tselect/tdata), which SurfCPU does not have",
    "rv32mi-p-pmpaddr": "needs PMP entries; SurfCPU has none (pmpaddr reads 0)",
    "rv32ui-p-ma_data": "needs misaligned loads and stores done in hardware; SurfCPU traps them",
}


def fetch(url: str, sha: str, dest: Path) -> None:
    if dest.exists() and hashlib.sha256(dest.read_bytes()).hexdigest() == sha:
        return
    data = urllib.request.urlopen(url, timeout=120).read()
    got = hashlib.sha256(data).hexdigest()
    if got != sha:
        raise SystemExit(f"riscv_tests: {url}: SHA-256 {got}, expected {sha}")
    dest.write_bytes(data)


def unpack(archive: Path, into: Path) -> Path:
    into.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as t:
        top = t.getnames()[0].split("/")[0]
        if not (into / top).exists():
            t.extractall(into, filter="data")     # refuses absolute paths and links leaving the tree
    return into / top


def build(src: Path, elf: Path, tests_dir: Path, env_dir: Path) -> str | None:
    if elf.exists() and elf.stat().st_mtime > src.stat().st_mtime:
        return None
    r = subprocess.run(GCC + [f"-I{env_dir / 'p'}", f"-I{tests_dir / 'isa' / 'macros' / 'scalar'}",
                              "-T", str(env_dir / "p" / "link.ld"), str(src), "-o", str(elf)],
                       capture_output=True, text=True)
    return None if r.returncode == 0 else r.stderr


def run(tb: str, elf: Path, timing: list[str]) -> tuple[int, str]:
    r = subprocess.run([tb, "--elf", str(elf), "--max-cycles", "2000000", "--quiet"] + timing,
                       capture_output=True, text=True, timeout=300)
    return r.returncode, (r.stderr.strip().splitlines() or [""])[-1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tb", default=str(ROOT / "build" / "cpu" / "surfcpu_tb"))
    ap.add_argument("--jobs", type=int, default=6)
    a = ap.parse_args()

    THIRD.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    fetch(TESTS_URL, TESTS_SHA256, THIRD / f"riscv-tests-{TESTS_COMMIT}.tar.gz")
    fetch(ENV_URL, ENV_SHA256, THIRD / f"riscv-test-env-{ENV_COMMIT}.tar.gz")
    tests_dir = unpack(THIRD / f"riscv-tests-{TESTS_COMMIT}.tar.gz", THIRD / "src")
    env_dir = unpack(THIRD / f"riscv-test-env-{ENV_COMMIT}.tar.gz", THIRD / "src")

    elfs: list[tuple[str, Path]] = []
    for suite in ("rv32ui", "rv32um", "rv32mi"):
        for src in sorted((tests_dir / "isa" / suite).glob("*.S")):
            name = f"{suite}-p-{src.stem}"
            elf = OUT / name
            err = build(src, elf, tests_dir, env_dir)
            if err:
                print(f"{name}: build failed\n{err}")
                return 1
            elfs.append((name, elf))

    results: dict[str, list[tuple[str, int, str]]] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        futs = [(name, tname, pool.submit(run, a.tb, elf, timing)) for name, elf in elfs for tname, timing in TIMINGS]
        for name, tname, fut in futs:
            code, msg = fut.result()
            results.setdefault(name, []).append((tname, code, msg))

    ok = True
    passed = 0
    for name in sorted(results):
        res = results[name]
        good = all(code == 0 for _, code, _ in res)
        if name in NOT_APPLICABLE:
            expect_fail = all(code != 0 for _, code, _ in res)
            print(f"  {name:<28} not applicable: {NOT_APPLICABLE[name]} "
                  f"({'fails as expected' if expect_fail else 'UNEXPECTED RESULT'}: {res[0][2]})")
            ok &= expect_fail
            continue
        passed += good
        print(f"  {name:<28} {'PASS' if good else 'FAIL'}")
        for tname, code, msg in res:
            if code != 0:
                print(f"      {tname}: exit {code}: {msg}")
        ok &= good
    n = len(results) - len(NOT_APPLICABLE)
    print(f"riscv-tests ({TESTS_COMMIT[:12]}, env {ENV_COMMIT[:12]}): {passed}/{n} applicable tests pass "
          f"all {len(TIMINGS)} timings ({', '.join(t for t, _ in TIMINGS)}); {len(NOT_APPLICABLE)} not applicable")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
