/*
SurfBoot: the boot sequence
--------------------
File: boot.c Date: 10/15/26 (SurfCPU)
--------------------
See boot.h. Each step logs one line saying what it found; a failure is logged with its
sentence and returned, and the caller (main.c on the device, the host tests) decides what
happens next.
*/
#include <string.h>
#include "boot.h"
#include "crc32.h"
#include "port.h"
#include "sberr.h"
#include "version.h"

uint32_t sb_version(void) { return SURFBOOT_VERSION; }
uint32_t sb_build_id(void) { return FW_BUILD_ID; }

int sb_card_read(void *ctx, uint32_t lba, void *buf)
{
    return sd_read((struct sd_card *)ctx, lba, buf);
}

static uint32_t pattern(uint32_t off) { return off ^ 0xC3A5F00Fu; }

/* Data lines (walking one and zero), address lines (a distinct word at every power-of-two
 * offset, all written before any is checked, so a stuck or shorted line shows up as an
 * overwritten word) and byte lanes, at a few dozen addresses: microseconds, destructive,
 * run before anything is loaded. */
static int ddr_quick_test(uint32_t base, uint32_t size)
{
    volatile uint32_t *w0 = port_ddr_word(base);
    for (int b = 0; b < 32; b++) {
        uint32_t v = 1u << b;
        *w0 = v;
        uint32_t r = *w0;
        if (r != v)
            return sb_fail(SB_E_DDR_TEST, "DDR data line test at 0x%08x: wrote 0x%08x, read 0x%08x",
                           base, v, r);
        *w0 = ~v;
        r = *w0;
        if (r != ~v)
            return sb_fail(SB_E_DDR_TEST, "DDR data line test at 0x%08x: wrote 0x%08x, read 0x%08x",
                           base, ~v, r);
    }
    for (uint32_t off = 4; off < size; off <<= 1)
        *port_ddr_word(base + off) = pattern(off);
    *w0 = pattern(0);
    for (uint32_t off = 0; off < size; off = off ? off << 1 : 4) {
        uint32_t r = *port_ddr_word(base + off);
        if (r != pattern(off))
            return sb_fail(SB_E_DDR_TEST, "DDR address line test at 0x%08x: wrote 0x%08x, read 0x%08x "
                           "(a stuck or shorted address line?)", base + off, pattern(off), r);
    }
    volatile uint32_t *w = port_ddr_word(base + 8);
    volatile uint8_t *b8 = (volatile uint8_t *)w;
    volatile uint16_t *b16 = (volatile uint16_t *)w;
    *w = 0;
    for (int k = 0; k < 4; k++)
        b8[k] = (uint8_t)(0xA0 + k);
    uint32_t r = *w;
    if (r != 0xA3A2A1A0u)
        return sb_fail(SB_E_DDR_TEST, "DDR byte lane test at 0x%08x: bytes a0 a1 a2 a3 read back as 0x%08x",
                       base + 8, r);
    b16[1] = 0x5AA5;
    r = *w;
    if (r != 0x5AA5A1A0u)
        return sb_fail(SB_E_DDR_TEST, "DDR halfword test at 0x%08x: expected 0x5aa5a1a0, read 0x%08x",
                       base + 8, r);
    return SB_OK;
}

int sb_ddr_init(struct sb_state *s)
{
    s->ddr_ok = 0;
    uint32_t st = port_sysinfo(SURF_SYSINFO_DDR_STATUS);
    if (!(st & SURF_SYSINFO_DDR_STATUS_PRESENT_MASK))
        return sb_fail(SB_E_DDR_ABSENT, "this bitstream has no DDR controller (DDR_STATUS 0x%08x): there "
                       "is nowhere to load a kernel", st);
    uint32_t t0 = port_now_us();
    while (!(st & SURF_SYSINFO_DDR_STATUS_CALIB_DONE_MASK)) {
        if (st & SURF_SYSINFO_DDR_STATUS_CALIB_FAIL_MASK)
            return sb_fail(SB_E_DDR_CALIB, "DDR calibration failed (DDR_STATUS 0x%08x)", st);
        if ((uint32_t)(port_now_us() - t0) >= SB_DDR_WAIT_US)
            return sb_fail(SB_E_DDR_TIMEOUT, "DDR calibration did not finish within %u ms (DDR_STATUS "
                           "0x%08x)", SB_DDR_WAIT_US / 1000u, st);
        st = port_sysinfo(SURF_SYSINFO_DDR_STATUS);
    }
    s->ddr_wait_us = port_now_us() - t0;
    uint32_t size = port_sysinfo(SURF_SYSINFO_DDR_SIZE);
    if (size == 0 || size > SURF_DDR_SIZE || (size & 0xFFFFFu))
        return sb_fail(SB_E_DDR_ABSENT, "sysinfo.DDR_SIZE reads 0x%08x: not a whole number of MiB up to "
                       "256 MiB", size);
    int e = ddr_quick_test(SURF_DDR_BASE, size);
    if (e)
        return e;
    s->ram_base = SURF_DDR_BASE;
    s->ram_size = size;
    s->ram_limit = SURF_DDR_BASE + size;
    if (s->ram_limit > SURF_DDR_TOP_BASE)
        s->ram_limit = SURF_DDR_TOP_BASE;
    s->ddr_ok = 1;
    return SB_OK;
}

static void log_size(const char *what, uint32_t sectors)
{
    if (sectors >= 2097152u)
        sb_log("%s%u.%u GiB", what, sectors / 2097152u, (sectors % 2097152u) / 209716u);
    else
        sb_log("%s%u MiB", what, sectors / 2048u);
}

int sb_card_init(struct sb_state *s)
{
    s->card_ok = s->vol_ok = 0;
    uint32_t sys_hz = port_sysinfo(SURF_SYSINFO_SYS_HZ);
    if (sys_hz == 0)
        sys_hz = SURF_SYS_HZ;
    uint32_t div = s->spi_fast_div ? s->spi_fast_div : sd_div_for(sys_hz, SB_SPI_FAST_HZ);
    int e = sd_init(&s->card, div);
    if (e)
        return e;
    s->card_ok = 1;
    struct sd_card *c = &s->card;
    sb_log("sd: %s", sd_type_name(c->type));
    if (c->sectors)
        log_size(", ", c->sectors);
    else
        sb_log(", capacity unknown (no CSD)");
    sb_log("; ready after %u ms; SPI %u kHz, data CRC %s\n", c->init_us / 1000u,
           sd_div_hz(sys_hz, c->div) / 1000u, c->crc_on ? "checked" : "not checked (CMD59 refused)");
    e = fat_mount(&s->vol, sb_card_read, &s->card, s->card.sectors);
    if (e)
        return e;
    s->vol_ok = 1;
    struct fat_vol *v = &s->vol;
    sb_log("fat32: partition %d (type 0x%02x) at sector %u, %u-byte clusters, %u clusters, label \"%s\"\n",
           v->part_index + 1, v->part_type, v->part_lba, v->cluster_bytes, v->clusters, v->label);
    return SB_OK;
}

static void progress(uint32_t done, uint32_t total)
{
    (void)done;
    (void)total;
    port_putc('.');
}

int sb_load_path(struct sb_state *s, const char *path)
{
    s->loaded = 0;
    if (!s->ddr_ok)
        return sb_fail(SB_E_DDR_ABSENT, "DDR is not ready");
    if (!s->vol_ok)
        return sb_fail(SB_E_NO_FAT32_PART, "no FAT32 volume is mounted");
    struct fat_dirent d;
    int e = fat_lookup(&s->vol, path, &d);
    if (e)
        return e;
    if (d.size >= 65536u)
        sb_log("boot: loading %u KiB ", d.size / 1024u);
    e = img_load(&s->vol, &d, s->ram_base, s->ram_limit, &s->img, progress);
    if (d.size >= 65536u)
        sb_log("\n");
    if (e)
        return e;
    s->image_path = path;
    s->loaded = 1;
    return SB_OK;
}

int sb_read_cmdline(struct sb_state *s)
{
    s->cmdline[0] = 0;
    s->cmdline_trunc = 0;
    struct fat_dirent d;
    int e = fat_lookup(&s->vol, SB_PATH_CMDLINE, &d);
    if (e)
        return e;
    struct fat_file f;
    e = fat_open(&s->vol, &d, &f);
    if (e)
        return e;
    static uint8_t buf[512];
    uint32_t len = 0;
    e = fat_read_sector(&s->vol, &f, buf, &len);
    if (e)
        return e;
    uint32_t i = 0, n = 0;
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
        i = 3;                                      /* a UTF-8 byte order mark */
    for (; i < len; i++) {
        uint8_t c = buf[i];
        if (c == '\r' || c == '\n' || c == 0)
            break;
        if (n >= SURF_BOOTINFO_CMDLINE_LEN - 1) {
            s->cmdline_trunc = 1;
            break;
        }
        s->cmdline[n++] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
    }
    s->cmdline[n] = 0;
    return SB_OK;
}

void sb_fill_bootinfo(struct sb_state *s)
{
    struct surf_boot_info *bi = port_bootinfo();
    memset(bi, 0, sizeof *bi);
    bi->magic = SURF_BOOTINFO_MAGIC;
    bi->version = SURF_BOOTINFO_VERSION;
    bi->size = sizeof *bi;
    bi->hart_id = SURF_HART_ID;
    bi->board_id = port_sysinfo(SURF_SYSINFO_BOARD_ID);
    bi->soc_version = port_sysinfo(SURF_SYSINFO_SOC_VERSION);
    bi->map_version = port_sysinfo(SURF_SYSINFO_MAP_VERSION);
    bi->sys_hz = port_sysinfo(SURF_SYSINFO_SYS_HZ);
    bi->timebase_hz = port_timebase_hz();
    bi->ram_base = s->ram_base;
    bi->ram_size = s->ram_size;
    bi->image_base = s->img.h.load_addr;
    bi->image_mem_size = s->img.h.mem_size;
    bi->boot_device = SURF_BOOT_DEVICE_SD;
    bi->part_lba = s->vol.part_lba;
    bi->boot_flags = (s->used_backup ? SURF_BOOTFLAG_BACKUP : 0u)
                   | (s->cmdline_trunc ? SURF_BOOTFLAG_CMDLINE_TRUNC : 0u);
    bi->surfboot_version = SURFBOOT_VERSION;
    bi->surfboot_build_id = FW_BUILD_ID;
    memcpy(bi->cmdline, s->cmdline, sizeof bi->cmdline);
    bi->bootinfo_crc32 = 0;
    bi->bootinfo_crc32 = crc32(bi, sizeof *bi);
}

static void log_image(struct sb_state *s)
{
    const struct img_info *im = &s->img;
    const struct surf_img_header *h = &im->h;
    char isa[40], name[13];
    img_isa_string(h->isa, isa, sizeof isa);
    memcpy(name, h->name, 12);
    name[12] = 0;
    for (int i = 0; i < 12; i++)
        if (name[i] && (name[i] < 0x20 || name[i] > 0x7E))
            name[i] = '?';
    sb_log("boot: %s %u.%u.%u build %08x: %u bytes in %u fragment%s, read in %u ms\n",
           name[0] ? name : "(no name)", h->image_version >> 16, (h->image_version >> 8) & 0xFFu,
           h->image_version & 0xFFu, h->build_id, im->file_size, im->fragments,
           im->fragments == 1 ? "" : "s", im->load_us / 1000u);
    sb_log("boot: load 0x%08x, entry 0x%08x, mem_size %u, ISA %s; payload CRC 0x%08x matches (card "
           "and DDR)\n", h->load_addr, h->entry, h->mem_size, isa, h->payload_crc32);
    if (h->flags)
        sb_log("boot: header flags 0x%08x are reserved and ignored\n", h->flags);
}

int sb_note_boot_failure(struct sb_state *s, int err)
{
    s->boot_err = err;
    memcpy(s->boot_msg, sb_last_msg, sizeof s->boot_msg);
    return err;
}

static int boot_once(struct sb_state *s, const char *path, int choice);

int sb_boot(struct sb_state *s, const char *path, int choice)
{
    int e = boot_once(s, path, choice);
    if (e)
        return sb_note_boot_failure(s, e);
    s->boot_err = SB_OK;
    s->boot_msg[0] = 0;
    return SB_OK;
}

static int boot_once(struct sb_state *s, const char *path, int choice)
{
    int e;
    if (!s->ddr_ok) {
        e = sb_ddr_init(s);
        if (e) {
            sb_log("ddr: %s\n", sb_last_msg);
            return e;
        }
        sb_log("ddr: calibrated after %u ms, %u MiB, quick test passed (data, address, byte lanes)\n",
               s->ddr_wait_us / 1000u, s->ram_size >> 20);
    }
    e = sb_card_init(s);
    if (e) {
        sb_log("%s: %s\n", s->card_ok ? "fat32" : "sd", sb_last_msg);
        return e;
    }

    const char *cand[2];
    int n = 0;
    if (path)
        cand[n++] = path;
    else if (choice == SB_BOOT_BACKUP)
        cand[n++] = SB_PATH_BACKUP;
    else {
        cand[n++] = SB_PATH_MAIN;
        cand[n++] = SB_PATH_BACKUP;
    }
    static char first_msg[sizeof sb_last_msg];
    int first_err = SB_OK;
    for (int i = 0; i < n; i++) {
        sb_log("boot: %s\n", cand[i]);
        e = sb_load_path(s, cand[i]);
        if (e == SB_OK)
            break;
        sb_log("boot: %s: %s\n", cand[i], sb_last_msg);
        if (first_err == SB_OK) {
            first_err = e;
            memcpy(first_msg, sb_last_msg, sizeof first_msg);
        }
        if (e == SB_E_NO_CARD)
            break;
        if (i + 1 < n)
            sb_log("boot: trying %s instead\n", cand[i + 1]);
    }
    if (e) {
        sb_last_err = first_err;
        memcpy(sb_last_msg, first_msg, sizeof first_msg);
        return first_err;
    }
    s->used_backup = strcmp(s->image_path, SB_PATH_BACKUP) == 0;
    log_image(s);

    e = sb_read_cmdline(s);
    if (e == SB_OK)
        sb_log("boot: command line \"%s\"%s\n", s->cmdline, s->cmdline_trunc ? " (truncated to 127 characters)" : "");
    else if (e == SB_E_NOT_FOUND)
        sb_log("boot: no %s; empty command line\n", SB_PATH_CMDLINE);
    else
        sb_log("boot: %s ignored: %s\n", SB_PATH_CMDLINE, sb_last_msg);
    sb_fill_bootinfo(s);
    return SB_OK;
}
