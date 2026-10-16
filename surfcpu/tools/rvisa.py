#!/usr/bin/env python3
"""
SurfCPU instruction decoder and CSR table (the contract of docs/ISA.md, as code)
--------------------
File: rvisa.py Date: 10/15/26 (SurfCPU)
--------------------
Decodes one 32-bit word the way SurfCPU must (docs/ISA.md section 1): RV32I, M, Zicsr,
Zifencei, ECALL, EBREAK, MRET and WFI are legal; every other word is an illegal instruction,
including every 16-bit encoding, 0x00000000 and 0xFFFFFFFF. disasm() prints the objdump style
of `riscv64-unknown-elf-objdump -M no-aliases,numeric` (x registers, CSR names), which
tests/test_rvisa.py checks against objdump itself.

The CSR table is ISA.md section 2: csr_access() says whether a CSR instruction traps on
SurfCPU and what reads return where the contract fixes it. The trace converter, the random
program generator, the CSR probe and the comparator's context lines use this module.

    python3 tools/rvisa.py 00000297 30200073      disassemble words
Only the Python standard library is used.
"""

from __future__ import annotations

import dataclasses
import sys

MASK32 = 0xFFFFFFFF


def sext(value: int, bits: int) -> int:
    """Sign-extend the low `bits` of value to a Python int."""
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


# --------------------------------------------------------------------------------------
# CSRs (docs/ISA.md section 2)
# --------------------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Csr:
    name: str
    addr: int
    reset: int | None        # None: not fixed (counters, mip, mtime)
    wmask: int               # bits a write changes; 0 = writes ignored (or read-only)
    fixed: int = 0           # value of the bits outside wmask, when the contract fixes it
    timing: bool = False     # value depends on time or on cycles (never compared)


def _csr_table() -> dict[int, Csr]:
    t: dict[int, Csr] = {}

    def add(name, addr, reset, wmask, fixed=None, timing=False):
        t[addr] = Csr(name, addr, reset, wmask, reset if fixed is None else fixed, timing)

    add("mvendorid", 0xF11, 0, 0)
    add("marchid", 0xF12, 0, 0)
    add("mimpid", 0xF13, 0x00000100, 0)
    add("mhartid", 0xF14, 0, 0)
    add("mconfigptr", 0xF15, 0, 0)
    add("mstatus", 0x300, 0x00001800, 0x00000088, fixed=0x00001800)
    add("misa", 0x301, 0x40001100, 0)
    add("mie", 0x304, 0, 0x00000888)
    add("mtvec", 0x305, 0, 0xFFFFFFFC)
    add("mstatush", 0x310, 0, 0)
    add("mcountinhibit", 0x320, 0, 0x00000005)
    for n in range(3, 32):
        add(f"mhpmevent{n}", 0x320 + n, 0, 0)
    add("mscratch", 0x340, 0, MASK32)
    add("mepc", 0x341, 0, 0xFFFFFFFC)
    add("mcause", 0x342, 0, 0x8000001F)
    add("mtval", 0x343, 0, MASK32)
    add("mip", 0x344, None, 0, timing=True)
    for n in range(16):
        add(f"pmpcfg{n}", 0x3A0 + n, 0, 0)
    for n in range(64):
        add(f"pmpaddr{n}", 0x3B0 + n, 0, 0)
    add("mcycle", 0xB00, None, MASK32, timing=True)
    add("minstret", 0xB02, None, MASK32)
    add("mcycleh", 0xB80, None, MASK32, timing=True)
    add("minstreth", 0xB82, None, MASK32)
    for n in range(3, 32):
        add(f"mhpmcounter{n}", 0xB00 + n, 0, 0)
        add(f"mhpmcounter{n}h", 0xB80 + n, 0, 0)
    add("cycle", 0xC00, None, 0, timing=True)
    add("time", 0xC01, None, 0, timing=True)
    add("instret", 0xC02, None, 0)
    add("cycleh", 0xC80, None, 0, timing=True)
    add("timeh", 0xC81, None, 0, timing=True)
    add("instreth", 0xC82, None, 0)
    for n in range(3, 32):
        add(f"hpmcounter{n}", 0xC00 + n, 0, 0)
        add(f"hpmcounter{n}h", 0xC80 + n, 0, 0)
    return t


CSRS: dict[int, Csr] = _csr_table()
CSR_BY_NAME: dict[str, Csr] = {c.name: c for c in CSRS.values()}

# Names objdump uses for CSRs SurfCPU does not have (for disassembly only).
_OTHER_CSR_NAMES = {
    0x001: "fflags", 0x002: "frm", 0x003: "fcsr",
    0x100: "sstatus", 0x104: "sie", 0x105: "stvec", 0x106: "scounteren", 0x10A: "senvcfg",
    0x140: "sscratch", 0x141: "sepc", 0x142: "scause", 0x143: "stval", 0x144: "sip",
    0x180: "satp", 0x302: "medeleg", 0x303: "mideleg", 0x306: "mcounteren",
    0x30A: "menvcfg", 0x31A: "menvcfgh", 0x34A: "mtinst", 0x34B: "mtval2",
    0x7A0: "tselect", 0x7A1: "tdata1", 0x7A2: "tdata2", 0x7A3: "tdata3", 0x7A4: "tinfo",
    0x7A5: "tcontrol", 0x7A8: "mcontext", 0x7B0: "dcsr", 0x7B1: "dpc", 0x7B2: "dscratch0",
    0x7B3: "dscratch1", 0x747: "mseccfg", 0x757: "mseccfgh",
}


def csr_name(addr: int) -> str:
    c = CSRS.get(addr)
    if c:
        return c.name
    return _OTHER_CSR_NAMES.get(addr, f"0x{addr:x}")


def csr_read_only(addr: int) -> bool:
    return (addr >> 10) & 3 == 3


def csr_access(addr: int, writes: bool) -> bool:
    """True if a CSR instruction on `addr` completes on SurfCPU, False if it is illegal.

    `writes` is the ISA's notion of a write: CSRRW/CSRRWI always write; CSRRS/CSRRC/CSRRSI/
    CSRRCI write unless their rs1 field (register number or uimm) is 0."""
    if addr not in CSRS:
        return False
    if writes and csr_read_only(addr):
        return False
    return True


def csr_after_write(addr: int, old: int, value: int) -> int:
    """The value SurfCPU's CSR holds after a write of `value` (contract fields only).
    Counters are written whole; the caller handles their increments."""
    c = CSRS[addr]
    return (old & ~c.wmask | value & c.wmask) & MASK32 if c.wmask else old


# --------------------------------------------------------------------------------------
# Instructions
# --------------------------------------------------------------------------------------

LOAD_SIZES = {0: 1, 1: 2, 2: 4, 4: 1, 5: 2}       # funct3 -> bytes (LB LH LW LBU LHU)
STORE_SIZES = {0: 1, 1: 2, 2: 4}                  # SB SH SW

_OP = {(0x00, 0): "add", (0x20, 0): "sub", (0x00, 1): "sll", (0x00, 2): "slt",
       (0x00, 3): "sltu", (0x00, 4): "xor", (0x00, 5): "srl", (0x20, 5): "sra",
       (0x00, 6): "or", (0x00, 7): "and",
       (0x01, 0): "mul", (0x01, 1): "mulh", (0x01, 2): "mulhsu", (0x01, 3): "mulhu",
       (0x01, 4): "div", (0x01, 5): "divu", (0x01, 6): "rem", (0x01, 7): "remu"}
_OPIMM = {0: "addi", 2: "slti", 3: "sltiu", 4: "xori", 6: "ori", 7: "andi"}
_BRANCH = {0: "beq", 1: "bne", 4: "blt", 5: "bge", 6: "bltu", 7: "bgeu"}
_LOAD = {0: "lb", 1: "lh", 2: "lw", 4: "lbu", 5: "lhu"}
_STORE = {0: "sb", 1: "sh", 2: "sw"}
_CSROP = {1: "csrrw", 2: "csrrs", 3: "csrrc", 5: "csrrwi", 6: "csrrsi", 7: "csrrci"}
_SYSTEM_EXACT = {0x00000073: "ecall", 0x00100073: "ebreak", 0x30200073: "mret",
                 0x10500073: "wfi"}


@dataclasses.dataclass
class Insn:
    word: int
    legal: bool
    mnemonic: str            # "illegal" when not legal
    kind: str                # alu, alui, lui, auipc, jal, jalr, branch, load, store, csr,
                             # fence, fencei, ecall, ebreak, mret, wfi, illegal
    rd: int = 0              # destination register written (0 when none)
    rs1: int = 0             # source registers read (0 when unused)
    rs2: int = 0
    imm: int = 0
    csr: int = -1
    size: int = 0            # memory access size in bytes (loads, stores)
    csr_writes: bool = False  # Zicsr "write" (decides read-only traps)

    @property
    def length16(self) -> bool:
        """A 16-bit encoding (bits 1:0 != 11): illegal on SurfCPU."""
        return self.word & 3 != 3


def decode(word: int) -> Insn:
    """Decode a 32-bit word against SurfCPU's ISA (docs/ISA.md section 1)."""
    word &= MASK32
    ill = Insn(word, False, "illegal", "illegal")
    if word & 3 != 3 or word & 0x1C == 0x1C:      # 16-bit, or 48-bit and longer encodings
        return ill
    op = word & 0x7F
    rd = (word >> 7) & 0x1F
    f3 = (word >> 12) & 7
    rs1 = (word >> 15) & 0x1F
    rs2 = (word >> 20) & 0x1F
    f7 = word >> 25
    imm_i = sext(word >> 20, 12)
    if op == 0x37:
        return Insn(word, True, "lui", "lui", rd=rd, imm=word & 0xFFFFF000)
    if op == 0x17:
        return Insn(word, True, "auipc", "auipc", rd=rd, imm=word & 0xFFFFF000)
    if op == 0x6F:
        imm = sext(((word >> 31) << 20) | (((word >> 12) & 0xFF) << 12)
                   | (((word >> 20) & 1) << 11) | (((word >> 21) & 0x3FF) << 1), 21)
        return Insn(word, True, "jal", "jal", rd=rd, imm=imm)
    if op == 0x67:
        if f3:
            return ill
        return Insn(word, True, "jalr", "jalr", rd=rd, rs1=rs1, imm=imm_i)
    if op == 0x63:
        if f3 not in _BRANCH:
            return ill
        imm = sext(((word >> 31) << 12) | (((word >> 7) & 1) << 11)
                   | (((word >> 25) & 0x3F) << 5) | (((word >> 8) & 0xF) << 1), 13)
        return Insn(word, True, _BRANCH[f3], "branch", rs1=rs1, rs2=rs2, imm=imm)
    if op == 0x03:
        if f3 not in _LOAD:
            return ill
        return Insn(word, True, _LOAD[f3], "load", rd=rd, rs1=rs1, imm=imm_i,
                    size=LOAD_SIZES[f3])
    if op == 0x23:
        if f3 not in _STORE:
            return ill
        imm = sext(((word >> 25) << 5) | ((word >> 7) & 0x1F), 12)
        return Insn(word, True, _STORE[f3], "store", rs1=rs1, rs2=rs2, imm=imm,
                    size=STORE_SIZES[f3])
    if op == 0x13:
        if f3 in _OPIMM:
            return Insn(word, True, _OPIMM[f3], "alui", rd=rd, rs1=rs1, imm=imm_i)
        if f3 == 1 and f7 == 0:
            return Insn(word, True, "slli", "alui", rd=rd, rs1=rs1, imm=rs2)
        if f3 == 5 and f7 in (0x00, 0x20):
            return Insn(word, True, "srli" if f7 == 0 else "srai", "alui", rd=rd, rs1=rs1,
                        imm=rs2)
        return ill
    if op == 0x33:
        m = _OP.get((f7, f3))
        if m is None:
            return ill
        return Insn(word, True, m, "alu", rd=rd, rs1=rs1, rs2=rs2)
    if op == 0x0F:
        if f3 == 0:                                   # FENCE: fm, pred, succ, rs1, rd ignored
            m = {0x8330000F: "fence.tso", 0x0100000F: "pause"}.get(word, "fence")
            return Insn(word, True, m, "fence")
        if f3 == 1:                                   # FENCE.I: imm, rs1, rd ignored
            return Insn(word, True, "fence.i", "fencei")
        return ill
    if op == 0x73:
        if f3 == 0:
            m = _SYSTEM_EXACT.get(word)
            if m is None:
                return ill
            return Insn(word, True, m, m)
        if f3 in _CSROP:
            m = _CSROP[f3]
            csr = word >> 20
            writes = f3 in (1, 5) or rs1 != 0
            immform = f3 >= 5
            return Insn(word, True, m, "csr", rd=rd, rs1=0 if immform else rs1,
                        imm=rs1 if immform else 0, csr=csr, csr_writes=writes)
        return ill
    return ill


def is_legal(word: int) -> bool:
    """Legal on SurfCPU as an instruction (a CSR instruction can still trap on its CSR)."""
    return decode(word).legal


def traps_on_surfcpu(word: int) -> bool:
    """True if executing `word` always raises an illegal-instruction exception."""
    d = decode(word)
    if not d.legal:
        return True
    if d.kind == "csr":
        return not csr_access(d.csr, d.csr_writes)
    return False


def _fence_set(bits: int) -> str:
    s = "".join(c for c, b in zip("iorw", (8, 4, 2, 1)) if bits & b)
    return s or "0"


def disasm(word: int, pc: int | None = None) -> str:
    """objdump-style text (`-M no-aliases,numeric`); branch and jump targets are absolute when
    pc is given, else written as offsets."""
    d = decode(word)
    if not d.legal:
        return f".word 0x{word & MASK32:08x}"
    m, k = d.mnemonic, d.kind

    def x(r):
        return f"x{r}"

    def target(off):
        return f"0x{(pc + off) & MASK32:x}" if pc is not None else f".{off:+d}"

    rd, rs1, rs2 = (word >> 7) & 31, (word >> 15) & 31, (word >> 20) & 31
    if k in ("lui", "auipc"):
        return f"{m} {x(rd)},0x{d.imm >> 12:x}"
    if k == "jal":
        return f"{m} {x(rd)},{target(d.imm)}"
    if k == "jalr":
        return f"{m} {x(rd)},{d.imm}({x(rs1)})"
    if k == "branch":
        return f"{m} {x(rs1)},{x(rs2)},{target(d.imm)}"
    if k == "load":
        return f"{m} {x(rd)},{d.imm}({x(rs1)})"
    if k == "store":
        return f"{m} {x(rs2)},{d.imm}({x(rs1)})"
    if k == "alui":
        if m in ("slli", "srli", "srai"):
            return f"{m} {x(rd)},{x(rs1)},0x{d.imm:x}"
        return f"{m} {x(rd)},{x(rs1)},{d.imm}"
    if k == "alu":
        return f"{m} {x(rd)},{x(rs1)},{x(rs2)}"
    if k == "csr":
        if m.endswith("i"):
            return f"{m} {x(rd)},{csr_name(d.csr)},{d.imm}"
        return f"{m} {x(rd)},{csr_name(d.csr)},{x(rs1)}"
    if k == "fence":
        fm, pred, succ = word >> 28, (word >> 24) & 15, (word >> 20) & 15
        if word == 0x8330000F:
            return "fence.tso"
        if word == 0x0100000F:
            return "pause"
        if fm == 0 and rd == 0 and rs1 == 0:
            return f"fence {_fence_set(pred)},{_fence_set(succ)}"
        return f"fence {_fence_set(pred)},{_fence_set(succ)} (fm={fm} rs1={rs1} rd={rd})"
    if k == "fencei":
        return "fence.i"
    return m


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__.strip())
        return 2
    for a in argv:
        w = int(a, 16)
        print(f"{w:08x}  {disasm(w)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
