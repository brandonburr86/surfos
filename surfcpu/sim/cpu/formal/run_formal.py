#!/usr/bin/env python3
"""
SurfCPU core: run the bounded formal check
--------------------
File: run_formal.py Date: 10/15/26 (SurfCPU)
--------------------
Runs sim/cpu/formal/fv.sby with SymbiYosys: the bmc task (every assertion of fv_top.sv for
DEPTH cycles from reset) and the cover task (its cover statements are reachable, so the
assertions are not vacuous), each with a time limit, and prints one summary line. A
different RTL directory can be checked (the mutation tool uses this). Exit 0 when bmc
passes and every cover statement is reached.

    run_formal.py [--depth 14] [--timeout 900] [--rtl DIR] [--out build/cpu/formal] [--tasks bmc cover]
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent                              # surfcpu/
RTL_FILES = ["cpu/surfcpu_alu.v", "cpu/surfcpu_core.v", "cpu/surfcpu_csr.v", "cpu/surfcpu_decode.v",
             "cpu/surfcpu_fetch.v", "cpu/surfcpu_muldiv.v", "cpu/surfcpu_regfile.v", "lib/surf_fifo.v",
             "cpu/surfcpu_defs.vh", "cpu/surfcpu_rvfi.vh"]


def make_sby(depth: int, timeout: int, rtl: Path) -> str:
    text = (HERE / "fv.sby").read_text()
    text = re.sub(r"^bmc: depth \d+", f"bmc: depth {depth}", text, flags=re.M)
    text = re.sub(r"^cover: depth \d+", f"cover: depth {depth}", text, flags=re.M)
    text = re.sub(r"^timeout \d+", f"timeout {timeout}", text, flags=re.M)
    files = "\n".join(str(rtl / f) for f in RTL_FILES) + f"\n{HERE / 'fv_top.sv'}\n"
    return re.sub(r"\[files\]\n(.*\n?)*", "[files]\n" + files, text)


def run_task(sby: Path, task: str, work: Path, timeout: int) -> tuple[str, str, float]:
    shutil.rmtree(work, ignore_errors=True)
    start = time.time()
    try:
        r = subprocess.run(["sby", "-f", "-d", str(work), str(sby), task], capture_output=True, text=True,
                           timeout=timeout + 120)
        out = r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return "TIMEOUT", "", time.time() - start
    status = re.findall(r"DONE \((\w+)", out)
    return (status[-1] if status else "ERROR"), out, time.time() - start


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--depth", type=int, default=14)
    ap.add_argument("--timeout", type=int, default=900, help="seconds allowed per task")
    ap.add_argument("--rtl", default=str(ROOT / "rtl"), help="directory holding cpu/ and lib/")
    ap.add_argument("--out", default=str(ROOT / "build" / "cpu" / "formal"))
    ap.add_argument("--tasks", nargs="+", default=["bmc", "cover"])
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    sby = out / "fv.sby"
    sby.write_text(make_sby(a.depth, a.timeout, Path(a.rtl).resolve()))
    ok = True
    parts = []
    for task in a.tasks:
        status, log, secs = run_task(sby, task, out / task, a.timeout)
        (out / f"{task}.txt").write_text(log)
        if task == "bmc":
            fails = re.findall(r"failed assertion .* at (fv_top\.sv:\d+)[^ ]* in step (\d+)", log)
            detail = f", first failing assertion {fails[0][0]} in step {fails[0][1]}" if fails else ""
            parts.append(f"bmc depth {a.depth} {status} ({secs:.0f} s{detail})")
            ok &= status == "PASS"
        else:
            reached = len(re.findall(r"Reached cover statement", log))
            unreached = len(re.findall(r"summary:\s+fv_top\._witness_\.check_cover", log))
            parts.append(f"cover {status}: {reached} of {reached + unreached} cover statements reached ({secs:.0f} s)")
            ok &= status == "PASS" and unreached == 0
    print("formal-cpu: " + "; ".join(parts) + (f"; logs in {out}" if not ok else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
