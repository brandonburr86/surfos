/*
SurfOS Paging
Copyright (C)2004 Brandon Burr, VMM API 10/2026 (roadmap M2)

The kernel address space is one 4 MB page-table array at PTBL_START that covers all
4 GB (so a page fault only ever fills a table entry, never a directory entry), with
the first 16 MB mapped 1:1 and the kernel heap at KHEAP_START mapped on demand.
Everything else that faults is a bug. Page 0 is unmapped once the drivers have read
the BIOS data area, so a NULL dereference faults (audit M6).
*/

#include <surfos/types.h>
#include <surfos/bit.h>
#include <surfos/panic.h>
#include <surfos/console.h>
#include <mm/memory.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <surfos/trap.h>
#include <blibc_common.h>

static u_long pd_storage[1024] __attribute__((aligned(4096))); /* used to live at 0x9C000, inside the EBDA on some machines */
u_long *page_directory = pd_storage;
u_long *page_table = (u_long*)PTBL_START;   /* 1024 tables back to back: entry i maps virtual i << 12 */

#define IDENTITY_PAGES (IDENTITY_END / PAGE_SIZE)

static void enablePaging(void) {
    asm volatile("movl %0, %%cr3" : : "r"((u_long)page_directory));
    asm volatile("movl %%cr0, %%eax\n"
                 "orl $0x80000000, %%eax\n"
                 "movl %%eax, %%cr0\n"
                 : : : "eax", "memory");
}

static inline void invlpg(u_long virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void init_paging() {
    u_long i;

    memset(page_table, 0, PTBL_SIZE);

    /* every directory entry points at its slice of the big table, so the directory never
       changes after this and a user process (roadmap P1) can share the kernel half of it */
    for(i = 0; i < 1024; i++) {
        page_directory[i] = ((u_long)page_table + i * PAGE_SIZE) | PTE_P | PTE_W;
    }

    /* the kernel area: 1:1, supervisor only */
    for(i = 0; i < IDENTITY_PAGES; i++) {
        page_table[i] = (i << 12) | PTE_P | PTE_W;
    }

    enablePaging();
}

void vmm_map(u_long virt, u_long phys, u_long flags) {
    page_table[virt >> 12] = (phys & PTE_FRAME) | (flags & (PTE_W | PTE_U)) | PTE_P;
    invlpg(virt);
}

void vmm_unmap(u_long virt) {
    page_table[virt >> 12] = 0;
    invlpg(virt);
}

u_long vmm_get_phys(u_long virt) {
    u_long pte = page_table[virt >> 12];
    if(!(pte & PTE_P)) return 0;
    return (pte & PTE_FRAME) | (virt & (PAGE_SIZE - 1));
}

/* Is a linear address backed by a present page? The backtrace checks every frame with this
   so a corrupt frame pointer cannot fault inside a panic. */
bool vmm_is_mapped(u_long virt) {
    if(!(page_directory[virt >> 22] & PTE_P)) return false;
    return (page_table[virt >> 12] & PTE_P) ? true : false;
}

void *mm_lookup_linear(void *linear) {
    return (void *)vmm_get_phys((u_long)linear);
}

/*
 ahh.. the infamous page fault. The kernel heap is mapped a page at a time as it is
 touched (the 2004 demand paging). Any other fault is reported like an exception.
 */
u_long *page_fault_trap(struct trapframe *tf) {
    u_long cr2, err = tf->errcode;
    asm volatile("movl %%cr2, %0" : "=r"(cr2));

    if(!(err & PF_PRESENT) && !(err & PF_USER) && cr2 >= KHEAP_START && cr2 < KHEAP_END) {
        u_long frame = pmm_alloc();
        if(!frame) panic("out of memory: no frame for kernel heap page 0x%08lx", PAGE_ALIGN_DOWN(cr2));
        vmm_map(PAGE_ALIGN_DOWN(cr2), frame, PTE_W);
        return (u_long*)tf;
    }

    return trap_fatal(tf, "Page Fault at 0x%08lx: %s %s%s", cr2,
                      (err & PF_PRESENT) ? "protection violation on" : "access to unmapped page,",
                      (err & PF_WRITE) ? "write" : "read",
                      (err & PF_USER) ? " from user mode" : "");
}
