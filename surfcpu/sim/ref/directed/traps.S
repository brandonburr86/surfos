/*
SurfCPU reference test: every exception of docs/ISA.md section 3
--------------------
File: traps.S Date: 10/15/26 (SurfCPU)
--------------------
ECALL, EBREAK, illegal instructions (0, all ones, 16-bit encodings with a zero upper half,
a reserved funct3/funct7, SRET/URET/SFENCE.VMA, an unknown CSR, a write to a read-only
CSR), misaligned jump targets (JAL, JALR, a taken branch; a not-taken one must not trap),
misaligned loads and stores of every width, load/store access faults and a fetch access
fault. The handler logs (mcause, mepc, mtval) of each into the signature; the trace
comparison checks the rest (no register written by a trapping instruction, MRET).
Unmapped address used for access faults: 0x40000000 (nothing there on Spike or in
build/cpu/surfcpu_tb, docs/VERIFICATION.md section 5).
*/
#include "ref_env.h"

REF_BEGIN
    la s0, buf
    li s1, 0x40000000
    ecall
    ebreak
    .word 0x00000000
    .word 0xffffffff
    .word 0x00000001              /* 16-bit encodings, upper half zero (see ISA.md mtval) */
    .word 0x00004501
    .word 0x00008082              /* c.ret-shaped 16-bit parcel */
    .word 0x00001033              /* sll x0,x0,x0: legal */
    .word 0x02005033              /* divu x0,x0,x0: legal */
    .word 0x40001033              /* OP funct7=0x20 funct3=1: reserved -> illegal */
    .word 0x0000302b              /* custom-1 opcode: illegal */
    .word 0x10200073              /* SRET */
    .word 0x00200073              /* URET */
    .word 0x12000073              /* SFENCE.VMA */
    .word 0x02059293              /* SLLI with imm[5] = 1 */
    .word 0x0000300f              /* MISC-MEM funct3=3: illegal */
    .word 0x00003003              /* LD (RV64): illegal */
    .word 0x00007023              /* store funct3=7: illegal */
    .word 0x00002063              /* branch funct3=2: illegal */
    .word 0x00001067              /* JALR funct3=1: illegal */
    .word 0x00100093 | (1 << 31)  /* addi with a negative immediate: legal */
    csrr a0, 0x7c0                /* unknown CSR */
    csrw mvendorid, a0            /* write to a read-only CSR */
    csrrs a0, mhartid, x0         /* read-only read: fine */
    csrrsi a0, mhartid, 0         /* fine */
    csrrsi a0, mhartid, 1         /* write: illegal */
    /* misaligned jump targets: the jump traps and writes no register */
    li a1, 0x1111
    jal a1, 1f + 2
1:  SAVE(a1, 0)
    la a2, 2f
    addi a2, a2, 2
    jalr a1, 0(a2)                /* target 2 mod 4: trap */
2:  SAVE(a1, 1)
    la a2, 3f
    addi a2, a2, 1                /* bit 0 is cleared by JALR: no trap */
    jalr a1, 0(a2)
    nop
3:  SAVE(a1, 2)
    li a3, 1
    beq a3, a3, 4f + 2            /* taken: trap */
4:  bne a3, a3, 4b + 2            /* not taken: no trap */
    /* misaligned loads and stores */
    lh a4, 1(s0)
    lhu a4, 3(s0)
    lw a4, 1(s0)
    lw a4, 2(s0)
    lw a4, 3(s0)
    sh a4, 1(s0)
    sw a4, 2(s0)
    sw a4, 3(s0)
    lb a4, 1(s0)                  /* byte accesses are never misaligned */
    sb a4, 3(s0)
    /* access faults */
    lw a5, 0(s1)
    lbu a5, 3(s1)
    sw a5, 0(s1)
    sb a5, 1(s1)
    li a6, 0x80400000             /* just past the 4 MiB of RAM */
    lw a5, 0(a6)
    jalr ra, 0(s1)                /* fetch fault: the handler returns to ra */
    SAVE(ra, 3)
    jalr ra, 0(a6)
    SAVE(ra, 4)
    REF_PASS

REF_TRAP_HANDLER

    .data
    .balign 16
buf: .word 0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00

REF_DATA(8, 48)
