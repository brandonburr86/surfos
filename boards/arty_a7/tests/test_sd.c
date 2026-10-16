/*
SurfOS host test: the SD card
--------------------
File: test_sd.c   Date: 10/15/26 (SurfCPU)
--------------------
boards/arty_a7/sdproto.c against the specification's examples, and sdcard.c against
the simulated card of sdmodel.c: both capacities and versions, random traffic, faults.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "sdproto.h"
#include "sdcard.h"
#include "sdmodel.h"

/**** SD helpers ****/

static void test_sdproto(unsigned seed) {
    (void)seed;
    u8 f[6], buf[512], csd[16];
    char name[80];
    static const struct { u8 cmd; u32 arg; u8 crc; } frames[] = {
        { SD_CMD0, 0, 0x95 }, { SD_CMD8, 0x1AA, 0x87 }, { SD_CMD55, 0, 0x65 }, { SD_ACMD41, 0x40000000, 0x77 },
        { SD_CMD58, 0, 0xFD }, { SD_CMD17, 0, 0x55 },
    };
    u_int i, ok = 1;
    for(i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        sd_frame(frames[i].cmd, frames[i].arg, f);
        if(f[0] != (0x40 | frames[i].cmd) || f[5] != frames[i].crc) ok = 0;
    }
    check(ok, "command frames: CMD0 ..95, CMD8 1AA ..87, CMD55 ..65, ACMD41 ..77, CMD58 ..FD, CMD17 ..55");
    memset(buf, 0xFF, sizeof(buf));
    check(sd_crc16(buf, 512) == 0x7FA1, "CRC16 of 512 bytes of 0xFF is 0x7FA1 (the specification's example)");
    check(sd_data_response(0x05) == SD_DATA_ACCEPTED && sd_data_response(0xE5) == SD_DATA_ACCEPTED &&
          sd_data_response(0x0B) == SD_DATA_CRC_ERROR && sd_data_response(0xEB) == SD_DATA_CRC_ERROR &&
          sd_data_response(0x0D) == SD_DATA_WRITE_ERROR && sd_data_response(0xED) == SD_DATA_WRITE_ERROR &&
          sd_data_response(0xFF) == SD_DATA_BAD, "data response tokens: low 5 bits 0x05, 0x0B, 0x0D (the model's 0xE5, 0xEB, 0xED)");
    /* a 32 GB SDHC card's CSD: C_SIZE 0xEDC7 */
    memset(csd, 0, sizeof(csd));
    csd[0] = 0x40; csd[7] = 0x00; csd[8] = 0xED; csd[9] = 0xC7;
    check(sd_csd_version(csd) == 2 && sd_csd_blocks(csd) == (u64)(0xEDC7 + 1) * 1024, "CSD 2.0: (C_SIZE + 1) x 512 KB");
    /* a 2 GB SDSC card: READ_BL_LEN 10, C_SIZE 4095, C_SIZE_MULT 7 */
    memset(csd, 0, sizeof(csd));
    csd[5] = 0x0A;                                   /* READ_BL_LEN = 10 (bits 83:80) */
    csd[6] = 0x03; csd[7] = 0xFF; csd[8] = 0xC0;     /* C_SIZE 4095 (bits 73:62) */
    csd[9] = 0x03; csd[10] = 0x80;                   /* C_SIZE_MULT 7 (bits 49:47) */
    check(sd_csd_version(csd) == 1 && sd_csd_bits(csd, 73, 62) == 4095 && sd_csd_bits(csd, 49, 47) == 7 &&
          sd_csd_blocks(csd) == 4096ull * 512 * 1024 / 512, "CSD 1.0: (C_SIZE + 1) x 2^(MULT + 2) x 2^READ_BL_LEN");
    { u8 cid[16] = { 0x03, 'S', 'D', 'S', 'U', 'R', 'F', '1', 0x10, 0x12, 0x34, 0x56, 0x78, 0x01, 0xAA, 0 };
      sd_cid_describe(cid, name, sizeof(name));
      check(!strcmp(name, "SURF1 rev 1.0, manufacturer 0x03 'SD', serial 12345678, made 2026-10"), "the CID in words"); }
}

/**** the SD driver against the simulated card ****/

static struct sdmodel model;
static unsigned long fake_bytes;

static u8 h_xfer(void *ctx, u8 out) { (void)ctx; fake_bytes++; return sdmodel_xfer(&model, out); }
static void h_select(void *ctx, bool on) { (void)ctx; sdmodel_select(&model, on); }
static void h_slow(void *ctx, bool slow) { (void)ctx; (void)slow; }
static bool h_present(void *ctx) { (void)ctx; return model.present; }
static u32 h_ms(void *ctx) { (void)ctx; return (u32)(fake_bytes / 1000); }    /* 1000 byte times a millisecond */
static const struct sd_host host = { NULL, h_xfer, h_select, h_slow, h_present, h_ms };

static void sd_case(const char *what, int v2, int hc, u32 blocks, unsigned seed) {
    u8 *img = malloc((size_t)blocks * 512), *shadow = malloc((size_t)blocks * 512), *buf = malloc(64 * 512);
    struct sdcard sd;
    char line[160];
    int r, i, bad = 0;
    u32 b;
    for(b = 0; b < blocks * 512; b++) img[b] = (u8)(b * 7 + (b >> 9) + seed);
    memcpy(shadow, img, (size_t)blocks * 512);
    sdmodel_init(&model, img, blocks, v2, hc, seed);
    memset(&sd, 0, sizeof(sd));
    r = sd_init(&sd, &host);
    snprintf(line, sizeof(line), "%s (seed %u): initializes as %s, %s addressing, CRC on, %u blocks", what, seed,
             v2 ? "version 2" : "version 1", hc ? "block" : "byte", blocks);
    check(r == SD_OK && sd.ready && sd.v2 == (v2 != 0) && sd.block_addr == (hc != 0) && sd.crc_on && sd.blocks == blocks, line);
    if(r) { printf("  sd_init: %s (%s)\n", sd_strerror(r), sd.why ? sd.why : "-"); goto out; }
    for(i = 0; i < 300 && !bad; i++) {               /* random reads and writes against the shadow copy */
        u32 count = 1 + rnd() % ((rnd() % 4 == 0) ? 64 : 4), lba = rnd() % (blocks - count);
        if(rnd() % 3 == 0) {
            u32 j;
            for(j = 0; j < count * 512; j++) buf[j] = (u8)rnd();
            if(sd_write(&sd, lba, count, buf) != SD_OK) bad = 1;
            memcpy(shadow + (size_t)lba * 512, buf, count * 512);
        } else {
            if(sd_read(&sd, lba, count, buf) != SD_OK || memcmp(buf, shadow + (size_t)lba * 512, count * 512)) bad = 2;
        }
    }
    check(!bad && !memcmp(img, shadow, (size_t)blocks * 512),
          "300 random reads and writes of 1-64 blocks, single and multiple: the card holds what was written");
    model.corrupt_reads = 1;
    r = sd_read(&sd, 10, 4, buf);
    check(r == SD_OK && sd.crc_retries >= 1 && !memcmp(buf, shadow + 10 * 512, 4 * 512), "a corrupted block is read again");
    model.corrupt_reads = 100;
    r = sd_read(&sd, 10, 1, buf);
    model.corrupt_reads = 0;
    check(r == SD_ECRC, "a block that stays corrupted is a CRC error");
    model.reject_writes = 1;
    memset(buf, 0x5A, 512);
    r = sd_write(&sd, 20, 1, buf);
    memcpy(shadow + 20 * 512, buf, 512);
    check(r == SD_OK && !memcmp(img + 20 * 512, buf, 512), "a write the card rejects for its CRC is sent again");
    check(sd_read(&sd, blocks - 1, 2, buf) == SD_ERANGE, "a read past the end is refused");
    model.no_token = 1;
    r = sd_read(&sd, 5, 1, buf);
    model.no_token = 0;
    check(r == SD_ETIMEOUT, "a data token that never comes is a timeout");
    check(sd_read(&sd, 5, 1, buf) == SD_OK && !memcmp(buf, shadow + 5 * 512, 512), "and the card works again afterwards");
    model.reject_writes = 1;
    memset(buf, 0xA5, 3 * 512);
    r = sd_write(&sd, 30, 3, buf);
    memcpy(shadow + 30 * 512, buf, 3 * 512);
    check(r == SD_OK && !memcmp(img + 30 * 512, buf, 3 * 512), "a rejected block in a multiple write: the stop token, then the rest again");
    model.no_multi = 1;                         /* a card without CMD18 and CMD25 */
    {
        u_long errors = sd.errors;
        memset(buf, 0x3C, 8 * 512);
        r = sd_write(&sd, 40, 8, buf);
        memcpy(shadow + 40 * 512, buf, 8 * 512);
        memset(buf, 0, 12 * 512);
        check(r == SD_OK && sd_read(&sd, 38, 12, buf) == SD_OK && !memcmp(buf, shadow + 38 * 512, 12 * 512) &&
              !memcmp(img + 40 * 512, shadow + 40 * 512, 8 * 512) && sd.single && sd.errors == errors,
              "a card that refuses CMD18 and CMD25: one block per command, no error");
    }
    model.no_multi = 0;
    check(model.violations == 0, "the driver never sent a command while the card was busy, nor let CS go inside a block");
out:
    free(img);
    free(shadow);
    free(buf);
}

static void test_sdcard(unsigned seed) {
    struct sdcard sd;
    u8 img[512 * 8];
    sd_case("SDHC card", 1, 1, 64 * 2048, seed);
    sd_case("SDSC card, version 2", 1, 0, 32 * 2048, seed);
    sd_case("SDSC card, version 1", 0, 0, 16 * 2048, seed);
    sdmodel_init(&model, img, 8, 1, 1, seed);
    model.present = 0;
    check(sd_init(&sd, &host) == SD_ENOCARD, "no card in the socket");
}

TEST(test_sdproto)
TEST(test_sdcard)
