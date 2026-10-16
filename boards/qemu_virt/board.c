/*
SurfOS board: QEMU's RISC-V virt machine
--------------------
File: board.c   Date: 10/15/26 (SurfCPU)
--------------------
A separate HAL so that the machine-mode kernel can be tested now, under emulation
(make -C surfcpu test-os): QEMU's 16550 UART, its CLINT timer (10 MHz), its PLIC, its
Goldfish real-time clock, its test device (to power off with a status) and a RAM disk
the harness loads with -device loader. None of these is SurfCPU's hardware
(boards/arty_a7/ is): a pass here is a pass of the portable kernel, not of the FPGA.

QEMU (-bios none) enters the image at the start of RAM in machine mode with a0 the
hart ID, a1 a device tree and a2 not SurfBoot's magic, so the kernel runs without boot
information: RAM is the linker script's (board.ld), and the command line is empty.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/system.h>
#include <surfos/irq.h>
#include <surfos/multiboot.h>
#include <sys/driver.h>
#include <sys/bdev.h>
#include <mm/kalloc.h>
#include <asm/io.h>
#include <riscv/board.h>
#include <riscv/csr.h>
#include "qemu_virt.h"
#include "../../shell/shell.h"
#include <blibc_common.h>

const char *board_name(void) { return "qemu_virt"; }
const char *board_machine(void) { return "QEMU virt"; }

/**** the UART: ns16550a, byte registers ****/

#define UART_RBR 0
#define UART_THR 0
#define UART_DLL 0
#define UART_IER 1
#define UART_DLM 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_SCR 7
#define LSR_DR   0x01
#define LSR_THRE 0x20

static bool uart_ok;

static void uart_w(u_int reg, u8 v) { mmio_write8(QV_UART_BASE + reg, v); }
static u8 uart_r(u_int reg) { return mmio_read8(QV_UART_BASE + reg); }

void board_early_init(void) {
    uart_w(UART_SCR, 0x5A);
    if(uart_r(UART_SCR) != 0x5A) return;
    uart_w(UART_IER, 0x00);
    uart_w(UART_LCR, 0x80);         /* the divisor (QEMU ignores the rate) */
    uart_w(UART_DLL, 1);
    uart_w(UART_DLM, 0);
    uart_w(UART_LCR, 0x03);         /* 8N1 */
    uart_w(UART_FCR, 0x07);         /* FIFOs on and cleared */
    uart_w(UART_MCR, 0x0B);
    uart_ok = true;
}

bool board_uart_present(void) { return uart_ok; }

void board_uart_putc(char c) {
    u_int spins;
    for(spins = 0; spins < 100000 && !(uart_r(UART_LSR) & LSR_THRE); spins++) ;
    uart_w(UART_THR, (u8)c);
}

int board_uart_getc(void) {
    if(!(uart_r(UART_LSR) & LSR_DR)) return -1;
    return uart_r(UART_RBR);
}

int board_uart_irq(void) { return QV_UART_IRQ; }

void board_uart_rx_irq(bool on) {
    uart_w(UART_IER, on ? 0x01 : 0x00);
}

const char *board_uart_describe(char *buf, size_t size) {
    snprintf(buf, size, "ns16550a at 0x%08lx (QEMU virt), PLIC source %d", QV_UART_BASE, QV_UART_IRQ);
    return buf;
}

/**** the machine timer: the CLINT ****/

u64 board_mtime(void) {
    u32 hi, lo, hi2;
    do {
        hi = mmio_read32(QV_CLINT_MTIME + 4);
        lo = mmio_read32(QV_CLINT_MTIME);
        hi2 = mmio_read32(QV_CLINT_MTIME + 4);
    } while(hi != hi2);
    return ((u64)hi << 32) | lo;
}

void board_set_mtimecmp(u64 when) {
    mmio_write32(QV_CLINT_MTIMECMP, 0xFFFFFFFFu);        /* never early on the way */
    mmio_write32(QV_CLINT_MTIMECMP + 4, (u32)(when >> 32));
    mmio_write32(QV_CLINT_MTIMECMP, (u32)when);
}

u32 board_timebase_hz(void) { return QV_TIMEBASE_HZ; }

/**** interrupts: the PLIC, context 0 (hart 0, machine mode) ****/

void board_irq_init(void) {
    u_int i;
    for(i = 0; i < QV_PLIC_SOURCES / 32; i++) mmio_write32(QV_PLIC_ENABLE + 4 * i, 0);
    for(i = 1; i < QV_PLIC_SOURCES; i++) mmio_write32(QV_PLIC_PRIORITY + 4 * i, 0);
    mmio_write32(QV_PLIC_THRESHOLD, 0);
}

void board_irq_enable(u_int irq, bool on) {
    u_long reg = QV_PLIC_ENABLE + 4 * (irq / 32);
    u32 bit = 1u << (irq % 32);
    if(irq == 0 || irq >= QV_PLIC_SOURCES) return;
    mmio_write32(QV_PLIC_PRIORITY + 4 * irq, on ? 1 : 0);
    mmio_write32(reg, on ? (mmio_read32(reg) | bit) : (mmio_read32(reg) & ~bit));
}

int board_irq_claim(void) {
    u32 id = mmio_read32(QV_PLIC_CLAIM);
    return id ? (int)id : -1;
}

void board_irq_complete(u_int irq) {
    mmio_write32(QV_PLIC_CLAIM, irq);
}

const char *board_irq_describe(char *buf, size_t size) {
    snprintf(buf, size, "PLIC at 0x%08lx (QEMU virt), context 0 (hart 0, M mode), %d sources", QV_PLIC_BASE, QV_PLIC_SOURCES);
    return buf;
}

/**** time of day: the Goldfish RTC (nanoseconds; reading the low word latches the high) ****/

bool board_rtc(u64 *seconds) {
    u32 lo = mmio_read32(QV_RTC_BASE), hi = mmio_read32(QV_RTC_BASE + 4);
    u64 ns = ((u64)hi << 32) | lo;
    if(!ns) return false;
    *seconds = ns / 1000000000ULL;
    return true;
}

/**** boot information, display, power ****/

void board_boot_info(const struct surf_boot_info *bi) {
    (void)bi;                       /* QEMU has no SurfBoot: nothing to take */
}

void board_print_info(void) {
    printf("  QEMU virt: entered at 0x80000000 by QEMU's reset vector (-bios none), RAM 0x80000000-0x8fffffff,\n");
    printf("  RAM disk window 0x%08lx (+%lu MB), CLINT %lu Hz, PLIC, ns16550a, Goldfish RTC, sifive_test\n",
           QV_RAMDISK_BASE, QV_RAMDISK_MAX >> 20, QV_TIMEBASE_HZ);
}

const struct board_display *board_display(void) { return NULL; }

void board_reset(void) {
    irq_disable();
    mmio_write32(QV_TEST_BASE, QV_TEST_RESET);
    for(;;) rv_wfi();
}

void board_poweroff(int code) {
    irq_disable();
    mmio_write32(QV_TEST_BASE, code ? (((u32)code << 16) | QV_TEST_FAIL) : QV_TEST_PASS);
    for(;;) rv_wfi();
}

void board_heartbeat(u_long beat) {
    (void)beat;                     /* no LEDs on QEMU virt */
}

/**** the RAM disk ****/

/*
    The harness loads a disk image at QV_RAMDISK_BASE (-device loader). Its size is read
    from what it holds: the end of its last MBR partition, or, for a volume without a
    partition table, the boot sector's total sector count. Nothing there (all zero, no
    signature): no RAM disk. It is rd0, on the RAM bus: fdisk and mkfs may change it.
*/
struct ramdisk {
    u8 *base;
    u32 blocks;
};
static struct ramdisk rd;

static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static u32 image_blocks(const u8 *b) {
    u32 end = 0, max = QV_RAMDISK_MAX / BDEV_BLOCK_SIZE, i;
    if(b[510] != 0x55 || b[511] != 0xAA) return 0;
    if((b[0] == 0xEB || b[0] == 0xE9) && le16(b + 11) == 512) {     /* a volume boot sector, no MBR */
        end = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    } else {
        for(i = 0; i < 4; i++) {
            const u8 *e = b + 446 + 16 * i;
            u32 start = le32(e + 8), count = le32(e + 12);
            if(e[4] && count && start + count > end && start + count >= start) end = start + count;
        }
    }
    return end <= max ? end : 0;
}

static int rd_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    struct ramdisk *r = (struct ramdisk *)d->priv;
    memcpy(buf, r->base + (u_long)lba * BDEV_BLOCK_SIZE, (u_long)count * BDEV_BLOCK_SIZE);
    return 0;
}

static int rd_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct ramdisk *r = (struct ramdisk *)d->priv;
    memcpy(r->base + (u_long)lba * BDEV_BLOCK_SIZE, buf, (u_long)count * BDEV_BLOCK_SIZE);
    return 0;
}

static const struct bdev_ops rd_ops = { rd_read, rd_write, NULL };

/* sdsim.c: the RAM disk as sd0, through the Arty's SD driver and a simulated card (the word sdsim) */
int qv_sdsim_init(u8 *base, u32 blocks);
void qv_sdsim_stats(void);
bool qv_sdsim_active(void);

static int qv_ramdisk(void) {
    struct bdev *d;
    rd.base = (u8 *)QV_RAMDISK_BASE;
    rd.blocks = image_blocks(rd.base);
    if(!rd.blocks) {
        kprintf("+RAMDISK: no disk image at 0x%08lx (the harness loads one with -device loader)\n", QV_RAMDISK_BASE);
        return 1;
    }
    if(bootinfo_has_arg(&bootinfo, "sdsim")) return qv_sdsim_init(rd.base, rd.blocks) ? -1 : 0;
    d = (struct bdev *)kcalloc(1, sizeof(struct bdev));
    if(!d) return -1;
    strlcpy(d->name, "rd0", sizeof(d->name));
    strlcpy(d->model, "QEMU loader image", sizeof(d->model));
    d->block_size = BDEV_BLOCK_SIZE;
    d->nblocks = rd.blocks;
    d->bus = BDEV_BUS_RAM;
    d->ops = &rd_ops;
    d->priv = &rd;
    if(bdev_register(d) != 0) { kfree(d); return -1; }
    kprintf("+RAMDISK: rd0 is the image at 0x%08lx, %lu blocks (%lu KB)\n", QV_RAMDISK_BASE, (u_long)rd.blocks,
            (u_long)rd.blocks / 2);
    bdev_scan_partitions(d);
    return 0;
}

static struct driver drivers[] = {
    { "ramdisk", qv_ramdisk, 0, 0 },
};

struct driver *board_drivers(u_int *count) {
    *count = sizeof(drivers) / sizeof(drivers[0]);
    return drivers;
}

void board_print_devices(void) {
    printf("    keyboard  none (QEMU virt: the serial console is the keyboard)\n");
    if(qv_sdsim_active()) qv_sdsim_stats();
    else if(rd.blocks) printf("    ramdisk   rd0: %lu blocks at 0x%08lx\n", (u_long)rd.blocks, QV_RAMDISK_BASE);
    else printf("    ramdisk   none\n");
}

/**** the board's commands ****/

static int cmd_poweroff(int argc, char **argv) {
    int code = argc > 1 ? (int)strtol(argv[1], NULL, 0) : 0;
    if(code < 0 || code > 0x7FFF) { printf("    usage: poweroff [status]   (0 to 32767)\n"); return CMD_USAGE; }
    printf("    powering off (QEMU exits%s)\n", code ? " with a failure status" : "");
    board_poweroff(code);
    return CMD_FAIL;
}

static const struct command board_cmds[] = {
    { "poweroff", "[status]", "leave QEMU (sifive_test), 0 for a pass", cmd_poweroff, 0 },
    { NULL, NULL, NULL, NULL, 0 }
};

const struct command *board_commands(void) {
    return board_cmds;
}
