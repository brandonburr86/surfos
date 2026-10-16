"""
SurfCPU decoder and CSR-table tests
--------------------
File: test_rvisa.py Date: 10/15/26 (SurfCPU)
--------------------
tools/rvisa.py is the contract of docs/ISA.md as code: which words are legal, what each one
reads and writes, and which CSR accesses trap. Checked against hand-picked encodings, the
contract's own lists, and (where the RISC-V binutils are installed: the toolchain container)
objdump's disassembly of 20,000 random words.

Run: python3 -m unittest discover -s sim/ref/tests   (make test-ref)
"""
from __future__ import annotations

import random
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import helpers  # noqa: F401
import rvisa


class Decode(unittest.TestCase):
    def test_examples(self):
        cases = {
            0x00000297: ("auipc", "x5,0x0"), 0x09428293: ("addi", "x5,x5,148"),
            0x0063a023: ("sw", "x6,0(x7)"), 0x0003ae03: ("lw", "x28,0(x7)"),
            0x00138e83: ("lb", "x29,1(x7)"), 0x0023d503: ("lhu", "x10,2(x7)"),
            0x30529073: ("csrrw", "x0,mtvec,x5"), 0x30200073: ("mret", ""),
            0x00000073: ("ecall", ""), 0x00100073: ("ebreak", ""), 0x10500073: ("wfi", ""),
            0x0000100f: ("fence.i", ""), 0x0ff0000f: ("fence", "iorw,iorw"),
            0x8330000f: ("fence.tso", ""), 0x0100000f: ("pause", ""),
            0x02c5d533: ("divu", "x10,x11,x12"), 0x40c5d533: ("sra", "x10,x11,x12"),
            0x4015d513: ("srai", "x10,x11,0x1"), 0xfff00093: ("addi", "x1,x0,-1"),
        }
        for w, (m, ops) in cases.items():
            with self.subTest(word=f"{w:08x}"):
                d = rvisa.decode(w)
                self.assertTrue(d.legal)
                self.assertEqual(d.mnemonic, m)
                self.assertEqual(rvisa.disasm(w), (m + " " + ops).strip())

    def test_contract_illegal(self):
        """docs/ISA.md section 1's list of illegal instructions."""
        illegal = [0x00000000, 0xFFFFFFFF, 0x00000001, 0x00004501, 0x12340001, 0x8082,
                   0x10200073,                      # SRET
                   0x00200073,                      # URET
                   0x12000073,                      # SFENCE.VMA
                   0x02059293, 0x0205d293, 0x4205d293,  # SLLI/SRLI/SRAI with imm[5] = 1
                   0x40001033, 0x02001033 | (1 << 30), 0x0000302b, 0x0000300f,
                   0x00003003, 0x00006003, 0x00007023, 0x00002063, 0x00003063,
                   0x00001067, 0x00104073,          # SYSTEM funct3 = 4
                   0x00000053,                      # F extension (OP-FP)
                   0x0000202f,                      # A extension (AMO)
                   0x0000001f,                      # 48-bit encoding
                   0x00100873,                      # EBREAK with rd != 0
                   0x00008073]                      # ECALL with rs1 != 0
        for w in illegal:
            with self.subTest(word=f"{w:08x}"):
                self.assertFalse(rvisa.is_legal(w))
                self.assertTrue(rvisa.traps_on_surfcpu(w))

    def test_fence_fields_ignored(self):
        """FENCE and FENCE.I ignore fm/pred/succ/rs1/rd and imm/rs1/rd (ISA.md section 1)."""
        rng = random.Random(1)
        for _ in range(200):
            hi = rng.getrandbits(17) << 15
            low = rng.getrandbits(5) << 7
            self.assertEqual(rvisa.decode(hi | low | 0x0000000F).mnemonic, "fence")
            self.assertEqual(rvisa.decode(hi | low | 0x0000100F).mnemonic, "fence.i")

    def test_fields(self):
        d = rvisa.decode(0x0023d503)              # lhu x10, 2(x7)
        self.assertEqual((d.kind, d.rd, d.rs1, d.imm, d.size), ("load", 10, 7, 2, 2))
        d = rvisa.decode(0xfe62ae23)              # sw x6, -4(x5)
        self.assertEqual((d.kind, d.rs1, d.rs2, d.imm, d.size), ("store", 5, 6, -4, 4))
        d = rvisa.decode(0xfe0008e3)              # beq x0, x0, -16
        self.assertEqual((d.kind, d.imm), ("branch", -16))
        d = rvisa.decode(0x800000ef)              # jal x1, -1 MiB
        self.assertEqual((d.kind, d.rd, d.imm), ("jal", 1, -(1 << 20)))
        d = rvisa.decode(0x3400d073)              # csrrwi x0, mscratch, 1
        self.assertEqual((d.kind, d.csr, d.imm, d.csr_writes), ("csr", 0x340, 1, True))
        d = rvisa.decode(0x30002573)              # csrrs x10, mstatus, x0
        self.assertFalse(d.csr_writes)


class Csrs(unittest.TestCase):
    def test_table_size(self):
        # 5 id CSRs, mstatus misa mie mtvec mstatush mcountinhibit, 29 mhpmevent,
        # mscratch mepc mcause mtval mip, 16 pmpcfg, 64 pmpaddr, 4 m counters, 58 mhpm,
        # 6 user counters, 58 hpm
        self.assertEqual(len(rvisa.CSRS), 5 + 6 + 29 + 5 + 16 + 64 + 4 + 58 + 6 + 58)

    def test_access_rules(self):
        self.assertTrue(rvisa.csr_access(0x340, True))
        self.assertTrue(rvisa.csr_access(0xF11, False))
        self.assertFalse(rvisa.csr_access(0xF11, True))      # read-only
        self.assertFalse(rvisa.csr_access(0x7A0, False))     # tselect: not implemented
        self.assertFalse(rvisa.csr_access(0x180, False))     # satp: no S-mode
        self.assertFalse(rvisa.csr_access(0x306, False))     # mcounteren: no U-mode
        self.assertTrue(rvisa.csr_access(0x3EF, True))       # pmpaddr63: writes ignored
        self.assertTrue(rvisa.csr_access(0xC9F, False))      # hpmcounter31h
        self.assertFalse(rvisa.csr_access(0xC00, True))      # cycle is read-only

    def test_masks(self):
        self.assertEqual(rvisa.csr_after_write(0x300, 0x1800, 0xFFFFFFFF), 0x1888)
        self.assertEqual(rvisa.csr_after_write(0x305, 0, 0xFFFFFFFF), 0xFFFFFFFC)
        self.assertEqual(rvisa.csr_after_write(0x341, 0, 0x80000003), 0x80000000)
        self.assertEqual(rvisa.csr_after_write(0x301, 0x40001100, 0), 0x40001100)
        self.assertEqual(rvisa.csr_after_write(0x320, 0, 0xFFFFFFFF), 5)


# Instructions objdump decodes whatever the ISA attribute says, which SurfCPU (M-mode only)
# must treat as illegal (docs/ISA.md section 1).
_PRIV_NOT_OURS = ("sret", "uret", "dret", "sfence.", "sinval.", "hfence.", "hinval.", "hlv",
                  "hsv", "wrs.", "mnret", "sctrclr")


@unittest.skipUnless(shutil.which("riscv64-unknown-elf-objdump"), "needs RISC-V binutils")
class AgainstObjdump(unittest.TestCase):
    """30,000 random 32-bit encodings, assembled with `.insn` under the ISA attribute
    rv32i2p1_m2p0_zicsr2p0_zifencei2p0 so that objdump decodes exactly our ISA: every word
    objdump decodes must be legal here with the same text, and every word objdump leaves as
    .insn must be illegal here (except FENCE/FENCE.I with ignored fields)."""

    def test_random_words(self):
        rng = random.Random(20261015)
        ws = []
        while len(ws) < 30000:
            w = rng.getrandbits(32) | 3
            if rng.random() < 0.8:                       # bias towards our opcodes
                w = (w & ~0x7F) | rng.choice([0x03, 0x0F, 0x13, 0x17, 0x23, 0x33, 0x37,
                                              0x63, 0x67, 0x6F, 0x73])
            if w & 0x1C == 0x1C:
                continue                                 # 48-bit and longer encodings
            ws.append(w)
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "w.S"
            src.write_text("    .attribute arch, \"rv32i2p1_m2p0_zicsr2p0_zifencei2p0\"\n"
                           "    .text\n" + "".join(f"    .insn 4, 0x{w:08x}\n" for w in ws))
            obj = Path(tmp) / "w.o"
            subprocess.run(["riscv64-unknown-elf-gcc", "-c", "-march=rv32im_zicsr_zifencei",
                            "-mabi=ilp32", "-o", str(obj), str(src)], check=True)
            out = subprocess.run(["riscv64-unknown-elf-objdump", "-d", "-M",
                                  "no-aliases,numeric", str(obj)], check=True,
                                 capture_output=True, text=True).stdout
        dis = {}
        for line in out.splitlines():
            parts = line.split("\t")
            if len(parts) >= 3 and parts[0].strip().endswith(":"):
                addr = int(parts[0].strip()[:-1], 16)
                dis[addr] = " ".join(p.strip() for p in parts[2:]).split("#")[0].strip()
        bad, legal, illegal, ignored_fields = [], 0, 0, 0
        for k, w in enumerate(ws):
            theirs = dis.get(k * 4, "?")
            d = rvisa.decode(w)
            mine = rvisa.disasm(w, k * 4)
            if theirs.startswith(".insn"):
                if d.legal and d.kind in ("fence", "fencei"):
                    ignored_fields += 1          # fields the ISA says to ignore
                elif d.legal:
                    bad.append(f"{w:08x}: rvisa '{mine}', objdump does not decode it")
                else:
                    illegal += 1
                continue
            if not d.legal:
                rv64_shift = (theirs.split(" ")[0] in ("slli", "srli", "srai")
                              and int(theirs.split(",")[-1], 16) >= 32)
                # objdump decodes RV64 shift amounts even for RV32; ISA.md: illegal.
                if not theirs.startswith(_PRIV_NOT_OURS) and not rv64_shift:
                    bad.append(f"{w:08x}: objdump '{theirs}', rvisa says illegal")
                continue
            legal += 1
            if d.kind == "csr" and d.csr not in rvisa.CSRS:
                # A CSR SurfCPU does not have: objdump may know a name for it.
                mine_ops, theirs_ops = _norm(mine).split(","), _norm(theirs).split(",")
                if len(mine_ops) == 3 and len(theirs_ops) == 3:
                    mine_ops[1] = theirs_ops[1] = "csr"
                if mine_ops != theirs_ops:
                    bad.append(f"{w:08x}: rvisa '{mine}' objdump '{theirs}'")
            elif _norm(mine) != _norm(theirs):
                bad.append(f"{w:08x}: rvisa '{mine}' objdump '{theirs}'")
        self.assertFalse(bad, f"{len(bad)} differ, first: {bad[:10]}")
        self.assertGreater(legal, 15000)
        self.assertGreater(illegal, 3000)
        self.assertLess(ignored_fields, 3000)


def _norm(s: str) -> str:
    s = re.sub(r" <[^>]*>", "", " ".join(s.split())).replace(", ", ",")
    m = s.split(" ", 1)
    if len(m) == 2 and m[0] in ("beq", "bne", "blt", "bge", "bltu", "bgeu", "jal"):
        ops = m[1].split(",")
        ops[-1] = f"{int(ops[-1], 16):x}"              # objdump: no 0x in an ELF listing
        s = m[0] + " " + ",".join(ops)
    return s


if __name__ == "__main__":
    unittest.main()
