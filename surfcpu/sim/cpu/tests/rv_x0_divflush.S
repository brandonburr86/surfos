/*
SurfCPU directed test: x0 at every forwarding distance; an abandoned divide (review)
--------------------
File: rv_x0_divflush.S Date: 10/15/26 (SurfCPU)
--------------------
Reviewer test (runs on Spike and the core): two gaps the directed suite leaves open.
 1. x0 written by every kind of instruction, read back at forwarding distance 1, 2 and 3
    (MEM forward, WB forward, ID bypass): a decoder that set rd_wen for rd = x0 on one
    opcode would forward that value and is otherwise unseen (mutant x0_lui survived the
    directed tests and riscv-tests).
 2. A divide abandoned by a flush (an older ECALL traps in MEM while the DIV is in EX) and
    a trap handler whose first instruction is another divide: the handler's DIV must not
    get the abandoned divide's result (mutant divclear survived everything).
*/
#include "surf_test.h"

#define X0_USE(num, ...)                                                \
        li      TESTNUM, num;                                           \
        __VA_ARGS__;                                                    \
        add     x5, x0, x0;                                             \
        add     x6, x0, x0;                                             \
        add     x7, x0, x0;                                             \
        or      x5, x5, x6;                                             \
        or      x5, x5, x7;                                             \
        bnez    x5, fail;                                               \
        __VA_ARGS__;                                                    \
        nop;                                                            \
        add     x6, x0, x0;                                             \
        bnez    x6, fail;                                               \
        __VA_ARGS__;                                                    \
        nop;                                                            \
        nop;                                                            \
        add     x7, x0, x0;                                             \
        bnez    x7, fail;

TEST_BEGIN
test_main:
        la      x20, data
        li      x8, 0x1234
        li      x9, 7
        X0_USE(1, lui x0, 0x12345)
        X0_USE(2, auipc x0, 0x12345)
        X0_USE(3, addi x0, x8, 5)
        X0_USE(4, add x0, x8, x9)
        X0_USE(5, slli x0, x8, 3)
        X0_USE(6, lw x0, 0(x20))
        X0_USE(7, csrr x0, mscratch)
        X0_USE(8, csrrw x0, mscratch, x8)
        X0_USE(9, mul x0, x8, x9)
        X0_USE(10, div x0, x8, x9)
        X0_USE(11, jal x0, 1f; 1:)
        X0_USE(12, auipc x10, 0; jalr x0, 8(x10))

        // ------------------------------------------------ divide abandoned by a flush
        li      TESTNUM, 20
        la      x5, div_handler
        csrrw   x21, mtvec, x5
        li      x8, 100                         // handler: 100 / 7 = 14
        li      x9, 7
        li      x10, 1000                       // abandoned: 1000 / 3 = 333
        li      x11, 3
        li      x12, 0
        ecall
        div     x13, x10, x11                   // in EX when the ECALL traps in MEM
        div     x13, x10, x11
        div     x13, x10, x11
        csrw    mtvec, x21
        li      TESTNUM, 21
        li      x7, 14
        bne     x12, x7, fail                   // the handler's divide
        li      TESTNUM, 22
        li      x7, 333
        bne     x13, x7, fail                   // and the main-line divides afterwards
        // the same with a remainder and a longer pipeline fill (a load before the ECALL)
        li      TESTNUM, 23
        la      x5, rem_handler
        csrw    mtvec, x5
        lw      x14, 0(x20)
        ecall
        rem     x13, x10, x11
        csrw    mtvec, x21
        li      x7, 2                           // 100 % 7
        bne     x12, x7, fail
        li      x7, 1                           // 1000 % 3
        bne     x13, x7, fail
        j       pass

        .balign 4
div_handler:
        div     x12, x8, x9                     // the handler's first instruction
        csrr    s10, mepc
        addi    s10, s10, 4
        csrw    mepc, s10
        mret
        .balign 4
rem_handler:
        rem     x12, x8, x9
        csrr    s10, mepc
        addi    s10, s10, 4
        csrw    mepc, s10
        mret

        .data
        .balign 16
data:   .word   0x5555aaaa, 0, 0, 0
TEST_DATA
