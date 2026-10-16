/*
SurfOS board: SurfCPU on the Digilent Arty A7
--------------------
File: board.c   Date: 10/15/26 (SurfCPU)
--------------------
The HAL for our own SoC, written to surfcpu/docs/MEMORY-MAP.md with every address and
bit from the generated surfcpu/firmware/include/surfcpu/soc.h (never typed by hand):
surf_uart (the console), surf_timer (mtime at sysinfo's timebase, 1 MHz), surf_intc
(level sources ORed into mip.MEIP, no claim or complete), surf_gpio (LEDs, switches,
buttons) and surf_sysinfo (who and what the board is). The microSD card
(boards/arty_a7/sd.c), the PS/2 keyboard (ps2kbd.c) and the 80x30 text display
(textcon.c) have files of their own.

Nothing here has run on the FPGA yet: the board is not connected (milestone 7's physical
tests are pending). What is tested is the logic that can be tested on a host.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/system.h>
#include <surfos/irq.h>
#include <sys/driver.h>
#include <sys/bdev.h>
#include <asm/io.h>
#include <surfcpu/soc.h>
#include <surfcpu/bootabi.h>
#include <riscv/board.h>
#include <riscv/csr.h>
#include "arty.h"
#include "../../shell/shell.h"
#include <blibc_common.h>

#define REG(base, off) ((u_long)(base) + (off))

const char *board_name(void) { return "arty_a7"; }
const char *board_machine(void) { return "SurfCPU on the Arty A7"; }

static bool is_surfcpu;             /* sysinfo answers with its magic: this is our SoC */
static u32 features;                /* sysinfo.FEATURES */

/**** the UART: surf_uart ****/

void board_early_init(void) {
    is_surfcpu = mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_MAGIC)) == 0x46525553u;
    features = is_surfcpu ? mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_FEATURES)) : 0;
    if(!(features & SURF_SYSINFO_FEATURES_UART_MASK)) return;
    mmio_write32(REG(SURF_UART_BASE, SURF_UART_CTRL), 0);           /* no interrupts until init_serial_irq() */
    mmio_write32(REG(SURF_UART_BASE, SURF_UART_STATUS), SURF_UART_STATUS_RX_OVF_MASK | SURF_UART_STATUS_RX_FRAME_MASK |
                 SURF_UART_STATUS_TX_OVF_MASK | SURF_UART_STATUS_RX_BREAK_MASK);   /* SurfBoot left 115200 8N1 */
}

bool board_uart_present(void) {
    return (features & SURF_SYSINFO_FEATURES_UART_MASK) != 0;
}

/* bounded: at 115200 baud a byte takes 87 us, the FIFO holds 16 */
void board_uart_putc(char c) {
    u_int spins;
    for(spins = 0; spins < 200000; spins++) {
        if(!(mmio_read32(REG(SURF_UART_BASE, SURF_UART_TXDATA)) & SURF_UART_TXDATA_FULL_MASK)) break;
    }
    mmio_write32(REG(SURF_UART_BASE, SURF_UART_TXDATA), (u8)c);
}

int board_uart_getc(void) {
    u32 v = mmio_read32(REG(SURF_UART_BASE, SURF_UART_RXDATA));     /* pops the FIFO */
    if(v & SURF_UART_RXDATA_EMPTY_MASK) return -1;
    return (int)(v & SURF_UART_RXDATA_DATA_MASK);
}

int board_uart_irq(void) { return SURF_IRQ_UART; }

void board_uart_rx_irq(bool on) {
    mmio_write32(REG(SURF_UART_BASE, SURF_UART_CTRL), on ? SURF_UART_CTRL_RX_IE_MASK : 0);
}

const char *board_uart_describe(char *buf, size_t size) {
    u32 div = mmio_read32(REG(SURF_UART_BASE, SURF_UART_DIV)) & SURF_UART_DIV_DIV_MASK;
    u32 hz = arty_sys_hz();
    snprintf(buf, size, "surf_uart at 0x%08lx, %lu baud (divisor %lu at %lu Hz), intc source %d",
             (u_long)SURF_UART_BASE, div ? (u_long)(hz / div) : 0UL, (u_long)div, (u_long)hz, SURF_IRQ_UART);
    return buf;
}

/**** the machine timer: surf_timer ****/

u64 board_mtime(void) {
    u32 hi, lo, hi2;
    do {
        hi = mmio_read32(REG(SURF_TIMER_BASE, SURF_TIMER_MTIME_HI));
        lo = mmio_read32(REG(SURF_TIMER_BASE, SURF_TIMER_MTIME_LO));
        hi2 = mmio_read32(REG(SURF_TIMER_BASE, SURF_TIMER_MTIME_HI));
    } while(hi != hi2);
    return ((u64)hi << 32) | lo;
}

/* MEMORY-MAP.md: lo = all ones, hi, lo, so it never passes through a value that fires early */
void board_set_mtimecmp(u64 when) {
    mmio_write32(REG(SURF_TIMER_BASE, SURF_TIMER_MTIMECMP_LO), 0xFFFFFFFFu);
    mmio_write32(REG(SURF_TIMER_BASE, SURF_TIMER_MTIMECMP_HI), (u32)(when >> 32));
    mmio_write32(REG(SURF_TIMER_BASE, SURF_TIMER_MTIMECMP_LO), (u32)when);
}

u32 board_timebase_hz(void) {
    u32 hz = mmio_read32(REG(SURF_TIMER_BASE, SURF_TIMER_TIMEBASE));
    return hz ? hz : SURF_TIMEBASE_HZ;
}

/**** interrupts: surf_intc (levels, no claim/complete) ****/

void board_irq_init(void) {
    mmio_write32(REG(SURF_INTC_BASE, SURF_INTC_ENABLE), 0);
}

void board_irq_enable(u_int irq, bool on) {
    u_long r = REG(SURF_INTC_BASE, SURF_INTC_ENABLE);
    if(irq >= 32) return;
    mmio_write32(r, on ? (mmio_read32(r) | (1u << irq)) : (mmio_read32(r) & ~(1u << irq)));
}

/* the lowest active source; it stays active until its device is served (a level) */
int board_irq_claim(void) {
    u32 next = mmio_read32(REG(SURF_INTC_BASE, SURF_INTC_NEXT));
    return next == 0xFFFFFFFFu ? -1 : (int)next;
}

void board_irq_complete(u_int irq) {
    (void)irq;
}

const char *board_irq_describe(char *buf, size_t size) {
    snprintf(buf, size, "surf_intc at 0x%08lx, %d level sources, enabled 0x%02lx, pending 0x%02lx",
             (u_long)SURF_INTC_BASE, SURF_IRQ_COUNT, (u_long)mmio_read32(REG(SURF_INTC_BASE, SURF_INTC_ENABLE)),
             (u_long)mmio_read32(REG(SURF_INTC_BASE, SURF_INTC_PENDING)));
    return buf;
}

/**** the board's identity: sysinfo, and what SurfBoot said ****/

static const struct surf_boot_info *boot;

void board_boot_info(const struct surf_boot_info *bi) {
    boot = bi;
}

u32 arty_sys_hz(void) {
    u32 hz = is_surfcpu ? mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_SYS_HZ)) : 0;
    return hz ? hz : SURF_SYS_HZ;
}

bool arty_has(u32 feature_mask) {
    return (features & feature_mask) != 0;
}

void board_print_info(void) {
    u32 flags;
    if(!is_surfcpu) {
        printf("  sysinfo: no SurfCPU magic at 0x%08lx: not our SoC?\n", (u_long)SURF_SYSINFO_BASE);
        return;
    }
    flags = mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_BUILD_FLAGS));
    printf("  sysinfo: board 0x%04lx, SoC 0x%06lx, map %lu, sys clock %lu Hz, bitstream %08lx%s%s, features 0x%02lx\n",
           (u_long)mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_BOARD_ID)),
           (u_long)mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_SOC_VERSION)),
           (u_long)mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_MAP_VERSION)), (u_long)arty_sys_hz(),
           (u_long)mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_BUILD_ID)),
           (flags & SURF_SYSINFO_BUILD_FLAGS_DIRTY_MASK) ? " (dirty)" : "",
           (flags & SURF_SYSINFO_BUILD_FLAGS_SIM_MASK) ? " (simulation)" : "", (u_long)features);
    printf("  DDR status 0x%lx, %lu MB; pixel clock %lu Hz\n",
           (u_long)mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_DDR_STATUS)),
           (u_long)(mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_DDR_SIZE)) >> 20),
           (u_long)mmio_read32(REG(SURF_SYSINFO_BASE, SURF_SYSINFO_PIX_HZ)));
    if(!boot) printf("  (no SurfBoot boot information: RAM is the linker script's)\n");
}

/* no real-time clock on the Arty */
bool board_rtc(u64 *seconds) {
    (void)seconds;
    return false;
}

/**** the end ****/

/* SurfBoot again from the reset vector, interrupts off, nothing pending enabled. It is a
   warm start: the devices keep their state until SurfBoot sets them up. */
void board_reset(void) {
    irq_disable();
    csr_write(mie, 0);
    mmio_write32(REG(SURF_INTC_BASE, SURF_INTC_ENABLE), 0);
    board_set_mtimecmp(0xFFFFFFFFFFFFFFFFULL);
    asm volatile("fence.i\n\tjr %0" : : "r"(SURF_RESET_PC) : "memory");
    for(;;) ;
}

void board_poweroff(int code) {
    (void)code;
    kprintf("\nThe Arty cannot switch itself off: halted (unplug it, or press PROG).\n");
    halt();
    for(;;) ;
}

/**** LEDs ****/

/* LD4 blinks with the heartbeat task (arch/riscv32/main.c): proof the scheduler runs */
void board_heartbeat(u_long beat) {
    u_long r = REG(SURF_GPIO_BASE, SURF_GPIO_LED);
    if(!arty_has(SURF_SYSINFO_FEATURES_GPIO_MASK)) return;
    mmio_write32(r, (mmio_read32(r) & ~1u) | (beat & 1));
}

static int drv_gpio(void) {
    if(!arty_has(SURF_SYSINFO_FEATURES_GPIO_MASK)) return 1;
    mmio_write32(REG(SURF_GPIO_BASE, SURF_GPIO_LED), 0);
    mmio_write32(REG(SURF_GPIO_BASE, SURF_GPIO_RGB), 0);
    mmio_write32(REG(SURF_GPIO_BASE, SURF_GPIO_BTN_EDGE), SURF_GPIO_BTN_EDGE_BTN_MASK);
    kprintf("+GPIO: LEDs, switches 0x%lx, buttons; LD4 is the heartbeat\n",
            (u_long)mmio_read32(REG(SURF_GPIO_BASE, SURF_GPIO_SW)) & SURF_GPIO_SW_SW_MASK);
    return 0;
}

static struct driver drivers[] = {
    { "gpio",    drv_gpio,      0, 0 },
    { "ps2kbd",  arty_ps2_init, 0, 0 },
};

struct driver *board_drivers(u_int *count) {
    *count = sizeof(drivers) / sizeof(drivers[0]);
    return drivers;
}

void board_print_devices(void) {
    char buf[160];
    printf("    keyboard  %s\n", arty_ps2_describe(buf, sizeof(buf)));
}

/**** the board's commands ****/

static int cmd_leds(int argc, char **argv) {
    u_long v;
    char *end;
    if(!arty_has(SURF_SYSINFO_FEATURES_GPIO_MASK)) { printf("    no GPIO block in this bitstream\n"); return CMD_FAIL; }
    if(argc > 2) { printf("    usage: leds [0-15]   (LD4..LD7 as bits; LD4 is also the heartbeat)\n"); return CMD_USAGE; }
    if(argc == 2) {
        v = strtoul(argv[1], &end, 0);
        if(*end || v > 15) { printf("    usage: leds [0-15]\n"); return CMD_USAGE; }
        mmio_write32(REG(SURF_GPIO_BASE, SURF_GPIO_LED), (u32)v);
    }
    printf("    LEDs 0x%lx, switches 0x%lx, buttons 0x%lx (pressed since last look 0x%lx)\n",
           (u_long)mmio_read32(REG(SURF_GPIO_BASE, SURF_GPIO_LED)) & SURF_GPIO_LED_LED_MASK,
           (u_long)mmio_read32(REG(SURF_GPIO_BASE, SURF_GPIO_SW)) & SURF_GPIO_SW_SW_MASK,
           (u_long)mmio_read32(REG(SURF_GPIO_BASE, SURF_GPIO_BTN)) & SURF_GPIO_BTN_BTN_MASK,
           (u_long)mmio_read32(REG(SURF_GPIO_BASE, SURF_GPIO_BTN_EDGE)) & SURF_GPIO_BTN_EDGE_BTN_MASK);
    mmio_write32(REG(SURF_GPIO_BASE, SURF_GPIO_BTN_EDGE), SURF_GPIO_BTN_EDGE_BTN_MASK);
    return CMD_OK;
}

static const struct command board_cmds[] = {
    { "leds",    "[0-15]",   "the LEDs, switches and buttons (set LD4..LD7)", cmd_leds, 0 },
    { "kbdinfo", "",         "the PS/2 keyboard: state and error counters", arty_cmd_kbdinfo, 0 },
    { NULL, NULL, NULL, NULL, 0 }
};

const struct command *board_commands(void) {
    return board_cmds;
}
