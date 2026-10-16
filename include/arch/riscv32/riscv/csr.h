/*
SurfOS riscv32: machine-mode CSRs
----------------------
File: csr.h     Date: 10/15/26 (SurfCPU)
----------------------
The control and status registers the machine-mode kernel uses, with the bits of
surfcpu/docs/ISA.md (Machine ISA 1.13, M mode only). Usable from assembly too.
*/

#ifndef _RISCV_CSR_H
#define _RISCV_CSR_H

/* mstatus: MPP is fixed at 11 (M) on SurfCPU; MIE and MPIE are the writable bits */
#define MSTATUS_MIE   0x00000008
#define MSTATUS_MPIE  0x00000080
#define MSTATUS_MPP   0x00001800

/* mie / mip */
#define MIE_MSIE      0x00000008
#define MIE_MTIE      0x00000080
#define MIE_MEIE      0x00000800

/* mcause */
#define MCAUSE_INTR          0x80000000
#define MCAUSE_CODE(c)       ((c) & 0x7FFFFFFF)
#define CAUSE_MISALIGNED_FETCH  0
#define CAUSE_FETCH_ACCESS      1
#define CAUSE_ILLEGAL_INSN      2
#define CAUSE_BREAKPOINT        3
#define CAUSE_MISALIGNED_LOAD   4
#define CAUSE_LOAD_ACCESS       5
#define CAUSE_MISALIGNED_STORE  6
#define CAUSE_STORE_ACCESS      7
#define CAUSE_ECALL_M           11
#define IRQ_M_SOFT              3
#define IRQ_M_TIMER             7
#define IRQ_M_EXT               11

#ifndef __ASSEMBLER__

#define csr_read(csr) ({ unsigned long __v; asm volatile("csrr %0, " #csr : "=r"(__v) : : "memory"); __v; })
#define csr_write(csr, val) ({ unsigned long __v = (unsigned long)(val); asm volatile("csrw " #csr ", %0" : : "rK"(__v) : "memory"); })
#define csr_set(csr, bits) ({ unsigned long __v = (unsigned long)(bits); asm volatile("csrs " #csr ", %0" : : "rK"(__v) : "memory"); })
#define csr_clear(csr, bits) ({ unsigned long __v = (unsigned long)(bits); asm volatile("csrc " #csr ", %0" : : "rK"(__v) : "memory"); })
/* read the old value and clear bits in one instruction (csrrc): the interrupt-disable primitive */
#define csr_read_clear(csr, bits) ({ unsigned long __v = (unsigned long)(bits); \
    asm volatile("csrrc %0, " #csr ", %1" : "=r"(__v) : "rK"(__v) : "memory"); __v; })

/* 64-bit counters on RV32: high, low, high again, retried when the high half moved (ISA.md 2) */
static inline unsigned long long rv_read_cycle64(void) {
    unsigned long hi, lo, hi2;
    do {
        hi = csr_read(mcycleh);
        lo = csr_read(mcycle);
        hi2 = csr_read(mcycleh);
    } while(hi != hi2);
    return ((unsigned long long)hi << 32) | lo;
}

static inline unsigned long long rv_read_instret64(void) {
    unsigned long hi, lo, hi2;
    do {
        hi = csr_read(minstreth);
        lo = csr_read(minstret);
        hi2 = csr_read(minstreth);
    } while(hi != hi2);
    return ((unsigned long long)hi << 32) | lo;
}

static inline unsigned long rv_read_sp(void) {
    unsigned long sp;
    asm volatile("mv %0, sp" : "=r"(sp));
    return sp;
}

static inline unsigned long rv_read_fp(void) {
    unsigned long fp;
    asm volatile("mv %0, s0" : "=r"(fp));
    return fp;
}

static inline unsigned long rv_read_gp(void) {
    unsigned long gp;
    asm volatile("mv %0, gp" : "=r"(gp));
    return gp;
}

static inline unsigned long rv_read_tp(void) {
    unsigned long tp;
    asm volatile("mv %0, tp" : "=r"(tp));
    return tp;
}

static inline void rv_wfi(void) { asm volatile("wfi" : : : "memory"); }
static inline void rv_fence(void) { asm volatile("fence" : : : "memory"); }
static inline void rv_fence_i(void) { asm volatile("fence.i" : : : "memory"); }

#endif /* !__ASSEMBLER__ */
#endif
