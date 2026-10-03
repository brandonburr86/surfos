/*
SurfOS Kernel Symbol Lookup and Backtraces
--------------------
File: ksym.c    Date: 10/3/26 (roadmap K3)
--------------------
*/

#include <surfos/types.h>
#include <surfos/ksym.h>
#include <surfos/console.h>
#include <mm/paging.h>
#include <blibc_common.h>

const char *ksym_lookup(u_long addr, u_long *offset) {
    u_long lo = 0, hi = ksym_count;

    if(!ksym_count || addr < ksym_table[0].addr) return NULL;
    while(hi - lo > 1) { /* binary search for the last symbol at or below addr */
        u_long mid = (lo + hi) / 2;
        if(ksym_table[mid].addr <= addr) lo = mid;
        else hi = mid;
    }
    if(offset) *offset = addr - ksym_table[lo].addr;
    return ksym_names + ksym_table[lo].name_off;
}

static void print_frame(u_long eip) {
    u_long off;
    const char *name = ksym_lookup(eip, &off);
    if(name) kprintf("  [<0x%08lx>] %s+0x%lx\n", eip, name, off);
    else kprintf("  [<0x%08lx>] ?\n", eip);
}

/* Walk the saved-EBP chain (the kernel is built with frame pointers). Every frame is
   checked against the page tables first so a bad pointer cannot fault inside a panic. */
void backtrace(u_long ebp, u_long eip) {
    int depth;
    u_long prev = 0;

    kprintf("Backtrace:\n");
    if(eip) print_frame(eip);
    for(depth = 0; depth < 24; depth++) {
        u_long *frame = (u_long *)ebp;
        if(ebp < 0x1000 || (ebp & 3) || ebp <= prev) break;
        if(!vmm_is_mapped(ebp) || !vmm_is_mapped(ebp + 4)) break;
        if(!frame[1]) break;
        print_frame(frame[1]);
        prev = ebp;
        ebp = frame[0];
    }
}

void backtrace_here(void) {
    u_long ebp;
    asm volatile("movl %%ebp, %0" : "=r"(ebp));
    backtrace(ebp, 0);
}
