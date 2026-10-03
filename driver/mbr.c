/*
SurfOS MBR Partition Tables
--------------------
File: mbr.c     Date: 10/3/26 (roadmap D2)
--------------------
Reads block 0 of a disk and registers the four primary partitions as block
devices named after the disk ("hda1".."hda4"). Extended partitions are listed
but not walked.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <sys/bdev.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

#define MBR_TABLE_OFFSET 446
#define MBR_SIGNATURE_OFFSET 510

static u32 le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

int bdev_scan_partitions(struct bdev *disk) {
    u8 *sector = (u8 *)kalloc(BDEV_BLOCK_SIZE);
    int i, found = 0;

    if(!sector) return -1;
    if(bdev_read(disk, 0, 1, sector) != 0) { kfree(sector); return -1; }
    if(sector[MBR_SIGNATURE_OFFSET] != 0x55 || sector[MBR_SIGNATURE_OFFSET + 1] != 0xAA) {
        kfree(sector);
        return 0;                                   /* no partition table: a raw volume */
    }
    for(i = 0; i < 4; i++) {
        const u8 *e = sector + MBR_TABLE_OFFSET + i * 16;
        u8 type = e[4];
        u32 start = le32(e + 8), count = le32(e + 12);
        struct bdev *p;

        if(!type || !count) continue;
        if(start >= disk->nblocks || start + count < start || start + count > disk->nblocks) {
            kprintf("+MBR: %s partition %i (type %02x) lies outside the disk; ignored\n", disk->name, i + 1, type);
            continue;
        }
        p = (struct bdev *)kcalloc(1, sizeof(struct bdev));
        if(!p) break;
        snprintf(p->name, sizeof(p->name), "%s%i", disk->name, i + 1);
        strlcpy(p->model, disk->model, sizeof(p->model));
        p->block_size = disk->block_size;
        p->nblocks = count;
        p->start = start;
        p->parent = disk;
        p->part_type = type;
        p->readonly = disk->readonly;
        if(bdev_register(p) != 0) { kfree(p); continue; }
        kprintf("+MBR: %s at block %lu, %lu blocks, type %02x (%s)%s\n", p->name, (u_long)start, (u_long)count,
                type, bdev_part_type_name(type), (e[0] & 0x80) ? ", active" : "");
        found++;
    }
    kfree(sector);
    return found;
}
