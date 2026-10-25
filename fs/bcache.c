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

10/24/26 (storage, milestone 1: interrupted saves, failed writes and concurrent
unmounts preserve existing files). The SurfCPU branch's fixes of this file (its
audit B1-B41), brought back where x86 needs them, with one rule of its own in
place of SurfCPU's copies: a file system that must know what reached its device
syncs holding its volume's lock (fs/fat.c), so nothing makes that device's blocks
dirty while it syncs. What it keeps of SurfCPU's, by audit item:
 - B1: a failed write no longer loses its block. A block that had to make room and
   could not be written was dropped ("lost, and said so"), and the next sync found
   nothing wrong: the command that wrote never heard of it, and a dropped FAT or
   directory block let FAT read its old copy back and hand out clusters a file had.
   Now the block stays dirty while its device is there (wfail), making room passes
   over it, and every sync tries it again. The console hears of a device's first
   failure and of the first write that works after it (B9: per device).
 - B3, B7: the order of writes. bsync() wrote dirty blocks in the order of the
   buffer table, so a stick pulled in the middle could keep a new file's entry with
   its cluster still free in the FAT, or a renamed file's old chain freed with its
   name still on it. A block now carries an order (bdirty_order()); one of order k
   is written only when its device has no dirty block of a lower order (set-aside
   ones apart), checked under the lock right before every request, for syncs and
   for making room alike; a run never mixes two orders. A block someone holds is
   not written (it may be in the middle of a change), and getblk() hands out no
   block being written, so what reaches the device is what was checked.
 - B6, B14: a device that refuses writes no longer fills the cache every device
   shares. Making room tries none of its blocks; it keeps at most BCACHE_HOLD dirty
   blocks, and past that bcache_room() and bget() say no (its file system's write
   fails with EIO) after one more try of its oldest block (it may work again). A
   device that works and holds that many writes some out first. Read-ahead looks
   past dirty buffers.
 - B8: making room writes what a block needs first, which lets the lock go: it
   looks at the cache again afterwards instead of writing a block that may have
   changed hands.
 - B9: each device keeps the cache's account of itself (struct bdev's bc_ fields),
   kept at every change of a block's state; bcache_audit() checks it, for the tests.
 - B12 (its debt): bsync() returns 0 only when every block that was dirty when it
   began reached the device (since: the sync call under way when a block became
   dirty); blocks others make dirty meanwhile are written when they may be, never
   waited for. Under other tasks' writes a sync that cannot finish says -1 (after
   SYNC_ROUNDS rounds), never 0: FAT's own syncs hold its volume's lock, where this
   cannot happen.
 - B13: a failed run of several blocks is tried again block by block in the same
   sync, every block once a sync; a device that refused DEAD_TRIES requests in a
   sync and took none is asked nothing more in it.
 - B15, B22: the straight path changes no cached copy before the device has the
   bytes: the copies are kept out of every other write meanwhile and take the bytes
   once the device took them; when it refuses, the clean ones are dropped (it may
   have stored part of the request) and the dirty ones keep their bytes, which the
   next sync writes over the device's.
 - B16, B28: blocks set aside. A file system freeing clusters behind a sync sets
   their dirty blocks aside first (baside()): not written, no block waits for them,
   and the setting task's own syncs do not owe them, so a block that refuses cannot
   keep its own file from being removed; other tasks' syncs wait for them (B26).
   Put back when that sync fails, dropped once the clusters are free (bdiscard()).
 - B18: a sync waits HOLD_WAIT_MS for a block it owes that someone holds, then
   gives up on it (-1, said on the console): a task killed at once while it held one
   (kernel/task.c's kill_escalate()) no longer leaves every later sync hanging. A sync
   of a task that is being killed gives up at once: the wait would outlast the kill's
   2 s, and the kill would come inside it, with the volume's lock held.
 - B35, B40: bdiscard() and binvalidate() drop a block that is held or under I/O
   when it is let go or when its I/O ends; until then it is out of the table, so
   nobody else gets it (binvalidate() kept it, and an umount left blocks of its
   volume cached).
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <surfos/interrupt.h>
#include <surfos/ktimer.h>
#include <sys/bdev.h>
#include <fs/bcache.h>
#include <blibc_common.h>

#define HASH 512                                /* buckets, a power of 2 */
#define SYNC_ROUNDS 16                          /* 10/24/26: rounds of a sync, after which what others keep dirty is -1 */
#define DEAD_TRIES 4                            /* B13: requests a device refuses in a sync, taking none, before it is let be */
#define HOLD_WAIT_MS 10000UL                    /* B18: how long a sync waits for a holder of a block it owes */
#define TALLY_DEVS 8                            /* B13: the devices a sync call counts the requests of */
#define RUN_BLOCKED 1                           /* write_run(): nothing tried (the order or a holder forbids it now) */

static struct buf bufs[BCACHE_BLOCKS];
static struct buf *hash[HASH];
static struct buf *newest, *oldest;             /* every buffer, most recently used first */
static mutex_t bcache_lock = MUTEX_INIT("bcache");
static wait_queue_t bwait = WAIT_QUEUE_INIT;    /* tasks waiting for a block's I/O, a holder, or a free buffer */
static u_int inflight;                          /* device requests under way */
static mutex_t bounce_lock = MUTEX_INIT("bounce");
static u8 bounce[BCACHE_RUN * 512];
static u_long hits, misses, writes;
static struct bcache_io io;
static u32 sync_calls;                          /* 10/24/26: bsync() calls begun; a block's since and pass */

struct tally {                                  /* B13: a sync call's requests of each device */
    struct bdev *dev[TALLY_DEVS];
    u16 refused[TALLY_DEVS], taken[TALLY_DEVS];
};

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

/**** 10/24/26 (B9): the account of a device's blocks, kept at every change of a block's state ****/

/* a block's dirt is the device's no more (written, dropped, given up); interrupts off around it (bdirty_order()
   changes the same counts without the lock) */
static void mark_clean(struct buf *b) {
    struct bdev *d = b->dev;
    u_long f = irq_save();
    if(b->dirty && d) {
        if(b->aside) { if(d->bc_aside) d->bc_aside--; }
        else if(d->bc_dirty[b->order]) d->bc_dirty[b->order]--;
        if(b->wfail && d->bc_failed) d->bc_failed--;
        if(!d->bc_aside) d->bc_aside_by = NULL;
    }
    b->dirty = b->wfail = b->aside = false;
    irq_restore(f);
}

static void mark_failed(struct buf *b) {
    u_long f = irq_save();
    if(b->dirty && !b->wfail) {
        b->wfail = true;
        b->dev->bc_failed++;
    }
    irq_restore(f);
}

static u_int dirty_total(const struct bdev *d) {
    return d->bc_dirty[0] + d->bc_dirty[1] + d->bc_dirty[2] + d->bc_dirty[3] + d->bc_aside;
}

/* may a block of this order of d go now: no dirty block of a lower order (set-aside ones apart) */
static bool deps_out(const struct bdev *d, u8 order) {
    u8 k;
    for(k = 0; k < order; k++) if(d->bc_dirty[k]) return false;
    return true;
}

static int lowest_order(const struct bdev *d) {
    int k;
    for(k = 0; k <= BORDER_MAX; k++) if(d->bc_dirty[k]) return k;
    return -1;
}

/* forget a block: not found any more, its buffer the next to be reused (10/24/26: tried by no sync call: a buffer
   reused for another block kept the call that wrote the old one, and that call then passed the new block over) */
static void drop(struct buf *b) {
    hash_out(b);
    mark_clean(b);
    b->valid = false;
    b->discard = false;
    b->pass = 0;
    lru_oldest(b);
}

/* 10/24/26 (B35, B40): drop now, or (held, under I/O) out of the table at once and dropped when let go or when the
   I/O ends; its dirt is nobody's from now on */
static void discard_buf(struct buf *b) {
    mark_clean(b);
    if(!b->refs && !b->loading && !b->writing) { drop(b); return; }
    hash_out(b);
    b->discard = true;
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

/* 10/24/26 (B18): the same for at most ms, for a wait for a holder (one that never lets go wakes nobody). A wake-up
   between the lock and the wait only makes it last ms; a kill or Ctrl+C pending cuts no wait here short (the sync
   goes on), so the task just lets the others run */
static void sleep_unlocked_ms(u_long ms) {
    if(!curTask || curTask == idle_task || in_interrupt()) {
        mutex_unlock(&bcache_lock);
        mutex_lock(&bcache_lock);
        return;
    }
    mutex_unlock(&bcache_lock);
    if(wait_on_timeout(&bwait, ms) == -EINTR) yield();
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

/**** writing ****/

/* may b be written now, as it is: dirty, nobody's to change or to read in, not set aside, and what it may point at
   on its device written (B3, B7) */
static bool writable(const struct buf *b) {
    return b->valid && b->dirty && !b->aside && !b->discard && !b->refs && !b->loading && !b->writing &&
           deps_out(b->dev, b->order);
}

/* may p go in the same request as a block of order `order` (a sync's call: one it tried already does not) */
static bool run_mate(const struct buf *p, u8 order, u32 call) {
    return p && writable(p) && p->order == order && (!call || p->pass != call);
}

/* Write b and its dirty neighbours of its order (up to BCACHE_RUN blocks in one request; single: b alone). Drops the
   lock while the device works. 0, BDEV_E*, or RUN_BLOCKED when b may not be written now. call is the sync call
   (0: making room), which marks the blocks tried; *first and *n say what was sent. A gone device's blocks are
   dropped instead, without a word. */
static int write_run(struct buf *b, u32 call, bool single, u32 *first_out, u32 *n_out) {
    struct bdev *dev = b->dev;
    struct buf *run[BCACHE_RUN];
    u32 gen[BCACHE_RUN], first = b->lba, n = 0, i;
    u8 order = b->order;
    int r;

    if(n_out) *n_out = 0;
    if(dev->gone) {
        for(i = 0; i < BCACHE_BLOCKS; i++) {    /* all of its dirty blocks go, whoever holds them */
            struct buf *p = &bufs[i];
            if(p->dev == dev && p->dirty) {
                mark_clean(p);
                if(!p->refs && !p->loading && !p->writing) drop(p);
            }
        }
        return BDEV_EGONE;
    }
    if(!writable(b)) return RUN_BLOCKED;
    while(!single && first > 0 && b->lba - first < BCACHE_RUN - 1 && run_mate(find(dev, first - 1), order, call)) first--;
    for(i = first; n < BCACHE_RUN && (i == b->lba || (!single && run_mate(find(dev, i), order, call))); i++) {
        run[n++] = i == b->lba ? b : find(dev, i);
        if(single) break;
    }
    for(i = 0; i < n; i++) {
        run[i]->writing = true;
        gen[i] = run[i]->gen;
        if(call) run[i]->pass = call;
    }
    if(first_out) *first_out = first;
    if(n_out) *n_out = n;
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
    for(i = 0; i < n; i++) {
        struct buf *p = run[i];
        p->writing = false;
        if(p->discard || dev->gone) {           /* given up meanwhile (B40), or unplugged */
            mark_clean(p);
            if(!p->refs) drop(p);
        } else if(r == 0 && p->gen == gen[i]) mark_clean(p);
        else if(r != 0) mark_failed(p);         /* B1: dirty still, for the next sync */
    }
    if(r == 0) {
        writes += n;
        io.written_blocks += n;
        if(dev->bc_said && !dev->bc_failed) {
            kprintf("bcache: %s takes writes again\n", dev->name);
            dev->bc_said = false;
        }
    } else if(!dev->gone && !dev->bc_said) {
        kprintf("bcache: write of %s blocks %lu-%lu failed (%i): kept for the next sync\n", dev->name, (u_long)first,
                (u_long)(first + n - 1), r);
        dev->bc_said = true;
    }
    wake_up_all(&bwait);
    return r;
}

/* the oldest idle dirty block of d at its lowest dirty order (it may go now), or NULL */
static struct buf *oldest_writable(struct bdev *d) {
    int k = lowest_order(d);
    struct buf *b;
    if(k < 0) return NULL;
    for(b = oldest; b; b = b->newer) if(b->dev == d && b->order == k && writable(b)) return b;
    return NULL;
}

/* B8: the write that takes the dirty block b closer to clean: b itself when it may go, else the oldest idle block
   of its device's lowest dirty order; NULL when that cannot be written now */
static struct buf *needed_write(struct buf *b) {
    if(b->aside || b->discard) return NULL;
    if(deps_out(b->dev, b->order)) return b;
    return oldest_writable(b->dev);
}

/* a buffer nobody holds or moves, clean and forgotten; NULL when every one is held, or dirty with nothing that may
   be written. May drop the lock (to write a dirty block out, or to wait for a request to finish). The least recently
   used comes first: a dirty one is written (B8: what it needs first, then the cache is looked at again), but for the
   dirty blocks of a device that refuses writes, which are passed over (B6: a sync tries them) */
static struct buf *reuse(void) {
    for(;;) {
        struct buf *b, *w = NULL;
        for(b = oldest; b && !w; b = b->newer) {
            if(b->refs || b->loading || b->writing) continue;
            if(!b->valid || !b->dirty || b->dev->gone) { drop(b); return b; }
            if(!b->dev->bc_failed) w = needed_write(b);
        }
        if(w) {
            write_run(w, 0, false, NULL, NULL);
            continue;
        }
        if(!inflight) return NULL;
        sleep_unlocked();
    }
}

/* the least recently used buffer that is free without a write, from *from on, or NULL (read-ahead) */
static struct buf *free_clean(struct buf **from) {
    struct buf *v;
    for(v = *from; v; v = v->newer) {
        if(v->refs || v->loading || v->writing || (v->valid && v->dirty)) continue;
        *from = v->newer;
        return v;
    }
    *from = NULL;
    return NULL;
}

/* the held buffer of a block: cached, or read (with up to ahead - 1 following blocks when it
   was not), or (fill false) zeroed for the caller to overwrite */
static struct buf *getblk(struct bdev *dev, u32 lba, bool fill, u32 ahead) {
    struct buf *b, *ra[BCACHE_RUN], *from;
    u32 n = 0, i;
    int r;

    if(!dev || dev->gone) return NULL;
    mutex_lock(&bcache_lock);
    for(;;) {
        b = find(dev, lba);
        if(b && (b->loading || b->writing)) { sleep_unlocked(); continue; }    /* read in or written out: wait for it (B7) */
        if(!fill && dev->bc_failed && dirty_total(dev) >= BCACHE_HOLD && !(b && b->dirty)) {
            mutex_unlock(&bcache_lock);     /* B6: a device that refuses gets no more dirty blocks past its share */
            return NULL;
        }
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
    b->dirty = b->wfail = b->aside = b->discard = false;
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
    from = oldest;
    while(n < ahead && n < BCACHE_RUN && lba + n < dev->nblocks && !find(dev, lba + n)) {   /* read-ahead */
        struct buf *v = free_clean(&from);      /* B6: only buffers that are free without a write, past the dirty ones */
        if(!v) break;
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
        if(ra[i] != b && ra[i]->discard) drop(ra[i]);   /* given up while it was read in (B40) */
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

/* 10/24/26 (B3): the holder changed b. Its order is the lowest it was dirtied with until it is written (a block's
   earlier content must not reach the disk after what points at it); since says which sync call was under way when
   it became dirty, so that call does not owe it. A change makes a block set aside an ordinary dirty one again */
void bdirty_order(struct buf *b, u8 order) {
    struct bdev *d = b->dev;
    u_long f = irq_save();
    if(order > BORDER_MAX) order = BORDER_MAX;
    b->gen++;
    if(!b->dirty) {
        b->dirty = true;
        b->order = order;
        b->since = sync_calls;
        d->bc_dirty[order]++;
    } else if(b->aside) {
        b->aside = false;
        if(d->bc_aside) d->bc_aside--;
        if(!d->bc_aside) d->bc_aside_by = NULL;
        if(order < b->order) b->order = order;
        d->bc_dirty[b->order]++;
    } else if(order < b->order) {
        d->bc_dirty[b->order]--;
        b->order = order;
        d->bc_dirty[order]++;
    }
    irq_restore(f);
}

void bdirty(struct buf *b) {
    bdirty_order(b, BORDER_DATA);
}

void brelse(struct buf *b) {
    mutex_lock(&bcache_lock);
    if(b->refs) b->refs--;
    if(!b->refs) {
        if((b->discard || (b->dev && b->dev->gone)) && !b->loading && !b->writing) drop(b);
        wake_up_all(&bwait);                    /* someone may wait for a free buffer, or for this block */
    }
    mutex_unlock(&bcache_lock);
}

/* B13: the request counts of a sync call for d; NULL past TALLY_DEVS devices (asked with no early stop) */
static int tally_slot(struct tally *t, struct bdev *d) {
    int i;
    for(i = 0; i < TALLY_DEVS; i++) {
        if(t->dev[i] == d) return i;
        if(!t->dev[i]) { t->dev[i] = d; return i; }
    }
    return -1;
}

static bool tally_dead(struct tally *t, struct bdev *d) {
    int i = tally_slot(t, d);
    return i >= 0 && t->refused[i] >= DEAD_TRIES && !t->taken[i];
}

static void tally_add(struct tally *t, struct bdev *d, int r) {
    int i = tally_slot(t, d);
    if(i < 0) return;
    if(r == 0) { if(t->taken[i] < 0xFFFF) t->taken[i]++; }
    else if(t->refused[i] < 0xFFFF) t->refused[i]++;
}

/* does the sync call `call` of dev (NULL: every device) up to order top owe b: dirty when it began, and not set
   aside by this task's own operation (B16, B26) */
static bool owed(const struct buf *b, const struct bdev *dev, u8 top, u32 call) {
    if(!b->valid || !b->dirty || b->discard || b->order > top || (dev && b->dev != dev)) return false;
    if((i32)(b->since - call) >= 0) return false;  /* dirty since the call began */
    return !(b->aside && b->dev->bc_aside_by == (const void *)curTask);
}

/* 10/24/26: in the sync call `call` of dev (NULL: every device) up to order top: is a block that may matter to what
   it owes busy with somebody else (held, under I/O, set aside by another task's operation), and is one held or set
   aside (no wake-up may come: the wait is timed); may another round write something (dirty, untried, writable now) */
static void sync_look(const struct bdev *dev, u8 top, u32 call, bool *busy, bool *held, bool *more) {
    int i;
    *busy = *held = *more = false;
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        const struct buf *b = &bufs[i];
        if(!b->valid || !b->dirty || b->discard || b->order > top || (dev && b->dev != dev)) continue;
        if(b->aside && b->dev->bc_aside_by == (const void *)curTask) continue;
        if(b->refs || b->aside) *busy = *held = true;
        else if(b->loading || b->writing) *busy = true;
        else if(b->pass != call && deps_out(b->dev, b->order)) *more = true;
    }
}

/* 10/24/26 (B3, B7, B12, B13, B18): the dirty blocks of dev (NULL: of every device) up to order top, lowest order
   first, each as soon as what it may point at is written: runs, a failed run again block by block, each block once a
   call, and no more of a device that refused DEAD_TRIES requests and took none. Blocks others make dirty meanwhile are
   written when they may be and never owed. It waits while a block that matters is under I/O, and HOLD_WAIT_MS at
   most for one someone holds or another task's operation set aside (B18). 0 when every block dirty at the call
   reached the device, else -1 */
int bsync_order(struct bdev *dev, u8 top) {
    struct tally t;
    u32 call, rounds;
    u_long held_from = 0;
    int i, r = 0;
    bool gave_up = false;

    if(top > BORDER_MAX) top = BORDER_MAX;
    memset(&t, 0, sizeof(t));
    mutex_lock(&bcache_lock);
    call = ++sync_calls;
    for(rounds = 0; ; rounds++) {
        bool wrote = false, busy, held, more, left = false;
        u8 k;
        for(k = 0; k <= top; k++) {
            for(i = 0; i < BCACHE_BLOCKS; i++) {
                struct buf *b = &bufs[i];
                u32 first, n, j;
                int w;
                if(!b->valid || !b->dirty || b->order != k || (dev && b->dev != dev) || b->discard) continue;
                if(b->pass == call || b->aside || b->refs || b->loading || b->writing) continue;
                if(b->dev->gone) { write_run(b, call, false, NULL, NULL); continue; }
                if(!deps_out(b->dev, k)) continue;              /* what it may point at is not written yet */
                if(tally_dead(&t, b->dev)) { b->pass = call; continue; }   /* it takes nothing now (B13) */
                w = write_run(b, call, false, &first, &n);
                if(w == RUN_BLOCKED) continue;
                tally_add(&t, b->dev, w);
                if(w == 0) { wrote = true; continue; }
                for(j = 0; n > 1 && j < n && !b->dev->gone && !tally_dead(&t, b->dev); j++) {   /* B13: one by one */
                    struct buf *p = find(b->dev, first + j);
                    if(!p || !p->valid || !p->dirty || p->aside || p->refs || p->loading || p->writing ||
                       !deps_out(p->dev, p->order))
                        continue;
                    w = write_run(p, call, true, NULL, NULL);
                    if(w != RUN_BLOCKED) tally_add(&t, b->dev, w);
                    if(w == 0) wrote = true;
                }
            }
        }
        for(i = 0; i < BCACHE_BLOCKS && !left; i++) left = owed(&bufs[i], dev, top, call);
        if(!left || rounds + 1 >= SYNC_ROUNDS) break;   /* all of it written; or others keep it dirty: -1 */
        if(wrote) { held_from = 0; continue; }
        sync_look(dev, top, call, &busy, &held, &more);
        if(more) continue;
        if(!busy) break;                                /* what is left failed in this call, or waits for what did */
        if(held) {                                      /* B18: a holder that does not let go is given up on, */
            if(!held_from) held_from = uptime_ms() | 1; /* at once by a task being killed: it would be killed at */
            else if(uptime_ms() - held_from >= HOLD_WAIT_MS || (curTask && (curTask->intr & TI_KILL))) {
                gave_up = true;                         /* once in here 2 s after its kill, holding what its */
                break;                                  /* file system holds (kernel/task.c's kill_escalate()) */
            }
            sleep_unlocked_ms(100);
        } else sleep_unlocked();
        rounds--;                                       /* a wait is no round */
    }
    for(i = 0; i < BCACHE_BLOCKS && !r; i++) if(owed(&bufs[i], dev, top, call)) r = -1;
    mutex_unlock(&bcache_lock);
    if(gave_up) kprintf("bcache: a sync of %s gave up on a block someone held or set aside for %lu s\n",
                        dev ? dev->name : "every device", HOLD_WAIT_MS / 1000);
    return r;
}

int bsync(struct bdev *dev) {
    return bsync_order(dev, BORDER_MAX);
}

/* 10/24/26 (B6, B14): bcache.h */
int bcache_room(struct bdev *dev) {
    struct buf *b;
    int r = 0;
    if(!dev || dev->gone) return 0;             /* the write fails as the device does */
    mutex_lock(&bcache_lock);
    if(dirty_total(dev) >= BCACHE_HOLD) {
        if(dev->bc_failed) {                    /* B14: asked again, with one write of its oldest block */
            b = oldest_writable(dev);
            if(!b || write_run(b, 0, true, NULL, NULL) != 0) r = -1;
        }
        while(!r && dirty_total(dev) > BCACHE_HOLD / 2 && (b = oldest_writable(dev)) != NULL)   /* down to half */
            if(write_run(b, 0, false, NULL, NULL) != 0) break;
        if(dev->bc_failed && dirty_total(dev) >= BCACHE_HOLD) r = -1;
    }
    mutex_unlock(&bcache_lock);
    return r;
}

void binvalidate(struct bdev *dev) {
    int i;
    mutex_lock(&bcache_lock);
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *b = &bufs[i];
        if(b->dev == dev && (b->valid || b->hashed || b->loading)) discard_buf(b);
    }
    wake_up_all(&bwait);
    mutex_unlock(&bcache_lock);
}

u_int bdirty_count(struct bdev *dev) {
    u_int n;
    mutex_lock(&bcache_lock);
    n = dirty_total(dev);
    mutex_unlock(&bcache_lock);
    return n;
}

u_int bdirty_blocks(struct bdev *dev, u32 *lbas, u_int max) {
    u_int n = 0;
    int i;
    mutex_lock(&bcache_lock);
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *b = &bufs[i];
        if(b->dev != dev || !b->valid || !b->dirty || b->discard) continue;
        if(n < max) lbas[n] = b->lba;
        n++;
    }
    mutex_unlock(&bcache_lock);
    return n;
}

void baside(struct bdev *dev, u32 lba, bool on) {
    struct buf *b;
    u_long f;
    mutex_lock(&bcache_lock);
    b = find(dev, lba);
    if(b && b->valid && b->dirty && b->aside != on) {
        f = irq_save();
        if(on) {
            if(dev->bc_dirty[b->order]) dev->bc_dirty[b->order]--;
            dev->bc_aside++;
            dev->bc_aside_by = curTask;
        } else {
            if(dev->bc_aside) dev->bc_aside--;
            dev->bc_dirty[b->order]++;
            if(!dev->bc_aside) dev->bc_aside_by = NULL;
        }
        b->aside = on;
        irq_restore(f);
        if(!on) wake_up_all(&bwait);            /* another task's sync waits for it */
    }
    mutex_unlock(&bcache_lock);
}

void bdiscard(struct bdev *dev, u32 lba) {
    struct buf *b;
    mutex_lock(&bcache_lock);
    b = find(dev, lba);
    if(b) {
        discard_buf(b);
        wake_up_all(&bwait);
    }
    mutex_unlock(&bcache_lock);
}

int bcache_audit(struct bdev *dev) {
    u_int dirty[BORDER_MAX + 1], aside = 0, failed = 0, off = 0;
    int i, k;
    memset(dirty, 0, sizeof(dirty));
    mutex_lock(&bcache_lock);
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *b = &bufs[i];
        if(b->dev != dev || !b->dirty) continue;
        if(b->aside) aside++;
        else dirty[b->order]++;
        if(b->wfail) failed++;
    }
    for(k = 0; k <= BORDER_MAX; k++) off += dirty[k] != dev->bc_dirty[k];
    off += (aside != dev->bc_aside) + (failed != dev->bc_failed);
    mutex_unlock(&bcache_lock);
    return (int)off;
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

/* src NULL: zeros. 10/24/26 (B15, B22): the cached copies of the run are kept out of every other write and change
   meanwhile (marked writing: getblk() waits for them) and take the bytes once the device has them (clean); when it
   refuses, a clean copy is dropped (the device may hold part of the request: what is read is what it holds) and a
   dirty one keeps its bytes, the next sync's to write over the device's. A refused write changes nothing here. */
static int write_blocks(struct bdev *dev, u32 lba, u32 count, const u8 *src) {
    struct buf *cp[BCACHE_RUN];
    while(count) {
        u32 n = count < BCACHE_RUN ? count : BCACHE_RUN, i;
        bool busy;
        int r;
        if(dev->gone) return BDEV_EGONE;
        mutex_lock(&bcache_lock);
        do {                                    /* none of the copies being read in or written out */
            busy = false;
            for(i = 0; i < n && !busy; i++) {
                struct buf *p = find(dev, lba + i);
                busy = p && (p->loading || p->writing);
            }
            if(busy) sleep_unlocked();
        } while(busy);
        for(i = 0; i < n; i++) {
            struct buf *p = find(dev, lba + i);
            cp[i] = p && p->valid ? p : NULL;
            if(cp[i]) cp[i]->writing = true;
        }
        inflight++;
        mutex_unlock(&bcache_lock);
        mutex_lock(&bounce_lock);
        if(src) memcpy(bounce, src, n * 512);
        else memset(bounce, 0, n * 512);
        r = bdev_write(dev, lba, n, bounce);
        mutex_unlock(&bounce_lock);
        mutex_lock(&bcache_lock);
        inflight--;
        io.writes++;
        if(r == 0) io.written_blocks += n;
        for(i = 0; i < n; i++) {
            struct buf *p = cp[i];
            if(!p) continue;
            p->writing = false;
            if(p->discard) { if(!p->refs) drop(p); continue; }
            if(r == 0) {                        /* the device has the bytes: so does the copy */
                if(src) memcpy(p->data, src + i * 512, 512);
                else memset(p->data, 0, 512);
                p->gen++;
                mark_clean(p);
            } else if(!p->dirty) discard_buf(p);
        }
        wake_up_all(&bwait);
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
