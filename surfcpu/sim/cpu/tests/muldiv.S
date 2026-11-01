/*
SurfCPU directed test: the M extension
--------------------
File: muldiv.S Date: 10/15/26 (SurfCPU)
--------------------
MUL, MULH, MULHSU, MULHU, DIV, DIVU, REM, REMU on the cases the specification singles out:
the signs of the high product halves, rounding of division toward zero, division by zero
(quotient all ones, remainder = dividend) and the signed overflow -2^31 / -1 (quotient
-2^31, remainder 0). Then results used by the very next instruction, back-to-back
divisions, a division whose operands were just computed or loaded, and a division whose
destination is one of its sources. build/cpu/gen/muldiv_edges.S (gen_muldiv.py) adds every
pair of edge values.
*/
#include "surf_test.h"

TEST_BEGIN
test_main:
        TEST_RR(1,  mul,    0x00000015, 3, 7)
        TEST_RR(2,  mul,    0x00000001, -1, -1)
        TEST_RR(3,  mul,    0x80000000, 0x80000000, -1)
        TEST_RR(4,  mul,    0xfffe0001, 0x0000ffff, 0x0000ffff)
        TEST_RR(5,  mul,    0x00000000, 0x80000000, 0x80000000)
        TEST_RR(6,  mul,    0xfffffff9, 7, -1)

        TEST_RR(10, mulh,   0x00000000, -1, -1)
        TEST_RR(11, mulh,   0x40000000, 0x80000000, 0x80000000)
        TEST_RR(12, mulh,   0x3fffffff, 0x7fffffff, 0x7fffffff)
        TEST_RR(13, mulh,   0xffffffff, -1, 1)
        TEST_RR(14, mulh,   0xc0000000, 0x80000000, 0x7fffffff)
        TEST_RR(15, mulh,   0x00000000, 3, 7)

        TEST_RR(20, mulhsu, 0xffffffff, -1, 0xffffffff)
        TEST_RR(21, mulhsu, 0x80000000, 0x80000000, 0xffffffff)
        TEST_RR(22, mulhsu, 0x00000000, 1, 0xffffffff)
        TEST_RR(23, mulhsu, 0x7ffffffe, 0x7fffffff, 0xffffffff)
        TEST_RR(24, mulhsu, 0xffffffff, -1, 1)

        TEST_RR(30, mulhu,  0xfffffffe, 0xffffffff, 0xffffffff)
        TEST_RR(31, mulhu,  0x00000001, 0x80000000, 2)
        TEST_RR(32, mulhu,  0x00000000, 0xffffffff, 1)
        TEST_RR(33, mulhu,  0x7fffffff, 0x80000000, 0xffffffff)

        TEST_RR(40, div,    3, 20, 6)
        TEST_RR(41, div,    -3, -20, 6)
        TEST_RR(42, div,    -3, 20, -6)
        TEST_RR(43, div,    3, -20, -6)
        TEST_RR(44, div,    -1, 1, 0)                   // division by zero
        TEST_RR(45, div,    -1, -1, 0)
        TEST_RR(46, div,    -1, 0x80000000, 0)
        TEST_RR(47, div,    0x80000000, 0x80000000, -1) // overflow
        TEST_RR(48, div,    0x80000000, 0x80000000, 1)
        TEST_RR(49, div,    0, 0, 7)

        TEST_RR(50, divu,   3, 20, 6)
        TEST_RR(51, divu,   0x2aaaaaa7, -20, 6)
        TEST_RR(52, divu,   0, 20, -6)
        TEST_RR(53, divu,   0xffffffff, 1, 0)           // division by zero
        TEST_RR(54, divu,   0xffffffff, 0, 0)
        TEST_RR(55, divu,   0, 0x80000000, -1)
        TEST_RR(56, divu,   0xffffffff, 0xffffffff, 1)

        TEST_RR(60, rem,    2, 20, 6)
        TEST_RR(61, rem,    -2, -20, 6)
        TEST_RR(62, rem,    2, 20, -6)
        TEST_RR(63, rem,    -2, -20, -6)
        TEST_RR(64, rem,    1, 1, 0)                    // remainder of a division by zero
        TEST_RR(65, rem,    -1, -1, 0)
        TEST_RR(66, rem,    0x80000000, 0x80000000, 0)
        TEST_RR(67, rem,    0, 0x80000000, -1)          // overflow
        TEST_RR(68, rem,    0, 0x80000000, 1)

        TEST_RR(70, remu,   2, 20, 6)
        TEST_RR(71, remu,   2, -20, 6)
        TEST_RR(72, remu,   20, 20, -6)
        TEST_RR(73, remu,   1, 1, 0)                    // remainder of a division by zero
        TEST_RR(74, remu,   0xffffffff, -1, 0)
        TEST_RR(75, remu,   0x80000000, 0x80000000, -1)

        // ---------------------------------------------------------- dependences
        li      TESTNUM, 80                     // results used by the next instruction
        li      x11, 1000
        li      x12, 7
        mul     x13, x11, x12
        addi    x14, x13, 1
        li      x7, 7001
        bne     x14, x7, fail
        li      TESTNUM, 81
        div     x13, x11, x12
        addi    x14, x13, 1
        li      x7, 143
        bne     x14, x7, fail
        li      TESTNUM, 82                     // back to back, each using the previous result
        rem     x13, x11, x12                   // 6
        div     x14, x11, x13                   // 166
        div     x15, x14, x13                   // 27
        remu    x16, x15, x12                   // 6
        mul     x17, x16, x15                   // 162
        mulh    x18, x17, x17                   // 0
        li      x7, 6
        bne     x13, x7, fail
        li      x7, 166
        bne     x14, x7, fail
        li      x7, 27
        bne     x15, x7, fail
        li      x7, 6
        bne     x16, x7, fail
        li      x7, 162
        bne     x17, x7, fail
        bnez    x18, fail
        li      TESTNUM, 83                     // operands just loaded (load-use into the divider)
        la      x5, operands
        lw      x11, 0(x5)
        lw      x12, 4(x5)
        div     x13, x11, x12
        li      x7, -12345
        bne     x13, x7, fail
        rem     x13, x11, x12
        li      x7, -6
        bne     x13, x7, fail
        li      TESTNUM, 84                     // destination = source
        li      x11, 100
        li      x12, 7
        div     x11, x11, x12
        li      x7, 14
        bne     x11, x7, fail
        mul     x12, x12, x12
        li      x7, 49
        bne     x12, x7, fail
        rem     x12, x12, x11
        li      x7, 7
        bne     x12, x7, fail
        li      TESTNUM, 85                     // a division result stored at once
        li      x11, 81
        li      x12, 9
        divu    x13, x11, x12
        la      x5, result
        sw      x13, 0(x5)
        lw      x14, 0(x5)
        li      x7, 9
        bne     x14, x7, fail
        li      TESTNUM, 86                     // writes to x0 are discarded
        div     x0, x11, x12
        mul     x0, x11, x12
        bnez    x0, fail
        j       pass

        .data
        .balign 4
operands:
        .word   -86421                          // = -12345 * 7 - 6
        .word   7
result: .word   0

TEST_DATA
