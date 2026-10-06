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
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

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

/* the partitions of a disk: how many were registered, or -1 when block 0 cannot be read */
int bdev_scan_partitions(struct bdev *disk) {
    struct part_table *t = (struct part_table *)kalloc(sizeof(struct part_table));
    struct part_io io;
    u_int i;
    int found = 0;

    if(!t) return -1;
    io.arg = disk;
    io.read = disk_read;
    io.note = boot_note;
    if(part_read(&io, disk->nblocks, t) != 0) { kfree(t); return -1; }
    disk->scheme = (u8)(t->damaged ? PART_SCHEME_NONE : t->scheme);
    for(i = 0; i < t->nparts; i++) {
        if(t->part[i].extended) continue;           /* the container of the logical partitions */
        if(t->part[i].index > 255) {                /* part_index is a byte */
            kprintf("+PART: %s: partition %u is numbered beyond 255; left out\n", disk->name, t->part[i].index);
            continue;
        }
        if(add_partition(disk, t, &t->part[i])) found++;
    }
    kfree(t);
    return found;
}
