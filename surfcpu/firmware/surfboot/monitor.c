/*
SurfBoot: the monitor
--------------------
File: monitor.c Date: 10/15/26 (SurfCPU)
--------------------
Where SurfBoot goes when booting fails (or BTN1 is held at power-on): a command line on the
UART for finding out what is wrong without a screen. One line in, one command run;
main.c (device) and the host tests feed the lines. Memory commands go through
port_peek32/port_poke32, which turn an access fault into an error message.
*/
#include <string.h>
#include "boot.h"
#include "crc32.h"
#include "port.h"
#include "sberr.h"
#include "version.h"

#define say sb_log

static char *word(char **p)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
        return 0;
    char *start = s;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    if (*s)
        *s++ = 0;
    *p = s;
    return start;
}

static int number(const char *s, uint32_t *v)
{
    uint32_t x = 0;
    int base = 10, digits = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (; *s; s++) {
        unsigned d;
        if (*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = (unsigned)(*s - 'a' + 10);
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = (unsigned)(*s - 'A' + 10);
        else return -1;
        if (x > (0xFFFFFFFFu - d) / (uint32_t)base)
            return -1;
        x = x * (uint32_t)base + d;
        digits++;
    }
    if (!digits)
        return -1;
    *v = x;
    return 0;
}

void sb_monitor_help(void)
{
    say("SurfBoot %u.%u.%u monitor commands:\n"
        "  boot [PATH]     boot /BOOT/SURFOS.BIN (BACKUP.BIN if it fails), or the 8.3 path given\n"
        "  backup          boot /BOOT/BACKUP.BIN\n"
        "  check PATH      run every package check on PATH (loads it into DDR, does not start it)\n"
        "  info            board, SoC, clocks, DDR and card state\n"
        "  ddr             wait for DDR calibration again and rerun the quick test\n"
        "  sd              initialize the card again and show what it is\n"
        "  part            the MBR partition table\n"
        "  fs              the FAT32 volume\n"
        "  ls [PATH]       list a directory (default /)\n"
        "  cat PATH [N]    print the first N bytes of a file (default 512)\n"
        "  chain PATH      a file's clusters, fragment by fragment\n"
        "  md ADDR [N]     show N memory words (default 16)\n"
        "  mw ADDR VALUE   write a memory word\n"
        "  spi [DIV]       show or set the SPI divider used after card initialization\n"
        "  reset           restart SurfBoot\n",
        SURFBOOT_VERSION_MAJOR, SURFBOOT_VERSION_MINOR, SURFBOOT_VERSION_PATCH);
}

static int need_ddr(struct sb_state *s)
{
    if (s->ddr_ok)
        return 0;
    int e = sb_ddr_init(s);
    if (e)
        say("ddr: %s\n", sb_last_msg);
    return e;
}

static int need_volume(struct sb_state *s)
{
    if (s->vol_ok && port_card_present())
        return 0;
    int e = sb_card_init(s);
    if (e)
        say("%s: %s\n", s->card_ok ? "fat32" : "sd", sb_last_msg);
    return e;
}

static void cmd_info(struct sb_state *s)
{
    uint32_t feat = port_sysinfo(SURF_SYSINFO_FEATURES), st = port_sysinfo(SURF_SYSINFO_DDR_STATUS);
    say("SurfBoot %u.%u.%u build %08x%s\n", SURFBOOT_VERSION_MAJOR, SURFBOOT_VERSION_MINOR,
        SURFBOOT_VERSION_PATCH, (unsigned)FW_BUILD_ID, FW_BUILD_DIRTY ? " (dirty)" : "");
    say("sysinfo: magic %08x, SoC %08x, map %u, board %08x, bitstream build %08x flags %x, time %u\n",
        port_sysinfo(SURF_SYSINFO_MAGIC), port_sysinfo(SURF_SYSINFO_SOC_VERSION),
        port_sysinfo(SURF_SYSINFO_MAP_VERSION), port_sysinfo(SURF_SYSINFO_BOARD_ID),
        port_sysinfo(SURF_SYSINFO_BUILD_ID), port_sysinfo(SURF_SYSINFO_BUILD_FLAGS),
        port_sysinfo(SURF_SYSINFO_BUILD_TIME));
    say("clocks: sys %u Hz, timebase %u Hz, pixel %u Hz; features %02x (uart timer irq gpio spi ps2 "
        "video ddr)\n", port_sysinfo(SURF_SYSINFO_SYS_HZ), port_timebase_hz(),
        port_sysinfo(SURF_SYSINFO_PIX_HZ), feat);
    say("ddr: status %x (%s%s%s%s), size %u MiB%s\n", st,
        (st & SURF_SYSINFO_DDR_STATUS_PRESENT_MASK) ? "present" : "absent",
        (st & SURF_SYSINFO_DDR_STATUS_CALIB_DONE_MASK) ? ", calibrated" : "",
        (st & SURF_SYSINFO_DDR_STATUS_CALIB_FAIL_MASK) ? ", calibration failed" : "",
        (st & SURF_SYSINFO_DDR_STATUS_TIMEOUT_MASK) ? ", watchdog timeout seen" : "",
        port_sysinfo(SURF_SYSINFO_DDR_SIZE) >> 20, s->ddr_ok ? ", tested" : "");
    say("card: %s%s; buttons %x\n", port_card_present() ? "present" : "absent (card-detect open)",
        s->card_ok ? (s->vol_ok ? ", FAT32 mounted" : ", initialized") : "", port_buttons());
    if (s->boot_err)
        say("last boot failure: %s: %s\n", sb_err_name(s->boot_err), s->boot_msg);
}

static void cmd_sd(struct sb_state *s)
{
    s->card_ok = s->vol_ok = 0;
    uint32_t sys_hz = port_sysinfo(SURF_SYSINFO_SYS_HZ);
    if (sys_hz == 0)
        sys_hz = SURF_SYS_HZ;
    uint32_t div = s->spi_fast_div ? s->spi_fast_div : sd_div_for(sys_hz, SB_SPI_FAST_HZ);
    int e = sd_init(&s->card, div);
    if (e) {
        say("sd: %s\n", sb_last_msg);
        return;
    }
    s->card_ok = 1;
    struct sd_card *c = &s->card;
    say("sd: %s, %s addressing, OCR %08x, ready after %u ms, data CRC %s\n", sd_type_name(c->type),
        c->block_addr ? "sector" : "byte", c->ocr, c->init_us / 1000u, c->crc_on ? "on" : "off");
    say("sd: SPI %u kHz for identification, %u kHz for data (DIV %u)\n",
        sd_div_hz(sys_hz, c->init_div) / 1000u, sd_div_hz(sys_hz, c->div) / 1000u, c->div);
    if (c->csd_ok)
        say("sd: CSD version %u, %u sectors (%u MiB)\n", (c->csd[0] >> 6) + 1u, c->sectors, c->sectors / 2048u);
    else
        say("sd: CSD unreadable: capacity unknown\n");
    if (c->cid_ok) {
        const uint8_t *d = c->cid;
        char oem[3] = { (char)d[1], (char)d[2], 0 }, name[6];
        for (int i = 0; i < 5; i++)
            name[i] = (d[3 + i] >= 0x20 && d[3 + i] < 0x7F) ? (char)d[3 + i] : '?';
        name[5] = 0;
        if (oem[0] < 0x20 || oem[0] > 0x7E) oem[0] = '?';
        if (oem[1] < 0x20 || oem[1] > 0x7E) oem[1] = '?';
        say("sd: CID manufacturer %02x, OEM %s, product %s rev %u.%u, serial %08x, made %u-%02u\n", d[0],
            oem, name, d[8] >> 4, d[8] & 15u,
            ((uint32_t)d[9] << 24) | ((uint32_t)d[10] << 16) | ((uint32_t)d[11] << 8) | d[12],
            2000u + (((d[13] & 15u) << 4) | (d[14] >> 4)), d[14] & 15u);
    }
}

static void cmd_part(struct sb_state *s)
{
    if (!s->card_ok) {
        cmd_sd(s);
        if (!s->card_ok)
            return;
    }
    static uint8_t buf[512];
    struct mbr_part p[4];
    int e = fat_read_mbr(sb_card_read, &s->card, buf, p);
    if (e && e != SB_E_SUPERFLOPPY && e != SB_E_NO_FAT32_PART && e != SB_E_EXFAT) {
        say("part: %s\n", sb_last_msg);
        return;
    }
    if (e)
        say("part: %s\n", sb_last_msg);
    say("  #  boot  type                    first sector    sectors        size\n");
    for (int i = 0; i < 4; i++)
        say("  %d  %s    0x%02x %-18s %12u %12u %8u MiB\n", i + 1, p[i].status == 0x80 ? "*" : " ",
            p[i].type, fat_part_type_name(p[i].type), p[i].lba, p[i].sectors, p[i].sectors / 2048u);
}

static void cmd_fs(struct sb_state *s)
{
    if (need_volume(s))
        return;
    struct fat_vol *v = &s->vol;
    say("fs: partition %d, type 0x%02x, sectors %u..%u; OEM \"%s\", label \"%s\", serial %08x\n",
        v->part_index + 1, v->part_type, v->part_lba, v->part_lba + v->part_sectors - 1u, v->oem, v->label,
        v->serial);
    say("fs: %u sectors, %u reserved, %u FAT%s of %u sectors (FAT %u in use), %u sectors per cluster\n",
        v->total_sectors, v->reserved, v->nfats, v->nfats == 1 ? "" : "s", v->fat_sectors, v->active_fat,
        v->spc);
    say("fs: %u clusters of %u bytes (2..%u), data from sector %u, root directory at cluster %u\n",
        v->clusters, v->cluster_bytes, v->max_cluster, v->data_lba, v->root_cluster);
}

struct ls_ctx {
    uint32_t files, dirs, bytes;
};

static int ls_cb(void *p, const struct fat_dirent *d)
{
    struct ls_ctx *x = p;
    char name[13];
    if (d->attr & FAT_ATTR_VOL) {
        say("  (volume label %.11s)\n", d->name);
        return 0;
    }
    fat_83_to_str(d->name, d->ntres, name);
    char attr[6] = {
        (d->attr & FAT_ATTR_DIR) ? 'd' : '-', (d->attr & FAT_ATTR_RO) ? 'r' : '-',
        (d->attr & FAT_ATTR_HID) ? 'h' : '-', (d->attr & FAT_ATTR_SYS) ? 's' : '-',
        (d->attr & FAT_ATTR_ARC) ? 'a' : '-', 0,
    };
    if (d->attr & FAT_ATTR_DIR) {
        x->dirs++;
        say("  %s  %-12s       <dir>  cluster %u\n", attr, name, d->cluster);
    } else {
        x->files++;
        x->bytes += d->size;
        say("  %s  %-12s %11u  cluster %u\n", attr, name, d->size, d->cluster);
    }
    return 0;
}

static void cmd_ls(struct sb_state *s, const char *path)
{
    if (need_volume(s))
        return;
    struct fat_dirent d;
    if (fat_lookup(&s->vol, path, &d)) {
        say("ls: %s\n", sb_last_msg);
        return;
    }
    struct ls_ctx x = { 0, 0, 0 };
    if (!(d.attr & FAT_ATTR_DIR)) {
        ls_cb(&x, &d);
        return;
    }
    if (fat_dir_scan(&s->vol, d.cluster ? d.cluster : s->vol.root_cluster, ls_cb, &x))
        say("ls: %s\n", sb_last_msg);
    say("  %u file%s (%u bytes), %u director%s\n", x.files, x.files == 1 ? "" : "s", x.bytes, x.dirs,
        x.dirs == 1 ? "y" : "ies");
}

static void cmd_check(struct sb_state *s, const char *path)
{
    if (need_ddr(s) || need_volume(s))
        return;
    int e = sb_load_path(s, path);
    const struct surf_img_header *h = &s->img.h;
    if (e && e < SB_E_IMG_ISA) {
        say("check: %s: %s (%s)\n", path, sb_last_msg, sb_err_name(e));
        return;
    }
    char isa[40], name[13];
    img_isa_string(h->isa, isa, sizeof isa);
    memcpy(name, h->name, 12);
    name[12] = 0;
    say("  magic %08x version %u header %u, header CRC %08x\n", h->magic, h->version, h->header_size,
        h->header_crc32);
    say("  name \"%s\" version %u.%u.%u build %08x flags %08x\n", name, h->image_version >> 16,
        (h->image_version >> 8) & 0xFFu, h->image_version & 0xFFu, h->build_id, h->flags);
    say("  isa %08x (%s) platform %u\n", h->isa, isa, h->platform);
    say("  load %08x payload %u mem %u entry %08x payload CRC %08x\n", h->load_addr, h->payload_size,
        h->mem_size, h->entry, h->payload_crc32);
    if (e)
        say("check: %s: FAILED: %s (%s)\n", path, sb_last_msg, sb_err_name(e));
    else
        say("check: %s: every check passed; CRC %08x from the card and from DDR, %u fragment%s, %u ms\n",
            path, s->img.crc_card, s->img.fragments, s->img.fragments == 1 ? "" : "s", s->img.load_us / 1000u);
    s->loaded = 0;                                  /* checked, not for booting */
}

static void cmd_cat(struct sb_state *s, const char *path, uint32_t max)
{
    if (need_volume(s))
        return;
    struct fat_dirent d;
    struct fat_file f;
    if (fat_lookup(&s->vol, path, &d) || fat_open(&s->vol, &d, &f)) {
        say("cat: %s\n", sb_last_msg);
        return;
    }
    static uint8_t buf[512];
    uint32_t shown = 0, len;
    while (shown < max) {
        if (fat_read_sector(&s->vol, &f, buf, &len)) {
            say("\ncat: %s\n", sb_last_msg);
            return;
        }
        if (len == 0)
            break;
        for (uint32_t i = 0; i < len && shown < max; i++, shown++) {
            uint8_t c = buf[i];
            port_putc((c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) ? (char)c : (c == '\r' ? '\r' : '.'));
        }
    }
    say("\n(%u of %u bytes)\n", shown, d.size);
}

static void cmd_chain(struct sb_state *s, const char *path)
{
    if (need_volume(s))
        return;
    struct fat_dirent d;
    if (fat_lookup(&s->vol, path, &d)) {
        say("chain: %s\n", sb_last_msg);
        return;
    }
    if (d.cluster == 0) {
        say("chain: %s has no clusters (size %u)\n", path, d.size);
        return;
    }
    struct fat_chain w;
    fat_chain_start(&w, d.cluster, s->vol.clusters);
    uint32_t run = d.cluster, prev = d.cluster, runs = 0, n = 1, next;
    int col = 0;
    say("chain:");
    for (;;) {
        if (fat_chain_step(&s->vol, &w, &next)) {
            say("\nchain: %s\n", sb_last_msg);
            return;
        }
        if (next != FAT_EOC && next == prev + 1u) {
            prev = next;
            n++;
            continue;
        }
        runs++;
        if (runs <= 64) {
            if (col > 60) {
                say("\n      ");
                col = 0;
            }
            char tmp[24];
            col += fmt_snprintf(tmp, sizeof tmp, run == prev ? " %u" : " %u-%u", run, prev);
            say("%s", tmp);
        }
        if (next == FAT_EOC)
            break;
        run = prev = next;
        n++;
    }
    uint32_t need = (d.attr & FAT_ATTR_DIR) ? n : (d.size ? (d.size - 1u) / s->vol.cluster_bytes + 1u : 0u);
    say("%s\nchain: %u cluster%s in %u fragment%s; the size needs %u\n", runs > 64 ? " ..." : "", n,
        n == 1 ? "" : "s", runs, runs == 1 ? "" : "s", need);
}

static void cmd_md(char *args)
{
    char *a = word(&args), *c = word(&args);
    uint32_t addr, count = 16;
    if (!a || number(a, &addr) || (c && number(c, &count))) {
        say("usage: md ADDR [N]\n");
        return;
    }
    addr &= ~3u;
    if (count > 256)
        count = 256;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v;
        if (i % 4 == 0)
            say("%08x:", addr + 4u * i);
        if (port_peek32(addr + 4u * i, &v)) {
            say(" (access fault at 0x%08x)\n", addr + 4u * i);
            return;
        }
        say(" %08x", v);
        if (i % 4 == 3 || i + 1 == count)
            say("\n");
    }
}

static void cmd_mw(char *args)
{
    char *a = word(&args), *b = word(&args);
    uint32_t addr, val;
    if (!a || !b || number(a, &addr) || number(b, &val) || (addr & 3u)) {
        say("usage: mw ADDR VALUE (ADDR 4-byte aligned)\n");
        return;
    }
    if (port_poke32(addr, val))
        say("mw: access fault at 0x%08x\n", addr);
}

int sb_monitor_line(struct sb_state *s, char *line)
{
    char *p = line;
    char *cmd = word(&p);
    if (!cmd)
        return MON_CONTINUE;
    sb_last_err = SB_OK;                            /* from here on: this command's outcome */
    sb_last_msg[0] = 0;
    char *arg = word(&p);
    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        sb_monitor_help();
    } else if (!strcmp(cmd, "boot") || !strcmp(cmd, "backup")) {
        int backup = !strcmp(cmd, "backup");
        if (sb_boot(s, backup ? 0 : arg, backup ? SB_BOOT_BACKUP : SB_BOOT_NORMAL) == SB_OK)
            return MON_BOOT;
        say("boot failed: %s (%s)\n", sb_err_text(sb_last_err), sb_err_name(sb_last_err));
    } else if (!strcmp(cmd, "check")) {
        if (!arg) say("usage: check PATH\n");
        else cmd_check(s, arg);
    } else if (!strcmp(cmd, "info")) {
        cmd_info(s);
    } else if (!strcmp(cmd, "ddr")) {
        s->ddr_ok = 0;
        if (need_ddr(s) == 0)
            say("ddr: calibrated after %u ms, %u MiB, quick test passed\n", s->ddr_wait_us / 1000u,
                s->ram_size >> 20);
    } else if (!strcmp(cmd, "sd")) {
        cmd_sd(s);
    } else if (!strcmp(cmd, "part")) {
        cmd_part(s);
    } else if (!strcmp(cmd, "fs")) {
        cmd_fs(s);
    } else if (!strcmp(cmd, "ls")) {
        cmd_ls(s, arg ? arg : "/");
    } else if (!strcmp(cmd, "cat")) {
        char *n = word(&p);
        uint32_t max = 512;
        if (!arg || (n && number(n, &max))) say("usage: cat PATH [N]\n");
        else cmd_cat(s, arg, max);
    } else if (!strcmp(cmd, "chain")) {
        if (!arg) say("usage: chain PATH\n");
        else cmd_chain(s, arg);
    } else if (!strcmp(cmd, "md")) {
        char rest[64];
        fmt_snprintf(rest, sizeof rest, "%s %s", arg ? arg : "", p);
        cmd_md(rest);
    } else if (!strcmp(cmd, "mw")) {
        char rest[64];
        fmt_snprintf(rest, sizeof rest, "%s %s", arg ? arg : "", p);
        cmd_mw(rest);
    } else if (!strcmp(cmd, "spi")) {
        uint32_t div;
        if (arg && (number(arg, &div) || div > 255)) {
            say("usage: spi [DIV 0..255]\n");
        } else {
            if (arg) {
                s->spi_fast_div = div;
                s->card_ok = s->vol_ok = 0;
            }
            uint32_t sys_hz = port_sysinfo(SURF_SYSINFO_SYS_HZ);
            if (sys_hz == 0)
                sys_hz = SURF_SYS_HZ;
            uint32_t d = s->spi_fast_div ? s->spi_fast_div : sd_div_for(sys_hz, SB_SPI_FAST_HZ);
            say("spi: data clock DIV %u = %u kHz (applies from the next card initialization)\n", d,
                sd_div_hz(sys_hz, d) / 1000u);
        }
    } else if (!strcmp(cmd, "reset")) {
        return MON_RESET;
    } else {
        say("unknown command \"%s\" (help lists them)\n", cmd);
    }
    return MON_CONTINUE;
}
