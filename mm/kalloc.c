/*
SurfOS kernel memory allocation
Copyright (C)2004 Brandon Burr
*/

#include <surfos/types.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <surfos/system.h>
#include <surfos/interrupt.h>
#include <surfos/task.h>
#include <mm/kalloc.h>
#include <stdio.h>

struct surf_alloc_desc *allocList; //linked list for allocation
struct surf_alloc_desc *deallocList; //linked list for deallocation

/**************** ALLOC LINKED LIST HANDLER*************/

/*
    This appends an allocation descriptor on a linked list during an allocation.
    Highly necessary.
*/
void addAllocItem(struct surf_alloc_desc **list, struct surf_alloc_desc *item) {
    KCRIT_ENTER
    if(!*list) {
        *list=item;
        item->next = NULL;
    } else {
        struct surf_alloc_desc *p = *list;
        *list=item;
        item->next = p;
    }
    KCRIT_LEAVE
}

/*
    This removes a given descriptor from one list, and automatically appends it to the
    inverse list. For example, when memory is deallocated it needs to be put on the
    deallocation pool!
    Highly necessary.
*/
struct surf_alloc_desc *delAllocItem(struct surf_alloc_desc **from,struct surf_alloc_desc **to, u_long address) {
    if(!address) return NULL;
    KCRIT_ENTER
    struct surf_alloc_desc *prev=NULL, *ptr=*from;
    while(ptr) {
        if(ptr->address == address) {
            if(ptr==*from) *from=ptr->next;
            if(prev) prev->next = ptr->next;
            addAllocItem(to,ptr);
            KCRIT_LEAVE
            return ptr;
        }
        prev=ptr;
        ptr=ptr->next;
    }
    KCRIT_LEAVE
    return NULL;
}


void print_list(struct surf_alloc_desc *start) { //for testing
    KCRIT_ENTER
    struct surf_alloc_desc *ptr=start;
    while(ptr) {
        printf("\'0x%x\':%i -> ",ptr->address,ptr->size);
        ptr = ptr->next;
    }
    printf("\n");
    KCRIT_LEAVE
}

/**************** LINKED LIST END *************/

/*
    Two different algorithms are provided here for kernel use. One is the first fit algorithm
    that searches the free pool for the first biggest one it can find. This is fast.. but not
    efficient. The next algorithm is the best fit. This searches through the entire free pool
    once O(n) and determines which is the smallest descriptor that still fits. I personally
    choose the best fit for the default algorithm.. because its more efficient. Note that it
    may have slignt performance implications.
*/

//#define FIRSTFIT //if this is not defined then the best fit algorithm will be used.

#ifdef FIRSTFIT //use first fit algorithm

void *kalloc(size_t size) {
    KCRIT_ENTER

    //search the deallocation pool for the first fit it can find
    struct surf_alloc_desc *cur = deallocList;
    while(cur) {
        if(cur->size >= size) {
            cur = delAllocItem(&deallocList,&allocList,cur->address);
            KCRIT_LEAVE
            if(!cur) return NULL;
            return (void*)(cur->address);
        }
        cur = cur->next;
    }

    //nothing found. allocate from the heap;
    struct surf_alloc_desc *ret = asbrk(sizeof(struct surf_alloc_desc));
    ret->address = ksbrk(size);
    ret->size = size;
    addAllocItem(&allocList,ret);
    KCRIT_LEAVE
    return (void*)(ret->address);
}

#else //Best Fit
void *kalloc(size_t size) {
    KCRIT_ENTER
    struct surf_alloc_desc *cur = deallocList, *best=NULL;

    //search the entire deallocation pool, and yield the smallest fit
    while(cur) {
        if(cur->size >= size) if(cur->size <= best->size) best=cur; //found a better fit
        cur = cur->next;
    }

    if(best) { //the best fit in the dealloc pool
            best = delAllocItem(&deallocList,&allocList,best->address);
            KCRIT_LEAVE
            if(!best) return NULL;
            return (void*)(best->address);
    }

    //nothing found. allocate from the heap;
    struct surf_alloc_desc *ret = asbrk(sizeof(struct surf_alloc_desc)); //allocation descriptor

    ret->address = (u_long)ksbrk(size); //allocate from the heap
    ret->size = size;
    addAllocItem(&allocList, ret);
    KCRIT_LEAVE
    return (void*)(ret->address);
}
#endif

/*
    Quite simple... remove the allocation descriptor from the allocated pool... to the
    deallocated pool!
*/
void kfree(void *mem) {
    KCRIT_ENTER
    struct surf_alloc_desc *cur = allocList;
    while(cur) {
        if(cur->address == (u_long)mem) { //look for the descriptor in the alloc list
            delAllocItem(&allocList, &deallocList, cur->address);
            KCRIT_LEAVE
            return;
        }
        cur = cur->next;
    }
    KCRIT_LEAVE
    //couldnt find it, error
    kprintf("kfree: invalid memory address\n");
}

//allocates from the kernel memory heap
void *ksbrk(size_t size) {
    KCRIT_ENTER
    static u_char *kernHeap = (u_char*)KHEAP_START;
    if(kernHeap>=(u_char*)KHEAP_END) {
        kprintf("Fatal: Kernel Address Space Depleted!\n\nHALTING");
        halt();
        KCRIT_LEAVE
    return NULL;
    }

    void *ret = kernHeap;
    kernHeap += size;
    KCRIT_LEAVE
    return ret;
}

//allocates from the heap containing allocation descriptors
// so that any potential kernel heap overflows dont break a linked list or something.
void *asbrk(size_t size) {
    KCRIT_ENTER
    static u_char *allocHeap = (u_char*)ALLOC_HEAP_START;
    if(allocHeap>=(u_char*)ALLOC_HEAP_END) {
        kprintf("Fatal: Kernel Allocation Heap Depleted!\n\nHALTING");
        halt();
        KCRIT_LEAVE
        return NULL;
    }

    void *ret = allocHeap;

    allocHeap += size;

    KCRIT_LEAVE
    return ret;
}

void print_lst_count() {
    KCRIT_ENTER
    struct surf_alloc_desc *cur = pallocList;
    u_long alloc=0,dealloc=0;
    while(cur) {
        alloc++;
        cur = cur->next;
    }

    cur=pdeallocList;
    while(cur) {
        dealloc++;
        cur = cur->next;
    }
    KCRIT_LEAVE
    printf("alloc: %i\n dealloc: %i\n",alloc,dealloc);
}
