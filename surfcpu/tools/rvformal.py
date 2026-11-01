#!/usr/bin/env python3
"""
SurfCPU riscv-formal setup: pinned framework, wrapper, checks for surfcpu_core
--------------------
File: rvformal.py Date: 10/15/26 (SurfCPU)
--------------------
    python3 tools/rvformal.py fetch               riscv-formal at a pinned commit (SHA-256)
    python3 tools/rvformal.py gen [--stub] [--altops] [--lsb-mem] [--root TREE] [--solver S]
                                  [--engine LINE] [--depth NAME=N ...]
                                                  copy the wrapper, the SurfBus slave model and
                                                  the core's RTL (CPU_RTL of mk/area-cpu.mk)
                                                  into riscv-formal's cores/surfcpu/ and
                                                  generate the checks
    python3 tools/rvformal.py run [-j N] [--timeout S] [CHECK...]
                                                  run them with SymbiYosys (all, or some)
    python3 tools/rvformal.py smoke               before the core exists: elaborate the wrapper
                                                  with sim/ref/formal/stub_core.v (the ISA.md
                                                  port list) and generate the checks

riscv-formal (github.com/YosysHQ/riscv-formal) proves each instruction's RVFI record against
its formal ISA model, and the consistency of pc, registers and order over bounded depths. The
configuration is sim/ref/formal/checks.cfg; the core needs SURFCPU_RVFI. The core's RVFI
memory fields follow RISCV_FORMAL_ALIGNED_MEM (MODULES.md section 5), so that is defined
(--lsb-mem drops it). The core does not implement riscv-formal's alternative M operations
(RISCV_FORMAL_ALTOPS), so the eight M instruction checks are left out (real multipliers and
dividers are beyond a bounded proof); --altops puts them back once it does.
Everything goes under build/third_party/riscv-formal/.
Only the Python standard library is used (the checks need yosys, sby and a solver: the
toolchain container has yosys 0.52 with yosys-abc, sby, z3 and boolector; checks.cfg uses
ABC's bmc3, see the reasons there).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rvfetch   # noqa: E402

ROOT = Path(__file__).resolve().parent.parent            # surfcpu/
THIRD = ROOT / "build" / "third_party"
FORMAL = ROOT / "sim" / "ref" / "formal"

COMMIT = "c992aa61fdfe0846c5ed90324c596202a1c69b76"
URL = f"https://codeload.github.com/YosysHQ/riscv-formal/tar.gz/{COMMIT}"
SHA256 = "0e357116da2aaa0b57e6dab6fd9ff7612e04d4fdc37372754f72ec963b01e000"
DIR = THIRD / "riscv-formal" / "src" / f"riscv-formal-{COMMIT}"
CORE = DIR / "cores" / "surfcpu"
STUB = DIR / "cores" / "surfcpu_stub"         # the smoke check's, so it never replaces CORE
M_INSNS = ("mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu")


def fetch() -> Path:
    """riscv-formal at the pinned commit, the cpu track's way (tools/rvfetch.py)."""
    archive = rvfetch.fetch(URL, SHA256, THIRD / "riscv-formal" / f"riscv-formal-{COMMIT}.tar.gz",
                            "rvformal")
    return rvfetch.unpack(archive, THIRD / "riscv-formal" / "src")


def gen(stub: bool, altops: bool, aligned: bool, rtl_root: Path = ROOT,
        solver: str | None = None, engine: str | None = None,
        depths: list[str] | None = None) -> Path:
    fetch()
    core = STUB if stub else CORE
    if core.exists():
        shutil.rmtree(core)
    core.mkdir(parents=True)
    # CPU_RTL of mk/area-cpu.mk: the core's modules and the FIFO it uses.
    rtl = [] if stub else (sorted((rtl_root / "rtl" / "cpu").glob("surfcpu_*.v"))
                           + [rtl_root / "rtl" / "lib" / "surf_fifo.v"])
    if not stub and not rtl:
        raise SystemExit("rvformal: rtl/cpu has no core yet (the cpu track's surfcpu_core); "
                         "use `smoke` to check the setup against the port list")
    files = ["wrapper.sv", "surfbus_formal_slave.sv"]
    for f in files:
        shutil.copy(FORMAL / f, core / f)
    for inc in [*sorted((rtl_root / "rtl" / "cpu").glob("*.vh")),
                *sorted((rtl_root / "rtl" / "lib").glob("*.vh")),
                ROOT / "rtl" / "generated" / "surfcpu_soc.vh"]:
        shutil.copy(inc, core / inc.name)
    if stub:
        shutil.copy(FORMAL / "stub_core.v", core / "stub_core.v")
        files.append("stub_core.v")
    for f in rtl:
        if f.name != "surfcpu_rvfi.vh":
            shutil.copy(f, core / f.name)
            files.append(f.name)
    cfg = (FORMAL / "checks.cfg").read_text()
    if solver:                            # riscv-formal: z3, boolector, bmc3 (ABC), btormc
        cfg = re.sub(r"(?m)^solver \S+$", f"solver {solver}", cfg)
    solver = re.search(r"(?m)^solver (\S+)$", cfg).group(1)
    for d in depths or []:                # NAME=N: the last number of NAME's [depth] line
        name, _, n = d.partition("=")
        cfg, k = re.subn(rf"(?m)^({re.escape(name)}\s.*?)(\d+)\s*$", rf"\g<1>{int(n)}", cfg)
        if k != 1:
            raise SystemExit(f"rvformal: no [depth] line for {name!r} in checks.cfg")
    defines = ["`define SURFCPU_RVFI"]
    if altops:
        defines.append("`define RISCV_FORMAL_ALTOPS")
    if aligned:
        defines.append("`define RISCV_FORMAL_ALIGNED_MEM")
    filters = [] if altops else [f"- insn_{m}_ch0" for m in M_INSNS]
    cfg = cfg.replace("@DEFINES@", "\n".join(defines))
    cfg = cfg.replace("@FILTERS@", "\n".join(filters))
    cfg = cfg.replace("@FILES@", "\n".join(f"@basedir@/cores/@core@/{f}" for f in files))
    (core / "checks.cfg").write_text(cfg)
    subprocess.run([sys.executable, str(DIR / "checks" / "genchecks.py")], cwd=core, check=True,
                   stdout=subprocess.DEVNULL)
    checks = sorted(p.stem for p in (core / "checks").glob("*.sby"))
    for c in checks:
        f = core / "checks" / f"{c}.sby"
        text = f.read_text().split("\n")
        if engine:                        # e.g. "smtbmc --nounroll boolector"
            text[text.index("[engines]") + 1] = engine
        if solver == "bmc3" or (engine or "").startswith("abc"):
            # sby takes "skip" (no assertions before the last step) only for smtbmc and
            # btor; riscv-formal's checks assert only in the last step anyway. A failure's
            # trace is replayed with z3 (sby's default, yices, is not in the container).
            text = [t for t in text if not t.startswith("skip ")]
            text.insert(text.index("[options]") + 1, "aigsmt z3")
        f.write_text("\n".join(text))
    print(f"rvformal: {len(checks)} checks generated in {core / 'checks'}"
          + (f" (engine {engine})" if engine else ""))
    return core


def run(jobs: int, which: list[str], timeout: float | None = None) -> int:
    """Run the checks (all, or those named) with make -k; after timeout seconds the checks
    still running are stopped and reported as TIMEOUT."""
    d = CORE / "checks"
    if not d.exists():
        raise SystemExit("rvformal: run `gen` first")
    names = which or sorted(p.stem for p in d.glob("*.sby"))
    for c in names:                       # a fresh run: no status left from an earlier one
        if (d / c).is_dir():
            shutil.rmtree(d / c)
    t0 = time.monotonic()
    p = subprocess.Popen(["make", "-C", str(d), f"-j{jobs}", "-k", *names],
                         stdout=subprocess.DEVNULL, start_new_session=True)
    try:
        rc = p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        rc = None
    status = {}
    for c in names:
        f = d / c / "status"
        if f.exists():
            status[c] = (f.read_text().split() or ["?"])[0]
        else:
            status[c] = "TIMEOUT" if rc is None else "NOT-RUN"
    passed = [c for c, s in status.items() if s == "PASS"]
    failed = [c for c, s in status.items() if s != "PASS"]
    print(f"rvformal: {len(passed)} passed, {len(failed)} not passed "
          f"({time.monotonic() - t0:.0f} s, {jobs} jobs)")
    for c in passed:
        print(f"  PASS {c}")
    for c in failed:
        print(f"  {status[c]} {c} (logs and counterexample under {d / c})")
    return 0 if rc == 0 and not failed else 1


def smoke() -> int:
    gen(stub=True, altops=True, aligned=True, solver="z3")
    # Elaborate rvfi_wrapper alone with riscv-formal's macros: checks the wrapper, the slave
    # model and the core's port list (docs/ISA.md section 4) fit together.
    script = (f"read -sv -D SURFCPU_RVFI -D RISCV_FORMAL -D RISCV_FORMAL_NRET=1 "
              f"-D RISCV_FORMAL_XLEN=32 -D RISCV_FORMAL_ILEN=32 "
              f"{DIR / 'checks' / 'rvfi_macros.vh'} {STUB / 'surfbus_formal_slave.sv'} "
              f"{STUB / 'stub_core.v'} {STUB / 'wrapper.sv'}; "
              f"hierarchy -check -top rvfi_wrapper; proc; check -assert")
    p = subprocess.run(["yosys", "-q", "-p", script], capture_output=True, text=True)
    if p.returncode:
        print(p.stdout[-3000:], p.stderr[-3000:])
        print("rvformal smoke: the wrapper does not elaborate")
        return 1
    print("rvformal smoke: the wrapper elaborates with surfcpu_core's ISA.md port list")
    # One instruction check end to end (yosys, sby, z3). The stand-in never retires, so the
    # check's assumption that an instruction retires cannot hold: sby must say so.
    d = STUB / "checks"
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
    ap.add_argument("--altops", action="store_true")
    ap.add_argument("--lsb-mem", action="store_true")
    ap.add_argument("--root", help="another surfcpu tree to take the core's RTL from")
    ap.add_argument("--timeout", type=float, help="run: stop after this many seconds")
    ap.add_argument("--solver", help="gen: instead of checks.cfg's z3 (bmc3 = ABC's bmc3)")
    ap.add_argument("--engine", help="gen: the sby [engines] line for every check, e.g. "
                                     "'smtbmc --nounroll boolector'")
    ap.add_argument("--depth", action="append",
                    help="gen: NAME=N sets the depth of checks.cfg's NAME line (repeatable)")
    a = ap.parse_args(argv)
    if a.command == "fetch":
        print(f"rvformal: {fetch()}")
        return 0
    if a.command == "gen":
        gen(a.stub, a.altops, not a.lsb_mem, Path(a.root) if a.root else ROOT, a.solver,
            a.engine, a.depth)
        return 0
    if a.command == "run":
        return run(a.jobs, a.checks, a.timeout)
    return smoke()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
