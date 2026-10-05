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
#include <surfos/timer.h>

#include <blibc_common.h>

/* SurfOS requires 32MB of RAM to run!!! */
void run_memcheck() {
    if(pmm_ram_bytes() < 32ULL * 1024 * 1024) {
        kprintf("\nKernel Panic: Less than 32MB of RAM detected (%llu KB).\n", pmm_ram_bytes() / 1024);
        kprintf("SYSTEM HALTED\n");
        halt();
    }
}

void init_mem() {
    pmm_init();      /* frames from the Multiboot map, minus the reserved ranges */
    heap_init();
    init_paging();   /* identity map, then paging on */
}

/* 10/5/26 (USB boot): the laptop's framebuffer may sit above 4 GB, where only a PSE-36
   4 MB page reaches. QEMU cannot put its framebuffer there, but it can put RAM there:
   4 MB of it are mapped the same way at boot, and `himem` writes and reads it back
   (the test harness checks the physical bytes through the QEMU monitor). */
static volatile char *himem_virt;
static u64 himem_phys;

void init_himem(void) {
    const char *why;
    u_int i;
    for(i = 0; i < bootinfo.nregions; i++) {
        struct mb_region *r = &bootinfo.regions[i];
        u64 start = r->base > 0x100000000ULL ? r->base : 0x100000000ULL, end = r->base + r->len;
        start = (start + 0x3FFFFF) & ~0x3FFFFFULL;
        if(r->type != MB_MEMORY_AVAILABLE || end < start + 0x400000) continue;
        himem_virt = (volatile char *)vmm_map_large(start, 0x400000, false, &why);
        if(!himem_virt) {
            kprintf("*RAM above 4 GB at 0x%llx cannot be mapped: %s\n", start, why);
            return;
        }
        himem_phys = start;
        kprintf("*4 MB of RAM at 0x%llx mapped at 0x%08lx with a PSE-36 4 MB page (see himem)\n", start, (u_long)himem_virt);
        return;
    }
}

int himem_test(char *wrote, char *read, size_t size, u64 *phys) {
    size_t i;
    if(!himem_virt) return -1;
    snprintf(wrote, size, "SURFOS-HIMEM-%lu", getticks());
    for(i = 0; i <= strlen(wrote); i++) himem_virt[i] = wrote[i];
    for(i = 0; i + 1 < size && himem_virt[i]; i++) read[i] = himem_virt[i];
    read[i] = 0;
    *phys = himem_phys;
    return strcmp(wrote, read) ? 1 : 0;
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
    printf("Physical memory: %lluKB", pmm_ram_bytes() / 1024);
    if(pmm_ram_high_bytes()) printf(" (%lluKB above 4 GB, not used)", pmm_ram_high_bytes() / 1024);
    printf("\nKernel: 0x%08lx-0x%08lx; page tables 0x%08lx, frame stack 0x%08lx, DMA heap 0x%08lx\n", KERNEL_START,
           KERNEL_END - 1, arena_page_tables, arena_frame_stack, arena_dma_heap);
    printf("Memstart: 0x%lx  Memend: 0x%lx\n", KERNEL_END, pmm_highest_address());
    printf("Free physical pages: %lu of %lu\n", pmm_free_frames(), pmm_total_frames());
    pmm_print_reserved();
    printf("\n");
    print_heap(&k);
    print_heap(&d);
    printf("\n");
}
