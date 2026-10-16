/*
SurfCPU firmware tests: a model of LiteDRAM behind the ddrc registers
--------------------
File: ddrc_model.c Date: 10/16/26 (SurfCPU)
--------------------
See ddrc_model.h.
*/
#include "ddrc_model.h"

#include <string.h>
#include "surfcpu/soc.h"

/* The generated init_sequence()'s mode register values for ip/litedram/arty.yml
 * (software/include/generated/sdram_phy.h): MR0 0x920, MR1 0x6, MR2 0x200, MR3 0. */
static const uint32_t want_mr[4] = { 0x920u, 0x6u, 0x200u, 0x0u };

#define STRIDE  (SURF_DDRC_DFII_P1_COMMAND - SURF_DDRC_DFII_P0_COMMAND)
#define C_CS    SURF_DDRC_DFII_P0_COMMAND_CS_MASK
#define C_WE    SURF_DDRC_DFII_P0_COMMAND_WE_MASK
#define C_CAS   SURF_DDRC_DFII_P0_COMMAND_CAS_MASK
#define C_RAS   SURF_DDRC_DFII_P0_COMMAND_RAS_MASK
#define C_WRD   SURF_DDRC_DFII_P0_COMMAND_WRDATA_MASK
#define C_RDD   SURF_DDRC_DFII_P0_COMMAND_RDDATA_MASK
#define CTL_SEL SURF_DDRC_DFII_CONTROL_SEL_MASK
#define CTL_CKE SURF_DDRC_DFII_CONTROL_CKE_MASK
#define CTL_RST SURF_DDRC_DFII_CONTROL_RESET_N_MASK

void ddrc_model_init(struct ddrc_model *m)
{
    struct ddrc_lane lane[DDRC_MODEL_LANES];
    int no_phy = m->no_phy;
    memcpy(lane, m->lane, sizeof lane);
    memset(m, 0, sizeof *m);
    memcpy(m->lane, lane, sizeof lane);
    m->no_phy = no_phy;
    m->control = CTL_SEL;                           /* LiteDRAM's reset value: hardware control */
}

static int dram_initialized(const struct ddrc_model *m)
{
    if (m->mr_loaded != 0xFu || !m->zq_done)
        return 0;
    for (int i = 0; i < 4; i++)
        if (m->mr[i] != want_mr[i])
            return 0;
    return 1;
}

static int lane_in_window(const struct ddrc_model *m, int l)
{
    const struct ddrc_lane *w = &m->lane[l];
    if (m->no_phy)
        return 1;                                   /* no delays: the data is always right */
    return m->bitslip[l] == w->bitslip && m->delay[l] >= w->delay_lo && m->delay[l] <= w->delay_hi;
}

static void issue(struct ddrc_model *m, int p)
{
    uint32_t c = m->cmd[p];
    if ((m->control & CTL_SEL) || m->phy_rst || !(c & C_CS))
        return;                                     /* the controller owns the DRAM, or no command */
    uint32_t kind = c & (C_RAS | C_CAS | C_WE);
    int powered = (m->control & (CTL_RST | CTL_CKE)) == (CTL_RST | CTL_CKE);
    if (kind == (C_RAS | C_CAS | C_WE)) {           /* MRS: the register number on the bank address */
        m->mrs++;
        if (!powered || m->baddr[p] > 3u) {
            m->violations++;
            return;
        }
        m->mr[m->baddr[p]] = m->addr[p];
        m->mr_loaded |= 1u << m->baddr[p];
        m->zq_done = 0;                             /* ZQ calibration must follow the mode registers */
    } else if (kind == C_WE) {                      /* ZQCL (A10 high) */
        m->zqcl++;
        if (!powered || m->addr[p] != 0x400u || m->mr_loaded != 0xFu)
            m->violations++;
        else
            m->zq_done = 1;
    } else if (kind == C_RAS) {                     /* ACTIVATE */
        if (!dram_initialized(m))
            m->violations++;
        m->row_open = 1;
    } else if (kind == (C_RAS | C_WE)) {            /* PRECHARGE */
        m->row_open = 0;
    } else if (kind == (C_CAS | C_WE) && (c & C_WRD)) {     /* WRITE with the four phases' data */
        m->writes++;
        if (!dram_initialized(m) || !m->row_open || (!m->no_phy && ((uint32_t)p != m->wrphase || m->wrphase != 3u))) {
            m->violations++;
            for (int i = 0; i < DDRC_MODEL_PHASES; i++)
                m->burst[i] = ~m->wrdata[i];
        } else {
            memcpy(m->burst, m->wrdata, sizeof m->burst);
        }
    } else if (kind == C_CAS && (c & C_RDD)) {      /* READ into the four phases' RDDATA */
        m->reads++;
        int ok = dram_initialized(m) && m->row_open &&
                 (m->no_phy || ((uint32_t)p == m->rdphase && m->rdphase == 3u));
        if (!ok)
            m->violations++;
        for (int i = 0; i < (m->no_phy ? 1 : DDRC_MODEL_PHASES); i++) {
            uint32_t v = m->burst[i];
            for (int l = 0; l < DDRC_MODEL_LANES; l++)
                if (!ok || !lane_in_window(m, l))   /* both of the lane's bytes, inverted */
                    v ^= 0xFFu << (8 * l) | 0xFFu << (8 * (l + 2));
            m->rddata[i] = v;
        }
    } else {
        m->violations++;                            /* nothing ddrinit.c sends */
    }
}

uint32_t ddrc_model_read(struct ddrc_model *m, uint32_t off)
{
    m->accesses++;
    if (off >= SURF_DDRC_DFII_P0_COMMAND && off < SURF_DDRC_DFII_P0_COMMAND + DDRC_MODEL_PHASES * STRIDE) {
        uint32_t p = (off - SURF_DDRC_DFII_P0_COMMAND) / STRIDE, r = off - p * STRIDE;
        if (r == SURF_DDRC_DFII_P0_COMMAND) return m->cmd[p];
        if (r == SURF_DDRC_DFII_P0_ADDRESS) return m->addr[p];
        if (r == SURF_DDRC_DFII_P0_BADDRESS) return m->baddr[p];
        if (r == SURF_DDRC_DFII_P0_WRDATA) return m->wrdata[p];
        if (r == SURF_DDRC_DFII_P0_RDDATA) return m->rddata[p];
        return 0;
    }
    if (m->no_phy && off >= SURF_DDRC_PHY_RST && off < SURF_DDRC_DFII_CONTROL)
        return 0;                                   /* no PHY registers */
    switch (off) {
    case SURF_DDRC_INIT_DONE: return m->init_done;
    case SURF_DDRC_INIT_ERROR: return m->init_error;
    case SURF_DDRC_PHY_RST: return m->phy_rst;
    case SURF_DDRC_PHY_DLY_SEL: return m->sel;
    case SURF_DDRC_PHY_RDPHASE: return m->rdphase;
    case SURF_DDRC_PHY_WRPHASE: return m->wrphase;
    case SURF_DDRC_DFII_CONTROL: return m->control;
    default: return 0;                              /* LiteX's CSR bus answers every address */
    }
}

void ddrc_model_write(struct ddrc_model *m, uint32_t off, uint32_t v)
{
    m->accesses++;
    if (off >= SURF_DDRC_DFII_P0_COMMAND && off < SURF_DDRC_DFII_P0_COMMAND + DDRC_MODEL_PHASES * STRIDE) {
        uint32_t p = (off - SURF_DDRC_DFII_P0_COMMAND) / STRIDE, r = off - p * STRIDE;
        if (r == SURF_DDRC_DFII_P0_COMMAND) m->cmd[p] = v & 0x3Fu;
        else if (r == SURF_DDRC_DFII_P0_COMMAND_ISSUE) { if (v & 1u) issue(m, (int)p); }
        else if (r == SURF_DDRC_DFII_P0_ADDRESS) m->addr[p] = v & 0x3FFFu;
        else if (r == SURF_DDRC_DFII_P0_BADDRESS) m->baddr[p] = v & 7u;
        else if (r == SURF_DDRC_DFII_P0_WRDATA) m->wrdata[p] = v;
        return;
    }
    if (m->no_phy && off >= SURF_DDRC_PHY_RST && off < SURF_DDRC_DFII_CONTROL)
        return;                                     /* no PHY registers */
    if (v & 1u) {                                   /* the lane strobes act on the selected lanes */
        for (int l = 0; l < DDRC_MODEL_LANES; l++) {
            if (!(m->sel & (1u << l)))
                continue;
            if (off == SURF_DDRC_PHY_RDLY_DQ_RST) m->delay[l] = 0;
            if (off == SURF_DDRC_PHY_RDLY_DQ_INC) m->delay[l] = (m->delay[l] + 1) & 31;
            if (off == SURF_DDRC_PHY_RDLY_DQ_BITSLIP_RST) m->bitslip[l] = 0;
            if (off == SURF_DDRC_PHY_RDLY_DQ_BITSLIP) m->bitslip[l] = (m->bitslip[l] + 1) & 7;
        }
    }
    switch (off) {
    case SURF_DDRC_INIT_DONE: m->init_done = v & 1u; break;
    case SURF_DDRC_INIT_ERROR: m->init_error = v & 1u; break;
    case SURF_DDRC_PHY_RST: m->phy_rst = v & 1u; break;
    case SURF_DDRC_PHY_DLY_SEL: m->sel = v & 3u; break;
    case SURF_DDRC_PHY_RDPHASE: m->rdphase = v & 3u; break;
    case SURF_DDRC_PHY_WRPHASE: m->wrphase = v & 3u; break;
    case SURF_DDRC_DFII_CONTROL:
        m->control = v & 0xFu;
        if (!(m->control & CTL_SEL) && !(m->control & CTL_RST)) {
            m->mr_loaded = 0;                       /* RESET# low: the DRAM forgets everything */
            m->zq_done = 0;
            m->row_open = 0;
        }
        break;
    default: break;
    }
}

int ddrc_model_ready(const struct ddrc_model *m)
{
    return m->init_done && !m->init_error;
}

int ddrc_model_lane_ok(const struct ddrc_model *m, int lane)
{
    return ddrc_model_ready(m) && (m->control & CTL_SEL) && dram_initialized(m) && lane_in_window(m, lane);
}
