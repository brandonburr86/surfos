#ifndef _MM_KALLOC_H
#define _MM_KALLOC_H

/*
SurfOS kernel allocation function prototypes
Copyright (C)2004 Brandon Burr, heap v2 10/2026 (roadmap M3)
*/
#include <mm/memory.h>

void *kalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *mem, size_t size);
void kfree(void *mem);

/* the 1:1 mapped DMA heap: use for anything a device reads or writes */
void *kalloc_dma(size_t size);
void kfree_dma(void *mem);
#define palloc kalloc_dma   /* 2004 names */
#define pfree kfree_dma

struct heap_stats {
    const char *name;
    u_long start, brk, limit;
    u_long bytes_used, bytes_free;
    u_long blocks_used, blocks_free;
    u_long grows;
};
void kheap_stats(struct heap_stats *st);
void dmaheap_stats(struct heap_stats *st);
bool kheap_check(void);      /* walk every block; false (after a message) when the heap is corrupt */
void heap_init(void);

#endif
