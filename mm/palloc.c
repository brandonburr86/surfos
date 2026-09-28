/*
SurfOS Physical Allocation MM (intended for DMA)
--------------------
File: palloc.c  Date: 7/3/04
--------------------
(C)2004 Brandon Burr
*/

#include <surfos/types.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <surfos/task.h>
#include <mm/kalloc.h>
#include <blibc_common.h>
#include <surfos/system.h>

/*
    This code is functionally equivalent to kalloc.c, except for the fact that it allocates
    from a pre-mapped heap going from 0xA00000 to 0xF00000. This memory has a 1:1 mapping,
    so that the developer can simply use the virutal = physical logic when constructing
    device drivers. DMA needs the physical address to write to.. so the physical memory
    allocater does this.
*/


void *palloc(size_t size);
void pfree(void *mem);
void *psbrk(size_t size);

struct surf_alloc_desc *pallocList; //linked list for DMA allocation
struct surf_alloc_desc *pdeallocList; //linked list for DMA deallocation


void *palloc(size_t size) { //best fit algorithm.. to conserve memory
    KCRIT_ENTER
    struct surf_alloc_desc *cur = pdeallocList, *best=NULL;

    while(cur) {
        if(cur->size >= size) if(cur->size <= best->size) best=cur; //found a better fit
        cur = cur->next;
    }

    if(best) { //the best fit in the dealloc pool
            best = delAllocItem(&pdeallocList,&pallocList,best->address);
            KCRIT_LEAVE
            if(!best) return NULL;
            return (void*)(best->address);
    }

    //nothing found. allocate from the heap;
    struct surf_alloc_desc *ret = asbrk(sizeof(struct surf_alloc_desc));
    ret->address = (u_long)psbrk(size);

    ret->size = size;
    addAllocItem(&pallocList, ret);
    //kprintf("allocated %i bytes at memory 0x%x\n",size,ret->address);
    KCRIT_LEAVE
    return (void*)(ret->address);
}

void pfree(void *mem) {
    KCRIT_ENTER
    struct surf_alloc_desc *cur = pallocList;
    while(cur) {
        if(cur->address == (u_long)mem) { //look for the descriptor in the alloc list
            delAllocItem(&pallocList, &pdeallocList, cur->address);
            KCRIT_LEAVE
            return;
        }
        cur = cur->next;
    }
    KCRIT_LEAVE
    //couldnt find it, error
    kprintf("pfree: invalid memory address\n");
}

//allocates from the physical DMA heap.. basically same as ksbrk()
void *psbrk(size_t size) {
    KCRIT_ENTER
    static u_char *dmaHeap = (u_char*)PHEAP_START;
    if(dmaHeap>=(u_char*)PHEAP_END) {
        kprintf("Fatal: DMA 1:1 Address Space Depleted!\n\nHALTING");
        halt();
        KCRIT_LEAVE
        return NULL;
    }

    void *ret = dmaHeap;
    dmaHeap += size;
    KCRIT_LEAVE
    return ret;
}
