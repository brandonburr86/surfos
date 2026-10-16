#!/usr/bin/env python3
"""
SurfCPU quick random-program check against Spike (cpu track, for its own confidence)
--------------------
File: fuzz_spike.py Date: 10/15/26 (SurfCPU)
--------------------
Generates seeded random RV32IM programs with dense register dependences (ALU and
immediate operations, LUI/AUIPC, loads and stores of every width in a data area, MUL and
DIV, forward branches and jumps, JALR through computed addresses, CSR operations on
mscratch, ECALL/EBREAK and misaligned loads handled by the framework's trap handler),
builds them, and compares surfcpu_tb's trace with Spike's commit log under several bus
timings (spike_compare.py). A throwaway sanity tool; the verif track owns the official
generator and comparator (tools/).

    fuzz_spike.py [--count N] [--seed S] [--length L] [--out DIR]
"""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REGS = [f"x{i}" for i in (1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)]   # not gp, t3..t6, s9..s11
DATA_REG = "x20"                                # points at the data area
TIMINGS = [["--ilat", "1:1", "--dlat", "1:1"], ["--ilat", "1:5", "--dlat", "1:9"],
           ["--ilat", "3:20", "--dlat", "2:30"]]


def gen_program(rng: random.Random, length: int) -> str:
    lines = ['#include "surf_test.h"', "TEST_BEGIN", "test_main:", f"        la      {DATA_REG}, fz_data"]
    for r in REGS:
        lines.append(f"        li      {r}, 0x{rng.getrandbits(32):08x}")
    label = 0
    pending: list[tuple[int, int]] = []          # (label, emit after this many more instructions)
    for i in range(length):
        for lab, at in list(pending):
            if at <= i:
                lines.append(f"fz_{lab}:")
                pending.remove((lab, at))
        k = rng.random()
        rd, rs1, rs2 = rng.choice(REGS), rng.choice(REGS), rng.choice(REGS)
        if rng.random() < 0.3:                   # make dependences dense
            rs1 = rd
        if k < 0.30:
            op = rng.choice(["add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and"])
            lines.append(f"        {op} {rd}, {rs1}, {rs2}")
        elif k < 0.45:
            op = rng.choice(["addi", "slti", "sltiu", "xori", "ori", "andi"])
            lines.append(f"        {op} {rd}, {rs1}, {rng.randint(-2048, 2047)}")
        elif k < 0.50:
            op = rng.choice(["slli", "srli", "srai"])
            lines.append(f"        {op} {rd}, {rs1}, {rng.randint(0, 31)}")
        elif k < 0.53:
            lines.append(f"        lui {rd}, 0x{rng.getrandbits(20):05x}")
        elif k < 0.55:
            lines.append(f"        auipc {rd}, 0x{rng.getrandbits(20):05x}")
        elif k < 0.65:
            width = rng.choice(["lb", "lbu", "lh", "lhu", "lw"])
            align = {"lb": 1, "lbu": 1, "lh": 2, "lhu": 2, "lw": 4}[width]
            off = rng.randrange(0, 256, align)
            if rng.random() < 0.03:
                off += 1 if align > 1 else 0     # a misaligned load now and then (trap)
            lines.append(f"        {width} {rd}, {off}({DATA_REG})")
        elif k < 0.73:
            width = rng.choice(["sb", "sh", "sw"])
            align = {"sb": 1, "sh": 2, "sw": 4}[width]
            lines.append(f"        {width} {rs2}, {rng.randrange(0, 256, align)}({DATA_REG})")
        elif k < 0.79:
            op = rng.choice(["mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"])
            lines.append(f"        {op} {rd}, {rs1}, {rs2}")
        elif k < 0.88:
            op = rng.choice(["beq", "bne", "blt", "bge", "bltu", "bgeu"])
            label += 1
            lines.append(f"        {op} {rs1}, {rs2}, fz_{label}")
            pending.append((label, i + rng.randint(1, 6)))
        elif k < 0.91:
            label += 1
            lines.append(f"        jal {rd}, fz_{label}")
            pending.append((label, i + rng.randint(1, 4)))
        elif k < 0.93:
            label += 1
            lines.append(f"        la {rs1}, fz_{label}")
            lines.append(f"        jalr {rd}, 0({rs1})")
            pending.append((label, i + rng.randint(1, 4)))
        elif k < 0.97:
            op = rng.choice(["csrrw", "csrrs", "csrrc"])
            lines.append(f"        {op} {rd}, mscratch, {rs1}")
        elif k < 0.985:
            lines.append("        ecall")
        else:
            lines.append("        ebreak")
    for lab, _ in pending:
        lines.append(f"fz_{lab}:")
    lines += ["        j       pass", "        .data", "        .balign 4", "fz_data:"]
    lines += [f"        .word 0x{rng.getrandbits(32):08x}" for _ in range(80)]
    lines += ["TEST_DATA", ""]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--length", type=int, default=400)
    ap.add_argument("--out", default="build/cpu/fuzz")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    bad = 0
    for n in range(a.count):
        seed = a.seed + n
        rng = random.Random(seed)
        src = out / f"fuzz_{seed}.S"
        elf = out / f"fuzz_{seed}.elf"
        src.write_text(gen_program(rng, a.length))
        r = subprocess.run(["riscv64-unknown-elf-gcc", "-march=rv32im_zicsr_zifencei", "-mabi=ilp32", "-nostdlib",
                            "-nostartfiles", "-static", "-Wl,--no-warn-rwx-segments", f"-I{HERE / 'tests'}",
                            "-T", str(HERE / "tests" / "link.ld"), str(src), "-o", str(elf)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"fuzz {seed}: build failed\n{r.stderr}")
            bad += 1
            continue
        for t, timing in enumerate(TIMINGS):
            r = subprocess.run([sys.executable, str(HERE / "spike_compare.py"), "--out", str(out / f"cmp{t}"), str(elf),
                                "--", "--seed", str(seed)] + timing, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"fuzz {seed} timing {' '.join(timing)}:\n{r.stdout}{r.stderr}")
                bad += 1
                break
    print(f"fuzz_spike: {a.count} programs x {len(TIMINGS)} timings, {bad} failing (seeds {a.seed}..{a.seed + a.count - 1})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
