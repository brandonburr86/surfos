/*
SurfCPU directed test: branches and jumps
--------------------
File: branch.S Date: 10/15/26 (SurfCPU)
--------------------
All six branch conditions taken and not taken, forward and backward, with signed and
unsigned edge values; JAL and JALR link values (pc + 4), offsets beyond the branch range,
JALR's immediate and the clearing of target bit 0, rd = rs1, link values and targets used by
the very next instruction, back-to-back taken branches and a counted loop.
*/
#include "surf_test.h"

// the branch must be taken both forward and backward
#define BR_TAKEN(num, inst, v1, v2)                                     \
        li      TESTNUM, num;                                           \
        li      x11, v1;                                                \
        li      x12, v2;                                                \
        inst    x11, x12, 2f;                                           \
        j       fail;                                                   \
1:      j       3f;                                                     \
2:      inst    x11, x12, 1b;                                           \
        j       fail;                                                   \
3:

// the branch must fall through both forward and backward
#define BR_NOT_TAKEN(num, inst, v1, v2)                                 \
        li      TESTNUM, num;                                           \
        li      x11, v1;                                                \
        li      x12, v2;                                                \
        inst    x11, x12, 1f;                                           \
        j       2f;                                                     \
1:      j       fail;                                                   \
2:      inst    x11, x12, 1b;

TEST_BEGIN
test_main:
        BR_TAKEN(1,  beq, 0, 0)
        BR_TAKEN(2,  beq, 1, 1)
        BR_TAKEN(3,  beq, -1, -1)
        BR_NOT_TAKEN(4, beq, 0, 1)
        BR_NOT_TAKEN(5, beq, 1, 0)
        BR_NOT_TAKEN(6, beq, -1, 1)
        BR_NOT_TAKEN(7, beq, 0x80000000, 0)

        BR_TAKEN(10, bne, 0, 1)
        BR_TAKEN(11, bne, 1, 0)
        BR_TAKEN(12, bne, -1, 1)
        BR_TAKEN(13, bne, 0x80000000, 0)
        BR_NOT_TAKEN(14, bne, 0, 0)
        BR_NOT_TAKEN(15, bne, 1, 1)
        BR_NOT_TAKEN(16, bne, -1, -1)

        BR_TAKEN(20, blt, 0, 1)
        BR_TAKEN(21, blt, -1, 1)
        BR_TAKEN(22, blt, -2, -1)
        BR_TAKEN(23, blt, 0x80000000, 0x7fffffff)
        BR_TAKEN(24, blt, 0x80000000, 0)
        BR_NOT_TAKEN(25, blt, 1, 0)
        BR_NOT_TAKEN(26, blt, 1, -1)
        BR_NOT_TAKEN(27, blt, -1, -2)
        BR_NOT_TAKEN(28, blt, 0, 0)
        BR_NOT_TAKEN(29, blt, 0x7fffffff, 0x80000000)

        BR_TAKEN(30, bge, 0, 0)
        BR_TAKEN(31, bge, 1, 1)
        BR_TAKEN(32, bge, -1, -1)
        BR_TAKEN(33, bge, 1, 0)
        BR_TAKEN(34, bge, 1, -2)
        BR_TAKEN(35, bge, -1, -2)
        BR_TAKEN(36, bge, 0x7fffffff, 0x80000000)
        BR_NOT_TAKEN(37, bge, 0, 1)
        BR_NOT_TAKEN(38, bge, -1, 1)
        BR_NOT_TAKEN(39, bge, -2, -1)
        BR_NOT_TAKEN(40, bge, 0x80000000, 0x7fffffff)

        BR_TAKEN(50, bltu, 0, 1)
        BR_TAKEN(51, bltu, 0xfffffffe, 0xffffffff)
        BR_TAKEN(52, bltu, 0, 0xffffffff)
        BR_TAKEN(53, bltu, 0x7fffffff, 0x80000000)
        BR_NOT_TAKEN(54, bltu, 1, 0)
        BR_NOT_TAKEN(55, bltu, 0xffffffff, 0xfffffffe)
        BR_NOT_TAKEN(56, bltu, 0xffffffff, 0)
        BR_NOT_TAKEN(57, bltu, 0x80000000, 0x7fffffff)
        BR_NOT_TAKEN(58, bltu, 5, 5)

        BR_TAKEN(60, bgeu, 0, 0)
        BR_TAKEN(61, bgeu, 1, 1)
        BR_TAKEN(62, bgeu, 0xffffffff, 0xffffffff)
        BR_TAKEN(63, bgeu, 1, 0)
        BR_TAKEN(64, bgeu, 0xffffffff, 0xfffffffe)
        BR_TAKEN(65, bgeu, 0xffffffff, 0)
        BR_TAKEN(66, bgeu, 0x80000000, 0x7fffffff)
        BR_NOT_TAKEN(67, bgeu, 0, 1)
        BR_NOT_TAKEN(68, bgeu, 0xfffffffe, 0xffffffff)
        BR_NOT_TAKEN(69, bgeu, 0, 0xffffffff)
        BR_NOT_TAKEN(70, bgeu, 0x7fffffff, 0x80000000)

        // operands produced by the instructions just before the branch
        li      TESTNUM, 75
        addi    x11, x0, 5
        addi    x12, x0, 5
        bne     x11, x12, fail
        addi    x12, x12, 1
        beq     x11, x12, fail
        blt     x12, x11, fail
        bltu    x12, x11, fail

        // a taken branch to the next instruction
        li      TESTNUM, 76
        li      x13, 0
        beq     x0, x0, 1f
1:      addi    x13, x13, 1
        li      x7, 1
        bne     x13, x7, fail

        // ---------------------------------------------------------- JAL
        li      TESTNUM, 80
        li      x1, 0
9:      jal     x1, 1f
        j       fail
1:      la      x7, 9b + 4
        bne     x1, x7, fail
        li      TESTNUM, 81                     // the link value used at once
9:      jal     x2, 1f
        j       fail
1:      addi    x8, x2, -4
        la      x7, 9b
        bne     x8, x7, fail
        li      TESTNUM, 82                     // beyond the 4 KiB branch range, both ways
9:      jal     x5, far_target
        j       fail
far_back:
        la      x7, 9b + 4
        bne     x5, x7, fail
        li      TESTNUM, 83
        bne     x6, x7, fail                    // set at far_target
        j       jalr_tests

        // ---------------------------------------------------------- JALR
jalr_tests:
        li      TESTNUM, 90
        la      x5, 1f
9:      jalr    x1, 0(x5)
        j       fail
1:      la      x7, 9b + 4
        bne     x1, x7, fail
        li      TESTNUM, 91                     // positive and negative immediates
        la      x5, 1f - 8
        jalr    x1, 8(x5)
        j       fail
1:      la      x5, 1f + 12
        jalr    x1, -12(x5)
        j       fail
1:      li      TESTNUM, 92                     // target bit 0 is cleared (no trap)
        mv      x9, t3
        la      x5, 1f
        ori     x5, x5, 1
        jalr    x1, 0(x5)
        j       fail
1:      la      x5, 1f + 2                      // (1f + 2) - 1 = 1f + 1
        jalr    x1, -1(x5)
        j       fail
1:      la      x5, 1f
        jalr    x1, 1(x5)
        j       fail
1:      CHECK_NO_TRAP(93, x9)
        li      TESTNUM, 94                     // rd = rs1: target from the old value
        la      x5, 1f
9:      jalr    x5, 0(x5)
        j       fail
1:      la      x7, 9b + 4
        bne     x5, x7, fail
        li      TESTNUM, 95                     // target computed just before
        la      x5, 1f - 4
        addi    x5, x5, 4
        jalr    x0, 0(x5)
        j       fail
1:      li      TESTNUM, 96                     // target loaded just before (load-use)
        la      x6, jump_table
        lw      x5, 0(x6)
        jalr    x0, 0(x5)
        j       fail
table_target:
        li      TESTNUM, 97                     // a call and a return
        jal     ra, subroutine
        li      x7, 0x55
        bne     x10, x7, fail

        // ---------------------------------------------------------- flow
        li      TESTNUM, 100                    // back-to-back taken branches and jumps
        li      x13, 0
        beq     x0, x0, 1f
        j       fail
1:      bne     x0, x13, fail
        beq     x0, x0, 2f
        j       fail
2:      jal     x0, 3f
        j       fail
3:      j       4f
        j       fail
4:      li      TESTNUM, 101                    // a counted loop
        li      x13, 100
        li      x14, 0
5:      addi    x14, x14, 3
        addi    x13, x13, -1
        bnez    x13, 5b
        li      x7, 300
        bne     x14, x7, fail
        li      TESTNUM, 102                    // a loop of single-instruction branches
        li      x13, 0
        li      x15, 50
6:      addi    x13, x13, 1
        blt     x13, x15, 6b
        bne     x13, x15, fail
        j       pass

subroutine:
        li      x10, 0x55
        ret

        .balign 4
jump_table:
        .word   table_target

        .space  8192                            // pushes far_target beyond +-4 KiB
far_target:
        mv      x6, x5
        j       far_back

TEST_DATA
