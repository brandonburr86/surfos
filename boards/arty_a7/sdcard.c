/*
SurfOS board: SurfCPU on the Arty A7 -- an SD card in SPI mode
--------------------
File: sdcard.c  Date: 10/15/26 (SurfCPU)
--------------------
See sdcard.h. Plain C over struct sd_host: no kernel calls, so the same file runs in the
kernel and in the host test against a simulated card. Timeouts follow the SD Physical
Layer Simplified Specification's limits with room to spare: 1 s for initialization,
100 ms for a read's data token, 500 ms of busy after a write.

10/16/26 (milestone 8): card detect is the first thing a read or write looks at, so a card
that was taken out fails at once (SD_ENOCARD) instead of in a timeout; whoever watches the
socket (boards/arty_a7/sdblk.c) sets ready to false, and a card put back is initialized
again before it is used. And after a written block the card refused (a write error, a CRC
error) the driver waits for the card's busy time before the stop token, as after a block
it took: the io track's model found the token sent into the busy time, the write-fault
test (boards/arty_a7/tests/iomodel) being the first to refuse a block inside CMD25.
*/

#include "sdcard.h"
#include "sdproto.h"

#define T_INIT_MS   1000
#define T_TOKEN_MS  100
#define T_BUSY_MS   500
#define CRC_TRIES   3
#define E_NO_MULTI  1           /* (internal) CMD18 or CMD25 refused as an illegal command */

static u8 xf(struct sdcard *sd, u8 out) {
    return sd->host->xfer(sd->host->ctx, out);
}

static u32 now(struct sdcard *sd) {
    return sd->host->ms(sd->host->ctx);
}

static int fail(struct sdcard *sd, int err, const char *why) {
    sd->why = why;
    if(err == SD_ETIMEOUT) sd->timeouts++;
    else sd->errors++;
    return err;
}

/* 0xFF out until 0xFF comes back: the card is not busy */
static bool wait_ready(struct sdcard *sd, u32 ms) {
    u32 t0 = now(sd);
    do {
        if(xf(sd, 0xFF) == 0xFF) return true;
    } while(now(sd) - t0 < ms);
    return false;
}

static void deselect(struct sdcard *sd) {
    sd->host->select(sd->host->ctx, false);
    xf(sd, 0xFF);                           /* the card lets go of MISO on a clock with CS high */
}

static bool select_card(struct sdcard *sd) {
    sd->host->select(sd->host->ctx, true);
    xf(sd, 0xFF);
    if(wait_ready(sd, T_BUSY_MS)) return true;
    deselect(sd);
    return false;
}

/* one command, CS low; its R1, or 0xFF when no response came within NCR (8 bytes) */
static u8 command(struct sdcard *sd, u8 cmd, u32 arg) {
    u8 f[6], r = 0xFF;
    int i;
    sd_frame(cmd, arg, f);
    for(i = 0; i < 6; i++) xf(sd, f[i]);
    if(cmd == SD_CMD12) xf(sd, 0xFF);       /* the stuff byte after STOP_TRANSMISSION */
    for(i = 0; i < 9; i++) {
        r = xf(sd, 0xFF);
        if(!(r & 0x80)) break;
    }
    return r;
}

/* an application command: CMD55, then cmd; the second R1 (or the first, if CMD55 failed) */
static u8 app_command(struct sdcard *sd, u8 cmd, u32 arg) {
    u8 r = command(sd, SD_CMD55, 0);
    if(r > SD_R1_IDLE) return r;
    return command(sd, cmd, arg);
}

/* the first byte that is not 0xFF: a start token or an error token; 0xFF when time ran out */
static u8 wait_token(struct sdcard *sd, u32 ms) {
    u32 t0 = now(sd);
    u8 t;
    do {
        t = xf(sd, 0xFF);
        if(t != 0xFF) return t;
    } while(now(sd) - t0 < ms);
    return 0xFF;
}

/* len bytes of a data block and its CRC16, after the start token; true when the CRC matched (a card
   sends a correct CRC16 with every block it reads out, whether it checks ours or not) */
static bool read_data(struct sdcard *sd, u8 *buf, u_int len) {
    u_int i;
    u16 crc;
    if(sd->host->rx) sd->host->rx(sd->host->ctx, buf, len);
    else for(i = 0; i < len; i++) buf[i] = xf(sd, 0xFF);
    crc = (u16)(xf(sd, 0xFF) << 8);
    crc |= xf(sd, 0xFF);
    return crc == sd_crc16(buf, len);
}

static int r1_error(struct sdcard *sd, u8 r, const char *why) {
    if(r == 0xFF) return fail(sd, SD_ETIMEOUT, why);
    if(r & (SD_R1_ADDRESS | SD_R1_PARAMETER)) return fail(sd, SD_ERANGE, why);
    return fail(sd, SD_EIO, why);
}

/* CSD or CID: R1, a data token, 16 bytes, their CRC */
static int read_register(struct sdcard *sd, u8 cmd, u8 out[16]) {
    int tries;
    for(tries = 0; tries < CRC_TRIES; tries++) {
        u8 r, t;
        bool ok;
        if(!select_card(sd)) return fail(sd, SD_ETIMEOUT, "the card stayed busy before a register read");
        r = command(sd, cmd, 0);
        if(r) { deselect(sd); return r1_error(sd, r, cmd == SD_CMD9 ? "CMD9 (CSD) refused" : "CMD10 (CID) refused"); }
        t = wait_token(sd, T_TOKEN_MS);
        if(t != SD_TOKEN_START) {
            deselect(sd);
            return fail(sd, t == 0xFF ? SD_ETIMEOUT : SD_EIO, "no data token for a register read");
        }
        ok = read_data(sd, out, 16) && (out[15] >> 1) == sd_crc7(out, 15);  /* the register's own CRC7 too */
        deselect(sd);
        if(ok) return SD_OK;
        sd->crc_retries++;
    }
    return fail(sd, SD_ECRC, "a register's CRC stayed wrong");
}

int sd_init(struct sdcard *sd, const struct sd_host *host) {
    u8 r = 0xFF, r7[4];
    u32 t0;
    int i, err;

    sd->host = host;
    sd->ready = sd->v2 = sd->block_addr = sd->crc_on = false;
    sd->blocks = 0;
    sd->why = 0;
    if(!host->present(host->ctx)) return fail(sd, SD_ENOCARD, "no card in the socket (card detect)");
    host->slow(host->ctx, true);
    deselect(sd);
    for(i = 0; i < 10; i++) xf(sd, 0xFF);   /* 80 clocks with CS high: the card enters SPI mode on the next CMD0 */

    for(i = 0; i < 10 && r != SD_R1_IDLE; i++) {   /* a card stopped in mid-transfer may need a few */
        host->select(host->ctx, true);
        r = command(sd, SD_CMD0, 0);
        deselect(sd);
    }
    if(r != SD_R1_IDLE)
        return fail(sd, r == 0xFF ? SD_ENOCARD : SD_EIO, r == 0xFF ? "no answer to CMD0 (no card?)" : "CMD0 did not reach the idle state");

    if(!select_card(sd)) return fail(sd, SD_ETIMEOUT, "busy after CMD0");
    r = command(sd, SD_CMD8, 0x1AA);
    if(r == SD_R1_IDLE) for(i = 0; i < 4; i++) r7[i] = xf(sd, 0xFF);
    deselect(sd);
    if(r == SD_R1_IDLE) {
        if((r7[2] & 0x0F) != 0x01 || r7[3] != 0xAA) return fail(sd, SD_EUNUSABLE, "CMD8: 2.7-3.6 V not accepted");
        sd->v2 = true;
    } else if(r != (SD_R1_IDLE | SD_R1_ILLEGAL)) {
        return r1_error(sd, r, "CMD8 failed");
    }

    if(!select_card(sd)) return fail(sd, SD_ETIMEOUT, "busy before CMD59");
    sd->crc_on = command(sd, SD_CMD59, 1) == SD_R1_IDLE;     /* the card checks our CRCs from now on */
    deselect(sd);

    t0 = now(sd);
    do {
        if(!select_card(sd)) return fail(sd, SD_ETIMEOUT, "busy during ACMD41");
        r = app_command(sd, SD_ACMD41, sd->v2 ? 0x40000000u : 0);
        deselect(sd);
    } while(r == SD_R1_IDLE && now(sd) - t0 < T_INIT_MS);
    if(r) return r == SD_R1_IDLE ? fail(sd, SD_ETIMEOUT, "ACMD41: still initializing after 1 s") : r1_error(sd, r, "ACMD41 failed");

    if(sd->v2) {
        if(!select_card(sd)) return fail(sd, SD_ETIMEOUT, "busy before CMD58");
        r = command(sd, SD_CMD58, 0);
        sd->ocr = 0;
        if(!r) for(i = 0; i < 4; i++) sd->ocr = (sd->ocr << 8) | xf(sd, 0xFF);
        deselect(sd);
        if(r) return r1_error(sd, r, "CMD58 (OCR) failed");
        if(!(sd->ocr & SD_OCR_BUSY)) return fail(sd, SD_EIO, "CMD58: the power-up bit is clear after ACMD41");
        sd->block_addr = (sd->ocr & SD_OCR_CCS) != 0;
    }
    if(!sd->block_addr) {                   /* standard capacity: byte addresses, 512-byte blocks */
        if(!select_card(sd)) return fail(sd, SD_ETIMEOUT, "busy before CMD16");
        r = command(sd, SD_CMD16, 512);
        deselect(sd);
        if(r) return r1_error(sd, r, "CMD16 (block length 512) failed");
    }
    err = read_register(sd, SD_CMD9, sd->csd);
    if(!err) err = read_register(sd, SD_CMD10, sd->cid);
    if(err) return err;
    sd->blocks = sd_csd_blocks(sd->csd);
    if(!sd->blocks) return fail(sd, SD_EUNUSABLE, "the CSD register is not one SurfOS knows");
    host->slow(host->ctx, false);
    sd->ready = true;
    return SD_OK;
}

/* CMD13: the card's status (R2), which holds the errors since the last CMD13; reading it clears
   them. 0 for a card with nothing to report, -1 when it did not answer. */
static int card_status(struct sdcard *sd) {
    u8 r, r2;
    if(!select_card(sd)) return -1;
    r = command(sd, SD_CMD13, 0);
    r2 = xf(sd, 0xFF);
    deselect(sd);
    if(r & 0x80) return -1;
    return (r << 8) | r2;
}

static int check_range(struct sdcard *sd, u32 lba, u32 count) {
    if(!sd->host->present(sd->host->ctx)) return fail(sd, SD_ENOCARD, "the card is not in the socket (card detect)");
    if(!sd->ready) return fail(sd, SD_EIO, "the card is not initialized");
    if(!count || (u64)lba + count > sd->blocks) return fail(sd, SD_ERANGE, "a block outside the card");
    if(!sd->block_addr && (u64)(lba + count) * 512 > 0xFFFFFFFFull) return fail(sd, SD_ERANGE, "beyond 4 GB of byte addresses");
    return SD_OK;
}

static u32 address(struct sdcard *sd, u32 lba) {
    return sd->block_addr ? lba : lba * 512;
}

/* one try at count blocks; *bad: the first block whose CRC was wrong */
static int read_once(struct sdcard *sd, u32 lba, u32 count, u8 *buf, u32 *bad) {
    u32 i;
    u8 r, t;
    int err = SD_OK;
    *bad = count;
    if(!select_card(sd)) return fail(sd, SD_ETIMEOUT, "busy before a read");
    r = command(sd, count > 1 ? SD_CMD18 : SD_CMD17, address(sd, lba));
    if(r) {
        deselect(sd);
        if(count > 1 && r == SD_R1_ILLEGAL) return E_NO_MULTI;
        return r1_error(sd, r, "the read command was refused");
    }
    for(i = 0; i < count; i++) {
        t = wait_token(sd, T_TOKEN_MS);
        if(t != SD_TOKEN_START) {
            err = fail(sd, t == 0xFF ? SD_ETIMEOUT : SD_EIO, t == 0xFF ? "no data token" : "the card sent an error token");
            break;
        }
        if(!read_data(sd, buf + i * 512, 512)) {
            *bad = i;
            err = SD_ECRC;
            break;
        }
    }
    if(count > 1) {
        command(sd, SD_CMD12, 0);
        if(!wait_ready(sd, T_BUSY_MS) && !err) err = fail(sd, SD_ETIMEOUT, "busy after CMD12");
    }
    deselect(sd);
    return err;
}

/* Every SD card has CMD18 and CMD25 (command classes 2 and 4), but a simulated card may lack
   them (the fw track's, whose SurfBoot reads with CMD17 only): one refused as an illegal
   command switches the driver to one block per command for good. */
int sd_read(struct sdcard *sd, u32 lba, u32 count, u8 *buf) {
    int err = check_range(sd, lba, count), tries = 0;
    u32 n, bad;
    if(err) return err;
    while(count) {
        n = sd->single ? 1 : count;
        err = read_once(sd, lba, n, buf, &bad);
        if(err == E_NO_MULTI) {
            sd->single = true;
            card_status(sd);
            continue;
        }
        if(err == SD_ECRC) {
            if(++tries >= CRC_TRIES) {
                err = fail(sd, SD_ECRC, "a block's CRC stayed wrong after 3 reads");
                break;
            }
            sd->crc_retries++;
            err = SD_OK;
        } else if(err) break;
        else bad = n;
        lba += bad;                         /* the blocks before a bad one are good */
        buf += bad * 512;
        count -= bad;
    }
    if(err) card_status(sd);                /* the card's error status, read so that it is clear for the next command */
    else sd->reads++;
    return err;
}

/* one try at count blocks */
static int write_once(struct sdcard *sd, u32 lba, u32 count, const u8 *buf, u32 *done) {
    u32 i, j;
    u8 r, resp = 0xFF;
    u16 crc;
    int err = SD_OK, dr, st;
    *done = 0;
    if(!select_card(sd)) return fail(sd, SD_ETIMEOUT, "busy before a write");
    r = command(sd, count > 1 ? SD_CMD25 : SD_CMD24, address(sd, lba));
    if(r) {
        deselect(sd);
        card_status(sd);
        if(count > 1 && r == SD_R1_ILLEGAL) return E_NO_MULTI;
        return r1_error(sd, r, "the write command was refused");
    }
    for(i = 0; i < count; i++) {
        const u8 *b = buf + i * 512;
        xf(sd, 0xFF);                       /* at least one byte before the token */
        xf(sd, count > 1 ? SD_TOKEN_START_MULTI : SD_TOKEN_START);
        if(sd->host->tx) sd->host->tx(sd->host->ctx, b, 512);
        else for(j = 0; j < 512; j++) xf(sd, b[j]);
        crc = sd_crc16(b, 512);
        xf(sd, (u8)(crc >> 8));
        xf(sd, (u8)crc);
        for(j = 0; j < 8 && (resp = xf(sd, 0xFF)) == 0xFF; j++) ;
        dr = sd_data_response(resp);
        if(dr != SD_DATA_ACCEPTED) {
            err = dr == SD_DATA_CRC_ERROR ? SD_ECRC : fail(sd, SD_EIO, "the card refused a written block");
            wait_ready(sd, T_BUSY_MS);      /* 10/16/26: a card may be busy after refusing a block too: */
            break;                          /* the stop token waits for it (the io track's model said so) */
        }
        if(!wait_ready(sd, T_BUSY_MS)) {
            err = fail(sd, SD_ETIMEOUT, "busy for more than 500 ms after a write");
            break;
        }
        (*done)++;
    }
    if(count > 1) {                         /* the stop token ends CMD25, after an error too (a command */
        xf(sd, SD_TOKEN_STOP_MULTI);        /* sent while the card is busy would be ignored) */
        xf(sd, 0xFF);
        if(!wait_ready(sd, T_BUSY_MS) && !err) err = fail(sd, SD_ETIMEOUT, "busy after the last block");
    }
    deselect(sd);
    st = card_status(sd);                   /* R2: an error while programming? (read after a failure too: it clears) */
    if(err) return err;
    if(st < 0) return fail(sd, SD_ETIMEOUT, "no answer to CMD13 after the write");
    if(st) return fail(sd, SD_EIO, "CMD13 after the write reports an error");
    return SD_OK;
}

int sd_write(struct sdcard *sd, u32 lba, u32 count, const u8 *buf) {
    int err = check_range(sd, lba, count), tries = 0;
    u32 n, done;
    if(err) return err;
    while(count) {
        n = sd->single ? 1 : count;
        err = write_once(sd, lba, n, buf, &done);
        if(err == E_NO_MULTI) {             /* (see sd_read) */
            sd->single = true;
            continue;
        }
        if(err == SD_ECRC) {
            if(++tries >= CRC_TRIES) return fail(sd, SD_ECRC, "the card kept rejecting a block's CRC");
            sd->crc_retries++;
            err = SD_OK;
        } else if(err) break;
        else done = n;
        lba += done;
        buf += done * 512;
        count -= done;
    }
    if(!err) sd->writes++;
    return err;
}

const char *sd_strerror(int err) {
    switch(err) {
    case SD_OK: return "ok";
    case SD_ENOCARD: return "no card";
    case SD_ETIMEOUT: return "timed out";
    case SD_ECRC: return "CRC error";
    case SD_EIO: return "card error";
    case SD_ERANGE: return "out of range";
    case SD_EUNUSABLE: return "unusable card";
    }
    return "?";
}
