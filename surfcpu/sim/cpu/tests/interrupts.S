/*
SurfCPU directed test: interrupts
--------------------
File: interrupts.S Date: 10/15/26 (SurfCPU)
--------------------
mip follows the three sources (CLINT msip and mtimecmp, the testbench's external level).
The timer, software and external interrupts each trap with mcause 0x80000007/3/B, mtval 0
and mstatus stacked; mstatus.MIE = 0 holds a pending enabled interrupt off, and setting
MIE takes it on the very next instruction (mepc = that instruction); the same for an mie
bit; with all three pending, they are taken in the order MEI, MSI, MTI, each at the same
mepc. Then the sweep: an external interrupt raised 1..96 cycles (then up to 1500 in steps of
14) into a block of loads,
stores, MMIO probe accesses, multiplies, divides, CSR writes and branches: the block's
results are always right, exactly one interrupt is taken, and the probes count every
access exactly once (an interrupt never splits or repeats a bus access).
*/
// surf-test: spike=no max-cycles=20000000
#define TEST_IRQ_HANDLER
#include "surf_test.h"

// x24 = log pointer (mcause, mepc pairs), x23 = handler scratch, x22 = log mark
#define LOGGED(n) addi x7, x22, 8 * (n); bne x24, x7, fail;

TEST_BEGIN
test_main:
        la      x24, irq_log
        li      x9, TB_BASE
        li      x5, CLINT_MSIP
        li      x6, CLINT_MTIMECMP

        // ---------------------------------------------------------- mip follows the sources
        li      TESTNUM, 1
        csrr    x14, mip
        bnez    x14, fail
        li      TESTNUM, 2
        li      x11, 1
        sw      x11, 0(x5)                      // msip
        csrr    x14, mip
        li      x7, MIP_MSIP
        bne     x14, x7, fail
        sw      x0, 0(x5)
        csrr    x14, mip
        bnez    x14, fail
        li      TESTNUM, 3
        sw      x11, TB_IRQ_EXT(x9)             // external level
        csrr    x14, mip
        li      x7, MIP_MEIP
        bne     x14, x7, fail
        sw      x0, TB_IRQ_EXT(x9)
        csrr    x14, mip
        bnez    x14, fail
        li      TESTNUM, 4
        sw      x0, 4(x6)                       // mtimecmp = 0: mtime >= mtimecmp
        sw      x0, 0(x6)
        csrr    x14, mip
        li      x7, MIP_MTIP
        bne     x14, x7, fail
        li      x11, -1
        sw      x11, 4(x6)
        sw      x11, 0(x6)
        csrr    x14, mip
        bnez    x14, fail

        // ---------------------------------------------------------- the timer
        li      TESTNUM, 10
        mv      x22, x24
        li      x7, CLINT_MTIME
        lw      x12, 0(x7)
        addi    x12, x12, 300
        sw      x0, 4(x6)                       // hi = 0 (lo is still all ones)
        sw      x12, 0(x6)                      // lo = now + 300
        li      x7, MIP_MTIP
        csrw    mie, x7
        csrsi   mstatus, MSTATUS_MIE
wait_timer:
        beq     x24, x22, wait_timer
        csrci   mstatus, MSTATUS_MIE
        LOGGED(1)
        lw      x14, 0(x22)
        li      x7, 0x80000007
        bne     x14, x7, fail
        lw      x14, 4(x22)
        la      x7, wait_timer
        bne     x14, x7, fail
        li      TESTNUM, 11                     // after MRET: MIE = 1 again (cleared above)
        csrr    x14, mstatus
        li      x7, 0x00001880
        bne     x14, x7, fail

        // ---------------------------------------------------------- software
        li      TESTNUM, 20
        mv      x22, x24
        li      x7, MIP_MSIP
        csrw    mie, x7
        csrsi   mstatus, MSTATUS_MIE
        li      x11, 1
        sw      x11, 0(x5)
1:      beq     x24, x22, 1b
        csrci   mstatus, MSTATUS_MIE
        LOGGED(1)
        lw      x14, 0(x22)
        li      x7, 0x80000003
        bne     x14, x7, fail

        // ---------------------------------------------------------- external
        li      TESTNUM, 30
        mv      x22, x24
        li      x7, MIP_MEIP
        csrw    mie, x7
        csrsi   mstatus, MSTATUS_MIE
        li      x11, 1
        sw      x11, TB_IRQ_EXT(x9)
1:      beq     x24, x22, 1b
        csrci   mstatus, MSTATUS_MIE
        LOGGED(1)
        lw      x14, 0(x22)
        li      x7, 0x8000000b
        bne     x14, x7, fail

        // ---------------------------------------------------------- MIE holds it off
        li      TESTNUM, 40
        mv      x22, x24
        li      x7, 0x888
        csrw    mie, x7
        li      x11, 1
        sw      x11, TB_IRQ_EXT(x9)             // pending and enabled in mie, MIE = 0
        li      x13, 100
1:      addi    x13, x13, -1
        bnez    x13, 1b
        LOGGED(0)                               // nothing taken
        csrr    x14, mip
        li      x7, MIP_MEIP
        bne     x14, x7, fail
        csrsi   mstatus, MSTATUS_MIE            // taken on the very next instruction
mie_next:
        nop
        nop
        csrci   mstatus, MSTATUS_MIE
        li      TESTNUM, 41
        LOGGED(1)
        lw      x14, 4(x22)
        la      x7, mie_next
        bne     x14, x7, fail

        // ---------------------------------------------------------- an mie bit holds it off
        li      TESTNUM, 50
        mv      x22, x24
        csrw    mie, x0
        csrsi   mstatus, MSTATUS_MIE
        li      x11, 1
        sw      x11, 0(x5)                      // msip pending, MIE = 1, mie.MSIE = 0
        li      x13, 100
1:      addi    x13, x13, -1
        bnez    x13, 1b
        LOGGED(0)
        li      x7, MIP_MSIP
        csrw    mie, x7                         // taken on the very next instruction
msie_next:
        nop
        nop
        csrci   mstatus, MSTATUS_MIE
        li      TESTNUM, 51
        LOGGED(1)
        lw      x14, 0(x22)
        li      x7, 0x80000003
        bne     x14, x7, fail
        lw      x14, 4(x22)
        la      x7, msie_next
        bne     x14, x7, fail

        // ---------------------------------------------------------- priorities
        li      TESTNUM, 60
        mv      x22, x24
        li      x7, 0x888
        csrw    mie, x7
        li      x11, 1
        sw      x11, 0(x5)                      // MSI
        sw      x11, TB_IRQ_EXT(x9)             // MEI
        sw      x0, 4(x6)                       // MTI
        sw      x0, 0(x6)
        csrsi   mstatus, MSTATUS_MIE
prio_next:
        nop
        nop
        nop
        csrci   mstatus, MSTATUS_MIE
        LOGGED(3)
        li      TESTNUM, 61
        lw      x14, 0(x22)
        li      x7, 0x8000000b
        bne     x14, x7, fail
        lw      x14, 8(x22)
        li      x7, 0x80000003
        bne     x14, x7, fail
        lw      x14, 16(x22)
        li      x7, 0x80000007
        bne     x14, x7, fail
        li      TESTNUM, 62                     // all three at the same instruction
        la      x7, prio_next
        lw      x14, 4(x22)
        bne     x14, x7, fail
        lw      x14, 12(x22)
        bne     x14, x7, fail
        lw      x14, 20(x22)
        bne     x14, x7, fail

        // ---------------------------------------------------------- the sweep
        li      TESTNUM, 70
        csrw    mie, x0
        la      x8, block_data
        jal     ra, block                       // reference run, no interrupt
        mv      x21, x15
        li      x7, MIP_MEIP
        csrw    mie, x7
        li      x20, 1                          // delay in cycles
        li      x4, 1                           // step: 1 up to 96, then 14 up to 1500
sweep:
        li      TESTNUM, 71
        mv      x22, x24
        sw      x0, TB_PROBE_SUM(x9)
        lw      x16, TB_PROBE_WR(x9)
        sw      x0, TB_PROBE_RD(x9)
        csrsi   mstatus, MSTATUS_MIE
        sw      x20, TB_IRQ_DELAY(x9)           // the interrupt comes x20 cycles later
        jal     ra, block
1:      beq     x24, x22, 1b                    // (if it has not come yet)
        csrci   mstatus, MSTATUS_MIE
        li      TESTNUM, 72                     // the block computed the same
        bne     x15, x21, sweep_fail
        li      TESTNUM, 73                     // exactly one interrupt
        addi    x7, x22, 8
        bne     x24, x7, sweep_fail
        li      TESTNUM, 74                     // every probe access exactly once
        lw      x14, TB_PROBE_RD(x9)            // the block read it twice
        li      x7, 2
        bne     x14, x7, sweep_fail
        lw      x14, TB_PROBE_WR(x9)
        sub     x14, x14, x16
        li      x7, 2
        bne     x14, x7, sweep_fail
        lw      x14, TB_PROBE_SUM(x9)
        li      x7, 0x1234 + 0x0001             // the two values the block writes
        bne     x14, x7, sweep_fail
        add     x20, x20, x4
        li      x7, 97
        blt     x20, x7, sweep
        li      x4, 14                          // longer delays reach further into the block
        li      x7, 1500                        // when the bus is slow
        blt     x20, x7, sweep
        j       pass

sweep_fail:
        mv      a0, x20
        j       fail

        // A block of mixed instructions; x15 = a checksum of what it computed. Uses x10..x19
        // except x16, x8 (data), x9 (TB_BASE); writes block_out (the same values every time).
block:
        lw      x10, 0(x8)
        lw      x11, 4(x8)
        add     x15, x10, x11
        lw      x12, TB_PROBE_RD(x9)            // MMIO read with a side effect
        mul     x13, x10, x11
        xor     x15, x15, x13
        li      x14, 0x1234
        sw      x14, TB_PROBE_WR(x9)            // MMIO write
        div     x13, x13, x11
        add     x15, x15, x13
        la      x17, block_out
        sw      x15, 0(x17)
        lb      x18, 8(x8)
        add     x15, x15, x18
        csrw    mscratch, x15
        bltz    x18, 1f
        addi    x15, x15, 7                     // not reached (x18 < 0)
1:      lw      x12, TB_PROBE_RD(x9)
        csrr    x19, mscratch
        sub     x15, x15, x19
        sw      x0, 4(x17)
        lw      x13, 0(x17)
        add     x15, x15, x13
        li      x14, 1
        sw      x14, TB_PROBE_WR(x9)
        remu    x13, x13, x10
        add     x15, x15, x13
        sh      x15, 6(x17)
        lhu     x13, 6(x17)
        add     x15, x15, x13
        ret

        .balign 4
irq_handler:
        bnez    t4, fail                        // mtval = 0
        csrr    s10, mstatus                    // MIE = 0, MPIE = 1
        andi    s10, s10, 0x88
        xori    s10, s10, 0x80
        bnez    s10, fail
        sw      t5, 0(x24)                      // log mcause and mepc
        sw      t6, 4(x24)
        addi    x24, x24, 8
        li      s10, 0x8000000b
        beq     t5, s10, 1f
        li      s10, 0x80000003
        beq     t5, s10, 2f
        li      s10, 0x80000007
        beq     t5, s10, 3f
        j       fail
1:      li      s10, TB_BASE                    // external: drop the level
        sw      x0, TB_IRQ_EXT(s10)
        mret
2:      li      s10, CLINT_MSIP                 // software: clear msip
        sw      x0, 0(s10)
        mret
3:      li      s10, CLINT_MTIMECMP             // timer: mtimecmp = never
        li      x23, -1
        sw      x23, 4(s10)
        sw      x23, 0(s10)
        mret

        .data
        .balign 4
block_data:
        .word   0x00012345, 0x00000067
        .byte   0x9c, 0, 0, 0
block_out:
        .word   0, 0
irq_log:
        .space  4096

TEST_DATA
