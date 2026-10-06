/*
SurfOS Partitions
--------------------
File: part.c    Date: 10/3/26 (roadmap D2; was mbr.c)
--------------------
Reads block 0 of a disk and registers its partitions as block devices named
after the disk ("hda1", "usb0p1").

10/5/26 (USB storage): the table is read by driver/parttab.c, so GPT disks and
the logical partitions of an MBR extended partition are found too. Logical
partitions are numbered from 5 as Linux numbers them; the extended partition
that holds them is listed by fdisk but not registered (writing to it would
destroy them).

The SurfOS boot drive is recognised here: a disk whose partition (or, with no
table, the disk itself) holds a FAT volume labelled SURFOSUSB, which is what
tools/mkusb.py writes, gets protect = bdev_boot_drive, and so do its
partitions; bdev_may_format() then refuses it to fdisk and mkfs. bdev_rescan()
reads a table again after fdisk has written it.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

const char bdev_boot_drive[] = "the SurfOS boot drive";

static int disk_read(void *arg, u32 lba, u32 count, void *buf) {
    return bdev_read((struct bdev *)arg, lba, count, buf);
}

static void boot_note(void *arg, const char *msg) {
    kprintf("+PART: %s: %s\n", ((struct bdev *)arg)->name, msg);
}

static struct bdev *add_partition(struct bdev *disk, const struct part_table *t, const struct part_entry *e) {
    struct bdev *p = (struct bdev *)kcalloc(1, sizeof(struct bdev));
    char size[8];
    if(!p) return NULL;
    bdev_part_name(disk, e->index, p->name, sizeof(p->name));
    strlcpy(p->model, disk->model, sizeof(p->model));
    p->block_size = disk->block_size;
    p->nblocks = e->count;
    p->start = e->start;
    p->parent = disk;
    p->part_type = e->type;
    p->part_index = (u8)e->index;
    p->scheme = (u8)t->scheme;
    p->type_name = part_type_name(t->scheme, e, true);
    strlcpy(p->part_label, e->name, sizeof(p->part_label));
    p->bus = disk->bus;
    p->removable = disk->removable;
    p->protect = disk->protect;
    p->readonly = disk->readonly;
    if(bdev_register(p) != 0) {
        kprintf("+PART: %s: %s is taken; partition %u left out\n", disk->name, p->name, e->index);
        kfree(p);
        return NULL;
    }
    part_fmt_size(e->count, size, sizeof(size));
    if(t->scheme == PART_SCHEME_GPT)
        kprintf("+PART: %s: GPT partition %u, blocks %lu-%lu (%s), %s \"%s\"\n", p->name, e->index, (u_long)e->start,
                (u_long)(e->start + e->count - 1), size, part_type_name(t->scheme, e, false), e->name);
    else
        kprintf("+PART: %s: MBR %spartition %u, blocks %lu-%lu (%s), type %02x %s%s\n", p->name, e->logical ? "logical " : "",
                e->index, (u_long)e->start, (u_long)(e->start + e->count - 1), size, e->type,
                part_type_name(t->scheme, e, false), e->active ? ", active" : "");
    return p;
}

/* block 0 of d is a FAT boot sector with the boot drive's volume label */
static bool boot_volume(struct bdev *d, u8 *sec) {
    char label[12];
    return bdev_read(d, 0, 1, sec) == 0 && part_fat_label(sec, label) && !strcmp(label, PART_BOOT_LABEL);
}

static void mark_boot_drive(struct bdev *disk, const char *where) {
    struct bdev *p;
    disk->protect = bdev_boot_drive;
    for(p = bdev_first(); p; p = p->next) if(p->parent == disk) p->protect = bdev_boot_drive;
    kprintf("+PART: %s is the SurfOS boot drive (%s holds the volume SURFOSUSB): protected from fdisk and mkfs\n",
            disk->name, where);
}

/* the partitions of a disk: how many were registered, or -1 when block 0 cannot be read */
int bdev_scan_partitions(struct bdev *disk) {
    struct part_table *t = (struct part_table *)kalloc(sizeof(struct part_table));
    u8 *sec = (u8 *)kalloc(PART_SECTOR);
    struct part_io io;
    struct bdev *p;
    u_int i;
    int found = 0;

    if(!t || !sec) { kfree(t); kfree(sec); return -1; }
    io.arg = disk;
    io.read = disk_read;
    io.note = boot_note;
    io.random = NULL;                               /* reading needs none */
    if(part_read(&io, disk->nblocks, t) != 0) { kfree(t); kfree(sec); return -1; }
    disk->scheme = (u8)(t->damaged ? PART_SCHEME_NONE : t->scheme);
    for(i = 0; i < t->nparts; i++) {
        if(t->part[i].extended) continue;           /* the container of the logical partitions */
        if(t->part[i].index > 255) {                /* part_index is a byte */
            kprintf("+PART: %s: partition %u is numbered beyond 255; left out\n", disk->name, t->part[i].index);
            continue;
        }
        if(add_partition(disk, t, &t->part[i])) found++;
    }
    if(found) {
        for(p = bdev_first(); p; p = p->next) {
            if(p->parent == disk && boot_volume(p, sec)) { mark_boot_drive(disk, p->name); break; }
        }
    } else if(boot_volume(disk, sec)) {
        mark_boot_drive(disk, "the whole disk");
    }
    kfree(sec);
    kfree(t);
    return found;
}

/* After fdisk: the old partitions are unregistered (marked gone and taken off the list, never
   freed: a command may still hold one), the disk's cached blocks dropped, and the table read
   again, the boot drive check included. Never while the disk or a partition is mounted. */
int bdev_rescan(struct bdev *disk) {
    struct bdev *p;
    u_int n, most;
    if(!disk || disk->parent) return -EINVAL;
    if(disk->gone) return -ENODEV;
    if(vfs_bdev_busy(disk)) return -EBUSY;
    most = bdev_count();
    for(n = 0; n < most; n++) {
        for(p = bdev_first(); p && p->parent != disk; p = p->next);
        if(!p) break;
        bdev_unregister(p);
    }
    binvalidate(disk);
    if(disk->protect == bdev_boot_drive) disk->protect = NULL;     /* found again below if it still is */
    disk->scheme = PART_SCHEME_NONE;
    return bdev_scan_partitions(disk);
}
