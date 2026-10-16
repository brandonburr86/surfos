#!/usr/bin/env python3
"""SurfCPU ISA audit of linked programs
--------------------
File: isa-audit.py Date: 10/15/26 (SurfCPU)
--------------------
Checks that every instruction in a linked RISC-V ELF is one SurfCPU implements: RV32I, M,
Zicsr and Zifencei plus ECALL, EBREAK, MRET and WFI (docs/ISA.md section 1), and that every
CSR it names is in the CSR table of docs/ISA.md section 2, with no write to a read-only CSR.
Used on every firmware build (mk/area-fw.mk) and meant for every kernel build:

    tools/isa-audit.py build/fw/surfboot.elf [more.elf ...] [--quiet] [--no-objdump]

The audit decodes the instruction words itself, from the executable sections, skipping the
data that the assembler marks with `$d` mapping symbols, so it does not depend on a
disassembler knowing the ISA. When riscv64-unknown-elf-objdump is available (the
toolchain container) it also disassembles the file and requires the two to agree on every
instruction address and encoding, and every mnemonic to be in the allowed set. It also
checks the ELF header (32-bit, little-endian, RISC-V, soft-float ABI, no RVC/RVE flags) and
the arch attribute.

Allowed exception: `unimp` (0xc0001073, csrrw zero, cycle, zero), the deliberate illegal
instruction GCC emits for __builtin_trap(); it traps as ISA.md says. Exit status 0 when
every file passes, 1 otherwise. Only the Python standard library is used.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

EM_RISCV = 243
SHT_SYMTAB = 2
SHT_RISCV_ATTRIBUTES = 0x70000003
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
UNIMP = 0xC0001073

# docs/ISA.md section 2: address -> name. Read-only ones are those with bits 11:10 = 3.
CSRS = {
    0xF11: "mvendorid", 0xF12: "marchid", 0xF13: "mimpid", 0xF14: "mhartid", 0xF15: "mconfigptr",
    0x300: "mstatus", 0x301: "misa", 0x304: "mie", 0x305: "mtvec", 0x310: "mstatush",
    0x320: "mcountinhibit", 0x340: "mscratch", 0x341: "mepc", 0x342: "mcause", 0x343: "mtval",
    0x344: "mip", 0xB00: "mcycle", 0xB02: "minstret", 0xB80: "mcycleh", 0xB82: "minstreth",
    0xC00: "cycle", 0xC01: "time", 0xC02: "instret", 0xC80: "cycleh", 0xC81: "timeh", 0xC82: "instreth",
}
for _n in range(3, 32):
    CSRS[0x320 + _n] = f"mhpmevent{_n}"
    CSRS[0xB00 + _n] = f"mhpmcounter{_n}"
    CSRS[0xB80 + _n] = f"mhpmcounter{_n}h"
    CSRS[0xC00 + _n] = f"hpmcounter{_n}"
    CSRS[0xC80 + _n] = f"hpmcounter{_n}h"
for _n in range(16):
    CSRS[0x3A0 + _n] = f"pmpcfg{_n}"
for _n in range(64):
    CSRS[0x3B0 + _n] = f"pmpaddr{_n}"

SYSTEM_EXACT = {0x00000073: "ecall", 0x00100073: "ebreak", 0x30200073: "mret", 0x10500073: "wfi"}

ALLOWED_MNEMONICS = {
    "lui", "auipc", "jal", "jalr", "beq", "bne", "blt", "bge", "bltu", "bgeu",
    "lb", "lh", "lw", "lbu", "lhu", "sb", "sh", "sw",
    "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
    "fence", "fence.tso", "pause", "fence.i", "ecall", "ebreak", "mret", "wfi",
    "csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci", "unimp",
    "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu",
}
ALLOWED_EXTENSIONS = {"i", "m", "zicsr", "zifencei", "zmmul"}


def decode(word: int) -> tuple[str | None, str | None, int | None]:
    """Returns (mnemonic, problem, csr). problem is None when SurfCPU implements the word."""
    op = word & 0x7F
    f3 = (word >> 12) & 7
    f7 = word >> 25
    rs1 = (word >> 15) & 31
    rd = (word >> 7) & 31
    if op == 0x37:
        return "lui", None, None
    if op == 0x17:
        return "auipc", None, None
    if op == 0x6F:
        return "jal", None, None
    if op == 0x67:
        return ("jalr", None, None) if f3 == 0 else (None, "JALR with funct3 != 0", None)
    if op == 0x63:
        names = {0: "beq", 1: "bne", 4: "blt", 5: "bge", 6: "bltu", 7: "bgeu"}
        return (names[f3], None, None) if f3 in names else (None, f"branch funct3 {f3}", None)
    if op == 0x03:
        names = {0: "lb", 1: "lh", 2: "lw", 4: "lbu", 5: "lhu"}
        return (names[f3], None, None) if f3 in names else (None, f"load funct3 {f3} (not RV32)", None)
    if op == 0x23:
        names = {0: "sb", 1: "sh", 2: "sw"}
        return (names[f3], None, None) if f3 in names else (None, f"store funct3 {f3} (not RV32)", None)
    if op == 0x13:
        if f3 == 1:
            return ("slli", None, None) if f7 == 0 else (None, "SLLI with imm[11:5] != 0 (shamt[5] is illegal on RV32)", None)
        if f3 == 5:
            if f7 == 0:
                return "srli", None, None
            if f7 == 0x20:
                return "srai", None, None
            return None, "SRLI/SRAI with bad imm[11:5] (shamt[5] is illegal on RV32)", None
        return ["addi", None, "slti", "sltiu", "xori", None, "ori", "andi"][f3], None, None
    if op == 0x33:
        if f7 == 0:
            return ["add", "sll", "slt", "sltu", "xor", "srl", "or", "and"][f3], None, None
        if f7 == 0x20 and f3 in (0, 5):
            return ("sub" if f3 == 0 else "sra"), None, None
        if f7 == 1:
            return ["mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"][f3], None, None
        return None, f"OP with funct7 {f7:#x} funct3 {f3} (an extension SurfCPU lacks)", None
    if op == 0x0F:
        if f3 == 0:
            if rd or rs1:
                return None, "FENCE with nonzero rd/rs1 (reserved)", None
            return "fence", None, None
        if f3 == 1:
            if rd or rs1 or (word >> 20):
                return None, "FENCE.I with nonzero reserved fields", None
            return "fence.i", None, None
        return None, f"MISC-MEM funct3 {f3}", None
    if op == 0x73:
        if f3 == 0:
            if word in SYSTEM_EXACT:
                return SYSTEM_EXACT[word], None, None
            return None, "SYSTEM instruction SurfCPU lacks (SRET, URET, SFENCE.VMA, ...)", None
        if f3 == 4:
            return None, "SYSTEM funct3 4 (reserved)", None
        csr = word >> 20
        name = ["", "csrrw", "csrrs", "csrrc", "", "csrrwi", "csrrsi", "csrrci"][f3]
        if word == UNIMP:
            return "unimp", None, csr
        if csr not in CSRS:
            return name, f"CSR {csr:#05x} is not implemented (an access raises illegal instruction)", csr
        writes = f3 in (1, 5) or rs1 != 0
        if writes and (csr >> 10) == 3:
            return name, f"write to read-only CSR {CSRS[csr]} (raises illegal instruction)", csr
        return name, None, csr
    if op in (0x2F,):
        return None, "atomic (A extension)", None
    if op in (0x07, 0x27, 0x43, 0x47, 0x4B, 0x4F, 0x53):
        return None, "floating point (F/D extension)", None
    if op == 0x57:
        return None, "vector (V extension)", None
    return None, f"opcode {op:#04x} is not RV32IM_Zicsr_Zifencei", None


class Elf:
    def __init__(self, path: str):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        if len(d) < 52 or d[:4] != b"\x7fELF":
            raise ValueError("not an ELF file")
        if d[4] != 1:
            raise ValueError("not a 32-bit ELF (ELFCLASS32 needed)")
        if d[5] != 1:
            raise ValueError("not little-endian")
        (self.e_type, self.e_machine, _, self.e_entry, _, self.e_shoff, self.e_flags, _, _, _,
         self.e_shentsize, self.e_shnum, self.e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        if self.e_machine != EM_RISCV:
            raise ValueError(f"machine {self.e_machine}, not RISC-V ({EM_RISCV})")
        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            (name, typ, flags, addr, offset, size, link, info, align, entsize) = struct.unpack_from("<10I", d, off)
            self.sections.append(dict(index=i, name_off=name, type=typ, flags=flags, addr=addr,
                                      offset=offset, size=size, link=link, entsize=entsize))
        if self.e_shstrndx < len(self.sections):
            strtab = self.sections[self.e_shstrndx]
            for s in self.sections:
                s["name"] = self.cstr(strtab["offset"] + s["name_off"])
        self.symbols = []
        for s in self.sections:
            if s["type"] != SHT_SYMTAB:
                continue
            strs = self.sections[s["link"]]
            for k in range(s["size"] // 16):
                st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                    "<IIIBBH", d, s["offset"] + 16 * k)
                self.symbols.append((self.cstr(strs["offset"] + st_name), st_value, st_size, st_info & 15, st_shndx))

    def cstr(self, off: int) -> str:
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("latin-1")

    def section_bytes(self, s) -> bytes:
        return self.data[s["offset"]:s["offset"] + s["size"]]

    def arch_attribute(self) -> str | None:
        """Tag_RISCV_arch from .riscv.attributes: 'A', then subsections (u32 length, vendor
        name), each holding sub-subsections (uleb tag, u32 length) of attributes; odd tags
        carry a NUL-terminated string, even tags a uleb128 (RISC-V psABI)."""
        for s in self.sections:
            if s["type"] != SHT_RISCV_ATTRIBUTES:
                continue
            b = self.section_bytes(s)
            if not b or b[0] != ord("A"):
                return None
            pos = 1
            while pos + 4 <= len(b):
                sublen = struct.unpack_from("<I", b, pos)[0]
                if sublen < 5:
                    break
                sub_end = pos + sublen
                name_end = b.index(b"\0", pos + 4)
                vendor = b[pos + 4:name_end]
                q = name_end + 1
                while vendor == b"riscv" and q < sub_end:
                    tag_start = q
                    tag, q = uleb(b, q)
                    length = struct.unpack_from("<I", b, q)[0]
                    q += 4
                    ss_end = tag_start + length
                    if tag == 1:                        # Tag_File: attributes of the whole file
                        while q < ss_end:
                            atag, q = uleb(b, q)
                            if atag % 2:
                                z = b.index(b"\0", q)
                                val = b[q:z].decode("latin-1")
                                q = z + 1
                                if atag == 5:           # Tag_RISCV_arch
                                    return val
                            else:
                                _, q = uleb(b, q)
                    if length < 5:
                        break
                    q = ss_end
                pos = sub_end
        return None


def uleb(b: bytes, pos: int) -> tuple[int, int]:
    val = shift = 0
    while True:
        c = b[pos]
        pos += 1
        val |= (c & 0x7F) << shift
        shift += 7
        if not c & 0x80:
            return val, pos


def arch_problems(arch: str) -> list[str]:
    m = re.match(r"rv32([a-z])", arch)
    if not m:
        return [f"arch attribute {arch!r} is not rv32"]
    parts = arch.split("_")
    base = parts[0][4:]
    exts = []
    # the base string holds single-letter extensions with versions: i2p1m2p0...
    for letter, _ in re.findall(r"([a-z])(\d+p\d+)?", base):
        exts.append(letter)
    for p in parts[1:]:
        mm = re.match(r"([a-z]+?)(\d+p\d+)?$", p)
        if mm:
            exts.append(mm.group(1))
    bad = [e for e in exts if e not in ALLOWED_EXTENSIONS]
    return [f"arch attribute {arch!r} includes {', '.join(bad)}"] if bad else []


def code_ranges(elf: Elf, sec) -> list[tuple[int, int]]:
    """[start, end) address ranges of instructions in an executable section ($x vs $d)."""
    marks = sorted((v, n) for n, v, _, _, shndx in elf.symbols
                   if shndx == sec["index"] and (n == "$d" or n.startswith("$x") or n.startswith("$d")))
    start, end = sec["addr"], sec["addr"] + sec["size"]
    if not marks:
        return [(start, end)]
    ranges = []
    kind = "x"
    pos = start
    for addr, name in marks:
        if addr > pos and kind == "x":
            ranges.append((pos, min(addr, end)))
        if addr >= pos:
            pos = addr
        kind = "d" if name.startswith("$d") else "x"
    if kind == "x" and pos < end:
        ranges.append((pos, end))
    return [r for r in ranges if r[1] > r[0]]


def symbolize(elf: Elf, addr: int) -> str:
    best = None
    for name, value, size, typ, shndx in elf.symbols:
        if typ in (1, 2) and value <= addr and (best is None or value > best[1]) and not name.startswith("$"):
            best = (name, value)
    return f"{best[0]}+{addr - best[1]:#x}" if best else "?"


def objdump_listing(path: str, objdump: str) -> dict[int, tuple[str, str]] | None:
    try:
        out = subprocess.run([objdump, "-d", "-M", "no-aliases,numeric", path], capture_output=True,
                             text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if out.returncode != 0:
        return None
    listing = {}
    for line in out.stdout.splitlines():
        m = re.match(r"^\s*([0-9a-f]+):\s+([0-9a-f]{4}|[0-9a-f]{8})\s+(\S+)", line)
        if not m:
            continue
        mnem = m.group(3)
        if mnem.startswith("."):
            continue                                    # data ($d) printed as .word/.short/.byte
        listing[int(m.group(1), 16)] = (m.group(2), mnem)
    return listing


def audit(path: str, use_objdump: bool, objdump: str, quiet: bool) -> int:
    try:
        elf = Elf(path)
    except (OSError, ValueError) as e:
        print(f"isa-audit: {path}: {e}")
        return 1
    problems = []
    if elf.e_flags & 0x1:
        problems.append("e_flags has EF_RISCV_RVC: built for compressed instructions")
    if elf.e_flags & 0x6:
        problems.append(f"e_flags float ABI {(elf.e_flags >> 1) & 3}: SurfCPU uses the soft-float ilp32 ABI")
    if elf.e_flags & 0x8:
        problems.append("e_flags has EF_RISCV_RVE")
    arch = elf.arch_attribute()
    if arch:
        problems += arch_problems(arch)
    found = {}
    csrs = set()
    count = 0
    texts = [s for s in elf.sections if (s["flags"] & (SHF_ALLOC | SHF_EXECINSTR)) == (SHF_ALLOC | SHF_EXECINSTR)
             and s["type"] != 8 and s["size"]]
    for s in texts:
        data = elf.section_bytes(s)
        for lo, hi in code_ranges(elf, s):
            pos = lo
            while pos + 2 <= hi:
                off = pos - s["addr"]
                half = struct.unpack_from("<H", data, off)[0]
                if half & 3 != 3:
                    problems.append(f"{pos:#010x} ({symbolize(elf, pos)}): {half:04x} is a 16-bit (compressed) instruction")
                    pos += 2
                    continue
                if pos + 4 > hi:
                    problems.append(f"{pos:#010x}: a 32-bit instruction runs past the end of the code")
                    break
                word = struct.unpack_from("<I", data, off)[0]
                if (word >> 2) & 7 == 7:
                    problems.append(f"{pos:#010x} ({symbolize(elf, pos)}): {word:08x} is a 48-bit or longer encoding")
                    pos += 4
                    continue
                mnem, problem, csr = decode(word)
                count += 1
                found[pos] = (f"{word:08x}", mnem)
                if csr is not None and csr in CSRS:
                    csrs.add(CSRS[csr])
                if problem:
                    problems.append(f"{pos:#010x} ({symbolize(elf, pos)}): {word:08x}: {problem}")
                pos += 4
    agree = None
    if use_objdump and objdump:
        listing = objdump_listing(path, objdump)
        if listing is None:
            problems.append(f"{objdump} could not disassemble the file")
        else:
            for addr, (raw, mnem) in sorted(listing.items()):
                if mnem not in ALLOWED_MNEMONICS:
                    problems.append(f"{addr:#010x} ({symbolize(elf, addr)}): objdump says {mnem} ({raw}), not in "
                                    "rv32im_zicsr_zifencei")
                ours = found.get(addr)
                if ours is None:
                    problems.append(f"{addr:#010x}: objdump shows an instruction ({mnem} {raw}) where the audit "
                                    "found data")
                elif ours[0] != raw:
                    problems.append(f"{addr:#010x}: objdump reads {raw}, the audit {ours[0]}")
            missing = set(found) - set(listing)
            for addr in sorted(missing)[:10]:
                problems.append(f"{addr:#010x}: the audit decoded {found[addr][0]} where objdump shows no instruction")
            agree = len(listing)
    if problems:
        print(f"isa-audit: {path}: FAILED, {len(problems)} problem(s)")
        for p in problems[:60]:
            print(f"  {p}")
        if len(problems) > 60:
            print(f"  ... and {len(problems) - 60} more")
        return 1
    if not quiet:
        extra = f"; objdump agrees on all {agree}" if agree is not None else "; objdump cross-check skipped"
        print(f"isa-audit: {path}: {count} instructions, all rv32im_zicsr_zifencei{extra}")
        print(f"isa-audit:   arch {arch or '(no attribute)'}; e_flags {elf.e_flags:#x}; "
              f"CSRs: {' '.join(sorted(csrs)) or 'none'}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="check that linked ELFs use only rv32im_zicsr_zifencei (docs/ISA.md)")
    ap.add_argument("elf", nargs="+")
    ap.add_argument("--no-objdump", action="store_true", help="skip the disassembler cross-check")
    ap.add_argument("--objdump", default=os.environ.get("RV_OBJDUMP", "riscv64-unknown-elf-objdump"))
    ap.add_argument("--quiet", action="store_true", help="print only failures")
    a = ap.parse_args(argv)
    objdump = shutil.which(a.objdump) if not a.no_objdump else None
    if not a.no_objdump and not objdump and not a.quiet:
        print(f"isa-audit: {a.objdump} not found; decoding without the disassembler cross-check")
    rc = 0
    for path in a.elf:
        rc |= audit(path, not a.no_objdump, objdump, a.quiet)
    return rc


if __name__ == "__main__":
    sys.exit(main())
