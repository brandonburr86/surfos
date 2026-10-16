/*
SurfCPU diagnostics: the DDR diagnostic, a SURFOS.BIN
--------------------
File: ddrdiag.c Date: 10/15/26 (SurfCPU)
--------------------
Packed as build/fw/DDRDIAG.BIN: put it on the card as /BOOT/SURFOS.BIN (or under any 8.3
name, booted from SurfBoot's monitor with "boot /BOOT/DDRDIAG.BIN"), and SurfBoot loads it
into DDR like a kernel (milestone 5's "real card boots a DDR diagnostic image"). Running
from DDR at 0x8000_0000, it checks the boot information it was handed (docs/BOOT-ABI.md
section 4), then tests all of DDR above itself (ddrtest.c): data lines, byte lanes,
address lines and aliases, boundaries including the top of DDR, patterns, sustained copies
and code copied elsewhere in DDR and run there. It continues SurfBoot's text on the screen.

The command line (/BOOT/CMDLINE.TXT) chooses the depth: "full" tests every word of DDR
(minutes on the board), "size=16M" limits the area, "loops=N" sets the copy passes. The
result is "DDRDIAG: PASS (n checks)" or "DDRDIAG: FAIL (...)" and LD0 green or red; then
"q" reruns the quick tests, "f" the full ones, anything else is echoed.
*/
#include <string.h>
#include "crc32.h"
#include "diag.h"
#include "version.h"
#include "surfcpu/bootabi.h"

extern char __image_end[];
static struct surf_boot_info bi;
static int bi_ok;
static uint32_t a0_in, a2_in;
static struct { int full; uint32_t size, loops; } opt;

static const char *find(const char *s, const char *w)
{
    for (; *s; s++) {
        const char *a = s, *b = w;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b)
            return a;
    }
    return 0;
}

static uint32_t number(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10u + (uint32_t)(*s++ - '0');
    if (*s == 'K' || *s == 'k') v <<= 10;
    if (*s == 'M' || *s == 'm') v <<= 20;
    return v;
}

static void check_bootinfo(uint32_t unused)
{
    (void)unused;
    diag_section("boot information");
    struct surf_boot_info z = bi;
    z.bootinfo_crc32 = 0;
    diag_check(a0_in == 0 && a2_in == SURF_BOOTINFO_MAGIC, "a0 %08x, a2 %08x", a0_in, a2_in);
    diag_check(bi_ok, "a1 did not point at the boot information page");
    diag_check(bi.magic == SURF_BOOTINFO_MAGIC && bi.version == 1 && bi.size == 256, "magic %08x version %u size %u",
               bi.magic, bi.version, bi.size);
    diag_check(crc32(&z, sizeof z) == bi.bootinfo_crc32, "boot information CRC");
    diag_check(bi.ram_base == SURF_DDR_BASE && bi.ram_size >= (1u << 20) && bi.ram_size <= SURF_DDR_SIZE,
               "RAM %08x + %u", bi.ram_base, bi.ram_size);
    diag_check(bi.image_base == SURF_KERNEL_BASE && bi.image_base + bi.image_mem_size >= (uint32_t)(uintptr_t)__image_end,
               "image [%08x + %u) does not cover this program (ends %08x)", bi.image_base, bi.image_mem_size,
               (uint32_t)(uintptr_t)__image_end);
    diag_section_end();
    con_printf("started by SurfBoot %06x build %08x from partition sector %u, flags %x, command line \"%s\"\n",
               bi.surfboot_version, bi.surfboot_build_id, bi.part_lba, bi.boot_flags, bi.cmdline);
}

static struct ddr_area area;

static void step(uint32_t which)
{
    switch (which) {
    case 0: ddr_test_data_lines(&area); break;
    case 1: ddr_test_byte_lanes(&area); break;
    case 2: ddr_test_address_lines(&area); break;
    case 3: ddr_test_boundaries(&area); break;
    case 4: ddr_test_patterns(&area, opt.full ? area.hi - area.lo : (4u << 20), 1); break;
    case 5: ddr_test_copies(&area, opt.full ? (16u << 20) : (1u << 20), opt.loops); break;
    default: ddr_test_exec(&area); break;
    }
}

static void run(void)
{
    diag_checks = diag_failures = 0;
    led_set(0x1, 0x004);
    diag_guard(check_bootinfo, 0);
    uint32_t ram_end = (bi_ok ? bi.ram_base + bi.ram_size : SURF_DDR_BASE + sysinfo(SURF_SYSINFO_DDR_SIZE));
    area.ram_base = SURF_DDR_BASE;
    area.ram_size = ram_end - SURF_DDR_BASE;
    area.lo = ((uint32_t)(uintptr_t)__image_end + 0xFFFFFu) & ~0xFFFFFu;
    area.hi = ram_end;
    if (opt.size && opt.size < area.hi - area.lo)
        area.hi = area.lo + opt.size;
    con_printf("testing DDR [0x%08x, 0x%08x) (%u MiB) of [0x%08x, 0x%08x); %s mode\n", area.lo, area.hi,
               (area.hi - area.lo) >> 20, area.ram_base, ram_end, opt.full ? "full" : "quick");
    for (uint32_t i = 0; i < 7; i++) {
        led_set(1u + (i & 7u), 0x004);
        diag_guard(step, i);
    }
    diag_summary("DDRDIAG");
}

int main(uint32_t hart, uint32_t bootinfo, uint32_t magic)
{
    a0_in = hart;
    a2_in = magic;
    if (bootinfo == SURF_BOOTINFO_BASE) {           /* copy it before anything else touches scratch RAM */
        memcpy(&bi, (const void *)(uintptr_t)bootinfo, sizeof bi);
        bi_ok = 1;
    }
    video_attach();
    fw_trap_hook = 0;
    con_printf("\nSurfCPU DDR diagnostic %s (build %08x%s), running from DDR at 0x%08x\n", DIAG_VERSION,
               (unsigned)FW_BUILD_ID, FW_BUILD_DIRTY ? ", dirty tree" : "", SURF_KERNEL_BASE);
    opt.loops = 4;
    if (bi_ok) {
        bi.cmdline[sizeof bi.cmdline - 1] = 0;
        const char *v;
        opt.full = find(bi.cmdline, "full") != 0;
        if ((v = find(bi.cmdline, "size=")))
            opt.size = number(v);
        if ((v = find(bi.cmdline, "loops=")) && number(v))
            opt.loops = number(v);
    }
    for (;;) {
        run();
        con_puts("Type q for the quick tests again, f for the full tests (every word: minutes on the board).\n");
        for (;;) {
            int c = con_getc();
            if (c == 'q' || c == 'f') {
                opt.full = c == 'f';
                break;
            }
            if (c >= 0)
                con_printf("'%c'\n", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
    }
}
