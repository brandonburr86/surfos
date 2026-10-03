/*
    SurfOS Paging Header -- (C)2004 Brandon Burr, VMM API 10/2026 (roadmap M2)
*/

#ifndef _MM_PAGING_H
#define _MM_PAGING_H

#include <surfos/types.h>
#include <surfos/trap.h>

/* page table entry bits */
#define PTE_P 0x001   /* present */
#define PTE_W 0x002   /* writable */
#define PTE_U 0x004   /* user accessible */
#define PTE_PWT 0x008 /* write-through */
#define PTE_PCD 0x010 /* cache disabled: device memory */
#define PTE_A 0x020   /* accessed */
#define PTE_D 0x040   /* dirty */
#define PTE_FRAME 0xFFFFF000

/* page fault error code bits */
#define PF_PRESENT 0x1  /* 0 = not-present page, 1 = protection violation */
#define PF_WRITE   0x2
#define PF_USER    0x4

#define PAGE_ALIGN_DOWN(a) ((a) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(a)   (((a) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

void init_paging(void);

/* kernel address space */
void vmm_map(u_long virt, u_long phys, u_long flags);   /* flags: PTE_W, PTE_U */
void vmm_unmap(u_long virt);                           /* does not free the frame */
u_long vmm_get_phys(u_long virt);                      /* 0 when not mapped */
bool vmm_is_mapped(u_long virt);
void *mm_lookup_linear(void *linear);                  /* 2004 name: linear to physical */
void *ioremap(u_long phys, u_long size, bool cached);  /* map device memory or a module; never unmapped */

u_long *page_fault_trap(struct trapframe *tf);

extern u_long *page_directory;
extern u_long *page_table;

#endif
