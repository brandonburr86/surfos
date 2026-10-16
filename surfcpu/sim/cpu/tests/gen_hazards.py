#!/usr/bin/env python3
"""
SurfCPU generated test: read-after-write hazards at every distance
--------------------
File: gen_hazards.py Date: 10/15/26 (SurfCPU)
--------------------
Writes hazards_matrix.S (to stdout): every kind of producer (ALU, LUI, load word, load
byte, MUL, DIV, CSR read, JAL and JALR links) writes x13, then after 0 to 4 unrelated
instructions every kind of consumer reads it (ALU rs1, rs2 and both, store data, store
address, load address, branch rs1 and rs2, JALR target, CSR source, multiplier and divider
operands). Each case checks the value the consumer saw. With zero-latency memory the
distances are exact cycle distances (EX/MEM/WB forwarding, the WB->ID bypass, the load-use
interlock); the random-latency runs of the same program mix in every bubble pattern.

    gen_hazards.py > build/cpu/gen/hazards_matrix.S
"""

from __future__ import annotations

import sys

# unrelated filler instructions (they write x16..x19 only)
FILL = ["addi x16, x16, 1", "xori x17, x17, 5", "add x18, x16, x17", "slli x19, x19, 1"]

# Producers: (name, setup, producing instruction(s), value written to x13 or None for a
# link, whether the value can be chosen to be an address). {v} is the value to produce.
PRODUCERS = ["add", "lui", "lw", "lb", "mul", "div", "csrr", "jal", "jalr"]
CONSUMERS = ["alu1", "alu2", "alub", "stdata", "staddr", "ldaddr", "br1", "br2", "jalr",
             "csrw", "mulop", "divop"]
ADDRESS_CONSUMERS = {"staddr", "ldaddr", "jalr"}


def producer(kind: str, value_sym: str | None, value: int, label: int) -> tuple[list[str], list[str]]:
    """Setup lines and producing lines; value_sym (a symbol) replaces value when given."""
    v = value_sym if value_sym else f"0x{value:08x}"
    li = "la" if value_sym else "li"            # a symbol (an address) needs la
    if kind == "add":
        return ([f"la x11, {v}", "li x12, 0"] if value_sym else
                [f"li x11, 0x{(value - 0x1234) & 0xffffffff:08x}", "li x12, 0x1234"]), ["add x13, x11, x12"]
    if kind == "lui":
        return [], [f"lui x13, 0x{value >> 12:05x}"]
    if kind == "lw":
        return [f"la x5, hz_word_{label}"], ["lw x13, 0(x5)"]
    if kind == "lb":
        return [f"la x5, hz_byte_{label}"], ["lb x13, 0(x5)"]
    if kind == "mul":
        return [f"{li} x11, {v}", "li x12, 1"], ["mul x13, x11, x12"]
    if kind == "div":
        return [f"{li} x11, {v}", "li x12, 1"], ["divu x13, x11, x12"]
    if kind == "csrr":
        return [f"{li} x11, {v}", "csrw mscratch, x11"], ["csrr x13, mscratch"]
    raise ValueError(kind)


def emit_case(out: list[str], data: list[str], num: int, prod: str, cons: str, dist: int) -> None:
    fill = FILL[:dist]
    lab = num
    out.append(f"        // case {num}: {prod} -> {cons}, {dist} between")
    out.append(f"        li      TESTNUM, {num}")
    if prod in ("jal", "jalr"):
        # x13 = the link: check it as the consumer at the given distance (as a branch operand
        # or through an ALU operation); the expected value is computed beforehand
        out.append(f"        la      x7, hz_link_{lab}")
        if prod == "jal":
            out.append(f"        jal     x13, hz_link_{lab}")
        else:
            out.append(f"        la      x6, hz_link_{lab}")
            out.append(f"        jalr    x13, 0(x6)")
        out.append(f"hz_link_{lab}:")
        out += [f"        {f}" for f in fill]
        if cons == "br1":
            out.append("        bne     x13, x7, fail")
        elif cons == "br2":
            out.append("        bne     x7, x13, fail")
        else:                                   # alu1
            out.append("        sub     x14, x13, x7")
            out.append("        bnez    x14, fail")
        return
    if cons in ADDRESS_CONSUMERS:
        value_sym, value = f"hz_target_{lab}", 0
    elif prod == "lui":
        value_sym, value = None, (0x12345 + num) << 12 & 0xfffff000
    elif prod == "lb":
        value_sym, value = None, 0xffffff80 | (num & 0x7f)   # a negative byte, sign extended
    else:
        value_sym, value = None, (0x5a000000 + num * 0x1013) & 0xffffffff
    setup, prodlines = producer(prod, value_sym, value, lab)
    if prod == "lw":
        data.append(f"hz_word_{lab}: .word {value_sym if value_sym else f'0x{value:08x}'}")
    if prod == "lb":
        data.append(f"hz_byte_{lab}: .byte 0x{value & 0xff:02x}")
        data.append("        .balign 4")
    out += [f"        {s}" for s in setup]
    if cons == "alu2":
        out.append("        li      x10, 0x0f0f0f0f")
    if cons in ("br1", "br2", "csrw"):
        out.append(f"        li      x10, 0x{value:08x}")
    if cons == "stdata":
        out.append(f"        la      x6, hz_scratch")
    if cons == "mulop":
        out.append("        li      x10, 3")
    if cons == "divop":
        out.append("        li      x10, 1")
    if cons == "jalr":
        pass
    out += [f"        {p}" for p in prodlines]
    out += [f"        {f}" for f in fill]
    if cons == "alu1":
        out += ["        addi    x14, x13, 1", f"        li      x7, 0x{(value + 1) & 0xffffffff:08x}",
                "        bne     x14, x7, fail"]
    elif cons == "alu2":
        out += ["        xor     x14, x10, x13", f"        li      x7, 0x{value ^ 0x0f0f0f0f:08x}",
                "        bne     x14, x7, fail"]
    elif cons == "alub":
        out += ["        add     x14, x13, x13", f"        li      x7, 0x{(2 * value) & 0xffffffff:08x}",
                "        bne     x14, x7, fail"]
    elif cons == "stdata":
        out += ["        sw      x13, 0(x6)", "        lw      x14, 0(x6)", f"        li      x7, 0x{value:08x}",
                "        bne     x14, x7, fail"]
    elif cons == "staddr":
        out += ["        li      x10, 0x600d0000 + " + str(num), "        sw      x10, 0(x13)",
                f"        lw      x14, hz_target_{lab}", "        bne     x14, x10, fail"]
        data.append(f"hz_target_{lab}: .word 0")
    elif cons == "ldaddr":
        out += ["        lw      x14, 0(x13)", f"        li      x7, 0x{0x0bad0000 + num:08x}",
                "        bne     x14, x7, fail"]
        data.append(f"hz_target_{lab}: .word 0x{0x0bad0000 + num:08x}")
    elif cons == "br1":
        out += ["        beq     x13, x10, 1f", "        j       fail", "1:"]
    elif cons == "br2":
        out += ["        bne     x10, x13, fail"]
    elif cons == "jalr":
        out += ["        jalr    x0, 0(x13)", "        j       fail", f"hz_target_{lab}:"]
    elif cons == "csrw":
        out += ["        csrw    mscratch, x13", "        csrr    x14, mscratch", "        bne     x14, x10, fail"]
    elif cons == "mulop":
        out += ["        mul     x14, x10, x13", f"        li      x7, 0x{(3 * value) & 0xffffffff:08x}",
                "        bne     x14, x7, fail"]
    elif cons == "divop":
        out += ["        divu    x14, x13, x10", f"        li      x7, 0x{value:08x}",
                "        bne     x14, x7, fail"]


def main() -> int:
    out = ["/* Generated by sim/cpu/tests/gen_hazards.py -- do not edit. */",
           '#include "surf_test.h"', "TEST_BEGIN", "test_main:"]
    data: list[str] = []
    num = 1
    for prod in PRODUCERS:
        for cons in CONSUMERS:
            if prod in ("jal", "jalr") and cons not in ("alu1", "br1", "br2"):
                continue
            if prod == "lui" and cons in ADDRESS_CONSUMERS:
                continue                        # a LUI value cannot be a chosen address
            if prod == "lb" and cons in ADDRESS_CONSUMERS:
                continue                        # nor a sign-extended byte
            for dist in range(5):
                emit_case(out, data, num, prod, cons, dist)
                num += 1
    out += ["        j       pass", "", "        .data", "        .balign 4", "hz_scratch: .word 0"]
    out += data
    out += ["", "TEST_DATA", ""]
    sys.stdout.write("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
