/*
SurfOS board: SurfCPU on the Arty A7 -- an SD card in SPI mode
----------------------
File: sdcard.h  Date: 10/15/26 (SurfCPU)
----------------------
The SD protocol over SPI, independent of the SPI controller: on the Arty the host
functions are surf_spi's (boards/arty_a7/sd.c), in the host test a simulated card
(boards/arty_a7/tests/sdmodel.c). Initialization at the slow clock (CMD0, CMD8, CRC
checking on with CMD59, ACMD41 until ready, CMD58 for the addressing, CMD16 for a
standard-capacity card, CMD9 and CMD10), then the fast clock; single and multiple block
reads (CMD17, CMD18 + CMD12) with every block's CRC16 checked and a bad one read again;
single and multiple block writes (CMD24, CMD25) with the card's data response and busy
time, and CMD13 after each write. SDHC/SDXC cards are addressed in blocks, SDSC cards in
bytes. Every wait is bounded and says what it gave up on.
*/

#ifndef _SDCARD_H
#define _SDCARD_H

#include <surfos/types.h>

struct sd_host {
    void *ctx;
    u8 (*xfer)(void *ctx, u8 out);          /* one byte out on MOSI, the byte that came in on MISO */
    void (*select)(void *ctx, bool on);     /* chip select (on = CS low) */
    void (*slow)(void *ctx, bool slow);     /* at most 400 kHz (identification), or the fastest */
    bool (*present)(void *ctx);             /* card detect (true when the socket cannot tell) */
    u32 (*ms)(void *ctx);                   /* a millisecond clock for the timeouts */
    /* 10/16/26, optional (NULL: a byte at a time through xfer): a data block's bytes in a tight loop,
       received (0xFF out) or sent (MISO not looked at) */
    void (*rx)(void *ctx, u8 *buf, u_int len);
    void (*tx)(void *ctx, const u8 *buf, u_int len);
};

#define SD_OK        0
#define SD_ENOCARD  -1      /* no card in the socket */
#define SD_ETIMEOUT -2      /* the card did not answer in time */
#define SD_ECRC     -3      /* a block's CRC stayed wrong after the retries */
#define SD_EIO      -4      /* the card reported an error */
#define SD_ERANGE   -5      /* outside the card */
#define SD_EUNUSABLE -6     /* not a card SurfOS can use (voltage, version) */

struct sdcard {
    const struct sd_host *host;
    bool ready;             /* initialized and usable */
    bool v2;                /* answered CMD8: physical layer 2.0 or later */
    bool block_addr;        /* SDHC/SDXC (OCR.CCS): arguments are block numbers, not bytes */
    bool crc_on;            /* CMD59 accepted: the card checks our CRCs too */
    u32 ocr;
    u64 blocks;             /* 512-byte blocks */
    u8 csd[16], cid[16];
    bool single;            /* CMD18/CMD25 refused as illegal: one block per command from then on */
    const char *why;        /* the last failure, in words */
    u_long reads, writes, crc_retries, timeouts, errors;
};

int sd_init(struct sdcard *sd, const struct sd_host *host);
int sd_read(struct sdcard *sd, u32 lba, u32 count, u8 *buf);
int sd_write(struct sdcard *sd, u32 lba, u32 count, const u8 *buf);
const char *sd_strerror(int err);

#endif
