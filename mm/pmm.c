/*
SurfOS Physical Memory Manager
--------------------
File: pmm.c     Date: 10/3/26 (roadmap M1), reservations and arenas 10/5/26 (USB boot)
--------------------
(C)2004 Brandon Burr (the frame stack), 2026 (the memory map)

The 2004 kernel sized RAM by writing a magic value to every page from 8 MB up
(memprobe), which is unsafe on real hardware and was optimised away at -O2
(audit A6, M7). The stack of free frames stays: it is O(1) and fits the kernel.

What is not handed out is an explicit list of reserved ranges (mm/memrange.c does the
arithmetic): everything below the end of the kernel image, the boot modules, the
framebuffer, and the three arenas the kernel needs before it has any allocator (the
page tables, this frame stack and the DMA heap). The arenas are placed here, in RAM
the boot loader's map calls available: under QEMU's loader the old fixed 2-15 MB was
always free, but OVMF keeps ACPI NVS data at 8 MB and real firmware may do anything
there. RAM accounting is 64-bit, so a machine with more than 4 GB reports its size
correctly; the frames themselves stop below 4 GB (no PAE).
*/

#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <surfos/console.h>
#include <surfos/panic.h>
#include <surfos/irq.h>
#include <mm/memory.h>
#include <mm/pmm.h>
#include <mm/memrange.h>
#include <blibc_common.h>

u_long arena_page_tables, arena_frame_stack, arena_frame_stack_bytes, arena_dma_heap;

static u_long *stack_bottom, *stack_limit;   /* first slot, one past the last slot */
static u_long *sp;                           /* next free slot */
static u_long total_frames, free_frames, highest;
static u64 ram_bytes, ram_high_bytes;
static bool overflowed;
static const char *image_problem;            /* reported by pmm_report() once there is a screen */

static struct mem_range reserved[PMM_MAX_RESERVED];
static const char *reserved_what[PMM_MAX_RESERVED];
static u_int nreserved;

void pmm_reserve(u64 base, u64 len, const char *what) {
    u64 end = base + len;
    if(!len) return;
    if(end < base) end = ~0ULL;
    /* losing a reservation could hand out memory that is in use: refuse to boot instead */
    if(nreserved == PMM_MAX_RESERVED) panic("pmm: more than %u reserved ranges (%s)", PMM_MAX_RESERVED, what);
    reserved[nreserved].base = base;
    reserved[nreserved].end = end;
    reserved_what[nreserved] = what;
    nreserved++;
}

bool pmm_is_reserved(u64 base, u64 len) {
    return memrange_overlaps(base, base + len, reserved, nreserved);
}

/* is all of [base, end) available RAM according to the map? */
static bool map_covers(u64 base, u64 end) {
    u_int i;
    while(base < end) {
        bool moved = false;
        for(i = 0; i < bootinfo.nregions; i++) {
            struct mb_region *r = &bootinfo.regions[i];
            if(r->type == MB_MEMORY_AVAILABLE && r->base <= base && base < r->base + r->len) {
                base = r->base + r->len;
                moved = true;
            }
        }
        if(!moved) return false;
    }
    return true;
}

/**** arenas ****/

struct fit {
    u64 size, base;
    bool found;
};

static void fit_piece(u64 base, u64 end, void *arg) {
    struct fit *f = (struct fit *)arg;
    if(end - base >= f->size && (!f->found || base < f->base)) {
        f->base = base;
        f->found = true;
    }
}

static void count_piece(u64 base, u64 end, void *arg) {
    *(u64 *)arg += (end - base) / PAGE_SIZE;
}

/* the lowest free stretch of available RAM between 1 MB and 1 GB that holds size bytes */
static u_long place_arena(u64 size, const char *what) {
    struct fit f;
    u_int i;
    f.size = size;
    f.base = 0;
    f.found = false;
    for(i = 0; i < bootinfo.nregions; i++) {
        if(bootinfo.regions[i].type != MB_MEMORY_AVAILABLE) continue;
        memrange_free_pieces(bootinfo.regions[i].base, bootinfo.regions[i].len, ARENA_MIN, ARENA_LIMIT,
                             reserved, nreserved, fit_piece, &f);
    }
    if(!f.found) panic("pmm: no %llu KB of free RAM below 1 GB for the %s", size / 1024, what);
    pmm_reserve(f.base, size, what);
    return (u_long)f.base;
}

/**** the frame stack ****/

static void push_piece(u64 base, u64 end, void *arg) {
    u_long frame;
    for(frame = (u_long)base; (u64)frame < end; frame += PAGE_SIZE) {
        if(sp >= stack_limit) {
            overflowed = true;
            return;
        }
        *sp++ = frame;
        total_frames++;
    }
    if((u_long)end > highest) highest = (u_long)end;
}

void pmm_init(void) {
    u64 frames = 0;
    u_int i;
    sp = NULL;
    total_frames = free_frames = highest = 0;
    ram_bytes = ram_high_bytes = 0;
    overflowed = false;
    image_problem = NULL;

    kprintf("Physical Memory\n");
    if(bootinfo.nregions == 0) {
        kprintf("*No memory map from the boot loader: assuming 32 MB\n");
        bootinfo.regions[0].base = 0x100000;
        bootinfo.regions[0].len = 31 * 1024 * 1024;
        bootinfo.regions[0].type = MB_MEMORY_AVAILABLE;
        bootinfo.nregions = 1;
    }

    /* the standard reservations, after whatever was registered earlier */
    pmm_reserve(0, KERNEL_END, "low memory and the kernel");
    for(i = 0; i < bootinfo.nmods; i++) {
        if(bootinfo.mods[i].end > bootinfo.mods[i].start)
            pmm_reserve(bootinfo.mods[i].start, bootinfo.mods[i].end - bootinfo.mods[i].start, "boot module");
    }
    if(bootinfo.fb.present) pmm_reserve(bootinfo.fb.addr, (u64)bootinfo.fb.pitch * bootinfo.fb.height, "framebuffer");

    /* the loader put the kernel and the modules somewhere: say so if that is not plain RAM */
    if(!map_covers(KERNEL_START, KERNEL_END)) image_problem = "the kernel image overlaps memory the firmware has not marked available";
    for(i = 0; i < bootinfo.nmods; i++) {
        if(bootinfo.mods[i].end > bootinfo.mods[i].start && !map_covers(bootinfo.mods[i].start, bootinfo.mods[i].end))
            image_problem = "a boot module overlaps memory the firmware has not marked available";
    }

    for(i = 0; i < bootinfo.nregions; i++) {
        u64 base = bootinfo.regions[i].base, len = bootinfo.regions[i].len, end = base + len;
        if(bootinfo.regions[i].type != MB_MEMORY_AVAILABLE) continue;
        if(end < base) end = ~0ULL;
        ram_bytes += end - base;
        if(end > 0x100000000ULL) ram_high_bytes += end - (base > 0x100000000ULL ? base : 0x100000000ULL);
        memrange_free_pieces(base, len, 0, PMM_TOP, reserved, nreserved, count_piece, &frames);
    }

    /* the arenas: an upper bound of the frames sizes the stack, the arenas only make it smaller */
    arena_frame_stack_bytes = (u_long)(((frames ? frames : 1) * sizeof(u_long) + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1));
    arena_page_tables = place_arena(PTBL_SIZE, "page tables");
    arena_frame_stack = place_arena(arena_frame_stack_bytes, "free-frame stack");
    arena_dma_heap = place_arena(PHEAP_SIZE, "DMA heap");
    stack_bottom = sp = (u_long *)arena_frame_stack;
    stack_limit = (u_long *)(arena_frame_stack + arena_frame_stack_bytes);

    for(i = 0; i < bootinfo.nregions; i++) {
        if(bootinfo.regions[i].type != MB_MEMORY_AVAILABLE) continue;
        memrange_free_pieces(bootinfo.regions[i].base, bootinfo.regions[i].len, 0, PMM_TOP, reserved, nreserved,
                             push_piece, NULL);
    }
    free_frames = total_frames;
    kprintf("*%llu KB of RAM", ram_bytes / 1024);
    if(ram_high_bytes) kprintf(" (%llu KB of it above 4 GB, not addressable without PAE)", ram_high_bytes / 1024);
    kprintf("\n*%lu free frames (%lu KB), top 0x%08lx\n", free_frames, free_frames * (PAGE_SIZE / 1024), highest);
    kprintf("*page tables 0x%08lx, frame stack 0x%08lx (%lu KB), DMA heap 0x%08lx\n", arena_page_tables,
            arena_frame_stack, arena_frame_stack_bytes / 1024, arena_dma_heap);
    if(overflowed) kprintf("*the frame stack is full: the frames above 0x%08lx were left out\n", highest);
    if(image_problem) kprintf("*WARNING: %s\n", image_problem);
}

/* after the screen is up: problems found before it was (they are in the log as well) */
const char *pmm_problem(void) {
    return image_problem;
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
    if(pmm_is_reserved(frame, PAGE_SIZE)) panic("pmm_free: frame 0x%08lx is reserved memory", frame);
    flags = irq_save();
    if(sp >= stack_limit) panic("pmm_free: frame stack overflow");
    *sp++ = frame;
    free_frames++;
    irq_restore(flags);
}

void pmm_print_reserved(void) {
    u_int i;
    for(i = 0; i < nreserved; i++) {
        printf("  reserved 0x%09llx-0x%09llx %s\n", reserved[i].base, reserved[i].end - 1, reserved_what[i]);
    }
}

u_long pmm_total_frames(void) { return total_frames; }
u_long pmm_free_frames(void) { return free_frames; }
u_long pmm_highest_address(void) { return highest; }
u64 pmm_ram_bytes(void) { return ram_bytes; }
u64 pmm_ram_high_bytes(void) { return ram_high_bytes; }
bool pmm_stack_overflowed(void) { return overflowed; }
