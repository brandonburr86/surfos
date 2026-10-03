/*
SurfOS Block Device Layer
--------------------
File: bdev.c    Date: 10/3/26 (roadmap D2)
--------------------
The registry of block devices and the one place that checks ranges and
translates partition offsets. See include/sys/bdev.h.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <sys/bdev.h>
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
    if(!disk || !disk->ops || !disk->ops->read) return -1;
    if(!count) return 0;
    r = disk->ops->read(disk, lba, count, buf);
    if(r == 0) { d->reads += count; if(disk != d) disk->reads += count; }
    return r;
}

int bdev_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct bdev *disk = resolve(d, &lba, count);
    int r;
    if(!disk || !disk->ops || !disk->ops->write) return -1;
    if(d->readonly || disk->readonly) return -2;
    if(!count) return 0;
    r = disk->ops->write(disk, lba, count, buf);
    if(r == 0) { d->writes += count; if(disk != d) disk->writes += count; }
    return r;
}

const char *bdev_part_type_name(u8 type) {
    switch(type) {
    case 0x01: return "FAT12";
    case 0x04: return "FAT16 <32M";
    case 0x05: return "extended";
    case 0x06: return "FAT16";
    case 0x07: return "NTFS/exFAT";
    case 0x0B: return "FAT32";
    case 0x0C: return "FAT32 LBA";
    case 0x0E: return "FAT16 LBA";
    case 0x0F: return "extended LBA";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI system";
    }
    return "?";
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
        if(d->parent) printf(" part %02x %-6s", d->part_type, bdev_part_type_name(d->part_type));
        else printf(" disk%s        ", d->readonly ? " ro" : "   ");
        printf(" %s\n", d->parent ? "" : d->model);
    }
    if(!bdevs) printf("    (no block devices)\n");
}
