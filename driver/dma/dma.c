/*
SurfOS DMA Driver
--------------------
File: dma.c Date: 7/13/04
--------------------
(C)2004 Brandon Burr
*/

#include <asm/io.h>
#include <sys/dma.h>
#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>

/* Defines for accessing the upper and lower byte of an integer. */
#define LOW_BYTE(x)         (x & 0x00FF)
#define HI_BYTE(x)          ((x & 0xFF00) >> 8)

#define FP_SEG(x) ((u_long)x / 0x1000)
#define FP_OFF(x) ((u_long)x % 0x1000)

/* Quick-access registers and ports for each DMA channel. */
u_char MaskReg[8]   = { 0x0A, 0x0A, 0x0A, 0x0A, 0xD4, 0xD4, 0xD4, 0xD4 };
u_char ModeReg[8]   = { 0x0B, 0x0B, 0x0B, 0x0B, 0xD6, 0xD6, 0xD6, 0xD6 };
u_char ClearReg[8]  = { 0x0C, 0x0C, 0x0C, 0x0C, 0xD8, 0xD8, 0xD8, 0xD8 };

u_char PagePort[8]  = { 0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A };
u_char AddrPort[8]  = { 0x00, 0x02, 0x04, 0x06, 0xC0, 0xC4, 0xC8, 0xCC };
u_char CountPort[8] = { 0x01, 0x03, 0x05, 0x07, 0xC2, 0xC6, 0xCA, 0xCE };




void dma_xfer(int chan, char *data, int size, bool write) {
    //      dma xfer(2,tbaddr,512,FALSE);
    DMA_block blk;
    LoadPageAndOffset(&blk,data);
}

void LoadPageAndOffset(DMA_block *blk, char *data) {
    u_int temp, segment, offset;
    u_long foo;
    segment = FP_SEG(data);
    offset  = FP_OFF(data);
    blk->page = (segment & 0xF000) >> 12;
    temp = (segment & 0x0FFF) << 4;
    foo = offset + temp;
    if (foo > 0xFFFF) blk->page++;
    blk->offset = (u_int)foo;
}

void StartDMA(u_char DMA_channel, DMA_block *blk, u_char mode) {
    /* First, make sure our 'mode' is using the DMA channel specified. */
    mode |= DMA_channel;

    /* Don't let anyone else mess up what we're doing. */
    KCRIT_ENTER

    /* Set up the DMA channel so we can use it.  This tells the DMA */
    /* that we're going to be using this channel.  (It's masked) */
    outb(MaskReg[DMA_channel], 0x04 | DMA_channel);

    /* Clear any data transfers that are currently executing. */
    outb(ClearReg[DMA_channel], 0x00);

    /* Send the specified mode to the DMA. */
    outb(ModeReg[DMA_channel], mode);

    /* Send the offset address.  The first byte is the low base offset, the */
    /* second byte is the high offset. */
    outb(AddrPort[DMA_channel], LOW_BYTE(blk->offset));
    outb(AddrPort[DMA_channel], HI_BYTE(blk->offset));

    /* Send the physical page that the data lies on. */
    outb(PagePort[DMA_channel], blk->page);

    /* Send the length of the data.  Again, low byte first. */
    outb(CountPort[DMA_channel], LOW_BYTE(blk->length));
    outb(CountPort[DMA_channel], HI_BYTE(blk->length));

    /* Ok, we're done.  Enable the DMA channel (clear the mask). */
    outb(MaskReg[DMA_channel], DMA_channel);

    /* Re-enable interrupts before we leave. */
    KCRIT_LEAVE
}

void PauseDMA(u_char DMA_channel) {
    /* All we have to do is mask the DMA channel's bit on. */
    outb(MaskReg[DMA_channel], 0x04 | DMA_channel);
}

void UnpauseDMA(u_char DMA_channel) {
    /* Simply clear the mask, and the DMA continues where it left off. */
    outb(MaskReg[DMA_channel], DMA_channel);
}

void StopDMA(u_char DMA_channel) {
    /* We need to set the mask bit for this channel, and then clear the */
    /* selected channel.  Then we can clear the mask. */
    outb(MaskReg[DMA_channel], 0x04 | DMA_channel);

    /* Send the clear command. */
    outb(ClearReg[DMA_channel], 0x00);

    /* And clear the mask. */
    outb(MaskReg[DMA_channel], DMA_channel);
}

u_int DMAComplete(u_char DMA_channel) {
    /* Register variables are compiled to use registers in C, not memory. */
    register int z;

    z = CountPort[DMA_channel];
    outb(0x0C, 0xFF);

    /* This *MUST* be coded in Assembly!  I've tried my hardest to get it */
    /* into C, and I've had no success.  :(  (Well, at least under Borland.) */


    return dma_stuff(z);
}

void init_dma() {
    kprintf("*Initializing DMA\n");
    kprintf("*DONE\n");
}
