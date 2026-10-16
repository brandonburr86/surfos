/*
SurfCPU reference test: branches and jumps
--------------------
File: branch.S Date: 10/15/26 (SurfCPU)
--------------------
Each branch taken and not taken, with signed and unsigned operands that disagree; branches
backwards (a counted loop) and forwards; JAL and JALR link values (also with rd = rs1, and
rd = x0); JALR clearing bit 0 of the target; a branch right after the instruction that
computes its operands; and calls through a small jump table.
*/
#include "ref_env.h"

/* record whether a branch was taken: a3 = 1 if taken, else 0 */
#define BR(op, a, b, idx) \
    li a1, a; li a2, b; li a3, 1; op a1, a2, 1f; li a3, 0; 1: SAVE(a3, idx)

REF_BEGIN
    BR(beq, 5, 5, 0)
    BR(beq, 5, 6, 1)
    BR(bne, 5, 6, 2)
    BR(bne, 5, 5, 3)
    BR(blt, 0xffffffff, 0, 4)                  /* -1 < 0: taken */
    BR(bltu, 0xffffffff, 0, 5)                 /* not taken */
    BR(bge, 0, 0xffffffff, 6)                  /* taken */
    BR(bgeu, 0, 0xffffffff, 7)                 /* not taken */
    BR(blt, 0x80000000, 0x7fffffff, 8)
    BR(bge, 0x7fffffff, 0x7fffffff, 9)
    BR(bgeu, 0x80000000, 0x80000000, 10)
    BR(bltu, 0x7fffffff, 0x80000000, 11)
    /* a counted loop: backwards branches, the last one not taken */
    li a4, 0
    li a5, 10
2:  addi a4, a4, 3
    addi a5, a5, -1
    bnez a5, 2b
    SAVE(a4, 12)
    /* JAL link value, JAL to x0 */
    jal ra, 3f
    j 4f
3:  la a6, 3b
    sub a6, ra, a6                             /* ra = (address of jal) + 4 = 3b - 4 */
    SAVE(a6, 13)
    ret
4:  jal x0, 5f
    li a3, 0x111                               /* skipped */
5:  /* JALR: rd = rs1, bit 0 cleared, negative offset */
    la a1, 6f + 9
    jalr a1, -8(a1)                            /* target 6f + 1 -> bit 0 cleared -> 6f */
    li a3, 0x222                               /* skipped */
6:  la a2, 6b
    sub a2, a1, a2                             /* a1 = link = 6b - 4 + ... */
    SAVE(a2, 14)
    /* a jump table */
    li s2, 0
    li s3, 0
7:  la t0, table
    slli t1, s3, 2
    add t0, t0, t1
    lw t0, 0(t0)
    jalr ra, 0(t0)
    addi s3, s3, 1
    li t2, 3
    bltu s3, t2, 7b
    SAVE(s2, 15)
    /* a branch on a value computed just before */
    li a1, 7
    addi a1, a1, -7
    beqz a1, 8f
    li a3, 0x333
8:  SAVE(a1, 16)
    REF_PASS

f0: addi s2, s2, 1
    ret
f1: addi s2, s2, 10
    ret
f2: addi s2, s2, 100
    ret

REF_TRAP_HANDLER

    .data
    .balign 4
table: .word f0, f1, f2

REF_DATA(20, 4)
