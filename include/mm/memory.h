/*
    SurfOS Memory Manager Header -- (C)2004 Brandon Burr, reworked 10/2026 (roadmap M1-M3)
*/

#ifndef _MM_MEMORY_H
#define _MM_MEMORY_H

#include <surfos/types.h>

/* SurfOS Memory***

/--------------------------------------------------------\
| 31 .. 22             | 21 .. 12         |  11 .. 0     |
| Page Directory Index | Page Table Index |  Page Offset |
| (10 bits)  (0-1023)  | (10 bits)(0-1023)|  (12 bits)   | <- (0-4095)
\--------------------------------------------------------/

Physical layout (all of it mapped 1:1):

    0x00000000 - 0x000FFFFF  BIOS, VGA memory, ISA DMA bounce buffers (DHEAP, dma.h)
    0x00100000 - kernel end  kernel image
    0x00200000 - 0x005FFFFF  kernel page tables (PTBL): 1024 tables covering 4 GB
    0x00600000 - 0x009FFFFF  stack of free frames (PSTK)
    0x00A00000 - 0x00EFFFFF  DMA heap (PHEAP): kalloc_dma(), virtual == physical
    0x00F00000 - end of RAM  free frames, handed out by pmm_alloc()

Virtual only:

    0xB0000000 - 0xBFFFFFFF  kernel heap (KHEAP): kalloc(), pages mapped on first touch
**************************/

#define PGDIR_MASK 0xFFC00000
#define PGTBL_MASK 0x3FF000
#define PGOFF_MASK 0xFFF

#define PAGE_SIZE 4096

#define IDENTITY_END 0x1000000   /* the first 16 MB are mapped 1:1 */

/* SurfOS Extended memory space: free frames start here */
#define PMEM_START 0xF00000

/** SurfOS Kernel Address Space **/
#define KHEAP_START 0xB0000000
#define KHEAP_END   0xC0000000   /* 256 MB of virtual room; RAM is the real limit */

/* ioremap(): device registers and boot modules that are not in the first 16 MB */
#define IOMAP_START 0xC0000000
#define IOMAP_END   0xD0000000

/* physical DMA heap (1:1) */
#define PHEAP_START 0xA00000     /* 10 MB */
#define PHEAP_END   0xF00000     /* 15 MB */

/* SurfOS Paging tables */
#define PTBL_SIZE 0x400000       /* 4 MB page table array */
#define PTBL_START 0x200000

/* free frame stack */
#define PSTK_START 0x600000      /* right after the page tables */
#define PSTK_SIZE 0x400000       /* 4 MB: room for a 4 GB machine */

#define VIRT_TO_PGDIR(addr) ((addr)/0x400000)
#define VIRT_TO_PGTBL(addr) (((addr)%0x400000)/0x1000)

void init_mem();
void run_memcheck();
void printMemInfo();

#endif
