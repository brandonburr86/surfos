/*
SurfCPU: DDR3L initialization and read leveling for LiteDRAM, run by SurfCPU
--------------------
File: ddrinit.h Date: 10/16/26 (SurfCPU)
--------------------
When sysinfo.DDR_STATUS.SWINIT is 1 the memory controller (LiteDRAM generated without its own
CPU, docs/DDR.md) waits for software: ddrinit_run() brings the DDR3L out of reset, loads its
mode registers, calibrates it (ZQ), levels every byte lane's read path (bitslip and input
delay, LiteDRAM's A7DDRPHY), gives the memory to the controller and writes LiteDRAM's
init_done (and init_error on a failure) through the ddrc registers. sysinfo.DDR_STATUS's
CALIB_DONE follows a few microseconds later when it succeeded. The sequence is LiteX's
liblitedram's (sdram_init, sdram_read_leveling) ported to these registers: ddrinit.c keeps its
licence notice. The generator's settings it needs are in ddrinit_phy.h.

The caller provides the register access, the clock and the console, so the code is the same in
SurfBoot (through its port), in the diagnostics (direct) and in host tests (a model).
*/
#ifndef SURFCPU_DDRINIT_H
#define SURFCPU_DDRINIT_H

#include <stdint.h>
#include "ddrinit_phy.h"

struct ddrinit_ops {
    uint32_t (*rd)(uint32_t off);              /* a ddrc register (SURF_DDRC_* offset) */
    void (*wr)(uint32_t off, uint32_t value);
    uint32_t (*now_us)(void);                  /* a free-running microsecond count */
    void (*out)(const char *s);                /* the console; NULL for none */
    int scans;                                 /* 1: print each lane's scan of every bitslip */
};

struct ddrinit_lane {
    int bitslip;        /* the bitslip chosen (0..DDRINIT_BITSLIPS-1) */
    int delay;          /* the read delay chosen, taps (the middle of the window) */
    int window;         /* the window's half width, taps; -1: no working delay at that bitslip */
};

struct ddrinit_result {
    int ok;                                    /* every lane has a window and was checked there */
    int leveling;                              /* 0: a PHY without delay registers, not leveled */
    uint32_t us;                               /* how long it took */
    struct ddrinit_lane lane[DDRINIT_MODULES];
};

/* Initialize and level; writes INIT_DONE (and INIT_ERROR when not ok). Returns res->ok.
 * The read path is leveled only when the PHY has its delay registers: PHY_RDPHASE and
 * PHY_WRPHASE read back what was written (LiteDRAM's A7DDRPHY). A PHY without them (LiteDRAM's
 * DRAM model in simulation) is initialized without leveling, as LiteX's sdram_init does for a
 * PHY that is not SDRAM_PHY_READ_LEVELING_CAPABLE; res->leveling then says so. */
int ddrinit_run(const struct ddrinit_ops *ops, struct ddrinit_result *res);

/* One line per lane, e.g. "lane 0: bitslip 3, delay 16 (window +-7)", for a console; without
 * leveling "lane 0: not leveled (the PHY has no delay registers)". */
void ddrinit_describe(const struct ddrinit_result *res, int lane, char *buf, unsigned len);

#endif /* SURFCPU_DDRINIT_H */
