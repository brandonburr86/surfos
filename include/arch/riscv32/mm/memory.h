/*
SurfOS Memory Layout (riscv32)
----------------------
File: memory.h  Date: 10/15/26 (SurfCPU), the riscv32 face of include/mm/memory.h
----------------------
Machine mode has no paging: addresses are physical, and every byte the kernel can
reach, every other byte too. There is no demand paging and no protection between
tasks (surfcpu/docs/PORTING-RISCV.md). The layout comes from the linker script
(arch/riscv32/linker.ld, with the board's RAM_END and HEAP_END):

    RAM base (0x80000000)   the kernel image: .text .rodata .data .ksyms .bss
    __dmaheap_start         1 MB for kalloc_dma(): nothing does DMA on SurfCPU, but the
                            second heap of mm/kalloc.c keeps its own arena as on x86
    __kheap_start           the kernel heap (kalloc()), up to the board's HEAP_END
    HEAP_END ..             what the board keeps for itself (the Arty: ddr_top, the top
                            16 MB SurfBoot leaves free; QEMU virt: the RAM disk window)

mm/kalloc.c is the x86 allocator unchanged; its kernel heap simply has RAM behind every
page from the start.
*/

#ifndef _MM_MEMORY_H
#define _MM_MEMORY_H

#define PAGE_SIZE 4096

#ifndef __ASSEMBLER__

#include <surfos/types.h>

extern char __kernel_start[], __kernel_end[];       /* the image, BSS included */
extern char __dmaheap_start[], __dmaheap_end[];
extern char __kheap_start[], __kheap_end[];

#define KERNEL_START ((u_long)__kernel_start)
#define KERNEL_END   ((u_long)__kernel_end)
#define KHEAP_START  ((u_long)__kheap_start)
#define KHEAP_END    ((u_long)__kheap_end)
#define PHEAP_START  ((u_long)__dmaheap_start)
#define PHEAP_END    ((u_long)__dmaheap_end)
#define PHEAP_SIZE   (PHEAP_END - PHEAP_START)

void init_mem(void);
void run_memcheck(void);
void printMemInfo(void);

#endif /* !__ASSEMBLER__ */
#endif
