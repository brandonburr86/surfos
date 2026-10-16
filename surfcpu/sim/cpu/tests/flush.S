/*
SurfCPU directed test: flushes and competing redirects
--------------------
File: flush.S Date: 10/15/26 (SurfCPU)
--------------------
Nothing behind a taken branch, JAL or JALR executes. When an older instruction traps in
MEM while a younger taken branch or jump is in EX, the trap wins (misaligned load, access
fault on a load, ECALL, illegal instruction, misaligned store); the same for MRET and
FENCE.I redirects against a younger branch, and for a taken branch against an even
younger jump. A divide flushed while it runs leaves no trace, and the next divide is
right. Back-to-back redirects, a branch decided by a load just before it, and a jump
whose target is the trapping instruction's resume point.
*/
#include "surf_test.h"

TEST_BEGIN
test_main:
        // ---------------------------------------------------------- shadows of taken flow
        li      TESTNUM, 1
        li      x15, 0
        beq     x0, x0, 1f
        addi    x15, x15, 1
        addi    x15, x15, 1
        addi    x15, x15, 1
        addi    x15, x15, 1
1:      bnez    x15, fail
        li      TESTNUM, 2
        jal     x0, 1f
        addi    x15, x15, 1
        addi    x15, x15, 1
1:      bnez    x15, fail
        li      TESTNUM, 3
        la      x6, 1f
        jalr    x0, 0(x6)
        addi    x15, x15, 1
        addi    x15, x15, 1
1:      bnez    x15, fail
        li      TESTNUM, 4                      // backward taken branch: the fall-through never runs
        li      x13, 3
2:      addi    x13, x13, -1
        bnez    x13, 2b
        addi    x15, x15, 1                     // runs once, after the loop
        li      x7, 1
        bne     x15, x7, fail

        // ---------------------------------------------------------- trap in MEM vs branch in EX
        la      x5, data
        li      x15, 0
        li      TESTNUM, 10                     // misaligned load, then a taken branch
        la      s11, 1f
        lh      x14, 1(x5)
        beq     x0, x0, wrong_10
1:      li      x7, 4
        bne     t5, x7, fail
        bnez    x15, fail
        li      TESTNUM, 11                     // a faulting load, then a taken jump
        li      x6, UNMAPPED_ADDR
        li      x1, 0
        la      s11, 1f
        lw      x14, 0(x6)
        jal     x1, wrong_11
1:      li      x7, 5
        bne     t5, x7, fail
        bnez    x1, fail                        // the JAL wrote no link
        bnez    x15, fail
        li      TESTNUM, 12                     // ECALL, then JALR
        la      x6, wrong_12
        la      s11, 1f
        ecall
        jalr    x1, 0(x6)
1:      li      x7, 11
        bne     t5, x7, fail
        bnez    x1, fail
        bnez    x15, fail
        li      TESTNUM, 13                     // illegal instruction, then a taken branch
        la      s11, 1f
        .word   0
        bne     x5, x0, wrong_13
1:      li      x7, 2
        bne     t5, x7, fail
        bnez    x15, fail
        li      TESTNUM, 14                     // misaligned store, then a taken branch
        la      s11, 1f
        sw      x5, 2(x5)
        blt     x0, x5, wrong_14                // (x5 is negative as signed: not taken)
        bltu    x0, x5, wrong_14
1:      li      x7, 6
        bne     t5, x7, fail
        bnez    x15, fail
        li      TESTNUM, 15                     // EBREAK, then a branch on a load just before
        la      s11, 1f
        ebreak
        lw      x13, 0(x5)
        bnez    x13, wrong_15
1:      li      x7, 3
        bne     t5, x7, fail
        bnez    x15, fail

        // ---------------------------------------------------------- MRET / FENCE.I vs branch
        li      TESTNUM, 20                     // MRET wins over a younger taken branch
        la      x6, 1f
        csrw    mepc, x6
        mret
        beq     x0, x0, wrong_20
1:      bnez    x15, fail
        li      TESTNUM, 21                     // FENCE.I refetches; the branch after it is decided again
        li      x13, 0
        fence.i
        beq     x0, x0, 1f
        j       fail
1:      li      TESTNUM, 22                     // a taken branch wins over a younger jump
        beq     x0, x0, 1f
        jal     x1, wrong_22
        j       fail
1:      bnez    x15, fail

        // ---------------------------------------------------------- the divider flushed while running
        li      TESTNUM, 30
        li      x11, 1000
        li      x12, 3
        li      x14, 0x5a5a
        la      s11, 1f
        lh      x13, 1(x5)                      // traps in MEM while the divide is in EX
        div     x14, x11, x12
        div     x14, x14, x12
1:      li      x7, 0x5a5a                      // the divide never completed
        bne     x14, x7, fail
        div     x14, x11, x12                   // and the next one is right
        li      x7, 333
        bne     x14, x7, fail
        li      TESTNUM, 31                     // a divide in a taken branch's shadow
        beq     x0, x0, 1f
        div     x14, x0, x0
1:      li      x7, 333
        bne     x14, x7, fail
        rem     x14, x11, x12
        li      x7, 1
        bne     x14, x7, fail

        // ---------------------------------------------------------- back-to-back redirects
        li      TESTNUM, 40
        j       1f
2:      j       3f
1:      j       2b
3:      jal     x1, 4f
        j       5f
4:      jalr    x0, 0(x1)
        j       fail
5:      li      TESTNUM, 41                     // a branch decided by a load just before
        lw      x13, 0(x5)
        li      x7, 0x12345678
        beq     x13, x7, 1f
        j       fail
1:      lw      x13, 4(x5)
        beq     x13, x7, wrong_41
        bne     x13, x7, 2f
        j       fail
2:      li      TESTNUM, 42                     // the handler resumes at a jump target
        la      s11, 3f
        ecall
3:      beq     x0, x0, 4f
        j       fail
4:      j       pass

wrong_10:
wrong_11:
wrong_12:
wrong_13:
wrong_14:
wrong_15:
wrong_20:
wrong_22:
wrong_41:
        li      x15, 1
        j       fail

        .data
        .balign 4
data:   .word   0x12345678
        .word   0x9abcdef0

TEST_DATA
