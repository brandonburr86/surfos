/*
SurfCPU directed test: MMIO accesses
--------------------
File: mmio.S Date: 10/15/26 (SurfCPU)
--------------------
With the testbench's registers (docs/CPU.md section 9): a fetch from an MMIO window is an
instruction access fault; a byte or halfword access to a register, or an access to an
offset with no register, is a load/store access fault that has no effect (the probe
counters prove it); every load and store reaches the register exactly once, in order,
however long the bus stalls: 64 reads of PROBE_RD return 0..63, 64 writes to PROBE_WR
count 64 and add up to the sum written; dependent MMIO read-modify-write chains; the timer
registers; console output.
*/
// surf-test: spike=no
#include "surf_test.h"

TEST_BEGIN
test_main:
        li      x9, TB_BASE

        // ---------------------------------------------------------- fetch from MMIO
        li      TESTNUM, 1
        li      x5, CLINT_MSIP
        addi    x8, t3, 1
        la      s11, 1f
        jalr    x0, 0(x5)
        j       fail
1:      bne     t3, x8, fail
        li      x7, 1
        bne     t5, x7, fail
        bne     t6, x5, fail
        bne     t4, x5, fail
        li      TESTNUM, 2
        addi    x5, x9, TB_PROBE_RD             // (a fetch must not pop the probe either)
        addi    x8, t3, 1
        la      s11, 1f
        jalr    x0, 0(x5)
        j       fail
1:      bne     t3, x8, fail
        li      x7, 1
        bne     t5, x7, fail

        // ---------------------------------------------------------- reset the probes
        sw      x0, TB_PROBE_RD(x9)
        sw      x0, TB_PROBE_SUM(x9)
        lw      x20, TB_PROBE_WR(x9)            // the write count so far

        // ---------------------------------------------------------- non-word accesses fault
        li      x11, 0x55
        TRAP_EXPECT(10, 7, sb x11, TB_PROBE_WR(x9))
        li      TESTNUM, 11
        addi    x7, x9, TB_PROBE_WR
        bne     t4, x7, fail
        TRAP_EXPECT(12, 7, sh x11, TB_PROBE_WR(x9))
        TRAP_EXPECT(13, 7, sb x11, TB_PROBE_WR + 1(x9))
        TRAP_EXPECT(14, 7, sh x11, TB_PROBE_WR + 2(x9))
        li      TESTNUM, 15                     // none of them counted
        lw      x14, TB_PROBE_WR(x9)
        bne     x14, x20, fail
        TRAP_EXPECT(16, 5, lb x14, TB_PROBE_RD(x9))
        TRAP_EXPECT(17, 5, lhu x14, TB_PROBE_RD(x9))
        TRAP_EXPECT(18, 5, lbu x14, TB_PROBE_RD + 3(x9))
        TRAP_EXPECT(19, 5, lh x14, TB_PROBE_RD + 2(x9))
        li      TESTNUM, 20                     // none of them popped
        lw      x14, TB_PROBE_RD(x9)
        bnez    x14, fail
        TRAP_EXPECT(21, 5, lw x14, 0x100(x9))   // offsets with no register
        TRAP_EXPECT(22, 7, sw x11, 0x7fc(x9))
        li      x5, CLINT_MSIP
        TRAP_EXPECT(23, 5, lw x14, 8(x5))
        TRAP_EXPECT(24, 5, lb x14, 0(x5))

        // ---------------------------------------------------------- exactly once, in order
        li      TESTNUM, 30
        sw      x0, TB_PROBE_RD(x9)
        li      x13, 0
        li      x15, 64
1:      lw      x14, TB_PROBE_RD(x9)
        bne     x14, x13, fail
        addi    x13, x13, 1
        bne     x13, x15, 1b
        li      TESTNUM, 31
        lw      x20, TB_PROBE_WR(x9)
        sw      x0, TB_PROBE_SUM(x9)
        li      x13, 0
2:      sw      x13, TB_PROBE_WR(x9)
        addi    x13, x13, 1
        bne     x13, x15, 2b
        lw      x14, TB_PROBE_WR(x9)
        sub     x14, x14, x20
        bne     x14, x15, fail
        lw      x14, TB_PROBE_SUM(x9)
        li      x7, 2016                        // 0 + 1 + ... + 63
        bne     x14, x7, fail
        li      TESTNUM, 32                     // straight-line reads, no loop overhead
        sw      x0, TB_PROBE_RD(x9)
        lw      x11, TB_PROBE_RD(x9)
        lw      x12, TB_PROBE_RD(x9)
        lw      x13, TB_PROBE_RD(x9)
        lw      x14, TB_PROBE_RD(x9)
        add     x11, x11, x12
        add     x11, x11, x13
        add     x11, x11, x14
        li      x7, 6                           // 0 + 1 + 2 + 3
        bne     x11, x7, fail
        li      x7, 3
        bne     x14, x7, fail
        li      TESTNUM, 33                     // read, modify, write back, dependent
        sw      x0, TB_PROBE_SUM(x9)
        lw      x20, TB_PROBE_WR(x9)
        lw      x13, TB_PROBE_RD(x9)            // 4
        addi    x13, x13, 10
        sw      x13, TB_PROBE_WR(x9)            // 14
        lw      x13, TB_PROBE_RD(x9)            // 5
        sw      x13, TB_PROBE_WR(x9)            // 5
        lw      x14, TB_PROBE_SUM(x9)
        li      x7, 19
        bne     x14, x7, fail
        lw      x14, TB_PROBE_WR(x9)
        sub     x14, x14, x20
        li      x7, 2
        bne     x14, x7, fail

        // ---------------------------------------------------------- the timer registers
        li      TESTNUM, 40
        li      x5, CLINT_MTIMECMP
        li      x11, 0x12345678
        sw      x11, 0(x5)
        li      x12, 0x0000abcd
        sw      x12, 4(x5)
        lw      x13, 0(x5)
        bne     x13, x11, fail
        lw      x13, 4(x5)
        bne     x13, x12, fail
        li      x11, -1                         // back to "never"
        sw      x11, 0(x5)
        sw      x11, 4(x5)
        li      TESTNUM, 41                     // mtime runs
        li      x5, CLINT_MTIME
        lw      x13, 0(x5)
        nop
        lw      x14, 0(x5)
        bgeu    x13, x14, fail

        // ---------------------------------------------------------- console
        li      x11, 'm'
        sw      x11, TB_CONSOLE(x9)
        li      x11, 'm'
        sw      x11, TB_CONSOLE(x9)
        li      x11, 'i'
        sw      x11, TB_CONSOLE(x9)
        li      x11, 'o'
        sw      x11, TB_CONSOLE(x9)
        li      x11, '\n'
        sw      x11, TB_CONSOLE(x9)
        j       pass

TEST_DATA
