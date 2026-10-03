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

#define GDT_ENTRIES 6

void init_gdt(void);
void tss_set_kernel_stack(u_long esp0); /* the stack the CPU switches to on a trap from ring 3 */

#endif
