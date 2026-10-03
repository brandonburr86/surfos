/*
SurfOS ATA Disk Driver
--------------------
File: ata.c     Date: 10/3/26 (roadmap D2)
--------------------
Parallel ATA in PIO mode, LBA28, on the two legacy channels (0x1F0/0x3F6 and
0x170/0x376). Transfers are polled with the device's interrupt line disabled
(nIEN), which is slow on real hardware and instant in QEMU; the channel mutex
keeps tasks from interleaving commands. ATAPI devices are reported and left
alone. Each disk becomes a block device hda..hdd, and its MBR is scanned.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/timer.h>
#include <surfos/sync.h>
#include <sys/bdev.h>
#include <asm/io.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

/* task file registers, offsets from the I/O base */
#define ATA_REG_DATA     0
#define ATA_REG_ERROR    1
#define ATA_REG_FEATURES 1
#define ATA_REG_NSECT    2
#define ATA_REG_LBA0     3
#define ATA_REG_LBA1     4
#define ATA_REG_LBA2     5
#define ATA_REG_DRIVE    6
#define ATA_REG_STATUS   7
#define ATA_REG_COMMAND  7
/* the control block: alternate status / device control */
#define ATA_CTRL_NIEN    0x02
#define ATA_CTRL_SRST    0x04

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_DRDY 0x40
#define ATA_SR_BSY  0x80

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH   0xE7
#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_IDENTIFY_PKT  0xA1

#define ATA_LBA28_MAX 0x0FFFFFFF

struct ata_channel {
    u16 io, ctrl;
    const char *name;
    mutex_t lock;
    bool present;
};

struct ata_dev {
    struct ata_channel *ch;
    int drive;                  /* 0 master, 1 slave */
    bool atapi, lba48;
    u32 sectors;                /* addressable with LBA28 */
    struct bdev bdev;
};

static struct ata_channel channels[2] = {
    { 0x1F0, 0x3F6, "primary" },
    { 0x170, 0x376, "secondary" },
};

/* ~400 ns: the status register needs that long after a drive select or command */
static void ata_delay400(struct ata_channel *ch) {
    inb(ch->ctrl); inb(ch->ctrl); inb(ch->ctrl); inb(ch->ctrl);
}

/* poll until BSY clears; returns the status, or -1 after timeout_ms */
static int ata_wait_busy(struct ata_channel *ch, u_long timeout_ms) {
    u_long spins = timeout_ms * 100;            /* 10 us steps */
    for(;;) {
        u8 st = inb(ch->ctrl);
        if(!(st & ATA_SR_BSY)) return st;
        if(!spins--) return -1;
        udelay(10);
    }
}

/* poll until the device wants to move data (DRQ) or reports an error */
static int ata_wait_drq(struct ata_channel *ch, u_long timeout_ms) {
    u_long spins = timeout_ms * 100;
    for(;;) {
        u8 st = inb(ch->ctrl);
        if(!(st & ATA_SR_BSY) && (st & (ATA_SR_DRQ | ATA_SR_ERR | ATA_SR_DF))) return st;
        if(!spins--) return -1;
        udelay(10);
    }
}

static void ata_select(struct ata_channel *ch, int drive, u8 lba_top) {
    outb(ch->io + ATA_REG_DRIVE, 0xE0 | (drive << 4) | (lba_top & 0x0F));
    ata_delay400(ch);
}

/* IDENTIFY words are little-endian but their strings are byte swapped per word */
static void ata_string(char *dst, const u16 *id, int first_word, int nwords) {
    int i, n = 0;
    for(i = 0; i < nwords; i++) {
        dst[n++] = (char)(id[first_word + i] >> 8);
        dst[n++] = (char)(id[first_word + i] & 0xFF);
    }
    dst[n] = 0;
    while(n > 0 && dst[n - 1] == ' ') dst[--n] = 0;
}

static int ata_identify(struct ata_dev *dev, u16 *id) {
    struct ata_channel *ch = dev->ch;
    u8 cmd = ATA_CMD_IDENTIFY, l1, l2;
    int st;

    outb(ch->io + ATA_REG_DRIVE, 0xA0 | (dev->drive << 4));
    ata_delay400(ch);
    outb(ch->io + ATA_REG_NSECT, 0);
    outb(ch->io + ATA_REG_LBA0, 0);
    outb(ch->io + ATA_REG_LBA1, 0);
    outb(ch->io + ATA_REG_LBA2, 0);
    outb(ch->io + ATA_REG_COMMAND, cmd);
    ata_delay400(ch);
    st = inb(ch->io + ATA_REG_STATUS);
    if(st == 0 || st == 0xFF) return 1;                 /* nothing there */
    if(ata_wait_busy(ch, 1000) < 0) return 1;
    l1 = inb(ch->io + ATA_REG_LBA1);
    l2 = inb(ch->io + ATA_REG_LBA2);
    if(l1 == 0x14 && l2 == 0xEB) {                      /* ATAPI: identify it, but it is not a disk for us */
        dev->atapi = true;
        cmd = ATA_CMD_IDENTIFY_PKT;
        outb(ch->io + ATA_REG_COMMAND, cmd);
        ata_delay400(ch);
        if(ata_wait_busy(ch, 1000) < 0) return 1;
    } else if(!(l1 == 0 && l2 == 0) && !(l1 == 0x3C && l2 == 0xC3)) {
        return 1;                                       /* not ATA, not SATA */
    }
    st = ata_wait_drq(ch, 1000);
    if(st < 0 || (st & (ATA_SR_ERR | ATA_SR_DF)) || !(st & ATA_SR_DRQ)) return 1;
    insw(ch->io + ATA_REG_DATA, id, 256);
    return 0;
}

static int ata_pio(struct ata_dev *dev, u32 lba, u32 count, void *buf, bool write) {
    struct ata_channel *ch = dev->ch;
    u8 *p = (u8 *)buf;
    int ret = 0;

    if(dev->atapi) return -3;
    if(lba + count < lba || lba + count > dev->sectors) return -1;
    mutex_lock(&ch->lock);
    while(count && !ret) {
        u32 n = count > 256 ? 256 : count, s;
        int st;

        if(ata_wait_busy(ch, 1000) < 0) { ret = -3; break; }
        ata_select(ch, dev->drive, lba >> 24);
        outb(ch->io + ATA_REG_NSECT, n & 0xFF);         /* 0 means 256 */
        outb(ch->io + ATA_REG_LBA0, lba & 0xFF);
        outb(ch->io + ATA_REG_LBA1, (lba >> 8) & 0xFF);
        outb(ch->io + ATA_REG_LBA2, (lba >> 16) & 0xFF);
        outb(ch->io + ATA_REG_COMMAND, write ? ATA_CMD_WRITE_SECTORS : ATA_CMD_READ_SECTORS);
        for(s = 0; s < n; s++) {
            st = ata_wait_drq(ch, 2000);
            if(st < 0 || (st & (ATA_SR_ERR | ATA_SR_DF)) || !(st & ATA_SR_DRQ)) {
                kprintf("ata: %s %s block %lu failed, status %02x error %02x\n", dev->bdev.name,
                        write ? "write" : "read", (u_long)(lba + s), st < 0 ? 0 : st, inb(ch->io + ATA_REG_ERROR));
                ret = -3;
                break;
            }
            if(write) outsw(ch->io + ATA_REG_DATA, p, 256);
            else insw(ch->io + ATA_REG_DATA, p, 256);
            p += BDEV_BLOCK_SIZE;
        }
        if(!ret && write) {
            outb(ch->io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
            ata_delay400(ch);
            if(ata_wait_busy(ch, 5000) < 0) ret = -3;
        }
        lba += n;
        count -= n;
    }
    mutex_unlock(&ch->lock);
    return ret;
}

static int ata_bdev_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    return ata_pio((struct ata_dev *)d->priv, lba, count, buf, false);
}

static int ata_bdev_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    return ata_pio((struct ata_dev *)d->priv, lba, count, (void *)buf, true);
}

static const struct bdev_ops ata_ops = { ata_bdev_read, ata_bdev_write };

static void ata_reset(struct ata_channel *ch) {
    outb(ch->ctrl, ATA_CTRL_SRST | ATA_CTRL_NIEN);
    udelay(10);
    outb(ch->ctrl, ATA_CTRL_NIEN);                      /* polled driver: no interrupts from the device */
    mdelay(2);
    ata_wait_busy(ch, 1000);
}

int init_ata(void) {
    int c, drive, ndisks = 0;
    u16 *id = (u16 *)kalloc(512);
    char serial[21];

    if(!id) return -1;
    kprintf("\nInitializing ATA\n");
    for(c = 0; c < 2; c++) {
        struct ata_channel *ch = &channels[c];
        mutex_init(&ch->lock, ch->name);
        if(inb(ch->io + ATA_REG_STATUS) == 0xFF) continue;   /* floating bus: no controller */
        ch->present = true;
        ata_reset(ch);
        for(drive = 0; drive < 2; drive++) {
            struct ata_dev *dev = (struct ata_dev *)kcalloc(1, sizeof(struct ata_dev));
            struct bdev *d;
            if(!dev) break;
            dev->ch = ch;
            dev->drive = drive;
            d = &dev->bdev;
            if(ata_identify(dev, id) != 0) { kfree(dev); continue; }
            snprintf(d->name, sizeof(d->name), "hd%c", 'a' + c * 2 + drive);
            ata_string(d->model, id, 27, 20);
            ata_string(serial, id, 10, 10);
            if(dev->atapi) {
                kprintf("+ATA: %s %s: %s (ATAPI, not used)\n", ch->name, drive ? "slave" : "master", d->model);
                kfree(dev);
                continue;
            }
            dev->lba48 = (id[83] & (1 << 10)) != 0;
            dev->sectors = (u32)id[60] | ((u32)id[61] << 16);
            if(dev->lba48 && (id[102] || id[103] || ((u32)id[100] | ((u32)id[101] << 16)) > ATA_LBA28_MAX))
                dev->sectors = ATA_LBA28_MAX;                /* bigger than this driver addresses */
            if(!dev->sectors || !(id[49] & (1 << 9))) {      /* no LBA: CHS-only drives are not supported */
                kprintf("+ATA: %s %s: %s has no LBA support; skipped\n", ch->name, drive ? "slave" : "master", d->model);
                kfree(dev);
                continue;
            }
            d->block_size = BDEV_BLOCK_SIZE;
            d->nblocks = dev->sectors;
            d->ops = &ata_ops;
            d->priv = dev;
            if(bdev_register(d) != 0) { kfree(dev); continue; }
            kprintf("+ATA: %s is %s %s: %s (serial %s), %lu MB, %lu blocks%s\n", d->name, ch->name,
                    drive ? "slave" : "master", d->model, serial, (u_long)(dev->sectors / 2048), (u_long)dev->sectors,
                    dev->lba48 ? ", LBA48 capable" : "");
            bdev_scan_partitions(d);
            ndisks++;
        }
    }
    kfree(id);
    kprintf("*%i disk%s\n*DONE\n", ndisks, ndisks == 1 ? "" : "s");
    return ndisks ? 0 : 1;
}
