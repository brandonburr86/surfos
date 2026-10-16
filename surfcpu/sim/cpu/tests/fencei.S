/*
SurfCPU directed test: FENCE.I and self-modifying code
--------------------
File: fencei.S Date: 10/15/26 (SurfCPU)
--------------------
A store to an instruction, then FENCE.I, then that instruction: the new one executes,
whether it is the very next instruction (already fetched when the store went out), a
few instructions further (already in the fetch buffer), or far away. Twenty rounds of
patching the same instruction in a loop; patches written with SB and SH (part of the
word); a patch whose store waits long on the bus (data latency) before FENCE.I.
*/
#include "surf_test.h"

#define ADDI_X13(imm) (((imm) << 20) | 0x00000693)     // addi x13, x0, imm

TEST_BEGIN
test_main:
        li      TESTNUM, 1                      // the instruction right after FENCE.I
        la      x5, patch1
        li      x6, ADDI_X13(42)
        sw      x6, 0(x5)
        fence.i
patch1: addi    x13, x0, 1
        li      x7, 42
        bne     x13, x7, fail

        li      TESTNUM, 2                      // three instructions further on
        la      x5, patch2
        li      x6, ADDI_X13(43)
        sw      x6, 0(x5)
        fence.i
        nop
        nop
patch2: addi    x13, x0, 2
        li      x7, 43
        bne     x13, x7, fail

        li      TESTNUM, 3                      // far away, reached by a call
        la      x5, far_patch
        li      x6, ADDI_X13(44)
        sw      x6, 0(x5)
        fence.i
        jal     ra, far_code
        li      x7, 44
        bne     x13, x7, fail

        li      TESTNUM, 4                      // twenty rounds on one instruction
        la      x5, patch4
        li      x20, 0
        li      x21, 20
1:      slli    x6, x20, 20
        li      x7, 0x693
        or      x6, x6, x7
        sw      x6, 0(x5)
        fence.i
patch4: addi    x13, x0, 0x7ff
        bne     x13, x20, fail
        addi    x20, x20, 1
        bne     x20, x21, 1b

        li      TESTNUM, 5                      // a byte of the instruction (imm[11:4])
        la      x5, patch5
        li      x6, 0x12
        sb      x6, 3(x5)                       // addi x13, x0, 0x055 -> 0x125
        fence.i
patch5: addi    x13, x0, 0x055
        li      x7, 0x125
        bne     x13, x7, fail
        li      TESTNUM, 6                      // a halfword (imm and rs1 bits)
        la      x5, patch6
        li      x6, 0x0330                      // bits 31:16: imm = 0x033, rs1 = x0
        sh      x6, 2(x5)
        fence.i
patch6: addi    x13, x0, 0x7ff
        li      x7, 0x033
        bne     x13, x7, fail

        li      TESTNUM, 7                      // a patch that changes the instruction's kind
        la      x5, patch7
        li      x6, 0x00c58733                  // add x14, x11, x12
        li      x11, 30
        li      x12, 12
        sw      x6, 0(x5)
        fence.i
patch7: sub     x14, x11, x12
        li      x7, 42
        bne     x14, x7, fail

        li      TESTNUM, 8                      // patch a jump target: a branch over the old code
        la      x5, patch8
        li      x6, 0x0080006f                  // j .+8
        sw      x6, 0(x5)
        fence.i
patch8: j       fail
        j       fail
        j       pass

far_code:
        nop
far_patch:
        addi    x13, x0, 3
        ret

TEST_DATA
