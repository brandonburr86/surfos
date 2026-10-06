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

fdisk's changes are planned by parttab.c and carried out by part_commit():
through the block layer, flushed, read back and compared, the table read again
and checked against the plan, and the disk rescanned.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/cpu.h>
#include <surfos/sync.h>
#include <surfos/timer.h>
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

static void shell_note(void *arg, const char *msg) {
    printf("    note: %s\n", msg);
}

/* for disk signatures and GUIDs: the time stamp counter (or the tick count) stirred into a counter,
   so two calls in a row never agree */
static u32 part_random(void *arg) {
    static u32 count;
    u32 x = (u32)getticks() * 2654435761u + (count += 0x9E3779B9);
    if(cpu.tsc) {
        u64 tsc = rdtsc();
        x ^= (u32)tsc ^ (u32)(tsc >> 32);
    }
    x ^= x >> 16; x *= 0x85EBCA6B;                  /* the MurmurHash3 finalizer */
    x ^= x >> 13; x *= 0xC2B2AE35;
    x ^= x >> 16;
    return x;
}

void part_io_init(struct part_io *io, struct bdev *disk, bool to_shell) {
    io->arg = disk;
    io->read = disk_read;
    io->note = to_shell ? shell_note : boot_note;
    io->random = part_random;
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
    part_io_init(&io, disk, false);
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

/**** fdisk's writes ****/

#define CHUNK 32                                    /* blocks per write of zeros and per read back */

static mutex_t write_lock = MUTEX_INIT("fdisk");

static int write_extent(struct bdev *disk, const struct part_write *w, const u8 *zero) {
    u32 done, c;
    if(w->data) return bdev_write(disk, w->lba, w->count, w->data);
    for(done = 0; done < w->count; done += c) {
        c = w->count - done < CHUNK ? w->count - done : CHUNK;
        if(bdev_write(disk, w->lba + done, c, zero) != 0) return -1;
    }
    return 0;
}

static int verify_extent(struct bdev *disk, const struct part_write *w, u8 *buf, const u8 *zero) {
    u32 done, c;
    for(done = 0; done < w->count; done += c) {
        c = w->count - done < CHUNK ? w->count - done : CHUNK;
        if(bdev_read(disk, w->lba + done, c, buf) != 0) return -1;
        if(memcmp(buf, w->data ? w->data + done * PART_SECTOR : zero, c * PART_SECTOR)) return -1;
    }
    return 0;
}

/* Carries a plan out on disk. before is the table the plan was made from: when the disk no longer
   reads the same (another fdisk, or a different stick under the same name), nothing is written.
   The safety rule is asked again, as the user took a while to answer. 0, or -1 with what went
   wrong in msg; either way the disk is rescanned once something was written. */
int part_commit(struct bdev *disk, const struct part_table *before, const struct part_plan *p, char *msg, size_t n) {
    struct part_table *t = (struct part_table *)kalloc(sizeof(struct part_table));
    u8 *buf = (u8 *)kalloc(CHUNK * PART_SECTOR), *zero = (u8 *)kcalloc(1, CHUNK * PART_SECTOR);
    struct part_io io;
    char why[100];
    u_int i;
    int r = -1, found;

    msg[0] = 0;
    if(!t || !buf || !zero) { snprintf(msg, n, "out of memory"); goto out; }
    if(!mutex_trylock(&write_lock)) { snprintf(msg, n, "another fdisk is writing a partition table; try again"); goto out; }
    if(bdev_may_format(disk, why, sizeof(why)) != 0) { snprintf(msg, n, "%s will not be changed: %s", disk->name, why); goto unlock; }
    part_io_init(&io, disk, false);
    io.note = NULL;
    if(part_read(&io, disk->nblocks, t) != 0 || memcmp(t, before, sizeof(struct part_table))) {
        snprintf(msg, n, "the partition table on %s changed since it was read; nothing was written", disk->name);
        goto unlock;
    }
    for(i = 0; i < p->nwrites; i++) {
        if(write_extent(disk, &p->w[i], zero) != 0) {
            snprintf(msg, n, "writing blocks %lu-%lu failed", (u_long)p->w[i].lba, (u_long)(p->w[i].lba + p->w[i].count - 1));
            goto rescan;
        }
    }
    if(bdev_flush(disk) != 0) { snprintf(msg, n, "%s did not write its cache to the medium", disk->name); goto rescan; }
    for(i = 0; i < p->nwrites; i++) {
        if(verify_extent(disk, &p->w[i], buf, zero) != 0) {
            snprintf(msg, n, "blocks %lu-%lu do not read back as written", (u_long)p->w[i].lba, (u_long)(p->w[i].lba + p->w[i].count - 1));
            goto rescan;
        }
    }
    if(part_read(&io, disk->nblocks, t) != 0) { snprintf(msg, n, "block 0 of %s cannot be read back", disk->name); goto rescan; }
    if(part_plan_check(p, t, why, sizeof(why)) != 0) {
        snprintf(msg, n, "the table read back is not what was planned: %s", why);
        goto rescan;
    }
    r = 0;
rescan:
    found = bdev_rescan(disk);                      /* the cached blocks of the disk go with it */
    if(found < 0 && !msg[0]) snprintf(msg, n, "%s could not be read again (%d)", disk->name, found);
    if(found < 0) r = -1;
unlock:
    mutex_unlock(&write_lock);
out:
    kfree(t);
    kfree(buf);
    kfree(zero);
    return r;
}
