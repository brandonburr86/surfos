/*
SurfOS board: SurfCPU on the Arty A7 -- an SD card as a block device, and its card detect
----------------------
File: sdblk.h   Date: 10/16/26 (SurfCPU, milestone 8)
----------------------
What the Arty's card (sd.c, through surf_spi) and QEMU's simulated one (boards/qemu_virt/
sdsim.c) share above the SD protocol: the block device sd0 over sdcard.c, and a task that
watches card detect, which surf_spi has no interrupt for. See sdblk.c.
*/

#ifndef _SDBLK_H
#define _SDBLK_H

#include <surfos/types.h>
#include <surfos/sync.h>
#include "sdcard.h"

struct bdev;

struct sdblk {
    /* the board's, before sdblk_attach() */
    const char *tag;                    /* the console's prefix: "SD" for +SD: ... */
    const struct sd_host *host;
    const char *model;                  /* the disk's model; NULL: the card's CID in words */
    void (*announce)(struct sdblk *s);  /* says a card that was just registered (at boot and when put in) */
    bool readonly;                      /* how a card's disk starts out (the Arty: no sd=rw) */
    /* the state */
    mutex_t lock;                       /* the card and disk (a transfer holds it) */
    struct sdcard card;
    struct bdev *disk;                  /* the card's disk while it is in, else NULL */
    int status;                         /* the last initialization: SD_OK or why not */
    bool failed;                        /* a card is in that would not initialize: left alone until it is out */
    bool watching;
    u_long taken_out, put_in;           /* what the watch saw */
};

int sdblk_attach(struct sdblk *s);      /* initialize the card and register it: 0, 1 no card, < 0 failed */
void sdblk_watch(struct sdblk *s);      /* start the task that watches card detect */
void sdblk_set_readonly(struct sdblk *s, bool ro);   /* the disk and its partitions, and cards put in later */
void sdblk_size(u64 blocks, char *buf, size_t size); /* "64 MB", "32 GB" */

#endif
