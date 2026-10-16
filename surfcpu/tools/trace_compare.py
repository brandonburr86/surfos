#!/usr/bin/env python3
"""
SurfCPU trace comparator: reference trace against the core's, first divergence out
--------------------
File: trace_compare.py Date: 10/15/26 (SurfCPU)
--------------------
Compares two retirement traces (docs/MODULES.md section 5) record by record: normally the
reference made by tools/trace_spike.py and the one build/cpu/surfcpu_tb wrote from RVFI.
Prints MATCH, or the first divergence with its kind, the fields that differ and context
lines from both traces.

    python3 tools/trace_compare.py REF DUT [--elf PROG.elf | --tohost ADDR]
                                   [--context N] [--json FILE] [--strict-final-store]

Exit status: 0 the traces agree, 1 they diverge, 2 a trace cannot be read.

Mismatch kinds: order (rvfi_order skips or repeats), pc, insn, trap, intr, rd (register
write: number, value, a write by a trapping instruction, or a value with rd = x0),
mem_addr, mem_mask (which bytes; a missing or extra access), mem_data (read or written
bytes differ), pc_next, missing (the core retired fewer instructions: its trace ends early
or skips one), extra (it retired more: a longer trace, or an inserted or repeated record).

The rules, and why (docs/VERIFICATION.md section 3):
  * Memory fields are compared as byte maps (tracefmt.mem_bytes), so riscv-formal's default
    layout and its RISCV_FORMAL_ALIGNED_MEM layout both work. A load may report more bytes
    than it needs (riscv-formal allows wider reads); a store must write exactly its bytes.
  * A trap record must have no register write and no store; its reads, mem_addr and
    pc_next are not compared (RVFI leaves them open: the handler's address is checked as
    the next record's pc). Its insn is compared, except that a 16-bit encoding may also be
    reported as its low 16 bits (RVFI's rule for short instructions), and a fetch fault
    must report 0 (riscv-formal's rule).
  * With a tohost address (from --elf), both traces end at the first store that writes
    tohost; records after it are ignored. The core's trace may stop just before that
    store (a testbench that ends the run on the bus write never logs it) unless
    --strict-final-store is given; the testbench's exit status still reports the value.
  * The reference's "#@ mem rdata=unknown" note (an MMIO load into x0) skips that rdata.
Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rvelf      # noqa: E402
import rvisa      # noqa: E402
import tracefmt   # noqa: E402
from tracefmt import Rec  # noqa: E402

KINDS = ("order", "pc", "insn", "trap", "intr", "rd", "mem_addr", "mem_mask", "mem_data",
         "pc_next", "missing", "extra")


@dataclasses.dataclass
class Result:
    ok: bool
    compared: int                       # records compared
    ref_len: int
    dut_len: int
    kind: str | None = None
    index: int | None = None            # position of the divergence (0-based)
    detail: str = ""
    diffs: list[tuple[str, str, str]] = dataclasses.field(default_factory=list)
    notes: list[str] = dataclasses.field(default_factory=list)
    context: list[str] = dataclasses.field(default_factory=list)

    def summary(self) -> str:
        if self.ok:
            extra = f" ({'; '.join(self.notes)})" if self.notes else ""
            return f"MATCH: {self.compared} retirements agree{extra}"
        return (f"MISMATCH {self.kind} at retirement {self.index}: {self.detail}")

    def to_json(self) -> dict:
        return dataclasses.asdict(self)


def _hits(rec: Rec, tohost: int) -> bool:
    return any(tohost <= a < tohost + 8 for a in tracefmt.writes(rec))


def cut_at_tohost(recs: list[Rec], tohost: int | None) -> list[Rec]:
    """Records up to and including the first store that writes non-zero data to tohost."""
    if tohost is None:
        return recs
    for i, r in enumerate(recs):
        if r.wmask and not r.trap and _hits(r, tohost):
            if any(v for a, v in tracefmt.writes(r).items() if tohost <= a < tohost + 8):
                return recs[:i + 1]
    return recs


def _is_fetch_fault(ref: Rec) -> bool:
    return ref.trap == 1 and ref.note("trap", "cause") == "1"


def compare_records(ref: Rec, dut: Rec) -> list[tuple[str, str]]:
    """The kinds in which dut differs from ref, each with a detail line, in priority order."""
    out: list[tuple[str, str]] = []
    if ref.pc != dut.pc:
        out.append(("pc", f"pc {ref.pc:08x} expected, {dut.pc:08x} retired"))
        return out                     # a different instruction: nothing else is comparable
    if ref.trap != dut.trap:
        what = "trapped" if ref.trap else "completed"
        out.append(("trap", f"the instruction at {ref.pc:08x} should have {what} "
                            f"(reference trap={ref.trap}, core trap={dut.trap})"))
    if _is_fetch_fault(ref):
        if dut.insn != 0:
            out.append(("insn", f"a fetch fault reports insn 0 (riscv-formal), not "
                                f"{dut.insn:08x}"))
    elif ref.insn != dut.insn:
        short_ok = (ref.trap and ref.insn & 3 != 3 and dut.insn == ref.insn & 0xFFFF)
        if not short_ok:
            out.append(("insn", f"insn {ref.insn:08x} expected, {dut.insn:08x} retired"))
    if ref.intr != dut.intr:
        out.append(("intr", f"intr {ref.intr} expected, {dut.intr} retired "
                            "(set on the first instruction of a trap handler)"))
    # register write
    if dut.rd == 0 and dut.rd_wdata != 0:
        out.append(("rd", f"rd_wdata {dut.rd_wdata:08x} with rd = x0 (RVFI: must be 0)"))
    elif ref.trap:
        if dut.rd != 0:
            out.append(("rd", f"a trapping instruction wrote x{dut.rd} = {dut.rd_wdata:08x}"))
    elif (ref.rd, ref.rd_wdata) != (dut.rd, dut.rd_wdata):
        exp = f"x{ref.rd} = {ref.rd_wdata:08x}" if ref.rd else "no register write"
        got = f"x{dut.rd} = {dut.rd_wdata:08x}" if dut.rd else "no register write"
        out.append(("rd", f"{exp} expected, {got} retired"))
    # stores
    rw, dw = tracefmt.writes(ref), tracefmt.writes(dut)
    if ref.trap:
        if dw:
            out.append(("mem_mask", f"a trapping instruction stored to {_bytes(dw)}"))
    elif rw != dw:
        out.append(_classify("store", rw, dw))
    # loads (the core may read more bytes than the instruction needs)
    if not ref.trap:
        rr, dr = tracefmt.reads(ref), tracefmt.reads(dut)
        unknown = ref.note("mem", "rdata") == "unknown"
        if not rr and dr:
            out.append(("mem_mask", f"a read of {_bytes(dr)} reported by an instruction "
                                    "that reads no memory"))
        elif rr and not set(rr) <= set(dr):
            out.append(_classify("load", rr, {a: v for a, v in dr.items()}))
        elif rr and not unknown and any(dr[a] != v for a, v in rr.items()):
            got = {a: dr[a] for a in rr}
            out.append(("mem_data", f"load read {_bytes(got)}, expected {_bytes(rr)}"))
    if not ref.trap and ref.pc_next != dut.pc_next:
        out.append(("pc_next", f"pc_next {ref.pc_next:08x} expected, {dut.pc_next:08x} "
                               "retired"))
    return out


def _bytes(m: dict[int, int]) -> str:
    if not m:
        return "nothing"
    lo = min(m)
    val = "".join(f"{m[a]:02x}" if a in m else "--" for a in range(max(m), lo - 1, -1))
    return f"{len(m)} byte(s) at {lo:08x} = {val}"


def _classify(what: str, exp: dict[int, int], got: dict[int, int]) -> tuple[str, str]:
    if not got:
        return ("mem_mask", f"{what} of {_bytes(exp)} expected, none reported")
    if not exp:
        return ("mem_mask", f"no {what} expected, {what} of {_bytes(got)} reported")
    if not set(exp) & set(got):
        return ("mem_addr", f"{what} at {min(exp):08x} expected, at {min(got):08x} reported")
    if set(exp) != set(got):
        return ("mem_mask", f"{what} of {_bytes(exp)} expected, {_bytes(got)} reported")
    return ("mem_data", f"{what} of {_bytes(exp)} expected, {_bytes(got)} reported")


def _same(a: Rec, b: Rec) -> bool:
    return not compare_records(a, b)


def _fmt(r: Rec | None) -> str:
    if r is None:
        return "(none)"
    parts = [f"{r.pc:08x} {r.insn:08x} {rvisa.disasm(r.insn, r.pc):<28}"]
    if r.trap:
        cause = r.note("trap", "cause")
        parts.append("TRAP" + (f" cause={cause}" if cause is not None else ""))
    if r.intr:
        parts.append("intr")
    if r.rd:
        parts.append(f"x{r.rd}={r.rd_wdata:08x}")
    if r.rmask:
        parts.append(f"rd[{r.mem_addr:08x}/{r.rmask:x}]={r.mem_rdata:08x}")
    if r.wmask:
        parts.append(f"wr[{r.mem_addr:08x}/{r.wmask:x}]={r.mem_wdata:08x}")
    parts.append(f"-> {r.pc_next:08x}")
    return " ".join(parts)


def _context(ref: list[Rec], dut: list[Rec], i: int, before: int, after: int) -> list[str]:
    lines = []
    lo = max(0, i - before)
    for j in range(lo, i):
        lines.append(f"   {j:>8}  {_fmt(ref[j])}")
    lines.append(f">> {i:>8}  ref: {_fmt(ref[i] if i < len(ref) else None)}")
    lines.append(f"   {'':>8}  dut: {_fmt(dut[i] if i < len(dut) else None)}")
    for j in range(i + 1, i + 1 + after):
        if j >= len(ref) and j >= len(dut):
            break
        lines.append(f"   {j:>8}  ref: {_fmt(ref[j] if j < len(ref) else None)}")
        lines.append(f"   {'':>8}  dut: {_fmt(dut[j] if j < len(dut) else None)}")
    return lines


def compare(ref: list[Rec], dut: list[Rec], *, tohost: int | None = None,
            strict_final_store: bool = False, context: int = 8) -> Result:
    ref = cut_at_tohost(ref, tohost)
    dut = cut_at_tohost(dut, tohost)
    res = Result(True, 0, len(ref), len(dut))
    base = dut[0].order if dut else 0
    if dut and base != 0:
        res.notes.append(f"the core's rvfi_order starts at {base}")
    n = min(len(ref), len(dut))
    for i in range(n):
        if dut[i].order != base + i:
            prev = dut[i - 1].order if i else None
            how = ("repeats" if prev is not None and dut[i].order <= prev else "skips")
            return _fail(res, ref, dut, i, "order",
                         f"rvfi_order {how}: {dut[i].order} after "
                         f"{prev if prev is not None else '(start)'} (expected {base + i})",
                         [], context)
        diffs = compare_records(ref[i], dut[i])
        if diffs:
            kind, detail = diffs[0]
            # A skipped or an inserted retirement shows as a pc/insn divergence: look ahead.
            if i + 1 < len(ref) and _same(ref[i + 1], dut[i]) and (
                    i + 2 >= len(ref) or i + 1 >= len(dut) or _same(ref[i + 2], dut[i + 1])):
                return _fail(res, ref, dut, i, "missing",
                             f"the core did not retire {ref[i].pc:08x} "
                             f"({rvisa.disasm(ref[i].insn, ref[i].pc)}): its next record "
                             f"is the reference's next", diffs, context)
            if i + 1 < len(dut) and _same(ref[i], dut[i + 1]) and (
                    i + 2 >= len(dut) or i + 1 >= len(ref) or _same(ref[i + 1], dut[i + 2])):
                return _fail(res, ref, dut, i, "extra",
                             f"the core retired an extra {dut[i].pc:08x} "
                             f"({rvisa.disasm(dut[i].insn, dut[i].pc)}) before the expected "
                             f"record", diffs, context)
            return _fail(res, ref, dut, i, kind, detail, diffs, context)
        res.compared += 1
    if len(dut) < len(ref):
        missing = len(ref) - len(dut)
        last = ref[-1]
        if (missing == 1 and tohost is not None and not strict_final_store
                and last.wmask and _hits(last, tohost)):
            res.notes.append("the core's trace stops before the final tohost store")
            return res
        return _fail(res, ref, dut, len(dut), "missing",
                     f"the core's trace ends after {len(dut)} retirements; the reference "
                     f"has {missing} more", [], context)
    if len(dut) > len(ref):
        return _fail(res, ref, dut, len(ref), "extra",
                     f"the core retired {len(dut) - len(ref)} more instruction(s) after the "
                     f"reference's last", [], context)
    return res


def _fail(res: Result, ref, dut, i, kind, detail, diffs, context) -> Result:
    res.ok = False
    res.kind, res.index, res.detail = kind, i, detail
    res.diffs = [(k, d, "") for k, d in diffs]
    res.context = _context(ref, dut, i, context, 2)
    return res


def report(res: Result, ref_name: str, dut_name: str) -> str:
    out = [res.summary()]
    if not res.ok:
        out.append(f"  reference: {ref_name}")
        out.append(f"  core:      {dut_name}")
        for k, d, _ in res.diffs[1:]:
            out.append(f"  also {k}: {d}")
        out.append("  context (index, pc, insn, disassembly, effects):")
        out += ["  " + c for c in res.context]
    return "\n".join(out)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("ref")
    ap.add_argument("dut")
    ap.add_argument("--elf", help="program ELF: its tohost symbol ends both traces")
    ap.add_argument("--tohost", help="tohost address (hex), instead of --elf")
    ap.add_argument("--context", type=int, default=8)
    ap.add_argument("--json", help="also write the result as JSON")
    ap.add_argument("--strict-final-store", action="store_true",
                    help="the core's trace must include the final tohost store")
    a = ap.parse_args(argv)
    try:
        ref = tracefmt.read_trace(a.ref)
        dut = tracefmt.read_trace(a.dut)
        tohost = None
        if a.elf:
            tohost = rvelf.read_elf(a.elf).symbols.get("tohost")
        elif a.tohost:
            tohost = int(a.tohost, 16)
    except (OSError, tracefmt.TraceError, rvelf.ElfError) as e:
        print(f"trace_compare: {e}", file=sys.stderr)
        return 2
    res = compare(ref, dut, tohost=tohost, strict_final_store=a.strict_final_store,
                  context=a.context)
    print(report(res, a.ref, a.dut))
    if a.json:
        Path(a.json).parent.mkdir(parents=True, exist_ok=True)
        Path(a.json).write_text(json.dumps(res.to_json(), indent=1) + "\n")
    return 0 if res.ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
