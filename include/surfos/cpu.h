/*
SurfOS Processor Features
----------------------
File: cpu.h     Date: 10/5/26 (USB boot)
----------------------
CPUID, MSRs and the two paging features the laptop port needs: 4 MB pages with
PSE-36, which let 32-bit paging reach a framebuffer the firmware put above 4 GB, and
PAT, which makes that framebuffer write-combining instead of uncached.
*/

#ifndef _SURFOS_CPU_H
#define _SURFOS_CPU_H

#include <surfos/types.h>

struct cpu_info {
    bool has_cpuid;
    char vendor[13];
    char brand[49];
    u32 max_leaf, max_ext_leaf;
    u32 family, model, stepping;
    u32 features_edx, features_ecx;     /* CPUID 1 */
    u32 phys_bits;                      /* MAXPHYADDR (36 when the CPU does not report it) */
    bool fpu, pse, tsc, msr, pae, apic, pse36, pat, invariant_tsc;
    bool pse_on;                        /* CR4.PSE set: 4 MB pages usable */
    bool pat_wc;                        /* PAT entry 4 is write-combining */
    u_long boot_cr0, boot_cr4;          /* as the boot loader left them (cpuinfo shows them) */
};

#define CR0_PE 0x00000001
#define CR0_MP 0x00000002
#define CR0_EM 0x00000004
#define CR0_TS 0x00000008
#define CR0_ET 0x00000010
#define CR0_NE 0x00000020
#define CR0_WP 0x00010000
#define CR0_AM 0x00040000
#define CR0_NW 0x20000000
#define CR0_CD 0x40000000

#define CR4_PSE 0x00000010
/* cleared at boot: VME PVI TSD PAE PGE PCE UMIP PCIDE SMEP SMAP PKE CET PKS */
#define CR4_CLEAR (0x1 | 0x2 | 0x4 | 0x20 | 0x80 | 0x100 | 0x800 | 0x20000 | 0x100000 | 0x200000 | 0x400000 | 0x800000 | 0x1000000)

extern struct cpu_info cpu;

void init_cpu(void);                    /* before init_mem(): CR4.PSE and PAT are set here */
void cpu_print(void);

void cpuid(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c, u32 *d);
u64 rdmsr(u32 msr);
void wrmsr(u32 msr, u64 value);

static inline u64 rdtsc(void) {
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

#define MSR_APIC_BASE 0x1B
#define MSR_PAT       0x277

#endif
