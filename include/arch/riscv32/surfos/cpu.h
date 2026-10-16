/*
SurfOS Processor Features (riscv32)
----------------------
File: cpu.h     Date: 10/15/26 (SurfCPU), the riscv32 face of include/surfos/cpu.h
----------------------
What the machine-mode CSRs say about the hart (misa, the ID registers). There is no
CPUID, no MSR and no paging feature to switch on. Shared code reads two things from
the x86 header: cpu.tsc and rdtsc(), the time stamp counter, which it stirs into
random numbers (driver/part.c); on riscv32 that is the cycle counter, mcycle.
*/

#ifndef _SURFOS_CPU_H
#define _SURFOS_CPU_H

#include <surfos/types.h>
#include <riscv/csr.h>

struct cpu_info {
    u32 misa, mvendorid, marchid, mimpid, mhartid;
    u32 mstatus_reset;                  /* mstatus as the kernel found it */
    bool tsc;                           /* a cycle counter to read: always (mcycle) */
    char isa[48];                       /* from misa: "rv32im" (Zicsr and Zifencei have no misa bits) */
};

extern struct cpu_info cpu;

void init_cpu(void);
void cpu_print(void);

/* the x86 name, kept for shared code: here the 64-bit cycle counter */
static inline u64 rdtsc(void) {
    return rv_read_cycle64();
}

#endif
