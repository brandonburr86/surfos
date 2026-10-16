/*
SurfCPU firmware tests: a microSD card in SPI mode
--------------------
File: sdcard_sim.h Date: 10/15/26 (SurfCPU)
--------------------
A byte-level model of an SD card on the SPI bus, behind a card image file (read only: the
model never writes the image), for the host tests of SurfBoot's SD, FAT and image code and
for the firmware emulator. It is written from the SD Physical Layer Simplified
Specification independently of firmware/surfboot/sd.c (its own CRC-7 and CRC-16), so the
two check each other. It models: the power-up clocks, CMD0 into SPI mode, CMD8, CMD55 +
ACMD41 with a configurable busy period, CMD58 (OCR, CCS), CMD59 (CRC checking of commands
when on; CMD0 and CMD8 always), CMD16, CMD9/CMD10 data blocks, CMD13 and CMD17 reads with
NCR/NAC delays, SDHC block addressing and SDSC byte addressing, and these faults: no card,
no card-detect switch, a dead card, MISO stuck low, ACMD41 never ready, a bad CMD8 echo, no
3.3 V support, an MMC card, CMD59 refused, wrong data CRCs, data error tokens, missing data
tokens, a clock too fast for the card, and removal after a number of reads.
(The cycle-level C++ model of docs/MODULES.md section 7 is the io track's sdcard_model.)
*/
#ifndef SURFCPU_SDCARD_SIM_H
#define SURFCPU_SDCARD_SIM_H

#include <stdint.h>

enum sdsim_type { SDSIM_SDHC = 0, SDSIM_SDSC2, SDSIM_SDSC1, SDSIM_MMC };

struct sdsim_opts {
    int type;               /* enum sdsim_type */
    int absent;             /* no card in the socket */
    int no_cd;              /* the socket has no card-detect switch: present() is 1 even when empty */
    int dead;               /* a card that never drives MISO */
    int miso_low;           /* MISO stuck low */
    int acmd41_busy;        /* ACMD41 answers idle this many times first; -1 = forever */
    int cmd8_bad_echo;
    int no_33v;             /* OCR without the 3.2-3.4 V window */
    int no_cmd59;           /* CMD59 is refused as illegal */
    int ncr;                /* 0xFF bytes before each response, 1..8 */
    int nac;                /* 0xFF bytes before each data token */
    int crc_errors;         /* the next N read blocks carry a wrong CRC */
    int crc_every;          /* every Nth read block carries a wrong CRC (0 = never) */
    int token_errors;       /* the next N reads answer with data error token 0x08 */
    int read_timeouts;      /* the next N reads never send a data token */
    int remove_after;       /* the card is pulled out after this many reads (0 = never) */
    int min_div;            /* the card garbles data at SPI dividers below this (0 = any speed) */
    int no_powerup_check;   /* do not insist on 74 clocks with CS high before CMD0 */
};

struct sdsim_stats {
    uint32_t bytes, cmds, cmd0, cmd8, acmd41, cmd17, cmd9, crc_injected, bad_cmd_crc;
    uint32_t last_cmd;
};

struct sdsim;

/* image may be NULL for an empty socket. Returns NULL (with a message on stderr) on error. */
struct sdsim *sdsim_open(const char *image, const struct sdsim_opts *o);
void sdsim_close(struct sdsim *c);
/* A card over a caller's buffer (not copied; the caller may change it between reads). */
struct sdsim *sdsim_open_mem(uint8_t *buf, uint64_t size, const struct sdsim_opts *o);
void sdsim_default_opts(struct sdsim_opts *o);
int sdsim_present(struct sdsim *c);                 /* what the card-detect switch says */
void sdsim_set_inserted(struct sdsim *c, int in);  /* pull the card out or put it back */
void sdsim_set_div(struct sdsim *c, uint32_t div); /* the host's SPI divider (for min_div) */
uint8_t sdsim_xfer(struct sdsim *c, int cs_asserted, uint8_t mosi);
uint32_t sdsim_sectors(struct sdsim *c);
const struct sdsim_stats *sdsim_stats(struct sdsim *c);
const uint8_t *sdsim_image(struct sdsim *c);       /* the mapped image (for checks) */

#endif /* SURFCPU_SDCARD_SIM_H */
