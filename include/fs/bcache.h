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

10/24/26 (storage, milestone 1; the SurfCPU branch's audit B1, B3, B6-B10, B13-B16,
B22, B35, B40, brought back): a block whose write failed stays dirty for the next
sync and is never reused before (wfail); a device whose blocks fail keeps at most
BCACHE_HOLD dirty blocks, and its file system's writes fail once it holds that many
(bcache_room(), bget()). Blocks are written in the order a file system gives them
(bdirty_order()): one of order k reaches its device only when the device has no dirty
block of a lower order, checked where every write is issued; a block someone holds is
not written and one being written is not handed out (bpeek() is for readers), so what
reaches the device is what was checked. FAT gives file data 0, its tables 1, directory
entries 2 and FSInfo 3, and syncs where it needs the reverse (fs/fat.c). bsync() returns
0 only when every block that was dirty when it began reached the device; a file system
that syncs holding its volume's lock is then sure of what is on the device, since
nobody else makes its blocks dirty meanwhile. A refused straight write changes nothing
in the cache (its clean copies are dropped: the device may have stored part of it).
An order, not atomicity: a request of several blocks can stop in the middle, and a
stick can lose the block it was writing when it is pulled.
*/

#ifndef _FS_BCACHE_H
#define _FS_BCACHE_H

#include <surfos/types.h>

#define BCACHE_BLOCKS 1024
#define BCACHE_RUN    128               /* the most blocks one device request moves: 64 KB */
#define BCACHE_HOLD   (BCACHE_BLOCKS / 4)   /* 10/24/26 (B6): a device's share of dirty blocks once its writes fail */

struct bdev;

struct buf {
    struct bdev *dev;
    u32 lba;
    u8 data[512];
    bool valid, dirty;
    bool loading;                       /* being read in: wait for it (the contents are not there yet) */
    bool writing;                       /* 10/24/26: being written out: not handed out (getblk() waits), not changed */
    bool hashed;
    bool wfail;                         /* 10/24/26 (B1): its last write failed: dirty until one works, never reused before */
    bool aside;                         /* 10/24/26 (B16): set aside (baside()): not written, no block waits for it */
    bool discard;                       /* 10/24/26 (B35, B40): nobody's bytes: dropped when let go or when its I/O ends */
    u8 order;                           /* 10/24/26 (B3): BORDER_*: when it is written (the lowest it was dirtied with) */
    u32 gen;                            /* bumped by bdirty(): a write-back only cleans what it wrote */
    u32 since;                          /* 10/24/26: the bsync() call under way when it became dirty (bsync()'s debt) */
    u32 pass;                           /* 10/24/26 (B13): the bsync() call that tried it last */
    u_int refs;
    struct buf *hnext, *hprev;          /* hash chain */
    struct buf *newer, *older;          /* least recently used order */
};

void init_bcache(void);
struct buf *bread(struct bdev *dev, u32 lba);        /* the block's contents; NULL on an I/O error */
struct buf *breada(struct bdev *dev, u32 lba, u32 n);/* bread; a miss also reads the next n-1 blocks in the same request */
struct buf *bpeek(struct bdev *dev, u32 lba);        /* the block if it is cached (held), else NULL; no I/O. 10/24/26: also
                                                        one being written: for readers only */
struct buf *bget(struct bdev *dev, u32 lba);         /* the buffer without reading (it will be fully overwritten); 10/24/26:
                                                        NULL also when dev refuses writes and holds its share (B6) */
void bdirty(struct buf *b);                          /* changed: write it back later (file data: BORDER_DATA) */
/* 10/24/26 (B3): the order a device's dirty blocks are written in, lowest first */
#define BORDER_DATA    0                /* file contents, a new directory's zeros (bdirty()) */
#define BORDER_TABLE   1                /* allocation tables */
#define BORDER_DIR     2                /* directory entries, which point at both */
#define BORDER_SUMMARY 3                /* free counts and hints */
#define BORDER_MAX     3
void bdirty_order(struct buf *b, u8 order);          /* bdirty(), written in that order (the lower, if dirty already) */
void brelse(struct buf *b);
/* write the dirty blocks of dev (NULL: of every device). 10/24/26: 0 when every block that was dirty when it began
   reached the device, else -1 (a write failed, or a block could not go: the order, or a holder that did not let go
   in 10 s, B18). Never called holding a block of the device: it waits for holders */
int bsync(struct bdev *dev);
int bsync_order(struct bdev *dev, u8 top);           /* 10/24/26 (B7): bsync() of the orders 0 .. top only */
/* 10/24/26 (B6, B14): before a file system makes blocks of dev dirty: 0, or -1 when dev refuses writes and holds
   BCACHE_HOLD dirty blocks already (the write must fail: EIO), after one more try of its oldest block. A device that
   works and holds that many writes some out first */
int bcache_room(struct bdev *dev);
void binvalidate(struct bdev *dev);                  /* drop every block of dev (after bsync, at umount); 10/24/26 (B40): one
                                                        held or under I/O is dropped when let go or when its I/O ends */
void bcache_stats(u_long *hits, u_long *misses, u_long *writes);
u_int bdirty_count(struct bdev *dev);                /* 10/24/26 (B10): dev's dirty blocks (what an umount gives up) */
/* 10/24/26 (B16, B28): for a file system that frees clusters behind a sync: dev's dirty blocks (up to max lbas;
   how many there are, also past max); one set aside (on) or back (no block waits for it, it is not written, and
   the setting task's own syncs do not owe it; other tasks' syncs wait for it); one dropped once its cluster is
   free (its bytes will never be read; held or under I/O: when let go or when the I/O ends) */
u_int bdirty_blocks(struct bdev *dev, u32 *lbas, u_int max);
void baside(struct bdev *dev, u32 lba, bool on);
void bdiscard(struct bdev *dev, u32 lba);
int bcache_audit(struct bdev *dev);                  /* 10/24/26 (B9): dev's account against its blocks: 0, or how many counts are off */

/* 10/5/26: count blocks from lba straight from or to the device, through the cache's 64 KB
   bounce buffer, so the device never sees the caller's memory. A read returns what the cache
   holds where it holds a block; a write updates the copies the cache holds (10/24/26, B15, B22:
   once the device has the bytes; a refused write leaves the dirty ones as they were and drops
   the clean ones, since the device may have stored part of the request). 0 or BDEV_E*. */
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
