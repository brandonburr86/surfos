/*
SurfOS Physical Memory Manager
----------------------
File: pmm.h     Date: 10/3/26 (roadmap M1)
----------------------
A stack of free 4 KB frames built from the Multiboot memory map. Frames below
PMEM_START (the identity-mapped kernel area) are never handed out.
*/

#ifndef _MM_PMM_H
#define _MM_PMM_H

#include <surfos/types.h>

void pmm_init(void);
u_long pmm_alloc(void);          /* physical address of a free frame, or 0 when out of memory */
void pmm_free(u_long frame);
u_long pmm_total_frames(void);
u_long pmm_free_frames(void);
u_long pmm_highest_address(void); /* end of the last usable region */
u_long pmm_ram_bytes(void);       /* all RAM the map reports, reserved areas included */

#endif
