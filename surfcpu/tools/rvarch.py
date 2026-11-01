#!/usr/bin/env python3
"""
SurfCPU riscv-arch-test integration: fetch, select, build, reference signatures
--------------------
File: rvarch.py Date: 10/15/26 (SurfCPU)
--------------------
    python3 tools/rvarch.py fetch           download the pinned suite, check SHA-256, unpack
    python3 tools/rvarch.py list            the tests in scope and out of scope, with reasons
    python3 tools/rvarch.py build [--jobs J] [--only NAME,...]
                                            build the tests in scope and their reference
                                            results on Spike; write manifest.json
    python3 tools/rvarch.py check           the reference signatures' SHA-256 against
                                            sim/ref/arch/reference.sha256 (shows any change in
                                            the suite, Spike, the toolchain or our config)
    python3 tools/rvarch.py record          rewrite sim/ref/arch/reference.sha256

The suite is riscv-arch-test 4.1.0 (ACT4, the current Architectural Certification Tests).
Its own framework needs Sail, Ruby and the UDB gem; this tool does what the framework does
for one fixed configuration (sim/ref/arch/act4/: rvtest_config.h, rvmodel_macros.h, link.ld)
with Spike as the reference model, which ACT4 supports:
  1. the unprivileged tests ship as assembly (tests/rv32i/<ext>/*.S); the machine-mode
     privileged ones are generated with the suite's own generator (generators/testgen, only
     the Python standard library needed) into build/ref/arch/gen;
  2. each test whose REQUIRED_EXTENSIONS and parameters SurfCPU meets is compiled in
     signature mode (-DSIGNATURE) and run on Spike, giving the reference signature;
  3. the signature is compiled back into the test (-DRVTEST_SELFCHECK), giving a
     self-checking ELF that stores 1 to tohost only if every result matches Spike's;
  4. that ELF runs on Spike too and must pass; tools/rvrun.py runs it on the core and
     compares exit status and retirement trace.
A pass is not a certification: the scope (docs/VERIFICATION.md section 6) is what an
M-mode-only RV32IM core without PMP can run, and the references come from Spike.
Everything generated goes under build/third_party/ (sources) and build/ref/arch/.
Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rvelf          # noqa: E402
import rvisa          # noqa: E402
import rvfetch        # noqa: E402
import trace_spike    # noqa: E402

ROOT = Path(__file__).resolve().parent.parent            # surfcpu/
THIRD = ROOT / "build" / "third_party"
OUT = ROOT / "build" / "ref" / "arch"
CONF = ROOT / "sim" / "ref" / "arch" / "act4"
REFERENCE_SHA = ROOT / "sim" / "ref" / "arch" / "reference.sha256"
CC = "riscv64-unknown-elf-gcc"

SUITE = {
    "version": "4.1.0",
    "commit": "6e8a45123f14cebfb3df151a0e7b849b4389b33b",
    "url": "https://codeload.github.com/riscv/riscv-arch-test/tar.gz/refs/tags/4.1.0",
    "sha256": "aa00df82ed33af9af822f6fee9f22ab0bf67c36ed4fc39c6ba2b1646782258bb",
    "dir": "riscv-arch-test-4.1.0",
}

# What SurfCPU implements, in ACT4's extension names (docs/ISA.md section 1). Zihpm: the
# hpmcounter CSRs exist and read as zero, which is what Zihpm allows. Ssstrict: every
# unimplemented instruction and CSR is illegal (ISA.md sections 1 and 2).
OURS = {"I", "M", "Zmmul", "Zicsr", "Zifencei", "Zicntr", "Zihpm", "Zihintpause", "Sm",
        "Ssstrict"}
# Parameters the tests may constrain, with SurfCPU's values.
PARAMS = {"MXLEN": 32, "MISALIGNED_LDST": False, "NUM_PMP_ENTRIES": 0}
# Privileged suites to generate (the others need S, U, H, F, V, PMP entries, ...).
PRIV_SUITES = ("ExceptionsSm", "Sm", "SsstrictSm", "InterruptsSm")
# Tests in scope by their requirements but left out, with the reason.
EXCLUDED = {
    "InterruptsSm": "raises machine external interrupts through RVMODEL_SET_MEXT_INT: the "
                    "CPU testbench has a register for that (IRQ_EXT, docs/CPU.md section 9) "
                    "but Spike does not, so Spike cannot make its reference signature; its "
                    "timer interrupts also depend on mtime's rate",
    "SsstrictSm_CompressedInstr": "the suite's fast illegal-instruction handler resumes a "
                    "16-bit encoding at mepc + 2, but with IALIGN = 32 mepc[1] is always 0 "
                    "(ISA.md section 2, as on Spike), so the test traps on the same word "
                    "forever (and -04 does not link: undefined trap_sigptr); a suite bug "
                    "for cores without C",
}
# Tests whose self-check assumes the core is fast: run with the testbench's default bus
# latency even when the runner is given --ilat/--dlat.
LATENCY_SENSITIVE = {
    "Sm_mcsr_cntr-00": "its mcycle wrap-around check sets mcycle to all ones, runs a "
                       "300-instruction loop and requires mcycle < 1000: it fails when the "
                       "loop takes more than 3.3 cycles per instruction (with --ilat 1:4 "
                       "--dlat 1:6 a pipelined core needs about 1200)",
}

# Reads of these CSRs depend on time or cycles (mcycle, cycle, time, mip), or on the
# instructions run since reset (minstret, instret: Spike's count includes its boot ROM):
# tests that read them are judged by their self-check only, not by retirement traces.
TIMING_CSRS = {0xB00, 0xB80, 0xC00, 0xC80, 0xC01, 0xC81, 0x344, 0xB02, 0xB82, 0xC02, 0xC82}

# A test that retires more than this on Spike has run away (the longest in scope is listed in
# docs/evidence/verif/arch.txt).
MAX_RECORDS = 1_000_000

_SAIL_DEFINES = ["-DSAIL_CLINT_BASE_ADDRESS=0x2000000",
                 "-DSAIL_SIMPLE_INTERRUPT_GENERATOR_BASE_ADDRESS=0x60000000"]


# --- fetch ------------------------------------------------------------------------------

# The parts of the suite used here (the tarball holds 650 MB of RV64, RV32E, coverage and
# framework files that are not).
KEEP = ("tests/env/", "tests/rv32i/", "tests/priv/", "generators/testgen/", "COPYING",
        "README.md", "CHANGELOG.md")


def act_root() -> Path:
    return THIRD / "riscv-arch-test" / "src" / SUITE["dir"]


def fetch() -> Path:
    """riscv-arch-test at the pinned tag, the cpu track's way (tools/rvfetch.py), unpacking
    only the parts used."""
    archive = rvfetch.fetch(SUITE["url"], SUITE["sha256"],
                            THIRD / "riscv-arch-test" / f"{SUITE['dir']}.tar.gz", "rvarch")
    return rvfetch.unpack(archive, THIRD / "riscv-arch-test" / "src", KEEP)


# --- selection --------------------------------------------------------------------------

@dataclasses.dataclass
class Test:
    name: str                    # e.g. I-add-00, ExceptionsSm-00
    group: str                   # rv32i/I, priv/ExceptionsSm
    src: Path
    march: str
    required: list
    params: dict
    reason: str | None = None    # why it is out of scope (None: in scope)


def parse_config(path: Path) -> tuple[list, dict, str]:
    text = path.read_text(errors="replace")
    m = re.search(r"#+ START_TEST_CONFIG #+\n(.*?)#+ END_TEST_CONFIG", text, re.S)
    if not m:
        raise ValueError(f"{path}: no START_TEST_CONFIG block")
    required, params, march = [], {}, ""
    in_params = False
    for raw in m.group(1).splitlines():
        line = raw.lstrip("#").rstrip()
        s = line.strip()
        if s.startswith("REQUIRED_EXTENSIONS:"):
            required = _flow_list(s.split(":", 1)[1].strip())
            in_params = False
        elif s.startswith("params:"):
            in_params = True
        elif s.startswith("MARCH:"):
            march = s.split(":", 1)[1].strip().replace("${XLEN}", "32")
            in_params = False
        elif in_params and ":" in s and line.startswith("   "):
            k, v = (x.strip() for x in s.split(":", 1))
            params[k] = {"true": True, "false": False}.get(v.lower(), _num(v))
    return required, params, march


def _flow_list(text: str) -> list:
    """A YAML flow sequence such as [I, 'Sm', [Zca, C]] (the tests use both quoting styles)."""
    toks = re.findall(r"\[|\]|,|[^\[\],\s]+", text)
    pos = 0

    def item():
        nonlocal pos
        if toks[pos] == "[":
            pos += 1
            out = []
            while toks[pos] != "]":
                if toks[pos] == ",":
                    pos += 1
                    continue
                out.append(item())
            pos += 1
            return out
        tok = toks[pos].strip("'\"")
        pos += 1
        return tok

    result = item()
    if not isinstance(result, list):
        raise ValueError(f"not a list: {text!r}")
    return result


def _num(v: str):
    try:
        return int(v, 0)
    except ValueError:
        return v.strip("'\"")


def judge_scope(required: list, params: dict) -> str | None:
    for r in required:
        if isinstance(r, list):
            if not any(x in OURS for x in r):
                return f"needs one of {', '.join(r)}, which SurfCPU does not implement"
        elif r not in OURS:
            return f"needs {r}, which SurfCPU does not implement (docs/ISA.md section 1)"
    for k, v in params.items():
        if k not in PARAMS:
            return f"constrains parameter {k}={v}, not evaluated for SurfCPU"
        if isinstance(v, str) and re.fullmatch(r"(>=|<=|>|<|==|!=)\s*-?\d+", v.strip()):
            op, n = re.match(r"(>=|<=|>|<|==|!=)\s*(-?\d+)", v.strip()).groups()
            ok = {">=": PARAMS[k] >= int(n), "<=": PARAMS[k] <= int(n), ">": PARAMS[k] > int(n),
                  "<": PARAMS[k] < int(n), "==": PARAMS[k] == int(n),
                  "!=": PARAMS[k] != int(n)}[op]
            if not ok:
                return (f"needs {k} {op} {n}; SurfCPU has {PARAMS[k]}"
                        + (" (no PMP entries)" if k == "NUM_PMP_ENTRIES" else ""))
            continue
        if PARAMS[k] != v:
            if k == "MISALIGNED_LDST":
                return ("needs misaligned loads and stores in hardware; SurfCPU traps on them "
                        "(ExceptionsSm and the directed/random programs test the traps)")
            return f"needs {k}={v}; SurfCPU has {PARAMS[k]}"
    return None


def generate_priv(out: Path) -> None:
    """Generate the machine-mode privileged tests with ACT4's own generator (a subprocess in
    isolated mode, so nothing from the suite is imported into this process)."""
    stamp = out / "priv" / ".surfcpu-generated"
    want = f"{SUITE['sha256']} {' '.join(PRIV_SUITES)}"
    if stamp.exists() and stamp.read_text().strip() == want:
        return
    if (out / "priv").exists():
        shutil.rmtree(out / "priv")
    subprocess.run([sys.executable, "-I", str(Path(__file__).resolve()), "_genpriv",
                    str(act_root()), str(out), *PRIV_SUITES], check=True)
    stamp.write_text(want + "\n")


def _genpriv(argv: list[str]) -> int:
    act, out, suites = Path(argv[0]), Path(argv[1]), argv[2:]
    sys.path.insert(0, str(act / "generators" / "testgen" / "src"))
    from testgen.generate.priv import generate_priv_test     # noqa: E402
    for s in suites:
        generate_priv_test(s, out)
    return 0


def discover() -> list[Test]:
    act = act_root()
    tests: list[Test] = []
    for src in sorted((act / "tests" / "rv32i").glob("*/*.S")):
        req, params, march = parse_config(src)
        t = Test(src.stem, f"rv32i/{src.parent.name}", src, march, req, params)
        t.reason = judge_scope(req, params)
        tests.append(t)
    for src in sorted((act / "tests" / "priv").glob("*/*.S")):
        req, params, march = parse_config(src)
        t = Test(src.stem, f"priv/{src.parent.name}", src, march, req, params)
        t.reason = judge_scope(req, params)
        tests.append(t)
    gen = OUT / "gen"
    generate_priv(gen)
    for src in sorted((gen / "priv").glob("*/*.S")):
        req, params, march = parse_config(src)
        t = Test(src.stem, f"priv/{src.parent.name}", src, march, req, params)
        t.reason = judge_scope(req, params)
        if t.reason is None:
            for key, why in EXCLUDED.items():
                if t.name == key or t.name.startswith(key + "-") or src.parent.name == key:
                    t.reason = why
        tests.append(t)
    return tests


def other_groups() -> dict[str, str]:
    """Test groups of the suite not unpacked or judged one by one, with the reason."""
    return {"tests/rv64i, tests/rv64e": "RV64 tests (SurfCPU is RV32)",
            "tests/rv32e": "RV32E tests (SurfCPU is RV32I: 32 registers)",
            "other privileged generators": "need S, U, H, F, V, Sdtrig, Smstateen, Zawrs, "
                                           "Zkr, Zicbo* or PMP entries"}


# --- build ------------------------------------------------------------------------------

def _compile(t: Test, out: Path, selfcheck: Path | None) -> None:
    cmd = [CC, f"-march={t.march}", "-mabi=ilp32", "-O0", "-g", "-mcmodel=medany",
           "-nostdlib", "-nostartfiles", "-Wl,--no-warn-rwx-segments", f"-I{CONF}",
           f"-I{act_root() / 'tests' / 'env'}", f"-T{CONF / 'link.ld'}",
           "-DTEST_FLEN=0", f'-DTEST_FILE="{t.name}"', "-o", str(out), str(t.src)]
    if selfcheck is None:
        cmd[1:1] = ["-DSIGNATURE", *_SAIL_DEFINES]
    else:
        cmd[1:1] = ["-DRVTEST_SELFCHECK", f'-DSIGNATURE_FILE="{selfcheck}"', "-DXLEN=32"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f"compiling {t.src} failed:\n{p.stderr[-3000:]}")


def _results_file(sig: list[int], path: Path) -> None:
    """ACT4's sig_modify.py: the signature as .word lines with its labels."""
    lines = []
    for n, w in enumerate(sig):
        text = f"{w:08x}"
        if n == len(sig) - 1:
            lines.append("sig_end_canary:")
        lines.append(f".word 0x{text}")
        if "4b8e2d17" in text:
            lines.append("final_sig_offset:")
        if "7a110ff5" in text:
            lines.append("final_trap_sig_offset:")
        if "d3a91f6c" in text:
            lines.append("trap_sigptr:")
    path.write_text("\n".join(lines) + "\n")


def _reads_timing_csr(records) -> str | None:
    """The first timing CSR the run read into a register (Spike's trace of the ELF), if any:
    code that is never executed (an interrupt path of the trap handler) does not count."""
    for r in records:
        w = r.insn
        if not r.trap and w & 0x7F == 0x73 and (w >> 12) & 7 in (1, 2, 3, 5, 6, 7) \
                and (w >> 20) in TIMING_CSRS and r.rd:
            return rvisa.csr_name(w >> 20)
    return None


def _inputs_key(t: Test) -> str:
    """What a test's build depends on: its source, our configuration, the env headers, the
    reference Spike and this tool."""
    h = hashlib.sha256()
    for f in [t.src, *sorted(CONF.glob("*")), *sorted((act_root() / "tests" / "env").glob("*")),
              Path(__file__)]:
        h.update(f.read_bytes())
    h.update(" ".join([trace_spike.SPIKE, trace_spike.SPIKE_ISA, *trace_spike.SPIKE_FLAGS])
             .encode())
    stamp = Path(trace_spike.SPIKE).parent.parent / ".surfcpu-stamp"
    if stamp.exists():
        h.update(stamp.read_bytes())
    return h.hexdigest()


def build_one(t: Test) -> dict:
    d = OUT / t.group
    d.mkdir(parents=True, exist_ok=True)
    sig_elf = d / f"{t.name}.sig.elf"
    sig_file = d / f"{t.name}.sig"
    results = d / f"{t.name}.results"
    elf = d / f"{t.name}.elf"
    done = d / f"{t.name}.done.json"
    key = _inputs_key(t)
    if done.exists():
        prev = json.loads(done.read_text())
        if prev.get("key") == key and elf.exists() and sig_file.exists():
            return prev["entry"]
    _compile(t, sig_elf, None)
    res = trace_spike.run_spike(sig_elf, max_records=MAX_RECORDS, timeout=300)
    # Signature builds print through ACT4's sail_macros.h (HTIF characters to tohost), so
    # the record of the run is Spike's own +signature dump, not the trace's tohost value.
    if res.spike_signature is None or res.spike_exit != 0:
        raise RuntimeError(f"{t.name}: Spike's signature run failed (exit {res.spike_exit}; "
                           f"{'; '.join(res.warnings)})")
    sig = res.spike_signature
    trace_spike.write_signature_file(sig, sig_file)
    _results_file(sig, results)
    _compile(t, elf, results)
    chk = trace_spike.run_spike(elf, max_records=MAX_RECORDS, timeout=300)
    if chk.status != "pass":
        raise RuntimeError(f"{t.name}: the self-checking ELF does not pass on Spike "
                           f"({chk.status}, tohost {chk.tohost})")
    timing = _reads_timing_csr(chk.records)
    entry = {"name": t.name, "group": t.group, "elf": str(elf.relative_to(ROOT)),
            "ref_sig": str(sig_file.relative_to(ROOT)),
            "ref_sig_sha256": hashlib.sha256(sig_file.read_bytes()).hexdigest(),
            "sig_words": len(sig), "retirements": len(chk.records),
            "compare_trace": not timing,
            "note": (f"reads {timing}: judged by its self-check, not the trace"
                     if timing else ""),
            "latency_sensitive": LATENCY_SENSITIVE.get(t.name, "")}
    done.write_text(json.dumps({"key": key, "entry": entry}) + "\n")
    # Kept: the self-checking ELF and Spike's signature. The signature-mode ELF and the
    # results file compiled into the self-checking one are rebuilt when needed.
    for f in (sig_elf, results):
        f.unlink()
    return entry


def build(jobs: int, only: list[str] | None) -> int:
    fetch()
    tests = discover()
    todo = [t for t in tests if t.reason is None and (not only or t.name in only)]
    print(f"rvarch: building {len(todo)} tests in scope ({sum(1 for t in tests if t.reason)} "
          f"out of scope) with {trace_spike.SPIKE_KIND} Spike, {jobs} jobs")
    entries, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(build_one, t): t for t in todo}
        for f in concurrent.futures.as_completed(futs):
            t = futs[f]
            try:
                e = f.result()
                entries.append(e)
            except (RuntimeError, trace_spike.ConvertError) as err:
                failed.append((t.name, str(err)))
                print(f"rvarch: FAILED {t.name}: {str(err)[:400]}", flush=True)
    entries.sort(key=lambda e: (e["group"], e["name"]))
    manifest = {"suite": f"riscv-arch-test {SUITE['version']}", "commit": SUITE["commit"],
                "spike": trace_spike.SPIKE_KIND, "spike_isa": trace_spike.SPIKE_ISA,
                "spike_flags": trace_spike.SPIKE_FLAGS, "tests": entries,
                "out_of_scope": [{"name": t.name, "group": t.group, "reason": t.reason}
                                 for t in tests if t.reason]}
    if not only:
        (OUT / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    n_trace = sum(1 for e in entries if e["compare_trace"])
    print(f"rvarch: {len(entries)} built and passing their self-check on Spike "
          f"({n_trace} also compared by trace), {len(failed)} failed")
    return 1 if failed else 0


def check() -> int:
    m = json.loads((OUT / "manifest.json").read_text())
    want = {}
    for line in REFERENCE_SHA.read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            h, name = line.split()
            want[name] = h
    got = {e["name"]: e["ref_sig_sha256"] for e in m["tests"]}
    bad = [n for n in sorted(set(want) | set(got)) if want.get(n) != got.get(n)]
    for n in bad:
        print(f"rvarch: {n}: recorded {want.get(n, 'none')[:16]}, now "
              f"{got.get(n, 'none')[:16]}")
    print(f"rvarch: {len(got) - len([b for b in bad if b in got])}/{len(want)} reference "
          f"signatures as recorded in {REFERENCE_SHA.relative_to(ROOT)}")
    return 1 if bad else 0


def record() -> int:
    m = json.loads((OUT / "manifest.json").read_text())
    lines = [f"# SHA-256 of Spike's reference signature for each riscv-arch-test "
             f"{SUITE['version']} test in scope,",
             f"# {m['spike']} Spike, {m['spike_isa']} {' '.join(m['spike_flags'])}.",
             "# Written by tools/rvarch.py record; checked by make test-ref."]
    lines += [f"{e['ref_sig_sha256']}  {e['name']}" for e in m["tests"]]
    REFERENCE_SHA.write_text("\n".join(lines) + "\n")
    print(f"rvarch: recorded {len(m['tests'])} signatures in {REFERENCE_SHA}")
    return 0


def list_scope() -> int:
    fetch()
    tests = discover()
    ins = [t for t in tests if t.reason is None]
    outs = [t for t in tests if t.reason]
    by_group: dict[str, list[str]] = {}
    for t in ins:
        by_group.setdefault(t.group, []).append(t.name)
    print(f"In scope ({len(ins)} tests):")
    for g, names in sorted(by_group.items()):
        print(f"  {g:<22} {len(names):>3}  {', '.join(names)}")
    reasons: dict[tuple[str, str], int] = {}
    for t in outs:
        reasons[(t.group, t.reason)] = reasons.get((t.group, t.reason), 0) + 1
    print(f"Out of scope ({len(outs)} tests looked at):")
    for (g, r), n in sorted(reasons.items()):
        print(f"  {g:<22} {n:>3}  {r}")
    print("Not looked at:")
    for g, r in other_groups().items():
        print(f"  {g:<22}      {r}")
    return 0


def main(argv: list[str]) -> int:
    if argv and argv[0] == "_genpriv":
        return _genpriv(argv[1:])
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("command", choices=("fetch", "list", "build", "check", "record"))
    ap.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    ap.add_argument("--only", help="comma-separated test names (build)")
    a = ap.parse_args(argv)
    if a.command == "fetch":
        print(f"rvarch: {fetch()}")
        return 0
    if a.command == "list":
        return list_scope()
    if a.command == "build":
        return build(a.jobs, a.only.split(",") if a.only else None)
    if a.command == "check":
        return check()
    return record()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
