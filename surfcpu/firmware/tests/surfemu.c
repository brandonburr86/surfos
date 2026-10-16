/*
SurfCPU firmware tests: the firmware emulator
--------------------
File: surfemu.c Date: 10/15/26 (SurfCPU)
--------------------
Runs a boot ROM image (SurfBoot, the BRAM diagnostics) on an instruction-level model of the
SurfCPU SoC, so the real RV32 binaries (start-up, traps, drivers, the handoff, the
monitor) are tested on the host before the RTL exists. A test aid, not a reference model:
the cpu and verif tracks check the core against Spike, and the RTL system simulation is
the real evidence. What it models:

  * the core of docs/ISA.md: RV32IM + Zicsr + Zifencei, M-mode, the CSR table with its
    masks and read-only rules, every trap with its mcause/mepc/mtval, the illegal
    instruction cases (compressed encodings, 0, SLLI with imm[5], unknown CSRs, writes
    to read-only CSRs, SRET/URET/SFENCE.VMA), interrupts MEI > MSI > MTI, WFI, MRET;
  * the memory map of docs/MEMORY-MAP.md: boot ROM (read only), scratch RAM, text RAM (no
    fetch), DDR (errors until calibration), MMIO word access only, unknown offsets and
    unmapped addresses answering with bus errors (access faults);
  * uart (FIFOs at the real baud rate), timer, intc, gpio (buttons, LEDs), spi (with the
    waiting DATA read and the SD card model of sdcard_sim.c), ps2 (idle), video (registers
    and text RAM, dumped as text with --screen), sysinfo (DDR calibration status).

    surfemu --rom build/fw/surfboot.mem [--card IMG] [card options as fwtest] [--ddr ok|absent|fail|never]
            [--btn MASK] [--send TRIGGER=TEXT]... [--expect TEXT]... [--fail-on TEXT]...
            [--max-ms N] [--screen FILE] [--trace-traps] [--quiet] [--card-insert-ms N]
            [--ddr-fault-at ADDR] [--ddr-stuck-at ADDR] [--ddr-size BYTES]

--rom takes a .mem file (docs/MODULES.md section 5, the exact ROM format) or a raw .bin.
--send queues TEXT (with \\n, \\r, \\e escapes) on the UART receiver once the output
contains TRIGGER. Exit 0 once every --expect text has appeared in order, 1 when a --fail-on
text appears, 2 when --max-ms of simulated time pass first, 3 on an emulator error.
*/
#define _DEFAULT_SOURCE
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdcard_sim.h"
#include "surfcpu/soc.h"

#define ROM_SIZE   SURF_BOOTROM_SIZE
#define RAM_SIZE   SURF_SCRATCH_SIZE
#define TEXT_SIZE  SURF_TEXTRAM_SIZE
#define DDR_SIZE   SURF_DDR_SIZE
#define DDR_EXTRA  10                       /* extra cycles per DDR access */

static uint8_t rom[ROM_SIZE], ram[RAM_SIZE], textram[TEXT_SIZE];
static uint8_t *ddr;
static uint64_t cycles;
static uint32_t sys_hz = SURF_SYS_HZ;

/* ---------------- options and test I/O ---------------- */
static int quiet, trace_traps;
static const char *screen_file;
static char *out;
static size_t out_len, out_cap, send_pos;
static struct { const char *trigger; char text[256]; int done; } sends[32];
static int nsends;
static const char *expects[32], *fails[16];
static int nexpects, nfails, expect_next;
static size_t expect_pos;
static uint64_t max_cycles;
static int ddr_mode;                        /* 0 ok, 1 absent, 2 fail, 3 never */
static uint32_t ddr_calib_us = 150000, ddr_size = DDR_SIZE;
static uint32_t buttons;
static uint32_t ddr_fault_at = 1;           /* a DDR word that answers with a bus error (1 = none) */
static uint32_t ddr_stuck_at = 1;           /* a DDR word whose bit 4 is stuck at 1 (1 = none) */
static uint64_t card_insert_cycle;          /* 0 = present from the start (if --card) */
static struct sdsim *card;
static uint64_t traps_seen;

/* ---------------- CPU state ---------------- */
static uint32_t x[32], pc;
static uint32_t mstatus_ie;                 /* MIE (bit 3) and MPIE (bit 7); MPP reads 11 */
static uint32_t csr_mie, mtvec, mscratch, mepc, mcause, mtval, mcountinhibit;
static uint64_t mcycle, minstret;

/* ---------------- peripherals ---------------- */
static struct {
    uint8_t tx[16];
    int tx_n;
    uint64_t tx_busy_until;
    uint8_t rx[64];
    int rx_head, rx_n;
    uint32_t sticky, ctrl, div;
    uint64_t rx_next;
    char pending[4096];
    size_t pend_head, pend_len;
} uart;
static struct { uint64_t mtime_offset; uint64_t cmp; uint32_t msip; } timer;
static uint32_t intc_enable;
static struct { uint32_t led, rgb, edge, ie, prev_btn; } gpio;
static struct { int cs; uint32_t ctrl, div, sticky; uint64_t busy_until; uint8_t rx; int started; } spi;
static struct { uint32_t ctrl, cursor, scroll, ie, palette[16], sticky; } video;
static uint32_t sysinfo_scratch;

static void emu_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "surfemu: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, " (pc 0x%08x, %llu cycles)\n", pc, (unsigned long long)cycles);
    va_end(ap);
    exit(3);
}

static uint64_t now_us(void) { return cycles * SURF_TIMEBASE_HZ / sys_hz; }
static uint64_t mtime(void) { return now_us() + timer.mtime_offset; }

/* ---------------- console output and the expect/send script ---------------- */
static void finish(int code);
static int finishing;

static void out_char(char c)
{
    if (out_len + 2 > out_cap) {
        out_cap = out_cap ? out_cap * 2 : 1 << 16;
        out = realloc(out, out_cap);
        if (!out)
            emu_fail("out of memory");
    }
    out[out_len++] = c;
    out[out_len] = 0;
    if (!quiet) {
        fputc(c, stdout);
        fflush(stdout);
    }
    if (finishing)
        return;
    for (int i = 0; i < nsends; i++) {
        if (sends[i].done)
            continue;
        if (strstr(out + send_pos, sends[i].trigger)) {
            sends[i].done = 1;
            send_pos = out_len;
            size_t n = strlen(sends[i].text);
            for (size_t k = 0; k < n && uart.pend_len < sizeof uart.pending; k++)
                uart.pending[(uart.pend_head + uart.pend_len++) % sizeof uart.pending] = sends[i].text[k];
            if (!uart.rx_next || uart.rx_next < cycles)
                uart.rx_next = cycles + uart.div * 10u;
        }
        break;                              /* triggers fire in order */
    }
    for (int i = 0; i < nfails; i++)
        if (strstr(out, fails[i])) {
            fprintf(stderr, "\nsurfemu: output contains \"%s\"\n", fails[i]);
            finish(1);
        }
    while (expect_next < nexpects) {
        char *hit = strstr(out + expect_pos, expects[expect_next]);
        if (!hit)
            break;
        expect_pos = (size_t)(hit - out) + strlen(expects[expect_next]);
        expect_next++;
    }
    if (nexpects && expect_next == nexpects)
        finish(0);
}

/* ---------------- UART ---------------- */
static void uart_update(void)
{
    while (uart.tx_n && cycles >= uart.tx_busy_until) {
        char c = (char)uart.tx[0];          /* take it off the FIFO first: out_char may end the run */
        memmove(uart.tx, uart.tx + 1, (size_t)--uart.tx_n);
        if (uart.tx_n)
            uart.tx_busy_until += uart.div * 10u;
        out_char(c);
    }
    while (uart.pend_len && uart.rx_next && cycles >= uart.rx_next) {
        char c = uart.pending[uart.pend_head];
        uart.pend_head = (uart.pend_head + 1) % sizeof uart.pending;
        uart.pend_len--;
        if (uart.rx_n < 64)
            uart.rx[(uart.rx_head + uart.rx_n++) % 64] = (uint8_t)c;
        else
            uart.sticky |= SURF_UART_STATUS_RX_OVF_MASK;
        uart.rx_next = uart.pend_len ? uart.rx_next + uart.div * 10u : 0;
    }
}

static int uart_tx_idle(void) { return uart.tx_n == 0 && cycles >= uart.tx_busy_until; }

static uint32_t uart_status(void)
{
    return (uart.tx_n == 16 ? SURF_UART_STATUS_TX_FULL_MASK : 0) | (uart_tx_idle() ? SURF_UART_STATUS_TX_IDLE_MASK : 0)
         | (uart.rx_n ? SURF_UART_STATUS_RX_AVAIL_MASK : 0) | (uart.rx_n == 64 ? SURF_UART_STATUS_RX_FULL_MASK : 0)
         | uart.sticky;
}

static int uart_irq(void)
{
    return ((uart.ctrl & SURF_UART_CTRL_RX_IE_MASK) && uart.rx_n)
        || ((uart.ctrl & SURF_UART_CTRL_TX_IE_MASK) && uart.tx_n == 0)
        || ((uart.ctrl & SURF_UART_CTRL_ERR_IE_MASK) && uart.sticky);
}

/* ---------------- other peripherals ---------------- */
static int card_present(void)
{
    if (!card)
        return 0;
    if (card_insert_cycle && cycles < card_insert_cycle)
        return 0;
    return sdsim_present(card);
}

static uint32_t ddr_status(void)
{
    uint32_t st = SURF_SYSINFO_DDR_STATUS_PRESENT_MASK;
    switch (ddr_mode) {
    case 1: return 0;
    case 2: return st | (now_us() >= ddr_calib_us ? SURF_SYSINFO_DDR_STATUS_CALIB_FAIL_MASK : 0);
    case 3: return st;
    default: return st | (now_us() >= ddr_calib_us ? SURF_SYSINFO_DDR_STATUS_CALIB_DONE_MASK : 0);
    }
}

static int ddr_ready(void) { return (ddr_status() & SURF_SYSINFO_DDR_STATUS_CALIB_DONE_MASK) != 0; }

static void gpio_update(void)
{
    uint32_t rise = buttons & ~gpio.prev_btn;
    gpio.edge |= rise;
    gpio.prev_btn = buttons;
}

static int spi_busy(void) { return cycles < spi.busy_until; }

static uint32_t irq_sources(void)
{
    gpio_update();
    return (uart_irq() ? 1u << SURF_IRQ_UART : 0) | ((gpio.edge & gpio.ie) ? 1u << SURF_IRQ_GPIO : 0)
         | (((video.ie & 1u) && (video.sticky & 2u)) ? 1u << SURF_IRQ_VIDEO : 0)
         | (((spi.ctrl & SURF_SPI_CTRL_DONE_IE_MASK) && (spi.sticky & SURF_SPI_STATUS_DONE_MASK)) ? 1u << SURF_IRQ_SPI : 0);
}

static uint32_t mip(void)
{
    return ((irq_sources() & intc_enable) ? 0x800u : 0) | (mtime() >= timer.cmp ? 0x80u : 0) | (timer.msip ? 0x8u : 0);
}

/* MMIO: returns 0 on success, -1 for a bus error (unknown offset). */
static int mmio_read(uint32_t addr, uint32_t *v)
{
    uint32_t base = addr & ~0xFFFu, off = addr & 0xFFFu;
    uart_update();
    switch (base) {
    case SURF_UART_BASE:
        switch (off) {
        case SURF_UART_TXDATA: *v = uart.tx_n == 16 ? SURF_UART_TXDATA_FULL_MASK : 0; return 0;
        case SURF_UART_RXDATA:
            if (!uart.rx_n) { *v = SURF_UART_RXDATA_EMPTY_MASK; return 0; }
            *v = uart.rx[uart.rx_head];
            uart.rx_head = (uart.rx_head + 1) % 64;
            uart.rx_n--;
            return 0;
        case SURF_UART_STATUS: *v = uart_status(); return 0;
        case SURF_UART_CTRL: *v = uart.ctrl; return 0;
        case SURF_UART_DIV: *v = uart.div; return 0;
        case SURF_UART_LEVEL: *v = (uint32_t)uart.tx_n | ((uint32_t)uart.rx_n << 16); return 0;
        }
        return -1;
    case SURF_TIMER_BASE:
        switch (off) {
        case SURF_TIMER_MTIME_LO: *v = (uint32_t)mtime(); return 0;
        case SURF_TIMER_MTIME_HI: *v = (uint32_t)(mtime() >> 32); return 0;
        case SURF_TIMER_MTIMECMP_LO: *v = (uint32_t)timer.cmp; return 0;
        case SURF_TIMER_MTIMECMP_HI: *v = (uint32_t)(timer.cmp >> 32); return 0;
        case SURF_TIMER_MSIP: *v = timer.msip; return 0;
        case SURF_TIMER_TIMEBASE: *v = SURF_TIMEBASE_HZ; return 0;
        }
        return -1;
    case SURF_INTC_BASE: {
        uint32_t pend = irq_sources(), act = pend & intc_enable;
        switch (off) {
        case SURF_INTC_PENDING: *v = pend; return 0;
        case SURF_INTC_ENABLE: *v = intc_enable; return 0;
        case SURF_INTC_ACTIVE: *v = act; return 0;
        case SURF_INTC_NEXT:
            *v = 0xFFFFFFFFu;
            for (int i = 0; i < 32; i++)
                if (act & (1u << i)) { *v = (uint32_t)i; break; }
            return 0;
        }
        return -1;
    }
    case SURF_GPIO_BASE:
        gpio_update();
        switch (off) {
        case SURF_GPIO_LED: *v = gpio.led; return 0;
        case SURF_GPIO_RGB: *v = gpio.rgb; return 0;
        case SURF_GPIO_SW: *v = 0; return 0;
        case SURF_GPIO_BTN: *v = buttons & 0xFu; return 0;
        case SURF_GPIO_BTN_EDGE: *v = gpio.edge; return 0;
        case SURF_GPIO_IE: *v = gpio.ie; return 0;
        }
        return -1;
    case SURF_SPI_BASE:
        switch (off) {
        case SURF_SPI_DATA:
            if (spi_busy())
                cycles = spi.busy_until;    /* the read waits for the byte in flight */
            *v = spi.rx;
            return 0;
        case SURF_SPI_STATUS:
            if (spi.started && !spi_busy()) {
                spi.sticky |= SURF_SPI_STATUS_DONE_MASK;
                spi.started = 0;
            }
            *v = (spi_busy() ? SURF_SPI_STATUS_BUSY_MASK : 0) | (card_present() ? SURF_SPI_STATUS_CARD_MASK : 0)
               | spi.sticky;
            return 0;
        case SURF_SPI_CTRL: *v = spi.ctrl; return 0;
        case SURF_SPI_DIV: *v = spi.div; return 0;
        }
        return -1;
    case SURF_PS2_BASE:
        switch (off) {
        case SURF_PS2_DATA: *v = SURF_PS2_DATA_EMPTY_MASK; return 0;
        case SURF_PS2_STATUS: *v = 0; return 0;
        case SURF_PS2_CTRL: *v = SURF_PS2_CTRL_RX_EN_MASK; return 0;
        case SURF_PS2_TXDATA: *v = 0; return 0;
        case SURF_PS2_ERRCOUNT: *v = 0; return 0;
        }
        return -1;
    case SURF_VIDEO_BASE: {
        uint64_t frame = now_us() * 60u / 1000000u;
        if (off >= SURF_VIDEO_PALETTE && off < SURF_VIDEO_PALETTE + 4u * SURF_VIDEO_PALETTE_COUNT) {
            *v = video.palette[(off - SURF_VIDEO_PALETTE) / 4u];
            return 0;
        }
        switch (off) {
        case SURF_VIDEO_CTRL: *v = video.ctrl; return 0;
        case SURF_VIDEO_STATUS: *v = ((uint32_t)frame << 16) | video.sticky; return 0;
        case SURF_VIDEO_CURSOR: *v = video.cursor; return 0;
        case SURF_VIDEO_SCROLL: *v = video.scroll; return 0;
        case SURF_VIDEO_IE: *v = video.ie; return 0;
        case SURF_VIDEO_INFO: *v = 80u | (30u << 8) | (8u << 16) | (16u << 24); return 0;
        case SURF_VIDEO_PIXCLK: *v = SURF_PIX_HZ; return 0;
        }
        return -1;
    }
    case SURF_SYSINFO_BASE:
        switch (off) {
        case SURF_SYSINFO_MAGIC: *v = 0x46525553u; return 0;
        case SURF_SYSINFO_SOC_VERSION: *v = SURF_SOC_VERSION; return 0;
        case SURF_SYSINFO_MAP_VERSION: *v = SURF_MAP_VERSION; return 0;
        case SURF_SYSINFO_BOARD_ID: *v = SURF_BOARD_SIM; return 0;
        case SURF_SYSINFO_SYS_HZ: *v = sys_hz; return 0;
        case SURF_SYSINFO_BUILD_ID: *v = 0x5e5e5e5eu; return 0;
        case SURF_SYSINFO_BUILD_FLAGS: *v = SURF_SYSINFO_BUILD_FLAGS_SIM_MASK; return 0;
        case SURF_SYSINFO_FEATURES: *v = ddr_mode == 1 ? 0x7Fu : 0xFFu; return 0;
        case SURF_SYSINFO_DDR_STATUS: *v = ddr_status(); return 0;
        case SURF_SYSINFO_DDR_SIZE: *v = ddr_mode == 1 ? 0 : ddr_size; return 0;
        case SURF_SYSINFO_PIX_HZ: *v = SURF_PIX_HZ; return 0;
        case SURF_SYSINFO_SCRATCH: *v = sysinfo_scratch; return 0;
        case SURF_SYSINFO_BUILD_TIME: *v = 0; return 0;
        }
        return -1;
    }
    return -1;
}

static int mmio_write(uint32_t addr, uint32_t v)
{
    uint32_t base = addr & ~0xFFFu, off = addr & 0xFFFu;
    uart_update();
    switch (base) {
    case SURF_UART_BASE:
        switch (off) {
        case SURF_UART_TXDATA:
            if (uart.tx_n == 16) {
                uart.sticky |= SURF_UART_STATUS_TX_OVF_MASK;
                return 0;
            }
            if (uart.tx_n == 0 && cycles >= uart.tx_busy_until)
                uart.tx_busy_until = cycles + uart.div * 10u;
            uart.tx[uart.tx_n++] = (uint8_t)v;
            return 0;
        case SURF_UART_RXDATA: return 0;
        case SURF_UART_STATUS: uart.sticky &= ~(v & 0xF0u); return 0;
        case SURF_UART_CTRL: uart.ctrl = v & 7u; return 0;
        case SURF_UART_DIV: uart.div = (v & 0xFFFFu) < 16 ? 16 : (v & 0xFFFFu); return 0;
        case SURF_UART_LEVEL: return 0;
        }
        return -1;
    case SURF_TIMER_BASE:
        switch (off) {
        case SURF_TIMER_MTIME_LO: timer.mtime_offset = ((mtime() & ~0xFFFFFFFFull) | v) - now_us(); return 0;
        case SURF_TIMER_MTIME_HI: timer.mtime_offset = ((uint64_t)v << 32 | (mtime() & 0xFFFFFFFFu)) - now_us(); return 0;
        case SURF_TIMER_MTIMECMP_LO: timer.cmp = (timer.cmp & ~0xFFFFFFFFull) | v; return 0;
        case SURF_TIMER_MTIMECMP_HI: timer.cmp = (timer.cmp & 0xFFFFFFFFull) | ((uint64_t)v << 32); return 0;
        case SURF_TIMER_MSIP: timer.msip = v & 1u; return 0;
        case SURF_TIMER_TIMEBASE: return 0;
        }
        return -1;
    case SURF_INTC_BASE:
        switch (off) {
        case SURF_INTC_ENABLE: intc_enable = v; return 0;
        case SURF_INTC_PENDING: case SURF_INTC_ACTIVE: case SURF_INTC_NEXT: return 0;
        }
        return -1;
    case SURF_GPIO_BASE:
        switch (off) {
        case SURF_GPIO_LED: gpio.led = v & 0xFu; return 0;
        case SURF_GPIO_RGB: gpio.rgb = v & 0xFFFu; return 0;
        case SURF_GPIO_BTN_EDGE: gpio.edge &= ~(v & 0xFu); return 0;
        case SURF_GPIO_IE: gpio.ie = v & 0xFu; return 0;
        case SURF_GPIO_SW: case SURF_GPIO_BTN: return 0;
        }
        return -1;
    case SURF_SPI_BASE:
        switch (off) {
        case SURF_SPI_DATA:
            if (spi_busy()) {
                spi.sticky |= SURF_SPI_STATUS_OVR_MASK;
                return 0;
            }
            spi.rx = (card && card_present()) ? sdsim_xfer(card, spi.cs, (uint8_t)v) : 0xFF;
            spi.busy_until = cycles + 16u * (spi.div + 1u);
            spi.started = 1;
            return 0;
        case SURF_SPI_STATUS: spi.sticky &= ~(v & 0xCu); return 0;
        case SURF_SPI_CTRL:
            spi.ctrl = v & 3u;
            spi.cs = (int)(v & SURF_SPI_CTRL_CS_MASK);
            return 0;
        case SURF_SPI_DIV:
            spi.div = v & 0xFFu;
            if (card)
                sdsim_set_div(card, spi.div);
            return 0;
        }
        return -1;
    case SURF_PS2_BASE:
        switch (off) {
        case SURF_PS2_DATA: case SURF_PS2_STATUS: case SURF_PS2_CTRL: case SURF_PS2_TXDATA: case SURF_PS2_ERRCOUNT:
            return 0;
        }
        return -1;
    case SURF_VIDEO_BASE:
        if (off >= SURF_VIDEO_PALETTE && off < SURF_VIDEO_PALETTE + 4u * SURF_VIDEO_PALETTE_COUNT) {
            video.palette[(off - SURF_VIDEO_PALETTE) / 4u] = v & 0xFFFu;
            return 0;
        }
        switch (off) {
        case SURF_VIDEO_CTRL: video.ctrl = v & 0x1Fu; return 0;
        case SURF_VIDEO_STATUS: video.sticky &= ~(v & 2u); return 0;
        case SURF_VIDEO_CURSOR: video.cursor = v & 0x1F7Fu; return 0;
        case SURF_VIDEO_SCROLL: video.scroll = v & 0x1Fu; return 0;
        case SURF_VIDEO_IE: video.ie = v & 1u; return 0;
        case SURF_VIDEO_INFO: case SURF_VIDEO_PIXCLK: return 0;
        }
        return -1;
    case SURF_SYSINFO_BASE:
        if (off == SURF_SYSINFO_SCRATCH) {
            sysinfo_scratch = v;
            return 0;
        }
        return off <= SURF_SYSINFO_BUILD_TIME && !(off & 3u) ? 0 : -1;
    }
    return -1;
}

/* ---------------- the bus ---------------- */
enum { REG_NONE, REG_ROM, REG_RAM, REG_TEXT, REG_DDR, REG_MMIO };

static int region(uint32_t a)
{
    if (a < SURF_BOOTROM_BASE + ROM_SIZE) return REG_ROM;
    if (a >= SURF_SCRATCH_BASE && a < SURF_SCRATCH_BASE + RAM_SIZE) return REG_RAM;
    if (a >= SURF_TEXTRAM_BASE && a < SURF_TEXTRAM_BASE + TEXT_SIZE) return REG_TEXT;
    if (a >= SURF_DDR_BASE && a - SURF_DDR_BASE < DDR_SIZE) return REG_DDR;
    if (a >= SURF_UART_BASE && a < SURF_SYSINFO_BASE + SURF_SYSINFO_SIZE) return REG_MMIO;
    return REG_NONE;
}

static uint8_t *mem_ptr(int r, uint32_t a)
{
    switch (r) {
    case REG_ROM: return rom + (a - SURF_BOOTROM_BASE);
    case REG_RAM: return ram + (a - SURF_SCRATCH_BASE);
    case REG_TEXT: return textram + (a - SURF_TEXTRAM_BASE);
    case REG_DDR: return ddr + (a - SURF_DDR_BASE);
    default: return 0;
    }
}

/* size 1, 2, 4; the caller has checked alignment. 0 = ok, -1 = bus error. */
static int bus_read(uint32_t a, int size, uint32_t *v, int fetch)
{
    int r = region(a);
    if (r == REG_NONE)
        return -1;
    if (fetch && (r == REG_TEXT || r == REG_MMIO))
        return -1;
    if (r == REG_DDR) {
        if (!ddr_ready() || a - SURF_DDR_BASE >= ddr_size || (a & ~3u) == ddr_fault_at)
            return -1;
        cycles += DDR_EXTRA;
    }
    if (r == REG_MMIO)
        return size == 4 ? mmio_read(a, v) : -1;
    uint8_t *p = mem_ptr(r, a);
    *v = size == 1 ? p[0] : size == 2 ? (uint32_t)(p[0] | p[1] << 8)
       : (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return 0;
}

static int bus_write(uint32_t a, int size, uint32_t v)
{
    int r = region(a);
    if (r == REG_NONE || r == REG_ROM)
        return -1;
    if (r == REG_DDR) {
        if (!ddr_ready() || a - SURF_DDR_BASE >= ddr_size || (a & ~3u) == ddr_fault_at)
            return -1;
        cycles += DDR_EXTRA;
    }
    if (r == REG_MMIO)
        return size == 4 ? mmio_write(a, v) : -1;
    uint8_t *p = mem_ptr(r, a);
    for (int i = 0; i < size; i++)
        p[i] = (uint8_t)(v >> (8 * i));
    if (r == REG_DDR && (a & ~3u) == ddr_stuck_at)
        ddr[ddr_stuck_at - SURF_DDR_BASE] |= 0x10;          /* a data bit that cannot hold 0 */
    return 0;
}

/* ---------------- CSRs (docs/ISA.md section 2) ---------------- */
static int csr_known(uint32_t n)
{
    switch (n) {
    case 0xF11: case 0xF12: case 0xF13: case 0xF14: case 0xF15:
    case 0x300: case 0x301: case 0x304: case 0x305: case 0x310: case 0x320:
    case 0x340: case 0x341: case 0x342: case 0x343: case 0x344:
    case 0xB00: case 0xB02: case 0xB80: case 0xB82:
    case 0xC00: case 0xC01: case 0xC02: case 0xC80: case 0xC81: case 0xC82:
        return 1;
    }
    if ((n >= 0x323 && n <= 0x33F) || (n >= 0x3A0 && n <= 0x3EF) || (n >= 0xB03 && n <= 0xB1F) ||
        (n >= 0xB83 && n <= 0xB9F) || (n >= 0xC03 && n <= 0xC1F) || (n >= 0xC83 && n <= 0xC9F))
        return 1;
    return 0;
}

static uint32_t csr_read(uint32_t n)
{
    switch (n) {
    case 0xF13: return SURF_SOC_VERSION;
    case 0x300: return mstatus_ie | 0x1800u;
    case 0x301: return 0x40001100u;
    case 0x304: return csr_mie;
    case 0x305: return mtvec;
    case 0x320: return mcountinhibit;
    case 0x340: return mscratch;
    case 0x341: return mepc;
    case 0x342: return mcause;
    case 0x343: return mtval;
    case 0x344: return mip();
    case 0xB00: case 0xC00: return (uint32_t)mcycle;
    case 0xB80: case 0xC80: return (uint32_t)(mcycle >> 32);
    case 0xB02: case 0xC02: return (uint32_t)minstret;
    case 0xB82: case 0xC82: return (uint32_t)(minstret >> 32);
    case 0xC01: return (uint32_t)mtime();
    case 0xC81: return (uint32_t)(mtime() >> 32);
    default: return 0;
    }
}

static int instret_written, cycle_written;

static void csr_write(uint32_t n, uint32_t v)
{
    switch (n) {
    case 0x300: mstatus_ie = v & 0x88u; break;
    case 0x304: csr_mie = v & 0x888u; break;
    case 0x305: mtvec = v & ~3u; break;
    case 0x320: mcountinhibit = v & 5u; break;
    case 0x340: mscratch = v; break;
    case 0x341: mepc = v & ~3u; break;
    case 0x342: mcause = v & 0x8000001Fu; break;
    case 0x343: mtval = v; break;
    case 0xB00: mcycle = (mcycle & ~0xFFFFFFFFull) | v; cycle_written = 1; break;
    case 0xB80: mcycle = (mcycle & 0xFFFFFFFFull) | ((uint64_t)v << 32); cycle_written = 1; break;
    case 0xB02: minstret = (minstret & ~0xFFFFFFFFull) | v; instret_written = 1; break;
    case 0xB82: minstret = (minstret & 0xFFFFFFFFull) | ((uint64_t)v << 32); instret_written = 1; break;
    default: break;                         /* misa, mstatush, mip, PMP, hpm: writes ignored */
    }
}

/* ---------------- traps ---------------- */
static int trapped;

static void take_trap(uint32_t cause, uint32_t tval, uint32_t epc)
{
    traps_seen++;
    if (trace_traps)
        fprintf(stderr, "[trap mcause=%08x mepc=%08x mtval=%08x mtvec=%08x at %llu us]\n", cause, epc, tval, mtvec,
                (unsigned long long)now_us());
    mepc = epc & ~3u;
    mcause = cause;
    mtval = tval;
    uint32_t mie_bit = mstatus_ie & 0x8u;
    mstatus_ie = (mie_bit ? 0x80u : 0);     /* MPIE = MIE, MIE = 0 */
    pc = mtvec & ~3u;
    trapped = 1;
}

#define EXC(cause, tval) do { take_trap((cause), (tval), pc); return; } while (0)

static int32_t sx(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static void execute(void);

static void step(void)
{
    uint64_t c0 = cycles;
    trapped = 0;
    instret_written = cycle_written = 0;
    execute();
    if (cycles == c0)
        cycles++;                           /* a trap or an interrupt entry takes a cycle too */
    if (!(mcountinhibit & 1u) && !cycle_written)
        mcycle += cycles - c0;              /* mcycle counts sys_clk cycles */
    uart_update();
}

static void execute(void)
{
    if (mstatus_ie & 0x8u) {
        uint32_t pending = mip() & csr_mie;
        if (pending) {
            uint32_t cause = (pending & 0x800u) ? 11 : (pending & 0x8u) ? 3 : 7;
            take_trap(0x80000000u | cause, 0, pc);
            return;
        }
    }
    uint32_t insn;
    if (bus_read(pc, 4, &insn, 1) != 0)
        EXC(1, pc);
    cycles++;
    uint32_t op = insn & 0x7F, rd = (insn >> 7) & 31, f3 = (insn >> 12) & 7, rs1 = (insn >> 15) & 31;
    uint32_t rs2 = (insn >> 20) & 31, f7 = insn >> 25;
    uint32_t a = x[rs1], b = x[rs2], next = pc + 4, res = 0;
    int wr = 0;
    if ((insn & 3u) != 3u)
        EXC(2, insn);
    switch (op) {
    case 0x37: res = insn & 0xFFFFF000u; wr = 1; break;
    case 0x17: res = pc + (insn & 0xFFFFF000u); wr = 1; break;
    case 0x6F: {
        uint32_t imm = (uint32_t)sx(((insn >> 31) << 20) | (((insn >> 12) & 0xFF) << 12) | (((insn >> 20) & 1) << 11)
                                    | (((insn >> 21) & 0x3FF) << 1), 21);
        uint32_t t = pc + imm;
        if (t & 3u)
            EXC(0, t);
        res = pc + 4;
        wr = 1;
        next = t;
        cycles += 2;
        break;
    }
    case 0x67: {
        if (f3)
            EXC(2, insn);
        uint32_t t = (a + (uint32_t)sx(insn >> 20, 12)) & ~1u;
        if (t & 3u)
            EXC(0, t);
        res = pc + 4;
        wr = 1;
        next = t;
        cycles += 2;
        break;
    }
    case 0x63: {
        int take;
        switch (f3) {
        case 0: take = a == b; break;
        case 1: take = a != b; break;
        case 4: take = (int32_t)a < (int32_t)b; break;
        case 5: take = (int32_t)a >= (int32_t)b; break;
        case 6: take = a < b; break;
        case 7: take = a >= b; break;
        default: EXC(2, insn);
        }
        if (take) {
            uint32_t imm = (uint32_t)sx(((insn >> 31) << 12) | (((insn >> 7) & 1) << 11) | (((insn >> 25) & 0x3F) << 5)
                                        | (((insn >> 8) & 0xF) << 1), 13);
            uint32_t t = pc + imm;
            if (t & 3u)
                EXC(0, t);
            next = t;
            cycles += 2;
        }
        break;
    }
    case 0x03: {
        uint32_t addr = a + (uint32_t)sx(insn >> 20, 12), v;
        int size;
        switch (f3) {
        case 0: case 4: size = 1; break;
        case 1: case 5: size = 2; break;
        case 2: size = 4; break;
        default: EXC(2, insn);
        }
        if (addr & (uint32_t)(size - 1))
            EXC(4, addr);
        if (bus_read(addr, size, &v, 0) != 0)
            EXC(5, addr);
        res = f3 == 0 ? (uint32_t)sx(v, 8) : f3 == 1 ? (uint32_t)sx(v, 16) : v;
        wr = 1;
        break;
    }
    case 0x23: {
        uint32_t addr = a + (uint32_t)sx(((insn >> 25) << 5) | ((insn >> 7) & 31), 12);
        int size = f3 == 0 ? 1 : f3 == 1 ? 2 : f3 == 2 ? 4 : 0;
        if (!size)
            EXC(2, insn);
        if (addr & (uint32_t)(size - 1))
            EXC(6, addr);
        if (bus_write(addr, size, b) != 0)
            EXC(7, addr);
        break;
    }
    case 0x13: {
        uint32_t imm = (uint32_t)sx(insn >> 20, 12), sh = rs2;
        switch (f3) {
        case 0: res = a + imm; break;
        case 2: res = (int32_t)a < (int32_t)imm; break;
        case 3: res = a < imm; break;
        case 4: res = a ^ imm; break;
        case 6: res = a | imm; break;
        case 7: res = a & imm; break;
        case 1: if (f7) EXC(2, insn); res = a << sh; break;
        case 5:
            if (f7 == 0) res = a >> sh;
            else if (f7 == 0x20) res = (uint32_t)((int32_t)a >> sh);
            else EXC(2, insn);
            break;
        }
        wr = 1;
        break;
    }
    case 0x33:
        if (f7 == 0) {
            switch (f3) {
            case 0: res = a + b; break;
            case 1: res = a << (b & 31); break;
            case 2: res = (int32_t)a < (int32_t)b; break;
            case 3: res = a < b; break;
            case 4: res = a ^ b; break;
            case 5: res = a >> (b & 31); break;
            case 6: res = a | b; break;
            case 7: res = a & b; break;
            }
        } else if (f7 == 0x20 && (f3 == 0 || f3 == 5)) {
            res = f3 == 0 ? a - b : (uint32_t)((int32_t)a >> (b & 31));
        } else if (f7 == 1) {
            int32_t sa = (int32_t)a, sb = (int32_t)b;
            switch (f3) {
            case 0: res = a * b; break;
            case 1: res = (uint32_t)(((int64_t)sa * (int64_t)sb) >> 32); break;
            case 2: res = (uint32_t)(((int64_t)sa * (int64_t)(uint64_t)b) >> 32); break;
            case 3: res = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); break;
            case 4: res = b == 0 ? 0xFFFFFFFFu : (sa == INT32_MIN && sb == -1) ? a : (uint32_t)(sa / sb); break;
            case 5: res = b == 0 ? 0xFFFFFFFFu : a / b; break;
            case 6: res = b == 0 ? a : (sa == INT32_MIN && sb == -1) ? 0 : (uint32_t)(sa % sb); break;
            case 7: res = b == 0 ? a : a % b; break;
            }
            cycles += (f3 >= 4) ? 33 : 2;
        } else {
            EXC(2, insn);
        }
        wr = 1;
        break;
    case 0x0F:
        if (f3 != 0 && f3 != 1)
            EXC(2, insn);
        break;                              /* FENCE, FENCE.I: no caches here */
    case 0x73: {
        if (f3 == 0) {
            switch (insn) {
            case 0x00000073: EXC(11, 0);
            case 0x00100073: EXC(3, pc);
            case 0x30200073:
                next = mepc;
                mstatus_ie = ((mstatus_ie & 0x80u) ? 0x8u : 0) | 0x80u;     /* MIE = MPIE, MPIE = 1 */
                break;
            case 0x10500073:
                while (!(mip() & csr_mie)) {   /* WFI: skip ahead to the next event that can wake us */
                    uint64_t until = cycles + sys_hz / 1000u;
                    if ((csr_mie & 0x80u) && timer.cmp != ~0ull) {
                        uint64_t t = (timer.cmp - timer.mtime_offset) * sys_hz / SURF_TIMEBASE_HZ + 1;
                        if (t > cycles && t < until)
                            until = t;
                    }
                    cycles = until;
                    uart_update();
                    if (max_cycles && cycles >= max_cycles)
                        return;
                }
                break;
            default:
                EXC(2, insn);
            }
            break;
        }
        if (f3 == 4)
            EXC(2, insn);
        uint32_t n = insn >> 20;
        uint32_t src = (f3 & 4) ? rs1 : a;
        int writes = (f3 == 1 || f3 == 5) || rs1 != 0;
        if (!csr_known(n) || (writes && (n >> 10) == 3))
            EXC(2, insn);
        uint32_t old = csr_read(n);
        if (writes) {
            uint32_t nv = (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? old | src : old & ~src;
            csr_write(n, nv);
        }
        res = old;
        wr = 1;
        break;
    }
    default:
        EXC(2, insn);
    }
    if (wr && rd)
        x[rd] = res;
    pc = next;
    if (!(mcountinhibit & 4u) && !instret_written)
        minstret++;
}

/* ---------------- loading, screen dump, main ---------------- */
static void load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        emu_fail("cannot open %s", path);
    size_t n = strlen(path);
    if (n > 4 && !strcmp(path + n - 4, ".mem")) {
        char line[64];
        uint32_t i = 0;
        while (fgets(line, sizeof line, f)) {
            if (i >= ROM_SIZE / 4)
                emu_fail("%s has more than %u lines", path, ROM_SIZE / 4);
            unsigned long w = strtoul(line, 0, 16);
            for (int k = 0; k < 4; k++)
                rom[4 * i + k] = (uint8_t)(w >> (8 * k));
            i++;
        }
        if (i != ROM_SIZE / 4)
            emu_fail("%s has %u lines; a boot ROM image has %u", path, i, ROM_SIZE / 4);
    } else if (fread(rom, 1, ROM_SIZE, f) == 0) {
        emu_fail("%s is empty", path);
    }
    fclose(f);
}

static void dump_screen(void)
{
    if (!screen_file)
        return;
    FILE *f = fopen(screen_file, "w");
    if (!f)
        return;
    for (unsigned r = 0; r < 30; r++) {
        char row[81];
        unsigned tr = (r + (video.scroll >= 30 ? 0 : video.scroll)) % 30;
        for (unsigned c = 0; c < 80; c++) {
            uint8_t g = textram[4 * (tr * 80 + c)];
            row[c] = (g >= 0x20 && g < 0x7F) ? (char)g : (g ? '?' : ' ');
        }
        int len = 80;
        while (len > 0 && row[len - 1] == ' ')
            len--;
        row[len] = 0;
        fprintf(f, "%s\n", row);
    }
    fprintf(f, "# ctrl %02x cursor row %u col %u scroll %u\n", video.ctrl, (video.cursor >> 8) & 31, video.cursor & 127,
            video.scroll);
    fclose(f);
}

static void finish(int code)
{
    if (finishing)
        return;
    finishing = 1;
    while (uart.tx_n) {                     /* let the last bytes out */
        cycles = uart.tx_busy_until;
        uart_update();
    }
    dump_screen();
    fprintf(stderr, "\nsurfemu: %s after %.3f s simulated (%llu cycles, %llu instructions, %llu traps); pc %08x; "
            "LEDs %x RGB %03x\n", code == 0 ? "all expected output seen" : code == 1 ? "failure text seen"
            : "time limit reached", (double)cycles / sys_hz, (unsigned long long)cycles,
            (unsigned long long)minstret, (unsigned long long)traps_seen, pc, gpio.led, gpio.rgb);
    if (code == 2 && nexpects)
        fprintf(stderr, "surfemu: still waiting for \"%s\"\n", expects[expect_next]);
    sdsim_close(card);
    free(ddr);
    free(out);
    exit(code);
}

static void unescape(char *dst, size_t size, const char *s)
{
    size_t n = 0;
    for (; *s && n + 1 < size; s++) {
        if (*s == '\\' && s[1] == 'x' && s[2] && s[3]) {
            char hex[3] = { s[2], s[3], 0 };
            dst[n++] = (char)strtoul(hex, 0, 16);
            s += 3;
        } else if (*s == '\\' && s[1]) {
            s++;
            *s == 'n' ? (dst[n++] = '\n') : *s == 'r' ? (dst[n++] = '\r') : *s == 'e' ? (dst[n++] = 27)
                : *s == 't' ? (dst[n++] = '\t') : (dst[n++] = *s);
        } else {
            dst[n++] = *s;
        }
    }
    dst[n] = 0;
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "rom", required_argument, 0, 'r' }, { "card", required_argument, 0, 'c' },
        { "type", required_argument, 0, 't' }, { "absent", no_argument, 0, 'A' },
        { "acmd41-busy", required_argument, 0, 'a' }, { "crc-every", required_argument, 0, 'E' },
        { "remove-after", required_argument, 0, 'R' }, { "no-cd", no_argument, 0, 'N' },
        { "ddr", required_argument, 0, 'd' }, { "ddr-calib-ms", required_argument, 0, 'D' },
        { "btn", required_argument, 0, 'b' }, { "send", required_argument, 0, 's' },
        { "expect", required_argument, 0, 'e' }, { "fail-on", required_argument, 0, 'f' },
        { "max-ms", required_argument, 0, 'm' }, { "screen", required_argument, 0, 'S' },
        { "trace-traps", no_argument, 0, 'T' }, { "quiet", no_argument, 0, 'q' },
        { "card-insert-ms", required_argument, 0, 'I' }, { "ddr-fault-at", required_argument, 0, 'F' },
        { "ddr-size", required_argument, 0, 'Z' }, { "ddr-stuck-at", required_argument, 0, 'K' },
        { 0, 0, 0, 0 },
    };
    struct sdsim_opts co;
    sdsim_default_opts(&co);
    const char *rom_path = 0, *card_path = 0;
    uint64_t max_ms = 20000, insert_ms = 0;
    int c;
    while ((c = getopt_long(argc, argv, "", opts, 0)) != -1) {
        switch (c) {
        case 'r': rom_path = optarg; break;
        case 'c': card_path = optarg; break;
        case 't':
            co.type = !strcmp(optarg, "sdsc2") ? SDSIM_SDSC2 : !strcmp(optarg, "sdsc1") ? SDSIM_SDSC1
                    : !strcmp(optarg, "mmc") ? SDSIM_MMC : SDSIM_SDHC;
            break;
        case 'A': co.absent = 1; break;
        case 'a': co.acmd41_busy = atoi(optarg); break;
        case 'E': co.crc_every = atoi(optarg); break;
        case 'R': co.remove_after = atoi(optarg); break;
        case 'N': co.no_cd = 1; break;
        case 'd':
            ddr_mode = !strcmp(optarg, "absent") ? 1 : !strcmp(optarg, "fail") ? 2 : !strcmp(optarg, "never") ? 3 : 0;
            break;
        case 'D': ddr_calib_us = (uint32_t)strtoul(optarg, 0, 0) * 1000u; break;
        case 'b': buttons = (uint32_t)strtoul(optarg, 0, 0); break;
        case 's': {
            if (nsends >= 32) break;
            char *eq = strchr(optarg, '=');
            if (!eq) emu_fail("--send needs TRIGGER=TEXT");
            *eq = 0;
            sends[nsends].trigger = optarg;
            unescape(sends[nsends].text, sizeof sends[nsends].text, eq + 1);
            nsends++;
            break;
        }
        case 'e': if (nexpects < 32) expects[nexpects++] = optarg; break;
        case 'f': if (nfails < 16) fails[nfails++] = optarg; break;
        case 'm': max_ms = strtoull(optarg, 0, 0); break;
        case 'S': screen_file = optarg; break;
        case 'T': trace_traps = 1; break;
        case 'q': quiet = 1; break;
        case 'I': insert_ms = strtoull(optarg, 0, 0); break;
        case 'F': ddr_fault_at = (uint32_t)strtoul(optarg, 0, 0) & ~3u; break;
        case 'Z': ddr_size = (uint32_t)strtoul(optarg, 0, 0); break;
        case 'K': ddr_stuck_at = (uint32_t)strtoul(optarg, 0, 0) & ~3u; break;
        default: return 3;
        }
    }
    if (!rom_path)
        emu_fail("--rom is required");
    load_rom(rom_path);
    ddr = calloc(1, DDR_SIZE);
    if (!ddr)
        emu_fail("no memory for DDR");
    if (card_path || co.absent || co.no_cd) {
        card = sdsim_open(card_path, &co);
        if (!card)
            return 3;
    }
    max_cycles = max_ms * (sys_hz / 1000u);
    card_insert_cycle = insert_ms * (sys_hz / 1000u);
    uart.div = (sys_hz + 57600u) / 115200u;
    timer.cmp = ~0ull;
    spi.div = 31;
    video.ctrl = SURF_VIDEO_CTRL_ENABLE_MASK;
    pc = SURF_RESET_PC;
    for (int i = 1; i < 32; i++)
        x[i] = 0xDEADBEEFu;                 /* the register file starts undefined */
    while (cycles < max_cycles)
        step();
    finish(2);
    return 2;
}
