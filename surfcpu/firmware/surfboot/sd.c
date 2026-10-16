/*
SurfBoot: SD card in SPI mode
--------------------
File: sd.c Date: 10/15/26 (SurfCPU)
--------------------
See sd.h. The protocol is the SD Association's Physical Layer Simplified Specification
(SPI mode, chapter 7). Limits used here, all generous against the specification:

    power-up wait before the first clocks     1 ms, then 80 clocks with CS high
    CMD0 attempts                             10
    response (NCR)                            up to 8 bytes after the command
    card ready (DO high) before a command     500 ms
    ACMD41 until the card leaves idle         2 s (the specification says 1 s)
    data token after CMD17/CMD9/CMD10         250 ms (the specification says 100 ms)
    attempts per sector read                  4; after a CRC error the clock is halved

Nothing here waits without a bound, so a missing, dead or unplugged card always ends in an
error with a sentence saying which step failed.
*/
#include <string.h>
#include "sd.h"
#include "port.h"
#include "sberr.h"

#define CMD0   0
#define CMD8   8
#define CMD9   9
#define CMD10 10
#define CMD16 16
#define CMD17 17
#define CMD55 55
#define CMD58 58
#define CMD59 59
#define ACMD41 41

#define R1_IDLE     0x01u
#define R1_ILLEGAL  0x04u
#define R1_CRC      0x08u
#define R1_ADDRESS  0x20u
#define R1_PARAM    0x40u
#define R1_NONE     0xFFu           /* no response within NCR */

#define TOKEN_START 0xFEu

#define POWERUP_US        1000u
#define READY_US        500000u
#define ACMD41_US      2000000u
#define TOKEN_US        250000u
#define READ_ATTEMPTS        4

enum { RD_OK = 0, RD_TIMEOUT, RD_R1, RD_TOKEN, RD_CRC, RD_GONE };

uint8_t sd_crc7(const uint8_t *p, unsigned n)
{
    uint8_t crc = 0;
    while (n--) {
        uint8_t d = *p++;
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if ((d ^ crc) & 0x80u)
                crc ^= 0x09u;
            d <<= 1;
        }
    }
    return crc & 0x7Fu;
}

static uint16_t crc16_table[256];
static int crc16_ready;

uint16_t sd_crc16(const uint8_t *p, unsigned n)
{
    if (!crc16_ready) {
        for (unsigned i = 0; i < 256; i++) {
            uint16_t c = (uint16_t)(i << 8);
            for (int b = 0; b < 8; b++)
                c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
            crc16_table[i] = c;
        }
        crc16_ready = 1;
    }
    uint16_t crc = 0;
    while (n--)
        crc = (uint16_t)((crc << 8) ^ crc16_table[((crc >> 8) ^ *p++) & 0xFFu]);
    return crc;
}

uint32_t sd_div_for(uint32_t sys_hz, uint32_t hz)
{
    if (hz == 0)
        return 255;
    uint32_t per = 2u * hz;
    uint32_t div = (sys_hz + per - 1u) / per;       /* sys / (2 (div + 1)) <= hz */
    div = div ? div - 1u : 0u;
    return div > 255u ? 255u : div;
}

uint32_t sd_div_hz(uint32_t sys_hz, uint32_t div)
{
    return sys_hz / (2u * (div + 1u));
}

const char *sd_type_name(int type)
{
    switch (type) {
    case SD_V1_SDSC: return "SDSC (SD 1.x)";
    case SD_V2_SDSC: return "SDSC (SD 2.0)";
    case SD_V2_SDHC: return "SDHC/SDXC";
    default: return "none";
    }
}

uint32_t sd_csd_sectors(const uint8_t csd[16])
{
    switch (csd[0] >> 6) {
    case 0: {                                       /* CSD 1.0: SDSC */
        uint32_t bl_len = csd[5] & 0x0Fu;
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        uint32_t mult = ((uint32_t)(csd[9] & 0x03u) << 1) | (csd[10] >> 7);
        if (bl_len < 9 || bl_len > 11)
            return 0;
        return (c_size + 1u) << (mult + 2u + bl_len - 9u);
    }
    case 1: {                                       /* CSD 2.0: SDHC/SDXC */
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        if (c_size >= 0x3FFFFFu)                    /* would not fit 32-bit sector numbers */
            return 0;
        return (c_size + 1u) * 1024u;
    }
    default:
        return 0;
    }
}

static void deselect(void)
{
    port_spi_select(0);
    port_spi_xfer(0xFF);                            /* 8 clocks so the card releases DO */
}

/* Asserts CS and waits for the card to drive DO high (not busy). */
static int select_card(void)
{
    port_spi_select(1);
    uint32_t t0 = port_now_us();
    for (;;) {
        if (port_spi_xfer(0xFF) == 0xFF)
            return 0;
        if ((uint32_t)(port_now_us() - t0) >= READY_US)
            return -1;
    }
}

/* Sends one command frame and returns R1 (R1_NONE if nothing came within NCR). */
static uint8_t send_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t f[6];
    f[0] = (uint8_t)(0x40u | cmd);
    f[1] = (uint8_t)(arg >> 24);
    f[2] = (uint8_t)(arg >> 16);
    f[3] = (uint8_t)(arg >> 8);
    f[4] = (uint8_t)arg;
    f[5] = (uint8_t)((sd_crc7(f, 5) << 1) | 1u);
    for (int i = 0; i < 6; i++)
        port_spi_xfer(f[i]);
    for (int i = 0; i < 9; i++) {
        uint8_t r = port_spi_xfer(0xFF);
        if (!(r & 0x80u))
            return r;
    }
    return R1_NONE;
}

/* A whole command transaction: select, command, optional extra response bytes, deselect. */
static uint8_t command(uint8_t cmd, uint32_t arg, uint8_t *extra, unsigned n_extra)
{
    if (select_card() != 0) {
        deselect();
        return R1_NONE;
    }
    uint8_t r = send_cmd(cmd, arg);
    if (extra && !(r & 0x80u) && !(r & R1_ILLEGAL))
        port_spi_recv(extra, n_extra);
    deselect();
    return r;
}

static uint8_t app_command(uint8_t cmd, uint32_t arg)
{
    uint8_t r = command(CMD55, 0, 0, 0);
    if (r & ~R1_IDLE)
        return r;
    return command(cmd, arg, 0, 0);
}

static uint8_t wait_token(void)
{
    uint32_t t0 = port_now_us();
    for (unsigned i = 0;; i++) {
        uint8_t t = port_spi_xfer(0xFF);
        if (t != 0xFF)
            return t;
        if ((i & 15u) == 15u && (uint32_t)(port_now_us() - t0) >= TOKEN_US)
            return 0xFF;
    }
}

/* Reads a data block of n bytes after cmd; *why gets a short reason on failure. */
static int read_data(struct sd_card *c, uint8_t cmd, uint32_t arg, uint8_t *buf, unsigned n,
                     uint8_t *detail)
{
    if (select_card() != 0) {
        deselect();
        return RD_TIMEOUT;
    }
    uint8_t r = send_cmd(cmd, arg);
    if (r != 0) {
        deselect();
        *detail = r;
        return r == R1_NONE ? RD_TIMEOUT : RD_R1;
    }
    uint8_t t = wait_token();
    if (t != TOKEN_START) {
        deselect();
        *detail = t;
        return t == 0xFF ? RD_TIMEOUT : RD_TOKEN;
    }
    uint8_t crc[2];
    port_spi_recv(buf, n);
    port_spi_recv(crc, 2);
    deselect();
    if (c->crc_on) {
        uint16_t want = (uint16_t)((crc[0] << 8) | crc[1]);
        if (sd_crc16(buf, n) != want)
            return RD_CRC;
    }
    return RD_OK;
}

int sd_init(struct sd_card *c, uint32_t fast_div)
{
    memset(c, 0, sizeof *c);
    if (!port_card_present())
        return sb_fail(SB_E_NO_CARD, "no card in the socket (the card-detect switch is open)");

    uint32_t sys_hz = port_sysinfo(SURF_SYSINFO_SYS_HZ);
    if (sys_hz == 0)
        sys_hz = SURF_SYS_HZ;
    c->init_div = sd_div_for(sys_hz, 400000u);
    c->fast_div = fast_div < c->init_div ? fast_div : c->init_div;
    c->div = c->init_div;
    port_spi_set_div(c->init_div);
    port_spi_select(0);

    uint32_t t0 = port_now_us();
    while ((uint32_t)(port_now_us() - t0) < POWERUP_US)
        ;
    for (int i = 0; i < 10; i++)                    /* 80 clocks with CS high */
        port_spi_xfer(0xFF);

    uint8_t r = R1_NONE;
    for (int i = 0; i < 10; i++) {
        port_spi_select(1);
        port_spi_xfer(0xFF);
        r = send_cmd(CMD0, 0);
        deselect();
        if (r == R1_IDLE)
            break;
        for (int k = 0; k < 2; k++)
            port_spi_xfer(0xFF);
    }
    if (r == R1_NONE)
        return sb_fail(SB_E_SD_NORESP, "no answer to CMD0 in 10 tries: no card, a card that is not "
                       "powered, or MISO not connected");
    if (r == 0x00)
        return sb_fail(SB_E_SD_NORESP, "CMD0 answered 0x00 instead of 0x01: MISO stuck low?");
    if (r != R1_IDLE)
        return sb_fail(SB_E_SD_INIT, "CMD0 (reset) answered 0x%02x instead of 0x01", r);

    uint8_t r7[4] = { 0, 0, 0, 0 };
    r = command(CMD8, 0x1AAu, r7, 4);
    int v1 = 0;
    if (r == R1_NONE)
        return sb_fail(SB_E_SD_INIT, "no answer to CMD8 (interface condition)");
    if (r & R1_ILLEGAL)
        v1 = 1;                                     /* SD 1.x (or MMC) */
    else if (r & ~R1_IDLE)
        return sb_fail(SB_E_SD_INIT, "CMD8 (interface condition) answered 0x%02x", r);
    else if (r7[3] != 0xAAu || (r7[2] & 0x0Fu) != 0x01u)
        return sb_fail(SB_E_SD_UNSUPPORTED, "CMD8 echo %02x %02x (expected 01 aa): the card does not "
                       "accept 2.7-3.6 V", r7[2], r7[3]);

    r = command(CMD59, 1, 0, 0);                    /* CRC checking on */
    c->crc_on = (r & ~R1_IDLE) == 0;

    uint8_t ocr[4];
    r = command(CMD58, 0, ocr, 4);
    if (!(r & 0x80u) && !(r & ~R1_IDLE)) {
        c->ocr = ((uint32_t)ocr[0] << 24) | ((uint32_t)ocr[1] << 16) | ((uint32_t)ocr[2] << 8) | ocr[3];
        if (!(c->ocr & 0x00300000u))                /* 3.2-3.3 V or 3.3-3.4 V */
            return sb_fail(SB_E_SD_UNSUPPORTED, "the card does not work at 3.3 V (OCR 0x%08x)", c->ocr);
    }

    t0 = port_now_us();
    for (;;) {
        r = app_command(ACMD41, v1 ? 0 : 0x40000000u);
        if (r == 0x00)
            break;
        if (r == R1_NONE)
            return sb_fail(SB_E_SD_INIT, "the card stopped answering during ACMD41");
        if (r & R1_ILLEGAL)
            return sb_fail(SB_E_SD_UNSUPPORTED, "ACMD41 is an illegal command for this card: an MMC "
                           "card? Only SD cards are supported");
        if (r != R1_IDLE)
            return sb_fail(SB_E_SD_INIT, "ACMD41 (initialize) answered 0x%02x", r);
        if ((uint32_t)(port_now_us() - t0) >= ACMD41_US)
            return sb_fail(SB_E_SD_INIT, "the card was still initializing after 2 s (ACMD41 kept "
                           "answering idle)");
        uint32_t t1 = port_now_us();
        while ((uint32_t)(port_now_us() - t1) < 1000u)
            ;
    }
    c->init_us = port_now_us() - t0;

    if (v1) {
        c->type = SD_V1_SDSC;
        c->block_addr = 0;
    } else {
        r = command(CMD58, 0, ocr, 4);
        if (r != 0)
            return sb_fail(SB_E_SD_INIT, "CMD58 (read OCR) after initialization answered 0x%02x", r);
        c->ocr = ((uint32_t)ocr[0] << 24) | ((uint32_t)ocr[1] << 16) | ((uint32_t)ocr[2] << 8) | ocr[3];
        if (!(c->ocr & 0x80000000u))
            return sb_fail(SB_E_SD_INIT, "the card reports power-up not finished (OCR 0x%08x)", c->ocr);
        c->block_addr = (c->ocr >> 30) & 1u;
        c->type = c->block_addr ? SD_V2_SDHC : SD_V2_SDSC;
    }
    if (!c->block_addr) {
        r = command(CMD16, SD_SECTOR, 0, 0);
        if (r != 0)
            return sb_fail(SB_E_SD_INIT, "CMD16 (512-byte blocks) answered 0x%02x", r);
    }

    c->div = c->fast_div;
    port_spi_set_div(c->div);

    /* The CSD gives the capacity, which bounds every later sector number. Its data CRC-16
     * is checked when CRCs are on; otherwise its own CRC-7 must match. */
    uint8_t detail = 0;
    if (read_data(c, CMD9, 0, c->csd, 16, &detail) == RD_OK) {
        uint8_t crc7 = (uint8_t)((sd_crc7(c->csd, 15) << 1) | 1u);
        if (c->crc_on || crc7 == c->csd[15]) {
            c->csd_ok = 1;
            c->sectors = sd_csd_sectors(c->csd);
        }
    }
    if (read_data(c, CMD10, 0, c->cid, 16, &detail) == RD_OK)
        c->cid_ok = 1;
    return SB_OK;
}

int sd_read(struct sd_card *c, uint32_t lba, void *buf)
{
    if (c->type == SD_NONE)
        return sb_fail(SB_E_SD_READ, "the card is not initialized");
    if (c->sectors && lba >= c->sectors)
        return sb_fail(SB_E_SD_READ, "sector %u is beyond the end of the card (%u sectors)", lba,
                       c->sectors);
    if (!c->block_addr && lba >= 0x00800000u)
        return sb_fail(SB_E_SD_READ, "sector %u is beyond the 4 GiB an SDSC card can address", lba);
    uint32_t addr = c->block_addr ? lba : lba * SD_SECTOR;
    int why = RD_OK;
    uint8_t detail = 0;
    for (int attempt = 0; attempt < READ_ATTEMPTS; attempt++) {
        if (!port_card_present()) {
            why = RD_GONE;
            break;
        }
        why = read_data(c, CMD17, addr, buf, SD_SECTOR, &detail);
        if (why == RD_OK) {
            c->reads++;
            return SB_OK;
        }
        c->retries++;
        if (why == RD_CRC) {
            c->crc_errors++;
            if (c->div < c->init_div) {             /* halve the clock and try again */
                c->div = c->div * 2u + 1u;
                if (c->div > c->init_div)
                    c->div = c->init_div;
                port_spi_set_div(c->div);
            }
        } else if (why == RD_TOKEN) {
            c->token_errors++;
        } else if (why == RD_TIMEOUT) {
            c->timeouts++;
        }
    }
    switch (why) {
    case RD_GONE:
        return sb_fail(SB_E_NO_CARD, "the card was removed (reading sector %u)", lba);
    case RD_TIMEOUT:
        return sb_fail(SB_E_SD_READ, "reading sector %u: the card did not answer or send data "
                       "within 250 ms (%d tries)", lba, READ_ATTEMPTS);
    case RD_R1:
        return sb_fail(SB_E_SD_READ, "reading sector %u: CMD17 answered 0x%02x (%s)", lba, detail,
                       (detail & R1_ADDRESS) ? "address error" : (detail & R1_PARAM) ? "parameter error"
                       : (detail & R1_CRC) ? "command CRC error" : "error");
    case RD_TOKEN:
        return sb_fail(SB_E_SD_READ, "reading sector %u: the card sent error token 0x%02x%s", lba,
                       detail, (detail & 0x08u) ? " (out of range)" : (detail & 0x04u) ? " (ECC failed)"
                       : (detail & 0x02u) ? " (card controller error)" : "");
    default:
        return sb_fail(SB_E_SD_READ, "reading sector %u: data CRC wrong %d times (clock now %u Hz)",
                       lba, READ_ATTEMPTS, sd_div_hz(port_sysinfo(SURF_SYSINFO_SYS_HZ), c->div));
    }
}
