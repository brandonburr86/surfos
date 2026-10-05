/*
SurfOS Paging
Copyright (C)2004 Brandon Burr, VMM API 10/2026 (roadmap M2)

The kernel address space is one 4 MB page-table array (an arena pmm_init() places in
free RAM) that covers all 4 GB (so a page fault only ever fills a table entry, never
a directory entry), with the first 16 MB and the arenas mapped 1:1 and the kernel heap
at KHEAP_START mapped on demand. Everything else that faults is a bug. Page 0 is unmapped once the
drivers have read the BIOS data area, so a NULL dereference faults (audit M6).

10/5/26 (USB boot): a reusable window for reading firmware tables, and a framebuffer
window that uses 4 MB pages with PSE-36 when the firmware put the framebuffer above
4 GB, where 32-bit page tables cannot point.
*/

#include <surfos/types.h>
#include <surfos/bit.h>
#include <surfos/panic.h>
#include <surfos/console.h>
#include <surfos/cpu.h>
#include <mm/memory.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <surfos/trap.h>
#include <surfos/task.h>
#include <mm/uvm.h>
#include <blibc_common.h>

static u_long pd_storage[1024] __attribute__((aligned(4096))); /* used to live at 0x9C000, inside the EBDA on some machines */
u_long *page_directory = pd_storage;
u_long *page_table;                         /* 1024 tables back to back: entry i maps virtual i << 12 */

#define IDENTITY_PAGES (IDENTITY_END / PAGE_SIZE)
#define LARGE_PAGE 0x400000UL

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

static inline void flush_tlb(void) {
    u_long cr3;
    asm volatile("movl %%cr3, %0\n\tmovl %0, %%cr3" : "=r"(cr3) : : "memory");
}

static void identity_map(u_long base, u_long size) {
    u_long a;
    for(a = PAGE_ALIGN_DOWN(base); a < base + size; a += PAGE_SIZE) page_table[a >> 12] = a | PTE_P | PTE_W;
}

void init_paging() {
    u_long i;

    page_table = (u_long *)arena_page_tables;   /* placed by pmm_init(), still reached 1:1 below */
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
    /* the arenas, wherever the memory map put them (below 1 GB) */
    identity_map(arena_page_tables, PTBL_SIZE);
    identity_map(arena_frame_stack, arena_frame_stack_bytes);
    identity_map(arena_dma_heap, PHEAP_SIZE);

    enablePaging();
}

void vmm_map(u_long virt, u_long phys, u_long flags) {
    page_table[virt >> 12] = (phys & PTE_FRAME) | (flags & (PTE_W | PTE_U | PTE_PWT | PTE_PCD | PTE_PAT)) | PTE_P;
    invlpg(virt);
}

void vmm_unmap(u_long virt) {
    page_table[virt >> 12] = 0;
    invlpg(virt);
}

/* 0 when not mapped, or when a 4 MB page points above 4 GB (not representable here) */
u_long vmm_get_phys(u_long virt) {
    u_long pde = page_directory[virt >> 22], pte;
    if((pde & (PTE_P | PTE_PS)) == (PTE_P | PTE_PS)) {
        if(pde & PDE_PS36_MASK) return 0;
        return (pde & 0xFFC00000) | (virt & (LARGE_PAGE - 1));
    }
    pte = page_table[virt >> 12];
    if(!(pte & PTE_P)) return 0;
    return (pte & PTE_FRAME) | (virt & (PAGE_SIZE - 1));
}

/* Is a linear address backed by a present page? The backtrace checks every frame with this
   so a corrupt frame pointer cannot fault inside a panic. */
bool vmm_is_mapped(u_long virt) {
    u_long pde = page_directory[virt >> 22];
    if(!(pde & PTE_P)) return false;
    if(pde & PTE_PS) return true;
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

    if(curTask && curTask->uvm && !(err & PF_PRESENT) && cr2 >= USER_START && cr2 < USER_END &&
       uvm_grow_stack(curTask->uvm, cr2) == 0) {
        return (u_long*)tf;                       /* the user stack grew by a page */
    }

    return trap_fatal(tf, "Page Fault at 0x%08lx: %s %s%s", cr2,
                      (err & PF_PRESENT) ? "protection violation on" : "access to unmapped page,",
                      (err & PF_WRITE) ? "write" : "read",
                      (err & PF_USER) ? " from user mode" : "");
}

/* Map physical memory that is not in the identity-mapped first 16 MB: device registers
   (uncached) or a boot module. Addresses come from a bump pointer in the IOMAP window and
   are never given back; drivers map once at init. */
static u_long iomap_next = IOMAP_START;

void *ioremap(u_long phys, u_long size, bool cached) {
    u_long off = phys & (PAGE_SIZE - 1), base = phys - off;
    u_long len = PAGE_ALIGN_UP(size + off), virt, i;
    if(!size || iomap_next + len > IOMAP_END || iomap_next + len < iomap_next) return NULL;
    virt = iomap_next;
    iomap_next += len;
    for(i = 0; i < len; i += PAGE_SIZE) vmm_map(virt + i, base + i, PTE_W | (cached ? 0 : (PTE_PCD | PTE_PWT)));
    return (void *)(virt + off);
}

/* A read-only view of up to 128 KB of physical memory below 4 GB, for firmware tables
   (kernel/acpi.c) during boot. One view at a time: the next call replaces it. */
void *vmm_temp_map(u64 phys, u_long len) {
    u_long off, base, pages, i, slots = (TMPMAP_END - TMPMAP_START) / PAGE_SIZE;
    if(!len || phys + len < phys || phys + len > 0x100000000ULL) return NULL;
    off = (u_long)phys & (PAGE_SIZE - 1);
    base = (u_long)phys - off;
    pages = (off + len + PAGE_SIZE - 1) / PAGE_SIZE;
    if(pages > slots) return NULL;
    for(i = 0; i < slots; i++) {
        if(i < pages) vmm_map(TMPMAP_START + i * PAGE_SIZE, base + i * PAGE_SIZE, 0);
        else if(page_table[(TMPMAP_START >> 12) + i]) vmm_unmap(TMPMAP_START + i * PAGE_SIZE);
    }
    return (void *)(TMPMAP_START + off);
}

/* Map a framebuffer (wc: write-combining when PAT allows it, else uncached) or RAM (cached)
   into the BIGMAP window, which hands out 4 MB-aligned slots and never takes them back.
   Below 4 GB with 4 KB pages. Above 4 GB 32-bit page tables cannot reach, but a 4 MB page
   can with PSE-36 (physical bits 32-39 in PDE bits 13-20), up to MAXPHYADDR. Done at boot,
   before any process copies the kernel's page directory. NULL and a reason when the range
   cannot be mapped. */
static u_long bigmap_next = BIGMAP_START;

void *vmm_map_large(u64 phys, u64 size, bool wc, const char **why) {
    u64 end = phys + size, base, len, i;
    u_long off, virt = bigmap_next, room = BIGMAP_END - bigmap_next;
    const char *dummy;
    if(!why) why = &dummy;
    if(!size || end < phys) { *why = "empty or wrapping range"; return NULL; }

    if(end <= 0x100000000ULL) {
        u_long flags = PTE_W | (wc ? (cpu.pat_wc ? PTE_PAT : (PTE_PCD | PTE_PWT)) : 0);
        off = (u_long)phys & (PAGE_SIZE - 1);
        base = phys - off;
        len = (size + off + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        if(len > room) { *why = "larger than the space left in the mapping window"; return NULL; }
        for(i = 0; i < len; i += PAGE_SIZE) vmm_map(virt + (u_long)i, (u_long)(base + i), flags);
    } else {
        u_long flags = PTE_PS | PTE_P | PTE_W | (wc ? (cpu.pat_wc ? PDE_PAT : (PTE_PCD | PTE_PWT)) : 0);
        if(!cpu.pse_on || !cpu.pse36) { *why = "above 4 GB, and the CPU has no PSE-36 4 MB pages"; return NULL; }
        if(end > (1ULL << (cpu.phys_bits < 40 ? cpu.phys_bits : 40))) { *why = "beyond the CPU's physical address width"; return NULL; }
        off = (u_long)(phys & (LARGE_PAGE - 1));
        base = phys - off;
        len = (size + off + LARGE_PAGE - 1) & ~(u64)(LARGE_PAGE - 1);
        if(len > room) { *why = "larger than the space left in the mapping window"; return NULL; }
        for(i = 0; i < len; i += LARGE_PAGE) {
            u64 p = base + i;
            page_directory[(virt + (u_long)i) >> 22] = ((u_long)p & 0xFFC00000) | (((u_long)(p >> 32) & 0xFF) << 13) | flags;
        }
        flush_tlb();
    }
    bigmap_next += (u_long)((len + LARGE_PAGE - 1) & ~(u64)(LARGE_PAGE - 1));
    return (void *)(virt + off);
}
