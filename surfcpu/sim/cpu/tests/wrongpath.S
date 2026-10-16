/*
SurfCPU directed test: no wrong-path stores or MMIO accesses
--------------------
File: wrongpath.S Date: 10/15/26 (SurfCPU)
--------------------
Stores to RAM, MMIO writes (PROBE_WR, which counts) and MMIO reads with a side effect
(PROBE_RD, which pops a counter) are placed where they must never execute: behind taken
branches, JAL and JALR, behind instructions that trap (ECALL, EBREAK, an illegal
instruction, a misaligned load, a faulting load, a faulting store, a misaligned jump),
behind MRET and FENCE.I, and behind a slow store whose branch shadow is long. Afterwards
memory is unchanged and the probe counters show exactly the accesses on the right path.
Run with long latencies on both ports, so the wrong-path instructions sit in the pipeline
for many cycles.
*/
// surf-test: spike=no
#include "surf_test.h"

// x9 = TB_BASE, x8 = &victim (a RAM word that must stay 0x600df00d), x11 = a value
#define WRONG_PATH_ACCESSES                                             \
        sw      x11, TB_PROBE_WR(x9);                                   \
        lw      x12, TB_PROBE_RD(x9);                                   \
        sw      x11, 0(x8);                                             \
        sb      x11, 1(x8);                                             \
        sw      x11, TB_PROBE_SUM(x9);

TEST_BEGIN
test_main:
        li      x9, TB_BASE
        la      x8, victim
        li      x11, 0xbad
        sw      x0, TB_PROBE_RD(x9)
        sw      x0, TB_PROBE_SUM(x9)
        lw      x20, TB_PROBE_WR(x9)            // write count at the start
        li      x21, 0                          // reads made on the right path

        // ---------------------------------------------------------- taken flow
        li      TESTNUM, 1
        beq     x0, x0, 1f
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 2
        jal     x1, 1f
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 3
        la      x6, 1f
        jalr    x1, 0(x6)
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 4                      // a branch decided by a load just before
        lw      x13, 0(x8)
        bnez    x13, 1f
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 5                      // a branch decided by an MMIO read just before
        lw      x13, TB_PROBE_RD(x9)            // 0: right path, counted
        addi    x21, x21, 1
        beqz    x13, 1f
        WRONG_PATH_ACCESSES
1:

        // ---------------------------------------------------------- shadows of traps
        li      TESTNUM, 10
        la      s11, 1f
        ecall
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 11
        la      s11, 1f
        ebreak
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 12
        la      s11, 1f
        .word   0xffffffff
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 13
        la      s11, 1f
        lw      x13, 2(x8)                      // misaligned
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 14
        li      x6, UNMAPPED_ADDR
        la      s11, 1f
        lw      x13, 0(x6)                      // access fault: known only at the response
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 15
        la      s11, 1f
        sw      x11, 0(x6)                      // store access fault
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 16
        la      x6, 1f + 2
        la      s11, 1f
        jalr    x0, 0(x6)                       // misaligned target
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 17                     // an MMIO store that faults (byte width)
        la      s11, 1f
        sb      x11, TB_PROBE_WR(x9)
        WRONG_PATH_ACCESSES
1:

        // ---------------------------------------------------------- MRET and FENCE.I
        li      TESTNUM, 20
        la      x6, 1f
        csrw    mepc, x6
        mret
        WRONG_PATH_ACCESSES
1:      li      TESTNUM, 21                     // FENCE.I: the next instructions are refetched,
        fence.i                                 // and run once (they are the right path)
        sw      x11, TB_PROBE_WR(x9)
        lw      x12, TB_PROBE_RD(x9)
        addi    x21, x21, 1
        li      TESTNUM, 22                     // so one write and one read were counted
        lw      x14, TB_PROBE_WR(x9)
        sub     x14, x14, x20
        li      x7, 1
        bne     x14, x7, fail
        addi    x20, x20, 1

        // ---------------------------------------------------------- a long shadow behind a store
        li      TESTNUM, 30
        sw      x11, 4(x8)                      // the store waits on the bus (long latency)
        j       1f                              // while the jump's shadow is fetched
        WRONG_PATH_ACCESSES
        WRONG_PATH_ACCESSES
1:      sw      x11, TB_PROBE_WR(x9)            // right path: counted once
        beq     x0, x0, 2f
        WRONG_PATH_ACCESSES
2:      addi    x20, x20, 1

        // ---------------------------------------------------------- the reckoning
        li      TESTNUM, 40                     // the victim word is untouched
        lw      x14, 0(x8)
        li      x7, 0x600df00d
        bne     x14, x7, fail
        li      TESTNUM, 41                     // writes: only the right-path ones
        lw      x14, TB_PROBE_WR(x9)
        bne     x14, x20, fail
        li      TESTNUM, 42                     // reads: only the right-path ones
        lw      x14, TB_PROBE_RD(x9)
        bne     x14, x21, fail
        li      TESTNUM, 43                     // PROBE_SUM was never written on a wrong path
        lw      x14, TB_PROBE_SUM(x9)
        li      x7, 0xbad + 0xbad               // the two right-path PROBE_WR writes
        bne     x14, x7, fail
        j       pass

        .data
        .balign 4
victim: .word   0x600df00d
        .word   0

TEST_DATA
