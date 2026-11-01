/* Host-side test of the block cache and FAT together (10/24/26, storage milestone 1: interrupted saves, failed
   writes and concurrent unmounts preserve existing files): fs/bcache.c, fs/fat.c and fs/fat_mkfs.c as they are, over
   RAM disks that refuse writes on request (all of them, those that touch some blocks, one write, or every write from
   the nth on, a stick pulled there) and reads. The harness is the SurfCPU branch's (its tests/host/test_fatfs.c, audit
   B1-B57): the kernel around the cache is a few stand-ins, one task, so the cache's waits only let go of its lock and
   take it again; "another task" is a hook that runs inside a device write, which is when the cache lets its lock go,
   as a second task would run then. Oracles read the image after every write the disk stores (every point at which a
   stick could be pulled): no directory entry, and no FAT link on the way, reaches a cluster the first FAT says is
   free (FAT16 and FAT32), and a file being saved is whole under its name, the old one or the new.

   The scenarios: SurfCPU's T1-T16, B4 and B11, which check what x86 brought back of its fixes (each says which audit
   item), and x86's own: a save (a new file, fsync, close, a rename over the old one: edit's, the desktop's and rx's)
   pulled at every write it makes, and with every write refused once or for good; twins, two names on one chain, as
   a rename cut short leaves them; a rename over a file, in place; an unmount while another task's write is under
   way, and one that gives up what a refusing stick holds; the clean mark. Then the review of 10/24/26's scenarios
   (under the audit items of their fixes: B30b, B3s, W2b-W2d, B25b) and those of its fixes (W3, B13p, B18k).
   Built like the kernel objects (pager_shim for the interrupt flag) and run by `make unittest`; `test_fatfs <name>`
   runs one scenario, FATFS_VERBOSE=1 says every check. */
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
int system(const char *cmd);
int open(const char *path, int flags, ...);
int close(int fd);
long write(int fd, const void *buf, unsigned long n);
int unlink(const char *path);
#define HOST_O_WRONLY 01
#define HOST_O_CREAT  0100
#define HOST_O_TRUNC  01000

void fatfs_init(void);

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
surf_task *curTask, *idle_task;         /* none: the cache's waits let go of its lock and take it again */
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
void wait_prepare_intr(wait_queue_t *q) { (void)q; }
void wake_up_all(wait_queue_t *q) { (void)q; }
/* what another task does while this one sleeps in the cache (yield() is reached only when curTask is set) */
static void (*yield_hook)(void);
void yield(void) { if(yield_hook) yield_hook(); }
/* the clock moves 50 ms each time it is read, so a wait for a holder that never lets go ends (bsync()'s 10 s) */
static u_long clock_ms;
u_long uptime_ms(void) { return clock_ms += 50; }
int wait_on_timeout(wait_queue_t *q, u_long ms) { (void)q; clock_ms += ms; if(yield_hook) yield_hook(); return -1; }

void ktimer_init(struct ktimer *t, void (*fn)(void *), void *arg) { (void)t; (void)fn; (void)arg; }
void ktimer_start(struct ktimer *t, u_long ms, bool periodic) { (void)t; (void)ms; (void)periodic; }
void ktimer_cancel(struct ktimer *t) { (void)t; }
void kcritical_enter(void) { if(curTask) curTask->crit++; }   /* (fs/fat.c's lock) */
void kcritical_leave(void) { if(curTask && curTask->crit) curTask->crit--; }

static char klog[1 << 15];
static u32 klog_len;

static void log_line(const char *line) {
    u32 n = strlen(line);
    if(klog_len + n + 1 < sizeof(klog)) { memcpy(klog + klog_len, line, n + 1); klog_len += n; }
    if(verbose) printf("      [kprintf] %s", line);
}

void kprintf(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_line(line);
}

void klog_write(const char *s) { log_line(s); }

/* how many times what appears in the console since the log was emptied */
static int said(const char *what) {
    int n = 0;
    u32 l = strlen(what), i;
    for(i = 0; i + l <= klog_len; i++) if(!memcmp(klog + i, what, l)) n++;
    return n;
}

void *kalloc(size_t n) { return malloc(n); }
static int kcalloc_fail;                        /* the next this many kcalloc() calls fail (ENOMEM), */
static void (*kcalloc_hook)(void);              /* each after this ran (another task, while the caller is preempted) */
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
    t->year = 2026; t->month = 10; t->date = 24; t->hour = 12;
}

/**** RAM disks that refuse on request ****/

struct ram {
    struct bdev bd;
    u8 *mem;
    int fail_all;                       /* every write fails */
    u32 bad_lo, bad_hi;                 /* a write that touches blocks bad_lo..bad_hi fails (bad_hi 0: none) */
    u32 rbad, rbad2;                    /* a read that touches one of these blocks fails (0: none) */
    u32 fail_once;                      /* the write asked for as the nth (attempts) fails, the others go (0: none) */
    u32 cut_at;                         /* a pull: the nth write asked for and every later one fail, the nth storing */
    u32 cut_keep;                       /*      only its first cut_keep blocks (a request stopped in the middle) */
    void (*hook)(struct ram *r, u32 lba, u32 count);    /* after a write is stored: another task runs */
    void (*check)(struct ram *r);       /* a scenario's own oracle, after every write stored */
    u32 attempts, writes, failed, reads, read_blocks;
    int oracle;
    u32 violations;
    char violation[200];
};

static u32 zero_length_writes;
static void oracle_check(struct ram *r);

static int ram_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    struct ram *r = (struct ram *)d->priv;
    if(r->rbad && lba <= r->rbad && r->rbad < lba + count) return BDEV_EIO;
    if(r->rbad2 && lba <= r->rbad2 && r->rbad2 < lba + count) return BDEV_EIO;
    r->reads++;
    r->read_blocks += count;
    memcpy(buf, r->mem + lba * 512, count * 512);
    return 0;
}

static void stored(struct ram *r) {
    r->writes++;
    if(r->oracle) oracle_check(r);
    if(r->check) r->check(r);
}

static int ram_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct ram *r = (struct ram *)d->priv;
    r->attempts++;
    if(r->cut_at && r->attempts >= r->cut_at) {
        if(r->attempts == r->cut_at && r->cut_keep) {
            memcpy(r->mem + lba * 512, buf, (count < r->cut_keep ? count : r->cut_keep) * 512);
            stored(r);
        }
        r->failed++;
        return BDEV_EIO;
    }
    if(r->fail_all || (r->fail_once && r->attempts == r->fail_once) ||
       (r->bad_hi && lba <= r->bad_hi && lba + count - 1 >= r->bad_lo)) {
        r->failed++;
        return BDEV_EIO;
    }
    memcpy(r->mem + lba * 512, buf, count * 512);
    stored(r);
    if(r->hook) r->hook(r, lba, count);
    return 0;
}

static const struct bdev_ops ram_ops = { ram_read, ram_write, NULL };

static struct ram *made[32];
static int nmade;

static struct ram *ram_new(const char *name, u32 blocks) {
    struct ram *r = (struct ram *)calloc(1, sizeof(*r));
    if(!r) die("out of memory");
    if(nmade < 32) made[nmade++] = r;
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

/* a disk holding what x holds now (the next computer's view of a stick pulled out of x) */
static struct ram *ram_copy(const char *name, struct ram *x) {
    struct ram *r = ram_new(name, x->bd.nblocks);
    memcpy(r->mem, x->mem, x->bd.nblocks * 512);
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
int vfs_bdev_change_begin(struct bdev *d, const char **why) { (void)d; if(why) *why = NULL; return 0; }
void vfs_bdev_change_end(struct bdev *d) { (void)d; }
const char *bdev_busy_words(const char *busy) { return busy ? busy : ""; }

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

static void mkfs_bits(struct ram *r, int bits) {
    char msg[256];
    if(fat_mkfs(&r->bd, "TEST", bits, msg, sizeof(msg)) != 0) { printf("mkfs: %s\n", msg); die("mkfs failed"); }
}

static void mkfs(struct ram *r) { mkfs_bits(r, 0); }

static int mount_try(struct ram *r, struct superblock **out) {
    struct superblock *sb = (struct superblock *)calloc(1, sizeof(*sb));
    int e;
    sb->type = fat;
    sb->dev = &r->bd;
    mutex_init(&sb->lock, "fat");
    e = fat->mount(sb);
    if(e) { free(sb); return e; }
    *out = sb;
    return 0;
}

static struct superblock *mount(struct ram *r) {
    struct superblock *sb;
    if(mount_try(r, &sb) != 0) die("mount failed");
    return sb;
}

/* fs/vfs.c's write_back(), what close and fsync say: the file system's sync, then its device's blocks */
static int fsync_sb(struct superblock *sb) {
    int r = sb->ops->sync(sb);
    if(sb->dev->gone) return -EIO;
    if(r == 0 || bsync(sb->dev) == 0) return 0;
    return -EIO;
}

/* fs/vfs.c's vfs_umount_lost() from its sync on: the changed blocks it gave up */
static u_int umount_sb(struct superblock *sb) {
    struct bdev *d = sb->dev;
    u_int lost;
    if(sb->ops->sync) sb->ops->sync(sb);
    bsync(d);
    vput(sb->root);
    sb->ops->unmount(sb);
    lost = d->gone ? 0 : bdirty_count(d);
    binvalidate(d);
    free(sb);
    return lost;
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

/**** the image as the next computer reads it (FAT16 and FAT32) ****/

static u32 g16(const u8 *p) { return (u32)(p[0] | (p[1] << 8)); }
static u32 g32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

struct geo { u32 type, spc, fat_lba, spf, root_lba, root_secs, root_cluster, data_lba, nclusters; };

static void geometry(struct ram *r, struct geo *g) {
    const u8 *bs = r->mem;
    u32 rsv = g16(bs + 14), nfats = bs[16], re = g16(bs + 17);
    u32 tot = g16(bs + 19) ? g16(bs + 19) : g32(bs + 32);
    g->spc = bs[13] ? bs[13] : 1;
    g->spf = g16(bs + 22) ? g16(bs + 22) : g32(bs + 36);
    g->fat_lba = rsv;
    g->root_lba = rsv + nfats * g->spf;
    g->root_secs = (re * 32 + 511) / 512;
    g->data_lba = g->root_lba + g->root_secs;
    g->nclusters = tot > g->data_lba ? (tot - g->data_lba) / g->spc : 0;
    g->type = g->nclusters < 4085 ? 12 : g->nclusters < 65525 ? 16 : 32;
    g->root_cluster = g->type == 32 ? g32(bs + 44) : 0;
}

static u32 fat1(struct ram *r, const struct geo *g, u32 c) {
    if(g->type == 32) return g32(r->mem + g->fat_lba * 512 + c * 4) & 0x0FFFFFFF;
    return g16(r->mem + g->fat_lba * 512 + c * 2);
}
static int at_end(const struct geo *g, u32 v) { return g->type == 32 ? v >= 0x0FFFFFF8 : v >= 0xFFF8; }
static u32 clus_lba(const struct geo *g, u32 c) { return g->data_lba + (c - 2) * g->spc; }
static u32 entry_first(const struct geo *g, const u8 *p) { return g16(p + 26) | (g->type == 32 ? g16(p + 20) << 16 : 0); }
static int valid_cluster(const struct geo *g, u32 c) { return c >= 2 && c < g->nclusters + 2; }

/* the directory at cluster first (0: the root) as up to max raw entries, to its end marker or its chain's end; the
   count, or -1 when its chain runs into a free cluster */
static int img_dir(struct ram *r, const struct geo *g, u32 first, u8 *out, u32 max) {
    u32 n = 0, c, i, steps = 0;
    if(!first && g->type != 32) {
        for(i = 0; i < g->root_secs * 16 && n < max; i++) {
            const u8 *p = r->mem + g->root_lba * 512 + i * 32;
            if(!p[0]) break;
            memcpy(out + n++ * 32, p, 32);
        }
        return (int)n;
    }
    c = first ? first : g->root_cluster;
    while(valid_cluster(g, c) && steps++ < 4096) {
        u32 next = fat1(r, g, c);
        if(!next) return -1;
        for(i = 0; i < g->spc * 16 && n < max; i++) {
            const u8 *p = r->mem + clus_lba(g, c) * 512 + i * 32;
            if(!p[0]) return (int)n;
            memcpy(out + n++ * 32, p, 32);
        }
        if(at_end(g, next)) break;
        c = next;
    }
    return (int)n;
}

static int is_name(const u8 *p) { return p[0] != 0xE5 && p[11] != 0x0F && !(p[11] & 0x08); }
static int is_dot(const u8 *p) { return p[0] == '.' && (p[1] == ' ' || p[1] == '.'); }

static void violation(struct ram *r, const char *what) {
    if(!r->violations++) {
        snprintf(r->violation, sizeof(r->violation), "after the disk's write %u: %s", r->writes, what);
        if(verbose) printf("      ORACLE: %s\n", r->violation);
    }
}

#define DIR_MAX 2048
static u8 *dirbuf[6];

/* B3's oracle: no entry, nor a link on its chain, reaches a free cluster (directories to depth 5) */
static void check_dir(struct ram *r, const struct geo *g, u32 first, int depth) {
    int n, i;
    u8 *e;
    if(depth > 5) return;
    if(!dirbuf[depth] && !(dirbuf[depth] = (u8 *)malloc(DIR_MAX * 32))) die("out of memory");
    e = dirbuf[depth];
    n = img_dir(r, g, first, e, DIR_MAX);
    if(n < 0) { violation(r, "a directory's chain reaches a free cluster"); return; }
    for(i = 0; i < n; i++) {
        const u8 *p = e + i * 32;
        u32 c, steps = 0;
        char m[120], name[12];
        if(!is_name(p) || is_dot(p)) continue;
        memcpy(name, p, 11);
        name[11] = 0;
        for(c = entry_first(g, p); valid_cluster(g, c) && steps++ < g->nclusters; ) {
            u32 v = fat1(r, g, c);
            if(!v) {
                snprintf(m, sizeof(m), "entry '%s' -> cluster %u (step %u of its chain) is free", name, c, steps - 1);
                violation(r, m);
                break;
            }
            if((p[11] & 0x10) || at_end(g, v)) break;
            c = v;
        }
        if((p[11] & 0x10) && entry_first(g, p)) check_dir(r, g, entry_first(g, p), depth + 1);
        e = dirbuf[depth];                              /* (the recursion may have used deeper buffers) */
    }
}

static void oracle_check(struct ram *r) {
    struct geo g;
    geometry(r, &g);
    check_dir(r, &g, 0, 0);
}

/* the clusters the first FAT on the disk marks used, how many of them the tree's entries reach, and how many are
   reached twice (two names on one chain, or chains that run into each other) */
static u8 *seen;
static void census_dir(struct ram *r, const struct geo *g, u32 first, int depth, u32 *reached, u32 *twice) {
    u8 *e;
    int n, i;
    u32 c, steps;
    if(depth > 5) return;
    if(!first && g->type == 32) first = g->root_cluster;
    for(c = first, steps = 0; valid_cluster(g, c) && steps++ < g->nclusters; ) {   /* the directory's own chain */
        u32 v = fat1(r, g, c);
        if(seen[c]++) (*twice)++;
        else (*reached)++;
        if(!v || at_end(g, v)) break;
        c = v;
    }
    if(depth == 0 && g->type != 32) first = 0;
    if(!(e = (u8 *)malloc(DIR_MAX * 32))) die("out of memory");
    n = img_dir(r, g, depth == 0 && g->type == 32 ? 0 : first, e, DIR_MAX);
    for(i = 0; i < n; i++) {
        const u8 *p = e + i * 32;
        if(!is_name(p) || is_dot(p)) continue;
        if(p[11] & 0x10) { if(entry_first(g, p)) census_dir(r, g, entry_first(g, p), depth + 1, reached, twice); continue; }
        for(c = entry_first(g, p), steps = 0; valid_cluster(g, c) && steps++ < g->nclusters; ) {
            u32 v = fat1(r, g, c);
            if(seen[c]++) { (*twice)++; break; }
            (*reached)++;
            if(!v || at_end(g, v)) break;
            c = v;
        }
    }
    free(e);
}

static void census2(struct ram *r, u32 *used, u32 *reached, u32 *twice) {
    struct geo g;
    u32 c;
    geometry(r, &g);
    *used = *reached = *twice = 0;
    if(!(seen = (u8 *)calloc(g.nclusters + 2, 1))) die("out of memory");
    for(c = 2; c < g.nclusters + 2; c++) if(fat1(r, &g, c)) (*used)++;
    census_dir(r, &g, 0, 0, reached, twice);
    free(seen);
    seen = NULL;
}

static void census(struct ram *r, u32 *used, u32 *reached) {
    u32 twice;
    census2(r, used, reached, &twice);
    if(twice) *reached = 0xFFFFFFFF;                    /* a census that finds a chain twice never matches */
}

/* the bytes of the file named n11 (8.3, space padded) in the directory at cluster dir (0: the root) of the image, in
   out (up to max); its size, or -1 when there is no such name, -2 when its chain is broken, -3 with two such names */
static int img_file(struct ram *r, u32 dir, const char *n11, u8 *out, u32 max) {
    struct geo g;
    u8 *e = (u8 *)malloc(DIR_MAX * 32);
    const u8 *hit = NULL;
    int n, i, size;
    u32 c, got = 0, steps = 0;
    if(!e) die("out of memory");
    geometry(r, &g);
    n = img_dir(r, &g, dir, e, DIR_MAX);
    for(i = 0; i < n; i++) {
        const u8 *p = e + i * 32;
        if(!is_name(p) || memcmp(p, n11, 11)) continue;
        if(hit) { free(e); return -3; }
        hit = p;
    }
    if(!hit) { free(e); return -1; }
    size = (int)g32(hit + 28);
    for(c = entry_first(&g, hit); got < (u32)size && steps++ < g.nclusters; ) {
        u32 k = g.spc * 512, v;
        if(!valid_cluster(&g, c) || !(v = fat1(r, &g, c))) { free(e); return -2; }
        if(k > (u32)size - got) k = (u32)size - got;
        if(got + k <= max) memcpy(out + got, r->mem + clus_lba(&g, c) * 512, k);
        got += k;
        if(got < (u32)size && at_end(&g, v)) { free(e); return -2; }
        c = v;
    }
    free(e);
    return size;
}

/* the raw 32-byte short entry named n11 in the root of the image, or NULL */
static u8 *img_entry(struct ram *r, const char *n11, u8 *out) {
    struct geo g;
    u8 *e = (u8 *)malloc(DIR_MAX * 32), *hit = NULL;
    int n, i;
    if(!e) die("out of memory");
    geometry(r, &g);
    n = img_dir(r, &g, 0, e, DIR_MAX);
    for(i = 0; i < n && !hit; i++) if(is_name(e + i * 32) && !memcmp(e + i * 32, n11, 11)) { memcpy(out, e + i * 32, 32); hit = out; }
    free(e);
    return hit;
}

/* B9: the account of every disk of the scenario that ends */
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
    yield_hook = NULL;
    kcalloc_hook = NULL;
    kcalloc_fail = 0;
    curTask = NULL;
    if(verbose && title) printf("%s\n", title);
}

/* fsck.fat -n on the image (when dosfstools is there, as in the devenv): 0 when it finds nothing to fix, 1 when it
   does, -1 when it cannot be run */
static int have_fsck = -1;
static int fsck(struct ram *r, const char *name) {
    char path[128], cmd[300];
    int fd, st;
    if(have_fsck < 0) have_fsck = system("command -v fsck.fat >/dev/null 2>&1") == 0;
    if(!have_fsck) return -1;
    snprintf(path, sizeof(path), "%s/surfos-fatfs-%s.img", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", name);
    fd = open(path, HOST_O_WRONLY | HOST_O_CREAT | HOST_O_TRUNC, 0644);
    if(fd < 0) return -1;
    if(write(fd, r->mem, (unsigned long)r->bd.nblocks * 512) != (long)r->bd.nblocks * 512) { close(fd); unlink(path); return -1; }
    close(fd);
    snprintf(cmd, sizeof(cmd), "fsck.fat -n '%s' >/dev/null 2>&1", path);
    st = system(cmd);
    if(st != 0 && verbose) { snprintf(cmd, sizeof(cmd), "fsck.fat -n '%s'", path); system(cmd); }
    unlink(path);
    return st == 0 ? 0 : 1;
}

static void fsck_clean(struct ram *r, const char *name, const char *what) {
    int f = fsck(r, name);
    if(f >= 0) check(f == 0, what);
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
    check(umount_sb(sb) == 0, "T1: unmounted, nothing given up");
    fsck_clean(x, "t1", "T1: fsck.fat finds nothing to fix");
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
    r = bsync(&x->bd);                              /* as an eviction or another sync does: no sb->lock held */
    check(hook_ran && r == 0, "T2: bsync() with another task's create in the middle");
    check(x->violations == 0, "T2 (B7): nothing on the disk reaches a free cluster while another task writes during a sync");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    r = fsync_sb(sb);
    fill(pat2, 512, 9);
    check(r == 0 && file_is(sub, "late", pat2, 512) && x->violations == 0, "T2: the next sync writes the rest, in order");
    vput(sub);
    umount_sb(sb);
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
    umount_sb(sb);
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
    umount_sb(sb);
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
    check(off < 2u << 20 && wrote <= BCACHE_HOLD * 512 + 16384,
          "T5 (B6): writes to the refusing card stop (EIO) once it holds about a quarter of the cache");
    before = x->failed + x->writes;
    for(i = 0; i < 64; i++) if((b = bread(&y->bd, i * 8)) != NULL) { ok++; brelse(b); }
    check(ok == 64, "T5 (B6): another disk is still read through the cache");
    check(x->failed + x->writes == before, "T5 (B6): reading another disk tries no write to the refusing card");
    check(fsync_sb(sb) == -EIO, "T5: fsync of the refusing card says EIO");
    vput(v);
    check(umount_sb(sb) > 0, "T5 (B10): its umount gives up what the card refused, and says how much");
    check(bdirty_count(&x->bd) == 0, "T5 (B40): and nothing of the card is left dirty in the cache");
}

/**** T6 (B6, B13): one sector that refuses writes must not hold back its neighbours ****/

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
    check(good == 20, "T6 (B6, B13): the 20 data blocks next to the bad one reach the disk (a failed run is tried block by block)");
    x->bad_hi = 0;
    check(fsync_sb(sb) == 0 && x->violations == 0, "T6: once the sector takes writes, sync writes the rest");
    umount_sb(sb);
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
    umount_sb(sb);
}

/**** T8 (B6, B14): a card that refused for a while and works again; another disk is read ****/

static void t8(void) {
    struct ram *x = ram_new("t8x", 8192), *y = ram_new("t8y", 8192);
    struct buf *b;
    u32 i;
    fresh("T8: the card refuses for a while, then works again");
    x->fail_all = 1;
    b = bget(&x->bd, 10);
    bdirty_order(b, BORDER_TABLE);                  /* a FAT block, dirtied first */
    brelse(b);
    bsync(&x->bd);                                  /* refused: the card's blocks fail */
    for(i = 0; i < 1100; i++) {
        b = bget(&x->bd, 100 + i);
        if(!b) break;
        bdirty(b);
        brelse(b);
    }
    if(verbose) printf("      data blocks dirtied while the card refused: %u\n", i);
    check(i <= BCACHE_HOLD + 8, "T8 (B6): bget() for the refusing card stops at about a quarter of the cache");
    x->fail_all = 0;
    b = bread(&y->bd, 0);
    check(b != NULL, "T8 (B6): with the card working again, another disk is read without a sync first");
    if(b) brelse(b);
    check(bcache_room(&x->bd) == 0, "T8 (B14): asked again, the card that works again takes writes at once");
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
    umount_sb(sb);
    fsck_clean(x, "t9", "T9: fsck.fat finds nothing to fix");
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
    check(bsync(&x->bd) == 0, "T10 (B1): the failed block was kept: the sync once the card works writes it");
}

/**** T11 (B8): the block being evicted is dropped while what it needs is written ****/

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
    check(bdirty_count(&x->bd) == 0, "T11 (B40): nothing of the dropped volume is left dirty");
}

/**** T12 (B6): a block that was never tried is not marked as failed ****/

static void t12(void) {
    struct ram *x = ram_new("t12x", 8192), *y = ram_new("t12y", 8192);
    struct buf *b;
    u32 i, before;
    fresh("T12: untried is not failed");
    b = bget(&x->bd, 200); bdirty(b); brelse(b);                         /* data, changed first */
    b = bget(&x->bd, 300); bdirty_order(b, BORDER_DIR); brelse(b);        /* and an entry block that may point at it */
    b = bpeek(&x->bd, 200); brelse(b);
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
    umount_sb(sb);
    fsck_clean(x, "t14", "T14: fsck.fat finds nothing to fix");
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
    umount_sb(sb);
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
    umount_sb(sb);
}

/**** B4: a write that stops early gives its clusters back ****/

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
    umount_sb(sb);
    fsck_clean(x, "b4", "B4: fsck.fat finds nothing to fix");
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
    umount_sb(sb);
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
    umount_sb(sb);
}

/**** S1-S2 (x86's own): a save, as edit, the desktop and rx make one: a new file beside the old, its bytes, fsync,
      close, a rename over the old file, the sync after it (fs/vfs.c's) ****/

#define SAVE_NAME "notes.txt"
#define SAVE_TMP  "notes.txt.tmp"
#define SAVE_N11  "NOTES   TXT"
#define OLD_LEN   3000                              /* six clusters of 512 bytes (FAT16 and FAT32 here) */
#define NEW_LEN   21000                             /* 42: a straight write of 32 blocks and the rest through the cache */

static u8 old_bytes[OLD_LEN], new_bytes[NEW_LEN], img_buf[65536];
static const char *save_what;
static u32 save_bad;                                /* the oracle's failures in this run */

/* the save oracle, after every write stored: the name names a whole file, the old one or the new */
static void save_check(struct ram *r) {
    int n = img_file(r, 0, SAVE_N11, img_buf, sizeof(img_buf));
    int ok = (n == OLD_LEN && !memcmp(img_buf, old_bytes, OLD_LEN)) || (n == NEW_LEN && !memcmp(img_buf, new_bytes, NEW_LEN));
    if(!ok && !save_bad++ && verbose) printf("      SAVE ORACLE (%s): after the disk's write %u the name holds %d bytes%s\n",
                                             save_what, r->writes, n, n > 0 ? " that are neither the old file nor the new" : "");
}

/* the save, as edit makes it, at the file system: 0 when it says the file is saved; what it does on each failure is
   edit's (user/bin/edit.c): a new file it could not write, fsync or close is deleted again, and after a rename that
   failed it keeps both names */
static int save(struct superblock *sb, u32 len, int *renamed) {
    struct vnode *root = sb->root, *v;
    int r = root->ops->create(root, SAVE_TMP, VN_FILE, &v), w, s;
    *renamed = 0;
    if(r) return r;
    w = v->ops->write(v, 0, new_bytes, 1000);       /* edit writes its text in pieces */
    if(w == 1000) w = v->ops->write(v, 1000, new_bytes + 1000, len - 1000);
    s = fsync_sb(sb);                               /* fsync() */
    vput(v);
    if(!s) s = fsync_sb(sb);                        /* close(): the VFS writes it back again */
    if(w != (int)(len - 1000) || s) {
        root->ops->unlink(root, SAVE_TMP);
        fsync_sb(sb);
        return w < 0 ? w : s ? s : -EIO;
    }
    r = root->ops->rename(root, SAVE_TMP, root, SAVE_NAME);
    if(r == 0) *renamed = 1;
    fsync_sb(sb);                                   /* fs/vfs.c's sync after a rename (its result is not the rename's) */
    return r;
}

/* the volume a save starts from: notes.txt with the old bytes, synced, unmounted (crowd: files that put the new
   name's slots in another directory block than the old file's) */
static struct ram *save_volume(const char *name, int bits, int crowd) {
    struct ram *x = ram_new(name, bits == 32 ? 70000 : 8192);
    struct superblock *sb;
    char n[16];
    int i;
    mkfs_bits(x, bits);
    sb = mount(x);
    if(put_file(sb->root, SAVE_NAME, old_bytes, OLD_LEN)) die("save: the old file");
    for(i = 0; i < crowd; i++) {
        snprintf(n, sizeof(n), "f%02d", i);
        if(put_file(sb->root, n, NULL, 0)) die("save: crowd");
    }
    if(fsync_sb(sb) || umount_sb(sb)) die("save: setup");
    return x;
}

/* S1: the save pulled out at each write it makes (each write the stick stores of it is a point it could be pulled
   at: the save oracle looks at every one), and also in the middle of each multi-block request. After the pull the
   stick is read as the next computer reads it: the old file or the new under its name, whole; nothing reaching a
   free cluster; no cluster reached twice (two names on one chain) */
static void s1(const char *title, int bits, int crowd) {
    struct ram *tmpl, *x, *y;
    struct superblock *sb;
    u32 total, k, keep, used, reached, twice, runs = 0, bad = 0, old_seen = 0, new_seen = 0;
    int renamed, r;
    char what[96];
    fresh(title);
    fill(old_bytes, OLD_LEN, 41);
    fill(new_bytes, NEW_LEN, 42);
    tmpl = save_volume("s1t", bits, crowd);
    x = ram_copy("s1x", tmpl);                      /* the whole save once, to count its writes */
    sb = mount(x);
    x->attempts = 0;
    x->oracle = 1;
    x->check = save_check;
    save_what = title;
    save_bad = 0;
    r = save(sb, NEW_LEN, &renamed);
    total = x->attempts;
    check(r == 0 && renamed && img_file(x, 0, SAVE_N11, img_buf, sizeof(img_buf)) == NEW_LEN &&
          !memcmp(img_buf, new_bytes, NEW_LEN), "S1: the save works on a stick that takes every write");
    check(x->violations == 0 && save_bad == 0, "S1 (B3): at every write of the save, nothing reaches a free cluster and the name holds a whole file");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    check(img_file(x, 0, "NOTES~1 TMP", img_buf, sizeof(img_buf)) == -1, "S1: the new file's own name is gone after the save");
    umount_sb(sb);
    census2(x, &used, &reached, &twice);
    check(used == reached && !twice, "S1: after the save, every used cluster is the file's, once");
    fsck_clean(x, "s1", "S1: fsck.fat finds nothing to fix after the save");
    for(k = 1; k <= total; k++) {
        for(keep = 0; keep <= 1; keep++) {
            fresh(NULL);
            x = ram_copy("s1p", tmpl);
            sb = mount(x);
            x->attempts = 0;
            x->cut_at = k;
            x->cut_keep = keep;
            x->oracle = 1;
            x->check = save_check;
            snprintf(what, sizeof(what), "%s, pulled at write %u%s", title, k, keep ? " after one block of it" : "");
            save_what = what;
            save_bad = 0;
            save(sb, NEW_LEN, &renamed);
            x->check = NULL;
            x->oracle = 0;
            runs++;
            y = ram_copy("s1n", x);                 /* the next computer: the stick as the pull left it */
            r = img_file(y, 0, SAVE_N11, img_buf, sizeof(img_buf));
            if(r == OLD_LEN && !memcmp(img_buf, old_bytes, OLD_LEN)) old_seen++;
            else if(r == NEW_LEN && !memcmp(img_buf, new_bytes, NEW_LEN)) new_seen++;
            else save_bad++;
            census2(y, &used, &reached, &twice);
            if(x->violations || save_bad || twice) {
                if(!bad++) printf("      %s: %s%s%s\n", what, x->violations ? x->violation : "",
                                  save_bad ? " (the name does not hold a whole file)" : "", twice ? " (a cluster reached twice)" : "");
            }
            x->cut_at = 0;                          /* the stick back, for the unmount's sake (its writes go nowhere) */
            x->bd.gone = true;
            umount_sb(sb);
            free(x->mem); free(y->mem);
            x->mem = y->mem = NULL;
        }
    }
    if(verbose) printf("      %u pulls: the old file %u times, the new one %u times\n", runs, old_seen, new_seen);
    check(bad == 0 && old_seen && new_seen, "S1: pulled at any write of the save, the stick holds the old file or the new under its name, whole, nothing on free clusters, no cluster twice");
}

/* S2: the save with one write refused (each write in turn), then the stick works again; and with every write from
   the nth on refused (a stick that stops taking writes), read as the next computer reads it. Edit's reactions are
   save()'s. The name holds the new file when the save said so, else the old one; nothing lost, no cluster twice */
static void s2(const char *title, int bits, int crowd, int for_good) {
    struct ram *tmpl, *x;
    struct superblock *sb;
    u32 total, k, used, reached, twice, bad = 0, saved = 0, failed = 0;
    int renamed, r, n;
    char what[96];
    fresh(title);
    fill(old_bytes, OLD_LEN, 43);
    fill(new_bytes, NEW_LEN, 44);
    tmpl = save_volume("s2t", bits, crowd);
    x = ram_copy("s2x", tmpl);
    sb = mount(x);
    x->attempts = 0;
    save(sb, NEW_LEN, &renamed);
    total = x->attempts + 2;
    umount_sb(sb);
    for(k = 1; k <= total; k++) {
        fresh(NULL);
        x = ram_copy("s2r", tmpl);
        sb = mount(x);
        x->attempts = 0;
        if(for_good) x->cut_at = k;
        else x->fail_once = k;
        x->oracle = 1;
        snprintf(what, sizeof(what), "%s, write %u refused%s", title, k, for_good ? " and every one after it" : "");
        r = save(sb, NEW_LEN, &renamed);
        if(r == 0) saved++;
        else failed++;
        x->cut_at = x->fail_once = 0;               /* the stick works again (for_good: what it holds is all there is) */
        if(!for_good) {
            fsync_sb(sb);
            umount_sb(sb);
        } else {
            struct ram *y = ram_copy("s2n", x);
            x->bd.gone = true;
            umount_sb(sb);
            free(x->mem);
            x = y;
        }
        n = img_file(x, 0, SAVE_N11, img_buf, sizeof(img_buf));
        census2(x, &used, &reached, &twice);
        {
            int is_old = n == OLD_LEN && !memcmp(img_buf, old_bytes, OLD_LEN), is_new = n == NEW_LEN && !memcmp(img_buf, new_bytes, NEW_LEN);
            /* refused once: the stick works again, so it holds what the save said (new when it said 0); stopped for good:
               the old file or the new, whichever its last write left (a save that failed may have put the new one in
               place before a later write was refused) */
            int ok = for_good ? (is_old || is_new) && (r != 0 || is_new) : (r == 0 ? is_new : is_old);
            if(x->violations || twice || (!for_good && used != reached) || !ok) {
                if(!bad++) printf("      %s: save %d, the name %d bytes, %u used %u reached %u twice %s\n", what, r, n, used,
                                  reached, twice, x->violations ? x->violation : "");
            }
        }
        free(x->mem);
        x->mem = NULL;
    }
    if(verbose) printf("      %u runs: saved %u, failed %u\n", total, saved, failed);
    check(bad == 0 && saved && failed, for_good ? "S2b: a stick that stops taking writes at any write of a save holds the old file or the new under its name, nothing twice"
                                                : "S2a: a save with any one write refused says so, the name keeps the old file then (else the new), nothing lost or twice");
}

/**** S3 (the ROADMAP's 10/17/26 note): twins, two names on one chain, as a rename cut short leaves them ****/

/* the short entry of `name` in the root (its slot in the cache), copied into a free slot under the 8.3 name n11: two
   names on one chain, as an interrupted rename (or another system) leaves them */
static void make_twin(struct superblock *sb, struct ram *x, const char *n11) {
    struct geo g;
    struct buf *b;
    u8 raw[32];
    u32 i;
    if(fsync_sb(sb)) die("twin: sync");
    geometry(x, &g);
    if(!img_entry(x, "A       TXT", raw)) die("twin: no a.txt");
    memcpy(raw, n11, 11);
    for(i = 0; i < g.root_secs * 16; i++) {
        u32 lba = g.root_lba + i / 16, off = (i % 16) * 32;
        b = bread(&x->bd, lba);
        if(!b) die("twin: read");
        if(b->data[off] == 0 || b->data[off] == 0xE5) {
            memcpy(b->data + off, raw, 32);
            bdirty_order(b, BORDER_DIR);
            brelse(b);
            if(fsync_sb(sb)) die("twin: sync 2");
            return;
        }
        brelse(b);
    }
    die("twin: no free slot");
}

static void s3(void) {
    struct ram *x;
    struct superblock *sb;
    struct vnode *root, *v;
    u32 used, reached, twice;
    int r;
    fresh("S3: twins");
    fill(pat, 5000, 51);
    fill(pat2, 7000, 52);

    x = ram_new("s3a", 8192);                       /* rm of one name: the other keeps the chain */
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    if(put_file(root, "a.txt", pat, 5000)) die("S3: a.txt");
    make_twin(sb, x, "B       TXT");
    x->oracle = 1;
    r = root->ops->unlink(root, "b.txt");
    fsync_sb(sb);
    fill(pat2, 7000, 53);
    put_file(root, "c.txt", pat2, 7000);            /* a new file takes free clusters: none of a's */
    fsync_sb(sb);
    check(r == 0 && file_is(root, "a.txt", pat, 5000) && file_is(root, "c.txt", pat2, 7000),
          "S3a: rm of one of two names on a chain keeps the chain for the other (a new file does not take it)");
    census2(x, &used, &reached, &twice);
    check(used == reached && !twice && x->violations == 0, "S3a: nothing lost, nothing twice, nothing on free clusters");
    umount_sb(sb);
    fsck_clean(x, "s3a", "S3a: fsck.fat finds nothing to fix once one name is left");

    fresh(NULL);                                    /* mv over one name: the other keeps the chain */
    x = ram_new("s3b", 8192);
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    if(put_file(root, "a.txt", pat, 5000) || put_file(root, "n.txt", pat2, 7000)) die("S3: files");
    make_twin(sb, x, "B       TXT");
    x->oracle = 1;
    r = root->ops->rename(root, "n.txt", root, "b.txt");
    fsync_sb(sb);
    put_file(root, "c.txt", pat2, 3000);
    fsync_sb(sb);
    check(r == 0 && file_is(root, "a.txt", pat, 5000) && file_is(root, "b.txt", pat2, 7000),
          "S3b: mv over one of two names keeps the chain for the other");
    census2(x, &used, &reached, &twice);
    check(used == reached && !twice && x->violations == 0, "S3b: nothing lost, nothing twice");
    umount_sb(sb);

    fresh(NULL);                                    /* the twins renamed onto each other: one name, the chain kept */
    x = ram_new("s3c", 8192);
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    if(put_file(root, "a.txt", pat, 5000)) die("S3: a.txt");
    make_twin(sb, x, "B       TXT");
    r = root->ops->rename(root, "b.txt", root, "a.txt");
    fsync_sb(sb);
    put_file(root, "c.txt", pat2, 7000);
    fsync_sb(sb);
    v = lookup(root, "b.txt");
    check(r == 0 && !v && file_is(root, "a.txt", pat, 5000), "S3c: mv of a twin onto the other: one name, its file whole");
    if(v) vput(v);
    census2(x, &used, &reached, &twice);
    check(used == reached && !twice, "S3c: nothing lost, nothing twice");
    umount_sb(sb);
    fsck_clean(x, "s3c", "S3c: fsck.fat finds nothing to fix");

    fresh(NULL);                                    /* truncate: to 0 lets go, to a smaller size refuses */
    x = ram_new("s3d", 8192);
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    if(put_file(root, "a.txt", pat, 5000)) die("S3: a.txt");
    make_twin(sb, x, "B       TXT");
    v = lookup(root, "b.txt");
    r = v->ops->truncate(v, 1000);
    check(r == -EBUSY && v->size == 5000, "S3d: a truncate of a twin to a smaller size says EBUSY (the other's bytes may be past the cut)");
    r = v->ops->truncate(v, 0);
    vput(v);
    fsync_sb(sb);
    put_file(root, "c.txt", pat2, 7000);
    fsync_sb(sb);
    check(r == 0 && file_is(root, "b.txt", pat, 0) && file_is(root, "a.txt", pat, 5000),
          "S3d: a truncate of a twin to 0 lets go of the chain, which the other keeps");
    census2(x, &used, &reached, &twice);
    check(used == reached && !twice, "S3d: nothing lost, nothing twice");
    umount_sb(sb);

    fresh(NULL);                                    /* a write that stops early on a twin does not cut the chain */
    x = ram_new("s3e", 8192);
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    if(put_file(root, "a.txt", pat, 5000)) die("S3: a.txt");
    make_twin(sb, x, "B       TXT");
    v = lookup(root, "b.txt");
    x->fail_all = 1;
    r = v->ops->write(v, 5000, pat2, 40000);        /* an append: what fits in the last cluster, then refused */
    x->fail_all = 0;
    check(r >= 0 && r < 40000 && v->size == 5000 + (u32)r, "S3e: an append to a twin that the card refuses comes up short");
    if(v->ops->read(v, 0, img_buf, 5000) != 5000 || memcmp(img_buf, pat, 5000)) r = -1;
    vput(v);
    fsync_sb(sb);
    check(r >= 0 && file_is(root, "a.txt", pat, 5000), "S3e: and leaves both names' bytes as they were (the chain is not cut)");
    census2(x, &used, &reached, &twice);
    check(used == reached && twice, "S3e: nothing lost (the chain still twice: the two names)");
    umount_sb(sb);
}

/**** S4 (x86's own): a rename over a file changes that file's entry in place ****/

static u8 s4_old[32];
static int s4_dup;
static void s4_check(struct ram *r) {                 /* at no write does the directory hold the target's name twice */
    if(img_file(r, 0, "N       TXT", img_buf, sizeof(img_buf)) == -3) s4_dup++;
}

static void s4(void) {
    struct ram *x;
    struct superblock *sb;
    struct vnode *root;
    struct geo g;
    u8 now[32];
    u32 used, reached, twice;
    int r;
    fresh("S4: a rename over a file, in place");
    fill(pat, 5000, 61);
    fill(pat2, 9000, 62);
    x = ram_new("s4", 8192);
    mkfs(x);
    sb = mount(x);
    root = sb->root;
    geometry(x, &g);
    if(put_file(root, "n.txt", pat, 5000) || put_file(root, "Long File Name.txt", pat2, 9000) || fsync_sb(sb)) die("S4: setup");
    if(!img_entry(x, "N       TXT", s4_old)) die("S4: entry");
    x->oracle = 1;
    x->check = s4_check;
    r = root->ops->rename(root, "Long File Name.txt", root, "n.txt");
    fsync_sb(sb);
    x->check = NULL;
    check(r == 0 && file_is(root, "n.txt", pat2, 9000) && !lookup(root, "Long File Name.txt"), "S4: mv over n.txt, the moved file under the name");
    check(img_entry(x, "N       TXT", now) && !memcmp(now, s4_old, 11) && now[12] == s4_old[12] && g32(now + 28) == 9000,
          "S4: the replaced entry keeps its name and case flags, and takes the moved file's size");
    check(s4_dup == 0, "S4: at no write is the name in the directory twice");
    census2(x, &used, &reached, &twice);
    check(used == reached && !twice && x->violations == 0, "S4: the replaced file's clusters are free, nothing lost");
    umount_sb(sb);
    fsck_clean(x, "s4", "S4: fsck.fat finds nothing to fix");
}

/**** S5 (B10, B40): an unmount while another task's write is under way, and one that gives up what a card refused ****/

static struct ram *s5_dev;
static void s5_hook(struct ram *r, u32 lba, u32 count) {   /* another console's umount, after its own sync, meanwhile */
    (void)lba; (void)count;
    r->hook = NULL;
    binvalidate(&s5_dev->bd);
}

static void s5(void) {
    struct ram *x;
    struct superblock *sb;
    struct vnode *v;
    struct buf *b;
    u32 lba, cached = 0, used, reached;
    u_int lost;
    fresh("S5: unmounts meanwhile");
    x = ram_new("s5a", 8192);
    mkfs(x);
    sb = mount(x);
    fill(pat, 4096, 71);
    if(put_file(sb->root, "a", pat, 4096) || fsync_sb(sb)) die("S5: setup");
    v = lookup(sb->root, "a");
    fill(pat2, 2048, 72);
    v->ops->write(v, 0, pat2, 2048);                /* dirty blocks of the volume */
    vput(v);
    s5_dev = x;
    x->oracle = 1;
    x->hook = s5_hook;                              /* the drop comes while a sync writes them */
    bsync(&x->bd);
    for(lba = 0; lba < x->bd.nblocks; lba++) if((b = bpeek(&x->bd, lba)) != NULL) { cached++; brelse(b); }
    check(cached == 0 && bdirty_count(&x->bd) == 0, "S5a (B40): a drop while the volume's blocks are written leaves none of them cached");
    check(x->violations == 0, "S5a: what reached the disk before the drop is consistent");
    free(sb);                                       /* (the mount is gone with it; a fresh look at the stick) */
    fresh(NULL);
    sb = mount(x);
    v = lookup(sb->root, "a");
    check(v && v->size == 4096 && v->ops->read(v, 0, img_buf, 4096) == 4096, "S5a: the file reads back afterwards, its size as on the disk");
    if(v) vput(v);
    umount_sb(sb);

    fresh(NULL);
    x = ram_new("s5b", 8192);
    mkfs(x);
    sb = mount(x);
    fill(pat, 4096, 73);
    if(put_file(sb->root, "keep", pat, 4096) || fsync_sb(sb)) die("S5: setup 2");
    x->fail_all = 1;
    fill(pat2, 3000, 74);
    put_file(sb->root, "new", pat2, 3000);
    lost = umount_sb(sb);
    check(lost > 0 && bdirty_count(&x->bd) == 0, "S5b (B10): an umount with the card refusing says how many changed blocks it gave up, and keeps none");
    x->fail_all = 0;
    x->oracle = 1;
    fresh(NULL);
    sb = mount(x);
    check(file_is(sb->root, "keep", pat, 4096), "S5b: the file that was there before is whole");
    umount_sb(sb);
    census(x, &used, &reached);
    check(used == reached && x->violations == 0, "S5b: what reached the disk is consistent");
}

/**** S6 (B16, B28): a file whose block refuses can be removed ****/

static void s6(void) {
    struct ram *x;
    struct superblock *sb;
    struct vnode *v;
    struct geo g;
    u32 used, reached;
    int r;
    fresh("S6: rm of a file whose block refuses");
    x = ram_new("s6", 8192);
    mkfs(x);
    sb = mount(x);
    geometry(x, &g);
    fill(pat, 4096, 81);
    if(put_file(sb->root, "keep", pat, 4096) || fsync_sb(sb)) die("S6: setup");
    if(sb->root->ops->create(sb->root, "bad", VN_FILE, &v)) die("S6: create");
    x->bad_lo = x->bad_hi = clus_lba(&g, 2 + 8 + 2);  /* a block of bad's third cluster */
    fill(pat2, 6000, 82);
    v->ops->write(v, 0, pat2, 6000);
    vput(v);
    check(fsync_sb(sb) == -EIO, "S6: the sync says EIO while the block refuses");
    x->oracle = 1;
    r = sb->root->ops->unlink(sb->root, "bad");
    check(r == 0, "S6 (B16): rm of the file whose block refuses works");
    check(fsync_sb(sb) == 0, "S6 (B28): and nothing of it is owed afterwards: the sync works");
    x->bad_hi = 0;
    check(file_is(sb->root, "keep", pat, 4096), "S6: the other file is whole");
    umount_sb(sb);
    census(x, &used, &reached);
    check(used == reached && x->violations == 0, "S6: nothing lost, nothing on free clusters");
    fsck_clean(x, "s6", "S6: fsck.fat finds nothing to fix");
}

/**** S7 (B24, B42): an undo that cannot be done leaves the volume read-only and marked; the mark stays ****/

/* while the move's first order point writes t's block: s's and t's blocks leave the cache and cannot be read again,
   so the old name cannot be deleted, nor the new one taken back */
static u32 s7_s, s7_t;
static struct ram *s7_y;
static void s7_hook(struct ram *r, u32 lba, u32 count) {
    u32 i;
    struct buf *b;
    if(!(lba <= s7_t && s7_t < lba + count)) return;
    r->hook = NULL;
    /* 10/31/26: another task's reads make room (bdiscard() drops only dirty blocks since the two lines were joined) */
    for(i = 0; i < 2100; i++) if((b = bread(&s7_y->bd, i)) != NULL) brelse(b);
    r->rbad = s7_s;
    r->rbad2 = s7_t;
}

static void s7(void) {
    struct ram *x;
    s7_y = ram_new("s7y", 8192);
    struct superblock *sb;
    struct vnode *s, *t;
    struct geo g;
    u8 raw[32];
    int r;
    fresh("S7: an undo that cannot be done");
    x = ram_new("s7", 8192);
    mkfs(x);
    sb = mount(x);
    geometry(x, &g);
    if(sb->root->ops->create(sb->root, "s", VN_DIR, &s) || sb->root->ops->create(sb->root, "t", VN_DIR, &t)) die("S7: dirs");
    fill(pat, 2000, 91);
    if(put_file(s, "a", pat, 2000) || fsync_sb(sb)) die("S7: setup");
    if(!img_entry(x, "S          ", raw)) die("S7: s");
    s7_s = clus_lba(&g, entry_first(&g, raw));
    if(!img_entry(x, "T          ", raw)) die("S7: t");
    s7_t = clus_lba(&g, entry_first(&g, raw));
    x->hook = s7_hook;
    r = s->ops->rename(s, "a", t, "a");
    x->rbad = x->rbad2 = 0;
    check(r == -EIO, "S7: mv s/a t/a whose old name cannot be deleted nor the new one taken back: EIO");
    check(said("could not be undone") == 1, "S7 (B24): the console says the volume could not be put back");
    check(put_file(sb->root, "b", pat, 100) == -EROFS && sb->root->ops->unlink(sb->root, "s") == -EROFS,
          "S7 (B24): from then on every change says EROFS");
    vput(s);
    vput(t);
    umount_sb(sb);
    check(said("damaged (an undo failed), the volume stays marked as not cleanly unmounted") == 1,
          "S7 (B24, B42): the unmount leaves it marked as not cleanly unmounted, and says why");
    fresh(NULL);
    sb = mount(x);
    check(said("s7 was not unmounted cleanly") == 1, "S7: the next mount sees the mark");
    put_file(sb->root, "c", pat, 100);
    umount_sb(sb);
    check(said("it was not unmounted cleanly before this mount, and only a check clears that, the volume stays marked") == 1,
          "S7 (B42): a session on a volume that was not clean when it was mounted leaves the mark, and says why");
    fresh(NULL);
    sb = mount(x);
    vput(sb->root); sb->ops->unmount(sb); free(sb);
    check(said("s7 was not unmounted cleanly") == 1, "S7 (B42): the mark is still there for the next computer");

    fresh(NULL);                                    /* a clean volume stays clean */
    x = ram_new("s7b", 8192);
    mkfs(x);
    sb = mount(x);
    put_file(sb->root, "b", pat, 100);
    umount_sb(sb);
    fresh(NULL);
    sb = mount(x);
    umount_sb(sb);
    check(said("not unmounted cleanly") == 0, "S7: a volume unmounted cleanly mounts clean");
}

/**** S8 (B22, B15): a straight write the card refuses changes nothing in the cache ****/

static void s8(void) {
    struct ram *x;
    struct buf *b;
    u8 blk[512];
    u32 i, same = 0;
    int r;
    fresh("S8: a refused straight write");
    x = ram_new("s8", 4096);
    for(i = 0; i < 64; i++) {                       /* cached, clean, holding what the disk holds */
        memset(x->mem + (100 + i) * 512, 0x11, 512);
        if((b = bread(&x->bd, 100 + i)) != NULL) brelse(b);
    }
    fill(pat, 64 * 512, 101);
    x->cut_at = 1;                                  /* the request stops after 8 of its blocks */
    x->cut_keep = 8;
    r = bwrite_blocks(&x->bd, 100, 64, pat);
    x->cut_at = 0;
    for(i = 0; i < 64; i++) {
        if(bdev_read(&x->bd, 100 + i, 1, blk) != 0) die("S8: read");
        b = bread(&x->bd, 100 + i);
        if(b && !memcmp(b->data, blk, 512)) same++;
        if(b) brelse(b);
    }
    check(r != 0, "S8: the refused write says so");
    check(same == 64, "S8 (B22): what is read through the cache afterwards is what the disk holds, block by block");
    check(bdirty_count(&x->bd) == 0, "S8 (B15): the refused write left nothing dirty");
}

/**** S9 (B23): a write past the end whose gap of zeros the card refuses ****/

static void s9(void) {
    struct ram *x;
    struct superblock *sb;
    struct vnode *v;
    u32 used, reached;
    int r;
    fresh("S9: a gap refused");
    x = ram_new("s9", 8192);
    mkfs(x);
    sb = mount(x);
    fill(pat, 512, 111);
    if(put_file(sb->root, "f", pat, 512) || fsync_sb(sb)) die("S9: setup");
    v = lookup(sb->root, "f");
    x->fail_all = 1;
    r = v->ops->write(v, 300000, pat, 1);
    x->fail_all = 0;
    check(r == -EIO && v->size == 512, "S9 (B23): a write past the end whose gap is refused says EIO, and the size stays");
    vput(v);
    check(fsync_sb(sb) == 0 && file_is(sb->root, "f", pat, 512), "S9: the file reads back as before");
    census(x, &used, &reached);
    check(used == reached && used == 1, "S9: its one cluster, nothing lost");
    umount_sb(sb);
}

/**** S10: FAT32 (the sticks are): the oracle at every write of ordinary work ****/

static void s10(void) {
    struct ram *x;
    struct superblock *sb;
    struct vnode *root, *d;
    u32 used, reached, twice;
    int r = 0, i;
    char name[16];
    fresh("S10: FAT32");
    x = ram_new("s10", 70000);
    mkfs_bits(x, 32);
    sb = mount(x);
    root = sb->root;
    x->oracle = 1;
    if(root->ops->create(root, "dir", VN_DIR, &d)) die("S10: mkdir");
    fill(pat, 65536, 121);
    for(i = 0; i < 40 && !r; i++) { snprintf(name, sizeof(name), "file number %d", i); r = put_file(d, name, pat, 1000 + i * 700); }
    if(!r) r = big_file(root, "big", pat, 300000);
    if(!r) r = fsync_sb(sb);
    for(i = 0; i < 40 && !r; i += 2) { snprintf(name, sizeof(name), "file number %d", i); r = d->ops->unlink(d, name); }
    if(!r) r = root->ops->rename(root, "big", d, "moved");
    if(!r) r = fsync_sb(sb);
    check(r == 0, "S10: files made, removed and moved on FAT32");
    check(x->violations == 0, "S10 (B3, B7): at every write, nothing on the disk reaches a free cluster");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    vput(d);
    umount_sb(sb);
    census2(x, &used, &reached, &twice);
    check(used == reached && !twice, "S10: nothing lost, nothing twice");
    fsck_clean(x, "s10", "S10: fsck.fat finds nothing to fix");
}

/**** B18: a block whose holder never lets go (a task killed at once in the middle of a change) ****/

static void b18(void) {
    struct ram *x = ram_new("b18", 4096);
    struct buf *b, *h;
    u_long t0;
    int r;
    fresh("B18: a holder that never lets go");
    b = bget(&x->bd, 40);
    bdirty(b);
    brelse(b);
    h = bread(&x->bd, 40);                          /* held, never let go (until below) */
    t0 = clock_ms;
    r = bsync(&x->bd);
    check(r == -1 && clock_ms - t0 >= 10000 && said("does not let go of it") == 1,
          "B18: a sync gives up on a block someone holds after 10 s, and says so");
    brelse(h);
    check(bsync(&x->bd) == 0, "B18: once it is let go, the next sync writes it");
}

/**** B30: a mkdir that fails after its name was written: the new directory's cluster waits to be freed ****/

static void b30(void) {
    struct ram *x = ram_new("b30", 8192);
    struct superblock *sb;
    struct vnode *d;
    u32 used, reached;
    int r;
    fresh("B30: a mkdir that fails after its name");
    mkfs(x);
    sb = mount(x);
    x->oracle = 1;
    kcalloc_fail = 1;                               /* its vnode: no memory */
    r = sb->root->ops->create(sb->root, "d", VN_DIR, &d);
    kcalloc_fail = 0;
    check(r == -ENOMEM && !lookup(sb->root, "d"), "B30: mkdir whose vnode cannot be made says ENOMEM, and its name is gone");
    check(fsync_sb(sb) == 0, "B30: sync");
    census(x, &used, &reached);
    check(used == reached && used == 0 && x->violations == 0, "B30: its cluster was freed after the sync (nothing names it, nothing lost)");
    umount_sb(sb);
}

/**** B36: a directory that cannot grow (its new cluster's table point refused) gives the cluster back ****/

static void b36(void) {
    struct ram *x = ram_new("b36", 8192);
    struct superblock *sb;
    struct vnode *d;
    char name[8];
    u32 used, reached;
    int i, r = 0;
    fresh("B36: a directory that cannot grow");
    mkfs(x);
    sb = mount(x);
    if(sb->root->ops->create(sb->root, "d", VN_DIR, &d)) die("B36: mkdir");
    for(i = 0; i < 14 && !r; i++) { snprintf(name, sizeof(name), "f%02d", i); r = put_file(d, name, NULL, 0); }   /* d's cluster full */
    if(r || fsync_sb(sb)) die("B36: setup");
    x->fail_all = 1;
    r = put_file(d, "x", NULL, 0);                  /* d needs a new cluster: its table point is refused */
    x->fail_all = 0;
    check(r == -EIO && !lookup(d, "x"), "B36: a create that needs a new cluster the card refuses says EIO, and nothing is made");
    check(fsync_sb(sb) == 0 && bdirty_count(&x->bd) == 0, "B36: the given-back cluster's zeros are nobody's: the sync owes nothing");
    census(x, &used, &reached);
    check(used == reached, "B36: nothing lost");
    vput(d);
    umount_sb(sb);
    fsck_clean(x, "b36", "B36: fsck.fat finds nothing to fix");
}

/**** the review of 10/24/26 (a reviewer's seven scenarios, under the audit item each checks, and those of its fixes) ****/

/* B30b: a mkdir that fails after another task's sync wrote its name; the next mkdir's table point freed its
   cluster, and the sync after it wrote the FAT (the cluster free) before the root block (the name gone), and a file
   made meanwhile took the cluster the disk's D still named. A table point frees no waiting chain now */
static struct ram *b30b_x;
static void b30b_other_task_syncs(void) { bsync(&b30b_x->bd); }
static u32 b30b_garbage;
static void b30b_check(struct ram *r) {               /* the disk's D names a cluster holding something else */
    struct geo g;
    u8 raw[32];
    geometry(r, &g);
    if(img_entry(r, "D          ", raw)) {
        u32 c = entry_first(&g, raw);
        const u8 *p = r->mem + clus_lba(&g, c) * 512;
        if(valid_cluster(&g, c) && !(p[0] == '.' && p[1] == ' ')) b30b_garbage++;
    }
}

static void b30b(void) {
    struct ram *x = ram_new("b30b", 8192);
    struct superblock *sb;
    struct vnode *d = NULL, *e = NULL;
    u8 raw[32];
    u32 used, reached, twice;
    int r1, r2, r3, s;
    fresh("B30b: a failed mkdir whose name another task's sync wrote, then a table point");
    mkfs(x);
    sb = mount(x);
    if(fsync_sb(sb)) die("B30b: setup");
    b30b_x = x;
    x->oracle = 1;
    kcalloc_fail = 1;                               /* its vnode: no memory, once the name is in its block */
    kcalloc_hook = b30b_other_task_syncs;           /* and meanwhile another task's sync writes that block */
    r1 = sb->root->ops->create(sb->root, "d", VN_DIR, &d);
    kcalloc_fail = 0;
    kcalloc_hook = NULL;
    check(r1 == -ENOMEM && img_entry(x, "D          ", raw), "B30b: the mkdir failed with its name on the disk");
    r2 = sb->root->ops->create(sb->root, "e", VN_DIR, &e);   /* a table point */
    fill(pat, 512, 131);
    r3 = put_file(sb->root, "f", pat, 512);         /* a file wants a cluster */
    x->check = b30b_check;
    s = fsync_sb(sb);
    x->check = NULL;
    check(r2 == 0 && r3 == 0 && s == 0 && x->violations == 0 && b30b_garbage == 0,
          "B30b: no write puts a free cluster, or another file's, under the failed mkdir's name on the disk");
    if(x->violations && !verbose) printf("      %s\n", x->violation);
    census2(x, &used, &reached, &twice);
    check(used == reached && !twice && !img_entry(x, "D          ", raw), "B30b: and its cluster is free once the name is gone");
    if(e) vput(e);
    umount_sb(sb);
    fsck_clean(x, "b30b", "B30b: fsck.fat finds nothing to fix");
}

/* B3s: FSInfo, a hint, refuses writes (FAT32): rm and mv of files whose blocks are fine work, a close says 0, and
   the unmount gives FSInfo up and leaves the volume marked as not cleanly unmounted (the next mount counts) */
static void b3s(void) {
    struct ram *x = ram_new("b3s", 70000);
    struct superblock *sb;
    int r, s, m;
    fresh("B3s: FSInfo refuses writes");
    mkfs_bits(x, 32);
    sb = mount(x);
    fill(pat, 4096, 132);
    if(put_file(sb->root, "a", pat, 4096) || fsync_sb(sb)) die("B3s: setup");
    x->bad_lo = x->bad_hi = 1;                      /* fat_mkfs puts FSInfo at sector 1 */
    r = put_file(sb->root, "b", pat, 512);          /* the free count changes */
    s = fsync_sb(sb);
    check(r == 0 && s == 0, "B3s: a file written and synced while FSInfo refuses: the sync says 0 (FSInfo is a hint)");
    r = sb->root->ops->unlink(sb->root, "a");
    m = sb->root->ops->rename(sb->root, "b", sb->root, "c");
    check(r == 0 && m == 0, "B3s: rm and mv work while FSInfo refuses writes");
    check(umount_sb(sb) == 1 && said("write errors, the volume stays marked as not cleanly unmounted") == 1,
          "B3s: the unmount gives FSInfo up, says so, and leaves the volume marked");
    x->bad_lo = x->bad_hi = 0;
    fresh(NULL);
    sb = mount(x);
    check(file_is(sb->root, "c", pat, 512) && said("(counted)") == 1, "B3s: the next mount counts the free clusters, the file is there");
    umount_sb(sb);
}

/* W2b-W2d: a rename over an object in another directory (a directory over an empty one, ".." pointed first;
   a file over a file) pulled at every write, and with one write refused and the stick pulled at any later one; an open
   file moved over a file: its vnode follows the replaced entry */
static void w2_setup(struct ram *x, int dirs) {
    struct superblock *sb = mount(x);
    struct vnode *a, *b, *ax, *bx;
    if(sb->root->ops->create(sb->root, "a", VN_DIR, &a) || sb->root->ops->create(sb->root, "b", VN_DIR, &b)) die("W2: dirs");
    if(dirs) {
        if(a->ops->create(a, "x", VN_DIR, &ax) || b->ops->create(b, "x", VN_DIR, &bx)) die("W2: x dirs");
        fill(pat, 3000, 71);
        if(put_file(ax, "f.txt", pat, 3000)) die("W2: f.txt");
        vput(ax); vput(bx);
    } else {
        fill(pat, 3000, 72);
        fill(pat2, 5000, 73);
        if(put_file(a, "x.txt", pat, 3000) || put_file(b, "x.txt", pat2, 5000)) die("W2: files");
    }
    vput(a); vput(b);
    if(fsync_sb(sb) || umount_sb(sb)) die("W2: setup sync");
}

static int w2_op(struct superblock *sb, int dirs) {
    struct vnode *a = lookup(sb->root, "a"), *b = lookup(sb->root, "b");
    int r = -1;
    if(a && b) r = dirs ? a->ops->rename(a, "x", b, "x") : a->ops->rename(a, "x.txt", b, "x.txt");
    if(a) vput(a);
    if(b) vput(b);
    fsync_sb(sb);
    return r;
}

/* the op of kind (0 file over file, 1 directory over directory, 2 the save) with write k refused (0: none) and the
   stick pulled at write j (0: never); whether what the stick holds after it is consistent */
static int pull_run(struct ram *tmpl, int kind, u32 k, u32 j, const char *what) {
    struct ram *x, *y;
    struct superblock *sb;
    u32 used, reached, twice;
    int renamed, bad;
    fresh(NULL);
    x = ram_copy("pr", tmpl);
    sb = mount(x);
    x->attempts = 0;
    x->fail_once = k;
    x->cut_at = j;
    x->oracle = 1;
    if(kind == 2) { x->check = save_check; save_what = what; save_bad = 0; }
    if(kind == 2) { save(sb, NEW_LEN, &renamed); fsync_sb(sb); }
    else w2_op(sb, kind);
    x->oracle = 0;
    x->check = NULL;
    y = ram_copy("pn", x);
    census2(y, &used, &reached, &twice);
    bad = x->violations || twice || (kind == 2 && save_bad);
    if(bad && verbose) printf("      %s, refused %u, pulled at %u: %s%s\n", what, k, j, x->violations ? x->violation : "",
                              twice ? " (a cluster reached twice)" : "");
    x->cut_at = 0;
    x->bd.gone = true;
    umount_sb(sb);
    free(x->mem); free(y->mem);
    x->mem = y->mem = NULL;
    return bad;
}

static void w2b(const char *title, int kind) {
    struct ram *tmpl, *x;
    struct superblock *sb;
    u32 total, k, j, runs = 0, bad = 0;
    int renamed;
    fresh(title);
    if(kind == 2) {
        fill(old_bytes, OLD_LEN, 81);
        fill(new_bytes, NEW_LEN, 82);
        tmpl = save_volume("w2s", 16, 20);
    } else {
        tmpl = ram_new("w2t", 8192);
        mkfs(tmpl);
        w2_setup(tmpl, kind);
    }
    x = ram_copy("w2x", tmpl);                      /* its writes, counted */
    sb = mount(x);
    x->attempts = 0;
    if(kind == 2) save(sb, NEW_LEN, &renamed);
    else w2_op(sb, kind);
    fsync_sb(sb);
    total = x->attempts + 3;
    umount_sb(sb);
    free(x->mem);
    for(j = 1; j <= total; j++, runs++) bad += pull_run(tmpl, kind, 0, j, title);      /* pulled at every write */
    for(k = 1; k <= total; k++)                                                        /* one refused, pulled later */
        for(j = k + 1; j <= total + 2; j++, runs++) bad += pull_run(tmpl, kind, k, j, title);
    if(verbose) printf("      %u runs, %u bad\n", runs, bad);
    check(bad == 0, title);
}

static void w2d(void) {
    struct ram *x = ram_new("w2d", 8192);
    struct superblock *sb;
    struct vnode *a, *b, *v, *w;
    u32 used, reached, twice;
    int r, n;
    fresh("W2d: an open file moved over a file elsewhere");
    mkfs(x);
    w2_setup(x, 0);
    sb = mount(x);
    a = lookup(sb->root, "a"); b = lookup(sb->root, "b");
    v = lookup(a, "x.txt");                         /* open: 3000 bytes */
    r = a->ops->rename(a, "x.txt", b, "x.txt");
    fill(pat, 9000, 74);
    n = v->ops->write(v, 0, pat, 9000);             /* through the moved vnode */
    w = lookup(b, "x.txt");
    fsync_sb(sb);
    check(r == 0 && n == 9000 && w == v && file_is(b, "x.txt", pat, 9000) && !lookup(a, "x.txt"),
          "W2d: the moved vnode is the replaced entry's: lookup finds it, and its write is the file's");
    r = v->ops->truncate(v, 0);
    fsync_sb(sb);
    census2(x, &used, &reached, &twice);
    check(r == 0 && used == reached && !twice, "W2d: a truncate through it frees its chain, nothing lost");
    if(w) vput(w);
    vput(v); vput(a); vput(b);
    umount_sb(sb);
}

/* B25b: truncate, rm, and a short write then a file across FAT sectors, with one write refused and the stick
   pulled at any later write */
static void b25b_setup(struct ram *x) {
    struct superblock *sb = mount(x);
    fill(pat, 65536, 75);
    if(put_file(sb->root, "a", pat, 20000) || put_file(sb->root, "b", pat, 6000)) die("B25b: files");
    if(big_file(sb->root, "big", pat, 240 * 512)) die("B25b: big");     /* the free clusters start near a FAT sector's end */
    if(fsync_sb(sb) || umount_sb(sb)) die("B25b: setup sync");
}

static void b25b_op(struct superblock *sb, int kind) {
    struct vnode *v;
    if(kind == 0) { v = lookup(sb->root, "a"); if(v) { v->ops->truncate(v, 3000); vput(v); } }
    else if(kind == 1) sb->root->ops->unlink(sb->root, "a");
    else {
        v = lookup(sb->root, "b");
        if(v) { v->ops->write(v, 6000, pat, 30000); vput(v); }        /* may stop short: trim, maybe a waiting chain */
        put_file(sb->root, "c", pat, 40000);                          /* across a FAT sector: table points */
    }
    fsync_sb(sb);
}

static void b25b(const char *title, int kind) {
    struct ram *tmpl, *x, *y;
    struct superblock *sb;
    u32 total, k, j, used, reached, twice, bad = 0, runs = 0;
    fresh(title);
    tmpl = ram_new("b25t", 8192);
    mkfs(tmpl);
    b25b_setup(tmpl);
    x = ram_copy("b25x", tmpl);
    sb = mount(x);
    x->attempts = 0;
    b25b_op(sb, kind);
    total = x->attempts + 3;
    umount_sb(sb);
    free(x->mem);
    for(k = 0; k <= total; k++) {
        for(j = k + 1; j <= total + 2; j++, runs++) {
            fresh(NULL);
            x = ram_copy("b25p", tmpl);
            sb = mount(x);
            x->attempts = 0;
            x->fail_once = k;
            x->cut_at = j;
            x->oracle = 1;
            b25b_op(sb, kind);
            x->oracle = 0;
            y = ram_copy("b25n", x);
            census2(y, &used, &reached, &twice);
            if(x->violations || twice) {
                if(!bad++ && verbose) printf("      refused %u, pulled at %u: %s%s\n", k, j, x->violations ? x->violation : "",
                                             twice ? " (a cluster reached twice)" : "");
            }
            x->cut_at = 0;
            x->bd.gone = true;
            umount_sb(sb);
            free(x->mem); free(y->mem);
            x->mem = y->mem = NULL;
        }
    }
    if(verbose) printf("      %u runs, %u bad\n", runs, bad);
    check(bad == 0, title);
}

/* W3: an rm of a file that is open (opened after the VFS looked, so its check did not see it) says EBUSY: its chain
   was freed while it was open, and its next write went to clusters the next file would take */
static void w3(void) {
    struct ram *x = ram_new("w3", 8192);
    struct superblock *sb;
    struct vnode *v;
    u32 used, reached, twice;
    int r;
    fresh("W3: rm of an open file");
    mkfs(x);
    sb = mount(x);
    fill(pat, 3000, 141);
    if(put_file(sb->root, "a", pat, 3000) || fsync_sb(sb)) die("W3: setup");
    v = lookup(sb->root, "a");                      /* open */
    r = sb->root->ops->unlink(sb->root, "a");
    check(r == -EBUSY && file_is(sb->root, "a", pat, 3000), "W3: rm of an open file says EBUSY, and the file stays whole");
    vput(v);
    r = sb->root->ops->unlink(sb->root, "a");
    fsync_sb(sb);
    census2(x, &used, &reached, &twice);
    check(r == 0 && used == 0 && reached == 0, "W3: once it is closed the rm works, its clusters free");
    umount_sb(sb);
}

/* B13p (the review's finding 4): a buffer a sync wrote, reused for another block that another task makes dirty while
   that sync goes on, kept the sync call's mark: the sync passed the new block over, and the entry block that waited
   for it made the sync say -1 though every write worked */
static struct ram *b13p_y;
static int b13p_ran;
static void b13p_hook(struct ram *r, u32 lba, u32 count) {
    struct buf *b;
    u32 i;
    if(b13p_ran || !(lba <= 300 && 300 < lba + count)) return;
    b13p_ran = 1;
    r->hook = NULL;
    for(i = 0; i < BCACHE_BLOCKS - 3; i++) if((b = bread(&b13p_y->bd, i)) != NULL) brelse(b);   /* the unused buffers */
    if((b = bget(&r->bd, 700)) != NULL) { bdirty(b); brelse(b); }  /* the oldest: the one the sync wrote first */
}

static void b13p(void) {
    struct ram *x = ram_new("b13p", 4096), *y = ram_new("b13py", 4096);
    struct buf *b;
    int r;
    fresh("B13p: a sync's mark on a reused buffer");
    b = bget(&x->bd, 100); bdirty(b); brelse(b);     /* bufs[0] */
    b = bget(&x->bd, 300); bdirty(b); brelse(b);     /* bufs[1]: written second, the hook runs then */
    b = bget(&x->bd, 500); bdirty_order(b, BORDER_DIR); brelse(b);   /* bufs[2]: waits for both */
    b13p_y = y;
    b13p_ran = 0;
    x->hook = b13p_hook;
    r = bsync(&x->bd);
    check(b13p_ran && r == 0, "B13p: a sync whose written buffer is reused and made dirty meanwhile still writes what it owes");
}

/* B18k (the review's finding 3): a sync of a task being killed gives up on a held block at once: its kill would come
   inside the 10 s wait, holding the volume's lock */
static void b18k(void) {
    static surf_task killed;
    struct ram *x = ram_new("b18k", 4096);
    struct buf *b, *h;
    u_long t0;
    int r;
    fresh("B18k: a held block and a task being killed");
    b = bget(&x->bd, 40);
    bdirty(b);
    brelse(b);
    h = bread(&x->bd, 40);                          /* held */
    memset(&killed, 0, sizeof(killed));
    killed.intr = TI_KILL;
    curTask = &killed;
    t0 = clock_ms;
    r = bsync(&x->bd);
    curTask = NULL;
    check(r == -1 && clock_ms - t0 < 2000, "B18k: a task being killed gives up on a block someone holds at once");
    brelse(h);
    check(bsync(&x->bd) == 0, "B18k: the next sync writes it");
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
    RUN("b18", b18());
    RUN("b30", b30());
    RUN("b36", b36());
    RUN("b30b", b30b());
    RUN("b3s", b3s());
    RUN("w2b", w2b("W2b: a file moved over a file in another directory, pulled at every write, and refused then pulled", 0));
    RUN("w2c", w2b("W2c: a directory moved over an empty one elsewhere, pulled at every write, and refused then pulled", 1));
    RUN("w2s", w2b("W2s: the save, one write refused and the stick pulled at any later one", 2));
    RUN("w2d", w2d());
    RUN("b25b", b25b("B25b: a truncate, one write refused and the stick pulled at any later one", 0);
                b25b("B25c: an rm, one write refused and the stick pulled at any later one", 1);
                b25b("B25d: a short write and a file across FAT sectors, refused then pulled", 2));
    RUN("w3", w3());
    RUN("b13p", b13p());
    RUN("b18k", b18k());
    RUN("s1", s1("S1a: a save pulled at every write (FAT16, one directory block)", 16, 0));
    RUN("s1b", s1("S1b: a save pulled at every write (FAT16, the new name in another block)", 16, 20));
    RUN("s1c", s1("S1c: a save pulled at every write (FAT32)", 32, 20));
    RUN("s2", s2("S2a: a save with each write refused once (FAT16)", 16, 20, 0));
    RUN("s2b", s2("S2b: a save on a stick that stops taking writes (FAT16)", 16, 20, 1));
    RUN("s2c", s2("S2c: a save with each write refused once (FAT32)", 32, 0, 0));
    RUN("s3", s3());
    RUN("s4", s4());
    RUN("s5", s5());
    RUN("s6", s6());
    RUN("s7", s7());
    RUN("s8", s8());
    RUN("s9", s9());
    RUN("s10", s10());
    audit();
    if(failures) { printf("FATFS UNIT TESTS FAILED: %d of %d checks\n", failures, checks); return 1; }
    printf("FATFS UNIT TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
