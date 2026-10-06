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

10/5/26 (USB storage): a disk says which bus it is on and whether its medium
comes out, and may carry a reason it is protected. bdev_may_format() is the one
safety rule fdisk and mkfs obey: only USB disks and ramdisks, never an internal
disk, the SurfOS boot drive, or anything mounted. An unplugged disk is
unregistered: it and its partitions are marked gone (every I/O fails) and taken
off the list, but never freed, so no mount, cache block or command is left
holding a dangling pointer. Partitions of a disk whose name ends in a digit
get a 'p' (usb0p1). Partition tables are MBR (with logical partitions) or GPT;
the disk that holds the SurfOS boot volume (label SURFOSUSB) is protected.
*/

#ifndef _SYS_BDEV_H
#define _SYS_BDEV_H

#include <surfos/types.h>

#define BDEV_NAME_LEN   16
#define BDEV_BLOCK_SIZE 512

struct bdev;

/* the bus a disk is on (a partition has its disk's); 0, what a driver that says
   nothing gets, is never formatted */
#define BDEV_BUS_OTHER 0
#define BDEV_BUS_RAM   1        /* a ramdisk made from a boot module: RAM only */
#define BDEV_BUS_ATA   2        /* the legacy IDE ports (QEMU's test disk; skipped on the laptop) */
#define BDEV_BUS_USB   3        /* USB mass storage */

/* I/O results */
#define BDEV_ERANGE -1          /* outside the device */
#define BDEV_EROFS  -2          /* read-only */
#define BDEV_EIO    -3          /* the driver failed */
#define BDEV_EGONE  -4          /* unplugged */

struct bdev_ops {
    int (*read)(struct bdev *d, u32 lba, u32 count, void *buf);         /* 0 ok, < 0 error */
    int (*write)(struct bdev *d, u32 lba, u32 count, const void *buf);
    int (*flush)(struct bdev *d);   /* the device's own write cache to its medium; NULL when it has none */
};

struct bdev {
    char name[BDEV_NAME_LEN];   /* hda, hda1, rd0, usb0, usb0p1 */
    char model[41];             /* what the hardware (or the module) calls itself */
    u32 block_size;             /* always 512 for now */
    u32 nblocks;                /* capacity */
    u32 start;                  /* a partition: first block on the parent */
    struct bdev *parent;        /* a partition: the disk, else NULL */
    u8 part_type;               /* a partition: the MBR type byte (GPT: its MBR equivalent, or 0) */
    u8 part_index;              /* a partition: its number, from 1 (MBR logical partitions from 5) */
    u8 scheme;                  /* a disk: the partition table found on it; a partition: the table it is in
                                   (PART_SCHEME_NONE, _MBR, _GPT in sys/part.h) */
    const char *type_name;      /* a partition: its type in a few words ("FAT32 LBA", "EFI system", "basic data") */
    char part_label[37];        /* a GPT partition: its name ("SURFOSUSB"), in ASCII; "" for MBR */
    u8 bus;                     /* BDEV_BUS_*; a partition copies its disk's */
    bool removable;             /* the medium can be taken out (a USB stick, a card reader) */
    bool readonly;
    const char *protect;        /* set: why fdisk and mkfs must leave it alone ("the SurfOS boot drive") */
    volatile bool gone;         /* unplugged: every read and write fails with BDEV_EGONE */
    u_int refs;                 /* mounts using it (bdev_get/bdev_put) */
    const struct bdev_ops *ops; /* NULL for a partition (the parent's are used) */
    void *priv;                 /* driver data; not touched once gone */
    u_long reads, writes;       /* blocks moved, for lsblk */
    struct bdev *next;
};

/* registry */
int bdev_register(struct bdev *d);          /* 0, or -1 when the name is taken */
void bdev_unregister(struct bdev *d);       /* unplugged: d and its partitions gone and off the list (never freed) */
struct bdev *bdev_find(const char *name);
struct bdev *bdev_first(void);              /* walk with ->next */
u_int bdev_count(void);
void bdev_get(struct bdev *d);              /* a mount holds the device */
void bdev_put(struct bdev *d);
struct bdev *bdev_disk(struct bdev *d);     /* the whole disk a partition is on (d itself for a disk) */
void bdev_part_name(const struct bdev *disk, u_int n, char *out, size_t size);  /* hda + 1 = hda1, usb0 + 1 = usb0p1 */

/* I/O: 0 ok, or BDEV_E* (a driver may return other negative values) */
int bdev_read(struct bdev *d, u32 lba, u32 count, void *buf);
int bdev_write(struct bdev *d, u32 lba, u32 count, const void *buf);
int bdev_flush(struct bdev *d);             /* the disk's write cache to its medium (USB: SYNCHRONIZE CACHE) */

/* The safety rule for everything that rewrites a device as a whole (fdisk, mkfs):
   0 when d may be repartitioned or formatted, else -1 and the reason in why. Only a
   USB disk or a ramdisk qualifies: never an internal disk (ATA today, any SATA or NVMe
   driver later), never one marked protect, never a read-only, unplugged or mounted one
   (a mount of the disk or of any of its partitions counts). */
int bdev_may_format(struct bdev *d, char *why, size_t size);

void bdev_print(void);                      /* lsblk: size, bus, removable, table and type, model or name, status */
void bdev_print_io(void);                   /* lsblk -s: blocks read and written */
const char *bdev_part_type_name(u8 type);

/* part.c: register the partitions of a disk (MBR with logical partitions from 5, or GPT) as
   <name>1, <name>2, ... (usb0p1 ...); the number registered, or -1 when block 0 cannot be read.
   A disk with a FAT volume labelled SURFOSUSB gets protect = bdev_boot_drive, and so do its
   partitions. bdev_rescan() reads the table again after it was rewritten: the old partitions are
   unregistered, the new ones registered; -EBUSY while the disk or a partition is mounted. */
int bdev_scan_partitions(struct bdev *disk);
int bdev_rescan(struct bdev *disk);
extern const char bdev_boot_drive[];        /* "the SurfOS boot drive" */

/* drivers (driver table entries) */
int init_ramdisk(void);
int init_ata(void);

#endif
