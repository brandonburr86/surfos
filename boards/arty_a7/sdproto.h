/*
SurfOS board: SurfCPU on the Arty A7 -- SD card protocol helpers
----------------------
File: sdproto.h Date: 10/15/26 (SurfCPU)
----------------------
The parts of the SD Physical Layer Simplified Specification's SPI mode that are pure
arithmetic: the command frame with its CRC7, the CRC16 of a data block, the data response
token, and reading the CSD (capacity) and CID (name) registers. Host-tested
(boards/arty_a7/tests/test_arty.c) against the specification's examples.
*/

#ifndef _SDPROTO_H
#define _SDPROTO_H

#include <surfos/types.h>

/* commands (SPI mode) */
#define SD_CMD0   0     /* GO_IDLE_STATE */
#define SD_CMD8   8     /* SEND_IF_COND: 0x1AA = 2.7-3.6 V, check pattern 0xAA */
#define SD_CMD9   9     /* SEND_CSD */
#define SD_CMD10  10    /* SEND_CID */
#define SD_CMD12  12    /* STOP_TRANSMISSION */
#define SD_CMD13  13    /* SEND_STATUS (R2) */
#define SD_CMD16  16    /* SET_BLOCKLEN */
#define SD_CMD17  17    /* READ_SINGLE_BLOCK */
#define SD_CMD18  18    /* READ_MULTIPLE_BLOCK */
#define SD_CMD24  24    /* WRITE_BLOCK */
#define SD_CMD25  25    /* WRITE_MULTIPLE_BLOCK */
#define SD_CMD55  55    /* APP_CMD */
#define SD_CMD58  58    /* READ_OCR (R3) */
#define SD_CMD59  59    /* CRC_ON_OFF */
#define SD_ACMD41 41    /* SD_SEND_OP_COND (after CMD55) */

/* R1 bits */
#define SD_R1_IDLE        0x01
#define SD_R1_ERASE_RESET 0x02
#define SD_R1_ILLEGAL     0x04
#define SD_R1_CRC         0x08
#define SD_R1_ERASE_SEQ   0x10
#define SD_R1_ADDRESS     0x20
#define SD_R1_PARAMETER   0x40

/* tokens */
#define SD_TOKEN_START       0xFE   /* a data block (single block, CSD, CID, and each block of CMD18) */
#define SD_TOKEN_START_MULTI 0xFC   /* each block of CMD25 */
#define SD_TOKEN_STOP_MULTI  0xFD   /* the end of CMD25 */

#define SD_OCR_CCS  0x40000000u     /* card capacity status: SDHC/SDXC, block addressing */
#define SD_OCR_BUSY 0x80000000u     /* power-up finished */

u8 sd_crc7(const u8 *data, u_int len);
u16 sd_crc16(const u8 *data, u_int len);                /* CRC-16/XMODEM: polynomial 0x1021, from 0 */
void sd_frame(u8 cmd, u32 arg, u8 out[6]);              /* 0x40 | cmd, the argument big-endian, CRC7 << 1 | 1 */

/* a data response token (after a written block): 0 accepted, or why not */
#define SD_DATA_ACCEPTED   0
#define SD_DATA_CRC_ERROR  1
#define SD_DATA_WRITE_ERROR 2
#define SD_DATA_BAD        3
int sd_data_response(u8 token);

/* the CSD register (16 bytes, as the card sends them, most significant first) */
u32 sd_csd_bits(const u8 csd[16], u_int msb, u_int lsb);   /* bits msb..lsb of 127..0 */
u64 sd_csd_blocks(const u8 csd[16]);                     /* 512-byte blocks; 0 for a CSD it does not know */
int sd_csd_version(const u8 csd[16]);                    /* 1 (SDSC) or 2 (SDHC/SDXC); 0 otherwise */

/* the CID register: "SD32G rev 8.0, manufacturer 0x03 'SD', serial ..., 2026-10" */
void sd_cid_describe(const u8 cid[16], char *buf, u_int size);

#endif
