/*
SurfOS GDT Handler
----------------------
File: gdt.h Date: Prior to 4/23/04, rebuilt 10/2026 (roadmap K2)
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_GDT_H
#define _SURFOS_GDT_H

#include <surfos/types.h>

/* selectors: index * 8, plus the requested privilege level for the ring-3 ones */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   0x1B
#define USER_DS   0x23
#define TSS_SEL   0x28
#define DF_TSS_SEL 0x30     /* 10/25/26 (robustness, L16): the double fault's task, reached through a task gate */

#define GDT_ENTRIES 7

/* a 32-bit task-state segment. SurfOS switches tasks in software, so the main one holds only the ring-0 stack for
   traps from ring 3 (and an I/O bitmap offset past its end); the double fault's task switch saves the interrupted
   state in it, where kernel/panic.c reads it */
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

void init_gdt(void);
void tss_set_kernel_stack(u_long esp0); /* the stack the CPU switches to on a trap from ring 3 */
const struct tss_entry *tss_main(void); /* 10/25/26 (L16): the main TSS: after a double fault, what it interrupted */

#endif
