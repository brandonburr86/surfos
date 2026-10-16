/*
SurfOS board: SurfCPU on the Arty A7 -- the microSD card as a block device
--------------------
File: sd.c      Date: 10/15/26 (SurfCPU)
--------------------
The card in the Pmod MicroSD adapter, through surf_spi (MEMORY-MAP.md: SPI mode 0, one
byte per exchange, a programmable divider, chip select, card detect), driven by sdcard.c,
and registered with the block layer as sd0 (its partitions sd0p1, ...), so the existing
partition code, block cache, VFS and fs/fat.c work on it unchanged: init_fs() mounts its
FAT32 partition on /sd0p1.

SurfBoot read the kernel from this card and left it initialized (BOOT-ABI.md section 4);
the kernel takes it over and initializes it again, which a card accepts at any time.

Writing: the card is the boot drive and holds the owner's files. Until the write path
has been tried on the real board it is read-only, as the brief asks ("read-only first,
read-write once tested"): the word sd=rw on the kernel command line (/BOOT/CMDLINE.TXT)
makes it writable from boot, and `sdmode rw` while it is not mounted. It is never
partitioned or formatted from SurfOS (bdev_may_format() refuses its bus), only its files
are written; `eject sd0` writes everything out and unmounts it before the card is taken
out or the board switched off.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/sync.h>
#include <surfos/multiboot.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <asm/io.h>
#include <surfcpu/soc.h>
#include <riscv/board.h>
#include "arty.h"
#include "sdcard.h"
#include "sdproto.h"
#include "../../shell/shell.h"
#include <blibc_common.h>

#define SREG(off) ((u_long)SURF_SPI_BASE + (off))

static struct sdcard card;
static mutex_t sd_lock = MUTEX_INIT("sd");
static struct bdev *disk;
static int init_status = 1;
static u32 slow_div;

/**** surf_spi as the card's host ****/

static u8 spi_xfer(void *ctx, u8 out) {
    (void)ctx;
    mmio_write32(SREG(SURF_SPI_DATA), out);
    return (u8)(mmio_read32(SREG(SURF_SPI_DATA)) & SURF_SPI_DATA_DATA_MASK);   /* waits for the exchange */
}

static void spi_select(void *ctx, bool on) {
    (void)ctx;
    mmio_write32(SREG(SURF_SPI_CTRL), on ? SURF_SPI_CTRL_CS_MASK : 0);
}

/* SCLK = sys_hz / (2 (DIV + 1)): at most 400 kHz to identify the card, then the fastest (12.5 MHz at 25 MHz) */
static void spi_slow(void *ctx, bool slow) {
    (void)ctx;
    mmio_write32(SREG(SURF_SPI_DIV), slow ? slow_div : 0);
}

static bool spi_present(void *ctx) {
    (void)ctx;
    return (mmio_read32(SREG(SURF_SPI_STATUS)) & SURF_SPI_STATUS_CARD_MASK) != 0;
}

static u32 spi_ms(void *ctx) {
    (void)ctx;
    return (u32)(board_mtime() / (board_timebase_hz() / 1000));
}

static const struct sd_host host = { NULL, spi_xfer, spi_select, spi_slow, spi_present, spi_ms };

/**** the block device ****/

static int sdb_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    int r;
    (void)d;
    mutex_lock(&sd_lock);
    r = sd_read(&card, lba, count, (u8 *)buf);
    mutex_unlock(&sd_lock);
    return r == SD_OK ? 0 : (r == SD_ERANGE ? BDEV_ERANGE : BDEV_EIO);
}

static int sdb_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    int r;
    (void)d;
    mutex_lock(&sd_lock);
    r = sd_write(&card, lba, count, (const u8 *)buf);
    mutex_unlock(&sd_lock);
    return r == SD_OK ? 0 : (r == SD_ERANGE ? BDEV_ERANGE : BDEV_EIO);
}

/* every write already waited for the card's busy time and checked CMD13: SPI mode has no cache to flush */
static int sdb_flush(struct bdev *d) {
    (void)d;
    return 0;
}

static const struct bdev_ops sd_ops = { sdb_read, sdb_write, sdb_flush };

static void size_text(u64 blocks, char *buf, size_t size) {
    u64 mb = blocks / 2048;
    if(mb >= 10240) snprintf(buf, size, "%lu GB", (u_long)(mb / 1024));
    else snprintf(buf, size, "%lu MB", (u_long)mb);
}

int arty_sd_init(void) {
    char name[96], size[16];                /* the CID in words is up to about 70 characters */
    int r;
    if(!arty_has(SURF_SYSINFO_FEATURES_SPI_MASK)) return 1;
    slow_div = (arty_sys_hz() + 2 * 400000 - 1) / (2 * 400000) - 1;
    if(slow_div > SURF_SPI_DIV_DIV_MASK) slow_div = SURF_SPI_DIV_DIV_MASK;
    mmio_write32(SREG(SURF_SPI_STATUS), SURF_SPI_STATUS_DONE_MASK | SURF_SPI_STATUS_OVR_MASK);
    mutex_lock(&sd_lock);
    r = sd_init(&card, &host);
    mutex_unlock(&sd_lock);
    init_status = r;
    if(r) {
        kprintf("+SD: %s (%s)\n", sd_strerror(r), card.why ? card.why : "?");
        return r == SD_ENOCARD ? 1 : -1;
    }
    disk = (struct bdev *)kcalloc(1, sizeof(struct bdev));
    if(!disk) return -1;
    sd_cid_describe(card.cid, name, sizeof(name));
    strlcpy(disk->name, "sd0", sizeof(disk->name));
    strlcpy(disk->model, name, sizeof(disk->model));
    disk->block_size = BDEV_BLOCK_SIZE;
    disk->nblocks = card.blocks > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)card.blocks;
    disk->bus = BDEV_BUS_OTHER;             /* never partitioned or formatted from SurfOS */
    disk->removable = true;
    disk->readonly = !bootinfo_has_arg(&bootinfo, "sd=rw");
    disk->ops = &sd_ops;
    if(bdev_register(disk) != 0) { kfree(disk); disk = NULL; return -1; }
    size_text(card.blocks, size, sizeof(size));
    kprintf("+SD: sd0: %s %s, %s, %s; %s (sd=rw on the command line writes)\n",
            card.block_addr ? (card.blocks > 64ull * 1024 * 2048 ? "SDXC" : "SDHC") : "SDSC", size, name,
            card.crc_on ? "CRC checked" : "no CRC", disk->readonly ? "read-only" : "read-write");
    bdev_scan_partitions(disk);
    return 0;
}

const char *arty_sd_describe(char *buf, size_t size) {
    char sz[16];
    if(!disk) {
        snprintf(buf, size, "%s", init_status == 1 ? "none (no SPI block or no card)" : card.why ? card.why : sd_strerror(init_status));
        return buf;
    }
    size_text(card.blocks, sz, sizeof(sz));
    snprintf(buf, size, "sd0, %s, %s, %lu reads, %lu writes, %lu CRC retries, %lu timeouts, %lu errors%s", sz,
             disk->readonly ? "read-only" : "read-write", card.reads, card.writes, card.crc_retries, card.timeouts, card.errors,
             card.single ? "; one block per command (the card refused CMD18/CMD25)" : "");
    return buf;
}

/**** commands ****/

int arty_cmd_sdinfo(int argc, char **argv) {
    char name[96], buf[160];
    int i;
    (void)argc;
    (void)argv;
    if(!arty_has(SURF_SYSINFO_FEATURES_SPI_MASK)) { printf("    no SPI block in this bitstream\n"); return CMD_FAIL; }
    printf("    surf_spi at 0x%08lx: card %s, divider %lu (identification %lu)\n", (u_long)SURF_SPI_BASE,
           spi_present(NULL) ? "present" : "absent", (u_long)(mmio_read32(SREG(SURF_SPI_DIV)) & SURF_SPI_DIV_DIV_MASK),
           (u_long)slow_div);
    if(!card.ready) {
        printf("    not initialized: %s (%s)\n", sd_strerror(init_status), card.why ? card.why : "-");
        return CMD_FAIL;
    }
    sd_cid_describe(card.cid, name, sizeof(name));
    printf("    %s, physical layer %s, %s addressing, OCR 0x%08lx, %s\n", name, card.v2 ? "2.0+" : "1.x",
           card.block_addr ? "block" : "byte", (u_long)card.ocr, card.crc_on ? "CRC on" : "CRC off");
    printf("    CSD version %d, %lu blocks of 512 bytes:", sd_csd_version(card.csd), (u_long)card.blocks);
    for(i = 0; i < 16; i++) printf(" %02x", card.csd[i]);
    printf("\n    %s\n", arty_sd_describe(buf, sizeof(buf)));
    if(card.why) printf("    last problem: %s\n", card.why);
    return CMD_OK;
}

/* sdmode ro|rw: whether the card may be written, while nothing on it is mounted */
int arty_cmd_sdmode(int argc, char **argv) {
    struct bdev *p;
    bool ro;
    if(!disk) { printf("    no card\n"); return CMD_FAIL; }
    if(argc != 2 || (strcmp(argv[1], "ro") && strcmp(argv[1], "rw"))) {
        printf("    sd0 is %s; usage: sdmode ro|rw   (while nothing on it is mounted: umount /sd0p1 first)\n",
               disk->readonly ? "read-only" : "read-write");
        return CMD_USAGE;
    }
    if(vfs_bdev_busy(disk)) { printf("    sd0 or a partition of it is mounted: umount it first\n"); return CMD_FAIL; }
    ro = !strcmp(argv[1], "ro");
    disk->readonly = ro;
    for(p = bdev_first(); p; p = p->next) if(p->parent == disk) p->readonly = ro;
    printf("    sd0 is now %s; mount sd0p1 to use it\n", ro ? "read-only" : "read-write");
    return CMD_OK;
}
