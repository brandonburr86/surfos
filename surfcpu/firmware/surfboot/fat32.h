/*
SurfBoot: MBR and FAT32, read only
--------------------
File: fat32.h Date: 10/15/26 (SurfCPU)
--------------------
The card layout of docs/BOOT-ABI.md section 5: an MBR partition table with a FAT32
partition (type 0x0b or 0x0c) and 512-byte sectors. exFAT, NTFS, FAT12/FAT16, GPT-only
cards and cards without a partition table are refused with a sentence saying which. Names
are 8.3 only (long-name entries are skipped). Every cluster number and chain is checked:
entries pointing to free, reserved, bad or out-of-range clusters, chains that loop (Brent's
cycle detection), chains shorter or longer than a file's size, and directories longer than
FAT's 65536 entries are errors, never hangs or out-of-bounds reads.
*/
#ifndef SURFBOOT_FAT32_H
#define SURFBOOT_FAT32_H

#include <stdint.h>

#define FAT_EOC        0xFFFFFFFFu  /* fat_next(): end of the chain */
#define FAT_ATTR_RO    0x01u
#define FAT_ATTR_HID   0x02u
#define FAT_ATTR_SYS   0x04u
#define FAT_ATTR_VOL   0x08u
#define FAT_ATTR_DIR   0x10u
#define FAT_ATTR_ARC   0x20u
#define FAT_ATTR_LFN   0x0Fu
#define FAT_MAX_DIR_ENTRIES 65536u

typedef int (*fat_read_fn)(void *ctx, uint32_t lba, void *buf);   /* one 512-byte sector */

struct mbr_part {
    uint8_t status, type;
    uint32_t lba, sectors;
};

struct fat_dirent {
    char name[11];              /* 8.3, space padded (a leading 0x05 is given back as 0xE5) */
    uint8_t attr, ntres;
    uint32_t cluster, size;
};

struct fat_vol {
    fat_read_fn read;
    void *ctx;
    uint32_t dev_sectors;       /* the card's size (0 if unknown) */
    int mounted;
    int part_index;             /* 0..3 */
    uint8_t part_type;
    uint32_t part_lba, part_sectors;
    uint32_t spc, cluster_bytes, reserved, nfats, fat_sectors, total_sectors;
    uint32_t root_cluster, active_fat;
    uint32_t fat_lba;           /* first sector of the FAT in use */
    uint32_t data_lba;          /* first sector of cluster 2 */
    uint32_t clusters;          /* data clusters: valid numbers are 2..clusters+1 */
    uint32_t max_cluster;
    uint32_t serial;
    char label[12], oem[9];
    uint32_t fat_cache_lba;
    int fat_cache_valid;
    uint8_t fat_cache[512];
    uint8_t sec[512];           /* directory sectors */
};

struct fat_chain {              /* a bounded walk with Brent's cycle detection */
    uint32_t cur, saved, power, lam, steps, limit;
};

struct fat_file {
    struct fat_dirent d;
    uint32_t pos;               /* bytes delivered so far */
    uint32_t cluster, sector;   /* current cluster, next sector in it */
    uint32_t nclusters;         /* clusters the size needs */
    uint32_t fragments;         /* runs of consecutive clusters seen so far */
    struct fat_chain walk;
};

int fat_read_mbr(fat_read_fn read, void *ctx, uint8_t buf[512], struct mbr_part p[4]);
int fat_mount(struct fat_vol *v, fat_read_fn read, void *ctx, uint32_t dev_sectors);
int fat_next(struct fat_vol *v, uint32_t cluster, uint32_t *next);
uint32_t fat_cluster_lba(const struct fat_vol *v, uint32_t cluster);

void fat_chain_start(struct fat_chain *w, uint32_t first, uint32_t limit);
int fat_chain_step(struct fat_vol *v, struct fat_chain *w, uint32_t *next);

/* fn returns 0 to go on, 1 to stop; fat_dir_scan returns SB_OK or an error. */
typedef int (*fat_dir_fn)(void *ctx, const struct fat_dirent *d);
int fat_dir_scan(struct fat_vol *v, uint32_t dir_cluster, fat_dir_fn fn, void *ctx);
int fat_lookup(struct fat_vol *v, const char *path, struct fat_dirent *out);

int fat_open(struct fat_vol *v, const struct fat_dirent *d, struct fat_file *f);
/* The next sector of the file into buf; *len = bytes of the file in it (0 at the end). */
int fat_read_sector(struct fat_vol *v, struct fat_file *f, uint8_t *buf, uint32_t *len);

int fat_to_83(const char *s, unsigned n, char out[11]);
void fat_83_to_str(const char name[11], uint8_t ntres, char out[13]);
const char *fat_part_type_name(uint8_t type);

#endif /* SURFBOOT_FAT32_H */
