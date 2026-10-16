/*
SurfBoot: what the portable boot code needs from the platform
--------------------
File: port.h Date: 10/15/26 (SurfCPU)
--------------------
sd.c, fat32.c, image.c, boot.c and monitor.c use only these functions, so they compile both
for the SoC (port_surf.c: the surf_spi controller, mtime, sysinfo, DDR at its real address)
and natively on the host (firmware/tests/port_host.c: a simulated card behind the SPI calls,
a simulated clock, a buffer standing in for DDR), where the tests run them against card
image files.
*/
#ifndef SURFBOOT_PORT_H
#define SURFBOOT_PORT_H

#include <stdint.h>
#include "surfcpu/bootabi.h"

/* A free-running microsecond counter (mtime's low word; wraps after about 71 minutes). */
uint32_t port_now_us(void);

/* The microSD socket behind the SPI master (docs/MEMORY-MAP.md, spi). */
int port_card_present(void);                    /* card-detect switch; 1 if the adapter has none */
void port_spi_select(int on);                   /* 1 = chip select asserted (CS low) */
uint8_t port_spi_xfer(uint8_t out);             /* exchange one byte */
void port_spi_recv(uint8_t *buf, uint32_t n);   /* receive n bytes, sending 0xFF */
void port_spi_set_div(uint32_t div);            /* SCLK = sys_hz / (2 * (div + 1)) */

/* Board identity and clocks. */
uint32_t port_sysinfo(uint32_t off);            /* a sysinfo register (SURF_SYSINFO_*) */
uint32_t port_timebase_hz(void);                /* timer.TIMEBASE */
uint32_t port_buttons(void);                    /* gpio.BTN, debounced, 1 = pressed */

/* DDR, through pointers the platform hands out. The callers check every range first. */
volatile uint32_t *port_ddr_word(uint32_t addr); /* one aligned DDR word */
uint8_t *port_mem(uint32_t addr, uint32_t len);  /* [addr, addr + len) inside DDR */
void port_mem_written(uint32_t addr, uint32_t len); /* the loader finished writing it (a FENCE) */

/* Memory access for the monitor's md/mw: 0 on success, -1 if the access faulted. */
int port_peek32(uint32_t addr, uint32_t *val);
int port_poke32(uint32_t addr, uint32_t val);

/* Where the boot information goes (SURF_BOOTINFO_BASE on the SoC). */
struct surf_boot_info *port_bootinfo(void);

/* The console: UART (and the screen) out, UART in. */
void port_putc(char c);
int port_getc(void);                            /* -1 when nothing has arrived */

#endif /* SURFBOOT_PORT_H */
