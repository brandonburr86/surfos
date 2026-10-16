#!/usr/bin/env python3
"""
SurfCPU reference traces from Spike
--------------------
File: trace_spike.py Date: 10/15/26 (SurfCPU)
--------------------
Runs an ELF on Spike (the container's pinned riscv-isa-sim), configured as close to SurfCPU's
contract (docs/ISA.md) as Spike allows, and turns Spike's log into the retirement trace of
docs/MODULES.md section 5, the one the CPU testbench writes from RVFI.

    python3 tools/trace_spike.py prog.elf -o prog.ref.trace
        [--signature FILE [--sig-begin SYM --sig-end SYM]] [--max-records N]
        [--log FILE]            keep Spike's raw log
    python3 tools/trace_spike.py --from-log spike.log prog.elf -o prog.ref.trace

Exit status: 0 the program stored 1 to tohost (pass), 1 it stored another value (fail),
2 it never did (record limit), 3 Spike or the conversion failed.

How the log becomes a trace (docs/VERIFICATION.md section 3 has the reasons):
  * Spike runs with -l --log-commits. A commit line is a retired instruction; an
    "exception" line (with its tval line) is a trap, which RVFI also reports, as a record
    with trap = 1, no register write and no store. The record after a trap or an interrupt
    has intr = 1 (RVFI's first instruction of a handler).
  * Spike's boot ROM (0x1000, five instructions) runs before the ELF's entry; those records
    are dropped, so the trace starts at 0x80000000 like the CPU testbench's.
  * The trace ends with the first store that makes tohost non-zero (Spike notices tohost
    only every few thousand instructions; what it runs after the store is dropped).
  * insn is the 32-bit word in memory at pc, as a core without the C extension fetches it;
    a trapping 16-bit encoding (bits 1:0 not 11) has its upper half 0 and a fetch fault 0
    (MODULES.md section 5, the CPU testbench's convention).
  * A shadow memory (the ELF image plus every logged store) gives each load's mem_rdata,
    also for a load into x0, whose value Spike does not print; every other load is checked
    against the value Spike wrote to rd.
  * pc_next is the pc of the next instruction: for a trap the handler's address (mtvec), for
    the record before an interrupt the interrupt's epc.
  * Memory fields follow riscv-formal's RISCV_FORMAL_ALIGNED_MEM convention, as MODULES.md
    section 5 says: mem_addr word aligned, masks select byte lanes, unused lanes are 0.
Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rvelf      # noqa: E402
import rvisa      # noqa: E402
import tracefmt   # noqa: E402
from tracefmt import Rec  # noqa: E402

# The reference configuration. Each flag is there to match docs/ISA.md:
#   --isa         RV32IM + Zicsr + Zifencei; Zicntr and Zihpm because ISA.md section 2 has
#                 cycle/time/instret and the hpmcounters (read as zero); this Spike makes
#                 those CSRs illegal without the two extensions
#   --priv=m      M-mode only: no S or U CSRs, mstatus.MPP fixed at M, misa without S/U
#   --pmpregions=0  zero PMP entries (pmpcfg/pmpaddr read as zero)
#   --triggers=0  no debug triggers (tselect and friends do not exist on SurfCPU)
#   -m            4 MiB of RAM at 0x80000000, like build/cpu/surfcpu_tb
# Misaligned loads and stores trap because --misaligned is not given. Spike's boot ROM still
# runs first (--disable-dtb would skip it but also removes the CLINT that programs and the
# testbench have): its five instructions leave t0, a0 and a1 set and count in minstret, so
# programs initialize every register and write minstret before relying on it.
#
# The binary: $SURFCPU_SPIKE if set; else the contract-patched build of the same commit
# (sim/ref/spike/build-spike.sh, make ref-spike), which makes the implementation-defined
# choices docs/ISA.md fixes (sim/ref/spike/surfcpu-contract.patch); else the container's
# stock Spike, which differs from the contract in the ways docs/VERIFICATION.md lists.
ROOT = Path(__file__).resolve().parent.parent
PATCHED_SPIKE = ROOT / "build" / "third_party" / "spike-surfcpu" / "bin" / "spike"


def spike_binary() -> tuple[str, str]:
    """(path, kind) of the reference binary; kind is 'env', 'contract' or 'stock'."""
    if os.environ.get("SURFCPU_SPIKE"):
        return os.environ["SURFCPU_SPIKE"], "env"
    if PATCHED_SPIKE.exists():
        return str(PATCHED_SPIKE), "contract"
    return "spike", "stock"


SPIKE, SPIKE_KIND = spike_binary()
SPIKE_ISA = "rv32im_zicsr_zifencei_zicntr_zihpm"
SPIKE_FLAGS = ["--priv=m", "--pmpregions=0", "--triggers=0", "-m0x80000000:0x400000"]
RAM_BASE = 0x80000000
RAM_SIZE = 0x400000
# Changes whenever the trace a given Spike run converts to changes (cached references are
# remade when it differs): 2 = aligned memory fields and 16-bit insn of MODULES.md 8.
CONVERTER_VERSION = 2
DEFAULT_MAX_RECORDS = 4_000_000
DRAIN_LINES = 400_000

TRAP_CAUSES = {
    "trap_instruction_address_misaligned": 0, "trap_instruction_access_fault": 1,
    "trap_illegal_instruction": 2, "trap_breakpoint": 3, "trap_load_address_misaligned": 4,
    "trap_load_access_fault": 5, "trap_store_address_misaligned": 6,
    "trap_store_access_fault": 7, "trap_user_ecall": 8, "trap_supervisor_ecall": 9,
    "trap_machine_ecall": 11, "trap_instruction_page_fault": 12,
    "trap_load_page_fault": 13, "trap_store_page_fault": 15,
}

_RE_DISASM = re.compile(r"^core\s+\d+: 0x([0-9a-f]+) \(0x([0-9a-f]+)\)")
_RE_COMMIT = re.compile(r"^core\s+\d+: (\d) 0x([0-9a-f]+) \(0x([0-9a-f]+)\)(.*)$")
_RE_EXC = re.compile(r"^core\s+\d+: exception (.+), epc 0x([0-9a-f]+)$")
_RE_TVAL = re.compile(r"^core\s+\d+:\s+tval 0x([0-9a-f]+)$")
_RE_SKIP = re.compile(r"^core\s+\d+: (>>>>|Executed \d+ times)")


class ConvertError(Exception):
    pass


@dataclasses.dataclass
class SpikeResult:
    records: list[Rec]
    status: str                    # pass | fail | timeout | error
    tohost: int | None             # the value stored to tohost
    signature: list[int] | None    # words between the signature symbols
    spike_signature: list[int] | None  # Spike's own +signature dump, when it made one
    warnings: list[str]
    command: list[str]
    spike_exit: int | None = None

    @property
    def exit_code(self) -> int:
        return {"pass": 0, "fail": 1, "timeout": 2}.get(self.status, 3)


def spike_command(elf: Path, log: Path, sigfile: Path | None,
                  binary: str | None = None) -> list[str]:
    cmd = [binary or SPIKE, f"--isa={SPIKE_ISA}", *SPIKE_FLAGS, "-l", "--log-commits",
           f"--log={log}"]
    if sigfile is not None:
        cmd += [f"+signature={sigfile}", "+signature-granularity=4"]
    cmd.append(str(elf))
    return cmd


class _Memory:
    """The shadow memory: the RAM window only (devices such as the CLINT are not memory),
    starting from the ELF image (zero elsewhere, as Spike's RAM is), plus every store."""

    def __init__(self, image: dict[int, int]):
        self.bytes = {a: b for a, b in image.items() if _in_ram(a)}

    def load(self, addr: int, size: int) -> int | None:
        val = 0
        for i in range(size):
            a = (addr + i) & 0xFFFFFFFF
            if not _in_ram(a):
                return None
            val |= self.bytes.get(a, 0) << (8 * i)
        return val

    def store(self, addr: int, size: int, data: int) -> None:
        for i in range(size):
            a = (addr + i) & 0xFFFFFFFF
            if _in_ram(a):
                self.bytes[a] = (data >> (8 * i)) & 0xFF


def _in_ram(a: int) -> bool:
    return RAM_BASE <= a < RAM_BASE + RAM_SIZE


def _parse_commit_tail(tail: str, where: str):
    """'x5  0x80000000 c768_mstatus 0x... mem 0x... [0x..]' -> (xregs, csrs, loads, stores)."""
    toks = tail.split()
    xw: list[tuple[int, int]] = []
    csrw: list[tuple[str, int]] = []
    loads: list[int] = []
    stores: list[tuple[int, int, int]] = []
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == "mem":
            if i + 1 >= len(toks):
                raise ConvertError(f"{where}: 'mem' without an address")
            addr = int(toks[i + 1], 16)
            if i + 2 < len(toks) and toks[i + 2].startswith("0x"):
                d = toks[i + 2]
                stores.append((addr, (len(d) - 2) // 2, int(d, 16)))
                i += 3
            else:
                loads.append(addr)
                i += 2
        elif re.fullmatch(r"x\d+", t):
            xw.append((int(t[1:]), int(toks[i + 1], 16)))
            i += 2
        elif re.fullmatch(r"c\d+_\w+", t):
            csrw.append((t, int(toks[i + 1], 16)))
            i += 2
        else:
            raise ConvertError(f"{where}: unexpected token {t!r} in a commit line")
    return xw, csrw, loads, stores


def convert_log(lines, elf: rvelf.Elf, warnings: list[str] | None = None,
                max_records: int | None = None):
    """Spike -l --log-commits output -> (records, tohost value or None, final memory).

    Stops at the first store that makes tohost non-zero, or after max_records records
    (then the tohost value is None: the program did not finish)."""
    if warnings is None:
        warnings = []
    tohost = elf.symbols.get("tohost")
    if tohost is None:
        raise ConvertError(f"{elf.path}: no tohost symbol")
    if elf.entry != RAM_BASE:
        warnings.append(f"ELF entry 0x{elf.entry:08x} is not 0x{RAM_BASE:08x}: the CPU "
                        "testbench starts at 0x80000000 whatever the entry")
    mem = _Memory(elf.image())
    recs: list[Rec] = []
    started = False
    pending: tuple[int, int] | None = None      # last disassembled (pc, bits), not resolved
    next_intr = 0
    tohost_value = None
    it = iter(enumerate(lines, 1))
    lookahead = None

    def nextline():
        nonlocal lookahead
        if lookahead is not None:
            x, lookahead = lookahead, None
            return x
        return next(it, None)

    def close_previous(next_pc: int) -> None:
        # The previous record's pc_next is the pc of whatever ran next, unless an interrupt
        # came in between (then it is already the interrupt's epc).
        if recs and not recs[-1].has_note("intr_after"):
            recs[-1].pc_next = next_pc

    def word_at(pc: int, fallback: int) -> int:
        w = mem.load(pc, 4)
        return fallback & 0xFFFFFFFF if w is None else w

    while True:
        item = nextline()
        if item is None:
            break
        n, raw = item
        line = raw.rstrip("\n")
        where = f"spike log line {n}"
        if not line.strip() or _RE_SKIP.match(line):
            continue
        m = _RE_COMMIT.match(line)
        if m:
            pc, bits = int(m.group(2), 16), int(m.group(3), 16)
            pending = None
            if not started:
                if pc != elf.entry:
                    continue                 # Spike's boot ROM
                started = True
            xw, _csrw, loads, stores = _parse_commit_tail(m.group(4), where)
            d = rvisa.decode(bits)
            word = word_at(pc, bits)
            if word != bits:
                warnings.append(f"{where}: Spike executed {bits:08x} at {pc:08x} but memory "
                                f"holds {word:08x} (code changed without FENCE.I?)")
            r = Rec(order=len(recs), pc=pc, insn=bits, intr=next_intr, pc_next=pc + 4)
            next_intr = 0
            if xw:
                r.rd, r.rd_wdata = xw[-1]
            if len(loads) > 1 or len(stores) > 1 or (loads and stores):
                raise ConvertError(f"{where}: more than one memory access in one instruction")
            if loads:
                addr = loads[0]
                size = d.size if d.kind == "load" else 4
                if d.kind != "load":
                    warnings.append(f"{where}: a load logged for a non-load instruction")
                val = mem.load(addr, size)
                if r.rd:
                    regval = r.rd_wdata
                    raw_from_rd = regval & ((1 << (8 * size)) - 1)
                    if val is None:
                        val = raw_from_rd
                    elif _extend(val, size, d.mnemonic) != regval:
                        raise ConvertError(
                            f"{where}: load at {addr:08x} returned {regval:08x} but the "
                            f"shadow memory holds {val:0{2 * size}x}")
                if val is None:
                    r.add_note("mem", rdata="unknown")
                    val = 0
                r.mem_addr, r.rmask, r.mem_rdata = _aligned(addr, size, val)
            if stores:
                addr, size, data = stores[0]
                if d.kind == "store" and d.size != size:
                    raise ConvertError(f"{where}: store size {size} but {d.mnemonic}")
                r.mem_addr, r.wmask, r.mem_wdata = _aligned(addr, size, data)
                mem.store(addr, size, data)
            close_previous(pc)
            recs.append(r)
            if max_records is not None and len(recs) >= max_records:
                break
            if stores and _hits_tohost(stores[0], tohost):
                v = mem.load(tohost, 4)
                if v:
                    tohost_value = v
                    break
            continue
        m = _RE_DISASM.match(line)
        if m:
            pending = (int(m.group(1), 16), int(m.group(2), 16))
            if pending[0] == elf.entry:
                started = True
            continue
        m = _RE_EXC.match(line)
        if m:
            name, epc = m.group(1), int(m.group(2), 16)
            tval = None
            la = nextline()
            if la is not None:
                tm = _RE_TVAL.match(la[1].rstrip("\n"))
                if tm:
                    tval = int(tm.group(1), 16)
                else:
                    lookahead = la
            if not started and epc == elf.entry:
                started = True
            if not started:
                raise ConvertError(f"{where}: a trap before the program started ({name})")
            if name.startswith("interrupt"):
                # The interrupted instruction never ran; the previous record's pc_next is
                # where it would have gone (the epc), and the handler's first is intr.
                if recs:
                    recs[-1].pc_next = epc
                    recs[-1].add_note("intr_after", epc=f"{epc:08x}")
                next_intr = 1
                pending = None
                continue
            cause = TRAP_CAUSES.get(name)
            if cause is None:
                raise ConvertError(f"{where}: unknown trap {name!r}")
            fetched = pending is not None and pending[0] == epc
            if cause == 1:
                insn = 0                          # nothing was fetched
            else:
                insn = word_at(epc, pending[1] if fetched else 0)
                if insn & 3 != 3:
                    insn &= 0xFFFF                # a 16-bit encoding: its 16 bits only
            r = Rec(order=len(recs), pc=epc, insn=insn, trap=1, intr=next_intr,
                    pc_next=epc)
            r.add_note("trap", cause=f"{cause:x}", tval=f"{(tval or 0):08x}",
                       name=name.replace("trap_", ""))
            next_intr = 1
            close_previous(epc)
            recs.append(r)
            pending = None
            if max_records is not None and len(recs) >= max_records:
                break
            continue
        if line.startswith("core"):
            raise ConvertError(f"{where}: unrecognised Spike log line: {line!r}")
        warnings.append(f"{where}: ignored: {line[:80]!r}")
    for r in recs:
        r.drop_note("intr_after")
    return recs, tohost_value, mem


def _aligned(addr: int, size: int, data: int) -> tuple[int, int, int]:
    """RVFI memory fields in the RISCV_FORMAL_ALIGNED_MEM convention: (word address, lane
    mask, data in its lanes with the others 0)."""
    sh = addr & 3
    return (addr & ~3 & 0xFFFFFFFF, ((1 << size) - 1) << sh,
            ((data & ((1 << (8 * size)) - 1)) << (8 * sh)) & 0xFFFFFFFF)


def _extend(val: int, size: int, mnemonic: str) -> int:
    if mnemonic in ("lb", "lh"):
        return rvisa.sext(val, 8 * size) & 0xFFFFFFFF
    return val


def _hits_tohost(store: tuple[int, int, int], tohost: int) -> bool:
    addr, size, _ = store
    return addr < tohost + 8 and tohost < addr + size


def signature_words(mem: _Memory, begin: int, end: int) -> list[int]:
    words = []
    for a in range(begin, end, 4):
        w = mem.load(a, 4)
        words.append(0 if w is None else w)
    return words


def read_signature_file(path: Path) -> list[int]:
    return [int(x, 16) for x in path.read_text().split()]


def write_signature_file(words: list[int], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"{w:08x}\n" for w in words))


def run_spike(elf_path: str | Path, *, max_records: int | None = DEFAULT_MAX_RECORDS,
              sig_symbols: tuple[str, str] | None = None, keep_log: Path | None = None,
              timeout: float = 600.0, binary: str | None = None) -> SpikeResult:
    """Run one ELF on Spike and convert its log (the reference for one program).

    Spike's log is read from a pipe while it runs. Spike's own --instructions limit is not
    used: it counts 5000-instruction time slices, and every trap ends a slice early, so it
    cuts trap-heavy programs short. Instead the run is stopped after max_records records or
    `timeout` seconds. After the tohost store Spike is left to finish (it notices tohost
    within one slice) so that its +signature dump can be cross-checked."""
    elf_path = Path(elf_path)
    elf = rvelf.read_elf(elf_path)
    warnings: list[str] = []
    with tempfile.TemporaryDirectory(prefix="surfcpu-spike-") as tmp:
        sigfile = None
        if "begin_signature" in elf.symbols and "end_signature" in elf.symbols:
            sigfile = Path(tmp) / "spike.sig"
        errfile = Path(tmp) / "spike.err"
        cmd = spike_command(elf_path, Path("/dev/stdout"), sigfile, binary)
        try:
            with errfile.open("w") as err:
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=err, text=True,
                                        errors="replace", bufsize=1 << 16)
        except FileNotFoundError:
            raise ConvertError(f"{binary or SPIKE} not found: run this inside the toolchain "
                               "container (surfcpu/devenv/run.sh)") from None
        fired = threading.Event()

        def on_timeout():
            fired.set()
            proc.kill()

        timer = threading.Timer(timeout, on_timeout)
        timer.start()
        logfh = keep_log.open("w") if keep_log is not None else None

        def lines():
            for line in proc.stdout:
                if logfh is not None:
                    logfh.write(line)
                yield line

        try:
            recs, tohost_value, mem = convert_log(lines(), elf, warnings, max_records)
            if tohost_value is None:
                proc.kill()                       # past max_records (or Spike stopped)
            else:
                # Let Spike notice tohost and exit (it polls between time slices), so that its
                # +signature dump exists; a program that wrote tohost by accident and runs on
                # is stopped after DRAIN_LINES more lines.
                for n, _ in enumerate(lines()):
                    if n >= DRAIN_LINES:
                        warnings.append("Spike did not stop after the tohost store")
                        proc.kill()
                        break
            proc.wait()
        except BaseException:
            proc.kill()
            proc.wait()
            raise
        finally:
            timer.cancel()
            proc.stdout.close()
            if logfh is not None:
                logfh.close()
        stderr = errfile.read_text(errors="replace").strip()
        spike_sig = read_signature_file(sigfile) if sigfile and sigfile.exists() else None
    if fired.is_set():
        warnings.append(f"Spike was stopped after {timeout:.0f} s")
    if not recs:
        return SpikeResult([], "error", None, None, None,
                           warnings + [f"Spike ran nothing of the program (exit "
                                       f"{proc.returncode}): {stderr[:300]}"],
                           cmd, proc.returncode)
    sig = None
    if sig_symbols:
        sig = signature_words(mem, elf.symbol(sig_symbols[0]), elf.symbol(sig_symbols[1]))
    if spike_sig is not None and tohost_value is not None:
        mine = signature_words(mem, elf.symbol("begin_signature"), elf.symbol("end_signature"))
        if mine != spike_sig:
            warnings.append("Spike's +signature dump differs from the shadow memory")
    if tohost_value is None:
        status = "timeout"
        warnings.append(f"no tohost store within {len(recs)} records")
    else:
        status = "pass" if tohost_value == 1 else "fail"
    return SpikeResult(recs, status, tohost_value, sig, spike_sig, warnings, cmd,
                       proc.returncode)


def trace_comments(res: SpikeResult, elf: Path) -> list[str]:
    commit = "unknown"
    cpath = Path("/opt/spike/COMMIT")
    if cpath.exists():
        commit = cpath.read_text().strip()[:12]
    kind = {"contract": "with surfcpu-contract.patch", "stock": "stock (no contract patch)",
            "env": f"from SURFCPU_SPIKE={SPIKE}"}[SPIKE_KIND]
    return [f"reference: spike {commit} {kind} " + " ".join(res.command[1:-1]),
            f"program: {elf}",
            f"result: {res.status}" + (f" tohost={res.tohost:#x}" if res.tohost is not None
                                       else "")]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("elf")
    ap.add_argument("-o", "--output", help="trace file to write (default: stdout)")
    ap.add_argument("--signature", help="write the signature words here")
    ap.add_argument("--sig-begin", default="begin_signature")
    ap.add_argument("--sig-end", default="end_signature")
    ap.add_argument("--max-records", type=int, default=DEFAULT_MAX_RECORDS,
                    help="stop after this many retirements (default %(default)s)")
    ap.add_argument("--log", help="keep Spike's raw log in this file")
    ap.add_argument("--from-log", help="convert this existing Spike log instead of running Spike")
    a = ap.parse_args(argv)
    elf_path = Path(a.elf)
    try:
        if a.from_log:
            elf = rvelf.read_elf(elf_path)
            warnings: list[str] = []
            with open(a.from_log, encoding="utf-8", errors="replace") as fh:
                recs, tohost_value, mem = convert_log(fh, elf, warnings)
            status = ("timeout" if tohost_value is None else
                      "pass" if tohost_value == 1 else "fail")
            sig = None
            if a.signature:
                sig = signature_words(mem, elf.symbol(a.sig_begin), elf.symbol(a.sig_end))
            res = SpikeResult(recs, status, tohost_value, sig, None, warnings,
                              ["spike", "(from log)", a.from_log])
        else:
            res = run_spike(elf_path, max_records=a.max_records,
                            sig_symbols=(a.sig_begin, a.sig_end) if a.signature else None,
                            keep_log=Path(a.log) if a.log else None)
    except (ConvertError, rvelf.ElfError) as e:
        print(f"trace_spike: {e}", file=sys.stderr)
        return 3
    for w in res.warnings:
        print(f"trace_spike: warning: {w}", file=sys.stderr)
    if res.status == "error":
        return 3
    comments = trace_comments(res, elf_path)
    if a.output:
        tracefmt.save_trace(res.records, a.output, comments)
    else:
        tracefmt.write_trace(res.records, sys.stdout, comments)
    if a.signature and res.signature is not None:
        write_signature_file(res.signature, Path(a.signature))
    print(f"trace_spike: {len(res.records)} records, {res.status}", file=sys.stderr)
    return res.exit_code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
