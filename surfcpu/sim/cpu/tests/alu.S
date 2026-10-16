/*
SurfCPU directed test: integer computation
--------------------
File: alu.S Date: 10/15/26 (SurfCPU)
--------------------
Every RV32I computational instruction (register and immediate forms, LUI, AUIPC) with
edge values: carries and overflow, signed and unsigned compares, shift amounts taken from
the low five bits only, sign extension of 12-bit immediates at both extremes; and x0:
writes are discarded by every instruction class and x0 reads 0 as an operand.
*/
#include "surf_test.h"

TEST_BEGIN
test_main:
        // ---------------------------------------------------------- ADD / SUB
        TEST_RR(1,  add, 0x00000000, 0x00000000, 0x00000000)
        TEST_RR(2,  add, 0x00000002, 0x00000001, 0x00000001)
        TEST_RR(3,  add, 0x0000000a, 0x00000003, 0x00000007)
        TEST_RR(4,  add, 0xffff8000, 0x00000000, 0xffff8000)
        TEST_RR(5,  add, 0x7fff8000, 0x80000000, 0xffff8000)
        TEST_RR(6,  add, 0x80000000, 0x7fffffff, 0x00000001)
        TEST_RR(7,  add, 0x00000000, 0xffffffff, 0x00000001)
        TEST_RR(8,  add, 0xfffffffe, 0x7fffffff, 0x7fffffff)
        TEST_RR(10, sub, 0x00000000, 0x00000000, 0x00000000)
        TEST_RR(11, sub, 0xfffffffc, 0x00000003, 0x00000007)
        TEST_RR(12, sub, 0x00008000, 0x00000000, 0xffff8000)
        TEST_RR(13, sub, 0x7fffffff, 0x80000000, 0x00000001)
        TEST_RR(14, sub, 0x80000000, 0x00000000, 0x80000000)
        TEST_RR(15, sub, 0x00000001, 0xffffffff, 0xfffffffe)

        // ---------------------------------------------------------- SLT / SLTU
        TEST_RR(20, slt,  0, 0x00000000, 0x00000000)
        TEST_RR(21, slt,  1, 0x00000003, 0x00000007)
        TEST_RR(22, slt,  0, 0x00000007, 0x00000003)
        TEST_RR(23, slt,  1, 0x80000000, 0x00000000)
        TEST_RR(24, slt,  0, 0x00000000, 0x80000000)
        TEST_RR(25, slt,  1, 0xffffffff, 0x00000000)
        TEST_RR(26, slt,  0, 0x7fffffff, 0x80000000)
        TEST_RR(27, slt,  1, 0x80000000, 0x7fffffff)
        TEST_RR(28, sltu, 1, 0x00000003, 0x00000007)
        TEST_RR(29, sltu, 0, 0xffffffff, 0x00000000)
        TEST_RR(30, sltu, 1, 0x00000000, 0xffffffff)
        TEST_RR(31, sltu, 0, 0x80000000, 0x7fffffff)
        TEST_RR(32, sltu, 1, 0x7fffffff, 0x80000000)
        TEST_RR(33, sltu, 0, 0x80000000, 0x80000000)

        // ---------------------------------------------------------- logic
        TEST_RR(40, and, 0x0f000f00, 0xff00ff00, 0x0f0f0f0f)
        TEST_RR(41, and, 0x00f000f0, 0x0ff00ff0, 0xf0f0f0f0)
        TEST_RR(42, or,  0xff0fff0f, 0xff00ff00, 0x0f0f0f0f)
        TEST_RR(43, or,  0xfff0fff0, 0x0ff00ff0, 0xf0f0f0f0)
        TEST_RR(44, xor, 0xf00ff00f, 0xff00ff00, 0x0f0f0f0f)
        TEST_RR(45, xor, 0xff00ff00, 0x0ff00ff0, 0xf0f0f0f0)

        // ---------------------------------------------------------- shifts (rs2[4:0] only)
        TEST_RR(50, sll, 0x00000080, 0x00000001, 7)
        TEST_RR(51, sll, 0x80000000, 0xffffffff, 31)
        TEST_RR(52, sll, 0x48484000, 0x21212121, 14)
        TEST_RR(53, sll, 0x00000001, 0x00000001, 0xffffffe0)
        TEST_RR(54, sll, 0x80000000, 0x00000001, 0xffffffff)
        TEST_RR(55, srl, 0x40000000, 0x80000000, 1)
        TEST_RR(56, srl, 0x00000001, 0xffffffff, 31)
        TEST_RR(57, srl, 0x00008484, 0x21212121, 0xffffffee)
        TEST_RR(58, srl, 0x21212121, 0x21212121, 0xffffffe0)
        TEST_RR(59, sra, 0xc0000000, 0x80000000, 1)
        TEST_RR(60, sra, 0xffffffff, 0x80000000, 31)
        TEST_RR(61, sra, 0x00000000, 0x7fffffff, 31)
        TEST_RR(62, sra, 0xc0c0c0c0, 0x81818181, 0xffffffc1)
        TEST_RR(63, sra, 0x81818181, 0x81818181, 0xffffffe0)
        TEST_RR(64, sra, 0x0000ffff, 0x7fffffff, 15)

        // ---------------------------------------------------------- immediate forms
        TEST_IMM(70, addi,  0x000007ff, 0x00000000, 0x7ff)
        TEST_IMM(71, addi,  0xfffff800, 0x00000000, -2048)
        TEST_IMM(72, addi,  0x80000000, 0x7fffffff, 1)
        TEST_IMM(73, addi,  0x7fffffff, 0x80000000, -1)
        TEST_IMM(74, addi,  0x800007fe, 0x7fffffff, 0x7ff)
        TEST_IMM(75, slti,  1, 0x80000000, 0)
        TEST_IMM(76, slti,  0, 0x00000000, -1)
        TEST_IMM(77, slti,  1, 0xfffff7ff, -2048)
        TEST_IMM(78, slti,  0, 0x7fffffff, 0x7ff)
        TEST_IMM(79, sltiu, 1, 0x00000000, -1)
        TEST_IMM(80, sltiu, 0, 0xffffffff, -1)
        TEST_IMM(81, sltiu, 1, 0xfffff7ff, -2048)
        TEST_IMM(82, sltiu, 1, 0x00000000, 1)
        TEST_IMM(83, sltiu, 0, 0x00000001, 1)
        TEST_IMM(84, xori,  0xff00f00f, 0x00ff0f00, -241)
        TEST_IMM(85, xori,  0x0ff00f00, 0x0ff00ff0, 0x0f0)
        TEST_IMM(86, ori,   0xff00ff0f, 0xff00ff00, 0x70f)
        TEST_IMM(87, ori,   0xffffff0f, 0x00000000, -241)
        TEST_IMM(88, andi,  0x00000700, 0xff00ff00, 0x70f)
        TEST_IMM(89, andi,  0xff00ff00, 0xff00ff00, -241)
        TEST_IMM(90, slli,  0x80000000, 0x00000001, 31)
        TEST_IMM(91, slli,  0x42424242, 0x21212121, 1)
        TEST_IMM(92, srli,  0x00000001, 0x80000000, 31)
        TEST_IMM(93, srli,  0x01030303, 0x81818181, 7)
        TEST_IMM(94, srai,  0xffffffff, 0x80000000, 31)
        TEST_IMM(95, srai,  0xc0c0c0c0, 0x81818181, 1)
        TEST_IMM(96, srai,  0x00000000, 0x7fffffff, 31)
        TEST_IMM(97, srai,  0x81818181, 0x81818181, 0)

        // ---------------------------------------------------------- LUI / AUIPC
        li      TESTNUM, 100
        lui     x14, 0x80000
        li      x7, 0x80000000
        bne     x14, x7, fail
        li      TESTNUM, 101
        lui     x14, 0xfffff
        li      x7, 0xfffff000
        bne     x14, x7, fail
        li      TESTNUM, 102
        lui     x14, 0
        bnez    x14, fail
        li      TESTNUM, 103
1:      auipc   x14, 0
        la      x7, 1b
        bne     x14, x7, fail
        li      TESTNUM, 104
1:      auipc   x14, 1
        la      x7, 1b
        li      x8, 0x1000
        add     x7, x7, x8
        bne     x14, x7, fail
        li      TESTNUM, 105
1:      auipc   x14, 0xfffff
        la      x7, 1b
        li      x8, 0xfffff000
        add     x7, x7, x8
        bne     x14, x7, fail

        // ---------------------------------------------------------- x0
        li      TESTNUM, 110
        li      x11, 0x12345678
        add     x0, x11, x11            // every class writing x0 leaves it 0
        addi    x0, x11, 5
        lui     x0, 0x12345
        auipc   x0, 0x12345
        slli    x0, x11, 3
        mv      x14, x0
        bnez    x14, fail
        bnez    x0, fail
        li      TESTNUM, 111
        la      x13, data_word
        lw      x0, 0(x13)              // a load into x0
        add     x14, x0, x0
        bnez    x14, fail
        li      TESTNUM, 112
        jal     x0, 1f                  // a jump linking into x0
1:      la      x15, 2f
        jalr    x0, 0(x15)
2:      bnez    x0, fail
        li      TESTNUM, 113
        csrr    x0, mscratch            // a CSR read into x0
        csrrw   x0, mscratch, x11
        bnez    x0, fail
        li      TESTNUM, 114
        add     x14, x11, x0            // x0 as an operand
        bne     x14, x11, fail
        sub     x14, x0, x11
        neg     x15, x11
        bne     x14, x15, fail
        li      TESTNUM, 115
        addi    x0, x0, 1               // a write to x0 is never forwarded
        addi    x14, x0, 0
        bnez    x14, fail
        addi    x0, x0, 1
        nop
        addi    x14, x0, 0
        bnez    x14, fail
        addi    x0, x0, 1
        nop
        nop
        addi    x14, x0, 0
        bnez    x14, fail
        lw      x0, 0(x13)              // nor is a load into x0
        add     x14, x0, x0
        bnez    x14, fail

        j       pass

        .balign 4
data_word:
        .word   0xdeadbeef

TEST_DATA
