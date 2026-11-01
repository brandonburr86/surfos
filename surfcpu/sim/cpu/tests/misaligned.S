/*
SurfCPU directed test: misaligned targets and accesses
--------------------
File: misaligned.S Date: 10/15/26 (SurfCPU)
--------------------
A taken branch, JAL or JALR whose target is not 4-byte aligned raises cause 0 on itself
with mtval = the target and writes no register (JALR clears bit 0 first); a branch that
is not taken never does; the misaligned target wins over a fault the target would give.
Halfword loads at odd addresses and word loads not 4-aligned raise cause 4, the stores
cause 6, with mtval = the address; the load writes no register and the store changes no
memory. Byte accesses at any address are fine.
*/
#include "surf_test.h"

#define SENTINEL 0x5a5a5a5a

// JAL x1, off (off even, within +-1 MiB)
#define JAL_X1(off) .word ((((off) >> 20) & 1) << 31) | ((((off) >> 1) & 0x3ff) << 21) | ((((off) >> 11) & 1) << 20) | ((((off) >> 12) & 0xff) << 12) | (1 << 7) | 0x6f
// BEQ/BNE x0, x0, off (off even, within +-4 KiB)
#define BR_X0(funct3, off) .word ((((off) >> 12) & 1) << 31) | ((((off) >> 5) & 0x3f) << 25) | ((funct3) << 12) | ((((off) >> 1) & 0xf) << 8) | ((((off) >> 11) & 1) << 7) | 0x63

TEST_BEGIN
test_main:
        // ---------------------------------------------------------- JAL
        li      x1, SENTINEL
        TRAP_EXPECT(1, 0, JAL_X1(6))
        li      TESTNUM, 2                      // mtval = the target
        addi    x7, t6, 6
        bne     t4, x7, fail
        CHECK(3, x1, SENTINEL)                  // no link written
        TRAP_EXPECT(4, 0, JAL_X1(-2))
        li      TESTNUM, 5
        addi    x7, t6, -2
        bne     t4, x7, fail
        CHECK(6, x1, SENTINEL)
        TRAP_EXPECT(7, 0, JAL_X1(0x7fffe))      // far, and still misaligned
        li      TESTNUM, 8
        li      x7, 0x7fffe
        add     x7, x7, t6
        bne     t4, x7, fail

        // ---------------------------------------------------------- JALR
        la      x5, jalr_target
        addi    x5, x5, 2
        li      x1, SENTINEL
        TRAP_EXPECT(10, 0, jalr x1, 0(x5))
        CHECK_REG(11, t4, x5)
        CHECK(12, x1, SENTINEL)
        la      x5, jalr_target
        addi    x5, x5, 3                       // bit 0 is cleared first: target + 2
        TRAP_EXPECT(13, 0, jalr x1, 0(x5))
        li      TESTNUM, 14
        addi    x7, x5, -1
        bne     t4, x7, fail
        la      x5, jalr_target
        TRAP_EXPECT(15, 0, jalr x1, 6(x5))      // through the immediate
        li      TESTNUM, 16
        addi    x7, x5, 6
        bne     t4, x7, fail
        CHECK(17, x1, SENTINEL)
        li      x5, UNMAPPED_ADDR + 2           // misaligned and unmapped: misaligned wins
        TRAP_EXPECT(18, 0, jalr x0, 0(x5))
        CHECK_REG(19, t4, x5)
        li      x5, SENTINEL                    // rd = rs1: the register keeps its old value
        li      x5, UNMAPPED_ADDR + 2
        TRAP_EXPECT(20, 0, jalr x5, 0(x5))
        li      TESTNUM, 21
        li      x7, UNMAPPED_ADDR + 2
        bne     x5, x7, fail

        // ---------------------------------------------------------- branches
        TRAP_EXPECT(30, 0, BR_X0(0, 6))         // beq taken, +6
        li      TESTNUM, 31
        addi    x7, t6, 6
        bne     t4, x7, fail
        TRAP_EXPECT(32, 0, BR_X0(0, -2))        // beq taken, -2
        li      TESTNUM, 33
        addi    x7, t6, -2
        bne     t4, x7, fail
        TRAP_EXPECT(34, 0, BR_X0(1, 0) | (1 << 15) | 0x300)    // bne x1, x0, +6 (x1 != 0)
        li      TESTNUM, 35                     // not taken: never a trap
        mv      x9, t3
        BR_X0(1, 6)                             // bne x0, x0, +6
        BR_X0(1, -2)                            // bne x0, x0, -2
        BR_X0(1, 0x7fe)                         // bne x0, x0, +2046
        CHECK_NO_TRAP(36, x9)

        // ---------------------------------------------------------- loads
        la      x13, data
        li      x14, SENTINEL
        TRAP_EXPECT(40, 4, lh x14, 1(x13))
        li      TESTNUM, 41
        addi    x7, x13, 1
        bne     t4, x7, fail
        CHECK(42, x14, SENTINEL)
        TRAP_EXPECT(43, 4, lhu x14, 3(x13))
        TRAP_EXPECT(44, 4, lw x14, 1(x13))
        TRAP_EXPECT(45, 4, lw x14, 2(x13))
        li      TESTNUM, 46
        addi    x7, x13, 2
        bne     t4, x7, fail
        TRAP_EXPECT(47, 4, lw x14, 3(x13))
        TRAP_EXPECT(48, 4, lw x14, -1(x13))
        CHECK(49, x14, SENTINEL)
        li      TESTNUM, 50                     // byte loads anywhere, halfwords at even addresses
        mv      x9, t3
        lb      x14, 1(x13)
        lbu     x14, 3(x13)
        lh      x14, 2(x13)
        lhu     x14, 0(x13)
        CHECK_NO_TRAP(51, x9)

        // ---------------------------------------------------------- stores
        li      x11, 0x11223344
        TRAP_EXPECT(60, 6, sh x11, 1(x13))
        li      TESTNUM, 61
        addi    x7, x13, 1
        bne     t4, x7, fail
        TRAP_EXPECT(62, 6, sh x11, 3(x13))
        TRAP_EXPECT(63, 6, sw x11, 1(x13))
        TRAP_EXPECT(64, 6, sw x11, 2(x13))
        TRAP_EXPECT(65, 6, sw x11, 3(x13))
        li      TESTNUM, 66
        addi    x7, x13, 3
        bne     t4, x7, fail
        li      TESTNUM, 67                     // memory is unchanged
        lw      x14, 0(x13)
        li      x7, 0x87654321
        bne     x14, x7, fail
        lw      x14, 4(x13)
        li      x7, 0x0fedcba9
        bne     x14, x7, fail
        li      TESTNUM, 68                     // byte stores anywhere
        mv      x9, t3
        sb      x11, 1(x13)
        sb      x11, 3(x13)
        sh      x11, 2(x13)
        CHECK_NO_TRAP(69, x9)
        lw      x14, 0(x13)
        li      x7, 0x33444421
        bne     x14, x7, fail
        j       pass

        .balign 4
jalr_target:
        j       fail
        j       fail

        .data
        .balign 4
data:   .word   0x87654321
        .word   0x0fedcba9

TEST_DATA
