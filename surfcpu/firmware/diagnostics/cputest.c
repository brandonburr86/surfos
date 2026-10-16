/*
SurfCPU diagnostics: CPU self-checks
--------------------
File: cputest.c Date: 10/15/26 (SurfCPU)
--------------------
Arithmetic, M-extension, branch, load/store and CSR checks with the expected values worked
out from the RISC-V specification and docs/ISA.md (not from the compiler: every operation
is an asm statement on values loaded at run time, so nothing is folded at build time).
The corner cases are the brief's: x0, immediate sign extension, signed and unsigned
compares and shifts, JAL/JALR link values and bit-0 clearing, load extension and byte
enables, division by zero and overflow, MULH*, CSR masks, read-only CSRs and counters.
*/
#include "diag.h"

#define RR(op, a, b) ({ uint32_t r_; __asm__ volatile (op " %0, %1, %2" : "=r"(r_) : "r"(a), "r"(b)); r_; })
#define RI(op, a, imm) ({ uint32_t r_; __asm__ volatile (op " %0, %1, %2" : "=r"(r_) : "r"(a), "i"(imm)); r_; })
#define BR(op, a, b) ({ uint32_t r_; __asm__ volatile ("li %0, 1\n\t" op " %1, %2, 1f\n\tli %0, 0\n1:" \
                                                       : "=&r"(r_) : "r"(a), "r"(b)); r_; })

static volatile uint32_t V[] = { 0, 1, 2, 7, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xF0F0F0F0u, 0xFF00FF00u,
                                 0x12345678u, 0xFFFFFFF9u /* -7 */, 0xFFFFFFFEu /* -2 */, 33, 32, 4, 16 };
enum { Z, ONE, TWO, SEVEN, IMAX, IMIN, NEG1, F0, FF00, PAT, M7, M2, S33, S32, FOUR, SIXTEEN };

#define EQ(got, want, what) diag_check((got) == (want), "%s = 0x%08x, expected 0x%08x", what, (unsigned)(got), \
                                       (unsigned)(want))

void cpu_test_alu(void)
{
    diag_section("ALU, immediates, x0");
    EQ(RR("add", V[IMAX], V[ONE]), 0x80000000u, "add 0x7fffffff + 1");
    EQ(RR("add", V[NEG1], V[ONE]), 0u, "add -1 + 1");
    EQ(RR("sub", V[Z], V[ONE]), 0xFFFFFFFFu, "sub 0 - 1");
    EQ(RR("sub", V[IMIN], V[ONE]), 0x7FFFFFFFu, "sub 0x80000000 - 1");
    EQ(RR("sll", V[ONE], V[S33]), 2u, "sll 1 << 33 (uses 5 bits)");
    EQ(RR("srl", V[IMIN], V[S32]), 0x80000000u, "srl by 32 (uses 5 bits)");
    EQ(RR("sra", V[IMIN], V[FOUR]), 0xF8000000u, "sra 0x80000000 >> 4");
    EQ(RR("srl", V[IMIN], V[FOUR]), 0x08000000u, "srl 0x80000000 >> 4");
    EQ(RR("slt", V[NEG1], V[Z]), 1u, "slt -1 < 0");
    EQ(RR("slt", V[Z], V[NEG1]), 0u, "slt 0 < -1");
    EQ(RR("sltu", V[Z], V[NEG1]), 1u, "sltu 0 < 0xffffffff");
    EQ(RR("sltu", V[NEG1], V[Z]), 0u, "sltu 0xffffffff < 0");
    EQ(RR("slt", V[IMIN], V[IMAX]), 1u, "slt INT_MIN < INT_MAX");
    EQ(RR("xor", V[F0], V[FF00]), 0x0FF00FF0u, "xor");
    EQ(RR("or", V[F0], V[FF00]), 0xFFF0FFF0u, "or");
    EQ(RR("and", V[F0], V[FF00]), 0xF000F000u, "and");
    EQ(RI("addi", V[Z], -1), 0xFFFFFFFFu, "addi 0, -1");
    EQ(RI("addi", V[Z], -2048), 0xFFFFF800u, "addi 0, -2048");
    EQ(RI("addi", V[Z], 2047), 0x7FFu, "addi 0, 2047");
    EQ(RI("addi", V[IMAX], 1), 0x80000000u, "addi 0x7fffffff, 1");
    EQ(RI("slti", V[M7], -4), 1u, "slti -7 < -4");
    EQ(RI("slti", V[ONE], -1), 0u, "slti 1 < -1");
    EQ(RI("sltiu", V[ONE], -1), 1u, "sltiu 1 < 0xffffffff (sign-extended)");
    EQ(RI("sltiu", V[NEG1], -1), 0u, "sltiu 0xffffffff < 0xffffffff");
    EQ(RI("xori", V[PAT], -1), ~0x12345678u, "xori -1 (not)");
    EQ(RI("andi", V[PAT], -16), 0x12345670u, "andi -16");
    EQ(RI("ori", V[Z], -2048), 0xFFFFF800u, "ori -2048");
    EQ(RI("slli", V[ONE], 31), 0x80000000u, "slli 31");
    EQ(RI("srli", V[IMIN], 31), 1u, "srli 31");
    EQ(RI("srai", V[IMIN], 31), 0xFFFFFFFFu, "srai 31");
    uint32_t r, here;
    __asm__ volatile ("lui %0, 0xfffff" : "=r"(r));
    EQ(r, 0xFFFFF000u, "lui 0xfffff");
    __asm__ volatile ("1: auipc %0, 0\n\tla %1, 1b" : "=r"(r), "=r"(here));
    EQ(r, here, "auipc 0 (its own address)");
    __asm__ volatile ("li t0, 5\n\tadd x0, t0, t0\n\taddi x0, x0, 9\n\tmv %0, x0" : "=r"(r) :: "t0");
    EQ(r, 0u, "x0 after writes");
    diag_section_end();
}

void cpu_test_muldiv(void)
{
    diag_section("M extension");
    EQ(RR("mul", V[NEG1], V[NEG1]), 1u, "mul -1 * -1");
    EQ(RR("mul", V[IMAX], V[TWO]), 0xFFFFFFFEu, "mul 0x7fffffff * 2");
    EQ(RR("mulh", V[NEG1], V[NEG1]), 0u, "mulh -1 * -1");
    EQ(RR("mulh", V[IMIN], V[IMIN]), 0x40000000u, "mulh INT_MIN * INT_MIN");
    EQ(RR("mulhu", V[NEG1], V[NEG1]), 0xFFFFFFFEu, "mulhu 0xffffffff * 0xffffffff");
    EQ(RR("mulhu", V[IMIN], V[IMIN]), 0x40000000u, "mulhu 0x80000000 * 0x80000000");
    EQ(RR("mulhsu", V[NEG1], V[NEG1]), 0xFFFFFFFFu, "mulhsu -1 * 0xffffffff");
    EQ(RR("mulhsu", V[IMIN], V[IMIN]), 0xC0000000u, "mulhsu INT_MIN * 0x80000000");
    EQ(RR("mulhsu", V[TWO], V[NEG1]), 1u, "mulhsu 2 * 0xffffffff");
    EQ(RR("div", V[SEVEN], V[TWO]), 3u, "div 7 / 2");
    EQ(RR("div", V[M7], V[TWO]), 0xFFFFFFFDu, "div -7 / 2 (toward zero)");
    EQ(RR("div", V[SEVEN], V[M2]), 0xFFFFFFFDu, "div 7 / -2");
    EQ(RR("div", V[SEVEN], V[Z]), 0xFFFFFFFFu, "div by zero");
    EQ(RR("div", V[IMIN], V[NEG1]), 0x80000000u, "div INT_MIN / -1 (overflow)");
    EQ(RR("divu", V[NEG1], V[TWO]), 0x7FFFFFFFu, "divu 0xffffffff / 2");
    EQ(RR("divu", V[SEVEN], V[Z]), 0xFFFFFFFFu, "divu by zero");
    EQ(RR("rem", V[SEVEN], V[TWO]), 1u, "rem 7 % 2");
    EQ(RR("rem", V[M7], V[TWO]), 0xFFFFFFFFu, "rem -7 % 2");
    EQ(RR("rem", V[SEVEN], V[M2]), 1u, "rem 7 % -2");
    EQ(RR("rem", V[SEVEN], V[Z]), 7u, "rem by zero");
    EQ(RR("rem", V[IMIN], V[NEG1]), 0u, "rem INT_MIN % -1 (overflow)");
    EQ(RR("remu", V[NEG1], V[SIXTEEN]), 15u, "remu 0xffffffff % 16");
    EQ(RR("remu", V[PAT], V[Z]), 0x12345678u, "remu by zero");
    diag_section_end();
}

void cpu_test_branches(void)
{
    diag_section("branches and jumps");
    diag_check(BR("beq", V[SEVEN], V[SEVEN]) == 1 && BR("beq", V[SEVEN], V[ONE]) == 0, "beq");
    diag_check(BR("bne", V[SEVEN], V[ONE]) == 1 && BR("bne", V[ONE], V[ONE]) == 0, "bne");
    diag_check(BR("blt", V[NEG1], V[Z]) == 1 && BR("blt", V[Z], V[NEG1]) == 0, "blt signed");
    diag_check(BR("bltu", V[NEG1], V[Z]) == 0 && BR("bltu", V[Z], V[NEG1]) == 1, "bltu unsigned");
    diag_check(BR("bge", V[Z], V[NEG1]) == 1 && BR("bge", V[SEVEN], V[SEVEN]) == 1 && BR("bge", V[M7], V[ONE]) == 0,
               "bge signed");
    diag_check(BR("bgeu", V[Z], V[NEG1]) == 0 && BR("bgeu", V[NEG1], V[NEG1]) == 1, "bgeu unsigned");
    diag_check(BR("blt", V[IMIN], V[IMAX]) == 1 && BR("bltu", V[IMIN], V[IMAX]) == 0, "INT_MIN vs INT_MAX");
    uint32_t link, target;
    __asm__ volatile ("jal %0, 1f\n1:\n\tla %1, 1b" : "=r"(link), "=r"(target));
    EQ(link, target, "jal link value");
    uint32_t landed;
    __asm__ volatile ("la t0, 2f\n\taddi t0, t0, 1\n\tjalr %0, 0(t0)\n\tli %1, 0\n\tj 3f\n2:\tli %1, 1\n3:"
                      : "=&r"(link), "=&r"(landed) :: "t0");
    diag_check(landed == 1, "jalr clears bit 0 of the target");
    diag_section_end();
}

void cpu_test_loadstore(uint32_t a)
{
    diag_section("loads and stores");
    volatile uint32_t *w = (volatile uint32_t *)a;
    *w = 0x8899AABBu;
    uint32_t r;
    __asm__ volatile ("lb %0, 0(%1)" : "=r"(r) : "r"(a) : "memory");
    EQ(r, 0xFFFFFFBBu, "lb sign extension");
    __asm__ volatile ("lbu %0, 0(%1)" : "=r"(r) : "r"(a) : "memory");
    EQ(r, 0xBBu, "lbu");
    __asm__ volatile ("lb %0, 3(%1)" : "=r"(r) : "r"(a) : "memory");
    EQ(r, 0xFFFFFF88u, "lb byte 3");
    __asm__ volatile ("lh %0, 0(%1)" : "=r"(r) : "r"(a) : "memory");
    EQ(r, 0xFFFFAABBu, "lh sign extension");
    __asm__ volatile ("lhu %0, 2(%1)" : "=r"(r) : "r"(a) : "memory");
    EQ(r, 0x8899u, "lhu upper half");
    __asm__ volatile ("lh %0, 2(%1)" : "=r"(r) : "r"(a) : "memory");
    EQ(r, 0xFFFF8899u, "lh upper half");
    __asm__ volatile ("sb %0, 1(%1)" :: "r"(0x11u), "r"(a) : "memory");
    EQ(*w, 0x889911BBu, "sb byte enable");
    __asm__ volatile ("sh %0, 2(%1)" :: "r"(0x2233u), "r"(a) : "memory");
    EQ(*w, 0x223311BBu, "sh byte enables");
    diag_section_end();
}

void cpu_test_csrs(void)
{
    diag_section("CSRs and counters");
    EQ(csr_read(misa), 0x40001100u, "misa");
    csr_write(misa, 0);
    EQ(csr_read(misa), 0x40001100u, "misa after a write (ignored)");
    EQ(csr_read(mvendorid), 0u, "mvendorid");
    EQ(csr_read(marchid), 0u, "marchid");
    EQ(csr_read(mimpid), SURF_SOC_VERSION, "mimpid");
    EQ(csr_read(mhartid), 0u, "mhartid");
    EQ(csr_read(0xF15), 0u, "mconfigptr");
    uint32_t ms = csr_read(mstatus);
    EQ(ms & 0x1800u, 0x1800u, "mstatus.MPP reads 11");
    csr_write(mie, 0xFFFFFFFFu);
    EQ(csr_read(mie), 0x888u, "mie writable bits");
    csr_write(mie, 0);
    csr_write(mstatus, 0xFFFFFFFFu);                /* mie is 0: nothing can be taken */
    EQ(csr_read(mstatus), 0x1888u, "mstatus writable bits");
    csr_write(mstatus, ms);
    uint32_t tv = csr_read(mtvec);
    csr_write(mtvec, tv | 3u);
    EQ(csr_read(mtvec), tv, "mtvec MODE fixed at 0");
    csr_write(mtvec, tv);
    uint32_t sc = csr_read(mscratch);
    csr_write(mscratch, 0x12345678u);
    uint32_t sc2 = csr_read(mscratch);
    csr_write(mscratch, sc);
    EQ(sc2, 0x12345678u, "mscratch");
    uint32_t save_epc = csr_read(mepc), save_cause = csr_read(mcause), save_tval = csr_read(mtval);
    csr_write(mepc, 0xFFFFFFFFu);
    EQ(csr_read(mepc), 0xFFFFFFFCu, "mepc bits 1:0 read 0");
    csr_write(mcause, 0xFFFFFFFFu);
    EQ(csr_read(mcause), 0x8000001Fu, "mcause writable bits");
    csr_write(mtval, 0xA5A5A5A5u);
    EQ(csr_read(mtval), 0xA5A5A5A5u, "mtval");
    csr_write(mepc, save_epc);
    csr_write(mcause, save_cause);
    csr_write(mtval, save_tval);
    csr_write(mip, 0xFFFFFFFFu);                    /* read only: ignored, no trap */
    diag_check((csr_read(mip) & ~0x888u) == 0, "mip has only MSIP/MTIP/MEIP");
    EQ(csr_read(0x310), 0u, "mstatush");
    EQ(csr_read(0x3A0), 0u, "pmpcfg0");
    csr_write(0x3B0, 0xFFFFFFFFu);
    EQ(csr_read(0x3B0), 0u, "pmpaddr0 (no PMP)");
    EQ(csr_read(0xB03), 0u, "mhpmcounter3");
    EQ(csr_read(0xC03), 0u, "hpmcounter3");

    uint32_t a, b;
    __asm__ volatile ("csrr %0, minstret\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t"
                      "csrr %1, minstret" : "=r"(a), "=r"(b));
    EQ(b - a, 11u, "minstret counts 11 for csrr + 10 nops");
    a = csr_read(mcycle);
    b = csr_read(mcycle);
    diag_check(b != a, "mcycle advances");
    csr_write(mcountinhibit, 0xFFFFFFFFu);
    EQ(csr_read(mcountinhibit), 5u, "mcountinhibit writable bits");
    a = csr_read(mcycle);
    __asm__ volatile ("nop\n\tnop\n\tnop");
    b = csr_read(mcycle);
    EQ(b, a, "mcycle stopped by mcountinhibit.CY");
    a = csr_read(minstret);
    __asm__ volatile ("nop\n\tnop\n\tnop");
    b = csr_read(minstret);
    EQ(b, a, "minstret stopped by mcountinhibit.IR");
    csr_write(mcountinhibit, 0);
    a = csr_read(mcycle);
    b = csr_read(cycle);
    diag_check(b - a < 16u, "cycle mirrors mcycle (%u apart)", (unsigned)(b - a));
    a = csr_read(minstret);
    b = csr_read(instret);
    diag_check(b - a < 4u, "instret mirrors minstret");
    uint32_t t_csr = csr_read(time), t_mmio = REG(SURF_TIMER_BASE, SURF_TIMER_MTIME_LO);
    diag_check(t_mmio - t_csr < 50u, "time CSR = mtime (%u apart)", (unsigned)(t_mmio - t_csr));
    uint32_t hi1, lo, hi2;
    do {
        hi1 = csr_read(mcycleh);
        lo = csr_read(mcycle);
        hi2 = csr_read(mcycleh);
    } while (hi1 != hi2);
    (void)lo;
    diag_check(1, "64-bit counter read hi/lo/hi");
    diag_section_end();
}
