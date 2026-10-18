/*
SurfCPU firmware tests: the handoff test kernel
--------------------
File: hello.c Date: 10/15/26 (SurfCPU)
--------------------
A SURFOS.BIN that checks what SurfBoot handed over against docs/BOOT-ABI.md section 4 and
prints the result on the UART: a0 = hart 0, a1 = the boot information at 0x0001F000,
a2 = its magic; MIE = 0, mie = 0, mtvec inside the boot ROM, intc.ENABLE = 0, mtimecmp all
ones, the card's chip select released, the UART drained, text mode on; the boot
information's magic, version, size, CRC and fields (its image range must cover this very
program). Then, if the command line asks for it, a deliberate trap ("trap": ECALL,
"illegal": an all-zero word, "load": a load from an unmapped address) for SurfBoot's fault
reporter to catch.
*/
#include <stdint.h>
#include "crc32.h"
#include "fmt.h"
#include "surfcpu/bootabi.h"
#include "surfcpu/soc.h"

#define REG(b, o) SURF_REG(b, o)

struct entry { uint32_t a0, a1, a2, mtvec, mstatus, mie, mscratch; };
extern char _start[], __image_end[];
int hello_main(const struct entry *e);
void hello_illegal(void);                   /* hello_start.S: an all-zero word */

static void putc_(void *ctx, char c)
{
    (void)ctx;
    if (c == '\n')
        putc_(ctx, '\r');
    while (REG(SURF_UART_BASE, SURF_UART_TXDATA) & SURF_UART_TXDATA_FULL_MASK)
        ;
    REG(SURF_UART_BASE, SURF_UART_TXDATA) = (uint8_t)c;
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fmt_vformat(putc_, 0, fmt, ap);
    va_end(ap);
}

static int bad;

static void check(int ok, const char *what)
{
    if (!ok) {
        bad++;
        say("HELLO: FAIL %s\n", what);
    }
}

static int contains(const char *s, const char *w)
{
    for (; *s; s++) {
        const char *a = s, *b = w;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b)
            return 1;
    }
    return 0;
}

int hello_main(const struct entry *e)
{
    uint32_t uart_status = REG(SURF_UART_BASE, SURF_UART_STATUS);
    say("\nHELLO: a0=%08x a1=%08x a2=%08x mtvec=%08x mstatus=%08x mie=%08x mscratch=%08x\n", e->a0, e->a1,
        e->a2, e->mtvec, e->mstatus, e->mie, e->mscratch);
    check(e->a0 == 0, "a0 is not hart 0");
    check(e->a1 == SURF_BOOTINFO_BASE, "a1 is not the boot information address");
    check(e->a2 == SURF_BOOTINFO_MAGIC, "a2 is not the boot information magic");
    check((e->mstatus & 0x8u) == 0, "mstatus.MIE is set");
    check(e->mie == 0, "mie is not 0");
    check(e->mtvec != 0 && e->mtvec < SURF_BOOTROM_BASE + SURF_BOOTROM_SIZE && (e->mtvec & 3u) == 0,
          "mtvec is not SurfBoot's reporter in ROM");
    check(REG(SURF_INTC_BASE, SURF_INTC_ENABLE) == 0, "intc.ENABLE is not 0");
    check(REG(SURF_TIMER_BASE, SURF_TIMER_MTIMECMP_LO) == 0xFFFFFFFFu &&
          REG(SURF_TIMER_BASE, SURF_TIMER_MTIMECMP_HI) == 0xFFFFFFFFu, "mtimecmp is not all ones");
    check((REG(SURF_SPI_BASE, SURF_SPI_CTRL) & SURF_SPI_CTRL_CS_MASK) == 0, "the card's chip select is asserted");
    check((uart_status & SURF_UART_STATUS_TX_IDLE_MASK) != 0, "the UART was not drained");
    if (REG(SURF_SYSINFO_BASE, SURF_SYSINFO_FEATURES) & SURF_SYSINFO_FEATURES_VIDEO_MASK)
        check(((REG(SURF_VIDEO_BASE, SURF_VIDEO_CTRL) & SURF_VIDEO_CTRL_MODE_MASK) >> SURF_VIDEO_CTRL_MODE_LSB) == 1,
              "the display is not in text mode");

    struct surf_boot_info bi = *(const struct surf_boot_info *)SURF_BOOTINFO_BASE;  /* copy it first */
    uint32_t want = bi.bootinfo_crc32;
    struct surf_boot_info zero = bi;
    zero.bootinfo_crc32 = 0;
    check(bi.magic == SURF_BOOTINFO_MAGIC && bi.version == 1 && bi.size == 256, "boot information header");
    check(crc32(&zero, sizeof zero) == want, "boot information CRC");
    check(bi.hart_id == 0 && bi.boot_device == SURF_BOOT_DEVICE_SD, "boot information hart/device");
    check(bi.ram_base == SURF_DDR_BASE && bi.ram_size >= (16u << 20), "boot information RAM");
    check(bi.image_base == (uint32_t)(uintptr_t)_start, "boot information image_base is not this program");
    check(bi.image_base + bi.image_mem_size >= (uint32_t)(uintptr_t)__image_end, "image_mem_size too small");
    check(bi.timebase_hz == SURF_TIMEBASE_HZ && bi.sys_hz == REG(SURF_SYSINFO_BASE, SURF_SYSINFO_SYS_HZ),
          "boot information clocks");
    check(bi.cmdline[sizeof bi.cmdline - 1] == 0, "command line not terminated");
    say("HELLO: boot_flags 0x%08x part_lba %u ram %u MiB surfboot %06x build %08x cmdline \"%s\"\n",
        bi.boot_flags, bi.part_lba, bi.ram_size >> 20, bi.surfboot_version, bi.surfboot_build_id, bi.cmdline);
    say(bad ? "HELLO: handoff FAILED (%d)\n" : "HELLO: handoff OK\n", bad);

    if (contains(bi.cmdline, "trap")) {
        say("HELLO: ECALL with no handler of my own\n");
        __asm__ volatile ("ecall");
    } else if (contains(bi.cmdline, "illegal")) {
        say("HELLO: an illegal instruction\n");
        hello_illegal();
    } else if (contains(bi.cmdline, "load")) {
        say("HELLO: a load from an unmapped address\n");
        (void)*(volatile uint32_t *)SURF_UNMAPPED_PROBE_BASE;   /* never mapped (spec/soc.toml) */
    }
    for (;;)
        __asm__ volatile ("wfi");
}
