/*
    SurfOS Paging Header -- (C)2004 Brandon Burr
*/

#ifndef _MM_PAGING_H
#define _MM_PAGING_H

#include <surfos/trap.h>

void init_paging(); //init paging!
inline void *getpage();

inline void *mm_pop_page();
inline void mm_push_page(void *page);

u_long *page_fault_trap(struct trapframe *tf);

#endif
