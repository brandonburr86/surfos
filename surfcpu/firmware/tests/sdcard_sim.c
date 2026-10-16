/*
SurfCPU firmware tests: a microSD card in SPI mode
--------------------
File: sdcard_sim.c Date: 10/15/26 (SurfCPU)
--------------------
See sdcard_sim.h. One call per byte exchanged: the byte the card shifts out comes from a
queue filled when a command completes (after NCR fill bytes), and the byte shifted in is
collected into the next command frame.
*/
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sdcard_sim.h"

#define R1_IDLE    0x01
#define R1_ILLEGAL 0x04
#define R1_CRC     0x08
#define R1_ADDRESS 0x20
#define R1_PARAM   0x40

struct sdsim {
    struct sdsim_opts o;
    int fd;
    uint8_t *img;
    uint64_t size;
    uint32_t sectors;
    int inserted;
    uint32_t div;
    uint32_t powerup_clocks;
    int spi_mode, idle, app, crc_on, acmd41_seen;
    uint8_t cmd[6];
    int cmd_len;
    uint8_t out[1100];
    int out_head, out_len;
    int prev_cs;
    uint32_t reads;
    uint8_t csd[16], cid[16];
    struct sdsim_stats st;
};

/* CRC-7 (x^7 + x^3 + 1), computed bit by bit from the MSB. */
static uint8_t crc7(const uint8_t *p, int n)
{
    unsigned reg = 0;
    for (int i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            unsigned in = (p[i] >> b) & 1u;
            unsigned top = (reg >> 6) & 1u;
            reg = (reg << 1) & 0x7Fu;
            if (in ^ top)
                reg ^= 0x09u;
        }
    return (uint8_t)reg;
}

/* CRC-16-CCITT (x^16 + x^12 + x^5 + 1), initial value 0, bit by bit. */
static uint16_t crc16(const uint8_t *p, int n)
{
    unsigned reg = 0;
    for (int i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            unsigned in = (p[i] >> b) & 1u;
            unsigned top = (reg >> 15) & 1u;
            reg = (reg << 1) & 0xFFFFu;
            if (in ^ top)
                reg ^= 0x1021u;
        }
    return (uint16_t)reg;
}

void sdsim_default_opts(struct sdsim_opts *o)
{
    memset(o, 0, sizeof *o);
    o->type = SDSIM_SDHC;
    o->acmd41_busy = 3;
    o->ncr = 1;
    o->nac = 4;
}

static void build_registers(struct sdsim *c)
{
    memset(c->csd, 0, 16);
    if (c->o.type == SDSIM_SDHC) {
        uint32_t csize = c->sectors / 1024u - 1u;
        c->csd[0] = 0x40;                           /* CSD_STRUCTURE 1 */
        c->csd[1] = 0x0E;
        c->csd[3] = 0x32;                           /* 25 MHz */
        c->csd[4] = 0x5B;
        c->csd[5] = 0x59;                           /* CCC, READ_BL_LEN 9 */
        c->csd[7] = (uint8_t)((csize >> 16) & 0x3F);
        c->csd[8] = (uint8_t)(csize >> 8);
        c->csd[9] = (uint8_t)csize;
        c->csd[10] = 0x7F;
        c->csd[11] = 0x80;
        c->csd[12] = 0x0A;
        c->csd[13] = 0x40;
    } else {                                        /* CSD 1.0: capacity = (C_SIZE+1) * 2^(MULT+2) * 2^BL */
        uint32_t bl = 9, mult = 0, csize = 0;
        int found = 0;
        for (bl = 9; bl <= 11 && !found; bl++)
            for (mult = 0; mult <= 7 && !found; mult++) {
                uint32_t shift = mult + 2 + bl - 9;
                if ((c->sectors >> shift) <= 4096u && (c->sectors >> shift) > 0) {
                    csize = (c->sectors >> shift) - 1u;
                    found = 1;
                }
            }
        bl--;
        mult--;
        c->csd[0] = 0x00;
        c->csd[1] = 0x26;
        c->csd[3] = 0x32;
        c->csd[4] = 0x5F;
        c->csd[5] = (uint8_t)(0x50 | bl);
        c->csd[6] = (uint8_t)(0x80 | ((csize >> 10) & 0x03));
        c->csd[7] = (uint8_t)(csize >> 2);
        c->csd[8] = (uint8_t)(((csize & 3u) << 6) | 0x2D);
        c->csd[9] = (uint8_t)(0xB4 | ((mult >> 1) & 0x03));
        c->csd[10] = (uint8_t)(((mult & 1u) << 7) | 0x7F);
        c->csd[11] = 0x80;
        c->csd[12] = 0x0A;
        c->csd[13] = 0x40;
    }
    c->csd[15] = (uint8_t)((crc7(c->csd, 15) << 1) | 1u);
    static const uint8_t cid[15] = { 0x03, 'S', 'D', 'S', 'U', 'R', 'F', '1', 0x10,
                                     0x12, 0x34, 0x56, 0x78, 0x01, 0xAA };
    memcpy(c->cid, cid, 15);
    c->cid[15] = (uint8_t)((crc7(c->cid, 15) << 1) | 1u);
}

struct sdsim *sdsim_open(const char *image, const struct sdsim_opts *o)
{
    struct sdsim *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->o = *o;
    if (c->o.ncr < 1) c->o.ncr = 1;
    if (c->o.ncr > 8) c->o.ncr = 8;
    c->fd = -1;
    c->inserted = !o->absent;
    if (image) {
        c->fd = open(image, O_RDONLY);
        struct stat sb;
        if (c->fd < 0 || fstat(c->fd, &sb) != 0) {
            perror(image);
            free(c);
            return NULL;
        }
        c->size = (uint64_t)sb.st_size & ~511ull;
        c->sectors = (uint32_t)(c->size / 512u);
        if (c->size) {
            c->img = mmap(NULL, c->size, PROT_READ, MAP_PRIVATE, c->fd, 0);
            if (c->img == MAP_FAILED) {
                perror("mmap");
                close(c->fd);
                free(c);
                return NULL;
            }
        }
    } else {
        c->inserted = 0;
    }
    build_registers(c);
    return c;
}

struct sdsim *sdsim_open_mem(uint8_t *buf, uint64_t size, const struct sdsim_opts *o)
{
    struct sdsim *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->o = *o;
    if (c->o.ncr < 1) c->o.ncr = 1;
    if (c->o.ncr > 8) c->o.ncr = 8;
    c->fd = -2;                                     /* memory: nothing to unmap */
    c->img = buf;
    c->size = size & ~511ull;
    c->sectors = (uint32_t)(c->size / 512u);
    c->inserted = !o->absent;
    build_registers(c);
    return c;
}

void sdsim_close(struct sdsim *c)
{
    if (!c)
        return;
    if (c->img && c->fd != -2)
        munmap(c->img, c->size);
    if (c->fd >= 0)
        close(c->fd);
    free(c);
}

int sdsim_present(struct sdsim *c) { return c->o.no_cd ? 1 : c->inserted; }
void sdsim_set_div(struct sdsim *c, uint32_t div) { c->div = div; }
uint32_t sdsim_sectors(struct sdsim *c) { return c->sectors; }
const struct sdsim_stats *sdsim_stats(struct sdsim *c) { return &c->st; }
const uint8_t *sdsim_image(struct sdsim *c) { return c->img; }

void sdsim_set_inserted(struct sdsim *c, int in)
{
    c->inserted = in && c->img;
    if (in) {                                       /* a freshly inserted card is powered up anew */
        c->spi_mode = 0;
        c->powerup_clocks = 0;
        c->out_len = c->cmd_len = 0;
    }
}

static void put(struct sdsim *c, uint8_t b)
{
    if (c->out_head + c->out_len < (int)sizeof c->out)
        c->out[c->out_head + c->out_len++] = b;
}

static void respond(struct sdsim *c, const uint8_t *bytes, int n)
{
    c->out_head = 0;
    c->out_len = 0;
    for (int i = 0; i < c->o.ncr; i++)
        put(c, 0xFF);
    for (int i = 0; i < n; i++)
        put(c, bytes[i]);
}

static void respond_r1(struct sdsim *c, uint8_t r1) { respond(c, &r1, 1); }

static void data_block(struct sdsim *c, const uint8_t *data, int n, int bad_crc)
{
    for (int i = 0; i < c->o.nac; i++)
        put(c, 0xFF);
    put(c, 0xFE);
    for (int i = 0; i < n; i++)
        put(c, data[i]);
    uint16_t crc = crc16(data, n);
    if (bad_crc)
        crc ^= 0x0100;
    put(c, (uint8_t)(crc >> 8));
    put(c, (uint8_t)crc);
}

static void read_block(struct sdsim *c, uint32_t arg, uint8_t r1)
{
    c->st.cmd17++;
    uint64_t addr = c->o.type == SDSIM_SDHC ? (uint64_t)arg * 512u : arg;
    if (c->o.type != SDSIM_SDHC && (arg % 512u)) {
        respond_r1(c, r1 | R1_ADDRESS);
        return;
    }
    if (c->o.remove_after && c->reads >= (uint32_t)c->o.remove_after) {
        c->inserted = 0;                            /* pulled out: nothing answers any more */
        c->out_len = 0;
        return;
    }
    c->reads++;
    respond_r1(c, r1);
    if (c->o.read_timeouts > 0) {
        c->o.read_timeouts--;
        return;                                     /* never a token */
    }
    if (c->o.token_errors > 0 || addr + 512u > c->size) {
        if (c->o.token_errors > 0)
            c->o.token_errors--;
        for (int i = 0; i < c->o.nac; i++)
            put(c, 0xFF);
        put(c, 0x08);                               /* data error token: out of range */
        return;
    }
    uint8_t block[512];
    memcpy(block, c->img + addr, 512);
    int bad = 0;
    if (c->o.crc_errors > 0) {
        c->o.crc_errors--;
        bad = 1;
    } else if (c->o.crc_every && c->reads % (uint32_t)c->o.crc_every == 0) {
        bad = 1;
    }
    if (c->o.min_div && c->div < (uint32_t)c->o.min_div) {
        block[c->reads % 512u] ^= 0x10;             /* too fast for this card: garbled data */
        data_block(c, block, 512, 0);
        block[c->reads % 512u] ^= 0x10;
        uint16_t good = crc16(block, 512);          /* ...with the CRC of the right data */
        c->out[c->out_head + c->out_len - 2] = (uint8_t)(good >> 8);
        c->out[c->out_head + c->out_len - 1] = (uint8_t)good;
        c->st.crc_injected++;
        return;
    }
    if (bad)
        c->st.crc_injected++;
    data_block(c, block, 512, bad);
}

static void command(struct sdsim *c)
{
    uint8_t cmd = c->cmd[0] & 0x3F;
    uint32_t arg = ((uint32_t)c->cmd[1] << 24) | ((uint32_t)c->cmd[2] << 16) | ((uint32_t)c->cmd[3] << 8) | c->cmd[4];
    int crc_ok = (uint8_t)((crc7(c->cmd, 5) << 1) | 1u) == c->cmd[5];
    c->st.cmds++;
    c->st.last_cmd = cmd;
    if (!crc_ok)
        c->st.bad_cmd_crc++;
    if (!c->spi_mode) {
        /* SD mode: only CMD0 with CS low (and a valid CRC, after the power-up clocks) enters SPI mode */
        if (cmd != 0 || !crc_ok || (!c->o.no_powerup_check && c->powerup_clocks < 74))
            return;
        c->spi_mode = 1;
        c->idle = 1;
        c->crc_on = 0;
        c->acmd41_seen = 0;
        c->st.cmd0++;
        respond_r1(c, R1_IDLE);
        return;
    }
    int app = c->app;
    c->app = 0;
    uint8_t r1 = c->idle ? R1_IDLE : 0;
    if ((cmd == 0 || cmd == 8 || c->crc_on) && !crc_ok) {
        respond_r1(c, r1 | R1_CRC);
        return;
    }
    if (app && cmd == 41) {
        c->st.acmd41++;
        if (c->o.type == SDSIM_MMC) {
            respond_r1(c, r1 | R1_ILLEGAL);
            return;
        }
        int hcs = (arg >> 30) & 1;
        if (c->o.acmd41_busy < 0 || c->acmd41_seen++ < c->o.acmd41_busy ||
            (c->o.type == SDSIM_SDHC && !hcs)) {    /* an SDHC card stays busy without HCS */
            respond_r1(c, R1_IDLE);
            return;
        }
        c->idle = 0;
        respond_r1(c, 0);
        return;
    }
    if (app) {
        respond_r1(c, r1 | R1_ILLEGAL);
        return;
    }
    switch (cmd) {
    case 0:
        c->st.cmd0++;
        c->idle = 1;
        c->crc_on = 0;
        c->acmd41_seen = 0;
        respond_r1(c, R1_IDLE);
        return;
    case 8: {
        c->st.cmd8++;
        if (c->o.type == SDSIM_SDSC1 || c->o.type == SDSIM_MMC) {
            respond_r1(c, r1 | R1_ILLEGAL);
            return;
        }
        uint8_t r7[5] = { r1, 0x00, 0x00, (uint8_t)((arg >> 8) & 0x0F),
                          (uint8_t)(c->o.cmd8_bad_echo ? (arg & 0xFF) ^ 0x55 : (arg & 0xFF)) };
        respond(c, r7, 5);
        return;
    }
    case 55:
        c->app = 1;
        respond_r1(c, r1);
        return;
    case 58: {
        uint32_t ocr = c->o.no_33v ? 0x00000080u : 0x00FF8000u;
        if (!c->idle) {
            ocr |= 0x80000000u;
            if (c->o.type == SDSIM_SDHC)
                ocr |= 0x40000000u;
        }
        uint8_t r3[5] = { r1, (uint8_t)(ocr >> 24), (uint8_t)(ocr >> 16), (uint8_t)(ocr >> 8), (uint8_t)ocr };
        respond(c, r3, 5);
        return;
    }
    case 59:
        if (c->o.no_cmd59) {
            respond_r1(c, r1 | R1_ILLEGAL);
            return;
        }
        c->crc_on = arg & 1u;
        respond_r1(c, r1);
        return;
    case 16:
        if (c->o.type == SDSIM_SDHC ? arg != 512u : (arg == 0 || arg > 512u)) {
            respond_r1(c, r1 | R1_PARAM);
            return;
        }
        respond_r1(c, r1);
        return;
    case 9:
    case 10:
        if (c->idle) {
            respond_r1(c, r1 | R1_ILLEGAL);
            return;
        }
        c->st.cmd9++;
        respond_r1(c, r1);
        data_block(c, cmd == 9 ? c->csd : c->cid, 16, 0);
        return;
    case 13: {
        uint8_t r2[2] = { r1, 0 };
        respond(c, r2, 2);
        return;
    }
    case 17:
        if (c->idle) {
            respond_r1(c, r1 | R1_ILLEGAL);
            return;
        }
        read_block(c, arg, r1);
        return;
    default:
        respond_r1(c, r1 | R1_ILLEGAL);
        return;
    }
}

uint8_t sdsim_xfer(struct sdsim *c, int cs, uint8_t mosi)
{
    c->st.bytes++;
    if (!c->inserted)
        return 0xFF;                                /* empty socket: MISO pulled up */
    if (c->o.miso_low)
        return 0x00;
    if (!cs) {
        if (c->prev_cs) {                           /* CS released: the card stops talking */
            c->out_len = 0;
            c->cmd_len = 0;
        }
        c->prev_cs = 0;
        if (c->powerup_clocks < 1000000u)
            c->powerup_clocks += 8;
        return 0xFF;
    }
    c->prev_cs = 1;
    if (c->o.dead)
        return 0xFF;
    uint8_t miso = 0xFF;
    if (c->out_len) {
        miso = c->out[c->out_head++];
        c->out_len--;
    }
    if (c->cmd_len == 0) {
        if ((mosi & 0xC0) == 0x40)
            c->cmd[c->cmd_len++] = mosi;
    } else {
        c->cmd[c->cmd_len++] = mosi;
        if (c->cmd_len == 6) {
            c->cmd_len = 0;
            command(c);
        }
    }
    return miso;
}
