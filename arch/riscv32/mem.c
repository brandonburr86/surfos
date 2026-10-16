/*
SurfOS riscv32 Memory -- after mm/memory.c, (C)2004 Brandon Burr
--------------------
File: mem.c     Date: 10/15/26 (SurfCPU)
--------------------
Machine mode, flat physical memory: no page tables, no frame allocator, no demand
paging. The image is followed by the two heaps of mm/kalloc.c (the allocator itself
is the x86 one, unchanged); their arenas come from the linker script, the kernel heap
ending where the board keeps RAM for itself (include/arch/riscv32/mm/memory.h).
run_memcheck() makes sure the RAM is really there: the SurfBoot boot information
says how much DDR passed its check, and the heap must end inside it.
*/

#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <surfos/console.h>
#include <surfos/system.h>
#include <mm/memory.h>
#include <mm/kalloc.h>
#include <riscv/arch.h>
#include <blibc_common.h>

extern char __ram_start[], __ram_end[];     /* the board's RAM, from the linker script */
extern char __bss_start[], __bss_end[];

/* the RAM the kernel may use: SurfBoot's verified DDR when it said, else the board's */
static void ram_range(u_long *start, u_long *end) {
    *start = (u_long)__ram_start;
    *end = (u_long)__ram_end;
    if(bootinfo.valid && bootinfo.nregions && bootinfo.regions[0].len) {
        *start = (u_long)bootinfo.regions[0].base;
        *end = (u_long)(bootinfo.regions[0].base + bootinfo.regions[0].len);
    }
}

void init_mem(void) {
    heap_init();
    kprintf("Memory: kernel 0x%08lx-0x%08lx, DMA heap 0x%08lx-0x%08lx, kernel heap 0x%08lx-0x%08lx (%lu MB)\n",
            KERNEL_START, KERNEL_END - 1, PHEAP_START, PHEAP_END - 1, KHEAP_START, KHEAP_END - 1,
            (KHEAP_END - KHEAP_START) >> 20);
}

/* The heap must end inside RAM that exists. Its last word is written and read back: on
   missing RAM that is an access fault now, with a clear report, not a mystery later. */
void run_memcheck(void) {
    u_long start, end;
    volatile u32 *last = (volatile u32 *)(KHEAP_END - 4);
    ram_range(&start, &end);
    if(KHEAP_START < start || KHEAP_END > end) {
        kprintf("\nKernel Panic: the kernel heap 0x%08lx-0x%08lx is not inside the RAM 0x%08lx-0x%08lx.\n",
                KHEAP_START, KHEAP_END - 1, start, end - 1);
        kprintf("SYSTEM HALTED\n");
        halt();
    }
    *last = 0x5355524Bu;
    if(*last != 0x5355524Bu) {
        kprintf("\nKernel Panic: RAM at 0x%08lx does not keep what is written.\nSYSTEM HALTED\n", (u_long)last);
        halt();
    }
}

static void print_heap(struct heap_stats *st) {
    printf("%s heap: 0x%08lx-0x%08lx (%lu KB grown in %lu steps, room to 0x%08lx)\n", st->name, st->start, st->brk,
           (st->brk - st->start) / 1024, st->grows, st->limit);
    printf("  used: %lu bytes in %lu blocks   free: %lu bytes in %lu blocks\n", st->bytes_used, st->blocks_used,
           st->bytes_free, st->blocks_free);
}

void printMemInfo(void) {
    struct heap_stats k, d;
    u_long start, end;
    kheap_stats(&k);
    dmaheap_stats(&d);
    ram_range(&start, &end);

    printf("\nSurfOS Memory Statistics\n");
    printf("--------------------------\n");
    printf("Physical memory: %luKB at 0x%08lx (machine mode: no paging, no protection between tasks)\n",
           (end - start) / 1024, start);
    printf("Kernel: 0x%08lx-0x%08lx (BSS 0x%08lx-0x%08lx)\n", KERNEL_START, KERNEL_END - 1,
           (u_long)__bss_start, (u_long)__bss_end - 1);
    printf("Memstart: 0x%lx  Memend: 0x%lx\n", KERNEL_END, end);
    printf("\n");
    print_heap(&k);
    print_heap(&d);
    printf("heap check: %s\n\n", kheap_check() ? "ok" : "CORRUPT");
}
