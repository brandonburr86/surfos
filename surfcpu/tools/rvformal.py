#!/usr/bin/env python3
"""
SurfCPU riscv-formal setup: pinned framework, wrapper, checks for surfcpu_core
--------------------
File: rvformal.py Date: 10/15/26 (SurfCPU)
--------------------
    python3 tools/rvformal.py fetch               riscv-formal at a pinned commit (SHA-256)
    python3 tools/rvformal.py gen [--stub] [--no-altops] [--aligned-mem]
                                                  copy the wrapper, the SurfBus slave model and
                                                  the core's RTL into riscv-formal's
                                                  cores/surfcpu/ and generate the checks
    python3 tools/rvformal.py run [-j N] [CHECK...]  run them with SymbiYosys (all, or some)
    python3 tools/rvformal.py smoke               before the core exists: elaborate the wrapper
                                                  with sim/ref/formal/stub_core.v (the ISA.md
                                                  port list) and generate the checks

riscv-formal (github.com/YosysHQ/riscv-formal) proves each instruction's RVFI record against
its formal ISA model, and the consistency of pc, registers and order over bounded depths. The
configuration is sim/ref/formal/checks.cfg; the core needs SURFCPU_RVFI. Two choices are
the cpu track's to make at integration (docs/VERIFICATION.md section 8):
  --aligned-mem   the core's RVFI memory fields use RISCV_FORMAL_ALIGNED_MEM (word address,
                  masks and data in their byte lanes) instead of riscv-formal's default;
  --no-altops     the core does not implement riscv-formal's alternative M operations under
                  RISCV_FORMAL_ALTOPS (then the M instruction checks are left out: real
                  multipliers and dividers are beyond a bounded proof).
Everything goes under build/third_party/riscv-formal-<commit>/.
Only the Python standard library is used (the checks need yosys, sby and a solver: the
toolchain container has yosys 0.52, sby and boolector).
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent            # surfcpu/
THIRD = ROOT / "build" / "third_party"
FORMAL = ROOT / "sim" / "ref" / "formal"

COMMIT = "c992aa61fdfe0846c5ed90324c596202a1c69b76"
URL = f"https://codeload.github.com/YosysHQ/riscv-formal/tar.gz/{COMMIT}"
SHA256 = "0e357116da2aaa0b57e6dab6fd9ff7612e04d4fdc37372754f72ec963b01e000"
DIR = THIRD / f"riscv-formal-{COMMIT}"
CORE = DIR / "cores" / "surfcpu"
M_INSNS = ("mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu")


def fetch() -> Path:
    stamp = DIR / ".surfcpu-fetched"
    if stamp.exists() and stamp.read_text().strip() == SHA256:
        return DIR
    dl = THIRD / "downloads" / f"riscv-formal-{COMMIT}.tar.gz"
    dl.parent.mkdir(parents=True, exist_ok=True)
    if not dl.exists() or _sha256(dl) != SHA256:
        print(f"rvformal: downloading {URL}")
        tmp = dl.with_suffix(".part")
        with urllib.request.urlopen(URL, timeout=300) as r, tmp.open("wb") as f:
            shutil.copyfileobj(r, f)
        if _sha256(tmp) != SHA256:
            tmp.unlink()
            raise SystemExit(f"rvformal: SHA-256 mismatch for {URL}; not using it")
        tmp.rename(dl)
    if DIR.exists():
        shutil.rmtree(DIR)
    with tarfile.open(dl) as t:
        t.extractall(THIRD, filter="data")
    stamp.write_text(SHA256 + "\n")
    return DIR


def _sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def gen(stub: bool, altops: bool, aligned: bool) -> Path:
    fetch()
    if CORE.exists():
        shutil.rmtree(CORE)
    CORE.mkdir(parents=True)
    rtl = [] if stub else sorted((ROOT / "rtl" / "cpu").glob("*.v"))
    if not stub and not rtl:
        raise SystemExit("rvformal: rtl/cpu has no core yet (the cpu track's surfcpu_core); "
                         "use `smoke` to check the setup against the port list")
    files = ["wrapper.sv", "surfbus_formal_slave.sv"]
    for f in files:
        shutil.copy(FORMAL / f, CORE / f)
    shutil.copy(ROOT / "rtl" / "cpu" / "surfcpu_rvfi.vh", CORE / "surfcpu_rvfi.vh")
    shutil.copy(ROOT / "rtl" / "generated" / "surfcpu_soc.vh", CORE / "surfcpu_soc.vh")
    if stub:
        shutil.copy(FORMAL / "stub_core.v", CORE / "stub_core.v")
        files.append("stub_core.v")
    for f in rtl:
        if f.name != "surfcpu_rvfi.vh":
            shutil.copy(f, CORE / f.name)
            files.append(f.name)
    cfg = (FORMAL / "checks.cfg").read_text()
    defines = ["`define SURFCPU_RVFI"]
    if altops:
        defines.append("`define RISCV_FORMAL_ALTOPS")
    if aligned:
        defines.append("`define RISCV_FORMAL_ALIGNED_MEM")
    filters = [] if altops else [f"- insn_{m}_ch0" for m in M_INSNS]
    cfg = cfg.replace("@DEFINES@", "\n".join(defines))
    cfg = cfg.replace("@FILTERS@", "\n".join(filters))
    cfg = cfg.replace("@FILES@", "\n".join(f"@basedir@/cores/@core@/{f}" for f in files))
    (CORE / "checks.cfg").write_text(cfg)
    subprocess.run([sys.executable, str(DIR / "checks" / "genchecks.py")], cwd=CORE, check=True,
                   stdout=subprocess.DEVNULL)
    checks = sorted(p.stem for p in (CORE / "checks").glob("*.sby"))
    print(f"rvformal: {len(checks)} checks generated in {CORE / 'checks'}")
    return CORE


def run(jobs: int, which: list[str]) -> int:
    d = CORE / "checks"
    if not d.exists():
        raise SystemExit("rvformal: run `gen` first")
    p = subprocess.run(["make", "-C", str(d), f"-j{jobs}", "-k", *which],
                       stdout=subprocess.DEVNULL)
    status = {}
    for f in sorted(d.glob("*/status")):
        if not which or f.parent.name in which:
            status[f.parent.name] = (f.read_text().split() or ["?"])[0]
    passed = [c for c, s in status.items() if s == "PASS"]
    failed = [c for c, s in status.items() if s != "PASS"]
    print(f"rvformal: {len(passed)} passed, {len(failed)} not passed")
    for c in failed:
        print(f"  {status[c]} {c} (logs and counterexample under {d / c})")
    return 0 if p.returncode == 0 and not failed else 1


def smoke() -> int:
    gen(stub=True, altops=True, aligned=False)
    # Elaborate rvfi_wrapper alone with riscv-formal's macros: checks the wrapper, the slave
    # model and the core's port list (docs/ISA.md section 4) fit together.
    script = (f"read -sv -D SURFCPU_RVFI -D RISCV_FORMAL -D RISCV_FORMAL_NRET=1 "
              f"-D RISCV_FORMAL_XLEN=32 -D RISCV_FORMAL_ILEN=32 "
              f"{DIR / 'checks' / 'rvfi_macros.vh'} {CORE / 'surfbus_formal_slave.sv'} "
              f"{CORE / 'stub_core.v'} {CORE / 'wrapper.sv'}; "
              f"hierarchy -check -top rvfi_wrapper; proc; check -assert")
    p = subprocess.run(["yosys", "-q", "-p", script], capture_output=True, text=True)
    if p.returncode:
        print(p.stdout[-3000:], p.stderr[-3000:])
        print("rvformal smoke: the wrapper does not elaborate")
        return 1
    print("rvformal smoke: the wrapper elaborates with surfcpu_core's ISA.md port list")
    # One instruction check end to end (yosys, sby, z3). The stand-in never retires, so the
    # check's assumption that an instruction retires cannot hold: sby must say so.
    d = CORE / "checks"
    subprocess.run(["make", "-C", str(d), "insn_add_ch0"], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    log = (d / "insn_add_ch0" / "logfile.txt").read_text(errors="replace")
    if "Assumptions are unsatisfiable" not in log:
        print(log[-2000:])
        print("rvformal smoke: the check did not run to the expected vacuous result")
        return 1
    print("rvformal smoke: insn_add_ch0 ran through yosys, sby and z3 (vacuous on the "
          "stand-in, as expected: it retires nothing)")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("command", choices=("fetch", "gen", "run", "smoke"))
    ap.add_argument("checks", nargs="*")
    ap.add_argument("-j", "--jobs", type=int, default=4)
    ap.add_argument("--stub", action="store_true")
    ap.add_argument("--no-altops", action="store_true")
    ap.add_argument("--aligned-mem", action="store_true")
    a = ap.parse_args(argv)
    if a.command == "fetch":
        print(f"rvformal: {fetch()}")
        return 0
    if a.command == "gen":
        gen(a.stub, not a.no_altops, a.aligned_mem)
        return 0
    if a.command == "run":
        return run(a.jobs, a.checks)
    return smoke()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
