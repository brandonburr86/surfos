/*
SurfCPU directed test: pipeline hazards beyond the matrix
--------------------
File: hazards.S Date: 10/15/26 (SurfCPU)
--------------------
The cases hazards_matrix.S (gen_hazards.py) does not cover: two writes to one register
(the younger wins, whether it is a load, an ALU result or a CSR read), the two operands
coming from different producers at different distances, a producer that retires while
its consumer waits in EX behind a slow load or store (the operand registers are refreshed;
run with long data latencies), link values feeding stores and returns, and long chains
where every instruction depends on the one before.
*/
#include "surf_test.h"

TEST_BEGIN
test_main:
        la      x5, words                       // words: 0x77, 0x88, 0x99, scratch...

        // ---------------------------------------------------------- write after write
        li      TESTNUM, 1
        addi    x13, x0, 1
        addi    x13, x0, 2
        addi    x14, x13, 0
        li      x7, 2
        bne     x14, x7, fail
        li      TESTNUM, 2                      // the younger load wins over the ALU write
        addi    x13, x0, 5
        lw      x13, 0(x5)
        add     x14, x13, x0
        li      x7, 0x77
        bne     x14, x7, fail
        li      TESTNUM, 3                      // the younger ALU write wins over the load
        lw      x13, 0(x5)
        addi    x13, x0, 5
        add     x14, x13, x0
        li      x7, 5
        bne     x14, x7, fail
        li      TESTNUM, 4                      // two loads
        lw      x13, 0(x5)
        lw      x13, 4(x5)
        add     x14, x13, x0
        li      x7, 0x88
        bne     x14, x7, fail
        li      TESTNUM, 5                      // CSR read, then an ALU write
        li      x11, 0x123
        csrw    mscratch, x11
        csrr    x13, mscratch
        addi    x13, x13, 1
        add     x14, x13, x0
        li      x7, 0x124
        bne     x14, x7, fail
        li      TESTNUM, 6                      // ALU write, then a CSR read
        addi    x13, x0, 9
        csrr    x13, mscratch
        add     x14, x13, x0
        li      x7, 0x123
        bne     x14, x7, fail

        // ---------------------------------------------------------- operands from two producers
        li      TESTNUM, 10                     // rs1 two back (WB), rs2 a load just before
        addi    x13, x0, 0x10
        lw      x12, 0(x5)
        add     x14, x13, x12
        li      x7, 0x87
        bne     x14, x7, fail
        li      TESTNUM, 11                     // rs1 a load two back, rs2 just before
        lw      x13, 4(x5)
        addi    x12, x0, 0x20
        add     x14, x13, x12
        li      x7, 0xa8
        bne     x14, x7, fail
        li      TESTNUM, 12                     // both loads, back to back
        lw      x13, 0(x5)
        lw      x12, 8(x5)
        sub     x14, x12, x13
        li      x7, 0x22
        bne     x14, x7, fail
        li      TESTNUM, 13                     // a store whose address and data both just changed
        addi    x6, x5, 12
        addi    x13, x0, 0x3c
        sw      x13, 0(x6)
        lw      x14, 12(x5)
        bne     x14, x13, fail

        // ---------------------------------------------------------- producer retires while waiting
        // (with long data latencies the consumer waits in EX behind the access)
        li      TESTNUM, 20
        addi    x13, x0, 7
        lw      x20, 0(x5)
        add     x14, x13, x13
        li      x7, 14
        bne     x14, x7, fail
        li      TESTNUM, 21
        addi    x13, x0, 8
        sw      x0, 16(x5)
        sw      x0, 20(x5)
        add     x14, x13, x13
        li      x7, 16
        bne     x14, x7, fail
        li      TESTNUM, 22
        addi    x13, x0, 9
        lw      x20, 0(x5)
        lw      x21, 4(x5)
        sw      x13, 16(x5)                     // store data from the old producer
        lw      x14, 16(x5)
        li      x7, 9
        bne     x14, x7, fail
        li      TESTNUM, 23
        addi    x13, x0, 10
        sw      x0, 20(x5)
        beq     x13, x0, fail                   // branch on the old producer
        li      x7, 10
        bne     x13, x7, fail
        li      TESTNUM, 24                     // and a divide waiting behind a store
        li      x11, 100
        addi    x12, x0, 7
        sw      x0, 20(x5)
        div     x14, x11, x12
        li      x7, 14
        bne     x14, x7, fail

        // ---------------------------------------------------------- link values
        li      TESTNUM, 30                     // link stored at once
        la      x7, 1f
        jal     x13, 1f
1:      sw      x13, 16(x5)
        lw      x14, 16(x5)
        bne     x14, x7, fail
        li      TESTNUM, 31                     // return through a link just made
        jal     x13, 2f
        j       3f
2:      jalr    x0, 0(x13)
        j       fail
3:      li      TESTNUM, 32                     // return address loaded just before
        la      x13, 4f
        sw      x13, 16(x5)
        lw      x12, 16(x5)
        jalr    x0, 0(x12)
        j       fail
4:

        // ---------------------------------------------------------- dependent chains
        li      TESTNUM, 40                     // every instruction uses the previous result
        li      x13, 1
        addi    x13, x13, 2                     // 3
        slli    x13, x13, 3                     // 24
        xori    x13, x13, 0x5                   // 29
        mul     x13, x13, x13                   // 841
        addi    x13, x13, -41                   // 800
        srli    x13, x13, 5                     // 25
        sw      x13, 16(x5)
        lw      x13, 16(x5)                     // 25
        add     x13, x13, x13                   // 50
        divu    x13, x13, x13                   // 1
        sub     x13, x0, x13                    // -1
        sra     x13, x13, x13                   // -1 >> 31 = -1
        sltu    x13, x0, x13                    // 1
        li      x7, 1
        bne     x13, x7, fail
        li      TESTNUM, 41                     // a load chain through the stores just made
        addi    x13, x5, 24
        sw      x13, 28(x5)                     // words[7] = &words[6]
        addi    x12, x0, 0x5e
        sw      x12, 24(x5)                     // words[6] = 0x5e
        lw      x13, 28(x5)
        lw      x13, 0(x13)
        addi    x13, x13, 1
        li      x7, 0x5f
        bne     x13, x7, fail
        j       pass

        .data
        .balign 4
words:  .word   0x77, 0x88, 0x99, 0, 0, 0, 0, 0

TEST_DATA
