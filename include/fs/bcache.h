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

10/5/26 (USB storage): 1024 blocks; no lock is held while a device works, so
a slow USB request holds up only the tasks that need that very block. A miss
can read the following blocks in the same request (breada), dirty neighbours
are written out together, and bread_blocks/bwrite_blocks move runs of blocks
straight between a device and the caller without filling the cache, coherent
with the copies it holds. A holder that changes a block calls bdirty() after
the change.
*/

#ifndef _FS_BCACHE_H
#define _FS_BCACHE_H

#include <surfos/types.h>

#define BCACHE_BLOCKS 1024
#define BCACHE_RUN    128               /* the most blocks one device request moves: 64 KB */

struct bdev;

struct buf {
    struct bdev *dev;
    u32 lba;
    u8 data[512];
    bool valid, dirty;
    bool loading;                       /* being read in: wait for it (the contents are not there yet) */
    bool writing;                       /* being written out: usable, but not reused or written by another */
    bool hashed;
    u32 gen;                            /* bumped by bdirty(): a write-back only cleans what it wrote */
    u_int refs;
    struct buf *hnext, *hprev;          /* hash chain */
    struct buf *newer, *older;          /* least recently used order */
};

void init_bcache(void);
struct buf *bread(struct bdev *dev, u32 lba);        /* the block's contents; NULL on an I/O error */
struct buf *breada(struct bdev *dev, u32 lba, u32 n);/* bread; a miss also reads the next n-1 blocks in the same request */
struct buf *bpeek(struct bdev *dev, u32 lba);        /* the block if it is cached (held), else NULL; no I/O */
struct buf *bget(struct bdev *dev, u32 lba);         /* the buffer without reading (it will be fully overwritten) */
void bdirty(struct buf *b);                          /* changed: write it back later */
void brelse(struct buf *b);
int bsync(struct bdev *dev);                         /* write dirty blocks of dev (NULL: of every device) */
void binvalidate(struct bdev *dev);                  /* drop every block of dev (after bsync, at umount) */
void bcache_stats(u_long *hits, u_long *misses, u_long *writes);

/* 10/5/26: count blocks from lba straight from or to the device, through the cache's 64 KB
   bounce buffer, so the device never sees the caller's memory. A read returns what the cache
   holds where it holds a block; a write updates the copies the cache holds. 0 or BDEV_E*. */
int bread_blocks(struct bdev *dev, u32 lba, u32 count, void *dst);
int bwrite_blocks(struct bdev *dev, u32 lba, u32 count, const void *src);
int bzero_blocks(struct bdev *dev, u32 lba, u32 count);

/* 10/5/26: the requests the cache has sent to the devices, and the blocks they moved */
struct bcache_io {
    u_long reads, read_blocks;
    u_long writes, written_blocks;
};
void bcache_io_stats(struct bcache_io *io);

#endif
