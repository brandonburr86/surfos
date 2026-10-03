/*
SurfOS User Address Spaces
----------------------
File: uvm.h     Date: 10/3/26 (roadmap P1)
----------------------
A process owns a page directory whose kernel half is a copy of the kernel's
(the kernel page tables themselves are shared, so kernel mappings made later
are visible everywhere) and whose user range, USER_START..USER_END, is built
from per-process page tables. Directory and tables live in kernel heap pages;
user frames come from the physical allocator. Mapping a page zeroes it, which
means the address space must be the active one (the frame is reached through
its user address), so a process builds its own image inside its own task.
*/

#ifndef _MM_UVM_H
#define _MM_UVM_H

#include <surfos/types.h>

#define USER_START     0x40000000
#define USER_END       0x80000000
#define USER_STACK_MAX 0x00400000       /* the stack may grow to 4 MB below USER_END */
#define USER_PDE_FIRST (USER_START >> 22)
#define USER_PDE_COUNT ((USER_END - USER_START) >> 22)

struct uvm {
    u_long *pdir;                       /* page directory (kernel virtual) */
    void *pdir_raw;                     /* the heap block it was aligned inside */
    u_long pdir_phys;
    u_long *tables[USER_PDE_COUNT];     /* user page tables, NULL until needed */
    void *tables_raw[USER_PDE_COUNT];
    u_long brk_start, brk;              /* heap: sbrk() moves brk up from brk_start */
    u_long stack_low;                   /* lowest mapped stack address */
    u_long pages;                       /* user frames owned */
};

struct uvm *uvm_create(void);
void uvm_destroy(struct uvm *m);                 /* frees every user frame; m must not be active */
void uvm_switch(struct uvm *m);                  /* load the directory (NULL: the kernel's) */
struct uvm *uvm_current(void);
int uvm_map_page(struct uvm *m, u_long virt, u_long flags);       /* a zeroed frame at virt (PTE_W to write) */
int uvm_map_range(struct uvm *m, u_long start, u_long len, u_long flags);
u_long uvm_lookup(struct uvm *m, u_long virt);                     /* physical address or 0 */
bool uvm_check(struct uvm *m, u_long start, u_long len, bool write);  /* user memory, present (stack grows) */
bool uvm_check_str(struct uvm *m, const char *s, u_long max);      /* a NUL-terminated user string */
int uvm_grow_stack(struct uvm *m, u_long addr);                    /* page fault below stack_low */
u_long uvm_sbrk(struct uvm *m, long incr);                         /* the old brk, or 0 on failure */
u_long kernel_pdir_phys(void);

#endif
