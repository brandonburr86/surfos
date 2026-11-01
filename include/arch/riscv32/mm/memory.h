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
    __kheap_start           the kernel heap (kalloc()), up to kheap_end
    kheap_end ..            what is kept free: 10/16/26, 16 MiB below the end of the RAM the
                            boot information reports (BOOT-ABI.md: ddr_top, the top 16 MiB of
                            the DDR that calibrated), and never above the board's HEAP_END
                            (the Arty: the map's ddr_top; QEMU virt: the RAM disk window)

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
extern char __kheap_start[], __kheap_end[];       /* __kheap_end: the board's HEAP_END, the most the heap may have */
extern u_long kheap_end;                            /* where it ends (arch/riscv32/mem.c, before heap_init()) */

#define KERNEL_START ((u_long)__kernel_start)
#define KERNEL_END   ((u_long)__kernel_end)
#define KHEAP_START  ((u_long)__kheap_start)
#define KHEAP_END    kheap_end
#define PHEAP_START  ((u_long)__dmaheap_start)
#define PHEAP_END    ((u_long)__dmaheap_end)
#define PHEAP_SIZE   (PHEAP_END - PHEAP_START)

void init_mem(void);
void run_memcheck(void);
void printMemInfo(void);

#endif /* !__ASSEMBLER__ */
#endif
