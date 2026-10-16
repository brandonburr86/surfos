#!/usr/bin/env python3
"""
SurfCPU constrained-random program generator
--------------------
File: rvgen.py Date: 10/15/26 (SurfCPU)
--------------------
Writes a self-contained RV32IM assembly program from a seed. The same seed, length and
generator version give the same program, byte for byte, on any Python (the generator has its
own SplitMix64 random numbers).

    python3 tools/rvgen.py --seed N [--length L] [-o rand.S] [--stats]
    python3 tools/rvgen.py --illegal-only --seed N -o ill.S     every item an illegal word

The program is for an M-mode-only RV32IM + Zicsr + Zifencei core without PMP entries
(docs/ISA.md), and runs the same on Spike and on SurfCPU:
  * RV32I and M arithmetic with corner values (0, +-1, the extremes, immediates at both
    ends, shift amounts 0/1/31), x0 as source and destination, sources drawn mostly from the
    last registers written (dependences at distance 1-4);
  * loads and stores of every width and sign in a 4 KiB data area (base x3), also through
    pointers made just before;
  * forward branches over blocks (taken and not taken), bounded loops (1-4 iterations),
    JAL/JALR forwards (link values, bit 0 of JALR targets), calls to leaf functions;
  * CSR instructions on the CSRs where Spike and the contract agree (docs/VERIFICATION.md
    section 4): mscratch, mtval, mepc, mcause (legal values only: WLRL), mstatus, mie (only
    cleared), mtvec (rewritten with its own value), mcountinhibit (CY and IR), minstret,
    the id CSRs that agree, mstatush and the hpm CSRs whose writes are ignored;
  * deliberate traps: ECALL, EBREAK, illegal words (fixed contract cases, 16-bit encodings
    with a zero upper half, random words with undefined fields), illegal CSR accesses
    (unknown CSRs, writes to read-only ones), misaligned loads/stores and jump targets,
    load/store/fetch access faults at addresses unmapped on Spike and on the testbench;
    the handler reads mcause, mtval and mepc (compared through the trace) and resumes at
    mepc + 4, or at ra after a fetch fault;
  * MRET to a label, FENCE (any fields), FENCE.I, NOPs.
Never: WFI, interrupts (mie stays 0), mcycle/cycle/time/mip (timing), marchid/mimpid reads,
pmp*, tselect/tdata*, misa writes, mtvec MODE bits, minstreth writes, stores outside the
data area. The end stores every register into the signature, then 1 to tohost.
Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import collections
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rvisa  # noqa: E402

VERSION = "rvgen 1"
MASK32 = 0xFFFFFFFF
MASK64 = (1 << 64) - 1

# Unmapped on Spike (checked by sim/ref/tests/test_rvgen.py) and, per docs/MODULES.md
# section 6, on build/cpu/surfcpu_tb as long as docs/CPU.md puts no register there.
FAULT_ADDRS = (0x00000000, 0x20000000, 0x40000000, 0x7FFFFFFC, 0x80400000, 0xF0000000,
               0xFFFFFFFC)

DATA_SIZE = 4096        # the data area; x3 points at its middle
HANDLER_REGS = (30, 31)
BASE_REG = 3

SPECIAL = (0, 1, 2, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000000, 0x7FFFFFFF, 0x80000001, 0x0000FFFF,
           0xFFFF0000, 0x55555555, 0xAAAAAAAA, 31, 32, 33)

ALU_RR = ("add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and")
ALU_RI = ("addi", "slti", "sltiu", "xori", "ori", "andi")
SHIFT_I = ("slli", "srli", "srai")
MULDIV = ("mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu")
LOADS = (("lb", 1), ("lh", 2), ("lw", 4), ("lbu", 1), ("lhu", 2))
STORES = (("sb", 1), ("sh", 2), ("sw", 4))
BRANCHES = ("beq", "bne", "blt", "bge", "bltu", "bgeu")

# CSRs whose values Spike and docs/ISA.md agree on (rvcsrmap.py's comparison).
CSR_FREE = ("mscratch", "mtval", "mepc")           # any value, any instruction
CSR_IGNORED_WRITES = ("mstatush", "mhpmevent3", "mhpmevent17", "mhpmevent31",
                      "mhpmcounter3", "mhpmcounter12h", "mhpmcounter31")
CSR_READ_ONLY_OK = ("mvendorid", "mhartid", "mconfigptr", "misa", "instret", "instreth",
                    "hpmcounter3", "hpmcounter20h", "minstreth", "mie", "mtvec")
CSR_RO_WRITE_TRAP = ("mvendorid", "marchid", "mimpid", "mhartid", "mconfigptr", "cycle",
                     "time", "instret", "cycleh", "instreth", "hpmcounter3", "hpmcounter31h")
LEGAL_CAUSES = (0, 1, 2, 3, 4, 5, 6, 7, 11, 0x80000003, 0x80000007, 0x8000000B)
# CSR addresses Spike implements although SurfCPU does not, or the other way round.
_CSR_DISAGREE = set(range(0x7A0, 0x7B0)) | set(range(0x3A0, 0x3F0))

DEFAULT_WEIGHTS = {
    "alu_rr": 14, "alu_ri": 12, "shift_i": 4, "upper": 3, "muldiv": 8, "muldiv_special": 3,
    "load": 9, "store": 9, "fresh_ptr": 2, "branch": 5, "loop": 2, "jal": 2, "jalr": 2,
    "call": 2, "csr": 6, "ecall": 1, "ebreak": 1, "illegal": 2, "csr_trap": 1.2,
    "misaligned_mem": 2, "access_fault": 1.5, "fetch_fault": 0.7, "misaligned_jump": 1.2,
    "mret": 0.5, "fence": 1.5,
}
_STRAIGHT = ("alu_rr", "alu_ri", "shift_i", "upper", "muldiv", "muldiv_special", "load",
             "store", "fresh_ptr", "csr", "ecall", "ebreak", "illegal", "csr_trap",
             "misaligned_mem", "access_fault", "misaligned_jump", "fence")


def _mix64(z: int) -> int:
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return z ^ (z >> 31)


class Rng:
    """SplitMix64: reproducible on every Python version. The seed is mixed first, so that
    neighbouring seeds do not give the same stream shifted by one step."""

    def __init__(self, seed: int):
        self.s = _mix64(_mix64((seed & MASK64) ^ 0x632BE59BD9B4E019))

    def next64(self) -> int:
        self.s = (self.s + 0x9E3779B97F4A7C15) & MASK64
        return _mix64(self.s)

    def below(self, n: int) -> int:
        if n <= 1:
            return 0
        lim = MASK64 - (MASK64 + 1) % n
        while True:
            x = self.next64()
            if x <= lim:
                return x % n

    def u32(self) -> int:
        return self.next64() >> 32

    def chance(self, p: float) -> bool:
        return self.below(1 << 30) < int(p * (1 << 30))

    def choice(self, seq):
        return seq[self.below(len(seq))]

    def weighted(self, weights: dict[str, float]) -> str:
        keys = sorted(weights)
        total = sum(int(weights[k] * 100) for k in keys)
        x = self.below(total)
        for k in keys:
            x -= int(weights[k] * 100)
            if x < 0:
                return k
        return keys[-1]

    def range(self, lo: int, hi: int) -> int:
        """lo..hi inclusive."""
        return lo + self.below(hi - lo + 1)


class Gen:
    def __init__(self, seed: int, length: int, fault_addrs=FAULT_ADDRS, weights=None,
                 illegal_only: bool = False):
        self.seed = seed
        self.length = length
        self.r = Rng(seed)
        self.fault_addrs = tuple(fault_addrs)
        self.weights = dict(weights or DEFAULT_WEIGHTS)
        self.illegal_only = illegal_only
        self.out: list[str] = []
        self.funcs: list[list[str]] = []
        self.nlabel = 0
        self.recent: collections.deque[int] = collections.deque(maxlen=4)
        self.locked: set[int] = set()
        self.in_func = False
        self.stats: collections.Counter[str] = collections.Counter()
        self.static = 0

    # --- emission -----------------------------------------------------------------------
    def emit(self, text: str, count: int = 1) -> None:
        self.out.append("    " + text)
        self.static += count

    def label(self) -> str:
        self.nlabel += 1
        return f"L{self.nlabel}"

    def put_label(self, name: str) -> None:
        self.out.append(f"{name}:")

    # --- registers ----------------------------------------------------------------------
    def dest(self, allow_x0: bool = True) -> int:
        if allow_x0 and self.r.chance(0.05):
            return 0
        while True:
            x = self.r.range(1, 29)
            if x == BASE_REG or x in self.locked or (self.in_func and x == 1):
                continue
            return x

    def wrote(self, rd: int) -> None:
        if rd:
            self.recent.append(rd)

    def src(self) -> int:
        if self.recent and self.r.chance(0.6):
            return self.r.choice(list(self.recent))
        return self.r.range(0, 31)

    def li(self, rd: int, value: int) -> None:
        value &= MASK32
        n = 1 if (value < 0x800 or value >= 0xFFFFF800) else 2
        self.emit(f"li x{rd}, 0x{value:08x}", n)
        self.wrote(rd)

    def value(self) -> int:
        if self.r.chance(0.35):
            return self.r.choice(SPECIAL)
        return self.r.u32()

    def imm12(self) -> int:
        if self.r.chance(0.3):
            return self.r.choice((0, 1, -1, 2047, -2048, 2046, -2047, 16, -16))
        return self.r.range(-2048, 2047)

    # --- items --------------------------------------------------------------------------
    def item(self, kind: str) -> None:
        self.stats[kind] += 1
        getattr(self, "i_" + kind)()

    def straight(self) -> None:
        """One item without control flow (for blocks, loop bodies, functions)."""
        w = {k: v for k, v in self.weights.items() if k in _STRAIGHT}
        if self.in_func:
            w.pop("misaligned_jump", None)       # it writes no ra, but keep functions plain
        self.item(self.r.weighted(w))

    def i_alu_rr(self):
        rd = self.dest()
        self.emit(f"{self.r.choice(ALU_RR)} x{rd}, x{self.src()}, x{self.src()}")
        self.wrote(rd)

    def i_alu_ri(self):
        rd = self.dest()
        self.emit(f"{self.r.choice(ALU_RI)} x{rd}, x{self.src()}, {self.imm12()}")
        self.wrote(rd)

    def i_shift_i(self):
        rd = self.dest()
        sh = self.r.choice((0, 1, 31, 16, self.r.range(0, 31)))
        self.emit(f"{self.r.choice(SHIFT_I)} x{rd}, x{self.src()}, {sh}")
        self.wrote(rd)

    def i_upper(self):
        rd = self.dest()
        op = self.r.choice(("lui", "auipc"))
        imm = self.r.choice((0, 0xFFFFF, 0x80000, 0x7FFFF, self.r.below(1 << 20)))
        self.emit(f"{op} x{rd}, 0x{imm:x}")
        self.wrote(rd)

    def i_muldiv(self):
        rd = self.dest()
        self.emit(f"{self.r.choice(MULDIV)} x{rd}, x{self.src()}, x{self.src()}")
        self.wrote(rd)

    def i_muldiv_special(self):
        a, b = self.dest(False), self.dest(False)
        self.li(a, self.r.choice(SPECIAL))
        self.li(b, self.r.choice((0, 0xFFFFFFFF, 1, 0x80000000, self.value())))
        rd = self.dest()
        self.emit(f"{self.r.choice(MULDIV)} x{rd}, x{a}, x{b}")
        self.wrote(rd)

    def _data_off(self, size: int, aligned: bool = True) -> int:
        off = self.r.range(-2048, 2047 - 3)
        off -= off % size
        if not aligned:
            off += self.r.range(1, size - 1)
        return off

    def i_load(self):
        op, size = self.r.choice(LOADS)
        rd = self.dest()
        self.emit(f"{op} x{rd}, {self._data_off(size)}(x{BASE_REG})")
        self.wrote(rd)

    def i_store(self):
        op, size = self.r.choice(STORES)
        self.emit(f"{op} x{self.src()}, {self._data_off(size)}(x{BASE_REG})")

    def i_fresh_ptr(self):
        """A pointer into the data area made just before its use (load or store)."""
        p = self.dest(False)
        k = self.r.below(DATA_SIZE // 16) * 16
        self.emit(f"la x{p}, rand_data + {k}", 2)
        self.wrote(p)
        if self.r.chance(0.5):
            op, size = self.r.choice(LOADS)
            rd = self.dest()
            off = self.r.range(0, 15) // size * size
            self.emit(f"{op} x{rd}, {off}(x{p})")
            self.wrote(rd)
        else:
            op, size = self.r.choice(STORES)
            off = self.r.range(0, 15) // size * size
            self.emit(f"{op} x{self.src()}, {off}(x{p})")

    def i_branch(self):
        end = self.label()
        a = self.src()
        b = a if self.r.chance(0.3) else (0 if self.r.chance(0.3) else self.src())
        self.emit(f"{self.r.choice(BRANCHES)} x{a}, x{b}, {end}")
        for _ in range(self.r.range(1, 4)):
            self.straight()
        self.put_label(end)

    def i_loop(self):
        c = self.dest(False)
        self.li(c, self.r.range(1, 4))
        self.locked.add(c)
        top = self.label()
        self.put_label(top)
        for _ in range(self.r.range(1, 5)):
            self.straight()
        self.emit(f"addi x{c}, x{c}, -1")
        self.emit(f"bne x{c}, x0, {top}")
        self.locked.discard(c)
        self.wrote(c)

    def i_jal(self):
        end = self.label()
        rd = self.dest()
        self.emit(f"jal x{rd}, {end}")
        self.wrote(rd)
        for _ in range(self.r.range(1, 3)):          # skipped: never executed
            self.straight()
        self.put_label(end)

    def i_jalr(self):
        end = self.label()
        t = self.dest(False)
        self.emit(f"la x{t}, {end}", 2)
        rd = t if self.r.chance(0.25) else self.dest()
        off = self.r.choice((0, 1))                   # bit 0 of the target is cleared
        self.emit(f"jalr x{rd}, {off}(x{t})")
        self.wrote(rd)
        for _ in range(self.r.range(0, 2)):
            self.straight()
        self.put_label(end)

    def i_call(self):
        name = f"F{len(self.funcs)}"
        self.emit(f"jal x1, {name}")
        self.wrote(1)
        saved_out, saved_static = self.out, self.static
        self.out = [f"{name}:"]
        self.in_func = True
        for _ in range(self.r.range(1, 5)):
            self.straight()
        self.emit("jalr x0, 0(x1)")
        self.in_func = False
        self.funcs.append(self.out)
        self.out, self.static = saved_out, saved_static + (self.static - saved_static)

    def i_csr(self):
        kind = self.r.weighted({"free": 5, "mcause": 1, "mstatus": 2, "mie": 1, "mtvec": 0.5,
                                "mcountinhibit": 1, "minstret": 1.5, "read": 2,
                                "ignored": 1})
        rd = self.dest()
        if kind == "free":
            csr = self.r.choice(CSR_FREE)
            self._csr_any(rd, csr)
        elif kind == "mcause":
            if self.r.chance(0.5):
                t = self.dest(False)
                self.li(t, self.r.choice(LEGAL_CAUSES))
                self.emit(f"csrrw x{rd}, mcause, x{t}")
            else:
                cause = self.r.choice(tuple(c for c in LEGAL_CAUSES if c < 32))
                self.emit(f"csrrwi x{rd}, mcause, {cause}")
        elif kind == "mstatus":
            self._csr_any(rd, "mstatus")              # only MIE/MPIE stick, on both models
        elif kind == "mie":
            if self.r.chance(0.5):
                self.emit(f"csrrc x{rd}, mie, x{self.src()}")   # clears: mie stays 0
            else:
                self.emit(f"csrrw x{rd}, mie, x0")
        elif kind == "mtvec":
            t = self.dest(False)
            self.emit(f"la x{t}, trap_handler", 2)
            self.wrote(t)
            self.emit(f"csrrw x{rd}, mtvec, x{t}")
        elif kind == "mcountinhibit":
            v = self.r.choice((0, 1, 4, 5))
            if self.r.chance(0.5):
                self.emit(f"{self.r.choice(('csrrwi', 'csrrsi', 'csrrci'))} x{rd}, "
                          f"mcountinhibit, {v}")
            else:
                t = self.dest(False)
                self.li(t, v)
                self.emit(f"{self.r.choice(('csrrw', 'csrrs', 'csrrc'))} x{rd}, "
                          f"mcountinhibit, x{t}")
        elif kind == "minstret":
            if self.r.chance(0.5):
                self.emit(f"csrrs x{rd}, minstret, x0")
            elif self.r.chance(0.5):
                self.emit(f"csrrw x{rd}, minstret, x{self.src()}")
            else:
                self.emit(f"csrrwi x{rd}, minstret, {self.r.range(0, 31)}")
        elif kind == "read":
            csr = self.r.choice(CSR_READ_ONLY_OK)
            op = self.r.choice(("csrrs", "csrrc", "csrrsi", "csrrci"))
            rs = "x0" if not op.endswith("i") else "0"
            self.emit(f"{op} x{rd}, {csr}, {rs}")
        else:
            self._csr_any(rd, self.r.choice(CSR_IGNORED_WRITES))
        self.wrote(rd)

    def _csr_any(self, rd: int, csr: str) -> None:
        op = self.r.choice(("csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci"))
        if op.endswith("i"):
            self.emit(f"{op} x{rd}, {csr}, {self.r.range(0, 31)}")
        else:
            self.emit(f"{op} x{rd}, {csr}, x{self.src()}")

    def i_ecall(self):
        self.emit("ecall")

    def i_ebreak(self):
        self.emit("ebreak")

    def illegal_word(self) -> int:
        k = self.r.weighted({"fixed": 1, "short": 1, "random": 3})
        if k == "fixed":
            return self.r.choice((
                0x00000000, 0xFFFFFFFF, 0x10200073, 0x00200073, 0x7B200073,
                0x12000073 | (self.r.range(0, 31) << 15) | (self.r.range(0, 31) << 20),
                0x02001013 | (self.r.range(0, 31) << 7) | (self.r.range(0, 31) << 15)
                | (self.r.range(0, 31) << 20),                              # slli imm[5]
                0x02005013 | (self.r.range(0, 31) << 7) | (self.r.range(0, 31) << 15),
                0x42005013 | (self.r.range(0, 31) << 7) | (self.r.range(0, 31) << 15),
                0x00100073 | (self.r.range(1, 31) << 7),                    # ebreak, rd
                0x00000073 | (self.r.range(1, 31) << 15),                   # ecall, rs1
            ))
        if k == "short":
            # 16-bit encodings with a zero upper half: mtval is the same whether the core
            # reports 16 or 32 bits (docs/VERIFICATION.md section 4).
            while True:
                w = self.r.below(1 << 16)
                if w & 3 != 3:
                    return w
        while True:
            w = self.r.u32() | 3
            if self.r.chance(0.7):
                op = self.r.choice((0x03, 0x0F, 0x13, 0x23, 0x33, 0x63, 0x67, 0x73, 0x2F,
                                    0x53, 0x07, 0x27, 0x0B, 0x2B, 0x5B, 0x7B, 0x1B, 0x3B,
                                    0x43, 0x47, 0x4B, 0x4F, 0x57))
                w = (w & ~0x7F) | op
            if w & 0x1C == 0x1C:
                continue
            d = rvisa.decode(w)
            if not d.legal:
                return w

    def i_illegal(self):
        self.emit(f".word 0x{self.illegal_word():08x}")

    def i_csr_trap(self):
        rd = self.dest()
        if self.r.chance(0.5):
            while True:
                a = self.r.below(4096)
                if a not in rvisa.CSRS and a not in _CSR_DISAGREE:
                    break
            op = self.r.choice(("csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci"))
            rs = str(self.r.range(0, 31)) if op.endswith("i") else f"x{self.src()}"
            self.emit(f"{op} x{rd}, 0x{a:03x}, {rs}")
        else:
            csr = self.r.choice(CSR_RO_WRITE_TRAP)
            op = self.r.choice(("csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci"))
            if op in ("csrrw", "csrrwi"):
                rs = str(self.r.range(0, 31)) if op.endswith("i") else f"x{self.src()}"
            else:                                   # a set/clear "writes" if rs1/uimm != 0
                rs = str(self.r.range(1, 31)) if op.endswith("i") else f"x{self.r.range(1, 31)}"
            self.emit(f"{op} x{rd}, {csr}, {rs}")

    def i_misaligned_mem(self):
        if self.r.chance(0.5):
            op, size = self.r.choice((("lh", 2), ("lhu", 2), ("lw", 4)))
            rd = self.dest()
            self.emit(f"{op} x{rd}, {self._data_off(size, aligned=False)}(x{BASE_REG})")
        else:
            op, size = self.r.choice((("sh", 2), ("sw", 4)))
            self.emit(f"{op} x{self.src()}, {self._data_off(size, aligned=False)}"
                      f"(x{BASE_REG})")

    def i_access_fault(self):
        p = self.dest(False)
        base = self.r.choice(self.fault_addrs)
        self.li(p, base)
        if self.r.chance(0.5):
            op, size = self.r.choice(LOADS)
            rd = self.dest()
            self.emit(f"{op} x{rd}, 0(x{p})")
        else:
            op, size = self.r.choice(STORES)
            self.emit(f"{op} x{self.src()}, 0(x{p})")

    def i_fetch_fault(self):
        if self.in_func:
            return self.i_ecall()
        p = self.dest(False)
        self.li(p, self.r.choice([a for a in self.fault_addrs if a % 4 == 0]))
        self.emit(f"jalr x1, 0(x{p})")              # the handler returns to x1
        self.wrote(1)

    def i_misaligned_jump(self):
        nxt = self.label()
        rd = self.dest()
        k = self.r.choice(("jal", "jalr", "branch"))
        if k == "jal":
            self.emit(f"jal x{rd}, {nxt} + 2")       # traps; rd is not written
        elif k == "jalr":
            t = self.dest(False)
            self.emit(f"la x{t}, {nxt}", 2)
            self.wrote(t)
            self.emit(f"jalr x{rd}, {self.r.choice((2, 3, 6))}(x{t})")
        else:
            a = self.src()
            op = self.r.choice(BRANCHES)
            self.emit(f"{op} x{a}, x{a}, {nxt} + 2")  # beq/bge/bgeu taken (trap), others not
        self.put_label(nxt)

    def i_mret(self):
        t = self.dest(False)
        target = self.label()
        self.emit(f"la x{t}, {target}", 2)
        self.wrote(t)
        self.emit(f"csrw mepc, x{t}")
        self.emit("mret")
        self.emit(".word 0x00000000                 # not executed", 0)
        self.put_label(target)

    def i_fence(self):
        k = self.r.choice(("fence", "fence.i", "nop", "hint", "fencefields"))
        if k == "fence":
            self.emit(self.r.choice(("fence", "fence rw, rw", "fence iorw, o", "fence.tso",
                                     "fence w, r")))
        elif k == "fence.i":
            self.emit("fence.i")
        elif k == "nop":
            self.emit("nop")
        elif k == "hint":
            self.emit(f"{self.r.choice(('addi', 'xori', 'slti'))} x0, x{self.src()}, "
                      f"{self.imm12()}")
        else:
            w = (self.r.u32() & ~0x707F) | 0x0F              # FENCE, every field random
            if self.r.chance(0.3):
                w = (self.r.u32() & ~0x707F) | 0x100F        # FENCE.I with fields set
            self.emit(f".word 0x{w:08x}")

    # --- the program --------------------------------------------------------------------
    def body(self) -> None:
        if self.illegal_only:
            while self.static < self.length:
                self.stats["illegal"] += 1
                self.i_illegal()
            return
        top = {k: v for k, v in self.weights.items()}
        while self.static < self.length:
            self.item(self.r.weighted(top))

    def program(self) -> str:
        hdr = [
            f"# SurfCPU random program: {VERSION}, seed {self.seed}, length {self.length}"
            + (", illegal words only" if self.illegal_only else ""),
            "# generated by tools/rvgen.py: do not edit; regenerate with the same seed.",
            "# fault addresses: " + " ".join(f"0x{a:08x}" for a in self.fault_addrs),
            "    .section .text.init, \"ax\"",
            "    .globl _start",
            "_start:",
        ]
        self.out = []
        # Known state: nothing below depends on reset values or on Spike's boot ROM.
        self.emit("la x5, trap_handler", 2)
        self.emit("csrw mtvec, x5")
        self.emit("li x5, 0x1800")
        self.emit("csrw mstatus, x5")
        self.emit("csrw mie, x0")
        self.emit("csrw mcountinhibit, x0")
        for csr in ("mscratch", "mtval", "mepc"):
            self.li(5, self.r.u32())
            self.emit(f"csrw {csr}, x5")
        self.li(5, self.r.choice(LEGAL_CAUSES))
        self.emit("csrw mcause, x5")
        for x in range(1, 32):
            if x == BASE_REG:
                self.emit(f"la x{BASE_REG}, rand_data + {DATA_SIZE // 2}", 2)
            else:
                self.li(x, self.value())
        self.recent.clear()
        self.emit("csrw minstret, x0")
        prologue = self.out
        self.out = []
        self.static = 0
        self.body()
        body = self.out
        tail = [
            "# the end: every register into the signature, then tohost",
            "    csrw mscratch, x31",
            "    la x31, regdump",
        ] + [f"    sw x{x}, {4 * x}(x31)" for x in range(1, 31)] + [
            "    csrr x30, mscratch",
            "    sw x30, 124(x31)",
            "    fence",
            "    li x5, 1",
            "    la x6, tohost",
            "1:  sw x5, 0(x6)",
            "    j 1b",
            "",
            "# leaf functions",
        ]
        funcs = [line for f in self.funcs for line in f]
        handler = [
            "",
            "    .balign 4",
            "trap_handler:                    # uses x30 and x31 only",
            "    csrr x31, mtval",
            "    csrr x31, mepc",
            "    csrr x30, mcause",
            "    addi x30, x30, -1",
            "    beq x30, x0, 1f                # instruction access fault: back to x1",
            "    addi x31, x31, 4",
            "    csrw mepc, x31",
            "    mret",
            "1:  csrw mepc, x1",
            "    mret",
        ]
        data = [
            "",
            "    .data",
            "    .balign 16",
            "    .globl begin_signature",
            "begin_signature:",
            "rand_data:",
        ]
        for i in range(0, DATA_SIZE, 16):
            data.append("    .word " + ", ".join(f"0x{self.r.u32():08x}" for _ in range(4)))
        data += [
            "regdump:",
            "    .fill 32, 4, 0",
            "    .globl end_signature",
            "end_signature:",
            "    .word 0",
            "",
            "    .section .tohost, \"aw\", @progbits",
            "    .balign 64",
            "    .globl tohost",
            "tohost: .dword 0",
            "    .balign 64",
            "    .globl fromhost",
            "fromhost: .dword 0",
        ]
        stats = "# items: " + " ".join(f"{k}={v}" for k, v in sorted(self.stats.items()))
        return "\n".join(hdr + prologue + ["# body"] + body + tail + funcs + handler
                         + data + [stats]) + "\n"


def generate(seed: int, length: int = 3000, fault_addrs=FAULT_ADDRS, weights=None,
             illegal_only: bool = False) -> tuple[str, collections.Counter]:
    g = Gen(seed, length, fault_addrs, weights, illegal_only)
    text = g.program()
    return text, g.stats


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--length", type=int, default=3000,
                    help="static instructions in the body (default %(default)s)")
    ap.add_argument("-o", "--output", help="assembly file to write (default: stdout)")
    ap.add_argument("--fault-addrs", help="comma-separated unmapped addresses (hex)")
    ap.add_argument("--illegal-only", action="store_true")
    ap.add_argument("--stats", action="store_true", help="print item counts on stderr")
    a = ap.parse_args(argv)
    faults = FAULT_ADDRS
    if a.fault_addrs:
        faults = tuple(int(x, 16) for x in a.fault_addrs.split(","))
    text, stats = generate(a.seed, a.length, faults, illegal_only=a.illegal_only)
    if a.output:
        Path(a.output).parent.mkdir(parents=True, exist_ok=True)
        Path(a.output).write_text(text)
    else:
        sys.stdout.write(text)
    if a.stats:
        print(" ".join(f"{k}={v}" for k, v in sorted(stats.items())), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
