/*
SurfOS DMA Driver
--------------------
File: dma.h Date: 7/13/04
--------------------
(C)2004 Brandon Burr
*/
#ifndef _SYS_DMA_H
#define _SYS_DMA_H

#include <surfos/types.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

typedef struct {
    char page;
    u_int offset;
    u_int length;
} DMA_block;

#define DMASIZE 65536 //64K
#define DHEAP_START 0x8000
#define DHEAP_END 0x48000//this spans 256K... 4 * 64K

extern struct surf_alloc_desc *dmaList; //linked list for DMA allocation
extern struct surf_alloc_desc *dmaDList; //linked list for DMA deallocation


void init_dma();
u_int dma_stuff(u_int port);

void LoadPageAndOffset(DMA_block *blk, char *data);
void StartDMA(u_char DMA_channel, DMA_block *blk, u_char mode);
void PauseDMA(u_char DMA_channel);
void UnpauseDMA(u_char DMA_channel);
void StopDMA(u_char DMA_channel);
u_int DMAComplete(u_char DMA_channel);

#endif
