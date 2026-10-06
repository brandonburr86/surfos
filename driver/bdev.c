/*
SurfOS Block Device Layer
--------------------
File: bdev.c    Date: 10/3/26 (roadmap D2)
--------------------
The registry of block devices and the one place that checks ranges and
translates partition offsets. See include/sys/bdev.h.

10/5/26 (USB storage): unregistering an unplugged disk, reference counts for
mounts, partition names, flushing, and bdev_may_format(), the safety rule of
the destructive tools.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/irq.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <blibc_common.h>

static struct bdev *bdevs;
static u_int nbdevs;

int bdev_register(struct bdev *d) {
    struct bdev **pp;
    if(!d->block_size) d->block_size = BDEV_BLOCK_SIZE;
    for(pp = &bdevs; *pp; pp = &(*pp)->next) {
        if(!strcmp((*pp)->name, d->name)) return -1;
    }
    d->next = NULL;
    *pp = d;
    nbdevs++;
    return 0;
}

/* An unplugged disk: it and its partitions fail every I/O from now on, leave the list
   (so the name can be used again) and drop their unheld cached blocks. They are never
   freed: a mount, a held cache block or a command may still point at them. */
void bdev_unregister(struct bdev *d) {
    struct bdev **pp, *off[64];
    u_int n = 0, i;
    u_long f = irq_save();
    for(pp = &bdevs; *pp; ) {
        struct bdev *x = *pp;
        if(x == d || x->parent == d) {
            x->gone = true;
            *pp = x->next;
            x->next = NULL;
            nbdevs--;
            if(n < 64) off[n++] = x;
        } else pp = &x->next;
    }
    d->gone = true;
    irq_restore(f);
    for(i = 0; i < n; i++) binvalidate(off[i]);
    if(!n) binvalidate(d);
}

void bdev_get(struct bdev *d) {
    u_long f = irq_save();
    d->refs++;
    if(d->parent) d->parent->refs++;
    irq_restore(f);
}

void bdev_put(struct bdev *d) {
    u_long f = irq_save();
    if(d->refs) d->refs--;
    if(d->parent && d->parent->refs) d->parent->refs--;
    irq_restore(f);
}

struct bdev *bdev_disk(struct bdev *d) {
    while(d && d->parent) d = d->parent;
    return d;
}

void bdev_part_name(const struct bdev *disk, u_int n, char *out, size_t size) {
    size_t l = strlen(disk->name);
    bool digit = l && disk->name[l - 1] >= '0' && disk->name[l - 1] <= '9';
    snprintf(out, size, digit ? "%sp%u" : "%s%u", disk->name, n);
}

int bdev_may_format(struct bdev *d, char *why, size_t size) {
    struct bdev *disk = bdev_disk(d);
    const char *no = NULL;
    if(!d || !disk) no = "no such device";
    else if(d->gone || disk->gone) no = "it was unplugged";
    else if(disk->protect) no = disk->protect;
    else if(d->readonly || disk->readonly) no = "it is read-only";
    else if(disk->bus != BDEV_BUS_USB && disk->bus != BDEV_BUS_RAM)
        no = "it is not a USB drive (internal disks are never repartitioned or formatted)";
    else if(vfs_bdev_busy(d)) no = "it, its disk or one of its partitions is mounted (umount or eject first)";
    if(no) {
        if(why && size) snprintf(why, size, "%s", no);
        return -1;
    }
    if(why && size) why[0] = 0;
    return 0;
}

struct bdev *bdev_find(const char *name) {
    struct bdev *d;
    for(d = bdevs; d; d = d->next) if(!strcmp(d->name, name)) return d;
    return NULL;
}

struct bdev *bdev_first(void) {
    return bdevs;
}

u_int bdev_count(void) {
    return nbdevs;
}

/* walk up to the disk that owns the blocks; lba becomes a disk block number */
static struct bdev *resolve(struct bdev *d, u32 *lba, u32 count) {
    while(d) {
        if(*lba + count < *lba || *lba + count > d->nblocks) return NULL;   /* overflow or past the end */
        if(!d->parent) return d;
        *lba += d->start;
        d = d->parent;
    }
    return NULL;
}

int bdev_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    struct bdev *disk = resolve(d, &lba, count);
    int r;
    if(d->gone || (disk && disk->gone)) return BDEV_EGONE;
    if(!disk || !disk->ops || !disk->ops->read) return BDEV_ERANGE;
    if(!count) return 0;
    r = disk->ops->read(disk, lba, count, buf);
    if(r == 0) { d->reads += count; if(disk != d) disk->reads += count; }
    return r;
}

int bdev_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct bdev *disk = resolve(d, &lba, count);
    int r;
    if(d->gone || (disk && disk->gone)) return BDEV_EGONE;
    if(!disk || !disk->ops || !disk->ops->write) return BDEV_ERANGE;
    if(d->readonly || disk->readonly) return BDEV_EROFS;
    if(!count) return 0;
    r = disk->ops->write(disk, lba, count, buf);
    if(r == 0) { d->writes += count; if(disk != d) disk->writes += count; }
    return r;
}

int bdev_flush(struct bdev *d) {
    struct bdev *disk = bdev_disk(d);
    if(!disk || disk->gone) return BDEV_EGONE;
    if(!disk->ops || !disk->ops->flush) return 0;
    return disk->ops->flush(disk);
}

/* the MBR type byte's name (driver/parttab.c keeps the table) */
const char *bdev_part_type_name(u8 type) {
    const char *n = part_mbr_type_name(type);
    return n ? n : "?";
}

/**** lsblk ****/

#define LSBLK_WIDTH 79                  /* the VGA console is 80 columns, and a full line wraps */

struct lsblk_row {
    char name[BDEV_NAME_LEN + 2], size[8], type[48];
    const char *bus, *label, *status;
};

struct lsblk_widths {
    u_int name, size, type, label, status;
};

static const char *bus_name(u8 bus) {
    switch(bus) {
    case BDEV_BUS_RAM: return "ram";
    case BDEV_BUS_ATA: return "ata";
    case BDEV_BUS_USB: return "usb";
    }
    return "-";
}

static const char *scheme_name(u8 scheme) {
    return scheme == PART_SCHEME_GPT ? "gpt" : scheme == PART_SCHEME_MBR ? "mbr" : "";
}

/* the device itself is mounted (vfs_bdev_busy() also counts its disk and partitions) */
static bool mounted(const struct bdev *d) {
    const struct mount *m;
    for(m = vfs_mounts(); m; m = m->next) if(m->sb && m->sb->dev == d) return true;
    return false;
}

/* the first of these that applies; bdev_may_format() is the rule, this only says it */
static const char *status_of(struct bdev *d) {
    if(mounted(d)) return "mounted";
    if(d->protect == bdev_boot_drive) return "boot drive: protected";
    if(d->protect) return "protected";
    if(bdev_disk(d)->bus == BDEV_BUS_ATA) return "internal: never formatted";
    if(d->readonly) return "read-only";
    return "";
}

static void lsblk_row(struct bdev *d, struct lsblk_row *r) {
    snprintf(r->name, sizeof(r->name), "%s%s", d->parent ? "  " : "", d->name);
    part_fmt_size(d->nblocks, r->size, sizeof(r->size));
    if(d->parent) snprintf(r->type, sizeof(r->type), "part %s %s", scheme_name(d->scheme), d->type_name ? d->type_name : "?");
    else snprintf(r->type, sizeof(r->type), "disk%s%s", d->scheme ? " " : "", scheme_name(d->scheme));
    r->bus = bus_name(bdev_disk(d)->bus);
    r->label = d->parent ? d->part_label : d->model;      /* a GPT partition's name; a disk's model */
    r->status = status_of(d);
}

/* every disk, each followed by its partitions in order (a rescan appends new ones at the end of
   the list) */
static void lsblk_walk(void (*fn)(struct bdev *d, void *arg), void *arg) {
    struct bdev *d, *p, *next;
    u_int last;
    for(d = bdevs; d; d = d->next) {
        if(d->parent) continue;
        fn(d, arg);
        for(last = 0;; last = next->part_index) {
            next = NULL;
            for(p = bdevs; p; p = p->next)
                if(p->parent == d && p->part_index > last && (!next || p->part_index < next->part_index)) next = p;
            if(!next) break;
            fn(next, arg);
        }
    }
}

static void lsblk_measure(struct bdev *d, void *arg) {
    struct lsblk_widths *w = (struct lsblk_widths *)arg;
    struct lsblk_row r;
    lsblk_row(d, &r);
    if(strlen(r.name) > w->name) w->name = strlen(r.name);
    if(strlen(r.size) > w->size) w->size = strlen(r.size);
    if(strlen(r.type) > w->type) w->type = strlen(r.type);
    if(strlen(r.label) > w->label) w->label = strlen(r.label);
    if(strlen(r.status) > w->status) w->status = strlen(r.status);
}

static void lsblk_print(struct bdev *d, void *arg) {
    const struct lsblk_widths *w = (const struct lsblk_widths *)arg;
    struct lsblk_row r;
    char line[160];
    int n;
    lsblk_row(d, &r);
    n = snprintf(line, sizeof(line), "%-*s %*s %-3s %2s %-*s %-*.*s %s", w->name, r.name, w->size, r.size, r.bus,
                 d->removable ? "1" : "0", w->type, r.type, w->label, w->label, r.label, r.status);
    if(n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
    while(n > 0 && line[n - 1] == ' ') line[--n] = 0;       /* no padding after the last column */
    printf("%s\n", line);
}

/* lsblk: the columns are as wide as what they hold, and the model or partition name gives way
   so a line stays within LSBLK_WIDTH */
void bdev_print(void) {
    struct lsblk_widths w = { 4, 4, 4, 11, 0 };          /* the headings */
    u_int fixed;
    if(!bdevs) { printf("(no block devices)\n"); return; }
    lsblk_walk(lsblk_measure, &w);
    fixed = w.name + 1 + w.size + 1 + 3 + 1 + 2 + 1 + w.type + 1 + (w.status ? w.status + 1 : 0);
    if(fixed + w.label > LSBLK_WIDTH) w.label = fixed + 8 < LSBLK_WIDTH ? LSBLK_WIDTH - fixed : 8;
    printf("%-*s %*s BUS RM %-*s ", w.name, "NAME", w.size, "SIZE", w.type, "TYPE");
    if(w.status) printf("%-*.*s STATUS\n", w.label, w.label, "MODEL/LABEL");
    else printf("%.*s\n", w.label, "MODEL/LABEL");
    lsblk_walk(lsblk_print, &w);
}

/* lsblk -s: the blocks each device has moved since boot */
void bdev_print_io(void) {
    struct bdev *d;
    printf("NAME          BLOCKS        READ     WRITTEN\n");
    for(d = bdevs; d; d = d->next) printf("%-8s %11lu %11lu %11lu\n", d->name, (u_long)d->nblocks, d->reads, d->writes);
    if(!bdevs) printf("(no block devices)\n");
}
