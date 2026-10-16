/*
SurfCPU directed test: interrupts enabled just before a WFI (review)
--------------------
File: rv_irq_wfi.S Date: 10/15/26 (SurfCPU)
--------------------
Reviewer test: an interrupt that is pending and enabled in mie when a CSR write (or MRET)
sets mstatus.MIE must be taken immediately after that instruction (privileged spec 3.1.9:
the trap condition "must also be evaluated immediately following the execution of an xRET
instruction or an explicit write to a CSR on which these interrupt trap conditions
expressly depend"), i.e. before the next instruction executes, whatever it is. Uses only
the CLINT msip (Spike has the same CLINT), so it runs on Spike and on the core.
  test 1..3: next instruction is a NOP          -> mepc = that NOP          (sanity)
  test 4..6: next instruction is WFI            -> mepc = the WFI
  test 7..9: MRET returns to a WFI, MPIE = 1    -> mepc = the WFI
*/
#define TEST_IRQ_HANDLER
#include "surf_test.h"

TEST_BEGIN
test_main:
        li      x5, CLINT_MSIP
        li      x6, 1
        li      x7, MIP_MSIP
        csrw    mie, x7

        // ------------------------------------------------ csrsi MIE; nop
        li      TESTNUM, 1
        li      x20, 0                          // interrupts seen by irq_handler
        sw      x6, 0(x5)                       // msip = 1: pending and enabled in mie
        csrsi   mstatus, MSTATUS_MIE
n1:     nop
        csrci   mstatus, MSTATUS_MIE
        li      TESTNUM, 2
        li      x7, 1
        bne     x20, x7, fail
        li      TESTNUM, 3
        la      x7, n1
        bne     x21, x7, fail                   // mepc

#ifndef SKIP_W1
        // ------------------------------------------------ csrsi MIE; wfi
        li      TESTNUM, 4
        li      x20, 0
        sw      x6, 0(x5)
        csrsi   mstatus, MSTATUS_MIE
w1:     wfi
        nop
        csrci   mstatus, MSTATUS_MIE
        li      TESTNUM, 5
        li      x7, 1
        bne     x20, x7, fail
        li      TESTNUM, 6
        la      x7, w1
        bne     x21, x7, fail                   // mepc = the WFI

#endif
        // ------------------------------------------------ MRET to a WFI with MPIE = 1
        li      TESTNUM, 7
        li      x20, 0
        la      x7, w2
        csrw    mepc, x7
        li      x7, MSTATUS_MPIE
        csrs    mstatus, x7                     // MPIE = 1, MIE = 0
        sw      x6, 0(x5)                       // pending
        mret
        j       fail
w2:     wfi
        nop
        csrci   mstatus, MSTATUS_MIE
        li      TESTNUM, 8
        li      x7, 1
        bne     x20, x7, fail
        li      TESTNUM, 9
        la      x7, w2
        bne     x21, x7, fail                   // mepc = the WFI
        j       pass

        .balign 4
irq_handler:
        addi    x20, x20, 1
        csrr    x21, mepc
        li      s10, CLINT_MSIP
        sw      x0, 0(s10)                      // clear msip
        lw      s10, 0(x21)                     // interrupted at a WFI: skip it (nothing
        li      s9, 0x10500073                  // would wake it again)
        bne     s10, s9, 1f
        addi    s10, x21, 4
        csrw    mepc, s10
1:      mret

TEST_DATA
