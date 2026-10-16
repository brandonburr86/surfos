"""
SurfCPU verification tests: shared helpers
--------------------
File: helpers.py Date: 10/15/26 (SurfCPU)
--------------------
Puts surfcpu/tools on the import path, names the fixtures, and builds small ELF files in
memory (one loadable segment and a symbol table) so the converter tests need neither GCC
nor Spike.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]                     # surfcpu/
TOOLS = ROOT / "tools"
DATA = HERE / "data"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

TRAPS_TRACE = DATA / "traps.ref.trace"
TRAPS_LOG = DATA / "traps.spike.txt"         # not .log: the repository ignores *.log
TRAPS_ELF = DATA / "traps.elf"
TRAPS_TOHOST = 0x80001000


def make_elf(path: Path, base: int, data: bytes, symbols: dict[str, int],
             entry: int | None = None, memsz: int | None = None) -> Path:
    """Write a minimal little-endian ELF32 RISC-V executable: one PT_LOAD segment at `base`
    holding `data` (memsz bytes in memory), and a .symtab with `symbols` (all global)."""
    strtab = b"\0"
    names = {}
    for name in symbols:
        names[name] = len(strtab)
        strtab += name.encode() + b"\0"
    shstr = b"\0.symtab\0.strtab\0.shstrtab\0.text\0"
    ehsize, phsize, shsize = 52, 32, 40
    off_text = ehsize + phsize
    off_sym = off_text + len(data)
    off_sym += (-off_sym) % 4
    symtab = b"\0" * 16
    for name, value in symbols.items():
        symtab += struct.pack("<IIIBBH", names[name], value, 0, 0x10, 0, 4)
    off_str = off_sym + len(symtab)
    off_shstr = off_str + len(strtab)
    off_sh = off_shstr + len(shstr)
    off_sh += (-off_sh) % 4
    hdr = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    hdr += struct.pack("<HHIIIIIHHHHHH", 2, 243, 1, entry if entry is not None else base,
                       ehsize, off_sh, 0, ehsize, phsize, 1, shsize, 5, 3)
    ph = struct.pack("<IIIIIIII", 1, off_text, base, base, len(data),
                     memsz if memsz is not None else len(data), 7, 4)
    sh = [b"\0" * shsize,
          struct.pack("<IIIIIIIIII", 1, 2, 0, 0, off_sym, len(symtab), 2, 1, 4, 16),
          struct.pack("<IIIIIIIIII", 9, 3, 0, 0, off_str, len(strtab), 0, 0, 1, 0),
          struct.pack("<IIIIIIIIII", 17, 3, 0, 0, off_shstr, len(shstr), 0, 0, 1, 0),
          struct.pack("<IIIIIIIIII", 27, 1, 6, base, off_text, len(data), 0, 0, 4, 0)]
    blob = bytearray(hdr + ph + data)
    blob += b"\0" * (off_sym - len(blob)) + symtab + strtab + shstr
    blob += b"\0" * (off_sh - len(blob))
    for s in sh:
        blob += s
    path.write_bytes(bytes(blob))
    return path


def words(*ws: int) -> bytes:
    return b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in ws)
