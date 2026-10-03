/*
SurfOS Block Cache
--------------------
File: bcache.c  Date: 10/3/26 (roadmap F1)
--------------------
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <sys/bdev.h>
#include <fs/bcache.h>
#include <blibc_common.h>

static struct buf bufs[BCACHE_BLOCKS];
static mutex_t bcache_lock = MUTEX_INIT("bcache");
static u_long clock_;
static u_long hits, misses, writes;

void init_bcache(void) {
    memset(bufs, 0, sizeof(bufs));
}

/* must hold bcache_lock */
static int writeback(struct buf *b) {
    int r = 0;
    if(b->valid && b->dirty) {
        r = bdev_write(b->dev, b->lba, 1, b->data);
        if(r == 0) { b->dirty = false; writes++; }
        else kprintf("bcache: write of %s block %lu failed (%i)\n", b->dev->name, (u_long)b->lba, r);
    }
    return r;
}

/* find the block, or the least recently used free buffer for it; holds bcache_lock */
static struct buf *lookup(struct bdev *dev, u32 lba, bool *found) {
    struct buf *victim = NULL;
    int i;
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *b = &bufs[i];
        if(b->valid && b->dev == dev && b->lba == lba) { *found = true; return b; }
        if(b->refs == 0 && (!victim || !b->valid || (victim->valid && b->last_used < victim->last_used))) {
            if(!victim || !b->valid || victim->valid) victim = b;
        }
    }
    *found = false;
    return victim;
}

static struct buf *getblk(struct bdev *dev, u32 lba, bool fill) {
    struct buf *b;
    bool found;

    mutex_lock(&bcache_lock);
    b = lookup(dev, lba, &found);
    if(!b) { mutex_unlock(&bcache_lock); kprintf("bcache: every buffer is busy\n"); return NULL; }
    if(found) hits++;
    else {
        misses++;
        writeback(b);                     /* evict */
        b->dev = dev;
        b->lba = lba;
        b->valid = true;
        b->dirty = false;
        if(fill && bdev_read(dev, lba, 1, b->data) != 0) {
            b->valid = false;
            mutex_unlock(&bcache_lock);
            return NULL;
        }
        if(!fill) memset(b->data, 0, sizeof(b->data));
    }
    b->refs++;
    b->last_used = ++clock_;
    mutex_unlock(&bcache_lock);
    return b;
}

struct buf *bread(struct bdev *dev, u32 lba) {
    return getblk(dev, lba, true);
}

struct buf *bget(struct bdev *dev, u32 lba) {
    return getblk(dev, lba, false);
}

void bdirty(struct buf *b) {
    b->dirty = true;
}

void brelse(struct buf *b) {
    mutex_lock(&bcache_lock);
    if(b->refs) b->refs--;
    mutex_unlock(&bcache_lock);
}

int bsync(struct bdev *dev) {
    int i, r = 0;
    mutex_lock(&bcache_lock);
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        if(bufs[i].valid && bufs[i].dirty && (!dev || bufs[i].dev == dev)) {
            if(writeback(&bufs[i]) != 0) r = -1;
        }
    }
    mutex_unlock(&bcache_lock);
    return r;
}

void binvalidate(struct bdev *dev) {
    int i;
    mutex_lock(&bcache_lock);
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        if(bufs[i].valid && bufs[i].dev == dev && bufs[i].refs == 0) bufs[i].valid = false;
    }
    mutex_unlock(&bcache_lock);
}

void bcache_stats(u_long *h, u_long *m, u_long *w) {
    *h = hits; *m = misses; *w = writes;
}
