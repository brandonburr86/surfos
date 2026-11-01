/*
SurfCPU directed test: control flow, forwarding and trap corners (review)
--------------------
File: rv_ctrl.S Date: 10/15/26 (SurfCPU)
--------------------
Reviewer test: control-flow, forwarding and trap corners (runs on Spike and the core).
*/
#include "surf_test.h"

TEST_BEGIN
test_main:
        la      x20, data

        // JALR clears bit 0 of the target; no trap; rd = pc + 4
        li      TESTNUM, 1
        la      x5, 1f
        addi    x5, x5, 1
        jalr    x6, 0(x5)
        j       fail
1:      la      x7, 1b
        addi    x7, x7, -4
        bne     x6, x7, fail
        CHECK_NO_TRAP(2, x0)

        // JALR to a target with bit 1 set: misaligned, mtval = target, rd unchanged
        li      x6, 0x5a5a
        la      x5, 2f
        addi    x5, x5, 2
        TRAP_EXPECT(3, 0, jalr x6, 0(x5))
        li      TESTNUM, 4
        la      x7, 2f
        addi    x7, x7, 2
        bne     t4, x7, fail
        li      x7, 0x5a5a
        bne     x6, x7, fail
        // the same with bit 0 also set (target+3 -> +2 after clearing bit 0)
        la      x5, 2f
        addi    x5, x5, 3
        TRAP_EXPECT(5, 0, jalr x6, 0(x5))
        li      TESTNUM, 6
        la      x7, 2f
        addi    x7, x7, 2
        bne     t4, x7, fail
        // JALR with rd == rs1, misaligned: rs1 keeps its value
        la      x5, 2f
        addi    x5, x5, 2
        mv      x8, x5
        TRAP_EXPECT(7, 0, jalr x5, 0(x5))
        CHECK_REG(8, x5, x8)
2:      nop
        nop

        // a taken branch to pc+6 (encoded by hand): misaligned, mtval = target
        li      x5, 1
        li      x6, 1
        TRAP_EXPECT(10, 0, .word 0x00628363)      // beq x5, x6, .+6
        li      TESTNUM, 11
        la      x7, 97b
        addi    x7, x7, 6
        bne     t4, x7, fail
        // not taken: no trap
        mv      x9, t3
        li      x6, 2
        .word   0x00628363                        // beq x5, x6, .+6 (not taken)
        CHECK_NO_TRAP(12, x9)
        // JAL to pc+6 (encoded by hand): misaligned, rd not written
        li      x1, 0x1234
        TRAP_EXPECT(13, 0, .word 0x006000ef)      // jal x1, .+6
        li      TESTNUM, 14
        la      x7, 97b
        addi    x7, x7, 6
        bne     t4, x7, fail
        li      x7, 0x1234
        bne     x1, x7, fail

        // load-use into a branch, a JALR, store data, a store address
        li      TESTNUM, 20
        la      x5, target_word
        sw      x5, 0(x20)
        li      x6, 77
        sw      x6, 4(x20)
        lw      x7, 4(x20)
        beq     x7, x6, 3f
        j       fail
3:      lw      x7, 4(x20)
        bne     x7, x6, fail
        li      TESTNUM, 21
        lw      x8, 0(x20)
        jalr    x9, 0(x8)
        j       fail
target_word:
        li      TESTNUM, 22
        lw      x7, 4(x20)
        sw      x7, 8(x20)                        // load -> store data
        lw      x10, 8(x20)
        bne     x10, x6, fail
        li      TESTNUM, 23
        addi    x11, x20, 12
        sw      x11, 16(x20)
        lw      x12, 16(x20)
        sw      x6, 0(x12)                        // load -> store address
        lw      x13, 12(x20)
        bne     x13, x6, fail

        // store then load the same address, every width, back to back
        li      TESTNUM, 30
        li      x5, 0x11223344
        sw      x5, 20(x20)
        lw      x6, 20(x20)
        bne     x6, x5, fail
        li      x7, 0xab
        sb      x7, 21(x20)
        lw      x6, 20(x20)
        li      x8, 0x1122ab44
        bne     x6, x8, fail
        lb      x6, 21(x20)
        li      x8, 0xffffffab
        bne     x6, x8, fail
        li      x7, 0x8765
        sh      x7, 22(x20)
        lh      x6, 22(x20)
        li      x8, 0xffff8765
        bne     x6, x8, fail
        lhu     x6, 22(x20)
        li      x8, 0x8765
        bne     x6, x8, fail
        // load then store the same address then load
        li      TESTNUM, 31
        lw      x6, 20(x20)
        sw      x0, 20(x20)
        lw      x9, 20(x20)
        bnez    x9, fail
        li      x8, 0x8765ab44
        bne     x6, x8, fail

        // CSR read into a branch and a JALR; MUL into JALR; DIV into a branch
        li      TESTNUM, 40
        la      x5, 4f
        csrw    mscratch, x5
        csrr    x6, mscratch
        jalr    x0, 0(x6)
        j       fail
4:      csrrw   x6, mscratch, x0
        bne     x6, x5, fail
        li      TESTNUM, 41
        la      x5, 5f
        srli    x6, x5, 2
        li      x7, 4
        mul     x8, x6, x7
        jalr    x0, 0(x8)
        j       fail
5:      li      TESTNUM, 42
        li      x5, 100
        li      x6, 7
        div     x7, x5, x6
        li      x8, 14
        bne     x7, x8, fail
        rem     x7, x5, x6
        li      x8, 2
        bne     x7, x8, fail
        // DIV/REM by zero and the overflow case, results used at once
        li      TESTNUM, 43
        li      x5, -7
        div     x7, x5, x0
        addi    x7, x7, 1
        bnez    x7, fail
        rem     x7, x5, x0
        bne     x7, x5, fail
        divu    x7, x5, x0
        addi    x7, x7, 1
        bnez    x7, fail
        remu    x7, x5, x0
        bne     x7, x5, fail
        li      TESTNUM, 44
        li      x5, 0x80000000
        li      x6, -1
        div     x7, x5, x6
        bne     x7, x5, fail
        rem     x7, x5, x6
        bnez    x7, fail
        // MULH* signs
        li      TESTNUM, 45
        li      x5, -2
        li      x6, 3
        mulh    x7, x5, x6
        li      x8, -1
        bne     x7, x8, fail
        mulhsu  x7, x5, x6
        bne     x7, x8, fail
        mulhu   x7, x5, x6
        li      x8, 2
        bne     x7, x8, fail
        mulhsu  x7, x6, x5                        // 3 * 0xfffffffe (unsigned)
        li      x8, 2
        bne     x7, x8, fail
        li      x5, 0x80000000
        mulh    x7, x5, x5
        li      x8, 0x40000000
        bne     x7, x8, fail

        // x0 is never written: by an ALU op, a load, a CSR read, a JAL(R), a MUL, a DIV
        li      TESTNUM, 50
        addi    x0, x0, 5
        lw      x0, 4(x20)
        csrr    x0, mscratch
        jal     x0, 6f
6:      mul     x0, x5, x5
        div     x0, x5, x6
        bnez    x0, fail
        add     x5, x0, x0
        bnez    x5, fail
        j       pass

        .data
        .balign 16
data:   .space  64
TEST_DATA
