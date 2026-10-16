/*
SurfOS board: QEMU virt -- the RAM disk through the SD card driver (the kernel word sdsim)
--------------------
File: sdsim.c   Date: 10/15/26 (SurfCPU)
--------------------
QEMU has no SPI controller with an SD card that SurfCPU's driver could talk to. With the
word sdsim on the kernel command line (the test harness passes it in the boot information)
the RAM disk is not rd0 but sd0: the Arty's SD protocol driver (boards/arty_a7/sdcard.c,
the same file the Arty kernel links) drives a simulated SDHC card (boards/arty_a7/tests/
sdmodel.c, byte by byte, CRCs, data tokens, busy time) whose blocks are the RAM disk. So the
partition code, the block cache (with its runs of up to 128 blocks: CMD18 and CMD25),
the VFS and fs/fat.c run over the real SD code path, under the kernel, at -O2 and -O0. What
this cannot test is the Arty's SPI controller and a real card: that is the board's job.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <sys/bdev.h>
#include <mm/kalloc.h>
#include <riscv/board.h>
#include "../arty_a7/sdcard.h"
#include "../arty_a7/tests/sdmodel.h"
#include <blibc_common.h>

static struct sdmodel model;
static struct sdcard card;
static mutex_t lock = MUTEX_INIT("sdsim");

static u8 m_xfer(void *ctx, u8 out) { (void)ctx; return sdmodel_xfer(&model, out); }
static void m_select(void *ctx, bool on) { (void)ctx; sdmodel_select(&model, on); }
static void m_slow(void *ctx, bool slow) { (void)ctx; (void)slow; }
static bool m_present(void *ctx) { (void)ctx; return model.present; }
static u32 m_ms(void *ctx) { (void)ctx; return (u32)(board_mtime() / (board_timebase_hz() / 1000)); }
static const struct sd_host host = { NULL, m_xfer, m_select, m_slow, m_present, m_ms, NULL, NULL };

static int s_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    int r;
    (void)d;
    mutex_lock(&lock);
    r = sd_read(&card, lba, count, (u8 *)buf);
    mutex_unlock(&lock);
    return r ? BDEV_EIO : 0;
}

static int s_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    int r;
    (void)d;
    mutex_lock(&lock);
    r = sd_write(&card, lba, count, (const u8 *)buf);
    mutex_unlock(&lock);
    return r ? BDEV_EIO : 0;
}

static int s_flush(struct bdev *d) {
    (void)d;
    return 0;
}

static const struct bdev_ops ops = { s_read, s_write, s_flush };

/* the RAM disk image at base, blocks long (a multiple of 1024: an SDHC card's size) */
int qv_sdsim_init(u8 *base, u32 blocks) {
    struct bdev *d;
    int r;
    sdmodel_init(&model, base, blocks, 1, 1, 12345);
    mutex_lock(&lock);
    r = sd_init(&card, &host);
    mutex_unlock(&lock);
    if(r) {
        kprintf("+SDSIM: %s (%s)\n", sd_strerror(r), card.why ? card.why : "?");
        return -1;
    }
    d = (struct bdev *)kcalloc(1, sizeof(struct bdev));
    if(!d) return -1;
    strlcpy(d->name, "sd0", sizeof(d->name));
    strlcpy(d->model, "simulated SDHC card (sdsim)", sizeof(d->model));
    d->block_size = BDEV_BLOCK_SIZE;
    d->nblocks = (u32)card.blocks;
    d->bus = BDEV_BUS_OTHER;
    d->removable = true;
    d->ops = &ops;
    if(bdev_register(d) != 0) { kfree(d); return -1; }
    kprintf("+SDSIM: sd0 is the RAM disk through the SD driver: %lu blocks, %s, %s addressing\n",
            (u_long)card.blocks, card.crc_on ? "CRC on" : "CRC off", card.block_addr ? "block" : "byte");
    bdev_scan_partitions(d);
    return 0;
}

void qv_sdsim_stats(void) {
    printf("    sdsim     %lu reads, %lu writes, %lu CRC retries, %lu timeouts, %lu errors; card saw %lu commands\n",
           card.reads, card.writes, card.crc_retries, card.timeouts, card.errors, model.commands);
}

bool qv_sdsim_active(void) {
    return card.ready;
}
