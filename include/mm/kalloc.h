#ifndef _MM_KALLOC_H
#define _MM_KALLOC_H

/*
SurfOS kernel allocation function prototypes
Copyright (C)2004 Brandon Burr
*/
#include <mm/memory.h>

void addAllocItem(struct surf_alloc_desc **list, struct surf_alloc_desc *item);
struct surf_alloc_desc *delAllocItem(struct surf_alloc_desc **from,struct surf_alloc_desc **to, u_long address);


void *kalloc(size_t size);
void kfree(void *mem);
void *ksbrk(size_t size);
void *asbrk(size_t size);


//DMA allocation
void *palloc(size_t size);
void pfree(void *mem);
void *psbrk(size_t size);


#endif
