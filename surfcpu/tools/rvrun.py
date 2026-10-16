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
        [--ilat MIN:MAX] [--dlat MIN:MAX] [--max-cycles N] [--jobs J] [--keep-programs]
        [--elf FILE ...] [--out DIR] [--json FILE] [--show N] [-v]
    python3 tools/rvrun.py selftest [--jobs J]
        the runner against tools/rvspike_tb.py injecting each kind of fault: every one
        must be caught, and the allowed variations (riscv-formal's default memory layout
        with --lenient-layout, no final tohost store with --lenient-final-store) must pass

Each program runs once on Spike (its trace kept in memory, never written) and then on the
DUT once per DUT seed. A pass leaves nothing on disk, and a passing random program's
generated files are removed too (--keep-programs keeps them). A failure leaves its report,
the DUT's signature and both traces cut to 20000 records before the divergence and 2000
after it. DUT runs are bounded: --max-cycles defaults to a multiple of Spike's retirement
count (cycle_limit), and a DUT whose trace grows far past Spike's is stopped.

Exit status: 0 every run passed, 1 something failed, 2 the setup is broken.
Run it inside the toolchain container (make test-cpu-ref / make test-ref do).
Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import itertools
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
import trace_inject    # noqa: E402
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
    fixed_latency: str = ""              # why --ilat/--dlat are not passed for this program


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
                             compare_trace=t["compare_trace"], note=t.get("note", ""),
                             fixed_latency=t.get("latency_sensitive", "")))
    return progs


def suite_elf(paths: list[str]) -> list[Program]:
    return [Program("elf", Path(p).stem, Path(p).resolve(), has_sig(Path(p))) for p in paths]


# --- reference and DUT runs ------------------------------------------------------------

def reference(prog: Program) -> tuple[list[tracefmt.Rec], dict]:
    """Spike's trace, status and signature for a program, in memory only (the runner keeps no
    reference trace on disk except next to a failure)."""
    res = trace_spike.run_spike(prog.elf, sig_symbols=prog.sig,
                                max_records=prog.max_records or trace_spike.DEFAULT_MAX_RECORDS)
    if res.status == "error":
        raise RuntimeError(f"Spike failed on {prog.elf}: {'; '.join(res.warnings)}")
    info = {"status": res.status, "exit": res.exit_code, "tohost": res.tohost,
            "records": len(res.records), "warnings": res.warnings,
            "signature": res.signature, "comments": trace_spike.trace_comments(res, prog.elf)}
    return res.records, info


# What a run leaves on disk. A pass: nothing. A failure: its report, the DUT's signature and
# both traces cut to KEEP_BEFORE records before the divergence and KEEP_AFTER after it.
KEEP_BEFORE = 20_000
KEEP_AFTER = 2_000
# The testbench writes about 85 bytes per record; a DUT whose trace grows past this many
# bytes per reference record (plus KEEP_AFTER + 1000 records of slack) is stopped.
TRACE_BYTES_PER_RECORD = 200


def _lat_max(spec: str | None) -> int:
    return int(spec.split(":")[-1]) if spec else 1      # the testbench's default is 1:1


def cycle_limit(prog: Program, args, records: int) -> int:
    """The DUT's --max-cycles unless one is given. Measured on the core (surfcpu@e792960):
    at most 5.4 cycles per retirement with the default 1:1 latencies, 6.7 at 1:4 and 22.6 at
    --ilat/--dlat 6:30. This allows 64 plus twice the two maximum latencies per reference
    retirement (68 at 1:1, 184 at 6:30), plus 200000 for short programs."""
    if args.max_cycles:
        return args.max_cycles
    if prog.fixed_latency:
        lat = 2
    else:
        lat = _lat_max(args.ilat) + _lat_max(args.dlat)
    return (64 + 2 * lat) * max(records, 1) + 200_000


def _tail(path: Path, limit: int = 16384) -> str:
    with path.open("rb") as fh:
        fh.seek(max(0, path.stat().st_size - limit))
        return fh.read().decode("utf-8", "replace").strip()


def trace_byte_limit(ref_records: int) -> int:
    return TRACE_BYTES_PER_RECORD * (ref_records + KEEP_AFTER + 1000) + 65536


def run_dut(dut: list[str], prog: Program, seed: int, args, workdir: Path, max_cycles: int,
            ref_records: int):
    """One DUT run, watched: stopped after --timeout seconds, or once its trace passes
    trace_byte_limit (a runaway core would otherwise fill the disk before --max-cycles)."""
    max_trace_bytes = trace_byte_limit(ref_records)
    trace = workdir / f"{prog.name}.s{seed}.dut.trace"
    sigf = workdir / f"{prog.name}.s{seed}.dut.sig"
    log = workdir / f"{prog.name}.s{seed}.dut.log"
    for f in (trace, sigf, log):
        if f.exists():
            f.unlink()
    cmd = dut + ["--elf", str(prog.elf), "--trace", str(trace), "--seed", str(seed),
                 "--max-cycles", str(max_cycles)]
    if args.ilat and not prog.fixed_latency:
        cmd += ["--ilat", args.ilat]
    if args.dlat and not prog.fixed_latency:
        cmd += ["--dlat", args.dlat]
    if prog.sig:
        cmd += ["--signature", str(sigf), "--sig-begin", prog.sig[0], "--sig-end", prog.sig[1]]
    t0 = time.monotonic()
    stopped = None
    with log.open("wb") as fh:
        p = subprocess.Popen(cmd, stdout=fh, stderr=subprocess.STDOUT, cwd=ROOT)
        while True:
            try:
                code = p.wait(timeout=0.05)
                break
            except subprocess.TimeoutExpired:
                pass
            if time.monotonic() - t0 > args.timeout:
                stopped = ("timeout", f"the DUT did not finish within {args.timeout:g} s")
            elif trace.exists() and trace.stat().st_size > max_trace_bytes:
                stopped = ("runaway", f"stopped: the DUT's trace passed {max_trace_bytes} "
                                      f"bytes, while Spike's run has {ref_records} records "
                                      f"(the testbench writes about 85 bytes each)")
            if stopped:
                p.kill()
                p.wait()
                code = None
                break
    err = _tail(log)
    log.unlink()
    return code, trace, sigf, err, time.monotonic() - t0, stopped


def _complete_lines(fh):
    """The lines of a file whose writer was killed: a last line cut short is dropped."""
    for line in fh:
        if line.endswith("\n"):
            yield line


def read_dut_trace(trace: Path, limit: int, killed: bool) -> list[tracefmt.Rec]:
    """At most limit records of the DUT's trace (more are never needed to judge it)."""
    with trace.open("r", encoding="utf-8", errors="replace") as fh:
        lines = _complete_lines(fh) if killed else fh
        return list(itertools.islice(tracefmt.parse_lines(lines, str(trace)), limit))


def judge(prog: Program, seed: int, dut: list[str], args, workdir: Path,
          refinfo: tuple[list[tracefmt.Rec], dict] | None = None) -> Outcome:
    t0 = time.monotonic()
    out = Outcome(prog.suite, prog.name, seed, "FAIL", "")
    try:
        ref, info = refinfo if refinfo is not None else reference(prog)
    except (RuntimeError, trace_spike.ConvertError) as e:
        out.verdict, out.reason, out.kind = "ERROR", str(e), "reference"
        return out
    n_ref = info["records"]
    limit = n_ref + KEEP_AFTER + 16                 # DUT records read: enough to judge
    code, trace, sigf, err, _, stopped = run_dut(dut, prog, seed, args, workdir,
                                                 cycle_limit(prog, args, n_ref), n_ref)
    out.seconds = time.monotonic() - t0
    report_path = workdir / f"{prog.name}.s{seed}.report.txt"
    ref_path = workdir / f"{prog.name}.s{seed}.ref.trace"
    lines: list[str] = [f"program {prog.elf}", f"dut seed {seed}",
                        f"dut exit {code if code is not None else '(none: stopped)'}"]
    if prog.fixed_latency and (args.ilat or args.dlat):
        lines.append(f"run with the testbench's default latencies: {prog.fixed_latency}")
    if err:
        lines.append("dut output (last lines):")
        lines += ["  " + x for x in err.splitlines()[-15:]]
    problems: list[tuple[str, str]] = []
    if stopped:
        problems.append(stopped)
    elif code == 3:
        problems.append(("exit", "exit 3: a testbench error (bad arguments or ELF, no tohost, "
                                 "a bus protocol violation by the core, or a tohost store "
                                 "that never retired; MODULES.md section 6)"))
    elif code not in (0, 1, 2):
        problems.append(("exit", f"the DUT exited with {code} (not 0-3: it crashed)"))
    elif code != info["exit"]:
        what = {0: "pass", 1: "fail", 2: "run past --max-cycles"}[code]
        problems.append(("exit", f"exit {code} ({what}); Spike's run gives "
                                 f"{info['exit']} ({info['status']})"))
    dut_recs = None
    diverged_at = None
    if trace.exists():
        try:
            dut_recs = read_dut_trace(trace, limit + 1, stopped is not None)
        except tracefmt.TraceError as e:
            problems.append(("format", f"the DUT's trace cannot be read: {e}"))
    elif not stopped:
        problems.append(("trace", "the DUT wrote no trace"))
    if dut_recs is not None:
        if len(dut_recs) > limit:
            dut_recs = dut_recs[:limit]
            lines.append(f"the DUT's trace has more than {limit} records; the first {limit} "
                         f"were read")
        out.retirements = len(dut_recs)
        if prog.check == "csrmap":
            rep = rvcsrmap.check(prog.elf, dut_recs)
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
                                        lenient_final_store=args.lenient_final_store,
                                        lenient_layout=args.lenient_layout)
            lines.append(trace_compare.report(res, str(ref_path), str(trace)))
            if not res.ok:
                problems.append((res.kind, res.summary()))
                diverged_at = res.index
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
    if problems:
        out.kind, out.reason = problems[0]
        if len(problems) > 1:
            out.reason += "; also " + "; ".join(f"{k}: {r}" for k, r in problems[1:])
        lines[3:3] = [f"FAIL ({k}): {r}" for k, r in problems]
        report_path.write_text("\n".join(lines) + "\n")
        out.report = str(report_path)
        _keep_failure(prog, trace, ref, dut_recs, diverged_at, info, ref_path)
        return out
    for f in (trace, sigf, report_path, ref_path):      # a pass leaves nothing on disk
        if f.exists():
            f.unlink()
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


def _keep_failure(prog: Program, trace: Path, ref: list[tracefmt.Rec], dut_recs,
                  diverged_at: int | None, info: dict, ref_path: Path) -> None:
    """Cut a failing run's traces down to what explains it: KEEP_BEFORE records before the
    divergence and KEEP_AFTER after it, or the last KEEP_AFTER when the traces agree (an exit
    status or signature failure). The CSR probe's trace is kept whole (it is checked whole)."""
    if dut_recs is None:
        if trace.exists() and trace.stat().st_size > 1_000_000:
            with trace.open("rb") as fh:                # unreadable: keep its first 1 MB
                head = fh.read(1_000_000)
            trace.write_bytes(head)
        lo, hi = max(0, len(ref) - KEEP_AFTER), len(ref)
        what = f"records {lo}..{hi - 1}, the end of the run"
    elif prog.check == "csrmap":
        tracefmt.save_trace(dut_recs, trace, ["the DUT's trace, as read by the runner"])
        return
    else:
        if diverged_at is not None:
            lo, hi = max(0, diverged_at - KEEP_BEFORE), diverged_at + KEEP_AFTER
            what = f"records {lo}..{hi - 1}, around the divergence at {diverged_at}"
        else:
            n = max(len(ref), len(dut_recs))
            lo, hi = max(0, n - KEEP_AFTER), n
            what = f"records {lo}..{hi - 1}, the end of the run"
        tracefmt.save_trace(dut_recs[lo:hi], trace, [f"the DUT's trace: {what}"])
    tracefmt.save_trace(ref[lo:hi], ref_path, [f"Spike's trace: {what}", *info["comments"]])


def run_program(prog: Program, seeds: list[int], dut: list[str], args,
                workdir: Path) -> list[Outcome]:
    """Every DUT seed of one program against one Spike run, which is kept in memory."""
    try:
        refinfo = reference(prog)
    except (RuntimeError, trace_spike.ConvertError, OSError) as e:
        return [Outcome(prog.suite, prog.name, s, "ERROR", str(e), "reference") for s in seeds]
    outs = [judge(prog, s, dut, args, workdir, refinfo) for s in seeds]
    if (prog.suite == "random" and not args.keep_programs
            and all(o.verdict == "PASS" for o in outs)):
        for f in (prog.elf, prog.elf.with_suffix(".S")):
            if f.exists():
                f.unlink()
    return outs


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
    outcomes: list[Outcome] = []
    t0 = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = []
        for p in progs:
            wd = out / p.suite
            wd.mkdir(parents=True, exist_ok=True)
            futs.append(ex.submit(run_program, p, dut_seeds, dut, args, wd))
        for f in concurrent.futures.as_completed(futs):
            for o in f.result():
                outcomes.append(o)
                if o.verdict != "PASS" or args.verbose:
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
    # (stand-in options, runner options, expected verdict, expected kind or None)
    ("", "", "PASS", None),
    ("--lsb-mem", "", "FAIL", "mem_layout"),
    ("--lsb-mem", "--lenient-layout", "PASS", None),
    ("--no-final-store", "", "FAIL", "missing"),
    ("--no-final-store", "--lenient-final-store", "PASS", None),
] + [(f"--inject {k}", "", "FAIL", trace_inject.KINDS[k][1]) for k in trace_inject.KINDS] + [
    ("--bad-signature", "", "FAIL", "signature"),
    ("--exit 1", "", "FAIL", "exit"),
    ("--exit 2", "", "FAIL", "exit"),
    ("--exit 3", "", "FAIL", "exit"),
    ("--exit 139", "", "FAIL", "exit"),
    ("--runaway", "", "FAIL", "runaway"),
]

_RUN_FILES = ("*.dut.trace", "*.dut.sig", "*.dut.log", "*.ref.trace", "*.report.txt")


def _clean(wd: Path) -> None:
    """Remove the runner's own files left in a work directory by an earlier run."""
    wd.mkdir(parents=True, exist_ok=True)
    for pat in _RUN_FILES:
        for f in wd.glob(pat):
            f.unlink()


def selftest(args) -> int:
    out = Path(args.out).resolve() / "selftest"
    out.mkdir(parents=True, exist_ok=True)
    progs = [p for p in suite_directed(out) if p.name in ("traps", "mem")]
    progs += suite_random(out, [7], 1500)
    refs = {p.name: reference(p) for p in progs}
    bad: list[str] = []
    n = 0
    lock = threading.Lock()
    # The most a failing run may leave: two traces of KEEP_BEFORE + KEEP_AFTER records, a
    # report and a signature.
    most = 2 * TRACE_BYTES_PER_RECORD * (KEEP_BEFORE + KEEP_AFTER) + 1_000_000

    def workdir(opts: str, ropts: str) -> Path:
        return out / ((opts + ropts).replace(" ", "").replace("-", "") or "clean")

    def one(opts, ropts, verdict, kind, prog):
        nonlocal n
        sub = argparse.Namespace(**vars(args))
        sub.dut = f"{STANDIN} {opts}".strip()
        sub.lenient_layout = "--lenient-layout" in ropts
        sub.lenient_final_store = "--lenient-final-store" in ropts
        wd = workdir(opts, ropts)
        o = judge(prog, 1, shlex.split(sub.dut), sub, wd, refs[prog.name])
        left = sorted(wd.glob(f"{prog.name}.s1.*"))
        size = sum(f.stat().st_size for f in left)
        ok = o.verdict == verdict and (kind is None or o.kind == kind)
        why = ""
        if ok and verdict == "PASS" and left:
            ok, why = False, f"; the pass left {[f.name for f in left]}"
        if ok and verdict == "FAIL" and (o.report is None or size > most):
            ok, why = False, f"; the failure left {size} bytes in {[f.name for f in left]}"
        with lock:
            n += 1
            if not ok:
                bad.append(f"{opts or '(clean)'} {ropts} on {prog.name}: got {o.verdict} "
                           f"{o.kind} ({o.reason}), expected {verdict} {kind}{why}")
        return ok

    for opts, ropts, _, _ in SELFTEST_FAULTS:
        _clean(workdir(opts, ropts))
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(one, opts, ropts, v, k, p)
                for opts, ropts, v, k in SELFTEST_FAULTS for p in progs]
        for f in futs:
            f.result()
    sub = argparse.Namespace(**vars(args))
    sub.lenient_layout = sub.lenient_final_store = False
    # Several DUT seeds of one program, as a run does them: each failing run keeps its own
    # files, and passing runs keep none.
    wd = out / "seeds"
    _clean(wd)
    outs = run_program(progs[0], [1, 2, 3], shlex.split(f"{STANDIN} --inject rd_data"), sub, wd)
    reports = sorted(Path(o.report).name for o in outs if o.report)
    kept = sorted(f.name for f in wd.glob(f"{progs[0].name}.s*.trace"))
    n += 1
    if len(set(reports)) != 3 or len(kept) != 6 or any(o.kind != "rd" for o in outs):
        bad.append(f"three failing DUT seeds: reports {reports}, traces {kept}, kinds "
                   f"{[o.kind for o in outs]}")
    wd = out / "seeds-pass"
    _clean(wd)
    outs = run_program(progs[0], [1, 2, 3], shlex.split(STANDIN), sub, wd)
    left = sorted(f.name for f in wd.iterdir())
    n += 1
    if any(o.verdict != "PASS" for o in outs) or left:
        bad.append(f"three passing DUT seeds: {[o.verdict for o in outs]}, left {left}")
    # A random program that passes is removed unless --keep-programs.
    rnd = progs[-1]
    for keep in (True, False):
        sub.keep_programs = keep
        outs = run_program(rnd, [1], shlex.split(STANDIN), sub, out / "random")
        n += 1
        if outs[0].verdict != "PASS" or rnd.elf.exists() != keep:
            bad.append(f"random program, keep_programs={keep}: {outs[0].verdict}, ELF "
                       f"{'still there' if rnd.elf.exists() else 'removed'}")
    for b in bad:
        say("rvrun selftest: " + b)
    say(f"rvrun selftest: {n - len(bad)}/{n} checks as expected ({len(SELFTEST_FAULTS)} "
        f"stand-in behaviours x {len(progs)} programs, including what each run leaves on "
        f"disk; three DUT seeds of one program failing and passing; a passing random program "
        f"kept and removed)")
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
    ap.add_argument("--max-cycles", type=int, default=None,
                    help="the DUT's cycle limit (default: 64 per reference retirement "
                         "+ 200000)")
    ap.add_argument("--keep-programs", action="store_true",
                    help="keep generated random programs that passed")
    ap.add_argument("-v", "--verbose", action="store_true", help="print passing runs too")
    ap.add_argument("--timeout", type=float, default=900.0, help="seconds per DUT run")
    ap.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    ap.add_argument("--elf", action="append", help="also run this ELF (suite 'elf')")
    ap.add_argument("--arch-manifest", default=str(ARCH_MANIFEST))
    ap.add_argument("--out", default=str(ROOT / "build" / "ref" / "run"))
    ap.add_argument("--json", help="write every outcome here")
    ap.add_argument("--show", type=int, default=3, help="print this many failure reports")
    ap.add_argument("--lenient-final-store", action="store_true",
                    help="accept a DUT trace that stops just before the final tohost store")
    ap.add_argument("--lenient-layout", action="store_true",
                    help="compare memory as bytes only, for another RVFI memory convention")
    a = ap.parse_args(argv)
    if a.command == "selftest":
        return selftest(a)
    return run(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
