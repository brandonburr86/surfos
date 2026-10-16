/*
SurfCPU firmware tests: the host side of port.h
--------------------
File: port_host.c Date: 10/15/26 (SurfCPU)
--------------------
See port_host.h. Time: an SPI byte costs 8 SCLK periods at the current divider plus about
ten CPU cycles; reading the clock costs a microsecond, so every busy-wait in the firmware
ends in simulated time.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"
#include "port_host.h"

struct host_env host;

void host_reset(void)
{
    uint8_t *ddr = host.ddr;
    char *out = host.out;
    size_t cap = host.out_cap;
    memset(&host, 0, sizeof host);
    host.sys_hz = SURF_SYS_HZ;
    host.ddr_size = SURF_DDR_SIZE;
    host.ddr_calib_us = 150000;
    host.spi_div = 31;
    if (!ddr)                                       /* zeroed once; runs share it (the fuzzer) */
        ddr = calloc(1, SURF_DDR_SIZE);
    if (!ddr) {
        fprintf(stderr, "port_host: no memory for the DDR buffer\n");
        exit(2);
    }
    host.ddr = ddr;
    host.out = out;
    host.out_cap = cap;
    if (host.out)
        host.out[0] = 0;
}

void host_free(void)
{
    if (host.card)
        sdsim_close(host.card);
    free(host.ddr);
    free(host.out);
    memset(&host, 0, sizeof host);
}

static void advance_ns(uint64_t ns)
{
    host.now_ns += ns;
    if (host.time_budget_ns && host.now_ns > host.time_budget_ns) {
        fprintf(stderr, "RUNAWAY: more than %llu ms of simulated time\n",
                (unsigned long long)(host.time_budget_ns / 1000000u));
        exit(3);
    }
}

uint32_t port_now_us(void)
{
    advance_ns(1000);
    return (uint32_t)(host.now_ns / 1000u);
}

int port_timer_check(void) { return 1; }

int port_card_present(void)
{
    if (host.present_hook)
        return host.present_hook();
    return host.card ? sdsim_present(host.card) : 0;
}
void port_spi_select(int on) { host.cs = on; }

uint8_t port_spi_xfer(uint8_t out)
{
    /* 8 bits at sys_hz / (2 (div + 1)), plus ~10 cycles of loop */
    advance_ns((uint64_t)(16u * (host.spi_div + 1u) + 10u) * 1000000000u / host.sys_hz);
    host.spi_bytes++;
    if (host.spi_budget && host.spi_bytes > host.spi_budget) {
        fprintf(stderr, "RUNAWAY: more than %llu SPI bytes\n", (unsigned long long)host.spi_budget);
        exit(3);
    }
    if (host.xfer_hook)
        return host.xfer_hook(host.cs, out);
    return host.card ? sdsim_xfer(host.card, host.cs, out) : 0xFF;
}

void port_spi_recv(uint8_t *buf, uint32_t n)
{
    while (n--)
        *buf++ = port_spi_xfer(0xFF);
}

void port_spi_set_div(uint32_t div)
{
    host.spi_div = div & 0xFFu;
    if (host.card)
        sdsim_set_div(host.card, host.spi_div);
}

uint32_t port_sysinfo(uint32_t off)
{
    uint32_t us = (uint32_t)(host.now_ns / 1000u);
    switch (off) {
    case SURF_SYSINFO_MAGIC: return 0x46525553u;
    case SURF_SYSINFO_SOC_VERSION: return SURF_SOC_VERSION;
    case SURF_SYSINFO_MAP_VERSION: return SURF_MAP_VERSION;
    case SURF_SYSINFO_BOARD_ID: return SURF_BOARD_SIM;
    case SURF_SYSINFO_SYS_HZ: return host.sys_hz;
    case SURF_SYSINFO_BUILD_ID: return 0x0f0f0f0fu;
    case SURF_SYSINFO_BUILD_FLAGS: return SURF_SYSINFO_BUILD_FLAGS_SIM_MASK;
    case SURF_SYSINFO_FEATURES: return 0xFFu;
    case SURF_SYSINFO_DDR_STATUS:
        switch (host.ddr_mode) {
        case DDR_ABSENT: return 0;
        case DDR_FAIL:
            return SURF_SYSINFO_DDR_STATUS_PRESENT_MASK | (us >= host.ddr_calib_us ? SURF_SYSINFO_DDR_STATUS_CALIB_FAIL_MASK : 0u);
        case DDR_NEVER: return SURF_SYSINFO_DDR_STATUS_PRESENT_MASK;
        default:
            return SURF_SYSINFO_DDR_STATUS_PRESENT_MASK | (us >= host.ddr_calib_us ? SURF_SYSINFO_DDR_STATUS_CALIB_DONE_MASK : 0u);
        }
    case SURF_SYSINFO_DDR_SIZE: return host.ddr_mode == DDR_ABSENT ? 0u : host.ddr_size;
    case SURF_SYSINFO_PIX_HZ: return SURF_PIX_HZ;
    default: return 0;
    }
}

uint32_t port_timebase_hz(void) { return SURF_TIMEBASE_HZ; }
uint32_t port_buttons(void) { return host.buttons; }

static int in_ddr(uint32_t addr, uint32_t len)
{
    return addr >= SURF_DDR_BASE && len <= SURF_DDR_SIZE && addr - SURF_DDR_BASE <= SURF_DDR_SIZE - len;
}

volatile uint32_t *port_ddr_word(uint32_t addr)
{
    if (!in_ddr(addr, 4) || (addr & 3u)) {
        fprintf(stderr, "port_host: DDR word access at 0x%08x outside DDR\n", addr);
        abort();
    }
    return (volatile uint32_t *)(host.ddr + (addr - SURF_DDR_BASE));
}

uint8_t *port_mem(uint32_t addr, uint32_t len)
{
    if (!in_ddr(addr, len)) {
        fprintf(stderr, "port_host: port_mem(0x%08x, %u) outside DDR: a range check is missing\n", addr, len);
        abort();
    }
    return host.ddr + (addr - SURF_DDR_BASE);
}

void port_mem_written(uint32_t addr, uint32_t len)
{
    if (host.ddr_flip && len) {
        host.ddr[addr - SURF_DDR_BASE + len / 2] ^= 0x04;     /* a DDR bit that did not hold */
        host.ddr_flip = 0;
    }
}

int port_peek32(uint32_t addr, uint32_t *val)
{
    if (in_ddr(addr, 4) && !(addr & 3u)) {
        memcpy(val, host.ddr + (addr - SURF_DDR_BASE), 4);
        return 0;
    }
    if (addr >= SURF_BOOTINFO_BASE && addr + 4u <= SURF_BOOTINFO_BASE + sizeof host.bootinfo && !(addr & 3u)) {
        memcpy(val, (uint8_t *)&host.bootinfo + (addr - SURF_BOOTINFO_BASE), 4);
        return 0;
    }
    return -1;
}

int port_poke32(uint32_t addr, uint32_t val)
{
    if (in_ddr(addr, 4) && !(addr & 3u)) {
        memcpy(host.ddr + (addr - SURF_DDR_BASE), &val, 4);
        return 0;
    }
    return -1;
}

struct surf_boot_info *port_bootinfo(void) { return &host.bootinfo; }

void port_putc(char c)
{
    if (host.out_len + 2 > host.out_cap) {
        size_t cap = host.out_cap ? host.out_cap * 2 : 65536;
        char *p = realloc(host.out, cap);
        if (!p)
            abort();
        host.out = p;
        host.out_cap = cap;
    }
    host.out[host.out_len++] = c;
    host.out[host.out_len] = 0;
    if (host.echo)
        fputc(c, stdout);
}

int port_getc(void)
{
    if (!host.in || !*host.in)
        return -1;
    return (unsigned char)*host.in++;
}
