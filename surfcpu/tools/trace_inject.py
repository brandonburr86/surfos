#!/usr/bin/env python3
"""
SurfCPU trace mutations: one deliberate mismatch of each kind
--------------------
File: trace_inject.py Date: 10/15/26 (SurfCPU)
--------------------
Damages a correct retirement trace in one place, the way a broken core would, so that the
comparator's self-tests (sim/ref/tests/test_trace_compare.py) and the runner's self-test
(the Spike stand-in for the CPU testbench, tools/rvspike_tb.py --inject) can check that every
kind of divergence is caught, at the right record, with the right name.

    python3 tools/trace_inject.py KIND[@N] IN.trace OUT.trace

KIND is one of KINDS below; @N picks the record (default: the first record, after the
first 16, where the mutation applies). Prints the kind and record the comparator should
report.
Only the Python standard library is used.
"""

from __future__ import annotations

import copy
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tracefmt   # noqa: E402
from tracefmt import Rec  # noqa: E402

# kind -> (what it does, the kind the comparator must report)
KINDS = {
    "pc": ("retire a different pc", "pc"),
    "insn": ("flip a bit of the instruction word", "insn"),
    "trap": ("report a completed instruction as a trap", "trap"),
    "notrap": ("report a trap as a completed instruction", "trap"),
    "intr": ("set intr on an ordinary instruction", "intr"),
    "rd_data": ("write a wrong value to rd", "rd"),
    "rd_addr": ("write the value to another register", "rd"),
    "rd_x0": ("report a value with rd = x0", "rd"),
    "trap_write": ("let a trapping instruction write a register", "rd"),
    "mem_addr": ("store to another address", "mem_addr"),
    "mem_mask": ("store fewer bytes", "mem_mask"),
    "mem_wdata": ("store a wrong value", "mem_data"),
    "mem_rdata": ("load a wrong value", "mem_data"),
    "load_addr": ("load from another address", "mem_addr"),
    "load_unreported": ("do not report a load's read", "mem_mask"),
    "trap_store": ("let a trapping instruction store", "mem_mask"),
    "pc_next": ("report a wrong next pc", "pc_next"),
    "missing": ("drop one retirement", "missing"),
    "extra": ("retire one instruction twice", "extra"),
    "order": ("skip an rvfi_order number", "order"),
    "truncate": ("stop the trace early", "missing"),
}


def _applies(kind: str, r: Rec) -> bool:
    if kind in ("insn", "intr", "trap", "pc_next"):
        return not r.trap and not r.intr
    if kind == "notrap":
        return bool(r.trap)
    if kind in ("rd_data", "rd_addr"):
        return not r.trap and r.rd != 0
    if kind == "rd_x0":
        return not r.trap and r.rd == 0
    if kind == "extra":
        return not r.trap
    if kind in ("trap_write", "trap_store"):
        return bool(r.trap)
    if kind in ("mem_addr", "mem_mask", "mem_wdata"):
        return not r.trap and r.wmask == 0xF
    if kind in ("mem_rdata", "load_addr", "load_unreported"):
        return not r.trap and r.rmask != 0
    return True


def pick(recs: list[Rec], kind: str, start: int = 16) -> int:
    """The first record at or after `start` where `kind` applies (before the last two)."""
    limit = len(recs) - 2
    for i in range(min(start, max(limit - 1, 0)), limit):
        if _applies(kind, recs[i]):
            return i
    for i in range(0, limit):
        if _applies(kind, recs[i]):
            return i
    raise ValueError(f"no record in this trace where {kind!r} applies")


def inject(recs: list[Rec], kind: str, at: int | None = None) -> tuple[list[Rec], int, str]:
    """Return (damaged copy, record index, kind the comparator must report)."""
    if kind not in KINDS:
        raise ValueError(f"unknown mutation {kind!r}; known: {', '.join(KINDS)}")
    out = copy.deepcopy(recs)
    i = pick(out, kind) if at is None else at
    r = out[i]
    if kind == "pc":
        r.pc ^= 0x40
    elif kind == "insn":
        r.insn ^= 1 << 20
    elif kind == "trap":
        r.trap, r.rd, r.rd_wdata, r.wmask, r.mem_wdata = 1, 0, 0, 0, 0
        if i + 1 < len(out):
            out[i + 1].intr = 1
    elif kind == "notrap":
        r.trap = 0
    elif kind == "intr":
        r.intr = 1
    elif kind == "rd_data":
        r.rd_wdata ^= 0x00010000
    elif kind == "rd_addr":
        r.rd = r.rd % 31 + 1
    elif kind == "rd_x0":
        r.rd_wdata = 0x1234
    elif kind == "trap_write":
        r.rd, r.rd_wdata = 5, 0xDEAD
    elif kind == "mem_addr":
        r.mem_addr = (r.mem_addr + 0x100) & 0xFFFFFFFF
    elif kind == "mem_mask":
        r.wmask = 0x3
    elif kind == "mem_wdata":
        r.mem_wdata ^= 0x80
    elif kind == "mem_rdata":
        r.mem_rdata ^= 0x1
    elif kind == "load_addr":
        r.mem_addr = (r.mem_addr + 0x100) & 0xFFFFFFFF
    elif kind == "load_unreported":
        r.rmask, r.mem_rdata = 0, 0
    elif kind == "trap_store":
        r.mem_addr, r.wmask, r.mem_wdata = 0x80001000, 0xF, 0x12345678
    elif kind == "pc_next":
        r.pc_next = (r.pc_next + 8) & 0xFFFFFFFF
    elif kind == "missing":
        del out[i]
        _renumber(out)
    elif kind == "extra":
        out.insert(i, copy.deepcopy(r))     # the second copy, at i + 1, is the extra one
        _renumber(out)
        return out, i + 1, KINDS[kind][1]
    elif kind == "order":
        for x in out[i:]:
            x.order += 1
    elif kind == "truncate":
        del out[i:]
    return out, i, KINDS[kind][1]


def _renumber(recs: list[Rec]) -> None:
    base = recs[0].order if recs else 0
    for k, x in enumerate(recs):
        x.order = base + k


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip())
        return 2
    spec, src, dst = argv
    kind, _, n = spec.partition("@")
    recs = tracefmt.read_trace(src)
    out, i, expect = inject(recs, kind, int(n) if n else None)
    tracefmt.save_trace(out, dst, [f"injected {kind} at record {i}"])
    print(f"{kind}: record {i}; the comparator must report {expect} at {i}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
