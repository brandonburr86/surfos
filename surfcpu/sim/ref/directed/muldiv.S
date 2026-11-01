/*
SurfCPU reference test: the M extension's corner cases
--------------------
File: muldiv.S Date: 10/15/26 (SurfCPU)
--------------------
MUL, MULH, MULHSU and MULHU on signed and unsigned extremes; DIV, DIVU, REM and REMU with
division by zero (quotient all ones, remainder the dividend) and the one overflow case
(-2^31 / -1 = -2^31, remainder 0); rd equal to a source; results used at once.
*/
#include "ref_env.h"

#define OP3(op, a, b, idx) li a1, a; li a2, b; op a3, a1, a2; SAVE(a3, idx)

REF_BEGIN
    OP3(mul, 0x7fffffff, 0x7fffffff, 0)
    OP3(mulh, 0x7fffffff, 0x7fffffff, 1)
    OP3(mulh, 0x80000000, 0x80000000, 2)
    OP3(mulh, 0x80000000, 0x7fffffff, 3)
    OP3(mulh, 0xffffffff, 0xffffffff, 4)
    OP3(mulhu, 0xffffffff, 0xffffffff, 5)
    OP3(mulhu, 0x80000000, 0x00000002, 6)
    OP3(mulhsu, 0xffffffff, 0xffffffff, 7)     /* -1 * 0xffffffff */
    OP3(mulhsu, 0x80000000, 0xffffffff, 8)
    OP3(mulhsu, 0x7fffffff, 0x80000000, 9)
    OP3(mul, 0x12345678, 0x9abcdef0, 10)
    OP3(div, 0x00000007, 0x00000000, 11)       /* by zero: -1 */
    OP3(divu, 0x00000007, 0x00000000, 12)      /* by zero: 0xffffffff */
    OP3(rem, 0x00000007, 0x00000000, 13)       /* by zero: the dividend */
    OP3(remu, 0x80000007, 0x00000000, 14)
    OP3(div, 0x80000000, 0xffffffff, 15)       /* overflow: -2^31 */
    OP3(rem, 0x80000000, 0xffffffff, 16)       /* overflow: 0 */
    OP3(div, 0xfffffff9, 0x00000002, 17)       /* -7 / 2 = -3 (towards zero) */
    OP3(rem, 0xfffffff9, 0x00000002, 18)       /* -1 */
    OP3(divu, 0xfffffff9, 0x00000002, 19)
    OP3(remu, 0xfffffff9, 0x00000002, 20)
    OP3(div, 0x00000007, 0xfffffffe, 21)       /* 7 / -2 = -3 */
    OP3(rem, 0x00000007, 0xfffffffe, 22)       /* 1 */
    OP3(divu, 0x00000001, 0xffffffff, 23)
    OP3(div, 0x80000000, 0x00000001, 24)
    /* rd = rs1 = rs2, and a result consumed at once */
    li a1, 0xfffffffd
    mul a1, a1, a1
    SAVE(a1, 25)
    li a1, 100
    li a2, 7
    div a3, a1, a2
    mul a4, a3, a2
    sub a5, a1, a4
    rem a6, a1, a2
    sub a7, a5, a6                             /* 0 */
    SAVE(a7, 26)
    mulh x0, a1, a2                            /* to x0: dropped */
    div x0, a1, x0
    SAVE(x0, 27)
    REF_PASS

REF_TRAP_HANDLER
REF_DATA(28, 4)
