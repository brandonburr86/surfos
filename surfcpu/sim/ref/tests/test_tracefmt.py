"""
SurfCPU trace format tests
--------------------
File: test_tracefmt.py Date: 10/15/26 (SurfCPU)
--------------------
The text format of docs/MODULES.md section 5: the example line, round trips, hex rd,
annotations, bad lines, and the byte maps of both RVFI memory layouts.

Run: python3 -m unittest discover -s sim/ref/tests   (make test-ref)
"""
from __future__ import annotations

import io
import unittest

import helpers  # noqa: F401
import tracefmt

EXAMPLE = "0 80000000 00000297 0 0 5 80000000 00000000 0 0 00000000 00000000 80000004"


class Format(unittest.TestCase):
    def test_modules_md_example(self):
        (r,) = tracefmt.parse_lines([tracefmt.HEADER, EXAMPLE])
        self.assertEqual((r.order, r.pc, r.insn, r.rd, r.rd_wdata, r.pc_next),
                         (0, 0x80000000, 0x297, 5, 0x80000000, 0x80000004))
        self.assertEqual(r.text(), EXAMPLE)

    def test_rd_is_hex(self):
        (r,) = tracefmt.parse_lines(["7 80000010 00a00513 0 0 a 0000000a 0 0 0 0 0 80000014"])
        self.assertEqual(r.rd, 10)
        self.assertIn(" a 0000000a ", r.text())

    def test_round_trip_with_notes(self):
        src = ["#@ trap cause=2 tval=00000000", "12 8000004c 00000000 1 0 0 00000000 "
               "00000000 0 0 00000000 00000000 80000094", "# a comment", ""]
        recs = list(tracefmt.parse_lines(src))
        self.assertEqual(recs[0].note("trap", "cause"), "2")
        buf = io.StringIO()
        tracefmt.write_trace(recs, buf, ["x"])
        again = list(tracefmt.parse_lines(buf.getvalue().splitlines()))
        self.assertEqual(again[0].text(), recs[0].text())
        self.assertEqual(again[0].notes, recs[0].notes)

    def test_bad_lines(self):
        for bad in ["0 80000000 00000297", EXAMPLE + " 0",
                    EXAMPLE.replace("00000297", "0000029g"),
                    EXAMPLE.replace(" 0 0 5 ", " 2 0 5 "),        # trap is 0 or 1
                    EXAMPLE.replace(" 0 0 5 ", " 0 0 20 "),       # rd 0x20 > 31
                    EXAMPLE.replace("00000000 0 0 00000000", "00000000 10 0 00000000")]:
            with self.subTest(line=bad), self.assertRaises(tracefmt.TraceError):
                list(tracefmt.parse_lines([bad]))

    def test_mem_bytes_layouts(self):
        # sb of 0x2a to 0x80002003: default layout and aligned layout give the same bytes
        self.assertEqual(tracefmt.mem_bytes(0x80002003, 0x1, 0x2a), {0x80002003: 0x2a})
        self.assertEqual(tracefmt.mem_bytes(0x80002000, 0x8, 0x2a000000), {0x80002003: 0x2a})
        # sh of 0x1234 to 0x80002002
        self.assertEqual(tracefmt.mem_bytes(0x80002002, 0x3, 0x1234),
                         {0x80002002: 0x34, 0x80002003: 0x12})
        self.assertEqual(tracefmt.mem_bytes(0x80002000, 0xC, 0x12340000),
                         {0x80002002: 0x34, 0x80002003: 0x12})
        self.assertEqual(tracefmt.mem_bytes(0x80002000, 0, 0xFFFFFFFF), {})


if __name__ == "__main__":
    unittest.main()
