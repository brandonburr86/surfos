/*
SurfCPU directed test: access faults
--------------------
File: access_fault.S Date: 10/15/26 (SurfCPU)
--------------------
Bus errors become precise access faults. Fetch: a jump to an unmapped address completes
(its link is written) and the target raises cause 1 with mepc = mtval = the target;
running off the end of RAM faults at the first address past it, while a jump just before that end never takes the fault its prefetch saw.
Loads (cause 5) and stores (cause 7) to unmapped addresses report the exact byte address
in mtval, write no register and change nothing; the last RAM word works and the next one
faults. (A fetch from an MMIO window is in mmio.S: Spike, unlike the testbench and the
SoC, fetches from its CLINT.)
*/
#include "surf_test.h"

#define SENTINEL   0x5a5a5a5a
#define RAM_END    0x80400000
#define NOP        0x00000013
#define JALR_X0_X5 0x00028067                   // jalr x0, 0(x5)

TEST_BEGIN
test_main:
        // ---------------------------------------------------------- fetch faults
        li      TESTNUM, 1                      // jump to an unmapped address
        li      x5, UNMAPPED_ADDR
        addi    x9, t3, 1
        li      x1, SENTINEL
        la      s11, 1f
9:      jalr    x1, 0(x5)
        j       fail
1:      bne     t3, x9, fail
        li      x7, 1
        bne     t5, x7, fail
        bne     t6, x5, fail                    // mepc = the target
        bne     t4, x5, fail                    // mtval = the target
        li      TESTNUM, 2                      // the jump itself completed
        la      x7, 9b + 4
        bne     x1, x7, fail

        li      TESTNUM, 4                      // running off the end of RAM
        li      x6, RAM_END - 4
        li      x7, NOP
        sw      x7, 0(x6)
        fence.i
        addi    x9, t3, 1
        la      s11, 1f
        jalr    x0, 0(x6)                       // executes the nop, then fetches RAM_END
        j       fail
1:      bne     t3, x9, fail
        li      x7, 1
        bne     t5, x7, fail
        li      x7, RAM_END
        bne     t6, x7, fail
        bne     t4, x7, fail

        li      TESTNUM, 5                      // a jump just before the end: the prefetched
        li      x6, RAM_END - 4                 // fault past it is never taken
        li      x7, JALR_X0_X5
        sw      x7, 0(x6)
        fence.i
        mv      x9, t3
        la      x5, 1f
        jalr    x0, 0(x6)
        j       fail
1:      CHECK_NO_TRAP(6, x9)

        // ---------------------------------------------------------- loads
        li      x13, UNMAPPED_ADDR
        li      x14, SENTINEL
        TRAP_EXPECT(10, 5, lw x14, 0(x13))
        CHECK_REG(11, t4, x13)
        CHECK(12, x14, SENTINEL)
        TRAP_EXPECT(13, 5, lb x14, 3(x13))
        li      TESTNUM, 14                     // mtval is the byte address
        addi    x7, x13, 3
        bne     t4, x7, fail
        TRAP_EXPECT(15, 5, lhu x14, 6(x13))
        li      TESTNUM, 16
        addi    x7, x13, 6
        bne     t4, x7, fail
        TRAP_EXPECT(17, 5, lbu x14, -1(x13))
        TRAP_EXPECT(18, 5, lh x14, -2(x13))
        CHECK(19, x14, SENTINEL)
        li      TESTNUM, 20                     // nothing after the faulting load ran
        li      x15, 0
        la      s11, 1f
        lw      x14, 0(x13)
        addi    x15, x14, 1
        addi    x15, x15, 1
1:      bnez    x15, fail
        CHECK(21, x14, SENTINEL)
        li      TESTNUM, 22                     // the last RAM word works, the next faults
        li      x13, RAM_END - 4
        mv      x9, t3
        lw      x14, 0(x13)
        CHECK_NO_TRAP(23, x9)
        TRAP_EXPECT(24, 5, lw x14, 4(x13))
        li      TESTNUM, 25
        li      x7, RAM_END
        bne     t4, x7, fail
        li      x13, 0x90000000                 // above the RAM
        TRAP_EXPECT(26, 5, lw x14, 0(x13))

        // ---------------------------------------------------------- stores
        li      x13, UNMAPPED_ADDR
        li      x11, 0x12345678
        TRAP_EXPECT(30, 7, sw x11, 0(x13))
        CHECK_REG(31, t4, x13)
        TRAP_EXPECT(32, 7, sb x11, 2(x13))
        li      TESTNUM, 33
        addi    x7, x13, 2
        bne     t4, x7, fail
        TRAP_EXPECT(34, 7, sh x11, -2(x13))
        li      x13, RAM_END
        TRAP_EXPECT(35, 7, sw x11, 0(x13))
        li      TESTNUM, 36                     // a store after a faulting store does not happen
        la      x12, word
        li      x13, UNMAPPED_ADDR
        la      s11, 1f
        sw      x11, 0(x13)
        sw      x11, 0(x12)
1:      lw      x14, 0(x12)
        li      x7, 0x0a0b0c0d
        bne     x14, x7, fail
        j       pass

        .data
        .balign 4
word:   .word   0x0a0b0c0d

TEST_DATA
