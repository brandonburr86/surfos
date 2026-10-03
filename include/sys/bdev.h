/*
SurfOS Block Devices
----------------------
File: bdev.h    Date: 10/3/26 (roadmap D2)
----------------------
A block device is a named array of 512-byte blocks with read and write
operations. Partitions are block devices too: they translate block numbers
and hand the request to their parent disk. Drivers fill in a struct bdev and
register it; the shell and (later) the file systems only use bdev_read() and
bdev_write().
*/

#ifndef _SYS_BDEV_H
#define _SYS_BDEV_H

#include <surfos/types.h>

#define BDEV_NAME_LEN   16
#define BDEV_BLOCK_SIZE 512

struct bdev;

struct bdev_ops {
    int (*read)(struct bdev *d, u32 lba, u32 count, void *buf);         /* 0 ok, < 0 error */
    int (*write)(struct bdev *d, u32 lba, u32 count, const void *buf);
};

struct bdev {
    char name[BDEV_NAME_LEN];   /* hda, hda1, rd0 */
    char model[41];             /* what the hardware (or the module) calls itself */
    u32 block_size;             /* always 512 for now */
    u32 nblocks;                /* capacity */
    u32 start;                  /* a partition: first block on the parent */
    struct bdev *parent;        /* a partition: the disk, else NULL */
    u8 part_type;               /* a partition: the MBR type byte */
    bool readonly;
    const struct bdev_ops *ops; /* NULL for a partition (the parent's are used) */
    void *priv;                 /* driver data */
    u_long reads, writes;       /* blocks moved, for lsblk */
    struct bdev *next;
};

/* registry */
int bdev_register(struct bdev *d);          /* 0, or -1 when the name is taken */
struct bdev *bdev_find(const char *name);
struct bdev *bdev_first(void);              /* walk with ->next */
u_int bdev_count(void);

/* I/O: 0 ok, -1 out of range, -2 read-only, < -2 driver error */
int bdev_read(struct bdev *d, u32 lba, u32 count, void *buf);
int bdev_write(struct bdev *d, u32 lba, u32 count, const void *buf);

void bdev_print(void);                      /* lsblk */
const char *bdev_part_type_name(u8 type);

/* mbr.c: register the MBR partitions of a disk as <name>1..<name>4 */
int bdev_scan_partitions(struct bdev *disk);

/* drivers (driver table entries) */
int init_ramdisk(void);
int init_ata(void);

#endif
