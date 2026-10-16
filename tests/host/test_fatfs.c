/* Host-side test of the block cache and FAT together (10/16/26, audit B5-B11): fs/bcache.c,
   fs/fat.c and fs/fat_mkfs.c as they are, over RAM disks that can refuse writes (all of them, or
   those that touch some blocks) and reads. The kernel around them is a few stand-ins below: one
   task, so bcache's waits only let go of its lock and take it again; "another task" is a hook
   that runs inside a device write, which is when bcache_lock is let go, as a second task would
   run then. An oracle reads the FAT16 image after every write the disk stores (every point at
   which a card could be pulled) and checks that no directory entry, and no FAT link on the way,
   reaches a cluster the first FAT says is free.

   The scenarios are those of the review of B1-B4 (T1-T10, B4a-c) and of its fixes; each says
   which audit item it checks. Built like the kernel objects (tests/host/pager_shim for the
   interrupt flag) and run by `make unittest`; surfcpu/docs/evidence/os/bcache-review.txt has
   what each scenario found before and after the fixes. */
#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <surfos/wait.h>
#include <surfos/task.h>
#include <surfos/interrupt.h>
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
void wake_up_all(wait_queue_t *q) { (void)q; }
void yield(void) {}

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

void *kalloc(size_t n) { return malloc(n); }
void *kcalloc(size_t n, size_t m) { return calloc(n, m); }
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
    b = bget(&x->bd, 300); bdirty_order(b, BORDER_DIR); brelse(b);        /* an entry block, the older */
    b = bget(&x->bd, 200); bdirty(b); brelse(b);                         /* and data it may point at */
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
    audit();
    printf("FATFS: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
