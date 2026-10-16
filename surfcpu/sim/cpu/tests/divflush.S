/*
SurfCPU directed test: a divide abandoned by a trap
--------------------
File: divflush.S Date: 10/15/26 (SurfCPU)
--------------------
A DIV that is in EX when an older instruction traps in MEM is abandoned, and the trap
handler's first instruction is another divide: it must compute its own result, not hand
over the abandoned one's. For the abandoned divide to be in EX at the trap whatever the
bus timing, both instructions are parked behind a WFI that waits for the timer (MIE = 0,
so the timer only wakes it): while the WFI waits in MEM, the trapping instruction reaches
EX and the DIV the head of ID; when the WFI completes, the trapping instruction enters MEM
and traps in the very cycle the DIV enters EX with its operands ready. Three cases: ECALL
then DIV, an illegal instruction then REM, EBREAK then DIVU (rv_x0_divflush.S has the same
idea without the parking, so it depends on the fetch timing).
*/
#include "surf_test.h"

// wake the parked WFI about 3000 cycles from now (mie.MTIE set, mstatus.MIE clear)
#define ARM_TIMER                                                       \
        li      x7, CLINT_MTIME;                                        \
        lw      x6, 0(x7);                                              \
        li      x7, 3000;                                               \
        add     x6, x6, x7;                                             \
        li      x7, CLINT_MTIMECMP;                                     \
        sw      x0, 4(x7);                                              \
        sw      x6, 0(x7);

#define DISARM_TIMER                                                    \
        li      x7, CLINT_MTIMECMP;                                     \
        li      x6, -1;                                                 \
        sw      x6, 4(x7);                                              \
        sw      x6, 0(x7);

TEST_BEGIN
test_main:
        csrr    x21, mtvec
        li      x7, MIP_MTIP
        csrw    mie, x7
        li      x8, 100                         // the handlers' divides: 100 and 7
        li      x9, 7
        li      x10, 1000                       // the abandoned divides: 1000 and 3
        li      x11, 3

        // ---------------------------------------------------------- ECALL, then DIV
        li      TESTNUM, 1
        la      x5, h_div
        csrw    mtvec, x5
        li      x12, 0
        li      x13, 0
        ARM_TIMER
        wfi
        ecall                                   // in EX while the WFI waits; traps on arrival
        div     x13, x10, x11                   // at the head of ID; in EX at the trap
        DISARM_TIMER
        li      TESTNUM, 2
        li      x7, 14                          // 100 / 7: the handler's own result
        bne     x12, x7, fail
        li      TESTNUM, 3                      // and the divide itself, re-run after the
        li      x7, 333                         // handler returned to it: 1000 / 3
        bne     x13, x7, fail

        // ---------------------------------------------------------- illegal, then REM
        li      TESTNUM, 4
        la      x5, h_rem
        csrw    mtvec, x5
        li      x12, 0
        li      x13, 0
        ARM_TIMER
        wfi
        .word   0                               // illegal
        rem     x13, x10, x11
        DISARM_TIMER
        li      TESTNUM, 5
        li      x7, 2                           // 100 % 7
        bne     x12, x7, fail
        li      TESTNUM, 6
        li      x7, 1                           // 1000 % 3
        bne     x13, x7, fail

        // ---------------------------------------------------------- EBREAK, then DIVU
        li      TESTNUM, 7
        la      x5, h_divu
        csrw    mtvec, x5
        li      x12, 0
        li      x13, 0
        li      x10, -1000                      // as unsigned: 0xfffffc18
        ARM_TIMER
        wfi
        ebreak
        divu    x13, x10, x11
        DISARM_TIMER
        li      TESTNUM, 8
        li      x7, 14                          // 100 / 7
        bne     x12, x7, fail
        li      TESTNUM, 9
        li      x7, 0x55555408                  // 0xfffffc18 / 3
        bne     x13, x7, fail

        csrw    mtvec, x21
        csrw    mie, x0
        j       pass

        // Handlers whose first instruction is a divide; they skip the trapping instruction.
        .balign 4
h_div:  div     x12, x8, x9
        j       skip
        .balign 4
h_rem:  rem     x12, x8, x9
        j       skip
        .balign 4
h_divu: divu    x12, x8, x9
skip:   csrr    s10, mepc
        addi    s10, s10, 4
        csrw    mepc, s10
        mret

TEST_DATA
