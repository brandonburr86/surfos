/*
SurfOS host test: a simulated SD card in SPI mode
--------------------
File: sdmodel.c Date: 10/15/26 (SurfCPU)
--------------------
See sdmodel.h. Its CRCs are computed its own way (a bit-serial register for CRC7, a table
for CRC16), not with boards/arty_a7/sdproto.c, so the two check each other; the test
also checks both against the specification's examples.

10/16/26: write errors, commands without an answer, and a card taken out and put back (see
sdmodel.h).
*/

#include <string.h>
#include "sdmodel.h"

enum { M_CMD, M_WR_TOKEN, M_WR_DATA };

static uint8_t m_crc7(const uint8_t *d, int n) {
    uint8_t crc = 0;
    int i, b;
    for(i = 0; i < n; i++) {
        for(b = 7; b >= 0; b--) {
            int bit = ((d[i] >> b) & 1) ^ ((crc >> 6) & 1);
            crc = (uint8_t)((crc << 1) & 0x7F);
            if(bit) crc ^= 0x09;
        }
    }
    return crc;
}

static uint16_t crc16_table[256];

static uint16_t m_crc16(const uint8_t *d, int n) {
    uint16_t crc = 0;
    int i;
    if(!crc16_table[1]) {
        for(i = 0; i < 256; i++) {
            uint16_t c = (uint16_t)(i << 8);
            int b;
            for(b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
            crc16_table[i] = c;
        }
    }
    for(i = 0; i < n; i++) crc = (uint16_t)((crc << 8) ^ crc16_table[((crc >> 8) ^ d[i]) & 0xFF]);
    return crc;
}

static unsigned rnd(struct sdmodel *m) {
    m->seed = m->seed * 1103515245u + 12345u;
    return (m->seed >> 16) & 0x7FFF;
}

static void put(struct sdmodel *m, uint8_t b) {
    if(m->out_len < (int)sizeof(m->out)) m->out[m->out_len++] = b;
}

static void queue_reset(struct sdmodel *m) {
    m->out_len = m->out_pos = 0;
}

/* a response after NCR: 1 to 8 bytes of 0xFF */
static void respond(struct sdmodel *m, const uint8_t *r, int n) {
    int i, ncr = 1 + (int)(rnd(m) % 8);
    queue_reset(m);
    for(i = 0; i < ncr - 1; i++) put(m, 0xFF);
    for(i = 0; i < n; i++) put(m, r[i]);
}

static void r1(struct sdmodel *m, uint8_t v) {
    respond(m, &v, 1);
}

/* a data block after NAC: the token, the bytes, the CRC16 (wrong when asked) */
static void data_block(struct sdmodel *m, const uint8_t *d, int n) {
    int i, nac = (int)(rnd(m) % 20);
    uint16_t crc = m_crc16(d, n);
    if(m->no_token) return;
    for(i = 0; i < nac; i++) put(m, 0xFF);
    put(m, 0xFE);
    for(i = 0; i < n; i++) put(m, d[i]);
    if(m->corrupt_reads > 0) {
        m->corrupt_reads--;
        crc ^= 0x1234;
    }
    put(m, (uint8_t)(crc >> 8));
    put(m, (uint8_t)crc);
}

static void set_bits(uint8_t r[16], int msb, int lsb, uint32_t v) {
    int bit;
    for(bit = lsb; bit <= msb; bit++, v >>= 1) {
        int byte = 15 - bit / 8;
        if(v & 1) r[byte] |= (uint8_t)(1 << (bit % 8));
        else r[byte] &= (uint8_t)~(1 << (bit % 8));
    }
}

static void make_csd(struct sdmodel *m, uint8_t csd[16]) {
    memset(csd, 0, 16);
    if(m->high_capacity) {                          /* CSD 2.0: (C_SIZE + 1) x 512 KB */
        set_bits(csd, 127, 126, 1);
        set_bits(csd, 69, 48, m->blocks / 1024 - 1);
    } else {                                        /* CSD 1.0: READ_BL_LEN 9, C_SIZE_MULT 7: (C_SIZE + 1) x 512 blocks */
        set_bits(csd, 127, 126, 0);
        set_bits(csd, 83, 80, 9);
        set_bits(csd, 49, 47, 7);
        set_bits(csd, 73, 62, m->blocks / 512 - 1);
    }
    set_bits(csd, 103, 96, 0x32);                   /* TRAN_SPEED 25 MHz */
    csd[15] = (uint8_t)((m_crc7(csd, 15) << 1) | 1);
}

static void make_cid(uint8_t cid[16]) {
    static const uint8_t c[15] = { 0x03, 'S', 'D', 'S', 'U', 'R', 'F', '1', 0x10, 0x12, 0x34, 0x56, 0x78, 0x01, 0xAA };
    memcpy(cid, c, 15);
    cid[15] = (uint8_t)((m_crc7(cid, 15) << 1) | 1);
}

void sdmodel_init(struct sdmodel *m, uint8_t *img, uint32_t blocks, int v2, int high_capacity, unsigned seed) {
    memset(m, 0, sizeof(*m));
    m->img = img;
    m->blocks = blocks;
    m->present = 1;
    m->v2 = v2;
    m->high_capacity = high_capacity;
    m->seed = seed;
    m->idle = 1;
    m->acmd41_left = 3;
}

/* out or back in: either way the card lost its power and starts over, in SD mode */
void sdmodel_set_present(struct sdmodel *m, int present) {
    if(!present && m->present) m->pulls++;
    m->present = present;
    m->spi = 0;
    m->idle = 1;
    m->crc_on = 0;
    m->app = 0;
    m->acmd41_left = 3;
    m->mode = M_CMD;
    m->multi = 0;
    m->cmd_len = 0;
    m->busy = 0;
    m->status2 = 0;
    m->pull_after = 0;
    queue_reset(m);
}

void sdmodel_select(struct sdmodel *m, int on) {
    if(!on && m->selected && m->mode == M_WR_DATA) m->violations++;    /* CS released inside a block */
    m->selected = on;
    m->resp_end = 0;                                /* a CS change separates commands as well as 8 clocks do */
}

/* the block an address names: 0x00, or the R1 error: 0x20 a misaligned SDSC address, 0x40 past the end */
static uint8_t address(struct sdmodel *m, uint32_t arg, uint32_t *block) {
    if(m->high_capacity) *block = arg;
    else {
        if(arg % 512) return 0x20;
        *block = arg / 512;
    }
    return *block < m->blocks ? 0x00 : 0x40;
}

static void command(struct sdmodel *m) {
    uint8_t cmd = m->cmd[0] & 0x3F, r[5], reg[16];
    uint32_t arg = ((uint32_t)m->cmd[1] << 24) | ((uint32_t)m->cmd[2] << 16) | ((uint32_t)m->cmd[3] << 8) | m->cmd[4];
    int app = m->app;
    uint8_t idle = m->idle ? 0x01 : 0x00;
    m->commands++;
    m->app = 0;
    if(m->mute > 0) {                               /* no response: MISO stays high */
        m->mute--;
        queue_reset(m);
        return;
    }
    if(!m->spi) {                                   /* SD mode: only CMD0 (CS is low: it is selected) counts */
        if(cmd != 0 || m->cmd[5] != (uint8_t)((m_crc7(m->cmd, 5) << 1) | 1)) { queue_reset(m); return; }
        m->spi = 1;
    }
    if((cmd == 0 || cmd == 8 || m->crc_on) && m->cmd[5] != (uint8_t)((m_crc7(m->cmd, 5) << 1) | 1)) {
        m->crc_errors_seen++;
        r1(m, idle | 0x08);                         /* command CRC error */
        return;
    }
    if(m->idle && !(cmd == 0 || cmd == 8 || cmd == 55 || cmd == 58 || cmd == 59 || (app && cmd == 41))) {
        r1(m, 0x05);                                /* illegal before initialization */
        return;
    }
    if(app && cmd == 41) {
        if(m->acmd41_left > 0) m->acmd41_left--;
        if(m->acmd41_left == 0 && (!m->high_capacity || (arg & 0x40000000u))) m->idle = 0;   /* SDHC needs HCS */
        r1(m, m->idle ? 0x01 : 0x00);
        return;
    }
    switch(cmd) {
    case 0:
        m->idle = 1;
        m->crc_on = 0;
        m->acmd41_left = 3;
        m->multi = 0;
        m->mode = M_CMD;
        r1(m, 0x01);
        return;
    case 8:
        if(!m->v2) { r1(m, idle | 0x04); return; }
        r[0] = idle; r[1] = 0; r[2] = 0; r[3] = (uint8_t)((arg >> 8) & 0x0F); r[4] = (uint8_t)arg;
        respond(m, r, 5);
        return;
    case 55:
        m->app = 1;
        r1(m, idle);
        return;
    case 58: {
        uint32_t ocr = 0x00FF8000u | (m->idle ? 0 : 0x80000000u) | ((!m->idle && m->high_capacity) ? 0x40000000u : 0);
        r[0] = idle; r[1] = (uint8_t)(ocr >> 24); r[2] = (uint8_t)(ocr >> 16); r[3] = (uint8_t)(ocr >> 8); r[4] = (uint8_t)ocr;
        respond(m, r, 5);
        return;
    }
    case 59:
        m->crc_on = arg & 1;
        r1(m, idle);
        return;
    case 16:
        r1(m, arg == 512 ? 0x00 : 0x40);
        return;
    case 9:
    case 10:
        if(cmd == 9) make_csd(m, reg); else make_cid(reg);
        r1(m, 0x00);
        data_block(m, reg, 16);
        return;
    case 13:
        r[0] = 0; r[1] = m->status2;
        m->status2 = 0;
        respond(m, r, 2);
        return;
    case 17:
    case 18:
        if(cmd == 18 && m->no_multi) break;
        if((r[0] = address(m, arg, &m->block))) { r1(m, r[0]); return; }
        r1(m, 0x00);
        data_block(m, m->img + (size_t)m->block * 512, 512);
        m->block++;
        m->multi = cmd == 18;
        return;
    case 24:
    case 25:
        if(cmd == 25 && m->no_multi) break;
        if((r[0] = address(m, arg, &m->block))) { r1(m, r[0]); return; }
        r1(m, 0x00);
        m->multi = cmd == 25;
        m->mode = M_WR_TOKEN;
        return;
    case 12:
        r1(m, 0x00);
        return;
    }
    r1(m, idle | 0x04);
}

uint8_t sdmodel_xfer(struct sdmodel *m, uint8_t in) {
    uint8_t out = 0xFF;
    int after_resp;
    m->bytes++;
    if(!m->present || !m->selected) return 0xFF;
    after_resp = m->resp_end;                       /* the previous byte out was a response's last */
    m->resp_end = 0;
    /* MISO: what is queued, then busy, then the next block of CMD18 */
    if(m->out_pos < m->out_len) {
        out = m->out[m->out_pos++];
        if(m->out_pos == m->out_len && m->mode == M_CMD && !m->multi && m->busy == 0) m->resp_end = 1;
    }
    else if(m->busy > 0) { m->busy--; out = 0x00; }
    else if(m->multi && m->mode == M_CMD && !m->no_token) {
        queue_reset(m);
        if(m->block < m->blocks) data_block(m, m->img + (size_t)m->block * 512, 512);
        m->block++;
        if(m->out_pos < m->out_len) out = m->out[m->out_pos++];
    }
    /* MOSI */
    switch(m->mode) {
    case M_CMD:
        if(m->cmd_len == 0 && (in & 0xC0) != 0x40) break;
        if(m->cmd_len == 0 && after_resp) {         /* N_RC: a real card needs 8 clocks after its response */
            m->violations++;
            m->nrc_violations++;
        }
        if(m->cmd_len == 0 && out == 0x00 && m->busy >= 0 && m->out_pos >= m->out_len && !m->multi) {
            m->violations++;                /* a command while the card holds MISO low (busy): ignored */
            break;
        }
        m->cmd[m->cmd_len++] = in;
        if(m->cmd_len == 6) {
            m->cmd_len = 0;
            if(m->multi && (m->cmd[0] & 0x3F) == 12) {  /* STOP_TRANSMISSION in the middle of CMD18 */
                uint8_t stop[2] = { 0xFF, 0x00 };
                m->multi = 0;
                queue_reset(m);
                respond(m, stop, 2);
                m->busy = (int)(rnd(m) % 4);
            } else {
                command(m);
            }
        }
        break;
    case M_WR_TOKEN:
        if(in == 0xFE || (m->multi && in == 0xFC)) {
            m->mode = M_WR_DATA;
            m->wpos = 0;
        } else if(m->multi && in == 0xFD) {        /* the end of CMD25 */
            m->multi = 0;
            m->mode = M_CMD;
            m->busy = 2 + (int)(rnd(m) % 8);
        }
        break;
    case M_WR_DATA:
        m->wbuf[m->wpos++] = in;
        if(m->wpos == 514) {
            uint16_t want = m_crc16(m->wbuf, 512), got = (uint16_t)((m->wbuf[512] << 8) | m->wbuf[513]);
            queue_reset(m);
            if((m->crc_on && got != want) || m->reject_writes > 0) {
                if(m->reject_writes > 0) m->reject_writes--;
                put(m, 0xEB);                       /* data rejected: CRC error */
                m->busy = 1 + (int)(rnd(m) % 4);
                m->mode = m->multi ? M_WR_TOKEN : M_CMD;   /* CMD25 waits for the stop token */
            } else if(m->block >= m->blocks || m->write_errors > 0) {
                if(m->block < m->blocks) { m->write_errors--; m->write_errors_sent++; }
                put(m, 0xED);                       /* write error: past the end, or asked for */
                m->status2 |= 0x04;                 /* and CMD13 says so (as the io track's model) */
                m->busy = 1 + (int)(rnd(m) % 4);
                m->mode = m->multi ? M_WR_TOKEN : M_CMD;
            } else {
                memcpy(m->img + (size_t)m->block * 512, m->wbuf, 512);
                m->block++;
                m->blocks_written++;
                put(m, 0xE5);                       /* accepted */
                m->busy = 2 + (int)(rnd(m) % 30);
                m->mode = m->multi ? M_WR_TOKEN : M_CMD;
                if(m->pull_after > 0 && --m->pull_after == 0) sdmodel_set_present(m, 0);   /* out, mid-transfer */
            }
        }
        break;
    }
    return out;
}
