/*
SurfOS Physical Address Ranges
--------------------
File: memrange.c    Date: 10/5/26 (USB boot)
--------------------
See include/mm/memrange.h. Everything is u64 so a map entry above 4 GB, or one that
claims to run past the end of the address space, is clipped instead of wrapping.
*/

#include <surfos/types.h>
#include <mm/memrange.h>

#define PG 4096ULL

static u64 page_up(u64 a) {
    u64 r = (a + PG - 1) & ~(PG - 1);
    return r < a ? ~(PG - 1) : r;              /* saturate at the last page */
}

static u64 page_down(u64 a) {
    return a & ~(PG - 1);
}

u_int memrange_free_pieces(u64 base, u64 len, u64 lo, u64 hi,
                           const struct mem_range *res, u_int nres, memrange_emit_t emit, void *arg) {
    u64 start, end, cur;
    u_int pieces = 0, i;

    if(!len || hi <= lo) return 0;
    end = base + len;
    if(end < base) end = ~0ULL;                /* wrapped: the region runs to the top */
    start = base > lo ? base : lo;
    if(end > hi) end = hi;
    start = page_up(start);
    end = page_down(end);

    for(cur = start; cur < end; ) {
        u64 next = end;
        bool blocked = false;
        for(i = 0; i < nres; i++) {
            u64 rb, re;
            if(res[i].end <= res[i].base) continue;
            rb = page_down(res[i].base);        /* every page the range touches */
            re = page_up(res[i].end);
            if(rb <= cur && cur < re) {         /* inside a reserved range: skip past it */
                cur = re;
                blocked = true;
                break;
            }
            if(rb > cur && rb < next) next = rb;
        }
        if(blocked) continue;
        if(emit) emit(cur, next, arg);
        pieces++;
        cur = next;
    }
    return pieces;
}

bool memrange_overlaps(u64 base, u64 end, const struct mem_range *res, u_int nres) {
    u_int i;
    for(i = 0; i < nres; i++) {
        if(res[i].end <= res[i].base) continue;
        if(base < res[i].end && res[i].base < end) return true;
    }
    return false;
}
