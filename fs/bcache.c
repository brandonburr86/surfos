/*
SurfOS Block Cache
--------------------
File: bcache.c  Date: 10/3/26 (roadmap F1)
--------------------
10/5/26 (USB storage): an unplugged device's blocks are never read from the
cache again, its dirty blocks are dropped instead of written, and a block
still held when it went away is dropped when released.

10/5/26 (USB storage, speed): 1024 blocks found through a hash table and
reused least recently used first. bcache_lock guards the table only and is
never held while a device works: a block being read in is marked loading and
only the tasks that want that block wait for it; a block being written out is
marked writing and stays usable meanwhile (bdirty() bumps a generation, so a
write-back only cleans what it wrote). Dirty neighbours go out in one request
(up to 64 KB), a miss can read the following blocks too (breada), and runs of
blocks can move between a device and the caller directly (bread_blocks,
bwrite_blocks), coherent with the cached copies. Those multi-block transfers
use one 64 KB bounce buffer under its own mutex, which is only ever taken
without bcache_lock held.

10/16/26 (SurfCPU, milestone 8; audit B1): a failed write no longer loses the block.
Before, a block that had to make room and could not be written was dropped ("lost,
and said so"): the next sync then found nothing wrong, so the command that wrote never
heard of it, and a dropped FAT or directory block let the file system read its old
copy back and hand out clusters a file already had. Now a block whose write failed
stays dirty while its device is there (wfail), the search for a buffer passes over it,
and the next bsync() writes it or fails; when every buffer is held or failed, one
failed run is tried again (the device may work now), and if that fails too the caller
gets no buffer (an I/O error). bsync() tries each run once per call (failed runs are
not sent again for each of their blocks) and returns -1 when any write failed. The
console hears of the first failure of a device and of the write that works again; the
failures between are quiet (their callers hear of them). A device that is gone still
has its blocks dropped.

10/16/26 (audit B3): the order of writes. Before, bsync() wrote dirty blocks in the
order of the buffer table, and a card pulled in the middle left what came of it: on
a FAT volume a new file's directory entry could be on the card while its cluster was
still free in the FAT (SurfOS, or the next computer, could give that cluster to
another file), or the entry and the FAT without the file's bytes (the file reading
whatever the cluster held before). Now a buffer carries an order (bdirty_order()), a
run never mixes two, bsync() writes a device's blocks order by order and stops at
the first order that fails, and a block of a higher order is written to make room
only after every dirty block of a lower order of its device (or not at all, until a
sync). What a file system can still leave behind is said in surfcpu/docs/CARD.md.

10/16/26 (audit B9): each device keeps the cache's account of it in its struct bdev
(bc_dirty[] by order, bc_failed, bc_said), kept by account() at every change of a block's
dirty, order or failed state. The console line about a device that refuses writes is
said once per device and the line about it working again follows it, whatever other
devices failed and went away before (the table of eight failing devices is gone).
bcache_audit() recounts a device's blocks against its account, for the tests.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <surfos/interrupt.h>
#include <sys/bdev.h>
#include <fs/bcache.h>
#include <blibc_common.h>

#define HASH 512                                /* buckets, a power of 2 */

static struct buf bufs[BCACHE_BLOCKS];
static struct buf *hash[HASH];
static struct buf *newest, *oldest;             /* every buffer, most recently used first */
static mutex_t bcache_lock = MUTEX_INIT("bcache");
static wait_queue_t bwait = WAIT_QUEUE_INIT;    /* tasks waiting for a block's I/O, or for a free buffer */
static u_int inflight;                          /* device requests under way */
static mutex_t bounce_lock = MUTEX_INIT("bounce");
static u8 bounce[BCACHE_RUN * 512];
static u_long hits, misses, writes;
static struct bcache_io io;
static u32 sync_passes;                         /* bsync() calls; 0 stands for reuse()'s writes */
static int evict_write(struct buf *b);
/* 10/16/26 (B9): a device's account (struct bdev's bc_dirty[]) has a counter for each write order */
typedef char bc_orders_fit[(BORDER_MAX + 1) * sizeof(u_int) == sizeof(((struct bdev *)0)->bc_dirty) ? 1 : -1];

/**** the table (all of this holds bcache_lock) ****/

static u_int hidx(const struct bdev *dev, u32 lba) {
    return (lba ^ (u32)((u_long)dev >> 6)) & (HASH - 1);
}

static struct buf *find(const struct bdev *dev, u32 lba) {
    struct buf *b;
    for(b = hash[hidx(dev, lba)]; b; b = b->hnext) if(b->dev == dev && b->lba == lba) return b;
    return NULL;
}

static void hash_in(struct buf *b) {
    u_int h = hidx(b->dev, b->lba);
    b->hprev = NULL;
    b->hnext = hash[h];
    if(hash[h]) hash[h]->hprev = b;
    hash[h] = b;
    b->hashed = true;
}

static void hash_out(struct buf *b) {
    if(!b->hashed) return;
    if(b->hprev) b->hprev->hnext = b->hnext;
    else hash[hidx(b->dev, b->lba)] = b->hnext;
    if(b->hnext) b->hnext->hprev = b->hprev;
    b->hnext = b->hprev = NULL;
    b->hashed = false;
}

static void lru_out(struct buf *b) {
    if(b->newer) b->newer->older = b->older;
    else newest = b->older;
    if(b->older) b->older->newer = b->newer;
    else oldest = b->newer;
    b->newer = b->older = NULL;
}

static void lru_newest(struct buf *b) {         /* just used */
    lru_out(b);
    b->older = newest;
    if(newest) newest->newer = b;
    else oldest = b;
    newest = b;
}

static void lru_oldest(struct buf *b) {         /* reused first */
    lru_out(b);
    b->newer = oldest;
    if(oldest) oldest->older = b;
    else newest = b;
    oldest = b;
}

/* 10/16/26 (B9): b's part in its device's account: sign -1 before a change of dirty, order or wfail,
   +1 after it, both with interrupts off (bdirty() runs without bcache_lock, and a task may be
   preempted between a counter's read and its write) */
static void account(struct buf *b, int sign) {
    if(!b->dirty || !b->dev) return;
    b->dev->bc_dirty[b->order] += sign;
    if(b->wfail) b->dev->bc_failed += sign;
}

/* forget a block: not found any more, its buffer the next to be reused */
static void drop(struct buf *b) {
    u_long f;
    hash_out(b);
    f = irq_save();
    account(b, -1);
    b->valid = b->dirty = b->wfail = false;
    irq_restore(f);
    b->fail_pass = 0;
    b->order = BORDER_DATA;
    lru_oldest(b);
}

/* sleep until a block's I/O ends or a buffer is released; returns holding the lock again */
static void sleep_unlocked(void) {
    u_long f;
    if(!curTask || curTask == idle_task || in_interrupt()) {    /* at boot nobody else uses the cache */
        mutex_unlock(&bcache_lock);
        mutex_lock(&bcache_lock);
        return;
    }
    f = irq_save();
    wait_prepare(&bwait);                       /* queued before the lock goes: no wake-up is missed */
    mutex_unlock(&bcache_lock);
    irq_restore(f);
    yield();
    mutex_lock(&bcache_lock);
}

void init_bcache(void) {
    int i;
    mutex_lock(&bcache_lock);
    memset(bufs, 0, sizeof(bufs));
    memset(hash, 0, sizeof(hash));
    newest = oldest = NULL;
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        bufs[i].older = newest;
        if(newest) newest->newer = &bufs[i];
        else oldest = &bufs[i];
        newest = &bufs[i];
    }
    mutex_unlock(&bcache_lock);
}

/* Write b and its dirty neighbours (up to BCACHE_RUN blocks in one request). Drops the lock
   while the device works. A gone device's blocks are dropped instead, without a word. pass:
   the bsync() call (0 from reuse()); a block that failed in it is not part of another run.
   A run is of one order (B3). */
static int write_run(struct buf *b, u32 pass) {
    struct bdev *dev = b->dev;
    struct buf *run[BCACHE_RUN];
    u32 gen[BCACHE_RUN], first = b->lba, n = 0, i;
    u_long f;
    int r;

    while(first > 0 && b->lba - first < BCACHE_RUN - 1) {
        struct buf *p = find(dev, first - 1);
        if(!p || !p->valid || !p->dirty || p->writing || p->order != b->order || (pass && p->fail_pass == pass)) break;
        first--;
    }
    for(i = first; n < BCACHE_RUN; i++) {
        struct buf *p = i == b->lba ? b : find(dev, i);
        if(!p || !p->valid || !p->dirty || p->writing || p->order != b->order || (pass && p->fail_pass == pass)) break;
        run[n++] = p;
    }
    if(dev->gone) {
        f = irq_save();
        for(i = 0; i < n; i++) {
            account(run[i], -1);
            run[i]->dirty = run[i]->wfail = false;
        }
        irq_restore(f);
        for(i = 0; i < n; i++) if(!run[i]->refs) drop(run[i]);
        dev->bc_said = false;
        return BDEV_EGONE;
    }
    for(i = 0; i < n; i++) { run[i]->writing = true; gen[i] = run[i]->gen; }
    inflight++;
    mutex_unlock(&bcache_lock);
    if(n == 1) r = bdev_write(dev, first, 1, run[0]->data);
    else {
        mutex_lock(&bounce_lock);
        for(i = 0; i < n; i++) memcpy(bounce + i * 512, run[i]->data, 512);
        r = bdev_write(dev, first, n, bounce);
        mutex_unlock(&bounce_lock);
    }
    mutex_lock(&bcache_lock);
    inflight--;
    io.writes++;
    f = irq_save();                             /* against a bdirty() between the test and the clearing */
    for(i = 0; i < n; i++) {
        account(run[i], -1);
        run[i]->writing = false;
        if((r == 0 && run[i]->gen == gen[i]) || dev->gone) run[i]->dirty = false;
        run[i]->wfail = r != 0 && !dev->gone;   /* kept dirty for the next sync (B1) */
        if(run[i]->wfail) run[i]->fail_pass = pass;
        account(run[i], +1);
    }
    irq_restore(f);
    for(i = 0; i < n; i++) if(dev->gone && !run[i]->refs) drop(run[i]);
    if(r == 0) { writes += n; io.written_blocks += n; }
    if(dev->gone) dev->bc_said = false;         /* 10/16/26 (B9): said once per device, in its own account */
    else if(r == 0 && dev->bc_said) {
        dev->bc_said = false;
        kprintf("bcache: %s takes writes again\n", dev->name);
    } else if(r != 0 && !dev->bc_said) {
        dev->bc_said = true;
        kprintf("bcache: write of %s blocks %lu-%lu failed (%i); kept for the next sync (more failures are not shown)\n",
                dev->name, (u_long)first, (u_long)(first + n - 1), r);
    }
    wake_up_all(&bwait);
    return r;
}

/* a buffer nobody holds or moves, clean and forgotten; NULL when every one is held or waits
   for a sync after a failed write. May drop the lock (to write a dirty block out, or to wait
   for a request to finish). */
static struct buf *reuse(void) {
    bool retried = false;
    for(;;) {
        struct buf *b;
        for(b = oldest; b; b = b->newer) if(!b->refs && !b->loading && !b->writing && !(b->dirty && b->wfail)) break;
        if(!b && !inflight && !retried) {           /* all held or failed: one failed run again (B1) */
            for(b = oldest; b; b = b->newer) if(!b->refs && !b->loading && !b->writing && b->valid && b->dirty) break;
            retried = true;
            if(b) {
                evict_write(b);
                continue;
            }
        }
        if(!b) {
            if(!inflight) return NULL;
            sleep_unlocked();
            continue;
        }
        if(b->valid && b->dirty) {
            evict_write(b);                     /* written, or failed: kept dirty and passed over (B1) */
            continue;
        }
        drop(b);
        return b;
    }
}

/* the held buffer of a block: cached, or read (with up to ahead - 1 following blocks when it
   was not), or (fill false) zeroed for the caller to overwrite */
static struct buf *getblk(struct bdev *dev, u32 lba, bool fill, u32 ahead) {
    struct buf *b, *ra[BCACHE_RUN];
    u32 n = 0, i;
    int r;

    if(!dev || dev->gone) return NULL;
    mutex_lock(&bcache_lock);
    for(;;) {
        b = find(dev, lba);
        if(b && b->loading) { sleep_unlocked(); continue; }     /* being read: wait for this block only */
        if(b) {
            hits++;
            b->refs++;
            lru_newest(b);
            mutex_unlock(&bcache_lock);
            return b;
        }
        b = reuse();
        if(!b) { mutex_unlock(&bcache_lock); kprintf("bcache: every buffer is busy\n"); return NULL; }
        if(!find(dev, lba)) break;              /* else another task read it in meanwhile: take that */
    }
    misses++;
    b->dev = dev;
    b->lba = lba;
    b->refs = 1;
    hash_in(b);
    lru_newest(b);
    if(!fill) {
        memset(b->data, 0, sizeof(b->data));
        b->valid = true;
        mutex_unlock(&bcache_lock);
        return b;
    }
    b->loading = true;
    ra[n++] = b;
    while(n < ahead && n < BCACHE_RUN && lba + n < dev->nblocks && !find(dev, lba + n)) {   /* read-ahead */
        struct buf *v;
        for(v = oldest; v; v = v->newer) if(!v->refs && !v->loading && !v->writing) break;
        if(!v || (v->valid && v->dirty)) break; /* only buffers that are free without a write */
        drop(v);
        v->dev = dev;
        v->lba = lba + n;
        v->loading = true;
        hash_in(v);
        lru_newest(v);
        ra[n++] = v;
    }
    inflight++;
    mutex_unlock(&bcache_lock);
    if(n == 1) r = bdev_read(dev, lba, 1, b->data);
    else {
        mutex_lock(&bounce_lock);
        r = bdev_read(dev, lba, n, bounce);
        if(r == 0) for(i = 0; i < n; i++) memcpy(ra[i]->data, bounce + i * 512, 512);
        mutex_unlock(&bounce_lock);
    }
    mutex_lock(&bcache_lock);
    io.reads++;
    if(r == 0) io.read_blocks += n;
    if(r != 0 && n > 1 && !dev->gone) {         /* the read-ahead may be what failed: just the block */
        for(i = 1; i < n; i++) { ra[i]->loading = false; drop(ra[i]); }
        n = 1;
        mutex_unlock(&bcache_lock);
        r = bdev_read(dev, lba, 1, b->data);
        mutex_lock(&bcache_lock);
        io.reads++;
        if(r == 0) io.read_blocks++;
    }
    inflight--;
    for(i = 0; i < n; i++) {
        ra[i]->loading = false;
        if(r == 0 && !dev->gone) ra[i]->valid = true;
        else if(ra[i] != b) drop(ra[i]);
    }
    if(r != 0 || dev->gone) {
        b->refs = 0;
        drop(b);
        b = NULL;
    }
    wake_up_all(&bwait);
    mutex_unlock(&bcache_lock);
    return b;
}

struct buf *bread(struct bdev *dev, u32 lba) {
    return getblk(dev, lba, true, 1);
}

struct buf *breada(struct bdev *dev, u32 lba, u32 n) {
    return getblk(dev, lba, true, n);
}

struct buf *bget(struct bdev *dev, u32 lba) {
    return getblk(dev, lba, false, 0);
}

struct buf *bpeek(struct bdev *dev, u32 lba) {
    struct buf *b;
    if(!dev || dev->gone) return NULL;
    mutex_lock(&bcache_lock);
    b = find(dev, lba);
    if(b && b->valid) {
        hits++;
        b->refs++;
        lru_newest(b);
    } else b = NULL;
    mutex_unlock(&bcache_lock);
    return b;
}

void bdirty(struct buf *b) {
    bdirty_order(b, BORDER_DATA);
}

void bdirty_order(struct buf *b, u8 order) {
    u_long f = irq_save();
    account(b, -1);
    if(!b->dirty || order < b->order) b->order = order;
    b->gen++;
    b->dirty = true;
    account(b, +1);
    irq_restore(f);
}

/* (bcache_lock held) every dirty block of dev below order written: 0, or -1 when one is still
   dirty (it failed, now or before) */
static int write_below(struct bdev *dev, u8 order) {
    int i, r = 0;
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *p = &bufs[i];
        if(p->dev != dev || !p->valid || !p->dirty || p->order >= order) continue;
        while(p->valid && p->dirty && p->writing && p->dev == dev) sleep_unlocked();
        if(p->dev != dev || !p->valid || !p->dirty || p->order >= order) continue;
        if(p->wfail || write_run(p, 0) != 0) r = -1;
    }
    return r;
}

/* reuse()'s write of a dirty block: what it may point at first (B3); when that cannot be
   written, neither is b: it waits for a sync like a failed block */
static int evict_write(struct buf *b) {
    if(b->order > BORDER_DATA && write_below(b->dev, b->order) != 0) {
        if(b->valid && b->dirty && !b->writing) {
            u_long f = irq_save();
            account(b, -1);
            b->wfail = true;
            account(b, +1);
            irq_restore(f);
        }
        return -1;
    }
    return write_run(b, 0);
}

void brelse(struct buf *b) {
    mutex_lock(&bcache_lock);
    if(b->refs) b->refs--;
    if(!b->refs) {
        if(b->dev && b->dev->gone && !b->loading && !b->writing) drop(b);
        wake_up_all(&bwait);                    /* someone may wait for a free buffer */
    }
    mutex_unlock(&bcache_lock);
}

/* order by order (B3); a device that failed in one order has nothing of a higher order
   written in this call */
int bsync(struct bdev *dev) {
    int i, r = 0;
    u32 pass;
    u8 order;
    mutex_lock(&bcache_lock);
    if(!++sync_passes) sync_passes = 1;
    pass = sync_passes;
    for(order = 0; order <= BORDER_MAX; order++) {
        for(i = 0; i < BCACHE_BLOCKS; i++) {
            struct buf *b = &bufs[i];
            if(dev && b->dev != dev) continue;
            while(b->valid && b->dirty && b->writing && (!dev || b->dev == dev)) sleep_unlocked();   /* another task writes it */
            if(!b->valid || !b->dirty || b->order != order || (dev && b->dev != dev) || b->fail_pass == pass) continue;
            if(b->dev->bc_bad_pass == pass) continue;           /* its device failed in this call (B9: its own mark) */
            if(write_run(b, pass) != 0) {
                r = -1;
                b->dev->bc_bad_pass = pass;
            }
        }
    }
    mutex_unlock(&bcache_lock);
    return r;
}

void binvalidate(struct bdev *dev) {
    int i;
    mutex_lock(&bcache_lock);
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *b = &bufs[i];
        if(b->dev == dev && (b->valid || b->hashed) && !b->refs && !b->loading && !b->writing) drop(b);
    }
    if(dev && dev->gone) dev->bc_said = false;
    mutex_unlock(&bcache_lock);
}

/* 10/16/26 (B9): how many of dev's counters differ from a count of its blocks (0: none) */
int bcache_audit(struct bdev *dev) {
    u_int dirty[BORDER_MAX + 1], failed = 0;
    int i, off = 0;
    memset(dirty, 0, sizeof(dirty));
    mutex_lock(&bcache_lock);
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *b = &bufs[i];
        if(b->dev != dev || !b->dirty) continue;
        dirty[b->order]++;
        if(b->wfail) failed++;
    }
    for(i = 0; i <= BORDER_MAX; i++) if(dirty[i] != dev->bc_dirty[i]) off++;
    if(failed != dev->bc_failed) off++;
    mutex_unlock(&bcache_lock);
    return off;
}

void bcache_stats(u_long *h, u_long *m, u_long *w) {
    *h = hits; *m = misses; *w = writes;
}

void bcache_io_stats(struct bcache_io *out) {
    mutex_lock(&bcache_lock);
    *out = io;
    mutex_unlock(&bcache_lock);
}

/**** straight to and from the device ****/

int bread_blocks(struct bdev *dev, u32 lba, u32 count, void *dst) {
    u8 *d = (u8 *)dst;
    u32 have[BCACHE_RUN / 32];
    while(count) {
        u32 n = count < BCACHE_RUN ? count : BCACHE_RUN, i, cached = 0;
        int r;
        if(dev->gone) return BDEV_EGONE;
        memset(have, 0, sizeof(have));
        mutex_lock(&bcache_lock);               /* the cached copies first: they are never older than the disk */
        for(i = 0; i < n; i++) {
            struct buf *p = find(dev, lba + i);
            if(p && p->valid) {
                memcpy(d + i * 512, p->data, 512);
                have[i / 32] |= 1UL << (i % 32);
                cached++;
            }
        }
        hits += cached;
        mutex_unlock(&bcache_lock);
        if(cached < n) {
            mutex_lock(&bounce_lock);
            r = bdev_read(dev, lba, n, bounce);
            if(r == 0) {
                for(i = 0; i < n; i++) if(!(have[i / 32] & (1UL << (i % 32)))) memcpy(d + i * 512, bounce + i * 512, 512);
            }
            mutex_unlock(&bounce_lock);
            mutex_lock(&bcache_lock);
            io.reads++;
            if(r == 0) io.read_blocks += n;
            mutex_unlock(&bcache_lock);
            if(r) return r;
        }
        lba += n;
        d += n * 512;
        count -= n;
    }
    return 0;
}

/* src NULL: zeros. Cached copies take the new contents first (dirty), so whatever writes them
   meanwhile writes the new data; once the device has it they are clean again. */
static int write_blocks(struct bdev *dev, u32 lba, u32 count, const u8 *src) {
    bool upd[BCACHE_RUN];
    u32 gen[BCACHE_RUN];
    while(count) {
        u32 n = count < BCACHE_RUN ? count : BCACHE_RUN, i;
        u_long f;
        int r;
        if(dev->gone) return BDEV_EGONE;
        mutex_lock(&bcache_lock);
        for(i = 0; i < n; i++) {
            struct buf *p;
            while((p = find(dev, lba + i)) != NULL && p->loading) sleep_unlocked();
            upd[i] = p && p->valid;
            if(!upd[i]) continue;
            if(src) memcpy(p->data, src + i * 512, 512);
            else memset(p->data, 0, 512);
            bdirty(p);
            gen[i] = p->gen;
        }
        mutex_unlock(&bcache_lock);
        mutex_lock(&bounce_lock);
        if(src) memcpy(bounce, src, n * 512);
        else memset(bounce, 0, n * 512);
        r = bdev_write(dev, lba, n, bounce);
        mutex_unlock(&bounce_lock);
        mutex_lock(&bcache_lock);
        io.writes++;
        if(r == 0) io.written_blocks += n;
        f = irq_save();
        for(i = 0; r == 0 && i < n; i++) {
            struct buf *p = upd[i] ? find(dev, lba + i) : NULL;
            if(p && p->valid && !p->writing && p->gen == gen[i]) {
                account(p, -1);
                p->dirty = p->wfail = false;
            }
        }
        irq_restore(f);
        mutex_unlock(&bcache_lock);
        if(r) return r;
        lba += n;
        if(src) src += n * 512;
        count -= n;
    }
    return 0;
}

int bwrite_blocks(struct bdev *dev, u32 lba, u32 count, const void *src) {
    return write_blocks(dev, lba, count, (const u8 *)src);
}

int bzero_blocks(struct bdev *dev, u32 lba, u32 count) {
    return write_blocks(dev, lba, count, NULL);
}
