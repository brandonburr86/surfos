/* Host test of fs/fat_check.c, the kernel command fsck's checker (11/01/26, the command-line suite). It runs over images
   made and read by the real driver: fs/fat.c, fs/bcache.c and fs/fat_mkfs.c through test_fatfs.c's harness, so that what
   says a repair worked is not the code that made it. What the brief asks to be proved, and where:

     a read-only run never writes       look(): no write function at all, the image compared byte for byte, in every
                                        scenario and on every damaged image of the fuzz
     a repair never deletes data        kept(): measured from the image the run began on, as the driver reads it and as it
                                        reads through the second FAT: every block of every file is still under its name, or
                                        in FOUND.nnn. The bytes of the tree are unique, so a block found is that block.
                                        write_judged(): every write as it is made, FAT entry by FAT entry, in both FATs: no
                                        cluster written that any link or entry names, no directory block but for an entry's
                                        smaller size or a free slot of the root, no link cut but a bad one
     a pull at any write is no worse    pull_one(): the repair pulled at each of its writes, that write landing whole, torn in
                                        four ways, or (with flushes that clear only what they clear, and flushes that lie)
                                        with any versions of what the stick still held; kept() then, and after a further
                                        repair. A tear can take a name with it: then the bytes must be in FOUND, or the run
                                        must say it is not done
     no damaged volume panics           fuzz(), lfn_fuzz(), boot_flips(), changing(): random damage to every part of the
                                        volume, long name sets of every shape, every bit of the boot sector, a FAT that reads
                                        differently each time. bad_reads(), dying(), stops(), refusals(), no_memory() and
                                        truncated() walk failing reads, stops, refused writes, refused allocations and short
                                        devices through one image each: those are not in the fuzz

   `test_fsck <seed> <images>` runs more of the fuzz by hand, `test_fsck --image <path>` one image from a file, `test_fsck
   --lfn <seed> <cases>` more of the long names. FSCK_VERBOSE=1 says everything; FSCK_ONLY=word runs what has that word in its
   name, in scens[] and in main(). fsck.fat -n judges the same images where it is installed (the devenv has it); the Mac has
   not, and says so.
   FSCK_DIFFS=1 prints what each side said where they disagree, and a verdict of fsck.fat's alone fails this test unless it
   is about the one thing left out on purpose, the boot sector's label. The sanitizers run on the Mac only: `make unittest`
   never builds this with them, see the account. */
#define main fatfs_main
#include "test_fatfs.c"
#undef main
#include <fs/fat_check.h>
#define ATTR_DIR_T 0x10

void *realloc(void *p, unsigned long n);
int atoi(const char *s);
int system(const char *cmd);
char *mkdtemp(char *tmpl);
int creat(const char *path, unsigned mode);
long write(int fd, const void *buf, unsigned long n);
int close(int fd);
int unlink(const char *path);
int rmdir(const char *path);
int open(const char *path, int flags, ...);
long read(int fd, void *buf, unsigned long n);
long lseek(int fd, long at, int whence);

static int fverbose;
static const char *only;
static u32 rng_state = 1;
static u32 rnd(void) { rng_state = rng_state * 1103515245u + 12345u; return (rng_state >> 8) & 0xFFFFFF; }
static u32 rn(u32 n) { return n ? rnd() % n : 0; }

/**** the checker's memory: every allocation counted, and any one of them refused ****/

static long mem_now, mem_peak;
static u32 mem_calls, mem_fail_at;

void *fsck_kalloc(size_t n) {
    size_t *p;
    if(mem_fail_at && ++mem_calls == mem_fail_at) return NULL;
    p = (size_t *)malloc(n + 16);
    if(!p) return NULL;
    p[0] = n;
    mem_now += (long)n;
    if(mem_now > mem_peak) mem_peak = mem_now;
    return (char *)p + 16;
}
void *fsck_kcalloc(size_t n, size_t m) {
    void *p = fsck_kalloc(n * m);
    if(p) memset(p, 0, n * m);
    return p;
}
void fsck_kfree(void *q) {
    size_t *p;
    if(!q) return;
    p = (size_t *)((char *)q - 16);
    mem_now -= (long)p[0];
    free(p);
}

/**** the volume as this test sees it, read by hand ****/

struct geo3 { u32 spc, cb, rsv, nfats, spf, fat1, fat2, root_lba, root_secs, data_lba, n, end, root_cluster, fsi, bk, eoc, bad; int type; };

static u32 rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 rd32(const u8 *p) { return rd16(p) | (rd16(p + 2) << 16); }
static void wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { wr16(p, v & 0xFFFF); wr16(p + 2, v >> 16); }

static void geo3(const u8 *bs, struct geo3 *g) {
    u32 tot = rd16(bs + 19) ? rd16(bs + 19) : rd32(bs + 32);
    memset(g, 0, sizeof(*g));
    g->spc = bs[13];
    g->cb = g->spc * 512;
    g->rsv = rd16(bs + 14);
    g->nfats = bs[16];
    g->spf = rd16(bs + 22) ? rd16(bs + 22) : rd32(bs + 36);
    g->fat1 = g->rsv;
    g->fat2 = g->rsv + g->spf;
    g->root_lba = g->rsv + g->nfats * g->spf;
    g->root_secs = (rd16(bs + 17) * 32 + 511) / 512;
    g->data_lba = g->root_lba + g->root_secs;
    g->n = g->spc && tot > g->data_lba ? (tot - g->data_lba) / g->spc : 0;     /* (a boot sector the fuzz has been at) */
    g->end = g->n + 2;
    g->type = g->n < 4085 ? 12 : g->n < 65525 ? 16 : 32;
    g->eoc = g->type == 12 ? 0xFFF : g->type == 16 ? 0xFFFF : 0x0FFFFFFF;
    g->bad = g->eoc - 8;
    if(g->type == 32) { g->root_cluster = rd32(bs + 44); g->fsi = rd16(bs + 48); g->bk = rd16(bs + 50); }
}

static u32 fget(const u8 *mem, const struct geo3 *g, u32 lba, u32 c) {
    const u8 *f = mem + lba * 512;
    if(g->type == 12) { u32 v = rd16(f + c + c / 2); return (c & 1) ? v >> 4 : v & 0xFFF; }
    if(g->type == 16) return rd16(f + c * 2);
    return rd32(f + c * 4) & 0x0FFFFFFF;
}

static void fset1(u8 *mem, const struct geo3 *g, u32 lba, u32 c, u32 v) {
    u8 *f = mem + lba * 512;
    if(g->type == 12) {
        u32 x = rd16(f + c + c / 2);
        wr16(f + c + c / 2, (c & 1) ? (x & 0x000F) | (v << 4) : (x & 0xF000) | (v & 0xFFF));
    } else if(g->type == 16) wr16(f + c * 2, v);
    else wr32(f + c * 4, (rd32(f + c * 4) & 0xF0000000) | v);
}

static void fset(struct ram *r, const struct geo3 *g, u32 c, u32 v) {
    fset1(r->mem, g, g->fat1, c, v);
    if(g->nfats == 2) fset1(r->mem, g, g->fat2, c, v);
}

static u32 clba(const struct geo3 *g, u32 c) { return g->data_lba + (c - 2) * g->spc; }
static u32 efirst(const struct geo3 *g, const u8 *p) { return rd16(p + 26) | (g->type == 32 ? rd16(p + 20) << 16 : 0); }
static void eset(const struct geo3 *g, u8 *p, u32 c) { wr16(p + 26, c & 0xFFFF); if(g->type == 32) wr16(p + 20, c >> 16); }

/* the slot holding the 8.3 name in the directory from cluster first (0: the root): the image's bytes, or NULL */
static u8 *slot_of(struct ram *r, const struct geo3 *g, u32 first, const char *name11) {
    u32 c = first ? first : g->root_cluster, i;
    if(!c) {
        u8 *p = r->mem + g->root_lba * 512;
        for(i = 0; i < g->root_secs * 16; i++, p += 32) if(!memcmp(p, name11, 11)) return p;
        return NULL;
    }
    while(c >= 2 && c < g->end) {
        u8 *p = r->mem + clba(g, c) * 512;
        for(i = 0; i < g->cb / 32; i++, p += 32) if(!memcmp(p, name11, 11)) return p;
        c = fget(r->mem, g, g->fat1, c);
    }
    return NULL;
}

/* the first slot at the end of the directory (first 0: the root) */
static u8 *slot_free(struct ram *r, const struct geo3 *g, u32 first) {
    u32 c = first ? first : g->root_cluster, i;
    if(!c) {
        u8 *p = r->mem + g->root_lba * 512;
        for(i = 0; i < g->root_secs * 16; i++, p += 32) if(!p[0]) return p;
        return NULL;
    }
    while(c >= 2 && c < g->end) {
        u8 *p = r->mem + clba(g, c) * 512;
        for(i = 0; i < g->cb / 32; i++, p += 32) if(!p[0]) return p;
        c = fget(r->mem, g, g->fat1, c);
    }
    return NULL;
}

/* the cluster n steps along the chain of the entry at p */
static u32 step(struct ram *r, const struct geo3 *g, const u8 *p, u32 n) {
    u32 c = efirst(g, p);
    while(n--) c = fget(r->mem, g, g->fat1, c);
    return c;
}

/* clusters free in the first FAT, the n-th from the top: far from what the driver gave out */
static u32 free_cluster(struct ram *r, const struct geo3 *g, u32 n) {
    u32 c;
    for(c = g->end - 1; c >= 2; c--) if(!fget(r->mem, g, g->fat1, c) && !n--) return c;
    die("no free cluster to damage");
    return 0;
}

/**** the device a run sees ****/

struct pending { u32 lba; u8 old[512]; };

/* how the write that is pulled lands */
#define TEAR_NONE  0                    /* whole */
#define TEAR_OLD   1                    /* a part of its sector, the rest as it was */
#define TEAR_FF    2                    /* a part, the rest erased */
#define TEAR_ALLFF 3                    /* the sector erased */
#define TEAR_ZERO  4                    /* the sector zeros */
#define TEARS      4

/* what the stick does with a flush */
#define FLUSH_TRUE  0                   /* no cache to speak of */
#define FLUSH_CACHE 1                   /* a cache, and a flush empties it */
#define FLUSH_LIES  2                   /* a cache, and a flush says yes and does nothing (driver/usb/msc.c:371 has no answer to that) */

struct tio {
    struct fat_check_io io;
    struct ram *r;
    u32 writes, reads, read_blocks, max_read, flushes;
    u32 pull_at;                        /* gone at this write (0: never) */
    int tear, device;                   /* how it lands; FLUSH_* */
    u32 bad_read;                       /* this block + 1 cannot be read (0: none) */
    u32 dying_at;                       /* from this read on nothing can be (0: never) */
    u32 write_fail_at;                  /* this write is refused */
    u32 stop_at;                        /* stop() says so from this device request on (0: never) */
    u32 limit;                          /* a run over this many requests is a run that does not end */
    bool gone, judged, answer, wild;
    u32 asks, holds;
    char plan[240];
    u32 *touched;
    u32 ntouched, cap;
    struct pending *pend;
    u32 npend;
    u32 logn;
    char log[1 << 14];
};

static u8 *before;                      /* the image the repair began on, for write_judged() */
static struct geo3 bg;
static u32 bad_writes;
static char bad_what[200];

static void bad_write(const char *what, u32 lba) {
    if(!bad_writes++) snprintf(bad_what, sizeof(bad_what), "%s (block %u)", what, lba);
}

/* what the image the repair began on says of each cluster */
#define J_NAMED 1                       /* an entry names it, or a link of a cluster the first FAT has in use, in either FAT */
#define J_DIR   2                       /* a directory's chain holds it */
#define J_FILE  4                       /* a file's does */
#define J_ROOT  8                       /* the root's does */
static u8 *jm, *raw_marks;
static u32 jm_cap;

/* one FAT entry as it goes to the disk. copy: the second FAT */
static void entry_judged(const u8 *mem, u32 lba, u32 c, u32 was, u32 now, int copy) {
    const struct geo3 *g = &bg;
    u32 clean = g->type == 16 ? 0x8000 : 0x08000000, x, n;
    if(c == 1 && g->type != 12 && !((was ^ now) & ~clean)) return;  /* the clean bit, the second FAT's first when it comes on */
    if(copy) {
        /* (a FAT12 entry across two sectors is half the first's until the next sector of the copy lands: only that one) */
        if(now != fget(mem, g, g->fat1, c) && !(g->type == 12 && (c + c / 2) % 512 == 511)) bad_write("the second FAT given what the first does not hold", lba);
        return;
    }
    if(c < 2) { bad_write("a reserved FAT entry written", lba); return; }
    if(c >= g->end) { bad_write("a FAT entry past the last cluster written", lba); return; }
    if(!now) { bad_write("a cluster in use set free", lba); return; }
    if(!was) {
        if((jm[c] & J_NAMED) && now <= g->bad) bad_write("a free cluster a link names given a link", lba);
        return;
    }
    if(now <= g->bad) { bad_write("a link made into another link", lba); return; }
    if(was == g->bad) { bad_write("a cluster marked bad unmarked", lba); return; }
    if(was > g->bad) { bad_write("an end of chain changed", lba); return; }
    if(was < 2 || was >= g->end) return;                /* a link to no cluster */
    for(x = was, n = 0; n <= g->n && x >= 2 && x < g->end; n++) {
        if(x == c) return;                              /* a loop's closing link */
        x = fget(mem, g, g->fat1, x);
    }
    bad_write("a good link cut", lba);
}

/* the sector as it goes to the disk: each entry it changes, by what the entries read before and after */
static void fat_judged(u8 *mem, u32 lba, const u8 *now, int copy) {
    static u32 a[400], b[400];
    u32 base = copy ? bg.fat2 : bg.fat1, s = lba - base, c0, n, i;
    u8 *sec = mem + lba * 512, save[512];
    if(bg.type == 12) { c0 = s * 1024 / 3; c0 = c0 ? c0 - 1 : 0; n = 345; }
    else { n = bg.type == 16 ? 256 : 128; c0 = s * n; }
    for(i = 0; i < n; i++) a[i] = fget(mem, &bg, base, c0 + i);
    memcpy(save, sec, 512);
    memcpy(sec, now, 512);
    for(i = 0; i < n; i++) b[i] = fget(mem, &bg, base, c0 + i);
    memcpy(sec, save, 512);
    for(i = 0; i < n; i++) if(a[i] != b[i]) entry_judged(mem, lba, c0 + i, a[i], b[i], copy);
}

/* one block as it goes to the disk, against the image the repair began on. What fat_check.c's head comment promises */
static void write_judged(u8 *mem, u32 lba, const u8 *now, u32 count) {
    u32 i, k, c;
    for(k = 0; k < count; k++, lba++, now += 512) {
        const u8 *was = before + lba * 512;
        int root = lba >= bg.root_lba && lba < bg.data_lba;
        if(!lba) { bad_write("the boot sector written", lba); continue; }
        if(bg.bk && (lba == bg.bk)) { bad_write("the backup boot sector written", lba); continue; }
        if(lba >= bg.fat1 && lba < bg.fat1 + bg.spf) { fat_judged(mem, lba, now, 0); continue; }
        if(bg.nfats == 2 && lba >= bg.fat2 && lba < bg.fat2 + bg.spf) { fat_judged(mem, lba, now, 1); continue; }
        if(bg.fsi && lba == bg.fsi) {
            for(i = 0; i < 512; i++)
                if(was[i] != now[i] && !(i >= 488 && i < 496)) bad_write("FSInfo written outside its count and hint", lba);
            continue;
        }
        if(lba < bg.root_lba) { bad_write("a reserved sector written", lba); continue; }
        if(lba >= bg.data_lba) {
            c = (lba - bg.data_lba) / bg.spc + 2;
            if(c >= bg.end) { bad_write("a block past the last cluster written", lba); continue; }
            if(!jm[c] && !fget(mem, &bg, bg.fat1, c) && (bg.nfats < 2 || !fget(mem, &bg, bg.fat2, c))) continue;    /* free, named by nothing */
            if((jm[c] & (J_DIR | J_FILE)) != J_DIR) { bad_write("a cluster written that is neither free nor only a directory's", lba); continue; }
            root = (jm[c] & J_ROOT) != 0;
        }
        for(i = 0; i < 512; i += 32) {                  /* a directory's block: an entry's smaller size, or a free slot of the root */
            const u8 *a = was + i, *b = now + i;
            if(!memcmp(a, b, 32)) continue;
            if(a[0] == 0 || a[0] == 0xE5) {
                if(!root) bad_write("a slot filled outside the root", lba);
                else if(memcmp(b, "FOUND   ", 8) || b[11] != ATTR_DIR_T) bad_write("a free slot filled with something else", lba);
                continue;
            }
            if(memcmp(a, b, 28) || rd32(b + 28) >= rd32(a + 28)) bad_write("an entry changed beyond a smaller size", lba);
        }
    }
}

static int t_read(void *ctx, u32 lba, u32 count, void *buf) {
    struct tio *t = (struct tio *)ctx;
    if(t->gone) return BDEV_EGONE;
    if(t->limit && t->reads + t->writes > t->limit) die("a run that does not end: too many device requests");
    t->reads++;
    t->read_blocks += count;
    if(count > t->max_read) t->max_read = count;
    if(t->bad_read && lba < t->bad_read && t->bad_read <= lba + count) return BDEV_EIO;
    if(t->dying_at && t->reads >= t->dying_at) return BDEV_EIO;
    if(lba + count > t->r->bd.nblocks) die("a read past the device");
    memcpy(buf, t->r->mem + lba * 512, count * 512);
    return 0;
}

static void touch(struct tio *t, u32 lba) {
    if(t->ntouched == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 1024;
        t->touched = (u32 *)realloc(t->touched, t->cap * sizeof(u32));
    }
    t->touched[t->ntouched++] = lba;
}

/* the write that is pulled. Of several sectors one tears: those before it landed, those after it did not */
static void tear(struct tio *t, u32 lba, u32 count, const u8 *buf) {
    u8 *m = t->r->mem + lba * 512;
    u32 k = count > 1 ? rn(count) : 0, cut = 1 + rn(511);
    if(t->tear == TEAR_NONE) { memcpy(m, buf, count * 512); return; }
    memcpy(m, buf, k * 512);
    m += k * 512;
    buf += k * 512;
    switch(t->tear) {
    case TEAR_OLD: memcpy(m, buf, cut); break;
    case TEAR_FF: memcpy(m, buf, cut); memset(m + cut, 0xFF, 512 - cut); break;
    case TEAR_ALLFF: memset(m, 0xFF, 512); break;
    case TEAR_ZERO: memset(m, 0, 512); break;
    }
}

static int t_write(void *ctx, u32 lba, u32 count, const void *buf) {
    struct tio *t = (struct tio *)ctx;
    u32 i;
    if(t->gone) return BDEV_EGONE;
    if(t->limit && t->reads + t->writes > t->limit) die("a run that does not end: too many device requests");
    if(lba + count > t->r->bd.nblocks) die("a write past the device");
    t->writes++;
    if(t->write_fail_at == t->writes) { t->gone = true; return BDEV_EIO; }
    if(t->judged) write_judged(t->r->mem, lba, (const u8 *)buf, count);
    for(i = 0; i < count; i++) {
        touch(t, lba + i);
        if(t->device != FLUSH_TRUE) {
            t->pend = (struct pending *)realloc(t->pend, (t->npend + 1) * sizeof(struct pending));
            t->pend[t->npend].lba = lba + i;
            memcpy(t->pend[t->npend++].old, t->r->mem + (lba + i) * 512, 512);
        }
    }
    if(t->pull_at && t->writes == t->pull_at) {
        tear(t, lba, count, (const u8 *)buf);
        t->gone = true;
        return 0;
    }
    memcpy(t->r->mem + lba * 512, buf, count * 512);
    return 0;
}

static int t_flush(void *ctx) {
    struct tio *t = (struct tio *)ctx;
    if(t->gone) return BDEV_EGONE;
    t->flushes++;
    if(t->device == FLUSH_CACHE) t->npend = 0;
    return 0;
}

static bool t_stop(void *ctx) {
    struct tio *t = (struct tio *)ctx;
    return t->stop_at && t->reads + t->writes >= t->stop_at;
}

static void t_say(void *ctx, const char *line) {
    struct tio *t = (struct tio *)ctx;
    u32 n = strlen(line);
    if(strstr(line, "holds cluster")) t->holds++;
    if(t->logn + n + 2 < sizeof(t->log)) {
        memcpy(t->log + t->logn, line, n);
        t->logn += n;
        t->log[t->logn++] = '\n';
        t->log[t->logn] = 0;
    }
    if(fverbose) printf("      | %s\n", line);
}

static bool t_ask(void *ctx, const char *plan) {
    struct tio *t = (struct tio *)ctx;
    t->asks++;
    strlcpy(t->plan, plan, sizeof(t->plan));
    return t->answer;
}

static void t_init(struct tio *t, struct ram *r, int writing) {
    memset(t, 0, sizeof(*t));
    t->r = r;
    t->io.ctx = t;
    t->io.blocks = r->bd.nblocks;
    t->io.read = t_read;
    t->io.write = writing ? t_write : NULL;
    t->io.flush = t_flush;
    t->io.stop = t_stop;
    t->io.say = t_say;
    t->io.ask = t_ask;
    t->answer = true;
    t->limit = 40000000;
}

/* a unit of a run: the log and the lists go when it ends; each ends with nothing of the checker's allocated */
static void t_done(struct tio *t) {
    free(t->touched);
    free(t->pend);
    t->touched = NULL;
    t->pend = NULL;
    t->npend = t->ntouched = t->cap = 0;
}

/* The stick's cache lost what it pleased of what it still held: each sector ends as one of its versions, in no order at
   all between sectors. A flush that clears nothing holds everything since the run began */
static void cache_loses(struct tio *t) {
    u32 i, j;
    for(i = 0; i < t->npend; i++) {
        u32 lba = t->pend[i].lba, versions = 1, pick;
        if(lba == 0xFFFFFFFF) continue;
        for(j = i + 1; j < t->npend; j++) if(t->pend[j].lba == lba) versions++;
        pick = rn(versions + 1);                        /* versions: the last write stands */
        for(j = i; j < t->npend; j++) {
            if(t->pend[j].lba != lba) continue;
            if(!pick--) memcpy(t->r->mem + lba * 512, t->pend[j].old, 512);
            t->pend[j].lba = 0xFFFFFFFF;
        }
    }
    t->npend = 0;
}

/* a run with the plan said (FAT_CHECK_PLAN): what the scenarios look for. The kernel command asks for none (words()) */
static int run(struct tio *t, int flags, struct fat_check_result *res) {
    int r = fat_check(&t->io, flags | FAT_CHECK_PLAN | (fverbose ? FAT_CHECK_VERBOSE : 0), res);
    char m[160];
    snprintf(m, sizeof(m), "the checker's memory all goes back (%ld bytes kept)", mem_now);
    check(mem_now == 0, m);
    mem_now = 0;
    return r;
}

/**** the volume as the driver sees it ****/

struct node { char path[300]; int dir; u32 size; u8 *data; int len; };
struct tree { struct node *n; u32 count, cap; };

static void tree_free(struct tree *t) {
    u32 i;
    for(i = 0; i < t->count; i++) free(t->n[i].data);
    free(t->n);
    memset(t, 0, sizeof(*t));
}

#define TREE_MOST 3000
static u32 file_most = 1 << 20;

static void tree_walk(struct tree *t, struct vnode *dv, const char *path, int depth) {
    struct dirent de;
    u_int i;
    for(i = 0; dv->ops->readdir(dv, i, &de) == 0 && t->count < TREE_MOST; i++) {
        struct node *n;
        struct vnode *v = lookup(dv, de.name);
        if(!v) continue;
        if(t->count == t->cap) {
            t->cap = t->cap ? t->cap * 2 : 64;
            t->n = (struct node *)realloc(t->n, t->cap * sizeof(struct node));
        }
        n = &t->n[t->count++];
        memset(n, 0, sizeof(*n));
        snprintf(n->path, sizeof(n->path), "%s/%s", path, de.name);
        n->dir = de.type == VN_DIR;
        n->size = v->size;
        if(!n->dir) {
            u32 want = v->size < file_most ? v->size : file_most, got = 0;
            n->data = (u8 *)malloc(want + 1);
            while(got < want) {                         /* a piece at a time: as far as it can be read, not all or nothing */
                int k = v->ops->read(v, got, n->data + got, want - got < 512 ? want - got : 512);
                if(k <= 0) break;
                got += (u32)k;
            }
            n->len = (int)got;
        } else if(depth < 12) {
            char at[300];
            strlcpy(at, n->path, sizeof(at));
            tree_walk(t, v, at, depth + 1);
        }
        vput(v);
    }
}

/* the driver mounts the image read-only (it writes nothing then), and every file is read through it. 0, or -1 when it
   does not mount */
static int tree_read(struct ram *r, struct tree *t) {
    struct superblock *sb = (struct superblock *)calloc(1, sizeof(*sb));
    int m;
    memset(t, 0, sizeof(*t));
    binvalidate(&r->bd);
    sb->type = fat;
    sb->dev = &r->bd;
    sb->readonly = true;
    mutex_init(&sb->lock, "fsck");
    m = fat->mount(sb);
    if(m) { free(sb); binvalidate(&r->bd); return -1; }
    tree_walk(t, sb->root, "", 0);
    vput(sb->root);
    sb->ops->unmount(sb);
    binvalidate(&r->bd);
    free(sb);
    return 0;
}

static const struct node *tree_find(const struct tree *t, const char *path) {
    u32 i;
    for(i = 0; i < t->count; i++) if(!strcmp(t->n[i].path, path)) return &t->n[i];
    return NULL;
}

static char why[300];

/* The yardstick: the image a run began on, as the driver reads it, and as it reads through the second FAT, whose record is
   worth as much until someone has looked. What it is measured from is never what the repair left */
struct yard { struct tree a, b; int mounted, two; };

/* the FATs change places, for one reading */
static void fat_swap(struct ram *x, const struct geo3 *g) {
    u32 i;
    u8 t[512];
    for(i = 0; i < g->spf; i++) {
        u8 *p = x->mem + (g->fat1 + i) * 512, *q = x->mem + (g->fat2 + i) * 512;
        memcpy(t, p, 512);
        memcpy(p, q, 512);
        memcpy(q, t, 512);
    }
}

static int fats_differ(const struct ram *x, const struct geo3 *g) {
    return g->nfats == 2 && memcmp(x->mem + g->fat1 * 512, x->mem + g->fat2 * 512, (size_t)g->spf * 512);
}

static void yard_read(struct ram *x, const struct geo3 *g, struct yard *y) {
    memset(y, 0, sizeof(*y));
    y->mounted = tree_read(x, &y->a) == 0;
    if(y->mounted && fats_differ(x, g)) {
        fat_swap(x, g);
        y->two = tree_read(x, &y->b) == 0;
        fat_swap(x, g);
    }
}

static void yard_free(struct yard *y) {
    tree_free(&y->a);
    if(y->two) tree_free(&y->b);
}

#define K_NAMES 1                       /* a tear may have taken the sector that held a name */
#define K_DISK  2                       /* not done, and saying so: what left its name may be on the disk, in use */

static int cluster_in(const struct node *n, const u8 *s, u32 len, u32 cb) {
    int o;
    for(o = 0; o + (int)len <= n->len; o += (int)cb) if(!memcmp(n->data + o, s, len)) return 1;
    return 0;
}

static int in_found(const struct tree *t, const u8 *s, u32 len, u32 cb) {
    u32 i;
    for(i = 0; i < t->count; i++)
        if(!t->n[i].dir && !strncmp(t->n[i].path, "/FOUND.", 7) && cluster_in(&t->n[i], s, len, cb)) return 1;
    return 0;
}

/* zeros are no evidence of anything: a link past the volume reads them from past its last cluster, and nothing in the tree is */
static int all_zero(const u8 *s, u32 len) {
    u32 i;
    for(i = 0; i < len; i++) if(s[i]) return 0;
    return 1;
}

/* in a cluster in use. A tear of a volume's only FAT can take that too: then the bytes being there at all is what is left */
static int on_disk(const struct ram *x, const struct geo3 *g, const u8 *s, u32 len, int how) {
    u32 c;
    for(c = 2; c < g->end; c++) {
        if(!(how & K_NAMES) && !fget(x->mem, g, g->fat1, c) && (g->nfats < 2 || !fget(x->mem, g, g->fat2, c))) continue;
        if(!memcmp(x->mem + clba(g, c) * 512, s, len)) return 1;
    }
    return 0;
}

/* what one reading of the yardstick held is still there. now: the driver's reading of the volume; now2: through its second FAT */
static int held_all(const struct tree *y, int first, const struct tree *now, const struct tree *now2, const struct ram *x, const struct geo3 *g, int how) {
    u32 i;
    for(i = 0; i < y->count; i++) {
        const struct node *a = &y->n[i], *b = tree_find(now, a->path), *b2 = now2 ? tree_find(now2, a->path) : NULL;
        int o;
        if(tree_find(y, a->path) != a) continue;        /* (a name twice: lookups find the first, on any volume) */
        if(!(how & K_NAMES)) {
            if(!b && !b2) { snprintf(why, sizeof(why), "%s is gone", a->path); return -1; }
            if(b && a->dir != b->dir) { snprintf(why, sizeof(why), "%s changed kind", a->path); return -1; }
        }
        if(a->dir) continue;
        if(b && b->dir) b = NULL;
        if(b2 && b2->dir) b2 = NULL;
        if(b && first && !(how & K_NAMES)) {
            if(memcmp(a->data, b->data, (size_t)(a->len < b->len ? a->len : b->len))) { snprintf(why, sizeof(why), "%s has other bytes", a->path); return -1; }
            if(b->len > a->len) { snprintf(why, sizeof(why), "%s grew: %d bytes, it read %d", a->path, b->len, a->len); return -1; }
        }
        for(o = 0; o < a->len; o += (int)g->cb) {
            const u8 *s = a->data + o;
            u32 len = (u32)(a->len - o) < g->cb ? (u32)(a->len - o) : g->cb;
            if(all_zero(s, len)) continue;
            if(b && o + (int)len <= b->len && !memcmp(b->data + o, s, len)) continue;
            if(b2 && o + (int)len <= b2->len && !memcmp(b2->data + o, s, len)) continue;
            if(b && cluster_in(b, s, len, g->cb)) continue;
            if(in_found(now, s, len, g->cb)) continue;
            if((how & K_DISK) && on_disk(x, g, s, len, how)) continue;
            snprintf(why, sizeof(why), "%s: the %u bytes at %d are nowhere (it read %d, it reads %d)%s", a->path, len, o, a->len, b ? b->len : -1, first ? "" : " (the second FAT's record)");
            return -1;
        }
    }
    return 0;
}

/* The yardstick against the volume as it stands: 0, or -1 and why */
static int kept(const struct yard *y, struct ram *x, const struct geo3 *g, int how) {
    struct tree now, now2;
    int two = 0, bad = 0;
    u32 i;
    if(!y->mounted) return 0;
    if(tree_read(x, &now) != 0) {
        if(!(how & K_NAMES)) { snprintf(why, sizeof(why), "the driver does not mount it"); return -1; }
    }
    if(fats_differ(x, g)) {
        fat_swap(x, g);
        two = tree_read(x, &now2) == 0;
        fat_swap(x, g);
    }
    bad = held_all(&y->a, 1, &now, two ? &now2 : NULL, x, g, how);
    if(!bad && y->two) bad = held_all(&y->b, 0, &now, two ? &now2 : NULL, x, g, how);
    if(!(how & K_NAMES))
        for(i = 0; i < now.count && !bad; i++) {
            if(!strncmp(now.n[i].path, "/FOUND.", 7) || tree_find(&y->a, now.n[i].path) || (y->two && tree_find(&y->b, now.n[i].path))) continue;
            snprintf(why, sizeof(why), "%s is new", now.n[i].path);
            bad = -1;
        }
    tree_free(&now);
    if(two) tree_free(&now2);
    return bad;
}

/* no cluster in use on the image `was` is free on `now` */
static int none_freed(const u8 *was, const u8 *now, const struct geo3 *g) {
    u32 c;
    for(c = 2; c < g->end; c++) {
        if(fget(was, g, g->fat1, c) && !fget(now, g, g->fat1, c)) {
            snprintf(why, sizeof(why), "cluster %u was in use and is free", c);
            return -1;
        }
    }
    return 0;
}

/* no entry the driver can reach, and no link on the way, names a free cluster that it did not name on `was`: the
   worst a repair could do, an entry over bytes the next file may take. The checker's own opinion stays out of it */
static int chains_whole(struct ram *r, const u8 *was, const struct geo3 *g, u32 first, int depth);

static int entries_whole(struct ram *r, const u8 *was, const struct geo3 *g, const u8 *p, u32 slots, int depth) {
    u32 i;
    for(i = 0; i < slots; i++, p += 32) {
        u32 c, v, n = 0;
        if(!p[0]) break;
        if(p[0] == 0xE5 || (p[11] & 0x0F) == 0x0F || (p[11] & 8)) continue;
        if(p[0] == '.' && (p[1] == ' ' || p[1] == '.')) continue;
        for(c = efirst(g, p); c >= 2 && c < g->end && n < g->n; n++) {
            v = fget(r->mem, g, g->fat1, c);
            if(!v) {
                if(fget(was, g, g->fat1, c)) { snprintf(why, sizeof(why), "an entry reaches cluster %u, freed", c); return -1; }
                break;
            }
            if(v >= g->bad) break;
            c = v;
        }
        if((p[11] & 0x10) && efirst(g, p) && chains_whole(r, was, g, efirst(g, p), depth + 1)) return -1;
    }
    return 0;
}

static int chains_whole(struct ram *r, const u8 *was, const struct geo3 *g, u32 first, int depth) {
    u32 c = first, n = 0;
    if(depth > 8) return 0;
    if(!first) return entries_whole(r, was, g, r->mem + g->root_lba * 512, g->root_secs * 16, depth);
    while(c >= 2 && c < g->end && n++ < g->n) {
        u32 v = fget(r->mem, g, g->fat1, c);
        if(!v || v == g->bad) return 0;
        if(entries_whole(r, was, g, r->mem + clba(g, c) * 512, g->cb / 32, depth)) return -1;
        if(v >= g->bad) return 0;
        c = v;
    }
    return 0;
}

/* A second reader that owes nothing to the driver: the image by hand, from the entries and the first FAT. For every entry
   that names a file, how many bytes its chain holds as the driver would read it: to its first bad link or a cluster it
   has been to, a free or bad cluster at the end counted, because fs/fat.c reads that one. A repair may only keep or raise
   that number, for every entry there is. raw_marks, when set, is told what each cluster is (write_judged()) */
struct rawe { u32 key, good; };
struct rawl { struct rawe *e; u32 n, cap; u32 shared, freelinks; };
static u32 *seen, *held, seen_gen, seen_cap, held_gen;

static u32 raw_mix(u32 h, const u8 *name) {
    u32 i;
    for(i = 0; i < 11; i++) h = (h ^ name[i]) * 16777619u;
    return h;
}

static void seen_reset(const struct geo3 *g) {
    if(g->end > seen_cap) {
        seen = (u32 *)realloc(seen, g->end * sizeof(u32));
        held = (u32 *)realloc(held, g->end * sizeof(u32));
        memset(seen, 0, g->end * sizeof(u32));
        memset(held, 0, g->end * sizeof(u32));
        seen_cap = g->end;
    }
    seen_gen++;
}

static u32 raw_good(const u8 *mem, const struct geo3 *g, u32 first, u32 size, struct rawl *l) {
    u32 c = first, n = 0;
    u64 bytes;
    seen_reset(g);
    while(c >= 2 && c < g->end && seen[c] != seen_gen) {
        u32 v = fget(mem, g, g->fat1, c);
        if(!v) l->freelinks++;
        seen[c] = seen_gen;
        if(held[c] == held_gen) l->shared++;
        held[c] = held_gen;
        if(raw_marks) raw_marks[c] |= J_FILE | J_NAMED;
        n++;
        if(!v || v >= g->bad) break;
        c = v;
    }
    bytes = (u64)n * g->cb;
    return bytes < size ? (u32)bytes : size;
}

static void raw_slots(const u8 *mem, const struct geo3 *g, const u8 *p, u32 slots, u32 key, int depth, struct rawl *l, u32 *start);

/* the directory from cluster first (0: the fixed root), each cluster once */
static void raw_dir(const u8 *mem, const struct geo3 *g, u32 first, u32 key, int depth, struct rawl *l) {
    u32 c = first, n = 0, start = 1;
    if(depth > 9) return;
    if(!first) { raw_slots(mem, g, mem + g->root_lba * 512, g->root_secs * 16, key, depth, l, &start); return; }
    while(c >= 2 && c < g->end && n++ < 4096) {
        u32 v = fget(mem, g, g->fat1, c);
        if(!v) l->freelinks++;
        if(held[c] == held_gen) l->shared++;
        held[c] = held_gen;
        if(raw_marks) raw_marks[c] |= J_DIR | J_NAMED | (depth ? 0 : J_ROOT);
        if(start) raw_slots(mem, g, mem + clba(g, c) * 512, g->cb / 32, key, depth, l, &start);
        if(!v || v >= g->bad) return;
        c = v;
    }
}

static void raw_slots(const u8 *mem, const struct geo3 *g, const u8 *p, u32 slots, u32 key, int depth, struct rawl *l, u32 *start) {
    u32 i;
    for(i = 0; i < slots; i++, p += 32) {
        u32 k, first;
        if(!p[0]) { *start = 0; return; }
        if(p[0] == 0xE5 || (p[11] & 0x0F) == 0x0F || (p[11] & 8)) continue;
        if(p[0] == '.' && (p[1] == ' ' || p[1] == '.')) continue;
        k = raw_mix(key, p);
        first = efirst(g, p);
        if(raw_marks && first >= 2 && first < g->end) raw_marks[first] |= J_NAMED;
        if(p[11] & 0x10) {
            if(first && l->n < 4000) raw_dir(mem, g, first, k, depth + 1, l);
            continue;
        }
        if(l->n == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 128;
            l->e = (struct rawe *)realloc(l->e, l->cap * sizeof(struct rawe));
        }
        l->e[l->n].key = k;
        l->e[l->n++].good = raw_good(mem, g, first, rd32(p + 28), l);
    }
}

static void raw_read(const u8 *mem, const struct geo3 *g, struct rawl *l) {
    memset(l, 0, sizeof(*l));
    seen_reset(g);
    held_gen++;
    raw_dir(mem, g, g->root_cluster, 2166136261u, 0, l);
}

/* what write_judged() knows of the volume it watches: from the image a repair begins on */
static void judge_prepare(const u8 *mem, const struct geo3 *g) {
    struct rawl l;
    u32 c, k;
    if(g->end > jm_cap) {
        jm = (u8 *)realloc(jm, g->end);
        jm_cap = g->end;
    }
    memset(jm, 0, g->end);
    for(k = 0; k < g->nfats; k++)
        for(c = 2; c < g->end; c++) {
            u32 v = fget(mem, g, k ? g->fat2 : g->fat1, c);
            if(v >= 2 && v < g->end && fget(mem, g, g->fat1, c)) jm[v] |= J_NAMED;
        }
    raw_marks = jm;
    raw_read(mem, g, &l);
    raw_marks = NULL;
    free(l.e);
}

/* every entry of `was` is on `now`, holding as much (entries of one name: as many, in their order). With a flush that lied
   FOUND's name can land and the FAT's marks not: a new directory on a free cluster, with nothing on it that was anyone's */
static int flush_lied;

static int none_shorter(const u8 *was, const u8 *now, const struct geo3 *g) {
    struct rawl a, b;
    u32 i, j;
    int bad = 0;
    raw_read(was, g, &a);
    raw_read(now, g, &b);
    for(i = 0; i < a.n && !bad; i++) {
        for(j = 0; j < b.n; j++) {
            if(b.e[j].key == a.e[i].key && b.e[j].good >= a.e[i].good) { b.e[j].key = 0; b.e[j].good = 0; break; }
        }
        if(j == b.n) {
            snprintf(why, sizeof(why), "an entry held %u bytes (%u of %u entries) and no longer does", a.e[i].good, i, a.n);
            bad = -1;
        }
    }
    if(!bad && b.shared > a.shared) { snprintf(why, sizeof(why), "%u clusters are named twice, %u were", b.shared, a.shared); bad = -1; }
    if(!bad && !flush_lied && b.freelinks > a.freelinks) { snprintf(why, sizeof(why), "%u chains reach a free cluster, %u did", b.freelinks, a.freelinks); bad = -1; }
    free(a.e);
    free(b.e);
    return bad;
}

/* every cluster in use has a name that reaches it, by the hand reader: a repair that says clean has left nothing behind */
static int none_lost(struct ram *x, const struct geo3 *g) {
    u8 *m = (u8 *)calloc(g->end, 1);
    struct rawl l;
    u32 c;
    int bad = 0;
    raw_marks = m;
    raw_read(x->mem, g, &l);
    raw_marks = NULL;
    free(l.e);
    for(c = 2; c < g->end && !bad; c++) {
        u32 v = fget(x->mem, g, g->fat1, c);
        if(v && v != g->bad && !(m[c] & (J_FILE | J_DIR))) {
            snprintf(why, sizeof(why), "cluster %u is in use, and no name reaches it", c);
            bad = -1;
        }
    }
    free(m);
    return bad;
}

/**** images ****/

/* FAT12 from nothing: SurfOS's mkfs does not make it, and the driver reads it */
static void mkfs12(struct ram *r, u32 nfats) {
    u8 *bs = r->mem;
    u32 spf = 12, rsv = 1, re = 112, data = rsv + nfats * spf + 7, i;
    memset(r->mem, 0, (size_t)r->bd.nblocks * 512);
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
    memcpy(bs + 3, "SURFTEST", 8);
    wr16(bs + 11, 512);
    bs[13] = 2;
    wr16(bs + 14, rsv);
    bs[16] = (u8)nfats;
    wr16(bs + 17, re);
    wr16(bs + 19, data + 2 * 3500);
    bs[21] = 0xF8;
    wr16(bs + 22, spf);
    wr16(bs + 24, 63);
    wr16(bs + 26, 16);
    bs[38] = 0x29;
    memcpy(bs + 43, "NO NAME    ", 11);
    memcpy(bs + 54, "FAT12   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    for(i = 0; i < nfats; i++) {
        u8 *f = r->mem + (rsv + i * spf) * 512;
        f[0] = 0xF8; f[1] = 0xFF; f[2] = 0xFF;
    }
}

/* bytes no other place of the volume holds: a block found somewhere can only have come from where it should */
static void unique_fill(u8 *p, u32 len, u32 seed, u32 at) {
    u32 i, x = 0;
    for(i = 0; i < len; i++, at++) {
        if(!i || !(at & 31)) x = (seed * 2654435761u) ^ (at * 2246822519u) ^ 0x9E3779B1u;
        x = x * 1664525u + 1013904223u;
        p[i] = (u8)(x >> 24);
    }
}

/* FAT32 from nothing with clusters of one sector, as macOS makes a small volume: SurfOS's mkfs takes the largest that fits */
static void mkfs32(struct ram *r) {
    u8 *bs = r->mem;
    u32 total = r->bd.nblocks, rsv = 32, spf = total / 128 + 8, i;
    memset(r->mem, 0, (size_t)total * 512);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "SURFTEST", 8);
    wr16(bs + 11, 512);
    bs[13] = 1;
    wr16(bs + 14, rsv);
    bs[16] = 2;
    bs[21] = 0xF8;
    wr16(bs + 24, 63);
    wr16(bs + 26, 16);
    wr32(bs + 32, total);
    wr32(bs + 36, spf);
    wr32(bs + 44, 2);
    wr16(bs + 48, 1);
    wr16(bs + 50, 6);
    bs[66] = 0x29;
    memcpy(bs + 71, "NO NAME    ", 11);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    memcpy(r->mem + 6 * 512, bs, 512);
    wr32(r->mem + 512, 0x41615252);
    wr32(r->mem + 512 + 484, 0x61417272);
    wr32(r->mem + 512 + 488, 0xFFFFFFFF);
    wr32(r->mem + 512 + 492, 3);
    wr32(r->mem + 512 + 508, 0xAA550000);
    for(i = 0; i < 2; i++) {
        u8 *f = r->mem + (rsv + i * spf) * 512;
        wr32(f, 0x0FFFFFF8);
        wr32(f + 4, 0x0FFFFFFF);
        wr32(f + 8, 0x0FFFFFFF);
    }
}

static void put_or_die(struct vnode *dir, const char *name, u32 len, u32 seed) {
    static u8 big[1 << 16];
    unique_fill(big, len, seed, 0);
    if(put_file(dir, name, big, len)) die("a file of the test tree");
}

static struct vnode *mkdir_or_die(struct vnode *dir, const char *name) {
    struct vnode *v;
    if(dir->ops->create(dir, name, VN_DIR, &v)) die("a directory of the test tree");
    return v;
}

/* the tree every scenario starts from, cb the volume's cluster size: files of a few clusters, a fragmented pair, long
   names, three levels of directories, a file with no clusters, room to spare */
static void populate(struct ram *r) {
    static u8 piece[8192];
    struct superblock *sb = mount(r);
    struct vnode *root = sb->root, *docs, *deep, *er, *a, *b;
    u32 cb, i;
    struct geo3 g;
    geo3(r->mem, &g);
    cb = g.cb;
    put_or_die(root, "README.TXT", 5 * cb + 100, 1);
    put_or_die(root, "a long file name with spaces.text", 4 * cb, 2);
    put_or_die(root, "EMPTY.TXT", 0, 3);
    put_or_die(root, "OTHER.DAT", 6 * cb, 4);
    put_or_die(root, "SMALL.DAT", 2 * cb, 5);
    docs = mkdir_or_die(root, "docs");
    put_or_die(docs, "notes.txt", 3 * cb + 7, 6);
    put_or_die(docs, "MORE.TXT", cb, 7);
    deep = mkdir_or_die(docs, "deep");
    er = mkdir_or_die(deep, "er");
    put_or_die(er, "leaf.bin", 4 * cb, 8);
    if(root->ops->create(root, "frag1", VN_FILE, &a) || root->ops->create(root, "frag2", VN_FILE, &b)) die("frag");
    for(i = 0; i < 8; i++) {
        unique_fill(piece, cb, 10, i * cb);
        if(a->ops->write(a, i * cb, piece, cb) != (int)cb) die("frag1");
        unique_fill(piece, cb, 11, i * cb);
        if(b->ops->write(b, i * cb, piece, cb) != (int)cb) die("frag2");
    }
    vput(a); vput(b); vput(er); vput(deep); vput(docs);
    if(fsync_sb(sb)) die("the test tree's sync");
    vput(sb->root);
    sb->ops->unmount(sb);
    binvalidate(&r->bd);
    free(sb);
}

static struct ram *base11, *base12, *base16, *base32, *work11, *work12, *work16, *work32;

static void make_images(void) {
    char msg[256];
    init_bcache();
    nmade = 0;
    base11 = ram_new("b11", 8000);                      /* 11: FAT12 with one FAT */
    mkfs12(base11, 1);
    populate(base11);
    base12 = ram_new("b12", 8000);
    mkfs12(base12, 2);
    populate(base12);
    base16 = ram_new("b16", 8192);
    mkfs(base16);
    populate(base16);
    base32 = ram_new("b32", 70000);
    if(fat_mkfs(&base32->bd, "TEST32", 32, msg, sizeof(msg))) { printf("%s\n", msg); die("mkfs of FAT32"); }
    populate(base32);
    work11 = ram_new("w11", 8000);
    work12 = ram_new("w12", 8000);
    work16 = ram_new("w16", 8192);
    work32 = ram_new("w32", 70000);
    nmade = 0;
}

static struct ram *base_of(int type) { return type == 11 ? base11 : type == 12 ? base12 : type == 16 ? base16 : base32; }
static struct ram *work_of(int type) { return type == 11 ? work11 : type == 12 ? work12 : type == 16 ? work16 : work32; }

static struct ram *image(int type) {
    struct ram *b = base_of(type), *w = work_of(type);
    memcpy(w->mem, b->mem, (size_t)b->bd.nblocks * 512);
    binvalidate(&w->bd);
    return w;
}

/**** damage, one kind at a time ****/

#define E_README "README  TXT"
#define E_OTHER  "OTHER   DAT"
#define E_SMALL  "SMALL   DAT"
#define E_DOCS   "DOCS       "
#define E_NOTES  "NOTES   TXT"

static u8 *root_entry(struct ram *r, const struct geo3 *g, const char *name) {
    u8 *p = slot_of(r, g, 0, name);
    if(!p) die("an entry of the test tree is not where it was");
    return p;
}

static void d_none(struct ram *r, const struct geo3 *g) { (void)r; (void)g; }

static void d_dirty(struct ram *r, const struct geo3 *g) {
    if(g->type == 12) return;
    fset(r, g, 1, g->type == 16 ? 0x7FFF : 0x07FFFFFF);
}

static void d_fsinfo(struct ram *r, const struct geo3 *g) {
    if(g->fsi) wr32(r->mem + g->fsi * 512 + 488, 12345);
}

static void d_fsinfo_sig(struct ram *r, const struct geo3 *g) {
    if(g->fsi) wr32(r->mem + g->fsi * 512, 0x12345678);
}

/* the second FAT alone has a cluster in use, in a sector of the first that has others: all zeros there would be another matter */
static void d_fat2(struct ram *r, const struct geo3 *g) {
    u32 c;
    for(c = 2; fget(r->mem, g, g->fat1, c); c++) ;
    fset1(r->mem, g, g->fat2, c, g->eoc);
}

static void d_out(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_README);
    fset(r, g, step(r, g, p, 2), g->end + 5);
}

static void d_free_link(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_README);
    fset(r, g, step(r, g, p, 3), 0);
}

static void d_loop(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_README);
    fset(r, g, step(r, g, p, 4), step(r, g, p, 1));
}

static void d_bad_cluster(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_README);
    fset(r, g, step(r, g, p, 2), g->bad);
}

static void d_size_past(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    wr32(p + 28, rd32(p + 28) + 2 * g->cb);
}

static void d_size_none(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, "EMPTY   TXT");
    wr32(p + 28, 3000);
}

static void d_long(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_OTHER);
    wr32(p + 28, 2 * g->cb);
}

static void d_long_zero(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    wr32(p + 28, 0);
}

static void d_first_out(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    eset(g, p, g->end + 9);
}

static void d_first_free(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    fset(r, g, efirst(g, p), 0);
}

/* a second entry on README's chain, as an interrupted rename leaves it */
static void d_twin(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_README), *q = slot_free(r, g, 0);
    memcpy(q, p, 32);
    memcpy(q, "TWIN    TXT", 11);
}

/* one in another directory */
static void d_twin_far(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_OTHER), *q = slot_free(r, g, efirst(g, root_entry(r, g, E_DOCS)));
    memcpy(q, p, 32);
    memcpy(q, "TWIN    DAT", 11);
}

/* a twin that needs less than the chain: the first entry's chain is longer than its size, and the twin's is not */
static void d_twin_sizes(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_OTHER), *q = slot_free(r, g, 0);
    memcpy(q, p, 32);
    memcpy(q, "TWIN    DAT", 11);
    wr32(p + 28, g->cb);                                /* OTHER.DAT, walked first, keeps one cluster: its twin wants all six */
}

/* SMALL's chain runs into OTHER's, past its own two clusters: a join */
static void d_join(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL), *o = root_entry(r, g, E_OTHER);
    fset(r, g, step(r, g, p, 1), step(r, g, o, 2));
    wr32(p + 28, 5 * g->cb);
}

/* the same, the join past SMALL's size */
static void d_join_past(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL), *o = root_entry(r, g, E_OTHER);
    fset(r, g, step(r, g, p, 1), step(r, g, o, 2));
}

static u32 lost_make(struct ram *r, const struct geo3 *g, u32 n, u32 tail, u32 seed) {
    u32 i, first = 0, prev = 0;
    for(i = 0; i < n; i++) {
        u32 c = free_cluster(r, g, 40 + i * 2 + seed * 64);
        memset(r->mem + clba(g, c) * 512, (int)(seed + i + 1), g->cb);
        fset(r, g, c, g->eoc);
        if(prev) fset(r, g, prev, c);
        else first = c;
        prev = c;
    }
    if(tail) fset(r, g, prev, tail);
    return first;
}

static void d_lost(struct ram *r, const struct geo3 *g) {
    lost_make(r, g, 3, 0, 0);
    lost_make(r, g, 1, 0, 1);
}

static void d_lost_ring(struct ram *r, const struct geo3 *g) {
    u32 first = lost_make(r, g, 3, 0, 0);
    u32 c = first, v;
    while((v = fget(r->mem, g, g->fat1, c)) < g->bad) c = v;
    fset(r, g, c, first);
}

/* a lost chain that runs into a file's: it stays reported. One that ends in a link to nothing is saved */
static void d_lost_into(struct ram *r, const struct geo3 *g) {
    lost_make(r, g, 2, step(r, g, root_entry(r, g, E_OTHER), 3), 0);
    lost_make(r, g, 2, g->end + 3, 1);
}

/* two lost chains into one tail */
static void d_lost_fork(struct ram *r, const struct geo3 *g) {
    u32 tail = lost_make(r, g, 2, 0, 0);
    lost_make(r, g, 1, tail, 1);
    lost_make(r, g, 2, tail, 2);
}

/* a directory's chain that stops at a bad link: past it there may be entries, so no repair at all. It grows into a second
   cluster first */
static void d_dir_link(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_DOCS);
    u32 c = efirst(g, p), add = free_cluster(r, g, 200);
    memset(r->mem + clba(g, add) * 512, 0, g->cb);
    fset(r, g, c, add);
    fset(r, g, add, g->end + 2);
}

static void d_all(struct ram *r, const struct geo3 *g) {
    d_dirty(r, g);
    d_fsinfo(r, g);
    d_fat2(r, g);
    d_out(r, g);
    d_size_none(r, g);
    d_lost(r, g);
}

/* these stay reported */
static void d_dot(struct ram *r, const struct geo3 *g) {
    u8 *p = r->mem + clba(g, efirst(g, root_entry(r, g, E_DOCS))) * 512;
    eset(g, p + 32, 7);                                 /* ".." of /docs names another cluster */
}

static void d_lfn_sum(struct ram *r, const struct geo3 *g) {
    u32 i;
    u8 *p = r->mem + (g->root_cluster ? clba(g, g->root_cluster) : g->root_lba) * 512;
    for(i = 0; i < 64; i++, p += 32) if((p[11] & 0x3F) == 0x0F) { p[13] ^= 0x55; return; }
    die("no long name to damage");
}

static void d_name(struct ram *r, const struct geo3 *g) { root_entry(r, g, E_SMALL)[3] = '*'; }

/* the same with a value of fatgen103's list that looks harmless, and a space to start a name, and 0x05, which stands for 0xE5 and is no fault */
static void d_name_plus(struct ram *r, const struct geo3 *g) { root_entry(r, g, E_SMALL)[3] = '+'; }
static void d_name_space(struct ram *r, const struct geo3 *g) { root_entry(r, g, E_SMALL)[0] = ' '; }
static void d_name_low(struct ram *r, const struct geo3 *g) { root_entry(r, g, E_SMALL)[4] = 0x05; }
static void d_name_e5(struct ram *r, const struct geo3 *g) { root_entry(r, g, E_SMALL)[0] = 0x05; }

static void d_dup(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL), *q = slot_free(r, g, 0);
    memset(q, 0, 32);
    memcpy(q, p, 11);
    q[11] = 0x20;
}

static void d_dir_twin(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_DOCS), *q = slot_free(r, g, 0);
    memcpy(q, p, 32);
    memcpy(q, "DOCS2      ", 11);
}

static void d_dir_size(struct ram *r, const struct geo3 *g) { wr32(root_entry(r, g, E_DOCS) + 28, 512); }

/* the walk is not whole: nothing is repaired, however repairable */
static void d_dir_first(struct ram *r, const struct geo3 *g) {
    eset(g, root_entry(r, g, E_DOCS), g->end + 1);
    d_size_none(r, g);
}

/* zeros in the first FAT alone, over a file's chain: the second knows it whole, and cut, the file would lose what the
   second still has. Nothing but that dispute stands in the way: no directory is touched, so the walk is whole */
static void d_fat1_zero(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_README);
    fset1(r->mem, g, g->fat1, step(r, g, p, 3), 0);
}

/* a chain that needs an end at a FAT12 entry across two sectors: one write cannot make that, and a half-written entry
   links anywhere: no repair at all */
static void d_straddle(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    u32 c = 341;                                        /* (341 + 170 = 511: its entry starts in a sector's last byte) */
    if(fget(r->mem, g, g->fat1, c)) die("cluster 341 of the FAT12 image is in use");
    fset(r, g, step(r, g, p, 1), c);
    fset(r, g, c, g->end + 7);
    wr32(p + 28, 3 * g->cb);
    lost_make(r, g, 2, 0, 3);
}

/* the same, the entry across two sectors the free cluster a chain runs into: it stays, as the end, and one write cannot say so */
static void d_straddle_free(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    fset(r, g, step(r, g, p, 1), 341);
    wr32(p + 28, 3 * g->cb);
    lost_make(r, g, 2, 0, 3);
}

/* a directory's next cluster, full of names, marked free in the FAT: the driver reads it */
static void d_dir_free_link(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_DOCS), *q;
    u32 c = efirst(g, p), add = free_cluster(r, g, 200);
    memset(r->mem + clba(g, add) * 512, 0, g->cb);
    q = slot_free(r, g, c);
    while(q) { *q = 0xE5; q = slot_free(r, g, c); }     /* (no end marker before it) */
    q = r->mem + clba(g, add) * 512;
    memcpy(q, root_entry(r, g, E_SMALL), 32);
    memcpy(q, "PASTCUT DAT", 11);
    fset(r, g, c, add);
    fset(r, g, add, 0);
    d_size_none(r, g);
}

/* a file with its directory bit on: its bytes are no entries. It has none here that a repair would want, but it is not walked */
static void d_dir_bit(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_OTHER);
    p[11] |= 0x10;
    d_size_none(r, g);
}

/* the same, with bytes that read as an entry a repair would want. Taken for a directory, the "repair" would write into
   the file: that it is not walked is all that stands in the way */
static void d_dir_fake(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_OTHER), *q = r->mem + clba(g, efirst(g, p)) * 512;
    p[11] |= 0x10;
    memset(q, 0, g->cb);
    memcpy(q, "FAKE    TXT", 11);
    q[11] = 0x20;
    wr32(q + 28, 5000);
}

/* a link in the first FAT alone that runs one file into another: the FATs disagree about a chain */
static void d_fat1_join(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL), *o = root_entry(r, g, E_OTHER);
    fset1(r->mem, g, g->fat1, step(r, g, p, 1), step(r, g, o, 2));
    wr32(p + 28, 5 * g->cb);
}

/* a directory that lost its directory bit: the size is 0, and the chain must stay its own */
static void d_file_bit(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_DOCS);
    p[11] &= (u8)~0x10;
}

/* a problem that stays, on a volume marked unclean: the mark stays */
static void d_dirty_dot(struct ram *r, const struct geo3 *g) {
    d_dirty(r, g);
    d_dot(r, g);
}

/* a volume with no free cluster: every free one marked bad */
static void all_bad(struct ram *r, const struct geo3 *g) {
    u32 c;
    for(c = 2; c < g->end; c++) if(!fget(r->mem, g, g->fat1, c)) fset(r, g, c, g->bad);
}

static void d_full_twin(struct ram *r, const struct geo3 *g) {
    d_twin(r, g);
    all_bad(r, g);
}

/* the one free cluster left is one a FAT12 entry across two sectors would have to mark */
static void d_full_straddle(struct ram *r, const struct geo3 *g) {
    u32 c;
    lost_make(r, g, 2, 0, 0);
    for(c = 2; c < g->end; c++) if(c != 341 && !fget(r->mem, g, g->fat1, c)) fset(r, g, c, g->bad);
}

/* lost clusters and nowhere to name them */
static void d_full_lost(struct ram *r, const struct geo3 *g) {
    lost_make(r, g, 2, 0, 0);
    all_bad(r, g);
}

static void fsi_set(struct ram *r, const struct geo3 *g) {
    u32 c, n = 0;
    if(!g->fsi) return;
    for(c = 2; c < g->end; c++) if(!fget(r->mem, g, g->fat1, c)) n++;
    wr32(r->mem + g->fsi * 512 + 488, n);
}

/* a root with no free slot */
static void root_full(struct ram *r, const struct geo3 *g) {
    u8 *p;
    u32 n = 0;
    while((p = slot_free(r, g, 0)) != NULL) {
        memset(p, 0, 32);
        snprintf((char *)p, 12, "FILL%04u   ", n++);
        p[11] = 0x20;
    }
}

static void d_root_full(struct ram *r, const struct geo3 *g) {
    lost_make(r, g, 2, 0, 0);
    root_full(r, g);
    fsi_set(r, g);
}

/* bytes past the end marker: a zeroed slot may have cut the directory short */
static void d_root_junk(struct ram *r, const struct geo3 *g) {
    u8 *p = slot_free(r, g, 0);
    lost_make(r, g, 2, 0, 0);
    memset(p + 32, 0x41, 32);
    fsi_set(r, g);
}

/* the name FOUND.000 taken: by a file, and FOUND.001 by a long name with another alias: FOUND.002 it is */
static void d_found_taken(struct ram *r, const struct geo3 *g) {
    u8 *p = slot_free(r, g, 0), *q;
    static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    static const char lname[] = "FOUND.001";
    u32 i;
    u8 sum = 0;
    lost_make(r, g, 2, 0, 0);
    memset(p, 0, 32);
    memcpy(p, "FOUND   000", 11);
    p[11] = 0x20;
    q = p + 32;
    memset(q, 0, 64);
    memcpy(q + 32, "OTHERN~1   ", 11);
    q[32 + 11] = 0x20;
    for(i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + q[32 + i]);
    q[0] = 0x41;
    q[11] = 0x0F;
    q[13] = sum;
    for(i = 0; i < 13; i++) wr16(q + pos[i], i < 9 ? (u32)(u8)lname[i] : i == 9 ? 0 : 0xFFFF);
    fsi_set(r, g);
}

/* the one FAT active that there is not */
static void d_mirror_none(struct ram *r, const struct geo3 *g) {
    if(g->type != 32) return;
    r->mem[40] = 0x85;
}

/* one FAT active, the second not a mirror: nothing here repairs that. The backup says so too: that is not what stops it */
static void d_mirror(struct ram *r, const struct geo3 *g) {
    if(g->type != 32) return;
    r->mem[40] = 0x80;
    r->mem[g->bk * 512 + 40] = 0x80;
    d_size_none(r, g);
}

/* only the backup differs, and in the volume's shape: which of the two is right is not known */
static void d_backup_shape(struct ram *r, const struct geo3 *g) {
    r->mem[g->bk * 512 + 13] ^= 2;
    d_size_none(r, g);
}

/* only the backup differs, and not in the volume's shape: harmless, and said */
static void d_backup(struct ram *r, const struct geo3 *g) {
    if(g->bk) r->mem[g->bk * 512 + 3] ^= 1;
}

static void d_boot_flag(struct ram *r, const struct geo3 *g) {
    r->mem[(g->type == 32 ? 64 : 36) + 1] |= 1;
    d_size_none(r, g);
}

/**** what the reviews found: each of these is a repair that lost something, or a walk that went where it should not ****/

static u32 fat_sec(const struct geo3 *g, u32 c) { return (g->type == 12 ? c + c / 2 : g->type == 16 ? c * 2 : c * 4) / 512; }

/* a file by hand, in directory `dir` (0: the root), at the top of the volume: far from the FAT sector the tree is in. Its
   clusters in raw_chain[] */
static u32 raw_chain[64], raw_skip = 12;

static u8 *raw_file(struct ram *r, const struct geo3 *g, u32 dir, const char *name, u32 n, u32 seed) {
    u8 *q = slot_free(r, g, dir);
    u32 i;
    if(!q || n > 64) die("no room for a file by hand");
    for(i = 0; i < n; i++) {
        u32 c = free_cluster(r, g, raw_skip);
        unique_fill(r->mem + clba(g, c) * 512, g->cb, 1000 + seed, i * g->cb);
        fset(r, g, c, g->eoc);
        if(i) fset(r, g, raw_chain[i - 1], c);
        raw_chain[i] = c;
    }
    memset(q, 0, 32);
    memcpy(q, name, 11);
    q[11] = 0x20;
    eset(g, q, raw_chain[0]);
    wr32(q + 28, n * g->cb);
    return q;
}

/* a directory by hand in the root, its one cluster free of entries */
static u32 raw_dir_make(struct ram *r, const struct geo3 *g, const char *name) {
    u32 c = free_cluster(r, g, 12);
    u8 *q = slot_free(r, g, 0), *e = r->mem + clba(g, c) * 512;
    memset(e, 0, g->cb);
    memcpy(e, ".          ", 11);
    e[11] = 0x10;
    eset(g, e, c);
    memcpy(e + 32, "..         ", 11);
    e[32 + 11] = 0x10;
    fset(r, g, c, g->eoc);
    memset(q, 0, 32);
    memcpy(q, name, 11);
    q[11] = 0x10;
    eset(g, q, c);
    return c;
}

/* (K1) a whole one-part long name, then a slot of the set with number 0: read as its next part, it indexes before lfn[] */
static void d_lfn0(struct ram *r, const struct geo3 *g) {
    u8 *q = slot_free(r, g, 0), sum = 0;
    u32 i;
    static const char name[] = "LFNZERO TXT";
    for(i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + (u8)name[i]);
    memset(q, 0, 96);
    q[0] = 0x41; q[11] = 0x0F; q[13] = sum;
    q[32] = g->type == 32 ? 0x20 : g->type == 16 ? 0x80 : 0xA0;
    q[32 + 11] = 0x0F; q[32 + 13] = sum;
    for(i = 1; i < 11; i += 2) { q[i] = 'a'; q[32 + i] = 0xEE; q[32 + i + 1] = 0xEE; }
    memcpy(q + 64, name, 11);
    q[64 + 11] = 0x20;
}

/* (D1) a chain into a free cluster, the one a repair would take first, with lost chains to save: the driver reads that cluster */
static void d_free_taken(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    u32 f;
    for(f = 2; fget(r->mem, g, g->fat1, f); f++) ;
    if(g->type == 12 && (f + f / 2) % 512 == 511) die("the first free cluster straddles");
    unique_fill(r->mem + clba(g, f) * 512, g->cb, 2000, 0);
    fset(r, g, step(r, g, p, 1), f);
    wr32(p + 28, 3 * g->cb);
    lost_make(r, g, 2, 0, 1);
}

/* (D1) the first cluster free, and everything after it free too: fs/fat.c reads that one cluster */
static void d_first_free_only(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    u32 c = efirst(g, p), c2 = fget(r->mem, g, g->fat1, c);
    fset(r, g, c, 0);
    fset(r, g, c2, 0);
    lost_make(r, g, 1, 0, 2);
}

/* (D2) one sector of the first FAT erased over a file's chain, the second whole */
static void d_fat1_ff(struct ram *r, const struct geo3 *g) {
    raw_file(r, g, 0, "ALBUM   BIN", 6, 1);
    memset(r->mem + (g->fat1 + fat_sec(g, raw_chain[2])) * 512, 0xFF, 512);
}

/* (D6) the same with no chain through it, so that no link says anything: only the sector being blank does */
static void d_fat1_ff_free(struct ram *r, const struct geo3 *g) {
    memset(r->mem + (g->fat1 + fat_sec(g, free_cluster(r, g, 700))) * 512, 0xFF, 512);
}

/* (D6) all zeros in the first, where the second has clusters in use: the first may be the one that lost them. No name is
   concerned, and nothing but the sector being blank says anything */
static void d_fat1_zero_only(struct ram *r, const struct geo3 *g) {
    u32 c = free_cluster(r, g, 700);
    fset1(r->mem, g, g->fat2, c, g->eoc);
}

/* (D6) one FAT, and a sector of it in the middle erased: nothing to compare it with, and it holds a file */
static void d_fat_ff_one(struct ram *r, const struct geo3 *g) {
    raw_skip = 700;
    raw_file(r, g, 0, "ALBUM   BIN", 6, 12);
    raw_skip = 12;
    memset(r->mem + (g->fat1 + fat_sec(g, raw_chain[2])) * 512, 0xFF, 512);
}

/* (D6) the same gone to zeros: all that says it was no free space is the chain that runs into it */
static void d_fat_zero_one(struct ram *r, const struct geo3 *g) {
    raw_skip = 700;
    raw_file(r, g, 0, "ALBUM   BIN", 6, 13);
    raw_skip = 12;
    memset(r->mem + (g->fat1 + fat_sec(g, raw_chain[2])) * 512, 0, 512);
}

/* (D6) the same, gone to zeros */
static void d_fat1_blank(struct ram *r, const struct geo3 *g) {
    raw_file(r, g, 0, "ALBUM   BIN", 6, 1);
    memset(r->mem + (g->fat1 + fat_sec(g, raw_chain[2])) * 512, 0, 512);
}

/* (D2) the first FAT ends a file early, its tail free; the second has it whole */
static void d_fat1_early(struct ram *r, const struct geo3 *g) {
    u32 i;
    raw_file(r, g, 0, "ALBUM   BIN", 12, 2);
    fset1(r->mem, g, g->fat1, raw_chain[3], g->eoc);
    for(i = 4; i < 12; i++) fset1(r->mem, g, g->fat1, raw_chain[i], 0);
}

/* (D2) the first FAT ends a directory early: the second knows a second cluster, full of names */
static void d_fat1_dir_early(struct ram *r, const struct geo3 *g) {
    u32 d = raw_dir_make(r, g, "DIRTWO     "), add = free_cluster(r, g, 12), i;
    u8 *e = r->mem + clba(g, d) * 512, *q = r->mem + clba(g, add) * 512;
    for(i = 2; i < g->cb / 32; i++) e[i * 32] = 0xE5;
    memset(q, 0, g->cb);
    raw_file(r, g, 0, "ONDIR   BIN", 2, 3);
    memcpy(q, slot_of(r, g, 0, "ONDIR   BIN"), 32);
    *slot_of(r, g, 0, "ONDIR   BIN") = 0xE5;
    fset(r, g, d, add);
    fset(r, g, add, g->eoc);
    fset1(r->mem, g, g->fat1, d, g->eoc);
}

/* (D3) the first FAT alone has a link to a cluster free in both; the second has the true link */
static void d_fat1_free_link(struct ram *r, const struct geo3 *g) {
    u32 f;
    raw_file(r, g, 0, "ALBUM   BIN", 4, 4);
    f = free_cluster(r, g, 20);
    fset1(r->mem, g, g->fat1, raw_chain[1], f);
}

/* (D3) the same, to a cluster marked bad in both */
static void d_fat1_bad_link(struct ram *r, const struct geo3 *g) {
    u32 f;
    raw_file(r, g, 0, "ALBUM   BIN", 4, 5);
    f = free_cluster(r, g, 20);
    fset(r, g, f, g->bad);
    fset1(r->mem, g, g->fat1, raw_chain[1], f);
}

/* (D3) the same, the second ending the chain there: only the bad link itself is different */
static void d_fat1_free_end(struct ram *r, const struct geo3 *g) {
    u32 low;
    raw_file(r, g, 0, "ALBUM   BIN", 4, 14);
    for(low = 2; fget(r->mem, g, g->fat1, low); low++) ;
    fset1(r->mem, g, g->fat1, raw_chain[1], low);
    fset1(r->mem, g, g->fat2, raw_chain[1], g->eoc);
}

/* (D3) a link to no cluster in the first FAT alone */
static void d_fat1_out(struct ram *r, const struct geo3 *g) {
    raw_file(r, g, 0, "ALBUM   BIN", 4, 6);
    fset1(r->mem, g, g->fat1, raw_chain[1], g->end + 3);
}

/* (D3) the same, the second ending the chain there */
static void d_fat1_out_end(struct ram *r, const struct geo3 *g) {
    raw_file(r, g, 0, "ALBUM   BIN", 4, 15);
    fset1(r->mem, g, g->fat1, raw_chain[1], g->end + 3);
    fset1(r->mem, g, g->fat2, raw_chain[1], g->eoc);
}

/* (D3) the FATs each with their own link, both in range */
static void d_fat1_other_link(struct ram *r, const struct geo3 *g) {
    u32 f;
    raw_file(r, g, 0, "ALBUM   BIN", 4, 7);
    f = free_cluster(r, g, 20);
    fset(r, g, f, g->eoc);
    fset1(r->mem, g, g->fat1, raw_chain[1], f);
}

/* the FATs differ past a file's size only, and on clusters nothing names: the second is overwritten, and nothing is lost by it */
static void d_fat2_past(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    u32 f = free_cluster(r, g, 20);
    fset1(r->mem, g, g->fat2, step(r, g, p, 1), f);
}

/* the second FAT is what is damaged, a sector of it erased: the first is sound */
static void d_fat2_ff(struct ram *r, const struct geo3 *g) {
    raw_file(r, g, 0, "ALBUM   BIN", 6, 8);
    memset(r->mem + (g->fat2 + fat_sec(g, raw_chain[2])) * 512, 0xFF, 512);
}

/* a repair's own half-done work: each ending of a chain stopped between the two FATs. A second run must finish these */
static void d_half_out(struct ram *r, const struct geo3 *g) {
    d_out(r, g);
    fset1(r->mem, g, g->fat1, step(r, g, root_entry(r, g, E_README), 2), g->eoc);
}

static void d_half_free(struct ram *r, const struct geo3 *g) {
    u32 c = step(r, g, root_entry(r, g, E_README), 3);
    d_free_link(r, g);
    fset1(r->mem, g, g->fat1, c, g->eoc);
}

static void d_half_loop(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_README);
    u32 c = step(r, g, p, 4), to = step(r, g, p, 1);
    fset1(r->mem, g, g->fat2, c, to);
    fset1(r->mem, g, g->fat1, c, g->eoc);
}

/* (D4) a directory's full cluster links, in both FATs, to the second cluster of a file walked after it: a log, whose records
   read as entries a repair would want. It is read as the directory, and the file's chain joins it */
static void d_dir_into_file(struct ram *r, const struct geo3 *g) {
    u32 c = efirst(g, root_entry(r, g, E_DOCS)), z = raw_dir_make(r, g, "ZZZZ       "), i;
    u8 *q;
    raw_file(r, g, z, "DATA    LOG", 2, 9);
    for(q = slot_free(r, g, c); q; q = slot_free(r, g, c)) *q = 0xE5;
    q = r->mem + clba(g, raw_chain[1]) * 512;
    for(i = 0; i < g->cb / 32; i++, q += 32) {
        snprintf((char *)q, 12, "REC%05u   ", i);
        q[11] = 0x20;
        memset(q + 12, 0, 20);
        wr32(q + 28, 1000 + i);
    }
    fset(r, g, c, raw_chain[1]);
}

/* (D4) a file's chain runs into a directory's */
static void d_file_into_dir(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    fset(r, g, step(r, g, p, 1), efirst(g, root_entry(r, g, E_DOCS)));
    wr32(p + 28, 3 * g->cb);
}

/* (D5) one bit of the cluster size: FAT32 has a backup that says otherwise, the others have directories that stop reading as such */
static void d_boot_spc(struct ram *r, const struct geo3 *g) {
    d_size_none(r, g);
    r->mem[13] = (u8)(r->mem[13] * 2);
}

/* the docs tree gone, cleanly: a volume of files in the root, where nothing reads as a directory that could give away a wrong
   cluster size */
static void chain_free(struct ram *r, const struct geo3 *g, u32 c) {
    while(c >= 2 && c < g->bad) {
        u32 v = fget(r->mem, g, g->fat1, c);
        fset(r, g, c, 0);
        c = v;
    }
}

static void dir_free(struct ram *r, const struct geo3 *g, u32 first) {
    u32 c, i;
    for(c = first; c >= 2 && c < g->bad; c = fget(r->mem, g, g->fat1, c)) {
        u8 *q = r->mem + clba(g, c) * 512;
        for(i = 0; i < g->cb / 32 && q[0]; i++, q += 32) {
            if(q[0] == 0xE5 || (q[11] & 0x0F) == 0x0F || q[0] == '.') continue;
            if(q[11] & 0x10) dir_free(r, g, efirst(g, q));
            else chain_free(r, g, efirst(g, q));
        }
    }
    chain_free(r, g, first);
}

/* (D5) one bit of the cluster size on such a volume: every chain is then longer than its size, and the size is right */
static void d_boot_flat(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_DOCS);
    dir_free(r, g, efirst(g, p));
    *p = 0xE5;
    d_size_none(r, g);
    r->mem[13] = (u8)(r->mem[13] * 2);
}

/* (D5) the volume's size cut, its FAT as it was: far larger than its clusters need, and nothing else to see */
static void d_boot_total(struct ram *r, const struct geo3 *g) {
    d_size_none(r, g);
    wr16(r->mem + 19, g->data_lba + (g->type == 12 ? g->n / 2 : g->n / 5 * 3) * g->spc);    /* (FAT16 stays FAT16) */
}

/* (D5) a tenth off the size: the FAT fits, a file past the cut has entries past the last, and its link looks like one to no cluster */
static void d_boot_tail(struct ram *r, const struct geo3 *g) {
    u8 *q = raw_file(r, g, 0, "ALBUM   BIN", 3, 10);
    u32 low;
    for(low = 2; fget(r->mem, g, g->fat1, low); low++) ;
    fset(r, g, low, raw_chain[0]);
    eset(g, q, low);
    wr32(q + 28, 4 * g->cb);
    wr16(r->mem + 19, g->data_lba + (g->n - g->n / 10) * g->spc);
}

/* (D9) a slot of the root zeroed: what comes after it is not the end of anything */
static void d_slot_zero(struct ram *r, const struct geo3 *g) {
    memset(root_entry(r, g, E_SMALL), 0, 32);
    d_size_none(r, g);
}

/* (D9) a directory with no cluster, and nothing lost to show for it */
static void d_dir_none(struct ram *r, const struct geo3 *g) {
    u8 *q = slot_free(r, g, 0);
    memset(q, 0, 32);
    memcpy(q, "NOCLUS     ", 11);
    q[11] = 0x10;
    d_size_none(r, g);
}

/* a directory that lost its bit, with only a file under it: nothing starts as a directory among what is lost */
static void d_file_bit_leaf(struct ram *r, const struct geo3 *g) {
    u32 z = raw_dir_make(r, g, "LEAF       ");
    raw_file(r, g, z, "UNDER   BIN", 2, 17);
    slot_of(r, g, 0, "LEAF       ")[11] = 0x20;
    d_size_none(r, g);
}

/* the reserved FAT entries, wrong in both FATs: what says this is the FAT the boot sector means */
static void d_fat0(struct ram *r, const struct geo3 *g) {
    fset(r, g, 0, g->type == 12 ? 0xFF0 : g->type == 16 ? 0xFFF0 : 0x0FFFFFF0);
    d_size_none(r, g);
}

static void d_fat1_entry(struct ram *r, const struct geo3 *g) {
    fset(r, g, 1, 0x123);
    d_size_none(r, g);
}

/* (D9) a label with clusters */
static void d_label_clusters(struct ram *r, const struct geo3 *g) {
    u8 *q = slot_free(r, g, 0);
    memset(q, 0, 32);
    memcpy(q, "A LABEL    ", 11);
    q[11] = 0x08;
    eset(g, q, efirst(g, root_entry(r, g, E_SMALL)));
    d_size_none(r, g);
}

/* a "." that names another cluster: this block may be no block of this directory */
static void d_dot_self(struct ram *r, const struct geo3 *g) {
    eset(g, r->mem + clba(g, efirst(g, root_entry(r, g, E_DOCS))) * 512, 7);
    d_size_none(r, g);
}

/* (D6) a directory no name reaches: all under it is out of reach, and a pile of CHK files is no way to get it back */
static void d_lost_dir(struct ram *r, const struct geo3 *g) {
    *root_entry(r, g, E_DOCS) = 0xE5;
    d_size_none(r, g);
}

/* (D6) most of what is in use out of reach */
static void d_majority(struct ram *r, const struct geo3 *g) {
    *root_entry(r, g, E_README) = 0xE5;
    *root_entry(r, g, E_OTHER) = 0xE5;
    *root_entry(r, g, "FRAG1      ") = 0xE5;
    *root_entry(r, g, "FRAG2      ") = 0xE5;
    d_size_none(r, g);
}

/* (D6) a root sector torn to zeros, and to ones */
static u8 *root_sector(struct ram *r, const struct geo3 *g) {
    return r->mem + (g->root_cluster ? clba(g, g->root_cluster) : g->root_lba) * 512;
}

static void d_root_zero(struct ram *r, const struct geo3 *g) { memset(root_sector(r, g), 0, 512); }
static void d_root_ones(struct ram *r, const struct geo3 *g) { memset(root_sector(r, g), 0xFF, 512); }

/* two chains into one free cluster, in a sector of the FAT with others */
static void d_join_free(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL), *o = root_entry(r, g, E_OTHER);
    u32 f;
    for(f = 2; fget(r->mem, g, g->fat1, f); f++) ;
    fset(r, g, step(r, g, p, 1), f);
    fset(r, g, step(r, g, o, 5), f);
}

/* a lost chain into a free cluster: that one stays with it */
static void d_lost_free(struct ram *r, const struct geo3 *g) {
    lost_make(r, g, 2, free_cluster(r, g, 20), 0);
}

/* a lost chain to no cluster */
static void d_lost_out(struct ram *r, const struct geo3 *g) {
    lost_make(r, g, 2, g->end + 3, 0);
}

/* a lost chain into a cluster marked bad: it stays reported */
static void d_lost_bad(struct ram *r, const struct geo3 *g) {
    u32 f = free_cluster(r, g, 20);
    fset(r, g, f, g->bad);
    lost_make(r, g, 2, f, 0);
}

/* a pull between FOUND's clusters and its name: a directory of ours with no name. It is saved like the rest */
static void d_orphan(struct ram *r, const struct geo3 *g) {
    u32 c = free_cluster(r, g, 30);
    u8 *e = r->mem + clba(g, c) * 512;
    memset(e, 0, g->cb);
    memcpy(e, ".          ", 11);
    e[11] = 0x10;
    eset(g, e, c);
    memcpy(e + 32, "..         ", 11);
    e[32 + 11] = 0x10;
    memcpy(e + 64, "FILE0000CHK", 11);
    e[64 + 11] = 0x20;
    fset(r, g, c, g->eoc);
}

/* a root of two clusters, the first full: FOUND's name goes into the second */
static void d_root_two(struct ram *r, const struct geo3 *g) {
    u32 add;
    u8 *p;
    lost_make(r, g, 2, 0, 0);
    for(p = slot_free(r, g, 0); p; p = slot_free(r, g, 0)) *p = 0xE5;
    add = free_cluster(r, g, 200);
    memset(r->mem + clba(g, add) * 512, 0, g->cb);
    fset(r, g, g->root_cluster, add);
    fset(r, g, add, g->eoc);
    fsi_set(r, g);
}

/**** one scenario, all the way ****/

#define EX_CLEAN  0                     /* the repair ends with nothing left */
#define EX_LEFT   1                     /* problems stay */
#define EX_BARRED 2                     /* no repair at all: nothing is written */
#define EX_NOTE   3                     /* no problem at all, only something said */

#define ON_ALL   0
#define ON_FAT32 1
#define ON_FAT12 2
#define ON_TWO   3                      /* two FATs */
#define ON_NO32  4                      /* no backup of the boot sector */
#define ON_ONE   5                      /* one FAT */

/* file, clusters: what the repair must leave: that root entry with exactly so many clusters, and its size at most that
   (it says where the chain ends, which "nothing lost" alone does not) */
struct scen { const char *name; void (*damage)(struct ram *, const struct geo3 *); int expect; int on; const char *file; int clusters; };

static const struct scen scens[] = {
    { "none", d_none, EX_CLEAN, 0 },
    { "dirty", d_dirty, EX_CLEAN, 0 },
    { "fsinfo", d_fsinfo, EX_CLEAN, ON_FAT32 },
    { "fsinfo-signature", d_fsinfo_sig, EX_LEFT, ON_FAT32 },
    { "fat2", d_fat2, EX_CLEAN, ON_TWO },
    { "fat2-past", d_fat2_past, EX_CLEAN, ON_TWO },
    { "fat2-ff", d_fat2_ff, EX_CLEAN, ON_TWO },
    { "half-out", d_half_out, EX_CLEAN, ON_TWO, E_README, 3 },
    { "half-free", d_half_free, EX_CLEAN, ON_TWO, E_README, 4 },
    { "half-loop", d_half_loop, EX_CLEAN, ON_TWO, E_README, 5 },
    { "out", d_out, EX_CLEAN, 0, E_README, 3 },
    { "free-link", d_free_link, EX_CLEAN, 0, E_README, 4 },
    { "free-taken", d_free_taken, EX_CLEAN, 0, E_SMALL, 3 },
    { "loop", d_loop, EX_CLEAN, 0, E_README, 5 },
    { "bad-cluster", d_bad_cluster, EX_BARRED, 0 },
    { "size-past", d_size_past, EX_CLEAN, 0, E_SMALL, 2 },
    { "size-none", d_size_none, EX_CLEAN, 0, "EMPTY   TXT", 0 },
    { "long", d_long, EX_LEFT, 0 },
    { "long-zero", d_long_zero, EX_LEFT, 0 },
    { "first-out", d_first_out, EX_BARRED, 0 },
    { "first-free", d_first_free, EX_CLEAN, 0, E_SMALL, 1 },
    { "first-free-only", d_first_free_only, EX_CLEAN, 0, E_SMALL, 1 },
    { "twin", d_twin, EX_BARRED, 0 },
    { "twin-far", d_twin_far, EX_BARRED, 0 },
    { "twin-sizes", d_twin_sizes, EX_BARRED, 0 },
    { "join", d_join, EX_BARRED, 0 },
    { "join-past", d_join_past, EX_BARRED, 0 },
    { "join-free", d_join_free, EX_BARRED, 0 },
    { "lost", d_lost, EX_CLEAN, 0 },
    { "lost-ring", d_lost_ring, EX_CLEAN, 0 },
    { "lost-free", d_lost_free, EX_CLEAN, 0 },
    { "lost-out", d_lost_out, EX_CLEAN, 0 },
    { "lost-orphan", d_orphan, EX_CLEAN, 0 },
    { "lost-into", d_lost_into, EX_LEFT, 0 },
    { "lost-fork", d_lost_fork, EX_LEFT, 0 },
    { "lost-bad", d_lost_bad, EX_LEFT, 0 },
    { "lost-dir", d_lost_dir, EX_BARRED, 0 },
    { "lost-most", d_majority, EX_BARRED, 0 },
    { "dir-link", d_dir_link, EX_BARRED, 0 },
    { "all", d_all, EX_CLEAN, ON_TWO },
    { "root-two", d_root_two, EX_CLEAN, ON_FAT32 },
    { "dot", d_dot, EX_LEFT, 0 },
    { "lfn-sum", d_lfn_sum, EX_LEFT, 0 },
    { "lfn-zero", d_lfn0, EX_LEFT, 0 },
    { "name", d_name, EX_LEFT, 0 },
    { "name-plus", d_name_plus, EX_LEFT, 0 },
    { "name-space", d_name_space, EX_LEFT, 0 },
    { "name-low", d_name_low, EX_LEFT, 0 },
    { "name-05", d_name_e5, EX_NOTE, 0 },
    { "dup", d_dup, EX_LEFT, 0 },
    { "dir-size", d_dir_size, EX_LEFT, 0 },
    { "dot-self", d_dot_self, EX_BARRED, 0 },
    { "dir-twin", d_dir_twin, EX_BARRED, 0 },
    { "dir-first", d_dir_first, EX_BARRED, 0 },
    { "dir-none", d_dir_none, EX_BARRED, 0 },
    { "dir-free-link", d_dir_free_link, EX_BARRED, 0 },
    { "dir-bit", d_dir_bit, EX_BARRED, 0 },
    { "dir-fake", d_dir_fake, EX_BARRED, 0 },
    { "dir-into-file", d_dir_into_file, EX_BARRED, 0 },
    { "file-into-dir", d_file_into_dir, EX_BARRED, 0 },
    { "file-bit", d_file_bit, EX_BARRED, 0 },
    { "file-bit-leaf", d_file_bit_leaf, EX_BARRED, 0 },
    { "label-clusters", d_label_clusters, EX_BARRED, 0 },
    { "slot-zero", d_slot_zero, EX_BARRED, 0 },
    { "root-junk", d_root_junk, EX_BARRED, 0 },
    { "root-zero", d_root_zero, EX_BARRED, 0 },
    { "root-ones", d_root_ones, EX_BARRED, 0 },
    { "fat1-zero", d_fat1_zero, EX_BARRED, ON_TWO },
    { "fat1-join", d_fat1_join, EX_BARRED, ON_TWO },
    { "fat1-ff", d_fat1_ff, EX_BARRED, 0 },
    { "fat1-ff-free", d_fat1_ff_free, EX_BARRED, ON_TWO },
    { "fat1-blank", d_fat1_blank, EX_BARRED, ON_TWO },
    { "fat1-zero-only", d_fat1_zero_only, EX_BARRED, ON_TWO },
    { "fat-ff-one", d_fat_ff_one, EX_BARRED, ON_ONE },
    { "fat-zero-one", d_fat_zero_one, EX_BARRED, ON_ONE },
    { "fat1-early", d_fat1_early, EX_BARRED, ON_TWO },
    { "fat1-dir-early", d_fat1_dir_early, EX_BARRED, ON_TWO },
    { "fat1-free-link", d_fat1_free_link, EX_BARRED, ON_TWO },
    { "fat1-bad-link", d_fat1_bad_link, EX_BARRED, ON_TWO },
    { "fat1-free-end", d_fat1_free_end, EX_BARRED, ON_TWO },
    { "fat1-out", d_fat1_out, EX_BARRED, ON_TWO },
    { "fat1-out-end", d_fat1_out_end, EX_BARRED, ON_TWO },
    { "fat1-other-link", d_fat1_other_link, EX_BARRED, ON_TWO },
    { "boot-spc", d_boot_spc, EX_BARRED, 0 },
    { "boot-flat", d_boot_flat, EX_BARRED, ON_NO32 },
    { "boot-total", d_boot_total, EX_BARRED, ON_NO32 },
    { "boot-tail", d_boot_tail, EX_BARRED, ON_NO32 },
    { "straddle", d_straddle, EX_BARRED, ON_FAT12 },
    { "straddle-free", d_straddle_free, EX_BARRED, ON_FAT12 },
    { "dirty-dot", d_dirty_dot, EX_LEFT, ON_TWO },
    { "full-twin", d_full_twin, EX_BARRED, 0 },
    { "full-lost", d_full_lost, EX_LEFT, 0 },
    { "full-straddle", d_full_straddle, EX_LEFT, ON_FAT12 },
    { "root-full", d_root_full, EX_LEFT, 0 },
    { "found-taken", d_found_taken, EX_CLEAN, 0 },
    { "mirror", d_mirror, EX_BARRED, ON_FAT32 },
    { "mirror-none", d_mirror_none, EX_BARRED, ON_FAT32 },
    { "backup", d_backup, EX_NOTE, ON_FAT32 },
    { "backup-shape", d_backup_shape, EX_BARRED, ON_FAT32 },
    { "reserved-0", d_fat0, EX_BARRED, 0 },
    { "reserved-1", d_fat1_entry, EX_BARRED, 0 },
    { "boot-flag", d_boot_flag, EX_LEFT, 0 },
};

static int nscen_ran, npulls;
static int fsck_fat(struct ram *x, const char *tag);
static void fsck_said(const char *tag);
static int have_fsck;

/* a read-only run: no write, and the image as it was, whatever it found */
static int look(struct ram *x, const u8 *was, struct fat_check_result *res, const char *what) {
    struct tio t;
    char m[200];
    int st;
    t_init(&t, x, 0);
    st = run(&t, 0, res);
    snprintf(m, sizeof(m), "%s: a look writes nothing and asks nothing", what);
    check(t.writes == 0 && t.asks == 0 && !res->wrote && !memcmp(x->mem, was, (size_t)x->bd.nblocks * 512), m);
    t_done(&t);
    return st;
}

/* a whole repair from the image `d` holds */
static int repair(struct tio *t, struct ram *x, struct fat_check_result *res) {
    t_init(t, x, 1);
    t->judged = true;
    return run(t, FAT_CHECK_REPAIR, res);
}

static void restore(struct ram *x, const u8 *d, struct tio *t) {
    u32 i;
    for(i = 0; i < t->ntouched; i++) memcpy(x->mem + t->touched[i] * 512, d + t->touched[i] * 512, 512);
}

/* The repair pulled at write n: that write landed whole or tore (tear), or the stick's cache lost what it pleased (device).
   The volume then holds all it held, by the yardstick; and a further repair, the owner's next step, ends where the whole
   repair ended, holding all of it still. What a stick can do to itself the test cannot undo: a tear can take a sector of
   names, or of the FAT. The promise then is that nothing makes that worse: the bytes are in FOUND, or the run says it is
   not done and has written nothing over them. A flush that lies leaves any order of writes, and may leave a dispute */
static void pull_one(struct ram *x, const u8 *d, const struct yard *y, const struct geo3 *g, const struct scen *s, u32 n, u32 writes, int tearing, int device, int final) {
    struct tio t, u;
    struct fat_check_result res, r2;
    char m[400];
    u32 i;
    int st, bad, how = tearing ? K_NAMES | K_DISK : K_DISK;
    const char *mode = tearing ? "a tear" : device == FLUSH_LIES ? "a flush that lies" : device ? "a cache" : "";
    flush_lied = device == FLUSH_LIES;
    t_init(&t, x, 1);
    t.judged = true;
    t.pull_at = n;
    t.tear = tearing;
    t.device = device;
    (void)run(&t, FAT_CHECK_REPAIR, &res);
    if(device) cache_loses(&t);
    if(!tearing && !device && g->type != 12 && writes > 1 && n < writes) {
        u32 b1 = fget(x->mem, g, g->fat1, 1);
        snprintf(m, sizeof(m), "%s, FAT%d: pulled at write %u of %u: the volume still says it was not unmounted cleanly", s->name, g->type, n, writes);
        check(!(b1 & (g->type == 16 ? 0x8000 : 0x08000000)), m);
    }
    bad = kept(y, x, g, how);
    if(!bad && !tearing) bad = none_freed(d, x->mem, g) || chains_whole(x, d, g, g->root_cluster, 0) || none_shorter(d, x->mem, g);
    snprintf(m, sizeof(m), "%s, FAT%d: pulled at write %u %s: nothing lost (%s)", s->name, g->type, n, mode, bad ? why : "ok");
    check(!bad, m);

    /* The owner plugs it in again, and repairs it. What its writes are judged against is the pulled volume: only the
       sectors the pull's run wrote differ from the damaged one, and a 35 MB copy at each pull would be most of the test */
    for(i = 0; i < t.ntouched; i++) memcpy(before + t.touched[i] * 512, x->mem + t.touched[i] * 512, 512);
    judge_prepare(before, g);
    st = repair(&u, x, &r2);
    if(!tearing && device != FLUSH_LIES) {
        snprintf(m, sizeof(m), "%s, FAT%d: pulled at write %u %s: a second repair ends as the first did (status %d, %u left): %s", s->name,
                 g->type, n, mode, st, r2.left, u.log);
        check(st == final, m);
    }
    how = st ? K_DISK : 0;
    if(tearing) how |= K_NAMES;
    if(!tearing && device != FLUSH_LIES) how = 0;
    bad = kept(y, x, g, how);
    if(!bad && !tearing) bad = none_freed(d, x->mem, g) || none_shorter(d, x->mem, g);
    snprintf(m, sizeof(m), "%s, FAT%d: pulled at write %u %s, repaired again (status %d): nothing lost (%s):\n%s", s->name, g->type, n, mode, st, bad ? why : "ok", bad ? u.log : "");
    check(!bad, m);
    for(i = 0; i < t.ntouched; i++) memcpy(before + t.touched[i] * 512, d + t.touched[i] * 512, 512);
    judge_prepare(before, g);
    restore(x, d, &u);
    t_done(&u);
    restore(x, d, &t);
    t_done(&t);
    flush_lied = 0;
    npulls++;
}

static void scenario(int type, const struct scen *s) {
    struct ram *x = image(type);
    struct geo3 g;
    struct yard y;
    struct tree tf;
    struct fat_check_result r0, r1, r2;
    struct tio t, t2;
    u8 *d;
    char m[400];
    int st, st2, k;
    u32 n, writes;

    if(type <= 12 && (s->damage == d_dirty || s->damage == d_dirty_dot)) return;   /* (FAT12 has no mark to damage) */
    d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    geo3(x->mem, &g);
    bg = g;
    if(fverbose) printf("  %s, FAT%d\n", s->name, type);
    s->damage(x, &g);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    memcpy(before, d, (size_t)x->bd.nblocks * 512);
    judge_prepare(d, &g);
    bad_writes = 0;
    nscen_ran++;

    /* a look */
    st = look(x, d, &r0, s->name);
    {
        int clean = s->damage == d_none || s->expect == EX_NOTE;
        snprintf(m, sizeof(m), "%s, FAT%d: a look says %s (status %d, %u problems)", s->name, type, clean ? "clean" : "problems", st, r0.found);
        check(clean ? st == 0 && !r0.found : st == 1 && r0.found > 0, m);
    }
    yard_read(x, &g, &y);
    if(!y.mounted && s->expect != EX_BARRED) die("a damaged test image does not mount");

    /* a run not asked to repair, with a write function in reach: it never gets to it */
    t_init(&t, x, 1);
    st = run(&t, 0, &r1);
    snprintf(m, sizeof(m), "%s, FAT%d: not asked to repair, nothing written, nothing asked", s->name, type);
    check(t.writes == 0 && t.asks == 0 && !memcmp(x->mem, d, (size_t)x->bd.nblocks * 512), m);
    if(s->expect == EX_BARRED) {
        snprintf(m, sizeof(m), "%s, FAT%d: it says that it will not repair this:\n%s", s->name, type, t.log);
        check(strstr(t.log, "no repair: ") || strstr(t.log, "the boot sector is damaged") || strstr(t.log, "as the active one"), m);
    }
    t_done(&t);

    /* the kernel command's look, which asks for no plan: the problems, and no word of a repair */
    t_init(&t, x, 0);
    (void)fat_check(&t.io, 0, &r1);
    mem_now = 0;
    snprintf(m, sizeof(m), "%s, FAT%d: a look as the kernel command makes it says nothing of a repair:\n%s", s->name, type, t.log);
    check(!strstr(t.log, "no repair") && !strstr(t.log, "would:") && !strstr(t.log, "can be repaired") && !strstr(t.log, "stay reported"), m);
    t_done(&t);

    /* the question: no answer, no write */
    t_init(&t, x, 1);
    t.answer = false;
    st = run(&t, FAT_CHECK_REPAIR, &r1);
    snprintf(m, sizeof(m), "%s, FAT%d: no answer to the question, nothing written", s->name, type);
    check(t.writes == 0 && !memcmp(x->mem, d, (size_t)x->bd.nblocks * 512), m);
    t_done(&t);

    /* the whole repair */
    st = repair(&t, x, &r1);
    writes = t.writes;
    if(fverbose) printf("      status %d, %u problems, %u left, %u writes, %u flushes\n", st, r1.found, r1.left, t.writes, t.flushes);
    snprintf(m, sizeof(m), "%s, FAT%d: every write as promised (%s)", s->name, type, bad_writes ? bad_what : "ok");
    check(!bad_writes, m);
    if(s->expect == EX_CLEAN || s->expect == EX_NOTE) {
        snprintf(m, sizeof(m), "%s, FAT%d: the repair leaves it clean (status %d, %u left):\n%s", s->name, type, st, r1.left, t.log);
        check(st == 0 && !r1.left, m);
    } else {
        snprintf(m, sizeof(m), "%s, FAT%d: problems stay reported (status %d, %u left):\n%s", s->name, type, st, r1.left, t.log);
        check(st == 1 && r1.left > 0, m);
    }
    if(s->file) {
        u8 *p = root_entry(x, &g, s->file);
        u32 c = efirst(&g, p), n = 0;
        while(c >= 2 && c < g.end && n < g.n) {
            u32 v = fget(x->mem, &g, g.fat1, c);
            n++;
            if(!v || v >= g.bad) break;
            c = v;
        }
        snprintf(m, sizeof(m), "%s, FAT%d: %.11s has %u clusters and %u bytes: it should have %d clusters", s->name, type, s->file, n, rd32(p + 28), s->clusters);
        check((int)n == s->clusters && rd32(p + 28) <= n * g.cb && (!n || rd32(p + 28) > (n - 1) * g.cb), m);
    }
    if(s->damage == d_lost || s->damage == d_lost_ring) {
        /* what no name reached has one now, every byte of it: lost_make() fills a cluster with one value */
        u32 want = s->damage == d_lost ? 4 : 3, i, got = 0, whole = 1;
        if(tree_read(x, &tf) != 0) die("a repaired image does not mount");
        for(i = 0; i < tf.count; i++) {
            const struct node *n = &tf.n[i];
            int k;
            if(strncmp(n->path, "/FOUND.000/FILE", 15)) continue;
            got += (u32)n->len / g.cb;
            for(k = 0; k < n->len; k++) if(n->data[k] != n->data[k - k % g.cb]) whole = 0;
        }
        snprintf(m, sizeof(m), "%s, FAT%d: the %u lost clusters are FOUND.000's files, byte for byte (%u)", s->name, type, want, got);
        check(got == want && whole, m);
        tree_free(&tf);
    }
    if(s->damage == d_found_taken) {
        snprintf(m, sizeof(m), "%s, FAT%d: it says the name it took:\n%s", s->name, type, t.log);
        check(strstr(t.log, "saved in FOUND.002") != NULL, m);
    }
    if(type != 12 && s->damage == d_dirty_dot) {
        snprintf(m, sizeof(m), "%s, FAT%d: a problem left: the volume stays marked as not clean", s->name, type);
        check(!(fget(x->mem, &g, g.fat1, 1) & (g.type == 16 ? 0x8000 : 0x08000000)), m);
    }
    if(s->expect == EX_BARRED) {
        snprintf(m, sizeof(m), "%s, FAT%d: no repair at all: nothing written", s->name, type);
        check(!t.writes && !memcmp(x->mem, d, (size_t)x->bd.nblocks * 512), m);
    }
    {
        int bad = kept(&y, x, &g, 0) || none_freed(d, x->mem, &g) || none_shorter(d, x->mem, &g);
        if(!bad && s->expect == EX_CLEAN) bad = none_lost(x, &g);
        snprintf(m, sizeof(m), "%s, FAT%d: nothing lost (%s)", s->name, type, bad ? why : "ok");
        check(!bad, m);
    }
    if(have_fsck && g.nfats == 2) {
        /* the other tool, on what was repaired, and on what was not */
        u8 *f = (u8 *)malloc((size_t)x->bd.nblocks * 512);
        int v;
        memcpy(f, x->mem, (size_t)x->bd.nblocks * 512);
        if(st == 0) {
            v = fsck_fat(x, "scenario");
            snprintf(m, sizeof(m), "%s, FAT%d: fsck.fat -n finds nothing on the repaired volume (status %d)", s->name, type, v);
            check(v == 0, m);
            if(v) fsck_said("scenario");
        }
        memcpy(x->mem, d, (size_t)x->bd.nblocks * 512);
        v = fsck_fat(x, "scenario");
        if(fverbose || (s->damage != d_none && s->expect != EX_NOTE && v == 0)) printf("    %s, FAT%d: fsck.fat -n finds nothing on this damaged volume\n", s->name, type);
        memcpy(x->mem, f, (size_t)x->bd.nblocks * 512);
        free(f);
    }

    /* it stays so: a look finds what the repair left, and a second repair writes nothing */
    {
        u8 *f = (u8 *)malloc((size_t)x->bd.nblocks * 512);
        memcpy(f, x->mem, (size_t)x->bd.nblocks * 512);
        st2 = look(x, f, &r2, s->name);
        snprintf(m, sizeof(m), "%s, FAT%d: a look after it agrees (status %d and %d)", s->name, type, st, st2);
        check(st2 == st, m);
        t_init(&t2, x, 1);
        (void)run(&t2, FAT_CHECK_REPAIR, &r2);
        snprintf(m, sizeof(m), "%s, FAT%d: a second repair finds nothing to do (%u writes):\n%s", s->name, type, t2.writes, t2.log);
        check(t2.writes == 0, m);
        t_done(&t2);
        free(f);
    }
    restore(x, d, &t);
    t_done(&t);

    /* a pull at every write: whole, torn every way on the small volumes (one at random on FAT32: each costs a 35 MB tree),
       and the stick's cache doing what it likes */
    for(n = 1; n <= writes; n++) {
        pull_one(x, d, &y, &g, s, n, writes, TEAR_NONE, FLUSH_TRUE, st);
        if(type == 32) pull_one(x, d, &y, &g, s, n, writes, 1 + (int)rn(TEARS), FLUSH_TRUE, st);
        else for(k = 1; k <= TEARS; k++) pull_one(x, d, &y, &g, s, n, writes, k, FLUSH_TRUE, st);
        pull_one(x, d, &y, &g, s, n, writes, TEAR_NONE, FLUSH_CACHE, st);
        pull_one(x, d, &y, &g, s, n, writes, TEAR_NONE, FLUSH_LIES, st);
    }
    snprintf(m, sizeof(m), "%s, FAT%d: every write of every pull as promised (%s)", s->name, type, bad_writes ? bad_what : "ok");
    check(!bad_writes, m);

    yard_free(&y);
    free(d);
}

/**** what a run does with what it may not have ****/

/* a read that fails, anywhere: no crash, no write, a status that says so */
static int st_many;

/* the n-th slot of the directory named by the root's entry `name` */
static u8 *r_dir_slots(struct ram *x, const struct geo3 *g, const char *name) { return root_entry(x, g, name); }

static u8 *dir_slot_n(struct ram *x, const struct geo3 *g, const u8 *entry, u32 n) {
    u32 c = efirst(g, entry), per = g->cb / 32;
    while(n >= per) { c = fget(x->mem, g, g->fat1, c); n -= per; }
    return x->mem + clba(g, c) * 512 + n * 32;
}

static void bad_reads(void) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u32 lba;
    geo3(x->mem, &g);
    d_size_none(x, &g);
    judge_prepare(x->mem, &g);
    for(lba = 0; lba < g.data_lba + 200; lba += lba < 8 ? 1 : 3) {
        u32 c = lba >= g.data_lba ? (lba - g.data_lba) / g.spc + 2 : 0;
        t_init(&t, x, 1);
        t.bad_read = lba + 1;
        (void)run(&t, FAT_CHECK_REPAIR, &r);
        if(t.writes && ((lba >= g.fat1 && lba < g.data_lba) || (c && (jm[c] & J_DIR)))) {
            char m[120];
            snprintf(m, sizeof(m), "block %u of the FAT, the root or a directory cannot be read: the run writes nothing", lba);
            check(0, m);
        }
        if(lba == 0) check(r.status == FAT_CHECK_TROUBLE, "a boot sector that cannot be read: trouble");
        restore(x, base16->mem, &t);
        d_size_none(x, &g);
        t_done(&t);
    }
    check(1, "a read that fails anywhere: no crash, and no repair from what could not be read");
}

/* the run is stopped at every request of a repair: what is done stays, a step is finished, nothing is lost */
static void stops(int type) {
    struct ram *x = image(type);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t, u;
    struct yard y;
    u8 *d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    u32 n, total;
    char m[300];
    geo3(x->mem, &g);
    bg = g;
    d_all(x, &g);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    memcpy(before, d, (size_t)x->bd.nblocks * 512);
    judge_prepare(d, &g);
    yard_read(x, &g, &y);
    (void)repair(&t, x, &r);
    total = t.reads + t.writes;
    restore(x, d, &t);
    t_done(&t);
    for(n = 1; n < total; n += 1 + total / 200) {
        t_init(&t, x, 1);
        t.stop_at = n;
        (void)run(&t, FAT_CHECK_REPAIR, &r);
        if(!r.stopped && r.status == FAT_CHECK_CLEAN) { restore(x, d, &t); t_done(&t); continue; }
        snprintf(m, sizeof(m), "FAT%d: stopped at request %u: it says so and ends", type, n);
        check(r.stopped && r.status == FAT_CHECK_PROBLEMS, m);
        {
            int bad = kept(&y, x, &g, K_DISK) || none_freed(d, x->mem, &g) || none_shorter(d, x->mem, &g);
            snprintf(m, sizeof(m), "FAT%d: stopped at request %u: nothing lost (%s)", type, n, bad ? why : "ok");
            check(!bad, m);
        }
        t_init(&u, x, 1);
        (void)run(&u, FAT_CHECK_REPAIR, &r);
        snprintf(m, sizeof(m), "FAT%d: stopped at request %u: a second run finishes it, and all is there (%s)", type, n, kept(&y, x, &g, 0) ? why : "ok");
        check(r.status == FAT_CHECK_CLEAN && !kept(&y, x, &g, 0), m);
        restore(x, d, &u);
        t_done(&u);
        restore(x, d, &t);
        t_done(&t);
    }
    yard_free(&y);
    free(d);
}

/* a stick that stops answering at any read of a repair: the run ends, and what it did so far is no worse than a pull */
static void dying(int type) {
    struct ram *x = image(type);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    struct yard y;
    u8 *d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    u32 n, total;
    char m[300];
    geo3(x->mem, &g);
    bg = g;
    d_all(x, &g);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    memcpy(before, d, (size_t)x->bd.nblocks * 512);
    judge_prepare(d, &g);
    yard_read(x, &g, &y);
    bad_writes = 0;
    (void)repair(&t, x, &r);
    total = t.reads;
    restore(x, d, &t);
    t_done(&t);
    for(n = 1; n < total; n += 1 + total / 150) {
        t_init(&t, x, 1);
        t.judged = true;
        t.dying_at = n;
        (void)run(&t, FAT_CHECK_REPAIR, &r);
        snprintf(m, sizeof(m), "FAT%d: reads stop at the %uth: it ends with trouble or problems, and nothing is lost (%s)", type, n, kept(&y, x, &g, K_DISK) ? why : "ok");
        check(r.status != FAT_CHECK_CLEAN && !kept(&y, x, &g, K_DISK) && !bad_writes, m);
        restore(x, d, &t);
        t_done(&t);
    }
    yard_free(&y);
    free(d);
}

/* What the look saw is not what a repair finds when it comes to write: nothing else writes while a device is held, so that
   is a device gone wrong, and the repair stops rather than write over what it did not look at */
static u32 under_at;

static int under_read(void *ctx, u32 lba, u32 count, void *buf) {
    struct tio *t = (struct tio *)ctx;
    int r = t_read(ctx, lba, count, buf);
    if(!r && t->writes && under_at / 512 >= lba && under_at / 512 < lba + count) ((u8 *)buf)[under_at - lba * 512] |= 0x40;
    return r;
}

static void changed_under(void) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u8 *p;
    u32 c;
    geo3(x->mem, &g);
    d_size_none(x, &g);
    p = root_entry(x, &g, "EMPTY   TXT");
    under_at = (u32)(p - x->mem) + 29;
    t_init(&t, x, 1);
    t.io.read = under_read;
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    check(r.status == FAT_CHECK_TROUBLE && strstr(t.log, "not what the look found") && rd32(p + 28) == 3000, "an entry that changed since the look: the repair stops, and leaves it");
    restore(x, base16->mem, &t);
    d_size_none(x, &g);
    t_done(&t);
    x = image(16);
    d_out(x, &g);
    c = step(x, &g, root_entry(x, &g, E_README), 2);
    under_at = g.fat1 * 512 + c * 2;
    t_init(&t, x, 1);
    t.io.read = under_read;
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    check(r.status == FAT_CHECK_TROUBLE && strstr(t.log, "not what the look found") && fget(x->mem, &g, g.fat1, c) == g.end + 5, "a FAT entry that changed since the look: the same");
    t_done(&t);

    /* the look after a repair finds what bars the rest: bytes past the root's end mark. The run ends there: no FOUND, no FSInfo */
    x = image(32);
    geo3(x->mem, &g);
    d_fsinfo(x, &g);
    d_size_none(x, &g);
    d_lost(x, &g);
    p = slot_free(x, &g, 0) + 40;
    under_at = (u32)(p - x->mem);
    t_init(&t, x, 1);
    t.io.read = under_read;
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    check(r.status == FAT_CHECK_PROBLEMS && r.sizes == 1 && !r.saved && strstr(t.log, "no more repairs"), "a bar that only a later look finds: the repairs stop there");
    check(!slot_of(x, &g, 0, "FOUND   000") && rd32(x->mem + g.fsi * 512 + 488) == 12345, "and nothing is written after it, FOUND and FSInfo included");
    t_done(&t);
}

/* the root full of names with sizes past their chains: more than one pass plans */
static u32 sizes_image(struct ram *x, const struct geo3 *g) {
    u32 n = 0;
    u8 *p = root_entry(x, g, "EMPTY   TXT"), *q;
    while((q = slot_free(x, g, 0)) != NULL) {
        memcpy(q, p, 32);
        snprintf((char *)q, 12, "S%07uTXT", n++);
        q[11] = 0x20;
        wr32(q + 28, 3000);
    }
    return n;
}

/* the name FOUND gets, read again just before it is written: the slot is taken. It stops, and writes over nothing */
static int single_read(void *ctx, u32 lba, u32 count, void *buf) {
    struct tio *t = (struct tio *)ctx;
    int r = t_read(ctx, lba, count, buf);
    if(!r && count == 1 && t->writes && under_at / 512 == lba) ((u8 *)buf)[under_at - lba * 512] = 'X';
    return r;
}

/* Bars after the first pass, with a plan still in hand: what is left of it stays undone. A slot taken behind FOUND's back */
static void changed_later(void) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u32 n;
    u8 *p;
    char m[200];
    geo3(x->mem, &g);
    n = sizes_image(x, &g);
    under_at = clba(&g, efirst(&g, root_entry(x, &g, E_DOCS))) * 512 + 26;
    t_init(&t, x, 1);
    t.io.read = under_read;
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    snprintf(m, sizeof(m), "%u sizes planned, a bar in the look after the first 256: %u made", n, r.sizes);
    check(n > 256 && r.sizes == 256 && strstr(t.log, "no more repairs"), m);
    t_done(&t);
    x = image(16);
    d_lost(x, &g);
    p = slot_free(x, &g, 0);
    under_at = (u32)(p - x->mem);
    t_init(&t, x, 1);
    t.io.read = single_read;
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    check(r.status == FAT_CHECK_TROUBLE && strstr(t.log, "not what the look found") && !p[0] && !r.saved, "FOUND's slot taken since the look: it stops, and the slot is as it was");
    t_done(&t);
}

/* Ctrl+C in the middle of a long plan: not a repair more than it takes to see it */
static void stop_soon(void) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u32 i, look, n;
    geo3(x->mem, &g);
    n = sizes_image(x, &g);
    t_init(&t, x, 0);
    (void)run(&t, 0, &r);
    look = t.reads;
    t_done(&t);
    t_init(&t, x, 1);
    t.stop_at = look + 40;
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    i = t.writes;
    check(n > 100 && r.stopped && r.sizes < 40 && i < 50, "stopped in a plan of hundreds of sizes: it ends after the one it is at");
    t_done(&t);
}

/* a device that refuses a write stops the run there, at any write */
static void refusals(int type) {
    struct ram *x = image(type);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u8 *d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    u32 n, writes;
    geo3(x->mem, &g);
    bg = g;
    d_all(x, &g);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    memcpy(before, d, (size_t)x->bd.nblocks * 512);
    judge_prepare(d, &g);
    (void)repair(&t, x, &r);
    writes = t.writes;
    restore(x, d, &t);
    t_done(&t);
    for(n = 1; n <= writes; n++) {
        char m[120];
        t_init(&t, x, 1);
        t.write_fail_at = n;
        (void)run(&t, FAT_CHECK_REPAIR, &r);
        snprintf(m, sizeof(m), "FAT%d: write %u refused: the run stops there, with trouble", type, n);
        check(r.status == FAT_CHECK_TROUBLE && t.writes == n, m);
        restore(x, d, &t);
        t_done(&t);
    }
    free(d);
}

/* every allocation of a run refused, one at a time: no crash, nothing kept, nothing written by a run that could not see */
static void no_memory(void) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u8 *d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    u32 n, calls;
    geo3(x->mem, &g);
    bg = g;
    d_all(x, &g);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    t_init(&t, x, 1);
    mem_fail_at = 0xFFFFFFFF;
    mem_calls = 0;
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    calls = mem_calls;
    restore(x, d, &t);
    t_done(&t);
    for(n = 1; n <= calls; n++) {
        t_init(&t, x, 1);
        mem_calls = 0;
        mem_fail_at = n;
        (void)run(&t, FAT_CHECK_REPAIR, &r);
        restore(x, d, &t);
        t_done(&t);
    }
    mem_fail_at = 0;
    check(calls > 8, "each of the run's allocations refused in turn: no crash, nothing kept");
    free(d);
}

/**** fuzz ****/

#define PATTERNS 6
static u32 fuzz_value(const struct geo3 *g) {
    switch(rn(PATTERNS)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return g->eoc;
    case 3: return g->bad;
    case 4: return g->end + rn(50);
    }
    return 2 + rn(g->n);
}

/* one blow at a part of the volume */
static void blow(struct ram *x, const struct geo3 *g) {
    u32 what = rn(100), i;
    u8 *p;
    if(what < 25) {                                     /* FAT entries, in both copies */
        fset(x, g, 2 + rn(g->n < 400 ? g->n : 400), fuzz_value(g));
    } else if(what < 35) {                              /* in one */
        fset1(x->mem, g, rn(2) ? g->fat1 : g->fat2, 2 + rn(g->n < 400 ? g->n : 400), fuzz_value(g));
    } else if(what < 45) {                              /* bytes of a FAT */
        p = x->mem + (g->fat1 + rn(2 * g->spf)) * 512;
        for(i = 1 + rn(8); i; i--) p[rn(512)] = (u8)rnd();
    } else if(what < 75) {                              /* a directory's first slots: the root's or a cluster's */
        u32 lba = g->root_cluster ? clba(g, g->root_cluster) : g->root_lba;
        if(rn(3) == 0) lba = clba(g, 2 + rn(60));
        p = x->mem + lba * 512 + rn(16) * 32;
        switch(rn(7)) {
        case 0: wr32(p + 28, rnd() * (rn(2) ? 1 : 251)); break;
        case 1: wr16(p + 26, 2 + rn(g->n + 40)); break;
        case 2: p[11] = (u8)rnd(); break;
        case 3: p[0] = (u8)(rn(2) ? 0xE5 : rnd()); break;
        case 4: p[rn(32)] = (u8)rnd(); break;
        case 5: memset(p, 0, 32); break;
        case 6: memcpy(p, p + 32 < x->mem + (size_t)(lba + 1) * 512 ? p + 32 : p, 32); break;
        }
    } else if(what < 85) {                              /* a sector, whole */
        u32 lba = rn(g->data_lba + 80);
        if(lba) {
            p = x->mem + lba * 512;
            if(rn(2)) memset(p, rn(2) ? 0 : 0xFF, 512);
            else for(i = 0; i < 512; i++) p[i] = (u8)rnd();
        }
    } else if(what < 95) {                              /* the boot sector's fields */
        static const u8 at[] = { 0, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 44, 45, 46, 47, 48, 50, 37, 65 };
        x->mem[at[rn(sizeof(at))]] = (u8)(rn(3) ? rnd() : 0);
    } else if(g->fsi) {                                 /* FSInfo */
        x->mem[g->fsi * 512 + (rn(2) ? 488 + rn(8) : rn(512))] = (u8)rnd();
    }
}

static char fsck_dir[64];
static u32 v_same, v_both[3], v_ours_only, v_theirs_only, v_ran;
static char v_first[200];

/* fsck.fat -n on the image: 0 clean, 1 it found something, 2 it gave up; -1: not run */
static int fsck_fat(struct ram *x, const char *tag) {
    char path[96], cmd[220];
    int fd, st;
    snprintf(path, sizeof(path), "%s/%s.img", fsck_dir, tag);
    fd = creat(path, 0644);
    if(fd < 0) return -1;
    if(write(fd, x->mem, (size_t)x->bd.nblocks * 512) != (long)x->bd.nblocks * 512) { close(fd); return -1; }
    close(fd);
    snprintf(cmd, sizeof(cmd), "fsck.fat -n '%s' > '%s.log' 2>&1", path, path);
    st = system(cmd);
    return st < 0 ? -1 : (st >> 8) & 0xFF;
}

/* what fsck.fat said of the last image, for someone reading why they disagree */
static void fsck_said(const char *tag) {
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "sed -e 's/^/      fsck.fat: /' '%s/%s.img.log' | grep -v 'fsck.fat 4\\|Checking\\|Reclaim\\|^ *$' | head -8", fsck_dir, tag);
    if(system(cmd)) printf("      (no log)\n");
}

/* What fsck.fat is shown. It reads on past a directory's end marker, where the driver stops and the FAT specification
   says nothing is: a stray byte there is a bad name to it and nothing to anyone else. So on its copy those slots are
   cleared, as far as a walk of the tree finds them, and what is left to compare is a difference about the volume */
static void zap_dir(u8 *mem, const struct geo3 *g, u32 first, int depth);
static u32 zap_budget;

static void zap_slots(u8 *mem, const struct geo3 *g, u8 *p, u32 slots, int *ended, int depth) {
    u32 i;
    for(i = 0; i < slots; i++, p += 32) {
        if(*ended || !p[0]) { *ended = 1; memset(p, 0, 32); continue; }
        if(p[0] == 0xE5 || (p[11] & 0x0F) == 0x0F || (p[11] & 8)) continue;
        if(p[0] == '.' && (p[1] == ' ' || p[1] == '.')) continue;
        if((p[11] & 0x10) && efirst(g, p) && zap_budget) { zap_budget--; zap_dir(mem, g, efirst(g, p), depth + 1); }
    }
}

static void zap_dir(u8 *mem, const struct geo3 *g, u32 first, int depth) {
    u32 c = first, n = 0;
    int ended = 0;
    if(depth > 9) return;
    if(!first) { zap_slots(mem, g, mem + g->root_lba * 512, g->root_secs * 16, &ended, depth); return; }
    while(c >= 2 && c < g->end && n++ < 4096) {
        u32 v = fget(mem, g, g->fat1, c);
        if(!v || v == g->bad) return;
        zap_slots(mem, g, mem + clba(g, c) * 512, g->cb / 32, &ended, depth);
        if(v > g->bad) return;
        c = v;
    }
}

static u32 fuzz_images, fuzz_repaired, fuzz_trouble;
static u32 said_ours, said_theirs;
static u8 *marks11, *marks12, *marks16, *marks32;

/* what the undamaged image's clusters are */
static u8 *marks_of(int type) {
    u8 **m = type == 11 ? &marks11 : type == 12 ? &marks12 : type == 16 ? &marks16 : &marks32;
    if(!*m) {
        struct geo3 g;
        struct rawl l;
        const struct ram *b = base_of(type);
        geo3(b->mem, &g);
        *m = (u8 *)calloc(g.end, 1);
        raw_marks = *m;
        raw_read(b->mem, &g, &l);
        raw_marks = NULL;
        free(l.e);
    }
    return *m;
}

/* A boot sector that no longer says what the volume is: what a run wrote, measured by what it said. No sector of a file
   has other bytes, on a cluster still in use. d: the image the run began on */
static int files_untouched(const struct tio *t, const struct ram *x, const u8 *d, const struct geo3 *g, const u8 *marks) {
    u32 i;
    for(i = 0; i < t->ntouched; i++) {
        u32 lba = t->touched[i], c;
        if(lba < g->data_lba) continue;
        c = (lba - g->data_lba) / g->spc + 2;
        if(c >= g->end || !(marks[c] & J_FILE) || !fget(d, g, g->fat1, c)) continue;
        if(memcmp(x->mem + lba * 512, d + lba * 512, 512)) {
            snprintf(why, sizeof(why), "block %u of a file written", lba);
            return -1;
        }
    }
    return 0;
}

static void fuzz_one(int type, u32 blows, int compare) {
    struct ram *x = image(type);
    struct geo3 g, gd;
    struct fat_check_result r, r2;
    struct tio t, u;
    struct yard y;
    u8 *d;
    char m[400];
    int same, st, bad;
    u32 i;
    geo3(x->mem, &g);
    for(i = 0; i < blows; i++) blow(x, &g);
    d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    fuzz_images++;

    /* a look: whatever it holds, no write */
    st = look(x, d, &r, "fuzz");
    if(st == FAT_CHECK_TROUBLE) fuzz_trouble++;
    if(compare && have_fsck) {
        int v;
        struct geo3 gv;
        geo3(d, &gv);
        if(gv.type == g.type && gv.spf == g.spf && gv.rsv == g.rsv && gv.nfats == g.nfats && gv.spc == g.spc && gv.data_lba == g.data_lba && gv.n == g.n) {
            zap_budget = 400;
            zap_dir(x->mem, &gv, gv.root_cluster, 0);
        }
        v = fsck_fat(x, "fuzz");
        memcpy(x->mem, d, (size_t)x->bd.nblocks * 512);
        v_ran++;
        if(v == 0 && st == 0) v_same++;
        else if(v > 0 && st > 0) v_both[st]++;
        else if(st > 0 && v == 0) {
            v_ours_only++;
            if(said_ours++ < 12 && getenv("FSCK_DIFFS")) {
                struct tio z;
                struct fat_check_result rz;
                t_init(&z, x, 0);
                (void)run(&z, 0, &rz);
                printf("    only this one, FAT%d:\n%s", type, z.log);
                t_done(&z);
            }
        } else if(st == 0 && v > 0) {
            v_theirs_only++;
            if(!v_first[0]) snprintf(v_first, sizeof(v_first), "seed state %u, FAT%d", rng_state, type);
            if(said_theirs++ < 12 && getenv("FSCK_DIFFS")) { printf("    only fsck.fat, FAT%d:\n", type); fsck_said("fuzz"); }
            /* the one thing it looks at and this does not (the account says why): anything else is a check this lacks */
            {
                char cmd[160];
                snprintf(cmd, sizeof(cmd), "grep -qi 'label' '%s/fuzz.img.log'", fsck_dir);
                check(system(cmd) == 0, "fsck.fat finds nothing but what this leaves alone: the boot sector's label");
            }
        }
    }

    /* a repair: whatever it holds, nothing in use goes free, and all the driver read, through either FAT, is still there */
    geo3(d, &gd);
    same = gd.type == g.type && gd.spf == g.spf && gd.rsv == g.rsv && gd.nfats == g.nfats && gd.spc == g.spc && gd.data_lba == g.data_lba && gd.n == g.n;
    memset(&y, 0, sizeof(y));
    if(same) {
        bg = g;
        memcpy(before, d, (size_t)x->bd.nblocks * 512);
        judge_prepare(d, &g);
        yard_read(x, &g, &y);
    }
    bad_writes = 0;
    t_init(&t, x, 1);
    t.judged = same != 0;
    t.limit = 2000000;
    st = run(&t, FAT_CHECK_REPAIR, &r);
    if(t.writes) fuzz_repaired++;
    if(same) {
        snprintf(m, sizeof(m), "fuzz FAT%d: every write as promised: %s", type, bad_writes ? bad_what : "ok");
        check(!bad_writes, m);
        bad = kept(&y, x, &g, 0) || none_freed(d, x->mem, &g) || none_shorter(d, x->mem, &g);
    } else bad = files_untouched(&t, x, d, &g, marks_of(type));
    snprintf(m, sizeof(m), "fuzz FAT%d: nothing lost (%s) (%u writes)", type, bad ? why : "ok", t.writes);
    check(!bad, m);
    if(bad) printf("%s\n", t.log);

    /* and it has come to rest: one more repair changes nothing, or it had a reason to stop */
    if(st == FAT_CHECK_CLEAN || (st == FAT_CHECK_PROBLEMS && !r.stopped)) {
        u8 *f = (u8 *)malloc((size_t)x->bd.nblocks * 512);
        memcpy(f, x->mem, (size_t)x->bd.nblocks * 512);
        t_init(&u, x, 1);
        u.limit = 2000000;
        (void)run(&u, FAT_CHECK_REPAIR, &r2);
        snprintf(m, sizeof(m), "fuzz FAT%d: status %d, then a second repair changes nothing (%u writes, %u then %u left):\n%s", type, st, u.writes, r.left, r2.left, u.log);
        check(!u.writes || r2.left < r.left, m);
        restore(x, f, &u);
        t_done(&u);
        free(f);
    }
    /* and pulled at a write taken at random, whole, torn, or the stick's cache doing what it likes: the same again */
    if(t.writes && same && rn(3) == 0) {
        struct tio p, q;
        u32 writes = t.writes;
        int tearing = rn(2) ? 1 + (int)rn(TEARS) : TEAR_NONE, how = tearing ? K_NAMES | K_DISK : K_DISK;
        restore(x, d, &t);
        t_init(&p, x, 1);
        p.judged = true;
        p.limit = 2000000;
        p.pull_at = 1 + rn(writes);
        p.tear = tearing;
        p.device = tearing ? FLUSH_TRUE : (int)rn(3);
        flush_lied = p.device == FLUSH_LIES;
        (void)run(&p, FAT_CHECK_REPAIR, &r);
        if(p.device) cache_loses(&p);
        bad = kept(&y, x, &g, how);
        if(!bad && !tearing) bad = none_freed(d, x->mem, &g) || none_shorter(d, x->mem, &g);
        snprintf(m, sizeof(m), "fuzz FAT%d: pulled at write %u of %u, tear %d, device %d: nothing lost (%s)", type, p.pull_at, writes, tearing, p.device, bad ? why : "ok");
        check(!bad, m);
        memcpy(before, x->mem, (size_t)x->bd.nblocks * 512);
        judge_prepare(before, &g);
        t_init(&q, x, 1);
        q.judged = true;
        q.limit = 2000000;
        st = run(&q, FAT_CHECK_REPAIR, &r);
        how = st ? K_DISK : 0;
        if(tearing) how |= K_NAMES;
        bad = kept(&y, x, &g, how);
        if(!bad && !tearing) bad = none_freed(d, x->mem, &g) || none_shorter(d, x->mem, &g);
        snprintf(m, sizeof(m), "fuzz FAT%d: pulled, then repaired (status %d): nothing lost (%s), every write as promised (%s):\n%s", type, st, bad ? why : "ok", bad_writes ? bad_what : "ok", bad ? q.log : "");
        check(!bad && !bad_writes, m);
        flush_lied = 0;
        t_done(&p);
        t_done(&q);
    }
    t_done(&t);
    if(same) yard_free(&y);
    free(d);
}

/* Every bit of the boot sector turned over, one at a time, on a volume with something to repair. What the run may do is
   measured by the boot sector as it was: whatever it took the volume to be, all of it is there afterwards */
static void boot_flips(int type) {
    struct ram *x;
    struct geo3 g;
    struct yard y;
    struct fat_check_result r;
    struct tio t;
    u32 at, bit, wrote = 0, runs = 0;
    u8 *d;
    char m[300];
    if(type == 32) {                                    /* clusters of one sector: one bit of the cluster size leaves it FAT32 */
        x = ram_new("b33", 140000);
        mkfs32(x);
        populate(x);
    } else x = image(type);
    d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    geo3(x->mem, &g);
    if(type == 32 && g.n / 2 < 65525) die("the wide FAT32 is not wide enough");
    d_size_none(x, &g);
    lost_make(x, &g, 2, 0, 0);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    yard_read(x, &g, &y);
    for(at = 3; at < (g.type == 32 ? 90u : 62u); at++) {
        for(bit = 0; bit < 8; bit++) {
            int bad;
            x->mem[at] ^= (u8)(1 << bit);
            t_init(&t, x, 1);
            t.limit = 2000000;
            (void)run(&t, FAT_CHECK_REPAIR, &r);
            x->mem[at] ^= (u8)(1 << bit);
            runs++;
            if(t.writes) {
                wrote++;
                bad = kept(&y, x, &g, 0);
                snprintf(m, sizeof(m), "FAT%d: bit %u of boot byte %u turned over, %u writes: nothing lost (%s)", type, bit, at, t.writes, bad ? why : "ok");
                check(!bad, m);
                restore(x, d, &t);
            }
            t_done(&t);
        }
    }
    printf("    FAT%d: %u bits of the boot sector turned over, %u of them harmless enough to repair past\n", type, runs, wrote);
    yard_free(&y);
    free(d);
}

/* Long name sets of every shape, where the numbering is what is at stake: whole sets, sets with a part too many of number
   0 (the review's 0x20, 0x80, 0xA0), a part out of its place, a wrong checksum, sets cut short by a short entry, by a deleted
   slot, by the end. Random bytes hardly ever make such a set. The sanitizers run this on the Mac (see the head comment) */
static void lfn_chars(u8 *q) {
    static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    u32 i;
    for(i = 0; i < 13; i++) {
        if(i < 12 && rn(16) == 0) {                     /* a pair, as a name outside the first plane has them */
            wr16(q + pos[i++], 0xD83D);
            wr16(q + pos[i], 0xDE00 + rn(64));
        } else wr16(q + pos[i], rn(8) ? 'a' + rn(26) : rnd() & 0xFFFF);
    }
}

static void lfn_shape(u8 *q, u32 slots) {
    u32 k = 0;
    memset(q, 0, slots * 32);
    while(k + 2 < slots) {
        u32 parts = rn(4) ? 1 + rn(4) : 1 + rn(22), j, i;
        char name[12];
        u8 sum = 0;
        snprintf(name, sizeof(name), "S%07uTXT", rn(9999999));
        for(i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + (u8)name[i]);
        if(rn(8) == 0) sum ^= (u8)rnd();
        for(j = parts; j && k + 2 < slots; j--, k++) {
            u8 *p = q + k * 32;
            lfn_chars(p);
            p[0] = (u8)(j | (j == parts ? 0x40 : 0));
            p[11] = 0x0F;
            p[12] = rn(16) ? 0 : (u8)rnd();
            p[13] = sum;
            if(rn(12) == 0) p[0] = (u8)(rn(4) ? 1 + rn(31) : rnd());
        }
        if(rn(3) == 0 && k + 2 < slots) {               /* a part after the last: 0x20, 0x80 and 0xA0 are number 0, and so are others */
            u8 *p = q + k * 32;
            static const u8 odd[] = { 0x20, 0x80, 0xA0, 0xC0, 0xE0, 0x60, 0x40, 0x41 };
            lfn_chars(p);
            p[0] = odd[rn(sizeof(odd))];
            p[11] = 0x0F;
            p[13] = sum;
            k++;
        }
        if(rn(10) == 0 && k + 1 < slots) q[k++ * 32] = 0xE5;
        if(rn(10)) {
            u8 *p = q + k * 32;
            memcpy(p, name, 11);
            p[11] = rn(16) ? 0x20 : (u8)rnd();
            p[12] = (u8)(rn(4) ? 0 : rnd());
            k++;
        }
    }
    if(rn(4) == 0) {                                    /* one byte of it, anywhere */
        u8 *p = q + rn(k ? k : 1) * 32;
        p[rn(32)] = (u8)rnd();
    }
}

static void lfn_fuzz(u32 seed, u32 cases) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u32 i, slots = 44, n;
    u8 *q, keep[44 * 32];
    rng_state = seed;
    geo3(x->mem, &g);
    q = slot_free(x, &g, 0);
    if(!q || q + slots * 32 > x->mem + g.data_lba * 512) die("no room for the long names");
    memcpy(keep, q, sizeof(keep));
    for(i = 0, n = 0; i < cases; i++) {
        lfn_shape(q, slots);
        t_init(&t, x, 0);
        (void)run(&t, (i & 1) ? FAT_CHECK_VERBOSE : 0, &r);
        if(t.writes || r.wrote) check(0, "a look at long names wrote");
        n += r.found != 0;
        t_done(&t);
        memcpy(q, keep, sizeof(keep));
    }
    printf("FSCK long names: %u shapes, %u of them with something said\n", cases, n);
    check(n > 0, "long name sets of every shape: no crash, nothing written");
}

/* A FAT that does not read the same twice: a stick dying, or one that lies. Nothing a look learned can be taken for a cluster
   later: no bitmap is indexed but by what was just checked to be a cluster */
static int changing_read(void *ctx, u32 lba, u32 count, void *buf) {
    struct tio *t = (struct tio *)ctx;
    int r = t_read(ctx, lba, count, buf);
    if(!r && t->reads > t->stop_at && lba >= bg.fat1 && lba < bg.root_lba) {
        u32 i;
        if(t->wild) memset(buf, 0xAB, count * 512);
        else for(i = 0; i < 200; i++) ((u8 *)buf)[rn(count * 512)] = (u8)(rn(4) ? rnd() : 0xFF);
    }
    return r;
}

/* (K2) a chain over more windows of the FAT than are kept, past its size, with a name joining something: what the look reads
   of the FAT is read again before it ends, and the second reading is not the first */
static void d_windows(struct ram *r, const struct geo3 *g) {
    u8 *q = slot_free(r, g, 0);
    u32 i, prev = 0, first = 0;
    for(i = 0; i < 5; i++) {
        u32 c = 1000 + i * 16384;                       /* (a window holds 16384 entries of FAT32, and four are kept) */
        if(c >= g->end || fget(r->mem, g, g->fat1, c)) die("no room for a chain over the windows");
        fset(r, g, c, g->eoc);
        if(prev) fset(r, g, prev, c);
        else first = c;
        prev = c;
    }
    memset(q, 0, 32);
    memcpy(q, "WINDOWS BIN", 11);
    q[11] = 0x20;
    eset(g, q, first);
    wr32(q + 28, 3 * g->cb);
    d_twin(r, g);
}

static void changing(int type, void (*damage)(struct ram *, const struct geo3 *), u32 cases) {
    struct ram *x = image(type);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u8 *d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    u32 i;
    geo3(x->mem, &g);
    bg = g;
    damage(x, &g);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    for(i = 0; i < cases; i++) {
        t_init(&t, x, 1);
        t.io.read = changing_read;
        t.io.stop = NULL;
        t.stop_at = i / 2;                              /* (here: the reads that are still true) */
        t.wild = i & 1;                                 /* all of it, or a little */
        t.limit = 400000;
        (void)run(&t, FAT_CHECK_REPAIR, &r);
        restore(x, d, &t);
        t_done(&t);
    }
    check(1, "a FAT that reads differently each time: no crash, and the run ends");
    free(d);
}

/* thirds, or (few_fat32: `make unittest`) one FAT32 image in nine: each costs a copy of 35 MB, several times over */
static void fuzz(u32 seed, u32 images, int few_fat32) {
    u32 i;
    rng_state = seed;
    for(i = 0; i < images; i++) {
        int type = i % 7 == 6 ? 11 : i % 3 == 0 ? 12 : i % 3 == 1 ? 16 : few_fat32 && i % 9 != 2 ? 16 : 32;
        fuzz_one(type, 1 + rn(i % 5 == 0 ? 40 : 6), i % (type == 32 ? 3 : 4) == 0);
    }
    printf("FSCK fuzz: %u images, %u with something to repair; %u could not be checked at all\n", fuzz_images, fuzz_repaired, fuzz_trouble);
    if(!have_fsck) printf("    (fsck.fat is not here: its verdicts are not compared)\n");
    else {
        printf("    fsck.fat -n on %u of them: %u both clean, %u both found problems, %u this one only, %u fsck.fat only%s%s\n", v_ran, v_same,
               v_both[1] + v_both[2], v_ours_only, v_theirs_only, v_first[0] ? ", first at " : "", v_first);
    }
}

/**** what one sentence each proves ****/

/* more than a pass plans: repairs past FIX_MAX, lost chains past LOST_MAX: later passes take them, all of them. A file of
   2000 clusters keeps what is lost from being most of what is in use, which is not repaired */
static void many(void) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    struct superblock *sb;
    struct vnode *dir;
    u32 c, n = 0, i, cb = rd16(x->mem + 11) * x->mem[13];
    char name[16], m[200];
    u8 *p;
    static u8 piece[8192];
    sb = mount(x);
    if(sb->root->ops->create(sb->root, "BIG.BIN", VN_FILE, &dir)) die("many: create");
    for(i = 0; i < 2000; i++) if(dir->ops->write(dir, i * cb, piece, cb) != (int)cb) die("many: write");
    vput(dir);
    dir = mkdir_or_die(sb->root, "many");
    for(i = 0; i < 400; i++) {
        snprintf(name, sizeof(name), "F%04u", i);
        put_or_die(dir, name, 300, i);
    }
    vput(dir);
    if(fsync_sb(sb)) die("many: sync");
    vput(sb->root);
    sb->ops->unmount(sb);
    binvalidate(&x->bd);
    free(sb);
    geo3(x->mem, &g);
    bg = g;
    p = r_dir_slots(x, &g, "MANY       ");
    for(i = 0; i < 400; i++) {                          /* every one of 400 sizes past its chain */
        u8 *q = dir_slot_n(x, &g, p, i + 2);
        wr32(q + 28, rd32(q + 28) + 3 * g.cb);
    }
    for(c = g.end - 1; c >= 2 && n < 1500; c--) {       /* and 1500 lost chains of one cluster */
        if(fget(x->mem, &g, g.fat1, c)) continue;
        memset(x->mem + clba(&g, c) * 512, 0x5A, g.cb);
        fset(x, &g, c, g.eoc);
        n++;
    }
    memcpy(before, x->mem, (size_t)x->bd.nblocks * 512);
    judge_prepare(before, &g);
    bad_writes = 0;
    t_init(&t, x, 1);
    t.judged = true;
    st_many = run(&t, FAT_CHECK_REPAIR, &r);
    snprintf(m, sizeof(m), "400 sizes and 1500 lost chains, in as many passes as they take: status %d, %u sizes, %u saved, %u left (%s)",
             st_many, r.sizes, r.saved, r.left, bad_writes ? bad_what : "every write as promised");
    check(st_many == 0 && r.sizes == 400 && r.saved == 1500 && !bad_writes, m);
    check(slot_of(x, &g, 0, "FOUND   000") && slot_of(x, &g, 0, "FOUND   001"), "they went into FOUND.000 and FOUND.001");
    t_done(&t);
}

/* a tree deeper than the walk's first stack, and a directory of more names than its first table; a name twice at the end */
static void deep_and_wide(int type) {
    struct ram *x = image(type);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    struct superblock *sb;
    struct vnode *v, *w, *dir;
    char name[32], m[200];
    u32 i;
    u8 *p, *q;
    sb = mount(x);
    v = sb->root;
    v->refs++;
    for(i = 0; i < 80; i++) {
        snprintf(name, sizeof(name), "level%u", i);
        w = mkdir_or_die(v, name);
        vput(v);
        v = w;
    }
    put_or_die(v, "bottom", 1000, 9);
    vput(v);
    dir = mkdir_or_die(sb->root, "wide");
    for(i = 0; i < 700; i++) {
        snprintf(name, sizeof(name), "W%05u.TXT", i);
        put_or_die(dir, name, 0, 0);
    }
    vput(dir);
    if(fsync_sb(sb)) die("deep: sync");
    vput(sb->root);
    sb->ops->unmount(sb);
    binvalidate(&x->bd);
    free(sb);
    geo3(x->mem, &g);
    t_init(&t, x, 0);
    (void)run(&t, 0, &r);
    snprintf(m, sizeof(m), "FAT%d: 80 levels of directories and 700 names in one: clean (status %d, %u directories, %u files):\n%s", type, r.status, r.dirs, r.files, t.log);
    check(r.status == 0 && r.dirs >= 83 && r.files == 711, m);
    t_done(&t);
    {
        /* each allocation refused, those of the table of names among them: it goes on without comparing names */
        u32 calls, n;
        t_init(&t, x, 0);
        mem_fail_at = 0xFFFFFFFF;
        mem_calls = 0;
        (void)run(&t, 0, &r);
        calls = mem_calls;
        t_done(&t);
        for(n = 1; n <= calls; n++) {
            t_init(&t, x, 0);
            mem_calls = 0;
            mem_fail_at = n;
            (void)run(&t, FAT_CHECK_VERBOSE, &r);
            t_done(&t);
        }
        mem_fail_at = 0;
    }
    p = r_dir_slots(x, &g, "WIDE       ");
    q = dir_slot_n(x, &g, p, 700);
    memcpy(dir_slot_n(x, &g, p, 701), q, 11);
    t_init(&t, x, 0);
    (void)run(&t, 0, &r);
    snprintf(m, sizeof(m), "FAT%d: the 700th name twice is found, past the table's first size", type);
    check(r.status == 1 && r.found == 1 && strstr(t.log, "a second entry with this short name"), m);
    t_done(&t);
}

/* a directory past what the driver reads: the walk is not whole, and nothing is repaired */
static void long_dir(void) {
    struct ram *x = image(32);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u8 *p;
    u32 add = 0, prev, need, i, n = 0;
    geo3(x->mem, &g);
    p = root_entry(x, &g, E_DOCS);
    prev = efirst(&g, p);
    for(i = 0; i < g.cb / 32; i++) {                    /* (no end marker before the clusters it gets) */
        u8 *q = x->mem + clba(&g, prev) * 512 + i * 32;
        if(!q[0]) q[0] = 0xE5;
    }
    need = (65536 + 64) / (g.cb / 32) + 1;
    for(i = 0; i < need; i++) {
        u32 k;
        add = free_cluster(x, &g, 0);
        for(k = 0; k < g.cb / 32; k++, n++) {
            u8 *q = x->mem + clba(&g, add) * 512 + k * 32;
            memset(q, 0, 32);
            snprintf((char *)q, 12, "L%07u   ", n);
            q[11] = 0x20;
        }
        fset(x, &g, prev, add);
        fset(x, &g, add, g.eoc);
        prev = add;
    }
    d_size_none(x, &g);
    t_init(&t, x, 1);
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    check(r.status == 1 && !t.writes && strstr(t.log, "more than 65536 slots") && strstr(t.log, "no repair"),
          "a directory past 65536 slots: said, and nothing is repaired");
    t_done(&t);
}

/* a volume on a device shorter than it says, at every size down to nothing: a stick's image cut short, a partition that
   shrank. Nothing is read past the device (t_read() stops the test), nothing is written, and it says so */
static void truncated(int type) {
    struct ram *x = image(type);
    struct fat_check_result r;
    struct tio t;
    u32 n, blocks = rd16(x->mem + 19) ? rd16(x->mem + 19) : rd32(x->mem + 32);
    char m[160];
    for(n = blocks - 1; n; n = n > 64 ? n - n / 5 - 1 : n - 1) {
        t_init(&t, x, 1);
        t.io.blocks = n;
        (void)run(&t, FAT_CHECK_REPAIR, &r);
        snprintf(m, sizeof(m), "FAT%d on a device of %u blocks of its %u: not clean, nothing written", type, n, blocks);
        check(r.status != FAT_CHECK_CLEAN && !t.writes, m);
        t_done(&t);
    }
    t_init(&t, x, 0);
    t.io.blocks = 0;
    (void)run(&t, 0, &r);
    check(r.status == FAT_CHECK_TROUBLE, "a device of no blocks: trouble");
    t_done(&t);
}

/* (K3) a thousand names on one cluster: the lines that say who shares it stop at the number problems stop at, with -v too */
static void flood(void) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u8 *p, *q;
    u32 n = 0;
    char m[200];
    geo3(x->mem, &g);
    p = root_entry(x, &g, E_OTHER);
    while((q = slot_free(x, &g, 0)) != NULL) {
        memcpy(q, p, 32);
        snprintf((char *)q, 12, "T%07uDAT", n++);
        q[11] = 0x20;
    }
    t_init(&t, x, 0);
    (void)fat_check(&t.io, FAT_CHECK_VERBOSE, &r);
    mem_now = 0;
    snprintf(m, sizeof(m), "%u names on one cluster: %u lines say who shares it, at most 40", n + 1, t.holds);
    check(n > 100 && t.holds > 0 && t.holds <= 40, m);
    t_done(&t);
}

/* (K4) a tree deeper than the walk goes, by hand: the cap is reached, the walk is not whole, nothing is repaired, and the
   memory of a run does not grow with what the volume asks for */
static long deep_run(u32 levels, int file, struct tio *t, struct fat_check_result *r) {
    struct ram *x = image(16);
    struct geo3 g;
    u32 i, parent = 0;
    geo3(x->mem, &g);
    for(i = 0; i < levels; i++) {
        u32 c = free_cluster(x, &g, 12);
        u8 *q = slot_free(x, &g, parent), *e = x->mem + clba(&g, c) * 512;
        memset(q, 0, 32);
        snprintf((char *)q, 12, "D%07u   ", i);
        q[11] = 0x10;
        eset(&g, q, c);
        memset(e, 0, g.cb);
        memcpy(e, ".          ", 11);
        e[11] = 0x10;
        eset(&g, e, c);
        memcpy(e + 32, "..         ", 11);
        e[32 + 11] = 0x10;
        eset(&g, e + 32, parent);
        fset(x, &g, c, g.eoc);
        parent = c;
    }
    if(file) raw_file(x, &g, parent, "BOTTOM  BIN", 2, 16);
    d_size_none(x, &g);
    t_init(t, x, 1);
    mem_peak = 0;
    (void)run(t, FAT_CHECK_REPAIR, r);
    return mem_peak;
}

static void too_deep(void) {
    struct tio t;
    struct fat_check_result r;
    long shallow, deep;
    char m[200];
    shallow = deep_run(40, 0, &t, &r);
    check(r.status == FAT_CHECK_CLEAN, "40 levels by hand, and a size to repair: it is repaired");
    t_done(&t);
    (void)deep_run(127, 1, &t, &r);
    check(r.status == FAT_CHECK_CLEAN, "as deep as the walk goes, a file at the bottom: walked, and repaired");
    t_done(&t);
    (void)deep_run(128, 1, &t, &r);
    check(!t.writes && strstr(t.log, "no repair") && strstr(t.log, "levels of directories"), "one level more, with only a file under it to show for it: no repair");
    t_done(&t);
    deep = deep_run(1200, 0, &t, &r);
    snprintf(m, sizeof(m), "1200 levels: nothing written, it says so, and %ld bytes of memory where 40 levels took %ld", deep, shallow);
    check(!t.writes && r.status == FAT_CHECK_PROBLEMS && strstr(t.log, "levels of directories") && strstr(t.log, "no repair") && deep <= shallow, m);
    t_done(&t);
}

/* what -v adds, and that each thing is said in words */
static void words(void) {
    struct ram *x = image(32);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    geo3(x->mem, &g);
    d_all(x, &g);
    t_init(&t, x, 0);
    (void)fat_check(&t.io, FAT_CHECK_VERBOSE, &r);
    mem_now = 0;
    check(strstr(t.log, "FAT32:") && strstr(t.log, "directories,") && strstr(t.log, "the two FATs differ") &&
          strstr(t.log, "not unmounted cleanly") && !strstr(t.log, "fsck -r would:"), "-v says the counts, and without FAT_CHECK_PLAN no plan");
    t_done(&t);
    t_init(&t, x, 0);
    (void)fat_check(&t.io, FAT_CHECK_VERBOSE | FAT_CHECK_PLAN, &r);
    mem_now = 0;
    check(strstr(t.log, "copy the first FAT over the second, end 1 chain, set 1 size, save 3 lost chains in a new FOUND directory, set FSInfo, mark it clean") != NULL,
          "the plan says exactly what will change");
    t_done(&t);
    t_init(&t, x, 1);
    t.answer = false;
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    check(t.asks == 1 && !strncmp(t.plan, "copy the first FAT over the second", 34) && strstr(t.plan, "mark it clean"), "the question is given the plan, whole");
    t_done(&t);
    memset(x->mem, 0, 1024);
    t_init(&t, x, 0);
    check(run(&t, 0, &r) == FAT_CHECK_PROBLEMS && strstr(t.log, "block 6"), "a boot sector gone, its backup there: problems, and it says where");
    t_done(&t);
    memset(x->mem + 6 * 512, 0, 512);
    t_init(&t, x, 0);
    check(run(&t, 0, &r) == FAT_CHECK_TROUBLE && strstr(t.log, "no FAT file system"), "nothing there: trouble, in words");
    t_done(&t);
}

/* the messages: a name off a stick cannot drive a terminal */
static void names_said(void) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    u8 *p;
    u32 i;
    geo3(x->mem, &g);
    p = root_entry(x, &g, E_SMALL);
    p[0] = 0x1B; p[1] = '['; p[2] = '2'; p[3] = 'J';
    wr32(p + 28, rd32(p + 28) + 3 * g.cb);
    t_init(&t, x, 0);
    (void)run(&t, 0, &r);
    for(i = 0; i < t.logn; i++) if(t.log[i] == 0x1B) break;
    check(i == t.logn && strstr(t.log, "?[2J") != NULL, "an escape in a name comes out as '?'");
    t_done(&t);
}

/* One image from a file, by hand: what a look says, what the repair does, and the yardstick over it. The file stays as it is */
static void image_file(const char *path) {
    struct ram *x;
    struct geo3 g;
    struct yard y;
    struct fat_check_result r;
    struct tio t;
    char m[400];
    int fd = open(path, 0), bad;
    long size, at = 0;
    u8 *d;
    if(fd < 0) die("the image cannot be opened");
    size = lseek(fd, 0, 2);
    lseek(fd, 0, 0);
    x = ram_new("file", (u32)(size / 512));
    while(at < size) {
        long k = read(fd, x->mem + at, (unsigned long)(size - at));
        if(k <= 0) die("the image cannot be read");
        at += k;
    }
    close(fd);
    file_most = 1 << 28;
    fverbose = 1;
    geo3(x->mem, &g);
    bg = g;
    d = (u8 *)malloc((size_t)size);
    before = (u8 *)realloc(before, (size_t)size);
    memcpy(d, x->mem, (size_t)size);
    memcpy(before, d, (size_t)size);
    judge_prepare(d, &g);
    yard_read(x, &g, &y);
    printf("  %s: a look\n", path);
    (void)look(x, d, &r, "the image");
    printf("  a repair\n");
    (void)repair(&t, x, &r);
    bad = kept(&y, x, &g, 0) || none_freed(d, x->mem, &g) || none_shorter(d, x->mem, &g);
    snprintf(m, sizeof(m), "%s: status %d, %u writes: every write as promised (%s), nothing lost (%s)", path, r.status, t.writes, bad_writes ? bad_what : "ok", bad ? why : "ok");
    printf("  %s\n", m);
    check(!bad && !bad_writes, m);
    t_done(&t);
    yard_free(&y);
    free(d);
}

/* What the 118 GB stick costs. mkfs's own layout for it, FAT read from a function: the device stores nothing */
static u8 *big_root;
static struct geo3 big;
static u32 big_max, big_req;

static int big_read(void *ctx, u32 lba, u32 count, void *buf) {
    u32 i;
    (void)ctx;
    big_req++;
    if(count > big_max) big_max = count;
    memset(buf, 0, count * 512);
    for(i = 0; i < count; i++) {
        u32 at = lba + i;
        u8 *o = (u8 *)buf + i * 512;
        if(!at) memcpy(o, big_root, 512);
        else if(at == 1) memcpy(o, big_root + 512, 512);
        else if(at == 6) memcpy(o, big_root, 512);
        else if((at >= big.fat1 && at < big.fat2) || (at >= big.fat2 && at < big.fat2 + big.spf)) {
            u32 s = (at - big.rsv) % big.spf, k;
            /* a chain of half the volume, the rest free; the first entries by hand */
            for(k = 0; k < 128; k++) {
                u32 c = s * 128 + k, v;
                if(c >= big.end) break;
                v = c == 0 ? 0x0FFFFFF8 : c == 1 ? 0x0FFFFFFF : c == 2 ? 0x0FFFFFFF : c < big.end / 2 - 1 ? (c == 3 ? 4 : c + 1) : c == big.end / 2 - 1 ? 0x0FFFFFFF : 0;
                wr32(o + k * 4, v);
            }
        } else if(at == clba(&big, 2)) {
            memcpy(o, "BIG     BIN", 11);
            o[11] = 0x20;
            wr16(o + 26, 3);
            wr32(o + 28, 0xFFFFFFFF);
        }
    }
    return 0;
}

static void big_stick(void) {
    static struct bdev d;
    struct fat_layout l;
    struct fat_check_io io;
    struct fat_check_result r;
    struct tio log;
    u8 *bs;
    char msg[128];
    char m[200];
    t_init(&log, work16, 0);
    d.nblocks = 247463936;                              /* 118 GB */
    d.block_size = 512;
    if(fat_mkfs_plan(&d, "BIG", 32, &l, msg, sizeof(msg))) die(msg);
    big_root = (u8 *)calloc(2, 512);
    bs = big_root;
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    wr16(bs + 11, 512);
    bs[13] = (u8)l.spc;
    wr16(bs + 14, l.rsv);
    bs[16] = 2;
    bs[21] = 0xF8;
    wr32(bs + 32, l.total);
    wr32(bs + 36, l.spf);
    wr32(bs + 44, 2);
    wr16(bs + 48, 1);
    wr16(bs + 50, 6);
    bs[510] = 0x55; bs[511] = 0xAA;
    geo3(bs, &big);
    wr32(bs + 512, 0x41615252);
    wr32(bs + 512 + 484, 0x61417272);
    wr32(bs + 512 + 488, l.clusters - (big.end / 2 - 2));
    wr32(bs + 512 + 492, 2);
    wr32(bs + 512 + 508, 0xAA550000);
    memset(&io, 0, sizeof(io));
    io.ctx = &log;
    io.blocks = l.total;
    io.read = big_read;
    io.say = t_say;
    mem_peak = 0;
    fat_check(&io, 0, &r);
    snprintf(m, sizeof(m), "118 GB (%u clusters): read in pieces of at most %u blocks, %ld KB of memory, %u requests; status %d, %u problems",
             big.n, big_max, mem_peak / 1024, big_req, r.status, r.found);
    printf("    %s\n", m);
    check(big_max <= 128 && mem_peak < 4 * 1024 * 1024 && r.status == FAT_CHECK_PROBLEMS, m);
    check(r.found == 1, "and it finds the one thing wrong with it: a chain past its size");
    mem_now = 0;
    free(big_root);
    t_done(&log);
}

static int want(const char *name) { return !only || strstr(name, only) != NULL; }

int main(int argc, char **argv) {
    u32 i;
    int types[4] = { 11, 12, 16, 32 };
    int k;
    fverbose = getenv("FSCK_VERBOSE") != NULL;
    only = getenv("FSCK_ONLY");
    fatfs_init();
    make_images();
    before = (u8 *)malloc((size_t)base32->bd.nblocks * 512);
    if(getenv("TMPDIR") && strlen(getenv("TMPDIR")) < 40) snprintf(fsck_dir, sizeof(fsck_dir), "%s/surfos-fsck-XXXXXX", getenv("TMPDIR"));
    else snprintf(fsck_dir, sizeof(fsck_dir), "/tmp/surfos-fsck-XXXXXX");
    have_fsck = system("command -v fsck.fat > /dev/null 2>&1") == 0 && mkdtemp(fsck_dir) != NULL;
    if(argc > 2 && !strcmp(argv[1], "--image")) image_file(argv[2]);
    else if(argc > 3 && !strcmp(argv[1], "--lfn")) lfn_fuzz((u32)atoi(argv[2]), (u32)atoi(argv[3]));
    else if(argc > 1) {
        fuzz((u32)atoi(argv[1]), argc > 2 ? (u32)atoi(argv[2]) : 200, 0);
    } else {
        for(k = 0; k < 4; k++) {
            for(i = 0; i < sizeof(scens) / sizeof(scens[0]); i++) {
                int on = scens[i].on;
                if((on == ON_FAT32 && types[k] != 32) || (on == ON_FAT12 && types[k] != 12) || (on == ON_TWO && types[k] == 11) ||
                   (on == ON_NO32 && types[k] == 32) || (on == ON_ONE && types[k] != 11)) continue;
                if(only && !strstr(scens[i].name, only)) continue;
                scenario(types[k], &scens[i]);
            }
        }
        if(want("bad-reads")) bad_reads();
        if(want("names-said")) names_said();
        if(want("words")) words();
        if(want("many")) many();
        if(want("long-dir")) long_dir();
        if(want("flood")) flood();
        if(want("too-deep")) too_deep();
        if(want("truncated")) for(k = 1; k < 4; k++) truncated(types[k]);
        if(want("deep-and-wide")) { deep_and_wide(16); deep_and_wide(32); }
        if(want("no-memory")) no_memory();
        if(want("stops")) for(k = 1; k < 4; k++) { stops(types[k]); refusals(types[k]); }
        if(want("dying")) for(k = 1; k < 4; k++) dying(types[k]);
        if(want("changed-under")) changed_under();
        if(want("changed-under")) changed_later();
        if(want("stop-soon")) stop_soon();
        if(want("boot-flips")) for(k = 1; k < 4; k++) boot_flips(types[k]);
        if(want("changing")) {
            for(k = 1; k < 4; k++) {
                changing(types[k], d_all, 40);
                changing(types[k], d_twin_sizes, 40);
            }
            changing(32, d_windows, 120);
        }
        if(want("big-stick")) big_stick();
        if(want("long-names")) lfn_fuzz(20261101, 4000);
        if(want("fuzz")) fuzz(20261101, 200, 1);
    }
    if(have_fsck) {
        char cmd[120];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", fsck_dir);
        if(system(cmd)) printf("    (the temporary directory stays: %s)\n", fsck_dir);
    }
    printf("FSCK: %d checks, %d scenarios, %d pulls, %d failures\n", checks, nscen_ran, npulls, failures);
    return failures ? 1 : 0;
}
