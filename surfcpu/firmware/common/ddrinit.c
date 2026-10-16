/*
SurfCPU: DDR3L initialization and read leveling for LiteDRAM, run by SurfCPU
--------------------
File: ddrinit.c Date: 10/16/26 (SurfCPU)
--------------------
LiteX's liblitedram (sdram.c: sdram_init, the generated init_sequence, sdram_read_leveling,
sdram_leveling_center_module, sdram_write_read_check_test_pattern; accessors.c; libbase's
lfsr.h), as LiteX 2026.08 (b6ae9e0b) runs it on the calibration CPU it puts inside a
LiteDRAM core, ported to SurfCPU: the same steps and the same choices (the exhaustive read
leveling LiteDRAM's BIOS used on the Arty), reaching the controller through the ddrc
registers (spec/soc.toml) instead of LiteX's CSR accessors, with microsecond waits instead of
cdelay() loops, with the three test patterns computed once instead of for every probe (the
same bytes: LiteX's 128 LFSR steps per probe cost SurfCPU at 25 MHz about as much as the
probe's register accesses), and with its console output shortened. What it does and why is in
ddrinit.h and docs/DDR.md section 10. Those parts of LiteX carry this notice:

    LiteX is copyright (C) 2012-2025 Enjoy-Digital & LiteX developers; MiSoC is copyright
    (C) 2012-2015 Enjoy-Digital and (C) 2007-2015 M-Labs Ltd. sdram.c is copyright (c)
    2013-2020 Florent Kermarrec, 2013-2014 Sebastien Bourdeauducq, 2018 Chris Ballance, 2018
    Dolu1990, 2019 Gabriel L. Somlo, 2018 Jean-Francois Nguyen, 2018 Sergiusz Bazanski, 2018
    Tim 'mithro' Ansell, 2021 Antmicro; lfsr.h is copyright (C) 2020 Anton Blanchard, IBM.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without modification, are
    permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice, this list of
       conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice, this list
       of conditions and the following disclaimer in the documentation and/or other
       materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
    THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
    OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "ddrinit.h"
#include "surfcpu/soc.h"

#define BYTES_PER_PHASE  (DDRINIT_DFI_DATABITS / 8)              /* DFII_PIX_DATA_BYTES */
#define MODULE_MASK      ((1u << DDRINIT_DQ_DQS_RATIO) - 1u)
#define SEEDS            3
#define MAX_ERRORS       (8 * DDRINIT_PHASES * BYTES_PER_PHASE / DDRINIT_MODULES)

#define CTRL_SOFTWARE    (SURF_DDRC_DFII_CONTROL_CKE_MASK | SURF_DDRC_DFII_CONTROL_ODT_MASK | \
                          SURF_DDRC_DFII_CONTROL_RESET_N_MASK)
#define CTRL_HARDWARE    SURF_DDRC_DFII_CONTROL_SEL_MASK
#define CMD_CS           SURF_DDRC_DFII_P0_COMMAND_CS_MASK
#define CMD_WE           SURF_DDRC_DFII_P0_COMMAND_WE_MASK
#define CMD_CAS          SURF_DDRC_DFII_P0_COMMAND_CAS_MASK
#define CMD_RAS          SURF_DDRC_DFII_P0_COMMAND_RAS_MASK
#define CMD_WRDATA       SURF_DDRC_DFII_P0_COMMAND_WRDATA_MASK
#define CMD_RDDATA       SURF_DDRC_DFII_P0_COMMAND_RDDATA_MASK

/* phase p's injector registers: the phases are 0x18 bytes apart */
#define PI_STRIDE        (SURF_DDRC_DFII_P1_COMMAND - SURF_DDRC_DFII_P0_COMMAND)
#define PI(reg, p)       (SURF_DDRC_DFII_P0_##reg + (uint32_t)(p) * PI_STRIDE)

static const struct ddrinit_ops *ops;

static void wr(uint32_t off, uint32_t v) { ops->wr(off, v); }

static void wait_us(uint32_t us)
{
    uint32_t t0 = ops->now_us();
    while ((uint32_t)(ops->now_us() - t0) < us)
        ;
}

static void out(const char *s)
{
    if (ops->out)
        ops->out(s);
}

/* small formatting without the C library: decimal, at least `width` digits */
static char *put_dec(char *p, int v, int width)
{
    char t[12];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < width) t[n++] = '0';
    while (n) *p++ = t[--n];
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

/* ---------------------------------------------------------------- injectors (sdram_phy.h) */
static void command(int phase, uint32_t cmd)
{
    wr(PI(COMMAND, phase), cmd);
    wr(PI(COMMAND_ISSUE, phase), 1);
}

static void mode_register(uint32_t reg, uint32_t value)
{
    wr(SURF_DDRC_DFII_P0_ADDRESS, value);
    wr(SURF_DDRC_DFII_P0_BADDRESS, reg);
    command(0, CMD_RAS | CMD_CAS | CMD_WE | CMD_CS);
}

/* init_sequence() as the generator writes it for ip/litedram/arty.yml (ddrinit_phy.h) */
static void init_sequence(void)
{
    wr(SURF_DDRC_DFII_P0_ADDRESS, 0);                /* release reset; CKE low */
    wr(SURF_DDRC_DFII_P0_BADDRESS, 0);
    wr(SURF_DDRC_DFII_CONTROL, SURF_DDRC_DFII_CONTROL_ODT_MASK | SURF_DDRC_DFII_CONTROL_RESET_N_MASK);
    wait_us(1000);                                   /* JEDEC: 500 us before CKE */
    wr(SURF_DDRC_DFII_CONTROL, CTRL_SOFTWARE);       /* bring CKE high */
    wait_us(200);                                    /* tXPR */
    mode_register(2, DDRINIT_MR2);
    mode_register(3, DDRINIT_MR3);
    mode_register(1, DDRINIT_MR1);
    mode_register(0, DDRINIT_MR0);
    wait_us(20);                                     /* tMOD, the DLL's reset (tDLLK) */
    wr(SURF_DDRC_DFII_P0_ADDRESS, DDRINIT_ZQCL);     /* ZQ calibration long */
    wr(SURF_DDRC_DFII_P0_BADDRESS, 0);
    command(0, CMD_WE | CMD_CS);
    wait_us(20);                                     /* tZQinit */
}

/* ---------------------------------------------------------------- the PHY's lanes (accessors.c) */
static void lane_action(int module, uint32_t reg)
{
    wr(SURF_DDRC_PHY_DLY_SEL, 1u << module);         /* sdram_select */
    wr(reg, 1);
    wr(SURF_DDRC_PHY_DLY_SEL, 0);                    /* sdram_deselect */
}

static int cur_delay[DDRINIT_MODULES];

static void rst_delay(int m)   { cur_delay[m] = 0; lane_action(m, SURF_DDRC_PHY_RDLY_DQ_RST); }
static void inc_delay(int m)   { cur_delay[m] = (cur_delay[m] + 1) & (DDRINIT_DELAYS - 1);
                                 lane_action(m, SURF_DDRC_PHY_RDLY_DQ_INC); }
static void rst_bitslip(int m) { lane_action(m, SURF_DDRC_PHY_RDLY_DQ_BITSLIP_RST); }
static void inc_bitslip(int m) { lane_action(m, SURF_DDRC_PHY_RDLY_DQ_BITSLIP); }

/* ---------------------------------------------------------------- the probe (sdram.c) */
static uint32_t lfsr32(uint32_t prev)                /* libbase lfsr(32, prev) */
{
    uint32_t lsb = prev & 1u;
    prev >>= 1;
    prev ^= (0u - lsb) & 0x80200003u;
    return prev;
}

static unsigned popcount8(uint32_t x)
{
    unsigned n = 0;
    for (x &= 0xFFu; x; x &= x - 1u)
        n++;
    return n;
}

/* The pseudo-random burst for each seed, as sdram_write_read_check_test_pattern makes it:
 * byte i of phase p takes the low bits of the next eight LFSR steps, first step in bit 0. */
static const uint32_t seeds[SEEDS] = {42, 84, 36};
static uint8_t patterns[SEEDS][DDRINIT_PHASES][BYTES_PER_PHASE];

static void make_patterns(void)
{
    for (int s = 0; s < SEEDS; s++) {
        uint32_t prv = seeds[s];
        for (int p = 0; p < DDRINIT_PHASES; p++)
            for (int i = 0; i < BYTES_PER_PHASE; i++) {
                uint8_t v = 0;
                for (int b = 0; b < 8; b++) {
                    prv = lfsr32(prv);
                    v |= (uint8_t)((prv & 1u) << b);
                }
                patterns[s][p][i] = v;
            }
    }
}

/* Write one burst of pseudo-random data to row 0, bank 0 through the injectors and read it
 * back; the bit errors on lane `module` (sdram_write_read_check_test_pattern). */
static unsigned check_pattern(int module, int seed)
{
    uint8_t (*prs)[BYTES_PER_PHASE] = patterns[seed];

    wr(SURF_DDRC_DFII_P0_ADDRESS, 0);                /* activate row 0 */
    wr(SURF_DDRC_DFII_P0_BADDRESS, 0);
    command(0, CMD_RAS | CMD_CS);

    for (int p = 0; p < DDRINIT_PHASES; p++)         /* csr_wr_buf_uint8: byte k in bits 8k+7..8k */
        wr(PI(WRDATA, p), (uint32_t)prs[p][0] | (uint32_t)prs[p][1] << 8 |
                          (uint32_t)prs[p][2] << 16 | (uint32_t)prs[p][3] << 24);
    wr(PI(ADDRESS, DDRINIT_WRPHASE), 0);
    wr(PI(BADDRESS, DDRINIT_WRPHASE), 0);
    command(DDRINIT_WRPHASE, CMD_CAS | CMD_WE | CMD_CS | CMD_WRDATA);

    wr(PI(ADDRESS, DDRINIT_RDPHASE), 0);
    wr(PI(BADDRESS, DDRINIT_RDPHASE), 0);
    command(DDRINIT_RDPHASE, CMD_CAS | CMD_CS | CMD_RDDATA);

    wr(SURF_DDRC_DFII_P0_ADDRESS, 0);                /* precharge */
    wr(SURF_DDRC_DFII_P0_BADDRESS, 0);
    command(0, CMD_RAS | CMD_WE | CMD_CS);

    /* the lane's bytes: the first edge's at `module`, the second's half a word further */
    int pebo = (module * DDRINIT_DQ_DQS_RATIO) / 8;
    int nebo = pebo + BYTES_PER_PHASE / DDRINIT_XDR;
    unsigned errors = 0;
    for (int p = 0; p < DDRINIT_PHASES; p++) {
        uint32_t t = ops->rd(PI(RDDATA, p));
        errors += popcount8((prs[p][pebo] ^ (uint8_t)(t >> (8 * pebo))) & MODULE_MASK);
        errors += popcount8((prs[p][nebo] ^ (uint8_t)(t >> (8 * nebo))) & MODULE_MASK);
    }
    return errors;
}

static unsigned run_pattern(int module)
{
    unsigned errors = 0;
    for (int i = 0; i < SEEDS; i++)
        errors += check_pattern(module, i);
    return errors;
}

/* ---------------------------------------------------------------- leveling (sdram.c) */
static char line[160];

/* Every tap at the current bitslip: the score sdram_read_leveling_scan_module gives it. */
static unsigned scan_module(int module, int bitslip)
{
    char *p = line;
    unsigned score = 0;
    if (ops->scans) {
        p = put_str(p, "  m");
        p = put_dec(p, module, 1);
        p = put_str(p, ", b");
        p = put_dec(p, bitslip, 2);
        p = put_str(p, ": |");
    }
    rst_delay(module);
    for (int i = 0; i < DDRINIT_DELAYS; i++) {
        unsigned errors = run_pattern(module);
        int working = errors == 0;
        score += (unsigned)working * SEEDS * MAX_ERRORS * DDRINIT_DELAYS + (SEEDS * MAX_ERRORS - errors);
        if (ops->scans)
            *p++ = working ? '1' : '0';
        inc_delay(module);
    }
    if (ops->scans) {
        p = put_str(p, "| ");
        *p = 0;
        out(line);
    }
    return score;
}

/* The widest window of working taps at the current bitslip, and the delay set to its middle
 * and checked (sdram_leveling_center_module); returns 1 on success. */
static int center_module(int module, int *mid_out, int *range_out)
{
    int working = 0, last_working, delay = 0;
    int delay_min = -1, delay_max, cur_min;
    unsigned errors = 1;

    rst_delay(module);
    for (;;) {                                        /* the smallest working delay (two in a row) */
        errors = run_pattern(module);
        last_working = working;
        working = errors == 0;
        if (working && last_working && delay_min < 0) {
            delay_min = delay - 1;
            break;
        }
        delay++;
        if (delay >= DDRINIT_DELAYS)
            break;
        inc_delay(module);
    }

    delay_max = delay_min;
    cur_min = delay_min;
    for (;;) {                                        /* the largest working range */
        errors = run_pattern(module);
        working = errors == 0;
        if (working) {
            if (delay - cur_min > delay_max - delay_min) {
                delay_min = cur_min;
                delay_max = delay;
            }
        } else {
            cur_min = delay + 1;
        }
        delay++;
        if (delay >= DDRINIT_DELAYS)
            break;
        inc_delay(module);
    }
    if (delay_max < 0)
        delay_max = delay;

    int mid = (delay_min + delay_max) / 2 % DDRINIT_DELAYS;
    int range = (delay_max - delay_min) / 2;
    *mid_out = mid;
    *range_out = delay_min < 0 ? -1 : range;

    if (delay_min >= 0) {                             /* the middle, checked up to 8 times */
        for (int retries = 8; retries > 0; retries--) {
            rst_delay(module);
            wait_us(1);
            for (int i = 0; i < mid; i++) {
                inc_delay(module);
                wait_us(1);
            }
            errors = run_pattern(module);
            if (errors == 0)
                break;
        }
    }
    return delay_min >= 0 && errors == 0;
}

static int read_leveling(struct ddrinit_result *res)
{
    int all_ok = 1;
    for (int module = 0; module < DDRINIT_MODULES; module++) {
        unsigned best_score = 0;
        int best_bitslip = 0, mid, range;

        rst_bitslip(module);
        for (int bitslip = 0; bitslip < DDRINIT_BITSLIPS; bitslip++) {
            unsigned score = scan_module(module, bitslip);
            center_module(module, &mid, &range);
            if (ops->scans) {
                char *p = put_str(line, range < 0 ? "delays: -" : "delays: ");
                if (range >= 0) {
                    p = put_dec(p, mid, 2);
                    p = put_str(p, "+-");
                    p = put_dec(p, range, 2);
                }
                p = put_str(p, "\n");
                *p = 0;
                out(line);
            }
            if (score > best_score) {
                best_bitslip = bitslip;
                best_score = score;
            }
            if (bitslip == DDRINIT_BITSLIPS - 1)
                break;
            inc_bitslip(module);
        }

        rst_bitslip(module);                          /* the best bitslip, then its window again */
        for (int b = 0; b < best_bitslip; b++)
            inc_bitslip(module);
        int ok = center_module(module, &mid, &range);
        res->lane[module].bitslip = best_bitslip;
        res->lane[module].delay = mid;
        res->lane[module].window = range;
        if (!ok)
            all_ok = 0;
    }
    return all_ok;
}

/* ---------------------------------------------------------------- the whole sequence */
int ddrinit_run(const struct ddrinit_ops *o, struct ddrinit_result *res)
{
    ops = o;
    uint32_t t0 = ops->now_us();
    make_patterns();
    for (int m = 0; m < DDRINIT_MODULES; m++) {
        res->lane[m].bitslip = 0;
        res->lane[m].delay = 0;
        res->lane[m].window = -1;
    }

    wr(SURF_DDRC_INIT_DONE, 0);
    wr(SURF_DDRC_INIT_ERROR, 0);
    wr(SURF_DDRC_PHY_RDPHASE, DDRINIT_RDPHASE);
    wr(SURF_DDRC_PHY_WRPHASE, DDRINIT_WRPHASE);
    /* LiteX decides at compile time (SDRAM_PHY_READ_LEVELING_CAPABLE); here the PHY says it: the
     * A7DDRPHY's phase registers read back, a PHY without registers (the simulation's DRAM
     * model, whose DFII reads come back on phase 0 only) reads 0 */
    res->leveling = ops->rd(SURF_DDRC_PHY_RDPHASE) == DDRINIT_RDPHASE &&
                    ops->rd(SURF_DDRC_PHY_WRPHASE) == DDRINIT_WRPHASE;
    wr(SURF_DDRC_DFII_CONTROL, CTRL_SOFTWARE);        /* sdram_software_control_on */
    wr(SURF_DDRC_PHY_RST, 1);                         /* the PHY to a known state */
    wait_us(10);
    wr(SURF_DDRC_PHY_RST, 0);
    wait_us(10);

    init_sequence();

    if (res->leveling) {
        for (int m = 0; m < DDRINIT_MODULES; m++) {   /* sdram_leveling: delays and bitslips to 0 */
            rst_delay(m);
            rst_bitslip(m);
        }
        res->ok = read_leveling(res);
    } else {
        res->ok = 1;                                  /* nothing to level */
    }

    wr(SURF_DDRC_DFII_CONTROL, CTRL_HARDWARE);       /* sdram_software_control_off */
    if (!res->ok)
        wr(SURF_DDRC_INIT_ERROR, 1);
    wr(SURF_DDRC_INIT_DONE, 1);
    res->us = ops->now_us() - t0;
    return res->ok;
}

void ddrinit_describe(const struct ddrinit_result *res, int lane, char *buf, unsigned len)
{
    char tmp[64], *p = tmp;
    const struct ddrinit_lane *l = &res->lane[lane];
    p = put_str(p, "lane ");
    p = put_dec(p, lane, 1);
    if (!res->leveling) {
        p = put_str(p, ": not leveled (the PHY has no delay registers)");
    } else {
        p = put_str(p, ": bitslip ");
        p = put_dec(p, l->bitslip, 1);
    }
    if (!res->leveling) {
        /* nothing more */
    } else if (l->window < 0) {
        p = put_str(p, ", no working delay");
    } else {
        p = put_str(p, ", delay ");
        p = put_dec(p, l->delay, 1);
        p = put_str(p, " (window +-");
        p = put_dec(p, l->window, 1);
        p = put_str(p, ")");
    }
    *p = 0;
    unsigned i = 0;
    for (; tmp[i] && i + 1 < len; i++)
        buf[i] = tmp[i];
    if (len)
        buf[i] = 0;
}
