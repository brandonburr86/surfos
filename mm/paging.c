/*
SurfOS Paging
Copyright (C)2004 Brandon Burr
*/

#include <surfos/types.h>
#include <surfos/bit.h>
#include <mm/memory.h>
#include <surfos/task.h>
#include <surfos/system.h>
#include <mm/paging.h>
#include <surfos/trap.h>
#include <blibc_common.h>

/*
    These variables are essential to the kernel memory manager:

    page directory holds the page directory that is to be loaded into cr3
    page table holds all the page table entries for the kernel
    page stack holds the complete stack for all the possible free physical page addresses
 */

static u_long pd_storage[1024] __attribute__((aligned(4096))); /* used to live at 0x9C000, inside the EBDA on some machines */
u_long *page_directory = pd_storage;
u_long *page_table = (u_long*)PTBL_START;
u_long *page_stack = (u_long*)PSTK_START;

/*
    These are stack pointers that make up the free physical page stack.
 */
u_long *stack_top, *stack_btm, *stack_cur;


void enablePaging(void);

/*
    This function determines all available memory on the system by testing each address. Returns byte count.
 */
u_long memprobe() {
    #define PADDR_LIMIT 0xBFFFFFFF
    #define MEMPROBE_MAGIC 0x5A5AA5A5

    volatile u_long *m; /* volatile: the read-back below is the whole point (audit A6) */
    u_long i,j,temp;
    m = (volatile u_long *)0;
    i = 0x200000;//0x40000

    temp = m[i];    // save copy of what we will be modifying
    m[i] = MEMPROBE_MAGIC;
    while(m[i] == MEMPROBE_MAGIC && (i << 2) < PADDR_LIMIT + 1) {
        m[i] = temp;    // restore value in memory
        i += 0x400; // go to the next page
        temp = m[i];    // save copy of what we will be modifying
        m[i] = MEMPROBE_MAGIC;
        for(j=0; j<100; j++){}; // short delay loop (some systems will lock or report incorrect sizes if we don't delay here)
    }
    return i << 2;
}


/*
    Initially paging is enabled for the kernel. This is to be done early!!!
 */
void enablePaging(void) {
    asm("movl %0,%%eax\n" :: "m" (page_directory));
    asm("movl %eax,%cr3\n");
    asm("movl %cr0,%eax\n");
    asm("orl $0x80000000,%eax\n");
    asm("movl %eax,%cr0\n");
    asm("jmp 1f\n");
    asm("1:\n");
    asm("movl $1f,%eax\n");
    asm("jmp *%eax\n");
    asm("1:");
    return;
}

/*
    This sets all the avaiable pages on the system as not in use. It also sets up the stack of free physical
    pages for use by the memory mapper.
 */
void init_paging() {
    unsigned int i,pcnt;
    u_long physAddr;

    //clear page table and directory
    memset(page_table,0,PTBL_SIZE);
    memset(page_directory,0,PDIR_SIZE);
    memset(page_stack,0,PSTK_SIZE);

    stack_top = (u_long*)(PSTK_START+PSTK_SIZE);
    stack_btm = (u_long*)PSTK_START;
   stack_cur = stack_btm;

    for(i=0;i<1024*1024;i++) { // Init the page table
        page_table[i] = 0 | 2;
    }

    for(i=0;i<1024;i++) { // Init the page directory
        page_directory[i] = 0 | 2;
    }

    setupSurfKAS(); //set up kernel address space

    pcnt=0;

    //push all free pages to the stack
    for(physAddr=PMEM_START;physAddr<PMEM_END;physAddr += PAGE_SIZE) {
        mm_push_page((void*)physAddr);
    }

    enablePaging(); //enable paging!!!!!!
}


/*
    This is a very important function. It maps 1:1 the kernel memory where it is needed.
    Right now it maps 4 page directory entries (4dir entries * 4MB = 16MB)

    The 10MB is mostly used for the page table, and free page stack.
    The last 6MB is used for the physical allocation manager, for DMA.. etc.
 */
void setupSurfKAS() { //set up SurfOS kernel address space

    u_long i,address = 0;
    for(i=0; i<1024*4; i++) {
        page_table[i] = address | 3;
        address = address + 4096;
    }

    // fill the first entry of the page directory
  for(i=0;i<4;i++) {
    // attribute set to: supervisor level, read/write, present(011 in binary)
      page_directory[i] = (u_long)(page_table+(i*1024));
      page_directory[i] = (u_long)(page_directory[i] | 3);
    }
}

/*
    This function maps a physical page to a page table entry during a page fault. This is ESSENTIAL to my demand
    paging model. This function needs to be fast.. no questions asked.
 */
inline void map_page(u_long *page, u_long address) {
    register u_long iDir, iTbl;
    u_long *pTmp,*pd;
    //get address locations
    if(page < (u_long*)PMEM_START || page > (u_long*)PMEM_END) BUG();

    iDir = (address & PGDIR_MASK)>>22;
    iTbl = (address & PGTBL_MASK)>>12;
    pd=&page_directory[iDir]; //wierd.... for some time i think this worked w/o a '&'??

    //set the page directory entry
    pTmp = (u_long*)((u_long)page_table + (iDir << 12));
    *pd = (u_long)pTmp | 3;

    //set the page table entry
    *(pTmp+=iTbl) = (u_long)page | 3;
}

/*
 ahh.. the infamous page fault.. better make this quick!.  For those who dont know.. this is called every time
 an area of memory is accessed that is not mapped. For the kernel this is simple.. just map the page, no
  questions asked..
 */
u_long *page_fault_trap(struct trapframe *tf) {
    u_long cr2=0;

    asm volatile("movl %%cr2, %0" :"=r"(cr2)); //cr2 holds the faulted memory address

    map_page(mm_pop_page(),cr2); // map a free page to the faulted address
    return (u_long*)tf;
}


/* Is a linear address backed by a present page? The backtrace checks every frame with this
   so a corrupt frame pointer cannot fault inside a panic. */
bool vmm_is_mapped(u_long addr) {
    if(!(page_directory[addr >> 22] & 1)) return false;
    return (page_table[addr >> 12] & 1) ? true : false;
}

/*
    Quite a redundant function if i do say so myself.
    /me wonders why I put this in here.
 */
inline void *getpage() { //retrieve a single page
    return mm_pop_page();
}


/*
    This function looks up a linear address in the page table, and returns what physical memory address it
    corresponds to. This type of function is normally used for DMA and stuff.. like Martin needed it.. then
    he says he doesnt need a memory manger for the Wazerbox... /me kicks martin
*/
void *mm_lookup_linear(void *linear) {
    /*
        To compute this.. basically for every 4MB add one page directory number.. and then for each 4KB add
        one page tbl nubmer.. its quite simple.
    */
    int pgdir,pgtbl,index;
    pgtbl = VIRT_TO_PGTBL((u_long)linear);
    pgdir = VIRT_TO_PGDIR((u_long)linear);

    index = (pgdir*1024)+pgtbl;

    return (void*)(page_table[index] & 0xFFFFF000)+ ((u_long)linear % 0x1000); //fancy
}
