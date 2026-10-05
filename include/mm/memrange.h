/*
SurfOS Physical Address Ranges
----------------------
File: memrange.h    Date: 10/5/26 (USB boot)
----------------------
64-bit interval arithmetic for the physical memory manager: which page frames of a
memory map region are free once the kernel, the boot modules, the framebuffer and
anything else reserved are taken out. Pure code (mm/memrange.c), tested on the host
by tests/host/test_bootinfo.c.
*/

#ifndef _MM_MEMRANGE_H
#define _MM_MEMRANGE_H

#include <surfos/types.h>

struct mem_range {
    u64 base, end;              /* [base, end); empty when end <= base */
};

typedef void (*memrange_emit_t)(u64 base, u64 end, void *arg);

/* Calls emit() for each page-aligned piece of [base, base + len) that lies inside
   [lo, hi) and shares no page with a reserved range. A region that wraps past 2^64 is
   clipped. Returns the number of pieces. */
u_int memrange_free_pieces(u64 base, u64 len, u64 lo, u64 hi,
                           const struct mem_range *res, u_int nres, memrange_emit_t emit, void *arg);

/* does [base, end) share a byte with any of the ranges? */
bool memrange_overlaps(u64 base, u64 end, const struct mem_range *res, u_int nres);

#endif
