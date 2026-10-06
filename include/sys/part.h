/*
SurfOS Partition Tables
----------------------
File: part.h    Date: 10/5/26 (USB storage)
----------------------
MBR partition tables (the four primary entries and the logical partitions in
an extended one) and GPT (both headers, their CRC32s, the backup when the
primary is damaged), read from 512-byte sectors through a callback. This is
pure code (driver/parttab.c): driver/part.c registers what it finds as block
devices, and tests/host/test_part.c runs it on the host.
*/

#ifndef _SYS_PART_H
#define _SYS_PART_H

#include <surfos/types.h>

#define PART_SECTOR      512
#define PART_MAX         128    /* partitions kept per disk: GPT's usual 128 entries, MBR 4 + 124 logical */
#define PART_NAME_LEN    37     /* a GPT partition name: 36 UTF-16 units, kept as ASCII */
#define PART_BOOT_LABEL  "SURFOSUSB  "  /* the FAT volume label of the SurfOS boot drive (tools/mkusb.py) */

#define PART_SCHEME_NONE 0      /* no partition table: a whole-disk volume, or blank */
#define PART_SCHEME_MBR  1
#define PART_SCHEME_GPT  2

#define GPT_ENTRY_SIZE   128    /* the only entry size read (and the one every tool writes) */
#define GPT_MAX_ENTRIES  1024   /* the most entries a header may announce */

struct part_entry {
    u_int index;                /* MBR: 1-4 primary, 5... logical, as Linux numbers them; GPT: the entry number */
    u32 start, count;           /* blocks */
    u8 type;                    /* MBR type byte; GPT: the MBR equivalent of the type GUID, or 0 */
    u8 active;                  /* MBR: the boot flag is set */
    u8 logical;                 /* MBR: a logical partition */
    u8 extended;                /* MBR: the extended partition itself (listed, never registered) */
    u8 slot;                    /* MBR: which of the four entries in its sector */
    u32 table_lba;              /* MBR: the sector holding the entry (0, or the logical partition's EBR) */
    u8 type_guid[16], guid[16]; /* GPT: the type and the partition's own GUID, as on disk */
    u64 attrs;                  /* GPT attribute bits */
    char name[PART_NAME_LEN];   /* GPT name; '?' stands for anything that is not printable ASCII */
};

struct part_table {
    int scheme;                 /* PART_SCHEME_* */
    u32 nblocks;                /* the disk's */
    u32 disk_sig;               /* MBR: the disk signature at offset 440 */
    u8 disk_guid[16];           /* GPT, from the header that was used: */
    u32 first_usable, last_usable;
    u32 entries_lba, nentries;
    u8 backup_used;             /* the primary header or its entries are damaged: the backup was used */
    u8 backup_bad;              /* the primary is fine; the backup is missing, damaged or different */
    u8 damaged;                 /* a protective MBR, but neither GPT header is valid: no partitions */
    u8 full;                    /* more than PART_MAX partitions (the rest were left out) */
    u_int nparts;
    struct part_entry part[PART_MAX];   /* in table order; the extended partition is one of them */
};

/* where the sectors come from; note() hears about everything that was left out and why */
struct part_io {
    void *arg;
    int (*read)(void *arg, u32 lba, u32 count, void *buf);   /* 0 ok */
    void (*note)(void *arg, const char *msg);                 /* may be NULL */
};

/* The partition table of a disk of nblocks blocks: 0 (t->scheme says what was found), or -1
   when block 0 cannot be read. GPT needs a protective MBR entry (type 0xEE starting at
   block 1); without one a stale GPT is ignored. Partitions that lie outside the disk, outside
   the usable area or beyond block 2^32 - 1 are left out with a note. */
int part_read(const struct part_io *io, u32 nblocks, struct part_table *t);
const struct part_entry *part_find(const struct part_table *t, u_int index);

/* names */
const char *part_mbr_type_name(u8 type);                    /* "FAT32 LBA"; NULL when unknown */
const char *part_gpt_type_name(const u8 *guid, bool brief); /* "Microsoft basic data" or brief "basic data"; NULL */
u8 part_gpt_mbr_type(const u8 *guid);                       /* EFI system 0xEF, basic data 0x07, ...; 0: none */
const char *part_type_name(int scheme, const struct part_entry *e, bool brief);   /* never NULL ("unknown") */
void part_guid_str(const u8 *guid, char *out);              /* 37 bytes: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
void part_fmt_size(u32 blocks, char *buf, size_t size);     /* "488K", "16M", "1.5G", "29G" */

/* the CRC32 of GPT (and zlib and Ethernet); start with 0, chain by passing the last result */
u32 part_crc32(u32 crc, const void *buf, size_t len);

/* a FAT boot sector: true, and its 11-character volume label (FAT32: offset 71, FAT12/16: 43) */
bool part_fat_label(const u8 *sector, char *label);         /* label: 12 bytes */

#endif
