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
*/

#ifndef _MM_PMM_H
#define _MM_PMM_H

#include <surfos/types.h>
#include <mm/memrange.h>

#define PMM_MAX_RESERVED 32
#define PMM_TOP 0xFFFFF000ULL           /* frames end below the last page of the 4 GB space */

void pmm_init(void);
void pmm_reserve(u64 base, u64 len, const char *what);  /* before pmm_init(), which adds the standard ones */
u_long pmm_alloc(void);          /* physical address of a free frame, or 0 when out of memory */
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
