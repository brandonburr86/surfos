/*
SurfOS Physical Memory Manager
----------------------
File: pmm.h     Date: 10/3/26 (roadmap M1), reservations 10/5/26 (USB boot)
----------------------
A stack of free 4 KB frames built from the boot loader's memory map. Frames are only
handed out from available RAM below 4 GB (no PAE) that is not reserved: low memory and
the kernel image, the boot modules, the framebuffer, the arenas pmm_init() places for
the page tables, the frame stack and the DMA heap, and whatever else asked through
pmm_reserve() before pmm_init().

10/25/26 (robustness, L1): each frame is asked for by a class. Programs (PMM_USER) stop while
the kernel's reserve is left, large kernel buffers (PMM_BULK) at half of it, and the kernel
(PMM_KERNEL, what pmm_alloc() is) may take the last frame: a program that takes all it can
leaves the kernel what it needs. The reserve is a 32nd of the frames, 2 MB at least and 16 MB
at most. Frames promised to the kernel heap (pmm_commit()) count as taken until its page
faults take them (pmm_take_committed()), so that fault always finds one.
*/

#ifndef _MM_PMM_H
#define _MM_PMM_H

#include <surfos/types.h>
#include <mm/memrange.h>

#define PMM_MAX_RESERVED 32
#define PMM_TOP 0xFFFFF000ULL           /* frames end below the last page of the 4 GB space */

#define PMM_KERNEL  0             /* 10/25/26 (L1): may take the last frame */
#define PMM_BULK    1             /* large kernel buffers: leave half the reserve */
#define PMM_USER    2             /* programs: leave the whole reserve */
#define PMM_CLASSES 3

/* may class cls take n frames when avail are left (free, less those promised) and the reserve is reserve? */
static inline bool pmm_class_may(int cls, u_long avail, u_long n, u_long reserve) {
    u_long keep = cls == PMM_USER ? reserve : cls == PMM_BULK ? reserve / 2 : 0;
    return n <= avail && avail - n >= keep;
}

/* the reserve, in frames, for a machine of total frames: a 32nd, 2 MB at least, 16 MB at most */
static inline u_long pmm_kernel_reserve(u_long total) {
    u_long r = total / 32;
    return r < 512 ? 512 : r > 4096 ? 4096 : r;
}

struct pmm_stats {
    u_long reserve;               /* frames programs leave the kernel */
    u_long committed;             /* promised to the kernel heap, not taken yet */
    u_long lowest;                /* the fewest frames left (free, less promised) since boot */
    u_long refused[PMM_CLASSES];  /* frames, or promises, a class asked for and did not get */
};

void pmm_init(void);
void pmm_reserve(u64 base, u64 len, const char *what);  /* before pmm_init(), which adds the standard ones */
u_long pmm_alloc(void);          /* physical address of a free frame, or 0 when out of memory (PMM_KERNEL) */
u_long pmm_alloc_as(int cls);     /* 10/25/26 (L1): the same for a class: 0 when it may not have one (counted) */
int pmm_commit(u_long n, int cls);   /* promise n frames to the kernel heap: 0, or -1 when cls may not (counted) */
u_long pmm_take_committed(void);  /* a promised frame (the kernel heap's page fault): 0 only with none promised */
u_long pmm_room(int cls);         /* frames cls may still take */
void pmm_stats(struct pmm_stats *st);
void pmm_free(u_long frame);
bool pmm_is_reserved(u64 base, u64 len);
u_long pmm_total_frames(void);
u_long pmm_free_frames(void);
u_long pmm_highest_address(void); /* end of the highest usable frame */
u64 pmm_ram_bytes(void);          /* available RAM the map reports, above 4 GB included */
u64 pmm_ram_high_bytes(void);     /* the part above 4 GB, which this kernel cannot address */
bool pmm_stack_overflowed(void);  /* more frames than the stack holds: the rest was left out */
void pmm_print_reserved(void);
const char *pmm_problem(void);    /* the kernel or a module is not in available RAM, or NULL */

#endif
