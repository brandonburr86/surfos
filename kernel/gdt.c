/*
SurfOS GDT Handler
--------------------
File: gdt.c Date: Prior to 4/23/04, rebuilt 10/2026 (roadmap K2)
--------------------
(C)2004 Brandon Burr

The 2004 version built the table at physical 0x6000 through a bitfield struct and
blanked the ring-3 entries again by accident (audit I8). The table and the TSS are
now ordinary kernel data: null, kernel code/data, user code/data, one TSS.
*/

#include <surfos/gdt.h>
#include <surfos/console.h>
#include <blibc_common.h>

struct gdt_entry {
    u_short limit_low;
    u_short base_low;
    u_char  base_mid;
    u_char  access;
    u_char  gran;      /* limit 19:16 in the low nibble, flags in the high nibble */
    u_char  base_high;
} __attribute__((packed));

struct gdt_ptr {
    u_short limit;
    u_long  base;
} __attribute__((packed));

struct tss_entry {
    u_long prev_tss;
    u_long esp0, ss0;
    u_long esp1, ss1;
    u_long esp2, ss2;
    u_long cr3, eip, eflags;
    u_long eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u_long es, cs, ss, ds, fs, gs;
    u_long ldt;
    u_short trap, iomap_base;
} __attribute__((packed));

/* access byte: present | DPL | S | type */
#define ACC_KCODE 0x9A  /* present, ring 0, code, readable */
#define ACC_KDATA 0x92  /* present, ring 0, data, writable */
#define ACC_UCODE 0xFA  /* present, ring 3, code, readable */
#define ACC_UDATA 0xF2  /* present, ring 3, data, writable */
#define ACC_TSS   0x89  /* present, ring 0, 32-bit TSS (available) */
#define FLAG_4K32 0xC   /* 4 KB granularity, 32-bit segment */

static struct gdt_entry gdt[GDT_ENTRIES] __attribute__((aligned(8)));
static struct tss_entry tss __attribute__((aligned(16)));

static void gdt_set(int i, u_long base, u_long limit, u_char access, u_char flags) {
    gdt[i].limit_low = limit & 0xFFFF;
    gdt[i].base_low  = base & 0xFFFF;
    gdt[i].base_mid  = (base >> 16) & 0xFF;
    gdt[i].access    = access;
    gdt[i].gran      = ((limit >> 16) & 0x0F) | ((flags & 0x0F) << 4);
    gdt[i].base_high = (base >> 24) & 0xFF;
}

void init_gdt() {
    struct gdt_ptr gdtr;

    kprintf("GDT Initialization\n");
    gdt_set(0, 0, 0, 0, 0);                              /* null descriptor */
    gdt_set(1, 0, 0xFFFFF, ACC_KCODE, FLAG_4K32);       /* 0x08 kernel code, 4 GB flat */
    gdt_set(2, 0, 0xFFFFF, ACC_KDATA, FLAG_4K32);       /* 0x10 kernel data */
    gdt_set(3, 0, 0xFFFFF, ACC_UCODE, FLAG_4K32);       /* 0x1B user code */
    gdt_set(4, 0, 0xFFFFF, ACC_UDATA, FLAG_4K32);       /* 0x23 user data */

    memset(&tss, 0, sizeof(tss));
    tss.ss0 = KERNEL_DS;
    tss.esp0 = 0;                                       /* set per task once ring 3 exists */
    tss.iomap_base = sizeof(tss);                       /* no I/O permission bitmap */
    gdt_set(5, (u_long)&tss, sizeof(tss) - 1, ACC_TSS, 0); /* 0x28 */
    kprintf("*GDT Populated\n");

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (u_long)gdt;
    asm volatile("lgdt %0" : : "m"(gdtr));

    /* Reload CS with a far jump, then the data segments. Until this the CPU runs on the
       boot loader's cached code descriptor: QEMU's -kernel loader happens to use 0x08,
       GRUB 2 uses 0x10, which is a data segment here, so the first iret would fault. */
    asm volatile("ljmp %0, $1f\n"
                 "1:\n"
                 "mov %1, %%ax\n"
                 "mov %%ax, %%ds\n"
                 "mov %%ax, %%es\n"
                 "mov %%ax, %%fs\n"
                 "mov %%ax, %%gs\n"
                 "mov %%ax, %%ss\n"
                 : : "i"(KERNEL_CS), "i"(KERNEL_DS) : "eax", "memory");
    kprintf("*GDT Loaded, segment registers initialized\n");

    asm volatile("ltr %%ax" : : "a"((u_short)TSS_SEL));
    kprintf("*TSS Loaded\n");

    kprintf("*DONE\n\n");
}

void tss_set_kernel_stack(u_long esp0) {
    tss.esp0 = esp0;
}
