/*
SurfCPU directed test framework
--------------------
File: surf_test.h Date: 10/15/26 (SurfCPU)
--------------------
Included by every sim/cpu/tests/*.S (riscv-tests style, self-checking). A test writes its
body after TEST_BEGIN as `test_main:` code and ends with `j pass`; any check that fails
jumps to `fail`. Code stays in .text.init with `fail`, so the assembler can relax a branch
to `fail` that is out of range. TEST_DATA goes at the end of the file. The result goes to `tohost`: 1 = pass, (TESTNUM << 1) | 1 = fail, which
build/cpu/surfcpu_tb and Spike both understand.

Registers reserved for the framework (tests use x1..x24 except gp):
  gp  (x3)   TESTNUM, the number of the check being made (reported on failure)
  s9  (x25)  scratch of the TRAP_EXPECT macro
  s10 (x26)  scratch of the trap handler
  s11 (x27)  resume address for the next exception (0: resume after the trapping instruction)
  t3  (x28)  number of traps taken so far
  t4  (x29)  mtval of the last trap
  t5  (x30)  mcause of the last trap
  t6  (x31)  mepc of the last trap

The default trap handler records mcause/mepc/mtval in t5/t6/t4, counts the trap in t3 and
returns to s11 (then clears it) or, when s11 is 0, to mepc + 4. An interrupt goes to the
test's `irq_handler` when the test defines TEST_IRQ_HANDLER before including this file, and
fails the test otherwise. More than TRAP_LIMIT traps fail the test (a runaway trap loop).

At reset, before anything else, the startup code saves the CSRs a test may want to see in
their reset state into `reset_csrs` (offsets RESET_*).
*/
#ifndef SURF_TEST_H
#define SURF_TEST_H

#define TESTNUM gp

#ifndef TRAP_LIMIT
#define TRAP_LIMIT 4000
#endif

// the testbench's devices (docs/CPU.md section 9)
#define CLINT_MSIP        0x02000000
#define CLINT_MTIMECMP    0x02004000
#define CLINT_MTIMECMPH   0x02004004
#define CLINT_MTIME       0x0200BFF8
#define CLINT_MTIMEH      0x0200BFFC
#define TB_BASE           0x03000000
#define TB_CONSOLE        0x000
#define TB_IRQ_EXT        0x004
#define TB_IRQ_DELAY      0x008
#define TB_IRQ_RANDOM     0x00C
#define TB_PROBE_RD       0x010
#define TB_PROBE_WR       0x014
#define TB_PROBE_SUM      0x018
#define TB_CYCLE          0x01C

// an address where nothing answers (bus error)
#define UNMAPPED_ADDR     0x70000000

// offsets in reset_csrs
#define RESET_MSTATUS     0
#define RESET_MTVEC       4
#define RESET_MIE         8
#define RESET_MIP         12
#define RESET_MSCRATCH    16
#define RESET_MEPC        20
#define RESET_MCAUSE      24
#define RESET_MTVAL       28
#define RESET_MCOUNTINH   32
#define RESET_MCYCLE      36
#define RESET_MINSTRET    40

#define MSTATUS_MIE  0x8
#define MSTATUS_MPIE 0x80
#define MIP_MSIP     0x8
#define MIP_MTIP     0x80
#define MIP_MEIP     0x800

#define TEST_BEGIN                                                      \
        .section .text.init, "ax", @progbits;                           \
        .globl _start;                                                  \
_start:                                                                 \
        la      t0, reset_csrs;                                         \
        csrr    t1, mstatus;        sw t1, RESET_MSTATUS(t0);           \
        csrr    t1, mtvec;          sw t1, RESET_MTVEC(t0);             \
        csrr    t1, mie;            sw t1, RESET_MIE(t0);               \
        csrr    t1, mip;            sw t1, RESET_MIP(t0);               \
        csrr    t1, mscratch;       sw t1, RESET_MSCRATCH(t0);          \
        csrr    t1, mepc;           sw t1, RESET_MEPC(t0);              \
        csrr    t1, mcause;         sw t1, RESET_MCAUSE(t0);            \
        csrr    t1, mtval;          sw t1, RESET_MTVAL(t0);             \
        csrr    t1, mcountinhibit;  sw t1, RESET_MCOUNTINH(t0);         \
        csrr    t1, mcycle;         sw t1, RESET_MCYCLE(t0);            \
        csrr    t1, minstret;       sw t1, RESET_MINSTRET(t0);          \
        la      t0, trap_entry;                                         \
        csrw    mtvec, t0;                                              \
        li      t3, 0;                                                  \
        li      s11, 0;                                                 \
        li      TESTNUM, 0;                                             \
        j       test_main;                                              \
                                                                        \
        .balign 4;                                                      \
trap_entry:                                                             \
        csrr    t5, mcause;                                             \
        csrr    t6, mepc;                                               \
        csrr    t4, mtval;                                              \
        addi    t3, t3, 1;                                              \
        li      s10, TRAP_LIMIT;                                        \
        bgeu    t3, s10, fail;                                          \
        bltz    t5, trap_interrupt;                                     \
        mv      s10, s11;                                               \
        li      s11, 0;                                                 \
        bnez    s10, 1f;                                                \
        addi    s10, t6, 4;                                             \
1:      csrw    mepc, s10;                                              \
        mret;                                                           \
trap_interrupt:                                                         \
        TRAP_IRQ_JUMP;                                                  \
                                                                        \
pass:                                                                   \
        fence;                                                          \
        li      t0, 1;                                                  \
        la      t1, tohost;                                             \
        sw      t0, 0(t1);                                              \
1:      j       1b;                                                     \
fail:                                                                   \
        fence;                                                          \
        bnez    TESTNUM, 2f;                                            \
        li      TESTNUM, 1023;                                          \
2:      slli    t0, TESTNUM, 1;                                         \
        ori     t0, t0, 1;                                              \
        la      t1, tohost;                                             \
        sw      t0, 0(t1);                                              \
1:      j       1b;                                                     \
        .balign 4;

#ifdef TEST_IRQ_HANDLER
#define TRAP_IRQ_JUMP j irq_handler
#else
#define TRAP_IRQ_JUMP j fail
#endif

#define TEST_DATA                                                       \
        .pushsection .tohost, "aw", @progbits;                          \
        .balign 64;                                                     \
        .globl tohost;                                                  \
tohost: .dword 0;                                                       \
        .balign 64;                                                     \
        .globl fromhost;                                                \
fromhost: .dword 0;                                                     \
        .popsection;                                                    \
        .pushsection .data, "aw", @progbits;                            \
        .balign 16;                                                     \
reset_csrs: .space 64;                                                  \
        .popsection;

// ------------------------------------------------------------------ checks
#define CHECK(num, reg, val)                                            \
        li      TESTNUM, num;                                           \
        li      x7, val;                                                \
        bne     reg, x7, fail;

#define CHECK_REG(num, reg, other)                                      \
        li      TESTNUM, num;                                           \
        bne     reg, other, fail;

// rd = inst(val1, val2) on x11/x12 into x14
#define TEST_RR(num, inst, result, val1, val2)                          \
        li      TESTNUM, num;                                           \
        li      x11, val1;                                              \
        li      x12, val2;                                              \
        inst    x14, x11, x12;                                          \
        li      x7, result;                                             \
        bne     x14, x7, fail;

// rd = inst(val1, imm)
#define TEST_IMM(num, inst, result, val1, imm)                          \
        li      TESTNUM, num;                                           \
        li      x11, val1;                                              \
        inst    x14, x11, imm;                                          \
        li      x7, result;                                             \
        bne     x14, x7, fail;

// The instructions after `cause` must trap exactly once with that mcause at their first
// instruction (label 97); execution resumes at 98. mtval stays in t4 for the caller.
#define TRAP_EXPECT(num, cause, ...)                                    \
        li      TESTNUM, num;                                           \
        addi    s9, t3, 1;                                              \
        la      s11, 98f;                                               \
97:     __VA_ARGS__;                                                    \
        j       fail;                                                   \
98:     bne     t3, s9, fail;                                           \
        li      x7, cause;                                              \
        bne     t5, x7, fail;                                           \
        la      x7, 97b;                                                \
        bne     t6, x7, fail;

// no trap may have happened since `mark` (a register holding an earlier t3)
#define CHECK_NO_TRAP(num, mark)                                        \
        li      TESTNUM, num;                                           \
        bne     t3, mark, fail;

#endif
