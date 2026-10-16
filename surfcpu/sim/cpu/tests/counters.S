/*
SurfCPU directed test: counters
--------------------
File: counters.S Date: 10/15/26 (SurfCPU)
--------------------
minstret counts exactly the retired instructions: one per instruction, none for an
instruction that traps (ECALL), every instruction of the handler; a write to minstret
replaces the value and is not itself counted; instret shadows minstret; the carry from
minstret into minstreth. mcycle runs, can be written (both halves) and carries;
mcountinhibit's IR and CY bits stop the counters, and a write to a stopped counter sets
exactly the value written; the 64-bit hi/lo/hi read sequence; time follows the timer's
mtime; hpmcounters read 0.
*/
#include "surf_test.h"

TEST_BEGIN
test_main:
        li      TESTNUM, 1                      // back-to-back reads differ by one
        csrr    x11, minstret
        csrr    x12, minstret
        sub     x13, x12, x11
        li      x7, 1
        bne     x13, x7, fail
        li      TESTNUM, 2                      // three instructions between two reads
        csrr    x11, minstret
        nop
        addi    x5, x5, 1
        nop
        csrr    x12, minstret
        sub     x13, x12, x11
        li      x7, 4
        bne     x13, x7, fail
        li      TESTNUM, 3                      // a taken branch and a load count once each
        la      x6, a_word
        csrr    x11, minstret
        beq     x0, x0, 1f
        nop
        nop
1:      lw      x5, 0(x6)
        csrr    x12, minstret
        sub     x13, x12, x11
        li      x7, 3
        bne     x13, x7, fail
        li      TESTNUM, 4                      // instret shadows minstret
        csrr    x11, minstret
        csrr    x12, instret
        sub     x13, x12, x11
        li      x7, 1
        bne     x13, x7, fail

        li      TESTNUM, 5                      // a write replaces the value, uncounted
        li      x11, 1000
        csrw    minstret, x11
        csrr    x12, minstret                   // 1000
        csrr    x13, minstret                   // 1001
        bne     x12, x11, fail
        addi    x11, x11, 1
        bne     x13, x11, fail
        li      TESTNUM, 6                      // the high half, and the carry into it
        csrw    minstreth, x0
        li      x11, -3
        csrw    minstret, x11                   // -3, then the next instructions count up
        nop                                     // -3 -> -2
        nop                                     // -> -1
        nop                                     // -> 0, carry
        csrr    x12, minstreth                  // 1 (and minstret 0 -> 1 as it retires)
        csrr    x13, minstret                   // 1
        li      x7, 1
        bne     x12, x7, fail
        bne     x13, x7, fail
        li      TESTNUM, 7
        li      x11, 0x12345678
        csrw    minstreth, x11
        csrr    x12, minstreth
        bne     x12, x11, fail
        csrr    x12, instreth
        bne     x12, x11, fail

        li      TESTNUM, 8                      // a trapping instruction is not counted
        la      x5, count_handler
        csrrw   x20, mtvec, x5
        csrr    x11, minstret
        ecall                                   // not counted; the 4-instruction handler is
        csrr    x12, minstret
        csrw    mtvec, x20
        sub     x13, x12, x11
        li      x7, 5
        bne     x13, x7, fail

        li      TESTNUM, 10                     // mcountinhibit.IR stops minstret
        csrwi   mcountinhibit, 4
        csrr    x11, minstret
        nop
        nop
        csrr    x12, minstret
        csrwi   mcountinhibit, 0
        bne     x11, x12, fail
        csrr    x13, minstret                   // and it runs again
        beq     x13, x12, fail

        li      TESTNUM, 11                     // mcycle runs
        csrr    x11, mcycle
        nop
        csrr    x12, mcycle
        bgeu    x11, x12, fail
        li      TESTNUM, 12                     // mcountinhibit.CY stops it
        csrwi   mcountinhibit, 1
        csrr    x11, mcycle
        nop
        nop
        nop
        csrr    x12, mcycle
        csrwi   mcountinhibit, 0
        bne     x11, x12, fail
        csrr    x13, cycle
        beq     x13, x12, fail
        li      TESTNUM, 13                     // a write to mcycle: it continues from there
        li      x11, 0x00100000
        csrw    mcycle, x11
        csrr    x12, mcycle
        bltu    x12, x11, fail
        sub     x13, x12, x11
        li      x7, 1000
        bgeu    x13, x7, fail
        li      TESTNUM, 14                     // mcycleh, and the carry into it
        csrw    mcycle, x0
        li      x11, 0x00000abc
        csrw    mcycleh, x11
        csrr    x12, mcycleh
        bne     x12, x11, fail
        li      x11, -16
        csrw    mcycle, x11
        li      x13, 40
1:      addi    x13, x13, -1
        bnez    x13, 1b
        csrr    x12, mcycleh
        li      x7, 0x00000abd
        bne     x12, x7, fail
        csrr    x12, cycleh
        bne     x12, x7, fail
        li      TESTNUM, 15                     // the hi/lo/hi read of a 64-bit counter
1:      csrr    x11, cycleh
        csrr    x12, cycle
        csrr    x13, cycleh
        bne     x11, x13, 1b
        li      x7, 0x00000abd
        bltu    x11, x7, fail

        li      TESTNUM, 16                     // counters stopped: a write sets exactly the
        csrwi   mcountinhibit, 5                // value written, half by half
        li      x11, 0x00012345
        csrw    mcycle, x11
        csrr    x12, mcycle
        bne     x12, x11, fail
        li      x13, 0x00000abc
        csrw    mcycleh, x13
        csrr    x12, mcycleh
        bne     x12, x13, fail
        csrr    x12, mcycle                     // (the low half is untouched)
        bne     x12, x11, fail
        li      TESTNUM, 17
        li      x11, 0x00054321
        csrw    minstret, x11
        csrr    x12, minstret
        bne     x12, x11, fail
        li      x13, 0x00000def
        csrw    minstreth, x13
        csrr    x12, minstreth
        bne     x12, x13, fail
        csrr    x12, minstret
        bne     x12, x11, fail
        csrwi   mcountinhibit, 0

        li      TESTNUM, 20                     // time follows the timer's mtime
1:      csrr    x11, timeh
        csrr    x12, time
        csrr    x13, timeh
        bne     x11, x13, 1b
        li      x5, CLINT_MTIME
        lw      x14, 0(x5)
        lw      x15, 4(x5)
        bltu    x15, x11, fail                  // mtime is not behind time
        bne     x15, x11, 2f
        bltu    x14, x12, fail
        sub     x16, x14, x12
        li      x7, 100000
        bgeu    x16, x7, fail
2:      li      TESTNUM, 21                     // hpmcounters read 0
        csrr    x11, hpmcounter3
        csrr    x12, hpmcounter17h
        csrr    x13, mhpmcounter31
        or      x11, x11, x12
        or      x11, x11, x13
        bnez    x11, fail
        j       pass

        .balign 4
a_word: .word   0x1234
count_handler:
        csrr    x21, mepc
        addi    x21, x21, 4
        csrw    mepc, x21
        mret

TEST_DATA
