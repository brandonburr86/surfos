"""
SurfCPU trace comparator self-tests
--------------------
File: test_trace_compare.py Date: 10/15/26 (SurfCPU)
--------------------
Every kind of mismatch, injected into a real reference trace (Spike's run of
sim/ref/directed/traps.S), must be reported with its name at the record where it was
injected. The trace conventions of MODULES.md sections 5 and 8 (aligned memory fields, a
trap's pc_next = mtvec and no memory access, a 16-bit encoding's upper half 0, the final
tohost store present) are enforced, and the lenient options relax exactly them.

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


def to_lsb(r):
    """riscv-formal's default memory layout for one record (the reference is aligned)."""
    r = copy.deepcopy(r)
    m = r.rmask | r.wmask
    if m:
        sh = (m & -m).bit_length() - 1
        r.mem_addr += sh
        r.rmask >>= sh
        r.wmask >>= sh
        r.mem_rdata >>= 8 * sh
        r.mem_wdata >>= 8 * sh
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


class Conventions(unittest.TestCase):
    """MODULES.md section 5 as clarified (section 8): strict by default, lenient on request."""

    def setUp(self):
        self.ref = load()

    def test_reference_follows_the_conventions(self):
        for r in self.ref:
            if r.rmask or r.wmask:
                self.assertEqual(r.mem_addr & 3, 0, r.text())
                lanes = trace_compare._lanes(r.rmask | r.wmask)
                self.assertEqual((r.mem_rdata | r.mem_wdata) & ~lanes, 0, r.text())
            if r.trap:
                self.assertEqual((r.rd, r.rmask, r.wmask), (0, 0, 0), r.text())
                if r.insn & 3 != 3:
                    self.assertEqual(r.insn >> 16, 0, r.text())
        traps = [k for k, r in enumerate(self.ref) if r.trap]
        for k in traps:                      # pc_next of a trap is the handler
            self.assertEqual(self.ref[k].pc_next, self.ref[k + 1].pc)
            self.assertEqual(self.ref[k + 1].intr, 1)

    def test_lsb_layout(self):
        dut = [to_lsb(r) for r in self.ref]
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertEqual(res.kind, "mem_layout", res.summary())
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST, lenient_layout=True)
        self.assertTrue(res.ok, res.summary())

    def test_wider_reads(self):
        dut = copy.deepcopy(self.ref)
        n = 0
        for d in dut:
            if d.rmask not in (0, 0xF) and not d.trap:
                d.mem_rdata |= 0xA5A5A5A5 & ~trace_compare._lanes(d.rmask)
                d.rmask = 0xF
                n += 1
        self.assertGreater(n, 0)
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertEqual(res.kind, "mem_layout", res.summary())
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST, lenient_layout=True)
        self.assertTrue(res.ok, res.summary())

    def test_unused_lanes_must_be_zero(self):
        dut = copy.deepcopy(self.ref)
        i = next(k for k, r in enumerate(dut) if r.wmask and r.wmask != 0xF)
        dut[i].mem_wdata |= 0xFF << (8 * ((dut[i].wmask & -dut[i].wmask).bit_length() % 4))
        dut[i].mem_wdata &= 0xFFFFFFFF
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertEqual((res.kind, res.index), ("mem_layout", i), res.summary())

    def test_trap_pc_next_is_the_handler(self):
        dut, i, _ = trace_inject.inject(self.ref, "trap_pc_next")
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertEqual((res.kind, res.index), ("pc_next", i))

    def test_short_encoding_upper_half_zero(self):
        dut = copy.deepcopy(self.ref)
        hits = [k for k, r in enumerate(dut) if r.trap and r.insn & 3 != 3]
        self.assertGreaterEqual(len(hits), 3)
        dut[hits[0]].insn |= 0x12340000
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertEqual((res.kind, res.index), ("insn", hits[0]), res.summary())

    def test_fetch_fault_insn_must_be_zero(self):
        dut = copy.deepcopy(self.ref)
        i = next(k for k, r in enumerate(dut) if r.note("trap", "cause") == "1")
        dut[i].insn = 0x00000013
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertEqual((res.kind, res.index), ("insn", i))

    def test_final_store_required(self):
        ref = trace_compare.cut_at_tohost(self.ref, TRAPS_TOHOST)
        dut = copy.deepcopy(ref[:-1])
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST)
        self.assertEqual((res.kind, res.index), ("missing", len(dut)))
        res = trace_compare.compare(self.ref, dut, tohost=TRAPS_TOHOST,
                                    lenient_final_store=True)
        self.assertTrue(res.ok, res.summary())
        self.assertTrue(any("tohost" in n for n in res.notes))

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
        # MODULES.md: a byte store of 0x2a to 0x80002003 reads 80002000 0 8 ... 2a000000
        ref = self.rec(insn=0x006381a3, mem_addr=0x80002000, wmask=8, mem_wdata=0x2a000000)
        lsb = self.rec(insn=0x006381a3, mem_addr=0x80002003, wmask=1, mem_wdata=0x2a)
        self.assertEqual(trace_compare.compare_records(ref, lsb)[0][0], "mem_layout")
        self.assertEqual(trace_compare.compare_records(ref, lsb, lenient_layout=True), [])
        dut = copy.deepcopy(ref)
        dut.mem_wdata = 0x2b000000
        self.assertEqual(trace_compare.compare_records(ref, dut)[0][0], "mem_data")
        dut.wmask = 0xC
        self.assertEqual(trace_compare.compare_records(ref, dut)[0][0], "mem_mask")

    def test_write_must_be_exact(self):
        ref = self.rec(mem_addr=0x80002000, wmask=0xC, mem_wdata=0x12340000)
        dut = self.rec(mem_addr=0x80002000, wmask=0xF, mem_wdata=0x12340000)
        self.assertEqual(trace_compare.compare_records(ref, dut)[0][0], "mem_mask")
        self.assertEqual(trace_compare.compare_records(ref, dut, lenient_layout=True)[0][0],
                         "mem_mask")

    def test_unknown_rdata_note(self):
        ref = self.rec(mem_addr=0x02000000, rmask=0xF, mem_rdata=0)
        ref.add_note("mem", rdata="unknown")
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
