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

static void print_blocks(u32 nblocks) {
    /* 512-byte blocks: 2048 per MB */
    if(nblocks >= 2048 * 1024) printf("%5luG", (u_long)(nblocks / (2048 * 1024)));
    else if(nblocks >= 2048) printf("%5luM", (u_long)(nblocks / 2048));
    else printf("%5luK", (u_long)(nblocks / 2));
}

void bdev_print(void) {
    struct bdev *d;
    printf("    NAME   SIZE   BLOCKS      RD/WR BLKS  TYPE          MODEL\n");
    for(d = bdevs; d; d = d->next) {
        printf("    %-6s", d->name);
        print_blocks(d->nblocks);
        printf(" %-10lu %6lu/%-6lu", (u_long)d->nblocks, d->reads, d->writes);
        if(d->parent) printf(" part %s %-10s", d->scheme == PART_SCHEME_GPT ? "gpt" : "mbr", d->type_name ? d->type_name : "?");
        else printf(" disk%s        ", d->readonly ? " ro" : "   ");
        printf(" %s\n", d->parent ? "" : d->model);
    }
    if(!bdevs) printf("    (no block devices)\n");
}
