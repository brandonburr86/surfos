/*
SurfCPU reference-test environment
--------------------
File: ref_env.h Date: 10/15/26 (SurfCPU)
--------------------
Start-up, trap handler and end of run for the programs in sim/ref/directed: they run on
build/cpu/surfcpu_tb and on Spike, and tools/rvrun.py compares the two retirement traces
and the signatures. Nothing here depends on timing or on reset values: REF_BEGIN zeroes
every register (Spike's boot ROM leaves t0, a0 and a1 set; the core's registers are
undefined after reset) and points mtvec at the handler.

The handler appends (mcause, mepc, mtval) to the trap log inside the signature, then
returns to mepc + 4, or to ra after an instruction access fault (tests reach an unmapped
address with `jalr ra, 0(reg)`). It uses only t5 and t6 (x30, x31): the tests leave
them alone. REF_PASS stores 1 to tohost; REF_FAIL(n) stores n*2+1 (docs/MODULES.md
section 6).
*/
#ifndef SURFCPU_REF_ENV_H
#define SURFCPU_REF_ENV_H

#define REF_BEGIN                     \
    .section .text.init, "ax";        \
    .globl _start;                    \
_start:                               \
    li x1, 0;  li x2, 0;  li x3, 0;   \
    li x4, 0;  li x5, 0;  li x6, 0;   \
    li x7, 0;  li x8, 0;  li x9, 0;   \
    li x10, 0; li x11, 0; li x12, 0;  \
    li x13, 0; li x14, 0; li x15, 0;  \
    li x16, 0; li x17, 0; li x18, 0;  \
    li x19, 0; li x20, 0; li x21, 0;  \
    li x22, 0; li x23, 0; li x24, 0;  \
    li x25, 0; li x26, 0; li x27, 0;  \
    li x28, 0; li x29, 0; li x30, 0;  \
    li x31, 0;                        \
    la t0, ref_trap;                  \
    csrw mtvec, t0;                   \
    li t0, 0

#define REF_PASS                      \
    fence;                            \
    li t0, 1;                         \
    la t1, tohost;                    \
1:  sw t0, 0(t1);                     \
    j 1b

#define REF_FAIL(n)                   \
    fence;                            \
    li t0, ((n) << 1) | 1;            \
    la t1, tohost;                    \
1:  sw t0, 0(t1);                     \
    j 1b

/* Store a result word: results[idx] = reg (uses t4). */
#define SAVE(reg, idx)                \
    la t4, results;                   \
    sw reg, ((idx) * 4)(t4)

/* The trap handler, the trap log and the tohost words; put it after the test's code. */
#define REF_TRAP_HANDLER              \
    .balign 4;                        \
    .globl ref_trap;                  \
ref_trap:                             \
    la t6, ref_trap_ptr;              \
    lw t6, 0(t6);                     \
    csrr t5, mcause;                  \
    sw t5, 0(t6);                     \
    csrr t5, mepc;                    \
    sw t5, 4(t6);                     \
    csrr t5, mtval;                   \
    sw t5, 8(t6);                     \
    addi t6, t6, 12;                  \
    la t5, ref_trap_ptr;              \
    sw t6, 0(t5);                     \
    csrr t5, mcause;                  \
    li t6, 1;                         \
    beq t5, t6, 2f;                   \
    csrr t5, mepc;                    \
    addi t5, t5, 4;                   \
    csrw mepc, t5;                    \
    mret;                             \
2:  csrw mepc, ra;                    \
    mret

/* Data: the signature (results, then the trap log) and tohost. SIG_WORDS results. */
#define REF_DATA(SIG_WORDS, TRAPS)    \
    .section .tohost, "aw", @progbits; \
    .balign 64;                       \
    .globl tohost;                    \
tohost: .dword 0;                     \
    .balign 64;                       \
    .globl fromhost;                  \
fromhost: .dword 0;                   \
    .data;                            \
    .balign 16;                       \
ref_trap_ptr: .word ref_trap_log;     \
    .balign 16;                       \
    .globl begin_signature;           \
begin_signature:                      \
results: .fill SIG_WORDS, 4, 0xdeadbeef; \
ref_trap_log: .fill (TRAPS) * 3, 4, 0; \
    .globl end_signature;             \
end_signature:                        \
    .word 0

#endif
