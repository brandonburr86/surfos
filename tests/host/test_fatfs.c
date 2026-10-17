/* Host-side test of the block cache and FAT together (10/16/26, audit B5-B11): fs/bcache.c,
   fs/fat.c and fs/fat_mkfs.c as they are, over RAM disks that can refuse writes (all of them, or
   those that touch some blocks) and reads. The kernel around them is a few stand-ins below: one
   task, so bcache's waits only let go of its lock and take it again; "another task" is a hook
   that runs inside a device write, which is when bcache_lock is let go, as a second task would
   run then. An oracle reads the FAT16 image after every write the disk stores (every point at
   which a card could be pulled) and checks that no directory entry, and no FAT link on the way,
   reaches a cluster the first FAT says is free.

   The scenarios are those of the review of B1-B4 (T1-T10, B4a-c) and of its fixes; each says
   which audit item it checks. 10/16/26 (round fs-fix-2): and those of the review of B5-B11 (its
   scenarios R1-R9, written by that review, here under the audit item that fixes each: B12a-B17a)
   and of their fixes (B12b-B18). 10/16/26 (round fs-fix-3): and those of the review of B12-B19 (its
   scenarios S1-S6, under the audit item that fixes each: B20-B24) and of their fixes (B25, B26).
   10/16/26 (round fs-fix-4): and those of the review of B20-B26 (its scenarios R1-R6, under the audit
   item that fixes each: B27-B31, B22b) and of their fixes (B28c, B28d, B30b, B31b, B32).
   10/17/26 (round fs-fix-6): and those of the review of B34-B38 (its scenarios Q1-Q6, under the audit item
   that fixes each: B39-B45; a kill is made as kill_task() makes it, TI_KILL in a call, and BEFORE builds
   them against 47fb4ec, which has no fs_hold).
   Built like the kernel objects (tests/host/pager_shim for the interrupt flag) and run by `make
   unittest`; surfcpu/docs/evidence/os/bcache-review.txt has what each scenario found before and
   after the fixes. */
#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <surfos/wait.h>
#include <surfos/task.h>
#include <surfos/interrupt.h>
#include <surfos/ktimer.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <fs/fat.h>
#include <mm/kalloc.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* the host's C library, declared by hand (the SurfOS headers stand in for its own); unsigned long
   for sizes, so the call is right whether long is 32 bits (-m32) or 64 */
void exit(int status) __attribute__((noreturn));
void *malloc(unsigned long n);
void *calloc(unsigned long n, unsigned long m);
void free(void *p);
char *getenv(const char *name);

static int failures, checks, verbose;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
    else if(verbose) printf("ok: %s\n", what);
}

static void die(const char *why) {
    printf("FATFS: stopped: %s\n", why);
    exit(2);
}

/**** the kernel around the file system ****/

int sim_if = 1;                         /* tests/host/pager_shim/surfos/irq.h's interrupt flag */
volatile int in_irq;
surf_task *curTask, *idle_task;         /* none: bcache's sleep_unlocked() lets go of its lock and takes it again */
static long lock_ops;

void mutex_init(mutex_t *m, const char *name) { m->locked = 0; m->owner = NULL; m->name = name; }
void mutex_lock(mutex_t *m) {
    if(++lock_ops > 200000000L) die("a wait that never ends (a lock taken 200 million times)");
    if(m->locked && strcmp(m->name, "bounce")) {   /* a hook's task may take the bounce buffer: its owner is done with it */
        printf("mutex %s\n", m->name);
        die("a lock taken twice: a second task would wait here for ever");
    }
    m->locked++;
}
void mutex_unlock(mutex_t *m) { if(m->locked) m->locked--; }
bool mutex_trylock(mutex_t *m) { if(m->locked) return false; m->locked = 1; return true; }
void wait_prepare(wait_queue_t *q) { (void)q; }
void wait_prepare_intr(wait_queue_t *q) { (void)q; }   /* 10/17/26 (round fs-fix-6, B39): a wait a kill cuts short */
void wake_up_all(wait_queue_t *q) { (void)q; }
void kcritical_enter(void) { if(curTask) curTask->crit++; }   /* (B39: fs/fat.c's lock) */
void kcritical_leave(void) { if(curTask && curTask->crit) curTask->crit--; }
/* 10/16/26 (round fs-fix-4, B33): what another task does while this one sleeps in the cache (yield() is reached
   only when curTask is set: a scenario that sets it); nothing otherwise */
static void (*yield_hook)(void);
void yield(void) { if(yield_hook) yield_hook(); }
/* 10/16/26 (B18): the clock moves 50 ms each time it is read, so a wait for a holder that never lets go
   ends (bcache's sleeps only let go of its lock: there is no other task); its timers are never armed */
static u_long clock_ms;
u_long uptime_ms(void) { return clock_ms += 50; }
void ktimer_init(struct ktimer *t, void (*fn)(void *), void *arg) { (void)t; (void)fn; (void)arg; }
void ktimer_start(struct ktimer *t, u_long ms, bool periodic) { (void)t; (void)ms; (void)periodic; }
void ktimer_cancel(struct ktimer *t) { (void)t; }

static char klog[1 << 15];
static u32 klog_len;

void kprintf(const char *fmt, ...) {
    char line[512];
    u32 n;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    n = strlen(line);
    if(klog_len + n + 1 < sizeof(klog)) { memcpy(klog + klog_len, line, n + 1); klog_len += n; }
    if(verbose) printf("      [kprintf] %s", line);
}

/* how many times what appears in the console since the log was emptied */
static int said(const char *what) {
    int n = 0;
    u32 l = strlen(what), i;
    for(i = 0; i + l <= klog_len; i++) if(!memcmp(klog + i, what, l)) n++;
    return n;
}

/* 10/16/26 (round fs-fix-4, B28d): while kalloc_fail is set, every kalloc() fails */
static int kalloc_fail;
void *kalloc(size_t n) { return kalloc_fail ? NULL : malloc(n); }
/* 10/16/26 (round fs-fix-4, B30): the next kcalloc_fail calls fail (ENOMEM), each after kcalloc_hook ran */
static int kcalloc_fail;
static void (*kcalloc_hook)(void);
void *kcalloc(size_t n, size_t m) {
    if(kcalloc_fail) {
        kcalloc_fail--;
        if(kcalloc_hook) kcalloc_hook();
        return NULL;
    }
    return calloc(n, m);
}
void kfree(void *p) { free(p); }
void get_time(time_t *t) {
    memset(t, 0, sizeof(*t));
    t->year = 2026; t->month = 10; t->date = 16; t->hour = 12;
}

/**** RAM disks that refuse on request ****/

struct ram {
    struct bdev bd;
    u8 *mem;
    int fail_all;                       /* every write fails */
    u32 bad_lo, bad_hi;                 /* a write that touches blocks bad_lo..bad_hi fails (bad_hi 0: none) */
    u32 rbad;                           /* a read that touches this block fails (0: none) */
    void (*hook)(struct ram *r, u32 lba, u32 count);    /* after a write is stored: another task runs */
    u32 writes, failed, reads, read_blocks;
    int oracle;
    u32 violations;
    char violation[200];
};

static u32 zero_length_writes;
static void oracle_check(struct ram *r);

static int ram_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    struct ram *r = (struct ram *)d->priv;
    if(r->rbad && lba <= r->rbad && r->rbad < lba + count) return BDEV_EIO;
    r->reads++;
    r->read_blocks += count;
    memcpy(buf, r->mem + lba * 512, count * 512);
    return 0;
}

static int ram_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct ram *r = (struct ram *)d->priv;
    if(r->fail_all || (r->bad_hi && lba <= r->bad_hi && lba + count - 1 >= r->bad_lo)) {
        r->failed++;
        return BDEV_EIO;
    }
    memcpy(r->mem + lba * 512, buf, count * 512);
    r->writes++;
    if(r->oracle) oracle_check(r);
    if(r->hook) r->hook(r, lba, count);
    return 0;
}

static const struct bdev_ops ram_ops = { ram_read, ram_write, NULL };

/* 10/16/26 (B9): the cache's account of a device against its blocks, where the cache has one */
int bcache_audit(struct bdev *dev);
static struct ram *made[16];
static int nmade;

static struct ram *ram_new(const char *name, u32 blocks) {
    struct ram *r = (struct ram *)calloc(1, sizeof(*r));
    if(nmade < 16) made[nmade++] = r;
    r->mem = (u8 *)calloc(blocks, 512);
    if(!r->mem) die("out of memory");
    strlcpy(r->bd.name, name, sizeof(r->bd.name));
    r->bd.block_size = 512;
    r->bd.nblocks = blocks;
    r->bd.bus = BDEV_BUS_RAM;
    r->bd.ops = &ram_ops;
    r->bd.priv = r;
    return r;
}

/* driver/bdev.c's checks, as they are */
static struct bdev *resolve(struct bdev *d, u32 *lba, u32 count) {
    while(d) {
        if(*lba + count < *lba || *lba + count > d->nblocks) return NULL;
        if(!d->parent) return d;
        *lba += d->start;
        d = d->parent;
    }
    return NULL;
}

int bdev_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    struct bdev *disk = resolve(d, &lba, count);
    if(d->gone || (disk && disk->gone)) return BDEV_EGONE;
    if(!disk || !disk->ops || !disk->ops->read) return BDEV_ERANGE;
    if(!count) return 0;
    return disk->ops->read(disk, lba, count, buf);
}

int bdev_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct bdev *disk = resolve(d, &lba, count);
    if(d->gone || (disk && disk->gone)) return BDEV_EGONE;
    if(!disk || !disk->ops || !disk->ops->write) return BDEV_ERANGE;
    if(d->readonly || disk->readonly) return BDEV_EROFS;
    if(!count) { zero_length_writes++; return 0; }
    return disk->ops->write(disk, lba, count, buf);
}

int bdev_flush(struct bdev *d) { return d->gone ? BDEV_EGONE : 0; }
struct bdev *bdev_disk(struct bdev *d) { while(d && d->parent) d = d->parent; return d; }
int bdev_may_format(struct bdev *d, char *why, size_t size) { (void)d; if(why && size) why[0] = 0; return 0; }

/**** the VFS, as much as fat.c needs ****/

static const struct fs_type *fat;
int vfs_register(const struct fs_type *t) { fat = t; return 0; }

struct vnode *vnode_alloc(struct superblock *sb, const struct vnode_ops *ops, int type) {
    struct vnode *v = (struct vnode *)calloc(1, sizeof(*v));
    v->sb = sb; v->ops = ops; v->type = type; v->refs = 1;
    if(sb) sb->nvnodes++;
    return v;
}

void vput(struct vnode *v) {
    if(!v || --v->refs) return;
    if(v->ops->release) v->ops->release(v);
    if(v->sb) v->sb->nvnodes--;
    free(v);
}

static void mkfs(struct ram *r) {
    char msg[256];
    if(fat_mkfs(&r->bd, "TEST", 0, msg, sizeof(msg)) != 0) { printf("mkfs: %s\n", msg); die("mkfs failed"); }
}

static struct superblock *mount(struct ram *r) {
    struct superblock *sb = (struct superblock *)calloc(1, sizeof(*sb));
    sb->type = fat;
    sb->dev = &r->bd;
    mutex_init(&sb->lock, "fat");
    if(fat->mount(sb) != 0) die("mount failed");
    return sb;
}

/* vfs_fsync(): the file system's sync, then its device's blocks */
static int fsync_sb(struct superblock *sb) {
    int r = sb->ops->sync(sb);
    if(bsync(sb->dev) != 0 && !r) r = -EIO;
    return r;
}

static struct vnode *lookup(struct vnode *dir, const char *name) {
    struct vnode *v;
    return dir->ops->lookup(dir, name, &v) ? NULL : v;
}

static int put_file(struct vnode *dir, const char *name, const u8 *data, u32 len) {
    struct vnode *v;
    int r = dir->ops->create(dir, name, VN_FILE, &v), w;
    if(r) return r;
    if(len) {
        w = v->ops->write(v, 0, data, len);
        if(w != (int)len) r = w < 0 ? w : -EIO;
    }
    vput(v);
    return r;
}

/* a file written in pieces of at most 64 KB */
static int big_file(struct vnode *dir, const char *name, const u8 *piece, u32 len) {
    struct vnode *v;
    u32 off = 0, k;
    int r = dir->ops->create(dir, name, VN_FILE, &v);
    while(!r && off < len) {
        k = len - off < 65536 ? len - off : 65536;
        if(v->ops->write(v, off, piece, k) != (int)k) r = -EIO;
        off += k;
    }
    if(!r) vput(v);
    return r;
}

static int file_is(struct vnode *dir, const char *name, const u8 *data, u32 len) {
    static u8 back[65536];
    struct vnode *v = lookup(dir, name);
    int ok;
    if(!v) return 0;
    ok = v->size == len && len <= sizeof(back) && v->ops->read(v, 0, back, len) == (int)len && !memcmp(back, data, len);
    vput(v);
    return ok;
}

static int names(struct vnode *dir, char *out, u32 size) {
    struct dirent de;
    u_int i;
    out[0] = 0;
    for(i = 0; dir->ops->readdir(dir, i, &de) == 0; i++) {
        if(out[0]) strlcat(out, " ", size);
        strlcat(out, de.name, size);
    }
    return (int)i;
}

static u8 pat[65536], pat2[65536];
static void fill(u8 *b, u32 n, u32 seed) { u32 i; for(i = 0; i < n; i++) b[i] = (u8)(seed * 31 + i * 7 + (i >> 8)); }

/**** the oracle: the image as the next computer would read it (FAT16) ****/

static u32 g16(const u8 *p) { return (u32)(p[0] | (p[1] << 8)); }

struct geo { u32 spc, fat_lba, spf, root_lba, root_secs, data_lba, nclusters; };

static void geometry(struct ram *r, struct geo *g) {
    const u8 *bs = r->mem;
    u32 rsv = g16(bs + 14), nfats = bs[16], re = g16(bs + 17);
    u32 tot = g16(bs + 19) ? g16(bs + 19) : (u32)bs[32] | ((u32)bs[33] << 8) | ((u32)bs[34] << 16) | ((u32)bs[35] << 24);
    g->spc = bs[13];
    g->spf = g16(bs + 22);
    g->fat_lba = rsv;
    g->root_lba = rsv + nfats * g->spf;
    g->root_secs = (re * 32 + 511) / 512;
    g->data_lba = g->root_lba + g->root_secs;
    g->nclusters = (tot - g->data_lba) / g->spc;
}

static u32 fat1(struct ram *r, const struct geo *g, u32 c) { return g16(r->mem + g->fat_lba * 512 + c * 2); }
static u32 clus_lba(const struct geo *g, u32 c) { return g->data_lba + (c - 2) * g->spc; }

static void violation(struct ram *r, const char *what) {
    if(!r->violations++) {
        snprintf(r->violation, sizeof(r->violation), "after the disk's write %u: %s", r->writes, what);
        if(verbose) printf("      ORACLE: %s\n", r->violation);
    }
}

static void check_entries(struct ram *r, const struct geo *g, const u8 *p, u32 n, int depth);

static void check_dir(struct ram *r, const struct geo *g, u32 first, int depth) {
    u32 c = first, steps = 0;
    if(depth > 4) return;
    if(!first) { check_entries(r, g, r->mem + g->root_lba * 512, g->root_secs * 16, depth); return; }
    while(c >= 2 && c < g->nclusters + 2 && steps++ < 4096) {
        u32 next = fat1(r, g, c);
        char m[120];
        if(!next) {
            snprintf(m, sizeof(m), "a directory's chain reaches cluster %u, which is free", c);
            violation(r, m);
            return;
        }
        check_entries(r, g, r->mem + clus_lba(g, c) * 512, g->spc * 16, depth);
        if(next >= 0xFFF8) return;
        c = next;
    }
}

static void check_entries(struct ram *r, const struct geo *g, const u8 *p, u32 n, int depth) {
    u32 i;
    for(i = 0; i < n; i++, p += 32) {
        u32 first, c, steps = 0;
        char m[120], name[12];
        if(p[0] == 0) return;
        if(p[0] == 0xE5 || p[11] == 0x0F || (p[11] & 0x08)) continue;
        if(p[0] == '.' && (p[1] == ' ' || p[1] == '.')) continue;
        memcpy(name, p, 11);
        name[11] = 0;
        first = g16(p + 26);
        for(c = first; c >= 2 && c < g->nclusters + 2 && steps++ < 65536; ) {
            u32 v = fat1(r, g, c);
            if(!v) {
                snprintf(m, sizeof(m), "entry '%s' -> cluster %u (step %u of its chain) is free", name, c, steps - 1);
                violation(r, m);
                break;
            }
            if((p[11] & 0x10) || v >= 0xFFF8) break;
            c = v;
        }
        if((p[11] & 0x10) && first) check_dir(r, g, first, depth + 1);
    }
}

static void oracle_check(struct ram *r) {
    struct geo g;
    geometry(r, &g);
    check_dir(r, &g, 0, 0);
}

/* the clusters the first FAT on the disk marks used, and how many of them the root's entries reach */
static void census(struct ram *r, u32 *used, u32 *reached) {
    struct geo g;
    const u8 *p;
    u32 c, i;
    geometry(r, &g);
    *used = *reached = 0;
    for(c = 2; c < g.nclusters + 2; c++) if(fat1(r, &g, c)) (*used)++;
    p = r->mem + g.root_lba * 512;
    for(i = 0; i < g.root_secs * 16; i++, p += 32) {
        u32 steps = 0;
        if(!p[0]) break;
        if(p[0] == 0xE5 || p[11] == 0x0F || (p[11] & 0x08)) continue;
        for(c = g16(p + 26); c >= 2 && c < g.nclusters + 2 && steps++ < g.nclusters; ) {
            u32 v = fat1(r, &g, c);
            (*reached)++;
            if(!v || v >= 0xFFF8) break;
            c = v;
        }
    }
}

/* the account of every disk of the scenario that ends (B9) */
static void audit(void) {
    int i, off = 0;
    for(i = 0; i < nmade; i++) off += bcache_audit(&made[i]->bd);
    if(nmade) check(off == 0, "the cache's account of each disk matches its blocks (B9)");
    nmade = 0;
}

/* each scenario starts with an empty cache and its own disks */
static void fresh(const char *title) {
    audit();
    init_bcache();
    klog_len = 0;
    klog[0] = 0;
    if(verbose) printf("%s\n", title);
}

/**** T1 (B7): a directory that grows by a cluster, its new entry, and the file it names ****/

static void t1(void) {
    struct ram *x = ram_new("t1", 8192);           /* FAT16, 512-byte clusters, 256 FAT entries a sector */
    struct superblock *sb;
    struct vnode *root, *d;
    char name[8];
    int i, r = 0, s;
    fresh("T1: a file created in a full directory");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    if(root->ops->create(root, "d", VN_DIR, &d)) die("T1: mkdir");
    fill(pat, 65536, 1);
    r = big_file(root, "big", pat, 600 * 512);      /* the free clusters start two FAT sectors on */
    for(i = 0; i < 14 && !r; i++) { snprintf(name, sizeof(name), "f%02d", i); r = put_file(d, name, NULL, 0); }   /* d's first cluster full */
    if(r || fsync_sb(sb)) die("T1: setup");
    x->oracle = 1;
    fill(pat, 1024, 2);
    r = put_file(d, "x", pat, 1024);
    s = fsync_sb(sb);
    check(r == 0 && s == 0, "T1: create and write a file in a full directory, sync");
    check(x->violations == 0, "T1 (B7): nothing on the disk reaches a free cluster at any write (directory grown, file created)");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    check(file_is(d, "x", pat, 1024), "T1: the file reads back");
    vput(d);
}

/**** T2 (B7): blocks dirtied while bsync() runs (another task writes while this one syncs) ****/

/* "another task": at the disk's write of block hook_lba it creates hook_name (512 bytes) in hook_dir;
   it never touches a block in the middle of a write (it would wait for it in the kernel) */
static struct vnode *hook_dir;
static u32 hook_lba;
static int hook_ran;
static const char *hook_name;

static void late_writer(struct ram *r, u32 lba, u32 count) {
    if(hook_ran || !(lba <= hook_lba && hook_lba < lba + count)) return;
    hook_ran = 1;
    r->hook = NULL;
    fill(pat2, 512, 9);
    if(put_file(hook_dir, hook_name, pat2, 512) != 0) die("the hook's create");
}

static void t2(void) {
    struct ram *x = ram_new("t2", 8192);
    struct superblock *sb;
    struct vnode *v, *sub;
    struct geo g;
    int r;
    fresh("T2: bsync() while another task creates a file");
    mkfs(x);
    sb = mount(x);
    geometry(x, &g);
    if(sb->root->ops->create(sb->root, "sub", VN_DIR, &sub)) die("T2: mkdir");
    fill(pat, 600, 3);
    if(put_file(sb->root, "a", pat, 600) || fsync_sb(sb)) die("T2: setup");
    v = lookup(sb->root, "a");
    fill(pat, 600, 4);
    v->ops->write(v, 600, pat, 600);                /* data, FAT and the root's entry block dirty */
    vput(v);
    hook_dir = sub; hook_lba = g.root_lba; hook_ran = 0; hook_name = "late";
    x->hook = late_writer;                          /* runs while bsync() writes the root's entry block */
    x->oracle = 1;
    r = bsync(&x->bd);                              /* as vfs_close() does: no sb->lock held */
    check(hook_ran && r == 0, "T2: bsync() with another task's create in the middle");
    check(x->violations == 0, "T2 (B7): nothing on the disk reaches a free cluster while another task writes during a sync");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    r = fsync_sb(sb);
    fill(pat2, 512, 9);
    check(r == 0 && file_is(sub, "late", pat2, 512) && x->violations == 0, "T2: the next sync writes the rest, in order");
    vput(sub);
}

/**** T3 (B7, B8): making room evicts a directory block; another task changes it meanwhile ****/

static void t3(void) {
    struct ram *x = ram_new("t3", 8192), *y = ram_new("t3y", 8192);
    struct superblock *sb;
    struct vnode *v;
    struct buf *b;
    struct geo g;
    u32 i, next;
    fresh("T3: eviction of a directory block while another task changes it");
    mkfs(x);
    sb = mount(x);
    geometry(x, &g);
    fill(pat, 600, 5);
    if(put_file(sb->root, "a", pat, 600) || fsync_sb(sb)) die("T3: setup");
    v = lookup(sb->root, "a");
    fill(pat, 600, 6);
    v->ops->write(v, 600, pat, 600);                /* clusters 3 and 4 written, the entry's block dirty last */
    vput(v);
    /* the FAT and data blocks used again, so the directory block is the oldest dirty one; the blocks the
       other task will use are cached (its create needs no new buffer) */
    for(next = 2; next < 64 && fat1(x, &g, next); next++) ;
    next += 1;                                      /* cluster 4, taken by the append, is not on the disk yet */
    for(i = 0; i < 6; i++) if((b = bread(&x->bd, clus_lba(&g, next) + i)) != NULL) brelse(b);
    for(i = 0; i < 2; i++) if((b = bread(&x->bd, g.fat_lba + i * g.spf)) != NULL) brelse(b);
    for(i = 0; i < 6; i++) if((b = bread(&x->bd, clus_lba(&g, 2) + i)) != NULL) brelse(b);
    hook_dir = sb->root; hook_lba = clus_lba(&g, 3); hook_ran = 0; hook_name = "late";
    x->hook = late_writer;                          /* runs while the eviction writes a's data, below the entry */
    x->oracle = 1;
    for(i = 0; i < 2100; i++) if((b = bread(&y->bd, i)) != NULL) brelse(b);     /* a reader of another disk makes room */
    check(hook_ran, "T3: the reader's eviction wrote the volume's blocks");
    check(x->violations == 0, "T3 (B7, B8): nothing on the disk reaches a free cluster when an evicted block changes meanwhile");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    x->hook = NULL;
    fill(pat2, 512, 9);
    check(fsync_sb(sb) == 0 && file_is(sb->root, "late", pat2, 512) && x->violations == 0, "T3: the sync after it");
}

/**** T4 (B5): rename while the card refuses writes; then the same mv again, or rm ****/

static void t4(int variant) {
    struct ram *x = ram_new("t4", 8192);
    struct superblock *sb;
    struct vnode *root;
    char list[256];
    u32 used, reached;
    int r, s;
    fresh(variant ? "T4b: rename refused, then rm" : "T4a: rename refused, then the same mv again");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    fill(pat, 2048, 7);
    if(put_file(root, "a", pat, 2048) || fsync_sb(sb)) die("T4: setup");
    x->fail_all = 1;
    r = root->ops->rename(root, "a", root, "b");
    names(root, list, sizeof(list));
    check(r == -EIO, variant ? "T4b: mv a b with the card refusing: EIO" : "T4a: mv a b with the card refusing: EIO");
    check(!strcmp(list, "a"), variant ? "T4b (B5): after the refused mv the directory holds a, as before (one name for the file)"
                                     : "T4a (B5): after the refused mv the directory holds a, as before (one name for the file)");
    if(strcmp(list, "a") && verbose) printf("      the directory: %s\n", list);
    x->fail_all = 0;
    if(!variant) {
        r = root->ops->rename(root, "a", root, "b");
        s = fsync_sb(sb);
        check(r == 0 && s == 0, "T4a: the same mv with the card working, and sync");
    } else {
        r = root->ops->unlink(root, "b");
        s = fsync_sb(sb);
        check(r == -ENOENT && s == 0, "T4b: rm b with the card working: no such file (there is no second name)");
    }
    fill(pat2, 2048, 8);
    r = put_file(root, "c", pat2, 2048);
    s = fsync_sb(sb);
    check(r == 0 && s == 0, "T4: a new file after it");
    check(file_is(root, variant ? "a" : "b", pat, 2048), variant ? "T4b (B5): a keeps its own data after rm and a new file"
                                                                   : "T4a (B5): b keeps its own data after the second mv and a new file");
    census(x, &used, &reached);
    check(used == reached, "T4: on the disk, every used cluster is a file's, and nothing else");
}

/**** T5 (B6): a card that refuses every write must not starve the cache of the other disks ****/

static void t5(void) {
    struct ram *x = ram_new("t5x", 16384), *y = ram_new("t5y", 4096);
    struct superblock *sb;
    struct vnode *v;
    struct buf *b;
    u32 off, wrote = 0, before, i, ok = 0;
    int w = 0;
    fresh("T5: a card that refuses every write");
    mkfs(x);
    sb = mount(x);
    x->fail_all = 1;
    if(sb->root->ops->create(sb->root, "f", VN_FILE, &v) != 0) die("T5: create");
    fill(pat, 4096, 10);
    for(off = 0; off < 2u << 20; off += 4096) {     /* 2 MB in 4 KB pieces: through the cache */
        w = v->ops->write(v, off, pat, 4096);
        if(w != 4096) break;
        wrote += 4096;
    }
    if(verbose) printf("      accepted %u KB, then write() = %d\n", wrote >> 10, w);
    check(off < 2u << 20 && wrote <= BCACHE_BLOCKS / 4 * 512 + 16384,
          "T5 (B6): writes to the refusing card stop (EIO) once it holds about a quarter of the cache");
    before = x->failed + x->writes;
    for(i = 0; i < 64; i++) if((b = bread(&y->bd, i * 8)) != NULL) { ok++; brelse(b); }
    check(ok == 64, "T5 (B6): another disk is still read through the cache");
    check(x->failed + x->writes == before, "T5 (B6): reading another disk tries no write to the refusing card");
    check(fsync_sb(sb) == -EIO, "T5: fsync of the refusing card says EIO");
    vput(v);
}

/**** T6 (B6): one sector that refuses writes must not hold back its neighbours ****/

static void t6(void) {
    struct ram *x = ram_new("t6", 8192);
    struct superblock *sb;
    struct geo g;
    char name[8];
    u32 i, good = 0;
    int r1, r2;
    fresh("T6: one data sector that refuses writes");
    mkfs(x);
    sb = mount(x);
    geometry(x, &g);
    x->bad_lo = x->bad_hi = clus_lba(&g, 2);        /* the first data cluster: file a's */
    fill(pat, 512, 11);
    put_file(sb->root, "a", pat, 512);
    for(i = 0; i < 20; i++) { snprintf(name, sizeof(name), "n%02u", i); fill(pat, 512, 100 + i); put_file(sb->root, name, pat, 512); }
    r1 = fsync_sb(sb);
    r2 = fsync_sb(sb);
    for(i = 0; i < 20; i++) { fill(pat, 512, 100 + i); if(!memcmp(x->mem + clus_lba(&g, 3 + i) * 512, pat, 512)) good++; }
    check(r1 == -EIO && r2 == -EIO, "T6: sync says EIO while the sector refuses");
    check(good == 20, "T6 (B6): the 20 data blocks next to the bad one reach the disk (a failed run is tried block by block)");
    x->bad_hi = 0;
    check(fsync_sb(sb) == 0 && x->violations == 0, "T6: once the sector takes writes, sync writes the rest");
}

/**** T7 (B7): an append whose new cluster's FAT entry is in another sector than the link to it ****/

static void t7(void) {
    struct ram *x = ram_new("t7", 8192);
    struct superblock *sb;
    struct vnode *v;
    int r, s;
    fresh("T7: an append across FAT sectors");
    mkfs(x);
    sb = mount(x);
    fill(pat, 512, 12);
    r = put_file(sb->root, "a", pat, 512);          /* cluster 2 */
    fill(pat2, 65536, 13);
    if(!r) r = big_file(sb->root, "big", pat2, 600 * 512);   /* clusters 3-602 */
    if(r || fsync_sb(sb)) die("T7: setup");
    v = lookup(sb->root, "a");
    x->oracle = 1;
    fill(pat, 512, 14);
    r = v->ops->write(v, 512, pat, 512);
    s = fsync_sb(sb);
    check(r == 512 && s == 0, "T7: append 512 bytes and sync");
    check(x->violations == 0, "T7 (B7): the link to the new cluster reaches the disk after the cluster's own FAT entry");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    vput(v);
}

/**** T8 (B6): a card that refused for a while and works again; another disk is read ****/

static void t8(void) {
    struct ram *x = ram_new("t8x", 8192), *y = ram_new("t8y", 8192);
    struct buf *b;
    u32 i;
    fresh("T8: the card refuses for a while, then works again");
    x->fail_all = 1;
    b = bget(&x->bd, 10);
    bdirty_order(b, BORDER_TABLE);                  /* a FAT block, dirtied first */
    brelse(b);
    for(i = 0; i < 1100; i++) {
        b = bget(&x->bd, 100 + i);
        if(!b) break;
        bdirty(b);
        brelse(b);
    }
    if(verbose) printf("      data blocks dirtied while the card refused: %u\n", i);
    check(i <= BCACHE_BLOCKS / 4 + 8, "T8 (B6): bget() for the refusing card stops at about a quarter of the cache");
    x->fail_all = 0;
    b = bread(&y->bd, 0);
    check(b != NULL, "T8 (B6): with the card working again, another disk is read without a sync first");
    if(b) brelse(b);
    check(bsync(&x->bd) == 0, "T8: and a sync writes the card's blocks");
}

/**** T9 (B5): rm, and open with O_TRUNC, while the card refuses writes ****/

static void t9(void) {
    struct ram *x = ram_new("t9", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    char list[256];
    u32 used, reached;
    int r1, r2;
    fresh("T9: rm and truncate to 0 refused");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    fill(pat, 4096, 15);
    if(put_file(root, "a", pat, 4096) || put_file(root, "t", pat, 4096) || fsync_sb(sb)) die("T9: setup");
    x->fail_all = 1;
    r1 = root->ops->unlink(root, "a");
    v = lookup(root, "t");
    r2 = v ? v->ops->truncate(v, 0) : -999;
    if(v) vput(v);
    names(root, list, sizeof(list));
    check(r1 == -EIO && r2 == -EIO, "T9: rm and truncate with the card refusing: EIO");
    check(!strcmp(list, "a t"), "T9 (B5): after the refused rm, a is still there");
    check(file_is(root, "a", pat, 4096) && file_is(root, "t", pat, 4096), "T9 (B5): a and t keep their bytes and sizes");
    x->fail_all = 0;
    check(fsync_sb(sb) == 0, "T9: the card works again: sync");
    census(x, &used, &reached);
    check(used == reached, "T9 (B5): no cluster on the disk is lost (used, and no entry reaching it)");
    check(root->ops->unlink(root, "a") == 0 && fsync_sb(sb) == 0 && !file_is(root, "a", pat, 4096),
          "T9: rm with the card working");
    census(x, &used, &reached);
    check(used == reached, "T9: and nothing is lost after it");
}

/**** T10 (B6): read-ahead after one block of the card failed to write ****/

static void t10(void) {
    struct ram *x = ram_new("t10x", 8192), *y = ram_new("t10y", 8192);
    struct buf *b;
    u32 i, before;
    fresh("T10: read-ahead with a failed block at the old end of the cache");
    x->fail_all = 1;
    b = bget(&x->bd, 5);
    bdirty(b);
    brelse(b);
    bsync(&x->bd);                                  /* fails: the block stays dirty */
    for(i = 0; i < 1100; i++) if((b = bread(&y->bd, 1000 + i)) != NULL) brelse(b);   /* it drifts to the old end */
    before = y->reads;
    for(i = 0; i < 512; i++) {
        b = (i % 8) ? bread(&y->bd, 4000 + i) : breada(&y->bd, 4000 + i, 8);
        if(b) brelse(b);
    }
    if(verbose) printf("      512 blocks in order: %u requests\n", y->reads - before);
    check(y->reads - before == 64, "T10 (B6): read-ahead still works (64 requests for 512 blocks, 8 at a time)");
    x->fail_all = 0;
}

/**** T11 (B8): the block being evicted is dropped while write_below() writes ****/

static struct ram *drop_dev;
static void dropper(struct ram *r, u32 lba, u32 count) {
    (void)lba; (void)count;
    r->hook = NULL;
    binvalidate(&drop_dev->bd);                     /* as an umount of the volume would, meanwhile */
}

static void t11(void) {
    struct ram *x = ram_new("t11", 8192), *y = ram_new("t11y", 8192);
    struct superblock *sb;
    struct vnode *v;
    struct buf *b;
    struct geo g;
    u32 i;
    fresh("T11: the evicted block is dropped meanwhile");
    mkfs(x);
    sb = mount(x);
    geometry(x, &g);
    fill(pat, 600, 16);
    if(put_file(sb->root, "a", pat, 600) || fsync_sb(sb)) die("T11: setup");
    v = lookup(sb->root, "a");
    v->ops->write(v, 600, pat, 600);
    vput(v);
    for(i = 0; i < 2; i++) if((b = bread(&x->bd, g.fat_lba + i * g.spf)) != NULL) brelse(b);
    for(i = 0; i < 6; i++) if((b = bread(&x->bd, clus_lba(&g, 2) + i)) != NULL) brelse(b);
    drop_dev = x;
    x->hook = dropper;
    zero_length_writes = 0;
    for(i = 0; i < 2100; i++) if((b = bread(&y->bd, i)) != NULL) brelse(b);
    check(zero_length_writes == 0, "T11 (B8): no zero-length write for a block that went away while its order was written");
}

/**** T12 (B6): a block that was never tried is not marked as failed ****/

static void t12(void) {
    struct ram *x = ram_new("t12x", 8192), *y = ram_new("t12y", 8192);
    struct buf *b;
    u32 i, before;
    fresh("T12: untried is not failed");
    b = bget(&x->bd, 200); bdirty(b); brelse(b);                         /* data, changed first */
    b = bget(&x->bd, 300); bdirty_order(b, BORDER_DIR); brelse(b);        /* and an entry block that may point at it */
    b = bpeek(&x->bd, 200); brelse(b);                                   /* (B12) the entry block the older in the cache */
    for(i = 0; i < BCACHE_BLOCKS - 2; i++) if((b = bread(&y->bd, i)) != NULL) brelse(b);   /* the cache full: both oldest */
    x->fail_all = 1;
    before = x->failed;
    for(; i < 3000; i++) if((b = bread(&y->bd, i)) != NULL) brelse(b);
    b = bpeek(&x->bd, 300);
    check(b && b->dirty && !b->wfail, "T12 (B6): the entry block held back by the data's failure is dirty, not marked failed");
    check(x->failed - before <= 1, "T12 (B6): making room tried the refusing card once, not on every miss");
    if(b) brelse(b);
    x->fail_all = 0;
    check(bsync(&x->bd) == 0, "T12: the sync once the card works");
}

/**** T13 (B9): a device's failures are said once, whatever devices came and went before ****/

static void t13(void) {
    struct ram *d[9];
    struct buf *b;
    u32 i, k;
    fresh("T13: the console line about a failing device");
    for(i = 0; i < 9; i++) {
        char name[8];
        snprintf(name, sizeof(name), "u%u", i);
        d[i] = ram_new(name, 1024);
        d[i]->fail_all = 1;
        for(k = 0; k < (i < 8 ? 1u : 3u); k++) {   /* the last one fails three times */
            b = bget(&d[i]->bd, 10 + k * 4);
            bdirty(b);
            brelse(b);
            bsync(&d[i]->bd);
        }
        if(i < 8) { d[i]->bd.gone = true; binvalidate(&d[i]->bd); }   /* unplugged while failing */
    }
    check(said("write of u8 blocks") == 1, "T13 (B9): a ninth failing device's failures are said once");
    d[8]->fail_all = 0;
    bsync(&d[8]->bd);
    check(said("u8 takes writes again") == 1, "T13 (B9): and that it takes writes again");
}

/**** T14 (B7): truncate to a smaller size: the cut on the disk before the clusters past it are free ****/

static void t14(void) {
    struct ram *x = ram_new("t14", 8192);
    struct superblock *sb;
    struct vnode *v;
    struct buf *b;
    struct geo g;
    int r, s;
    fresh("T14: truncate to a smaller size");
    mkfs(x);
    sb = mount(x);
    geometry(x, &g);
    fill(pat, 65536, 30);
    r = big_file(sb->root, "f", pat, 300 * 512);    /* clusters 2-301: FAT sectors 0 and 1 */
    if(!r) r = big_file(sb->root, "g", pat, 500 * 512);    /* 302-801 */
    v = r ? NULL : lookup(sb->root, "f");
    if(!v || v->ops->write(v, 300 * 512, pat, 100 * 512) != 100 * 512 || fsync_sb(sb)) die("T14: setup");   /* f: 802-901, sector 3 */
    init_bcache();                                  /* (all clean) FAT sector 3 cached first: written first */
    if((b = bread(&x->bd, g.fat_lba + 3)) != NULL) brelse(b);
    if((b = bread(&x->bd, g.fat_lba + g.spf + 3)) != NULL) brelse(b);
    x->oracle = 1;
    r = v->ops->truncate(v, 300 * 512);             /* the cut at cluster 301 (sector 1), the frees in sector 3 */
    s = fsync_sb(sb);
    check(r == 0 && s == 0 && v->size == 300 * 512, "T14: truncate to 300 clusters, sync");
    check(x->violations == 0, "T14 (B7): the chain on the disk never runs into a freed cluster");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    vput(v);
}

/**** T15 (B5, B7): truncate to a smaller size while the card refuses writes ****/

static void t15(void) {
    struct ram *x = ram_new("t15", 8192);
    struct superblock *sb;
    struct vnode *v;
    u32 used, reached;
    int r;
    fresh("T15: truncate refused");
    mkfs(x);
    sb = mount(x);
    fill(pat, 65536, 31);
    if(big_file(sb->root, "f", pat, 8192) || fsync_sb(sb)) die("T15: setup");
    v = lookup(sb->root, "f");
    x->fail_all = 1;
    r = v->ops->truncate(v, 1024);
    check(r == -EIO && v->size == 8192, "T15 (B5): truncate to 1 KB with the card refusing: EIO, the size stays");
    x->fail_all = 0;
    vput(v);
    check(fsync_sb(sb) == 0 && file_is(sb->root, "f", pat, 8192), "T15 (B5): the file reads back whole after the card works");
    census(x, &used, &reached);
    check(used == reached && used == 16, "T15: its 16 clusters, nothing lost");
}

/**** T16 (B7): a directory made, and a file in it before any sync ****/

static void t16(void) {
    struct ram *x = ram_new("t16", 8192);
    struct superblock *sb;
    struct vnode *d;
    int r, s;
    fresh("T16: mkdir and a file in it");
    mkfs(x);
    sb = mount(x);
    x->oracle = 1;
    if(sb->root->ops->create(sb->root, "d", VN_DIR, &d)) die("T16: mkdir");
    fill(pat, 2048, 32);
    r = put_file(d, "x", pat, 2048);
    s = fsync_sb(sb);
    check(r == 0 && s == 0 && file_is(d, "x", pat, 2048), "T16: mkdir, a file in it, sync");
    check(x->violations == 0, "T16 (B7): nothing on the disk reaches a free cluster at any write");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    vput(d);
}

/**** B4 (regression): a write that stops early gives its clusters back ****/

static void b4(void) {
    struct ram *x = ram_new("b4", 4400);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    u32 used, reached;
    int w, w2;
    fresh("B4: writes that stop early");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    x->oracle = 1;
    fill(pat, 65536, 20);
    if(big_file(root, "big", pat, (g.nclusters - 20) * g.spc * 512)) die("B4: setup");
    if(root->ops->create(root, "f", VN_FILE, &v)) die("B4: create");
    fill(pat, 65536, 21);
    w = v->ops->write(v, 0, pat, 65536);
    w2 = v->ops->write(v, w > 0 ? (u32)w : 0, pat, 4096);
    check(w == (int)(20 * g.spc * 512) && w2 == -ENOSPC, "B4a: a 64 KB write on a nearly full volume puts in what fits; the next gets ENOSPC");
    check(fsync_sb(sb) == 0, "B4a: sync");
    census(x, &used, &reached);
    check(used == reached, "B4a: no cluster lost");
    vput(v);
    root->ops->unlink(root, "f");
    root->ops->unlink(root, "big");
    fsync_sb(sb);
    if(root->ops->create(root, "g", VN_FILE, &v)) die("B4: create g");
    x->fail_all = 1;
    w = v->ops->write(v, 0, pat, 65536);            /* straight to the device: refused */
    x->fail_all = 0;
    check(w == -EIO && fsync_sb(sb) == 0, "B4b: a new file's 64 KB write refused by the card");
    census(x, &used, &reached);
    check(used == reached && used == 0, "B4b: its clusters are free again");
    fill(pat, 1024, 23);
    w = v->ops->write(v, 0, pat, 1024);
    check(w == 1024 && fsync_sb(sb) == 0, "B4c: 1 KB written");
    x->fail_all = 1;
    fill(pat2, 65536, 24);
    w = v->ops->write(v, 1024, pat2, 65536);
    x->fail_all = 0;
    check(w == -EIO && v->size == 1024, "B4c: an append of 64 KB refused: the size stays");
    check(fsync_sb(sb) == 0, "B4c: sync");
    census(x, &used, &reached);
    check(used == reached && used == 2 && x->violations == 0, "B4c: the chain is cut back to the file's 2 clusters");
    check(file_is(root, "g", pat, 1024), "B4c: the 1 KB reads back");
    vput(v);
}

/**** B11: fat_set() changes both FATs or neither ****/

static void b11(void) {
    struct ram *x = ram_new("b11", 8192);
    struct superblock *sb;
    struct geo g;
    u32 used, reached, fat2_sector;
    int r;
    fresh("B11: a FAT change whose second copy cannot be read");
    mkfs(x);
    sb = mount(x);
    geometry(x, &g);
    fill(pat, 65536, 25);
    if(big_file(sb->root, "big", pat, 600 * 512) || fsync_sb(sb)) die("B11: setup");
    fat2_sector = g.fat_lba + g.spf + (602 * 2) / 512;   /* the second FAT's copy of the next free cluster's sector */
    init_bcache();                                  /* nothing cached (the volume is synced) */
    x->rbad = fat2_sector;
    fill(pat, 512, 26);
    r = put_file(sb->root, "n", pat, 512);
    x->rbad = 0;
    check(r == -EIO, "B11: a write whose cluster's second FAT copy cannot be read fails");
    check(fsync_sb(sb) == 0, "B11: sync");
    check(!memcmp(x->mem + g.fat_lba * 512, x->mem + (g.fat_lba + g.spf) * 512, g.spf * 512), "B11: the two FATs on the disk agree");
    census(x, &used, &reached);
    check(used == reached, "B11: no cluster lost");
}

/* the same for a cut: truncate whose cluster's second FAT sector cannot be read */
static void b11b(void) {
    struct ram *x = ram_new("b11b", 8192);
    struct superblock *sb;
    struct vnode *v;
    struct geo g;
    u32 used, reached;
    int r;
    fresh("B11b: a cut whose second FAT copy cannot be read");
    mkfs(x);
    sb = mount(x);
    geometry(x, &g);
    fill(pat, 65536, 27);
    if(big_file(sb->root, "f", pat, 8192) || fsync_sb(sb)) die("B11b: setup");     /* clusters 2-17 */
    v = lookup(sb->root, "f");
    init_bcache();                                  /* nothing cached (the volume is synced) */
    x->rbad = g.fat_lba + g.spf;                    /* the second FAT's first sector */
    r = v->ops->truncate(v, 1024);
    x->rbad = 0;
    check(r == -EIO && v->size == 8192, "B11b: a truncate whose cut cannot reach the second FAT fails, the size stays");
    vput(v);
    check(fsync_sb(sb) == 0, "B11b: sync");
    check(!memcmp(x->mem + g.fat_lba * 512, x->mem + (g.fat_lba + g.spf) * 512, g.spf * 512), "B11b: the two FATs on the disk agree");
    census(x, &used, &reached);
    check(used == reached && used == 16, "B11b: the file keeps its 16 clusters, none lost");
}

/**** the review of B5-B11 (round fs-fix-2): its scenarios R1-R9 (fsrev-tests/review.c, kept as they were
      found, renamed for the audit item that fixes each) and those of the fixes ****/

/* the dirty blocks of a disk, counted through bpeek() */
static u32 count_dirty(struct ram *x) {
    u32 lba, n = 0;
    for(lba = 0; lba < x->bd.nblocks; lba++) {
        struct buf *b = bpeek(&x->bd, lba);
        if(!b) continue;
        if(b->dirty) n++;
        brelse(b);
    }
    return n;
}

static u32 le32m(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

/* the 32-byte entry named n11 (8.3, space padded) among n entries at lba, in the image */
static u8 *disk_entry(struct ram *r, u32 lba, u32 n, const char *n11) {
    u8 *p = r->mem + lba * 512;
    u32 i;
    for(i = 0; i < n; i++, p += 32) {
        if(!p[0]) return NULL;
        if(p[0] == 0xE5 || p[11] == 0x0F) continue;
        if(!memcmp(p, n11, 11)) return p;
    }
    return NULL;
}

/**** B12a (R1): a close's write-back while another task keeps writing other files ****/

static struct vnode *r1_w[2];
static u32 r1_wlba[2], r1_sublba, r1_runs, r1_writes_seen;
static int r1_in;

/* "another task" writing 512 bytes at the start of w1 and w2 (each one cluster, in sub) whenever the
   cache lets go of its lock for a device write that does not hold the blocks it needs */
static void r1_hook(struct ram *r, u32 lba, u32 count) {
    int k;
    (void)r;
    r1_writes_seen++;
    if(r1_in || r1_runs >= 100000) return;
    if(lba <= r1_sublba && r1_sublba < lba + count) return;
    r1_in = 1;
    for(k = 0; k < 2; k++) {
        if(lba <= r1_wlba[k] && r1_wlba[k] < lba + count) continue;
        fill(pat2, 512, 40 + r1_runs + k);
        if(r1_w[k]->ops->write(r1_w[k], 0, pat2, 512) != 512) die("B12a: the other task's write");
    }
    r1_runs++;
    r1_in = 0;
}

static void b12a(void) {
    struct ram *x = ram_new("b12a", 8192);
    struct superblock *sb;
    struct vnode *root, *sub, *a;
    struct geo g;
    u8 *e, *s;
    u32 size_on_disk;
    int r_fs, r_dev;
    fresh("B12a (R1): a close's write-back while another task keeps writing");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    if(root->ops->create(root, "sub", VN_DIR, &sub)) die("B12a: mkdir");
    fill(pat, 600, 1);
    if(put_file(sub, "w1", pat, 512) || put_file(sub, "pad", pat, 512) || put_file(sub, "w2", pat, 512) ||
       put_file(root, "a", pat, 600) || fsync_sb(sb))   /* w1 and w2 not next to each other on the disk */
        die("B12a: setup");
    s = disk_entry(x, g.root_lba, g.root_secs * 16, "SUB        ");
    if(!s) die("B12a: sub on the disk");
    r1_sublba = clus_lba(&g, g16(s + 26));
    e = disk_entry(x, r1_sublba, 16, "W1         "); if(!e) die("B12a: w1"); r1_wlba[0] = clus_lba(&g, g16(e + 26));
    e = disk_entry(x, r1_sublba, 16, "W2         "); if(!e) die("B12a: w2"); r1_wlba[1] = clus_lba(&g, g16(e + 26));
    r1_w[0] = lookup(sub, "w1");
    r1_w[1] = lookup(sub, "w2");
    a = lookup(root, "a");
    fill(pat, 600, 2);
    if(a->ops->write(a, 600, pat, 600) != 600) die("B12a: append");   /* a: data, FAT and the root's entry dirty */
    /* vfs_close(a) now: write_back() = fat_sync() (its bsync) and bsync(), no sb->lock held; the other task
       writes w1 and w2 whenever the device works */
    r1_runs = r1_writes_seen = 0;
    x->hook = r1_hook;
    x->oracle = 1;
    r_fs = sb->ops->sync(sb);
    r_dev = bsync(&x->bd);
    x->hook = NULL;
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "A          ");
    size_on_disk = e ? le32m(e + 28) : 0;
    if(verbose) printf("      syncs %d %d, device writes %u, the other task wrote %u times; a's size on the disk %u\n",
                       r_fs, r_dev, r1_writes_seen, r1_runs, size_on_disk);
    check(r1_runs > 0 && !(r_fs == 0 && r_dev == 0 && size_on_disk != 1200),
          "B12a (R1): when close's write-back says 0, the closed file's entry is on the disk");
    check(x->violations == 0, "B12a: nothing on the disk reaches a free cluster meanwhile");
    vput(a);
    vput(r1_w[0]); vput(r1_w[1]);
    check(fsync_sb(sb) == 0 && count_dirty(x) == 0, "B12a (R1): a sync without the other task writes the rest");
    vput(sub);
}

/**** B12b: another task keeps changing the very directory block a sync owes, and what it points at ****/

static struct vnode *b12b_k[2];
static u32 b12b_root, b12b_fat_lo, b12b_fat_hi, b12b_klba[2], b12b_runs;
static int b12b_in;

/* "another task" writing the 512 bytes of k1 or k2 again (both in the root, as a is: each write changes the
   file's data, then its entry, in the same directory block as a's) whenever a device write does not hold a
   block it needs: never the root's entries or a FAT sector, and the one of k1, k2 that is not being written */
static void b12b_hook(struct ram *r, u32 lba, u32 count) {
    int k;
    (void)r;
    if(b12b_in || b12b_runs >= 100000) return;
    if((lba <= b12b_root && b12b_root < lba + count) || (lba <= b12b_fat_hi && b12b_fat_lo < lba + count)) return;
    k = lba <= b12b_klba[0] && b12b_klba[0] < lba + count;
    if(lba <= b12b_klba[k] && b12b_klba[k] < lba + count) return;
    b12b_in = 1;
    fill(pat2, 512, 50 + b12b_runs);
    if(b12b_k[k]->ops->write(b12b_k[k], 0, pat2, 512) != 512) die("B12b: the other task's write");
    b12b_runs++;
    b12b_in = 0;
}

static void b12b(void) {
    struct ram *x = ram_new("b12b", 8192);
    struct superblock *sb;
    struct vnode *root, *a;
    struct geo g;
    u8 *e;
    u32 size_on_disk, runs_then;
    int r_fs, r_dev;
    fresh("B12b: a sync whose directory block another task keeps changing");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 1024, 3);
    if(put_file(root, "k1", pat, 512) || put_file(root, "gap1", pat, 1024) || put_file(root, "k2", pat, 512) ||
       put_file(root, "gap2", pat, 1024) || put_file(root, "a", pat, 600) || fsync_sb(sb))
        die("B12b: setup");                         /* k1: cluster 2, k2: 5, a: 8-9 (nothing of theirs side by side) */
    b12b_root = g.root_lba;                          /* a's, k1's and k2's entries */
    b12b_fat_lo = g.fat_lba;
    b12b_fat_hi = g.fat_lba + 2 * g.spf - 1;
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "K1         ");
    if(!e) die("B12b: k1 on the disk");
    b12b_klba[0] = clus_lba(&g, g16(e + 26));
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "K2         ");
    if(!e) die("B12b: k2 on the disk");
    b12b_klba[1] = clus_lba(&g, g16(e + 26));
    b12b_k[0] = lookup(root, "k1");
    b12b_k[1] = lookup(root, "k2");
    a = lookup(root, "a");
    fill(pat, 600, 4);
    if(a->ops->write(a, 600, pat, 600) != 600) die("B12b: append");   /* a: data, FAT and the root's entry dirty */
    b12b_runs = 0;
    x->hook = b12b_hook;
    x->oracle = 1;
    r_fs = sb->ops->sync(sb);                       /* vfs_close(a)'s write-back, while the other task writes */
    r_dev = bsync(&x->bd);
    runs_then = b12b_runs;
    x->hook = NULL;
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "A          ");
    size_on_disk = e ? le32m(e + 28) : 0;
    if(verbose) printf("      syncs %d %d; the other task wrote %u times; a's size on the disk %u\n", r_fs, r_dev, runs_then, size_on_disk);
    check(runs_then >= 4 && r_fs == 0 && r_dev == 0 && size_on_disk == 1200,
          "B12b: the syncs say 0, and the closed file's entry is on the disk (written from a copy)");
    check(x->violations == 0, "B12b: nothing on the disk reaches a free cluster at any write meanwhile");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    vput(a);
    vput(b12b_k[0]);
    vput(b12b_k[1]);
    check(fsync_sb(sb) == 0 && count_dirty(x) == 0 && x->violations == 0, "B12b: a sync without the other task writes the rest");
}

/**** B13a, B16a (R2): one sector that refuses, files written after the first sync, and the file it is in ****/

static void b13a(void) {
    struct ram *x = ram_new("b13a", 8192);
    struct superblock *sb;
    struct vnode *root;
    struct geo g;
    char name[8];
    u32 i, good_m = 0, before;
    int s3, rm_a;
    fresh("B13a, B16a (R2): one data sector that refuses writes, and files written after the first sync");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    x->bad_lo = x->bad_hi = clus_lba(&g, 2);        /* the first data cluster: file a's */
    fill(pat, 512, 11);
    put_file(root, "a", pat, 512);
    for(i = 0; i < 20; i++) { snprintf(name, sizeof(name), "n%02u", i); fill(pat, 512, 100 + i); put_file(root, name, pat, 512); }
    fsync_sb(sb);
    fsync_sb(sb);
    for(i = 0; i < 20; i++) { snprintf(name, sizeof(name), "m%02u", i); fill(pat, 512, 200 + i); put_file(root, name, pat, 512); }
    s3 = fsync_sb(sb);
    before = x->failed;
    fsync_sb(sb);
    for(i = 0; i < 20; i++) { fill(pat, 512, 200 + i); if(!memcmp(x->mem + clus_lba(&g, 23 + i) * 512, pat, 512)) good_m++; }
    check(s3 == -EIO && good_m == 20,
          "B13a (R2): data written after the first failed sync reaches the disk (one bad block does not hold back the others)");
    check(x->failed - before <= 2, "B13a: a sync asks the refusing sector once, not for every block");
    rm_a = root->ops->unlink(root, "a");
    check(rm_a == 0 && x->bd.bc_failed == 0, "B16a (R2): rm of the file whose block refuses works (its block is dropped)");
    check(root->ops->unlink(root, "n00") == 0 && root->ops->rename(root, "n01", root, "zz") == 0,
          "B16a (R2): and so do the volume's other operations");
    check(fsync_sb(sb) == 0 && x->violations == 0, "B16a: then a sync, the sector still refusing (nothing left for it)");
    x->bad_hi = 0;
    {
        u32 used, reached;
        census(x, &used, &reached);
        check(fsync_sb(sb) == 0 && used == reached, "B13a: once the sector takes writes, sync; no cluster lost");
    }
}

/**** B13b (R2b): two refusing sectors at the head of a run ****/

static void b13b(void) {
    struct ram *x = ram_new("b13b", 8192);
    struct superblock *sb;
    struct vnode *root;
    struct geo g;
    char name[8];
    u32 i, good = 0;
    int s1, s2, s3;
    fresh("B13b (R2b): two data sectors that refuse writes at the head of a run");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    x->bad_lo = clus_lba(&g, 2);
    x->bad_hi = clus_lba(&g, 3);                    /* files a and b's clusters */
    fill(pat, 512, 11);
    put_file(root, "a", pat, 512);
    put_file(root, "b", pat, 512);
    for(i = 0; i < 20; i++) { snprintf(name, sizeof(name), "n%02u", i); fill(pat, 512, 100 + i); put_file(root, name, pat, 512); }
    s1 = fsync_sb(sb); s2 = fsync_sb(sb); s3 = fsync_sb(sb);
    for(i = 0; i < 20; i++) { fill(pat, 512, 100 + i); if(!memcmp(x->mem + clus_lba(&g, 4 + i) * 512, pat, 512)) good++; }
    if(verbose) printf("      three syncs %d %d %d; good data blocks on the disk %u of 20\n", s1, s2, s3, good);
    check(s1 == -EIO && good == 20, "B13b (R2b): blocks of a failed run that the card would take are written (two bad ones do not hold back the rest)");
    x->bad_hi = 0;
    check(fsync_sb(sb) == 0, "B13b: once the sectors take writes, sync");
}

/**** B13c (R8): a refused FAT run, and another task's data write during it ****/

static int r8_hooked;
static int r8_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    int w = ram_write(d, lba, count, buf);
    if(w != 0 && count > 1 && !r8_hooked) {        /* the card refused the FAT run: meanwhile another task */
        struct buf *b = bget(d, 100);              /* writes a block of file data */
        r8_hooked = 1;
        if(b) { memset(b->data, 7, 512); bdirty(b); brelse(b); }
    }
    return w;
}
static const struct bdev_ops r8_ops = { ram_read, r8_write, NULL };

static void b13c(void) {
    struct ram *x = ram_new("b13c", 8192);
    struct buf *b;
    u32 lba;
    int r1, r2;
    fresh("B13c (R8): a refused FAT run, another task's data write during it");
    x->bd.ops = &r8_ops;
    for(lba = 10; lba <= 11; lba++) { b = bread(&x->bd, lba); memset(b->data, 1, 512); bdirty_order(b, BORDER_TABLE); brelse(b); }
    b = bread(&x->bd, 50); memset(b->data, 2, 512); bdirty_order(b, BORDER_DIR); brelse(b);
    x->bad_lo = x->bad_hi = 10;                     /* one FAT sector refuses writes */
    r8_hooked = 0;
    r1 = bsync(&x->bd);
    r2 = bsync(&x->bd);
    check(r8_hooked && r1 != 0 && r2 != 0, "B13c (R8): a sync in which the card refused a write does not return 0, nor does the next");
    check(x->mem[11 * 512] == 1 && x->mem[100 * 512] == 7 && x->mem[50 * 512] == 0,
          "B13c: the FAT sector that works and the data are written, the entry block waits");
    x->bad_hi = 0;
    check(bsync(&x->bd) == 0 && x->mem[10 * 512] == 1 && x->mem[50 * 512] == 2, "B13c (R8): once the sector takes writes, sync writes the rest");
}

/**** B14a (R4): the card refused, then works again: the next operations work without a sync first ****/

static void b14a(void) {
    struct ram *x = ram_new("b14a", 16384);
    struct superblock *sb;
    struct vnode *v, *root;
    u32 off;
    int w = 0, c1, c2, c3;
    fresh("B14a (R4): a card that refused, then works again");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    if(root->ops->create(root, "f", VN_FILE, &v) != 0) die("B14a: create");
    fill(pat, 512, 12);
    if(put_file(root, "old", pat, 512) || fsync_sb(sb)) die("B14a: setup");
    x->fail_all = 1;
    fill(pat, 4096, 10);
    for(off = 0; off < 2u << 20; off += 4096) if((w = v->ops->write(v, off, pat, 4096)) != 4096) break;
    check(w != 4096 && off < 2u << 20, "B14a: the refusing card's share fills, and writes stop");
    x->fail_all = 0;                                /* the card takes writes again */
    c1 = put_file(root, "g", pat, 512);
    c2 = root->ops->unlink(root, "old");
    c3 = v->ops->write(v, 0, pat, 512);
    if(verbose) printf("      refusing: stopped at %u KB; working again: create %d, rm %d, write %d\n", off >> 10, c1, c2, c3);
    check(c1 == 0 && c2 == 0 && c3 == 512, "B14a (R4): once the card works again, a create, an rm and a write work (without a sync first)");
    vput(v);
    check(fsync_sb(sb) == 0 && file_is(root, "g", pat, 512), "B14a: and a sync");
}

/**** B14b (R9): a directory block refused once; the card works again; a long write ****/

static void b14b(void) {
    struct ram *x = ram_new("b14b", 16384);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    u32 off;
    int w = 0, s1;
    fresh("B14b (R9): a directory block refused once, then a long write on the working card");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 512, 13);
    if(put_file(root, "a", pat, 512)) die("B14b: a");
    x->bad_lo = x->bad_hi = g.root_lba;             /* the root's entry block: refused by one sync */
    s1 = bsync(&x->bd);
    x->bad_hi = 0;                                  /* the card takes everything again */
    if(root->ops->create(root, "big", VN_FILE, &v)) die("B14b: create");
    fill(pat, 2048, 80);
    for(off = 0; off < (1u << 20); off += 2048) if((w = v->ops->write(v, off, pat, 2048)) != 2048) break;
    if(verbose) printf("      the sync with the block refused: %d; the 1 MiB write stopped at %u KiB (%d)\n", s1, off >> 10, w);
    check(s1 != 0 && off == (1u << 20), "B14b (R9): a card that works is not refused (one block refused before and not synced since)");
    vput(v);
    check(fsync_sb(sb) == 0, "B14b: sync");
}

/**** B15a (R5): the share of a refusing card, with writes straight to it over cached blocks ****/

static void b15a(void) {
    struct ram *x = ram_new("b15a", 16384);
    struct superblock *sb;
    struct vnode *root, *v[3], *small;
    u32 i, k, dirty;
    int w[3], ws = 0;
    char name[8];
    static u8 back[512];
    fresh("B15a (R5): dirty blocks a refusing card can hold");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    fill(pat, 65536, 50);
    for(k = 0; k < 3; k++) {
        snprintf(name, sizeof(name), "big%u", k);
        if(big_file(root, name, pat, 65536)) die("B15a: setup");
        v[k] = lookup(root, name);
        for(i = 0; i < 128; i++) v[k]->ops->read(v[k], i * 512, back, 512);    /* read in small pieces: cached */
    }
    if(big_file(root, "small", pat, 250 * 512) || fsync_sb(sb)) die("B15a: setup sync");
    small = lookup(root, "small");
    x->fail_all = 1;
    /* 250 blocks of an existing file written over through the cache (each write checks the share), then
       64 KB over each big file (straight to the card, over the blocks the cache holds of it) */
    for(i = 0; i < 250 && ws >= 0; i++) ws = small->ops->write(small, i * 512, pat, 512);
    fill(pat2, 65536, 51);
    for(k = 0; k < 3; k++) w[k] = v[k]->ops->write(v[k], 0, pat2, 65536);
    dirty = count_dirty(x);
    if(verbose) printf("      small writes %d, 64 KB writes %d %d %d; dirty blocks %u (BCACHE_HOLD %u)\n", ws, w[0], w[1], w[2], dirty, (u32)BCACHE_HOLD);
    check(dirty <= BCACHE_HOLD + 8, "B15a (R5): a refusing card keeps at most about a quarter of the cache (128 KiB) dirty");
    x->fail_all = 0;
    for(k = 0; k < 3; k++) vput(v[k]);
    vput(small);
    check(fsync_sb(sb) == 0, "B15a: sync once the card works");
}

/**** B15b: a write straight to the card that it refuses changes nothing, in the cache either ****/

static void b15b(void) {
    struct ram *x = ram_new("b15b", 8192);
    struct superblock *sb;
    struct vnode *v;
    static u8 back[512];
    u32 i;
    int w;
    fresh("B15b: a refused write straight to the card, over blocks the cache holds");
    mkfs(x);
    sb = mount(x);
    fill(pat, 65536, 52);
    if(big_file(sb->root, "f", pat, 65536) || fsync_sb(sb)) die("B15b: setup");
    v = lookup(sb->root, "f");
    for(i = 0; i < 128; i++) v->ops->read(v, i * 512, back, 512);         /* cached */
    x->fail_all = 1;
    fill(pat2, 65536, 53);
    w = v->ops->write(v, 0, pat2, 65536);
    check(w == -EIO && count_dirty(x) == 0, "B15b: the write fails, and leaves no dirty block");
    vput(v);
    check(file_is(sb->root, "f", pat, 65536), "B15b: the file reads as it was (the cached copies keep their bytes)");
    x->fail_all = 0;
    check(fsync_sb(sb) == 0 && !memcmp(x->mem + 0, x->mem + 0, 1) && file_is(sb->root, "f", pat, 65536),
          "B15b: and a sync once the card works does not write the refused bytes either");
}

/**** B16b: mv over the file whose block refuses (10/16/26, B20: EIO, then rm and mv); truncate past a block that refuses ****/

static void b16b(void) {
    struct ram *x = ram_new("b16b", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    u32 used, reached;
    int r1, r2;
    fresh("B16b: mv over the file whose block refuses, truncate past a block that refuses");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 1024, 54);
    fill(pat2, 512, 55);
    if(put_file(root, "a", pat, 512) || put_file(root, "t", pat, 1024) || put_file(root, "src", pat2, 512) || fsync_sb(sb))
        die("B16b: setup");                         /* a: cluster 2; t: 3-4; src: 5 */
    x->bad_lo = x->bad_hi = clus_lba(&g, 2);
    v = lookup(root, "a"); fill(pat, 512, 56); v->ops->write(v, 0, pat, 512); vput(v);
    if(fsync_sb(sb) != -EIO) die("B16b: the refused sync");
    r1 = root->ops->rename(root, "src", root, "a");     /* 10/16/26 (B20): EIO, as it is not B16's to set them aside */
    check(r1 == -EIO && file_is(root, "src", pat2, 512) && file_is(root, "a", pat, 512),
          "B16b (B20): mv over the file whose block refuses says EIO while it refuses, and changes nothing");
    check(root->ops->unlink(root, "a") == 0 && root->ops->rename(root, "src", root, "a") == 0 && file_is(root, "a", pat2, 512),
          "B16b: an rm of that file works, then the mv");
    check(fsync_sb(sb) == 0, "B16b: then a sync, the block still refusing (nothing left for it)");
    x->bad_lo = x->bad_hi = clus_lba(&g, 4);
    v = lookup(root, "t"); fill(pat, 1024, 57); v->ops->write(v, 512, pat + 512, 512); vput(v);   /* t's second cluster */
    if(fsync_sb(sb) != -EIO) die("B16b: the second refused sync");
    v = lookup(root, "t");
    r2 = v ? v->ops->truncate(v, 512) : -999;
    if(v) vput(v);
    fill(pat, 1024, 54);
    check(r2 == 0 && file_is(root, "t", pat, 512), "B16b: truncate past the block that refuses works");
    check(fsync_sb(sb) == 0, "B16b: then a sync, the block still refusing (nothing left for it)");
    x->bad_hi = 0;
    census(x, &used, &reached);
    check(used == reached && used == 2 && x->violations == 0, "B16b: no cluster lost");
}

/**** B17a (R6): mv over an existing file whose source directory cannot be read after the replace ****/

static struct ram *r6_y;
static u32 r6_dirlba, r6_rootlba, r6_root_writes, r6_armed, r6_done, r6_at;
static void r6_hook(struct ram *r, u32 lba, u32 count) {
    struct buf *b;
    u32 i;
    if(!r6_armed || r6_done || !(lba <= r6_rootlba && r6_rootlba < lba + count)) return;
    if(++r6_root_writes < r6_at) return;            /* R6: the second order point (the replaced name gone) */
    r6_done = 1;
    for(i = 0; i < 2100; i++) if((b = bread(&r6_y->bd, i)) != NULL) brelse(b);   /* another task's reads make room */
    r->rbad = r6_dirlba;                            /* and the source directory's block cannot be read now */
}

static void r6_run(const char *title, u32 at, int variant) {
    struct ram *x = ram_new("b17a", 8192), *y = ram_new("b17ay", 8192);
    struct superblock *sb;
    struct vnode *root, *s;
    struct geo g;
    u8 *e;
    char list[256], slist[256];
    u32 used, reached;
    int r;
    fresh(title);
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    if(root->ops->create(root, "s", VN_DIR, &s)) die("B17a: mkdir");
    fill(pat, 2048, 60);
    if(put_file(s, "a", pat, 2048)) die("B17a: a");
    fill(pat2, 4096, 61);
    if(put_file(root, "b", pat2, 4096) || fsync_sb(sb)) die("B17a: b");
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "S          ");
    if(!e) die("B17a: s on the disk");
    r6_dirlba = clus_lba(&g, g16(e + 26));
    r6_rootlba = g.root_lba;
    r6_y = y;
    r6_root_writes = r6_done = 0;
    r6_at = at;
    r6_armed = 1;
    x->hook = r6_hook;
    x->oracle = 1;
    r = root->ops->rename(s, "a", root, "b");
    x->hook = NULL;
    x->rbad = 0;
    names(root, list, sizeof(list));
    names(s, slist, sizeof(slist));
    if(verbose) printf("      mv s/a b = %d (the hook ran: %u); the root now: '%s'; s now: '%s'\n", r, r6_done, list, slist);
    if(!variant) check(r6_done && (r == 0 || (strstr(list, "b") && file_is(root, "b", pat2, 4096))),
                       "B17a (R6): a mv that fails leaves the file it would have replaced");
    else check(r6_done && r == -EIO && !strcmp(list, "s b") && !strcmp(slist, "a") && file_is(root, "b", pat2, 4096) &&
               file_is(s, "a", pat, 2048), "B17b: a mv whose old name cannot be deleted changes nothing (EIO)");
    check(fsync_sb(sb) == 0 && x->violations == 0, variant ? "B17b: sync" : "B17a: sync");
    census(x, &used, &reached);                     /* (the census reaches into the root's files only: a, in s, is 4 more) */
    check(used == reached + (variant ? 4 : 0), variant ? "B17b: no cluster lost" : "B17a: no cluster lost");
    vput(s);
}

/**** B17c: mv of a directory to another parent whose first block cannot be read ****/

static void b17c(void) {
    struct ram *x = ram_new("b17c", 8192);
    struct superblock *sb;
    struct vnode *root, *s, *d = NULL;
    struct geo g;
    char list[256], slist[256];
    u8 *e;
    u32 dlba;
    int r;
    fresh("B17c: mv s/d d2 when d's own block cannot be read");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    if(root->ops->create(root, "s", VN_DIR, &s) || s->ops->create(s, "d", VN_DIR, &d)) die("B17c: mkdir");
    vput(d);
    if(fsync_sb(sb)) die("B17c: sync");
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "S          ");
    e = e ? disk_entry(x, clus_lba(&g, g16(e + 26)), 16, "D          ") : NULL;
    if(!e) die("B17c: d on the disk");
    dlba = clus_lba(&g, g16(e + 26));
    init_bcache();                                  /* nothing cached (the volume is synced) */
    x->rbad = dlba;                                 /* d's first block, with its "..", cannot be read */
    r = root->ops->rename(s, "d", root, "d2");
    x->rbad = 0;
    names(root, list, sizeof(list));
    names(s, slist, sizeof(slist));
    if(verbose) printf("      mv = %d; the root: '%s'; s: '%s'\n", r, list, slist);
    check(r == -EIO && !strcmp(list, "s") && !strcmp(slist, "d"), "B17c: the mv says EIO, and nothing changed");
    r = root->ops->rename(s, "d", root, "d2");
    check(r == 0 && fsync_sb(sb) == 0, "B17c: the same mv once the block can be read");
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "D2         ");
    check(e && g16(x->mem + clus_lba(&g, g16(e + 26)) * 512 + 32 + 26) == 0, "B17c: d2's \"..\" points at the root");
    vput(s);
}

/**** B17d: a file made in a full directory whose first block cannot be read once the directory grew ****/

static struct ram *b17d_y;
static u32 b17d_lba, b17d_writes;
static int b17d_done;
static void b17d_hook(struct ram *r, u32 lba, u32 count) {
    struct buf *b;
    u32 i;
    (void)lba; (void)count;
    if(b17d_done || ++b17d_writes < 2) return;      /* the new cluster's zeros are written: the FAT goes now */
    b17d_done = 1;
    for(i = 0; i < 2100; i++) if((b = bread(&b17d_y->bd, i)) != NULL) brelse(b);   /* another task's reads make room */
    r->rbad = b17d_lba;                             /* and d's first block cannot be read from now on */
}

static void b17d(void) {
    struct ram *x = ram_new("b17d", 8192), *y = ram_new("b17dy", 8192);
    struct superblock *sb;
    struct vnode *root, *d, *v = NULL;
    struct geo g;
    char name[8];
    u8 *e;
    int i, r = 0;
    fresh("B17d: a file made in a full directory that grows, its first block unreadable meanwhile");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    if(root->ops->create(root, "d", VN_DIR, &d)) die("B17d: mkdir");
    for(i = 0; i < 14 && !r; i++) { snprintf(name, sizeof(name), "f%02d", i); r = put_file(d, name, NULL, 0); }   /* d's cluster full */
    if(r || fsync_sb(sb)) die("B17d: setup");
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "D          ");
    if(!e) die("B17d: d on the disk");
    b17d_lba = clus_lba(&g, g16(e + 26));
    b17d_y = y;
    b17d_done = 0;
    b17d_writes = 0;
    x->hook = b17d_hook;                            /* in the order point of d's new cluster */
    r = d->ops->create(d, "x", VN_FILE, &v);
    x->hook = NULL;
    x->rbad = 0;
    if(verbose) printf("      create = %d (the hook ran: %d)\n", r, b17d_done);
    check(b17d_done && r == 0, "B17d: the create says it made the file (it does not read the directory back)");
    if(r == 0) vput(v);
    v = lookup(d, "x");
    check(v != NULL && fsync_sb(sb) == 0, "B17d: the file is there; sync");
    if(v) vput(v);
    vput(d);
}

/**** B18: a dirty block whose holder never lets go (a task killed in the middle of a change) ****/

static void b18(void) {
    struct ram *x = ram_new("b18", 1024);
    struct buf *b, *held[3];
    u_long t0, t1, t2;
    int r1, r2, r3, k;
    fresh("B18: dirty blocks whose holder never lets go");
    for(k = 0; k < 3; k++) {                        /* "another task" changes blocks 10, 12 and 14, and is killed holding them */
        held[k] = bread(&x->bd, 10 + 2 * k);
        memset(held[k]->data, 1 + k, 512);
        bdirty(held[k]);
    }
    t0 = clock_ms;
    b = bread(&x->bd, 11);
    memset(b->data, 9, 512);
    bdirty(b);
    brelse(b);
    r1 = bsync(&x->bd);
    t1 = clock_ms;
    r2 = bsync(&x->bd);
    t2 = clock_ms;
    if(verbose) printf("      syncs %d (%lu ms) and %d (%lu ms)\n", r1, t1 - t0, r2, t2 - t1);
    check(r1 == -1 && t1 - t0 >= 10000 && t1 - t0 < 12000,
          "B18: a sync waits 10 s for the holds (once for the three), then says their blocks were not written");
    check(r2 == -1 && t2 - t1 < 1000, "B18: the next sync gives up on those holds at once");
    check(x->mem[11 * 512] == 9 && x->mem[10 * 512] == 0 && x->mem[14 * 512] == 0, "B18: the other block is written, the held ones are not");
    check(said("by a task that does not let go") == 3, "B18: the console says it once for each");
    for(k = 0; k < 3; k++) brelse(held[k]);
    r3 = bsync(&x->bd);
    check(r3 == 0 && x->mem[10 * 512] == 1 && x->mem[14 * 512] == 3, "B18: once they are let go, a sync writes them");
}

/**** the review of B12-B19 (round fs-fix-3): its scenarios S1-S6 (fsrev3-tests/review3.c, ported here under the
      audit item that fixes each: B20-B24) and those of the fixes (B25, B26) ****/

/* the root's entries on the disk whose first cluster is c: how many, and the largest size among them */
static u32 disk_refs(struct ram *r, u32 c, u32 *maxsize) {
    struct geo g;
    u8 *p;
    u32 i, n = 0;
    geometry(r, &g);
    p = r->mem + g.root_lba * 512;
    if(maxsize) *maxsize = 0;
    for(i = 0; i < g.root_secs * 16; i++, p += 32) {
        if(!p[0]) break;
        if(p[0] == 0xE5 || p[11] == 0x0F) continue;
        if(g16(p + 26) == c) { n++; if(maxsize && le32m(p + 28) > *maxsize) *maxsize = le32m(p + 28); }
    }
    return n;
}

/**** B20a, B20b (S1a, S1b): mv over a file whose appended block refuses. B16 set the replaced file's refused
      blocks aside before the first order point, which then wrote the directory block holding the replaced
      file's entry, whose size covers the refused block, ahead of it ****/

static u32 s1_xlba, s1_bfirst, s1_bad, s1_bad_write, s1_armed, s1_done, s1_dirlba;
static u8 s1_new[512], s1_secret[512];
static struct ram *s1_y;
static int s1_variant;

/* after every write the disk stores (any point a card could be pulled): an entry for b's chain whose size covers
   block X while X on the disk does not hold b's bytes */
static void s1_hook(struct ram *r, u32 lba, u32 count) {
    u32 maxsize = 0, i;
    struct buf *b;
    struct geo g;
    geometry(r, &g);
    if(disk_refs(r, s1_bfirst, &maxsize) && maxsize > 512 && memcmp(r->mem + s1_xlba * 512, s1_new, 512)) {
        if(!s1_bad++) {
            s1_bad_write = r->writes;
            if(verbose) printf("      (the write that showed it: block %u, the root's first block is %u)\n", lba, g.root_lba);
        }
    }
    if(s1_variant && s1_armed && !s1_done && lba <= g.root_lba && g.root_lba < lba + count) {
        s1_done = 1;                                 /* the mv's first order point writes the root: another task's reads */
        for(i = 0; i < 2100; i++) if((b = bread(&s1_y->bd, i)) != NULL) brelse(b);   /* make room, and s's block */
        r->rbad = s1_dirlba;                         /* cannot be read now (as B17b does) */
    }
}

static void s1(int variant) {
    struct ram *x = ram_new(variant ? "b20b" : "b20a", 8192), *y = ram_new("b20ay", 8192);
    struct superblock *sb;
    struct vnode *root, *s = NULL, *v;
    struct geo g;
    u8 *e;
    u32 maxsize = 0, nrefs, arefs, afirst, used, reached;
    int r, s_before;
    static u8 bback[1024];
    fresh(variant ? "B20b (S1b): mv s/a b over a file whose appended block refuses; s's block unreadable after the root is written"
                  : "B20a (S1a): mv a b over a file whose appended block refuses");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    s1_y = y;
    memset(s1_secret, 'S', 512);
    memcpy(s1_secret, "ANOTHER FILE'S OLD BYTES", 24);
    fill(pat, 512, 70);
    if(put_file(root, "b", pat, 512) || put_file(root, "old", s1_secret, 512)) die("B20a: setup");
    if(variant) { if(root->ops->create(root, "s", VN_DIR, &s)) die("B20b: mkdir"); }
    fill(pat2, 512, 71);
    if(put_file(variant ? s : root, "a", pat2, 512) || fsync_sb(sb)) die("B20a: setup 2");
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "B          ");
    if(!e) die("B20a: b on the disk");
    s1_bfirst = g16(e + 26);                        /* cluster 2 */
    if(variant) {
        e = disk_entry(x, g.root_lba, g.root_secs * 16, "S          ");
        if(!e) die("B20b: s on the disk");
        s1_dirlba = clus_lba(&g, g16(e + 26));
    }
    if(root->ops->unlink(root, "old") || fsync_sb(sb)) die("B20a: rm old");   /* cluster 3 free again, its bytes on the disk */
    s1_xlba = clus_lba(&g, s1_bfirst + 1);
    if(memcmp(x->mem + s1_xlba * 512, s1_secret, 512)) die("B20a: old's bytes are not in cluster 3");
    x->bad_lo = x->bad_hi = s1_xlba;                /* that sector refuses writes from now on */
    fill(s1_new, 512, 72);
    v = lookup(root, "b");
    if(!v || v->ops->write(v, 512, s1_new, 512) != 512) die("B20a: append");   /* b grows into cluster 3 (cache path) */
    vput(v);
    s_before = fsync_sb(sb);                        /* refused: X failed; b's entry waits for it */
    nrefs = disk_refs(x, s1_bfirst, &maxsize);
    if(s_before != -EIO || nrefs != 1 || maxsize != 512) die("B20a: the refused append");
    s1_bad = s1_bad_write = s1_done = 0;
    s1_variant = variant;
    s1_armed = 1;
    x->hook = s1_hook;
    x->oracle = 1;
    r = root->ops->rename(variant ? s : root, "a", root, "b");
    x->hook = NULL;
    s1_armed = 0;
    x->rbad = 0;
    if(verbose) printf("      mv = %d; writes of the disk at which b's entry covered X while X held another file's bytes: %u\n",
                       r, s1_bad);
    check(s1_bad == 0, variant ? "B20b (S1b): no entry on the disk covers bytes that were not written (a mv that fails)"
                               : "B20a (S1a): no entry on the disk covers bytes that were not written (mv over a refusing file)");
    memcpy(bback, pat, 512);
    memcpy(bback + 512, s1_new, 512);
    check(r == -EIO && file_is(variant ? s : root, "a", pat2, 512) && file_is(root, "b", bback, 1024),
          variant ? "B20b: the mv says EIO while the replaced file's block refuses, and changed nothing"
                  : "B20a: the mv says EIO while the replaced file's block refuses, and changed nothing");
    fsync_sb(sb);
    fsync_sb(sb);
    if(variant) {
        e = disk_entry(x, s1_dirlba, 16, "A          ");
        afirst = e ? g16(e + 26) : 0;
        arefs = disk_refs(x, afirst, NULL) + (e ? 1 : 0);   /* the root's entries for a's chain, and s/a */
    } else {
        e = disk_entry(x, g.root_lba, g.root_secs * 16, "A          ");
        afirst = e ? g16(e + 26) : 0;
        arefs = disk_refs(x, afirst, NULL);
    }
    disk_refs(x, s1_bfirst, &maxsize);
    check(afirst && arefs == 1 && maxsize == 512 && x->violations == 0,
          variant ? "B20b (S1b): two syncs later (X still refusing) one name for a's chain on the disk, b's entry as it was"
                  : "B20a: two syncs later (X still refusing) one name for a's chain on the disk, b's entry as it was");
    x->bad_hi = 0;
    check(fsync_sb(sb) == 0 && disk_refs(x, s1_bfirst, &maxsize) == 1 && maxsize == 1024 && !memcmp(x->mem + s1_xlba * 512, s1_new, 512),
          variant ? "B20b: once X takes writes, a sync puts b's bytes and then its entry on the disk"
                  : "B20a: once X takes writes, a sync puts b's bytes and then its entry on the disk");
    r = root->ops->rename(variant ? s : root, "a", root, "b");
    check(r == 0 && file_is(root, "b", pat2, 512) && fsync_sb(sb) == 0 && x->violations == 0,
          variant ? "B20b: then the mv works" : "B20a: then the mv works");
    census(x, &used, &reached);
    check(used == reached, variant ? "B20b: no cluster lost" : "B20a: no cluster lost");
    if(s) vput(s);
}

/**** B20c (S1c): as B20a, and the card also refuses the root's block once the mv's first order point has
      written it (a card that refuses now and then): B16 had the mv fail at its second order point, with what
      the first one wrote (the long name b, and b's entry covering X) on the card for as long as X refused ****/

static u32 s1c_root, s1c_root_writes, s1c_armed;
static int s1c_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct ram *r = (struct ram *)d->priv;
    int w;
    if(s1c_armed && s1c_root_writes && lba <= s1c_root && s1c_root < lba + count) { r->failed++; return BDEV_EIO; }
    w = ram_write(d, lba, count, buf);
    if(w == 0 && s1c_armed && lba <= s1c_root && s1c_root < lba + count) s1c_root_writes++;
    return w;
}
static const struct bdev_ops s1c_ops = { ram_read, s1c_write, NULL };

static void b20c(void) {
    struct ram *x = ram_new("b20c", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    u8 *e;
    u32 maxsize = 0, afirst, arefs, used, reached;
    int r;
    fresh("B20c (S1c): mv a b over a file whose appended block refuses; the card refuses the root's block once it wrote it");
    mkfs(x);
    x->bd.ops = &s1c_ops;
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    memset(s1_secret, 'S', 512);
    memcpy(s1_secret, "ANOTHER FILE'S OLD BYTES", 24);
    fill(pat, 512, 70);
    if(put_file(root, "b", pat, 512) || put_file(root, "old", s1_secret, 512)) die("B20c: setup");
    fill(pat2, 512, 71);
    if(put_file(root, "a", pat2, 512) || fsync_sb(sb)) die("B20c: setup 2");
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "B          "); if(!e) die("B20c: b");
    s1_bfirst = g16(e + 26);
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "A          "); if(!e) die("B20c: a");
    afirst = g16(e + 26);
    if(root->ops->unlink(root, "old") || fsync_sb(sb)) die("B20c: rm old");
    s1_xlba = clus_lba(&g, s1_bfirst + 1);
    x->bad_lo = x->bad_hi = s1_xlba;
    fill(s1_new, 512, 72);
    v = lookup(root, "b");
    if(!v || v->ops->write(v, 512, s1_new, 512) != 512) die("B20c: append");
    vput(v);
    if(fsync_sb(sb) != -EIO) die("B20c: the refused sync");
    s1c_root = g.root_lba;
    s1c_root_writes = 0;
    s1c_armed = 1;
    x->oracle = 1;
    r = root->ops->rename(root, "a", root, "b");
    fsync_sb(sb);
    fsync_sb(sb);
    s1c_armed = 0;                                  /* the root's block takes writes again; X still refuses */
    fsync_sb(sb);
    arefs = disk_refs(x, afirst, NULL);
    disk_refs(x, s1_bfirst, &maxsize);
    if(verbose) printf("      mv a b = %d; root writes taken during it %u; names on the card for a's chain: %u; b's size there %u\n",
                       r, s1c_root_writes, arefs, maxsize);
    check(r == 0 || arefs <= 1, "B20c (S1c): a mv that failed leaves one name for a's chain on the card once syncs have run");
    check(r == 0 || maxsize <= 512, "B20c (S1c): b's entry on the card covers no bytes that were not written");
    x->bad_hi = 0;
    check(fsync_sb(sb) == 0 && x->violations == 0, "B20c: once X takes writes, sync");
    census(x, &used, &reached);
    check(used == reached, "B20c: no cluster lost");
}

/**** B21a (S2): rm of a file whose refused blocks are more than B16's 16 ****/

static void s2(u32 nblocks) {
    struct ram *x = ram_new("b21a", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    u32 i, used, reached;
    int r, sy;
    char title[100];
    snprintf(title, sizeof(title), "B21a (S2): rm of a file with %u refused blocks", nblocks);
    fresh(title);
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 512, 80);
    if(put_file(root, "keep", pat, 512) || fsync_sb(sb)) die("B21a: setup");
    x->bad_lo = clus_lba(&g, 3);
    x->bad_hi = clus_lba(&g, 3 + nblocks - 1);      /* the clusters the next file takes */
    if(root->ops->create(root, "f", VN_FILE, &v)) die("B21a: create");
    for(i = 0; i < nblocks; i++) {                  /* 512 bytes at a time: through the cache (FAT_DIRECT is 16 blocks) */
        fill(pat, 512, 81 + i);
        if(v->ops->write(v, i * 512, pat, 512) != 512) die("B21a: write");
    }
    vput(v);
    sy = fsync_sb(sb);
    r = root->ops->unlink(root, "f");
    if(verbose) printf("      sync %d; rm f = %d; the card's failed blocks after it %u\n", sy, r, x->bd.bc_failed);
    snprintf(title, sizeof(title), "B21a (S2): rm of the file whose %u blocks refuse works", nblocks);
    check(sy == -EIO && r == 0 && !lookup(root, "f"), title);
    snprintf(title, sizeof(title), "B21a: and a sync then (its %u blocks dropped, the card still refusing them)", nblocks);
    check(fsync_sb(sb) == 0 && x->bd.bc_failed == 0, title);
    x->bad_hi = 0;
    census(x, &used, &reached);
    check(used == reached && used == 1, "B21a: no cluster lost");
}

/**** B21b (S6): rm of a file whose chain runs back into itself (a damaged volume), its cluster holding 3 of the
      card's 16 failed blocks: B16's aside_failed() added them to its array of 16 at each visit, checking the
      count only between clusters (AddressSanitizer: a stack-buffer-overflow); and fat_free_chain() counted
      the looping cluster free twice ****/

static void b21b(void) {
    struct ram *x = ram_new("b21b", 40000);         /* FAT16, 4 blocks a cluster */
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    struct buf *b;
    struct statfs st;
    u8 *e;
    u32 i, fc = 7, fat_off, free0;
    int r, r2, there;
    fresh("B21b (S6): rm of a file whose chain loops, with failed blocks in the loop");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    if(g.spc != 4) die("B21b: geometry");
    fill(pat, 512, 120);
    if(put_file(root, "keep", pat, 512) || fsync_sb(sb)) die("B21b: setup");
    /* g: 4 clusters (16 blocks), f: 1 cluster (4 blocks), written 512 bytes at a time (the cache) */
    if(root->ops->create(root, "g", VN_FILE, &v)) die("B21b: g");
    for(i = 0; i < 16; i++) { fill(pat, 512, 121 + i); if(v->ops->write(v, i * 512, pat, 512) != 512) die("B21b: g write"); }
    vput(v);
    if(root->ops->create(root, "f", VN_FILE, &v)) die("B21b: f");
    for(i = 0; i < 4; i++) { fill(pat, 512, 140 + i); if(v->ops->write(v, i * 512, pat, 512) != 512) die("B21b: f write"); }
    vput(v);
    /* keep is cluster 2; g 3-6; f 7. The card refuses g's last 13 blocks and f's last 3: 16 failed blocks */
    x->bad_lo = clus_lba(&g, 3) + 3;
    x->bad_hi = clus_lba(&g, 7) + 3;
    if(fsync_sb(sb) != -EIO) die("B21b: the refused sync");
    if(verbose) printf("      failed blocks on the card's account: %u\n", x->bd.bc_failed);
    /* the volume is damaged: f's chain loops (cluster 7 -> 7), as a card from elsewhere may have it */
    fat_off = fc * 2;
    b = bread(&x->bd, g.fat_lba + fat_off / 512);
    if(!b) die("B21b: FAT");
    b->data[fat_off % 512] = fc & 0xFF;
    b->data[fat_off % 512 + 1] = fc >> 8;
    bdirty_order(b, BORDER_TABLE);
    brelse(b);
    r = root->ops->unlink(root, "f");               /* (g's refused blocks hold its order point back: EIO) */
    v = lookup(root, "f");
    if(verbose) printf("      rm f = %d; f is %s\n", r, v ? "there" : "not there");
    check((r == 0 && !v) || (r == -EIO && v), "B21b (S6): rm of the file whose chain loops says what it did (no crash)");
    there = v != NULL;
    if(v) vput(v);
    x->bad_hi = 0;                                  /* the card takes writes again */
    if(fsync_sb(sb) != 0) die("B21b: sync");
    r2 = there ? root->ops->unlink(root, "f") : -ENOENT;
    check(r2 == 0 && fsync_sb(sb) == 0, "B21b: once the card works, the rm works");
    if(root->ops->create(root, "h", VN_FILE, &v)) die("B21b: h");   /* a file of one cluster whose chain loops, */
    if(v->ops->write(v, 0, pat, 512) != 512 || fsync_sb(sb)) die("B21b: h write");   /* no failed block this time */
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "H          ");
    if(!e) die("B21b: h on the disk");
    vput(v);
    fc = g16(e + 26);
    fat_off = fc * 2;
    if(!(b = bread(&x->bd, g.fat_lba + fat_off / 512))) die("B21b: FAT");
    b->data[fat_off % 512] = fc & 0xFF;
    b->data[fat_off % 512 + 1] = fc >> 8;
    bdirty_order(b, BORDER_TABLE);
    brelse(b);
    if(fsync_sb(sb) != 0 || sb->ops->statfs(sb, &st) != 0) die("B21b: sync 2");
    free0 = st.free_units;
    r2 = root->ops->unlink(root, "h");
    if(sb->ops->statfs(sb, &st) != 0) die("B21b: statfs");
    if(verbose) printf("      rm of a file whose one cluster loops = %d; free clusters %u before, %u after\n", r2, free0, st.free_units);
    check(r2 == 0 && st.free_units == free0 + 1 && fsync_sb(sb) == 0, "B21b: the rm of a looping chain counts its cluster free once");
}

/**** B22 (S5): a card that fails in the middle of a multi-block write (as sdcard.c's sd_write does: CMD25, the
      blocks before the failing one are written) under a straight write over blocks the cache holds clean ****/

static u32 s5_bad;                                  /* a write touching this block stores the blocks before it, then fails */
static int s5_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct ram *r = (struct ram *)d->priv;
    if(s5_bad && lba <= s5_bad && s5_bad < lba + count) {
        memcpy(r->mem + lba * 512, buf, (s5_bad - lba) * 512);
        r->failed++;
        return BDEV_EIO;
    }
    return ram_write(d, lba, count, buf);
}
static const struct bdev_ops s5_ops = { ram_read, s5_write, NULL };

static void b22(void) {
    struct ram *x = ram_new("b22", 8192), *y = ram_new("b22y", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    static u8 back1[65536], back2[65536], newb[65536];
    u32 i, diff1 = 0, diff2 = 0, ondisk = 0;
    struct buf *b;
    int w;
    fresh("B22 (S5): a straight write the card fails half way through, over blocks the cache holds");
    mkfs(x);
    x->bd.ops = &s5_ops;
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 99);
    if(big_file(root, "f", pat, 65536) || fsync_sb(sb)) die("B22: setup");
    v = lookup(root, "f");
    for(i = 0; i < 65536; i += 4096) if(v->ops->read(v, i, back1 + i, 4096) != 4096) die("B22: read");   /* through the cache */
    s5_bad = clus_lba(&g, 2) + 64;                  /* the card takes the first 64 blocks of the request, then fails */
    fill(newb, 65536, 100);
    w = v->ops->write(v, 0, newb, 65536);
    s5_bad = 0;
    for(i = 0; i < 65536; i += 4096) if(v->ops->read(v, i, back1 + i, 4096) != 4096) die("B22: read 2");
    for(i = 0; i < 2100; i++) if((b = bread(&y->bd, i)) != NULL) brelse(b);   /* another task's reads make room */
    for(i = 0; i < 65536; i += 4096) if(v->ops->read(v, i, back2 + i, 4096) != 4096) die("B22: read 3");
    for(i = 0; i < 65536; i += 512) {
        diff1 += !!memcmp(back1 + i, pat + i, 512);
        diff2 += !!memcmp(back2 + i, pat + i, 512);
        ondisk += !memcmp(x->mem + (clus_lba(&g, 2) + i / 512) * 512, newb + i, 512);
    }
    if(verbose) printf("      write = %d; blocks reading differently from before it: %u right after, %u once the cache made room;"
                       " blocks of the new bytes on the card: %u\n", w, diff1, diff2, ondisk);
    check(w == -EIO && diff1 == diff2, "B22 (S5): after a refused write, f reads the same before and after the cache makes room");
    check(diff1 == ondisk && !memcmp(back1, x->mem + clus_lba(&g, 2) * 512, 65536),
          "B22: what it reads is what the card holds (the blocks the card stored before the one it refused)");
    vput(v);
    check(fsync_sb(sb) == 0, "B22: sync");
}

/**** B23 (S3): a write past the end of a file whose gap of zeros the card refuses part of ****/

static void b23(void) {
    struct ram *x = ram_new("b23", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    u32 used, reached, size_after, size_after2, size_t1;
    int r, t, w;
    fresh("B23 (S3): a write past the end of a file, the card refusing part of the gap");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 512, 90);
    if(put_file(root, "z", pat, 512) || fsync_sb(sb)) die("B23: setup");
    x->bad_lo = clus_lba(&g, 2 + 200);
    x->bad_hi = clus_lba(&g, 2 + 400);
    v = lookup(root, "z");
    r = v->ops->write(v, 300000, (const u8 *)"x", 1);
    size_after = v->size;
    t = v->ops->truncate(v, 300000);                /* a truncate that grows, likewise */
    size_t1 = v->size;
    fill(pat2, 65536, 91);
    w = v->ops->write(v, 200 * 512, pat2, 65536);   /* the gap is taken, the bytes after it are refused */
    size_after2 = v->size;
    if(verbose) printf("      write of 1 byte at 300000 = %d; the size now %u (was 512); truncate to 300000 = %d, size %u;"
                       " 64 KB at %u = %d, size %u\n", r, size_after, t, size_t1, 200 * 512, w, size_after2);
    vput(v);
    check(r == -EIO && size_after == 512, "B23 (S3): a write past the end that the card refuses part of says EIO, the size as it was");
    check(t == -EIO && size_t1 == 512 && w == -EIO && size_after2 == 512 && file_is(root, "z", pat, 512),
          "B23: so do a truncate that grows and a write whose gap was written but not its bytes; the file reads as it was");
    x->bad_hi = 0;
    check(fsync_sb(sb) == 0, "B23: sync");
    census(x, &used, &reached);
    check(used == reached && used == 1 && x->violations == 0, "B23: the chain is cut back: no cluster lost");
}

/**** B24 (S4): mv s/a t/a fails after its first order point (s's block unreadable), and the undo meets a second
      read error (t's block, written at the order point and since made room for) ****/

static struct ram *s4_y;
static u32 s4_slba, s4_tlba, s4_s_bad, s4_t_bad, s4_evicted, s4_armed;
static int s4_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    struct buf *b;
    u32 i;
    if(s4_armed && s4_s_bad && lba <= s4_slba && s4_slba < lba + count) {
        if(!s4_evicted) {                           /* meanwhile another task's reads make room: t's block (clean) goes */
            s4_evicted = 1;
            for(i = 0; i < 2100; i++) if((b = bread(&s4_y->bd, i)) != NULL) brelse(b);
            s4_t_bad = 1;                           /* and the card cannot read it either now */
        }
        return BDEV_EIO;
    }
    if(s4_armed && s4_t_bad && lba <= s4_tlba && s4_tlba < lba + count) return BDEV_EIO;
    return ram_read(d, lba, count, buf);
}
static const struct bdev_ops s4_ops = { s4_read, ram_write, NULL };

static void s4_hook(struct ram *r, u32 lba, u32 count) {
    struct buf *b;
    u32 i;
    if(!s4_armed || s4_s_bad || !(lba <= s4_tlba && s4_tlba < lba + count)) return;
    for(i = 0; i < 2100; i++) if((b = bread(&s4_y->bd, i)) != NULL) brelse(b);   /* s's block goes from the cache */
    s4_s_bad = 1;                                   /* and cannot be read from now on */
    (void)r;
}

static void b24(void) {
    struct ram *x = ram_new("b24", 8192), *y = ram_new("b24y", 8192);
    struct superblock *sb;
    struct vnode *root, *s = NULL, *t = NULL, *v, *w, *nv = NULL;
    struct geo g;
    u8 *e;
    char sl[128], tl[128];
    int r, sy, r1, r2, r3, r4;
    fresh("B24 (S4): mv s/a t/a: the old name cannot be deleted, and the new one cannot be taken back");
    mkfs(x);
    x->bd.ops = &s4_ops;
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    s4_y = y;
    if(root->ops->create(root, "s", VN_DIR, &s) || root->ops->create(root, "t", VN_DIR, &t)) die("B24: mkdir");
    fill(pat, 1024, 95);
    if(put_file(s, "a", pat, 1024) || fsync_sb(sb)) die("B24: setup");
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "S          "); if(!e) die("B24: s"); s4_slba = clus_lba(&g, g16(e + 26));
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "T          "); if(!e) die("B24: t"); s4_tlba = clus_lba(&g, g16(e + 26));
    s4_s_bad = s4_t_bad = s4_evicted = 0;
    s4_armed = 1;
    x->hook = s4_hook;
    r = root->ops->rename(s, "a", t, "a");
    x->hook = NULL;
    sy = fsync_sb(sb);
    s4_armed = 0;                                   /* the read errors were passing ones */
    v = lookup(t, "a");
    w = lookup(s, "a");
    names(s, sl, sizeof(sl));
    names(t, tl, sizeof(tl));
    if(verbose) printf("      mv s/a t/a = %d (s's block unreadable: %u, t's unreadable: %u); sync %d; s holds '%s', t holds '%s'\n",
                       r, s4_s_bad, s4_t_bad, sy, sl, tl);
    check(r == -EIO && (!v || !w || said("read-only until unmounted") == 1),
          "B24 (S4): a mv that fails changed nothing, or the volume says it could not be undone");
    r1 = t->ops->unlink(t, "a");                    /* either name gone would leave the other on freed clusters */
    r2 = s->ops->unlink(s, "a");
    r3 = root->ops->create(root, "n", VN_FILE, &nv);
    if(r3 == 0) vput(nv);
    r4 = root->ops->rename(root, "s", root, "s2");
    check(!v || !w || (r1 == -EROFS && r2 == -EROFS && r3 == -EROFS && r4 == -EROFS && file_is(s, "a", pat, 1024)),
          "B24: and then refuses every change (EROFS): no rm frees the chain under the other name");
    if(v) vput(v);
    if(w) vput(w);
    vput(s);
    vput(t);
    if(r == -EIO && said("read-only until unmounted")) {
        check(said("read-only until unmounted") == 1 && fsync_sb(sb) == 0, "B24: the console says so once; syncs go on");
        sb->ops->unmount(sb);
        check(said("stays marked as not cleanly unmounted") == 1 && (g16(x->mem + g.fat_lba * 512 + 2) & 0x8000) == 0,
              "B24: and the volume stays marked as not cleanly unmounted (to be checked before it is written)");
    }
}

/**** B25: trim_chain()'s window (the threaded test's seed 44): a write whose clusters past its end are cut off
      while the card refuses the order point; the link into them reached the card before (a table point), and
      the frees, in another FAT sector than the cut, were written ahead of it ****/

static u32 b25_lo;
static int b25_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct ram *r = (struct ram *)d->priv;
    if(b25_lo && lba <= b25_lo && b25_lo < lba + count) r->fail_all = 1;   /* the card stops taking writes here */
    return ram_write(d, lba, count, buf);
}
static const struct bdev_ops b25_ops = { ram_read, b25_write, NULL };

static void b25(void) {
    struct ram *x = ram_new("b25", 40000);          /* FAT16, 4 blocks a cluster: 32 clusters to a 64 KB request */
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    struct buf *b;
    u32 used, reached;
    u8 *e;
    int w, s1;
    fresh("B25: a write cut back while the card refuses: the cut and the frees in two FAT sectors");
    mkfs(x);
    x->bd.ops = &b25_ops;
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    if(g.spc != 4) die("B25: geometry");
    fill(pat, 65536, 130);
    if(big_file(root, "a", pat, 198 * 2048) || big_file(root, "f", pat, 65536) || big_file(root, "b", pat, 768 * 2048) ||
       fsync_sb(sb)) die("B25: setup");             /* a: clusters 2-199; f: 200-231 (FAT sector 0); b: 232-999 */
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "F          ");
    if(!e || g16(e + 26) != 200) die("B25: f's clusters");
    init_bcache();                                  /* (the volume is synced) the FAT sectors in the cache in this order: */
    if(!(b = bread(&x->bd, g.fat_lba + 3))) die("B25: FAT");   /* where f's next clusters go (1000-1023) */
    brelse(b);
    if(!(b = bread(&x->bd, g.fat_lba + 4))) die("B25: FAT");   /* (1024-1031) */
    brelse(b);
    if(!(b = bread(&x->bd, g.fat_lba + 0))) die("B25: FAT");   /* and the cut's (f ends at 231) */
    brelse(b);
    b25_lo = clus_lba(&g, 1000);                    /* the card refuses f's next 64 KB, and every write after that */
    v = lookup(root, "f");
    fill(pat2, 65536, 131);
    w = v->ops->write(v, 65536, pat2, 65536);       /* 1000-1031 (a table point at 1024 puts the link 231 -> 1000 */
    b25_lo = 0;                                     /* on the card), refused: cut back to 231 */
    x->fail_all = 0;
    x->oracle = 1;
    s1 = fsync_sb(sb);
    if(verbose) printf("      write = %d; sync once the card works %d; oracle: %s\n", w, s1, x->violations ? x->violation : "nothing");
    check(w == -EIO && v->size == 65536, "B25: the append refused: the size as it was");
    check(s1 == 0 && x->violations == 0,
          "B25: the cut reaches the card before the frees of the clusters past it (no chain on the card runs into a free cluster)");
    vput(v);
    census(x, &used, &reached);
    check(used == reached && used == 198 + 32 + 768, "B25: and the clusters past the cut are free once it is (no cluster lost)");
    check(file_is(root, "f", pat, 65536), "B25: f reads as it was");
}

/**** B26a: a sync's count of the requests a device refused (B13) is its own: making room in between (another
      task's write) started the device's count again, and a card that took nothing was asked for every block ****/

static int b26a_nested, b26a_armed;
static u32 b26a_sync_requests;
static int b26a_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    int r;
    if(!b26a_armed || b26a_nested) return ram_write(d, lba, count, buf);
    b26a_sync_requests++;                           /* a request of the sync */
    r = ram_write(d, lba, count, buf);
    if(r) {                                         /* refused: meanwhile another task makes room on the card */
        b26a_nested = 1;
        bcache_room(d);
        b26a_nested = 0;
    }
    return r;
}
static const struct bdev_ops b26a_ops = { ram_read, b26a_write, NULL };

static void b26a(void) {
    struct ram *x = ram_new("b26a", 2048);
    struct buf *b;
    u32 i;
    int r;
    fresh("B26a: a card that takes nothing, while making room asks it too");
    x->bd.ops = &b26a_ops;
    for(i = 0; i < 300; i++) {                      /* 300 dirty blocks, none next to another (one request each) */
        if(!(b = bread(&x->bd, 2 * i))) die("B26a: read");
        memset(b->data, 1 + (i & 0x7F), 512);
        bdirty(b);
        brelse(b);
    }
    x->fail_all = 1;
    b26a_sync_requests = 0;
    b26a_armed = 1;
    r = bsync(&x->bd);
    b26a_armed = 0;
    if(verbose) printf("      sync %d; its requests %u, of the card's %u refused\n", r, b26a_sync_requests, x->failed);
    check(r == -1 && b26a_sync_requests <= 6, "B26a: the sync asks a card that takes nothing at most a few times (B13's 4), room() or not");
    x->fail_all = 0;
    check(bsync(&x->bd) == 0 && x->mem[2 * 299 * 512] == 1 + (299 & 0x7F), "B26a: once it works, a sync writes them all");
}

/**** B26b: a block set aside (B16) is the debt of every other task's sync: one that began while an rm's order point
      had it aside said 0 although the rm then failed and put it back, not written ****/

static surf_task b26b_task;
static u32 b26b_dlba;
static int b26b_other, b26b_ran;
static void b26b_hook(struct ram *r, u32 lba, u32 count) {
    surf_task *was = curTask;
    if(b26b_ran || !(lba <= b26b_dlba && b26b_dlba < lba + count)) return;
    b26b_ran = 1;                                   /* the rm's order point writes d's block: another task syncs the */
    curTask = &b26b_task;                           /* data and tables (fat_table_point() for a file of its own) */
    b26b_other = bsync_order(&r->bd, BORDER_TABLE);
    curTask = was;
}

static void b26b(void) {
    struct ram *x = ram_new("b26b", 8192);
    struct superblock *sb;
    struct vnode *root, *d = NULL, *v;
    struct geo g;
    u8 *e;
    int r;
    fresh("B26b: another task's sync while an rm has its file's refused block set aside");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 512, 140);
    if(put_file(root, "f", pat, 512) || root->ops->create(root, "d", VN_DIR, &d) || fsync_sb(sb)) die("B26b: setup");
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "D          ");
    if(!e) die("B26b: d on the disk");
    b26b_dlba = clus_lba(&g, g16(e + 26));          /* f: cluster 2; d: 3 */
    x->bad_lo = g.root_lba;                         /* the root's blocks and f's refuse */
    x->bad_hi = clus_lba(&g, 2);
    v = lookup(root, "f");
    fill(pat, 512, 141);
    if(!v || v->ops->write(v, 0, pat, 512) != 512) die("B26b: write");
    vput(v);
    if(fsync_sb(sb) != -EIO) die("B26b: the refused sync");
    if(put_file(d, "g", NULL, 0)) die("B26b: a file in d");   /* d's block: the one write the rm's order point gets done */
    b26b_ran = 0;
    b26b_other = 99;
    x->hook = b26b_hook;
    r = root->ops->unlink(root, "f");               /* f's block aside; the root's refuses: EIO, the block put back */
    x->hook = NULL;
    if(verbose) printf("      rm f = %d; the other task's sync (it ran: %d) = %d\n", r, b26b_ran, b26b_other);
    check(r == -EIO && b26b_ran && b26b_other == -1,
          "B26b: a sync of another task does not say 0 while the block it owes is set aside (and then put back, not written)");
    check(said("set aside for 10 s by an operation that does not end") == 1, "B26b: (here the rm waits for that task: it gives up)");
    x->bad_hi = 0;
    check(fsync_sb(sb) == 0 && file_is(root, "f", pat, 512), "B26b: once the card works, a sync writes f's block");
    check(root->ops->unlink(root, "f") == 0 && fsync_sb(sb) == 0, "B26b: then the rm works");
    vput(d);
}

/**** the review of B20-B26 (round fs-fix-4): its scenarios R1-R6 (fsrev4-tests/review4.c, ported here under the
      audit item that fixes each: B27-B31, B22) and those of the fixes (B28c, B28d, B30b, B31b, B32) ****/

static u32 free_clusters(struct superblock *sb) {
    struct statfs st;
    if(sb->ops->statfs(sb, &st)) die("statfs");
    return st.free_units;
}

/**** B28a, B28b (R1a, R1b): a write past the end of a file on a full volume, one sector of its gap refusing. The gap
      of zeros goes through the cache (dirty, never tried), the write then gets ENOSPC (nothing written), B23 takes
      the size back and the chain is cut back; B25 dropped the failed blocks of the cut-off part before the order
      point, and a gap block that failed IN it stayed, its chain pending: every later order point and sync owed it,
      EIO for as long as it refused, and no rm could help (the block was in no file's chain). b: f empty, the whole
      chain freed at once with its untried blocks, one of which then failed in the next sync. d (B28's own): a, with no
      memory for the list of blocks to drop while the write runs (as for a block a task holds or writes then: not
      dropped), so the refusing block stays dirty in the chain that waits: the next order point or sync drops it ****/

static void b28(int variant) {
    static const char *const name[] = { "b28a", "b28b", "b28d" }, *const what[] = {
        "B28a (R1a): 1 byte past the end of a 512-byte file on a full volume; one sector of the gap refuses",
        "B28b (R1b): 1 byte past the end of an empty file on a full volume; one sector of the gap refuses",
        "B28d: B28a with no memory for the list of blocks to drop while the write runs" };
    struct ram *x = ram_new(name[variant], 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    u32 nfree, size, off, badlba, used, reached;
    int w, s1, rmf, rmfill, tr, s2, s3, rmf2, empty = variant == 1;
    char m1[160], m2[160], m3[160];
    fresh(what[variant]);
    snprintf(m1, sizeof(m1), "%s: the write gets ENOSPC, the size as it was", variant == 2 ? "B28d" : empty ? "B28b" : "B28a");
    snprintf(m2, sizeof(m2), "%s: the gap's blocks, nobody's, hold nothing back while their sector refuses: sync, rm, truncate",
             variant == 2 ? "B28d (the waiting chain's)" : empty ? "B28b (R1b)" : "B28a (R1a)");
    snprintf(m3, sizeof(m3), "%s: once the card works, nothing lost", variant == 2 ? "B28d" : empty ? "B28b" : "B28a");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 7);
    if(put_file(root, "f", pat, empty ? 0 : 512)) die("B28: f");              /* f: cluster 2 (a, d) */
    if(put_file(root, "h", pat, 8 * 512)) die("B28: h");                      /* h: the next 8 clusters */
    nfree = free_clusters(sb);
    if(big_file(root, "fill", pat, nfree * 512) || fsync_sb(sb)) die("B28: fill");   /* the rest of the volume */
    if(root->ops->unlink(root, "h") || fsync_sb(sb)) die("B28: rm h");
    badlba = clus_lba(&g, empty ? 5 : 6);                                     /* one block in the middle of them */
    x->bad_lo = x->bad_hi = badlba;                                           /* refuses every write, from now on */
    v = lookup(root, "f");
    if(!v) die("B28: lookup");
    off = (empty ? 0 : 512) + 8 * 512;                                        /* the gap takes all 8 free clusters */
    kalloc_fail = variant == 2;
    w = v->ops->write(v, off, (const u8 *)"x", 1);
    kalloc_fail = 0;
    size = v->size;
    vput(v);
    s1 = fsync_sb(sb);
    rmf = root->ops->unlink(root, "f");
    v = lookup(root, "fill");
    tr = v ? v->ops->truncate(v, 512) : -99;
    if(v) vput(v);
    rmfill = root->ops->unlink(root, "fill");
    s2 = fsync_sb(sb);
    if(verbose) printf("      write of 1 byte at %u = %d, size %u; while block %u refuses: sync %d, rm f %d, truncate fill %d,"
                       " rm fill %d, sync %d\n", off, w, size, badlba, s1, rmf, tr, rmfill, s2);
    check(w == -ENOSPC && size == (empty ? 0 : 512), m1);
    check(s1 == 0 && rmf == 0 && tr == 0 && rmfill == 0 && s2 == 0, m2);
    x->bad_hi = 0;
    s3 = fsync_sb(sb);
    rmf2 = rmf == 0 ? 0 : root->ops->unlink(root, "f");
    if(rmfill != 0) root->ops->unlink(root, "fill");
    fsync_sb(sb);
    census(x, &used, &reached);
    check(s3 == 0 && rmf2 == 0 && used == reached && used == 0 && !x->violations, m3);
}

/**** B28c: rm of a file whose block, written and never tried, refuses: the order point that takes its entry away
      wrote it (B16 set aside only blocks that had failed), it refused, and the rm said EIO once ****/

static void b28c(void) {
    struct ram *x = ram_new("b28c", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    u32 used, reached;
    int r, s;
    fresh("B28c: rm of a file whose block, never tried, refuses");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 1024, 8);
    if(put_file(root, "keep", pat, 512) || put_file(root, "f", pat, 1024) || fsync_sb(sb)) die("B28c: setup");   /* f: 3-4 */
    x->bad_lo = x->bad_hi = clus_lba(&g, 4);
    v = lookup(root, "f");
    fill(pat2, 512, 9);
    if(!v || v->ops->write(v, 512, pat2, 512) != 512) die("B28c: write");    /* cluster 4's block dirty, never tried */
    vput(v);
    r = root->ops->unlink(root, "f");
    s = fsync_sb(sb);
    x->bad_hi = 0;
    census(x, &used, &reached);
    if(verbose) printf("      rm f %d; sync %d (the block refusing); clusters used %u reached %u\n", r, s, used, reached);
    check(r == 0 && s == 0 && !lookup(root, "f") && used == reached && used == 1,
          "B28c: the rm works at once: its order point neither writes nor waits for the file's own blocks");
}

/**** B29 (R2): a truncate to a smaller size whose update_entry() cannot read the directory block (evicted, then a
      passing read error): update_entry() changed nothing, yet entry_put() of the undo failed to read as well, and
      B24 made the volume read-only until unmounted for an undo that had nothing to undo ****/

static struct ram *b29_y;
static u32 b29_fat2, b29_dirlba, b29_armed, b29_done;
static int b29_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    struct ram *r = (struct ram *)d->priv;
    struct buf *b;
    u32 i;
    if(b29_armed && !b29_done && lba <= b29_fat2 && b29_fat2 < lba + count) {
        b29_done = 1;                                /* fat_set() reads the second FAT: meanwhile another task's reads */
        for(i = 0; i < 2100; i++) if((b = bread(&b29_y->bd, i)) != NULL) brelse(b);   /* make room (the root's block, */
        r->rbad = b29_dirlba;                        /* clean, goes), and the card cannot read it for a moment */
    }
    return ram_read(d, lba, count, buf);
}
static const struct bdev_ops b29_ops = { b29_read, ram_write, NULL };

static void b29(void) {
    struct ram *x = ram_new("b29", 8192), *y = ram_new("b29y", 8192);
    struct superblock *sb;
    struct vnode *root, *v, *nv = NULL;
    struct geo g;
    u8 *e;
    int t, c, rm, s;
    fresh("B29 (R2): truncate to a smaller size; update_entry() cannot read the directory block (a passing read error)");
    mkfs(x);
    x->bd.ops = &b29_ops;
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    b29_y = y;
    fill(pat, 1536, 21);
    if(put_file(root, "t", pat, 1536) || put_file(root, "u", pat, 512) || fsync_sb(sb)) die("B29: setup");
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "T          ");
    if(!e || g16(e + 26) != 2) die("B29: t's clusters");
    b29_dirlba = g.root_lba;
    b29_fat2 = g.fat_lba + g.spf;                    /* the second FAT's first sector (clusters 0-255) */
    init_bcache();                                   /* (synced) nothing cached */
    v = lookup(root, "t");
    if(!v) die("B29: lookup");
    b29_done = 0;
    b29_armed = 1;
    t = v->ops->truncate(v, 600);                    /* cut after cluster 3 */
    b29_armed = 0;
    x->rbad = 0;                                     /* the read error passed */
    vput(v);
    c = root->ops->create(root, "n", VN_FILE, &nv);
    if(c == 0) vput(nv);
    rm = root->ops->unlink(root, "u");
    s = fsync_sb(sb);
    if(verbose) printf("      truncate %d; then create %d, rm u %d, sync %d; \"read-only\" said %d\n", t, c, rm, s,
                       said("read-only until unmounted"));
    check(t == -EIO && file_is(root, "t", pat, 1536), "B29: the truncate that failed changed nothing");
    check(c == 0 && rm == 0 && s == 0 && !said("read-only until unmounted"),
          "B29 (R2): and leaves the volume writable: there was no change to undo (update_entry() changed nothing)");
}

/**** B27 (R4): a truncate to a smaller, non-zero size whose entry_get() cannot read the directory block (one passing
      read error): raw[] was never filled, and the undo's entry_put() wrote those 32 bytes of the stack over the
      file's directory entry once the block read again (the file renamed to junk, its clusters lost; zeros there
      made it the directory's end, and every name after it went) ****/

static u32 b27_lba, b27_armed, b27_failed;
static int b27_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    if(b27_armed && !b27_failed && lba <= b27_lba && b27_lba < lba + count) { b27_failed = 1; return BDEV_EIO; }   /* once */
    return ram_read(d, lba, count, buf);
}
static const struct bdev_ops b27_ops = { b27_read, ram_write, NULL };

/* what an earlier call left on the stack below this frame: here (paint >= 0) a byte the scenario chooses */
static void __attribute__((noinline)) b27_paint(int paint) {
    volatile u8 junk[16384];
    u32 i;
    for(i = 0; i < sizeof(junk); i++) junk[i] = (u8)paint;
}

static int __attribute__((noinline)) b27_truncate(struct vnode *v, u32 size, int paint) {
    if(paint >= 0) b27_paint(paint);
    return v->ops->truncate(v, size);
}

static void b27(int paint) {
    struct ram *x = ram_new("b27", 8192), *y = ram_new("b27y", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    struct buf *b;
    u8 before[32], *e;
    char ls[256], title[160];
    u32 i, used, reached, n0, n1;
    int t, s;
    snprintf(title, sizeof(title), "B27 (R4): truncate 1536 -> 600 bytes; the directory block cannot be read once (entry_get());"
             " the stack %s", paint < 0 ? "as it is" : paint ? "painted 0x41" : "painted 0x00");
    fresh(title);
    mkfs(x);
    x->bd.ops = &b27_ops;
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 1536, 41);
    if(put_file(root, "t", pat, 1536) || put_file(root, "u", pat, 512) || put_file(root, "w", pat, 512) || fsync_sb(sb))
        die("B27: setup");
    n0 = names(root, ls, sizeof(ls));
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "T          ");
    if(!e) die("B27: t");
    memcpy(before, e, 32);
    v = lookup(root, "t");
    if(!v) die("B27: lookup");
    for(i = 0; i < 2100; i++) if((b = bread(&y->bd, i)) != NULL) brelse(b);   /* another task's reads: the root's block goes */
    b27_lba = g.root_lba;
    b27_failed = 0;
    b27_armed = 1;
    t = b27_truncate(v, 600, paint);
    b27_armed = 0;
    vput(v);
    s = fsync_sb(sb);
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "T          ");
    n1 = names(root, ls, sizeof(ls));
    census(x, &used, &reached);
    if(verbose) printf("      truncate %d (the read error seen: %u), sync %d; t's entry on the card %s; the root lists %u"
                       " names (was %u): '%s'; clusters used %u reached %u\n", t, b27_failed, s,
                       e && !memcmp(e, before, 32) ? "as it was" : "CHANGED", n1, n0, ls, used, reached);
    snprintf(title, sizeof(title), "B27 (R4, the stack %s): a truncate that failed at entry_get() leaves t's entry as it was",
             paint < 0 ? "as it is" : paint ? "painted 0x41" : "painted 0x00");
    check(t == -EIO && b27_failed && s == 0 && e && !memcmp(e, before, 32) && n1 == n0 && used == reached &&
          file_is(root, "t", pat, 1536), title);
}

/**** B30 (R5): mkdir whose vnode cannot be made (ENOMEM) after its entry was written; meanwhile another task's sync
      put that entry on the card. The undo put the slot back and freed the new directory's cluster with no order
      point between: the next sync wrote the FAT (table order) before the directory block (entry order), and the
      card named a free cluster in between ****/

static struct ram *b30_x;
static int b30_synced;
static void b30_hook(void) {                        /* the creating task is preempted: another task syncs */
    surf_task other, *was = curTask;
    curTask = &other;
    b30_synced = bsync(&b30_x->bd);
    curTask = was;
}

static void b30(void) {
    struct ram *x = ram_new("b30", 8192);
    struct superblock *sb;
    struct vnode *root, *d = NULL;
    struct geo g;
    u8 *e;
    u32 used, reached;
    int r, s, on_card;
    fresh("B30 (R5): mkdir d fails with ENOMEM after its entry was written; another task's sync wrote that entry meanwhile");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 512, 51);
    if(put_file(root, "f", pat, 512) || fsync_sb(sb)) die("B30: setup");
    b30_x = x;
    b30_synced = 99;
    x->oracle = 1;
    kcalloc_hook = b30_hook;
    kcalloc_fail = 1;                                /* fat_vnode()'s node: ENOMEM, after the sync above */
    r = root->ops->create(root, "d", VN_DIR, &d);
    kcalloc_fail = 0;
    kcalloc_hook = NULL;
    on_card = disk_entry(x, g.root_lba, g.root_secs * 16, "D          ") != NULL;
    s = fsync_sb(sb);
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "D          ");
    census(x, &used, &reached);
    if(verbose) printf("      mkdir d %d (the other task's sync %d; d on the card then: %d); sync %d; oracle: %s\n", r,
                       b30_synced, on_card, s, x->violations ? x->violation : "nothing");
    check(r == -ENOMEM && b30_synced == 0 && on_card, "B30: the mkdir says ENOMEM, the other task's sync having put d on the card");
    check(s == 0 && !e && x->violations == 0 && used == reached && used == 1,
          "B30 (R5): the next sync takes d off the card before its cluster is freed (no name on the card for a free cluster)");
    x->oracle = 0;
}

/**** B30b: a mkdir that fails before anything of its name was written (the root directory is full) gives the new
      directory's cluster back at once, as before B30: nothing on the card can name it ****/

static void b30b(void) {
    struct ram *x = ram_new("b30b", 8192);
    struct superblock *sb;
    struct vnode *root, *v, *d = NULL;
    u32 i, free0, free1, used, reached;
    int r = 0, s;
    char nm[16];
    fresh("B30b: mkdir d in a full root directory (ENOSPC before any of its name is written)");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    for(i = 0; i < 4096 && r == 0; i++) {                /* the root directory fills up (FAT16: a fixed size) */
        snprintf(nm, sizeof(nm), "F%u", i);
        if((r = root->ops->create(root, nm, VN_FILE, &v)) == 0) vput(v);
    }
    if(r != -ENOSPC || fsync_sb(sb)) die("B30b: filling the root directory");
    free0 = free_clusters(sb);
    r = root->ops->create(root, "D", VN_DIR, &d);
    free1 = free_clusters(sb);
    s = fsync_sb(sb);
    census(x, &used, &reached);
    if(verbose) printf("      %u files; mkdir D %d; free clusters %u before, %u right after; sync %d; used %u reached %u\n",
                       i - 1, r, free0, free1, s, used, reached);
    check(r == -ENOSPC && free1 == free0 && s == 0 && used == reached && used == 0,
          "B30b: a mkdir that wrote nothing of its name gives its cluster back at once");
}

/**** B31a (R3): nine write cut-backs while the card refuses every write: B25 kept eight chains to free later and lost
      the ninth (its clusters allocated with nothing pointing at them) without a word ****/

static void b31a(void) {
    struct ram *x = ram_new("b31a", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    u32 used, reached, i, free0, free1;
    int w, s, bad = 0;
    fresh("B31a (R3): nine 8 KB appends refused while the card refuses (each cut back while the order point is refused)");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    fill(pat, 65536, 31);
    if(put_file(root, "f", pat, 512) || fsync_sb(sb)) die("B31a: setup");
    free0 = free_clusters(sb);
    v = lookup(root, "f");
    x->fail_all = 1;
    for(i = 0; i < 9; i++) if((w = v->ops->write(v, 512, pat, 8192)) != -EIO) bad++;   /* 16 clusters each, refused */
    x->fail_all = 0;
    vput(v);
    s = fsync_sb(sb);
    free1 = free_clusters(sb);
    census(x, &used, &reached);
    if(verbose) printf("      sync once the card works %d; free clusters %u before, %u after; on the card used %u, reached %u\n",
                       s, free0, free1, used, reached);
    check(!bad && s == 0 && free1 == free0 && used == reached && x->violations == 0,
          "B31a (R3): every chain cut back while the card refused is freed once it works (nine of them)");
}

/**** B31b: more chains cut back than can wait (PENDING_MAX): the one with no place stays allocated with nothing
      pointing at it until the volume is checked, and the console says so, then and at unmount ****/

static void b31b(void) {
    struct ram *x = ram_new("b31b", 8192);
    struct superblock *sb;
    struct vnode *root, *v, *d = NULL;
    struct geo g;
    u32 used, reached, i;
    int s, said1;
    fresh("B31b: 65 write cut-backs while a directory block refuses (only order points fail: the table points work)");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 32);
    if(put_file(root, "f", pat, 512) || root->ops->create(root, "d", VN_DIR, &d) || fsync_sb(sb)) die("B31b: setup");
    x->bad_lo = clus_lba(&g, 3);                     /* d's block (cluster 3) and every cluster after it refuse */
    x->bad_hi = g.data_lba + g.nclusters - 1;
    if(put_file(d, "g", NULL, 0)) die("B31b: a file in d");   /* d's block dirty: every full sync owes it, refused */
    vput(d);
    if(fsync_sb(sb) != -EIO) die("B31b: the refused sync");
    v = lookup(root, "f");
    for(i = 0; i < 65; i++) if(v->ops->write(v, 512, pat, 8192) != -EIO) die("B31b: an append not refused");
    vput(v);
    said1 = said("cannot wait to be freed");
    x->bad_hi = 0;                                   /* the card works again */
    s = fsync_sb(sb);
    census(x, &used, &reached);
    if(verbose) printf("      said when it happened: %d; sync once the card works %d; on the card used %u, reached %u\n",
                       said1, s, used, reached);
    check(said1 == 1 && s == 0 && used - reached == 16 && x->violations == 0,
          "B31b: of 65 chains given back while the card refused, the one with no place left is said lost (16 clusters), the rest freed");
    sb->ops->unmount(sb);
    check(said("stay allocated with nothing pointing at them") == 1, "B31b: and the unmount says so again");
}

/**** B32: the blocks an rm sets aside stay so only for its order point. B16 kept them set aside until the chain was
      freed; another task's sync owes them (B26) and waits for them, 10 s at most: a long chain's free outlasts it ****/

static struct ram *b32_x;
static u32 b32_lba, b32_armed;
static int b32_other;
static int b32_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    if(b32_armed && lba <= b32_lba && b32_lba < lba + count) {   /* the free reaches the second FAT's ninth sector: */
        surf_task other, *was = curTask;             /* another task syncs meanwhile */
        b32_armed = 0;
        curTask = &other;
        b32_other = bsync(&b32_x->bd);
        curTask = was;
    }
    return ram_read(d, lba, count, buf);
}
static const struct bdev_ops b32_ops = { b32_read, ram_write, NULL };

static void b32(void) {
    struct ram *x = ram_new("b32", 8192), *y = ram_new("b32y", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    struct buf *b;
    u32 i, used, reached;
    int r;
    fresh("B32: another task's sync while an rm frees a long chain whose last block refuses");
    mkfs(x);
    x->bd.ops = &b32_ops;
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 61);
    if(big_file(root, "f", pat, 2100 * 512) || fsync_sb(sb)) die("B32: setup");  /* f: clusters 2-2101 (nine FAT sectors) */
    x->bad_lo = x->bad_hi = clus_lba(&g, 2101);      /* f's last block refuses */
    v = lookup(root, "f");
    fill(pat2, 512, 62);
    if(!v || v->ops->write(v, 2099 * 512, pat2, 512) != 512) die("B32: write");
    vput(v);
    if(fsync_sb(sb) != -EIO) die("B32: the refused sync");
    for(i = 0; i < 2100; i++) if((b = bread(&y->bd, i)) != NULL) brelse(b);   /* the FAT goes from the cache */
    b32_x = x;
    b32_lba = g.fat_lba + g.spf + 8;                 /* (only the free reads the second FAT: fat_set()) */
    b32_other = 99;
    b32_armed = 1;
    r = root->ops->unlink(root, "f");
    b32_armed = 0;
    x->bad_hi = 0;
    if(verbose) printf("      rm f %d; the other task's sync, during the free %d\n", r, b32_other);
    check(r == 0 && b32_other == 0 && !said("by an operation that does not end"),
          "B32: another task's sync during the free does not wait for the rm's blocks (set aside for its order point only)");
    r = fsync_sb(sb);
    census(x, &used, &reached);
    check(r == 0 && used == 0 && reached == 0, "B32: sync; nothing lost");
}

/**** B33: a sync writes a directory block from a copy (B12: another task keeps changing it and what it needs); a
      block the copy needs written first is held by that task, which then lets go of it and sets it aside (its rm's
      order point, B28), and that order point waits for the copy to end (it owes the directory block). The copy
      waited for the block set aside as for a hold (B26), each waited for the other, and after 10 s the copy was
      given up: the sync failed, "set aside for 10 s by an operation that does not end" ****/

#define B33_DATA 3
static struct ram *b33_x;
static struct buf *b33_y;                            /* the directory block's buffer (dirty throughout: it stays) */
static u32 b33_d[B33_DATA], b33_ylba;
static int b33_state;                                /* 0 the sync's rounds, 1 the other task holds a block the copy
                                                        needs, 2 it set it aside, 3 its operation ended */
static struct buf *b33_hold;
static surf_task b33_w1, b33_w2;
static void b33_touch(u32 lba, u8 order, u32 v) {
    struct buf *b = bread(&b33_x->bd, lba);
    if(!b) die("B33: read");
    b->data[0] = (u8)v;
    bdirty_order(b, order);
    brelse(b);
}
static int b33_in(u32 blk, u32 lba, u32 count) { return lba <= blk && blk < lba + count; }
static int b33_dirty(u32 lba) {
    struct buf *b = bpeek(&b33_x->bd, lba);
    int d = b && b->dirty;
    if(b) brelse(b);
    return d;
}
static void b33_hook(struct ram *r, u32 lba, u32 count) {   /* after each write the disk stores: the other task runs */
    surf_task *was = curTask;
    int i;
    (void)r;
    if(b33_in(b33_ylba, lba, count)) {               /* (the directory block itself being written) */
        if(b33_state == 2 && !b33_y->copying) goto end;
        return;
    }
    curTask = &b33_w2;
    if(b33_state == 0 && !b33_y->copying) {          /* it changes the data blocks the sync is not writing, then the
                                                        directory block (which then needs them) */
        for(i = 0; i < B33_DATA; i++) if(!b33_in(b33_d[i], lba, count)) b33_touch(b33_d[i], BORDER_DATA, lba + i);
        b33_touch(b33_ylba, BORDER_DIR, lba);
    } else if(b33_state == 0) {                      /* the copy writes the first block it needs: the other task takes
                                                        hold of another (dirty: one the copy needs too) */
        for(i = 0; i < B33_DATA; i++) if(!b33_in(b33_d[i], lba, count) && b33_dirty(b33_d[i])) break;
        if(i < B33_DATA) { b33_hold = bread(&b33_x->bd, b33_d[i]); b33_state = 1; }
    }
    curTask = was;
    return;
end:
    curTask = &b33_w2;                               /* its order point: the copy let go of the block, its operation */
    for(i = 0; i < B33_DATA; i++) bdiscard(&b33_x->bd, b33_d[i]);   /* ends (the block set aside is dropped) */
    b33_state = 3;
    curTask = was;
}
static void b33_yield(void) {                        /* the sync sleeps in the cache: the other task runs */
    surf_task *was = curTask;
    int i;
    curTask = &b33_w2;
    if(b33_state == 1) {                             /* it lets go of the block and sets it aside (its rm, B28) */
        u32 lba = b33_hold->lba;
        brelse(b33_hold);
        baside(&b33_x->bd, lba, true);
        b33_state = 2;
    } else if(b33_state == 2 && !b33_y->copying) {   /* its order point waits for the copy to end; then it ends */
        for(i = 0; i < B33_DATA; i++) bdiscard(&b33_x->bd, b33_d[i]);
        b33_state = 3;
    }
    curTask = was;
}

static void b33(void) {
    struct ram *x = ram_new("b33", 2048);
    int r, i, said_end;
    fresh("B33: a sync writes a block from a copy; another task sets aside a block the copy needs, and waits for the copy");
    b33_x = x;
    for(i = 0; i < B33_DATA; i++) b33_d[i] = 100 + 200 * i;   /* data blocks apart (never in one request), and the */
    b33_ylba = 900;                                  /* directory block that needs them (order: data, then entries) */
    for(i = 0; i < B33_DATA; i++) b33_touch(b33_d[i], BORDER_DATA, 1);
    b33_touch(b33_ylba, BORDER_DIR, 1);
    b33_y = bpeek(&x->bd, b33_ylba);
    if(!b33_y) die("B33: the directory block");
    brelse(b33_y);
    b33_state = 0;
    x->hook = b33_hook;
    yield_hook = b33_yield;
    curTask = &b33_w1;                               /* (a task: the cache's sleeps reach yield()) */
    r = bsync(&x->bd);
    curTask = NULL;
    yield_hook = NULL;
    x->hook = NULL;
    said_end = said("by an operation that does not end");
    if(verbose) printf("      the sync %d; the other task: %s; \"does not end\" said %d\n", r,
                       b33_state == 3 ? "its operation ended" : b33_state == 2 ? "still waits for the copy" :
                       b33_state == 1 ? "never set the block aside" : "never held a block the copy needs", said_end);
    check(b33_state >= 2, "B33: the scenario took place (the copy needed a block the other task set aside)");
    check(r == 0 && b33_state == 3 && !said_end,
          "B33: the copy lets the block go: the other task's operation ends, and the sync says 0 at once");
    curTask = &b33_w2;                               /* (what is left: the other task's blocks dropped, then a sync) */
    for(i = 0; i < B33_DATA; i++) bdiscard(&x->bd, b33_d[i]);
    curTask = NULL;
    bsync(&x->bd);
}

/**** the review of B27-B33 (round fs-fix-5): its scenarios P1-P3 (fsrev5-tests/review5.c, ported here under the
      audit item that fixes each: B36, B34, B38) and those of the fixes (B36b, B37) ****/

/**** B34a, B34b (P2, P2b): a sync writes a directory block Y from a copy (B12); the copy was taken while Y held the
      entry of file F, whose data block D has a change older than the copy (one of the copy's dependencies). While the
      copy waits for another dependency (a), or writes it (b), another task's rm of F deletes the entry in Y (in the
      cache) and sets D aside (B28). D left the lists: the copy never looked for it again (B33 restarted only for the
      dependency it was waiting for), reached the disk naming F, and D's older change never did: the card named F over
      whatever D held. Checked at every write of Y the disk stores, copy or not ****/

#define B34_T  50                                    /* a data block the sync owes and writes first */
#define B34_D1 100                                   /* another file's data block, in its writer's hands for a while */
#define B34_D  300                                   /* F's data block: in F's writer's hands, then set aside by the rm */
#define B34_Y  900                                   /* the directory block with F's entry ('E'; 0xE5 once deleted) */
static struct ram *b34_x;
static struct buf *b34_y, *b34_h1, *b34_hd;
static int b34_mode, b34_state, b34_ywrites, b34_named_stale, b34_copies, b34_touches, b34_waited_d1;
static surf_task b34_w1, b34_w2;
static int b34_in(u32 blk, u32 lba, u32 count) { return lba <= blk && blk < lba + count; }
static void b34_touch(u32 lba, u8 order, u8 v) {
    struct buf *b = bread(&b34_x->bd, lba);
    if(!b) die("B34: read");
    b->data[0] = v;
    b->data[1] = (u8)++b34_touches;
    bdirty_order(b, order);
    brelse(b);
}
static void b34_rm(void) {                           /* the rm of F: its entry deleted in Y, then D set aside (B28) */
    struct buf *b = bread(&b34_x->bd, B34_Y);
    if(!b) die("B34: read Y");
    b->data[0] = 0xE5;
    bdirty_order(b, BORDER_DIR);
    brelse(b);
    baside(&b34_x->bd, B34_D, true);
    b34_state = 1;
}
static void b34_hook(struct ram *r, u32 lba, u32 count) {   /* after each write the disk stores: the other task runs */
    surf_task *was = curTask;
    if(b34_in(B34_Y, lba, count)) {                  /* Y on the disk now: does it name F over a block F never wrote? */
        b34_ywrites++;
        if(b34_y->copying) b34_copies++;
        if(r->mem[B34_Y * 512] == 'E' && r->mem[B34_D * 512] != 0xDD) b34_named_stale++;
        return;
    }
    if(b34_mode == 1 && b34_state == 0 && b34_y->copying && b34_in(B34_D1, lba, count) && b34_hd) {
        curTask = &b34_w2;                           /* (b) the copy writes D1: F's writer lets go, the rm runs */
        brelse(b34_hd);
        b34_hd = NULL;
        b34_rm();
        curTask = was;
        return;
    }
    if(b34_state == 0 && b34_in(B34_T, lba, count) && !b34_h1) {
        curTask = &b34_w2;                           /* two writers: another file's block, then F's; then F's entry */
        b34_h1 = bread(&r->bd, B34_D1);              /* (each still in its writer's hands: not written meanwhile) */
        b34_hd = bread(&r->bd, B34_D);
        if(!b34_h1 || !b34_hd) die("B34: read D1, D");
        b34_h1->data[0] = 0x11; bdirty_order(b34_h1, BORDER_DATA);
        b34_hd->data[0] = 0xDD; bdirty_order(b34_hd, BORDER_DATA);
        b34_touch(B34_Y, BORDER_DIR, 'E');           /* Y needs both now: B12, copied after two rounds */
        curTask = was;
    }
}
static void b34_yield(void) {                        /* the sync sleeps in the cache: the other task runs */
    surf_task *was = curTask;
    if(b34_state != 0 || !b34_y->copying || !b34_h1) return;
    curTask = &b34_w2;
    if(b34_mode == 1) {                              /* (b) only D1's writer lets go: the copy writes D1 */
        brelse(b34_h1);
        b34_h1 = NULL;
        curTask = was;
        return;
    }
    b34_waited_d1 = 1;                               /* (a) the copy waits for D1: both writers let go, the rm runs */
    brelse(b34_h1);
    brelse(b34_hd);
    b34_h1 = b34_hd = NULL;
    b34_rm();
    curTask = was;
}

static void b34(int mode) {
    struct ram *x = ram_new(mode ? "b34b" : "b34a", 2048);
    int r;
    fresh(mode ? "B34b (P2b): a sync writes a directory block from a copy; another task's rm sets aside a block the copy"
                 " needs while the copy writes a different one"
               : "B34a (P2): a sync writes a directory block from a copy; another task's rm sets aside a block the copy"
                 " needs while the copy waits for a different one");
    b34_x = x;
    b34_mode = mode;
    b34_state = 0; b34_ywrites = b34_named_stale = b34_copies = b34_touches = b34_waited_d1 = 0;
    b34_h1 = b34_hd = NULL;
    b34_touch(B34_T, BORDER_DATA, 0x55);             /* owed: written first, and its write lets the writers in */
    b34_touch(B34_Y, BORDER_DIR, 'E');               /* owed: F's entry */
    b34_y = bpeek(&x->bd, B34_Y);
    if(!b34_y) die("B34: Y");
    brelse(b34_y);
    x->hook = b34_hook;
    yield_hook = b34_yield;
    curTask = &b34_w1;                               /* (a task: the cache's sleeps reach yield()) */
    r = bsync(&x->bd);
    curTask = NULL;
    yield_hook = NULL;
    x->hook = NULL;
    if(verbose) printf("      the sync %d; the rm ran during Y's copy %d (the copy waited for D1 %d); writes of Y %d, from"
                       " the copy %d; Y on the disk naming F over a block F never wrote: %d times\n", r, b34_state,
                       b34_waited_d1, b34_ywrites, b34_copies, b34_named_stale);
    check(b34_state == 1, mode ? "B34b: the scenario took place (the rm set D aside while Y's copy wrote D1)"
                               : "B34a: the scenario took place (the rm set D aside while Y's copy waited for D1)");
    check(b34_named_stale == 0 && r == 0,
          mode ? "B34b (P2b): Y never reaches the disk naming F while F's older data change has not (the copy is taken again)"
               : "B34a (P2): Y never reaches the disk naming F while F's older data change has not (the copy is taken again)");
    curTask = &b34_w2;                               /* the rm's order point and free: D dropped, Y written */
    if(b34_h1) brelse(b34_h1);
    if(b34_hd) brelse(b34_hd);
    bdiscard(&x->bd, B34_D);
    curTask = NULL;
    bsync(&x->bd);
}

/**** B36 (P1): a directory grows by a cluster (diter_extend() -> fat_alloc(prev, dir)): the new cluster's zeros are
      dirtied, then fat_table_point(); the card refuses one of them there, and fat_alloc() gives the cluster back with
      fat_set(c, 0), leaving its dirty, failed zero blocks: every later order point and sync owed a block of a cluster
      nobody has, and failed for as long as that sector refused (rm of an unrelated file, sync) ****/

static void b36(void) {
    struct ram *x = ram_new("b36", 8192);
    struct superblock *sb;
    struct vnode *root, *d;
    struct geo g;
    u8 *e;
    u32 dfirst, ext, i, per, used, reached;
    int cr, s1, rmg, s2, s3, rmg2, zdirty = -1;
    char name[16];
    struct buf *b;
    fresh("B36 (P1): a directory grows; the card refuses a sector of its new cluster: the cluster is given back");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 3);
    if(put_file(root, "g", pat, 2048)) die("B36: g");                   /* an unrelated file, with clusters */
    if(root->ops->create(root, "d", VN_DIR, &d)) die("B36: mkdir d");
    if(fsync_sb(sb)) die("B36: sync");
    e = disk_entry(x, g.root_lba, g.root_secs * 16, "D          ");
    if(!e) die("B36: d's entry");
    dfirst = g16(e + 26);
    ext = dfirst + 1;
    if(fat1(x, &g, ext) != 0) die("B36: the cluster after d's is not free");
    per = 16 * g.spc;                                                   /* d's cluster: "." ".." and these */
    for(i = 0; i < per - 2; i++) {
        snprintf(name, sizeof(name), "e%u", (unsigned)i);
        if(put_file(d, name, pat, 0)) die("B36: fill d");
    }
    if(fsync_sb(sb)) die("B36: sync 2");
    x->bad_lo = x->bad_hi = clus_lba(&g, ext);                          /* the cluster d grows into: one sector refuses */
    cr = put_file(d, "one.more", pat, 0);                               /* d needs a second cluster */
    b = bpeek(&x->bd, clus_lba(&g, ext));
    if(b) { zdirty = b->dirty; brelse(b); }
    s1 = fsync_sb(sb);
    rmg = root->ops->unlink(root, "g");                                 /* unrelated to d */
    s2 = fsync_sb(sb);
    if(verbose) printf("      create %d; the given-back cluster %u's block dirty in the cache %d; while it refuses: sync %d,"
                       " rm g %d, sync %d\n", cr, ext, zdirty, s1, rmg, s2);
    check(cr != 0, "B36: the create fails (the table point was refused)");
    check(zdirty != 1 && s1 == 0 && rmg == 0 && s2 == 0,
          "B36 (P1): a cluster fat_alloc() gives back leaves no block owed: later syncs and rm work while its sector refuses");
    x->bad_hi = 0;
    s3 = fsync_sb(sb);
    rmg2 = rmg == 0 ? 0 : root->ops->unlink(root, "g");
    fsync_sb(sb);
    census(x, &used, &reached);
    check(s3 == 0 && rmg2 == 0 && used == reached && !x->violations, "B36: once the card works, nothing lost");
    vput(d);
}

/**** B36b: B28d's write (1 byte past the end of a file on a full volume, one sector of its gap refusing) with no
      memory for the list of the device's dirty blocks: the gap's blocks are dropped at once all the same, each looked
      up (before, nothing was dropped until a later order point or sync could make the list) ****/

static void b36b(void) {
    struct ram *x = ram_new("b36b", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    struct geo g;
    struct buf *b;
    u32 nfree, badlba, used, reached;
    int w, dirty = -1, s;
    fresh("B36b: B28a's write with no memory for the list of the device's dirty blocks");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 7);
    if(put_file(root, "f", pat, 512) || put_file(root, "h", pat, 8 * 512)) die("B36b: f, h");
    nfree = free_clusters(sb);
    if(big_file(root, "fill", pat, nfree * 512) || fsync_sb(sb)) die("B36b: fill");
    if(root->ops->unlink(root, "h") || fsync_sb(sb)) die("B36b: rm h");
    badlba = clus_lba(&g, 6);
    x->bad_lo = x->bad_hi = badlba;
    v = lookup(root, "f");
    if(!v) die("B36b: lookup");
    kalloc_fail = 1;
    w = v->ops->write(v, 512 + 8 * 512, (const u8 *)"x", 1);
    kalloc_fail = 0;
    vput(v);
    b = bpeek(&x->bd, badlba);
    if(b) { dirty = b->dirty; brelse(b); }
    s = fsync_sb(sb);
    if(verbose) printf("      write %d; the gap's refusing block dirty in the cache right after it %d; sync %d\n", w, dirty, s);
    check(w == -ENOSPC && dirty != 1 && s == 0,
          "B36b: the gap's blocks are dropped at once with no list to look in (each block looked up)");
    x->bad_hi = 0;
    root->ops->unlink(root, "f");
    root->ops->unlink(root, "fill");
    fsync_sb(sb);
    census(x, &used, &reached);
    check(used == reached && used == 0 && !x->violations, "B36b: once the card works, nothing lost");
}

/**** B37, B35: a write past the end of a file on a volume with room, the card refusing its data and its gap;
      the gap's first block is in a reader's hands while the write is cut back, so it can be neither dropped nor
      written (the order point waits for the reader 10 s, then fails: the chain waits to be freed). The reader then
      lets go, and a folder is made: its table point owed the waiting chain's block, never dropped (B28 dropped the
      chains' blocks before order points and syncs only, and a block in someone's hands not at all), and the mkdir
      failed while that sector refused ****/

static void b37(void) {
    struct ram *x = ram_new("b37", 8192);
    struct superblock *sb;
    struct vnode *root, *v, *nd = NULL;
    struct geo g;
    struct buf *held, *b;
    u32 dlba, used, reached;
    int w, mk, s, dirty = -1;
    fresh("B37: a waiting chain's block in a reader's hands when it was given up; then a mkdir (a table point)");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 37);
    if(put_file(root, "f", pat, 512) || fsync_sb(sb)) die("B37: setup");           /* f: cluster 2 */
    dlba = clus_lba(&g, 3);                                                    /* the gap's first block (cluster 3) */
    if(!(held = bread(&x->bd, dlba))) die("B37: the reader");                  /* a reader holds it from now on */
    x->bad_lo = dlba;                                                          /* the gap and the data refuse */
    x->bad_hi = g.data_lba + g.nclusters - 1;
    v = lookup(root, "f");
    if(!v) die("B37: lookup");
    w = v->ops->write(v, 512 + 2 * 512, pat, 65536);    /* the gap's 2 blocks through the cache, the data straight */
    vput(v);
    brelse(held);                                                              /* the reader lets go */
    b = bpeek(&x->bd, dlba);
    if(b) { dirty = b->dirty; brelse(b); }
    x->bad_lo = x->bad_hi = dlba;                                              /* only that sector refuses now */
    mk = root->ops->create(root, "nd", VN_DIR, &nd);
    if(nd) vput(nd);
    s = fsync_sb(sb);
    if(verbose) printf("      write %d; the block dirty after the reader let go %d; mkdir %d; sync %d\n", w, dirty, mk, s);
    check(w < 0, "B37: the write fails (the card refuses its data)");
    check(dirty != 1, "B35: the waiting chain's block goes when its reader lets go (it was in its hands when given up)");
    check(mk == 0 && s == 0, "B37: a table point (mkdir) and a sync work while that block's sector refuses");
    x->bad_hi = 0;
    if(fsync_sb(sb)) die("B37: the sync once the card works");
    census(x, &used, &reached);
    check(used == reached && !x->violations, "B37: once the card works, nothing lost");
}

/**** B38 (P3): 65 cut-backs while only order points fail (B31b), then the card works and the volume is unmounted:
      the unmount said 16 clusters may stay allocated with nothing pointing at them ("check the volume"), then marked
      it cleanly unmounted (FAT[1]'s bit): the next mount, and any other computer, was told not to check it ****/

static void b38(void) {
    struct ram *x = ram_new("b38", 8192);
    struct superblock *sb;
    struct vnode *root, *v, *d = NULL;
    struct geo g;
    u32 used, reached, i, fat1v;
    int s, said_lost, said_mount;
    fresh("B38 (P3): 65 cut-backs while a directory block refuses, then the card works, then unmount: the clean bit");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 32);
    if(put_file(root, "f", pat, 512) || root->ops->create(root, "d", VN_DIR, &d) || fsync_sb(sb)) die("B38: setup");
    x->bad_lo = clus_lba(&g, 3);
    x->bad_hi = g.data_lba + g.nclusters - 1;
    if(put_file(d, "g", NULL, 0)) die("B38: a file in d");
    vput(d);
    if(fsync_sb(sb) != -EIO) die("B38: the refused sync");
    v = lookup(root, "f");
    for(i = 0; i < 65; i++) if(v->ops->write(v, 512, pat, 8192) != -EIO) die("B38: an append not refused");
    vput(v);
    x->bad_hi = 0;
    s = fsync_sb(sb);
    vput(sb->root);
    sb->ops->unmount(sb);
    census(x, &used, &reached);
    fat1v = fat1(x, &g, 1);
    said_lost = said("stay allocated with nothing pointing at them");
    klog_len = 0;
    klog[0] = 0;
    sb = mount(x);                                   /* the next mount: does it say so? */
    said_mount = said("was not unmounted cleanly");
    if(verbose) printf("      sync %d; the unmount said lost %d; used %u reached %u; FAT[1] %#x (clean bit %s); the next"
                       " mount warns %d\n", s, said_lost, used, reached, fat1v, (fat1v & 0x8000) ? "set" : "clear", said_mount);
    check(said_lost == 1 && used - reached == 16 && !(fat1v & 0x8000) && said_mount == 1,
          "B38 (P3): a volume with lost clusters stays marked as not cleanly unmounted, and the next mount says so");
    vput(sb->root);
    sb->ops->unmount(sb);
}

/**** B22b (R6): a clean copy a reader holds while a straight write over it is refused after the card stored it all:
      it leaves the table at once (a new read gets the card's bytes) and is dropped when let go; a dirty copy in the
      same request keeps its bytes and is written by the next sync (B22 holds: the review's check) ****/

static int b22b_store_fail;
static int b22b_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct ram *r = (struct ram *)d->priv;
    if(b22b_store_fail) { memcpy(r->mem + lba * 512, buf, count * 512); r->failed++; return BDEV_EIO; }   /* stored, refused */
    return ram_write(d, lba, count, buf);
}
static const struct bdev_ops b22b_ops = { ram_read, b22b_write, NULL };

static void b22b(void) {
    struct ram *x = ram_new("b22b", 2048);
    struct buf *held, *again, *dirty;
    static u8 newb[16 * 512];
    int w, s, held_valid_after, again_new, dirty_kept, dirty_on_card;
    fresh("B22b (R6): a held clean copy and a dirty copy under a refused straight write that the card stored");
    x->bd.ops = &b22b_ops;
    memset(x->mem + 100 * 512, 'A', 512);
    memset(x->mem + 101 * 512, 'B', 512);
    if(!(held = bread(&x->bd, 100))) die("B22b: read");          /* a reader holds block 100 (clean, 'A') */
    if(!(dirty = bread(&x->bd, 101))) die("B22b: read 2");
    memset(dirty->data, 'D', 512);                               /* block 101: dirty 'D' in the cache */
    bdirty(dirty);
    brelse(dirty);
    memset(newb, 'N', sizeof(newb));
    b22b_store_fail = 1;
    w = bwrite_blocks(&x->bd, 100, 16, newb);
    b22b_store_fail = 0;
    again = bread(&x->bd, 100);
    again_new = again && again != held && again->data[0] == 'N';
    brelse(held);
    held_valid_after = held->valid;
    if(again) brelse(again);
    dirty = bpeek(&x->bd, 101);
    dirty_kept = dirty && dirty->dirty && dirty->data[0] == 'D';
    if(dirty) brelse(dirty);
    s = bsync(&x->bd);
    dirty_on_card = x->mem[101 * 512] == 'D';
    check(w != 0 && again_new && !held_valid_after && dirty_kept && s == 0 && dirty_on_card,
          "B22b (R6): the held clean copy leaves the table and goes when let go; the dirty copy keeps its bytes");
}

/**** round fs-fix-6: the review of B34-B38 (its scenarios Q1-Q6, here under the audit item that fixes each: B39-B45) ****/

#ifdef BEFORE
#define FS_HOLD(t) 0                                  /* (47fb4ec has no such count) */
#else
#define FS_HOLD(t) ((t)->fs_hold)
#endif

/**** B39 (Q5, Q5b): a task killed while its sync waits in the cache. kill_task() marks a task inside a call (TI_KILL) and
      wakes the wait it is in if that wait may be cut short; KILL_WAIT_MS (2 s) later a task still in its call was killed
      at once (kill_escalate() -> become_zombie()). The cache's waits were none of those that are cut short, and B18's
      hold waits last 10 s: a sync waiting in write_copy() for a block another task held was killed at once there, its
      struct copy_rec left in the cache's list of copies on its dead stack (the next block set aside or dropped walked
      freed memory: the review's Q5, a longjmp() out of the wait, crashed in spoil_copies()), and the copied block marked
      copying for good (Q5b: the next sync that owed it never returned). Now the wait is cut short and the sync unwinds
      (the record unlinked, the block let go, -1), and the kill at once never comes while the task is inside the cache
      or a volume's lock (surf_task.fs_hold; kernel/task.c's kill_escalate(), which the kernel's self test checks:
      shell/selftest.c). Here: the kill as kill_task() makes it (TI_KILL inside a call) while the copy waits, the count
      at that moment, and the cache's state after ****/

#define B39_T 50
#define B39_H 300
#define B39_Y 900
static struct ram *b39_x;
static struct buf *b39_y, *b39_h;
static int b39_state, b39_hold_seen, b39_yields;
static surf_task b39_w1, b39_w2;
static void b39_hook(struct ram *r, u32 lba, u32 count) {   /* the sync's first write: the other task holds H, changes Y */
    surf_task *was = curTask;
    if(b39_state || !(lba <= B39_T && B39_T < lba + count)) return;
    b39_state = 1;
    curTask = &b39_w2;
    b39_h = bread(&r->bd, B39_H);
    if(!b39_h) die("B39: read H");
    b39_h->data[0] = 'H';
    bdirty(b39_h);                                   /* held by its writer */
    { struct buf *b = bread(&r->bd, B39_Y); if(!b) die("B39: read Y"); b->data[0] = 'E'; bdirty_order(b, BORDER_DIR); brelse(b); }
    curTask = was;
}
static void b39_yield(void) {                        /* the copy waits for H: the sync's task is killed (kill_task()) */
    b39_yields++;
    if(b39_state != 1 || !b39_y->copying) return;
    b39_hold_seen = FS_HOLD(curTask);                /* where kill_escalate() would find it, KILL_WAIT_MS later */
    curTask->in_call = TC_SYSCALL;
    curTask->intr |= TI_KILL;
    b39_state = 2;
}
static void __attribute__((noinline)) b39_reuse_stack(void) {   /* (Q5: another task's frames where the sync's were) */
    volatile u8 junk[32768];
    u32 i;
    for(i = 0; i < sizeof(junk); i++) junk[i] = 0xA7;
    (void)junk[100];
}
static void b39(void) {
    struct ram *x = ram_new("b39", 2048);
    struct buf *b;
    int r, r2, copying_after, hold_after, gave_up;
    fresh("B39 (Q5, Q5b): a task killed while its sync's copy waits for a block another task holds");
    b39_x = x;
    b39_state = b39_hold_seen = b39_yields = 0;
    memset(&b39_w1, 0, sizeof(b39_w1));
    b = bread(&x->bd, B39_T); b->data[0] = 'T'; bdirty(b); brelse(b);
    b = bread(&x->bd, B39_Y); b->data[0] = 'e'; bdirty_order(b, BORDER_DIR); brelse(b);
    b39_y = bpeek(&x->bd, B39_Y);
    brelse(b39_y);
    x->hook = b39_hook;
    yield_hook = b39_yield;
    curTask = &b39_w1;
    r = bsync(&x->bd);                               /* a close's write-back (no volume lock held) */
    hold_after = FS_HOLD(curTask);
    curTask = NULL;
    yield_hook = NULL;
    x->hook = NULL;
    copying_after = b39_y->copying;
    gave_up = said("held for");                      /* B18's give-up after 10 s: the wait was not cut short */
    b39_reuse_stack();
    curTask = &b39_w2;                               /* Q5: another task's rm drops a dirty block (walks the copies) */
    b = bread(&x->bd, 400); b->data[0] = 'Z'; bdirty(b); brelse(b);
    bdiscard(&x->bd, 400);
    brelse(b39_h);                                   /* Q5b: H let go; the next sync owes Y */
    r2 = bsync(&x->bd);
    curTask = NULL;
    if(verbose) printf("      the killed sync %d (yields %d); inside the cache when killed %d, after %d; Y copying after %d; B18's 10 s"
                       " give-up said %d; the next sync %d, Y on the card '%c'\n", r, b39_yields, b39_hold_seen, hold_after,
                       copying_after, gave_up, r2, x->mem[B39_Y * 512]);
    check(b39_state == 2, "B39: the scenario took place (the sync's task killed while its copy waited for a held block)");
    check(b39_hold_seen > 0, "B39 (Q5): the killed task is inside the cache while its copy waits (fs_hold: no kill at once there)");
    check(r == -1 && !gave_up && b39_yields <= 3, "B39 (Q5): the kill cuts the copy's wait short: the sync gives up at once (-1)");
    check(!copying_after && hold_after == 0, "B39 (Q5b): the killed sync leaves nothing behind (its copy's block let go, out of the cache)");
    check(r2 == 0 && x->mem[B39_Y * 512] == 'E', "B39 (Q5b): the next sync writes the block (it returns, 0)");
}

/* B39b: the kill in a sync's wait for a hold outside a copy (bsync()'s first rounds, through the file system's sync), as
   kill_task() makes it for a system call (TI_KILL) and Ctrl+C for a kernel command (TI_INT): that wait holds nothing of
   the cache with its lock let go (not counted), and is cut short; the count during an rm's order point (fs/fat.c's lock,
   and the cache's around its device request); a task that ends by itself (TC_EXIT, killed before) is not cut short: its
   files' close writes them out */
static struct buf *b39b_held;
static int b39b_mode, b39b_seen, b39b_released, b39b_yields, b39b_rm_hold;
static surf_task b39b_t;
static void b39b_yield(void) {
    b39b_yields++;
    if(!b39b_held) return;
    if(b39b_mode == 2) {                             /* TC_EXIT: the holder lets go after a few looks */
        if(b39b_yields >= 3) { brelse(b39b_held); b39b_held = NULL; b39b_released = 1; }
        return;
    }
    if(b39b_seen) return;
    b39b_seen = 1 + FS_HOLD(curTask);               /* (1: none: the wait holds nothing of the cache, its lock let go) */
    if(b39b_mode == 0) { curTask->in_call = TC_SYSCALL; curTask->intr |= TI_KILL; }   /* kill_task() */
    else { curTask->in_call = TC_COMMAND; curTask->intr |= TI_INT; }                  /* Ctrl+C (task_interrupt_job()) */
}
static void b39b_hook(struct ram *r, u32 lba, u32 count) {   /* an rm's order point: inside the lock and the cache */
    (void)r; (void)lba; (void)count;
    if(curTask == &b39b_t && FS_HOLD(curTask) > b39b_rm_hold) b39b_rm_hold = FS_HOLD(curTask);
}
static void b39b(int mode) {
    static const char *what[] = { "a kill (TI_KILL, a system call)", "Ctrl+C (TI_INT, a kernel command)",
                                  "a task ending by itself (TC_EXIT, killed before)" };
    struct ram *x = ram_new("b39b", 8192);
    struct superblock *sb;
    struct vnode *root;
    struct geo g;
    int r, r2, gave_up;
    fresh("B39b: a sync of the file system waits for a block another task holds, and its task is killed");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 39);
    if(put_file(root, "f", pat, 4096) || put_file(root, "g", pat, 512) || fsync_sb(sb)) die("B39b: setup");
    memset(&b39b_t, 0, sizeof(b39b_t));
    b39b_rm_hold = 0;
    if(mode == 0) {                                  /* the count inside an rm (the volume's lock, then the cache) */
        x->hook = b39b_hook;
        curTask = &b39b_t;
        if(root->ops->unlink(root, "g")) die("B39b: rm");
        curTask = NULL;
        x->hook = NULL;
    }
    b39b_mode = mode;
    b39b_seen = b39b_released = b39b_yields = 0;
    b39b_held = bread(&x->bd, clus_lba(&g, 2));      /* another task holds f's first block, changed */
    if(!b39b_held) die("B39b: read");
    b39b_held->data[0] ^= 1;
    bdirty(b39b_held);
    if(mode == 2) { b39b_t.in_call = TC_EXIT; b39b_t.intr = TI_KILL; }
    yield_hook = b39b_yield;
    curTask = &b39b_t;
    r = fsync_sb(sb);                                /* vfs_fsync(): the file system's sync, then the device's blocks */
    yield_hook = NULL;
    gave_up = said("held for");
    if(verbose) printf("      %s: the sync %d (yields %d); counted inside the cache in the wait %d, after %d; B18 gave up %d\n",
                       what[mode], r, b39b_yields, b39b_seen - 1, FS_HOLD(curTask), gave_up);
    if(mode == 0) {
        if(verbose) printf("      the rm's order point's device writes ran counted %d (the volume's lock and the request: 2)\n", b39b_rm_hold);
        check(b39b_rm_hold >= 2, "B39b: an rm's order point's device writes run counted inside the file system (the volume's lock, the request)");
    }
    if(mode < 2) {
        check(b39b_seen > 0 && FS_HOLD(curTask) == 0 && !gave_up && b39b_yields <= 2 && r == -EIO,
              mode ? "B39b: Ctrl+C in a kernel command cuts the sync's wait for a hold short (EIO at once, nothing held after)"
                   : "B39b: a kill cuts the sync's wait for a hold short (EIO at once, nothing held after)");
        curTask = NULL;
        brelse(b39b_held);
        r2 = fsync_sb(sb);
        check(r2 == 0, "B39b: the next sync writes the block");
    } else {
        curTask = NULL;
        check(r == 0 && b39b_released && !gave_up, "B39b: a task ending by itself (TC_EXIT) waits on: its close writes its files out");
    }
    vput(sb->root);
    sb->ops->unmount(sb);
}

/* B39c: a large write in one call, its task killed after its first request: it stops there with a short count (a
   task inside the volume's lock is not killed at once, so the kill would wait for the whole transfer otherwise);
   the file is what it says, the volume consistent, the task out of the file system */
static surf_task b39c_t;
static int b39c_kill_at, b39c_writes;
static void b39c_hook(struct ram *r, u32 lba, u32 count) {
    (void)r; (void)lba; (void)count;
    if(++b39c_writes == b39c_kill_at) { b39c_t.in_call = TC_SYSCALL; b39c_t.intr |= TI_KILL; }
}
static void b39c(void) {
    struct ram *x = ram_new("b39c", 8192);
    struct superblock *sb;
    struct vnode *root, *v;
    static u8 big[262144];
    u32 used, reached;
    int w, ok_bytes;
    fresh("B39c: a 256 KB write in one call, its task killed after its first request");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    fill(big, sizeof(big), 391);
    if(root->ops->create(root, "big", VN_FILE, &v) || fsync_sb(sb)) die("B39c: setup");
    memset(&b39c_t, 0, sizeof(b39c_t));
    b39c_writes = 0;
    b39c_kill_at = 1;
    x->hook = b39c_hook;
    curTask = &b39c_t;
    w = v->ops->write(v, 0, big, sizeof(big));
    curTask = NULL;
    x->hook = NULL;
    ok_bytes = w > 0 && v->size == (u32)w;
    vput(v);
    if(fsync_sb(sb)) die("B39c: sync");
    census(x, &used, &reached);
    if(verbose) printf("      the write said %d of %u (size %u); the task inside after it %u; clusters used %u, reached %u\n", w,
                       (u32)sizeof(big), ok_bytes ? (u32)w : 0, FS_HOLD(&b39c_t), used, reached);
    check(w > 0 && w < (int)sizeof(big) && ok_bytes && FS_HOLD(&b39c_t) == 0,
          "B39c: a kill stops a large write between two requests (a short count, the size what it says)");
    {
        static u8 back[262144];
        struct vnode *f = lookup(root, "big");
        int same = f && w > 0 && f->size == (u32)w && f->ops->read(f, 0, back, (u32)w) == w && !memcmp(back, big, (u32)w);
        if(f) vput(f);
        check(same && used == reached, "B39c: the file holds what was written, nothing lost");
    }
    vput(sb->root);
    sb->ops->unmount(sb);
}

/**** B40 (Q4): binvalidate() (umount giving up what could not be written: vfs_umount_lost(); a mount that failed; mkfs)
      dropped every idle block of the device, dirty ones included, without spoiling the copies under way (B34 spoiled in
      baside() and discard_now() only), and left the blocks in someone's hands or under I/O: another task's sync, writing
      directory block Y from a copy that waited for a block, wrote the copy, which names F, after F's data block X was
      given up unwritten. Now binvalidate() drops as bdiscard() does, and marks what it cannot drop yet (Y's copy: not
      written; Y dropped when let go) ****/

#define B40_T 50
#define B40_X 100
#define B40_H 300
#define B40_Y 900
static struct ram *b40_x;
static struct buf *b40_y, *b40_hx, *b40_hh;
static int b40_state, b40_named_stale;
static surf_task b40_w1, b40_w2;
static void b40_hook(struct ram *r, u32 lba, u32 count) {
    surf_task *was = curTask;
    if(lba <= B40_Y && B40_Y < lba + count) {        /* Y on the card: does it name F over a block F never wrote? */
        if(r->mem[B40_Y * 512] == 'E' && r->mem[B40_X * 512] != 'X') b40_named_stale++;
        return;
    }
    if(b40_state || !(lba <= B40_T && B40_T < lba + count)) return;
    b40_state = 1;
    curTask = &b40_w2;                               /* F's writer: X (F's block), then H (another's), then F's entry */
    b40_hx = bread(&r->bd, B40_X); b40_hx->data[0] = 'X'; bdirty(b40_hx);
    b40_hh = bread(&r->bd, B40_H); b40_hh->data[0] = 'H'; bdirty(b40_hh);
    { struct buf *b = bread(&r->bd, B40_Y); b->data[0] = 'E'; bdirty_order(b, BORDER_DIR); brelse(b); }
    curTask = was;
}
static void b40_yield(void) {
    surf_task *was = curTask;
    if(b40_state != 1 || !b40_y->copying) return;
    curTask = &b40_w2;
    if(b40_hx) {                                     /* the copy waits for X: X let go, then umount gives up */
        brelse(b40_hx); b40_hx = NULL;
        binvalidate(&b40_x->bd);
    } else if(b40_hh) { brelse(b40_hh); b40_hh = NULL; b40_state = 2; }
    curTask = was;
}
static void b40(void) {
    struct ram *x = ram_new("b40", 2048);
    struct buf *b, *left;
    int r;
    fresh("B40 (Q4): binvalidate() while a sync's copy waits");
    b40_x = x;
    b40_state = b40_named_stale = 0;
    b = bread(&x->bd, B40_T); b->data[0] = 'T'; bdirty(b); brelse(b);
    b = bread(&x->bd, B40_Y); b->data[0] = 'e'; bdirty_order(b, BORDER_DIR); brelse(b);
    b40_y = bpeek(&x->bd, B40_Y); brelse(b40_y);
    x->hook = b40_hook;
    yield_hook = b40_yield;
    curTask = &b40_w1;
    r = bsync(NULL);                                 /* vfs_sync(): every device */
    curTask = NULL; yield_hook = NULL; x->hook = NULL;
    left = bpeek(&x->bd, B40_Y);
    if(verbose) printf("      the sync %d; scenario state %d; Y on the card '%c', X on the card %#x: Y named F over a block F never"
                       " wrote %d times; Y still cached %d\n", r, b40_state, x->mem[B40_Y * 512] ? x->mem[B40_Y * 512] : '0',
                       x->mem[B40_X * 512], b40_named_stale, left != NULL);
    if(left) brelse(left);
    check(b40_state == 2, "B40: the scenario took place (X dropped by binvalidate() while Y's copy waited for it)");
    check(b40_named_stale == 0, "B40 (Q4): no copy reaches the card after a block it needs left the lists unwritten (binvalidate())");
    check(!left, "B40: nothing of the device stays cached after binvalidate() (Y, under a copy then, went when let go)");
}

/**** B41 (Q1, Q1b): a sync writes directory block Y from a copy (B12) under sustained rm/truncate traffic: each look of
      the copy waits for a block another task holds (H_k), and meanwhile that task drops (bdiscard(): an rm's free, B28)
      another block the copy needs (V_k), or one it had set aside before the copy was taken (X_k: aside_end()), and
      writes on (a new H, V, Y changed again). Every look was spoiled; after COPY_LOOKS (16) the sync said -1 though the
      card took every write. Now a spoil takes only copies of blocks that changed since they were taken, by blocks not set
      aside before, and spoiled looks are taken as long as it takes (c, below), ending when the call's debt is paid by
      another task's write (d) ****/

#define B41_T  50
#define B41_Y  900
#define B41_MAXK 40
static struct ram *b41_x;
static struct buf *b41_y, *b41_h, *b41_v, *b41_yheld;
static int b41_mode, b41_k, b41_looks, b41_drops, b41_ready, b41_vheld, b41_paid;
static surf_task b41_w1, b41_w2;
static u32 b41_H(int k) { return 100 + 4 * (u32)k; }
static u32 b41_V(int k) { return 300 + 4 * (u32)k; }
static u32 b41_X(int k) { return 500 + 4 * (u32)k; }
static struct buf *b41_dirty(u32 lba, u8 order, u8 v) {   /* read, change, mark dirty; kept in hand */
    struct buf *b = bread(&b41_x->bd, lba);
    if(!b) die("B41: read");
    b->data[0] = v;
    b->data[1]++;
    bdirty_order(b, order);
    return b;
}
static void b41_touch(u32 lba, u8 order, u8 v) { brelse(b41_dirty(lba, order, v)); }
static void b41_setup(int k) {                       /* the other task's next round: what look k needs, then Y */
    if(b41_mode != 1 && b41_mode != 3) { b41_v = b41_dirty(b41_V(k), BORDER_DATA, 0x77); b41_vheld = 1; }
    b41_h = b41_dirty(b41_H(k), BORDER_DATA, 0x66);
    if(b41_mode == 1 || b41_mode == 3) {             /* an rm's aside_begin(): set aside before the copy is taken */
        b41_touch(b41_X(k), BORDER_DATA, 0x88);
        baside(&b41_x->bd, b41_X(k), true);
    }
    b41_touch(B41_Y, BORDER_DIR, 'E');               /* Y needs them now */
}
static void b41_hook(struct ram *r, u32 lba, u32 count) {   /* the sync's first write: the other task starts */
    surf_task *was = curTask;
    (void)r;
    if(b41_ready || !(lba <= B41_T && B41_T < lba + count)) return;
    b41_ready = 1;
    curTask = &b41_w2;
    b41_k = 1;
    b41_setup(1);
    curTask = was;
}
static void b41_yield(void) {                        /* the copy waits for a held block: the other task runs */
    surf_task *was = curTask;
    if(b41_yheld) {                                  /* (d) the copy let go, Y in the other task's hands: it writes Y */
        static u8 yb[512];
        curTask = &b41_w2;
        memcpy(yb, b41_yheld->data, 512);
        yb[0] = 'W';
        if(bwrite_blocks(&b41_x->bd, B41_Y, 1, yb) == 0) b41_paid = 1;
        b41_yheld->data[0] = 'W';
        b41_yheld->data[1]++;
        bdirty_order(b41_yheld, BORDER_DIR);         /* and changes it again: dirty with a change after the sync began */
        brelse(b41_yheld);
        b41_yheld = NULL;
        curTask = was;
        return;
    }
    if(!b41_ready || !b41_y->copying) return;
    curTask = &b41_w2;
    if(b41_vheld) {                                  /* the copy waits for V_k: V_k let go; dropped (modes 0, 4, 5) */
        if(b41_mode >= 4) b41_touch(B41_Y, BORDER_DIR, 'E');   /* (c, d) first Y changes: the copy may name V_k now */
        brelse(b41_v);
        b41_vheld = 0;
        if(b41_mode == 0 || b41_mode >= 4) { bdiscard(&b41_x->bd, b41_V(b41_k)); b41_drops++; }
        curTask = was;
        return;
    }
    if(b41_mode == 1) { bdiscard(&b41_x->bd, b41_X(b41_k)); b41_drops++; }   /* aside_end(): the order point worked */
    b41_looks++;                                     /* it waits for H_k: the next round, then H_k let go */
    if(b41_mode == 5 && b41_looks == 5) b41_yheld = bread(&b41_x->bd, B41_Y);   /* (d) it takes Y meanwhile */
    if(b41_k < B41_MAXK) { struct buf *old = b41_h; b41_k++; b41_setup(b41_k); brelse(old); }
    else { brelse(b41_h); b41_h = NULL; }
    curTask = was;
}
static void b41(int mode) {
    struct ram *x = ram_new("b41", 2048);
    int r, r2, i;
    u32 failed0;
    static const char *what[] = { "a needed block dropped during each look", "a block set aside before the copy, dropped during each look",
                                  "control of mode 0 (let go, not dropped)", "control of mode 1 (set aside before, never dropped)",
                                  "Y changed and a needed block dropped during each look, 40 rounds",
                                  "as mode 4, and another task writes Y at the fifth look" };
    fresh("B41 (Q1): a sync's copy under sustained rm/truncate traffic");
    b41_x = x;
    b41_mode = mode;
    b41_k = b41_looks = b41_drops = b41_ready = b41_vheld = b41_paid = 0;
    b41_h = b41_v = b41_yheld = NULL;
    b41_touch(B41_T, BORDER_DATA, 0x55);             /* owed: written first; its write lets the other task in */
    b41_touch(B41_Y, BORDER_DIR, 'E');               /* owed */
    b41_y = bpeek(&x->bd, B41_Y);
    if(!b41_y) die("B41: Y");
    brelse(b41_y);
    failed0 = x->failed;
    x->hook = b41_hook;
    yield_hook = b41_yield;
    curTask = &b41_w1;
    r = bsync(&x->bd);                               /* a close's write-back or an fsync: no volume lock held */
    curTask = NULL;
    yield_hook = NULL;
    x->hook = NULL;
    if(verbose) printf("      mode %d (%s): the sync %d; looks %d, needed blocks dropped %d; writes the card refused %u; Y on the card"
                       " '%c'%s\n", mode, what[mode], r, b41_looks, b41_drops, x->failed - failed0, x->mem[B41_Y * 512],
                       mode == 5 ? (b41_paid ? " (written by the other task)" : " (the other task's write failed)") : "");
    if(mode == 0) check(r == 0, "B41a (Q1): a sync whose card took every write says 0 though rm traffic dropped a needed block each look");
    if(mode == 1) check(r == 0, "B41b (Q1b): blocks set aside before the copy was taken, then dropped, spoil nothing (0)");
    if(mode == 2 || mode == 3) check(r == 0, "B41: control: without the drops the copy goes at once");
    if(mode == 4) check(r == 0 && b41_looks > 16 && x->failed == failed0,
                        "B41c: every look spoiled for 40 rounds (more than B34's 16): the copy is taken again until it goes (0)");
    if(mode == 5) check(r == 0 && b41_paid && b41_looks <= 6,
                        "B41d: the copy ends once another task's write paid the sync's debt (0, at once)");
    curTask = &b41_w2;                               /* the other task ends: everything let go, its blocks dropped */
    if(b41_vheld) brelse(b41_v);
    if(b41_h) brelse(b41_h);
    if(b41_yheld) brelse(b41_yheld);
    for(i = 1; i <= B41_MAXK; i++) if(mode == 1 || mode == 3) bdiscard(&x->bd, b41_X(i));
    curTask = NULL;
    r2 = bsync(&x->bd);
    check(r2 == 0 && x->mem[B41_Y * 512] == (mode == 5 ? 'W' : 'E'), "B41: once the traffic stops the next sync writes Y");
}

/**** B42 (Q2): B38's volume with lost clusters stays marked as not cleanly unmounted, and the next mount says so; if that
      next mount is SurfOS's own (a reboot), it said so once, then marked the volume clean at its unmount: the clusters
      still lost, and the computer after it told not to check. Now a volume not marked cleanly unmounted when it was
      mounted stays so at unmount (only a checker clears it) ****/

static void b42(void) {
    struct ram *x = ram_new("b42", 8192);
    struct superblock *sb;
    struct vnode *root, *v, *d = NULL;
    struct geo g;
    u32 used, reached, used2, reached2, i, fat1a, fat1b;
    int s, warn2, warn3, said_stays;
    fresh("B42 (Q2): B38's unclean volume through SurfOS's own next mount and unmount");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 32);
    if(put_file(root, "f", pat, 512) || root->ops->create(root, "d", VN_DIR, &d) || fsync_sb(sb)) die("B42: setup");
    x->bad_lo = clus_lba(&g, 3);
    x->bad_hi = g.data_lba + g.nclusters - 1;
    if(put_file(d, "g", NULL, 0)) die("B42: a file in d");
    vput(d);
    if(fsync_sb(sb) != -EIO) die("B42: the refused sync");
    v = lookup(root, "f");
    for(i = 0; i < 65; i++) if(v->ops->write(v, 512, pat, 8192) != -EIO) die("B42: an append not refused");
    vput(v);
    x->bad_hi = 0;
    s = fsync_sb(sb);
    vput(sb->root);
    sb->ops->unmount(sb);                            /* 1st unmount: 16 clusters lost: stays unclean (B38) */
    census(x, &used, &reached);
    fat1a = fat1(x, &g, 1);
    klog_len = 0; klog[0] = 0;
    sb = mount(x);                                   /* SurfOS mounts it again (a reboot) */
    warn2 = said("was not unmounted cleanly");
    if(fsync_sb(sb)) die("B42: sync after the 2nd mount");
    vput(sb->root);
    sb->ops->unmount(sb);                            /* and unmounts it, nothing done in between */
    said_stays = said("only a check clears that, the volume stays marked as not cleanly unmounted");
    census(x, &used2, &reached2);
    fat1b = fat1(x, &g, 1);
    klog_len = 0; klog[0] = 0;
    sb = mount(x);
    warn3 = said("was not unmounted cleanly");
    if(verbose) printf("      sync %d; after the 1st unmount: lost %u, FAT[1] %#x; the 2nd mount warns %d; after its unmount (said so %d):"
                       " lost %u, FAT[1] %#x; the 3rd mount warns %d\n", s, used - reached, fat1a, warn2, said_stays,
                       used2 - reached2, fat1b, warn3);
    check(warn2 == 1 && used2 > reached2 && !(fat1b & 0x8000) && warn3 == 1 && said_stays == 1,
          "B42 (Q2): a volume unclean when mounted stays marked so at SurfOS's next unmount, which says so; the mount after warns");
    vput(sb->root);
    sb->ops->unmount(sb);
}

/**** B43 (Q3): clusters an rm gives back that could not be freed: fat_free_chain() meets an unreadable FAT sector in the
      middle of the chain, after the rm's order point took the name off the card. The rm said EIO with the name gone (the
      next rm: ENOENT), the rest of the chain stayed allocated with nothing pointing at it, uncounted and unsaid, and the
      unmount marked the volume cleanly unmounted. Now they are counted (as far as the FAT can be read: "at least") and
      said, the volume stays unclean, and the rm says 0: it has happened ****/

static void b43(void) {
    struct ram *x = ram_new("b43", 8192), *y = ram_new("b43y", 4096);
    struct superblock *sb;
    struct vnode *root;
    struct geo g;
    u32 used, reached, lba, fat1v;
    int rm, rm2, warn, said_rm, said_um;
    fresh("B43 (Q3): an rm whose free meets an unreadable FAT sector; then unmount");
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    fill(pat, 65536, 33);
    if(big_file(root, "big", pat, 600 * 512) || fsync_sb(sb)) die("B43: setup");   /* clusters 2..601: 3 FAT sectors */
    for(lba = 0; lba < 2000; lba++) { struct buf *b = bread(&y->bd, lba); if(b) brelse(b); }   /* the FAT leaves the cache */
    x->rbad = g.fat_lba + 1;                         /* the first FAT's second sector (clusters 256..511) unreadable */
    rm = root->ops->unlink(root, "big");
    said_rm = said("given back could not be freed (the FAT could not be read or written there)");
    x->rbad = 0;
    rm2 = root->ops->unlink(root, "big");
    fsync_sb(sb);
    vput(sb->root);
    sb->ops->unmount(sb);
    said_um = said("at least 1 cluster given back could not be freed and may stay allocated");
    census(x, &used, &reached);
    fat1v = fat1(x, &g, 1);
    klog_len = 0; klog[0] = 0;
    sb = mount(x);
    warn = said("was not unmounted cleanly");
    if(verbose) printf("      rm %d (said so %d), again %d; after unmount (said so %d): used %u reached %u (lost %u), FAT[1] %#x; the next"
                       " mount warns %d\n", rm, said_rm, rm2, said_um, used, reached, used - reached, fat1v, warn);
    check(rm == 0 && rm2 == -ENOENT, "B43 (Q3): an rm whose name is off the card says 0 though its free failed (the next one ENOENT)");
    check(said_rm == 1 && said_um == 1, "B43 (Q3): the clusters it could not free are counted and said (at least, as far as the FAT reads)");
    check(used > reached && !(fat1v & 0x8000) && warn == 1,
          "B43 (Q3): clusters given back that could not be freed keep the volume marked as not cleanly unmounted");
    vput(sb->root);
    sb->ops->unmount(sb);
}

/**** B44 (cosmetic): FAT12 has no clean-unmount mark (fat_mark() does nothing), yet an unmount with write errors, lost
      clusters or damage said "the volume stays marked as not cleanly unmounted". A FAT12 volume made here by hand (SurfOS's
      mkfs makes FAT16 and FAT32 only): 2048 sectors of one sector a cluster ****/

static void b44(void) {
    struct ram *x = ram_new("b44", 2048);
    struct superblock *sb;
    u8 *bs = x->mem;
    int i, said12, said_marked;
    fresh("B44: the unmount of a FAT12 volume with write errors");
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
    memcpy(bs + 3, "SURFTEST", 8);
    bs[11] = 0x00; bs[12] = 0x02;                    /* 512 bytes a sector */
    bs[13] = 1;                                      /* a sector a cluster */
    bs[14] = 1; bs[15] = 0;                          /* one reserved sector */
    bs[16] = 2;                                      /* two FATs */
    bs[17] = 224; bs[18] = 0;                        /* 224 root entries: 14 sectors */
    bs[19] = 0x00; bs[20] = 0x08;                    /* 2048 sectors */
    bs[21] = 0xF8;
    bs[22] = 6; bs[23] = 0;                          /* 6 sectors a FAT: 2021 clusters, FAT12 */
    bs[38] = 0x29;
    memcpy(bs + 43, "TWELVE     ", 11);
    memcpy(bs + 54, "FAT12   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    for(i = 0; i < 2; i++) { u8 *f = x->mem + (1 + i * 6) * 512; f[0] = 0xF8; f[1] = 0xFF; f[2] = 0xFF; }
    sb = mount(x);
    if(put_file(sb->root, "a", pat, 1000)) die("B44: a file");
    x->fail_all = 1;                                 /* the card refuses from now on */
    vput(sb->root);
    sb->ops->unmount(sb);
    x->fail_all = 0;
    said12 = said("+FAT12: b44: write errors; FAT12 keeps no mark of a clean unmount");
    said_marked = said("stays marked as not cleanly unmounted");
    if(verbose) printf("      the unmount said the FAT12 line %d, the line about the mark %d\n", said12, said_marked);
    check(said12 == 1 && !said_marked, "B44: a FAT12 unmount that fails says FAT12 keeps no mark (not that the volume stays marked)");
    binvalidate(&x->bd);                             /* (what could not be written goes, as umount gives it up) */
}

/**** B45 (Q6): an rm's aside_end(done) drops (bdiscard()) a block of its chain that a reader took while it was set aside:
      the drop waits for the reader (B35). The cluster goes to a new file, which writes the block through the cache before
      the reader lets go: "a change came after", so the drop was called off (the block has an owner again), but the block
      stayed set aside: no sync wrote it (the new owner's own syncs did not even owe it), another task's sync gave up on it
      after 10 s. Now such a change starts the block's dirt again, set aside no more ****/

static void b45(void) {
    struct ram *x = ram_new("b45", 2048);
    struct buf *b, *reader;
    static surf_task ta, tr;
    int r1, r2, aside_after, dirty_after;
    fresh("B45 (Q6): a set-aside block read meanwhile, dropped, then written by its cluster's next owner");
    curTask = &ta;
    b = bread(&x->bd, 300); b->data[0] = 'F'; bdirty(b); brelse(b);        /* F's dirty block */
    baside(&x->bd, 300, true);                                             /* task A's rm: aside_begin() */
    curTask = &tr;
    reader = bpeek(&x->bd, 300);                                           /* a reader takes it meanwhile */
    curTask = &ta;
    bdiscard(&x->bd, 300);                                                 /* aside_end(): order point worked: drop (waits) */
    b = bread(&x->bd, 300); b->data[0] = 'B'; bdirty(b); brelse(b);        /* the next owner (task A again) writes it */
    curTask = &tr;
    brelse(reader);                                                        /* the reader lets go */
    curTask = &ta;
    b = bpeek(&x->bd, 300);
    aside_after = b ? b->aside : -1; dirty_after = b ? b->dirty : -1;
    if(b) brelse(b);
    r1 = bsync(&x->bd);                                                    /* the new owner's close: its write-back */
    curTask = NULL;
    r2 = bsync(&x->bd);
    if(verbose) printf("      after the reader let go the block is dirty %d, set aside %d; the owner's sync %d, another sync %d; the card"
                       " has '%c' (the owner wrote 'B')\n", dirty_after, aside_after, r1, r2, x->mem[300 * 512] ? x->mem[300 * 512] : '0');
    check(!(r1 == 0 && x->mem[300 * 512] != 'B'), "B45 (Q6): a sync that says 0 wrote the new owner's block (it is not left set aside)");
    check(x->mem[300 * 512] == 'B' && r2 == 0, "B45 (Q6): the block written by its cluster's next owner reaches the card");
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : NULL;
    verbose = getenv("FATFS_VERBOSE") != NULL;
    fatfs_init();
#define RUN(name, call) if(!only || !strcmp(only, name)) { call; }
    RUN("t1", t1());
    RUN("t2", t2());
    RUN("t3", t3());
    RUN("t4", t4(0); t4(1));
    RUN("t5", t5());
    RUN("t6", t6());
    RUN("t7", t7());
    RUN("t8", t8());
    RUN("t9", t9());
    RUN("t10", t10());
    RUN("t11", t11());
    RUN("t12", t12());
    RUN("t13", t13());
    RUN("t14", t14());
    RUN("t15", t15());
    RUN("t16", t16());
    RUN("b4", b4());
    RUN("b11", b11());
    RUN("b11b", b11b());
    RUN("b12a", b12a());
    RUN("b12b", b12b());
    RUN("b13a", b13a());
    RUN("b13b", b13b());
    RUN("b13c", b13c());
    RUN("b14a", b14a());
    RUN("b14b", b14b());
    RUN("b15a", b15a());
    RUN("b15b", b15b());
    RUN("b16b", b16b());
    RUN("b17a", r6_run("B17a (R6): mv s/a b (b exists) when s's block cannot be read after b went", 2, 0));
    RUN("b17b", r6_run("B17b: mv s/a b (b exists) when s's block cannot be read after the new name is on the disk", 1, 1));
    RUN("b17c", b17c());
    RUN("b17d", b17d());
    RUN("b18", b18());
    RUN("b20a", s1(0));
    RUN("b20b", s1(1));
    RUN("b20c", b20c());
    RUN("b21a", s2(12); s2(20));
    RUN("b21b", b21b());
    RUN("b22", b22());
    RUN("b23", b23());
    RUN("b24", b24());
    RUN("b25", b25());
    RUN("b26a", b26a());
    RUN("b26b", b26b());
    RUN("b27", b27(-1); b27(0); b27(0x41));
    RUN("b28a", b28(0));
    RUN("b28b", b28(1));
    RUN("b28c", b28c());
    RUN("b28d", b28(2));
    RUN("b29", b29());
    RUN("b30", b30());
    RUN("b30b", b30b());
    RUN("b31a", b31a());
    RUN("b31b", b31b());
    RUN("b32", b32());
    RUN("b22b", b22b());
    RUN("b33", b33());
    RUN("b34a", b34(0));
    RUN("b34b", b34(1));
    RUN("b36", b36());
    RUN("b36b", b36b());
    RUN("b37", b37());
    RUN("b38", b38());
    RUN("b39", b39());
    RUN("b39b", b39b(0); b39b(1); b39b(2));
    RUN("b39c", b39c());
    RUN("b40", b40());
    RUN("b41a", b41(0); b41(2));
    RUN("b41b", b41(1); b41(3));
    RUN("b41c", b41(4));
    RUN("b41d", b41(5));
    RUN("b42", b42());
    RUN("b43", b43());
    RUN("b44", b44());
    RUN("b45", b45());
    audit();
    printf("FATFS: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
