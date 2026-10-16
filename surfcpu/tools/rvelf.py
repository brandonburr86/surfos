#!/usr/bin/env python3
"""
SurfCPU ELF reader for the verification tools
--------------------
File: rvelf.py Date: 10/15/26 (SurfCPU)
--------------------
Reads a 32-bit little-endian RISC-V ELF: the entry point, the loadable segments (by physical
address, as Spike and the CPU testbench load them) and the symbol table. Enough for the
trace converter's memory image, the signature dump and the runner; nothing else.

    python3 tools/rvelf.py prog.elf            entry, segments and a few symbols
Only the Python standard library is used.
"""

from __future__ import annotations

import dataclasses
import struct
import sys
from pathlib import Path


class ElfError(Exception):
    pass


@dataclasses.dataclass
class Segment:
    paddr: int
    data: bytes        # p_filesz bytes from the file
    memsz: int         # the rest up to memsz is zero


@dataclasses.dataclass
class Elf:
    path: Path
    entry: int
    segments: list[Segment]
    symbols: dict[str, int]

    def symbol(self, name: str) -> int:
        if name not in self.symbols:
            raise ElfError(f"{self.path}: no symbol {name!r}")
        return self.symbols[name]

    def image(self) -> dict[int, int]:
        """Byte-addressed memory image: address -> byte, for every loaded byte (bss as 0)."""
        mem: dict[int, int] = {}
        for s in self.segments:
            for i, b in enumerate(s.data):
                mem[s.paddr + i] = b
            for i in range(len(s.data), s.memsz):
                mem[s.paddr + i] = 0
        return mem


def read_elf(path: str | Path) -> Elf:
    path = Path(path)
    raw = path.read_bytes()
    if raw[:4] != b"\x7fELF":
        raise ElfError(f"{path}: not an ELF file")
    if raw[4] != 1 or raw[5] != 1:
        raise ElfError(f"{path}: not a 32-bit little-endian ELF")
    (e_type, e_machine, _ver, e_entry, e_phoff, e_shoff, _flags, _ehsize, e_phentsize,
     e_phnum, e_shentsize, e_shnum, _shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", raw, 16)
    if e_machine != 243:
        raise ElfError(f"{path}: not a RISC-V ELF (e_machine {e_machine})")
    segs: list[Segment] = []
    for i in range(e_phnum):
        (p_type, p_offset, _vaddr, p_paddr, p_filesz, p_memsz, _pflags,
         _align) = struct.unpack_from("<IIIIIIII", raw, e_phoff + i * e_phentsize)
        if p_type == 1 and p_memsz:
            segs.append(Segment(p_paddr, raw[p_offset:p_offset + p_filesz], p_memsz))
    sections = [struct.unpack_from("<IIIIIIIIII", raw, e_shoff + i * e_shentsize)
                for i in range(e_shnum)]
    symbols: dict[str, int] = {}
    for sh in sections:
        sh_type, sh_offset, sh_size, sh_link, sh_entsize = sh[1], sh[4], sh[5], sh[6], sh[9]
        if sh_type != 2:            # SHT_SYMTAB
            continue
        strtab = sections[sh_link]
        str_off, str_size = strtab[4], strtab[5]
        names = raw[str_off:str_off + str_size]
        for j in range(sh_size // (sh_entsize or 16)):
            st_name, st_value, _size, st_info, _other, st_shndx = struct.unpack_from(
                "<IIIBBH", raw, sh_offset + j * (sh_entsize or 16))
            if not st_name or st_shndx == 0:
                continue
            end = names.index(b"\0", st_name)
            name = names[st_name:end].decode("ascii", "replace")
            # Prefer global symbols when a name repeats (local labels such as "1").
            if name not in symbols or (st_info >> 4) == 1:
                symbols[name] = st_value
    return Elf(path, e_entry, segs, symbols)


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__.strip())
        return 2
    e = read_elf(argv[0])
    print(f"entry 0x{e.entry:08x}")
    for s in e.segments:
        print(f"segment 0x{s.paddr:08x} filesz {len(s.data)} memsz {s.memsz}")
    for name in ("tohost", "fromhost", "begin_signature", "end_signature"):
        if name in e.symbols:
            print(f"{name} 0x{e.symbols[name]:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
