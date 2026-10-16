/*
SurfCPU directed test: random interrupts during real work
--------------------
File: irq_stress.S Date: 10/15/26 (SurfCPU)
--------------------
A loop of 200 iterations mixing RAM loads and stores, an MMIO read with a side effect
(PROBE_RD returns how often it was read) and an MMIO write (PROBE_WR, adding into
PROBE_SUM), multiply, divide, CSR writes and reads, an ECALL (handled by the framework's
trap handler) and data-dependent branches runs once
without interrupts (the reference) and once while the testbench raises external
interrupts at random (from --seed, at a rate scaled to the reference run's length so that
about 300 come whatever the bus timing; the handler counts them and drops the level). Both runs must compute the same checksum, read and write the probes
exactly 200 times each with the same sum, take exactly 200 ECALL traps (an interrupt
landing on the ECALL is taken first, then the ECALL traps after MRET), and the handler must
have run at least 100 times, always with mepc inside the interrupted region.
*/
// surf-test: spike=no max-cycles=40000000
#define TEST_IRQ_HANDLER
#define TRAP_LIMIT 100000
#include "surf_test.h"

#define ITER 200

TEST_BEGIN
test_main:
        li      x9, TB_BASE
        la      x8, array_a

        // ---------------------------------------------------------- reference run
        li      TESTNUM, 1
        sw      x0, TB_PROBE_RD(x9)
        sw      x0, TB_PROBE_SUM(x9)
        lw      x20, TB_PROBE_WR(x9)
        mv      x25, t3                         // traps so far (t3 counts every trap)
        lw      x4, TB_CYCLE(x9)
        jal     ra, work
        lw      x2, TB_CYCLE(x9)
        sub     x2, x2, x4                      // cycles the work took at this bus speed
        mv      x21, x15                        // reference checksum
        mv      x22, x16                        // reference probe sum
        sub     x25, t3, x25                    // one ECALL per iteration
        li      x7, ITER
        bne     x25, x7, fail
        lw      x14, TB_PROBE_RD(x9)
        li      x7, ITER
        bne     x14, x7, fail
        lw      x14, TB_PROBE_WR(x9)
        sub     x14, x14, x20
        bne     x14, x7, fail
        lw      x14, TB_PROBE_SUM(x9)
        bne     x14, x22, fail

        // ---------------------------------------------------------- with random interrupts
        li      TESTNUM, 2
        sw      x0, TB_PROBE_RD(x9)
        sw      x0, TB_PROBE_SUM(x9)
        lw      x20, TB_PROBE_WR(x9)
        li      x24, 0                          // interrupts taken
        mv      x25, t3                         // traps so far
        li      x7, MIP_MEIP
        csrw    mie, x7
        li      x7, 300                         // about 300 interrupts during the work,
        divu    x11, x2, x7                     // whatever the bus timing
        li      x7, 8
        bgeu    x11, x7, 1f
        li      x11, 8
1:      sw      x11, TB_IRQ_RANDOM(x9)
stress_begin:
        csrsi   mstatus, MSTATUS_MIE
        jal     ra, work
        csrci   mstatus, MSTATUS_MIE
stress_end:
        sw      x0, TB_IRQ_RANDOM(x9)
        sw      x0, TB_IRQ_EXT(x9)
        li      TESTNUM, 3                      // same results
        bne     x15, x21, fail
        bne     x16, x22, fail
        li      TESTNUM, 4                      // every probe access exactly once
        lw      x14, TB_PROBE_RD(x9)
        li      x7, ITER
        bne     x14, x7, fail
        lw      x14, TB_PROBE_WR(x9)
        sub     x14, x14, x20
        bne     x14, x7, fail
        lw      x14, TB_PROBE_SUM(x9)
        bne     x14, x22, fail
        li      TESTNUM, 5                      // and the interrupts really came
        li      x7, 100
        bltu    x24, x7, fail
        li      TESTNUM, 6                      // every ECALL trapped exactly once:
        sub     x25, t3, x25                    // traps = ECALLs + interrupts
        sub     x25, x25, x24
        li      x7, ITER
        bne     x25, x7, fail
        j       pass

        // x15 = checksum, x16 = sum of the values written to PROBE_WR. Uses x10..x19, x6.
work:
        la      x17, array_b                    // clear B
        li      x10, 64
1:      sw      x0, 0(x17)
        addi    x17, x17, 4
        addi    x10, x10, -1
        bnez    x10, 1b
        li      x15, 0x1234
        li      x16, 0
        li      x10, 0                          // i
2:      andi    x11, x10, 63
        slli    x11, x11, 2
        add     x12, x8, x11
        lw      x13, 0(x12)                     // a = A[i % 64]
        lw      x14, TB_PROBE_RD(x9)            // b = i (if read exactly once each time)
        add     x13, x13, x14
        li      x6, 0x9e3779b1
        mul     x13, x13, x6                    // m
        ori     x6, x10, 1
        divu    x14, x13, x6                    // d = m / (i | 1)
        xor     x18, x14, x13
        la      x17, array_b
        add     x17, x17, x11
        sw      x18, 0(x17)                     // B[i % 64] = d ^ m
        andi    x19, x14, 0xff
        sw      x19, TB_PROBE_WR(x9)
        add     x16, x16, x19
        addi    x11, x11, 4
        andi    x11, x11, 0xff
        la      x17, array_b
        add     x17, x17, x11
        lw      x19, 0(x17)                     // B[(i + 1) % 64]
        slli    x6, x15, 5
        sub     x15, x6, x15                    // checksum * 31
        add     x15, x15, x14
        add     x15, x15, x19
        csrw    mscratch, x15
        ecall                                   // an exception every iteration; the trap
        csrr    x19, mscratch                   // handler skips it (an interrupt may come first)
        bne     x19, x15, fail
        andi    x6, x13, 1
        beqz    x6, 3f
        addi    x15, x15, 3
        j       4f
3:      xori    x15, x15, 5
4:      addi    x10, x10, 1
        li      x6, ITER
        bne     x10, x6, 2b
        ret

        .balign 4
irq_handler:
        addi    x24, x24, 1
        li      s10, TB_BASE
        sw      x0, TB_IRQ_EXT(s10)
        la      x23, stress_begin               // mepc inside the region
        bltu    t6, x23, fail
        la      x23, stress_end
        bgeu    t6, x23, 5f
        mret
5:      la      x23, work_end                   // or inside work
        bgeu    t6, x23, fail
        la      x23, work
        bltu    t6, x23, fail
        mret
work_end:

        .data
        .balign 4
array_a:
        .word   0x00000001, 0x12345678, 0x9abcdef0, 0x0badf00d, 0xdeadbeef, 0x00000000, 0xffffffff, 0x80000000
        .word   0x7fffffff, 0x01010101, 0x10101010, 0xcafebabe, 0x55555555, 0xaaaaaaaa, 0x13579bdf, 0x2468ace0
        .word   0x0f0f0f0f, 0xf0f0f0f0, 0x00ff00ff, 0xff00ff00, 0x11111111, 0x22222222, 0x33333333, 0x44444444
        .word   0x66666666, 0x77777777, 0x88888888, 0x99999999, 0xbbbbbbbb, 0xcccccccc, 0xdddddddd, 0xeeeeeeee
        .word   0x00000003, 0x00000007, 0x0000000f, 0x0000001f, 0x0000003f, 0x0000007f, 0x000000ff, 0x000001ff
        .word   0x12121212, 0x34343434, 0x56565656, 0x78787878, 0x9a9a9a9a, 0xbcbcbcbc, 0xdededede, 0xf0f0f0f1
        .word   0x31415926, 0x27182818, 0x16180339, 0x14142135, 0x17320508, 0x22360679, 0x26457513, 0x28284271
        .word   0x00010000, 0x00020000, 0x00040000, 0x00080000, 0x00100000, 0x00200000, 0x00400000, 0x00800000
array_b:
        .space  256

TEST_DATA
