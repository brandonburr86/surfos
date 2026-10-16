"""
SurfCPU Spike-log converter tests
--------------------
File: test_trace_spike.py Date: 10/15/26 (SurfCPU)
--------------------
tools/trace_spike.py on hand-written Spike logs (every line shape Spike's -l --log-commits
prints) with small ELF files made in memory, and on a real log (Spike's run of
sim/ref/directed/traps.S), whose conversion must equal the fixture trace. No Spike needed;
make test-ref also converts fresh Spike runs.

Run: python3 -m unittest discover -s sim/ref/tests   (make test-ref)
"""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import helpers
import rvelf
import trace_spike
import tracefmt
from helpers import make_elf, words

BOOT = """core   0: 0x00001000 (0x00000297) auipc   t0, 0x0
core   0: 3 0x00001000 (0x00000297) x5  0x00001000
core   0: 0x00001004 (0x02028593) addi    a1, t0, 32
core   0: 3 0x00001004 (0x02028593) x11 0x00001020
core   0: 0x00001008 (0xf1402573) csrr    a0, mhartid
core   0: 3 0x00001008 (0xf1402573) x10 0x00000000
core   0: 0x0000100c (0x0182a283) lw      t0, 24(t0)
core   0: 3 0x0000100c (0x0182a283) x5  0x80000000 mem 0x00001018
core   0: 0x00001010 (0x00028067) jr      t0
core   0: 3 0x00001010 (0x00028067)
"""


class Synthetic(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def elf(self, code: bytes, data_at: int = 0x80002000, data: bytes = b""):
        # code at 0x80000000, tohost at 0x80001000, data at 0x80002000 (one segment)
        img = bytearray(code.ljust(0x1000, b"\0"))
        img += b"\0" * 0x1000                          # tohost page
        img += data
        p = make_elf(self.dir / "t.elf", 0x80000000, bytes(img),
                     {"tohost": 0x80001000, "_start": 0x80000000, "data": data_at})
        return rvelf.read_elf(p)

    def convert(self, log: str, elf):
        warnings = []
        recs, tohost, _ = trace_spike.convert_log(log.splitlines(True), elf, warnings)
        return recs, tohost, warnings

    def test_boot_rom_skipped_and_fields(self):
        code = words(0x00000297, 0x0063a023, 0x0003ae03, 0x0003a003, 0x00138e83)
        elf = self.elf(code, data=words(0x11223344))
        log = BOOT + """core   0: >>>>  _start
core   0: 0x80000000 (0x00000297) auipc   t0, 0x0
core   0: 3 0x80000000 (0x00000297) x5  0x80000000
core   0: 0x80000004 (0x0063a023) sw      t1, 0(t2)
core   0: 3 0x80000004 (0x0063a023) mem 0x80002000 0x0000002a
core   0: 0x80000008 (0x0003ae03) lw      t3, 0(t2)
core   0: 3 0x80000008 (0x0003ae03) x28 0x0000002a mem 0x80002000
core   0: 0x8000000c (0x0003a003) lw      zero, 0(t2)
core   0: 3 0x8000000c (0x0003a003) mem 0x80002000
core   0: 0x80000010 (0x00138e83) lb      t4, 1(t2)
core   0: 3 0x80000010 (0x00138e83) x29 0x00000000 mem 0x80002001
"""
        recs, tohost, warnings = self.convert(log, elf)
        self.assertIsNone(tohost)
        self.assertEqual([r.pc for r in recs], [0x80000000 + 4 * k for k in range(5)])
        self.assertEqual([r.order for r in recs], list(range(5)))
        self.assertEqual((recs[0].rd, recs[0].rd_wdata), (5, 0x80000000))
        self.assertEqual((recs[1].mem_addr, recs[1].wmask, recs[1].mem_wdata),
                         (0x80002000, 0xF, 0x2a))
        self.assertEqual((recs[2].rmask, recs[2].mem_rdata), (0xF, 0x2a))
        # a load into x0: the value comes from the shadow memory
        self.assertEqual((recs[3].rd, recs[3].rmask, recs[3].mem_rdata), (0, 0xF, 0x2a))
        # aligned convention: word address, lane mask, data in its lane (MODULES.md 5)
        self.assertEqual((recs[4].mem_addr, recs[4].rmask, recs[4].mem_rdata),
                         (0x80002000, 0x2, 0x00))
        self.assertEqual([r.pc_next for r in recs[:4]], [r.pc for r in recs[1:]])

    def test_store_sizes_and_tohost_cut(self):
        code = words(0x006381a3, 0x00639123, 0x00532023, 0x00532023)
        elf = self.elf(code)
        log = BOOT + """core   0: 3 0x80000000 (0x006381a3) mem 0x80002003 0x2a
core   0: 3 0x80000004 (0x00639123) mem 0x80002002 0x002a
core   0: 3 0x80000008 (0x00532023) mem 0x80001000 0x00000003
core   0: 3 0x8000000c (0x00532023) mem 0x80001000 0x00000003
"""
        recs, tohost, _ = self.convert(log, elf)
        self.assertEqual(tohost, 3)
        self.assertEqual(len(recs), 3)                 # cut after the first tohost store
        self.assertEqual((recs[0].mem_addr, recs[0].wmask, recs[0].mem_wdata),
                         (0x80002000, 0x8, 0x2a000000))
        self.assertEqual((recs[1].mem_addr, recs[1].wmask, recs[1].mem_wdata),
                         (0x80002000, 0xC, 0x002a0000))

    def test_zero_store_to_tohost_does_not_end(self):
        code = words(0x00532023, 0x00532023)
        elf = self.elf(code)
        log = BOOT + """core   0: 3 0x80000000 (0x00532023) mem 0x80001000 0x00000000
core   0: 3 0x80000004 (0x00532023) mem 0x80001000 0x00000001
"""
        recs, tohost, _ = self.convert(log, elf)
        self.assertEqual((len(recs), tohost), (2, 1))

    def test_traps(self):
        code = words(0x00000073, 0x12340001, 0x000880e7, 0x0013a783)
        elf = self.elf(code)
        log = BOOT + """core   0: 0x80000000 (0x00000073) ecall
core   0: exception trap_machine_ecall, epc 0x80000000
core   0: >>>>  handler
core   0: 0x80000100 (0x34202f73) csrr    t5, mcause
core   0: 3 0x80000100 (0x34202f73) x30 0x0000000b
core   0: 0x80000104 (0x30200073) mret
core   0: 3 0x80000104 (0x30200073) c768_mstatus 0x00001880 c784_mstatush 0x00000000
core   0: 0x80000004 (0x00000001) c.nop
core   0: exception trap_illegal_instruction, epc 0x80000004
core   0:           tval 0x00000001
core   0: 3 0x80000100 (0x34202f73) x30 0x00000002
core   0: 3 0x80000104 (0x30200073) c768_mstatus 0x00001880 c784_mstatush 0x00000000
core   0: 0x80000008 (0x000880e7) jalr    a7
core   0: 3 0x80000008 (0x000880e7) x1  0x8000000c
core   0: exception trap_instruction_access_fault, epc 0x60000000
core   0:           tval 0x60000000
core   0: 3 0x80000100 (0x34202f73) x30 0x00000001
core   0: 3 0x80000104 (0x30200073) c768_mstatus 0x00001880 c784_mstatush 0x00000000
core   0: 0x8000000c (0x0013a783) lw      a5, 1(t2)
core   0: exception trap_load_address_misaligned, epc 0x8000000c
core   0:           tval 0x80002001
core   0: Executed 2 times
core   0: 3 0x80000100 (0x34202f73) x30 0x00000004
"""
        recs, _, _ = self.convert(log, elf)
        t = [r for r in recs if r.trap]
        self.assertEqual([r.pc for r in t], [0x80000000, 0x80000004, 0x60000000, 0x8000000c])
        self.assertEqual([r.note("trap", "cause") for r in t], ["b", "2", "1", "4"])
        self.assertEqual(t[1].insn, 0x0001)            # a 16-bit encoding: upper half 0
        self.assertEqual(t[1].note("trap", "tval"), "00000001")
        self.assertEqual(t[2].insn, 0)                 # a fetch fault fetched nothing
        for r in t:
            self.assertEqual((r.rd, r.wmask), (0, 0))
            self.assertEqual(r.pc_next, 0x80000100)    # the handler
            nxt = recs[recs.index(r) + 1]
            self.assertEqual(nxt.intr, 1)
        self.assertEqual(sum(r.intr for r in recs), 4)

    def test_interrupt(self):
        code = words(0x00000013, 0x00000013)
        elf = self.elf(code)
        log = BOOT + """core   0: 3 0x80000000 (0x00000013)
core   0: exception interrupt #7, epc 0x80000004
core   0: 3 0x80000100 (0x34202f73) x30 0x80000007
core   0: 3 0x80000104 (0x30200073) c768_mstatus 0x00001880 c784_mstatush 0x00000000
core   0: 3 0x80000004 (0x00000013)
"""
        recs, _, _ = self.convert(log, elf)
        self.assertEqual(len(recs), 4)                 # no record for the interrupt itself
        self.assertEqual(recs[0].pc_next, 0x80000004)
        self.assertEqual((recs[1].pc, recs[1].intr, recs[1].trap), (0x80000100, 1, 0))

    def test_errors(self):
        elf = self.elf(words(0x00000013))
        with self.assertRaises(trace_spike.ConvertError):
            self.convert(BOOT + "core   0: 3 0x80000000 (0x00000013) q9 0x1\n", elf)
        with self.assertRaises(trace_spike.ConvertError):
            self.convert(BOOT + "core   0: something new\n", elf)
        with self.assertRaises(trace_spike.ConvertError):
            # a load whose value disagrees with memory
            self.convert(BOOT + "core   0: 3 0x80000000 (0x0003ae03) x28 0x00000001 "
                                "mem 0x80000000\n", elf)


class RealLog(unittest.TestCase):
    def test_traps_log_matches_fixture(self):
        elf = rvelf.read_elf(helpers.TRAPS_ELF)
        with open(helpers.TRAPS_LOG) as fh:
            recs, tohost, _ = trace_spike.convert_log(fh, elf)
        self.assertEqual(tohost, 1)
        want = tracefmt.read_trace(helpers.TRAPS_TRACE)
        self.assertEqual([r.text() for r in recs], [r.text() for r in want])
        self.assertEqual([r.notes for r in recs], [r.notes for r in want])


if __name__ == "__main__":
    unittest.main()
