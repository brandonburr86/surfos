/* Review harness (conc) for round fs-fix (d7452f4): the real fs/bcache.c, fs/fat.c and fs/fat_mkfs.c run
   by several host threads at once, with the kernel's waits made real (a SurfOS mutex blocks, a wait for
   a block blocks until a wake-up, irq_save() regions exclude each other). A wait that never ends shows
   as the watchdog's report.
     workers   each does random FAT operations in its own directory of one volume (create, write, append,
               truncate, rm, mv) and keeps a model of it; a failed rm, mv, truncate or create must have
               changed nothing (B5); after a write it sometimes "closes" the file (vfs_close()'s write-back:
               the file system's sync, then bsync()) and, when that says 0, looks at the file's entry on
               the disk (B10)
     reader    reads another disk through the cache (making room: evictions of the volume's blocks)
     syncer    bsync(NULL) now and then (vfs_sync())
     chaos     the volume's disk refuses every write for a few milliseconds now and then
   The oracle looks at the image after every write the disk stores (no entry or link on the way reaching a
   free cluster; since round fs-fix-5, no named file showing another file's bytes: each block carries its file's
   tag; since round fs-fix-6, exactly: no block of a file's range carries the tag of a file that never had that
   name in that directory, the streamer's too); at the end, with the disk working, a sync, the model against the file system and against
   a fresh mount of a copy of the image, and bcache_audit() (B9).
   usage: conc <seed> <seconds> [workers] [chaos 0/1] [io_us]
   (10/16/26, round fs-fix-2: written by the independent review of B5-B11 as fsrev-tests/conc.c, with
   conc_host.c and conc_shim/ (here fatconc_host.c, fatconc_shim/), and kept as it was but for the end of
   main(), which now fails (exit 1) when a check found something, and bcache's clock (B18) below.
   `make unittest` runs it for a few seconds with three fixed settings; the review's campaigns ran it for
   minutes: fsrev-tests/conc_campaign.sh, conc_campaign2.sh, recorded in bcache-review.txt. 10/16/26, round
   fs-fix-3: four settings; the fourth, six writers on a small volume, is the one with which the review of
   B12-B19 found trim_chain()'s window (B25): 2 of its 9 runs on fd37406. 10/17/26, round fs-fix-7: `conc umount`, the
   review of B39-B45's R1 with threads: umount_main() below.) */
#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <surfos/wait.h>
#include <surfos/task.h>
#include <surfos/interrupt.h>
#include <surfos/irq.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <fs/fat.h>
#include <mm/kalloc.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

void exit(int status) __attribute__((noreturn));
void *malloc(unsigned long n);
void *calloc(unsigned long n, unsigned long m);
void *realloc(void *p, unsigned long n);
void free(void *p);
int atoi(const char *s);
char *getenv(const char *name);

/* conc_host.c */
void host_init(void);
void host_tbl_lock(void);
void host_tbl_unlock(void);
void host_tbl_wait(void);
void host_tbl_broadcast(void);
void host_wait_prepare(void);
void host_wake_all(void);
void host_yield(void);
void host_sleep_us(unsigned us);
void host_set_name(const char *n);
const char *host_name(void);
void *host_thread(void (*fn)(void *), void *arg, const char *name);
void host_join(void *th);
void host_watchdog(int seconds, void (*dump)(void));
void host_watchdog_stop(void);
unsigned long host_ms(void);
void host_out_lock(void);
void host_out_unlock(void);
void *host_mutex_new(void);
void host_mutex_lock(void *m);
void host_mutex_unlock(void *m);
extern volatile unsigned long host_progress;

#ifdef BEFORE
/* 9b8621c has neither: no account to audit, and the dirty blocks counted through bpeek() */
int bcache_audit(struct bdev *dev) { (void)dev; return 0; }
u_int bdirty_count(struct bdev *dev) {
    u32 lba, n = 0;
    for(lba = 0; lba < dev->nblocks; lba++) { struct buf *b = bpeek(dev, lba); if(b) { n += b->dirty; brelse(b); } }
    return n;
}
#else
int bcache_audit(struct bdev *dev);
u_int bdirty_count(struct bdev *dev);
#endif

/**** the kernel around the file system ****/

volatile int in_irq;
static surf_task the_task;
surf_task *curTask = &the_task, *idle_task;     /* (for fs/ built without the definition below: one task for all) */

/* 10/16/26 (round fs-fix-3): make unittest builds fs/ with curTask standing for *fatconc_task(): each thread is a
   task of its own, so a block one thread's operation sets aside (B16) is another thread's sync's debt (B26), which
   that sync waits for as for a hold */
static __thread surf_task thread_task;
static __thread surf_task *thread_cur;
surf_task **fatconc_task(void) {
    if(!thread_cur) thread_cur = &thread_task;
    return &thread_cur;
}

/* 10/18/26 (round fs-fix-11, audit B70): the kernel's mutex (kernel/sync.c, B63), where this was wake-and-compete (the
   one that let go could take it again before a woken waiter ran): a free mutex is taken at once; a busy one puts its
   caller at the end of its line (m->waiters, linked through the tasks as the kernel's wait queues are), and
   mutex_unlock() makes the first in line the owner, counted in its surf_task.mutexes, before it runs again. The owner
   is the calling thread's own task (*fatconc_task(), curTask to fs/), so bcache_holds() answers as in the kernel (it
   never did here: the owner was the thread's name). Misuse stops the test as the kernel's panic stops the system. A
   thread's count must be 0 between its operations (count_left, in main()'s verdict). Under the table lock; a waiter
   sleeps on the table's condition until it is the owner. */
static volatile u32 count_left;
static void mutex_misuse(mutex_t *m, const char *what) {
    printf("PANIC: mutex '%s': %s by '%s'\n", m->name, what, host_name());
    exit(3);
}
void mutex_init(mutex_t *m, const char *name) {
    m->locked = 0;
    m->owner = NULL;
    m->name = name;
    m->waiters.first = m->waiters.last = NULL;
    m->waiters.count = 0;
}
void mutex_lock(mutex_t *m) {
    surf_task *me = *fatconc_task();
    host_tbl_lock();
    if(m->locked && m->owner == me) mutex_misuse(m, "recursive lock");
    if(!m->locked) {
        m->locked = 1;
        m->owner = me;
        me->mutexes++;
    } else {                                        /* in line (task_list_append()) until mutex_unlock() hands it over */
        me->next = NULL;
        me->prev = m->waiters.last;
        if(m->waiters.last) m->waiters.last->next = me;
        else m->waiters.first = me;
        m->waiters.last = me;
        m->waiters.count++;
        while(m->owner != me) host_tbl_wait();
    }
    host_tbl_unlock();
}
bool mutex_trylock(mutex_t *m) {
    surf_task *me = *fatconc_task();
    bool ok;
    host_tbl_lock();
    ok = !m->locked;
    if(ok) { m->locked = 1; m->owner = me; me->mutexes++; }
    host_tbl_unlock();
    return ok;
}
void mutex_unlock(mutex_t *m) {
    surf_task *me = *fatconc_task(), *next;
    host_tbl_lock();
    if(!m->locked || m->owner != me) mutex_misuse(m, "unlock by a task that does not hold it");
    if(me->mutexes) me->mutexes--;
    next = m->waiters.first;
    if(next) {                                      /* the first in line holds it from now (wake_up(): out of the line) */
        m->waiters.first = next->next;
        if(m->waiters.first) m->waiters.first->prev = NULL;
        else m->waiters.last = NULL;
        m->waiters.count--;
        next->next = next->prev = NULL;
        m->owner = next;
        next->mutexes++;
        host_tbl_broadcast();
    } else {
        m->locked = 0;
        m->owner = NULL;
    }
    host_tbl_unlock();
}
static void count_check(void) {                     /* between a thread's operations it holds no mutex */
    if((*fatconc_task())->mutexes) count_left++;
}
void wait_prepare(wait_queue_t *q) { (void)q; host_wait_prepare(); }
void wait_prepare_intr(wait_queue_t *q) { (void)q; host_wait_prepare(); }   /* 10/17/26 (round fs-fix-6, B39) */
void kcritical_enter(void) { }                                              /* (B39: fs/fat.c's lock) */
void kcritical_leave(void) { }
/* 10/16/26 (B18): bcache's timed waits: the host clock; its timers are not needed (a wait wakes within 1 s) */
#include <surfos/ktimer.h>
u_long uptime_ms(void) { return host_ms(); }
void ktimer_init(struct ktimer *t, void (*fn)(void *), void *arg) { (void)t; (void)fn; (void)arg; }
void ktimer_start(struct ktimer *t, u_long ms, bool periodic) { (void)t; (void)ms; (void)periodic; }
void ktimer_cancel(struct ktimer *t) { (void)t; }
void wake_up_all(wait_queue_t *q) { (void)q; host_wake_all(); }
void yield(void) { host_yield(); }

static int quiet_kprintf;
static volatile u32 said_unclean;   /* 10/17/26 (round fs-fix-7): what says a volume is left unclean or lost something */
void kprintf(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if(strstr(line, "not cleanly unmounted") || strstr(line, "not unmounted cleanly") || strstr(line, "write errors") ||
       strstr(line, "could not be freed") || strstr(line, "given up")) said_unclean++;
    if(quiet_kprintf) return;
    host_out_lock();
    printf("      [kprintf %s] %s", host_name(), line);
    host_out_unlock();
}
void *kalloc(size_t n) { return malloc(n); }
void *kcalloc(size_t n, size_t m) { return calloc(n, m); }
void kfree(void *p) { free(p); }
void get_time(time_t *t) { memset(t, 0, sizeof(*t)); t->year = 2026; t->month = 10; t->date = 16; t->hour = 12; }

/**** RAM disks ****/

struct ram {
    struct bdev bd;
    u8 *mem;
    void *lock;
    volatile int fail_all;
    u32 io_us;
    volatile u32 slow_from, slow_us;    /* 10/17/26 (round fs-fix-7): writes reaching past slow_from take slow_us more */
    volatile int slow_seen;             /*      (0: none), and one has begun */
    int oracle;
    volatile u32 writes, failed, violations;
    char violation[240];
};

static void oracle_check(struct ram *r);

static int ram_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    struct ram *r = (struct ram *)d->priv;
    host_sleep_us(r->io_us);
    host_mutex_lock(r->lock);
    memcpy(buf, r->mem + lba * 512, count * 512);
    host_mutex_unlock(r->lock);
    return 0;
}

static int ram_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct ram *r = (struct ram *)d->priv;
    if(r->slow_us && lba + count > r->slow_from) { r->slow_seen = 1; host_sleep_us(r->slow_us); }
    host_sleep_us(r->io_us);
    host_mutex_lock(r->lock);
    if(r->fail_all) { r->failed++; host_mutex_unlock(r->lock); return BDEV_EIO; }
    memcpy(r->mem + lba * 512, buf, count * 512);
    r->writes++;
    if(r->oracle) oracle_check(r);
    host_mutex_unlock(r->lock);
    return 0;
}

static const struct bdev_ops ram_ops = { ram_read, ram_write, NULL };

static struct ram *ram_new(const char *name, u32 blocks, u32 io_us) {
    struct ram *r = (struct ram *)calloc(1, sizeof(*r));
    r->mem = (u8 *)calloc(blocks, 512);
    r->lock = host_mutex_new();
    r->io_us = io_us;
    strlcpy(r->bd.name, name, sizeof(r->bd.name));
    r->bd.block_size = 512;
    r->bd.nblocks = blocks;
    r->bd.bus = BDEV_BUS_RAM;
    r->bd.ops = &ram_ops;
    r->bd.priv = r;
    return r;
}

int bdev_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    if(d->gone) return BDEV_EGONE;
    if(lba + count > d->nblocks) return BDEV_ERANGE;
    if(!count) return 0;
    return d->ops->read(d, lba, count, buf);
}
static volatile u32 zero_writes;
int bdev_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    if(d->gone) return BDEV_EGONE;
    if(lba + count > d->nblocks) return BDEV_ERANGE;
    if(!count) { zero_writes++; return 0; }
    return d->ops->write(d, lba, count, buf);
}
int bdev_flush(struct bdev *d) { return d->gone ? BDEV_EGONE : 0; }
struct bdev *bdev_disk(struct bdev *d) { while(d && d->parent) d = d->parent; return d; }
int bdev_may_format(struct bdev *d, char *why, size_t size) { (void)d; if(why && size) why[0] = 0; return 0; }
int vfs_bdev_change_begin(struct bdev *d, const char **why) { (void)d; if(why) *why = NULL; return 0; }   /* (B61) */
void vfs_bdev_change_end(struct bdev *d) { (void)d; }
const char *bdev_busy_words(const char *busy) { return busy ? busy : ""; }

/**** the VFS, as much as fat.c needs ****/

static const struct fs_type *fat;
int vfs_register(const struct fs_type *t) { fat = t; return 0; }
struct vnode *vnode_alloc(struct superblock *sb, const struct vnode_ops *ops, int type) {
    struct vnode *v = (struct vnode *)calloc(1, sizeof(*v));
    u_long f;
    v->sb = sb; v->ops = ops; v->type = type; v->refs = 1;
    f = irq_save();
    if(sb) sb->nvnodes++;
    irq_restore(f);
    return v;
}
void vput(struct vnode *v) {
    u_long f;
    bool last;
    if(!v) return;
    f = irq_save();
    last = --v->refs == 0;
    irq_restore(f);
    if(!last) return;
    if(v->ops->release) v->ops->release(v);
    f = irq_save();
    if(v->sb) v->sb->nvnodes--;
    irq_restore(f);
    free(v);
}

/**** the oracle (FAT12/16/32) ****/

static u32 gle16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 gle32(const u8 *p) { return gle16(p) | (gle16(p + 2) << 16); }
struct ggeo { u32 spc, fat_lba, spf, root_lba, root_secs, data_lba, nclusters, root_cluster; int type; };
static void ggeometry(const u8 *bs, struct ggeo *g) {
    u32 rsv = gle16(bs + 14), nfats = bs[16], re = gle16(bs + 17), tot = gle16(bs + 19) ? gle16(bs + 19) : gle32(bs + 32);
    g->spc = bs[13];
    g->spf = gle16(bs + 22) ? gle16(bs + 22) : gle32(bs + 36);
    g->fat_lba = rsv;
    g->root_lba = rsv + nfats * g->spf;
    g->root_secs = (re * 32 + 511) / 512;
    g->data_lba = g->root_lba + g->root_secs;
    g->nclusters = (tot - g->data_lba) / g->spc;
    g->type = g->nclusters < 4085 ? 12 : g->nclusters < 65525 ? 16 : 32;
    g->root_cluster = g->type == 32 ? gle32(bs + 44) : 0;
}
static u32 gfat(const u8 *mem, const struct ggeo *g, u32 c) {
    const u8 *f = mem + g->fat_lba * 512;
    if(g->type == 12) { u32 v = gle16(f + c + c / 2); return (c & 1) ? v >> 4 : v & 0xFFF; }
    if(g->type == 16) return gle16(f + c * 2);
    return gle32(f + c * 4) & 0x0FFFFFFF;
}
static int geoc(const struct ggeo *g, u32 v) { return g->type == 12 ? v >= 0xFF8 : g->type == 16 ? v >= 0xFFF8 : v >= 0x0FFFFFF8; }
static void gviol(struct ram *r, const char *m) {
    if(!r->violations++) snprintf(r->violation, sizeof(r->violation), "after the disk's write %u: %s", r->writes, m);
}
/* 10/16/26 (round fs-fix-5): stale data. Every 512-byte block of a file's bytes starts with the file's tag (8 bytes:
   a mark and the file's number, the same whatever wrote it: a write that covers those bytes writes them), and after
   every write the disk stores, each named file's range is looked at: every block in it whose tag lies within the size
   and is whole carries the same one (the number twice, once inverted: a tag a later write or a gap's zeros cut into
   is no tag; zeros carry none). Another file's tag there is a block the file never wrote: the
   card names it over bytes it held before (B20's and B34's class). A file keeps its number through a rename */
#define TAG_MARK0 0xA5
#define TAG_MARK1 0x5A
static u8 tag_byte(u32 id, u32 k) {
    static const u8 mark[8] = { TAG_MARK0, TAG_MARK1, 0, 0, 0, 0, 'T', 'G' };
    if(k == 2 || k == 3) return (u8)(id >> (8 * (k - 2)));
    if(k == 4 || k == 5) return (u8)(~id >> (8 * (k - 4)));
    return mark[k];
}
static void tag_fill(u8 *b, u32 off, u32 len, u32 id) {
    u32 i;
    for(i = 0; i < len; i++) if((off + i) % 512 < 8) b[i] = tag_byte(id, (off + i) % 512);
}
/* 10/17/26 (round fs-fix-6): the exact check (the review of B34-B38). The check above only saw a range holding two
   different files' tags: a file whose blocks all carry one other file's tag passed, and the streamer wrote zeros (no
   tags). Now every name a file of worker k's directory ever had (created, or moved to) is recorded with the file's
   number before the operation that could put that name on the disk (hist_add()), and a block within a named file's
   range carrying the tag of a file that never had that name in that directory is a block the file never wrote: stale
   (exact_viol). Names come from the long name when one precedes the short entry (its checksum checked), else from the
   8.3 name. The two-tags check stays (mixed_viol); ONLY_MIXED=1 in the environment checks with it alone, as round
   fs-fix-5 did (for the evidence) */
static const char *names8[] = { "f0.dat", "f1.dat", "f2.dat", "f3.dat", "f4.dat", "f5.dat" };
static u8 hist[8][10][65536 / 8];                  /* [worker's directory][name index][file number] */
static volatile u32 exact_checked, exact_viol, mixed_viol;
static int only_mixed;
static int lname_eq(const char *a, const char *b);
static int name_index(const char *name) {
    int i;
    for(i = 0; i < 6; i++) if(lname_eq(name, names8[i])) return i;
    if(!strncmp(name, "a long name number ", 19) && name[19] >= '0' && name[19] <= '2' && name[20] == ' ') return 6 + name[19] - '0';
    if(lname_eq(name, "s.dat")) return 9;
    return -1;
}
static void hist_add(int wd, const char *name, u32 id) {
    int ni = name_index(name);
    if(wd < 0 || wd >= 8 || ni < 0 || !id) return;
    __sync_fetch_and_or(&hist[wd][ni][id >> 3], (u8)(1 << (id & 7)));
}
static void gstale(struct ram *r, const struct ggeo *g, const u8 *p, const char *name, int wd, int ni) {
    u32 size = gle32(p + 28), c = gle16(p + 26) | (g->type == 32 ? gle16(p + 20) << 16 : 0), steps = 0, off = 0, id = 0, s;
    int ex = 0, mx = 0;
    char m[200];
    while(c >= 2 && c < g->nclusters + 2 && off < size && steps++ < g->nclusters && !(ex && mx)) {
        u32 v;
        for(s = 0; s < g->spc && off < size; s++, off += 512) {
            const u8 *d = r->mem + (g->data_lba + (c - 2) * g->spc + s) * 512;
            u32 t;
            if(off + 8 > size || d[0] != TAG_MARK0 || d[1] != TAG_MARK1 || d[6] != 'T' || d[7] != 'G' ||
               (u8)(d[2] ^ d[4]) != 0xFF || (u8)(d[3] ^ d[5]) != 0xFF) continue;   /* (whole, and within the size: the
                                                                                  bytes past the end are anybody's) */
            t = gle16(d + 2);
            if(wd >= 0 && ni >= 0 && !only_mixed && !ex) {   /* the exact check */
                exact_checked++;
                if(!(hist[wd][ni][t >> 3] & (1 << (t & 7)))) {
                    ex = 1;
                    if(!exact_viol++) {
                        snprintf(m, sizeof(m), "entry '%s' in t%d: its block at %u (cluster %u) holds file %u's bytes, a file that never had"
                                 " that name there (stale data, exact check)", name, wd, off, c, t);
                        gviol(r, m);
                    } else r->violations++;
                }
            }
            if(!id) id = t;                          /* round fs-fix-5's check: two numbers in one range */
            else if(t != id && !mx) {
                mx = 1;
                if(!mixed_viol++) {
                    snprintf(m, sizeof(m), "entry '%s': its block at %u holds file %u's bytes, its others file %u's (stale data)",
                             name, off, t, id);
                    gviol(r, m);
                } else r->violations++;
            }
        }
        v = gfat(r->mem, g, c);
        if(!v || geoc(g, v)) break;
        c = v;
    }
}
static u8 lfn_sum(const u8 *n11) {
    u8 sum = 0;
    int i;
    for(i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + n11[i]);
    return sum;
}
static void gentries(struct ram *r, const struct ggeo *g, const u8 *p, u32 n, int depth, int wd);
static void gdir(struct ram *r, const struct ggeo *g, u32 first, int depth, int wd) {
    u32 c = first, steps = 0;
    if(depth > 4) return;
    while(c >= 2 && c < g->nclusters + 2 && steps++ < 65536) {
        u32 next = gfat(r->mem, g, c);
        char m[120];
        if(!next) { snprintf(m, sizeof(m), "a directory's chain reaches cluster %u, which is free", c); gviol(r, m); return; }
        gentries(r, g, r->mem + (g->data_lba + (c - 2) * g->spc) * 512, g->spc * 16, depth, wd);
        if(geoc(g, next)) return;
        c = next;
    }
}
static void gentries(struct ram *r, const struct ggeo *g, const u8 *p, u32 n, int depth, int wd) {
    u32 i;
    char lfn[300];
    int lfn_max = 0, lfn_ok = 0;
    u8 lfn_ck = 0;
    for(i = 0; i < n; i++, p += 32) {
        u32 first, c, steps = 0;
        char m[120], name[12], full[300];
        int ni = -1;
        if(p[0] == 0) return;
        if(p[0] == 0xE5) { lfn_ok = 0; continue; }
        if(p[11] == 0x0F) {                          /* a long name's part: 13 UTF-16 characters (ASCII here) */
            static const int at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            int seq = p[0] & 0x1F, k;
            if(p[0] & 0x40) { lfn_ok = 1; lfn_max = seq * 13; lfn_ck = p[13]; memset(lfn, 0, sizeof(lfn)); }
            if(!lfn_ok || seq < 1 || seq * 13 > (int)sizeof(lfn) - 1 || p[13] != lfn_ck) { lfn_ok = 0; continue; }
            for(k = 0; k < 13; k++) {
                u16 ch = (u16)(p[at[k]] | (p[at[k] + 1] << 8));
                lfn[(seq - 1) * 13 + k] = ch == 0xFFFF ? 0 : (char)ch;
            }
            continue;
        }
        if(p[11] & 0x08) { lfn_ok = 0; continue; }
        if(p[0] == '.' && (p[1] == ' ' || p[1] == '.')) { lfn_ok = 0; continue; }
        memcpy(name, p, 11); name[11] = 0;
        if(lfn_ok && lfn_ck == lfn_sum(p)) { lfn[lfn_max < (int)sizeof(lfn) ? lfn_max : (int)sizeof(lfn) - 1] = 0; strlcpy(full, lfn, sizeof(full)); }
        else {                                       /* "F3      DAT" -> "f3.dat" */
            int k, j = 0;
            for(k = 0; k < 8 && p[k] != ' '; k++) full[j++] = (char)(p[k] >= 'A' && p[k] <= 'Z' ? p[k] + 32 : p[k]);
            if(p[8] != ' ') { full[j++] = '.'; for(k = 8; k < 11 && p[k] != ' '; k++) full[j++] = (char)(p[k] >= 'A' && p[k] <= 'Z' ? p[k] + 32 : p[k]); }
            full[j] = 0;
        }
        lfn_ok = 0;
        first = gle16(p + 26) | (g->type == 32 ? gle16(p + 20) << 16 : 0);
        for(c = first; c >= 2 && c < g->nclusters + 2 && steps++ < 1000000; ) {
            u32 v = gfat(r->mem, g, c);
            if(!v) { snprintf(m, sizeof(m), "entry '%s' -> cluster %u (step %u of its chain) is free", name, c, steps - 1); gviol(r, m); break; }
            if((p[11] & 0x10) || geoc(g, v)) break;
            c = v;
        }
        if((p[11] & 0x10) && first) gdir(r, g, first, depth + 1, depth == 0 && full[0] == 't' && full[1] >= '0' && full[1] <= '7' && !full[2] ? full[1] - '0' : -1);
        else if(!(p[11] & 0x10)) {
            if(wd >= 0) ni = name_index(full);
            gstale(r, g, p, full, wd, ni);
        }
    }
}
static void oracle_check(struct ram *r) {
    struct ggeo g;
    ggeometry(r->mem, &g);
    if(g.type == 32) gdir(r, &g, g.root_cluster, 0, -1);
    else gentries(r, &g, r->mem + g.root_lba * 512, g.root_secs * 16, 0, -1);
}

/* the size field of entry n11 (8.3) in directory dir11 of the root, on the disk now; -1 when not found */
static long disk_size(struct ram *r, const char *dir11, const char *n11) {
    struct ggeo g;
    const u8 *p;
    u32 i, c, steps = 0, first = 0;
    long size = -1;
    host_mutex_lock(r->lock);
    ggeometry(r->mem, &g);
    if(g.type == 32) { host_mutex_unlock(r->lock); return -2; }
    p = r->mem + g.root_lba * 512;
    for(i = 0; i < g.root_secs * 16; i++, p += 32) {
        if(!p[0]) break;
        if(p[0] == 0xE5 || p[11] == 0x0F) continue;
        if(!memcmp(p, dir11, 11)) { first = gle16(p + 26); break; }
    }
    for(c = first; c >= 2 && c < g.nclusters + 2 && steps++ < 1000 && size < 0; ) {
        p = r->mem + (g.data_lba + (c - 2) * g.spc) * 512;
        for(i = 0; i < g.spc * 16; i++, p += 32) {
            if(!p[0]) break;
            if(p[0] == 0xE5 || p[11] == 0x0F) continue;
            if(!memcmp(p, n11, 11)) { size = (long)gle32(p + 28); break; }
        }
        c = gfat(r->mem, &g, c);
        if(geoc(&g, c)) break;
    }
    host_mutex_unlock(r->lock);
    return size;
}

/**** the workers ****/

#define NF 8
#define MAXSZ 24000

struct mfile { int used, plain; char name[64]; u8 *data; u32 size, id; };   /* plain: its short entry is its 8.3 name;
                                                                              id: its number (its tag's, round fs-fix-5) */
static volatile u32 file_ids;
struct worker {
    int id;
    u32 rng;
    struct vnode *dir;
    char dname[16], d11[12];
    struct mfile f[NF];
    u32 ops, fails, b5_bad, close_checks, close_stale, model_bad;
    const char *where;
    char last[160];
};

static struct superblock *sb;
static struct ram *x, *y;
static volatile int running = 1;
static struct worker *workers;
static int nworkers;
static volatile u32 syncs_done, syncs_failed, reads_done;

static u32 wrand(struct worker *w) { w->rng = w->rng * 1103515245u + 12345u; return (w->rng >> 8) & 0xFFFFFF; }
static u32 wrn(struct worker *w, u32 n) { return n ? wrand(w) % n : 0; }

static void say(struct worker *w, const char *fmt, ...) {
    va_list ap;
    char line[300];
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    host_out_lock();
    printf("  [w%d op %u] %s (last: %s)\n", w->id, w->ops, line, w->last);
    host_out_unlock();
}

static int lname_eq(const char *a, const char *b) {
    while(*a && *b) { int p = *a, q = *b; if(p >= 'A' && p <= 'Z') p += 32; if(q >= 'A' && q <= 'Z') q += 32; if(p != q) return 0; a++; b++; }
    return *a == *b;
}
static int wfind(struct worker *w, const char *name) {
    int i;
    for(i = 0; i < NF; i++) if(w->f[i].used && lname_eq(w->f[i].name, name)) return i;
    return -1;
}
static void wname(struct worker *w, char *out, size_t n) {
    if(wrn(w, 4)) strlcpy(out, names8[wrn(w, 6)], n);
    else snprintf(out, n, "a long name number %u of worker %d", (unsigned)wrn(w, 3), w->id);
}
static void to11(const char *name, char *n11) {          /* "f3.dat" -> "F3      DAT" */
    int i = 0, k;
    memset(n11, ' ', 11); n11[11] = 0;
    for(k = 0; name[k] && name[k] != '.' && i < 8; k++) n11[i++] = (char)(name[k] >= 'a' && name[k] <= 'z' ? name[k] - 32 : name[k]);
    if(name[k] == '.') for(k++, i = 8; name[k] && i < 11; k++) n11[i++] = (char)(name[k] >= 'a' && name[k] <= 'z' ? name[k] - 32 : name[k]);
}

static u8 *rbuf(void) { return (u8 *)malloc(MAXSZ + 1024); }

/* the worker's directory as the file system shows it, against the model; 0 when they agree */
static int wcompare(struct worker *w, const char *why) {
    struct dirent de;
    u_int i;
    int n = 0, k, bad = 0;
    u8 *b = rbuf();
    for(i = 0; w->dir->ops->readdir(w->dir, i, &de) == 0; i++) {
        struct vnode *v;
        int m = wfind(w, de.name);
        n++;
        if(m < 0) { if(!bad++) say(w, "%s: '%s' is there, not in the model", why, de.name); continue; }
        if(w->dir->ops->lookup(w->dir, de.name, &v)) { if(!bad++) say(w, "%s: '%s' cannot be looked up", why, de.name); continue; }
        if(v->size != w->f[m].size) { if(!bad++) say(w, "%s: '%s' size %u, model %u", why, de.name, v->size, w->f[m].size); }
        else if(v->size && (v->ops->read(v, 0, b, v->size) != (int)v->size || memcmp(b, w->f[m].data, v->size))) {
            if(!bad++) say(w, "%s: '%s' bytes differ", why, de.name);
        }
        vput(v);
    }
    for(k = 0, i = 0; k < NF; k++) if(w->f[k].used) i++;
    if((u_int)n != i && !bad++) say(w, "%s: %d entries, the model %u", why, n, i);
    free(b);
    return bad;
}

/* the file's bytes, as the file system has them now, into the model */
static void wreload(struct worker *w, int m) {
    struct vnode *v;
    u8 *b;
    if(w->dir->ops->lookup(w->dir, w->f[m].name, &v)) { say(w, "reload: '%s' gone", w->f[m].name); w->model_bad++; return; }
    b = rbuf();
    if(v->size > MAXSZ + 1000 || (v->size && v->ops->read(v, 0, b, v->size) != (int)v->size)) { free(b); vput(v); return; }
    w->f[m].data = (u8 *)realloc(w->f[m].data, v->size + 1);
    memcpy(w->f[m].data, b, v->size);
    w->f[m].size = v->size;
    free(b);
    vput(v);
}

/* where the file's entry is: the worker's directory on the disk and in the cache */
static void debug_dir(struct worker *w, const char *n11) {
    struct ggeo g;
    const u8 *p;
    u32 i, c, steps = 0, first = 0, lba;
    host_mutex_lock(x->lock);
    ggeometry(x->mem, &g);
    p = x->mem + g.root_lba * 512;
    for(i = 0; i < g.root_secs * 16; i++, p += 32) if(!memcmp(p, w->d11, 11)) { first = gle16(p + 26); break; }
    host_out_lock();
    printf("      dir %s first cluster %u; chain on the disk:", w->d11, first);
    for(c = first; c >= 2 && c < g.nclusters + 2 && steps++ < 50; ) { printf(" %u", c); c = gfat(x->mem, &g, c); if(geoc(&g, c)) break; }
    printf(" (then %#x)\n", c);
    for(c = first, steps = 0; c >= 2 && c < g.nclusters + 2 && steps++ < 50; ) {
        u32 s;
        for(s = 0; s < g.spc; s++) {
            struct buf *b;
            lba = g.data_lba + (c - 2) * g.spc + s;
            p = x->mem + lba * 512;
            printf("      block %u on the disk:", lba);
            for(i = 0; i < 16; i++) printf(" %02x%c", p[i * 32], !memcmp(p + i * 32, n11, 11) ? '*' : ' ');
            b = bpeek(&x->bd, lba);
            if(b) {
                printf("\n      block %u in the cache (dirty %d order %u wfail %d writing %d):", lba, b->dirty, b->order, b->wfail, b->writing);
                for(i = 0; i < 16; i++) printf(" %02x%c", b->data[i * 32], !memcmp(b->data + i * 32, n11, 11) ? '*' : ' ');
                brelse(b);
            }
            printf("\n");
        }
        c = gfat(x->mem, &g, c);
        if(geoc(&g, c)) break;
    }
    host_out_unlock();
    host_mutex_unlock(x->lock);
}

static int write_heavy, streamers, no_syncer, stream_mode, stream_tags = 1;
/* a streamer: appends 2 KB to its one file, again and again (a background cp), never closing it. 10/17/26 (round
   fs-fix-6): with its file's tag where a block starts (it wrote zeros, which carry none: the oracle saw nothing of
   it); NO_STREAM_TAGS=1 in the environment writes zeros again (for the evidence) */
static volatile u32 stream_fail, stream_ok;
static void stream_op(struct worker *w) {
    struct vnode *v;
    u8 chunk[2048];
    u32 off;
    if(!w->f[0].used) {
        u32 id = 1 + (__sync_add_and_fetch(&file_ids, 1) - 1) % 65534;
        hist_add(w->id, "s.dat", id);                /* (before the name can reach the disk) */
        if(w->dir->ops->create(w->dir, "s.dat", VN_FILE, &v)) return;
        vput(v);
        w->f[0].used = 1; w->f[0].plain = 1; strlcpy(w->f[0].name, "s.dat", sizeof(w->f[0].name)); w->f[0].id = id;
    }
    if(w->dir->ops->lookup(w->dir, "s.dat", &v)) return;
    if(v->size > 200000) { v->ops->truncate(v, 0); w->f[0].size = 0; }
    w->where = "stream";
    off = stream_mode == 2 ? 0 : v->size;           /* 2: rewrites its first 2 KB (a log kept short) */
    memset(chunk, 0, sizeof(chunk));
    if(stream_tags) tag_fill(chunk, off, sizeof(chunk), w->f[0].id);
    if(v->ops->write(v, off, chunk, sizeof(chunk)) == (int)sizeof(chunk)) stream_ok++;
    else stream_fail++;
    w->f[0].size = v->size;
    w->where = "between";
    vput(v);
}
static void wop(struct worker *w) {
    u32 what = wrn(w, 100);
    if(write_heavy && what >= 15 && what < 90) what = 20;   /* mostly writes */
    char name[64];
    int m, r;
    struct vnode *v;
    if(what < 15) {                                        /* create */
        wname(w, name, sizeof(name));
        if(wfind(w, name) >= 0) return;
        for(m = 0; m < NF && w->f[m].used; m++) ;
        if(m == NF) return;
        w->where = "create";
        {
            u32 id = 1 + (__sync_add_and_fetch(&file_ids, 1) - 1) % 65534;   /* (16 bits, never 0) */
            hist_add(w->id, name, id);                 /* (round fs-fix-6: before the name can reach the disk) */
            r = w->dir->ops->create(w->dir, name, VN_FILE, &v);
            snprintf(w->last, sizeof(w->last), "create %s: %d", name, r);
            if(!r) {
                w->f[m].used = 1; w->f[m].plain = !strchr(name, ' '); strlcpy(w->f[m].name, name, sizeof(w->f[m].name)); w->f[m].size = 0;
                w->f[m].id = id;
                vput(v);
            }
            else { w->fails++; if(wcompare(w, "after a failed create")) w->b5_bad++; }
        }
    } else if(what < 55) {                                 /* write, and sometimes close */
        u32 off, len, i;
        u8 *b;
        int k = wrn(w, NF);
        for(m = -1, i = 0; i < NF; i++) if(w->f[(k + i) % NF].used) { m = (k + i) % NF; break; }
        if(m < 0) return;
        if(w->dir->ops->lookup(w->dir, w->f[m].name, &v)) { say(w, "lookup of '%s' failed", w->f[m].name); w->model_bad++; return; }
        off = wrn(w, 3) ? v->size : wrn(w, v->size + 1);
        len = 1 + wrn(w, wrn(w, 3) ? 3000 : 16000);
        if(off + len > MAXSZ) { vput(v); return; }
        b = rbuf();
        for(i = 0; i < len; i++) b[i] = (u8)wrand(w);
        tag_fill(b, off, len, w->f[m].id);              /* (round fs-fix-5: the file's tag where a block starts) */
        w->where = "write";
        r = v->ops->write(v, off, b, len);
        snprintf(w->last, sizeof(w->last), "write %s off %u len %u: %d", w->f[m].name, off, len, r);
        free(b);
        if(r != (int)len) w->fails++;
        wreload(w, m);
        if(r == (int)len && wrn(w, 3) == 0 && w->f[m].plain) {   /* close: vfs_close()'s write-back */
            int s1, s2;
            long ds;
            char n11[12];
            w->where = "close";
            s1 = sb->ops->sync(sb);
            s2 = bsync(&x->bd);
            to11(w->f[m].name, n11);
            ds = disk_size(x, w->d11, n11);
            snprintf(w->last, sizeof(w->last), "close %s: %d %d, size %u, on the disk %ld", w->f[m].name, s1, s2, v->size, ds);
            if(s1 == 0 && s2 == 0 && ds != -2) {
                w->close_checks++;
                if(ds != (long)v->size) {
                    if(!w->close_stale++) {
                        say(w, "close's write-back said 0, yet '%s' has size %ld on the disk, %u in memory", w->f[m].name, ds, v->size);
                        debug_dir(w, n11);
                    }
                }
            }
        }
        vput(v);
    } else if(what < 67) {                                 /* truncate */
        u32 ns, old;
        int k = wrn(w, NF), i;
        for(m = -1, i = 0; i < NF; i++) if(w->f[(k + i) % NF].used) { m = (k + i) % NF; break; }
        if(m < 0) return;
        if(w->dir->ops->lookup(w->dir, w->f[m].name, &v)) return;
        old = v->size;
        ns = wrn(w, 3) == 0 ? 0 : wrn(w, old + 2000);
        if(ns > MAXSZ) ns = MAXSZ;
        w->where = "truncate";
        r = v->ops->truncate(v, ns);
        snprintf(w->last, sizeof(w->last), "truncate %s %u -> %u: %d", w->f[m].name, old, ns, r);
        vput(v);
        if(!r) {
            w->f[m].data = (u8 *)realloc(w->f[m].data, ns + 1);
            if(ns > w->f[m].size) memset(w->f[m].data + w->f[m].size, 0, ns - w->f[m].size);
            w->f[m].size = ns;
        } else {
            w->fails++;
            if(ns <= old) { if(wcompare(w, "after a failed truncate")) w->b5_bad++; }
            else wreload(w, m);
        }
    } else if(what < 80) {                                 /* rm */
        int k = wrn(w, NF), i;
        for(m = -1, i = 0; i < NF; i++) if(w->f[(k + i) % NF].used) { m = (k + i) % NF; break; }
        if(m < 0) return;
        w->where = "rm";
        r = w->dir->ops->unlink(w->dir, w->f[m].name);
        snprintf(w->last, sizeof(w->last), "rm %s: %d", w->f[m].name, r);
        if(!r) { w->f[m].used = 0; free(w->f[m].data); w->f[m].data = NULL; w->f[m].size = 0; }
        else { w->fails++; if(wcompare(w, "after a failed rm")) w->b5_bad++; }
    } else if(what < 95) {                                 /* mv, within the directory */
        int k = wrn(w, NF), i, d;
        for(m = -1, i = 0; i < NF; i++) if(w->f[(k + i) % NF].used) { m = (k + i) % NF; break; }
        if(m < 0) return;
        wname(w, name, sizeof(name));
        d = wfind(w, name);
        if(d == m) return;
        w->where = "mv";
        hist_add(w->id, name, w->f[m].id);             /* (round fs-fix-6: before the new name can reach the disk) */
        r = w->dir->ops->rename(w->dir, w->f[m].name, w->dir, name);
        snprintf(w->last, sizeof(w->last), "mv %s %s%s: %d", w->f[m].name, name, d >= 0 ? " (replacing)" : "", r);
        if(!r) {
            if(d >= 0) { w->f[d].used = 0; free(w->f[d].data); w->f[d].data = NULL; }
            strlcpy(w->f[m].name, name, sizeof(w->f[m].name));
            w->f[m].plain = 0;                             /* (a replacing mv gives it a ~N alias) */
        } else { w->fails++; if(wcompare(w, "after a failed mv")) w->b5_bad++; }
    } else {                                               /* a look at the whole directory */
        w->where = "compare";
        if(wcompare(w, "a look")) w->model_bad++;
    }
    w->where = "between";
}

static void worker_main(void *p) {
    struct worker *w = (struct worker *)p;
    while(running) {
        if(w->id >= nworkers - streamers) stream_op(w);
        else wop(w);
        count_check();
        w->ops++;
        host_progress++;
    }
}

static void reader_main(void *p) {
    u32 rng = 7, i;
    (void)p;
    while(running) {
        u32 base;
        rng = rng * 1103515245u + 12345u;
        base = (rng >> 8) % 7000;
        for(i = 0; i < 300 && running; i++) {
            struct buf *b = (rng & 0x100) ? breada(&y->bd, base + i, 8) : bread(&y->bd, base + i);
            if(b) brelse(b);
        }
        count_check();
        reads_done++;
        host_progress++;
    }
}

static void syncer_main(void *p) {
    (void)p;
    while(running) {
        host_sleep_us(3000);
        if(no_syncer) continue;
        if(bsync(NULL) != 0) syncs_failed++;
        count_check();
        syncs_done++;
        host_progress++;
    }
}

static int chaos_on;
static void chaos_main(void *p) {
    u32 rng = 99;
    (void)p;
    while(running) {
        rng = rng * 1103515245u + 12345u;
        host_sleep_us(20000 + (rng >> 8) % 60000);
        if(!chaos_on || !running) continue;
        x->fail_all = 1;
        host_sleep_us(2000 + (rng >> 12) % 8000);
        x->fail_all = 0;
    }
}

static void dump(void) {
    int i;
    for(i = 0; i < nworkers; i++) printf("  worker %d: op %u, in %s, last: %s\n", i, workers[i].ops, workers[i].where, workers[i].last);
    printf("  syncs %u, reads %u\n", syncs_done, reads_done);
}

/**** 10/17/26 (round fs-fix-7, audit B46): the review of B39-B45's R1 as it wrote it, with threads: Ctrl+C, a kill or a
      hangup during umount while another thread's write of the volume's device is in flight (a `sync` on another console,
      vfs_sync()'s bsync(NULL); each write of the data area takes 0.1 s). The umount (fs/vfs.c's vfs_umount_lost() once
      the mount is off the list) found its waits cut short: it gave the changes up and left a working card marked as not
      cleanly unmounted. Now it is a step that must finish (fs_finish): it waits for that write, gives nothing up, marks
      the volume clean, and the next mount reads every file. usage: conc umount ****/

#ifdef TI_NOCALL
#define UM_FINISH_ENTER(t) ((t)->fs_hold++, (t)->fs_finish++)
#define UM_FINISH_LEAVE(t) ((t)->fs_hold--, (t)->fs_finish--)
#else                                               /* (BEFORE builds against 3fe29bf: no step that must finish) */
#define UM_FINISH_ENTER(t) ((void)(t))
#define UM_FINISH_LEAVE(t) ((void)(t))
#endif
static volatile int um_sync_r;
static void um_syncer(void *p) { (void)p; um_sync_r = bsync(NULL); count_check(); }
static struct superblock *um_mount(struct ram *r) {
    struct superblock *s = (struct superblock *)calloc(1, sizeof(*s));
    s->type = fat;
    s->dev = &r->bd;
    mutex_init(&s->lock, "fat");
    return fat->mount(s) == 0 ? s : NULL;
}
static int um_file(struct vnode *dir, const char *name, const u8 *data, u32 len, int write) {   /* written, or as written */
    static u8 back[8192];
    struct vnode *v;
    int ok;
    if(write ? dir->ops->create(dir, name, VN_FILE, &v) : dir->ops->lookup(dir, name, &v)) return 0;
    if(write) ok = v->ops->write(v, 0, data, len) == (int)len;
    else ok = v->size == len && v->ops->read(v, 0, back, len) == (int)len && !memcmp(back, data, len);
    vput(v);
    return ok;
}
static int umount_case(int mode) {
    static const char *what[] = { "Ctrl+C (TI_INT, a kernel command)", "a kill (TI_KILL, a system call)",
                                  "a hangup (TI_KILL and TI_HANGUP, in a call)", "the control: no interrupt" };
    struct ram *r = ram_new("cu", 8192, 0);
    surf_task *t = *fatconc_task();                 /* this thread's task, as the file system sees it */
    struct superblock *s, *s2;
    struct ggeo g;
    static u8 a[4096], b[2048];
    char msg[256];
    void *th;
    int s1, s2r, a_ok, b_ok, clean, bad;
    u_int gave_up;
    u32 i, said0;
    u_long f;
    for(i = 0; i < sizeof(a); i++) a[i] = (u8)(i * 7 + mode);
    for(i = 0; i < sizeof(b); i++) b[i] = (u8)(i * 13 + mode + 1);
    if(fat_mkfs(&r->bd, "UMOUNT", 0, msg, sizeof(msg)) != 0 || !(s = um_mount(r))) { printf("umount %d: setup\n", mode); return 1; }
    if(!um_file(s->root, "a", a, sizeof(a), 1) || s->ops->sync(s) || bsync(&r->bd)) { printf("umount %d: a\n", mode); return 1; }
    if(!um_file(s->root, "b", b, sizeof(b), 1)) { printf("umount %d: b\n", mode); return 1; }   /* changed, not written */
    ggeometry(r->mem, &g);
    r->slow_from = g.data_lba;
    r->slow_seen = 0;
    r->slow_us = 100000;
    um_sync_r = 1;
    said0 = said_unclean;
    th = host_thread(um_syncer, NULL, "sync");
    while(!r->slow_seen) host_sleep_us(500);        /* its write of b's block is in flight */
    f = irq_save();
    if(mode == 0) { t->in_call = TC_COMMAND; t->intr = TI_INT; }                 /* task_interrupt_job() */
    else if(mode == 1) { t->in_call = TC_SYSCALL; t->intr = TI_KILL; }           /* kill_task() */
    else if(mode == 2) { t->in_call = TC_SYSCALL; t->intr = TI_KILL | TI_HANGUP; }   /* kill_hangup() */
    else t->in_call = TC_COMMAND;
    irq_restore(f);
    UM_FINISH_ENTER(t);                             /* vfs_umount_lost(): the mount is off the list */
    s1 = s->ops->sync(s);
    s2r = bsync(&r->bd);
    vput(s->root);
    s->ops->unmount(s);
    gave_up = bdirty_count(&r->bd);
    binvalidate(&r->bd);
    UM_FINISH_LEAVE(t);
    f = irq_save();
    t->in_call = 0;
    t->intr = 0;
    irq_restore(f);
    host_join(th);
    r->slow_us = 0;
    host_progress++;
    clean = (gfat(r->mem, &g, 1) & (g.type == 32 ? 0x08000000 : 0x8000)) != 0;
    s2 = um_mount(r);
    a_ok = s2 && um_file(s2->root, "a", a, sizeof(a), 0);
    b_ok = s2 && um_file(s2->root, "b", b, sizeof(b), 0);
    bad = gave_up != 0 || !clean || !a_ok || !b_ok || said_unclean != said0 || um_sync_r != 0;
    printf("CONC umount, %s: the other thread's sync %d; umount: fs sync %d, bsync %d, %u changed blocks given up, %u lines said"
           " the volume unclean or something lost; the card: clean bit %d, a %s, b %s: %s\n", what[mode], um_sync_r, s1, s2r, gave_up,
           said_unclean - said0, clean, a_ok ? "ok" : "BAD", b_ok ? "ok" : "MISSING", bad ? "FAIL" : "ok");
    if(s2) { vput(s2->root); s2->ops->unmount(s2); }
    binvalidate(&r->bd);
    return bad;
}
static int umount_main(void) {
    int bad = 0, m;
    host_init();
    quiet_kprintf = 1;
    fatfs_init();
    init_bcache();
    host_watchdog(30, NULL);
    for(m = 0; m < 4; m++) bad += umount_case(m);
    host_watchdog_stop();
    count_check();                                  /* 10/18/26 (B70): the counts, this thread's and the syncers' */
    if(count_left) { printf("CONC umount: mutex counts left after a step: %u\n", count_left); bad++; }
    printf("CONC umount: %s\n", bad ? "FAIL" : "ok");
    return bad ? 1 : 0;
}

/**** 10/18/26 (round fs-fix-11, audit B70): the shim's mutex against the kernel's rules (kernel/sync.c since B63): three
      threads queue for a mutex this one holds; let go and taken again at once, it comes back to this one only after
      each of them has had it, in the order they came; each counts it while it holds it and not after; a trylock
      fails while it is held. (The shim before was wake-and-compete: this thread took it straight back.) usage: conc
      mutex ****/
static mutex_t mx;
static volatile int mx_order[3], mx_n, mx_bad;
static void mx_waiter(void *p) {
    surf_task *me = *fatconc_task();
    mutex_lock(&mx);
    mx_order[mx_n++] = (int)(long)p;
    if(me->mutexes != 1) mx_bad++;
    host_sleep_us(2000);
    mutex_unlock(&mx);
    if(me->mutexes != 0) mx_bad++;
}
static int mutex_main(void) {
    static const char *names[3] = { "q1", "q2", "q3" };
    surf_task *me = *fatconc_task();
    void *th[3];
    int i, waited, back_after, bad;
    host_init();
    mutex_init(&mx, "conc-mutex");
    mutex_lock(&mx);
    for(i = 0; i < 3; i++) {                        /* each in line before the next comes */
        th[i] = host_thread(mx_waiter, (void *)(long)(i + 1), names[i]);
        for(waited = 0; (int)mx.waiters.count < i + 1 && waited < 1000; waited++) host_sleep_us(1000);
        host_sleep_us(20000);
    }
    bad = me->mutexes != 1 || mutex_trylock(&mx);   /* held: one counted, a trylock refused */
    mutex_unlock(&mx);
    mutex_lock(&mx);                                /* at once: behind the three */
    back_after = mx_n;
    if(me->mutexes != 1) bad++;
    mutex_unlock(&mx);
    for(i = 0; i < 3; i++) host_join(th[i]);
    bad += mx_bad + (me->mutexes != 0) + (back_after != 3) + (mx_order[0] != 1 || mx_order[1] != 2 || mx_order[2] != 3);
    printf("CONC mutex: taken back after %d of the 3 waiters; their order %d %d %d; counts wrong %d: %s\n", back_after,
           mx_order[0], mx_order[1], mx_order[2], mx_bad, bad ? "FAIL" : "ok");
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    u32 seed = argc > 1 ? (u32)atoi(argv[1]) : 1, secs = argc > 2 ? (u32)atoi(argv[2]) : 10;
    int i, bad = 0, s1, s2;
    u32 io_us;
    void *th[16];
    char msg[256];
    if(argc > 1 && !strcmp(argv[1], "umount")) return umount_main();   /* 10/17/26 (round fs-fix-7, B46) */
    if(argc > 1 && !strcmp(argv[1], "mutex")) return mutex_main();     /* 10/18/26 (round fs-fix-11, B70) */
    nworkers = argc > 3 ? atoi(argv[3]) : 3;
    chaos_on = argc > 4 ? atoi(argv[4]) : 1;
    io_us = argc > 5 ? (u32)atoi(argv[5]) : 30;
    write_heavy = argc > 6 ? atoi(argv[6]) : 0;
    streamers = argc > 7 ? atoi(argv[7]) : 0;
    no_syncer = argc > 8 ? atoi(argv[8]) : 0;
    stream_mode = argc > 9 ? atoi(argv[9]) : 1;
    if(nworkers < 1 || nworkers > 8) nworkers = 3;
    host_init();
    quiet_kprintf = 1;
    fatfs_init();
    init_bcache();
    {
        u32 blocks = argc > 10 ? (u32)atoi(argv[10]) : 16384;
        x = ram_new("cx", blocks, io_us);
    }
    y = ram_new("cy", 8192, io_us);
    if(getenv("ONLY_MIXED")) only_mixed = 1;        /* 10/17/26 (round fs-fix-6): round fs-fix-5's oracle alone */
    if(getenv("NO_STREAM_TAGS")) stream_tags = 0;   /*      and its untagged streamer (for the evidence) */
    if(fat_mkfs(&x->bd, "CONC", argc > 11 ? atoi(argv[11]) : 0, msg, sizeof(msg)) != 0) { printf("mkfs: %s\n", msg); return 2; }
    sb = (struct superblock *)calloc(1, sizeof(*sb));
    sb->type = fat; sb->dev = &x->bd;
    mutex_init(&sb->lock, "fat");
    if(fat->mount(sb) != 0) { printf("mount failed\n"); return 2; }
    workers = (struct worker *)calloc(nworkers, sizeof(struct worker));
    for(i = 0; i < nworkers; i++) {
        struct worker *w = &workers[i];
        struct vnode *d;
        w->id = i;
        w->rng = seed * 7919 + i * 104729;
        snprintf(w->dname, sizeof(w->dname), "t%d", i);
        to11(w->dname, w->d11);
        if(sb->root->ops->create(sb->root, w->dname, VN_DIR, &d)) { printf("mkdir t%d failed\n", i); return 2; }
        w->dir = d;
        w->where = "start";
        strlcpy(w->last, "-", sizeof(w->last));
    }
    if(sb->ops->sync(sb) || bsync(&x->bd)) { printf("first sync failed\n"); return 2; }
    x->oracle = 1;
    host_watchdog(30, dump);
    for(i = 0; i < nworkers; i++) {
        static char names[8][8];
        snprintf(names[i], sizeof(names[i]), "w%d", i);
        th[i] = host_thread(worker_main, &workers[i], names[i]);
    }
    th[nworkers] = host_thread(reader_main, NULL, "reader");
    th[nworkers + 1] = host_thread(syncer_main, NULL, "syncer");
    th[nworkers + 2] = host_thread(chaos_main, NULL, "chaos");
    {
        unsigned long end = host_ms() + secs * 1000UL;
        while(host_ms() < end) host_sleep_us(100000);
    }
    running = 0;
    for(i = 0; i < nworkers + 3; i++) host_join(th[i]);
    host_watchdog_stop();
    x->fail_all = 0;
#ifndef BEFORE
    printf("     before the final sync: the card's dirty blocks %u, of them failed %u; writes refused by the card in all %u\n",
           bdirty_count(&x->bd), x->bd.bc_failed, x->failed);
#endif
    s1 = sb->ops->sync(sb);
    s2 = bsync(&x->bd);
    for(i = 0; i < nworkers - streamers; i++) if(wcompare(&workers[i], "at the end")) bad++;
    printf("CONC seed %u, %u s, %d workers, chaos %d, io %u us: ops", seed, secs, nworkers, chaos_on, io_us);
    for(i = 0; i < nworkers; i++) printf(" %u", workers[i].ops);
    printf(", failed ops");
    for(i = 0; i < nworkers; i++) printf(" %u", workers[i].fails);
    printf("; syncs %u (%u failed), reads %u; disk writes %u, refused %u, zero-length %u\n", syncs_done, syncs_failed, reads_done,
           x->writes, x->failed, zero_writes);
    if(streamers) printf("     streamers: %u appends written, %u failed\n", stream_ok, stream_fail);
    printf("     stale data: tags checked exactly %u, exact violations %u, two files' tags in one range %u%s%s\n", exact_checked,
           exact_viol, mixed_viol, only_mixed ? " (ONLY_MIXED: no exact check)" : "", stream_tags ? "" : " (NO_STREAM_TAGS)");
    printf("     final syncs %d %d, dirty left %u; oracle %u%s%s; audit %d %d; B5", s1, s2, bdirty_count(&x->bd), x->violations,
           x->violations ? ": " : "", x->violations ? x->violation : "", bcache_audit(&x->bd), bcache_audit(&y->bd));
    for(i = 0; i < nworkers; i++) printf(" %u", workers[i].b5_bad);
    printf("; model");
    for(i = 0; i < nworkers; i++) printf(" %u", workers[i].model_bad);
    printf("; close checks");
    for(i = 0; i < nworkers; i++) printf(" %u/%u stale", workers[i].close_stale, workers[i].close_checks);
    printf("; end compare bad %d; mutex counts left between operations %u\n", bad, count_left);
    {   /* 10/16/26: what the review read off these lines, as the exit code */
        u32 b5 = 0, model = 0, stale = 0;
        for(i = 0; i < nworkers; i++) { b5 += workers[i].b5_bad; model += workers[i].model_bad; stale += workers[i].close_stale; }
        bad += (s1 != 0) + (s2 != 0) + (bdirty_count(&x->bd) != 0) + (x->violations != 0) + (zero_writes != 0) +
               (bcache_audit(&x->bd) != 0) + (bcache_audit(&y->bd) != 0) + (b5 != 0) + (model != 0) + (stale != 0) +
               (count_left != 0);
    }
    printf("CONC: %s\n", bad ? "FAIL" : "ok");
    return bad ? 1 : 0;
}
