/*
SurfCPU directed test: illegal instructions
--------------------
File: illegal.S Date: 10/15/26 (SurfCPU)
--------------------
Every class of illegal instruction in docs/ISA.md section 1 raises cause 2 with mtval =
the instruction bits (just the low 16 for a 16-bit encoding, as the privileged spec asks)
and mepc = its address, and writes no register: 0x00000000 and
0xFFFFFFFF, 16-bit encodings, longer-instruction prefixes, SRET/URET/DRET/SFENCE.VMA and
every other SYSTEM funct3 = 0 encoding that is not exactly ECALL/EBREAK/MRET/WFI, SYSTEM
funct3 = 4, shift immediates with imm[5] = 1 or a wrong funct7, every undefined funct3 and
funct7 of OP, LOAD, STORE, BRANCH, JALR and MISC-MEM, the A, F/D, RV64, vector and custom
opcodes. Then the encodings that must not trap: FENCE with any fields (FENCE.TSO, PAUSE,
nonzero rd/rs1), FENCE.I with nonzero fields. CSR-address cases are in csr.S/csr_impl.S.
*/
#include "surf_test.h"

#define SENTINEL 0x5a5a5a5a

// the bits must trap as illegal with mtval = tval; x1 and x14 (rd fields used) unchanged
#define ILLEGAL_TVAL(num, bits, tval)                                   \
        li      x1, SENTINEL;                                           \
        li      x14, SENTINEL;                                          \
        TRAP_EXPECT(num, 2, .word bits)                                 \
        li      x7, tval;                                               \
        bne     t4, x7, fail;                                           \
        li      x7, SENTINEL;                                           \
        bne     x1, x7, fail;                                           \
        bne     x14, x7, fail;

// a 32-bit (or longer) encoding: mtval is the whole word
#define ILLEGAL(num, bits) ILLEGAL_TVAL(num, bits, bits)
// a 16-bit encoding (bits 1:0 != 11): mtval is the instruction, the low half only
#define ILLEGAL16(num, bits) ILLEGAL_TVAL(num, bits, ((bits) & 0xffff))

TEST_BEGIN
test_main:
        // ---------------------------------------------------------- whole-word patterns
        ILLEGAL(1,  0x00000000)
        ILLEGAL(2,  0xffffffff)
        // 16-bit encodings (bits 1:0 != 11): mtval holds just those 16 bits (the faulting
        // instruction, right-justified: privileged spec, mtval on illegal instructions)
        ILLEGAL16(3,  0x00000001)
        ILLEGAL16(4,  0x00000002)
        ILLEGAL16(5,  0x00004501)               // c.li a0, 0
        ILLEGAL16(6,  0x12345678)
        ILLEGAL16(7,  0xdeadfffe)
        ILLEGAL16(11, 0xffff0000)
        // longer-instruction prefixes: mtval holds the first 32 bits (ILEN)
        ILLEGAL(8,  0x0000001f)                 // 48-bit prefix
        ILLEGAL(9,  0x0000003f)                 // 64-bit prefix
        ILLEGAL(10, 0x0000007f)

        // ---------------------------------------------------------- SYSTEM
        ILLEGAL(20, 0x10200073)                 // sret
        ILLEGAL(21, 0x00200073)                 // uret
        ILLEGAL(22, 0x7b200073)                 // dret
        ILLEGAL(23, 0x12000073)                 // sfence.vma x0, x0
        ILLEGAL(24, 0x12b70073)                 // sfence.vma x14, x11
        ILLEGAL(25, 0x22000073)                 // hfence.vvma
        ILLEGAL(26, 0x000000f3)                 // ecall with rd = x1
        ILLEGAL(27, 0x00108073)                 // ebreak with rs1 = x1
        ILLEGAL(28, 0x302000f3)                 // mret with rd = x1
        ILLEGAL(29, 0x30208073)                 // mret with rs1 = x1
        ILLEGAL(30, 0x10500773)                 // wfi with rd = x14
        ILLEGAL(31, 0x10508073)                 // wfi with rs1 = x1
        ILLEGAL(32, 0x00300073)
        ILLEGAL(33, 0x00004073)                 // funct3 = 4
        ILLEGAL(34, 0x60004773)                 // hlv-style, rd = x14

        // ---------------------------------------------------------- OP-IMM shifts
        ILLEGAL(40, 0x02071713)                 // slli x14, x14, 32 (imm[5] = 1)
        ILLEGAL(41, 0x02075713)                 // srli x14, x14, 32
        ILLEGAL(42, 0x42075713)                 // srai x14, x14, 32
        ILLEGAL(43, 0x40071713)                 // slli with funct7 = 0100000
        ILLEGAL(44, 0x60075713)                 // srli/srai with funct7 = 0110000
        ILLEGAL(45, 0x80071713)                 // slli with funct7 = 1000000
        ILLEGAL(46, 0x00175713 | 0x08000000)    // srli with funct7 = 0000100

        // ---------------------------------------------------------- OP funct7
        ILLEGAL(50, 0x40001733)                 // sll  with funct7 = 0100000
        ILLEGAL(51, 0x40002733)                 // slt  with 0100000
        ILLEGAL(52, 0x40003733)                 // sltu with 0100000
        ILLEGAL(53, 0x40004733)                 // xor  with 0100000 (xnor in Zbb)
        ILLEGAL(54, 0x40006733)                 // or   with 0100000 (orn)
        ILLEGAL(55, 0x40007733)                 // and  with 0100000 (andn)
        ILLEGAL(56, 0x04000733)                 // funct7 = 0000010
        ILLEGAL(57, 0x80000733)                 // funct7 = 1000000
        ILLEGAL(58, 0xfe000733)                 // funct7 = 1111111
        ILLEGAL(59, 0x0e005733)                 // czero.eqz (Zicond)
        ILLEGAL(60, 0x20002733)                 // sh1add (Zba)

        // ---------------------------------------------------------- LOAD / STORE funct3
        ILLEGAL(70, 0x00003703)                 // ld
        ILLEGAL(71, 0x00006703)                 // lwu
        ILLEGAL(72, 0x00007703)
        ILLEGAL(73, 0x00e03023)                 // sd
        ILLEGAL(74, 0x00e04023)
        ILLEGAL(75, 0x00e05023)
        ILLEGAL(76, 0x00e06023)
        ILLEGAL(77, 0x00e07023)

        // ---------------------------------------------------------- BRANCH, JALR, MISC-MEM
        ILLEGAL(80, 0x00002063)                 // branch funct3 = 2
        ILLEGAL(81, 0x00003063)                 // branch funct3 = 3
        ILLEGAL(82, 0x00001767)                 // jalr funct3 = 1
        ILLEGAL(83, 0x00007767)                 // jalr funct3 = 7
        ILLEGAL(84, 0x0000200f)                 // misc-mem funct3 = 2 (cbo.* in Zicbom)
        ILLEGAL(85, 0x0000300f)
        ILLEGAL(86, 0x0000400f)
        ILLEGAL(87, 0x0000500f)
        ILLEGAL(88, 0x0000600f)
        ILLEGAL(89, 0x0000700f)

        // ---------------------------------------------------------- other opcodes
        ILLEGAL(90, 0x1000272f)                 // lr.w (A)
        ILLEGAL(91, 0x00b5272f)                 // amoadd.w
        ILLEGAL(92, 0x00002707)                 // flw
        ILLEGAL(93, 0x00e02027)                 // fsw
        ILLEGAL(94, 0x00000753)                 // fadd.s
        ILLEGAL(95, 0x00000743)                 // fmadd.s
        ILLEGAL(96, 0x00000747)                 // fmsub.s
        ILLEGAL(97, 0x0000074b)                 // fnmsub.s
        ILLEGAL(98, 0x0000074f)                 // fnmadd.s
        ILLEGAL(99, 0x0000073b)                 // addw (RV64)
        ILLEGAL(100, 0x0000071b)                // addiw (RV64)
        ILLEGAL(101, 0x00000757)                // OP-V
        ILLEGAL(102, 0x0000070b)                // custom-0
        ILLEGAL(103, 0x0000072b)                // custom-1
        ILLEGAL(104, 0x0000075b)                // custom-2
        ILLEGAL(105, 0x0000077b)                // custom-3
        ILLEGAL(106, 0x0000076b)                // reserved
        ILLEGAL(107, 0x00000777)                // reserved (OP-P)

        // ---------------------------------------------------------- must not trap
        li      TESTNUM, 120
        mv      x9, t3
        fence
        .word   0x8330000f                      // fence.tso
        .word   0x0100000f                      // pause
        .word   0x0000070f                      // fence with rd = x14
        .word   0x0005800f                      // fence with rs1 = x11
        .word   0xf000000f                      // fence with fm = 1111
        .word   0xffff870f                      // fence: fm, pred, succ, rs1 all ones, rd = x14
        .word   0x0000170f                      // fence.i with rd = x14
        .word   0xfff5900f                      // fence.i with imm and rs1 set
        CHECK_NO_TRAP(121, x9)
        li      TESTNUM, 122                    // and they wrote no register
        li      x7, SENTINEL
        bne     x14, x7, fail
        j       pass

TEST_DATA
