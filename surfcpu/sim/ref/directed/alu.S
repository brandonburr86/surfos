/*
SurfCPU reference test: integer arithmetic of RV32I
--------------------
File: alu.S Date: 10/15/26 (SurfCPU)
--------------------
Every register-register and register-immediate operation, LUI and AUIPC, with the values
where implementations go wrong: 0, 1, -1, the most negative and most positive numbers,
immediates at both ends (-2048, 2047), shift amounts 0, 1, 31, register shift amounts with
bits above 4 set (ignored), signed against unsigned comparisons, and x0 as destination
(the write is dropped) and as source. Results land in the signature; the trace comparison
checks every register write on the way.
*/
#include "ref_env.h"

#define OP3(op, a, b, idx) li a1, a; li a2, b; op a3, a1, a2; SAVE(a3, idx)
#define OPI(op, a, imm, idx) li a1, a; op a3, a1, imm; SAVE(a3, idx)

REF_BEGIN
    OP3(add, 0x7fffffff, 1, 0)
    OP3(add, 0xffffffff, 1, 1)
    OP3(sub, 0x80000000, 1, 2)
    OP3(sub, 0, 0x80000000, 3)
    OP3(sll, 0x00000001, 31, 4)
    OP3(sll, 0x12345678, 0xffffffe4, 5)        /* only rs2[4:0] = 4 counts */
    OP3(srl, 0x80000000, 31, 6)
    OP3(srl, 0x80000000, 0x20, 7)              /* shift by 0 */
    OP3(sra, 0x80000000, 31, 8)
    OP3(sra, 0x7fffffff, 30, 9)
    OP3(slt, 0xffffffff, 0, 10)                /* -1 < 0 */
    OP3(sltu, 0xffffffff, 0, 11)               /* 0xffffffff > 0 unsigned */
    OP3(slt, 0x80000000, 0x7fffffff, 12)
    OP3(sltu, 0x80000000, 0x7fffffff, 13)
    OP3(xor, 0xf0f0f0f0, 0xff00ff00, 14)
    OP3(or, 0xf0f0f0f0, 0x0f0f0000, 15)
    OP3(and, 0xf0f0f0f0, 0xff00ff00, 16)
    OPI(addi, 0x00000000, -2048, 17)
    OPI(addi, 0x00000000, 2047, 18)
    OPI(addi, 0x7fffffff, 1, 19)
    OPI(slti, 0x00000000, -1, 20)
    OPI(sltiu, 0x00000000, -1, 21)             /* imm sign-extends to 0xffffffff */
    OPI(sltiu, 0xfffffffe, -1, 22)
    OPI(slti, 0x80000000, -2048, 23)
    OPI(xori, 0x12345678, -1, 24)              /* NOT */
    OPI(ori, 0x12340000, -2048, 25)
    OPI(andi, 0xffffffff, 0x7ff, 26)
    OPI(andi, 0xffffffff, -2048, 27)
    OPI(slli, 0x00000001, 0, 28)
    OPI(slli, 0x00000003, 31, 29)
    OPI(srli, 0xffffffff, 31, 30)
    OPI(srai, 0x80000000, 1, 31)
    OPI(srai, 0x80000000, 0, 32)
    lui a3, 0xfffff
    SAVE(a3, 33)
    lui a3, 0x80000
    SAVE(a3, 34)
1:  auipc a3, 0
    la a4, 1b
    sub a3, a3, a4                             /* 0: auipc gives its own pc */
    SAVE(a3, 35)
    auipc a3, 0xfffff
    SAVE(a3, 36)
    /* x0: writes are dropped, reads are zero */
    li a1, 0x55
    add x0, a1, a1
    addi x0, x0, 5
    lui x0, 0x12345
    add a3, x0, x0
    SAVE(a3, 37)
    or a3, x0, a1
    SAVE(a3, 38)
    sub a3, x0, a1
    SAVE(a3, 39)
    /* dependent chains (forwarding at distance 1, 2, 3) */
    li a1, 3
    add a2, a1, a1
    add a3, a2, a1
    add a4, a3, a2
    add a5, a4, a1
    sub a6, a5, a4
    SAVE(a6, 40)
    SAVE(a5, 41)
    REF_PASS

REF_TRAP_HANDLER
REF_DATA(48, 4)
