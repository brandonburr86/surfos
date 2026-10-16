"""
SurfCPU random program generator tests
--------------------
File: test_rvgen.py Date: 10/15/26 (SurfCPU)
--------------------
The generator is reproducible (same seed, same text; neighbouring seeds unrelated), uses
every item kind, and its assumptions about the reference hold on Spike (in the toolchain
container): every word it uses as an illegal instruction traps with cause 2 and mtval equal
to the word; every fault address faults for loads, stores and fetches; and random programs
run to tohost = 1 with traps of every cause.

Run: python3 -m unittest discover -s sim/ref/tests   (make test-ref)
"""
from __future__ import annotations

import collections
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import helpers
import rvgen
import rvisa
import trace_spike

CC = "riscv64-unknown-elf-gcc"
HAVE_TOOLS = bool(shutil.which(CC) and shutil.which("spike"))
LINK = helpers.ROOT / "sim" / "ref" / "common" / "link.ld"


def build(asm_text: str, tmp: Path, name: str) -> Path:
    src, elf = tmp / f"{name}.S", tmp / f"{name}.elf"
    src.write_text(asm_text)
    subprocess.run([CC, "-march=rv32im_zicsr_zifencei", "-mabi=ilp32", "-nostdlib",
                    "-nostartfiles", "-Wl,--no-warn-rwx-segments", "-T", str(LINK), "-o",
                    str(elf), str(src)], check=True)
    return elf


class Reproducible(unittest.TestCase):
    def test_same_seed_same_text(self):
        a, _ = rvgen.generate(12345, 1500)
        b, _ = rvgen.generate(12345, 1500)
        self.assertEqual(a, b)

    def test_seeds_differ(self):
        texts = {rvgen.generate(s, 800)[0].split("# body", 1)[1] for s in range(1, 9)}
        self.assertEqual(len(texts), 8)
        # neighbouring seeds must not be the same stream shifted by one draw
        r1, r2 = rvgen.Rng(1), rvgen.Rng(2)
        s1 = [r1.next64() for _ in range(50)]
        s2 = [r2.next64() for _ in range(50)]
        self.assertFalse(set(s1) & set(s2))

    def test_every_item_kind(self):
        seen = collections.Counter()
        for s in range(1, 6):
            seen.update(rvgen.generate(s, 3000)[1])
        for kind in rvgen.DEFAULT_WEIGHTS:
            self.assertGreater(seen[kind], 0, kind)

    def test_illegal_words_are_illegal(self):
        g = rvgen.Gen(99, 10)
        for _ in range(5000):
            w = g.illegal_word()
            self.assertTrue(rvisa.traps_on_surfcpu(w), f"{w:08x}")
            if w & 3 != 3:
                self.assertEqual(w >> 16, 0, "16-bit encodings keep a zero upper half")

    def test_no_forbidden_csrs(self):
        """docs/VERIFICATION.md section 4: what random programs must never do."""
        never = {"mcycle", "mcycleh", "mip", "tselect", "tdata1", "tdata2", "tdata3",
                 "tinfo", "mcontext"}
        no_read = {"marchid", "mimpid", "cycle", "cycleh", "time", "timeh"}  # writes trap
        no_write = {"misa", "minstreth", "mcycle"}
        prev = ""
        for seed in range(1, 6):
            for line in rvgen.generate(seed, 3000)[0].splitlines():
                t = line.split("#")[0].replace(",", " ").split()
                if t and t[0] == "wfi":
                    self.fail(line)
                if not t or not t[0].startswith("csrr") or len(t) != 4:
                    prev = line
                    continue
                op, rd, csr, src = t
                writes = op in ("csrrw", "csrrwi") or src not in ("x0", "0")
                self.assertNotIn(csr, never, line)
                self.assertFalse(csr.startswith("pmp"), line)
                if csr in no_read:
                    self.assertTrue(writes, f"reads {csr}: {line}")
                if csr in no_write:
                    self.assertFalse(writes, f"writes {csr}: {line}")
                if csr == "mie" and writes:
                    self.assertTrue(op == "csrrc" or src == "x0", line)
                if csr == "mtvec" and writes:
                    self.assertIn("trap_handler", prev, line)
                if csr == "mcountinhibit" and op.endswith("i"):
                    self.assertIn(int(src), (0, 1, 4, 5), line)
                prev = line


@unittest.skipUnless(HAVE_TOOLS, "needs the RISC-V GCC and Spike (toolchain container)")
class OnSpike(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_illegal_words_trap_on_spike(self):
        text, stats = rvgen.generate(4242, 3000, illegal_only=True)
        res = trace_spike.run_spike(build(text, self.dir, "ill"))
        self.assertEqual(res.status, "pass", res.warnings)
        traps = [r for r in res.records if r.trap]
        self.assertGreaterEqual(len(traps), 2900)
        for r in traps:
            self.assertEqual(r.note("trap", "cause"), "2", f"{r.insn:08x}")
            self.assertEqual(int(r.note("trap", "tval"), 16), r.insn, f"{r.insn:08x}")

    def test_fault_addresses_fault_on_spike(self):
        lines = ["    .section .text.init, \"ax\"", "    .globl _start", "_start:",
                 "    la t0, h", "    csrw mtvec, t0"]
        for a in rvgen.FAULT_ADDRS:
            lines += [f"    li s1, 0x{a:08x}", "    lw a0, 0(s1)", "    lb a0, 0(s1)",
                      "    sw a0, 0(s1)", "    sb a0, 0(s1)"]
            if a % 4 == 0:
                lines += ["    jalr ra, 0(s1)"]
        lines += ["    li t0, 1", "    la t1, tohost", "1:  sw t0, 0(t1)", "    j 1b",
                  "h:  csrr t5, mcause", "    li t6, 1", "    beq t5, t6, 2f",
                  "    csrr t6, mepc", "    addi t6, t6, 4", "    csrw mepc, t6", "    mret",
                  "2:  csrw mepc, ra", "    mret",
                  "    .section .tohost, \"aw\", @progbits", "    .globl tohost",
                  "tohost: .dword 0", "    .globl fromhost", "fromhost: .dword 0"]
        res = trace_spike.run_spike(build("\n".join(lines) + "\n", self.dir, "faults"))
        self.assertEqual(res.status, "pass")
        causes = collections.Counter(r.note("trap", "cause") for r in res.records if r.trap)
        n = len(rvgen.FAULT_ADDRS)
        fetchable = sum(1 for a in rvgen.FAULT_ADDRS if a % 4 == 0)
        self.assertEqual(causes, {"5": 2 * n, "7": 2 * n, "1": fetchable})

    def test_random_programs_pass(self):
        causes = collections.Counter()
        for s in (101, 102):
            res = trace_spike.run_spike(build(rvgen.generate(s, 3000)[0], self.dir, f"r{s}"))
            self.assertEqual(res.status, "pass", res.warnings)
            self.assertFalse(res.warnings)
            causes.update(r.note("trap", "cause") for r in res.records if r.trap)
        for c in ("0", "1", "2", "3", "4", "5", "6", "7", "b"):
            self.assertGreater(causes[c], 0, f"cause {c}")


if __name__ == "__main__":
    unittest.main()
