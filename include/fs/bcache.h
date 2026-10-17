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

10/16/26 (SurfCPU, milestone 8; audit B1): a block whose write failed stays
dirty while its device is there, for the next bsync() to try again, and is not
reused meanwhile; bsync() tries each run once and fails when one did.

10/16/26 (audit B3): blocks are written in an order a file system gives them
(bdirty_order()): a block of one order reaches its device only after the dirty
blocks of a lower order it may point at have (10/16/26, B12: every one with a
change older than the block's last change; B3 waited for all of them), by bsync()
and when a block is written to make room. FAT gives file data 0, its tables 1, directory entries 2, FSInfo 3,
so that an interruption leaves what fsck.fat repairs rather than an entry
pointing at data that is not there yet (fs/fat.c says what can still be left;
10/16/26, B7: the order is checked at every write). An order, not atomicity: a
request of several blocks can stop in the middle, and a card can lose the block
it was writing when its power went.

10/16/26 (audit B6): a device whose blocks fail keeps at most BCACHE_HOLD dirty
blocks of file data; past that its new writes fail at once (bcache_room(), bget()).
One refusing device leaves three quarters of the cache to the others; there is no
share across devices. Making room writes nothing else to a device that refuses: a
sync tries again, a failed run block by block (B13: in every sync, each block that
may go), and (B14) bcache_room() asks it again with one write of its oldest block.
B15: what the share holds back exactly (fs/bcache.c).

10/16/26 (audit B12): bsync() returns 0 only when every block that was dirty when it
began reached the device (other tasks' later changes are not waited for), and a block
goes only after the lower-order changes older than its own last change. B16: a file
system may set the failed blocks of clusters it is freeing aside (baside(), bdiscard()).
B18: a block a sync owes is given up on (not written) once syncs have waited 10 s for its hold.
B22: a refused bwrite_blocks() may have changed some of its blocks on the device: the cache drops
its clean copies of them (a dirty one keeps its bytes, the next sync's to write over the device's).
B26: a block set aside is still the debt of other tasks' syncs (they wait for it as for a hold);
each sync counts its own requests (B13). B28: bdirty_blocks() lists a device's dirty blocks.
*/

#ifndef _FS_BCACHE_H
#define _FS_BCACHE_H

#include <surfos/types.h>

#define BCACHE_BLOCKS 1024
#define BCACHE_RUN    128               /* the most blocks one device request moves: 64 KB */
#define BCACHE_HOLD   (BCACHE_BLOCKS / 4)   /* 10/16/26 (B6): a device's share of dirty blocks (bcache_room()) */

struct bdev;

struct buf {
    struct bdev *dev;
    u32 lba;
    u8 data[512];
    bool valid, dirty;
    bool loading;                       /* being read in: wait for it (the contents are not there yet) */
    bool writing;                       /* being written out: usable, but not reused or written by another */
    bool hashed;
    bool wfail;                         /* its last write failed: dirty until a sync writes it, never reused before */
    u8 order;                           /* BORDER_*: when it is written (the lowest it was dirtied with) */
    bool copying;                       /* 10/16/26 (B12): a copy of it is being written: other writes of it wait */
    bool aside;                         /* 10/16/26 (B16): set aside (baside()): not written, nobody's dependency */
    bool stuck;                         /* 10/16/26 (B18): a sync gave up on its present hold */
    u32 fail_pass;                      /* the bsync() call it failed in (each run is tried once per call) */
    u32 skip;                           /* 10/16/26 (B7): the reuse() call that found it cannot be written yet */
    u32 gen;                            /* bumped by bdirty(): a write-back only cleans what it wrote */
    u_long held_since;                  /* 10/16/26 (B18): when a sync first waited for its present hold (0: none) */
    u64 first_seq;                      /* 10/16/26 (B12): its first change the device does not have (dirty) */
    u64 last_seq;                       /*      its last change */
    u64 next_seq;                       /*      its first change since a write or a copy of it began */
    u_int refs;
    struct buf *hnext, *hprev;          /* hash chain */
    struct buf *newer, *older;          /* least recently used order */
    struct buf *dnext, *dprev;          /* 10/16/26 (B12): its device's dirty blocks of its order (bc_head) */
};

void init_bcache(void);
struct buf *bread(struct bdev *dev, u32 lba);        /* the block's contents; NULL on an I/O error */
struct buf *breada(struct bdev *dev, u32 lba, u32 n);/* bread; a miss also reads the next n-1 blocks in the same request */
struct buf *bpeek(struct bdev *dev, u32 lba);        /* the block if it is cached (held), else NULL; no I/O.
                                                        10/16/26 (B19): also one being written: for readers only */
struct buf *bget(struct bdev *dev, u32 lba);         /* the buffer without reading (it will be fully overwritten) */
void bdirty(struct buf *b);                          /* changed: write it back later */
/* 10/16/26 (B3): the order a device's dirty blocks are written in, lowest first */
#define BORDER_DATA    0                /* file contents, a new directory's cluster (bdirty()) */
#define BORDER_TABLE   1                /* allocation tables */
#define BORDER_DIR     2                /* directory entries, which point at both */
#define BORDER_SUMMARY 3                /* free counts and hints */
#define BORDER_MAX     3
void bdirty_order(struct buf *b, u8 order);          /* bdirty(), written in that order (the lower, if dirty already) */
void brelse(struct buf *b);
int bsync(struct bdev *dev);                         /* write dirty blocks of dev (NULL: of every device); -1 when one failed.
                                                        Never with a block of the device held (10/16/26, B7: it waits for holders;
                                                        B18: 10 s for a hold). 10/16/26 (B12): 0 when every block dirty
                                                        at the call reached the device, else -1 */
int bsync_order(struct bdev *dev, u8 top);           /* 10/16/26 (B7): bsync() of the orders 0 .. top only */
/* 10/16/26 (B6): before a file system makes blocks of dev dirty: 0, or -1 when dev refuses writes and
   already holds BCACHE_HOLD dirty blocks (the write must fail: EIO; B14: one write of its oldest block
   tells first whether it takes writes again). A device that works and holds that many writes some
   out first. bget() asks the same. */
int bcache_room(struct bdev *dev);
void binvalidate(struct bdev *dev);                  /* drop every block of dev (after bsync, at umount) */
void bcache_stats(u_long *hits, u_long *misses, u_long *writes);
u_int bdirty_count(struct bdev *dev);                /* 10/16/26 (B10): dev's dirty blocks (umount says what it gives up) */
/* 10/16/26 (B16): for a file system that frees clusters behind an order point: the failed blocks of dev (up to
   max lbas; how many: B21, all of them, also past max), one of them set aside (on) or back (not written, nobody's
   dependency, no debt of the setting task's syncs while aside: B26, other tasks' syncs wait for it), and one
   dropped once its cluster is free (its bytes will never be read) */
u_int bfailed_blocks(struct bdev *dev, u32 *lbas, u_int max);
u_int bdirty_blocks(struct bdev *dev, u32 *lbas, u_int max);  /* 10/16/26 (B28): every dirty block of dev, the same way */
void baside(struct bdev *dev, u32 lba, bool on);
void bdiscard(struct bdev *dev, u32 lba);
int bcache_audit(struct bdev *dev);                  /* 10/16/26 (B9): dev's account against its blocks: 0, or how many counters are off */

/* 10/5/26: count blocks from lba straight from or to the device, through the cache's 64 KB
   bounce buffer, so the device never sees the caller's memory. A read returns what the cache
   holds where it holds a block; a write updates the copies the cache holds (10/16/26, B15: once
   the device has the bytes; a refused write leaves the dirty ones as they were, and B22 drops the
   clean ones: the device may have stored part of the request). 0 or BDEV_E*. */
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
