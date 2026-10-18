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

10/16/26 (milestone 8): the block device and card detect are boards/arty_a7/sdblk.c's, which
QEMU's sdsim shares: a card taken out is noticed within half a second (surf_spi has no
interrupt for card detect: a task looks), its disk goes, and a card put back is sd0 again.

10/18/26 (audit B51): sdmode refuses the card while its umount still writes, and says so.
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
#include "sdblk.h"
#include "../../shell/shell.h"
#include <blibc_common.h>

#define SREG(off) ((u_long)SURF_SPI_BASE + (off))

static struct sdblk sd;
static bool have_spi;
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

/* 10/16/26: a data block in a tight loop: on SurfCPU every instruction is fetched from DDR, and the
   byte at a time through xfer cost several times what the SPI transfer itself takes */
static void spi_rx(void *ctx, u8 *buf, u_int len) {
    volatile u32 *data = (volatile u32 *)SREG(SURF_SPI_DATA);
    (void)ctx;
    while(len--) {
        *data = 0xFF;
        *buf++ = (u8)*data;                 /* the read waits for the exchange */
    }
}

static void spi_tx(void *ctx, const u8 *buf, u_int len) {
    volatile u32 *data = (volatile u32 *)SREG(SURF_SPI_DATA);
    (void)ctx;
    while(len--) {
        *data = *buf++;
        (void)*data;
    }
}

static const struct sd_host host = { NULL, spi_xfer, spi_select, spi_slow, spi_present, spi_ms, spi_rx, spi_tx };

/**** the card ****/

/* the line the soc track's system test and the owner read at boot, and again when a card is put in */
static void announce(struct sdblk *s) {
    char name[96], size[16];                /* the CID in words is up to about 70 characters */
    sd_cid_describe(s->card.cid, name, sizeof(name));
    sdblk_size(s->card.blocks, size, sizeof(size));
    kprintf("+SD: %s: %s %s, %s, %s; %s (sd=rw on the command line writes)\n", s->disk->name,
            s->card.block_addr ? (s->card.blocks > 64ull * 1024 * 2048 ? "SDXC" : "SDHC") : "SDSC", size, name,
            s->card.crc_on ? "CRC checked" : "no CRC", s->disk->readonly ? "read-only" : "read-write");
}

int arty_sd_init(void) {
    int r;
    if(!arty_has(SURF_SYSINFO_FEATURES_SPI_MASK)) return 1;
    have_spi = true;
    slow_div = (arty_sys_hz() + 2 * 400000 - 1) / (2 * 400000) - 1;
    if(slow_div > SURF_SPI_DIV_DIV_MASK) slow_div = SURF_SPI_DIV_DIV_MASK;
    mmio_write32(SREG(SURF_SPI_STATUS), SURF_SPI_STATUS_DONE_MASK | SURF_SPI_STATUS_OVR_MASK);
    sd.tag = "SD";
    sd.host = &host;
    sd.announce = announce;
    sd.readonly = !bootinfo_has_arg(&bootinfo, "sd=rw");
    r = sdblk_attach(&sd);
    sdblk_watch(&sd);                       /* a card put in later, or this one taken out */
    return r;
}

const char *arty_sd_describe(char *buf, size_t size) {
    char sz[16];
    if(!sd.disk) {
        if(!have_spi) snprintf(buf, size, "none (no SPI block)");
        else if(!spi_present(NULL)) snprintf(buf, size, "none (no card in the socket)");
        else snprintf(buf, size, "%s", sd.card.why ? sd.card.why : sd_strerror(sd.status));
        return buf;
    }
    sdblk_size(sd.card.blocks, sz, sizeof(sz));
    snprintf(buf, size, "%s, %s, %s, %lu reads, %lu writes, %lu CRC retries, %lu timeouts, %lu errors%s", sd.disk->name, sz,
             sd.disk->readonly ? "read-only" : "read-write", sd.card.reads, sd.card.writes, sd.card.crc_retries,
             sd.card.timeouts, sd.card.errors, sd.card.single ? "; one block per command (the card refused CMD18/CMD25)" : "");
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
    if(sd.taken_out || sd.put_in)
        printf("    card detect: taken out %lu time%s, put in %lu time%s since boot\n", sd.taken_out, sd.taken_out == 1 ? "" : "s",
               sd.put_in, sd.put_in == 1 ? "" : "s");
    if(!sd.disk || !sd.card.ready) {
        if(!spi_present(NULL)) printf("    no card in the socket\n");
        else printf("    not initialized: %s (%s)\n", sd_strerror(sd.status), sd.card.why ? sd.card.why : "-");
        return CMD_FAIL;
    }
    sd_cid_describe(sd.card.cid, name, sizeof(name));
    printf("    %s, physical layer %s, %s addressing, OCR 0x%08lx, %s\n", name, sd.card.v2 ? "2.0+" : "1.x",
           sd.card.block_addr ? "block" : "byte", (u_long)sd.card.ocr, sd.card.crc_on ? "CRC on" : "CRC off");
    printf("    CSD version %d, %lu blocks of 512 bytes:", sd_csd_version(sd.card.csd), (u_long)sd.card.blocks);
    for(i = 0; i < 16; i++) printf(" %02x", sd.card.csd[i]);
    printf("\n    %s\n", arty_sd_describe(buf, sizeof(buf)));
    if(sd.card.why) printf("    last problem: %s\n", sd.card.why);
    return CMD_OK;
}

/* sdmode ro|rw: whether the card may be written, while nothing on it is mounted */
int arty_cmd_sdmode(int argc, char **argv) {
    struct bdev *d = sd.disk;
    bool ro;
    if(!have_spi) { printf("    no SPI block in this bitstream\n"); return CMD_FAIL; }
    if(argc != 2 || (strcmp(argv[1], "ro") && strcmp(argv[1], "rw"))) {
        printf("    %s is %s; usage: sdmode ro|rw   (while nothing on it is mounted: umount /%sp1 first)\n",
               d ? d->name : "the card", sd.readonly ? "read-only" : "read-write", d ? d->name : "sd0");
        return CMD_USAGE;
    }
    if(d && vfs_bdev_unmounting(d)) {           /* 10/18/26 (audit B51): its umount still writes */
        printf("    %s or a partition of it is being unmounted: try again once that has finished\n", d->name);
        return CMD_FAIL;
    }
    if(d && vfs_bdev_busy(d)) { printf("    %s or a partition of it is mounted: umount it first\n", d->name); return CMD_FAIL; }
    ro = !strcmp(argv[1], "ro");
    sdblk_set_readonly(&sd, ro);
    if(d) printf("    %s is now %s; mount %sp1 to use it\n", d->name, ro ? "read-only" : "read-write", d->name);
    else printf("    no card in; a card put in will be %s\n", ro ? "read-only" : "read-write");
    return CMD_OK;
}
