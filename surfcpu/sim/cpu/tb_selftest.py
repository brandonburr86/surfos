#!/usr/bin/env python3
"""
SurfCPU core testbench self-test
--------------------
File: tb_selftest.py Date: 10/15/26 (SurfCPU)
--------------------
Checks build/cpu/surfcpu_tb against its contract (docs/MODULES.md section 6, sim/README.md):
exit 0 for a pass, 1 for a failure (with the test number), 2 past --max-cycles, 3 for bad
arguments or ELF files; --signature writes the words between the two symbols, one per
line as 8 hex digits; --trace writes the header and one 13-field line per record, with
consecutive order numbers and the tohost store last; --trace-tail keeps only the last
records. The small programs are generated
into build/cpu/selftest/.

    tb_selftest.py [--tb build/cpu/surfcpu_tb]
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OUT = ROOT / "build" / "cpu" / "selftest"
GCC = ["riscv64-unknown-elf-gcc", "-march=rv32im_zicsr_zifencei", "-mabi=ilp32", "-nostdlib", "-nostartfiles",
       "-static", "-Wl,--no-warn-rwx-segments", f"-I{HERE / 'tests'}"]

PROGRAMS = {
    "pass": """
        li      x5, 3
        addi    x5, x5, 4
        j       pass
""",
    "fail5": """
        li      TESTNUM, 5
        j       fail
""",
    "loop": """
1:      j       1b
""",
    "signature": """
        la      x5, begin_signature
        li      x6, 0x11111111
        sw      x6, 0(x5)
        li      x6, 0xdeadbeef
        sw      x6, 8(x5)
        j       pass
        .data
        .balign 16
        .globl  begin_signature
begin_signature:
        .word   0, 0x22222222, 0, 0x0000abcd
        .globl  end_signature
end_signature:
        .word   0x99999999
""",
}


def build(name: str, body: str, link: Path) -> Path:
    src = OUT / f"{name}.S"
    elf = OUT / f"{name}.elf"
    src.write_text('#include "surf_test.h"\nTEST_BEGIN\ntest_main:\n' + body + "\nTEST_DATA\n")
    subprocess.run(GCC + ["-T", str(link), str(src), "-o", str(elf)], check=True)
    return elf


def tb(args: list[str], tbpath: str) -> subprocess.CompletedProcess:
    return subprocess.run([tbpath] + args, capture_output=True, text=True, timeout=120)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tb", default=str(ROOT / "build" / "cpu" / "surfcpu_tb"))
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    link = HERE / "tests" / "link.ld"
    elfs = {name: build(name, body, link) for name, body in PROGRAMS.items()}
    # an ELF whose segment is outside the RAM
    bad_link = OUT / "low.ld"
    bad_link.write_text(link.read_text().replace(". = 0x80000000;", ". = 0x10000000;"))
    elfs["outside"] = build("outside", PROGRAMS["pass"], bad_link)
    notelf = OUT / "not-an-elf.txt"
    notelf.write_text("this is not an ELF file\n")

    checks: list[tuple[str, bool, str]] = []

    def check(what: str, ok: bool, detail: str = "") -> None:
        checks.append((what, ok, detail))

    r = tb(["--elf", str(elfs["pass"])], a.tb)
    check("a passing program exits 0", r.returncode == 0, r.stderr.strip())
    r = tb(["--elf", str(elfs["pass"]), "--ilat", "5:20", "--dlat", "3:9", "--seed", "42"], a.tb)
    check("... also with random latencies", r.returncode == 0, r.stderr.strip())
    r = tb(["--elf", str(elfs["fail5"])], a.tb)
    check("a failing program exits 1 and names test 5", r.returncode == 1 and "test 5" in r.stderr, r.stderr.strip())
    r = tb(["--elf", str(elfs["loop"]), "--max-cycles", "1000"], a.tb)
    check("running past --max-cycles exits 2", r.returncode == 2, r.stderr.strip())
    r = tb([], a.tb)
    check("no --elf exits 3", r.returncode == 3, r.stderr.strip())
    r = tb(["--elf", str(notelf)], a.tb)
    check("a file that is not an ELF exits 3", r.returncode == 3, r.stderr.strip())
    r = tb(["--elf", str(elfs["outside"])], a.tb)
    check("a segment outside the RAM exits 3", r.returncode == 3, r.stderr.strip())
    r = tb(["--elf", str(elfs["pass"]), "--ilat", "0:5"], a.tb)
    check("a latency range starting at 0 exits 3", r.returncode == 3, r.stderr.strip())
    r = tb(["--elf", str(elfs["pass"]), "--frobnicate"], a.tb)
    check("an unknown option exits 3", r.returncode == 3, r.stderr.strip())
    r = tb(["--elf", str(elfs["pass"]), "--signature", str(OUT / "x.sig")], a.tb)
    check("--signature without the symbols exits 3", r.returncode == 3, r.stderr.strip())

    sig = OUT / "signature.sig"
    r = tb(["--elf", str(elfs["signature"]), "--signature", str(sig), "--sig-begin", "begin_signature",
            "--sig-end", "end_signature"], a.tb)
    lines = sig.read_text().split("\n") if sig.exists() else []
    check("--signature writes the four words, 8 hex digits per line",
          r.returncode == 0 and lines == ["11111111", "22222222", "deadbeef", "0000abcd", ""], repr(lines))

    trace = OUT / "pass.trace"
    r = tb(["--elf", str(elfs["pass"]), "--trace", str(trace), "--dlat", "2:7", "--seed", "3"], a.tb)
    tl = trace.read_text().splitlines() if trace.exists() else []
    header_ok = bool(tl) and tl[0] == ("# surfcpu-trace v1: order pc insn trap intr rd rd_wdata mem_addr rmask "
                                       "wmask mem_rdata mem_wdata pc_next")
    recs = [l.split() for l in tl if not l.startswith("#")]
    fields_ok = bool(recs) and all(len(f) == 13 for f in recs)
    order_ok = fields_ok and [int(f[0]) for f in recs] == list(range(len(recs)))
    nm = subprocess.run(["riscv64-unknown-elf-nm", str(elfs["pass"])], capture_output=True, text=True).stdout
    tohost = next(int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(" tohost"))
    last_ok = fields_ok and int(recs[-1][9], 16) != 0 and int(recs[-1][7], 16) == tohost and \
        int(recs[-1][11], 16) == 1
    check("--trace: the header line", header_ok, tl[0] if tl else "(empty)")
    check("--trace: 13 fields per record, order from 0", fields_ok and order_ok)
    check("--trace: the last record is the store of 1 to tohost", last_ok, " ".join(recs[-1]) if recs else "")
    first_ok = fields_ok and recs[0][1] == "80000000"
    check("--trace: execution starts at 0x80000000", first_ok)

    tail = OUT / "loop.trace"
    r = tb(["--elf", str(elfs["loop"]), "--max-cycles", "20000", "--trace", str(tail), "--trace-tail", "3"], a.tb)
    tt = tail.read_text().splitlines() if tail.exists() else []
    trecs = [l.split() for l in tt if not l.startswith("#")]
    check("--trace-tail 3 keeps only the last 3 records of a long run",
          r.returncode == 2 and len(trecs) == 3 and int(trecs[0][0]) > 100 and
          [int(f[0]) for f in trecs] == list(range(int(trecs[0][0]), int(trecs[0][0]) + 3)),
          f"exit {r.returncode}, {len(trecs)} records")
    r = tb(["--elf", str(elfs["pass"]), "--trace", str(tail), "--trace-tail", "0"], a.tb)
    check("--trace-tail 0 exits 3", r.returncode == 3, r.stderr.strip())

    bad = 0
    for what, ok, detail in checks:
        print(f"  {'ok  ' if ok else 'FAIL'} {what}" + ("" if ok or not detail else f"\n         {detail}"))
        bad += not ok
    print(f"tb_selftest: {len(checks) - bad}/{len(checks)} checks pass")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
