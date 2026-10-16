#!/usr/bin/env python3
"""
SurfCPU verification runner: the core against Spike, program by program
--------------------
File: rvrun.py Date: 10/15/26 (SurfCPU)
--------------------
Runs test programs on a DUT command line (default build/cpu/surfcpu_tb, docs/MODULES.md
section 6) and on Spike, and judges each run: the DUT's exit status, its retirement trace
against Spike's (tools/trace_compare.py: first divergence and context), and its signature
against Spike's. Suites:

  directed   sim/ref/directed/*.S (each instruction class, every trap, CSRs, FENCE.I)
  random     tools/rvgen.py programs, one per seed
  arch       riscv-arch-test, as built by tools/rvarch.py (signatures from Spike)
  csrmap     tools/rvcsrmap.py's probe, checked against docs/ISA.md itself (not Spike)
  elf        ELF files given with --elf (for example the cpu track's tests)

    python3 tools/rvrun.py [--dut CMD] [--suites directed,random,arch,csrmap]
        [--seeds N | --seed-list 1,5,9] [--length L] [--dut-seeds 1,2,3]
        [--ilat MIN:MAX] [--dlat MIN:MAX] [--max-cycles N] [--jobs J]
        [--elf FILE ...] [--out DIR] [--json FILE] [--show N]
    python3 tools/rvrun.py selftest [--jobs J]
        the runner against tools/rvspike_tb.py injecting each kind of fault: every one
        must be caught, and the allowed variations (aligned memory layout, no final
        tohost store) must pass

Exit status: 0 every run passed, 1 something failed, 2 the setup is broken.
Run it inside the toolchain container (make test-cpu-ref / make test-ref do).
Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import json
import os
import shlex
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rvcsrmap        # noqa: E402
import rvelf           # noqa: E402
import rvgen           # noqa: E402
import trace_compare   # noqa: E402
import trace_spike     # noqa: E402
import tracefmt        # noqa: E402

ROOT = Path(__file__).resolve().parent.parent           # surfcpu/
DEFAULT_DUT = "build/cpu/surfcpu_tb"
STANDIN = f"{sys.executable} tools/rvspike_tb.py"
LINK_LD = ROOT / "sim" / "ref" / "common" / "link.ld"
CC = "riscv64-unknown-elf-gcc"
CFLAGS = ["-march=rv32im_zicsr_zifencei", "-mabi=ilp32", "-nostdlib", "-nostartfiles",
          "-Wl,--no-warn-rwx-segments", f"-I{ROOT / 'sim' / 'ref' / 'common'}", "-T",
          str(LINK_LD)]
ARCH_MANIFEST = ROOT / "build" / "ref" / "arch" / "manifest.json"
_print_lock = threading.Lock()


@dataclasses.dataclass
class Program:
    suite: str
    name: str
    elf: Path
    sig: tuple[str, str] | None = None
    ref_sig: Path | None = None          # a reference signature made elsewhere (arch)
    check: str = "trace"                 # trace | csrmap
    max_records: int | None = None
    compare_trace: bool = True           # False: judged by exit status (and signature) only
    note: str = ""


@dataclasses.dataclass
class Outcome:
    suite: str
    name: str
    dut_seed: int
    verdict: str                         # PASS | FAIL | ERROR
    reason: str
    kind: str | None = None              # the comparator's kind, "exit", "signature", ...
    retirements: int = 0
    seconds: float = 0.0
    report: str | None = None            # path of the detailed report


def say(msg: str) -> None:
    with _print_lock:
        print(msg, flush=True)


def compile_asm(src: Path, elf: Path) -> None:
    elf.parent.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([CC, *CFLAGS, "-o", str(elf), str(src)], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f"building {src} failed:\n{p.stderr}")


def _stale(out: Path, *inputs: Path) -> bool:
    return not out.exists() or any(i.stat().st_mtime > out.stat().st_mtime for i in inputs)


def has_sig(elf: Path) -> tuple[str, str] | None:
    syms = rvelf.read_elf(elf).symbols
    if "begin_signature" in syms and "end_signature" in syms:
        return ("begin_signature", "end_signature")
    return None


# --- suites ---------------------------------------------------------------------------

def suite_directed(out: Path) -> list[Program]:
    progs = []
    deps = [ROOT / "sim" / "ref" / "common" / "ref_env.h", LINK_LD]
    for src in sorted((ROOT / "sim" / "ref" / "directed").glob("*.S")):
        elf = out / "directed" / (src.stem + ".elf")
        if _stale(elf, src, *deps):
            compile_asm(src, elf)
        progs.append(Program("directed", src.stem, elf, has_sig(elf)))
    return progs


def suite_random(out: Path, seeds: list[int], length: int) -> list[Program]:
    progs = []
    for s in seeds:
        src = out / "random" / f"rand-{s}.S"
        elf = src.with_suffix(".elf")
        text, _ = rvgen.generate(s, length)
        if not src.exists() or src.read_text() != text:
            src.parent.mkdir(parents=True, exist_ok=True)
            src.write_text(text)
        if _stale(elf, src, LINK_LD):
            compile_asm(src, elf)
        progs.append(Program("random", f"rand-{s}", elf, has_sig(elf)))
    return progs


def suite_csrmap(out: Path) -> list[Program]:
    src = out / "csrmap" / "csrmap.S"
    elf = src.with_suffix(".elf")
    text = rvcsrmap.generate()
    if not src.exists() or src.read_text() != text:
        src.parent.mkdir(parents=True, exist_ok=True)
        src.write_text(text)
    if _stale(elf, src, LINK_LD):
        compile_asm(src, elf)
    return [Program("csrmap", "csrmap", elf, None, check="csrmap")]


def suite_arch(manifest: Path) -> list[Program]:
    """riscv-arch-test, as tools/rvarch.py built it: self-checking ELFs (the reference
    signature from Spike is compiled in; tohost = 1 only if every result matches), judged
    by exit status and, unless they read timing CSRs, by retirement trace."""
    if not manifest.exists():
        raise RuntimeError(f"{manifest} is missing: build the architecture tests first "
                           "(python3 tools/rvarch.py build, or make ref-arch)")
    m = json.loads(manifest.read_text())
    progs = []
    for t in m["tests"]:
        progs.append(Program("arch", t["name"], ROOT / t["elf"], None,
                             compare_trace=t["compare_trace"], note=t.get("note", "")))
    return progs


def suite_elf(paths: list[str]) -> list[Program]:
    return [Program("elf", Path(p).stem, Path(p).resolve(), has_sig(Path(p))) for p in paths]


# --- reference and DUT runs ------------------------------------------------------------

_ref_locks: dict[Path, threading.Lock] = {}
_ref_locks_guard = threading.Lock()


def reference(prog: Program) -> tuple[list[tracefmt.Rec], dict]:
    """Spike's trace (and status, signature) for a program, cached next to the ELF."""
    with _ref_locks_guard:
        lock = _ref_locks.setdefault(prog.elf, threading.Lock())
    with lock:
        tr = prog.elf.with_suffix(".ref.trace")
        meta = prog.elf.with_suffix(".ref.json")
        if not _stale(tr, prog.elf) and not _stale(meta, prog.elf):
            return tracefmt.read_trace(tr), json.loads(meta.read_text())
        res = trace_spike.run_spike(prog.elf, sig_symbols=prog.sig,
                                    max_records=prog.max_records or
                                    trace_spike.DEFAULT_MAX_RECORDS)
        if res.status == "error":
            raise RuntimeError(f"Spike failed on {prog.elf}: {'; '.join(res.warnings)}")
        tracefmt.save_trace(res.records, tr, trace_spike.trace_comments(res, prog.elf))
        info = {"status": res.status, "exit": res.exit_code, "tohost": res.tohost,
                "records": len(res.records), "warnings": res.warnings,
                "signature": res.signature}
        meta.write_text(json.dumps(info) + "\n")
        return res.records, info


def run_dut(dut: list[str], prog: Program, seed: int, args, workdir: Path):
    stem = workdir / f"{prog.name}.s{seed}"
    trace = stem.with_suffix(".dut.trace")
    sigf = stem.with_suffix(".dut.sig")
    for f in (trace, sigf):
        if f.exists():
            f.unlink()
    cmd = dut + ["--elf", str(prog.elf), "--trace", str(trace), "--seed", str(seed),
                 "--max-cycles", str(args.max_cycles)]
    if args.ilat:
        cmd += ["--ilat", args.ilat]
    if args.dlat:
        cmd += ["--dlat", args.dlat]
    if prog.sig:
        cmd += ["--signature", str(sigf), "--sig-begin", prog.sig[0], "--sig-end", prog.sig[1]]
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout,
                           cwd=ROOT)
        code, err = p.returncode, (p.stderr or "") + (p.stdout or "")
    except subprocess.TimeoutExpired:
        code, err = None, f"killed after {args.timeout} s"
    return code, trace, sigf, err.strip(), time.monotonic() - t0


def judge(prog: Program, seed: int, dut: list[str], args, workdir: Path) -> Outcome:
    t0 = time.monotonic()
    out = Outcome(prog.suite, prog.name, seed, "FAIL", "")
    try:
        ref, info = reference(prog)
    except RuntimeError as e:
        out.verdict, out.reason, out.kind = "ERROR", str(e), "reference"
        return out
    code, trace, sigf, err, _ = run_dut(dut, prog, seed, args, workdir)
    out.seconds = time.monotonic() - t0
    report_path = workdir / f"{prog.name}.s{seed}.report.txt"
    lines: list[str] = [f"program {prog.elf}", f"dut seed {seed}", f"dut exit {code}"]
    if err:
        lines.append("dut output (last lines):")
        lines += ["  " + x for x in err.splitlines()[-15:]]
    problems: list[tuple[str, str]] = []
    if code is None:
        problems.append(("timeout", f"the DUT did not finish within {args.timeout} s"))
    elif code not in (0, 1, 2):
        problems.append(("exit", f"the DUT exited with {code} (not 0/1/2: it crashed or "
                                 "refused the command line)"))
    elif code != info["exit"]:
        what = {0: "pass", 1: "fail", 2: "run past --max-cycles"}[code]
        problems.append(("exit", f"exit {code} ({what}); Spike's run gives "
                                 f"{info['exit']} ({info['status']})"))
    dut_recs = None
    if trace.exists():
        try:
            dut_recs = tracefmt.read_trace(trace)
        except tracefmt.TraceError as e:
            problems.append(("format", f"the DUT's trace cannot be read: {e}"))
    elif code is not None:
        problems.append(("trace", "the DUT wrote no trace"))
    if dut_recs is not None:
        out.retirements = len(dut_recs)
        if prog.check == "csrmap":
            rep = rvcsrmap.check(prog.elf, trace)
            if rep["issues"]:
                first = rep["issues"][0]
                problems.append(("csrmap", f"{len(rep['issues'])} CSR probes differ from "
                                           f"docs/ISA.md, first {first['csr']} "
                                           f"{first['name']} {first['probe']}: contract "
                                           f"{first['expected']}, observed {first['observed']}"))
                lines += [f"  {i['csr']} {i['name']} {i['probe']}: contract {i['expected']}, "
                          f"observed {i['observed']}" for i in rep["issues"]]
        elif prog.compare_trace:
            tohost = rvelf.read_elf(prog.elf).symbols.get("tohost")
            res = trace_compare.compare(ref, dut_recs, tohost=tohost,
                                        strict_final_store=args.strict_final_store)
            lines.append(trace_compare.report(res, str(prog.elf.with_suffix(".ref.trace")),
                                              str(trace)))
            if not res.ok:
                problems.append((res.kind, res.summary()))
        else:
            lines.append(f"trace not compared: {prog.note or 'timing-dependent program'}")
    if prog.sig and code is not None and prog.check == "trace":
        want = info.get("signature")
        if prog.ref_sig is not None:
            want = trace_spike.read_signature_file(prog.ref_sig)
        if not sigf.exists():
            problems.append(("signature", "the DUT wrote no signature"))
        elif want is not None:
            got = trace_spike.read_signature_file(sigf)
            if got != want:
                begin = rvelf.read_elf(prog.elf).symbol(prog.sig[0])
                k = next((i for i, (g, w) in enumerate(zip(got, want)) if g != w),
                         min(len(got), len(want)))
                if k < len(got) and k < len(want):
                    detail = (f"word {k} (0x{begin + 4 * k:08x}) is {got[k]:08x}, Spike's "
                              f"{want[k]:08x}")
                else:
                    detail = f"word {k} (0x{begin + 4 * k:08x}) is missing in one of them"
                problems.append(("signature", f"signature differs ({len(got)} words, Spike "
                                              f"{len(want)}): first at {detail}"))
    report_path.write_text("\n".join(lines) + "\n")
    out.report = str(report_path)
    if problems:
        out.kind, out.reason = problems[0]
        if len(problems) > 1:
            out.reason += "; also " + "; ".join(f"{k}: {r}" for k, r in problems[1:])
    else:
        out.verdict = "PASS"
        if prog.check == "csrmap":
            out.reason = "every CSR probe as docs/ISA.md says"
        elif prog.compare_trace:
            out.reason = f"{out.retirements} retirements agree" + (
                ", signature agrees" if prog.sig else "")
        else:
            out.reason = f"exit status agrees ({out.retirements} retirements; trace not " \
                         f"compared: {prog.note or 'timing'})"
    return out


# --- the run ----------------------------------------------------------------------------

def collect(args, out: Path) -> list[Program]:
    suites = [s for s in args.suites.split(",") if s]
    progs: list[Program] = []
    for s in suites:
        if s == "directed":
            progs += suite_directed(out)
        elif s == "random":
            seeds = ([int(x) for x in args.seed_list.split(",")] if args.seed_list
                     else list(range(1, args.seeds + 1)))
            progs += suite_random(out, seeds, args.length)
        elif s == "arch":
            progs += suite_arch(Path(args.arch_manifest))
        elif s == "csrmap":
            progs += suite_csrmap(out)
        elif s == "elf":
            pass
        else:
            raise RuntimeError(f"unknown suite {s!r}")
    if args.elf:
        progs += suite_elf(args.elf)
    return progs


def run(args) -> int:
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    dut = shlex.split(args.dut)
    exe = dut[1] if Path(dut[0]).name.startswith("python") and len(dut) > 1 else dut[0]
    if not ((ROOT / exe).exists() or Path(exe).exists() or shutil.which(exe)):
        say(f"rvrun: the DUT {exe} does not exist (the cpu track builds "
            f"{DEFAULT_DUT}; until then try --dut \"{STANDIN}\")")
        return 2
    try:
        progs = collect(args, out)
    except RuntimeError as e:
        say(f"rvrun: {e}")
        return 2
    dut_seeds = [int(x) for x in args.dut_seeds.split(",")]
    say(f"rvrun: {len(progs)} programs x {len(dut_seeds)} DUT seed(s) on {args.dut} "
        f"(jobs {args.jobs}, results in {out})")
    jobs = [(p, s) for p in progs for s in dut_seeds]
    outcomes: list[Outcome] = []
    t0 = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {}
        for p, s in jobs:
            wd = out / p.suite
            wd.mkdir(parents=True, exist_ok=True)
            futs[ex.submit(judge, p, s, dut, args, wd)] = (p, s)
        for f in concurrent.futures.as_completed(futs):
            o = f.result()
            outcomes.append(o)
            say(f"{o.verdict:5} {o.suite}/{o.name} seed {o.dut_seed}: {o.reason}")
    outcomes.sort(key=lambda o: (o.suite, o.name, o.dut_seed))
    failed = [o for o in outcomes if o.verdict != "PASS"]
    for o in failed[:args.show]:
        say(f"\n--- {o.suite}/{o.name} seed {o.dut_seed} ({o.report})")
        say(Path(o.report).read_text().rstrip() if o.report else o.reason)
    by_suite: dict[str, list[int]] = {}
    for o in outcomes:
        c = by_suite.setdefault(o.suite, [0, 0])
        c[0 if o.verdict == "PASS" else 1] += 1
    total_ret = sum(o.retirements for o in outcomes)
    say("\nrvrun summary: " + ", ".join(f"{s} {c[0]} passed {c[1]} failed"
                                        for s, c in sorted(by_suite.items()))
        + f"; {total_ret} retirements compared in {time.monotonic() - t0:.1f} s")
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps({
            "dut": args.dut, "dut_seeds": dut_seeds, "suites": args.suites,
            "outcomes": [dataclasses.asdict(o) for o in outcomes]}, indent=1) + "\n")
    return 1 if failed else 0


# --- self-test --------------------------------------------------------------------------

SELFTEST_FAULTS = [
    # (stand-in options, expected verdict, expected kind or None)
    ("", "PASS", None),
    ("--aligned-mem", "PASS", None),
    ("--no-final-store", "PASS", None),
] + [(f"--inject {k}", "FAIL", e) for k, e in (
    ("pc", "pc"), ("insn", "insn"), ("trap", "trap"), ("notrap", "trap"), ("intr", "intr"),
    ("rd_data", "rd"), ("rd_addr", "rd"), ("rd_x0", "rd"), ("trap_write", "rd"),
    ("mem_addr", "mem_addr"), ("mem_mask", "mem_mask"), ("mem_wdata", "mem_data"),
    ("mem_rdata", "mem_data"), ("load_addr", "mem_addr"), ("load_unreported", "mem_mask"),
    ("trap_store", "mem_mask"), ("pc_next", "pc_next"), ("missing", "missing"),
    ("extra", "extra"), ("order", "order"), ("truncate", "missing"))] + [
    ("--bad-signature", "FAIL", "signature"),
    ("--exit 1", "FAIL", "exit"),
    ("--exit 2", "FAIL", "exit"),
    ("--exit 139", "FAIL", "exit"),
]


def selftest(args) -> int:
    out = Path(args.out).resolve() / "selftest"
    out.mkdir(parents=True, exist_ok=True)
    progs = [p for p in suite_directed(out) if p.name in ("traps", "mem")]
    progs += suite_random(out, [7], 1500)
    bad = []
    n = 0
    lock = threading.Lock()

    def one(opts, verdict, kind, prog):
        nonlocal n
        sub = argparse.Namespace(**vars(args))
        sub.dut = f"{STANDIN} {opts}".strip()
        wd = out / (opts.replace(" ", "").replace("-", "") or "clean")
        wd.mkdir(parents=True, exist_ok=True)
        o = judge(prog, 1, shlex.split(sub.dut), sub, wd)
        ok = o.verdict == verdict and (kind is None or o.kind == kind)
        with lock:
            n += 1
            if not ok:
                bad.append(f"{opts or '(clean)'} on {prog.name}: got {o.verdict} {o.kind} "
                           f"({o.reason}), expected {verdict} {kind}")
        return ok

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(one, opts, v, k, p) for opts, v, k in SELFTEST_FAULTS for p in progs]
        for f in futs:
            f.result()
    for b in bad:
        say("rvrun selftest: " + b)
    say(f"rvrun selftest: {n - len(bad)}/{n} runs judged as expected "
        f"({len(SELFTEST_FAULTS)} stand-in behaviours x {len(progs)} programs)")
    return 1 if bad else 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("command", nargs="?", default="run", choices=("run", "selftest"))
    ap.add_argument("--dut", default=DEFAULT_DUT, help="DUT command (default %(default)s)")
    ap.add_argument("--suites", default="directed,random,arch,csrmap")
    ap.add_argument("--seeds", type=int, default=20, help="random programs: seeds 1..N")
    ap.add_argument("--seed-list", help="random programs: these seeds instead")
    ap.add_argument("--length", type=int, default=3000, help="random program length")
    ap.add_argument("--dut-seeds", default="1", help="the DUT's --seed values, e.g. 1,2,3")
    ap.add_argument("--ilat", help="instruction-port latency MIN:MAX for the DUT")
    ap.add_argument("--dlat", help="data-port latency MIN:MAX for the DUT")
    ap.add_argument("--max-cycles", type=int, default=20_000_000)
    ap.add_argument("--timeout", type=float, default=900.0, help="seconds per DUT run")
    ap.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    ap.add_argument("--elf", action="append", help="also run this ELF (suite 'elf')")
    ap.add_argument("--arch-manifest", default=str(ARCH_MANIFEST))
    ap.add_argument("--out", default=str(ROOT / "build" / "ref" / "run"))
    ap.add_argument("--json", help="write every outcome here")
    ap.add_argument("--show", type=int, default=3, help="print this many failure reports")
    ap.add_argument("--strict-final-store", action="store_true",
                    help="the DUT's trace must contain the final tohost store")
    a = ap.parse_args(argv)
    if a.command == "selftest":
        return selftest(a)
    return run(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
