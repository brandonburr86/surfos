/*
SurfCPU reference test: loads and stores
--------------------
File: mem.S Date: 10/15/26 (SurfCPU)
--------------------
Every width at every legal alignment: LB/LBU and LH/LHU sign and zero extension, SB and SH
into each byte lane (byte enables), SW, negative and positive offsets at the immediate's
ends, a load into x0 (the read still happens), store data forwarded from the instruction
before, a load used at once, and a store followed by a load of the same bytes.
*/
#include "ref_env.h"

REF_BEGIN
    la s0, buf
    addi s1, s0, 2047                          /* for offsets -2048..2047 */
    lw a1, 0(s0)
    SAVE(a1, 0)
    lb a1, 0(s0)
    SAVE(a1, 1)
    lb a1, 3(s0)                               /* 0x80: sign-extended */
    SAVE(a1, 2)
    lbu a1, 3(s0)
    SAVE(a1, 3)
    lh a1, 2(s0)                               /* 0x80f1: negative */
    SAVE(a1, 4)
    lhu a1, 2(s0)
    SAVE(a1, 5)
    lh a1, 0(s0)
    SAVE(a1, 6)
    /* byte lanes */
    li a2, 0xa1
    sb a2, 4(s0)
    li a2, 0xb2
    sb a2, 5(s0)
    li a2, 0xc3
    sb a2, 6(s0)
    li a2, 0xd4
    sb a2, 7(s0)
    lw a1, 4(s0)
    SAVE(a1, 7)
    li a2, 0x1234abcd
    sh a2, 8(s0)                               /* only the low half */
    li a2, 0x5678
    sh a2, 10(s0)
    lw a1, 8(s0)
    SAVE(a1, 8)
    sw zero, 12(s0)
    li a2, 0xffffffff
    sb a2, 13(s0)
    lw a1, 12(s0)
    SAVE(a1, 9)
    /* immediate ends */
    li a2, 0x5a5a5a5a
    sw a2, -2047(s1)                           /* s1 - 2047 = s0 */
    lw a1, -2047(s1)
    SAVE(a1, 10)
    sw a2, 2044(s0)
    lw a1, 2044(s0)
    SAVE(a1, 11)
    lw a1, -2048(s1)                           /* s0 - 1: misaligned, traps */
    lbu a1, -2048(s1)                          /* s0 - 1: the last byte before buf */
    SAVE(a1, 12)
    /* x0 as destination: the load still reads */
    lw x0, 0(s0)
    lbu x0, 3(s0)
    /* store data from the instruction just before; load used at once */
    li a3, 0x0bad0bad
    sw a3, 16(s0)
    lw a4, 16(s0)
    addi a4, a4, 1
    sw a4, 20(s0)
    lh a5, 20(s0)
    add a5, a5, a4
    SAVE(a5, 13)
    lw a6, 20(s0)
    SAVE(a6, 14)
    REF_PASS

REF_TRAP_HANDLER

    .data
    .balign 16
before: .word 0xcafef00d
buf:    .word 0x80f17f01, 0, 0, 0, 0, 0
    .fill 1024, 4, 0x01010101

REF_DATA(16, 4)
