#!/usr/bin/env python3
"""
SurfCPU retirement trace format (docs/MODULES.md section 5)
--------------------
File: tracefmt.py Date: 10/15/26 (SurfCPU)
--------------------
One line per rvfi_valid; '#' starts a comment line. Thirteen space-separated fields, hex
without 0x except order (decimal):

  # surfcpu-trace v1: order pc insn trap intr rd rd_wdata mem_addr rmask wmask mem_rdata mem_wdata pc_next
  0 80000000 00000297 0 0 5 80000000 00000000 0 0 00000000 00000000 80000004

rd is hex too (x10 is "a"). This module reads and writes that format. Reference traces made
by tools/trace_spike.py also carry annotation comments, "#@ <tag> key=value ...", that belong
to the record after them (for a trap: its cause and mtval). Other readers can ignore them:
they are ordinary comment lines.

mem_bytes() turns a record's memory fields into {byte address: value}, which is the same for
both RVFI conventions: riscv-formal's default (mem_addr is the first byte, masks and data
start at bit 0) and RISCV_FORMAL_ALIGNED_MEM (mem_addr word-aligned, masks and data in their
byte lanes). The comparator works on those maps.
Only the Python standard library is used.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Iterable, Iterator, TextIO

HEADER = ("# surfcpu-trace v1: order pc insn trap intr rd rd_wdata mem_addr rmask wmask "
          "mem_rdata mem_wdata pc_next")
FIELDS = ("order", "pc", "insn", "trap", "intr", "rd", "rd_wdata", "mem_addr", "rmask",
          "wmask", "mem_rdata", "mem_wdata", "pc_next")
_LIMITS = {"trap": 1, "intr": 1, "rd": 31, "rmask": 15, "wmask": 15}


class TraceError(Exception):
    pass


@dataclasses.dataclass
class Rec:
    order: int
    pc: int
    insn: int
    trap: int = 0
    intr: int = 0
    rd: int = 0
    rd_wdata: int = 0
    mem_addr: int = 0
    rmask: int = 0
    wmask: int = 0
    mem_rdata: int = 0
    mem_wdata: int = 0
    pc_next: int = 0
    line: int = 0                                   # line number in the file it came from
    notes: dict[str, dict[str, str]] = dataclasses.field(default_factory=dict)

    def fields(self) -> tuple[int, ...]:
        return tuple(getattr(self, f) for f in FIELDS)

    def text(self) -> str:
        return (f"{self.order} {self.pc:08x} {self.insn:08x} {self.trap} {self.intr} "
                f"{self.rd:x} {self.rd_wdata:08x} {self.mem_addr:08x} {self.rmask:x} "
                f"{self.wmask:x} {self.mem_rdata:08x} {self.mem_wdata:08x} {self.pc_next:08x}")

    def note(self, tag: str, key: str, default: str | None = None) -> str | None:
        return self.notes.get(tag, {}).get(key, default)


def mem_bytes(addr: int, mask: int, data: int) -> dict[int, int]:
    """Bytes an RVFI access touches: mask bit k is byte addr+k, data byte k is its value.

    Holds for both RVFI layouts, because the aligned layout has a word-aligned address and
    the byte lanes in place, and the default layout has the first byte's address with the
    data starting at bit 0."""
    return {(addr + k) & 0xFFFFFFFF: (data >> (8 * k)) & 0xFF
            for k in range(4) if mask >> k & 1}


def reads(r: Rec) -> dict[int, int]:
    return mem_bytes(r.mem_addr, r.rmask, r.mem_rdata)


def writes(r: Rec) -> dict[int, int]:
    return mem_bytes(r.mem_addr, r.wmask, r.mem_wdata)


def _parse_note(text: str) -> tuple[str, dict[str, str]]:
    parts = text.split()
    tag = parts[0] if parts else ""
    kv: dict[str, str] = {}
    for p in parts[1:]:
        k, _, v = p.partition("=")
        kv[k] = v
    return tag, kv


def parse_lines(lines: Iterable[str], name: str = "<trace>") -> Iterator[Rec]:
    notes: dict[str, dict[str, str]] = {}
    for n, raw in enumerate(lines, 1):
        s = raw.strip()
        if not s:
            continue
        if s.startswith("#"):
            if s.startswith("#@"):
                tag, kv = _parse_note(s[2:])
                notes[tag] = kv
            continue
        parts = s.split()
        if len(parts) != len(FIELDS):
            raise TraceError(f"{name}:{n}: expected {len(FIELDS)} fields, found {len(parts)}")
        try:
            vals = [int(parts[0], 10)] + [int(p, 16) for p in parts[1:]]
        except ValueError:
            raise TraceError(f"{name}:{n}: a field is not a number: {s!r}") from None
        for f, v in zip(FIELDS, vals):
            lim = _LIMITS.get(f, 0xFFFFFFFF if f != "order" else (1 << 64) - 1)
            if not 0 <= v <= lim:
                raise TraceError(f"{name}:{n}: {f} = {v:#x} is out of range (max {lim:#x})")
        rec = Rec(*vals, line=n, notes=notes)
        notes = {}
        yield rec


def read_trace(path: str | Path) -> list[Rec]:
    path = Path(path)
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        return list(parse_lines(fh, str(path)))


def write_trace(recs: Iterable[Rec], out: TextIO, comments: Iterable[str] = (),
                with_notes: bool = True) -> int:
    out.write(HEADER + "\n")
    for c in comments:
        out.write(f"# {c}\n")
    n = 0
    for r in recs:
        if with_notes:
            for tag, kv in r.notes.items():
                extra = "".join(f" {k}={v}" for k, v in kv.items())
                out.write(f"#@ {tag}{extra}\n")
        out.write(r.text() + "\n")
        n += 1
    return n


def save_trace(recs: Iterable[Rec], path: str | Path, comments: Iterable[str] = (),
               with_notes: bool = True) -> int:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        return write_trace(recs, fh, comments, with_notes)
