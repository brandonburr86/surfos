/*
    SurfOS Memory Manager -- (C)2004 Brandon Burr, reworked 10/2026
*/

#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <surfos/console.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <mm/kalloc.h>
#include <surfos/system.h>

#include <blibc_common.h>

/* SurfOS requires 32MB of RAM to run!!! */
void run_memcheck() {
    if(pmm_ram_bytes() < 32UL * 1024 * 1024) {
        kprintf("\nKernel Panic: Less than 32MB of RAM detected (%lu KB).\n", pmm_ram_bytes() / 1024);
        kprintf("SYSTEM HALTED\n");
        halt();
    }
}

void init_mem() {
    pmm_init();      /* frames from the Multiboot map */
    heap_init();
    init_paging();   /* identity map, then paging on */
}

static void print_heap(struct heap_stats *st) {
    printf("%s heap: 0x%08lx-0x%08lx (%lu KB grown in %lu steps)\n", st->name, st->start, st->brk, (st->brk - st->start) / 1024, st->grows);
    printf("  used: %lu bytes in %lu blocks   free: %lu bytes in %lu blocks\n", st->bytes_used, st->blocks_used, st->bytes_free, st->blocks_free);
}

void printMemInfo() {
    struct heap_stats k, d;
    kheap_stats(&k);
    dmaheap_stats(&d);

    printf("\nSurfOS Memory Statistics\n");
    printf("--------------------------\n");
    printf("Physical memory: %luKB\n", pmm_ram_bytes() / 1024);
    printf("Memstart: 0x%x  Memend: 0x%lx\n", PMEM_START, pmm_highest_address());
    printf("Free physical pages: %lu of %lu\n\n", pmm_free_frames(), pmm_total_frames());
    print_heap(&k);
    print_heap(&d);
    printf("\n");
}
