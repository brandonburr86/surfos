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

Changes (fdisk's new, add, delete, type) are planned here too: a plan is the
list of whole sectors to write, in order, plus what the table must say when
it is read back. driver/part.c carries a plan out through the block layer;
the host test carries it out in RAM and checks it with sfdisk and sgdisk.
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
#define GPT_ENTRIES      128    /* what a new GPT gets (32 blocks of entries), and the most fdisk changes */
#define PART_ALIGN       2048   /* new partitions start on 1 MB boundaries, and are at least that big */

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
    u_int skipped;              /* entries left out (outside the disk, a loop, ...): fdisk will not change the table */
    u_int nparts;
    struct part_entry part[PART_MAX];   /* in table order; the extended partition is one of them */
};

/* where the sectors come from; note() hears about everything that was left out and why */
struct part_io {
    void *arg;
    int (*read)(void *arg, u32 lba, u32 count, void *buf);   /* 0 ok */
    void (*note)(void *arg, const char *msg);                 /* may be NULL */
    u32 (*random)(void *arg);                                 /* for disk signatures and GUIDs (plans only) */
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

/**** changes ****/

/* free space where an aligned partition of at least PART_ALIGN blocks fits, in disk order:
   (start, count) with start aligned; the number found (at most max) */
struct part_region { u32 start, count; };
u_int part_free(const struct part_table *t, struct part_region *r, u_int max);

/* fdisk's words: sizes "500M", "2G", "1T", "4096K" (binary units, at least 1M) or "all" (0: the
   largest free region); types fat32 (MBR 0c / GPT basic data), fat16 (0e / basic data), linux
   (83 / Linux filesystem), efi (ef / EFI system), or an MBR type byte in hex. 0 or -1. */
struct part_kind {
    u8 mbr;                     /* the MBR type byte */
    const u8 *gpt;              /* the GPT type GUID, NULL when the byte has none */
};
int part_parse_size(const char *s, u32 *blocks);
int part_parse_type(const char *s, struct part_kind *k);

#define PART_PLAN_WRITES 6
#define PART_PLAN_BUF    (3 * PART_SECTOR + GPT_ENTRIES * GPT_ENTRY_SIZE)   /* sector 0 or an EBR, 2 headers, entries */

struct part_write {
    u32 lba, count;
    const u8 *data;             /* count blocks; NULL: zeros */
};

struct part_plan {
    u8 *buf;                    /* PART_PLAN_BUF bytes, the caller's: the data written points into it */
    u_int nwrites;
    struct part_write w[PART_PLAN_WRITES];  /* in this order: the step that makes the change valid comes last */
    char what[100];             /* the change, in a line */
    /* what the table must say when it is read back (part_plan_check) */
    int scheme;
    u_int index;                /* the partition added, deleted or retyped; 0: a new table, no partitions */
    u8 present;
    u32 start, count;           /* added: where (count 0: not checked) */
    u8 type;                    /* MBR type, or GPT type GUID */
    u8 type_guid[16];
};

/* Each fills p (p->buf set by the caller) and returns 0, or -1 with the reason in err. A new
   table: MBR (a random disk signature, no partitions; the GPT areas, blocks 1-33 and the last 33,
   are zeroed so no stale GPT is found later) or GPT (protective MBR, both headers and 128 entries,
   random GUIDs). add uses the first free entry and the largest free region (count 0) or the
   first one count blocks fit in; MBR adds primary partitions only. delete and type take the
   partition number; an MBR logical partition can be retyped, not deleted (delete its extended
   partition). Changing a GPT rewrites both copies, the backup at the end of the disk. Tables
   with entries left out (t->skipped) are not changed. */
int part_plan_new(const struct part_io *io, u32 nblocks, int scheme, struct part_plan *p, char *err, size_t n);
int part_plan_add(const struct part_io *io, const struct part_table *t, u32 count, const struct part_kind *k,
                  struct part_plan *p, char *err, size_t n);
int part_plan_delete(const struct part_io *io, const struct part_table *t, u_int index, struct part_plan *p,
                     char *err, size_t n);
int part_plan_type(const struct part_io *io, const struct part_table *t, u_int index, const struct part_kind *k,
                   struct part_plan *p, char *err, size_t n);
/* the table read back after the plan was written says what it should: 0, or -1 and why */
int part_plan_check(const struct part_plan *p, const struct part_table *t, char *err, size_t n);

/**** driver/part.c, in the kernel ****/

struct bdev;
/* reading a disk through the block layer; notes go to the shell (fdisk) or the kernel log */
void part_io_init(struct part_io *io, struct bdev *disk, bool to_shell);
/* writes a plan made from the table before (nothing is written if the disk now reads otherwise),
   flushes, reads every block back, checks the table against the plan and rescans the disk:
   0, or -1 and what went wrong in msg */
int part_commit(struct bdev *disk, const struct part_table *before, const struct part_plan *p, char *msg, size_t n);

#endif
