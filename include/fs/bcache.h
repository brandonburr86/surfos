/*
SurfOS Block Cache
----------------------
File: bcache.h  Date: 10/3/26 (roadmap F1)
----------------------
A small write-back cache of 512-byte blocks in front of the block devices.
File systems read metadata and data through it; bsync() writes the dirty
blocks out. A buffer stays valid while the caller holds it (bread ... brelse);
file systems serialize their own updates with the superblock mutex, the cache
only protects its table.
*/

#ifndef _FS_BCACHE_H
#define _FS_BCACHE_H

#include <surfos/types.h>

#define BCACHE_BLOCKS 128

struct bdev;

struct buf {
    struct bdev *dev;
    u32 lba;
    u8 data[512];
    bool valid, dirty;
    u_int refs;
    u_long last_used;
};

void init_bcache(void);
struct buf *bread(struct bdev *dev, u32 lba);        /* the block's contents; NULL on an I/O error */
struct buf *bget(struct bdev *dev, u32 lba);         /* the buffer without reading (it will be fully overwritten) */
void bdirty(struct buf *b);                          /* changed: write it back later */
void brelse(struct buf *b);
int bsync(struct bdev *dev);                         /* write dirty blocks of dev (NULL: of every device) */
void binvalidate(struct bdev *dev);                  /* drop every block of dev (after bsync, at umount) */
void bcache_stats(u_long *hits, u_long *misses, u_long *writes);

#endif
