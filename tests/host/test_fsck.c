/* Host test of fs/fat_check.c, the kernel command fsck's checker (11/01/26, the command-line suite). It runs over images
   made and read by the real driver: fs/fat.c, fs/bcache.c and fs/fat_mkfs.c through test_fatfs.c's harness, so that what
   says a repair worked is not the code that made it. What the brief asks to be proved, and where:

     a read-only run never writes       look(): no write function at all, the image compared byte for byte, in every
                                        scenario and on every damaged image of the fuzz
     a repair never deletes data        oracle(): every file the driver read before reads the same after, up to what its
                                        chain held; no cluster in use becomes free; every write is judged as it is made
                                        (write_judged()): no boot sector, no in-use data cluster, no entry but for its
                                        first cluster and size, or a free slot filled
     a pull at any write is no worse    pulls(): the repair run again and pulled after each of its writes, the volume then
                                        judged by the driver and by a further repair, which must end where the whole
                                        repair ended; and with the device's write cache losing what was not flushed
     no damaged volume panics           fuzz(): random damage to every part of the volume, with allocations failing, reads
                                        failing, and a bound on the device requests of a run

   `test_fsck <seed> <images>` runs more of the fuzz by hand. FSCK_VERBOSE=1 says everything; FSCK_ONLY=word runs the
   scenarios whose name holds it. fsck.fat -n judges the same images where it is installed (the devenv has it); the Mac has
   not, and says so. FSCK_DIFFS=1 prints what each side said where they disagree, and a verdict of fsck.fat's alone fails
   this test unless it is about the one thing left out on purpose, the boot sector's label. The sanitizers run on the Mac
   only: `make unittest` never builds this with them, see the account. */
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

struct tio {
    struct fat_check_io io;
    struct ram *r;
    u32 writes, reads, read_blocks, max_read, flushes;
    u32 pull_at;                        /* gone after this many writes (0: never) */
    u32 bad_read;                       /* this block + 1 cannot be read (0: none) */
    u32 write_fail_at;                  /* this write is refused */
    u32 stop_at;                        /* stop() says so from this device request on (0: never) */
    u32 limit;                          /* a run over this many requests is a run that does not end */
    bool gone, judged, cached, answer;
    u32 asks;
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

/* one block as it goes to the disk, against the image the repair began on. What fat_check.c's head comment promises */
static void write_judged(u32 lba, const u8 *now, u32 count) {
    u32 i, k;
    for(k = 0; k < count; k++, lba++, now += 512) {
        const u8 *was = before + lba * 512;
        if(!lba) { bad_write("the boot sector written", lba); continue; }
        if(bg.bk && (lba == bg.bk)) { bad_write("the backup boot sector written", lba); continue; }
        if(lba >= bg.fat1 && lba < bg.fat1 + bg.spf) {
            u32 s = lba - bg.fat1, per = bg.type == 12 ? 0 : bg.type == 16 ? 256 : 128;
            if(per) {
                for(i = 0; i < per; i++) {
                    u32 c = s * per + i;
                    u32 a = bg.type == 16 ? rd16(was + i * 2) : rd32(was + i * 4) & 0x0FFFFFFF;
                    u32 b = bg.type == 16 ? rd16(now + i * 2) : rd32(now + i * 4) & 0x0FFFFFFF;
                    if(c >= 2 && a && !b) bad_write("a cluster in use set free", lba);
                }
            }
            continue;
        }
        if(lba >= bg.fat2 && lba < bg.fat2 + bg.spf && bg.nfats == 2) continue;
        if(bg.fsi && lba == bg.fsi) {
            for(i = 0; i < 512; i++)
                if(was[i] != now[i] && !(i < 4 || (i >= 484 && i < 496) || i >= 508)) bad_write("FSInfo written outside its fields", lba);
            continue;
        }
        if(lba < bg.root_lba) { bad_write("a reserved sector written", lba); continue; }
        if(lba >= bg.data_lba) {
            u32 c = (lba - bg.data_lba) / bg.spc + 2;
            if(c < bg.end && !fget(before, &bg, bg.fat1, c)) continue;  /* a cluster that was free: anything may go there */
        }
        for(i = 0; i < 512; i += 32) {                  /* a directory's block: one entry's first cluster and size, or a free slot */
            const u8 *a = was + i, *b = now + i;
            u32 j;
            if(!memcmp(a, b, 32)) continue;
            if(a[0] == 0 || a[0] == 0xE5) {
                if(memcmp(b, "FOUND   ", 8) || b[11] != ATTR_DIR_T) bad_write("a free slot filled with something else", lba);
                continue;
            }
            for(j = 0; j < 32; j++)
                if(a[j] != b[j] && !(j == 20 || j == 21 || j >= 26)) bad_write("an entry changed beyond its cluster and size", lba);
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

static int t_write(void *ctx, u32 lba, u32 count, const void *buf) {
    struct tio *t = (struct tio *)ctx;
    u32 i;
    if(t->gone) return BDEV_EGONE;
    if(t->limit && t->reads + t->writes > t->limit) die("a run that does not end: too many device requests");
    if(lba + count > t->r->bd.nblocks) die("a write past the device");
    t->writes++;
    if(t->write_fail_at == t->writes) { t->gone = true; return BDEV_EIO; }
    if(t->judged) write_judged(lba, (const u8 *)buf, count);
    for(i = 0; i < count; i++) {
        touch(t, lba + i);
        if(t->cached) {
            t->pend = (struct pending *)realloc(t->pend, (t->npend + 1) * sizeof(struct pending));
            t->pend[t->npend].lba = lba + i;
            memcpy(t->pend[t->npend++].old, t->r->mem + (lba + i) * 512, 512);
        }
    }
    memcpy(t->r->mem + lba * 512, buf, count * 512);
    if(t->pull_at && t->writes >= t->pull_at) t->gone = true;
    return 0;
}

static int t_flush(void *ctx) {
    struct tio *t = (struct tio *)ctx;
    if(t->gone) return BDEV_EGONE;
    t->flushes++;
    t->npend = 0;
    return 0;
}

static bool t_stop(void *ctx) {
    struct tio *t = (struct tio *)ctx;
    return t->stop_at && t->reads + t->writes >= t->stop_at;
}

static void t_say(void *ctx, const char *line) {
    struct tio *t = (struct tio *)ctx;
    u32 n = strlen(line);
    if(t->logn + n + 2 < sizeof(t->log)) {
        memcpy(t->log + t->logn, line, n);
        t->logn += n;
        t->log[t->logn++] = '\n';
        t->log[t->logn] = 0;
    }
    if(fverbose) printf("      | %s\n", line);
}

static bool t_ask(void *ctx) {
    struct tio *t = (struct tio *)ctx;
    t->asks++;
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

/* the device's own cache lost some of what was never flushed: each sector's last write kept or not at random, as a
   stick loses them, and what was not kept goes back to what it was before the first write that is lost */
static void cache_loses(struct tio *t) {
    u32 i, j;
    for(i = t->npend; i--; ) {
        bool later = false;
        if(rn(2)) { for(j = i + 1; j < t->npend; j++) if(t->pend[j].lba == t->pend[i].lba) t->pend[j].lba = 0xFFFFFFFF; continue; }
        for(j = i + 1; j < t->npend && !later; j++) if(t->pend[j].lba == t->pend[i].lba) later = true;
        if(!later) memcpy(t->r->mem + t->pend[i].lba * 512, t->pend[i].old, 512);
    }
    t->npend = 0;
}

static int run(struct tio *t, int flags, struct fat_check_result *res) {
    int r = fat_check(&t->io, flags | (fverbose ? FAT_CHECK_VERBOSE : 0), res);
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
#define FILE_MOST (1 << 20)

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
            u32 want = v->size < FILE_MOST ? v->size : FILE_MOST;
            n->data = (u8 *)malloc(want + 1);
            n->len = want ? v->ops->read(v, 0, n->data, want) : 0;
            if(n->len < 0) n->len = 0;
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

/* Every name of `was` is in `now`, each file with the bytes it had, at least as far as `least` (the repaired volume)
   holds them, nothing other. least may be NULL: then only what `now` holds is compared. 0, or -1 and why */
static int same_files(const struct tree *was, const struct tree *now, const struct tree *least) {
    u32 i;
    for(i = 0; i < was->count; i++) {
        const struct node *a = &was->n[i], *b = tree_find(now, a->path), *l = least ? tree_find(least, a->path) : NULL;
        if(tree_find(was, a->path) != a) continue;      /* (a name twice: lookups find the first, on any volume) */
        if(!b) { snprintf(why, sizeof(why), "%s is gone", a->path); return -1; }
        if(a->dir != b->dir) { snprintf(why, sizeof(why), "%s changed kind", a->path); return -1; }
        if(a->dir) continue;
        if(b->len > a->len) { snprintf(why, sizeof(why), "%s grew: %d bytes, it read %d", a->path, b->len, a->len); return -1; }
        if(l && b->len < l->len) { snprintf(why, sizeof(why), "%s reads %d bytes, the repair kept %d", a->path, b->len, l->len); return -1; }
        if(memcmp(a->data, b->data, b->len)) { snprintf(why, sizeof(why), "%s has other bytes", a->path); return -1; }
    }
    return 0;
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
static int chains_whole(const struct tree *now_tree, struct ram *r, const u8 *was, const struct geo3 *g, u32 first, int depth);

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
        if((p[11] & 0x10) && efirst(g, p) && chains_whole(NULL, r, was, g, efirst(g, p), depth + 1)) return -1;
    }
    return 0;
}

static int chains_whole(const struct tree *t, struct ram *r, const u8 *was, const struct geo3 *g, u32 first, int depth) {
    u32 c = first, n = 0;
    (void)t;
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

/* What says a repair lost nothing must not be the repair's own idea of a file, and same_files() alone would pass a
   repair that emptied them all: it compares what is there. So this reads the image by hand, from the entries and the
   first FAT: for every entry that names a file, how many bytes its chain really holds, as far as the first bad link, a
   cluster it has been to, or its size. A repair may only keep or raise that number, for every entry there is */
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
    while(c >= 2 && c < g->end) {
        u32 v = fget(mem, g, g->fat1, c);
        if(!v) l->freelinks++;
        if(!v || v == g->bad || seen[c] == seen_gen) break;
        seen[c] = seen_gen;
        if(held[c] == held_gen) l->shared++;
        held[c] = held_gen;
        n++;
        if(v > g->bad) break;
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
    while(c >= 2 && c < g->end && n++ < 4096 && start) {
        u32 v = fget(mem, g, g->fat1, c);
        if(!v) l->freelinks++;
        if(!v || v == g->bad) return;
        if(held[c] == held_gen) l->shared++;
        held[c] = held_gen;
        raw_slots(mem, g, mem + clba(g, c) * 512, g->cb / 32, key, depth, l, &start);
        if(v > g->bad) return;
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

/* every entry of `was` is on `now`, holding as much (entries of one name: as many, in their order) */
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
    if(!bad && b.freelinks > a.freelinks) { snprintf(why, sizeof(why), "%u chains reach a free cluster, %u did", b.freelinks, a.freelinks); bad = -1; }
    free(a.e);
    free(b.e);
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

static void put_or_die(struct vnode *dir, const char *name, u32 len, u32 seed) {
    static u8 big[1 << 16];
    u32 i;
    for(i = 0; i < len; i++) big[i] = (u8)(seed * 37 + i * 11 + (i >> 9));
    if(put_file(dir, name, big, len)) die("a file of the test tree");
}

static struct vnode *mkdir_or_die(struct vnode *dir, const char *name) {
    struct vnode *v;
    if(dir->ops->create(dir, name, VN_DIR, &v)) die("a directory of the test tree");
    return v;
}

/* the tree every scenario starts from, cb the volume's cluster size: files of a few clusters, a fragmented pair *
   long names, three levels of directories, a file with no clusters, room to spare */
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
        memset(piece, (int)(0x40 + i), cb);
        if(a->ops->write(a, i * cb, piece, cb) != (int)cb) die("frag1");
        memset(piece, (int)(0x80 + i), cb);
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

static void d_fat2(struct ram *r, const struct geo3 *g) {
    fset1(r->mem, g, g->fat2, free_cluster(r, g, 3), g->eoc);
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

/* SMALL's chain runs into OTHER's, past its own two clusters: a join, its size past what it keeps */
static void d_join(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL), *o = root_entry(r, g, E_OTHER);
    fset(r, g, step(r, g, p, 1), step(r, g, o, 2));
    wr32(p + 28, 5 * g->cb);
}

/* the same, the join past SMALL's size: only a cut is needed */
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

/* a lost chain that runs into a file's, and one that ends in a link to nothing */
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

/* a directory's chain that stops at a bad link: it grows into a second cluster first */
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
    d_twin_far(r, g);
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

/* a chain that needs a cut at a FAT12 entry across two sectors: one write cannot make that, and a half-written entry
   links anywhere. It stays reported, and nothing is allocated while the link could reach what would be taken */
static void d_straddle(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    u32 c = 341;                                        /* (341 + 170 = 511: its entry starts in a sector's last byte) */
    if(fget(r->mem, g, g->fat1, c)) die("cluster 341 of the FAT12 image is in use");
    fset(r, g, step(r, g, p, 1), c);
    fset(r, g, c, g->end + 7);
    wr32(p + 28, 3 * g->cb);
    lost_make(r, g, 2, 0, 3);
}

/* the same with a link into a free cluster: FOUND's clusters would be free clusters, and one could be this one, so
   nothing is allocated while the link stays */
static void d_straddle_free(struct ram *r, const struct geo3 *g) {
    u8 *p = root_entry(r, g, E_SMALL);
    u32 c = 341, i, f = 0;
    for(i = 2; i < g->end && !f; i++) if(i != c && !fget(r->mem, g, g->fat1, i)) f = i;     /* the first a repair would take */
    fset(r, g, step(r, g, p, 1), c);
    fset(r, g, c, f);
    wr32(p + 28, 3 * g->cb);
    lost_make(r, g, 2, 0, 3);
}

/* a directory's next cluster, full of names, marked free in the FAT: the driver reads it, and a cut would take them away */
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

/* a link in the first FAT alone that runs one file into another: the second FAT does not share it, so it decides nothing */
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

/* bytes past the end marker: one entry more would show them, so that slot is not free for FOUND */
static void d_root_junk(struct ram *r, const struct geo3 *g) {
    u8 *p = slot_free(r, g, 0);
    lost_make(r, g, 2, 0, 0);
    memset(p + 32, 0x41, 32);
    fsi_set(r, g);
}

/* the name FOUND.000 taken: by a file, and FOUND.001 by a long name with another alias */
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

/* one FAT active, the second not a mirror: nothing here repairs that */
static void d_mirror(struct ram *r, const struct geo3 *g) {
    if(g->type != 32) return;
    r->mem[40] = 0x80;
    d_size_none(r, g);
}

/* only the backup differs: harmless, and said */
static void d_backup(struct ram *r, const struct geo3 *g) {
    if(g->bk) r->mem[g->bk * 512 + 3] ^= 1;
}

static void d_boot_flag(struct ram *r, const struct geo3 *g) {
    r->mem[(g->type == 32 ? 64 : 36) + 1] |= 1;
    d_size_none(r, g);
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

/* file, clusters: what the repair must leave: that root entry with exactly so many clusters, and its size at most that
   (it says where the chain is cut, which "nothing lost" alone does not) */
struct scen { const char *name; void (*damage)(struct ram *, const struct geo3 *); int expect; int on; const char *file; int clusters; };

static const struct scen scens[] = {
    { "none", d_none, EX_CLEAN, 0 },
    { "dirty", d_dirty, EX_CLEAN, 0 },
    { "fsinfo", d_fsinfo, EX_CLEAN, ON_FAT32 },
    { "fsinfo-signature", d_fsinfo_sig, EX_CLEAN, ON_FAT32 },
    { "fat2", d_fat2, EX_CLEAN, ON_TWO },
    { "out", d_out, EX_CLEAN, 0, E_README, 3 },
    { "free-link", d_free_link, EX_CLEAN, 0, E_README, 3 },
    { "loop", d_loop, EX_CLEAN, 0, E_README, 5 },
    { "bad-cluster", d_bad_cluster, EX_CLEAN, 0, E_README, 2 },
    { "size-past", d_size_past, EX_CLEAN, 0, E_SMALL, 2 },
    { "size-none", d_size_none, EX_CLEAN, 0, "EMPTY   TXT", 0 },
    { "long", d_long, EX_CLEAN, 0, E_OTHER, 2 },
    { "long-zero", d_long_zero, EX_CLEAN, 0, E_SMALL, 0 },
    { "first-out", d_first_out, EX_CLEAN, 0, E_SMALL, 0 },
    { "first-free", d_first_free, EX_CLEAN, 0, E_SMALL, 0 },
    { "twin", d_twin, EX_CLEAN, 0, "TWIN    TXT", 6 },
    { "twin-far", d_twin_far, EX_CLEAN, 0, E_OTHER, 6 },
    { "twin-sizes", d_twin_sizes, EX_CLEAN, 0, "TWIN    DAT", 6 },
    { "join", d_join, EX_CLEAN, 0, E_SMALL, 5 },
    { "join-past", d_join_past, EX_CLEAN, 0, E_SMALL, 2 },
    { "lost", d_lost, EX_CLEAN, 0 },
    { "lost-ring", d_lost_ring, EX_CLEAN, 0 },
    { "lost-into", d_lost_into, EX_CLEAN, 0 },
    { "lost-fork", d_lost_fork, EX_CLEAN, 0 },
    { "dir-link", d_dir_link, EX_CLEAN, 0 },
    { "all", d_all, EX_CLEAN, ON_TWO },
    { "dot", d_dot, EX_LEFT, 0 },
    { "lfn-sum", d_lfn_sum, EX_LEFT, 0 },
    { "name", d_name, EX_LEFT, 0 },
    { "dup", d_dup, EX_LEFT, 0 },
    { "dir-twin", d_dir_twin, EX_LEFT, 0 },
    { "dir-size", d_dir_size, EX_LEFT, 0 },
    { "dir-first", d_dir_first, EX_BARRED, 0 },
    { "dir-free-link", d_dir_free_link, EX_BARRED, 0 },
    { "dir-bit", d_dir_bit, EX_BARRED, 0 },
    { "dir-fake", d_dir_fake, EX_BARRED, 0 },
    { "fat1-join", d_fat1_join, EX_BARRED, ON_TWO },
    { "file-bit", d_file_bit, EX_BARRED, 0 },
    { "fat1-zero", d_fat1_zero, EX_BARRED, ON_TWO },
    { "straddle", d_straddle, EX_LEFT, ON_FAT12 },
    { "straddle-free", d_straddle_free, EX_LEFT, ON_FAT12 },
    { "dirty-dot", d_dirty_dot, EX_LEFT, ON_TWO },
    { "full-twin", d_full_twin, EX_LEFT, 0 },
    { "full-lost", d_full_lost, EX_LEFT, 0 },
    { "root-full", d_root_full, EX_LEFT, 0 },
    { "root-junk", d_root_junk, EX_LEFT, 0 },
    { "found-taken", d_found_taken, EX_CLEAN, 0 },
    { "mirror", d_mirror, EX_BARRED, ON_FAT32 },
    { "backup", d_backup, EX_NOTE, ON_FAT32 },
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

/* A pull after write n of the repair, and with `cached` the device's cache losing some of what was not flushed. The
   volume is then what it was or better: the driver reads every file, nothing in use is free, no entry reaches a free
   cluster; and a further repair ends where the whole repair ended, losing nothing */
static void pull_one(struct ram *x, const u8 *d, const struct tree *td, const struct tree *tf, const struct geo3 *g,
                     const struct scen *s, u32 n, u32 writes, int cached, int final) {
    struct tio t, u;
    struct fat_check_result res;
    struct tree tp;
    char m[300];
    t_init(&t, x, 1);
    t.judged = true;
    t.pull_at = n;
    t.cached = cached != 0;
    (void)run(&t, FAT_CHECK_REPAIR, &res);
    if(cached) cache_loses(&t);
    if(!cached && g->type != 12 && writes > 1 && n < writes) {
        u32 b1 = fget(x->mem, g, g->fat1, 1);
        snprintf(m, sizeof(m), "%s, FAT%d: pulled after write %u of %u: the volume still says it was not unmounted cleanly", s->name, g->type, n, writes);
        check(!(b1 & (g->type == 16 ? 0x8000 : 0x08000000)), m);
    }
    if(tree_read(x, &tp) != 0) {
        snprintf(m, sizeof(m), "%s, FAT%d: pulled after write %u%s: the driver does not mount it", s->name, g->type, n, cached ? " (cache)" : "");
        check(0, m);
    } else {
        int bad = same_files(td, &tp, tf) || none_freed(d, x->mem, g) || chains_whole(NULL, x, d, g, g->root_cluster, 0) ||
                  none_shorter(d, x->mem, g);
        snprintf(m, sizeof(m), "%s, FAT%d: pulled after write %u%s: every file as it was (%s)", s->name, g->type, n,
                 cached ? " (cache)" : "", bad ? why : "ok");
        check(!bad, m);
        tree_free(&tp);
    }
    {
        /* the owner plugs it in again, and repairs it. What its writes are judged against is the pulled volume: only
           the sectors the pull's run wrote differ from the damaged one, and a 35 MB copy at each pull would be most of
           the test */
        struct tree tr;
        struct fat_check_result r2;
        u32 i;
        int st;
        for(i = 0; i < t.ntouched; i++) memcpy(before + t.touched[i] * 512, x->mem + t.touched[i] * 512, 512);
        st = repair(&u, x, &r2);
        snprintf(m, sizeof(m), "%s, FAT%d: pulled after write %u%s: a second repair ends as the first did (status %d, %u left): %s", s->name,
                 g->type, n, cached ? " (cache)" : "", st, r2.left, u.log);
        check(st == final, m);
        if(tree_read(x, &tr) == 0) {
            int bad = same_files(td, &tr, tf) || none_freed(d, x->mem, g) || none_shorter(d, x->mem, g);
            snprintf(m, sizeof(m), "%s, FAT%d: pulled after write %u%s: repaired again, every file as the first repair left it (%s)", s->name,
                     g->type, n, cached ? " (cache)" : "", bad ? why : "ok");
            check(!bad, m);
            tree_free(&tr);
        } else check(0, "a second repair leaves a volume the driver mounts");
        for(i = 0; i < t.ntouched; i++) memcpy(before + t.touched[i] * 512, d + t.touched[i] * 512, 512);
        restore(x, d, &u);
        t_done(&u);
    }
    restore(x, d, &t);
    t_done(&t);
    npulls++;
}

static void scenario(int type, const struct scen *s) {
    struct ram *x = image(type);
    struct geo3 g;
    struct tree td, tf;
    struct fat_check_result r0, r1, r2;
    struct tio t, t2;
    u8 *d;
    char m[300];
    int st, st2;
    u32 n, writes;

    if(type <= 12 && (s->damage == d_dirty || s->damage == d_dirty_dot)) return;   /* (FAT12 has no mark to damage) */
    d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    geo3(x->mem, &g);
    bg = g;
    if(fverbose) printf("  %s, FAT%d\n", s->name, type);
    s->damage(x, &g);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    memcpy(before, d, (size_t)x->bd.nblocks * 512);
    bad_writes = 0;
    nscen_ran++;

    /* a look */
    st = look(x, d, &r0, s->name);
    {
        int clean = s->damage == d_none || s->expect == EX_NOTE;
        snprintf(m, sizeof(m), "%s, FAT%d: a look says %s (status %d, %u problems)", s->name, type, clean ? "clean" : "problems", st, r0.found);
        check(clean ? st == 0 && !r0.found : st == 1 && r0.found > 0, m);
    }
    if(tree_read(x, &td) != 0) die("a damaged test image does not mount");

    /* a run not asked to repair, with a write function in reach: it never gets to it */
    t_init(&t, x, 1);
    st = run(&t, 0, &r1);
    snprintf(m, sizeof(m), "%s, FAT%d: not asked to repair, nothing written, nothing asked", s->name, type);
    check(t.writes == 0 && t.asks == 0 && !memcmp(x->mem, d, (size_t)x->bd.nblocks * 512), m);
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
    if(tree_read(x, &tf) != 0) die("a repaired image does not mount");
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
        for(i = 0; i < tf.count; i++) {
            const struct node *n = &tf.n[i];
            int k;
            if(strncmp(n->path, "/FOUND.000/FILE", 15)) continue;
            got += (u32)n->len / g.cb;
            for(k = 0; k < n->len; k++) if(n->data[k] != n->data[k - k % g.cb]) whole = 0;
        }
        snprintf(m, sizeof(m), "%s, FAT%d: the %u lost clusters are FOUND.000's files, byte for byte (%u)", s->name, type, want, got);
        check(got == want && whole, m);
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
        int bad = same_files(&td, &tf, NULL) || none_freed(d, x->mem, &g) || none_shorter(d, x->mem, &g);
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

    /* a pull at every write */
    for(n = 1; n <= writes; n++) {
        pull_one(x, d, &td, &tf, &g, s, n, writes, 0, st);
        if(rn(3) == 0) pull_one(x, d, &td, &tf, &g, s, n, writes, 1, st);
    }
    snprintf(m, sizeof(m), "%s, FAT%d: every write of every pull as promised (%s)", s->name, type, bad_writes ? bad_what : "ok");
    check(!bad_writes, m);

    tree_free(&td);
    tree_free(&tf);
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
    for(lba = 0; lba < g.data_lba + 200; lba += lba < 8 ? 1 : 3) {
        t_init(&t, x, 1);
        t.bad_read = lba + 1;
        (void)run(&t, FAT_CHECK_REPAIR, &r);
        if(lba && lba < g.data_lba && t.writes && lba >= g.fat1 && lba < g.root_lba) check(0, "a run that could not read its FAT wrote");
        if(lba == 0) check(r.status == FAT_CHECK_TROUBLE, "a boot sector that cannot be read: trouble");
        restore(x, base16->mem, &t);
        d_size_none(x, &g);
        t_done(&t);
    }
    check(1, "a read that fails anywhere: no crash");
}

/* the run is stopped at every request of a repair: what is done stays, a step is finished, nothing is lost */
static void stops(int type) {
    struct ram *x = image(type);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t, u;
    struct tree td, tp;
    u8 *d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    u32 n, total;
    char m[300];
    geo3(x->mem, &g);
    bg = g;
    d_all(x, &g);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    memcpy(before, d, (size_t)x->bd.nblocks * 512);
    if(tree_read(x, &td) != 0) die("stops: mount");
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
        if(tree_read(x, &tp) == 0) {
            int bad = same_files(&td, &tp, NULL) || none_freed(d, x->mem, &g) || none_shorter(d, x->mem, &g);
            snprintf(m, sizeof(m), "FAT%d: stopped at request %u: nothing lost (%s)", type, n, bad ? why : "ok");
            check(!bad, m);
            tree_free(&tp);
        } else check(0, "a stopped repair leaves a volume the driver mounts");
        (void)repair(&u, x, &r);
        snprintf(m, sizeof(m), "FAT%d: stopped at request %u: a second run finishes it", type, n);
        check(r.status == FAT_CHECK_CLEAN, m);
        restore(x, d, &u);
        t_done(&u);
        restore(x, d, &t);
        t_done(&t);
    }
    tree_free(&td);
    free(d);
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

static void fuzz_one(int type, u32 blows, int compare) {
    struct ram *x = image(type);
    struct geo3 g, gd;
    struct fat_check_result r, r2;
    struct tio t, u;
    struct tree td, tf;
    u8 *d;
    char m[300];
    int mounted, st;
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

    /* a repair: whatever it holds, nothing in use goes free, and every file the driver read reads the same */
    geo3(d, &gd);
    bg = gd;
    if(gd.type != g.type || gd.spf != g.spf || gd.rsv != g.rsv || gd.nfats != g.nfats || gd.spc != g.spc || gd.data_lba != g.data_lba ||
       gd.n != g.n) {
        /* the boot sector no longer describes this volume: the run is watched for crashes, its writes are not judged */
        memset(&bg, 0, sizeof(bg));
    }
    mounted = tree_read(x, &td) == 0;
    memcpy(before, d, (size_t)x->bd.nblocks * 512);
    bad_writes = 0;
    t_init(&t, x, 1);
    t.judged = bg.spc != 0;
    t.limit = 2000000;
    st = run(&t, FAT_CHECK_REPAIR, &r);
    if(t.writes) fuzz_repaired++;
    snprintf(m, sizeof(m), "fuzz FAT%d: every write as promised: %s", type, bad_writes ? bad_what : "ok");
    check(!bad_writes, m);
    if(mounted) {
        if(tree_read(x, &tf) == 0) {
            int bad = same_files(&td, &tf, NULL) || (bg.spc && (none_freed(d, x->mem, &bg) || none_shorter(d, x->mem, &bg)));
            snprintf(m, sizeof(m), "fuzz FAT%d: nothing lost (%s) (%u writes)", type, bad ? why : "ok", t.writes);
            check(!bad, m);
            if(bad) printf("%s\n", t.log);
            tree_free(&tf);
        } else {
            snprintf(m, sizeof(m), "fuzz FAT%d: a volume the driver mounted before it mounts after", type);
            check(0, m);
        }
        tree_free(&td);
    }
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
    /* and pulled at a write taken at random, the device's cache losing a part: the same again */
    if(t.writes && bg.spc && rn(3) == 0) {
        struct tio p, q;
        u32 writes = t.writes;
        restore(x, d, &t);
        t_init(&p, x, 1);
        p.judged = true;
        p.limit = 2000000;
        p.pull_at = 1 + rn(writes);
        p.cached = rn(2);
        (void)run(&p, FAT_CHECK_REPAIR, &r);
        if(p.cached) cache_loses(&p);
        snprintf(m, sizeof(m), "fuzz FAT%d: pulled after write %u of %u: nothing lost (%s)", type, p.pull_at, writes,
                 none_freed(d, x->mem, &bg) || none_shorter(d, x->mem, &bg) ? why : "ok");
        check(!none_freed(d, x->mem, &bg) && !none_shorter(d, x->mem, &bg), m);
        memcpy(before, x->mem, (size_t)x->bd.nblocks * 512);
        t_init(&q, x, 1);
        q.limit = 2000000;
        (void)run(&q, FAT_CHECK_REPAIR, &r);
        snprintf(m, sizeof(m), "fuzz FAT%d: pulled, then repaired: nothing lost (%s)", type,
                 none_freed(d, x->mem, &bg) || none_shorter(d, x->mem, &bg) ? why : "ok");
        check(!none_freed(d, x->mem, &bg) && !none_shorter(d, x->mem, &bg), m);
        t_done(&p);
        t_done(&q);
    }
    t_done(&t);
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

/* more than a pass plans: repairs past FIX_MAX, lost chains past LOST_MAX: later passes take them, all of them */
static void many(void) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    struct superblock *sb;
    struct vnode *dir;
    u32 c, n = 0, i;
    char name[16], m[200];
    u8 *p;
    sb = mount(x);
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

/* a copy reads what it copies, not the FAT over and over: the second of two names on a long chain, its FAT reads counted.
   One 64 KB window a cluster would be more blocks of FAT than the clusters copied have, many times over */
static void copy_cost(void) {
    struct ram *x = image(32);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    struct superblock *sb;
    u8 *p, *q;
    char m[200];
    static u8 piece[8192];
    struct vnode *v;
    u32 i, n = 3000;
    sb = mount(x);
    if(sb->root->ops->create(sb->root, "LONG.BIN", VN_FILE, &v)) die("copy_cost: create");
    for(i = 0; i < n; i++) if(v->ops->write(v, i * 512, piece, 512) != 512) die("copy_cost: write");
    vput(v);
    if(fsync_sb(sb)) die("copy_cost: sync");
    vput(sb->root);
    sb->ops->unmount(sb);
    binvalidate(&x->bd);
    free(sb);
    geo3(x->mem, &g);
    bg = g;
    p = root_entry(x, &g, "LONG    BIN");
    q = slot_free(x, &g, 0);
    memcpy(q, p, 32);
    memcpy(q, "TWIN    BIN", 11);
    t_init(&t, x, 1);
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    snprintf(m, sizeof(m), "a copy of %u clusters reads the FAT in windows, not at each cluster: %u blocks read, %u requests (status %d)", n, t.read_blocks, t.reads, r.status);
    check(r.status == 0 && r.copies == 1 && t.read_blocks < 12 * n + 60000, m);
    if(fverbose) printf("    %s\n", m);
    t_done(&t);
}

/* a device that takes a write and keeps something else: the copy is never linked in */
static int lying_read(void *ctx, u32 lba, u32 count, void *buf) {
    struct tio *t = (struct tio *)ctx;
    int r = t_read(ctx, lba, count, buf);
    if(!r && t->writes && lba >= bg.data_lba && count == bg.spc) ((u8 *)buf)[7] ^= 0x10;
    return r;
}

static void lying(void) {
    struct ram *x = image(16);
    struct geo3 g;
    struct fat_check_result r;
    struct tio t;
    struct tree td, tf;
    u8 *d = (u8 *)malloc((size_t)x->bd.nblocks * 512);
    geo3(x->mem, &g);
    bg = g;
    d_twin(x, &g);
    memcpy(d, x->mem, (size_t)x->bd.nblocks * 512);
    if(tree_read(x, &td)) die("lying: mount");
    t_init(&t, x, 1);
    t.io.read = lying_read;
    (void)run(&t, FAT_CHECK_REPAIR, &r);
    check(r.status == FAT_CHECK_TROUBLE && strstr(t.log, "does not read back"), "a copy that does not read back stops the run");
    check(rd16(root_entry(x, &g, "TWIN    TXT") + 26) == rd16(root_entry(x, &g, E_README) + 26), "and its name is still where it was");
    if(tree_read(x, &tf)) die("lying: mount after");
    check(!same_files(&td, &tf, NULL) && !none_freed(d, x->mem, &g), "and every file reads as it did");
    tree_free(&td);
    tree_free(&tf);
    t_done(&t);
    free(d);
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
          strstr(t.log, "not unmounted cleanly") && strstr(t.log, "fsck -r would:"), "-v says the counts and the plan");
    check(strstr(t.log, "copy the first FAT over the second, cut 1 chain, set 1 size, copy 6 clusters for 1 name, save 3 lost chains, set FSInfo, mark it clean") != NULL,
          "the plan says exactly what will change");
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
    if(argc > 1) {
        fuzz((u32)atoi(argv[1]), argc > 2 ? (u32)atoi(argv[2]) : 200, 0);
    } else {
        for(k = 0; k < 4; k++) {
            for(i = 0; i < sizeof(scens) / sizeof(scens[0]); i++) {
                int on = scens[i].on;
                if((on == ON_FAT32 && types[k] != 32) || (on == ON_FAT12 && types[k] != 12) || (on == ON_TWO && types[k] == 11)) continue;
                if(only && !strstr(scens[i].name, only)) continue;
                scenario(types[k], &scens[i]);
            }
        }
        if(!only) {
            bad_reads();
            names_said();
            words();
            many();
            long_dir();
            copy_cost();
            lying();
            for(k = 1; k < 4; k++) truncated(types[k]);
            deep_and_wide(16);
            deep_and_wide(32);
            no_memory();
            for(k = 1; k < 4; k++) { stops(types[k]); refusals(types[k]); }
            big_stick();
            fuzz(20261101, 200, 1);
        }
    }
    if(have_fsck) {
        char cmd[120];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", fsck_dir);
        if(system(cmd)) printf("    (the temporary directory stays: %s)\n", fsck_dir);
    }
    printf("FSCK: %d checks, %d scenarios, %d pulls, %d failures\n", checks, nscen_ran, npulls, failures);
    return failures ? 1 : 0;
}
