/*
SurfCPU firmware tests: the host side of port.h
--------------------
File: port_host.h Date: 10/15/26 (SurfCPU)
--------------------
The simulated board the portable SurfBoot code runs on in the host tests: an SD card model
behind the SPI calls, a clock that advances with every SPI byte (at the divider in use) and
every time read, sysinfo registers with a DDR controller that calibrates (or fails, or
never finishes), a 256 MiB buffer standing in for DDR, buttons, the boot information page
and a captured console.
*/
#ifndef SURFCPU_PORT_HOST_H
#define SURFCPU_PORT_HOST_H

#include <stddef.h>
#include <stdint.h>
#include "sdcard_sim.h"
#include "surfcpu/bootabi.h"

enum ddr_mode { DDR_OK = 0, DDR_ABSENT, DDR_FAIL, DDR_NEVER };

struct host_env {
    struct sdsim *card;         /* NULL: an empty socket without a model */
    uint64_t now_ns;            /* simulated time */
    uint32_t sys_hz;
    uint32_t spi_div;
    int cs;
    int ddr_mode;
    uint32_t ddr_calib_us;      /* calibration finishes this long after start */
    uint32_t ddr_size;
    uint8_t *ddr;               /* SURF_DDR_SIZE bytes */
    int ddr_flip;               /* flip a payload bit between the copy and the read-back */
    uint32_t buttons;
    struct surf_boot_info bootinfo;
    char *out;                  /* console capture */
    size_t out_len, out_cap;
    const char *in;             /* console input still to deliver */
    int echo;                   /* copy the console to stdout */
    uint64_t spi_bytes;
    uint64_t spi_budget;        /* abort (exit 3) after this many SPI bytes: a runaway loop */
    uint64_t time_budget_ns;    /* abort (exit 3) after this much simulated time */
};

extern struct host_env host;

void host_reset(void);              /* fresh state, no card (DDR keeps its contents) */
void host_free(void);

#endif /* SURFCPU_PORT_HOST_H */
