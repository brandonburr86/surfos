/*
SurfCPU firmware tests: a model of LiteDRAM behind the ddrc registers
--------------------
File: ddrc_model.h Date: 10/16/26 (SurfCPU)
--------------------
What the host tests (port_host.c) and the firmware emulator (surfemu.c) put behind the ddrc
region when sysinfo.DDR_STATUS.SWINIT is 1: LiteDRAM generated without its own CPU
(ip/litedram/arty.yml), as far as common/ddrinit.c uses it. The registers keep what is
written (spec/soc.toml's ddrc block); the strobes act on the lanes DLY_SEL selects; a command
written to a phase's COMMAND register is carried out when its COMMAND_ISSUE is written with 1,
and only while software controls the DRAM (DFII_CONTROL.SEL 0).

The DRAM side is checked the way the real part would punish mistakes, so a test fails when the
firmware gets the sequence wrong: mode register loads count only with RESET# released and CKE
high, and the DRAM works only once MR2, MR3, MR1 and MR0 hold the generator's values
(ddrinit_phy.h repeats them; these are taken from the generated sdram_phy.h independently)
and a ZQ calibration (long) followed them; a write or read needs an activated row and must
come on the phase PHY_WRPHASE or PHY_RDPHASE names. Data written through the injectors is
kept (one burst, row 0 of bank 0 is all the leveling uses); a read gives each lane its bytes
back unchanged only at its working bitslip with its read delay inside its window
(struct ddrc_lane) and every bit inverted otherwise. Anything outside these rules gives
inverted data and counts in `violations`, which the tests require to stay 0.

Once INIT_DONE is 1 with INIT_ERROR 0, the controller has the DRAM (ddrc_model_ready); DDR
data through it is right on a lane only when that lane's bitslip and delay are inside its
window (ddrc_model_lane_ok), as on the board, where a badly leveled lane reads back wrong.

With no_phy set it is LiteDRAM's simulation core instead (its DRAM model in place of the
A7DDRPHY): no PHY registers (they read 0 and ignore writes), no delays to level (every lane
reads right), and an injected read updates phase 0's RDDATA only (the model raises
rddata_valid on phase 0 alone).
*/
#ifndef SURFCPU_DDRC_MODEL_H
#define SURFCPU_DDRC_MODEL_H

#include <stdint.h>

#define DDRC_MODEL_LANES  2
#define DDRC_MODEL_PHASES 4

/* A lane reads back what was written only at bitslip `bitslip` with its delay in
 * [delay_lo, delay_hi]; delay_lo > delay_hi: at no setting. */
struct ddrc_lane { int bitslip, delay_lo, delay_hi; };

struct ddrc_model {
    struct ddrc_lane lane[DDRC_MODEL_LANES];        /* the configuration */
    int no_phy;                                     /* ... LiteDRAM's simulation core instead */
    /* registers */
    uint32_t init_done, init_error, phy_rst, sel, rdphase, wrphase, control;
    uint32_t cmd[DDRC_MODEL_PHASES], addr[DDRC_MODEL_PHASES], baddr[DDRC_MODEL_PHASES];
    uint32_t wrdata[DDRC_MODEL_PHASES], rddata[DDRC_MODEL_PHASES];
    int bitslip[DDRC_MODEL_LANES], delay[DDRC_MODEL_LANES];
    /* the DRAM */
    uint32_t mr[4];
    unsigned mr_loaded;             /* bit per mode register */
    int zq_done, row_open;
    uint32_t burst[DDRC_MODEL_PHASES];
    /* counts, for the tests */
    uint32_t accesses, mrs, zqcl, writes, reads, violations;
};

void ddrc_model_init(struct ddrc_model *m);        /* power-up state; keeps m->lane */
uint32_t ddrc_model_read(struct ddrc_model *m, uint32_t off);
void ddrc_model_write(struct ddrc_model *m, uint32_t off, uint32_t value);

/* INIT_DONE 1 with INIT_ERROR 0: LiteDRAM's init_done/init_error, as the board top combines
 * them into init_calib_complete (sysinfo.DDR_STATUS.CALIB_DONE). */
int ddrc_model_ready(const struct ddrc_model *m);
/* The DRAM initialized, the controller in charge and this lane leveled into its window. */
int ddrc_model_lane_ok(const struct ddrc_model *m, int lane);

#endif /* SURFCPU_DDRC_MODEL_H */
