/*
SurfOS FAT Checker
------------------
File: fat_check.c  Date: 11/01/26 (the command-line suite, track fsck)
------------------
11/02/26: the kernel command only looks. Two reviews of the repairs below found that they could
still lose data, so shell/fsckcmd.c refuses -r, gives this no write function and never asks it
to repair. The repairs stay, tested on the host (tests/host/test_fsck.c), for a later round: the
account (docs/superpowers/plans/2026-11-01-cli-suite-fsck.md) lists what that round must fix.
A check makes the kernel heap grow by CHECK_MEM_MOST at most, and the heap keeps what it once
grew to: so everything a look takes is taken at its start, the table of names too, sized once for
what the volume can hold (names_size()), and a volume that needs more is refused before anything is
taken (the third review: a table that doubled left its old copies behind it). FAT_CHECK_PLAN: a
look says what a repair would do; the kernel command does not ask for that.

fsck: a FAT12, FAT16 or FAT32 volume checked, and on request repaired. Pure: its reads,
writes, words and its one question go through struct fat_check_io (fs/fat_check.h), so
tests/host/test_fsck.c runs it over images with the driver (fs/fat.c) as a second reader.
A run that only looks gets no write function. The kernel stack is small and the volume may
be damaged on purpose: nothing here recurses, everything of any size is on the heap, and
every walk is bounded, by a bitmap of the clusters it has been to and a limit on its depth.

What is looked at. The boot sector and its BPB, the backup, FSInfo, whether the FATs agree,
the reserved entries, the clean bit; from the root every directory's entries, "." and "..",
long name sets (order, checksum), names, each chain (out of range, free or bad clusters,
loops, length against size), two names on one chain, clusters in use that nothing reaches,
and the free count. The FAT is read 64 KB at a time through a few windows, so 118 GB takes
no more memory than three bitmaps of one bit a cluster and those windows.

The rule for -r: when in doubt, it changes nothing. A look at the whole volume comes first,
and every doubt it meets goes through bar(): the run then writes nothing at all, FSInfo
included. Being clever about a doubt is how a repair comes to lose what a person could have
saved, so nothing here is. What bars repair:

  - the boot sector: its shape differs from its backup's, the FAT is far larger than its
    clusters need or holds something past its last entry (a flipped bit in the cluster size,
    or in the volume's), the reserved FAT entries are wrong, the FATs are not mirrored;
  - the FATs: a blank sector of the first where the second differs, a chain into a sector of
    zeros, or a place where they disagree about a chain (below);
  - a walk that is not whole: a directory it cannot read, one with no "." and ".." or a "."
    that names another cluster (it may be a file with its bit flipped, and a "repair" of an
    entry would write into the file), a chain of a directory with anything wrong with it, a
    depth past DEPTH_MOST, anything but zeros past a directory's end mark, a label with
    clusters, a directory with none;
  - two names on one chain, of files or directories, or a file running into a directory;
  - a chain into a cluster marked bad, an entry's first cluster that is no cluster, a FAT12
    entry across two sectors that needs changing (no one write can do that);
  - clusters no name reaches when they are most of what is in use, or when one of them
    starts as a directory: the tree has lost its top, and FILE0000.CHK is no way to find it.

What is repaired, when nothing bars it, one kind in each pass, the whole volume looked at
again between two: what a repair changes decides what the next may do.

  1. The second FAT from the first, sector by sector.
  2. A FAT entry made the end of its chain, and nothing else: a link to no cluster or back
     into its own chain ends where it stands, and a free cluster a chain runs into stays as
     that chain's last, because fs/fat.c reads it. Then an entry's size, when it runs past a
     chain that ends well.
  3. Lost chains that end well, saved as FOUND.nnn/FILEnnnn.CHK.
  4. FSInfo's count and hint, in a sector that has its signatures. The clean bit last, and
     only when no problem is left: only a check clears the mark (fs/fat.c, B42), and a check
     that leaves something has not.

Reported and left, with the rest going on: a chain longer than its size, names, long names,
"..", a directory's size, the boot sector's dirty flag, FSInfo with no signatures (it may be
no FSInfo, and fs/fat.c leaves such a sector alone too), a lost chain that runs into another
or into a bad cluster.

Nothing is ever deleted. No entry in use in the first FAT is set free; the second is the one
place a value goes, when the first is copied over it, and nothing reads the second. A
cluster is written only when both FATs call it free and no link or entry names it. A
directory's block is written only to change one entry's size or fill one free slot. An
entry's first cluster, the boot sector and its backup are never written.

A pull at any write leaves the volume no worse, on two assumptions about the stick: one sector
is written whole, and a flush means what it says. Each step's writes are in order, with a
flush wherever the next depends on the last:

  - the end of a chain goes to the first FAT, then the second; the size waits for the next
    pass, so a file reads the same bytes at every step;
  - FOUND.nnn is written whole into free clusters, marked, and named by one write of one
    root slot, so it has a name entirely or not at all;
  - the volume is marked as not cleanly unmounted before the first repair and the mark
    goes last, the second FAT's before the first's, which is the one other systems read.

A second run finishes what a pull interrupted, and that is what the first FAT's being written
first is for. When the FATs differ, they are disputed wherever a chain's clusters are concerned
and the second could be right: a bad link the second does not share; a first FAT that ends a
chain short of its size, or a directory's, where the second goes on; two different links. A
repair's own half-done write never looks like that: it ends chains only at links bad in both
FATs, so where it was stopped the second holds no cluster to go on to.

Known limits, neither of them harm: names that collide in names_put() take longer, and a chain
that crosses a FAT window at each step reads slowly. Both are bounded, and Ctrl+C stops them.
*/

#include <surfos/types.h>
#include <fs/fat_check.h>
#include <mm/kalloc.h>
#include <blibc_common.h>
#include <stdarg.h>

#define SECTOR      512
#define CHUNK       128             /* sectors of the FAT in a window: 64 KB */
#define WINDOWS     4
#define DIR_AHEAD   8               /* sectors of a directory read at once */
#define SLOTS_MAX   65536           /* fs/fat.c's DIR_MAX_ENTRIES: it reads no further, nor does this */
#define LFN_CHARS   13
#define LFN_PARTS   20              /* 260 characters; a name has 255 */
#define FIX_MAX     256             /* repairs planned in one pass; the next pass plans the rest */
#define LOST_MAX    1024            /* lost chains saved in one pass */
#define PASS_MAX    64              /* passes of one run */
#define SAY_MAX     40              /* problems said without -v */
#define DEPTH_MOST  128             /* levels of directories: a path of 256 bytes goes no deeper */
#define NAMES_FIRST 512             /* slots of the table of a directory's short names, at the least */
#define NAMES_MOST  (1UL << 17)     /* twice what a directory holds */
#define NAME_SLOT   12              /* a slot of it: the 11 bytes of a short name and the mark of the directory it is of */
#define ROOM_NAMES  1000            /* FOUND.000 to FOUND.999 */
#define CHECK_MEM_MOST (4UL << 20)  /* how far a check may make the kernel heap grow, which keeps what it grows to: the owner's
                                       118 GB stick with its 32 KB clusters takes 1.8 MB, and the table of names for a
                                       directory of 65536 of them 1.5 MB more. Of FAT32 with 32 KB clusters, 5.75 million
                                       clusters (188 GB) have that table whole, and 9.93 million (325 GB) fit at all
                                       (tests/host/test_fsck.c, names_at_cap(); tests/host/test_fsckheap.c, the heap itself) */
#define PIECE_EXTRA 32              /* mm/kalloc.c: a block's 16-byte header, and its size rounded up to 16 */
#define CHECK_HEAP_SLACK (68UL << 10)   /* mm/kalloc.c grows its heap 64 KB at least at a time, in whole 4 KB pages: what may
                                       be left over above the last piece of a check */

#define ATTR_LABEL   0x08
#define ATTR_DIR     0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN     0x0F
#define NT_LOWER_BASE 0x08
#define NT_LOWER_EXT  0x10

#define FSINFO_LEAD   0x41615252
#define FSINFO_STRUCT 0x61417272
#define FSINFO_TRAIL  0xAA550000
#define FSINFO_NONE   0xFFFFFFFFUL  /* "does not know", in either of its two fields */

/* how a chain ends */
#define LINK_END  0                 /* an end-of-chain mark */
#define LINK_OUT  1                 /* a link to no cluster */
#define LINK_FREE 2                 /* a link to a free cluster */
#define LINK_BADC 3                 /* a link to a cluster marked bad */
#define LINK_LOOP 4                 /* back into itself */
#define LINK_JOIN 5                 /* into a chain walked before */

/* whose chain is walked */
#define CH_FILE 0
#define CH_DIR  1
#define CH_LOST 2

/* the kinds of repair */
#define FX_END  0                   /* a FAT entry becomes the end of its chain */
#define FX_SIZE 1                   /* an entry's size */

/* why nothing is repaired */
#define BAR_WALK "the walk was not whole, so what it found cannot be taken to be all"
#define BAR_FATS "the FATs disagree about a chain: which is right is for a person to say"
#define BAR_BOOT "the boot sector cannot be trusted"
#define BAR_TWO  "two names on one chain"
#define BAR_LOST "most of what is in use is out of reach, or a directory is: the tree has lost its top"
#define BAR_LINK "a chain has a link no repair here can be sure of"

struct window {
    u8 *data;
    u32 base;                       /* the sector of the FAT it starts at */
    u32 age;
    u8 copy;
    bool valid;
};

/* one level of the walk */
struct frame {
    u32 first;                      /* its first cluster; 0: the fixed root of FAT12 and FAT16 */
    u32 parent;                     /* the first cluster of its parent; 0: the root */
    u32 nclus;                      /* clusters of its chain that are its own */
    u32 clus, left;                 /* the cluster it is in, and how many of its own follow */
    u32 lba, end;                   /* the sector it is in, and where this run of sectors ends */
    u32 idx;                        /* slots read */
    u16 off;                        /* the slot's offset in the sector */
    u16 plen;                       /* the path's length without this directory */
    u8 phase;                       /* 0: the entries; 1: into the directories among them */
    bool root;
    bool ended;                     /* it has no slot left in this look */
    bool past;                      /* past its end mark, where nothing should be */
};

/* a repair planned. An entry is known by where it is; its name, first cluster and size are compared before
   it is changed, and a FAT entry's value before that */
struct fix {
    u8 kind;
    u16 off;                        /* FX_SIZE: where the entry is */
    u32 lba;
    u8 name[11];
    u32 first;
    u32 size, oldsize;              /* FX_SIZE: the size it gets, and has */
    u32 cut, was;                   /* FX_END: the cluster that becomes the end, and what its entry holds now */
};

struct lost {
    u32 head, n;
};

struct chain {
    u32 n;                          /* clusters that are this chain's */
    u32 last;                       /* the last of them */
    u32 at;                         /* the value that ended it */
    int end;                        /* LINK_* */
};

struct fck {
    const struct fat_check_io *io;
    struct fat_check_result *res;
    bool verbose, naming, quiet;
    bool halt, stopped, failed;     /* halt: everything stops (stopped by the caller, failed: a device or memory) */
    /* the volume */
    int type;
    u32 spc, cshift, cb, rsv, nfats, spf, total, root_entries, root_lba, root_secs, data_lba, nclusters, end;
    u32 root_cluster, fat0, fat1, nsec, fat_bytes, eoc, eoc_min, badval, media, maxk, fsi;
    bool no_mirror, boot_dirty, fat_big;
    const char *boot_doubt;         /* what boot_load() found to doubt, for every look to say */
    u32 bits;                       /* the size of a bitmap, in bytes */
    u32 *reached, *dirhead, *aux;   /* been to; a directory starts there; joined, then has a lost predecessor */
    struct window win[WINDOWS];
    u32 age, lastw;
    u8 *chunk;                      /* the other FAT's window */
    u8 *clus;                       /* a cluster being made */
    u8 *dbuf;
    u32 dlba, dn;
    /* one walk */
    u32 found, stay, soft;          /* problems; of them what cannot be repaired; FSInfo and the clean bit */
    u32 free_now, badc, diff_secs, first_free;
    const char *bar;                /* why this look allows no repair; NULL: nothing bars it */
    bool unclean, fsi_wrong;
    u32 joins, named, c_end, c_size, c_lost;
    u32 ndirs, nfiles, shown;
    struct fix *fix;
    u32 nfix;
    struct lost *lost;
    u32 nlost;
    u32 lostc;
    u8 found_used[ROOM_NAMES / 8 + 1];
    /* the walk's place */
    struct frame *stack;
    u32 depth;
    u32 lfn_total, lfn_next;        /* parts of the set; the part it waits for */
    u8 lfn_sum;
    u8 *names;                      /* this directory's short names, an open hash table */
    u32 names_slots, names_used;    /* its slots, a power of two fixed for the check; the names of this directory in it */
    u8 names_mark;                  /* this directory's mark: a slot with another is empty */
    bool names_off;
    u32 names_lost;                 /* directories whose names could not all be compared: the check is not whole */
    u32 backup_bad;                 /* the backup boot sector, when it cannot be read */
    bool plan_words;                /* FAT_CHECK_PLAN */
    u16 lfn[LFN_PARTS * LFN_CHARS + 1];
    char name[LFN_PARTS * LFN_CHARS * 3 + 8];
    char path[256], who[320];
    char line[420];
    u8 sec[SECTOR], sec2[SECTOR];
};

/**** little helpers ****/

static u32 le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 le32(const u8 *p) { return le16(p) | (le16(p + 2) << 16); }
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static bool bit_get(const u32 *b, u32 i) { return (b[i >> 5] >> (i & 31)) & 1; }
static void bit_set(u32 *b, u32 i) { b[i >> 5] |= (u32)1 << (i & 31); }
static u32 min32(u32 a, u32 b) { return a < b ? a : b; }

static void say(struct fck *f, const char *fmt, ...) {
    va_list ap;
    if(f->quiet) return;
    va_start(ap, fmt);
    vsnprintf(f->line, sizeof(f->line), fmt, ap);
    va_end(ap);
    f->io->say(f->io->ctx, f->line);
}

/* a problem: counted, and said while there are few or -v asks. stay: no repair of this one exists */
static void problem(struct fck *f, bool stay, const char *fmt, ...) {
    va_list ap;
    if(f->naming) return;
    f->found++;
    if(stay) f->stay++;
    if(f->quiet || (!f->verbose && ++f->shown > SAY_MAX)) return;
    va_start(ap, fmt);
    vsnprintf(f->line, sizeof(f->line), fmt, ap);
    va_end(ap);
    f->io->say(f->io->ctx, f->line);
}

/* the first reason this look found to change nothing: a doubt is not argued with */
static void bar(struct fck *f, const char *why) {
    if(!f->naming && !f->bar) f->bar = why;
}

/* a problem that bars every repair */
#define doubt(f, why, ...) do { problem(f, true, __VA_ARGS__); bar(f, why); } while(0)

/* one that only says something about the volume and counts for nothing */
#define note(f, ...) do { if((f)->verbose) say(f, __VA_ARGS__); } while(0)

/* the caller wants this to end, and so does a device that failed */
static bool halted(struct fck *f) {
    if(!f->halt && f->io->stop && f->io->stop(f->io->ctx)) f->halt = f->stopped = true;
    return f->halt;
}

static void fail(struct fck *f) { f->halt = f->failed = true; }

/* a read that may be stopped: between two steps. 0 or -1 */
static int rd(struct fck *f, u32 lba, u32 count, void *buf) {
    if(halted(f) || lba + count < lba || lba + count > f->io->blocks) return -1;
    return f->io->read(f->io->ctx, lba, count, buf) ? -1 : 0;
}

/* a read inside a step, which ends the run when it fails: a step once begun goes to its end */
static int rd_in_step(struct fck *f, u32 lba, void *buf) {
    if(f->halt) return -1;
    if(f->io->read(f->io->ctx, lba, 1, buf) == 0) return 0;
    say(f, "block %lu cannot be read: stopped", (u_long)lba);
    fail(f);
    return -1;
}

/* every write of a run goes through here: what is kept of what it wrote goes with it. A write past the FATs leaves
   the windows alone */
static int wr(struct fck *f, u32 lba, u32 count, const void *buf) {
    u32 i;
    if(f->halt || !f->io->write) return -1;
    f->dn = 0;
    if(lba < f->root_lba) for(i = 0; i < WINDOWS; i++) f->win[i].valid = false;
    f->res->wrote = true;
    f->res->writes++;
    if(f->io->write(f->io->ctx, lba, count, buf) == 0) return 0;
    say(f, "block %lu cannot be written: stopped", (u_long)lba);
    fail(f);
    return -1;
}

/* the writes so far are on the medium before the next one is made */
static int barrier(struct fck *f) {
    if(f->halt) return -1;
    if(!f->io->flush || f->io->flush(f->io->ctx) == 0) return 0;
    say(f, "the device did not confirm its write: stopped");
    fail(f);
    return -1;
}

/**** the file allocation table ****/

static bool in_range(const struct fck *f, u32 c) { return c >= 2 && c < f->end; }
static u32 fat_off(const struct fck *f, u32 c) { return f->type == 12 ? c + c / 2 : f->type == 16 ? c * 2 : c * 4; }

/* one write changes this cluster's entry: what FAT12 cannot promise for an entry across two sectors */
static bool fat_atomic(const struct fck *f, u32 c) { return f->type != 12 || fat_off(f, c) % SECTOR != SECTOR - 1; }

/* the sector of a FAT copy (0: the one in use) in memory, read 64 KB around it */
static const u8 *fat_sector(struct fck *f, u32 copy, u32 sector) {
    u32 base = sector & ~(u32)(CHUNK - 1), i, pick = 0;
    struct window *l = &f->win[f->lastw];
    if(l->valid && l->base == base && l->copy == copy) return l->data + (sector - base) * SECTOR;
    for(i = 0; i < WINDOWS; i++) {
        struct window *w = &f->win[i];
        if(w->valid && w->base == base && w->copy == copy) {
            w->age = ++f->age;
            f->lastw = i;
            return w->data + (sector - base) * SECTOR;
        }
        if(!w->valid || w->age < f->win[pick].age) pick = i;
        if(!w->valid) break;
    }
    i = pick;
    f->win[i].valid = false;
    if(rd(f, (copy ? f->fat1 : f->fat0) + base, min32(CHUNK, f->spf - base), f->win[i].data) != 0) {
        if(!f->halt) {
            say(f, "the FAT cannot be read at block %lu: stopped", (u_long)((copy ? f->fat1 : f->fat0) + base));
            fail(f);
        }
        return NULL;
    }
    f->win[i].base = base;
    f->win[i].copy = (u8)copy;
    f->win[i].valid = true;
    f->win[i].age = ++f->age;
    f->lastw = i;
    return f->win[i].data + (sector - base) * SECTOR;
}

/* the entry of cluster c: 0, or -1 once the run has stopped */
static int fat_get(struct fck *f, u32 copy, u32 c, u32 *v) {
    u32 off = fat_off(f, c), o = off % SECTOR;
    const u8 *p = fat_sector(f, copy, off / SECTOR);
    if(!p) return -1;
    if(f->type == 12) {
        u32 x = p[o];
        if(o == SECTOR - 1) {
            if(!(p = fat_sector(f, copy, off / SECTOR + 1))) return -1;
            x |= (u32)p[0] << 8;
        } else x |= (u32)p[o + 1] << 8;
        *v = (c & 1) ? x >> 4 : x & 0xFFF;
    } else *v = f->type == 16 ? le16(p + o) : le32(p + o) & 0x0FFFFFFF;
    return 0;
}

/* n bytes all zero (1), all ones (2), or neither (0) */
static int blank(const u8 *p, u32 n) {
    u32 i;
    if(p[0] && p[0] != 0xFF) return 0;
    for(i = 1; i < n; i++) if(p[i] != p[0]) return 0;
    return p[0] ? 2 : 1;
}

/* whether the two copies differ, the free count, the reserved entries, in one sweep. A sector of the first FAT that
   is blank where the second is not is a torn or erased sector until shown otherwise; with one FAT there is nothing to
   compare it with, and a sector of all ones is what erased flash reads. What lies past the last entry bars repair
   without being a problem: a volume cut smaller is healthy, and a flipped bit in its size shows nowhere else */
static void fat_scan(struct fck *f) {
    u32 s, c, v, i, k, b0, b1, last = f->fat_bytes % SECTOR;
    bool tail = false;
    f->free_now = f->badc = f->diff_secs = f->first_free = 0;
    for(s = 0; s < f->nsec; s += k) {
        const u8 *a = fat_sector(f, 0, s);
        k = min32(CHUNK, f->nsec - s);
        if(!a) return;
        if(f->fat1 && rd(f, f->fat1 + s, k, f->chunk) != 0) {
            if(!f->halt) {
                say(f, "the second FAT cannot be read at block %lu: stopped", (u_long)(f->fat1 + s));
                fail(f);
            }
            return;
        }
        for(i = 0; i < k; i++) {
            const u8 *sec = a + i * SECTOR;
            u32 n = s + i == f->nsec - 1 && last ? last : SECTOR;     /* (what is past the last entry is nobody's) */
            bool differs = f->fat1 && memcmp(sec, f->chunk + i * SECTOR, n);
            int b = blank(sec, n);
            if(differs) f->diff_secs++;
            if(differs ? b != 0 : !f->fat1 && b == 2)
                doubt(f, BAR_FATS, "sector %lu of the first FAT is all %s", (u_long)(s + i), b == 2 ? "ones" : "zeros");
            if(n < SECTOR && blank(sec + n, SECTOR - n) != 1) tail = true;
        }
    }
    for(s = f->nsec; s < f->spf && !f->fat_big; s++) {
        const u8 *p = fat_sector(f, 0, s);
        if(!p) return;
        if(blank(p, SECTOR) != 1) tail = true;
    }
    if(tail) {
        note(f, "the FAT holds something past its last entry: the boot sector's size may be wrong");
        bar(f, BAR_BOOT);
    }
    for(c = 2; c < f->end; c++) {
        if(fat_get(f, 0, c, &v) != 0) return;
        if(!v) { f->free_now++; if(!f->first_free) f->first_free = c; }
        else if(v == f->badval) f->badc++;
    }
    if(fat_get(f, 0, 0, &b0) != 0 || fat_get(f, 0, 1, &b1) != 0) return;
    if(b0 != ((f->type == 12 ? 0xF00 : f->type == 16 ? 0xFF00 : 0x0FFFFF00) | f->media))
        doubt(f, BAR_BOOT, "the first FAT's entry 0 is %lx: it should repeat the media byte %lx", (u_long)b0, (u_long)f->media);
    if((b1 | (f->type == 16 ? 0xC000 : f->type == 32 ? 0x0C000000 : 0)) < f->eoc_min)
        doubt(f, BAR_BOOT, "the first FAT's entry 1 is %lx: no end-of-chain mark", (u_long)b1);
    if((f->type == 16 && !(b1 & 0x8000)) || (f->type == 32 && !(b1 & 0x08000000))) {
        problem(f, false, "the volume was not unmounted cleanly");
        f->soft++;
        f->unclean = true;
    }
    if((f->type == 16 && !(b1 & 0x4000)) || (f->type == 32 && !(b1 & 0x04000000)))
        note(f, "a system noted a disk error on this volume (the second bit of FAT entry 1)");
}

/**** FSInfo ****/

/* its free count against the FAT's. The next-free field is a hint nobody must believe, and is
   not a problem whatever it says; it is set right when the sector is written */
static void fsinfo_check(struct fck *f) {
    u32 v;
    f->fsi_wrong = false;
    if(!f->fsi) return;
    if(rd(f, f->fsi, 1, f->sec) != 0) {
        if(!f->halt) problem(f, true, "FSInfo (block %lu) cannot be read", (u_long)f->fsi);
        return;
    }
    if(le32(f->sec) != FSINFO_LEAD || le32(f->sec + 484) != FSINFO_STRUCT || le32(f->sec + 508) != FSINFO_TRAIL) {
        problem(f, true, "FSInfo (block %lu) has no valid signatures", (u_long)f->fsi);
        return;
    }
    v = le32(f->sec + 488);
    if(v != FSINFO_NONE && v != f->free_now) {
        problem(f, false, "FSInfo says %lu free clusters, the FAT has %lu", (u_long)v, (u_long)f->free_now);
        f->soft++;
        f->fsi_wrong = true;
    }
}

/**** the plan ****/

static struct fix *fix_add(struct fck *f, int kind) {
    struct fix *x;
    if(f->naming || f->nfix == FIX_MAX) return NULL;         /* (one that does not fit is the next pass's) */
    x = &f->fix[f->nfix++];
    memset(x, 0, sizeof(*x));
    x->kind = (u8)kind;
    return x;
}

/* cluster c's entry, value was, becomes the end of its chain */
static void end_fix(struct fck *f, u32 c, u32 was) {
    struct fix *x;
    if(f->naming) return;
    if(!fat_atomic(f, c)) {
        doubt(f, BAR_LINK, "cluster %lu's FAT12 entry spans two sectors: one write cannot change it", (u_long)c);
        return;
    }
    f->c_end++;
    if((x = fix_add(f, FX_END)) != NULL) { x->cut = c; x->was = was; }
}

/**** names ****/

static u32 entry_first(const struct fck *f, const u8 *p) {
    return le16(p + 26) | (f->type == 32 ? le16(p + 20) << 16 : 0);
}

static u8 lfn_checksum(const u8 *p) {
    u8 sum = 0;
    int i;
    for(i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + p[i]);
    return sum;
}

static u32 utf8_put(char *o, u32 cp) {
    if(cp < 0x80) { o[0] = (char)cp; return 1; }
    if(cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if(cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* The short name for people. A name comes off a stick nobody vouches for and goes to a terminal: what is not
   plain text, a control character and an escape above all, is a '?'. So in the long name */
static void name_short(struct fck *f, const u8 *p) {
    u32 i, n = 0, base = 8, ext = 3;
    char *o = f->name;
    while(base && p[base - 1] == ' ') base--;
    while(ext && p[8 + ext - 1] == ' ') ext--;
    for(i = 0; i < base; i++) {
        int c = p[i];
        if((p[12] & NT_LOWER_BASE) && c >= 'A' && c <= 'Z') c += 32;
        o[n++] = (char)(c < 0x20 || c >= 0x7F ? '?' : c);
    }
    if(ext) o[n++] = '.';
    for(i = 0; i < ext; i++) {
        int c = p[8 + i];
        if((p[12] & NT_LOWER_EXT) && c >= 'A' && c <= 'Z') c += 32;
        o[n++] = (char)(c < 0x20 || c >= 0x7F ? '?' : c);
    }
    o[n] = 0;
}

static void name_long(struct fck *f) {
    u32 i, o = 0, n = f->lfn_total * LFN_CHARS;
    for(i = 0; i < n; i++) {
        u32 cp = f->lfn[i];
        if(!cp || cp == 0xFFFF) break;
        if(cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && f->lfn[i + 1] >= 0xDC00 && f->lfn[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (f->lfn[i + 1] - 0xDC00);
            i++;
        } else if(cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        if(cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || cp == 0x061C || cp == 0x200E || cp == 0x200F || (cp >= 0x2028 && cp <= 0x202E) ||
           (cp >= 0x2066 && cp <= 0x2069))
            cp = '?';                                        /* (past C1: the marks and controls of direction, which can turn a name
                                                                round, and the separators of a line and a paragraph) */
        o += utf8_put(f->name + o, cp);
    }
    f->name[o] = 0;
}

static void who_set(struct fck *f) {
    snprintf(f->who, sizeof(f->who), "%s/%s", f->path, f->name);
}

static void lfn_clear(struct fck *f) { f->lfn_total = f->lfn_next = 0; }

/* what a set that stops here without its short entry says */
static void lfn_orphan(struct fck *f, const struct frame *fr) {
    if(f->lfn_total && !fr->phase) problem(f, true, "%s/: a long name stops before its short entry", f->path);
    lfn_clear(f);
}

static void lfn_part(struct fck *f, const struct frame *fr, const u8 *p) {
    static const u8 pos[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    u32 seq = p[0] & 0x1F, k;
    bool quiet = fr->phase != 0;
    if(p[0] & 0x40) {
        lfn_orphan(f, fr);
        if(!seq || seq > LFN_PARTS) {
            if(!quiet) problem(f, true, "%s/: a long name of %lu parts", f->path, (u_long)seq);
            return;
        }
        f->lfn_total = seq;
        f->lfn_next = seq;
        f->lfn_sum = p[13];
        memset(f->lfn, 0, sizeof(f->lfn));
    }
    /* (lfn_next 0: the set is whole, and a part more, of number 0 too, is no part: seq - 1 would index before lfn[]) */
    if(!f->lfn_total || !f->lfn_next || seq != f->lfn_next || p[13] != f->lfn_sum) {
        if(!quiet) problem(f, true, "%s/: a part of a long name out of order", f->path);
        lfn_clear(f);
        return;
    }
    if(!quiet && (p[12] || le16(p + 26))) problem(f, true, "%s/: a part of a long name with its reserved fields set", f->path);
    for(k = 0; k < LFN_CHARS; k++) f->lfn[(seq - 1) * LFN_CHARS + k] = (u16)le16(p + pos[k]);
    f->lfn_next--;
}

/* the name of the short entry at p, from the long name before it when that is whole; said when it is not */
static void entry_name(struct fck *f, const struct frame *fr, const u8 *p) {
    bool whole = f->lfn_total && !f->lfn_next && lfn_checksum(p) == f->lfn_sum;
    name_short(f, p);
    if(whole) {
        name_long(f);
        if(!f->name[0]) name_short(f, p);
    }
    who_set(f);
    if(f->lfn_total && !whole && !fr->phase) {
        if(f->lfn_next) problem(f, true, "%s: its long name lacks a part", f->who);
        else problem(f, true, "%s: its long name's checksum does not match", f->who);
    }
    lfn_clear(f);
}

/* fatgen103 on DIR_Name: its first byte may not be a space, and no byte may be below 0x20, but for 0x05 as the first, or one of
   the values below, in its order. DEL is not on its list: that one is here for being a control character like the rest. The
   driver's aliases never hold any of these (short_char_ok(), fs/fat.c) */
static bool short_name_bad(const u8 *p) {
    static const u8 banned[] = { 0x22, 0x2A, 0x2B, 0x2C, 0x2E, 0x2F, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x5B, 0x5C, 0x5D, 0x7C, 0x7F };
    u32 i, k;
    if(p[0] == 0x20) return true;
    for(i = 0; i < 11; i++) {
        if(p[i] < 0x20 && (i || p[i] != 0x05)) return true;
        for(k = 0; k < sizeof(banned); k++) if(p[i] == banned[k]) return true;
    }
    return false;
}

/* A short name seen before in this directory: an open hash table, taken once for the whole check at the size this
   volume's directories can need (names_size()). A directory is read through once for names, and the table is its alone:
   the walk goes into the directories under it after that. Each slot has the mark of the directory that filled it, so a
   new directory empties the table by taking a new mark; every 255 directories the marks are cleared */
static void names_reset(struct fck *f) {
    u32 i;
    f->names_used = 0;
    f->names_off = false;
    if(++f->names_mark == 0) {
        for(i = 0; i < f->names_slots; i++) f->names[i * NAME_SLOT + 11] = 0;
        f->names_mark = 1;
    }
}

static u32 names_hash(const u8 *p) {
    u32 h = 2166136261UL, i;
    for(i = 0; i < 11; i++) h = (h ^ p[i]) * 16777619UL;
    return h;
}

/* p into the table, which is never more than half full; false when the name is in it already */
static bool names_put(struct fck *f, const u8 *p) {
    u8 *t = f->names;
    u32 i = names_hash(p) & (f->names_slots - 1);
    while(t[i * NAME_SLOT + 11] == f->names_mark) {
        if(!memcmp(t + i * NAME_SLOT, p, 11)) return false;
        i = (i + 1) & (f->names_slots - 1);
    }
    memcpy(t + i * NAME_SLOT, p, 11);
    t[i * NAME_SLOT + 11] = f->names_mark;
    return true;
}

/* FOUND.nnn taken, by a short name or a long one: both are what a lookup finds */
static void found_note(struct fck *f, const u8 *p) {
    const char *n = f->name;
    u32 i, k = 0;
    if(!memcmp(p, "FOUND   ", 8) && p[8] >= '0' && p[8] <= '9' && p[9] >= '0' && p[9] <= '9' && p[10] >= '0' && p[10] <= '9')
        k = (p[8] - '0') * 100 + (p[9] - '0') * 10 + (p[10] - '0') + 1;
    if(k) f->found_used[(k - 1) >> 3] |= (u8)(1 << ((k - 1) & 7));
    for(i = 0; i < 5; i++) if((n[i] | 0x20) != "found"[i]) return;
    if(n[5] != '.' || n[6] < '0' || n[6] > '9' || n[7] < '0' || n[7] > '9' || n[8] < '0' || n[8] > '9' || n[9]) return;
    k = (n[6] - '0') * 100 + (n[7] - '0') * 10 + (n[8] - '0');
    f->found_used[k >> 3] |= (u8)(1 << (k & 7));
}

/**** chains ****/

/* is c among the first n clusters of the chain from first? */
static bool chain_has(struct fck *f, u32 first, u32 n, u32 c) {
    u32 i, v, x = first;
    for(i = 0; i < n; i++) {
        if(x == c) return true;
        if(fat_get(f, 0, x, &v) != 0) return false;
        x = v;
    }
    return false;
}

/* the FATs disagree about a chain */
static void disputed(struct fck *f, u32 c, u32 first, u32 second) {
    doubt(f, BAR_FATS, "%s: at cluster %lu the first FAT says %lx, the second %lx", f->who, (u_long)c, (u_long)first, (u_long)second);
}

/* a bad link the second FAT must share, or the second may know better (the head comment) */
static void agree(struct fck *f, u32 c, u32 value) {
    u32 v;
    if(!f->diff_secs || f->naming) return;
    if(fat_get(f, 1, c, &v) == 0 && v != value) disputed(f, c, value, v);
}

/* The FATs differ, and this chain holds c, value v in the first, ch->n clusters so far. That is no matter while the second
   has no cluster to go to. When it has: the first going on elsewhere is two chains, and the first ending short of the file's
   size or a directory's chain is the second knowing more, unless the second only goes back into the chain (a loop's cut, half
   done). Past the size, what the second goes on to is nobody's */
static void fats_compare(struct fck *f, u32 first, const struct chain *ch, u32 need, int kind, u32 c, u32 v) {
    u32 w;
    if(fat_get(f, 1, c, &w) != 0 || w == v || !in_range(f, w)) return;
    if(v >= f->eoc_min && ((kind == CH_FILE && ch->n >= need) || chain_has(f, first, ch->n, w))) return;
    disputed(f, c, v, w);
}

/* Walk the chain from first, marking each cluster as reached, to its first bad link or the first cluster marked
   before. A free cluster it runs into is marked too: fs/fat.c reads it as the chain's last, so it is taken by nothing.
   need is a file's size in clusters. What a chain walk meets is its caller's to say */
static void chain_walk(struct fck *f, u32 first, u32 need, int kind, struct chain *ch) {
    u32 c = first, v, steps = 0;
    bool said = false, named = kind != CH_LOST;
    ch->n = 0;
    ch->last = 0;
    ch->at = first;
    ch->end = LINK_END;
    for(;;) {
        if(!(++steps & 4095) && halted(f)) return;
        if(!in_range(f, c)) {
            ch->end = LINK_OUT;
            ch->at = c;
            if(ch->last && named) agree(f, ch->last, c);
            return;
        }
        if(fat_get(f, 0, c, &v) != 0) return;
        if(!v || v == f->badval) {
            ch->end = v ? LINK_BADC : LINK_FREE;
            ch->at = c;
            if(named) {
                if(ch->last) agree(f, ch->last, c);
                agree(f, c, v);
            }
            if(!v && bit_get(f->reached, c)) ch->end = LINK_JOIN;  /* (a free cluster two chains run into) */
            if(!v) bit_set(f->reached, c);
            return;
        }
        if(f->naming && !said && bit_get(f->aux, c)) {
            said = true;
            if(f->named++ < SAY_MAX) {
                f->quiet = false;
                say(f, "%s holds cluster %lu", f->who, (u_long)c);
                f->quiet = true;
            }
        }
        if(bit_get(f->reached, c)) {
            ch->at = c;
            ch->end = chain_has(f, first, ch->n, c) ? LINK_LOOP : LINK_JOIN;
            if(ch->last && named) agree(f, ch->last, c);
            return;
        }
        bit_set(f->reached, c);
        ch->n++;
        ch->last = c;
        if(named && f->diff_secs && !f->naming) fats_compare(f, first, ch, need, kind, c, v);
        if(v >= f->eoc_min) return;
        c = v;
    }
}

/* A chain ran into free cluster c. One entry of a sector gone to zero is a link to look at; the whole sector is the FAT
   gone, torn or erased. Where there is a second FAT fat_scan() has seen that; with one there is nothing else to see it by */
static void free_end(struct fck *f, u32 c) {
    u32 s = fat_off(f, c) / SECTOR;
    const u8 *p = fat_sector(f, 0, s);
    if(p && blank(p, s == f->nsec - 1 && f->fat_bytes % SECTOR ? f->fat_bytes % SECTOR : SECTOR) == 1)
        doubt(f, BAR_FATS, "%s: a chain runs into cluster %lu, in a sector of the FAT that is all zeros", f->who, (u_long)c);
}

static const char *link_words(int end) {
    return end == LINK_OUT ? "no cluster" : end == LINK_FREE ? "free" : end == LINK_BADC ? "marked bad" :
           end == LINK_LOOP ? "earlier in the same chain" : "another chain";
}

static int starts_as_dir(struct fck *f, u32 c);

/* the entry at p, in block lba, gets a new size */
static void size_fix(struct fck *f, const u8 *p, u32 lba, u32 off, u32 size) {
    struct fix *x;
    f->c_size++;
    if((x = fix_add(f, FX_SIZE)) == NULL) return;
    x->lba = lba;
    x->off = (u16)off;
    memcpy(x->name, p, 11);
    x->first = entry_first(f, p);
    x->oldsize = le32(p + 28);
    x->size = size;
}

/* the short entry of a file at p, in block lba: its chain against its size */
static void file_check(struct fck *f, const u8 *p, u32 lba, u32 off) {
    u32 first = entry_first(f, p), size = le32(p + 28), need = size ? ((size - 1) >> f->cshift) + 1 : 0;
    struct chain ch;
    if(!first) {
        if(size && !f->naming) {
            problem(f, false, "%s: size %lu but no clusters", f->who, (u_long)size);
            size_fix(f, p, lba, off, 0);
        }
        return;
    }
    chain_walk(f, first, need, CH_FILE, &ch);
    if(f->halt || f->naming) return;
    if(ch.end == LINK_END) {
        if(ch.n < need) {
            problem(f, false, "%s: size %lu, but its chain holds %lu bytes", f->who, (u_long)size, (u_long)(ch.n << f->cshift));
            size_fix(f, p, lba, off, ch.n << f->cshift);
        } else if(ch.n > need) {
            /* left as it is. With no size at all it may be a directory that lost its bit, and all under it out of reach */
            int d = need ? 0 : starts_as_dir(f, first);
            if(f->halt) return;
            if(d) doubt(f, BAR_WALK, d > 0 ? "%s: size 0 on what looks like a directory: not walked" : "%s: its first block cannot be read", f->who);
            else problem(f, true, "%s: size %lu, but its chain goes on past %lu cluster%s", f->who, (u_long)size, (u_long)need, need == 1 ? "" : "s");
        }
    } else if(ch.end == LINK_JOIN) {
        bit_set(f->aux, ch.at);
        f->joins++;
        doubt(f, BAR_TWO, "%s: shares cluster %lu with another name", f->who, (u_long)ch.at);
    } else if(ch.end == LINK_BADC) {
        doubt(f, BAR_LINK, "%s: its chain runs into cluster %lu, marked bad", f->who, (u_long)ch.at);
    } else if(ch.end == LINK_FREE) {
        /* fs/fat.c reads that cluster as the chain's last: it stays, as the end */
        problem(f, false, "%s: cluster %lu of its chain is free: it stays, as the chain's end", f->who, (u_long)ch.at);
        free_end(f, ch.at);
        end_fix(f, ch.at, 0);
    } else if(!ch.n) {
        doubt(f, BAR_LINK, "%s: its first cluster %lu is %s", f->who, (u_long)first, link_words(ch.end));
    } else {
        problem(f, false, "%s: cluster %lu links to %lu, %s", f->who, (u_long)ch.last, (u_long)ch.at, link_words(ch.end));
        end_fix(f, ch.last, ch.at);
    }
}

/**** the directory tree ****/

static int frame_push(struct fck *f, u32 first, u32 parent, u32 nclus, bool root, u32 plen) {
    struct frame *fr;
    if(f->depth == DEPTH_MOST) {
        doubt(f, BAR_WALK, "%s: more than %u levels of directories: not walked", f->who, DEPTH_MOST);
        return -1;
    }
    fr = &f->stack[f->depth++];
    memset(fr, 0, sizeof(*fr));
    fr->first = first;
    fr->parent = parent;
    fr->nclus = nclus;
    fr->root = root;
    fr->plen = (u16)plen;
    return 0;
}

/* to the directory's first slot */
static void frame_rewind(struct fck *f, struct frame *fr) {
    fr->idx = 0;
    fr->off = 0;
    if(fr->first) {
        fr->clus = fr->first;
        fr->left = fr->nclus - 1;
        fr->lba = f->data_lba + (fr->clus - 2) * f->spc;
        fr->end = fr->lba + f->spc;
    } else {
        fr->lba = f->root_lba;
        fr->end = f->root_lba + f->root_secs;
    }
}

/* the next slot of the directory's own space: 0, 1 at its end, -1 when the run stops */
static int frame_next(struct fck *f, struct frame *fr) {
    u32 v;
    fr->idx++;
    fr->off += 32;
    if(fr->off < SECTOR) return 0;
    fr->off = 0;
    if(++fr->lba < fr->end) return 0;
    if(!fr->left) return 1;
    if(fat_get(f, 0, fr->clus, &v) != 0) return -1;
    fr->clus = v;
    fr->left--;
    fr->lba = f->data_lba + (v - 2) * f->spc;
    fr->end = fr->lba + f->spc;
    return 0;
}

/* a sector of a directory, up to DIR_AHEAD of them read at once; NULL when it cannot be read */
static const u8 *dir_sector(struct fck *f, u32 lba, u32 end) {
    u32 n = min32(end - lba, DIR_AHEAD);
    if(f->dn && lba >= f->dlba && lba < f->dlba + f->dn) return f->dbuf + (lba - f->dlba) * SECTOR;
    f->dn = 0;
    if(rd(f, lba, n, f->dbuf) != 0 && (n == 1 || rd(f, lba, n = 1, f->dbuf) != 0)) return NULL;
    f->dlba = lba;
    f->dn = n;
    return f->dbuf;
}

/* the short entry of a directory, on the first look through its parent */
static void dir_entry_check(struct fck *f, const u8 *p) {
    if(le32(p + 28)) problem(f, true, "%s: a directory with a size (%lu)", f->who, (u_long)le32(p + 28));
    if(!entry_first(f, p)) doubt(f, BAR_WALK, "%s: a directory with no cluster", f->who);
}

/* "." and ".." where they stand (dir_enter() looked that they begin the directory): true when p is one */
static bool dots_check(struct fck *f, const struct frame *fr, const u8 *p) {
    bool dot = !memcmp(p, ".          ", 11), dotdot = !memcmp(p, "..         ", 11);
    if(!dot && !dotdot) return false;
    if(fr->root) problem(f, true, "the root directory holds a \"%s\" entry", dot ? "." : "..");
    else if(fr->idx != (dot ? 0 : 1)) problem(f, true, "%s/: a \"%s\" entry at slot %lu", f->path, dot ? "." : "..", (u_long)fr->idx);
    else if(dot && entry_first(f, p) != fr->first)         /* this block may be no block of this directory */
        doubt(f, BAR_WALK, "%s/: its \".\" entry names cluster %lu, not %lu", f->path, (u_long)entry_first(f, p), (u_long)fr->first);
    else if(dotdot && entry_first(f, p) != fr->parent)
        problem(f, true, "%s/: its \"..\" entry names cluster %lu, not %lu", f->path, (u_long)entry_first(f, p), (u_long)fr->parent);
    return true;
}

/* one slot on the first look through a directory: everything but going into what it names */
static void slot_look(struct fck *f, struct frame *fr, const u8 *p, u32 lba) {
    if(p[0] == 0xE5) { lfn_orphan(f, fr); return; }
    if((p[11] & ATTR_LFN) == ATTR_LFN) { lfn_part(f, fr, p); return; }
    if(p[11] & ATTR_LABEL) {
        lfn_orphan(f, fr);
        if(!fr->root) problem(f, true, "%s/: a volume label in a directory that is not the root", f->path);
        if(entry_first(f, p) || le32(p + 28)) doubt(f, BAR_WALK, "%s/: a volume label with clusters or a size", f->path);
        return;
    }
    if(dots_check(f, fr, p)) { lfn_clear(f); return; }
    entry_name(f, fr, p);
    if(fr->root) found_note(f, p);
    if(short_name_bad(p)) problem(f, true, "%s: a short name with forbidden characters", f->who);
    if(!f->names_off) {
        if(f->names_used * 2 >= f->names_slots) {           /* (only when the cap made the table smaller than the volume needs) */
            f->names_off = true;                             /* a second entry of one name is not looked for there */
            if(!f->naming) f->names_lost++;
            note(f, "%s/: more names than a check may compare", f->path);
        }
        if(!f->names_off) {
            if(!names_put(f, p)) problem(f, true, "%s: a second entry with this short name", f->who);
            else f->names_used++;
        }
    }
    if(p[11] & ATTR_DIR) { dir_entry_check(f, p); return; }
    f->nfiles++;
    {
        u8 raw[32];                                          /* (file_check() may read another sector over p) */
        memcpy(raw, p, 32);
        file_check(f, raw, lba, fr->off);
    }
}

/* does the cluster start as a directory does, with "." and ".."? 1 yes, 0 no, -1 it cannot be read */
static int starts_as_dir(struct fck *f, u32 c) {
    u32 lba = f->data_lba + (c - 2) * f->spc;
    const u8 *p = dir_sector(f, lba, lba + f->spc);
    if(!p) return -1;
    return !memcmp(p, ".          ", 11) && (p[11] & ATTR_DIR) && !memcmp(p + 32, "..         ", 11) && (p[32 + 11] & ATTR_DIR);
}

/* the directory at c is a FOUND.nnn of this file's that a pull left unnamed: it has no parent but the root, and its first
   entry is the name fix_lost() gives (the sector is the one starts_as_dir() just read) */
static bool found_orphan(struct fck *f, u32 c) {
    u32 lba = f->data_lba + (c - 2) * f->spc;
    const u8 *p = dir_sector(f, lba, lba + f->spc);
    return p && entry_first(f, p) == c && !entry_first(f, p + 32) && !memcmp(p + 64, "FILE0000CHK", 11);
}

/* The directory at p in the second look through its parent: its chain, then the walk goes in. 1 when it did.
   Its files have then been walked once, however many names it has, unless it cannot be walked at all */
static int dir_enter(struct fck *f, const struct frame *fr, const u8 *p) {
    u32 first = entry_first(f, p), parent = fr->root ? 0 : fr->first, plen = (u32)strlen(f->path);
    struct chain ch;
    int d;
    if(!first) return 0;
    if(in_range(f, first) && bit_get(f->dirhead, first)) {
        doubt(f, BAR_TWO, "%s: a second name of a directory", f->who);
        return 0;
    }
    chain_walk(f, first, 0, CH_DIR, &ch);
    if(f->halt) return 0;
    if(ch.end == LINK_JOIN && !f->naming) {
        bit_set(f->aux, ch.at);
        f->joins++;
    }
    /* no bad link of a directory is cut: past it may be entries, and the driver may read them */
    if(ch.end != LINK_END) doubt(f, BAR_WALK, "%s: a directory whose chain runs into cluster %lu, %s", f->who, (u_long)ch.at, link_words(ch.end));
    if(!ch.n) return 0;
    d = starts_as_dir(f, first);
    if(f->halt) return 0;
    if(d != 1) {
        doubt(f, BAR_WALK, d ? "%s: no \".\" and \"..\" at its start: it may be no directory, and is not walked" :
                               "%s: its first block cannot be read", f->who);
        return 0;
    }
    bit_set(f->dirhead, first);
    if(!f->naming) f->ndirs++;
    if(frame_push(f, first, parent, ch.n, false, plen) != 0) return 0;
    snprintf(f->path + plen, sizeof(f->path) - plen, "/%s", f->name);
    return 1;
}

/* the root, as the walk's first level. -1: it cannot be walked */
static int root_push(struct fck *f) {
    struct chain ch;
    f->ndirs = 1;
    f->who[0] = 0;
    f->path[0] = 0;
    if(!f->root_cluster) return frame_push(f, 0, 0, 0, true, 0);
    snprintf(f->who, sizeof(f->who), "/");
    chain_walk(f, f->root_cluster, 0, CH_DIR, &ch);
    if(f->halt) return -1;
    if(ch.end != LINK_END) doubt(f, BAR_WALK, "the root directory's chain runs into cluster %lu, %s", (u_long)ch.at, link_words(ch.end));
    if(!ch.n) return -1;
    bit_set(f->dirhead, f->root_cluster);
    return frame_push(f, f->root_cluster, 0, ch.n, true, 0);
}

/* the top level of the walk, until it ends or a directory under it begins */
static void dir_step(struct fck *f) {
    struct frame *fr = &f->stack[f->depth - 1];
    if(!fr->lba) {
        frame_rewind(f, fr);
        names_reset(f);
        lfn_clear(f);
    }
    while(!fr->ended) {
        const u8 *sec, *p;
        u32 lba = fr->lba;
        int r;
        if(!(fr->idx & 255) && halted(f)) return;
        if(fr->idx >= SLOTS_MAX) {
            if(!fr->phase && !fr->past) doubt(f, BAR_WALK, "%s/: more than %u slots: the rest is not read", f->path, SLOTS_MAX);
            break;
        }
        sec = dir_sector(f, fr->lba, fr->end);
        if(!sec) {
            if(f->halt) return;
            if(!fr->phase) doubt(f, BAR_WALK, "%s/: block %lu cannot be read", f->path, (u_long)fr->lba);
            break;
        }
        p = sec + fr->off;
        if(!p[0] && !fr->past) {
            lfn_orphan(f, fr);
            if(fr->phase) break;
            fr->past = true;                                 /* on, to see that nothing is there */
        }
        if(fr->past) {
            if(blank(p, 32) != 1) {
                doubt(f, BAR_WALK, "%s/: entries past the end mark at slot %lu: a zeroed slot may have cut the directory short", f->path, (u_long)fr->idx);
                break;
            }
        } else if(!fr->phase) slot_look(f, fr, p, lba);
        else if(p[0] == 0xE5) lfn_clear(f);
        else if((p[11] & ATTR_LFN) == ATTR_LFN) lfn_part(f, fr, p);
        else if((p[11] & ATTR_LABEL) || !(p[11] & ATTR_DIR) || !memcmp(p, ".          ", 11) || !memcmp(p, "..         ", 11))
            lfn_clear(f);
        else {
            u8 raw[32];                                      /* (going in reads other sectors) */
            memcpy(raw, p, 32);
            entry_name(f, fr, raw);
            r = frame_next(f, fr);
            if(r < 0) return;
            fr->ended = r > 0;
            if(dir_enter(f, fr, raw)) return;                /* this level goes on where it is when that one ends */
            if(f->halt) return;
            continue;
        }
        if(f->halt) return;
        r = frame_next(f, fr);
        if(r < 0) return;
        if(r) {
            lfn_orphan(f, fr);
            fr->ended = true;
        }
    }
    if(!fr->phase) {                                         /* the second look: the walk goes into what it holds */
        fr->phase = 1;
        fr->ended = fr->past = false;
        frame_rewind(f, fr);
        lfn_clear(f);
        return;
    }
    f->path[fr->plen] = 0;
    f->depth--;
}

static void dir_walk(struct fck *f) {
    f->depth = 0;
    if(root_push(f) != 0) return;
    while(f->depth && !f->halt) dir_step(f);
}

/**** lost chains ****/

/* A chain no name reaches. One that ends well is saved in FOUND. A link to no cluster or to a free one is ended first, as a
   named chain's, and it is saved in the next pass; one that runs into something else, or is marked bad, stays as it is. A
   directory among them means a tree that has lost its top: nothing is done to it, but for a FOUND.nnn a pull left unnamed */
static void lost_chain(struct fck *f, u32 head) {
    struct chain ch;
    int d;
    chain_walk(f, head, 0, CH_LOST, &ch);
    if(f->halt) return;
    d = starts_as_dir(f, head);
    if(f->halt) return;
    if(d > 0 && found_orphan(f, head)) d = 0;
    f->who[0] = 0;
    if(d) doubt(f, BAR_LOST, "cluster %lu starts as a directory, and no name reaches it", (u_long)head);
    if(ch.end == LINK_JOIN || ch.end == LINK_BADC || ch.n > f->maxk) {
        problem(f, true, "%lu cluster%s from cluster %lu on that no name reaches, running into %s", (u_long)ch.n, ch.n == 1 ? "" : "s",
                (u_long)head, ch.n > f->maxk ? "more than a file can hold" : link_words(ch.end));
        return;
    }
    problem(f, false, "%lu cluster%s from cluster %lu on that no name reaches", (u_long)ch.n, ch.n == 1 ? "" : "s", (u_long)head);
    f->c_lost++;
    if(ch.end == LINK_FREE) {
        free_end(f, ch.at);
        end_fix(f, ch.at, 0);
    } else if(ch.end != LINK_END) end_fix(f, ch.last, ch.at);
    else if(f->nlost < LOST_MAX) {
        f->lost[f->nlost].head = head;
        f->lost[f->nlost++].n = ch.n;
    }
}

/* clusters in use the walk did not reach: their chains, from the ones nothing lost points at, then the rings */
static void lost_walk(struct fck *f) {
    u32 c, v, pass, in_use = f->nclusters - f->free_now - f->badc;
    memset(f->aux, 0, f->bits);
    f->lostc = 0;
    for(c = 2; c < f->end; c++) {
        if(fat_get(f, 0, c, &v) != 0) return;
        if(!v || v == f->badval || bit_get(f->reached, c)) continue;
        f->lostc++;
        if(in_range(f, v) && !bit_get(f->reached, v)) bit_set(f->aux, v);
    }
    if(!f->lostc) return;
    if(f->lostc >= in_use - f->lostc) doubt(f, BAR_LOST, "%lu of the %lu clusters in use are out of reach", (u_long)f->lostc, (u_long)in_use);
    else if(f->bar) problem(f, true, "%lu clusters in use that no name reaches", (u_long)f->lostc);
    if(f->bar) return;
    for(pass = 0; pass < 2; pass++) {
        for(c = 2; c < f->end; c++) {
            if(fat_get(f, 0, c, &v) != 0) return;
            if(!v || v == f->badval || bit_get(f->reached, c)) continue;
            if(!pass && bit_get(f->aux, c)) continue;
            lost_chain(f, c);
            if(f->halt) return;
        }
    }
}

/**** one look at the whole volume ****/

static void walk(struct fck *f, bool first_look) {
    f->found = f->stay = f->soft = 0;
    f->bar = NULL;
    f->unclean = false;
    f->joins = f->c_end = f->c_size = f->c_lost = 0;
    f->names_lost = 0;
    f->ndirs = f->nfiles = 0;
    f->nfix = f->nlost = 0;
    memset(f->found_used, 0, sizeof(f->found_used));
    if(f->boot_doubt) doubt(f, BAR_BOOT, "the boot sector: %s", f->boot_doubt);
    if(f->backup_bad) problem(f, true, "the backup boot sector (block %lu) cannot be read", (u_long)f->backup_bad);
    if(f->fat_big) {
        note(f, "the FAT is far larger than its clusters need: the boot sector's cluster size may be wrong");
        bar(f, BAR_BOOT);
    }
    if(f->no_mirror) {
        note(f, "the FATs are not mirrored: SurfOS's driver does not know that mode");
        bar(f, "its FATs are not mirrored");
    }
    if(f->boot_dirty) problem(f, true, "the boot sector's own dirty flag is set");
    fat_scan(f);
    if(f->halt) return;
    fsinfo_check(f);
    if(f->diff_secs) problem(f, false, "the two FATs differ in %lu sector%s", (u_long)f->diff_secs, f->diff_secs == 1 ? "" : "s");
    memset(f->reached, 0, f->bits);
    memset(f->dirhead, 0, f->bits);
    memset(f->aux, 0, f->bits);
    dir_walk(f);
    if(f->halt) return;
    if(first_look && f->joins && !f->quiet) {                /* who the others are: only now is it known what to look for */
        u32 ndirs = f->ndirs, nfiles = f->nfiles;
        memset(f->reached, 0, f->bits);
        memset(f->dirhead, 0, f->bits);
        f->named = 0;
        f->naming = f->quiet = true;
        dir_walk(f);
        f->naming = f->quiet = false;
        f->ndirs = ndirs;
        f->nfiles = nfiles;
        if(f->halt) return;
    }
    lost_walk(f);
}

/**** repairs: the writes ****/

/* the entry of cluster c in a copy of its FAT sector, the neighbours as they are */
static void fat_poke(const struct fck *f, u8 *sec, u32 c, u32 v) {
    u32 o = fat_off(f, c) % SECTOR;
    if(f->type == 12) {
        if(c & 1) { sec[o] = (u8)((sec[o] & 0x0F) | ((v << 4) & 0xF0)); sec[o + 1] = (u8)(v >> 4); }
        else { sec[o] = (u8)v; sec[o + 1] = (u8)((sec[o + 1] & 0xF0) | ((v >> 8) & 0x0F)); }
    } else if(f->type == 16) put16(sec + o, v);
    else put32(sec + o, (le32(sec + o) & 0xF0000000) | v);
}

/* the clusters c[0..m-1], ascending, become a chain ending in tail: the first FAT, then the second, each from the tail
   on, so that at any write what has been written is a chain with a proper end, and one sector of each FAT at a time.
   One cluster and the end mark is the end of a chain. 0, or -1 */
static int fat_store(struct fck *f, const u32 *c, u32 m, u32 tail) {
    u32 copy, i;
    for(copy = 0; copy < 2; copy++) {
        u32 base = copy ? f->fat1 : f->fat0;
        if(!base) continue;
        for(i = m; i; ) {
            u32 s = fat_off(f, c[i - 1]) / SECTOR;
            if(rd_in_step(f, base + s, f->sec) != 0) return -1;
            while(i && fat_off(f, c[i - 1]) / SECTOR == s) {
                fat_poke(f, f->sec, c[i - 1], i == m ? tail : c[i]);
                i--;
            }
            if(wr(f, base + s, 1, f->sec) != 0) return -1;
        }
        if(barrier(f) != 0) return -1;
    }
    return 0;
}

/* the entry as the look saw it: nothing else may be changed. The device is held and nothing else writes, so a
   difference ends the repair */
static u8 *entry_read(struct fck *f, const struct fix *x) {
    u8 *p = f->sec2 + x->off;
    if(rd_in_step(f, x->lba, f->sec2) != 0) return NULL;
    if(memcmp(p, x->name, 11) || entry_first(f, p) != x->first || le32(p + 28) != x->oldsize) {
        say(f, "an entry in block %lu is not what the look found: stopped", (u_long)x->lba);
        fail(f);
        return NULL;
    }
    return p;
}

/* a FAT entry becomes the end of its chain */
static int fix_end(struct fck *f, const struct fix *x) {
    u32 v;
    if(fat_get(f, 0, x->cut, &v) != 0) return -1;
    if(v != x->was) {
        say(f, "cluster %lu's FAT entry is not what the look found: stopped", (u_long)x->cut);
        fail(f);
        return -1;
    }
    return fat_store(f, &x->cut, 1, f->eoc);
}

/* an entry's size */
static int fix_size(struct fck *f, const struct fix *x) {
    u8 *p = entry_read(f, x);
    if(!p) return -1;
    put32(p + 28, x->size);
    if(wr(f, x->lba, 1, f->sec2) != 0 || barrier(f) != 0) return -1;
    return 0;
}

/* count clusters into list, ascending: free, with no link or entry that names them (those are marked as reached), and each
   with a FAT entry one write can change. 0, or -1 */
static int take_free(struct fck *f, u32 *list, u32 count) {
    u32 c, v, n = 0;
    for(c = 2; c < f->end && n < count; c++) {
        if(fat_get(f, 0, c, &v) != 0) return -1;
        if(!v && fat_atomic(f, c) && !bit_get(f->reached, c)) list[n++] = c;
    }
    return n == count ? 0 : -1;
}

static void fat_stamp(u8 *p) {
    time_t t;
    u32 date = (1 << 5) | 1, tm = 0;
    get_time(&t);
    if(t.year >= 1980 && t.year <= 2107) {
        date = ((t.year - 1980) << 9) | (t.month << 5) | t.date;
        tm = (t.hour << 11) | (t.minute << 5) | (t.second / 2);
    }
    put16(p + 14, tm);
    put16(p + 16, date);
    put16(p + 18, date);
    put16(p + 22, tm);
    put16(p + 24, date);
}

static void entry_make(const struct fck *f, u8 *p, const char *name, u32 attr, u32 first, u32 size) {
    memset(p, 0, 32);
    memcpy(p, name, 11);
    p[11] = (u8)attr;
    fat_stamp(p);
    put16(p + 26, first & 0xFFFF);
    if(f->type == 32) put16(p + 20, first >> 16);
    put32(p + 28, size);
}

/* a free slot of the root: *lba and *off. One at the end mark is as good as a deleted one: the look saw nothing past the mark. 0, or -1 */
static int root_slot(struct fck *f, u32 *lba, u32 *off) {
    struct frame fr;
    const u8 *s;
    memset(&fr, 0, sizeof(fr));
    fr.first = f->root_cluster;
    if(fr.first) {
        u32 c = fr.first, v;
        while(in_range(f, c) && fr.nclus < f->nclusters) {   /* its length: repairs come after a sound look at it */
            if(fat_get(f, 0, c, &v) != 0) return -1;
            fr.nclus++;
            if(!v || v >= f->eoc_min) break;
            c = v;
        }
    }
    frame_rewind(f, &fr);
    for(;;) {
        if(!(s = dir_sector(f, fr.lba, fr.end))) return -1;
        s += fr.off;
        if(!s[0] || s[0] == 0xE5) { *lba = fr.lba; *off = fr.off; return 0; }
        if(fr.idx + 1 >= SLOTS_MAX || frame_next(f, &fr) != 0) return -1;
    }
}

/* The lost chains of this look, in a new directory FOUND.nnn. 1 made, 0 not (they stay reported), -1 stopped */
static int fix_lost(struct fck *f) {
    u32 d = (((f->nlost + 2) * 32) + f->cb - 1) >> f->cshift, i, k, num, slot_lba, slot_off, *list;
    char name[12];
    u8 *p;
    for(num = 0; num < ROOM_NAMES; num++) if(!(f->found_used[num >> 3] & (1 << (num & 7)))) break;
    if(num == ROOM_NAMES) { say(f, "FOUND.000 to FOUND.999 are taken: the lost chains stay as they are"); return 0; }
    if(d > f->free_now) { say(f, "no room for FOUND.%03lu: the lost chains stay as they are", (u_long)num); return 0; }
    if(root_slot(f, &slot_lba, &slot_off) != 0) {
        if(!f->halt) say(f, "the root directory has no free slot: the lost chains stay as they are");
        return f->halt ? -1 : 0;
    }
    if(!(list = (u32 *)kalloc(d * sizeof(u32)))) { say(f, "no memory for FOUND.%03lu", (u_long)num); return 0; }
    if(take_free(f, list, d) != 0) {
        if(!f->halt) say(f, "no room for FOUND.%03lu: the lost chains stay as they are", (u_long)num);
        kfree(list);
        return f->halt ? -1 : 0;
    }
    /* the directory, in free clusters that nothing names yet */
    for(i = 0, k = 0; i < d; i++) {
        u32 s;
        memset(f->clus, 0, f->cb);
        s = 0;
        if(!i) {
            entry_make(f, f->clus, ".          ", ATTR_DIR, list[0], 0);
            entry_make(f, f->clus + 32, "..         ", ATTR_DIR, 0, 0);
            s = 2;
        }
        for(; s < f->cb / 32 && k < f->nlost; s++, k++) {
            snprintf(name, sizeof(name), "FILE%04luCHK", (u_long)k);
            entry_make(f, f->clus + s * 32, name, ATTR_ARCHIVE, f->lost[k].head, f->lost[k].n << f->cshift);
        }
        if(wr(f, f->data_lba + (list[i] - 2) * f->spc, f->spc, f->clus) != 0) { kfree(list); return -1; }
    }
    if(barrier(f) != 0 || fat_store(f, list, d, f->eoc) != 0) { kfree(list); return -1; }
    /* one write of one slot: all of it has a name, or none */
    if(rd_in_step(f, slot_lba, f->sec2) != 0) { kfree(list); return -1; }
    p = f->sec2 + slot_off;
    if(p[0] && p[0] != 0xE5) {
        say(f, "the slot in block %lu is not what the look found: stopped", (u_long)slot_lba);
        fail(f);
        kfree(list);
        return -1;
    }
    snprintf(name, sizeof(name), "FOUND   %03lu", (u_long)num);
    entry_make(f, p, name, ATTR_DIR, list[0], 0);
    i = wr(f, slot_lba, 1, f->sec2) != 0 || barrier(f) != 0;
    kfree(list);
    if(i) return -1;
    f->res->saved += f->nlost;
    f->free_now -= d;
    say(f, "%lu lost chain%s saved in FOUND.%03lu", (u_long)f->nlost, f->nlost == 1 ? "" : "s", (u_long)num);
    return 1;
}

/* the second FAT takes the first's sectors, those that differ. A window's buffer holds the first FAT's 64 KB:
   wr() drops the windows, and this one's bytes stay what they are until the next read into it */
static int fix_sync(struct fck *f) {
    u32 s, i, k, last = f->fat_bytes % SECTOR;
    u8 *a = f->win[0].data;
    for(s = 0; s < f->nsec; s += k) {
        k = min32(CHUNK, f->nsec - s);
        f->win[0].valid = false;
        if(rd(f, f->fat0 + s, k, a) != 0 || rd(f, f->fat1 + s, k, f->chunk) != 0) {
            if(!f->halt) { say(f, "a FAT cannot be read at sector %lu: stopped", (u_long)s); fail(f); }
            return -1;
        }
        for(i = 0; i < k; i++) {
            u32 n = s + i == f->nsec - 1 && last ? last : SECTOR;
            if(!memcmp(a + i * SECTOR, f->chunk + i * SECTOR, n)) continue;
            if(wr(f, f->fat1 + s + i, 1, a + i * SECTOR) != 0) return -1;
            f->res->synced++;
        }
    }
    return barrier(f);
}

/* FAT[1]'s clean bit in both FATs. Going off, the one in use first; coming on, it last: what other systems read says
   "check this" until the very end */
static int clean_mark(struct fck *f, bool clean) {
    u32 i, bit = f->type == 16 ? 0x8000 : 0x08000000, o = f->type == 16 ? 2 : 4;
    if(f->type == 12) return 0;
    for(i = 0; i < 2; i++) {
        u32 base = (i == 0) == !clean ? f->fat0 : f->fat1, v;
        if(!base) continue;
        if(rd_in_step(f, base, f->sec) != 0) return -1;
        v = f->type == 16 ? le16(f->sec + o) : le32(f->sec + o);
        if(((v & bit) != 0) == clean) continue;
        v = clean ? v | bit : v & ~bit;
        if(f->type == 16) put16(f->sec + o, v);
        else put32(f->sec + o, v);
        if(wr(f, base, 1, f->sec) != 0 || barrier(f) != 0) return -1;
    }
    return 0;
}

/* FSInfo's free count and hint, in a sector the look found its signatures in; its other bytes as they are */
static int fix_fsinfo(struct fck *f) {
    u32 hint;
    if(rd_in_step(f, f->fsi, f->sec) != 0) return -1;
    hint = le32(f->sec + 492);
    if(!in_range(f, hint)) hint = f->first_free ? f->first_free : 2;
    put32(f->sec + 488, f->free_now);
    put32(f->sec + 492, hint);
    if(wr(f, f->fsi, 1, f->sec) != 0) return -1;
    return barrier(f);
}

/* this look's chain ends and sizes: how many were made; -1 when the run stopped */
static int fix_plan(struct fck *f) {
    u32 i;
    for(i = 0; i < f->nfix; i++) {
        const struct fix *x = &f->fix[i];
        if(halted(f)) return -1;
        if((x->kind == FX_END ? fix_end(f, x) : fix_size(f, x)) != 0) return -1;
        if(x->kind == FX_END) f->res->cuts++;
        else f->res->sizes++;
    }
    return (int)i;
}

/* the next repair the look allows: something was written (the volume must be looked at again): 1; nothing is left
   that can be done: 0; the run stopped: -1 */
static int repair_step(struct fck *f) {
    int r;
    if(f->diff_secs) return fix_sync(f) == 0 ? 1 : -1;
    if((r = fix_plan(f)) != 0) return r < 0 ? -1 : 1;
    if(f->nlost) return fix_lost(f);
    return 0;
}

/* the run's end for a stop in the middle */
static int stopped_status(struct fck *f) {
    if(f->stopped) say(f, f->res->wrote ? "interrupted: what was repaired stays, and the volume stays marked: run it again" : "interrupted");
    return f->stopped ? FAT_CHECK_PROBLEMS : FAT_CHECK_TROUBLE;
}

/**** the volume's description ****/

/* the boot sector read and judged, into f. -1: go on; else the status it ends on */
static int boot_load(struct fck *f) {
    u8 *bs = f->sec;
    u32 tot16, tot32, spf16, spf32, bps, fsi, bk;
    const char *why = NULL;
    if(rd(f, 0, 1, bs) != 0) {
        if(f->halt) return stopped_status(f);
        say(f, "the boot sector cannot be read");
        return FAT_CHECK_TROUBLE;
    }
    bps = le16(bs + 11);
    f->spc = bs[13];
    f->rsv = le16(bs + 14);
    f->nfats = bs[16];
    f->root_entries = le16(bs + 17);
    tot16 = le16(bs + 19);
    f->media = bs[21];
    spf16 = le16(bs + 22);
    tot32 = le32(bs + 32);
    spf32 = le32(bs + 36);
    if(bs[510] != 0x55 || bs[511] != 0xAA || (bs[0] != 0xEB && bs[0] != 0xE9)) why = "no boot sector";
    else if(bps != SECTOR) why = "its sectors are not 512 bytes";
    else if(!f->spc || (f->spc & (f->spc - 1))) why = "its sectors per cluster are no power of two";
    else if(!f->rsv) why = "no reserved sector";
    else if(!f->nfats || f->nfats > 2) why = "not one FAT or two";
    else if(!tot16 && !tot32) why = "no size";
    else if(!spf16 && !spf32) why = "no FAT size";
    if(why) {
        /* what mount also refuses. Whether this is a FAT volume at all: its backup says, where it has one */
        if(rd(f, 6, 1, f->sec2) == 0 && f->sec2[510] == 0x55 && f->sec2[511] == 0xAA && le16(f->sec2 + 11) == SECTOR &&
           (f->sec2[0] == 0xEB || f->sec2[0] == 0xE9) && f->sec2[13] && !le16(f->sec2 + 22) && le32(f->sec2 + 36)) {
            say(f, "the boot sector is damaged (%s), and block 6 holds what looks like its backup: that repair is not made here", why);
            f->res->found = f->res->left = 1;
            return FAT_CHECK_PROBLEMS;
        }
        say(f, "no FAT file system found (%s)", why);
        return FAT_CHECK_TROUBLE;
    }
    f->total = tot16 ? tot16 : tot32;
    f->spf = spf16 ? spf16 : spf32;
    if(f->total > f->io->blocks) {
        say(f, "the boot sector says %lu blocks, the device has %lu", (u_long)f->total, (u_long)f->io->blocks);
        f->res->found = f->res->left = 1;
        return FAT_CHECK_PROBLEMS;
    }
    f->fat0 = f->rsv;
    f->root_secs = (f->root_entries * 32 + SECTOR - 1) / SECTOR;
    if((u64)f->rsv + (u64)f->nfats * f->spf + f->root_secs >= f->total) why = "it has no data area";
    else {
        f->root_lba = f->rsv + f->nfats * f->spf;
        f->data_lba = f->root_lba + f->root_secs;
    }
    if(why) {
        say(f, "the boot sector is damaged: %s", why);
        f->res->found = f->res->left = 1;
        return FAT_CHECK_PROBLEMS;
    }
    f->nclusters = (f->total - f->data_lba) / f->spc;
    f->end = f->nclusters + 2;
    f->type = f->nclusters < 4085 ? 12 : f->nclusters < 65525 ? 16 : 32;
    f->cb = f->spc * SECTOR;
    f->cshift = 9;
    while((1UL << f->cshift) < f->cb) f->cshift++;
    f->maxk = 0xFFFFFFFFUL >> f->cshift;
    f->eoc = f->type == 12 ? 0xFFF : f->type == 16 ? 0xFFFF : 0x0FFFFFFF;
    f->eoc_min = f->eoc - 7;
    f->badval = f->eoc - 8;
    f->fat_bytes = f->type == 12 ? f->end + f->end / 2 + (f->end & 1) : f->end * (f->type == 16 ? 2 : 4);
    f->nsec = (f->fat_bytes + SECTOR - 1) / SECTOR;
    f->root_cluster = 0;
    if(!f->nclusters) why = "it has no clusters";
    else if(f->nclusters > 0x0FFFFFF5UL) why = "too many clusters";
    else if(f->nsec > f->spf) why = "its FAT is too small for its clusters";
    else if(f->type == 32 && (spf16 || f->root_entries)) why = "it has FAT32's size and a fixed root directory";
    else if(f->type != 32 && !f->root_entries) why = "it has no root directory";
    else if(f->type != 32 && !spf16) why = "its FAT size is where only FAT32 has it";
    if(!why && f->type == 32) {
        f->root_cluster = le32(bs + 44);
        if(!in_range(f, f->root_cluster)) why = "its root directory's cluster is no cluster";
        else if(le16(bs + 42)) why = "a version of FAT32 this does not know";
    }
    if(why) {
        say(f, "the boot sector is damaged: %s", why);
        f->res->found = f->res->left = 1;
        return FAT_CHECK_PROBLEMS;
    }
    f->fat1 = f->nfats == 2 ? f->rsv + f->spf : 0;
    if(f->spf > f->nsec + f->nsec / 2 + 2) f->fat_big = true;
    f->boot_dirty = (bs[(f->type == 32 ? 64 : 36) + 1] & 1) != 0;
    if(f->type == 32) {
        u32 flags = le16(bs + 40);
        if(flags & 0x80) {
            /* one FAT active: the other may be anything. SurfOS's driver knows nothing of this */
            f->no_mirror = true;
            if((flags & 15) >= f->nfats) {
                say(f, "the boot sector names FAT %lu as the active one, and there are %lu", (u_long)(flags & 15), (u_long)f->nfats);
                f->res->found = f->res->left = 1;
                return FAT_CHECK_PROBLEMS;
            }
            f->fat0 = f->rsv + (flags & 15) * f->spf;
            f->fat1 = 0;
        }
        fsi = le16(bs + 48);
        bk = le16(bs + 50);
        f->fsi = fsi && fsi != 0xFFFF && fsi < f->rsv && fsi != bk ? fsi : 0;
        if(!f->fsi) note(f, "no usable FSInfo sector (the boot sector says %lu)", (u_long)fsi);
        if(bk && bk != 0xFFFF && bk < f->rsv) {
            if(rd(f, bk, 1, f->sec2) != 0) {                /* a problem, as FSInfo's would be: every look says it */
                if(f->halt) return stopped_status(f);
                f->backup_bad = bk;
            }
            else if(memcmp(bs + 11, f->sec2 + 11, 13) || memcmp(bs + 32, f->sec2 + 32, 20)) f->boot_doubt = "its backup describes another shape of volume";
            else if(memcmp(bs, f->sec2, SECTOR)) say(f, "note: the backup boot sector (block %lu) differs from the boot sector", (u_long)bk);
        } else note(f, "no backup boot sector");
    }
    return -1;
}

/* what the fck and tables_make() take of the heap, in bytes, but for the table of names: each piece with its header */
static u32 tables_fixed(const struct fck *f) {
    u32 bits = ((f->end + 31) >> 5) * 4;
    return 3 * (bits + PIECE_EXTRA) + (WINDOWS + 1) * (CHUNK * SECTOR + PIECE_EXTRA) + f->cb + DIR_AHEAD * SECTOR +
           FIX_MAX * sizeof(struct fix) + LOST_MAX * sizeof(struct lost) + DEPTH_MOST * sizeof(struct frame) + sizeof(struct fck) +
           6 * PIECE_EXTRA;
}

/* The slots of the table of names, taken once: twice the entries one directory of this volume can hold (SLOTS_MAX at
   most, as many as its data area holds, or the fixed root's), as a power of two; within room bytes, halved as far as
   NAMES_FIRST. 0: not even that fits */
static u32 names_size(const struct fck *f, u32 room) {
    u32 most = SLOTS_MAX, per = f->cb / 32, slots = NAMES_FIRST;
    if(f->nclusters < SLOTS_MAX / per) most = f->nclusters * per;
    if(f->root_entries > most) most = f->root_entries;
    if(most > SLOTS_MAX) most = SLOTS_MAX;
    while(slots < NAMES_MOST && slots < 2 * most) slots <<= 1;
    while(slots > NAMES_FIRST && slots * NAME_SLOT > room) slots >>= 1;
    return slots * NAME_SLOT <= room ? slots : 0;
}

/* room for the walk; false when there is none */
static bool tables_make(struct fck *f) {
    u32 i;
    f->bits = ((f->end + 31) >> 5) * 4;
    f->reached = (u32 *)kalloc(f->bits);
    f->dirhead = (u32 *)kalloc(f->bits);
    f->aux = (u32 *)kalloc(f->bits);
    f->chunk = (u8 *)kalloc(CHUNK * SECTOR);
    f->clus = (u8 *)kalloc(f->cb);
    f->dbuf = (u8 *)kalloc(DIR_AHEAD * SECTOR);
    f->fix = (struct fix *)kalloc(FIX_MAX * sizeof(struct fix));
    f->lost = (struct lost *)kalloc(LOST_MAX * sizeof(struct lost));
    f->stack = (struct frame *)kalloc(DEPTH_MOST * sizeof(struct frame));
    f->names = (u8 *)kcalloc(f->names_slots, NAME_SLOT);     /* every mark 0: no directory's */
    if(!f->reached || !f->dirhead || !f->aux || !f->chunk || !f->clus || !f->dbuf || !f->fix || !f->lost || !f->stack || !f->names) return false;
    for(i = 0; i < WINDOWS; i++) if(!(f->win[i].data = (u8 *)kalloc(CHUNK * SECTOR))) return false;
    return true;
}

static void tables_free(struct fck *f) {
    u32 i;
    for(i = 0; i < WINDOWS; i++) kfree(f->win[i].data);
    kfree(f->reached);
    kfree(f->dirhead);
    kfree(f->aux);
    kfree(f->chunk);
    kfree(f->clus);
    kfree(f->dbuf);
    kfree(f->fix);
    kfree(f->lost);
    kfree(f->stack);
    kfree(f->names);
}

/* a clause more on the end of text, never past its end: a damaged volume may make every count ten digits long */
static void clause(char *text, u32 size, const char *fmt, ...) {
    u32 n = (u32)strlen(text);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text + n, size - n, fmt, ap);
    va_end(ap);
}

/* what a repair of this look would change, in a line, into text */
static void plan_words(struct fck *f, char *text, u32 size) {
    memset(text, 0, size);
    if(f->diff_secs) clause(text, size, ", copy the first FAT over the second");
    if(f->c_end) clause(text, size, ", end %lu chain%s", (u_long)f->c_end, f->c_end == 1 ? "" : "s");
    if(f->c_size) clause(text, size, ", set %lu size%s", (u_long)f->c_size, f->c_size == 1 ? "" : "s");
    if(f->c_lost) clause(text, size, ", save %lu lost chain%s in a new FOUND directory", (u_long)f->c_lost, f->c_lost == 1 ? "" : "s");
    if(f->fsi_wrong) clause(text, size, ", set FSInfo");
    if(f->unclean) clause(text, size, ", mark it clean");
}

/* something is left to repair */
static bool plan_any(const struct fck *f) {
    return f->stay < f->found;
}

static void result_take(struct fck *f) {
    struct fat_check_result *r = f->res;
    r->dirs = f->ndirs;
    r->files = f->nfiles;
    r->clusters = f->nclusters;
    r->free_clusters = f->free_now;
    r->bad_clusters = f->badc;
}

static void look_again(struct fck *f);

/* after the passes: FSInfo whatever is left, the clean bit only when nothing is */
static void repair_finish(struct fck *f) {
    bool none_left;
    if(f->halt || f->bar) return;
    none_left = f->found == f->soft;
    if(f->fsi_wrong && fix_fsinfo(f) != 0) return;
    if(f->unclean && none_left && clean_mark(f, true) != 0) return;
    look_again(f);
}

/* a look with nothing said */
static void look_again(struct fck *f) {
    bool q = f->quiet;
    f->quiet = true;
    walk(f, false);
    f->quiet = q;
}

/* what is left, in full: it is what the owner has to deal with */
static void more_say(struct fck *f) {
    if(!f->verbose && f->shown > SAY_MAX) say(f, "and %lu more (fsck -v says them all)", (u_long)(f->shown - SAY_MAX));
}

/* the names a look could not compare: said, and true when there were such */
static bool not_whole(struct fck *f) {
    if(!f->names_lost) return false;
    say(f, "not checked whole: %lu director%s held more names than a check may compare", (u_long)f->names_lost, f->names_lost == 1 ? "y" : "ies");
    return true;
}

/* from the first look on */
static int run(struct fck *f) {
    struct fat_check_result *r = f->res;
    char plan[240];
    u32 pass;
    int st;
    bool marked = false;
    say(f, "FAT%i: %lu clusters of %lu bytes, %lu blocks", f->type, (u_long)f->nclusters, (u_long)f->cb, (u_long)f->total);
    walk(f, true);
    result_take(f);
    r->found = r->left = f->found;
    if(f->halt) return stopped_status(f);
    more_say(f);
    if(f->verbose) say(f, "%lu directories, %lu files, %lu clusters free, %lu marked bad", (u_long)f->ndirs, (u_long)f->nfiles,
                       (u_long)f->free_now, (u_long)f->badc);
    if(!f->found) {
        if(not_whole(f)) return FAT_CHECK_TROUBLE;
        say(f, "clean");
        return FAT_CHECK_CLEAN;
    }
    say(f, "%lu problem%s", (u_long)f->found, f->found == 1 ? "" : "s");
    not_whole(f);
    if(f->bar) {
        if(f->io->write || f->plan_words) say(f, "no repair: %s", f->bar);
        return FAT_CHECK_PROBLEMS;
    }
    if(!plan_any(f)) {
        if(f->io->write || f->plan_words) say(f, "nothing here can be repaired");
        return FAT_CHECK_PROBLEMS;
    }
    if(f->stay && (f->io->write || f->plan_words)) say(f, "%lu of them stay%s reported", (u_long)f->stay, f->stay == 1 ? "s" : "");
    plan_words(f, plan, sizeof(plan));
    if(!f->io->write) {
        if(f->plan_words) say(f, "fsck -r would: %s", plan + 2);
        return FAT_CHECK_PROBLEMS;
    }
    if(!f->io->ask || !f->io->ask(f->io->ctx, plan + 2)) { r->cancelled = true; return FAT_CHECK_PROBLEMS; }
    for(pass = 0; pass < PASS_MAX && !f->bar; pass++) {
        if(!marked && (f->diff_secs || f->nfix || f->nlost)) {
            if(clean_mark(f, false) != 0) break;
            marked = true;
        }
        st = repair_step(f);
        if(st <= 0) break;
        look_again(f);
        if(halted(f)) break;
    }
    if(pass == PASS_MAX) say(f, "that is enough for one run: run it again for the rest");
    repair_finish(f);
    result_take(f);
    r->left = f->found;
    if(f->halt) return stopped_status(f);
    if(f->found) {
        u32 left = f->found;
        f->shown = 0;
        walk(f, false);
        r->left = f->found;
        if(f->halt) return stopped_status(f);
        more_say(f);
        if(f->bar) say(f, "no more repairs: %s", f->bar);
        say(f, "repaired what it could: %lu problem%s left", (u_long)left, left == 1 ? "" : "s");
        return FAT_CHECK_PROBLEMS;
    }
    if(not_whole(f)) return FAT_CHECK_TROUBLE;
    say(f, "repaired: clean");
    return FAT_CHECK_CLEAN;
}

int fat_check(const struct fat_check_io *io, int flags, struct fat_check_result *res) {
    struct fck *f;
    int st;
    memset(res, 0, sizeof(*res));
    f = (struct fck *)kcalloc(1, sizeof(struct fck));
    if(!f) {
        io->say(io->ctx, "out of memory");
        return res->status = FAT_CHECK_TROUBLE;
    }
    f->io = io;
    f->res = res;
    f->verbose = (flags & FAT_CHECK_VERBOSE) != 0;
    f->plan_words = (flags & FAT_CHECK_PLAN) != 0;
    st = boot_load(f);
    if(st < 0) {
        /* every piece, the table of names among them, and what the heap may leave over above them: within the cap */
        u32 fixed = tables_fixed(f) + PIECE_EXTRA + CHECK_HEAP_SLACK;
        f->names_slots = fixed < CHECK_MEM_MOST ? names_size(f, CHECK_MEM_MOST - fixed) : 0;
        if(!f->names_slots) {
            /* nothing is taken: the heap would keep it. (FAT32's largest volumes, and big ones with small clusters) */
            say(f, "a volume of %lu clusters takes %lu KB to check, more than the %lu KB a check may have: check it on another computer",
                (u_long)f->nclusters, (u_long)((fixed + NAMES_FIRST * NAME_SLOT + 1023) >> 10), (u_long)(CHECK_MEM_MOST >> 10));
            st = FAT_CHECK_TROUBLE;
        } else if(!tables_make(f)) {
            say(f, "not enough memory for a volume of %lu clusters", (u_long)f->nclusters);
            st = FAT_CHECK_TROUBLE;
        } else {
            if(!(flags & FAT_CHECK_REPAIR)) {
                /* the run's plan is the same, and it has no way to write */
                struct fat_check_io ro = *io;
                ro.write = NULL;
                f->io = &ro;
                st = run(f);
            } else st = run(f);
        }
        tables_free(f);
    }
    res->stopped = f->stopped;
    kfree(f);
    return res->status = st;
}
