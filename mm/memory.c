/*
    SurfOS Memory Manager -- (C)2004 Brandon Burr
*/

#include <surfos/types.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <surfos/interrupt.h>
#include <surfos/task.h>
#include <surfos/system.h>

#include <blibc_common.h>

u_long PMEM_END; //the variable holding the ending address of physical memory

u_long memprobe();

/* SurfOS requires 32MB of RAM to run!!! */
void run_memcheck() {
    if(PMEM_SIZE < (1024*1024*32)-0x1000000) { //less than 32MB of memory...
        kprintf("\nKernel Panic: Less than 32MB of RAM detected.\n");
        kprintf("SYSTEM HALTED\n");
        asm("cli");
        asm("hlt");
    }
}

/*
    Basically null the allocation linked list pointers
*/
void init_mem() {
    fillMemInfo();

    allocList = NULL;
    deallocList=NULL;

    pallocList = NULL;
    pdeallocList=NULL;

    init_paging();
}

void fillMemInfo() {
  PMEM_END=memprobe()-PAGE_SIZE; //take off a page at the end, dont want to go over
}

extern struct surf_alloc_desc *allocList, *deallocList;

void printMemInfo() {

    u_long kPageAlloc=0,kPageDeAlloc=0;
    u_long kByteAlloc=0,kByteFree=0;

    KCRIT_ENTER
    u_long kMax = KHEAP_END-KHEAP_START;
    struct surf_alloc_desc *cur = allocList;

    while(cur) {
        kPageAlloc++;
        kByteAlloc+=cur->size;
        cur = cur->next;
    }

    cur=deallocList;
    while(cur) {
        kPageDeAlloc++;
        kByteFree+=kByteFree;
        cur = cur->next;
    }
    KCRIT_LEAVE

    printf("\nSurfOS Memory Statistics\n");
    printf("--------------------------\n");
  printf("Physical memory: %iKB\n",(PMEM_SIZE>>10));
  printf("Memstart: 0x%x  Memend: 0x%x\n",PMEM_START,PMEM_END);
  printf("Free physical pages: %d\n\n",PMEM_PCNT);
  printf("Kernel Memory\n");
  printf("-------------\n");
  printf("Total kernel memory: %i KB\n",kMax/1024);
  printf("Used kernel memory: %i KB\n",kByteAlloc/1024);
  printf("Freed kernel memory: %i KB\n",kByteFree/1024);
  printf("Total free kern memory: %i KB\n\n",(kMax-kByteAlloc)/1024);
}

/****************************
mm pop page:   (6/7/04)
    Helped with problem by Josh Carlson
****************************/
//pop a page off of the stack
inline void *mm_pop_page() {
    if(stack_cur<=stack_btm) {
        kprintf("mm_pop_page: stack underflow\n");
        BUG();
        return NULL;
    }
    --stack_cur;
    return (void*)*(stack_cur+1);
}

 //push a free page onto the stack
inline void mm_push_page(void *page) {
    if(stack_cur>=stack_top) {
        kprintf("mm_push_page: stack overflow\n");
        BUG();
        return;
    }
    ++stack_cur;
    *stack_cur = (u_long)page;
}
