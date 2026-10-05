/*
    SurfOS Memory Manager Header -- (C)2004 Brandon Burr, reworked 10/2026 (roadmap M1-M3)
    arenas placed from the memory map, and the mapping windows 10/5/26 (USB boot)
*/

#ifndef _MM_MEMORY_H
#define _MM_MEMORY_H

/* SurfOS Memory***

/--------------------------------------------------------\
| 31 .. 22             | 21 .. 12         |  11 .. 0     |
| Page Directory Index | Page Table Index |  Page Offset |
| (10 bits)  (0-1023)  | (10 bits)(0-1023)|  (12 bits)   | <- (0-4095)
\--------------------------------------------------------/

Physical layout (the first 16 MB and the arenas mapped 1:1):

    0x00000000 - 0x000FFFFF  BIOS, VGA memory, ISA DMA bounce buffers (DHEAP, dma.h)
    0x00100000 - kernel end  the kernel image: text, data, .bss
    the arenas               page tables (4 MB), the free-frame stack (4 bytes per frame)
                             and the DMA heap (5 MB, kalloc_dma(), virtual == physical):
                             pmm_init() puts them in RAM the memory map calls available,
                             below 1 GB, clear of the kernel, the boot modules and the
                             framebuffer. They used to be fixed at 2-15 MB, which OVMF
                             (and real firmware) may use for ACPI NVS.
    the rest of RAM          free frames, handed out by pmm_alloc(), except what
                             pmm_reserve() keeps back

Virtual only:

    0x40000000 - 0x7FFFFFFF  user processes (mm/uvm.h)
    0xB0000000 - 0xBFFFFFFF  kernel heap (KHEAP): kalloc(), pages mapped on first touch
    0xC0000000 - 0xCFFDFFFF  ioremap(): device registers and boot modules above 16 MB
    0xCFFE0000 - 0xCFFFFFFF  a reusable window for reading firmware tables (vmm_temp_map)
    0xD0000000 - 0xDFFFFFFF  vmm_map_large(): the framebuffer, and a test page of RAM above 4 GB
                             (4 KB pages below 4 GB, 4 MB PSE-36 pages above)
**************************/

#define PGDIR_MASK 0xFFC00000
#define PGTBL_MASK 0x3FF000
#define PGOFF_MASK 0xFFF

#define PAGE_SIZE 4096

#define IDENTITY_END 0x1000000   /* the first 16 MB are mapped 1:1 */

/* the arenas pmm_init() places (mm/pmm.c) */
#define PTBL_SIZE  0x400000      /* 4 MB page table array: 1024 tables covering 4 GB */
#define PHEAP_SIZE 0x500000      /* the 1:1 DMA heap */
#define ARENA_MIN   0x100000     /* never in the first megabyte */
#define ARENA_LIMIT 0x40000000   /* identity mapped, so below the user range */

/** SurfOS Kernel Address Space **/
#define KHEAP_START 0xB0000000
#define KHEAP_END   0xC0000000   /* 256 MB of virtual room; RAM is the real limit */

/* ioremap(): device registers and boot modules that are not in the first 16 MB */
#define IOMAP_START 0xC0000000
#define IOMAP_END   0xCFFE0000

/* vmm_temp_map(): one firmware table at a time */
#define TMPMAP_START 0xCFFE0000
#define TMPMAP_END   0xD0000000

/* vmm_map_large(): 4 MB aligned so large pages fit */
#define BIGMAP_START 0xD0000000
#define BIGMAP_END   0xE0000000

#define VIRT_TO_PGDIR(addr) ((addr)/0x400000)
#define VIRT_TO_PGTBL(addr) (((addr)%0x400000)/0x1000)

#ifndef __ASSEMBLER__

#include <surfos/types.h>

/* physical addresses of the arenas (virtual == physical), set by pmm_init() */
extern u_long arena_page_tables, arena_frame_stack, arena_frame_stack_bytes, arena_dma_heap;
extern char __kernel_start[], __kernel_end[];  /* linker.ld */

#define KERNEL_START ((u_long)__kernel_start)
#define KERNEL_END   ((u_long)__kernel_end)
#define PHEAP_START  arena_dma_heap
#define PHEAP_END    (arena_dma_heap + PHEAP_SIZE)

void init_mem();
void run_memcheck();
void printMemInfo();
void init_himem(void);             /* map 4 MB of RAM above 4 GB, if there is any (tests PSE-36) */
int himem_test(char *wrote, char *read, size_t size, u64 *phys);  /* 0: the pattern came back; -1: nothing mapped */

#endif /* !__ASSEMBLER__ */
#endif
