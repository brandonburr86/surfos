/*
SurfCPU reference test: FENCE and FENCE.I
--------------------
File: fence.S Date: 10/15/26 (SurfCPU)
--------------------
FENCE with several predecessor/successor sets, FENCE.TSO and PAUSE, FENCE and FENCE.I with
the fields the ISA says to ignore set (both still legal), and self-modifying code: a
routine in RAM is rewritten by stores, FENCE.I makes the new code visible, and it is run
before and after (docs/ISA.md: FENCE.I completes after every older store and refetches).
*/
#include "ref_env.h"

REF_BEGIN
    fence
    fence rw, rw
    fence i, o
    fence.tso
    .word 0x0100000f                           /* PAUSE (fence w,0) */
    .word 0x9ff5850f                           /* FENCE, fm=9 rs1=11 rd=10: ignored fields */
    .word 0x0055918f                           /* FENCE.I, rd=3 rs1=11 imm=5: still FENCE.I */
    /* run the routine: it returns 1 */
    la s0, patch
    jalr ra, 0(s0)
    SAVE(a0, 0)
    /* rewrite its first instruction to "li a0, 42" and make it visible */
    li a1, 0x02a00513                          /* addi a0, x0, 42 */
    sw a1, 0(s0)
    fence.i
    jalr ra, 0(s0)
    SAVE(a0, 1)
    /* rewrite it again: "li a0, -7", then FENCE.I with ignored fields set */
    li a1, 0xff900513
    sw a1, 0(s0)
    .word 0x0000100f | (3 << 7) | (9 << 15)    /* fence.i, rd=3 rs1=9 */
    jalr ra, 0(s0)
    SAVE(a0, 2)
    REF_PASS

REF_TRAP_HANDLER

    .data
    .balign 4
patch:
    li a0, 1
    ret

REF_DATA(4, 4)
