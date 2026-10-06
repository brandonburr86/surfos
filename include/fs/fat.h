/*
SurfOS FAT File System
----------------------
File: fat.h     Date: 10/5/26 (USB storage)
----------------------
What the FAT driver (fs/fat.c) and its formatter (fs/fat_mkfs.c) offer
besides the VFS: describing a device's FAT volume, and making a new one.
fat_mkfs() obeys bdev_may_format() itself, whoever calls it; the shell's
mkfs asks the user first (shell/fscmd.c).
*/

#ifndef _FS_FAT_H
#define _FS_FAT_H

#include <surfos/types.h>

struct bdev;

/* where a new file system's parts go, in sectors of 512 bytes from the volume's start */
struct fat_layout {
    int bits;                   /* 16 or 32 */
    u32 total;                  /* sectors in the volume */
    u32 hidden;                 /* sectors before it on its disk (a partition's start) */
    u32 spc;                    /* sectors per cluster */
    u32 rsv;                    /* reserved sectors: boot sector, FSInfo and the backups */
    u32 spf;                    /* sectors per FAT; there are two */
    u32 root_entries;           /* FAT16's fixed root directory (512); 0 for FAT32 */
    u32 data;                   /* the first sector of the data region (cluster 2) */
    u32 clusters;
};

/* the layout mkfs would write (fat_bits: 0 = FAT16 below 512 MB, else FAT32; or 16, 32).
   0, or -EINVAL with the reason in msg (too small, too large for the type). Writes nothing. */
int fat_mkfs_plan(struct bdev *d, int fat_bits, struct fat_layout *l, char *msg, size_t n);

/* a new, empty FAT file system on d with this label (NULL or "": NO NAME). 0 and a summary
   in msg, or -errno and the reason: -EACCES when bdev_may_format() refuses, -EINVAL for a bad
   label or size, -EIO, -ENOMEM. */
int fat_mkfs(struct bdev *d, const char *label, int fat_bits, char *msg, size_t n);

/* what the boot sector of d says ("FAT32, label TESTVOL"); -1 when it holds no FAT volume */
int fat_describe(struct bdev *d, char *buf, size_t n);

#endif
