/*
SurfBoot: the SoC side of port.h
--------------------
File: port_surf.c Date: 10/15/26 (SurfCPU)
--------------------
port.h on the real hardware: surf_spi for the card (a DATA read waits for the byte in
flight, so write-then-read is a whole exchange), mtime for time, sysinfo for identity, and
DDR at its own addresses. port_peek32/port_poke32 install a trap hook for the one load or
store they make, so a bad address in the monitor's md/mw is reported instead of stopping
the machine.
*/
#include "fw.h"
#include "port.h"

#define SPI(off) REG(SURF_SPI_BASE, off)

static int timer_dead;
static uint32_t soft_us;

/* mtime in microseconds; with a timer found not to run (port_timer_check), a software count
 * of about 4 us per call instead, so that every bounded wait in SurfBoot stays bounded. */
uint32_t port_now_us(void)
{
    if (!timer_dead)
        return timer_now32();
    soft_us += 4u;
    return soft_us;
}

int port_timer_check(void)
{
    uint32_t t0 = timer_now32();
    for (uint32_t i = 0; i < 200000u; i++)          /* about 25 ms at 25 MHz: mtime must move */
        if (timer_now32() != t0)
            return 1;
    timer_dead = 1;
    return 0;
}

int port_card_present(void)
{
    return (SPI(SURF_SPI_STATUS) & SURF_SPI_STATUS_CARD_MASK) != 0;
}

void port_spi_select(int on)
{
    SPI(SURF_SPI_CTRL) = on ? SURF_SPI_CTRL_CS_MASK : 0u;
}

uint8_t port_spi_xfer(uint8_t out)
{
    SPI(SURF_SPI_DATA) = out;
    return (uint8_t)SPI(SURF_SPI_DATA);
}

void port_spi_recv(uint8_t *buf, uint32_t n)
{
    volatile unsigned int *data = &SPI(SURF_SPI_DATA);      /* SURF_REG is an unsigned int */
    while (n--) {
        *data = 0xFF;
        *buf++ = (uint8_t)*data;
    }
}

void port_spi_set_div(uint32_t div)
{
    SPI(SURF_SPI_DIV) = div & SURF_SPI_DIV_DIV_MASK;
}

uint32_t port_sysinfo(uint32_t off) { return sysinfo(off); }
uint32_t port_timebase_hz(void) { return REG(SURF_TIMER_BASE, SURF_TIMER_TIMEBASE); }
uint32_t port_buttons(void) { return REG(SURF_GPIO_BASE, SURF_GPIO_BTN) & SURF_GPIO_BTN_BTN_MASK; }

volatile uint32_t *port_ddr_word(uint32_t addr) { return (volatile uint32_t *)(uintptr_t)addr; }

uint8_t *port_mem(uint32_t addr, uint32_t len)
{
    (void)len;
    return (uint8_t *)(uintptr_t)addr;
}

void port_mem_written(uint32_t addr, uint32_t len)
{
    (void)addr;
    (void)len;
    __asm__ volatile ("fence rw, rw" ::: "memory");  /* every copy store done before the read-back */
}

struct surf_boot_info *port_bootinfo(void)
{
    return (struct surf_boot_info *)(uintptr_t)SURF_BOOTINFO_BASE;
}

void port_putc(char c) { con_putc(c); }
int port_getc(void) { return con_getc(); }

static volatile int probing, probe_fault;

static int probe_hook(struct trap_frame *f)
{
    if (!probing)
        return 0;
    if (f->mcause == 4 || f->mcause == 5 || f->mcause == 6 || f->mcause == 7) {
        probe_fault = 1;
        f->mepc += 4;                               /* step over the one load or store */
        return 1;
    }
    return 0;
}

int port_peek32(uint32_t addr, uint32_t *val)
{
    trap_hook_fn old = fw_trap_hook;
    uint32_t v = 0;
    fw_trap_hook = probe_hook;
    probe_fault = 0;
    probing = 1;
    __asm__ volatile ("lw %0, 0(%1)" : "+r"(v) : "r"(addr) : "memory");
    probing = 0;
    fw_trap_hook = old;
    if (probe_fault)
        return -1;
    *val = v;
    return 0;
}

int port_poke32(uint32_t addr, uint32_t val)
{
    trap_hook_fn old = fw_trap_hook;
    fw_trap_hook = probe_hook;
    probe_fault = 0;
    probing = 1;
    __asm__ volatile ("sw %0, 0(%1)" :: "r"(val), "r"(addr) : "memory");
    probing = 0;
    fw_trap_hook = old;
    return probe_fault ? -1 : 0;
}
