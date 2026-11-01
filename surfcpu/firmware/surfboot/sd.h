/*
SurfBoot: SD card in SPI mode
--------------------
File: sd.h Date: 10/15/26 (SurfCPU)
--------------------
Initialization (slow clock, CMD0, CMD8, CMD59, CMD58, ACMD41, CMD58, CMD16 for SDSC, CSD and
CID), then 512-byte sector reads with CMD17. SDHC/SDXC cards are addressed by sector,
SDSC cards by byte. Every wait is bounded (sd.c lists the limits), data blocks are checked
against their CRC-16 when the card accepted CMD59, and a failed read is retried, at a
slower clock after CRC errors. Read only: SurfBoot never writes the card.
*/
#ifndef SURFBOOT_SD_H
#define SURFBOOT_SD_H

#include <stdint.h>

#define SD_SECTOR 512u

enum sd_type { SD_NONE = 0, SD_V1_SDSC, SD_V2_SDSC, SD_V2_SDHC };

struct sd_card {
    int type;                   /* enum sd_type */
    int block_addr;             /* 1: sector addresses (SDHC/SDXC); 0: byte addresses (SDSC) */
    int crc_on;                 /* the card accepted CMD59: data CRCs are checked */
    uint32_t ocr;
    uint8_t csd[16], cid[16];
    int csd_ok, cid_ok;
    uint32_t sectors;           /* capacity from the CSD (0 if unknown) */
    uint32_t init_div;          /* SPI divider for identification (<= 400 kHz) */
    uint32_t fast_div;          /* divider after initialization */
    uint32_t div;               /* divider in use (raised after CRC errors) */
    uint32_t init_us;           /* time from the first ACMD41 to ready */
    uint32_t reads, retries, crc_errors, token_errors, timeouts;
};

/* Divider for the fastest SCLK not above hz. */
uint32_t sd_div_for(uint32_t sys_hz, uint32_t hz);
uint32_t sd_div_hz(uint32_t sys_hz, uint32_t div);

int sd_init(struct sd_card *c, uint32_t fast_div);
int sd_read(struct sd_card *c, uint32_t lba, void *buf);
const char *sd_type_name(int type);

/* CSD helpers (exposed for the monitor and the tests). */
uint32_t sd_csd_sectors(const uint8_t csd[16]);
uint8_t sd_crc7(const uint8_t *p, unsigned n);
uint16_t sd_crc16(const uint8_t *p, unsigned n);

#endif /* SURFBOOT_SD_H */
