/*
SurfOS Physical Memory Manager
--------------------
File: pmm.c     Date: 10/3/26 (roadmap M1)
--------------------
(C)2004 Brandon Burr (the frame stack), 2026 (the memory map)

The 2004 kernel sized RAM by writing a magic value to every page from 8 MB up
(memprobe), which is unsafe on real hardware and was optimised away at -O2
(audit A6, M7). The stack of free frames stays: it is O(1) and fits the kernel.
*/

#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <surfos/console.h>
#include <surfos/panic.h>
#include <surfos/irq.h>
#include <mm/memory.h>
#include <mm/pmm.h>
#include <blibc_common.h>

static u_long *stack_bottom = (u_long *)PSTK_START;               /* first slot */
static u_long *stack_limit = (u_long *)(PSTK_START + PSTK_SIZE);  /* one past the last slot */
static u_long *sp;                                                 /* next free slot */
static u_long total_frames, free_frames, highest, ram_bytes;

static bool frame_in_module(u_long frame) {
    u_int i;
    for(i = 0; i < bootinfo.nmods; i++) {
        if(frame + PAGE_SIZE > bootinfo.mods[i].start && frame < bootinfo.mods[i].end) return true;
    }
    return false;
}

static void push_region(u64 base, u64 len) {
    u64 start = base, end = base + len;
    u_long frame;

    if(start < PMEM_START) start = PMEM_START;         /* the kernel's identity-mapped area */
    if(end > 0xFFFFF000ULL) end = 0xFFFFF000ULL;        /* keep clear of the top page */
    start = (start + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
    end &= ~(u64)(PAGE_SIZE - 1);
    if(start >= end) return;

    for(frame = (u_long)start; frame < (u_long)end; frame += PAGE_SIZE) {
        if(frame_in_module(frame)) continue;           /* an initrd GRUB placed above 15 MB */
        if(sp >= stack_limit) {
            kprintf("pmm: frame stack full at 0x%08lx, ignoring the rest\n", frame);
            return;
        }
        *sp++ = frame;
        total_frames++;
    }
    if((u_long)end > highest) highest = (u_long)end;
}

void pmm_init(void) {
    u_int i;
    sp = stack_bottom;
    total_frames = free_frames = highest = ram_bytes = 0;

    kprintf("Physical Memory\n");
    if(bootinfo.nregions == 0) {
        kprintf("*No memory map from the boot loader: assuming 32 MB\n");
        push_region(0x100000, 31 * 1024 * 1024);
        ram_bytes = 32 * 1024 * 1024;
    } else {
        for(i = 0; i < bootinfo.nregions; i++) {
            if(bootinfo.regions[i].type != MB_MEMORY_AVAILABLE) continue;
            ram_bytes += (u_long)bootinfo.regions[i].len;
            push_region(bootinfo.regions[i].base, bootinfo.regions[i].len);
        }
    }
    free_frames = total_frames;
    kprintf("*%lu KB of RAM, %lu free frames above 0x%lx, top of memory 0x%08lx\n",
            ram_bytes / 1024, free_frames, (u_long)PMEM_START, highest);
}

u_long pmm_alloc(void) {
    u_long flags = irq_save();
    u_long frame = 0;
    if(sp > stack_bottom) {
        frame = *--sp;
        free_frames--;
    }
    irq_restore(flags);
    return frame;
}

void pmm_free(u_long frame) {
    u_long flags;
    if(frame & (PAGE_SIZE - 1)) panic("pmm_free: unaligned frame 0x%08lx", frame);
    if(frame < PMEM_START) panic("pmm_free: frame 0x%08lx is kernel memory", frame);
    flags = irq_save();
    if(sp >= stack_limit) panic("pmm_free: frame stack overflow");
    *sp++ = frame;
    free_frames++;
    irq_restore(flags);
}

u_long pmm_total_frames(void) { return total_frames; }
u_long pmm_free_frames(void) { return free_frames; }
u_long pmm_highest_address(void) { return highest; }
u_long pmm_ram_bytes(void) { return ram_bytes; }
