/* The kernel's side of System Monitor and Disks (shell/guisys.c, shell/guisys_disk.c) built into test_guisys, with
   stand-ins for the rest of the kernel, as tests/host/test_usbmouse.c builds a driver: tasks as plain structures
   (curTask, the shells above it, idle, init, programs in and out of a call), block devices and mounts as tables
   in lsblk's order, statfs answers and failures the test sets, and an allocator that can fail. Interrupts are a
   no-op here: the kernel's irq_save() is privileged. test_guisys.c includes this first, so that the kernel's
   headers come before the desktop's (shell/guipriv.h then takes their errno values). 10/18/26, track SYS. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#define _SURFOS_IRQ_H
static u_long irq_save(void) { return 0; }
static void irq_restore(u_long flags) { (void)flags; }
#include <surfos/task.h>
#include <surfos/proc.h>
#include <surfos/cpu.h>
#include <surfos/multiboot.h>
#include <mm/kalloc.h>
#include <mm/pmm.h>
#include <sys/bdev.h>
#include <sys/framebuffer.h>
#include <surfos/win.h>                 /* 10/25/26: struct win_state, for programs' windows' stand-ins */
#include <fs/vfs.h>

void setpos(surf_console *con) { (void)con; }      /* surfos/console.h declares it inline; kernel/console.c's */

/**** tasks ****/

#define KTASKS 16
surf_task *curTask, *idle_task, *init_task_ptr;
static surf_task ktask[KTASKS];
static int nktasks;
static int patient_kills;               /* kill_task_patient() calls, and the last task it was given */
static surf_task *patient_last;

static surf_task *ktask_add(int pid, const char *name, u_int flags, surf_task *parent, u_int in_call) {
    surf_task *t = &ktask[nktasks++];
    memset(t, 0, sizeof(*t));
    t->pid = (u_long)pid;
    strlcpy(t->name, name, sizeof(t->name));
    t->flags = flags;
    t->parent = parent;
    t->in_call = in_call;
    t->state = TS_BLOCKED;
    return t;
}

surf_task *task_find(u_long pid) {
    int i;
    for(i = 0; i < nktasks; i++) if(ktask[i].pid == pid) return &ktask[i];
    return NULL;
}

/* as kernel/task.c's: gone, ending (TC_EXIT) or killed already, that stands; in its own code a task ends at once;
   inside a call it is marked and ends with the call */
int kill_task_patient(surf_task *t) {
    patient_kills++;
    patient_last = t;
    if(t->state == TS_DEAD || (t->in_call & TC_EXIT) || (t->intr & TI_KILL)) return t->state != TS_DEAD && t->in_call;
    if(!t->in_call) {
        t->state = TS_DEAD;
        return 0;
    }
    t->intr |= TI_KILL | TI_PATIENT;
    return 1;
}

u_int task_count(void) { return (u_int)nktasks; }

int task_info_all(struct procinfo *out, int max) {
    int i, n = 0, skip = nktasks > max ? nktasks - max : 0;
    for(i = skip; i < nktasks && n < max; i++, n++) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].pid = (int)ktask[i].pid;
        out[n].ppid = ktask[i].parent ? (int)ktask[i].parent->pid : -1;
        out[n].state = ktask[i].state == TS_DEAD ? PROC_DEAD : PROC_BLOCKED;
        out[n].flags = (ktask[i].flags & TF_USER) ? PROC_USER : 0;
        out[n].tty = -1;
        strlcpy(out[n].name, ktask[i].name, sizeof(out[n].name));
    }
    return n;
}

/**** the machine ****/

struct cpu_info cpu;
struct boot_info bootinfo;
u_long uptime_ms(void) { return 3725000; }
u_int ktimers_count(void) { return 3; }
u_long klog_copy(char *buf, u_long size, u_long *written) { (void)buf; (void)size; *written = 0; return 0; }
u_long pmm_total_frames(void) { return 124589; }
u_long pmm_free_frames(void) { return 123000; }
u64 pmm_ram_bytes(void) { return (u64)517868 * 1024; }
u64 pmm_ram_high_bytes(void) { return 0; }
void kheap_stats(struct heap_stats *st) { memset(st, 0, sizeof(*st)); }
void dmaheap_stats(struct heap_stats *st) { memset(st, 0, sizeof(*st)); }
const char *framebuffer_describe(char *buf, size_t size) { strlcpy(buf, "none", size); return buf; }
int vfs_open(const char *path, int flags, struct file **out) { (void)path; (void)flags; *out = NULL; return -ENOENT; }
int vfs_read(struct file *f, void *buf, u32 len) { (void)f; (void)buf; (void)len; return -EIO; }
int vfs_close(struct file *f) { (void)f; return 0; }
/* the Terminal's side (shell/guisys_term.c, track TERM) is not this test's */
int guisys_term_open(void *ctx, int cols, int rows) { (void)ctx; (void)cols; (void)rows; return -ENOSYS; }
int guisys_term_read(void *ctx, int h, char *buf, u32 n) { (void)ctx; (void)h; (void)buf; (void)n; return -ENOSYS; }
int guisys_term_write(void *ctx, int h, const char *buf, u32 n) { (void)ctx; (void)h; (void)buf; (void)n; return -ENOSYS; }
int guisys_term_resize(void *ctx, int h, int cols, int rows) { (void)ctx; (void)h; (void)cols; (void)rows; return -ENOSYS; }
void guisys_term_close(void *ctx, int h) { (void)ctx; (void)h; }
/* 10/25/26: nor are programs' windows (shell/guisys_win.c) */
int guisys_win_accept(void *ctx) { (void)ctx; return -EAGAIN; }
int guisys_win_update(void *ctx, int h, struct win_state *st, struct win_view *view) {
    (void)ctx; (void)h; (void)st; (void)view; return -EINVAL;
}
int guisys_win_event(void *ctx, int h, const struct win_event *e) { (void)ctx; (void)h; (void)e; return -EINVAL; }
void guisys_win_close(void *ctx, int h) { (void)ctx; (void)h; }

/**** memory: an allocator that can fail ****/

static char kpool[64 * 1024];
static u32 kpool_used;
static int kalloc_fails;                /* the next kalloc() fails */
void *kalloc(size_t size) {
    void *p;
    if(kalloc_fails) { kalloc_fails = 0; return NULL; }
    size = (size + 15) & ~(size_t)15;
    if(kpool_used + size > sizeof(kpool)) kpool_used = 0;   /* the test's calls never hold two at once */
    p = kpool + kpool_used;
    kpool_used += size;
    return p;
}
void kfree(void *mem) { (void)mem; }

/**** block devices, mounts and statfs, in lsblk's order ****/

#define KDEVS 48
static struct bdev *kdev[KDEVS];        /* bdev_walk()'s order: each disk, then its partitions */
static int nkdevs;
static struct vfs_mount_view kview[8];
static int nkviews;
static struct { char path[VFS_PATH_MAX]; int err; u32 unit, units, free_units; char fs[8], label[12]; } kstat[8];
static int nkstats;

static struct bdev *kdev_add(struct bdev *d, const char *name, u32 nblocks, struct bdev *parent, u8 bus, bool listed) {
    memset(d, 0, sizeof(*d));
    strlcpy(d->name, name, sizeof(d->name));
    d->nblocks = nblocks;
    d->parent = parent;
    d->bus = parent ? parent->bus : bus;
    d->removable = d->bus == BDEV_BUS_USB;
    d->type_name = parent ? "FAT32 LBA" : NULL;
    d->gone = !listed;
    if(listed && nkdevs < KDEVS) kdev[nkdevs++] = d;
    return d;
}

static void kmount(const char *path, const char *type, struct bdev *dev, bool readonly) {
    struct vfs_mount_view *v = &kview[nkviews++];
    memset(v, 0, sizeof(*v));
    strlcpy(v->path, path, sizeof(v->path));
    v->type = type;
    v->dev = dev;
    v->readonly = readonly;
}

static void kstatfs(const char *path, int err, u32 units, u32 free_units, const char *fs, const char *label) {
    int i = nkstats++;
    memset(&kstat[i], 0, sizeof(kstat[i]));
    strlcpy(kstat[i].path, path, sizeof(kstat[i].path));
    kstat[i].err = err;
    kstat[i].unit = 512;
    kstat[i].units = units;
    kstat[i].free_units = free_units;
    strlcpy(kstat[i].fs, fs, sizeof(kstat[i].fs));
    strlcpy(kstat[i].label, label, sizeof(kstat[i].label));
}

void bdev_walk(void (*fn)(struct bdev *d, void *arg), void *arg) {
    int i;
    for(i = 0; i < nkdevs; i++) fn(kdev[i], arg);
}

struct bdev *bdev_disk(struct bdev *d) {
    while(d && d->parent) d = d->parent;
    return d;
}

bool bdev_mount_readonly(struct bdev *d) { return d->protect || bdev_disk(d)->protect; }

/* lsblk's words, enough of them: the size in MB, disk or part, the bus, the model, a status of mounted or none */
void bdev_describe(struct bdev *d, struct bdev_info *r) {
    int j;
    snprintf(r->size, sizeof(r->size), "%uM", d->nblocks / 2048);
    if(d->parent) snprintf(r->type, sizeof(r->type), "part mbr %s", d->type_name ? d->type_name : "?");
    else strlcpy(r->type, "disk", sizeof(r->type));
    r->bus = d->bus == BDEV_BUS_USB ? "usb" : d->bus == BDEV_BUS_RAM ? "ram" : "-";
    r->label = d->parent ? d->part_label : d->model;
    r->status = d->protect ? "boot drive: protected" : "";
    for(j = 0; j < nkviews; j++) if(kview[j].dev == d) r->status = "mounted";
}

int vfs_mount_views(struct vfs_mount_view *out, int max) {
    int j;
    for(j = 0; j < nkviews && j < max; j++) out[j] = kview[j];
    return nkviews;
}

int vfs_statfs(const char *path, struct statfs *st) {
    int i;
    memset(st, 0, sizeof(*st));
    for(i = 0; i < nkstats && strcmp(kstat[i].path, path); i++) ;
    if(i == nkstats) return 0;                          /* a file system that keeps no count: all zero */
    if(kstat[i].err) return kstat[i].err;
    st->unit = kstat[i].unit;
    st->units = kstat[i].units;
    st->free_units = kstat[i].free_units;
    strlcpy(st->fs, kstat[i].fs, sizeof(st->fs));
    strlcpy(st->label, kstat[i].label, sizeof(st->label));
    return 0;
}

#include "../../shell/guisys.c"
#include "../../shell/guisys_disk.c"
