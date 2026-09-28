/*
SurfOS DMA Driver memory manager
--------------------
File: dma-mm.c  Date: 7/14/04
--------------------
(C)2004 Brandon Burr
*/

#include <surfos/types.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <surfos/task.h>
#include <mm/kalloc.h>
#include <sys/dma.h>
#include <surfos/system.h>

void *dma_alloc();
void dma_free(void *mem);
void *dmabrk();

struct surf_alloc_desc *dmaList; //linked list for DMA allocation
struct surf_alloc_desc *dmaDList; //linked list for DMA deallocation


void *dma_alloc() {
    KCRIT_ENTER
    struct surf_alloc_desc *cur = dmaDList;

    while(!cur) cur = cur->next;

    if(cur) {
            cur = delAllocItem(&dmaDList,&dmaList,cur->address);
            KCRIT_LEAVE
            if(!cur) return NULL;
            return (void*)(cur->address);
    }

    //nothing found. allocate from the heap;
    struct surf_alloc_desc *ret = asbrk(sizeof(struct surf_alloc_desc));
    ret->address = (u_long)dmabrk();

    ret->size = DMASIZE;
    addAllocItem(&dmaList, ret);
    kprintf("allocated %i bytes at memory 0x%x\n",DMASIZE,ret->address);
    KCRIT_LEAVE
    return (void*)(ret->address);
}

void dma_free(void *mem) {
    KCRIT_ENTER
    struct surf_alloc_desc *cur = dmaList;
    while(cur) {
        if(cur->address == (u_long)mem) { //look for the descriptor in the alloc list
            delAllocItem(&dmaList, &dmaDList, cur->address);
            KCRIT_LEAVE
            return;
        }
        cur = cur->next;
    }
    KCRIT_LEAVE
    //couldnt find it, error
    kprintf("dma_free: invalid memory address\n");
}

//allocates from the physical DMA heap.. basically same as ksbrk()
void *dmabrk() {
    KCRIT_ENTER
    static u_char *dmaHeap = (u_char*)DHEAP_START;
    if(dmaHeap>=(u_char*)DHEAP_END) {
        kprintf("Fatal: DMA 1:1 Address Space Depleted!\n\nHALTING");
        halt();
        KCRIT_LEAVE
        return NULL;
    }

    void *ret = dmaHeap;
    dmaHeap += DMASIZE;
    KCRIT_LEAVE
    return ret;
}
