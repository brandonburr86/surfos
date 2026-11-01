/*
SurfCPU directed test: CSR instructions
--------------------
File: csr.S Date: 10/15/26 (SurfCPU)
--------------------
The six CSR instructions (old value returned, write/set/clear, the immediate forms, rs1 = x0
and rd = x0), writable masks of mstatus, mie, mtvec, mepc, mtval and mscratch, operands
computed just before a CSR write and CSR values used just after a read (the late-result
interlock), writes to read-only CSRs (decided by the rs1/uimm field, not its value) and
accesses to CSRs that do not exist: illegal instruction, mtval = the instruction, rd and
the CSR unchanged. Behaviour that is specific to this implementation and differs from
Spike (reset values, ignored writes, the CSR address map) is in csr_impl.S.
*/
#include "surf_test.h"

// the trapping instruction is in t6 (mepc) and mtval holds its bits
#define CHECK_TVAL_INSN(num)                                            \
        li      TESTNUM, num;                                           \
        lw      x8, 0(t6);                                              \
        bne     x8, t4, fail;

#define SENTINEL 0x5a5a5a5a

TEST_BEGIN
test_main:
        // ---------------------------------------------------------- the six forms on mscratch
        li      TESTNUM, 1
        li      x11, 0x12345678
        csrw    mscratch, x11
        csrr    x14, mscratch
        bne     x14, x11, fail
        li      TESTNUM, 2                      // CSRRW returns the old value
        li      x12, 0xcafebabe
        csrrw   x14, mscratch, x12
        bne     x14, x11, fail
        csrr    x14, mscratch
        bne     x14, x12, fail
        li      TESTNUM, 3                      // CSRRS
        li      x11, 0x0000ffff
        csrw    mscratch, x11
        li      x12, 0x00ff0f00
        csrrs   x14, mscratch, x12
        bne     x14, x11, fail
        csrr    x14, mscratch
        li      x7, 0x00ffffff
        bne     x14, x7, fail
        li      TESTNUM, 4                      // CSRRC
        li      x12, 0x00f0f0f0
        csrrc   x14, mscratch, x12
        li      x7, 0x00ffffff
        bne     x14, x7, fail
        csrr    x14, mscratch
        li      x7, 0x000f0f0f
        bne     x14, x7, fail
        li      TESTNUM, 5                      // CSRRWI, CSRRSI, CSRRCI (uimm zero-extended)
        csrrwi  x14, mscratch, 31
        li      x7, 0x000f0f0f
        bne     x14, x7, fail
        csrr    x14, mscratch
        li      x7, 31
        bne     x14, x7, fail
        csrrci  x14, mscratch, 0x15
        li      x7, 31
        bne     x14, x7, fail
        csrrsi  x14, mscratch, 0x10
        li      x7, 0x0a
        bne     x14, x7, fail
        csrr    x14, mscratch
        li      x7, 0x1a
        bne     x14, x7, fail
        li      TESTNUM, 6                      // rs1 = x0 / uimm = 0: no write
        li      x11, 0x87654321
        csrw    mscratch, x11
        csrrs   x14, mscratch, x0
        csrrc   x15, mscratch, x0
        csrrsi  x16, mscratch, 0
        csrrci  x17, mscratch, 0
        csrr    x18, mscratch
        bne     x14, x11, fail
        bne     x15, x11, fail
        bne     x16, x11, fail
        bne     x17, x11, fail
        bne     x18, x11, fail
        li      TESTNUM, 7                      // rd = x0 still writes
        li      x11, 0x11110000
        csrrw   x0, mscratch, x11
        li      x12, 0xff
        csrrs   x0, mscratch, x12
        csrrsi  x0, mscratch, 0
        csrr    x14, mscratch
        li      x7, 0x111100ff
        bne     x14, x7, fail
        csrrc   x0, mscratch, x11
        csrrwi  x0, mepc, 0
        csrr    x14, mscratch
        li      x7, 0x000000ff
        bne     x14, x7, fail

        // ---------------------------------------------------------- dependences
        li      TESTNUM, 10                     // operand computed just before the write
        li      x11, 0x100
        addi    x11, x11, 0x23
        csrw    mscratch, x11
        csrr    x14, mscratch
        li      x7, 0x123
        bne     x14, x7, fail
        li      TESTNUM, 11                     // value read and used at once
        csrr    x14, mscratch
        addi    x15, x14, 1
        li      x7, 0x124
        bne     x15, x7, fail
        li      TESTNUM, 12                     // read, modify, write back, read
        csrr    x14, mscratch
        slli    x14, x14, 4
        csrw    mscratch, x14
        csrr    x15, mscratch
        li      x7, 0x1230
        bne     x15, x7, fail
        li      TESTNUM, 13                     // a loaded value written at once
        la      x5, csr_value
        lw      x11, 0(x5)
        csrw    mscratch, x11
        csrr    x14, mscratch
        bne     x14, x11, fail
        li      TESTNUM, 14                     // a read value stored at once
        la      x5, csr_store
        csrr    x14, mscratch
        sw      x14, 0(x5)
        lw      x15, 0(x5)
        bne     x15, x11, fail
        li      TESTNUM, 15                     // the swap idiom with the same register
        li      x11, 0xaaaa5555
        csrrw   x11, mscratch, x11
        li      x7, 0xdeadbeef
        bne     x11, x7, fail
        csrr    x14, mscratch
        li      x7, 0xaaaa5555
        bne     x14, x7, fail

        // ---------------------------------------------------------- writable masks
        csrw    mie, x0
        li      TESTNUM, 20                     // mstatus: MIE, MPIE; MPP reads 11
        li      x11, -1
        csrw    mstatus, x11
        csrr    x14, mstatus
        li      x7, 0x00001888
        bne     x14, x7, fail
        csrw    mstatus, x0
        csrr    x14, mstatus
        li      x7, 0x00001800
        bne     x14, x7, fail
        li      TESTNUM, 21                     // mie: MSIE, MTIE, MEIE (with MIE clear)
        csrw    mie, x11
        csrr    x14, mie
        li      x7, 0x00000888
        bne     x14, x7, fail
        csrw    mie, x0
        csrr    x14, mie
        bnez    x14, fail
        li      TESTNUM, 22                     // mtvec: BASE
        csrr    x20, mtvec
        li      x11, 0xfffffffc
        csrw    mtvec, x11
        csrr    x14, mtvec
        bne     x14, x11, fail
        csrw    mtvec, x20
        li      TESTNUM, 23                     // mepc: bits 1:0 read 0
        li      x11, -1
        csrw    mepc, x11
        csrr    x14, mepc
        li      x7, 0xfffffffc
        bne     x14, x7, fail
        li      x11, 0x80000006
        csrw    mepc, x11
        csrr    x14, mepc
        li      x7, 0x80000004
        bne     x14, x7, fail
        li      TESTNUM, 24                     // mtval and mscratch: every bit
        li      x11, -1
        csrw    mtval, x11
        csrr    x14, mtval
        bne     x14, x11, fail
        csrw    mtval, x0
        csrr    x14, mtval
        bnez    x14, fail
        csrw    mscratch, x11
        csrr    x14, mscratch
        bne     x14, x11, fail

        // ---------------------------------------------------------- read-only CSRs
        li      x11, 0                          // a zero value, but a nonzero register index
        li      x14, SENTINEL
        TRAP_EXPECT(30, 2, csrw mvendorid, x11)
        CHECK_TVAL_INSN(31)
        TRAP_EXPECT(32, 2, csrrs x14, mhartid, x11)
        CHECK_TVAL_INSN(33)
        CHECK(34, x14, SENTINEL)
        TRAP_EXPECT(35, 2, csrrsi x14, mimpid, 1)
        CHECK_TVAL_INSN(36)
        CHECK(37, x14, SENTINEL)
        TRAP_EXPECT(38, 2, csrrci x14, mconfigptr, 31)
        CHECK(39, x14, SENTINEL)
        TRAP_EXPECT(40, 2, csrrwi x0, cycle, 0)
        CHECK_TVAL_INSN(41)
        TRAP_EXPECT(42, 2, csrrw x0, time, x0)
        TRAP_EXPECT(43, 2, csrrc x14, instret, x12)
        CHECK(44, x14, SENTINEL)
        TRAP_EXPECT(45, 2, csrrw x14, marchid, x14)
        CHECK(46, x14, SENTINEL)
        TRAP_EXPECT(47, 2, csrrs x14, hpmcounter3, x11)
        CHECK(48, x14, SENTINEL)
        li      TESTNUM, 49                     // reads of read-only CSRs are fine
        mv      x9, t3
        csrrs   x14, mhartid, x0
        bnez    x14, fail
        csrrsi  x14, mhartid, 0
        bnez    x14, fail
        csrrc   x14, mvendorid, x0
        bnez    x14, fail
        csrrci  x14, mconfigptr, 0
        bnez    x14, fail
        csrr    x14, cycleh
        csrr    x14, instreth
        CHECK_NO_TRAP(50, x9)

        // ---------------------------------------------------------- CSRs that do not exist
        li      x14, SENTINEL
        TRAP_EXPECT(60, 2, csrr x14, 0x000)       // ustatus (no U mode)
        CHECK_TVAL_INSN(61)
        CHECK(62, x14, SENTINEL)
        TRAP_EXPECT(63, 2, csrr x14, 0x100)       // sstatus (no S mode)
        TRAP_EXPECT(64, 2, csrw 0x180, x11)       // satp
        TRAP_EXPECT(65, 2, csrr x14, 0xb01)       // between mcycle and minstret
        TRAP_EXPECT(66, 2, csrr x14, 0xf16)
        TRAP_EXPECT(67, 2, csrr x14, 0x7b0)       // dcsr (no debug mode)
        CHECK(68, x14, SENTINEL)
        li      TESTNUM, 69                     // and the failed writes changed nothing
        csrr    x14, mscratch
        li      x7, -1
        bne     x14, x7, fail
        j       pass

        .data
        .balign 4
csr_value:
        .word   0xdeadbeef
csr_store:
        .word   0

TEST_DATA
