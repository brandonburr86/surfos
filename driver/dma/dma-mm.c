/*
SurfOS DMA Driver memory manager
--------------------
File: dma-mm.c  Date: 7/14/04, rewritten 10/2026
--------------------
(C)2004 Brandon Burr

Four 64 KB bounce buffers below 1 MB for the 8237 ISA DMA controller, which can only
address 16 MB and cannot cross a 64 KB boundary. The 2004 version was a copy of
kalloc() that dereferenced NULL on its first call (audit M12).
*/

#include <surfos/types.h>
#include <surfos/irq.h>
#include <sys/dma.h>

#define DMA_SLOTS ((DHEAP_END - DHEAP_START) / DMASIZE)

static bool dma_slot_used[DMA_SLOTS];

void *dma_alloc() {
    u_long flags = irq_save();
    u_int i;
    for(i = 0; i < DMA_SLOTS; i++) {
        if(!dma_slot_used[i]) {
            dma_slot_used[i] = true;
            irq_restore(flags);
            return (void *)(DHEAP_START + i * DMASIZE);
        }
    }
    irq_restore(flags);
    return NULL;
}

void dma_free(void *mem) {
    u_long i = ((u_long)mem - DHEAP_START) / DMASIZE;
    if((u_long)mem < DHEAP_START || i >= DMA_SLOTS || ((u_long)mem % DMASIZE)) return;
    dma_slot_used[i] = false;
}
