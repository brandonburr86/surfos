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

10/25/26 (robustness, L4-L6): the directory and the tables are frames of the
programs' class (PMM_USER, mm/pmm.h) mapped for the kernel in the window UVMWIN
(mm/memory.h), no longer 8 KB of kernel heap each: a program that maps all it
can stops at the kernel's reserve and drains nothing the kernel needs. Mapping
says -ENOMEM when there is no frame; sbrk() gives back what a growth it could
not finish mapped; a stack that cannot grow says -ENOMEM (its program is ended).
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
    u_long *pdir;                       /* page directory (kernel virtual: 10/25/26, in the window) */
    u_long pdir_phys;
    u_long *tables[USER_PDE_COUNT];     /* user page tables (in the window), NULL until needed */
    u_long brk_start, brk;              /* heap: sbrk() moves brk up from brk_start */
    u_long stack_low;                   /* lowest mapped stack address */
    u_long pages;                       /* user frames owned */
    u_long table_pages;                 /* 10/25/26: frames of its directory and tables */
    struct uvm *reap_next;              /* 10/25/26: on uvmreap's list (kernel/task.c), its task killed on the spot */
};

struct uvm *uvm_create(void);
void uvm_destroy(struct uvm *m);                 /* frees every user frame; m must not be active */
void uvm_switch(struct uvm *m);                  /* load the directory (NULL: the kernel's) */
struct uvm *uvm_current(void);
int uvm_map_page(struct uvm *m, u_long virt, u_long flags);       /* a zeroed frame at virt (PTE_W to write):
                                                                     0, -ENOMEM, -EINVAL (10/25/26) */
int uvm_map_range(struct uvm *m, u_long start, u_long len, u_long flags);
u_long uvm_lookup(struct uvm *m, u_long virt);                     /* physical address or 0 */
bool uvm_check(struct uvm *m, u_long start, u_long len, bool write);  /* user memory, present (stack grows) */
bool uvm_check_str(struct uvm *m, const char *s, u_long max);      /* a NUL-terminated user string */
int uvm_grow_stack(struct uvm *m, u_long addr);                    /* page fault below stack_low: 0, -1 (not the
                                                                     stack's), -ENOMEM (10/25/26) */
u_long uvm_sbrk(struct uvm *m, long incr);                         /* the old brk, or 0 on failure */
u_long kernel_pdir_phys(void);

#endif
