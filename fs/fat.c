/*
SurfOS FAT File System
--------------------
File: fat.c     Date: 10/3/26 (roadmap F1)
--------------------
FAT12, FAT16 and FAT32 with long file names, read and write: lookup, readdir,
read, write (extending the cluster chain), truncate, create (files and
directories, with VFAT long-name entries when the name is not a plain 8.3
name), unlink and rmdir. Every access goes through the block cache; the
superblock mutex serializes operations on one volume. No FSInfo updates,
no extended partition walking.

10/5/26 (USB storage): rename, and one vnode per object in use (lookups of an
open file share its vnode, so its size stays right and it follows its entry
when renamed; the VFS can tell that it is open). Names are UTF-8: long names
are UTF-16 on the disk, lookups fold ASCII letters only, an 8.3 alias puts '_'
for what it cannot hold, and short names without a long name are read as code
page 437. Once the device is unplugged every operation fails with EIO and the
volume can still be unmounted. statfs with the volume label;
a free-cluster count, from FAT32's FSInfo sector when that can be trusted, else
counted, and written back to FSInfo at sync and unmount; the clean-shutdown bit
in FAT[1]: cleared while mounted read-write, set again at unmount, so macOS and
Windows check a volume that was unplugged while mounted. A read-only mount
writes nothing.

10/16/26 (SurfCPU, milestone 8; audit B3): the blocks it changes carry the order
the block cache writes them in: file data and a new directory's cluster first,
then the FAT copies, then directory entries, then FSInfo. Taking away needs the
reverse, so unlink, truncate to zero and rename put an order point in the middle
(fat_order_point(): a sync of the volume): the entry is gone from the disk before
its clusters are freed, and a renamed file's new entry is on the disk before the
old one goes. A card pulled in the middle then leaves at worst (10/16/26, as B5-B11
below made it) clusters marked used that no entry points at, a name too many, a
free count that is off, the two FATs differing, or a file whose size runs past its
chain (a truncate cut short): fsck.fat finds and fixes them. Not an entry, or a
link on the way, pointing at a free cluster or at bytes not yet written, except in
the two narrow cases B7's note below names (a write that stopped early on a device
that refused after taking part of it, closed by B25; FAT12's entries that straddle two
sectors). A write the device refuses changes nothing on it, and (10/16/26, B15) a write
that went straight to it and was refused changes nothing in the cache either: a write
that returns an error, or fewer bytes than it was given, changed only the bytes it
returned (before, the refused bytes stayed in the cache's copies, read back as the file
and written to the card by the next sync that worked). 10/16/26 (B22): not quite: a
device may store the blocks of a refused request before the one it refused (an SD
card's CMD25, USB mass storage's 32 KB pieces, ATA's sectors), so such a write may also
have changed, on the device, bytes of the file in the request it stopped in (up to 64 KB
past what it returned: a block of them the cache held clean is read from the device
afterwards; one it held dirty keeps the cache's bytes, which the next sync writes over
the device's); the file's size grew by the bytes it returned only (B23). An overwrite in place (append, or a
write into a file's existing bytes) can still leave old and new bytes mixed: FAT32
has no journal (surfcpu/docs/CARD.md, "Writing from SurfOS"). It is an order, not
atomicity: nothing here is power-fail atomic.

10/16/26 (audit B4): a write that stops early leaves no clusters behind. A run of
blocks is allocated ahead of the data, and when the volume ran out in the middle
of one (or a block could not be written) the clusters already linked stayed in the
file's chain past its size: fsck.fat found the chain longer than the file and the
free count low. Now a run ends where the volume does (the write puts in what fits
and returns the bytes it wrote; the next one gets ENOSPC), and after a write that
stopped early the chain is cut back to the file's size.

10/16/26 (audit B5): an order point that fails (the device refuses a write) puts back
what the operation changed so far, in the cache, and the operation fails with EIO:
nothing changed. Before, rename left both names on one cluster chain (and the same mv
again, or an rm of either name, then freed the chain under the other: a file reading
another's bytes), unlink left the entry gone and the clusters lost, truncate to zero
the file empty and its clusters lost. The slots a name takes or gives up are noted
(struct slot_undo: their first bytes) and put back; truncate puts the short entry back.
A cluster a directory grew by stays (empty). An error after the last order point (a
FAT block that cannot be read while clusters are freed) loses clusters, which fsck.fat
finds, but never leaves two names for a file or a name on freed clusters (B17 below: rename
too, which could still fail after the replaced file was freed).

10/16/26 (audit B7): the order within the FAT. The cache writes the sectors of one order
in no order among themselves, so two cases needed one more order point (a sync):
 - a link into a new cluster (prev -> c) that is in another FAT sector than c's own value
   (end of chain), and every link that grows a directory: c's value (and a directory's
   zeros) reach the disk first, so a chain there never runs into a cluster that is free
   there; a new directory (mkdir) is on the disk before anything names it or goes into it,
   so entries written into a directory's new cluster carry the directory order, never the
   data order of its zeros (bdirty_order() keeps the lower of two orders: a block's earlier
   content must not go after what points at it, and FAT now never mixes two in one block);
 - a cut (truncate to a smaller size, and trim_chain()): the end of chain and the new size
   reach the disk before the clusters past it are freed, as unlink does; a truncate that
   cannot do that changes nothing (B5); trim_chain() frees them all the same when the
   device refuses (the links into them reached the disk only if it took writes meanwhile,
   and then a card pulled in the sync that writes the cut and the frees, in the order of
   their FAT sectors, may leave the chain running into freed clusters: the first narrow case;
   since B25 they wait until the cut is on the disk).
The second: a FAT12 entry that straddles two sectors goes out in two blocks and can be cut
in half (a FAT16 or FAT32 entry never straddles).
A file that grows costs a sync of the data and the tables (bsync_order(), not the entries:
fat_table_point()) each time its chain crosses into another FAT sector: every 128
clusters on FAT32, 256 on FAT16 (every 4 MB with 32 KB clusters; every 64 KB with the
512-byte clusters of a small test card); on a fragmented volume, more often.

10/16/26 (audit B11): fat_set() has every FAT copy's block in hand before it changes any,
so both copies change or neither does (a copy whose block could not be read was left
behind, and the FATs differed); trim_chain() lets go of a write's chain only once its head
is free, so a chain it could not free stays the file's for its next write.

10/16/26 (audit B16): a file whose blocks refuse can be removed. The order point that takes
an entry, a cut or a replaced name off the disk writes directory and FAT blocks, which since
the cache's B12 wait for every older block of a lower order: a refused block of the very file
being removed held them back for as long as it refused, so its rm, a truncate past it and a mv
over it said EIO. Now the failed blocks of the clusters about to be freed are set aside before
that order point (fs/bcache.c: not written, nobody's dependency), put back when it fails
(nothing changed) and dropped once the clusters are free; fat_free_chain() drops the failed
blocks of every cluster it frees. Other failed blocks still hold an order point back: while a
block refuses, an operation that needs one (rm, mv, truncate, mkdir, a file growing into another
FAT sector) fails with EIO unless the refusing blocks are those it frees; writes of data go on,
up to the device's share. (B20: a mv over that file fails while they refuse; B21: all of them,
not the first 16; B28: every dirty block of those clusters, failed or not.)

10/16/26 (audit B17): no EIO from an operation that took place, nothing left by one that failed.
rename pointed a moved directory's ".." last, after the old name was gone, and said EIO when that
failed although the rename had happened; and it took the replaced file away (its entry off the
disk, its clusters freed) before deleting the old name, so an old name that could not be deleted
then (its directory's block unreadable) failed the rename with the file it was to replace gone.
Now ".." is pointed first and the old name deleted before the replaced one, each undone when a
later step fails, and nothing fails once the replaced file's clusters go. create read the new
entry back from the directory for its vnode, and a read error there said EIO for a file it had
made: the vnode comes from the entry it wrote (when it cannot be made, ENOMEM, the entry goes).

10/16/26 (audit B19): trim_chain() also let go of a chain whose head's FAT entry could not be
read; it lets go only of a head it sees free, as B11 meant.

10/16/26 (audit B20): rename set the replaced file's refused blocks aside (B16) before its first
order point, which writes the new name, whose directory block may hold the replaced file's entry
too: written then, that entry's size covered the refused bytes on the disk (a card pulled there,
or a rename failing later and leaving it so, showed a file reaching bytes another file once had).
They are set aside for the second order point only, once the replaced entry is deleted (in the
cache), so the first one owes them: a rename over a file whose block refuses fails (EIO) while it
refuses; an rm of that file works, then the rename. unlink and truncate set aside only once the
entry is deleted, emptied or cut back (in the cache), as before.

10/16/26 (audit B21): B16 looked at the device's first 16 failed blocks (an rm of a file with more
said EIO for as long as they refused) and gathered a chain's into an array of 16, checking the
count only between clusters: a chain that runs back into itself (a damaged volume) wrote past the
array. Now every failed block is looked at (a sorted list, kalloc'd), each is set aside once, the
walks end after as many clusters as the volume has, and fat_free_chain() stops at a cluster that
is free already (it counted a looping one free twice).

10/16/26 (audit B23): a write past the end of a file whose gap of zeros the device refused part of
left the file grown by the zeros that were written and said ENOSPC, and one whose gap was written
but none of its bytes left it grown by the gap, with EIO. The gap is all or nothing, and goes with a
write that wrote nothing: the size stays as it was, the error is what stopped it (EIO, or ENOSPC on
a full volume), and the chain is cut back; a truncate that grows likewise.

10/16/26 (audit B24): an undo that could not be done (B5, B17: a block it needs could not be read
back) was best effort, silently: a rename could leave two names on one chain (an rm of either then
freed it under the other). Now every change of the volume fails with EROFS after that, the console
says so once, and the volume stays marked as not cleanly unmounted (fsck.fat before it is written
again).

10/16/26 (audit B25): trim_chain() freed the clusters past its cut also when the device refused the
order point between (B7's first narrow case): the cut and the frees went out in a later sync in the
order of their FAT sectors, and a card pulled between them held a chain running into a freed cluster
(the threaded test found it). They now wait for a sync begun after the cut that works (an order
point, fat_sync(), unmount); their failed blocks are dropped first (past the end of the file; B28: all
their dirty blocks). Of the two narrow cases above, FAT12's straddling entries are left.

10/16/26 (audit B27): an undo puts back only what it saved. A truncate to a smaller size put the short
entry back (B5) also when entry_get() had not read it: 32 bytes of the stack went over the file's
directory entry once the block read again (the file renamed to junk, its clusters lost; zeros there ended
the directory at it, and the names after it went). The saved entry starts zeroed, and goes back only when
update_entry() changed the one in the cache; a rename puts ".." back only when set_dotdot() changed it.

10/16/26 (audit B28): the bytes of a chain nobody will read again are never owed. B16 set aside only the
failed blocks of a chain about to lose its last reference, and B25 dropped only the failed blocks of a part
cut off before its order point: one of it that failed IN that order point stayed while its chain waited to
be freed, and every later order point and sync owed it, EIO for as long as it refused, with no rm to help
(it was in no file's chain any more); a gap of zeros given up (B23), dirty and never tried, fed it, and a
chain freed at once (trim_chain(), a file that had none) left its untried blocks to fail later. Now, failed
or not: the dirty blocks of a part cut off or given back (trim_chain(), a failed mkdir's cluster: B30) are
dropped before its order point, again when that fails, and before each later order point and sync of the
volume while the chain waits to be freed; those of a chain an rm, a truncate or a mv over a file takes away
are set aside for its order point and dropped once it worked (the operation neither writes nor waits for
them: one never tried that refused in it failed the rm once); fat_free_chain() drops those of every cluster
it frees. Exactly: once the operation that gives a chain up has run, no sync owes a dirty block of it, but
for one a task held or was writing at that moment (a reader, another task's sync, the cache making room):
that one is written as any other, an order point that cannot write it fails (an rm then says EIO, nothing
changed, and the next rm works), and one of a waiting chain is dropped by the next order point or sync.

10/16/26 (audit B29): an undo that could not be done makes the volume read-only (B24) only when there was a
change to undo: an update_entry() or set_dotdot() that could not read its block changed nothing, and the
put-back, which would not read it either, is not tried.

10/16/26 (audit B30): a mkdir that fails after something of its name was written frees the new directory's
cluster once the slots put back are on the disk (a chain waiting as in B25, its blocks dropped: B28): another
task's sync may have written the name, and a free reaching the disk first (tables go before entries) left the
name pointing at a free cluster. One that wrote nothing of its name frees it at once, as before.

10/16/26 (audit B31): 64 chains can wait to be freed (B25 had 8, and lost the ninth without a word); one more
is said lost on the console, until the volume is checked, and the unmount says how many clusters are so.

10/16/26 (audit B32): blocks set aside for an order point are aside for that order point only: collected
first, set aside right before it, dropped or put back right after. B16 kept them aside until the chain was
freed; another task's sync owes them (B26) and waits for them, 10 s at most, and a long chain's free outlasted
that (the sync said "an operation that does not end").

10/16/26 (audit B36): B28's "every dirty block of a part given back is dropped" did not cover fat_alloc(): a
folder growing whose new cluster's table point was refused gave the cluster back and left its dirty, failed
zero blocks, and every later order point and sync failed while that sector of a free cluster refused. They
are dropped now; fat_free_chain() and drop_chain() drop each block of the chain when there is no memory for
the list of the device's dirty blocks; a mkdir whose new cluster's block cannot be read frees the cluster. A
block in a task's hands or under I/O then is dropped when let go or when its I/O ends (fs/bcache.c, B35), so
B28's exception remains for an rm's, truncate's or mv's own order point only: a block of its chain that cannot
be set aside then is owed by it (an rm it fails in says EIO, nothing changed, and the next rm works).

10/16/26 (audit B37): a table point drops the waiting chains' blocks first, as order points and syncs do; it
owed one that could not be dropped when given up, and a file crossing into another FAT sector, a folder
growing and mkdir failed while it refused.

10/16/26 (audit B38): the unmount counts the clusters given back that could not be freed before it marks the
volume; with any, the volume stays marked as not cleanly unmounted (as a damaged one, B24), so that the next
mount says so and the next computer checks it. B31 marked it clean right after saying "check the volume".

10/17/26 (audit B39): a task holding a volume's lock is inside the file system (surf_task.fs_hold, fat_lock()):
a kill that waited is put off until it lets go (kernel/task.c), since killed at once it left the lock, the
blocks it held and the cache's state behind; the mount, the unmount and the boot sector's probe and
description count themselves the same way (fat_in()). The cache's waits a kill
cuts short end the call along its error paths (fs/bcache.c), and a long read or write stops between two pieces
(rw_locked(): a short count).

10/17/26 (audit B42): a volume not marked cleanly unmounted when it was mounted stays so at unmount, which says
so: SurfOS's own next mount warned once and its unmount marked the volume clean (B38's lost clusters, B24's
damage still there). Only a checker clears the mark.

10/17/26 (audit B43): clusters a free could not give back (the FAT could not be read or written in the middle of
the chain; fat_alloc()'s give-back) are counted (lose_chain(): as far as the FAT can be read) and said, and keep
the volume marked as not cleanly unmounted; before, nothing counted them and the unmount marked the volume
clean. An rm or a truncate whose change is on the disk says 0 even then: it has happened.

10/17/26 (audit B44): FAT12 has no clean-unmount mark: its unmount says so instead of claiming the volume stays
marked.

10/17/26 (audit B46): the unmount is a step that must finish (fs_finish, surfos/task.h; fs/vfs.c's umount is one as
a whole): Ctrl+C, a kill or a hangup cut its syncs short (B39), and it left a working card marked as not cleanly
unmounted ("write errors") and counted the chains waiting to be freed as lost.

10/17/26 (audit B49): what B39 calls a piece: one block through the cache, up to 64 KB straight between the device
and the caller (rw_locked()); the comments and the documents said 64 KB for both. A straight piece whose wait the
kill cut short says EINTR (bcache's BCACHE_EINTR), not EIO: the device refused nothing.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <fs/fat.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

#define ATTR_RO      0x01
#define ATTR_HIDDEN  0x02
#define ATTR_SYSTEM  0x04
#define ATTR_LABEL   0x08
#define ATTR_DIR     0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN     0x0F
#define NTRES_LOWER_BASE 0x08
#define NTRES_LOWER_EXT  0x10
#define LFN_CHARS 13
#define LFN_MAX_ENTRIES 20          /* 255 characters */
#define NO_IDX 0xFFFFFFFFUL         /* no directory entry */
#define DIR_MAX_ENTRIES 65536       /* the most a FAT directory may hold: walks stop there */
#define FAT_DIRECT 16               /* whole-block runs this long or longer bypass the cache (8 KB) */
#define FAT_READAHEAD BCACHE_RUN    /* blocks a cache miss reads ahead along a file (64 KB) */
#define META_AHEAD 8                /* and in a FAT or a directory (4 KB) */
#define RW_READ  0
#define RW_WRITE 1
#define RW_ZERO  2                  /* write zeros */
#define FREE_UNKNOWN 0xFFFFFFFFUL   /* also what FSInfo says when it does not know */
#define FSINFO_LEAD   0x41615252
#define FSINFO_STRUCT 0x61417272
#define FSINFO_TRAIL  0xAA550000
#define FAT16_CLEAN 0x8000          /* FAT[1]: set = cleanly unmounted */
#define FAT32_CLEAN 0x08000000
#define PENDING_MAX 64              /* 10/16/26 (B25): chains cut off, freed once the cut is on the disk (B31: 8 before) */

struct fat_node;
struct fat_fs;

/* position inside a directory */
struct diter {
    struct fat_fs *fs;
    u32 first;                      /* 0: the fixed FAT12/16 root */
    u32 idx, clus, lba, off;
    bool at_end;
};

struct fat_fs {
    struct superblock *sb;
    struct bdev *dev;
    int type;                       /* 12, 16 or 32 */
    u32 spc, rsv, nfats, root_entries, spf, total;
    u32 fat_lba, root_lba, root_sectors, data_lba, nclusters, root_cluster;
    u32 cluster_bytes, eoc, next_free;
    struct fat_node *live;          /* the objects that have a vnode now (not the root) */
    u32 free;                       /* free clusters; FREE_UNKNOWN until counted */
    u32 fsinfo;                     /* FAT32: the FSInfo sector, 0 when there is no valid one */
    bool fsinfo_dirty;              /* free or next_free changed since FSInfo was written */
    bool marked;                    /* this read-write mount cleared the clean bit */
    char label[12];                 /* "" for none */
    bool damaged;                   /* 10/16/26 (B24): an undo failed: nothing more is changed (EROFS) */
    u32 pending[PENDING_MAX];       /* 10/16/26 (B25): the first clusters of chains trim_chain() cut off while */
    u32 pending_no[PENDING_MAX];    /*      the device refused, each with its number (pend_added then), freed */
    u32 npending, pend_added;       /*      once a sync begun after it worked (the cut is on the disk then) */
    u32 lost_clusters;              /* 10/16/26 (B31): clusters no pending place was left for (unmount says so); 10/17/26
                                       (B43): and those a free could not give back (lose_chain()) */
    bool lost_unsure;               /* 10/17/26 (B43): some of them were counted as far as the FAT could be read: more */
    bool unclean;                   /* 10/17/26 (B42): not marked cleanly unmounted when mounted: it stays so */
};

struct fat_node {
    struct vnode *v;                /* the one vnode of this object while it is in use */
    struct fat_node *next;          /* fs->live */
    u32 first;                      /* first cluster; 0 = empty file, or the FAT12/16 root */
    u32 dir_lba, dir_off;           /* where the short entry lives (not for the root) */
    bool is_root;
    u32 pos_ci, pos_c;              /* the last cluster used: its index in the chain, its number (0: none) */
    bool rd_valid;                  /* readdir resumes at rd_it for entry rd_index */
    u_int rd_index;
    struct diter rd_it;
};

/* one logical directory entry as the iterator returns it */
struct fat_dirent {
    char name[VFS_NAME_MAX + 1];
    u8 raw[32];                     /* the short entry */
    u32 first, size;
    u32 lba, off;                   /* short entry location */
    u32 idx, lfn_idx;               /* entry indexes: the short entry and the first LFN entry */
};


static const struct vnode_ops fat_vops;

/**** little helpers ****/

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void put16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* names compare with ASCII letters folded; any other byte must match exactly */
static bool name_eq(const char *a, const char *b) {
    while(*a && *b) { if(lower(*a) != lower(*b)) return false; a++; b++; }
    return *a == *b;
}

/* 10/17/26 (audit B39): the volume's lock, and the calling task inside the file system while it holds it (surf_task.fs_hold):
   a kill that waited is not escalated before it lets go (kernel/task.c), so that the lock, the blocks it holds and the
   cache's state are not left behind (the cache counts none of its calls for each block: this count covers them). Counted in a critical section (KCRIT_ENTER: no preemption, and a kill that waited
   is put off a tick) so that no kill comes between taking the lock and counting it; a task still waiting for the lock
   holds nothing here (10/18/26, audit B73: what its VFS call holds above, the vnodes of its walk, puts the kill off all
   the same: fs/vfs.c). Let go first, then counted out */
static void fat_lock(struct superblock *sb) {
    KCRIT_ENTER
    mutex_lock(&sb->lock);
    if(curTask) curTask->fs_hold++;
    KCRIT_LEAVE
}
static void fat_unlock(struct superblock *sb) {
    mutex_unlock(&sb->lock);
    if(curTask && curTask->fs_hold) curTask->fs_hold--;
}
/* the same count for what runs without the volume's lock and holds blocks between the cache's calls (mount,
   unmount, the boot sector's probe and description) */
static void fat_in(void) { if(curTask) curTask->fs_hold++; }
static void fat_out(void) { if(curTask && curTask->fs_hold) curTask->fs_hold--; }

/**** UTF-8, UTF-16 and code page 437 ****/

/* the upper half of code page 437, what short names without a long name are read as */
static const u16 cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

/* code point cp as UTF-8 at out; the number of bytes (1 to 4) */
static u32 utf8_put(char *out, u32 cp) {
    if(cp < 0x80) { out[0] = (char)cp; return 1; }
    if(cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if(cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* the code point at *s, which moves past it; -1 for malformed UTF-8 (a stray continuation
   byte, a short sequence, an overlong form, a surrogate, beyond U+10FFFF) */
static i32 utf8_next(const char **s) {
    const u8 *p = (const u8 *)*s;
    u32 c = p[0], cp, need, i, min;
    if(c < 0x80) { *s += 1; return (i32)c; }
    if(c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; min = 0x80; }
    else if(c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; min = 0x800; }
    else if(c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; min = 0x10000; }
    else return -1;
    for(i = 1; i <= need; i++) {
        if((p[i] & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if(cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
    *s += need + 1;
    return (i32)cp;
}

/* a UTF-8 name as the UTF-16 units of a long name: -EINVAL for malformed UTF-8,
   -ENAMETOOLONG beyond 255 units */
static int name_to_utf16(const char *name, u16 *out, u32 *count) {
    u32 n = 0;
    while(*name) {
        i32 cp = utf8_next(&name);
        if(cp < 0) return -EINVAL;
        if(n + (cp >= 0x10000 ? 2 : 1) > 255) return -ENAMETOOLONG;
        if(cp >= 0x10000) {                                  /* a surrogate pair */
            out[n++] = (u16)(0xD800 + ((cp - 0x10000) >> 10));
            out[n++] = (u16)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else out[n++] = (u16)cp;
    }
    *count = n;
    return 0;
}

/* n UTF-16 units as UTF-8 (a lone surrogate becomes U+FFFD); false when it does not fit size */
static bool utf16_to_utf8(const u16 *in, u32 n, char *out, size_t size) {
    size_t o = 0;
    u32 i;
    char tmp[4];
    for(i = 0; i < n; i++) {
        u32 cp = in[i], len;
        if(cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        } else if(cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        len = utf8_put(tmp, cp);
        if(o + len >= size) return false;
        memcpy(out + o, tmp, len);
        o += len;
    }
    out[o] = 0;
    return true;
}

static u32 clus_lba(struct fat_fs *fs, u32 c) {
    return fs->data_lba + (c - 2) * fs->spc;
}

static bool is_eoc(struct fat_fs *fs, u32 v) {
    if(v < 2) return true;                                   /* free or reserved: a broken chain ends here */
    return fs->type == 12 ? v >= 0xFF7 : fs->type == 16 ? v >= 0xFFF7 : v >= 0x0FFFFFF7;
}

/**** time ****/

static i32 days_from_civil(i32 y, u32 m, u32 d) {
    i32 era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (i32)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (i32)d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static u32 fat_to_unix(u16 date, u16 time) {
    u32 y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if(!m || m > 12 || !d) return 0;
    return (u32)days_from_civil((i32)y, m, d) * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}

static void fat_now(u16 *date, u16 *time) {
    time_t t;
    get_time(&t);
    if(t.year < 1980 || t.year > 2107) { *date = (1 << 5) | 1; *time = 0; return; }
    *date = (u16)(((t.year - 1980) << 9) | (t.month << 5) | t.date);
    *time = (u16)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
}

/**** the file allocation table ****/

static int fat_get(struct fat_fs *fs, u32 c, u32 *val) {
    u32 off, lba, o;
    struct buf *b;
    if(c < 2 || c >= fs->nclusters + 2) return -EIO;
    off = fs->type == 12 ? c + c / 2 : fs->type == 16 ? c * 2 : c * 4;
    lba = fs->fat_lba + off / 512;
    o = off % 512;
    b = breada(fs->dev, lba, fs->fat_lba + fs->spf - lba < META_AHEAD ? fs->fat_lba + fs->spf - lba : META_AHEAD);
    if(!b) return -EIO;
    if(fs->type == 12) {
        u32 v = b->data[o];
        if(o == 511) {
            struct buf *b2 = bread(fs->dev, lba + 1);
            if(!b2) { brelse(b); return -EIO; }
            v |= (u32)b2->data[0] << 8;
            brelse(b2);
        } else v |= (u32)b->data[o + 1] << 8;
        *val = (c & 1) ? v >> 4 : v & 0xFFF;
    } else if(fs->type == 16) *val = le16(b->data + o);
    else *val = le32(b->data + o) & 0x0FFFFFFF;
    brelse(b);
    return 0;
}

/* 10/16/26 (B11): every copy's blocks are in hand before any is changed, so both FATs change or
   neither does (a copy whose block could not be read was left behind before: the FATs differed) */
static int fat_set(struct fat_fs *fs, u32 c, u32 val) {
    struct buf *b[2] = { NULL, NULL }, *b2[2] = { NULL, NULL };
    u32 off, o, i;
    int r = 0;
    if(c < 2 || c >= fs->nclusters + 2) return -EIO;
    off = fs->type == 12 ? c + c / 2 : fs->type == 16 ? c * 2 : c * 4;
    o = off % 512;
    for(i = 0; i < fs->nfats && i < 2 && !r; i++) {
        u32 lba = fs->fat_lba + i * fs->spf + off / 512, end = fs->fat_lba + (i + 1) * fs->spf;
        b[i] = breada(fs->dev, lba, end - lba < META_AHEAD ? end - lba : META_AHEAD);
        if(!b[i]) r = -EIO;
        else if(fs->type == 12 && o == 511 && !(b2[i] = bread(fs->dev, lba + 1))) r = -EIO;
    }
    for(i = 0; i < 2; i++) {
        if(!r && b[i]) {
            if(fs->type == 12) {
                u8 *lo = b[i]->data + o, *hi = b2[i] ? b2[i]->data : b[i]->data + o + 1;
                if(c & 1) { *lo = (*lo & 0x0F) | ((val << 4) & 0xF0); *hi = (val >> 4) & 0xFF; }
                else { *lo = val & 0xFF; *hi = (*hi & 0xF0) | ((val >> 8) & 0x0F); }
                if(b2[i]) bdirty_order(b2[i], BORDER_TABLE);
            } else if(fs->type == 16) put16(b[i]->data + o, (u16)val);
            else put32(b[i]->data + o, (le32(b[i]->data + o) & 0xF0000000) | (val & 0x0FFFFFFF));
            bdirty_order(b[i], BORDER_TABLE);
        }
        if(b2[i]) brelse(b2[i]);
        if(b[i]) brelse(b[i]);
    }
    return r;
}

static int zero_cluster(struct fat_fs *fs, u32 c) {
    u32 s;
    for(s = 0; s < fs->spc; s++) {
        struct buf *b = bget(fs->dev, clus_lba(fs, c) + s);
        if(!b) return -EIO;
        memset(b->data, 0, 512);
        bdirty(b);
        brelse(b);
    }
    return 0;
}

static int fat_table_point(struct fat_fs *fs);

/* 10/16/26 (B7): are the FAT entries of clusters a and b in one sector (in each copy)? An entry
   that straddles two sectors (FAT12) counts as not */
static bool same_fat_sector(struct fat_fs *fs, u32 a, u32 b) {
    u32 oa = fs->type == 12 ? a + a / 2 : fs->type == 16 ? a * 2 : a * 4;
    u32 ob = fs->type == 12 ? b + b / 2 : fs->type == 16 ? b * 2 : b * 4;
    u32 len = fs->type == 32 ? 4 : 2;
    return oa / 512 == (oa + len - 1) / 512 && ob / 512 == (ob + len - 1) / 512 && oa / 512 == ob / 512;
}

static void drop_blocks(struct fat_fs *fs, u32 c);
static void lose_chain(struct fat_fs *fs, u32 c);

/* A free cluster, chained after prev (0: a new chain): prev + 1 when that is free, so files
   stay contiguous, else the first free one from the hint on. Only a directory's is zeroed: a
   file's bytes past its end are never read, and a gap is written as zeros (grow_zeros).
   0, -ENOSPC or -EIO. */
static int fat_alloc(struct fat_fs *fs, u32 prev, bool dir, u32 *out) {
    u32 n, c, v;
    bool taken;
    int r;
    if(fs->free == 0) return -ENOSPC;                        /* known to be full */
    if(prev >= 2 && prev + 1 < fs->nclusters + 2) {
        r = fat_get(fs, prev + 1, &v);
        if(r) return r;
        if(!v) { c = prev + 1; goto found; }
    }
    for(n = 0, c = fs->next_free; n < fs->nclusters; n++, c++) {
        if(c < 2 || c >= fs->nclusters + 2) c = 2;
        r = fat_get(fs, c, &v);
        if(r) return r;
        if(!v) goto found;
    }
    fs->free = 0;
    return -ENOSPC;
found:
    r = fat_set(fs, c, fs->eoc);
    taken = r == 0;
    if(r == 0 && dir) r = zero_cluster(fs, c);
    /* 10/16/26 (B7): c's own FAT value (and a directory's zeros) on the disk before the link to it.
       The cache writes FAT sectors in no order among themselves, so a link in another sector could
       reach the disk first, and a chain there would run into a cluster that is free there (the next
       file's to take; for a directory, whose entries it would show). One sync when the link crosses
       a FAT sector, or the chain is a directory's (entries later written into its new cluster then
       carry the directory order, not the order of its zeros) */
    if(r == 0 && prev && (dir || !same_fat_sector(fs, prev, c))) r = fat_table_point(fs);
    if(r == 0 && prev) r = fat_set(fs, prev, c);
    if(r) {
        if(taken && fat_set(fs, c, 0) != 0) lose_chain(fs, c);   /* c is free again (best effort; 10/17/26, B43: */
        drop_blocks(fs, c);                                  /* or lost, and counted) 10/16/26 (B36): its zeros nobody's */
        return r;
    }
    fs->next_free = c + 1;
    if(fs->free != FREE_UNKNOWN && fs->free) fs->free--;
    fs->fsinfo_dirty = true;
    *out = c;
    return 0;
}

static bool free_pending(struct fat_fs *fs, u32 upto);
static void drop_pending(struct fat_fs *fs);

/* 10/16/26 (B3): what this operation changed so far is on the disk before what it changes next
   (the cache writes data, tables, entries in that order; taking away needs entries first).
   10/16/26 (B25): once one works, the chains trim_chain() cut off while the device refused are freed;
   10/16/26 (B28): it owes none of their blocks */
static int fat_order_point(struct fat_fs *fs) {
    if(fs->npending) drop_pending(fs);
    if(bsync(fs->dev)) return -EIO;
    free_pending(fs, fs->pend_added);
    return 0;
}

/* 10/16/26 (B7): an order point for a new cluster: its zeros and its FAT entry (data and tables) on the
   disk before the link to it; the entries wait for the next sync. 10/16/26 (B37): it owes none of the waiting
   chains' blocks either (B28 dropped them before order points and syncs only: a table point owed one that could
   not be dropped then, and a file crossing into another FAT sector or a folder growing failed while it refused) */
static int fat_table_point(struct fat_fs *fs) {
    if(fs->npending) drop_pending(fs);
    return bsync_order(fs->dev, BORDER_TABLE) ? -EIO : 0;
}

/* 10/16/26 (B21; B28): the device's dirty blocks, failed or not, sorted (kalloc'd, *n of them, and n bytes after
   them for aside_collect()'s marks); NULL when it has none, or there is no memory for the list (then nothing is set
   aside or dropped: an order point writes them and waits for them, EIO while one refuses, as before B16) */
static u32 *dirty_list(struct fat_fs *fs, u32 *n) {
    u32 want = bdirty_blocks(fs->dev, NULL, 0), got, gap, i, j, v, *l;
    *n = 0;
    while(want) {
        if(!(l = (u32 *)kalloc(want * (sizeof(u32) + 1)))) return NULL;
        got = bdirty_blocks(fs->dev, l, want);
        if(got > want) { kfree(l); want = got; continue; }  /* more meanwhile (another task's writes) */
        if(!got) { kfree(l); return NULL; }
        for(gap = got / 2; gap; gap /= 2) {                  /* a shell sort: BCACHE_BLOCKS of them at most */
            for(i = gap; i < got; i++) {
                for(v = l[i], j = i; j >= gap && l[j - gap] > v; j -= gap) l[j] = l[j - gap];
                l[j] = v;
            }
        }
        *n = got;
        return l;
    }
    return NULL;
}

/* where the first of the n sorted blocks of l that is lo or more is */
static u32 first_at(const u32 *l, u32 n, u32 lo) {
    u32 a = 0, b = n, m;
    while(a < b) {
        m = a + (b - a) / 2;
        if(l[m] < lo) a = m + 1;
        else b = m;
    }
    return a;
}

/* 10/16/26 (B16; B28): the dirty blocks of cluster c (of the n in l) are dropped, failed or not: the cluster is
   free, or nothing names it any more, so their bytes will never be read. Kept, a failed one would hold the
   device's later blocks back as a refusing one (bcache.c, B12-B14), and one never tried would be written to a
   cluster nobody has, and could fail there */
static void drop_cluster(struct fat_fs *fs, u32 c, const u32 *l, u32 n) {
    u32 lo = clus_lba(fs, c), i;
    for(i = first_at(l, n, lo); i < n && l[i] < lo + fs->spc; i++) bdiscard(fs->dev, l[i]);
}

/* 10/16/26 (B36): every block of cluster c is dropped where the cache has it dirty (bdiscard()): a cluster given
   back with no list of the device's dirty blocks to look in (fat_alloc()'s, one cluster; or no memory for the list) */
static void drop_blocks(struct fat_fs *fs, u32 c) {
    u32 s;
    if(c < 2 || c >= fs->nclusters + 2) return;
    for(s = 0; s < fs->spc; s++) bdiscard(fs->dev, clus_lba(fs, c) + s);
}

/* 10/17/26 (audit B43): the clusters of the chain from c on that a free could not give back (the FAT could not be
   read or written there) and that nothing names any more: allocated for good, lost until the volume is checked.
   Counted (fs->lost_clusters: the volume stays marked as not cleanly unmounted, B38) and said; c itself at least, the
   rest as far as the chain can be read (fs->lost_unsure when it could not be read to its end). Before, they were
   neither counted nor said: the unmount marked the volume clean, and the next mount trusted it */
static void lose_chain(struct fat_fs *fs, u32 c) {
    u32 n = 0, next;
    bool whole = false;
    while(c >= 2 && c < fs->nclusters + 2 && n < fs->nclusters) {
        n++;
        if(fat_get(fs, c, &next) != 0) break;
        if(!next || is_eoc(fs, next)) { whole = true; break; }
        c = next;
    }
    if(!n) return;
    fs->lost_clusters += n;
    if(!whole) fs->lost_unsure = true;
    kprintf("+FAT%i: %s: %s%lu cluster%s given back could not be freed (the FAT could not be read or written there):"
            " lost until the volume is checked (fsck.fat)\n", fs->type, fs->dev->name, whole ? "" : "at least ",
            (u_long)n, n == 1 ? "" : "s");
}

/* the clusters of the chain from c on are free. 10/16/26 (B21): a chain that runs back into itself (a
   damaged volume) ends at the first cluster that is free already: each is counted once. 10/16/26 (B28): the
   dirty blocks of every cluster it frees are dropped, failed or not (B16 dropped the failed ones only).
   10/17/26 (B43): what a failure leaves allocated is lost (lose_chain()): nothing names the chain any more, but for
   a caller that keeps a chain whose head it could not free (kept: trim_chain()'s file, for its next write) */
static int fat_free_chain(struct fat_fs *fs, u32 c, bool kept) {
    u32 n = 0, next, nl, *l = dirty_list(fs, &nl);
    bool each = !l && bdirty_count(fs->dev);                 /* 10/16/26 (B36): no memory for the list */
    int r = 0;
    while(!is_eoc(fs, c) && n++ < fs->nclusters) {
        if(fat_get(fs, c, &next) != 0) { r = -EIO; break; }
        if(!next) break;                                     /* free already */
        if(fat_set(fs, c, 0) != 0) { r = -EIO; break; }
        if(nl) drop_cluster(fs, c, l, nl);
        else if(each) drop_blocks(fs, c);
        if(c < fs->next_free) fs->next_free = c;
        if(fs->free != FREE_UNKNOWN) fs->free++;
        fs->fsinfo_dirty = true;
        c = next;
    }
    kfree(l);
    if(r && (n > 1 || !kept)) lose_chain(fs, c);            /* (n 1: it failed at the head) */
    return r;
}

/* 10/16/26 (B28): the dirty blocks of the chain from c on are dropped: it is cut off past the end of its file
   (or given up whole, as a gap of zeros that did not get its bytes: B23), so its bytes will never be read. The
   walk ends once every dirty block of the device's data area was looked at, or after as many clusters as the
   volume has */
static void drop_chain(struct fat_fs *fs, u32 c) {
    u32 nl, steps = 0, next, left, i, *l = dirty_list(fs, &nl);
    u8 *done = l ? (u8 *)(l + nl) : NULL;
    if(!l && bdirty_count(fs->dev)) {                        /* 10/16/26 (B36): no memory for the list: each block */
        while(c >= 2 && c < fs->nclusters + 2 && steps++ < fs->nclusters) {
            drop_blocks(fs, c);
            if(fat_get(fs, c, &next) != 0 || is_eoc(fs, next)) break;
            c = next;
        }
        return;
    }
    if(done) memset(done, 0, nl);
    left = nl - first_at(l, nl, fs->data_lba);
    while(left && c >= 2 && c < fs->nclusters + 2 && steps++ < fs->nclusters) {
        u32 lo = clus_lba(fs, c);
        for(i = first_at(l, nl, lo); i < nl && l[i] < lo + fs->spc; i++) {
            if(done[i]) continue;                            /* (a chain that runs back into itself) */
            done[i] = 1;
            left--;
            bdiscard(fs->dev, l[i]);
        }
        if(fat_get(fs, c, &next) != 0 || is_eoc(fs, next)) break;
        c = next;
    }
    kfree(l);
}

/* 10/16/26 (B25): the chains trim_chain() cut off while the device refused its order point, those added
   up to number upto, are freed: a sync begun after they were worked, so their cuts are on the disk and no
   chain there runs into them any more. Whether one was (sb->lock) */
static bool free_pending(struct fat_fs *fs, u32 upto) {
    u32 i, k = 0;
    bool freed = false;
    for(i = 0; i < fs->npending; i++) {
        if((i32)(fs->pending_no[i] - upto) <= 0 && !fs->damaged) {
            fat_free_chain(fs, fs->pending[i], false);
            freed = true;
        } else {
            fs->pending[k] = fs->pending[i];
            fs->pending_no[k++] = fs->pending_no[i];
        }
    }
    fs->npending = k;
    return freed;
}

/* 10/16/26 (B28): the dirty blocks of the chains waiting to be freed are dropped (their bytes are nobody's),
   before each order point and sync of the volume. Those who give a chain up drop its blocks (drop_chain());
   this takes one a task held or was writing then (a block under I/O stays: bdiscard()), which a sync would
   owe, and a refusing device would fail every order point with it, the chain never freed (sb->lock) */
static void drop_pending(struct fat_fs *fs) {
    u32 i;
    for(i = 0; i < fs->npending; i++) drop_chain(fs, fs->pending[i]);
}

/* how many clusters the chain from c on has (as far as the volume has clusters) */
static u32 chain_length(struct fat_fs *fs, u32 c) {
    u32 n = 0, next;
    while(c >= 2 && c < fs->nclusters + 2 && n < fs->nclusters) {
        n++;
        if(fat_get(fs, c, &next) != 0 || is_eoc(fs, next)) break;
        c = next;
    }
    return n;
}

/* 10/16/26 (B25; B30, B31): the chain from c on, which nothing names in the cache any more, is freed once a sync
   that begins after now has worked (what took its last reference away is on the disk then). Its dirty blocks
   are the caller's to drop (drop_chain()). With PENDING_MAX chains waiting already, it stays allocated with
   nothing pointing at it until the volume is checked: said on the console, and again at unmount (B25 lost the
   ninth without a word) */
static void pend_chain(struct fat_fs *fs, u32 c) {
    u32 n;
    if(fs->npending < PENDING_MAX) {
        fs->pending[fs->npending] = c;
        fs->pending_no[fs->npending++] = ++fs->pend_added;
        return;
    }
    n = chain_length(fs, c);
    fs->lost_clusters += n;
    kprintf("+FAT%i: %s: %lu cluster%s given back cannot wait to be freed (%u wait already): lost until the volume"
            " is checked (fsck.fat)\n", fs->type, fs->dev->name, (u_long)n, n == 1 ? "" : "s", PENDING_MAX);
}

/* 10/16/26 (B5): a directory slot's first byte before a change, to put back when the change cannot
   go on (the first byte says whether a slot is in use: 0x00 or 0xE5 frees it again) */
struct slot_undo {
    u32 lba;
    u16 off;
    u8 first;
};
#define UNDO_MAX (LFN_MAX_ENTRIES + 1)

/* 10/16/26 (B16; B21; B28, B32): the dirty blocks of a chain about to lose its last reference on the disk,
   set aside for the order point that takes that reference away (aside_collect(), aside_begin(), aside_end()) */
struct aside {
    u32 *lba;                       /* the device's dirty blocks, sorted (dirty_list()); NULL: none */
    u8 *set;                        /* (after them) 1 for each one of the chain's, 2 for the operation's own */
    u32 n;
};

/* 10/16/26 (B28): the directory blocks the operation itself changed (the slots of its entries, a file's entry
   updated): they carry what its order point must write, so none is set aside, and the walk does not look for
   them along the chain. On a FAT32 root or in a folder they are in the data area, and every rm walked its file's
   whole chain once more for them (SurfCPU: half a million cycles for a few thousand clusters) */
struct keep {
    u32 lba[3 * UNDO_MAX + 1];
    u32 n;
};

static void keep_lba(struct keep *k, u32 lba) {
    if(k->n < sizeof(k->lba) / sizeof(k->lba[0])) k->lba[k->n++] = lba;
}

static void keep_undo(struct keep *k, const struct slot_undo *u, u32 n) {
    u32 i;
    for(i = 0; i < n; i++) keep_lba(k, u[i].lba);
}

/* 10/16/26 (B16): before an order point that takes the last reference to a chain off the disk (an entry
   deleted or emptied, a cut), the blocks of the chain from cluster c on whose writes failed are set aside:
   they are older than the entry or cut, so the order point would wait for them for as long as the device
   refuses them, and once the chain is free their bytes will never be read. aside_end() puts them back when
   the order point fails (nothing changed) or drops them once the chain is free. 10/16/26 (B20): only once
   nothing in the cache names them any more (the entry deleted, emptied or cut back): what the order point
   writes then covers none of them. 10/16/26 (B21): every block of the device is looked at, each is set aside
   once, and the walk ends after as many clusters as the volume has. 10/16/26 (B28): every dirty block of the
   chain, failed or not: the order point neither waits for nor writes a byte of the chain it takes away (one
   never tried that refused in it failed the operation, once). 10/16/26 (B32): collected here, with nothing set
   aside yet; aside_begin() sets them aside right before the order point and aside_end() ends it right after
   (B16 set them aside during the walk and kept them so until the chain was freed: another task's sync waits
   for a block set aside, 10 s at most, and a long chain's walk and free could take longer). The walk only when
   the device has dirty blocks, and only until each of its data area's is placed, but for the operation's own
   directory blocks (k: never set aside) (sb->lock) */
static void aside_collect(struct fat_fs *fs, u32 c, struct aside *a, const struct keep *k) {
    u32 steps = 0, left, next, i, j;
    a->set = NULL;
    a->lba = dirty_list(fs, &a->n);
    if(!a->lba) return;
    a->set = (u8 *)(a->lba + a->n);
    memset(a->set, 0, a->n);
    left = a->n - first_at(a->lba, a->n, fs->data_lba);
    for(j = 0; k && j < k->n; j++) {
        i = first_at(a->lba, a->n, k->lba[j]);
        if(i == a->n || a->lba[i] != k->lba[j] || a->set[i]) continue;
        a->set[i] = 2;
        if(a->lba[i] >= fs->data_lba) left--;
    }
    while(left && c >= 2 && c < fs->nclusters + 2 && steps++ < fs->nclusters) {
        u32 lo = clus_lba(fs, c);
        for(i = first_at(a->lba, a->n, lo); i < a->n && a->lba[i] < lo + fs->spc; i++) {
            if(a->set[i]) continue;
            a->set[i] = 1;
            left--;
        }
        if(fat_get(fs, c, &next) != 0 || is_eoc(fs, next)) break;
        c = next;
    }
}

static void aside_begin(struct fat_fs *fs, struct aside *a) {
    u32 i;
    for(i = 0; i < a->n; i++) if(a->set[i] == 1) baside(fs->dev, a->lba[i], true);
}

/* the order point is over: worked (done: nothing names the chain any more, its clusters are freed next) and
   the blocks are dropped, or failed and they are put back */
static void aside_end(struct fat_fs *fs, struct aside *a, bool done) {
    u32 i;
    for(i = 0; i < a->n; i++) {
        if(a->set[i] != 1) continue;
        if(done) bdiscard(fs->dev, a->lba[i]);
        else baside(fs->dev, a->lba[i], false);
    }
    kfree(a->lba);
    a->lba = NULL;
    a->set = NULL;
    a->n = 0;
}

/* the free clusters, counted in one pass over the first FAT, read 64 KB at a time */
static int count_free(struct fat_fs *fs) {
    u32 c, v, n = 0, first = 0;
    if(fs->type == 12) {                                     /* entries straddle sectors; the FAT is small */
        for(c = 2; c < fs->nclusters + 2; c++) {
            if(fat_get(fs, c, &v) != 0) return -EIO;
            if(!v) { n++; if(!first) first = c; }
        }
    } else {
        u32 per = fs->type == 16 ? 256 : 128, nsec = (fs->nclusters + 2 + per - 1) / per, sec, k, i;
        u8 *chunk = (u8 *)kalloc(BCACHE_RUN * 512);
        if(!chunk) return -ENOMEM;
        for(sec = 0; sec < nsec; sec += k) {
            k = nsec - sec < BCACHE_RUN ? nsec - sec : BCACHE_RUN;
            if(bread_blocks(fs->dev, fs->fat_lba + sec, k, chunk) != 0) { kfree(chunk); return -EIO; }
            for(i = 0; i < k * per; i++) {
                c = sec * per + i;
                if(c < 2) continue;
                if(c >= fs->nclusters + 2) break;
                v = fs->type == 16 ? le16(chunk + i * 2) : le32(chunk + i * 4) & 0x0FFFFFFF;
                if(!v) { n++; if(!first) first = c; }
            }
        }
        kfree(chunk);
    }
    fs->free = n;
    if(first) fs->next_free = first;
    return 0;
}

/* FAT[1]'s clean-shutdown bit, in every copy of the FAT (FAT12 has none) */
static int fat_mark(struct fat_fs *fs, bool clean) {
    u32 i;
    if(fs->type == 12) return 0;
    for(i = 0; i < fs->nfats; i++) {
        struct buf *b = bread(fs->dev, fs->fat_lba + i * fs->spf);
        if(!b) return -EIO;
        if(fs->type == 32) {
            u32 v = le32(b->data + 4);
            put32(b->data + 4, clean ? v | FAT32_CLEAN : v & ~FAT32_CLEAN);
        } else {
            u16 v = le16(b->data + 2);
            put16(b->data + 2, (u16)(clean ? v | FAT16_CLEAN : v & ~FAT16_CLEAN));
        }
        bdirty_order(b, BORDER_TABLE);
        brelse(b);
    }
    return 0;
}

/* the free count and the next-free hint back into FSInfo (FAT32) */
static int write_fsinfo(struct fat_fs *fs) {
    struct buf *b;
    if(!fs->fsinfo || !fs->fsinfo_dirty) return 0;
    b = bread(fs->dev, fs->fsinfo);
    if(!b) return -EIO;
    if(le32(b->data) == FSINFO_LEAD && le32(b->data + 484) == FSINFO_STRUCT) {
        put32(b->data + 488, fs->free);
        put32(b->data + 492, fs->next_free >= 2 && fs->next_free < fs->nclusters + 2 ? fs->next_free : 2);
        bdirty_order(b, BORDER_SUMMARY);
    }
    brelse(b);
    fs->fsinfo_dirty = false;
    return 0;
}

/**** directory iteration ****/

static void diter_init(struct diter *it, struct fat_fs *fs, struct fat_node *dir) {
    it->fs = fs;
    it->idx = 0;
    it->off = 0;
    if(dir->is_root && fs->type != 32) {                     /* the fixed FAT12/16 root */
        it->first = it->clus = 0;
        it->lba = fs->root_lba;
        it->at_end = fs->root_sectors == 0;
    } else {                                                 /* a cluster chain; a broken entry is empty */
        it->first = it->clus = dir->first;
        it->at_end = dir->first < 2 || dir->first >= fs->nclusters + 2;
        it->lba = it->at_end ? 0 : clus_lba(fs, dir->first);
    }
}

/* step to the next entry; at_end becomes true when the directory's space is used up */
static int diter_next(struct diter *it) {
    struct fat_fs *fs = it->fs;
    it->idx++;
    if(it->idx >= DIR_MAX_ENTRIES) { it->at_end = true; return 0; }   /* a looping chain ends here too */
    it->off += 32;
    if(it->off < 512) return 0;
    it->off = 0;
    it->lba++;
    if(!it->first) {
        if(it->lba >= fs->root_lba + fs->root_sectors) it->at_end = true;
    } else if(it->lba - clus_lba(fs, it->clus) >= fs->spc) {
        u32 next;
        int r = fat_get(fs, it->clus, &next);
        if(r) return r;
        if(is_eoc(fs, next)) { it->lba--; it->at_end = true; }   /* stay on the last valid sector */
        else { it->clus = next; it->lba = clus_lba(fs, next); }
    }
    return 0;
}

/* give the directory one more (zeroed) cluster and point the iterator at its first entry */
static int diter_extend(struct diter *it) {
    struct fat_fs *fs = it->fs;
    u32 c;
    int r;
    if(!it->first || it->idx >= DIR_MAX_ENTRIES) return -ENOSPC;    /* the FAT12/16 root cannot grow */
    r = fat_alloc(fs, it->clus, true, &c);
    if(r) return r;
    it->clus = c;
    it->lba = clus_lba(fs, c);
    it->off = 0;
    it->at_end = false;
    return 0;
}

static u8 lfn_checksum(const u8 *short_name) {
    u8 sum = 0;
    int i;
    for(i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

/* the 8.3 name as UTF-8 (code page 437 above 0x7F; NT's lower-case flags honoured) */
static void short_to_name(const u8 *raw, char *out) {
    u32 n = 0;
    int i, base = 8, ext = 3;
    while(base && raw[base - 1] == ' ') base--;
    while(ext && raw[8 + ext - 1] == ' ') ext--;
    for(i = 0; i < base; i++) {
        u8 c = (i == 0 && raw[0] == 0x05) ? 0xE5 : raw[i];  /* 0x05 stands for a leading 0xE5 */
        if(raw[12] & NTRES_LOWER_BASE) c = (u8)lower(c);
        n += utf8_put(out + n, c < 0x80 ? c : cp437[c - 0x80]);
    }
    if(ext) {
        out[n++] = '.';
        for(i = 0; i < ext; i++) {
            u8 c = raw[8 + i];
            if(raw[12] & NTRES_LOWER_EXT) c = (u8)lower(c);
            n += utf8_put(out + n, c < 0x80 ? c : cp437[c - 0x80]);
        }
    }
    out[n] = 0;
}

/* the directory block the iterator is in, read with the rest of its cluster (or root area) */
static struct buf *dir_block(struct diter *it) {
    struct fat_fs *fs = it->fs;
    u32 end = it->first ? clus_lba(fs, it->clus) + fs->spc : fs->root_lba + fs->root_sectors;
    return breada(fs->dev, it->lba, end > it->lba ? (end - it->lba < META_AHEAD ? end - it->lba : META_AHEAD) : 1);
}

/* the next logical entry (long name resolved, free and label entries skipped): 0, 1 at the end, < 0 */
static int dir_read(struct diter *it, struct fat_dirent *e) {
    u16 lfn[LFN_MAX_ENTRIES * LFN_CHARS];
    u32 lfn_start = 0, len;
    int lfn_expected = 0, lfn_seen = 0;
    u8 lfn_sum = 0;

    for(;;) {
        struct buf *b;
        u8 *p;
        int r;
        if(it->at_end) return 1;
        b = dir_block(it);
        if(!b) return -EIO;
        p = b->data + it->off;
        if(p[0] == 0) { brelse(b); it->at_end = true; return 1; }
        if(p[0] == 0xE5) {
            lfn_expected = lfn_seen = 0;
        } else if((p[11] & ATTR_LFN) == ATTR_LFN) {
            int seq = p[0] & 0x1F, k;
            if(p[0] & 0x40) { lfn_expected = seq; lfn_seen = 0; lfn_sum = p[13]; lfn_start = it->idx; memset(lfn, 0, sizeof(lfn)); }
            if(lfn_expected && seq >= 1 && seq <= LFN_MAX_ENTRIES && p[13] == lfn_sum) {
                static const u8 pos[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for(k = 0; k < LFN_CHARS; k++) lfn[(seq - 1) * LFN_CHARS + k] = le16(p + pos[k]);
                lfn_seen++;
            } else lfn_expected = 0;
        } else if(p[11] & ATTR_LABEL) {
            lfn_expected = lfn_seen = 0;
        } else {
            memcpy(e->raw, p, 32);
            e->lba = it->lba;
            e->off = it->off;
            e->idx = it->idx;
            for(len = 0; lfn_expected && len < (u32)lfn_expected * LFN_CHARS && lfn[len] && lfn[len] != 0xFFFF; len++);
            if(lfn_expected && lfn_seen == lfn_expected && lfn_checksum(p) == lfn_sum && len &&
               utf16_to_utf8(lfn, len, e->name, sizeof(e->name))) {   /* else (too long as UTF-8, say) the 8.3 name */
                e->lfn_idx = lfn_start;
            } else {
                short_to_name(p, e->name);
                e->lfn_idx = it->idx;
            }
            e->first = le16(p + 26) | (it->fs->type == 32 ? (u32)le16(p + 20) << 16 : 0);
            e->size = le32(p + 28);
            brelse(b);
            r = diter_next(it);
            return r < 0 ? r : 0;
        }
        brelse(b);
        r = diter_next(it);
        if(r) return r;
    }
}

static int dir_find(struct fat_fs *fs, struct fat_node *dir, const char *name, struct fat_dirent *e) {
    struct diter it;
    int r;
    diter_init(&it, fs, dir);
    while((r = dir_read(&it, e)) == 0) {
        if(name_eq(e->name, name)) return 0;
    }
    return r < 0 ? r : -ENOENT;
}

/* a run of `need` free entries, extending the directory if it has to; pos ends at the first one */
static int dir_find_free(struct fat_fs *fs, struct fat_node *dir, u32 need, struct diter *pos) {
    struct diter it, start;
    u32 run = 0;
    diter_init(&it, fs, dir);
    for(;;) {
        struct buf *b;
        bool free;
        int r;
        if(it.at_end) { r = diter_extend(&it); if(r) return r; }
        b = bread(fs->dev, it.lba);
        if(!b) return -EIO;
        free = (b->data[it.off] == 0 || b->data[it.off] == 0xE5);
        brelse(b);
        if(free) {
            if(!run) start = it;
            if(++run == need) { *pos = start; return 0; }
        } else run = 0;
        r = diter_next(&it);
        if(r) return r;
    }
}

/* 10/16/26 (B24): an undo that could not be done (a block it needed could not be read back): the volume
   may now name one file twice (a rename's new name left with the old one: an rm of either would free the
   chain under the other), or lose one. Nothing more is changed on it: every change fails with EROFS until
   it is unmounted, it stays marked as not cleanly unmounted (fsck.fat on another computer), and the console
   says so once */
static void fat_damaged(struct fat_fs *fs, const char *what) {
    if(fs->damaged) return;
    fs->damaged = true;
    kprintf("+FAT%i: %s: %s could not be undone (a block could not be read): read-only until unmounted;"
            " check it (fsck.fat) before it is written again\n", fs->type, fs->dev->name, what);
}

/* put the slots back as they were, the last changed first. 10/16/26 (B24): how many could not be (their
   block could not be read; the others are put back all the same) */
static u32 slots_restore(struct fat_fs *fs, const struct slot_undo *u, u32 n) {
    u32 lost = 0;
    while(n--) {
        struct buf *b = bread(fs->dev, u[n].lba);
        if(!b) { lost++; continue; }
        b->data[u[n].off] = u[n].first;
        bdirty_order(b, BORDER_DIR);
        brelse(b);
    }
    return lost;
}

/* write n raw entries at pos (the space was found by dir_find_free); *lba and *off (when
   given) say where the last one, the short entry, went. 10/16/26 (B5): each slot written is
   noted in undo (*nundo of them), also when it stops early */
static int dir_write_entries(struct diter *pos, const u8 *entries, u32 n, u32 *lba, u32 *off,
                             struct slot_undo *undo, u32 *nundo) {
    u32 i;
    *nundo = 0;
    for(i = 0; i < n; i++) {
        struct buf *b;
        int r;
        if(pos->at_end) { r = diter_extend(pos); if(r) return r; }
        b = bread(pos->fs->dev, pos->lba);
        if(!b) return -EIO;
        undo[*nundo].lba = pos->lba;
        undo[*nundo].off = (u16)pos->off;
        undo[(*nundo)++].first = b->data[pos->off];
        memcpy(b->data + pos->off, entries + i * 32, 32);
        bdirty_order(b, BORDER_DIR);
        brelse(b);
        if(i == n - 1 && lba) { *lba = pos->lba; *off = pos->off; }
        r = diter_next(pos);
        if(r) return r;
    }
    return 0;
}

/* mark the entries idx_from..idx_to of dir as deleted; 10/16/26 (B5): noted in undo, as above */
static int dir_delete_entries(struct fat_fs *fs, struct fat_node *dir, u32 idx_from, u32 idx_to,
                              struct slot_undo *undo, u32 *nundo) {
    struct diter it;
    *nundo = 0;
    diter_init(&it, fs, dir);
    while(!it.at_end && it.idx <= idx_to) {
        if(it.idx >= idx_from) {
            struct buf *b = bread(fs->dev, it.lba);
            if(!b) return -EIO;
            if(*nundo < UNDO_MAX) {
                undo[*nundo].lba = it.lba;
                undo[*nundo].off = (u16)it.off;
                undo[(*nundo)++].first = b->data[it.off];
            }
            b->data[it.off] = 0xE5;
            bdirty_order(b, BORDER_DIR);
            brelse(b);
        }
        if(diter_next(&it)) return -EIO;
    }
    return 0;
}

/* 10/16/26 (B5): a file's short entry, as it is, and put back */
static int entry_get(struct fat_fs *fs, struct fat_node *n, u8 *raw) {
    struct buf *b;
    if(n->is_root) return 0;
    if(!(b = bread(fs->dev, n->dir_lba))) return -EIO;
    memcpy(raw, b->data + n->dir_off, 32);
    brelse(b);
    return 0;
}

static int entry_put(struct fat_fs *fs, struct fat_node *n, const u8 *raw) {
    struct buf *b;
    if(n->is_root) return 0;
    if(!(b = bread(fs->dev, n->dir_lba))) return -EIO;
    memcpy(b->data + n->dir_off, raw, 32);
    bdirty_order(b, BORDER_DIR);
    brelse(b);
    return 0;
}

/* the short entry of a file after its size, first cluster or time changed */
static int update_entry(struct fat_fs *fs, struct fat_node *n, u32 size, bool touch) {
    struct buf *b;
    u8 *p;
    if(n->is_root) return 0;
    b = bread(fs->dev, n->dir_lba);
    if(!b) return -EIO;
    p = b->data + n->dir_off;
    put32(p + 28, size);
    put16(p + 26, (u16)(n->first & 0xFFFF));
    if(fs->type == 32) put16(p + 20, (u16)(n->first >> 16));
    if(touch) {
        u16 d, t;
        fat_now(&d, &t);
        put16(p + 22, t);
        put16(p + 24, d);
        put16(p + 18, d);
        p[11] |= ATTR_ARCHIVE;
    }
    bdirty_order(b, BORDER_DIR);
    brelse(b);
    return 0;
}

/**** names ****/

static bool legal_char(int c) {
    if(c < 0x20 || c == 0x7F) return false;
    return !strchr("\\/:*?\"<>|", c);
}

/* a name a new entry may have: well-formed UTF-8 of at most 255 UTF-16 units, none of
   \\ / : * ? " < > | or control characters, not "." or "..", not ending in a space or a dot */
static int validate_name(const char *name) {
    u16 units[LFN_MAX_ENTRIES * LFN_CHARS];
    u32 count;
    size_t n = strlen(name);
    size_t i;
    if(!n) return -EINVAL;
    if(n > VFS_NAME_MAX) return -ENAMETOOLONG;
    if(!strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
    for(i = 0; i < n; i++) if(!legal_char((u8)name[i])) return -EINVAL;
    if(name[n - 1] == ' ' || name[n - 1] == '.') return -EINVAL;
    return name_to_utf16(name, units, &count);
}

static bool short_char_ok(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

/* is raw11 the short name of an entry of dir other than the one at index skip? */
static bool dir_has_short(struct fat_fs *fs, struct fat_node *dir, const u8 *raw11, u32 skip) {
    struct diter it;
    struct fat_dirent e;
    diter_init(&it, fs, dir);
    while(dir_read(&it, &e) == 0) if(e.idx != skip && !memcmp(e.raw, raw11, 11)) return true;
    return false;
}

/* the alias character for code point cp: upper case, or '_' for what an 8.3 name cannot
   hold, everything beyond ASCII included */
static int short_char(i32 cp) {
    if(cp > 0 && cp < 0x80 && short_char_ok(upper((int)cp))) return upper((int)cp);
    return '_';
}

/* The 8.3 form of name in raw[11] with the lowercase flags in *ntres, unique in dir (the
   entry at index skip, which is being renamed, does not count). *lfn says whether long-name
   entries are needed as well (the name is not representable as 8.3). A name that is not
   8.3 gets fatgen103's basis name: upper case, '_' for what cannot be held, spaces and
   leading dots dropped, the base up to the first dot that is left (8 at most), the extension
   after the last dot (3 at most); then a ~N tail, unless only the case was in the way.
   -ENOSPC when every ~N tail is taken. */
static int make_short_name(struct fat_fs *fs, struct fat_node *dir, const char *name, u32 skip, u8 *raw, u8 *ntres, bool *lfn) {
    const char *dot = strrchr(name, '.'), *q, *last;
    size_t blen = dot ? (size_t)(dot - name) : strlen(name), elen = dot ? strlen(dot + 1) : 0;
    bool fits = true, base_lower = false, base_upper = false, ext_lower = false, ext_upper = false;
    size_t i, n, nb = 0, ne = 0;
    char base[9], ext[4];

    memset(raw, ' ', 11);
    *ntres = 0;
    *lfn = true;
    if(dot == name) { fits = false; blen = 0; }              /* ".name" */
    if(blen > 8 || elen > 3 || (dot && !elen)) fits = false;
    for(i = 0; i < blen && fits; i++) {
        int c = (u8)name[i];
        if(c == '.' || c == ' ') fits = false;
        if(c >= 'a' && c <= 'z') base_lower = true;
        else if(c >= 'A' && c <= 'Z') base_upper = true;
        if(!short_char_ok(upper(c))) fits = false;           /* bytes beyond ASCII are never 8.3 */
    }
    for(i = 0; i < elen && fits; i++) {
        int c = (u8)dot[1 + i];
        if(c == '.' || c == ' ') fits = false;
        if(c >= 'a' && c <= 'z') ext_lower = true;
        else if(c >= 'A' && c <= 'Z') ext_upper = true;
        if(!short_char_ok(upper(c))) fits = false;
    }
    if(fits) {                                               /* 8.3 already: upper case, and flags for lower case */
        for(i = 0; i < blen; i++) raw[i] = upper((u8)name[i]);
        for(i = 0; i < elen; i++) raw[8 + i] = upper((u8)dot[1 + i]);
        if(!dir_has_short(fs, dir, raw, skip)) {             /* else another entry's alias: a ~N name and a long name */
            if((base_lower && base_upper) || (ext_lower && ext_upper)) return 0;   /* mixed case: the long name keeps it */
            if(base_lower) *ntres |= NTRES_LOWER_BASE;
            if(ext_lower) *ntres |= NTRES_LOWER_EXT;
            *lfn = false;
            return 0;
        }
    }
    /* the basis name, then BASENA~1.EXT */
    for(q = name; *q == '.'; q++);
    last = strrchr(q, '.');
    while(*q && *q != '.' && nb < 8) {
        i32 cp = utf8_next(&q);
        if(cp < 0) break;
        if(cp != ' ') base[nb++] = (char)short_char(cp);
    }
    if(!nb) base[nb++] = '_';
    base[nb] = 0;
    for(q = last ? last + 1 : ""; *q && ne < 3; ) {
        i32 cp = utf8_next(&q);
        if(cp < 0) break;
        if(cp != ' ') ext[ne++] = (char)short_char(cp);
    }
    ext[ne] = 0;
    for(n = 1; n < 1000; n++) {
        char tail[8];
        size_t bl = nb, tl;
        snprintf(tail, sizeof(tail), "~%u", (u_int)n);
        tl = strlen(tail);
        if(bl + tl > 8) bl = 8 - tl;
        memset(raw, ' ', 11);
        memcpy(raw, base, bl);
        memcpy(raw + bl, tail, tl);
        memcpy(raw + 8, ext, ne);
        if(!dir_has_short(fs, dir, raw, skip)) return 0;
    }
    return -ENOSPC;
}

/* Raw entries for a name: the LFN entries (if any) followed by the short entry; *count
   of them. tmpl, when given, is the short entry of the object being renamed (at index skip
   of dir): its attributes, times, cluster and size carry over to the new entry. */
static int build_entries(struct fat_fs *fs, struct fat_node *dir, const char *name, u8 attr, u32 first,
                         const u8 *tmpl, u32 skip, u8 *out, u32 *count) {
    u8 raw[11], ntres, sum;
    u32 n = 0, nlfn = 0, k;
    bool lfn;
    u16 d, t;
    u8 *p;
    u16 units[LFN_MAX_ENTRIES * LFN_CHARS];
    u32 len;
    int r = name_to_utf16(name, units, &len);

    if(r == 0) r = make_short_name(fs, dir, name, skip, raw, &ntres, &lfn);
    if(r) return r;
    if(lfn) {
        nlfn = (len + LFN_CHARS - 1) / LFN_CHARS;
        sum = lfn_checksum(raw);
        for(k = nlfn; k >= 1; k--) {
            static const u8 pos[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            u32 j;
            p = out + n * 32;
            memset(p, 0, 32);
            p[0] = (u8)(k | (k == nlfn ? 0x40 : 0));
            p[11] = ATTR_LFN;
            p[13] = sum;
            for(j = 0; j < LFN_CHARS; j++) {
                u32 ci = (k - 1) * LFN_CHARS + j;
                put16(p + pos[j], ci < len ? units[ci] : (ci == len ? 0 : 0xFFFF));
            }
            n++;
        }
    }
    p = out + n * 32;
    if(tmpl) memcpy(p, tmpl, 32);                            /* a rename: everything but the name stays */
    else {
        memset(p, 0, 32);
        p[11] = attr;
        fat_now(&d, &t);
        put16(p + 14, t); put16(p + 16, d);                  /* created */
        put16(p + 18, d);                                    /* accessed */
        put16(p + 20, (u16)(fs->type == 32 ? first >> 16 : 0));
        put16(p + 22, t); put16(p + 24, d);                  /* modified */
        put16(p + 26, (u16)(first & 0xFFFF));
    }
    memcpy(p, raw, 11);
    p[12] = (p[12] & ~(NTRES_LOWER_BASE | NTRES_LOWER_EXT)) | ntres;
    *count = n + 1;
    return 0;
}

/**** vnodes ****/

/* the in-use node of the entry at lba/off, or NULL */
static struct fat_node *live_node(struct fat_fs *fs, u32 lba, u32 off) {
    struct fat_node *n;
    for(n = fs->live; n; n = n->next) if(n->dir_lba == lba && n->dir_off == off && n->v->refs) return n;
    return NULL;
}

/* the vnode of a directory entry: the one in use already, else a new one (holds sb->lock) */
static struct vnode *fat_vnode(struct superblock *sb, const struct fat_dirent *e) {
    struct fat_fs *fs = (struct fat_fs *)sb->priv;
    struct fat_node *n;
    struct vnode *v;
    for(n = fs->live; n; n = n->next) {
        if(n->dir_lba == e->lba && n->dir_off == e->off) {
            u_long f = irq_save();
            bool alive = n->v->refs > 0;                     /* 0: vput is releasing it right now */
            if(alive) n->v->refs++;
            irq_restore(f);
            if(alive) return n->v;
        }
    }
    n = (struct fat_node *)kcalloc(1, sizeof(struct fat_node));
    if(!n) return NULL;
    v = vnode_alloc(sb, &fat_vops, (e->raw[11] & ATTR_DIR) ? VN_DIR : VN_FILE);
    if(!v) { kfree(n); return NULL; }
    n->v = v;
    n->first = e->first;
    n->dir_lba = e->lba;
    n->dir_off = e->off;
    v->size = (e->raw[11] & ATTR_DIR) ? 0 : e->size;
    v->mtime = fat_to_unix(le16(e->raw + 24), le16(e->raw + 22));
    v->ino = (e->lba << 4) | (e->off / 32);
    v->priv = n;
    n->next = fs->live;
    fs->live = n;
    return v;
}

static int fat_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct fat_dirent e;
    int r;
    if(fs->dev->gone) return -EIO;                           /* unplugged */
    fat_lock(dir->sb);
    r = dir_find(fs, (struct fat_node *)dir->priv, name, &e);
    if(r == 0) {
        *out = fat_vnode(dir->sb, &e);
        if(!*out) r = -ENOMEM;
    }
    fat_unlock(dir->sb);
    return r;
}

/* entry index of the directory; a listing asks for 0, 1, 2... and each call resumes where the
   last one stopped instead of reading the directory from its start again */
static int fat_readdir(struct vnode *dir, u_int index, struct dirent *de) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct fat_node *dn = (struct fat_node *)dir->priv;
    struct diter it;
    struct fat_dirent e;
    u_int skip = index;
    int r;
    if(fs->dev->gone) return -EIO;
    fat_lock(dir->sb);
    if(dn->rd_valid && dn->rd_index == index) { it = dn->rd_it; skip = 0; }
    else diter_init(&it, fs, dn);
    dn->rd_valid = false;
    while((r = dir_read(&it, &e)) == 0) {
        if(!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        if(skip-- == 0) {
            strlcpy(de->name, e.name, sizeof(de->name));
            de->type = (e.raw[11] & ATTR_DIR) ? VN_DIR : VN_FILE;
            de->size = (e.raw[11] & ATTR_DIR) ? 0 : e.size;
            de->mtime = fat_to_unix(le16(e.raw + 24), le16(e.raw + 22));
            dn->rd_it = it;
            dn->rd_index = index + 1;
            dn->rd_valid = true;
            break;
        }
    }
    fat_unlock(dir->sb);
    return r;
}

/* the cluster after c in its chain; on a write past the end of the chain a new one (c + 1 when
   that is free). 0, 1 when a read reaches the end, < 0. */
static int next_cluster(struct fat_fs *fs, u32 c, bool grow, u32 *out) {
    u32 next;
    int r = fat_get(fs, c, &next);
    if(r) return r;
    if(is_eoc(fs, next)) return grow ? fat_alloc(fs, c, false, out) : 1;
    *out = next;
    return 0;
}

/* The cluster at index ci of n's chain, walked to from the position n remembers when that is
   not past ci (sequential I/O takes one step per cluster, not a walk from the start). With
   grow the chain is extended as needed. 0, 1 when ci is past the end (reading), < 0. */
static int seek_cluster(struct fat_fs *fs, struct fat_node *n, u32 ci, bool grow, u32 *out) {
    u32 c, i;
    int r;
    if(!n->first) {
        if(!grow) return 1;
        r = fat_alloc(fs, 0, false, &c);
        if(r) return r;
        n->first = c;
        n->pos_c = 0;
    }
    if(n->pos_c && n->pos_ci <= ci) { i = n->pos_ci; c = n->pos_c; }
    else { i = 0; c = n->first; }
    while(i < ci) {
        r = next_cluster(fs, c, grow, &c);
        if(r) return r;
        i++;
    }
    n->pos_ci = ci;
    n->pos_c = c;
    *out = c;
    return 0;
}

/* How many blocks from block s of cluster c on are next to each other on the disk and in the
   file (the chain going c, c + 1, ...), up to max; with grow the chain is extended as far as
   that needs (c + 1 first). */
static int run_blocks(struct fat_fs *fs, u32 c, u32 s, u32 max, bool grow, u32 *count) {
    u32 n = fs->spc - s, next;
    while(n < max) {
        int r = next_cluster(fs, c, grow, &next);
        if(r == -ENOSPC) break;                             /* B4: the run ends where the volume does */
        if(r < 0) return r;
        if(r == 1 || next != c + 1) break;
        c = next;
        n += fs->spc;
    }
    *count = n < max ? n : max;
    return 0;
}

/* chunk bytes at so in block s of cluster c through the cache; pos is the file offset of the
   first of them. A read that misses reads ahead along the file; a write to a whole block, or
   to one that starts past the end of the file, does not read it first. */
static int block_rw(struct fat_fs *fs, struct vnode *v, u32 c, u32 s, u32 so, u8 *buf, u32 chunk, u32 pos, int mode) {
    u32 lba = clus_lba(fs, c) + s;
    struct buf *b;
    if(mode == RW_READ) {
        b = bpeek(fs->dev, lba);
        if(!b) {
            u32 ahead, left = (v->size - (pos - so) + 511) / 512;
            if(run_blocks(fs, c, s, FAT_READAHEAD, false, &ahead) != 0) ahead = 1;
            b = breada(fs->dev, lba, ahead < left ? ahead : (left ? left : 1));
        }
        if(!b) return -EIO;
        memcpy(buf, b->data + so, chunk);
    } else {
        if(bcache_room(fs->dev)) return -EIO;                /* 10/16/26 (B6): the device refuses, past its share */
        b = (chunk == 512 || pos - so >= v->size) ? bget(fs->dev, lba) : bread(fs->dev, lba);
        if(!b) return -EIO;
        if(mode == RW_WRITE) memcpy(b->data + so, buf, chunk);
        else memset(b->data + so, 0, chunk);
        bdirty(b);
    }
    brelse(b);
    return 0;
}

/* len bytes at off of the file: read into buf, written from buf, or (RW_ZERO, buf NULL) zeroed;
   writes grow the chain. Runs of FAT_DIRECT whole blocks or more that lie next to each other
   on the disk move straight between the device and buf, up to 64 KB a request; the rest goes
   through the cache. Holds sb->lock. The bytes done, else < 0; 10/16/26 (B23): *why (when asked) is the
   error that stopped it short, 0 when it did not stop short. 10/17/26 (B39): a kill or, in a kernel command,
   Ctrl+C stops it between two pieces (-EINTR in *why), as the device would: a task inside the volume's lock is
   not killed at once (fat_lock()), and a large transfer in one call would put its kill off to its end. A piece is a
   block through the cache, up to 64 KB straight to or from the device (10/17/26, B49: an earlier comment said 64 KB
   for both); the first piece always goes, and a straight one cut short in its wait says -EINTR, not -EIO */
static int rw_locked(struct fat_fs *fs, struct vnode *v, u32 off, u8 *buf, u32 len, int mode, int *why) {
    struct fat_node *n = (struct fat_node *)v->priv;
    bool grow = mode != RW_READ;
    u32 done = 0, c, within = off % fs->cluster_bytes;
    int r = seek_cluster(fs, n, off / fs->cluster_bytes, grow, &c);

    while(r == 0 && done < len) {
        u32 s = within / 512, so = within % 512, left = len - done, chunk = 512 - so, nb = 0;
        u8 *p = buf ? buf + done : NULL;
        if(done && fs_cut_short()) { r = -EINTR; break; }
        if(!so && left >= 512) {
            r = run_blocks(fs, c, s, left / 512 < BCACHE_RUN ? left / 512 : BCACHE_RUN, grow, &nb);
            if(r) break;
        }
        if(nb >= FAT_DIRECT) {
            u32 lba = clus_lba(fs, c) + s;
            r = mode == RW_READ ? bread_blocks(fs->dev, lba, nb, p) : mode == RW_WRITE ? bwrite_blocks(fs->dev, lba, nb, p)
                                                                      : bzero_blocks(fs->dev, lba, nb);
            if(r) { r = r == BCACHE_EINTR ? -EINTR : -EIO; break; }   /* (B49: cut short in its wait, not refused) */
            chunk = nb * 512;
        } else {
            if(chunk > left) chunk = left;
            r = block_rw(fs, v, c, s, so, p, chunk, off + done, mode);
            if(r) break;
        }
        done += chunk;
        within += chunk;
        while(within >= fs->cluster_bytes && done < len) {   /* on to the next cluster (several after a run) */
            within -= fs->cluster_bytes;
            r = next_cluster(fs, c, grow, &c);
            if(r) break;
            n->pos_ci++;
            n->pos_c = c;
        }
    }
    if(r == 1) r = 0;                                        /* a read reached the end of the chain */
    if(why) *why = r;
    return done ? (int)done : r;
}

/* 10/16/26 (B4): after a write that stopped early, the clusters it linked past the file's end go
   back: the chain ends at size again. first0 is the first cluster the entry had before the write
   (an entry is never left pointing at a freed cluster). Best effort: if this fails as well, the
   next fsck finds the chain longer than the file. sb->lock. 10/16/26 (B7): the cut is put on the
   disk before the clusters past it are freed (a sync, or making room, may have put the links into
   them there during the write). When the device refuses that, they are freed all the same (B4: a
   refused copy leaves nothing taken): the links into them reached the disk only if the device took
   writes during this write, and then the cut and the frees go out in one later sync, in the order
   of their FAT sectors (a card pulled in it may leave the chain running into freed clusters). The
   walk starts where the write was, when that is not past the cut.
   10/16/26 (B25): no more: when the device refuses the order point, the clusters past the cut are freed
   only after an order point that works (fs->pending; fat_order_point(), and unmount): the cut reaches the
   disk before their frees, so no chain there runs into them (the threaded test found it: the cut in one FAT
   sector, a freed cluster in another, written in the order of their sectors). Their failed blocks are
   dropped first (past the end of the file, never read: kept, they would hold that order point back for as
   long as the device refuses them). More than PENDING_MAX such chains at once stay lost (fsck.fat). */
static void trim_chain(struct fat_fs *fs, struct fat_node *n, u32 size, u32 first0) {
    u32 keep = (size + fs->cluster_bytes - 1) / fs->cluster_bytes, c = n->first, next, i = 0;
    if(!c) return;
    if(!keep) {
        n->pos_c = 0;
        if(first0) keep = 1;                                 /* the entry has it: it stays */
        else {                                               /* taken by this write: all of it goes */
            u32 v;
            /* 10/16/26 (B11): the file lets go of it once its head is free; a chain that could not be
               freed from its head stays the file's, for its next write (B19: also when the head's entry
               cannot be read: it lets go only of a head it sees free) */
            drop_chain(fs, c);                               /* 10/16/26 (B28): its bytes are nobody's */
            if(fat_free_chain(fs, c, true) == 0 || (fat_get(fs, c, &v) == 0 && v == 0)) n->first = 0;
            return;
        }
    }
    if(n->pos_c && n->pos_ci < keep) { i = n->pos_ci; c = n->pos_c; }
    n->pos_c = 0;                                            /* the remembered position may go */
    for(; i + 1 < keep; i++) {
        if(fat_get(fs, c, &next) != 0 || is_eoc(fs, next)) return;
        c = next;
    }
    if(fat_get(fs, c, &next) != 0 || is_eoc(fs, next) || fat_set(fs, c, fs->eoc) != 0) return;
    drop_chain(fs, next);                                    /* 10/16/26 (B28): past the end: nobody's bytes */
    if(fat_order_point(fs) == 0) fat_free_chain(fs, next, false);
    else {
        drop_chain(fs, next);                                /* (one another task's sync was writing, refused) */
        pend_chain(fs, next);
    }
}

/* the file grows to size with zeros (a write past its end, a truncate that grows); sb->lock.
   10/16/26 (B23): all or nothing: when the zeros cannot all be written (the device refuses, the volume is
   full) the size stays as it was, and the error is what stopped them (before, the file kept the zeros that
   were written, and a refused part said ENOSPC); the caller cuts the chain back (trim_chain()) */
static int grow_zeros(struct fat_fs *fs, struct vnode *v, u32 size) {
    int why = 0, r = rw_locked(fs, v, v->size, NULL, size - v->size, RW_ZERO, &why);
    if(r >= 0 && (u32)r == size - v->size) { v->size = size; return 0; }
    return why < 0 ? why : r < 0 ? r : -EIO;
}

static int fat_rw(struct vnode *v, u32 off, u8 *buf, u32 len, bool write) {
    struct fat_fs *fs = (struct fat_fs *)v->sb->priv;
    struct fat_node *n = (struct fat_node *)v->priv;
    u32 size, first0;
    int r = 0;

    if(!write) {
        if(off >= v->size) return 0;
        if(len > v->size - off) len = v->size - off;
    }
    if(!len) return 0;
    if(write && off + len < off) return -EFBIG;
    if(fs->dev->gone) return -EIO;
    if(write && bcache_room(fs->dev)) return -EIO;          /* 10/16/26 (B6): back-pressure from a refusing device */
    fat_lock(v->sb);
    if(write && fs->damaged) { fat_unlock(v->sb); return -EROFS; }   /* 10/16/26 (B24) */
    size = v->size;
    first0 = n->first;
    if(write && off > v->size) r = grow_zeros(fs, v, off);   /* the gap reads as zeros */
    if(r == 0) r = rw_locked(fs, v, off, buf, len, write ? RW_WRITE : RW_READ, NULL);
    if(write && r < 0) v->size = size;                       /* 10/16/26 (B23): nothing written: no gap either */
    if(write && r > 0 && off + (u32)r > v->size) v->size = off + r;
    if(write && (r < 0 || (u32)r < len)) trim_chain(fs, n, v->size, first0);   /* B4: stopped early */
    if(write && (r > 0 || v->size != size) && update_entry(fs, n, v->size, true) != 0 && r >= 0) r = -EIO;
    fat_unlock(v->sb);
    return r;
}

static int fat_read(struct vnode *v, u32 off, void *buf, u32 len) {
    return fat_rw(v, off, (u8 *)buf, len, false);
}

static int fat_write(struct vnode *v, u32 off, const void *buf, u32 len) {
    return fat_rw(v, off, (u8 *)buf, len, true);
}

static int fat_truncate(struct vnode *v, u32 size) {
    struct fat_fs *fs = (struct fat_fs *)v->sb->priv;
    struct fat_node *n = (struct fat_node *)v->priv;
    int r = 0;
    if(fs->dev->gone || bcache_room(fs->dev)) return -EIO;   /* B6 */
    fat_lock(v->sb);
    if(fs->damaged) { fat_unlock(v->sb); return -EROFS; }   /* 10/16/26 (B24) */
    if(size > v->size) {                                     /* growing: zeros */
        u32 first0 = n->first;
        r = grow_zeros(fs, v, size);
        if(r) trim_chain(fs, n, v->size, first0);            /* B4: stopped early (B23: the size as it was) */
        else if(update_entry(fs, n, v->size, true) != 0) r = -EIO;
        fat_unlock(v->sb);
        return r;
    }
    n->pos_c = 0;                                            /* the remembered position may be freed */
    /* 10/16/26 (B27, B29): an undo puts back only what it saved and changed: raw is the entry as entry_get() read
       it (zeroed until then), and `changed` says update_entry() changed it in the cache. B5 put raw back whenever a
       later step failed, also when entry_get() had not read it (32 bytes of the stack over the entry), and B24 made
       the volume read-only when that put-back could not read the block, also when nothing had changed */
    if(size == 0) {                                          /* 10/16/26 (B3): the entry says empty on the */
        u32 old = n->first, oldsize = v->size;               /* disk before its clusters are freed */
        struct aside aside = { NULL, NULL, 0 };
        u8 raw[32];
        bool changed = false;
        memset(raw, 0, sizeof(raw));
        r = entry_get(fs, n, raw);
        if(r == 0) {
            n->first = 0;
            v->size = 0;
            r = update_entry(fs, n, 0, true);
            changed = r == 0;
            if(r == 0 && old) {
                struct keep k = { { 0 }, 0 };
                keep_lba(&k, n->dir_lba);                    /* (the entry's block: update_entry()) */
                aside_collect(fs, old, &aside, &k);          /* 10/16/26 (B16, B28, B32) */
                aside_begin(fs, &aside);
                r = fat_order_point(fs);
                aside_end(fs, &aside, r == 0);
            }
            if(r) {                                          /* B5: it cannot go on: nothing changed */
                n->first = old;
                v->size = oldsize;
                if(changed && entry_put(fs, n, raw) != 0) fat_damaged(fs, "a truncate that failed");   /* B24 */
            } else if(old) fat_free_chain(fs, old, false);   /* 10/17/26 (B43): the new size is on the disk: 0 (as rm) */
        }
    } else {
        u32 keep = (size + fs->cluster_bytes - 1) / fs->cluster_bytes, c = n->first, next, oldsize = v->size;
        struct aside aside = { NULL, NULL, 0 };
        u8 raw[32];
        bool cut = false, changed = false;
        memset(raw, 0, sizeof(raw));
        while(--keep && !r) { if(fat_get(fs, c, &next) != 0 || is_eoc(fs, next)) r = -EIO; else c = next; }
        if(!r && fat_get(fs, c, &next) == 0 && !is_eoc(fs, next)) {
            /* 10/16/26 (B7): the cut and the new size on the disk before the clusters past it are free,
               as unlink does: a chain on the disk never runs into a cluster the next file may take.
               When that cannot be done, nothing changed (B5) */
            r = entry_get(fs, n, raw);
            if(!r && !(r = fat_set(fs, c, fs->eoc))) cut = true;
            if(!r) {
                v->size = size;
                r = update_entry(fs, n, size, true);
                changed = r == 0;
            }
            if(!r) {
                struct keep k = { { 0 }, 0 };
                keep_lba(&k, n->dir_lba);
                aside_collect(fs, next, &aside, &k);         /* 10/16/26 (B16, B28, B32) */
                aside_begin(fs, &aside);
                r = fat_order_point(fs);
                aside_end(fs, &aside, r == 0);
            }
            if(r) {
                bool lost = cut && fat_set(fs, c, next) != 0;
                v->size = oldsize;
                if(changed && entry_put(fs, n, raw) != 0) lost = true;
                if(lost) fat_damaged(fs, "a truncate that failed");   /* B24 */
            } else fat_free_chain(fs, next, false);         /* 10/17/26 (B43): the cut and the size are on the disk: 0 */
        } else if(!r) { v->size = size; r = update_entry(fs, n, size, true); }
    }
    fat_unlock(v->sb);
    return r;
}

static int fat_create(struct vnode *dir, const char *name, int type, struct vnode **out) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct fat_node *dn = (struct fat_node *)dir->priv;
    struct fat_dirent e;
    struct diter pos;
    u8 entries[(LFN_MAX_ENTRIES + 1) * 32];
    struct slot_undo undo[UNDO_MAX];
    u32 n, first = 0, nundo = 0;
    int r = validate_name(name);

    if(r) return r;
    if(fs->dev->gone || bcache_room(fs->dev)) return -EIO;   /* B6 */
    fat_lock(dir->sb);
    if(fs->damaged) { r = -EROFS; goto out; }                /* 10/16/26 (B24) */
    dn->rd_valid = false;
    r = dir_find(fs, dn, name, &e);
    if(r == 0) { r = -EEXIST; goto out; }
    if(r != -ENOENT) goto out;
    if(type == VN_DIR) {                                     /* a directory starts with "." and ".." */
        struct buf *b;
        u8 *p;
        u16 d, t;
        u32 parent = dn->is_root ? 0 : dn->first;
        r = fat_alloc(fs, 0, true, &first);
        if(r) goto out;
        b = bread(fs->dev, clus_lba(fs, first));
        if(!b) { fat_free_chain(fs, first, false); r = -EIO; goto out; }   /* 10/16/26 (B36): (it was lost, its zeros owed) */
        fat_now(&d, &t);
        p = b->data;
        memset(p, ' ', 11); p[0] = '.'; p[11] = ATTR_DIR;
        put16(p + 22, t); put16(p + 24, d); put16(p + 26, (u16)first); put16(p + 20, (u16)(fs->type == 32 ? first >> 16 : 0));
        p += 32;
        memset(p, ' ', 11); p[0] = '.'; p[1] = '.'; p[11] = ATTR_DIR;
        put16(p + 22, t); put16(p + 24, d); put16(p + 26, (u16)parent); put16(p + 20, (u16)(fs->type == 32 ? parent >> 16 : 0));
        bdirty(b);
        brelse(b);
        r = fat_table_point(fs);                             /* 10/16/26 (B7): the new directory on the disk before */
        if(r) { fat_free_chain(fs, first, false); goto out; }       /* anything names it or is written into it */
    }
    r = build_entries(fs, dn, name, type == VN_DIR ? ATTR_DIR : ATTR_ARCHIVE, first, NULL, NO_IDX, entries, &n);
    if(r == 0) r = dir_find_free(fs, dn, n, &pos);
    if(r == 0) r = dir_write_entries(&pos, entries, n, &e.lba, &e.off, undo, &nundo);
    if(r == 0) {                                             /* 10/16/26 (B17): the vnode from what was written */
        memcpy(e.raw, entries + (n - 1) * 32, 32);           /* (it was read back, and a read error there said */
        e.first = first;                                     /* EIO for a file that had been made) */
        e.size = 0;
        *out = fat_vnode(dir->sb, &e);
        if(!*out) r = -ENOMEM;
    }
    if(r) {                                                  /* B5: what was written of the name goes again */
        if(slots_restore(fs, undo, nundo)) fat_damaged(fs, "a create that failed");   /* B24: (the new name may */
        else if(first && !nundo) fat_free_chain(fs, first, false);                           /* name the cluster: kept) */
        else if(first) {
            /* 10/16/26 (B30): something of the name was written: the new directory's cluster is freed once the
               slots put back are on the disk. Another task's sync may have written the new name, and a free now
               would reach the disk first (tables go before entries), the name there pointing at a free cluster */
            drop_chain(fs, first);
            pend_chain(fs, first);
        }
    }
out:
    fat_unlock(dir->sb);
    return r;
}

/* 0 when the directory starting at cluster first holds nothing but "." and "..", else -ENOTEMPTY */
static int dir_empty(struct fat_fs *fs, u32 first) {
    struct fat_node sub;
    struct diter it;
    struct fat_dirent x;
    int r;
    memset(&sub, 0, sizeof(sub));
    sub.first = first;
    diter_init(&it, fs, &sub);
    while((r = dir_read(&it, &x)) == 0) {
        if(strcmp(x.name, ".") && strcmp(x.name, "..")) return -ENOTEMPTY;
    }
    return r < 0 ? r : 0;
}

static int fat_unlink(struct vnode *dir, const char *name) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct fat_node *dn = (struct fat_node *)dir->priv;
    struct fat_dirent e;
    struct slot_undo undo[UNDO_MAX];
    struct aside aside = { NULL, NULL, 0 };
    u32 nundo = 0;
    int r;
    if(fs->dev->gone || bcache_room(fs->dev)) return -EIO;   /* B6 */
    fat_lock(dir->sb);
    if(fs->damaged) { r = -EROFS; goto out; }                /* 10/16/26 (B24) */
    dn->rd_valid = false;
    r = dir_find(fs, dn, name, &e);
    if(r) goto out;
    if((e.raw[11] & ATTR_DIR) && (r = dir_empty(fs, e.first)) != 0) goto out;   /* only an empty directory goes */
    r = dir_delete_entries(fs, dn, e.lfn_idx, e.idx, undo, &nundo);
    if(r == 0 && e.first) {
        struct keep k = { { 0 }, 0 };
        keep_undo(&k, undo, nundo);                          /* (the deleted slots' blocks) */
        aside_collect(fs, e.first, &aside, &k);              /* 10/16/26 (B16, B28, B32): its blocks hold nothing back */
        aside_begin(fs, &aside);
        r = fat_order_point(fs);                             /* B3: the entry gone on the disk first */
        aside_end(fs, &aside, r == 0);
    }
    if(r) {                                                  /* 10/16/26 (B5): it cannot go on: the name is back */
        if(slots_restore(fs, undo, nundo)) fat_damaged(fs, "an rm that failed");   /* B24 */
    } else if(e.first) fat_free_chain(fs, e.first, false);  /* 10/17/26 (B43): the rm has happened: its name is off the
                                                               disk and cannot come back, so it says 0 (an EIO here told
                                                               the caller the file was still there, and the next try said
                                                               ENOENT); clusters a failure leaves allocated are counted
                                                               and said (lose_chain()), and the volume stays unclean */
out:
    fat_unlock(dir->sb);
    return r;
}

/* do two directory nodes stand for the same directory? */
static bool dir_same(const struct fat_node *a, const struct fat_node *b) {
    if(a->is_root || b->is_root) return a->is_root && b->is_root;
    return a->first == b->first;
}

/* point the ".." entry of the directory at cluster first to cluster pc (its parent; 0 for the root).
   10/16/26 (B17): what it pointed at before in *old, when asked (to put it back) */
static int set_dotdot(struct fat_fs *fs, u32 first, u32 pc, u32 *old, bool *changed) {
    struct buf *b;
    u8 *p;
    if(old) *old = pc;
    if(changed) *changed = false;                            /* 10/16/26 (B29): whether there is a change to undo */
    if(first < 2 || first >= fs->nclusters + 2) return 0;
    b = bread(fs->dev, clus_lba(fs, first));
    if(!b) return -EIO;
    p = b->data + 32;
    if(p[0] == '.' && p[1] == '.' && p[2] == ' ' && (p[11] & ATTR_DIR)) {
        if(old) *old = le16(p + 26) | (fs->type == 32 ? (u32)le16(p + 20) << 16 : 0);
        put16(p + 26, (u16)(pc & 0xFFFF));
        put16(p + 20, (u16)(fs->type == 32 ? pc >> 16 : 0));
        bdirty_order(b, BORDER_DIR);
        if(changed) *changed = true;
    }
    brelse(b);
    return 0;
}

/* 10/5/26: rename within the volume. The new entries (a fresh short alias, the long name if
   one is needed, everything else copied from the old short entry) are written before the
   old ones are deleted, so an interruption leaves two names rather than none (10/16/26, B3:
   on the disk too, an order point between them). A file at the target is replaced (POSIX),
   so is an empty directory; a directory that moves to another parent gets its ".." pointed
   there. The VFS has refused moving a directory into itself.
   10/16/26 (B17): every step that can fail comes before the replaced file's clusters are freed,
   and is undone when one fails (B5: nothing changed): the ".." of a directory that moves (it was
   pointed last, when the rename was done, and an error there said EIO for a rename that had
   happened), the new name, the old name's deletion, then the replaced name's, and the order point
   that takes both off the disk. Before, the replaced file went first: its clusters were freed,
   and an old name that could not be deleted then (its directory's block unreadable) failed the
   rename with the file it was to replace gone.
   10/16/26 (B20): the replaced file's failed blocks are set aside (B16) for the second order point only,
   once its entry is deleted (in the cache): the first one writes the new name, whose directory block may
   hold the replaced file's entry too, which covers them (B16 set them aside from the first: that order
   point wrote the entry ahead of its file's bytes, and when the rename failed later the entry stayed so
   on the disk). While a block of the replaced file refuses, the rename fails (EIO): an rm of that file
   works (B16), then the rename. 10/16/26 (B24): an undo that could not be done leaves the volume
   read-only (fat_damaged()). */
static int fat_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname) {
    struct fat_fs *fs = (struct fat_fs *)odir->sb->priv;
    struct fat_node *on = (struct fat_node *)odir->priv, *nn = (struct fat_node *)ndir->priv, *x;
    struct fat_dirent src, dst;
    struct diter pos;
    u8 entries[(LFN_MAX_ENTRIES + 1) * 32];
    struct slot_undo nu[UNDO_MAX] = { { 0 } }, su[UNDO_MAX] = { { 0 } },   /* (zeroed: gcc -O2 cannot tell that */
                     du[UNDO_MAX] = { { 0 } };                              /* the undo reads only what was noted) */
    u32 n, new_lba = 0, new_off = 0, nnu = 0, nsu = 0, ndu = 0, old_parent = 0, lost;
    struct aside aside = { NULL, NULL, 0 };
    bool same = dir_same(on, nn), is_dir, replace = false, dotdot = false;
    int r = validate_name(nname);

    if(r) return r;
    if(fs->dev->gone || bcache_room(fs->dev)) return -EIO;   /* B6 */
    fat_lock(odir->sb);
    if(fs->damaged) { r = -EROFS; goto out; }                /* 10/16/26 (B24) */
    on->rd_valid = nn->rd_valid = false;
    r = dir_find(fs, on, oname, &src);
    if(r) goto out;
    is_dir = (src.raw[11] & ATTR_DIR) != 0;
    r = dir_find(fs, nn, nname, &dst);
    if(r == 0 && !(same && dst.idx == src.idx)) {            /* another object has the name (else: a change of case) */
        if(live_node(fs, dst.lba, dst.off)) { r = -EBUSY; goto out; }   /* in use: not deleted under its user */
        if(is_dir && !(dst.raw[11] & ATTR_DIR)) { r = -ENOTDIR; goto out; }
        if(!is_dir && (dst.raw[11] & ATTR_DIR)) { r = -EISDIR; goto out; }
        if(is_dir && (r = dir_empty(fs, dst.first)) != 0) goto out;
        replace = true;
    } else if(r != 0 && r != -ENOENT) goto out;
    r = build_entries(fs, nn, nname, 0, 0, src.raw, same ? src.idx : NO_IDX, entries, &n);
    if(r == 0) r = dir_find_free(fs, nn, n, &pos);
    if(r == 0 && is_dir && !same) {                          /* B17: ".." first, while it can be undone */
        r = set_dotdot(fs, src.first, nn->is_root ? 0 : nn->first, &old_parent, &dotdot);
        dotdot = dotdot && r == 0;                           /* 10/16/26 (B29): put back only what it changed */
    }
    if(r == 0) r = dir_write_entries(&pos, entries, n, &new_lba, &new_off, nu, &nnu);
    if(r == 0) r = fat_order_point(fs);                      /* B3: the new name on the disk before any goes */
    if(r == 0) r = dir_delete_entries(fs, on, src.lfn_idx, src.idx, su, &nsu);
    if(r == 0 && replace) r = dir_delete_entries(fs, nn, dst.lfn_idx, dst.idx, du, &ndu);
    if(r == 0 && replace && dst.first) {                     /* the replaced entry gone before its clusters */
        struct keep k = { { 0 }, 0 };
        keep_undo(&k, nu, nnu);                              /* (the slots it wrote and deleted) */
        keep_undo(&k, su, nsu);
        keep_undo(&k, du, ndu);
        if(dotdot) keep_lba(&k, clus_lba(fs, src.first));
        aside_collect(fs, dst.first, &aside, &k);            /* 10/16/26 (B16, B20, B28, B32): only now */
        aside_begin(fs, &aside);
        r = fat_order_point(fs);
        aside_end(fs, &aside, r == 0);
    }
    if(r) {                                                  /* 10/16/26 (B5): it cannot go on: both names as they were */
        lost = slots_restore(fs, du, ndu);
        lost += slots_restore(fs, su, nsu);
        lost += slots_restore(fs, nu, nnu);
        if(dotdot && set_dotdot(fs, src.first, old_parent, NULL, NULL) != 0) lost++;
        if(lost) fat_damaged(fs, "a rename that failed");    /* B24 */
        goto out;
    }
    if(replace && dst.first) fat_free_chain(fs, dst.first, false);   /* nothing fails after this: the rename stands (a */
                                                              /* failure loses clusters: fsck.fat finds them) */
    for(x = fs->live; x; x = x->next) {                      /* an open file or directory follows its entry */
        if(x->dir_lba == src.lba && x->dir_off == src.off) { x->dir_lba = new_lba; x->dir_off = new_off; }
    }
out:
    fat_unlock(odir->sb);
    return r;
}

/* the last reference is gone: forget the node (the root's goes at unmount) */
static void fat_release(struct vnode *v) {
    struct fat_node *n = (struct fat_node *)v->priv, **pp;
    struct fat_fs *fs = (struct fat_fs *)v->sb->priv;
    if(n && !n->is_root) {
        fat_lock(v->sb);
        for(pp = &fs->live; *pp; pp = &(*pp)->next) if(*pp == n) { *pp = n->next; break; }
        fat_unlock(v->sb);
    }
    kfree(n);
}

static const struct vnode_ops fat_vops = {
    fat_lookup, fat_readdir, fat_read, fat_write, fat_truncate, fat_create, fat_unlink, fat_release, fat_rename
};

/**** mount ****/

static bool bpb_sane(const u8 *bs) {
    u16 bps = le16(bs + 11);
    u8 spc = bs[13], nfats = bs[16];
    u16 rsv = le16(bs + 14);
    if(bs[510] != 0x55 || bs[511] != 0xAA) return false;
    if(bs[0] != 0xEB && bs[0] != 0xE9) return false;
    if(bps != 512 || !spc || (spc & (spc - 1)) || !rsv || !nfats || nfats > 2) return false;
    if(!le16(bs + 19) && !le32(bs + 32)) return false;
    return true;
}

static bool fat_probe(struct bdev *dev) {
    struct buf *b;
    bool ok;
    fat_in();                                                /* 10/17/26 (B39) */
    b = bread(dev, 0);
    ok = b && bpb_sane(b->data);
    if(b) brelse(b);
    fat_out();
    return ok;
}

static void copy_label(char *out, const u8 *raw);

/* 10/5/26: what the boot sector says, for mkfs and mount: "FAT32, label TESTVOL" */
static int describe(struct bdev *d, char *buf, size_t n);
int fat_describe(struct bdev *d, char *buf, size_t n) {
    int r;
    fat_in();                                                /* 10/17/26 (B39) */
    r = describe(d, buf, n);
    fat_out();
    return r;
}
static int describe(struct bdev *d, char *buf, size_t n) {
    struct buf *b = bread(d, 0);
    const u8 *bs;
    u32 total, spf, data, clusters;
    char label[12];
    if(!b) return -1;
    bs = b->data;
    if(!bpb_sane(bs)) { brelse(b); return -1; }
    total = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
    spf = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
    data = le16(bs + 14) + bs[16] * spf + (le16(bs + 17) * 32 + 511) / 512;
    clusters = total > data ? (total - data) / bs[13] : 0;
    label[0] = 0;
    if(le16(bs + 22) && bs[38] == 0x29) copy_label(label, bs + 43);
    else if(!le16(bs + 22) && bs[66] == 0x29) copy_label(label, bs + 71);
    brelse(b);
    snprintf(buf, n, "FAT%i%s%s", clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32, label[0] ? ", label " : "", label);
    return 0;
}

/* a label for people: trailing spaces gone, "NO NAME" is none, anything unusual a '_' */
static void copy_label(char *out, const u8 *raw) {
    int i, n = 11;
    while(n && raw[n - 1] == ' ') n--;
    for(i = 0; i < n; i++) out[i] = (raw[i] >= 0x20 && raw[i] < 0x7F) ? raw[i] : '_';
    out[n] = 0;
    if(!strcmp(out, "NO NAME")) out[0] = 0;
}

/* the root directory's volume-label entry, which wins over the boot sector's copy */
static void read_root_label(struct fat_fs *fs, struct fat_node *root) {
    struct diter it;
    diter_init(&it, fs, root);
    while(!it.at_end) {
        struct buf *b = bread(fs->dev, it.lba);
        const u8 *p;
        if(!b) return;
        p = b->data + it.off;
        if(p[0] == 0) { brelse(b); return; }
        if(p[0] != 0xE5 && (p[11] & ATTR_LFN) != ATTR_LFN && (p[11] & ATTR_LABEL)) {
            copy_label(fs->label, p);
            brelse(b);
            return;
        }
        brelse(b);
        if(diter_next(&it)) return;
    }
}

/* the volume's state reaches the disk: FSInfo, then the dirty blocks. 10/16/26 (B25): when that worked,
   the chains trim_chain() cut off before it began are freed, and that is synced too */
static int fat_sync(struct superblock *sb) {
    struct fat_fs *fs = (struct fat_fs *)sb->priv;
    u32 upto = 0;
    bool freed = false;
    int r = 0;
    if(fs->dev->gone) { bsync(sb->dev); return -EIO; }       /* drops what can no longer be written */
    if(!sb->readonly) {
        fat_lock(sb);
        if(fs->npending) drop_pending(fs);                   /* 10/16/26 (B28): it owes none of their blocks */
        r = write_fsinfo(fs);
        upto = fs->pend_added;
        fat_unlock(sb);
    }
    if(bsync(sb->dev)) return -EIO;
    if(!sb->readonly) {
        fat_lock(sb);
        if(fs->npending && (freed = free_pending(fs, upto)) && write_fsinfo(fs) != 0) r = -EIO;
        fat_unlock(sb);
        if(freed && bsync(sb->dev)) r = -EIO;
    }
    return r;
}

/* after the VFS has synced: mark the volume clean once everything else is on the disk. 10/16/26 (B25):
   the chains trim_chain() could not free yet go first (their cuts on the disk, then their frees);
   (B24) a damaged volume stays marked as not cleanly unmounted. 10/17/26 (B42): so does one that was not marked
   cleanly unmounted when it was mounted (fs->unclean): SurfOS's own next mount said so once and its unmount marked
   it clean, and the computer after it trusted a volume nobody had checked (B38's lost clusters, B24's damage). Only
   a checker clears the mark. (B44) FAT12 has no mark: its unmount says so instead. (B39) The calling task is inside
   the file system meanwhile: a kill that waited lets the unmount finish. (B46) Its syncs are not cut short either (a
   step that must finish): cut short, they left the volume unclean ("write errors") on a card that took every write,
   and counted the chains still waiting to be freed as lost */
static int fat_unmount(struct superblock *sb) {
    struct fat_fs *fs = (struct fat_fs *)sb->priv;
    u32 i, lost = 0;
    bool ok = false;
    const char *why = NULL;
    fs_finish_enter();
    if(fs->marked && !fs->dev->gone) {
        ok = !fs->damaged;
        if(ok && fs->npending) {
            drop_pending(fs);                                /* 10/16/26 (B28) */
            ok = bsync(fs->dev) == 0;
            if(ok) free_pending(fs, fs->pend_added);
        }
    }
    /* 10/16/26 (B31): clusters given back that could not be freed: allocated, nothing pointing at them (those that
       waited in vain may not have reached the card at all, or still hang off a file's chain there past its size).
       10/16/26 (B38): counted first: a volume with such clusters stays marked as not cleanly unmounted, as a
       damaged one does (B24), so that the next computer checks it (B31 marked it clean right after saying so) */
    for(lost = fs->lost_clusters, i = 0; i < fs->npending && !fs->dev->gone; i++) lost += chain_length(fs, fs->pending[i]);
    if(fs->marked && !fs->dev->gone) {
        if(ok && write_fsinfo(fs) == 0 && bsync(fs->dev) == 0 &&
           (lost || fs->unclean || (fat_mark(fs, true) == 0 && bsync(fs->dev) == 0)))
            bdev_flush(fs->dev);
        else ok = false;
        why = fs->damaged ? "damaged (an undo failed)" : !ok ? "write errors" : lost ? "clusters lost (below)" :
              fs->unclean ? "it was not unmounted cleanly before this mount, and only a check clears that" : NULL;
        if(why && fs->type == 12)                            /* 10/17/26 (B44) */
            kprintf("+FAT12: %s: %s; FAT12 keeps no mark of a clean unmount, so the next mount cannot tell: check the"
                    " volume (fsck.fat)\n", fs->dev->name, why);
        else if(why) kprintf("+FAT%i: %s: %s, the volume stays marked as not cleanly unmounted\n", fs->type, fs->dev->name, why);
    }
    if(lost) kprintf("+FAT%i: %s: %s%lu cluster%s given back could not be freed and may stay allocated with nothing"
                     " pointing at them: check the volume (fsck.fat frees them)\n", fs->type, fs->dev->name,
                     fs->lost_unsure ? "at least " : "", (u_long)lost, lost == 1 ? "" : "s");
    kfree(fs);
    fs_finish_leave();
    return 0;
}

static int fat_statfs(struct superblock *sb, struct statfs *st) {
    struct fat_fs *fs = (struct fat_fs *)sb->priv;
    int r = 0;
    if(fs->dev->gone) return -EIO;
    fat_lock(sb);
    if(fs->free == FREE_UNKNOWN) r = count_free(fs);         /* a read-only mount counts when asked */
    st->unit = fs->cluster_bytes;
    st->units = fs->nclusters;
    st->free_units = fs->free == FREE_UNKNOWN ? 0 : fs->free;
    snprintf(st->fs, sizeof(st->fs), "fat%i", fs->type);
    strlcpy(st->label, fs->label, sizeof(st->label));
    fat_unlock(sb);
    return r;
}

static const struct superblock_ops fat_sops = { fat_sync, fat_unmount, fat_statfs };

static int mount_volume(struct superblock *sb);
static int fat_mount(struct superblock *sb) {
    int r;
    fat_in();                                                /* 10/17/26 (B39): a kill that waited lets it finish */
    r = mount_volume(sb);
    fat_out();
    return r;
}
static int mount_volume(struct superblock *sb) {
    struct fat_fs *fs;
    struct fat_node *root;
    struct buf *b;
    const u8 *bs;
    u32 tot16, tot32, spf16, spf32, fsi = 0, hint_free = FREE_UNKNOWN, hint_next = 0;
    u8 bpb_label[11];
    bool clean = true, trusted;
    const char *from = "counted";

    if(!sb->dev) return -ENODEV;
    if(sb->dev->gone) return -EIO;
    b = bread(sb->dev, 0);
    if(!b) return -EIO;
    bs = b->data;
    if(!bpb_sane(bs)) { brelse(b); return -EINVAL; }
    fs = (struct fat_fs *)kcalloc(1, sizeof(struct fat_fs));
    root = (struct fat_node *)kcalloc(1, sizeof(struct fat_node));
    if(!fs || !root) { brelse(b); kfree(fs); kfree(root); return -ENOMEM; }
    fs->sb = sb;
    fs->dev = sb->dev;
    fs->spc = bs[13];
    fs->rsv = le16(bs + 14);
    fs->nfats = bs[16];
    fs->root_entries = le16(bs + 17);
    tot16 = le16(bs + 19);
    spf16 = le16(bs + 22);
    tot32 = le32(bs + 32);
    spf32 = le32(bs + 36);
    fs->total = tot16 ? tot16 : tot32;
    fs->spf = spf16 ? spf16 : spf32;
    fs->root_cluster = spf16 ? 0 : le32(bs + 44);
    memset(bpb_label, ' ', 11);
    if(spf16 && bs[38] == 0x29) memcpy(bpb_label, bs + 43, 11);          /* extended boot record: FAT12/16 */
    else if(!spf16 && bs[66] == 0x29) memcpy(bpb_label, bs + 71, 11);    /* and FAT32 */
    if(!spf16) fsi = le16(bs + 48);
    brelse(b);

    if(fs->total > sb->dev->nblocks) fs->total = sb->dev->nblocks;
    fs->root_sectors = (fs->root_entries * 32 + 511) / 512;
    fs->fat_lba = fs->rsv;
    fs->root_lba = fs->rsv + fs->nfats * fs->spf;
    fs->data_lba = fs->root_lba + fs->root_sectors;
    if(!fs->spf || fs->data_lba >= fs->total) { kfree(fs); kfree(root); return -EINVAL; }
    fs->nclusters = (fs->total - fs->data_lba) / fs->spc;
    fs->type = fs->nclusters < 4085 ? 12 : fs->nclusters < 65525 ? 16 : 32;
    if(fs->type == 32 && fs->root_cluster < 2) { kfree(fs); kfree(root); return -EINVAL; }
    fs->cluster_bytes = fs->spc * 512;
    fs->eoc = fs->type == 12 ? 0xFFF : fs->type == 16 ? 0xFFFF : 0x0FFFFFFF;
    fs->next_free = 2;
    fs->free = FREE_UNKNOWN;
    copy_label(fs->label, bpb_label);

    if(fs->type == 32 && fsi >= 1 && fsi < fs->rsv && (b = bread(sb->dev, fsi)) != NULL) {   /* FSInfo */
        if(le32(b->data) == FSINFO_LEAD && le32(b->data + 484) == FSINFO_STRUCT && le32(b->data + 508) == FSINFO_TRAIL) {
            fs->fsinfo = fsi;
            hint_free = le32(b->data + 488);
            hint_next = le32(b->data + 492);
        }
        brelse(b);
    }
    if(fs->type != 12 && (b = bread(sb->dev, fs->fat_lba)) != NULL) {     /* the clean-shutdown bit */
        clean = fs->type == 32 ? (le32(b->data + 4) & FAT32_CLEAN) != 0 : (le16(b->data + 2) & FAT16_CLEAN) != 0;
        brelse(b);
    }
    root->is_root = true;
    root->first = fs->type == 32 ? fs->root_cluster : 0;
    sb->priv = fs;
    read_root_label(fs, root);

    if(!clean) kprintf("+FAT%i: %s was not unmounted cleanly (check it on another computer if files look wrong)\n",
                       fs->type, fs->dev->name);
    fs->unclean = !clean;                                    /* 10/17/26 (B42): it stays so at unmount */
    if(!sb->readonly) {                                      /* mounted read-write: dirty until unmounted */
        if(fat_mark(fs, false) != 0 || bsync(fs->dev) != 0) {
            binvalidate(fs->dev);                            /* nothing of this attempt is written later */
            kfree(fs); kfree(root); sb->priv = NULL;
            return -EIO;
        }
        fs->marked = true;
    }
    trusted = fs->fsinfo && clean && hint_free <= fs->nclusters;
    if(hint_next >= 2 && hint_next < fs->nclusters + 2) fs->next_free = hint_next;
    if(trusted) { fs->free = hint_free; from = "FSInfo"; }
    else if(!sb->readonly && count_free(fs) != 0) { kfree(fs); kfree(root); sb->priv = NULL; return -EIO; }
    if(fs->free == FREE_UNKNOWN) from = "not counted yet";

    sb->ops = &fat_sops;
    sb->root = vnode_alloc(sb, &fat_vops, VN_DIR);
    if(!sb->root) { kfree(fs); kfree(root); sb->priv = NULL; return -ENOMEM; }
    sb->root->priv = root;
    kprintf("+FAT%i: %s: %s%s%s%lu clusters of %lu bytes, %lu FAT sectors x %lu, data at block %lu; ", fs->type, fs->dev->name,
            fs->label[0] ? "label " : "", fs->label, fs->label[0] ? ", " : "", (u_long)fs->nclusters, (u_long)fs->cluster_bytes,
            (u_long)fs->spf, (u_long)fs->nfats, (u_long)fs->data_lba);
    if(fs->free == FREE_UNKNOWN) kprintf("free space %s\n", from);
    else kprintf("%lu free (%s)\n", (u_long)fs->free, from);
    return 0;
}

static const struct fs_type fat_type = { "fat", fat_probe, fat_mount, NULL };

void fatfs_init(void) {
    vfs_register(&fat_type);
}
