/*
    SurfOS Memory Manager Header -- (C)2004 Brandon Burr
*/

#ifndef _MM_MEMORY_H
#define _MM_MEMORY_H

#include <surfos/types.h>

/* SurfOS Memory***


/--------------------------------------------------------\
| 31 .. 22             | 21 .. 12         |  11 .. 0     |
| Page Directory Index | Page Table Index |  Page Offset |
| (10 bits)  (0-1023)  | (10 bits)(0-1023)|  (12 bits)   | <- (0-4095)
\--------------------------------------------------------/

Appropriate Masks:

11111111110000000000000000000000 = 0xFFC00000
00000000001111111111000000000000 = 0x3FF000
00000000000000000000111111111111 = 0xFFF

**************************/

//Masks for virtual addresses to return location in page directories/etc...
#define PGDIR_MASK 0xFFC00000
#define PGTBL_MASK 0x3FF000
#define PGOFF_MASK 0xFFF

#define PAGE_SIZE 4096
#define SPAGE_SIZE 4194304 //4MB super page table size

/*****************************/

/* SurfOS Extended memory space */
#define PMEM_START 0xF00000
extern u_long  PMEM_END;
#define PMEM_SIZE (PMEM_END-PMEM_START)
#define PMEM_PCNT (PMEM_SIZE/PAGE_SIZE)
/********************************/

/** SurfOS Kernel Address Space **/
#define KHEAP_START 0xB0000000
#define KHEAP_END   0xB0F00000 //16MB

//memory allocation heap
#define ALLOC_HEAP_START 0xA0000000
#define ALLOC_HEAP_END   0xAFFFF000

//physical DMA heap
#define PHEAP_START 0xA00000 //10MB
#define PHEAP_END 0xF00000 //10MB

/*********************************/

/* SurfOS Paging tables */

#define PDIR_SIZE 1024 //1KB
#define PTBL_SIZE 0x400000 //4MB Page table

#define PDIR_START 0x9C000
#define PTBL_START 0x200000//9D000

//free page stack
#define PSTK_START 0x600000 //right after the page table
#define PSTK_SIZE 0x400000 //4MB stack maximum

#define VIRT_TO_PGDIR(addr) ((addr)/0x400000)
#define VIRT_TO_PGTBL(addr) (((addr)%0x400000)/0x1000)
/******************************/

// SurfOS Allocation descriptor linked list node
struct surf_alloc_desc {
    u_long address; //address of pointer
    u_long size; //size of allocation
    struct surf_alloc_desc *next;
};


extern struct surf_alloc_desc *allocList,*deallocList; //start of the alloc lists
extern struct surf_alloc_desc *pallocList,*pdeallocList; //start of the palloc lists

extern u_long *stack_top, *stack_btm, *stack_cur;
extern u_char *kernel_mem, *kmem_end; //the kernel memory

void init_mem();
void fillMemInfo();
void setupSurfKAS();

void run_memcheck();
void printMemInfo();
void print_lst_count();

void *mm_lookup_linear(void *linear);

#endif
