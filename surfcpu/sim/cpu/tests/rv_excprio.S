/*
SurfCPU directed test: exception priority on loads and stores (review)
--------------------
File: rv_excprio.S Date: 10/15/26 (SurfCPU)
--------------------
Reviewer test (Spike and the core): exception priority on loads and stores. A misaligned
access to an address where nothing answers must raise the misaligned exception (cause 4/6,
mtval = the address) and never reach the bus; an aligned one there raises the access fault
(cause 5/7). An illegal instruction in a fetch-faulting location is covered by the formal
harness; here also ECALL/EBREAK mtval (0 / the pc, as ISA.md says).
*/
#include "surf_test.h"

TEST_BEGIN
test_main:
        li      x20, UNMAPPED_ADDR
        TRAP_EXPECT(1, 4, lw x5, 1(x20))
        li      TESTNUM, 2
        addi    x7, x20, 1
        bne     t4, x7, fail
        TRAP_EXPECT(3, 4, lh x5, 3(x20))
        TRAP_EXPECT(4, 6, sw x5, 2(x20))
        li      TESTNUM, 5
        addi    x7, x20, 2
        bne     t4, x7, fail
        TRAP_EXPECT(6, 6, sh x5, 1(x20))
        TRAP_EXPECT(7, 5, lw x5, 4(x20))
        li      TESTNUM, 8
        addi    x7, x20, 4
        bne     t4, x7, fail
        TRAP_EXPECT(9, 5, lb x5, 3(x20))
        TRAP_EXPECT(10, 7, sb x5, 1(x20))
        li      TESTNUM, 11
        addi    x7, x20, 1
        bne     t4, x7, fail
        TRAP_EXPECT(12, 11, ecall)
        li      TESTNUM, 13
        bnez    t4, fail
        TRAP_EXPECT(14, 3, ebreak)
        li      TESTNUM, 15
        la      x7, 97b
        bne     t4, x7, fail
        j       pass
TEST_DATA
