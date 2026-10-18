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
gets no buffer (an I/O error) (that retry is gone since B6, below). bsync() tries each run once per call (failed runs are
not sent again for each of their blocks; since B13 a failed run is tried again block by block)
and returns -1 when any write failed (since B12: when a block it owes was not written). The
console hears of the first failure of a device and of the write that works again; the
failures between are quiet (their callers hear of them). A device that is gone still
has its blocks dropped.

10/16/26 (audit B3): the order of writes. Before, bsync() wrote dirty blocks in the
order of the buffer table, and a card pulled in the middle left what came of it: on
a FAT volume a new file's directory entry could be on the card while its cluster was
still free in the FAT (SurfOS, or the next computer, could give that cluster to
another file), or the entry and the FAT without the file's bytes (the file reading
whatever the cluster held before). Now a buffer carries an order (bdirty_order()), a
run never mixes two, bsync() writes a device's blocks order by order (B12: a block of a
higher order once what it may point at is written), and a block of a higher order is
written to make room only after the dirty blocks of a lower order of its device that it
may point at (B12: those with a change older than its last), or not at all, until a sync.
What a file system can still leave behind is said in surfcpu/docs/CARD.md.

10/16/26 (audit B9): each device keeps the cache's account of it in its struct bdev
(bc_dirty[] by order, bc_failed, bc_said; B12-B16 added the lists of its dirty blocks,
bc_aside and a sync's tally of its requests), kept by mark_clean(), mark_failed() and
bdirty_order() at every change of a block's dirty, order or failed state. The console line
about a device that refuses writes is said once per device and the line about it working
again follows it, whatever other devices failed and went away before (the table of eight
failing devices is gone). bcache_audit() recounts a device's blocks against its account,
for the tests.

10/16/26 (audit B8): making room writes a block's lower orders first, which lets the lock
go; the block may be written, dropped or taken for another block meanwhile. evict_write()
looks at it again and leaves it alone if it is not still the same dirty block, and
write_run() never sends an empty request.

10/16/26 (audit B7): the order is kept where a write is issued, not only by the callers
that sort blocks: write_run() sends a device's block of order k only when the device has
no dirty block of an order below k with a change older than the block's last one (B12;
B7 asked for none at all; the lists say, under the lock, just before the write), for bsync(),
for making room and for evict_write() alike. (B7's rounds of bsync() for blocks dirtied by
another task, up to 8 and then 0 whatever was left, are gone: B12.) A block is not written
while someone holds it (it may be in the middle of a change; bsync() waits for it, making
room passes it over), and getblk() hands out no block while it is being written, so what
reaches the device is what was checked (B12's copies are the exception: the copy is what
was checked).
bdirty_order() still keeps the lower of two orders: a block's earlier content must not
reach the disk after what points at it, and FAT no longer makes one block dirty with two
orders (fs/fat.c, B7).

10/16/26 (audit B6): a device that refuses writes no longer fills the cache that every
device shares. Once one of its blocks failed (bc_failed), its dirty blocks are held back:
making room passes over them and tries no write bound to fail (B1's retry when nothing
else is free is gone: sync, close, fsync and eject try again), and a block held back
because a block below it failed is not marked failed, since it was never tried. A device
keeps at most BCACHE_HOLD dirty blocks (a quarter of the cache): one that works and has
that many writes some out first (room()); one that refuses gets no more (bget() returns
NULL, bcache_room() says no, and the file system's write fails with EIO; since B14 it is
asked again first; B15 says exactly what the share holds back). One refusing device leaves
the other three quarters to the rest; there is no share across devices (four refusing at
once could hold every buffer). (B6 tried a failed run again block by block, and stopped
after two failures before a success; B13 replaced that.) Read-ahead looks past the dirty
buffers at the old end of the list. "takes writes again" is said when none of the
device's blocks waits after a failure any more.

10/16/26 (audit B12): the order by what a block may point at, not by any lower-order dirt.
B7 held a block back while its device had any dirty block of a lower order: another task
that wrote file data whenever the device worked held a directory block back for good, and
bsync() gave up after 8 rounds and returned 0 with it still dirty (a close said a file's
entry was on the card when it was not; an unlink's order point could let its clusters be
freed before its entry was off the card). Now each change has a number (bdirty_order()), a
block records the first change the device does not have yet (first_seq) and its last one
(last_seq), and each device keeps its dirty blocks of each order in a list by first_seq
(bc_head[], bc_tail[]). A block may be written when no dirty block of a lower order on its
device has a change older than the block's last one: what it may point at is on the device,
and what came later cannot be what it points at (deps_out(), for every write, where B7's
check was). bsync() owes the blocks that were dirty when it began (a change numbered up to
its start that the device does not have) and returns 0 only when every one of them reached
the device; blocks made dirty meanwhile are written when they may be and never waited for.
When other tasks keep changing what an owed block needs, after two rounds the sync writes a
copy of it (write_copy()): taken while nobody holds the block, it goes after every lower-order
change older than the copy (a copy of their own for blocks that cannot go as they are, one
order down), and the block stays open to changes meanwhile (only other writes of it wait),
dirty afterwards with what came after the copy (its first_seq then the first change after
the copy, next_seq). That ends: what must go first is older than the copy, and nobody makes
old changes. Making room writes what a block needs from the lists (evict_write()), oldest
first, and passes the block over at the first that cannot go now: B7's write_below() walked
the whole cache for each lower order, twice, every time a block was passed over.

10/16/26 (audit B13): one block that refuses no longer holds back a device's other blocks in a
sync. B6 tried nothing more of a device in a sync after its first failure (bc_bad_pass), tried
a failed run again block by block only when it had several blocks, and stopped that after two
failures before a success: one bad sector kept every block made dirty after the first failed
sync off the device, two at the head of a run kept the rest of the run off it, and a block
that write_singly() could not try (RUN_BLOCKED) counted as written, so a sync whose FAT sector
refused could return 0. Now a sync tries every block it may (blocks that never failed first),
tries a failed run of several blocks again block by block in the same call, takes RUN_BLOCKED
as "not written", and asks nothing more of a device only once it has refused DEAD_TRIES (4)
requests in that call and taken none (since B26 each call keeps that count: tally()): it takes nothing.

10/16/26 (audit B14): a device that refused is asked again. room() said no to a device with a
failed block that held BCACHE_HOLD dirty blocks, and never asked the device again: after a card
had refused once, every write to it failed with EIO until something synced, even once it worked
(and FAT's order points for a growing file sync only data and tables, so a failed directory
block was never tried there). Now room() first writes the oldest block of the device's lowest
order alone (it waits for nothing of the device): one request per call, so a device that still
refuses is asked once per file system call, not for every block; when it works, room() goes on
as for a device that works, down to half the share, the failed blocks with the others.

10/16/26 (audit B15): the straight path changes no cached copy before the device has the bytes.
write_blocks() put a run's bytes into the cached copies first (dirty), so that nothing could write
an older copy over them; when the device refused, the copies kept the bytes, dirty: the refused
write was in the file as read back, reached the device with the next sync that worked, and took a
refusing device past its share (a 64 KB write over cached blocks: 379 dirty, BCACHE_HOLD 256). Now
the copies are taken out of every other write while the device works (marked writing: getblk()
waits for them), take the bytes once the device has them (clean), and keep what they had when it
refuses (B22: the dirty ones; a refused request may have changed the device). The share, exactly: file data never takes a device that refuses past BCACHE_HOLD dirty
blocks (each block through the cache asks room() first; the straight path dirties nothing); a file
system operation that was let in still adds its own table, directory and FSInfo blocks, which are
not counted (a few for each operation under way when the share filled).

10/16/26 (audit B16): blocks set aside. A file whose data block refused could not be removed: the
order point that takes its entry off the disk must write the directory block, which waits for the
older data block, so rm said EIO as long as the block refused. A file system about to free clusters
behind an order point may now set their failed blocks aside (baside()): not written, nobody's
dependency and no sync's debt (a sync of another task does not wait for them either: their cluster
is on its way out; B26: it does now); it puts them back when the order point fails (nothing changed)
and drops them once the clusters are free (bdiscard(): their bytes will never be read).
bfailed_blocks() lists a device's failed blocks (B21: and says how many there are, all of them).

10/16/26 (audit B18): a holder that does not let go. bsync() waited for a held block for ever: a task
killed while it held one (kernel/task.c's kill_escalate(): still in a call 2 s after a kill, killed at
once with what it holds) left every later sync of that device hanging. Now a sync waits HOLD_WAIT_MS
(10 s) for a hold of a block it owes, counted from when a sync first had to wait for one of the device's
holds (each hold is stamped then, all at once: a dead task's several holds cost one wait, and taking a
block costs no clock), then gives up on it: not written (-1), said on the console; later syncs give up on
that hold at once (stuck, until the block is let go).

10/16/26 (audit B19): bpeek() hands out a block that is being written as well (its contents are
what goes to the device): for readers only, as fs/bcache.h says now.

10/16/26 (audit B22): a refused straight write may have changed the device. B15 kept the cached
copies of a refused run as they were, as if a refused request stored nothing; but an SD card's
multi-block write (CMD25) stores the blocks before the one it refuses, USB mass storage its 32 KB
pieces and ATA its sectors one by one, so a clean copy could hold what the device no longer did: a
file read one thing until the cache made room and then another, with no write between. Now the
clean copies of a refused run are dropped (one a reader holds then is taken out of the table and
dropped when let go): what is read of them afterwards is what the device holds. A dirty copy keeps
its bytes (what the file system holds, and what is read: the device may hold the refused request's
bytes for that block until the next sync writes the cache's over them).

10/16/26 (audit B26): B13 and B16 under several tasks. B13's count of the requests a device refused
was kept in the device for the sync call that counted last, so making room or another task's sync
in between started it again, and a sync could ask a card that takes nothing for every block: each
sync call keeps its own (struct tally; making room counts none, it asks once). B16's blocks set
aside were no sync's debt, so another task's sync could return 0 while an rm's order point had one
aside, and the rm then failed and put it back, not written: a block set aside is now the debt of
every other task's sync (the device records whose operation set its blocks aside: one at a time,
under the volume's lock), which waits for it as for a hold (B18: 10 s at most, then not written).
A sync of every device counts the requests of the first TALLY_DEVS (8) devices it meets; it asks the
others with no early stop (each block at most twice in the call all the same).

10/16/26 (audit B28): bdirty_blocks() lists a device's dirty blocks, failed or not: a file system drops
or sets aside every dirty block of the clusters it frees or cuts off, not only those that failed
(fs/fat.c); fs/fat.c's B32 keeps blocks set aside for one order point only.

10/16/26 (audit B33): a sync writing a block from a copy (B12) waited, with that block held open, for a
block the copy needs written first that another task's operation had set aside (B26: as for a hold), and
that operation's order point may owe the copied block and wait for the copy: each waited for the other, the
copy was given up after 10 s and its sync failed ("set aside for 10 s by an operation that does not end").
B28, which sets every dirty block of a chain an rm takes away aside, not only failed ones, made it likelier.
Such a block now ends the copy: the copied block is let go and looked at again (a block set aside is
nobody's dependency: nothing in the cache names it any more).

10/16/26 (audit B34): B33 held only for the block the copy was waiting for. A block the copy needed that
was set aside, or dropped, while it wrote or waited for another left the dirty lists, the copy never looked
for it again, and the copy, taken while the directory block still named the file, reached the disk while
that block's older change never did: the card named the file over whatever the block held. B33's "a block
set aside is nobody's dependency" is false for a copy taken before the block was set aside: the copy names
it. Each copy under way is now recorded (struct copy_rec); setting aside or dropping a dirty block spoils
every copy that needed it (a lower order, a change older than the copy), and under the lock, right before
its write, a copy goes only when it is not spoiled and nothing it needs is dirty in the lists, else it is
taken again: at most COPY_LOOKS looks, then -1 (the block stays dirty, the sync says so, the next sync tries
again). A block set aside before the copy was taken is not its dependency. write_run's runs, write_singly()
and bsync(NULL) check deps_out() under the lock right before writing the buffer itself.

10/16/26 (audit B35): bdiscard() of a block in someone's hands or under I/O marks it, and it is dropped when
let go or when its I/O ends, unless a change came after (the block has an owner again). B16 left it, and a
write of it the device refused made every later sync owe a block of a cluster nothing names.

10/17/26 (audit B39): a kill and the cache. A kill waits KILL_WAIT_MS for the end of a call and then kills at once
(kernel/task.c), and none of the cache's waits was one a kill cuts short (B18's for a hold last 10 s): a sync
killed at once in write_copy() left its record on its dead stack in the list of copies (every later set-aside or
drop walked freed memory) and the copied block marked copying for good. Now the waits that can last (for a hold,
a block set aside by another task, another task's copy or write under way, a hold that came back) are cut short
by a kill or, in a kernel command, Ctrl+C (bsleep(), wait_prepare_intr()), and the call unwinds; a task ending
by itself (TC_EXIT) waits on. A kill that waited is put off while the task holds the cache's lock or the bounce
buffer's (bcache_holds(), kernel/task.c) and while it has let go of the lock with state of its own here (blocks it
marked as read, written or copied, a copy's record and buffer: counted in surf_task.fs_hold around device requests
and copies), so nothing it holds here stays behind. A timed wait's timer is the task's wait_timer.

10/17/26 (audit B40): binvalidate() drops as bdiscard() does: a dirty block's drop spoils the copies that needed
it (B34), and a block in someone's hands or under I/O goes when let go or when its I/O ends; a copy whose block
is to go is not written. It dropped idle dirty blocks without a spoil and kept the others: another task's copy
went out after a block it needed was given up unwritten, and its block stayed cached after the umount.

10/17/26 (audit B41): B34's spoils and bound. The drop of a block set aside before a copy was taken spoiled the
copy all the same (it spoiled it when it was set aside), so every rm, truncate and mv over a file took the copies
under way again, and after 16 looks a sync said -1 on a card that took every write. Now only copies of blocks
that changed since they were taken are spoiled (an unchanged copy names nothing the block does not: a file
system drops or sets aside a block only once nothing in the cache names it), the drop of a block set aside
spoils none, a copy ends once another task's write paid the call's debt, and a spoiled copy is taken again as
long as it takes (each look waits for or writes something; a kill ends it).

10/17/26 (audit B45): a change to a block whose drop was asked for (bdiscard(): its bytes were nobody's) starts its
dirt again, with that change's order, set aside no more: a block set aside, read meanwhile, dropped, and written
by its cluster's next owner before the reader let go stayed set aside, and nobody wrote it.

10/17/26 (audit B46): no wait is cut short in a step that must finish (surf_task.fs_finish: umount, eject's
unmount), whatever the task's call: cut short, an umount gave up what the card would take. (B49) A straight write
(write_blocks()) whose wait is cut short returns BCACHE_EINTR, not BDEV_EIO: nothing was sent, nothing refused.
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
#define SYNC_ROUNDS 2                           /* B12: rounds of a sync before it writes copies */
#define DEAD_TRIES 4                            /* B13: requests a device refuses in a sync, taking none, before it is let be */
#define HOLD_WAIT_MS 10000UL                    /* B18: how long a sync waits for one hold of a block it owes */

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
static u32 reuse_calls;                         /* 10/16/26 (B7): reuse() calls, for buf.skip */
static u64 changes;                             /* 10/16/26 (B12): the number of the last change (bdirty_order()) */
static int evict_write(struct buf *b);
static int room(struct bdev *dev);
#define RUN_BLOCKED 1                           /* write_run(): nothing tried: the order or a holder forbids it now (B7) */
#define WAIT_ASIDE 2                            /* 10/16/26 (B33): wait_for() of a copy's dependency: another task set it aside */
#define COPY_ASIDE 2                            /*      write_copy() below the top: the block was set aside by another task */
#define COPY_AGAIN 3                            /*      (write_copy() within: its copy let go, a fresh look at the block) */
#define COPY_DROPPED 4                          /* 10/17/26 (B40): (write_copy() within: the block goes, its copy is not written) */

/* 10/16/26 (B34): a copy under way (write_copy()): the block, the last change the copy holds, its order, and
   whether a block it needs written first (of a lower order, with a change older than the copy) has left the lists
   since it was taken without being written: set aside (B16) or dropped. The copy may name that block (a file
   system sets a block aside only once nothing in the cache names it, but a copy taken before still does), so it
   is not written: write_copy() takes it again. A block set aside before the copy was taken is not its dependency.
   10/17/26 (B41): and only while the block has changed since the copy was taken (gen: its generation then). A file
   system sets a block aside or drops it only once nothing in the cache names it: a copy of a block that has not
   changed since is what the block holds now and names it no more than the block does (taken again, it would be
   the same copy, needing what it needed less that block) */
struct copy_rec {
    struct buf *b;
    u64 at;
    u32 gen;
    u8 order;
    bool spoiled;
    struct copy_rec *next;
};
static struct copy_rec *copies;                 /* (bcache_lock) the copies under way, every task's */

static void copy_unlink(struct copy_rec *rec) {
    struct copy_rec **pp;
    for(pp = &copies; *pp; pp = &(*pp)->next) {
        if(*pp == rec) {
            *pp = rec->next;
            return;
        }
    }
}
#define TALLY_DEVS 8                            /* B26: the devices a sync call counts requests of (tally()); a sync of every
                                                   device that meets more asks the others without a count: no early stop
                                                   for them, each block still asked at most twice in the call */
struct tally {                                  /* 10/16/26 (B26): a sync call's requests of each device */
    struct bdev *dev[TALLY_DEVS];
    u16 refused[TALLY_DEVS], taken[TALLY_DEVS];
    u_int n;
};
/* 10/16/26 (B9): a device's account (struct bdev's bc_dirty[] and lists) has a place for each write order */
typedef char bc_orders_fit[(BORDER_MAX + 1) * sizeof(u_int) == sizeof(((struct bdev *)0)->bc_dirty) &&
                           (BORDER_MAX + 1) * sizeof(struct buf *) == sizeof(((struct bdev *)0)->bc_head) ? 1 : -1];

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

/**** a device's account (B9): all of this with interrupts off (bdirty_order() runs without bcache_lock) ****/

/* 10/16/26 (B12): b into its device's list of dirty blocks of its order, which runs from the oldest first
   unwritten change to the newest (a block made dirty just now goes at the end at once). A dirty block is on
   its list unless it is set aside (B16) */
static void dlist_in(struct buf *b) {
    struct bdev *d = b->dev;
    struct buf *p = d->bc_tail[b->order];
    while(p && p->first_seq > b->first_seq) p = p->dprev;
    b->dprev = p;
    b->dnext = p ? p->dnext : d->bc_head[b->order];
    if(b->dnext) b->dnext->dprev = b;
    else d->bc_tail[b->order] = b;
    if(p) p->dnext = b;
    else d->bc_head[b->order] = b;
}

static void dlist_out(struct buf *b) {
    struct bdev *d = b->dev;
    if(b->dprev) b->dprev->dnext = b->dnext;
    else d->bc_head[b->order] = b->dnext;
    if(b->dnext) b->dnext->dprev = b->dprev;
    else d->bc_tail[b->order] = b->dprev;
    b->dnext = b->dprev = NULL;
}

/* b's changes reached its device, or never will (it went away; its cluster was freed): clean */
static void mark_clean(struct buf *b) {
    struct bdev *d = b->dev;
    if(b->dirty && d) {
        if(b->aside) d->bc_aside--;
        else dlist_out(b);
        d->bc_dirty[b->order]--;
        if(b->wfail) d->bc_failed--;
    }
    b->dirty = b->wfail = b->aside = false;
}

/* b's last write failed (true) or reached the device (false) */
static void mark_failed(struct buf *b, bool failed) {
    if(b->dirty && b->dev && b->wfail != failed) {
        if(failed) b->dev->bc_failed++;
        else b->dev->bc_failed--;
    }
    b->wfail = failed && b->dirty;
}

/* a dirty block's first unwritten change is a later one now (part of it reached the device: B12) */
static void move_first(struct buf *b, u64 first) {
    if(!b->dirty || !b->dev || first <= b->first_seq) return;
    if(!b->aside) dlist_out(b);
    b->first_seq = first;
    if(!b->aside) dlist_in(b);
}

/**** waiting ****/

/* forget a block: not found any more, its buffer the next to be reused */
static void drop(struct buf *b) {
    u_long f;
    hash_out(b);
    f = irq_save();
    mark_clean(b);
    b->valid = false;
    irq_restore(f);
    b->fail_pass = 0;
    b->order = BORDER_DATA;
    b->stuck = false;
    b->discard = false;
    b->held_since = 0;
    lru_oldest(b);
}

/* (bcache_lock held) 10/16/26 (B34): d, dirty, leaves its device's lists without being written (set aside, dropped):
   the copies under way that need it are spoiled (struct copy_rec). 10/17/26 (B41): those of blocks that changed
   since their copy was taken */
static void spoil_copies(const struct buf *d) {
    struct copy_rec *c;
    for(c = copies; c; c = c->next)
        if(c->b->dev == d->dev && d->order < c->order && d->first_seq < c->at && c->b->gen != c->gen) c->spoiled = true;
}

/* (bcache_lock held) 10/16/26 (B35): a block whose bytes are nobody's (bdiscard()) is dropped, the copies that needed
   it spoiled; one in someone's hands or under I/O is marked, and dropped when let go or when its I/O ends, unless a
   change came after (bdirty_order() clears the mark: the block has an owner again). 10/17/26 (B41): a block set
   aside spoiled the copies under way when it was set aside (baside()), and no copy taken since needs it: dropped,
   it spoils none (it took the copies of every rm, truncate and mv over a file again, and enough of them failed a
   sync on a card that took every write) */
static bool discard_one(struct buf *b) {         /* (true: dropped, the caller wakes the waiters) */
    if(!b->discard && b->dirty && !b->aside) spoil_copies(b);   /* (once, when its bytes became nobody's: B45 may */
    if(b->refs || b->loading || b->writing || b->copying) {      /* give the block a new owner before it is dropped) */
        b->discard = true;
        return false;
    }
    drop(b);
    return true;
}
static void discard_now(struct buf *b) {
    if(discard_one(b)) wake_up_all(&bwait);
}

/* 10/17/26 (B39): is the calling task's call to end? A kill (TI_KILL) or, in a kernel command, Ctrl+C (TI_INT)
   cuts the cache's long waits short (surfos/task.h), so that the call gives up and unwinds; not while the task
   ends by itself (TC_EXIT: its files are closing and written out, and no kill comes any more). 10/17/26 (B46): nor
   in a step that must finish (fs_finish: umount, whose syncs cut short gave up changes the card would take) */
static bool cut_short(void) {
    return curTask != idle_task && fs_cut_short();
}

/* 10/17/26 (B39): what a kill at once (kernel/task.c's kill_escalate()) would leave behind here: the cache's lock or
   the bounce buffer's while the task holds one (bcache_holds(): the kill is put off while it does), and, while it
   has let go of the lock, the blocks it marked as read, written or copied, its copy's record and the copy's buffer:
   the task is counted inside the cache then (surf_task.fs_hold, inside() before it lets go of the lock, outside()
   once it has it again, so no moment is uncovered). Counted only around device requests and copies, never in the
   calls made for each block (a count there cost SurfCPU about 1 % on its card's commands) */
static inline void inside(void) { if(curTask) curTask->fs_hold++; }
static inline void outside(void) { if(curTask && curTask->fs_hold) curTask->fs_hold--; }
#define lock() mutex_lock(&bcache_lock)
#define unlock() mutex_unlock(&bcache_lock)

bool bcache_holds(const struct surf_task *t) {
    return t && (bcache_lock.owner == t || bounce_lock.owner == t);
}

static void wake_waiters(void *arg) {
    (void)arg;
    wake_up_all(&bwait);
}

/* sleep until a block's I/O ends or a buffer is released; returns holding the lock again. 10/16/26 (B18): for ms
   at most (0: no limit; a timer wakes the waiters then). 10/17/26 (B39): intr, a wait that a kill or Ctrl+C cuts
   short (wait_prepare_intr(): kill_task() wakes it): -1 then, at once when the call is to end already, else 0. The
   timer is the task's wait_timer meanwhile: a task killed at once in the wait takes it off the list as it dies
   (kernel/task.c), since it lives on the stack that goes with the task */
static int bsleep(u_long ms, bool intr) {
    struct ktimer t;
    u_long f;
    if(!curTask || curTask == idle_task || in_interrupt()) {    /* at boot nobody else uses the cache */
        mutex_unlock(&bcache_lock);
        mutex_lock(&bcache_lock);
        return 0;
    }
    if(ms) ktimer_init(&t, wake_waiters, NULL);
    f = irq_save();
    if(intr && cut_short()) {
        irq_restore(f);
        return -1;
    }
    if(intr) wait_prepare_intr(&bwait);         /* queued before the lock goes: no wake-up is missed */
    else wait_prepare(&bwait);
    if(ms) {
        ktimer_start(&t, ms, false);
        curTask->wait_timer = &t;
    }
    mutex_unlock(&bcache_lock);
    irq_restore(f);
    yield();
    if(ms) {
        f = irq_save();
        ktimer_cancel(&t);
        curTask->wait_timer = NULL;
        irq_restore(f);
    }
    mutex_lock(&bcache_lock);
    return intr && cut_short() ? -1 : 0;
}
#define sleep_unlocked() ((void)bsleep(0, false))   /* I/O under way (a read, a write): it ends */

/* (bcache_lock held) 10/16/26 (B26): is b set aside (B16) by an operation of another task than the caller
   (a file system sets blocks of a device aside only within one operation, under its volume's lock: the
   device records whose it is) */
static inline bool aside_other(const struct buf *b) {
    return b->aside && b->dev && b->dev->bc_aside_by != (const void *)curTask;
}

/* (bcache_lock held) 10/16/26 (B18): the holds of dev's blocks start their clocks now, every one that has
   none: a sync waits for one of them. Not when a hold begins (that is every read and write of a block,
   and a clock costs SurfCPU a few hundred cycles each time); a task killed with several blocks in hand
   has them all stamped at the first wait, so the sync that meets them waits once. 10/16/26 (B26): blocks
   another task's operation set aside count as held */
static void stamp_holds(struct bdev *dev, u_long now) {
    int i;
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *p = &bufs[i];
        if(p->dev == dev && (p->refs || aside_other(p)) && !p->held_since) p->held_since = now;
    }
}

/* (bcache_lock held) 10/16/26 (B18): wait until b, a dirty block a sync owes, is in nobody's hands and
   under no write. A write ends; a hold may not: once a sync has waited HOLD_WAIT_MS for it (stamp_holds())
   it is given up on: false, b stays dirty (not written), and later syncs give up on that hold at once
   (b->stuck, until it is let go). true also when b was written or dropped meanwhile (look again).
   10/16/26 (B26): a block another task's operation set aside is waited for in the same way, until it is
   put back or dropped (an operation that never ends, a task killed in it, is given up on likewise).
   10/16/26 (B33): 1 (look again) or 0 (given up), and for a block a copy needs written first (dep, from
   write_copy(), which holds the block it copies open meanwhile), WAIT_ASIDE as soon as another task's
   operation has it set aside: not waited for. That operation's order point may be waiting for the copy (a
   sync owes the copied block): each waited for the other, 10 s, and the copy was given up.
   10/17/26 (B39): a kill or Ctrl+C cuts the wait short (bsleep()): 0, given up on (nothing said, not stuck: the
   holder is not the one at fault) */
static int wait_for(struct buf *b, bool dep) {
    struct bdev *dev = b->dev;
    u32 lba = b->lba;
    for(;;) {
        u_long held, now;
        bool other;
        if(b->dev != dev || b->lba != lba || !b->valid || !b->dirty) return 1;
        if(b->writing || b->copying) {          /* (a copy of another task's may take long: it waits for holders too) */
            if(bsleep(0, true)) return 0;
            continue;
        }
        other = aside_other(b);
        if(other && dep) return WAIT_ASIDE;
        if((!b->refs && !other) || dev->gone) return 1;
        if(b->stuck) return 0;
        now = uptime_ms();
        if(!now) now = 1;                       /* (0 stands for no clock yet) */
        if(!b->held_since) stamp_holds(dev, now);
        held = now - b->held_since;
        if(held >= HOLD_WAIT_MS) {
            b->stuck = true;
            kprintf(other ? "bcache: %s block %lu set aside for %lu s by an operation that does not end: not written\n"
                          : "bcache: %s block %lu held for %lu s by a task that does not let go of it: not written\n",
                    dev->name, (u_long)lba, held / 1000);
            return 0;
        }
        if(bsleep(HOLD_WAIT_MS - held < HOLD_WAIT_MS / 20 ? HOLD_WAIT_MS - held : HOLD_WAIT_MS / 20, true)) return 0;
    }
}

void init_bcache(void) {
    int i;
    mutex_lock(&bcache_lock);
    memset(bufs, 0, sizeof(bufs));
    memset(hash, 0, sizeof(hash));
    newest = oldest = NULL;
    copies = NULL;
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        bufs[i].older = newest;
        if(newest) newest->newer = &bufs[i];
        else oldest = &bufs[i];
        newest = &bufs[i];
    }
    mutex_unlock(&bcache_lock);
}

/**** writing ****/

/* (bcache_lock held) 10/16/26 (B12): is everything b may point at on its device? No dirty block of a lower
   order on it has a change older than b's last one (each list starts with its oldest) */
static bool deps_out(const struct buf *b) {
    struct bdev *d = b->dev;
    bool ok = true;
    u_long f = irq_save();
    u8 k;
    for(k = 0; k < b->order && ok; k++) if(d->bc_head[k] && d->bc_head[k]->first_seq < b->last_seq) ok = false;
    irq_restore(f);
    return ok;
}

/* the oldest dirty block of order k on dev with a change older than `before`, or NULL */
static struct buf *oldest_below(struct bdev *dev, u8 k, u64 before) {
    struct buf *d;
    u_long f = irq_save();
    d = dev->bc_head[k];
    if(d && d->first_seq >= before) d = NULL;
    irq_restore(f);
    return d;
}

/* may p go out with b's run: dirty, of b's order, nobody writing it, copying it (B12) or (B7) holding it,
   not set aside (B16), not failed in this bsync() call, and (B12) what it may point at on the device */
static bool runs_with(const struct buf *p, const struct buf *b, u32 pass) {
    return p && p->valid && p->dirty && !p->writing && !p->copying && !p->aside && p->order == b->order &&
           !(pass && p->fail_pass == pass) && (b->dev->gone || (!p->refs && deps_out(p)));
}

/* (bcache_lock held) a new bsync() call number (0 stands for reuse()'s writes) */
static u32 new_pass(void) {
    if(!++sync_passes) sync_passes = 1;
    return sync_passes;
}

/* 10/16/26 (B13): a request of a sync call to dev, counted in the call's tally (t; NULL: not counted); a
   device that refused DEAD_TRIES of them and took none is asked nothing more in that call (dead()).
   10/16/26 (B26): each call keeps its own tally (B13 kept one in each device, for the call that counted
   last: a room() or another task's sync in between started it again), for its first TALLY_DEVS devices */
static void tally(struct tally *t, struct bdev *dev, int r) {
    u_int i;
    if(!t) return;
    for(i = 0; i < t->n && t->dev[i] != dev; i++) ;
    if(i == t->n) {
        if(t->n == TALLY_DEVS) return;
        t->dev[t->n++] = dev;
        t->refused[i] = t->taken[i] = 0;
    }
    if(r == 0) { if(t->taken[i] < 0xFFFF) t->taken[i]++; }
    else if(t->refused[i] < 0xFFFF) t->refused[i]++;
}

static bool dead(const struct tally *t, const struct bdev *dev) {
    u_int i;
    if(!t) return false;
    for(i = 0; i < t->n; i++) if(t->dev[i] == dev) return !t->taken[i] && t->refused[i] >= DEAD_TRIES;
    return false;
}

/* the console's line after a request of dev (B1, B9): once when it refuses, and (B6) once when it takes
   writes again, when none of its blocks waits after a failure any more */
static void tell(struct bdev *dev, int r, u32 first, u32 n) {
    if(dev->gone) dev->bc_said = false;
    else if(r == 0 && dev->bc_said && !dev->bc_failed) {
        dev->bc_said = false;
        kprintf("bcache: %s takes writes again\n", dev->name);
    } else if(r != 0 && !dev->bc_said) {
        dev->bc_said = true;
        kprintf("bcache: write of %s blocks %lu-%lu failed (%i); kept for the next sync (more failures are not shown)\n",
                dev->name, (u_long)first, (u_long)(first + n - 1), r);
    }
}

/* Write b and its dirty neighbours (up to max blocks, BCACHE_RUN at most, in one request; the run's
   first block and length in *pfirst, *pn when given). Drops the lock
   while the device works. A gone device's blocks are dropped instead, without a word. pass:
   the bsync() call (0 from reuse()); a block that failed in it is not part of another run; t: its
   tally of requests (B13, B26; NULL: not counted).
   A run is of one order (B3). 10/16/26 (B7): the order is kept here, for every caller: nothing
   is written while (B12) a dirty block of a lower order with an older change is on its device, nor
   a block someone holds (it may be in the middle of a change), one being copied (B12) or one set
   aside (B16); RUN_BLOCKED then, nothing tried. getblk() hands out no block while it is written, so
   what the device gets is what was checked. */
static int write_run(struct buf *b, u32 pass, struct tally *t, u32 max, u32 *pfirst, u32 *pn) {
    struct bdev *dev = b->dev;
    struct buf *run[BCACHE_RUN];
    u32 gen[BCACHE_RUN], first = b->lba, n = 0, i;
    u_long f;
    int r;

    if(pn) *pn = 0;
    if(!dev || !b->valid || !b->dirty || b->writing) return 0;   /* 10/16/26 (B8): nothing to write: no empty request */
    if(b->copying || b->aside) return RUN_BLOCKED;
    if(!dev->gone && (b->refs || !deps_out(b))) return RUN_BLOCKED;
    while(first > 0 && b->lba - first < max - 1 && runs_with(find(dev, first - 1), b, pass)) first--;
    for(i = first; n < max; i++) {
        struct buf *p = i == b->lba ? b : find(dev, i);
        if(p != b && !runs_with(p, b, pass)) break;
        run[n++] = p;
    }
    if(dev->gone) {
        f = irq_save();
        for(i = 0; i < n; i++) mark_clean(run[i]);
        irq_restore(f);
        for(i = 0; i < n; i++) if(!run[i]->refs) drop(run[i]);
        dev->bc_said = false;
        return BDEV_EGONE;
    }
    if(pfirst) *pfirst = first;
    if(pn) *pn = n;
    for(i = 0; i < n; i++) {
        run[i]->writing = true;
        run[i]->next_seq = 0;
        gen[i] = run[i]->gen;
    }
    inflight++;
    inside();                                   /* 10/17/26 (B39): the run marked writing while the device works */
    mutex_unlock(&bcache_lock);
    if(n == 1) r = bdev_write(dev, first, 1, run[0]->data);
    else {
        mutex_lock(&bounce_lock);
        for(i = 0; i < n; i++) memcpy(bounce + i * 512, run[i]->data, 512);
        r = bdev_write(dev, first, n, bounce);
        mutex_unlock(&bounce_lock);
    }
    mutex_lock(&bcache_lock);
    outside();
    inflight--;
    io.writes++;
    f = irq_save();                             /* against a bdirty() between the test and the clearing */
    for(i = 0; i < n; i++) {
        run[i]->writing = false;
        if(dev->gone || (r == 0 && run[i]->gen == gen[i])) mark_clean(run[i]);
        else if(r == 0) {                       /* written, and changed meanwhile: dirty with what came after */
            mark_failed(run[i], false);
            move_first(run[i], run[i]->next_seq);
        } else {
            mark_failed(run[i], true);          /* kept dirty for the next sync (B1) */
            run[i]->fail_pass = pass;
        }
    }
    irq_restore(f);
    for(i = 0; i < n; i++) if(dev->gone && !run[i]->refs) drop(run[i]);
    for(i = 0; i < n; i++) if(run[i]->discard) discard_now(run[i]);   /* B35: nobody's: its write is over */
    if(r == 0) { writes += n; io.written_blocks += n; }
    tally(t, dev, r);
    tell(dev, r, first, n);
    wake_up_all(&bwait);
    return r;
}

/* may reuse() take b in its call `call`: nobody holds or moves it, and if it is dirty it can be
   written now. 10/16/26 (B6): a dirty block of a device that refuses writes (one of its blocks
   failed and waits for a sync) is held back with it: making room tries no write that is bound
   to fail, and the device keeps only its share of the cache (room()) */
static bool free_now(const struct buf *b, u32 call) {
    if(b->refs || b->loading || b->writing || b->copying || b->aside) return false;
    return !b->dirty || !(b->wfail || b->dev->bc_failed || b->skip == call);
}

/* a buffer nobody holds or moves, clean and forgotten; NULL when every one is held or waits
   for a sync after a failed write. May drop the lock (to write a dirty block out, or to wait
   for a request to finish). 10/16/26 (B6): no failed run is tried again here: a sync does
   that, and room() keeps a refusing device from filling the cache. */
static struct buf *reuse(void) {
    u32 call = ++reuse_calls ? reuse_calls : ++reuse_calls;
    for(;;) {
        struct buf *b;
        for(b = oldest; b; b = b->newer) if(free_now(b, call)) break;
        if(!b) {
            if(!inflight) return NULL;
            sleep_unlocked();
            continue;
        }
        if(b->valid && b->dirty) {
            u32 lba = b->lba;
            struct bdev *dev = b->dev;
            /* written; or it failed, or (B6) its device failed below it: held back with the device;
               10/16/26 (B7): or not writable yet (its order, or a holder): the next candidate, in this call */
            if(evict_write(b) == RUN_BLOCKED && b->dev == dev && b->lba == lba) b->skip = call;
            continue;
        }
        drop(b);
        return b;
    }
}

/* the held buffer of a block: cached, or read (with up to ahead - 1 following blocks when it
   was not), or (fill false) zeroed for the caller to overwrite */
static struct buf *getblk(struct bdev *dev, u32 lba, bool fill, u32 ahead) {
    struct buf *b, *ra[BCACHE_RUN], *scan;
    u32 n = 0, i;
    int r;

    if(!dev || dev->gone) return NULL;
    lock();
    if(!fill && room(dev) != 0) {                /* 10/16/26 (B6): a write to a device that refuses, past its share */
        unlock();
        return NULL;
    }
    for(;;) {
        b = find(dev, lba);
        if(b && (b->loading || b->writing)) { sleep_unlocked(); continue; }   /* being read, or (B7) written: wait for it */
        if(b) {
            hits++;
            b->refs++;
            lru_newest(b);
            unlock();
            return b;
        }
        b = reuse();
        if(!b) { unlock(); kprintf("bcache: every buffer is busy\n"); return NULL; }
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
        unlock();
        return b;
    }
    b->loading = true;
    ra[n++] = b;
    scan = oldest;
    while(n < ahead && n < BCACHE_RUN && lba + n < dev->nblocks && !find(dev, lba + n)) {   /* read-ahead */
        struct buf *v;
        /* only buffers that are free without a write; 10/16/26 (B6): past the dirty ones at the old end
           (a refusing device's, held back), one look over the list for the whole read-ahead */
        for(v = scan; v; v = v->newer) if(!v->refs && !v->loading && !v->writing && !v->copying && !v->dirty) break;
        if(!v) break;
        scan = v->newer;
        drop(v);
        v->dev = dev;
        v->lba = lba + n;
        v->loading = true;
        hash_in(v);
        lru_newest(v);
        ra[n++] = v;
    }
    inflight++;
    inside();                                   /* 10/17/26 (B39): blocks marked loading while the device works */
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
    outside();
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
    for(i = 1; i < n; i++) if(ra[i]->discard && ra[i]->valid) discard_now(ra[i]);   /* 10/17/26 (B40): binvalidate() meanwhile */
    wake_up_all(&bwait);
    unlock();
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

/* the block if it is cached; 10/16/26 (B19): also while it is being written (readers only) */
struct buf *bpeek(struct bdev *dev, u32 lba) {
    struct buf *b;
    if(!dev || dev->gone) return NULL;
    lock();
    b = find(dev, lba);
    if(b && b->valid) {
        hits++;
        b->refs++;
        lru_newest(b);
    } else b = NULL;
    unlock();
    return b;
}

void bdirty(struct buf *b) {
    bdirty_order(b, BORDER_DATA);
}

/* 10/16/26 (B12): each change gets the next number; a block made dirty records it as its first unwritten
   change, every change as its last; one changed while a write or a copy of it is under way records the
   first such change too (next_seq: what is left dirty when that write is done) */
void bdirty_order(struct buf *b, u8 order) {
    u_long f = irq_save();
    u64 n = ++changes;
    if(b->discard) {                            /* 10/16/26 (B35): a change: the block has an owner again */
        b->discard = false;
        if(b->dirty && b->dev) {                /* 10/17/26 (B45): its bytes were nobody's (bdiscard()): its dirt starts */
            if(b->aside) b->dev->bc_aside--;    /* again with this change, of this order, and it is set aside no more */
            else dlist_out(b);                  /* (it stayed so, and was never written: the new owner's change was lost) */
            b->dev->bc_dirty[b->order]--;
            if(b->wfail) b->dev->bc_failed--;
        }
        b->dirty = b->wfail = b->aside = false;
    }
    if(order > BORDER_MAX) order = BORDER_MAX;
    if(!b->dirty) {
        b->order = order;
        b->first_seq = n;
        b->dirty = true;
        b->wfail = false;
        if(b->dev) {
            b->dev->bc_dirty[order]++;
            if(b->aside) b->dev->bc_aside++;
            else dlist_in(b);
        }
    } else if(order < b->order) {               /* the lower of two orders (B3) */
        if(b->dev) {
            if(!b->aside) dlist_out(b);
            b->dev->bc_dirty[b->order]--;
            b->order = order;
            b->dev->bc_dirty[order]++;
            if(!b->aside) dlist_in(b);
        } else b->order = order;
    }
    if((b->writing || b->copying) && !b->next_seq) b->next_seq = n;
    b->last_seq = n;
    b->gen++;
    irq_restore(f);
}

/* reuse()'s write of a dirty block (B3; 10/16/26, B12): first the dirty blocks of a lower order with a
   change older than b's last one, the oldest first (each with its run), then b. RUN_BLOCKED at the first
   one that cannot go now (a holder, a write of another task, its own order): nothing waits here (the task
   making room may hold it) and nothing walks the cache. When one of them failed before, b is held back
   with its device (B6: -1, untried, not marked failed). 10/16/26 (B8): each write lets the lock go, and b
   may be written, dropped or taken for another block meanwhile: it is written only if it is still that
   dirty block. */
static int evict_write(struct buf *b) {
    struct bdev *dev = b->dev;
    u32 lba = b->lba, guard = 0;
    u8 order = b->order, k;
    u64 last = b->last_seq;
    int r;
    for(k = 0; k < order; k++) {
        struct buf *d;
        while((d = oldest_below(dev, k, last)) != NULL) {
            if(d->wfail) return -1;
            if(d->writing || d->copying || ++guard > BCACHE_BLOCKS) return RUN_BLOCKED;
            r = write_run(d, 0, NULL, BCACHE_RUN, NULL, NULL);
            if(r != 0) return r;
            if(b->dev != dev || b->lba != lba || !b->valid || !b->dirty || b->writing || b->order != order) return 0;
        }
    }
    if(b->dev != dev || b->lba != lba || !b->valid || !b->dirty || b->writing || b->order != order) return 0;  /* look again */
    return write_run(b, 0, NULL, BCACHE_RUN, NULL, NULL);
}

void brelse(struct buf *b) {
    lock();
    if(b->refs) b->refs--;
    if(!b->refs) {
        b->stuck = false;                       /* 10/16/26 (B18): that hold is over */
        b->held_since = 0;
        /* a gone device's block; 10/16/26 (B22): a clean copy taken out of the table while it was held (it may
           hold what its device no longer does: write_blocks()) */
        if(b->dev && (b->dev->gone || (!b->hashed && b->valid && !b->dirty)) && !b->loading && !b->writing && !b->copying)
            drop(b);
        else if(b->discard) discard_now(b);     /* 10/16/26 (B35): nobody's, and let go now */
        wake_up_all(&bwait);                    /* someone may wait for a free buffer */
    }
    unlock();
}

/* (bcache_lock held) the dirty blocks of dev */
static u_int dirty_count(const struct bdev *dev) {
    u_int n = 0, k;
    for(k = 0; k <= BORDER_MAX; k++) n += dev->bc_dirty[k];
    return n;
}

/* (bcache_lock held) 10/16/26 (B14): the block a device that refused is asked to take first: the oldest of
   its lowest order (it waits for nothing of its device), past one that is held or under a write */
static struct buf *probe_block(struct bdev *dev) {
    struct buf *p = NULL;
    u_long f = irq_save();
    u8 k;
    for(k = 0; k <= BORDER_MAX; k++) {
        if(!dev->bc_head[k]) continue;
        for(p = dev->bc_head[k]; p && (p->refs || p->writing || p->copying); p = p->dnext) ;
        break;                                  /* a higher order waits for this one */
    }
    irq_restore(f);
    return p;
}

/* (bcache_lock held) 10/16/26 (B6): may more of dev's blocks be made dirty? 0, or -1 when dev refuses
   writes and already holds its share of the cache (BCACHE_HOLD dirty blocks): its new writes fail at
   once instead of filling the cache that every device shares. 10/16/26 (B14): a device that refused is
   asked first, with one write of the block everything else of it waits for: when that works it is a
   device that works again. A device that works and has that many writes some out first, order by
   order as bsync() does, down to half its share; it waits for no held block (the caller may hold one). */
static int room(struct bdev *dev) {
    u32 pass;
    u8 order;
    int i;
    if(!dev || dev->gone || dirty_count(dev) < BCACHE_HOLD) return 0;
    pass = new_pass();
    if(dev->bc_failed) {
        struct buf *p = probe_block(dev);
        if(!p || write_run(p, pass, NULL, 1, NULL, NULL) != 0) return -1;
    }
    for(order = 0; order <= BORDER_MAX && dirty_count(dev) > BCACHE_HOLD / 2; order++) {
        for(i = 0; i < BCACHE_BLOCKS && dirty_count(dev) > BCACHE_HOLD / 2; i++) {
            struct buf *b = &bufs[i];
            if(b->dev != dev || !b->valid || !b->dirty || b->writing || b->order != order || b->fail_pass == pass) continue;
            if(write_run(b, pass, NULL, BCACHE_RUN, NULL, NULL) < 0) return -1;
        }
    }
    return 0;
}

int bcache_room(struct bdev *dev) {
    int r;
    lock();
    r = room(dev);
    unlock();
    return r;
}

/* (bcache_lock held) 10/16/26 (B6): the blocks of a run that failed in sync call `pass`, each on its own,
   once: one block that refuses must not hold back its neighbours. 0 when every one is written now, -1 when
   one is not: it failed, or (10/16/26, B13) it could not be tried (RUN_BLOCKED is not written, nor is a
   block another task is writing), or its device takes nothing (dead()) */
static int write_singly(struct bdev *dev, u32 first, u32 n, u8 order, u32 pass, struct tally *t) {
    u32 lba;
    int r = 0;
    for(lba = first; lba < first + n; lba++) {
        struct buf *p = find(dev, lba);
        if(!p || !p->valid || !p->dirty || p->order != order || p->fail_pass != pass) continue;   /* written meanwhile */
        if(dead(t, dev) || p->writing || write_run(p, pass, t, 1, NULL, NULL) != 0) r = -1;
    }
    return r;
}

/* (bcache_lock held) 10/16/26 (B12): does a sync of dev (NULL: every device) up to order top, begun at
   change `start`, owe b: a change of it up to `start` is not on its device. 10/16/26 (B26): a block set
   aside (B16) is not the debt of the calling task's sync when that task set it aside (its order point must
   not wait for it); it is the debt of every other task's (it may be put back, still not written). Inline:
   each sync asks it of every buffer, twice (on SurfCPU, which fetches every instruction from DDR, a call
   for each cost about 0.4 million cycles a sync) */
static inline bool owed(const struct buf *b, const struct bdev *dev, u8 top, u64 start) {
    return b->valid && b->dirty && b->order <= top && b->first_seq <= start && (!dev || b->dev == dev) &&
           (!b->aside || aside_other(b));
}

/* (bcache_lock held) 10/16/26 (B12): an owed block that one more try could write: not failed in this call,
   not given up on (B18), on a device that is still asked (B13) */
static bool owed_open(const struct bdev *dev, u8 top, u64 start, u32 pass, const struct tally *t) {
    int i;
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *b = &bufs[i];
        if(owed(b, dev, top, start) && b->fail_pass != pass && !b->stuck && !dead(t, b->dev)) return true;
    }
    return false;
}

/* (bcache_lock held) 10/16/26 (B12): b (order 1 or more), which other tasks keep from being written as it
   is (they keep changing what it needs), written from a copy. The copy is taken while nobody holds b; every
   dirty block of a lower order with a change older than the copy goes first (from a copy of its own when it
   cannot go as it is, one order down), then the copy. b stays open to changes meanwhile (b->copying: only
   other writes of it wait), and dirty afterwards with whatever came after the copy. The copy is a buffer of
   the cache (static memory, as every block a device reads or writes). 0, BDEV_EGONE, or -1: not written (a
   write failed now or before in this call, a holder that does not let go, no buffer for the copy). t: the
   call's tally (B26). 10/16/26 (B33): a block the copy needs written first that another task's operation sets
   aside meanwhile ends the copy: b is let go (that operation's order point may be waiting for it) and looked
   at again, from the start; a block set aside is nobody's dependency (a file system sets one aside only once
   nothing in the cache names it), so the next look does not need it. Below the top (depth), b itself set
   aside so: COPY_ASIDE, for the copy above to do the same. 10/16/26 (B34): B33 held only for the block the copy
   was waiting for: one it needed that was set aside (or dropped) while it wrote or waited for another left the
   lists, the copy never looked for it again, and the copy, which may name it (taken before the file system took
   the name away), reached the disk while that block's older change never did. Now every such block spoils the
   copy (struct copy_rec), and the copy goes only when, under the lock and right before its write, nothing it
   needs is dirty any more and nothing it needed left the lists unwritten; a spoiled copy is taken again. A block
   set aside before the copy was taken is not its dependency (nothing the copy holds names it). At most
   COPY_LOOKS looks: then -1, b not written (the sync says so, and the next sync tries again).
   10/17/26 (B41): no more COPY_LOOKS: rm, truncate and mv traffic alone took every look again and failed syncs of
   a card that took every write. Spoiled now are only the copies of blocks that changed since (struct copy_rec),
   by blocks not set aside before (discard_now()); the copy ends as soon as b no longer has a change older than
   `below` (the call's debt: owed by a sync up to its start, below the top; needed below the copy above): another
   task's write paid it (an rm's order point writes the directory block it changed); and a spoiled copy is taken
   again as long as it takes: every look waits for something or writes something (no spoil comes while the lock is
   held), so it goes once the traffic pauses, and a kill or Ctrl+C ends it (B39). (guard bounds one look.) */
static int write_copy(struct buf *b, u32 pass, struct tally *t, int depth, u64 below) {
    struct bdev *dev = b->dev;
    struct buf *c;
    struct copy_rec rec;
    u32 lba = b->lba, gen, first, n, guard;
    u64 at;
    bool tried = false;
    int r = 0, w;
    u8 k, order;
    u_long f;

again:
    guard = 0;
    if(!(w = wait_for(b, depth > 0))) return -1;
    if(w == WAIT_ASIDE) return COPY_ASIDE;      /* B33 */
    if(b->dev != dev || b->lba != lba || !b->valid || !b->dirty) return 0;   /* written meanwhile */
    if(b->first_seq >= below) return 0;         /* B41: what the call needed of it is on the device (another task's write) */
    if(b->aside || b->fail_pass == pass || dead(t, dev)) return -1;
    if(deps_out(b)) {                           /* it may go as it is after all */
        w = write_run(b, pass, t, BCACHE_RUN, &first, &n);
        if(w != 0 && w != RUN_BLOCKED && n > 1 && !dev->gone) w = write_singly(dev, first, n, b->order, pass, t);
        return w == RUN_BLOCKED ? -1 : w;
    }
    if(depth > BORDER_MAX || !(c = reuse())) return -1;   /* (reuse() may write another block: the lock goes) */
    c->refs = 1;                                /* unhashed and not valid: nobody finds it, nobody takes it */
    c->dev = NULL;
    inside();                                   /* B39: the copy's buffer, b marked copying and the record, until done */
    if(!(w = wait_for(b, depth > 0)) || w == WAIT_ASIDE || b->dev != dev || b->lba != lba || !b->valid || !b->dirty ||
       b->aside) {
        r = w == WAIT_ASIDE ? COPY_ASIDE : b->dev == dev && b->lba == lba && b->valid && b->dirty ? -1 : 0;
        goto done;
    }
    at = b->last_seq;
    gen = b->gen;
    order = b->order;
    memcpy(c->data, b->data, 512);
    b->copying = true;
    b->next_seq = 0;
    rec.b = b;                                  /* B34: under way, spoiled by what it needs leaving the lists */
    rec.at = at;
    rec.gen = gen;                              /* B41: (while b changes after the copy) */
    rec.order = order;
    rec.spoiled = false;
    rec.next = copies;
    copies = &rec;
    for(;;) {
        for(k = 0; k < order && r == 0; k++) {
            struct buf *d;
            while(r == 0 && (d = oldest_below(dev, k, at)) != NULL) {
                if(++guard > 4 * BCACHE_BLOCKS || d->fail_pass == pass || dead(t, dev) || !(w = wait_for(d, true))) { r = -1; break; }
                if(w == WAIT_ASIDE) { r = COPY_AGAIN; break; }   /* B33 */
                if(!d->valid || !d->dirty || d->dev != dev || d->order != k || d->aside) continue;   /* the head again */
                if(d->order > 0 && !deps_out(d)) w = write_copy(d, pass, t, depth + 1, at);
                else {
                    w = write_run(d, pass, t, BCACHE_RUN, &first, &n);
                    if(w != 0 && w != RUN_BLOCKED && n > 1 && !dev->gone) write_singly(dev, first, n, k, pass, t);
                }
                if(w == COPY_ASIDE) r = COPY_AGAIN;              /* B33 */
                else if(w == BDEV_EGONE) r = w;
                else if(w == RUN_BLOCKED && bsleep(0, true)) r = -1;   /* (a hold that came back: look again; B39) */
                else if(w != 0 && d->dev == dev && d->valid && d->dirty && d->first_seq < at) r = -1;
            }
        }
        if(r) break;
        if(b->discard) { r = COPY_DROPPED; break; }          /* B40: b goes (nobody's: bdiscard(); given up: binvalidate()) */
        if(rec.spoiled) { r = COPY_AGAIN; break; }           /* B34: what it needed left the lists unwritten */
        for(k = 0; k < order && !oldest_below(dev, k, at); k++) ;
        if(k == order) break;                               /* nothing it needs is dirty: it goes, now (the lock is held) */
        if(++guard > 4 * BCACHE_BLOCKS) { r = -1; break; }  /* (one came back to the lists: write it first) */
    }
    if(r == 0 && !dev->gone) {
        tried = true;
        inflight++;
        mutex_unlock(&bcache_lock);
        r = bdev_write(dev, lba, 1, c->data);
        mutex_lock(&bcache_lock);
        inflight--;
        io.writes++;
    }
    copy_unlink(&rec);
    f = irq_save();
    b->copying = false;
    if(dev->gone) mark_clean(b);
    else if(tried && r == 0) {
        if(b->gen == gen) mark_clean(b);
        else {                                  /* changed after the copy: dirty with what came after */
            mark_failed(b, false);
            move_first(b, b->next_seq);
        }
    } else if(tried) {
        mark_failed(b, true);
        b->fail_pass = pass;
    }
    irq_restore(f);
    if(dev->gone) {
        if(!b->refs) drop(b);
        r = BDEV_EGONE;
    } else if(b->discard) discard_now(b);       /* B35: its bytes are nobody's now */
    if(tried) {
        if(r == 0) { writes++; io.written_blocks++; }
        tally(t, dev, r);
        tell(dev, r, lba, 1);
    }
done:
    c->refs = 0;
    drop(c);
    outside();
    wake_up_all(&bwait);
    if(r == COPY_AGAIN) {                       /* B33, B34: let go above; look again (B41: as long as it takes) */
        r = 0;
        goto again;
    }
    return r == COPY_DROPPED ? 0 : r;           /* (dropped above: owed no more) */
}

/* order by order (B3; B12: a block once what it may point at is on its device); a device that failed
   has nothing of a higher order that waits for the failed block written. 10/16/26 (B7): a block held by
   another task is waited for (bsync() is never called with a block of its device held). 10/16/26 (B12):
   0 only when every block that was dirty when the call began reached the device. */
int bsync(struct bdev *dev) {
    return bsync_order(dev, BORDER_MAX);
}

/* 10/16/26 (B7): bsync() up to an order: the dirty blocks of orders 0 .. top (an order point that
   needs no more, such as a new cluster's FAT entry before the link to it, leaves the entries be).
   10/16/26 (B12): it owes the blocks of those orders that were dirty when it began; it writes them,
   and whatever else of those orders may go, for SYNC_ROUNDS rounds (B13: blocks that never failed
   first in each order, a failed run again block by block), then the owed blocks other tasks kept from
   it as copies; 0 when every owed block reached its device (written by this call or another task),
   -1 when one did not (a write failed, a device went away, a holder that does not let go: B18).
   10/16/26 (B26): a block another task's operation set aside (B16) is owed as well: waited for as a hold
   is (until it is put back, then written, or dropped), and not written while it is aside */
int bsync_order(struct bdev *dev, u8 top) {
    int i, round;
    u32 pass, first, n;
    u64 start;
    u8 order, sweep;
    bool lost = false, left;
    struct tally t;
    u_long f;
    if(top > BORDER_MAX) top = BORDER_MAX;
    memset(&t, 0, sizeof(t));
    lock();
    pass = new_pass();
    f = irq_save();
    start = changes;
    irq_restore(f);
    for(round = 0; round < SYNC_ROUNDS; round++) {
        for(order = 0; order <= top; order++) {
            if(dev && !dev->bc_dirty[order]) continue;
            for(sweep = 0; sweep < 2; sweep++) {
                if(sweep && dev && !dev->bc_failed) break;   /* no block of it failed: no second look */
                for(i = 0; i < BCACHE_BLOCKS; i++) {
                    struct buf *b = &bufs[i];
                    int w;
                    if(!b->valid || !b->dirty || (b->aside && !aside_other(b)) || b->order != order || (dev && b->dev != dev) ||
                       b->fail_pass == pass || b->wfail != (sweep == 1) || dead(&t, b->dev)) continue;
                    if(owed(b, dev, top, start) && !wait_for(b, false)) continue;      /* B18; B26 */
                    if(!b->valid || !b->dirty || b->aside || b->order != order || (dev && b->dev != dev) ||
                       b->fail_pass == pass || b->writing || b->copying) continue;
                    w = write_run(b, pass, &t, BCACHE_RUN, &first, &n);
                    if(w == BDEV_EGONE) lost = true;
                    else if(w != 0 && w != RUN_BLOCKED && n > 1 && !b->dev->gone) write_singly(b->dev, first, n, order, pass, &t);
                }
            }
        }
        if(!owed_open(dev, top, start, pass, &t)) break;
    }
    if(round == SYNC_ROUNDS && owed_open(dev, top, start, pass, &t)) {
        for(order = 1; order <= top; order++) {
            for(i = 0; i < BCACHE_BLOCKS; i++) {
                struct buf *b = &bufs[i];
                if(!owed(b, dev, top, start) || b->order != order || b->fail_pass == pass || b->stuck || dead(&t, b->dev)) continue;
                if(write_copy(b, pass, &t, 0, start + 1) == BDEV_EGONE) lost = true;
            }
        }
    }
    for(;;) {                                   /* what is owed and still dirty, once writes under way are done */
        bool busy = false;
        left = false;
        for(i = 0; i < BCACHE_BLOCKS && !busy; i++) {
            struct buf *b = &bufs[i];
            if(!owed(b, dev, top, start)) continue;
            if(b->writing || b->copying) busy = true;
            else left = true;
        }
        if(!busy) break;
        if(bsleep(0, true)) {                   /* 10/17/26 (B39): (another task's copy may take long) cut short */
            left = true;
            break;
        }
    }
    unlock();
    return left || lost ? -1 : 0;
}

/* nothing of dev stays in the cache (a umount gave up what could not be written: fs/vfs.c; a mount that failed; mkfs;
   a device gone). 10/17/26 (B40): as bdiscard() does (discard_now()): a dirty block's drop spoils the copies under way
   that needed it (B34; the copy of another task's sync went out over a block it needed that was given up here, never
   written), and a block in someone's hands or under I/O (a copy under way, of a block given up) goes when it is let go
   or its I/O ends (before, it stayed: dirty, it was written after its device's umount) */
void binvalidate(struct bdev *dev) {
    int i;
    bool dropped = false;
    lock();
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *b = &bufs[i];
        if(b->dev == dev && (b->valid || b->hashed) && discard_one(b)) dropped = true;
    }
    if(dropped) wake_up_all(&bwait);            /* (once: a wake-up for each block cost SurfCPU's eject 5 %) */
    if(dev && dev->gone) dev->bc_said = false;
    unlock();
}

u_int bdirty_count(struct bdev *dev) {
    u_int n;
    lock();
    n = dev ? dirty_count(dev) : 0;
    unlock();
    return n;
}

/* 10/16/26 (B16): the blocks of dev whose last write failed (up to max of them, into lbas): a file system
   about to free clusters looks for its own among them. How many there are (10/16/26, B21: all of them,
   also past max: at most BCACHE_BLOCKS) */
u_int bfailed_blocks(struct bdev *dev, u32 *lbas, u_int max) {
    u_int n = 0;
    int i;
    if(!dev) return 0;
    lock();
    for(i = 0; i < BCACHE_BLOCKS && dev->bc_failed; i++) {
        struct buf *b = &bufs[i];
        if(b->dev == dev && b->valid && b->dirty && b->wfail) {
            if(n < max) lbas[n] = b->lba;
            n++;
        }
    }
    unlock();
    return n;
}

/* 10/16/26 (B28): every dirty block of dev, failed or not, set aside or not (up to max of them, into lbas): a
   file system that frees clusters, or cuts a chain off, drops or sets aside its own among them (their bytes are
   nobody's any more). How many there are (at most BCACHE_BLOCKS). From dev's lists of dirty blocks (B12), which
   hold every one that is not set aside: a few blocks, where a look over the cache's 1024 buffers costs SurfCPU
   about 0.1 million cycles (and an rm makes two); over the cache only while some of dev's are set aside */
u_int bdirty_blocks(struct bdev *dev, u32 *lbas, u_int max) {
    u_int n = 0, k, guard = 0;
    int i;
    u_long f;
    if(!dev) return 0;
    lock();
    if(!dev->bc_aside) {
        f = irq_save();                         /* (bdirty() adds to the lists with the interrupts off) */
        for(k = 0; k <= BORDER_MAX; k++) {
            struct buf *b;
            for(b = dev->bc_head[k]; b && guard++ < BCACHE_BLOCKS; b = b->dnext) {
                if(n < max) lbas[n] = b->lba;
                n++;
            }
        }
        irq_restore(f);
    } else for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *b = &bufs[i];
        if(b->dev == dev && b->valid && b->dirty) {
            if(n < max) lbas[n] = b->lba;
            n++;
        }
    }
    unlock();
    return n;
}

/* 10/16/26 (B16): set the dirty block at lba aside (on) or put it back. Aside it is not written, nobody's
   dependency and no debt of the setting task's syncs: its cluster is on its way out (the file system frees
   it behind an order point that must not wait for it). Put back, it is all three again. A block in someone's
   hands or under a write is not set aside. 10/16/26 (B26): the syncs of other tasks owe it all the same
   (it may be put back, not written): they wait for it as for a hold (wait_for()), so the device records the
   task whose operation set it aside */
void baside(struct bdev *dev, u32 lba, bool on) {
    struct buf *b;
    u_long f;
    lock();
    b = find(dev, lba);
    if(b && b->valid && b->dirty && b->aside != on && (!on || (!b->refs && !b->writing && !b->copying))) {
        f = irq_save();
        if(on) {
            spoil_copies(b);                    /* 10/16/26 (B34) */
            dlist_out(b);
            b->aside = true;
            dev->bc_aside++;
            dev->bc_aside_by = (const void *)curTask;
        } else {
            b->aside = false;
            dev->bc_aside--;
            dlist_in(b);
        }
        irq_restore(f);
        b->held_since = 0;                      /* B26: a wait for it (another task's sync) starts again */
        b->stuck = false;
        wake_up_all(&bwait);
    }
    unlock();
}

/* 10/16/26 (B16): the cluster of the block at lba is free: its dirty bytes will never be read, and the
   block is dropped. 10/16/26 (B35): one in someone's hands or under I/O is dropped when let go or when its I/O
   ends (B16 left it, and a write of it that the device refused made every later sync owe it); B34: the copies
   under way that needed it are taken again */
void bdiscard(struct bdev *dev, u32 lba) {
    struct buf *b;
    lock();
    b = find(dev, lba);
    if(b && b->valid && b->dirty) discard_now(b);
    unlock();
}

/* 10/16/26 (B9): how many of dev's counters differ from a count of its blocks (0: none); (B12) and
   whether each list of its dirty blocks holds them, each once, in the order of their first change */
int bcache_audit(struct bdev *dev) {
    u_int dirty[BORDER_MAX + 1], listed[BORDER_MAX + 1], failed = 0, aside = 0;
    int i, off = 0;
    memset(dirty, 0, sizeof(dirty));
    memset(listed, 0, sizeof(listed));
    lock();
    for(i = 0; i < BCACHE_BLOCKS; i++) {
        struct buf *b = &bufs[i];
        if(b->dev != dev || !b->dirty) continue;
        dirty[b->order]++;
        if(b->wfail) failed++;
        if(b->aside) aside++;
        else listed[b->order]++;
    }
    for(i = 0; i <= BORDER_MAX; i++) {
        struct buf *p, *prev = NULL;
        u_int n = 0;
        if(dirty[i] != dev->bc_dirty[i]) off++;
        for(p = dev->bc_head[i]; p && n <= BCACHE_BLOCKS; prev = p, p = p->dnext, n++) {
            if(p->dev != dev || !p->dirty || p->aside || p->order != i || p->dprev != prev ||
               (prev && prev->first_seq > p->first_seq) || p->first_seq > p->last_seq) { off++; break; }
        }
        if(n != listed[i] || dev->bc_tail[i] != prev) off++;
    }
    if(failed != dev->bc_failed) off++;
    if(aside != dev->bc_aside) off++;
    unlock();
    return off;
}

void bcache_stats(u_long *h, u_long *m, u_long *w) {
    *h = hits; *m = misses; *w = writes;
}

void bcache_io_stats(struct bcache_io *out) {
    lock();
    *out = io;
    unlock();
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

/* src NULL: zeros. 10/16/26 (B15): the cached copies of the run's blocks are out of every other write
   and reuse while the device works (marked writing: getblk() waits for them, bpeek()'s readers see what
   they held), take the new contents once the device has them (clean then), and keep what they had when
   it refuses. 10/16/26 (B22): a refused request may have changed some of its blocks on the device all the
   same (an SD card's CMD25 stores the blocks before the one it refuses, USB mass storage its 32 KB pieces,
   ATA its sectors one by one), so the clean copies go (the next read is the device's), and a dirty one keeps
   its bytes (what the file system holds; the next sync writes them over whatever the device has): what is
   read afterwards is what the device holds, and does not change when the cache makes room. */
static int write_blocks(struct bdev *dev, u32 lba, u32 count, const u8 *src) {
    struct buf *copy[BCACHE_RUN];
    while(count) {
        u32 n = count < BCACHE_RUN ? count : BCACHE_RUN, i;
        u_long f;
        int r;
        if(dev->gone) return BDEV_EGONE;
        mutex_lock(&bcache_lock);
        for(i = 0; i < n; i++) {                /* none of them in the middle of a read, a write or a copy */
            struct buf *p = find(dev, lba + i);
            if(p && (p->loading || p->writing || p->copying)) {
                if(bsleep(0, true)) {           /* 10/17/26 (B39): (another task's copy may take long) cut short */
                    mutex_unlock(&bcache_lock);
                    return BDEV_EIO;
                }
                i = (u32)-1;
            }
        }
        for(i = 0; i < n; i++) {                /* then all of them taken at once (nothing waits in between) */
            struct buf *p = find(dev, lba + i);
            copy[i] = p && p->valid ? p : NULL;
            if(copy[i]) copy[i]->writing = true;
        }
        inflight++;
        inside();                               /* 10/17/26 (B39): the run's cached copies marked writing meanwhile */
        mutex_unlock(&bcache_lock);
        mutex_lock(&bounce_lock);
        if(src) memcpy(bounce, src, n * 512);
        else memset(bounce, 0, n * 512);
        r = bdev_write(dev, lba, n, bounce);
        mutex_unlock(&bounce_lock);
        mutex_lock(&bcache_lock);
        outside();
        inflight--;
        io.writes++;
        if(r == 0) io.written_blocks += n;
        for(i = 0; i < n; i++) {
            struct buf *p = copy[i];
            if(!p) continue;
            if(r == 0) {
                if(src) memcpy(p->data, src + i * 512, 512);
                else memset(p->data, 0, 512);
            }
            f = irq_save();
            p->writing = false;
            if(r == 0) {
                p->gen++;
                mark_clean(p);
            }
            irq_restore(f);
            if(r != 0 && !p->dirty) {           /* B22 */
                if(!p->refs) drop(p);
                else hash_out(p);               /* (a reader holds it: found no more, dropped when let go) */
            }
            if(p->discard) discard_now(p);      /* 10/16/26 (B35) */
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
