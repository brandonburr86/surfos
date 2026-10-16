/*
SurfOS riscv32: do traps keep every register?
--------------------
File: rvregs.S  Date: 10/15/26 (SurfCPU)
--------------------
For the self test (arch/riscv32/rvtest.c). Each function fills ra, gp, tp and x5..x30
with a pattern made from the seed (xN = N * 0x01010101 ^ seed, a0 = seed ^ 0x2a), counts
x31 down from spins to 0, and then stores all 31 registers into out[32] (out[0] = 0):

  void rv_regs_spin(u_long seed, u_long spins, u_long *out)    timer interrupts arrive meanwhile
  void rv_regs_ecall(u_long seed, u_long spins, u_long *out)   ECALL (yield) on every round

The caller compares out[] with the pattern. While the pattern is in the registers this
code uses no register for anything else: not gp (relaxed accesses would use it), not
the stack (sp only, which a broken trap path would lose). So any register that a trap
or a task switch failed to restore shows up as a difference. The callee-saved registers,
ra, gp and tp are kept in the frame and restored before returning.
*/

#define FRAME   (16 * 4 + 32 * 4)       /* 16 saved words, then the 32-word dump */
#define DUMP    64

        .macro  fill
        .irp    n, 1, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
        li      x\n, (\n * 0x01010101)
        xor     x\n, x\n, a0
        .endr
        .endm

        .macro  dump
        .irp    n, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
        sw      x\n, (DUMP + \n * 4)(sp)
        .endr
        sw      zero, DUMP(sp)
        sw      sp, (DUMP + 8)(sp)
        .endm

        .macro  regcheck name, yield
        .globl  \name
        .balign 4
\name:
        addi    sp, sp, -FRAME
        sw      ra, 0(sp)
        sw      gp, 4(sp)
        sw      tp, 8(sp)
        sw      s0, 12(sp)
        sw      s1, 16(sp)
        sw      s2, 20(sp)
        sw      s3, 24(sp)
        sw      s4, 28(sp)
        sw      s5, 32(sp)
        sw      s6, 36(sp)
        sw      s7, 40(sp)
        sw      s8, 44(sp)
        sw      s9, 48(sp)
        sw      s10, 52(sp)
        sw      s11, 56(sp)
        sw      a2, 60(sp)              /* out */
        mv      x31, a1                 /* the counter */
        fill
        xori    a0, a0, 0x2a
1:      beqz    x31, 2f
        .if \yield
        ecall
        .endif
        addi    x31, x31, -1
        j       1b
2:      dump
        lw      a2, 60(sp)              /* the dump to out[], 32 words */
        addi    t0, sp, DUMP
        li      t1, 32
3:      lw      t2, 0(t0)
        sw      t2, 0(a2)
        addi    t0, t0, 4
        addi    a2, a2, 4
        addi    t1, t1, -1
        bnez    t1, 3b
        lw      ra, 0(sp)
        lw      gp, 4(sp)
        lw      tp, 8(sp)
        lw      s0, 12(sp)
        lw      s1, 16(sp)
        lw      s2, 20(sp)
        lw      s3, 24(sp)
        lw      s4, 28(sp)
        lw      s5, 32(sp)
        lw      s6, 36(sp)
        lw      s7, 40(sp)
        lw      s8, 44(sp)
        lw      s9, 48(sp)
        lw      s10, 52(sp)
        lw      s11, 56(sp)
        addi    sp, sp, FRAME
        ret
        .endm

        .text
        regcheck rv_regs_spin, 0
        regcheck rv_regs_ecall, 1
