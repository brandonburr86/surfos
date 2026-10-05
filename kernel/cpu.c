/*
SurfOS Processor Features
--------------------
File: cpu.c     Date: 10/5/26 (USB boot)
--------------------
See include/surfos/cpu.h. A 486 without CPUID still boots: every feature reads as
absent and nothing here is used.
*/

#include <surfos/types.h>
#include <surfos/cpu.h>
#include <surfos/console.h>
#include <blibc_common.h>

struct cpu_info cpu;

/* CPUID exists when the ID flag (EFLAGS bit 21) can be toggled */
static bool detect_cpuid(void) {
    u_long a, b;
    asm volatile("pushfl\n\t"
                 "popl %0\n\t"
                 "movl %0, %1\n\t"
                 "xorl $0x200000, %0\n\t"
                 "pushl %0\n\t"
                 "popfl\n\t"
                 "pushfl\n\t"
                 "popl %0\n\t"
                 "pushl %1\n\t"
                 "popfl"
                 : "=&r"(a), "=&r"(b) : : "cc");
    return ((a ^ b) & 0x200000) ? true : false;
}

void cpuid(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    u32 ra, rb, rc, rd;
    asm volatile("cpuid" : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd) : "a"(leaf), "c"(subleaf));
    if(a) *a = ra;
    if(b) *b = rb;
    if(c) *c = rc;
    if(d) *d = rd;
}

u64 rdmsr(u32 msr) {
    u32 lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

void wrmsr(u32 msr, u64 value) {
    asm volatile("wrmsr" : : "c"(msr), "a"((u32)value), "d"((u32)(value >> 32)) : "memory");
}

static void read_brand(void) {
    u32 r[12];
    int i;
    char *s;
    if(cpu.max_ext_leaf < 0x80000004) return;
    for(i = 0; i < 3; i++) cpuid(0x80000002 + i, 0, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
    memcpy(cpu.brand, r, 48);
    cpu.brand[48] = 0;
    for(s = cpu.brand; *s == ' '; s++);         /* Intel pads on the left */
    memmove(cpu.brand, s, strlen(s) + 1);
}

/* CR0 as the 2004 kernel always had it from QEMU's loader (protected mode, no write
   protection, caches on), plus a working x87: Multiboot 2 leaves CR0 undefined, and UEFI
   firmware runs with WP set, which would fault the kernel's writes into read-only user
   pages while it loads a program. Paging is still off here. */
static void set_cr0(void) {
    u_long cr0;
    asm volatile("movl %%cr0, %0" : "=r"(cr0));
    cpu.boot_cr0 = cr0;
    cr0 &= ~(CR0_EM | CR0_TS | CR0_WP | CR0_AM | CR0_NW | CR0_CD);
    cr0 |= CR0_PE | CR0_MP | CR0_ET | CR0_NE;
    asm volatile("movl %0, %%cr0" : : "r"(cr0) : "memory");
    asm volatile("fninit");                     /* every x87 exception masked */
}

void init_cpu(void) {
    u32 a, b, c, d;

    memset(&cpu, 0, sizeof(cpu));
    cpu.phys_bits = 36;
    cpu.has_cpuid = detect_cpuid();
    if(cpu.has_cpuid) {                         /* CR4 exists wherever CPUID does */
        asm volatile("movl %%cr4, %0" : "=r"(cpu.boot_cr4));
        /* CR4 first: CR0.WP cannot be cleared while CR4.CET is set (#GP, and no IDT yet) */
        asm volatile("movl %0, %%cr4" : : "r"(cpu.boot_cr4 & ~CR4_CLEAR) : "memory");
    }
    set_cr0();
    if(!cpu.has_cpuid) {
        kprintf("CPU: no CPUID instruction (a 486?)\n");
        return;
    }
    cpuid(0, 0, &a, &b, &c, &d);
    cpu.max_leaf = a;
    memcpy(cpu.vendor, &b, 4);
    memcpy(cpu.vendor + 4, &d, 4);
    memcpy(cpu.vendor + 8, &c, 4);
    if(cpu.max_leaf >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        cpu.family = (a >> 8) & 0xF;
        cpu.model = (a >> 4) & 0xF;
        cpu.stepping = a & 0xF;
        if(cpu.family == 0xF) cpu.family += (a >> 20) & 0xFF;
        if(cpu.family == 0x6 || cpu.family >= 0xF) cpu.model += ((a >> 16) & 0xF) << 4;
        cpu.features_edx = d;
        cpu.features_ecx = c;
        cpu.fpu = (d >> 0) & 1;
        cpu.pse = (d >> 3) & 1;
        cpu.tsc = (d >> 4) & 1;
        cpu.msr = (d >> 5) & 1;
        cpu.pae = (d >> 6) & 1;
        cpu.apic = (d >> 9) & 1;
        cpu.pat = (d >> 16) & 1;
        cpu.pse36 = (d >> 17) & 1;
    }
    cpuid(0x80000000, 0, &a, 0, 0, 0);
    cpu.max_ext_leaf = (a & 0x80000000) ? a : 0;
    if(cpu.max_ext_leaf >= 0x80000007) {
        cpuid(0x80000007, 0, 0, 0, 0, &d);
        cpu.invariant_tsc = (d >> 8) & 1;
    }
    if(cpu.max_ext_leaf >= 0x80000008) {
        cpuid(0x80000008, 0, &a, 0, 0, 0);
        if((a & 0xFF) >= 32) cpu.phys_bits = a & 0xFF;
    }
    read_brand();

    /* CR4 as this kernel expects it, whatever the firmware enabled: in particular SMAP would
       fault every system call that reads user memory and PVI would let ring 3 run cli. Only
       4 MB pages are added (the framebuffer window, mm/paging.c). */
    {
        u_long cr4 = cpu.boot_cr4 & ~CR4_CLEAR;
        if(cpu.pse) cr4 |= CR4_PSE;
        asm volatile("movl %0, %%cr4" : : "r"(cr4) : "memory");
        cpu.pse_on = cpu.pse;
    }
    /* PAT entry 4 (PAT bit set, PCD and PWT clear) becomes write-combining; nothing used it
       before, and paging is still off, so no stale mapping can disagree */
    if(cpu.pat && cpu.msr) {
        u64 pat = rdmsr(MSR_PAT);
        pat = (pat & ~(0xFFULL << 32)) | (0x01ULL << 32);
        wrmsr(MSR_PAT, pat);
        cpu.pat_wc = true;
    }
    kprintf("CPU: %s%s%s, family %u model 0x%x, %u physical address bits\n", cpu.vendor, cpu.brand[0] ? " " : "",
            cpu.brand, cpu.family, cpu.model, cpu.phys_bits);
}

void cpu_print(void) {
    if(!cpu.has_cpuid) {
        printf("    no CPUID instruction\n");
        return;
    }
    printf("    vendor   %s\n", cpu.vendor);
    printf("    model    %s\n", cpu.brand[0] ? cpu.brand : "(no brand string)");
    printf("    family %u, model 0x%x, stepping %u; CPUID leaves up to 0x%x and 0x%x\n", cpu.family, cpu.model,
           cpu.stepping, cpu.max_leaf, cpu.max_ext_leaf);
    printf("    physical address bits %u\n", cpu.phys_bits);
    printf("    features%s%s%s%s%s%s%s%s%s\n", cpu.fpu ? " fpu" : "", cpu.pse ? " pse" : "", cpu.tsc ? " tsc" : "",
           cpu.msr ? " msr" : "", cpu.pae ? " pae" : "", cpu.apic ? " apic" : "", cpu.pat ? " pat" : "",
           cpu.pse36 ? " pse36" : "", cpu.invariant_tsc ? " invariant-tsc" : "");
    printf("    4 MB pages %s, framebuffer %s\n", cpu.pse_on ? "on" : "off",
           cpu.pat_wc ? "write-combining (PAT)" : "uncached");
    printf("    from the boot loader: CR0 0x%08lx, CR4 0x%08lx\n", cpu.boot_cr0, cpu.boot_cr4);
}
