"""
SurfCPU trace comparator self-tests
--------------------
File: test_trace_compare.py Date: 10/15/26 (SurfCPU)
--------------------
Every kind of mismatch, injected into a real reference trace (Spike's run of
sim/ref/directed/traps.S), must be reported with its name at the record where it was
injected; and the differences RVFI allows (the aligned memory layout, wider reads, a trap's
pc_next, a 16-bit encoding's low half, a missing final tohost store) must not be reported.

Run: python3 -m unittest discover -s sim/ref/tests   (make test-ref)
"""
from __future__ import annotations

import copy
import unittest

import helpers  # noqa: F401  (sets the import path)
import trace_compare
import trace_inject
import tracefmt
from helpers import TRAPS_TOHOST, TRAPS_TRACE


def load():
    return tracefmt.read_trace(TRAPS_TRACE)


def to_aligned(r):
    """riscv-formal's RISCV_FORMAL_ALIGNED_MEM layout for one record."""
    r = copy.deepcopy(r)
    if r.rmask or r.wmask:
        sh = r.mem_addr & 3
        r.mem_addr &= ~3
        r.rmask = (r.rmask << sh) & 0xF
        r.wmask = (r.wmask << sh) & 0xF
        r.mem_rdata = (r.mem_rdata << (8 * sh)) & 0xFFFFFFFF
        r.mem_wdata = (r.mem_wdata << (8 * sh)) & 0xFFFFFFFF
    return r


class InjectedMismatches(unittest.TestCase):
    def setUp(self):
        self.ref = load()

    def test_identical(self):
        res = trace_compare.compare(self.ref, load(), tohost=TRAPS_TOHOST)
        self.assertTrue(res.ok, res.summary())
        self.assertEqual(res.compared, len(self.ref))

    def test_every_kind(self):
        for kind in trace_inject.KINDS:
            with self.subTest(kind=kind):
                dut, i, expect = trace_inject.inject(self.ref, kind)
                res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
                self.assertFalse(res.ok, f"{kind} not detected")
                self.assertEqual(res.kind, expect, res.summary())
                self.assertEqual(res.index, i, res.summary())

    def test_every_kind_elsewhere(self):
        """The same mutations at other places (late records, inside the handler)."""
        for kind in trace_inject.KINDS:
            for start in (100, 400, len(self.ref) - 40):
                with self.subTest(kind=kind, start=start):
                    try:
                        i = trace_inject.pick(self.ref, kind, start)
                    except ValueError:
                        continue
                    dut, i, expect = trace_inject.inject(self.ref, kind, i)
                    res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
                    self.assertEqual((res.kind, res.index), (expect, i), res.summary())

    def test_extra_at_end(self):
        ref = trace_compare.cut_at_tohost(self.ref, TRAPS_TOHOST)
        dut = copy.deepcopy(ref)
        dut.append(copy.deepcopy(dut[-1]))
        dut[-1].order += 1
        res = trace_compare.compare(ref, dut)            # no tohost: nothing is cut
        self.assertEqual((res.kind, res.index), ("extra", len(ref)), res.summary())

    def test_context_lines(self):
        dut, i, _ = trace_inject.inject(self.ref, "rd_data")
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST, context=5)
        self.assertEqual(sum(1 for c in res.context if c.startswith(">>")), 1)
        self.assertEqual(len(res.context), 5 + 2 + 2 * 2)
        text = trace_compare.report(res, "ref", "dut")
        self.assertIn("MISMATCH rd", text)
        self.assertIn("ref:", text)


class AllowedDifferences(unittest.TestCase):
    def setUp(self):
        self.ref = load()

    def test_aligned_memory_layout(self):
        dut = [to_aligned(r) for r in self.ref]
        self.assertTrue(any(r.mem_addr != d.mem_addr for r, d in zip(self.ref, dut)))
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertTrue(res.ok, res.summary())

    def test_wider_reads(self):
        """A core that reads whole words on the bus may report rmask = 1111 for a byte load
        (in the aligned layout), with the other bytes of the word as data."""
        dut = []
        for r in self.ref:
            d = to_aligned(r)
            if d.rmask and not d.trap:
                d.rmask = 0xF
                d.mem_rdata |= 0xA5A5A5A5 & ~_lanes(to_aligned(r).rmask)
            dut.append(d)
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertTrue(res.ok, res.summary())

    def test_trap_pc_next_and_reads(self):
        dut = copy.deepcopy(self.ref)
        n = 0
        for r in dut:
            if r.trap:
                r.pc_next = r.pc + 4                 # the "would-be next" convention
                r.mem_addr, r.rmask, r.mem_rdata = 0x40000000, 0xF, 0x12345678
                n += 1
        self.assertGreater(n, 30)
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertTrue(res.ok, res.summary())

    def test_short_encoding_low_half(self):
        dut = copy.deepcopy(self.ref)
        hits = [r for r in dut if r.trap and r.insn & 3 != 3]
        self.assertGreaterEqual(len(hits), 3)
        for r in hits:
            r.insn &= 0xFFFF
        self.assertTrue(trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST).ok)

    def test_fetch_fault_insn_must_be_zero(self):
        dut = copy.deepcopy(self.ref)
        i = next(k for k, r in enumerate(dut) if r.note("trap", "cause") == "1")
        dut[i].insn = 0x00000013
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertEqual((res.kind, res.index), ("insn", i))

    def test_final_store_optional(self):
        ref = trace_compare.cut_at_tohost(self.ref, TRAPS_TOHOST)
        dut = copy.deepcopy(ref[:-1])
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertTrue(res.ok, res.summary())
        self.assertTrue(any("tohost" in n for n in res.notes))
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST,
                                    strict_final_store=True)
        self.assertEqual(res.kind, "missing")

    def test_records_after_tohost_ignored(self):
        dut = trace_compare.cut_at_tohost(load(), TRAPS_TOHOST)
        tail = copy.deepcopy(dut[-3:])
        for k, r in enumerate(tail):
            r.order = dut[-1].order + 1 + k
            r.pc ^= 0x100
        res = trace_compare.compare(self.ref, dut + tail, tohost=TRAPS_TOHOST)
        self.assertTrue(res.ok, res.summary())

    def test_order_offset(self):
        dut = copy.deepcopy(self.ref)
        for r in dut:
            r.order += 1
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertTrue(res.ok)
        self.assertTrue(any("starts at 1" in n for n in res.notes))


class RecordRules(unittest.TestCase):
    def rec(self, **kw):
        base = dict(order=0, pc=0x80000000, insn=0x00000013, pc_next=0x80000004)
        base.update(kw)
        return tracefmt.Rec(**base)

    def test_byte_store_layouts(self):
        ref = self.rec(insn=0x006381a3, mem_addr=0x80002003, wmask=1, mem_wdata=0x2a)
        dut = self.rec(insn=0x006381a3, mem_addr=0x80002000, wmask=8, mem_wdata=0x2a000000)
        self.assertEqual(trace_compare.compare_records(ref, dut), [])
        dut.mem_wdata = 0x2b000000
        self.assertEqual(trace_compare.compare_records(ref, dut)[0][0], "mem_data")
        dut.wmask = 0xC
        self.assertEqual(trace_compare.compare_records(ref, dut)[0][0], "mem_mask")

    def test_write_must_be_exact(self):
        ref = self.rec(mem_addr=0x80002002, wmask=3, mem_wdata=0x1234)
        dut = self.rec(mem_addr=0x80002000, wmask=0xF, mem_wdata=0x12340000)
        self.assertEqual(trace_compare.compare_records(ref, dut)[0][0], "mem_mask")

    def test_unknown_rdata_note(self):
        ref = self.rec(mem_addr=0x02000000, rmask=0xF, mem_rdata=0)
        ref.notes["mem"] = {"rdata": "unknown"}
        dut = self.rec(mem_addr=0x02000000, rmask=0xF, mem_rdata=0x55)
        self.assertEqual(trace_compare.compare_records(ref, dut), [])

    def test_read_by_non_load(self):
        ref = self.rec()
        dut = self.rec(mem_addr=0x80002000, rmask=0xF)
        self.assertEqual(trace_compare.compare_records(ref, dut)[0][0], "mem_mask")


def _lanes(mask: int) -> int:
    return sum(0xFF << (8 * k) for k in range(4) if mask >> k & 1)


if __name__ == "__main__":
    unittest.main()
