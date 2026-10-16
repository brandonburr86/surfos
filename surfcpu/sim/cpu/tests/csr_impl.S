/*
SurfCPU directed test: the CSR table of docs/ISA.md
--------------------
File: csr_impl.S Date: 10/15/26 (SurfCPU)
--------------------
What docs/ISA.md section 2 fixes for this implementation (and Spike does differently):
reset values (saved by the startup code before anything wrote them, including minstret =
22 at the 23rd instruction), the identification CSRs, misa/mstatush/mip writes ignored,
mtvec MODE fixed at 0, the WLRL mcause mask, mcountinhibit's two bits, PMP and HPM CSRs
reading 0 and ignoring writes. Then all 4096 CSR addresses: a read (CSRRS rd, csr, x0)
traps exactly when the address is not in the table, and a CSRRS with a nonzero rs1 index
(value 0) traps exactly when the address is missing or read only. The probe instructions
are written into the code and run after FENCE.I.
*/
// surf-test: spike=no max-cycles=30000000
#define TRAP_LIMIT 100000
#include "surf_test.h"

#define CHECK_CSR(num, csr, val)                                        \
        li      TESTNUM, num;                                           \
        csrr    x14, csr;                                               \
        li      x7, val;                                                \
        bne     x14, x7, fail;

#define CHECK_RESET(num, off, val)                                      \
        li      TESTNUM, num;                                           \
        la      x5, reset_csrs;                                         \
        lw      x14, off(x5);                                           \
        li      x7, val;                                                \
        bne     x14, x7, fail;

// writes are ignored: write all ones and zero, read val back each time
#define CHECK_FIXED(num, csr, val)                                      \
        li      TESTNUM, num;                                           \
        li      x11, -1;                                                \
        csrw    csr, x11;                                               \
        csrr    x14, csr;                                               \
        li      x7, val;                                                \
        bne     x14, x7, fail;                                          \
        csrw    csr, x0;                                                \
        csrr    x14, csr;                                               \
        bne     x14, x7, fail;

TEST_BEGIN
test_main:
        // ---------------------------------------------------------- reset values
        CHECK_RESET(1, RESET_MSTATUS, 0x00001800)
        CHECK_RESET(2, RESET_MTVEC, 0)
        CHECK_RESET(3, RESET_MIE, 0)
        CHECK_RESET(4, RESET_MIP, 0)
        CHECK_RESET(5, RESET_MSCRATCH, 0)
        CHECK_RESET(6, RESET_MEPC, 0)
        CHECK_RESET(7, RESET_MCAUSE, 0)
        CHECK_RESET(8, RESET_MTVAL, 0)
        CHECK_RESET(9, RESET_MCOUNTINH, 0)
        CHECK_RESET(10, RESET_MINSTRET, 22)     // 22 instructions retired before that csrr
        li      TESTNUM, 11                     // mcycle counts from reset: small, at least 22
        la      x5, reset_csrs
        lw      x14, RESET_MCYCLE(x5)
        li      x7, 22
        bltu    x14, x7, fail
        li      x7, 2000
        bgeu    x14, x7, fail

        // ---------------------------------------------------------- identification
        CHECK_CSR(20, mvendorid, 0)
        CHECK_CSR(21, marchid, 0)
        CHECK_CSR(22, mimpid, 0x00000100)
        CHECK_CSR(23, mhartid, 0)
        CHECK_CSR(24, mconfigptr, 0)
        CHECK_CSR(25, misa, 0x40001100)

        // ---------------------------------------------------------- writes ignored
        CHECK_FIXED(30, misa, 0x40001100)
        CHECK_FIXED(31, mstatush, 0)
        CHECK_FIXED(32, mip, 0)
        CHECK_FIXED(33, pmpcfg0, 0)
        CHECK_FIXED(34, pmpcfg15, 0)
        CHECK_FIXED(35, pmpaddr0, 0)
        CHECK_FIXED(36, pmpaddr63, 0)
        CHECK_FIXED(37, mhpmevent3, 0)
        CHECK_FIXED(38, mhpmevent31, 0)
        CHECK_FIXED(39, mhpmcounter3, 0)
        CHECK_FIXED(40, mhpmcounter31, 0)
        CHECK_FIXED(41, mhpmcounter3h, 0)
        CHECK_FIXED(42, mhpmcounter31h, 0)
        CHECK_CSR(43, hpmcounter3, 0)
        CHECK_CSR(44, hpmcounter31, 0)
        CHECK_CSR(45, hpmcounter3h, 0)
        CHECK_CSR(46, hpmcounter31h, 0)

        // ---------------------------------------------------------- masks specific to SurfCPU
        li      TESTNUM, 50                     // mtvec MODE is fixed at 0 (direct)
        csrr    x20, mtvec
        ori     x11, x20, 3
        csrw    mtvec, x11
        csrr    x14, mtvec
        csrw    mtvec, x20
        bne     x14, x20, fail
        li      TESTNUM, 51                     // mcause: bit 31 and 4:0
        li      x11, -1
        csrw    mcause, x11
        csrr    x14, mcause
        li      x7, 0x8000001f
        bne     x14, x7, fail
        li      x11, 0x7fffffe0
        csrw    mcause, x11
        csrr    x14, mcause
        bnez    x14, fail
        li      TESTNUM, 52                     // mcountinhibit: CY and IR
        li      x11, -1
        csrw    mcountinhibit, x11
        csrr    x14, mcountinhibit
        csrw    mcountinhibit, x0
        li      x7, 5
        bne     x14, x7, fail
        csrr    x14, mcountinhibit
        bnez    x14, fail

        // ---------------------------------------------------------- every CSR address
        la      x5, csr_ranges                  // turn the ranges into a bitmap
        la      x15, csr_bitmap
0:      lhu     x6, 0(x5)                       // lo
        lhu     x7, 2(x5)                       // hi
        li      x8, 0xffff
        beq     x6, x8, 2f
        addi    x5, x5, 4
1:      srli    x8, x6, 5                       // word index
        slli    x8, x8, 2
        add     x8, x8, x15
        lw      x9, 0(x8)
        andi    x10, x6, 31
        li      x12, 1
        sll     x12, x12, x10
        or      x9, x9, x12
        sw      x9, 0(x8)
        addi    x6, x6, 1
        bgeu    x7, x6, 1b
        j       0b
2:      li      x20, 0                          // the address being probed
        li      x21, 4096
        li      x11, 0                          // CSRRS ..., x11: a write of value 0
probe_loop:
        srli    x8, x20, 5                      // x22 = 1 if x20 is in the table
        slli    x8, x8, 2
        add     x8, x8, x15
        lw      x9, 0(x8)
        srl     x9, x9, x20                     // shift by x20 mod 32
        andi    x22, x9, 1
        // x23 = 1 if read only (bits 11:10 = 11)
        srli    x23, x20, 10
        xori    x23, x23, 3
        seqz    x23, x23
        // read probe: csrrs x14, csr, x0
        slli    x6, x20, 20
        li      x7, 0x00002773
        or      x6, x6, x7
        la      x5, probe_site
        sw      x6, 0(x5)
        fence.i
        mv      x9, t3
        la      s11, 4f
        jal     x0, probe_site
4:      sub     x10, t3, x9                     // 1 if it trapped
        li      TESTNUM, 60
        seqz    x8, x22                         // expected: trap iff absent
        bne     x10, x8, probe_fail
        beqz    x10, 5f
        li      TESTNUM, 61                     // a trapped probe: illegal instruction, mtval = insn
        li      x7, 2
        bne     t5, x7, probe_fail
        lw      x8, probe_site
        bne     t4, x8, probe_fail
5:      // write-intent probe: csrrs x14, csr, x11 (rs1 index 11, value 0)
        slli    x6, x20, 20
        li      x7, 0x0005a773
        or      x6, x6, x7
        la      x5, probe_site
        sw      x6, 0(x5)
        fence.i
        mv      x9, t3
        la      s11, 6f
        jal     x0, probe_site
6:      sub     x10, t3, x9
        li      TESTNUM, 62                     // expected: trap iff absent or read only
        seqz    x8, x22
        or      x8, x8, x23
        bne     x10, x8, probe_fail
        addi    x20, x20, 1
        bne     x20, x21, probe_loop
        j       pass

probe_fail:
        mv      a0, x20                         // the failing address, for a debugger
        j       fail

        .balign 4
probe_site:
        nop                                     // replaced by the probe instruction
        la      x5, 7f                          // no trap: continue at the resume address
        jr      s11
7:      j       fail

        .data
        .balign 4
csr_ranges:                                     // docs/ISA.md section 2, inclusive ranges
        .half   0x300, 0x301                    // mstatus misa
        .half   0x304, 0x305                    // mie mtvec
        .half   0x310, 0x310                    // mstatush
        .half   0x320, 0x320                    // mcountinhibit
        .half   0x323, 0x33f                    // mhpmevent3..31
        .half   0x340, 0x344                    // mscratch mepc mcause mtval mip
        .half   0x3a0, 0x3ef                    // pmpcfg0..15 pmpaddr0..63
        .half   0xb00, 0xb00                    // mcycle
        .half   0xb02, 0xb1f                    // minstret mhpmcounter3..31
        .half   0xb80, 0xb80                    // mcycleh
        .half   0xb82, 0xb9f                    // minstreth mhpmcounter3h..31h
        .half   0xc00, 0xc1f                    // cycle time instret hpmcounter3..31
        .half   0xc80, 0xc9f                    // cycleh timeh instreth hpmcounter3h..31h
        .half   0xf11, 0xf15                    // mvendorid marchid mimpid mhartid mconfigptr
        .half   0xffff, 0xffff
        .balign 4
csr_bitmap:
        .space  512

TEST_DATA
