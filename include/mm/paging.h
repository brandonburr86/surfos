/*
    SurfOS Paging Header -- (C)2004 Brandon Burr
*/

#ifndef _MM_PAGING_H
#define _MM_PAGING_H

void init_paging(); //init paging!
inline void *getpage();

inline void *mm_pop_page();
inline void mm_push_page(void *page);

void exPageFault();

#endif
