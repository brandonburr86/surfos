/*
SurfCPU directed test: ECALL, EBREAK, MRET and trap entry
--------------------
File: traps.S Date: 10/15/26 (SurfCPU)
--------------------
ECALL (mcause 11, mtval 0) and EBREAK (mcause 3, mtval = its address) with mepc = the
instruction; trap entry stacks mstatus (MPIE = MIE, MIE = 0, MPP = M) and MRET unstacks it
(MIE = MPIE, MPIE = 1) from both MIE states; MRET to an address written into mepc just
before; a new mtvec takes effect for the next trap; nothing after the trapping instruction
executes; ECALL, EBREAK and an illegal instruction on a path not taken never trap.
*/
#include "surf_test.h"

TEST_BEGIN
test_main:
        // ---------------------------------------------------------- ECALL / EBREAK
        TRAP_EXPECT(1, 11, ecall)
        CHECK(2, t4, 0)
        TRAP_EXPECT(3, 3, ebreak)
        li      TESTNUM, 4                      // mtval = the EBREAK's address
        bne     t4, t6, fail

        // ---------------------------------------------------------- nothing younger executes
        li      TESTNUM, 5
        li      x15, 0
        la      s11, 1f
        ecall
        addi    x15, x15, 1
        addi    x15, x15, 1
        addi    x15, x15, 1
1:      bnez    x15, fail
        li      TESTNUM, 6
        la      s11, 1f
        ebreak
        li      x15, 7
        j       fail
1:      bnez    x15, fail

        // ---------------------------------------------------------- the mstatus stack
        la      x5, stat_handler
        csrrw   x20, mtvec, x5
        li      TESTNUM, 10                     // MIE = 1 before the trap
        csrw    mie, x0                         // no interrupt can be taken
        csrsi   mstatus, MSTATUS_MIE
        csrci   mstatus, 0                      // (no-op read-modify-write)
        li      x11, MSTATUS_MPIE
        csrc    mstatus, x11
        ecall
        li      x7, 0x00001880                  // in the handler: MPP = 11, MPIE = 1, MIE = 0
        bne     x21, x7, fail
        csrr    x14, mstatus                    // after MRET: MIE = 1, MPIE = 1
        li      x7, 0x00001888
        bne     x14, x7, fail
        li      TESTNUM, 11                     // MIE = 0 before the trap
        csrci   mstatus, MSTATUS_MIE
        ebreak
        li      x7, 0x00001800                  // in the handler: MPIE = 0, MIE = 0
        bne     x21, x7, fail
        csrr    x14, mstatus                    // after MRET: MIE = 0, MPIE = 1
        li      x7, 0x00001880
        bne     x14, x7, fail
        li      TESTNUM, 12                     // the handler saw the right mcause and mepc
        li      x7, 3
        bne     x22, x7, fail
        csrw    mtvec, x20

        // ---------------------------------------------------------- MRET on its own
        li      TESTNUM, 20                     // MPIE = 1 -> MIE = 1; pc = mepc
        li      x11, MSTATUS_MPIE
        csrs    mstatus, x11
        csrci   mstatus, MSTATUS_MIE
        la      x12, 1f
        csrw    mepc, x12
        mret
        j       fail
1:      csrr    x14, mstatus
        li      x7, 0x00001888
        bne     x14, x7, fail
        li      TESTNUM, 21                     // MPIE = 0 -> MIE = 0, MPIE = 1
        li      x11, MSTATUS_MPIE
        csrc    mstatus, x11
        csrsi   mstatus, MSTATUS_MIE
        la      x12, 1f - 8
        addi    x12, x12, 8                     // mepc computed just before
        csrw    mepc, x12
        mret
        j       fail
1:      csrr    x14, mstatus
        li      x7, 0x00001880
        bne     x14, x7, fail
        li      TESTNUM, 22                     // MRET to an mepc loaded just before
        la      x5, mret_target
        lw      x12, 0(x5)
        csrw    mepc, x12
        mret
        j       fail
mret_back:

        // ---------------------------------------------------------- a new mtvec
        li      TESTNUM, 30
        la      x5, other_handler
        csrrw   x20, mtvec, x5
        li      x23, 0
        ecall
        csrw    mtvec, x20
        li      x7, 0x77
        bne     x23, x7, fail

        // ---------------------------------------------------------- not on the taken path
        li      TESTNUM, 40
        mv      x9, t3
        beq     x0, x0, 1f
        ecall
        ebreak
        .word   0x00000000                      // illegal
1:      j       2f
        ecall
        .word   0xffffffff
2:      jal     x1, 3f
        ebreak
3:      CHECK_NO_TRAP(41, x9)
        j       pass

        .balign 4
stat_handler:                                   // records mstatus/mcause, skips the instruction
        csrr    x21, mstatus
        csrr    x22, mcause
        csrr    x24, mepc
        addi    x24, x24, 4
        csrw    mepc, x24
        mret

other_handler:
        li      x23, 0x77
        csrr    x24, mepc
        addi    x24, x24, 4
        csrw    mepc, x24
        mret

        .data
        .balign 4
mret_target:
        .word   mret_back

TEST_DATA
