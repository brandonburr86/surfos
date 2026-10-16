/*
SurfCPU firmware tests: the host test driver
--------------------
File: fwtest.c Date: 10/15/26 (SurfCPU)
--------------------
Runs SurfBoot's portable code (sd.c, fat32.c, image.c, boot.c, monitor.c, compiled natively
with AddressSanitizer and UBSan) on the simulated board of port_host.c:

    fwtest unit                                  CRC, formatting, 8.3 names, CSD, dividers
    fwtest boot [--card IMG] [options]           sb_boot() as the ROM runs it, then checks the
                                                 boot information and the loaded payload
    fwtest monitor [--card IMG] -c LINE ...      monitor command lines
    fwtest fuzz --card IMG --seed N [--iterations M] [--mutations K]
                                                 random damage to the MBR, boot sector, FAT,
                                                 directories and package header; every run must
                                                 end with a result, never a crash or a hang

Card options: --type sdhc|sdsc2|sdsc1|mmc, --absent, --no-cd, --dead, --miso-low,
--acmd41-busy N (-1 = forever), --cmd8-bad-echo, --no-33v, --no-cmd59, --ncr N, --nac N,
--crc-errors N, --crc-every N, --token-errors N, --read-timeouts N, --remove-after N,
--min-div N; --io-model runs the io track's pin-level model instead (built with
IO_MODEL_DIR; --type, --absent, --crc-errors and --token-errors carry over, and any protocol
violation it logs fails the run). Board options: --ddr ok|absent|fail|never, --ddr-size BYTES, --ddr-calib-ms N,
--ddr-flip, --btn0, --path PATH. Checks: --expect NAME (OK or an error name such as
FAT_CYCLE), --expect-msg TEXT, --expect-log TEXT (repeatable), --expect-flags N,
--expect-cmdline TEXT. Exit 0 when every expectation holds, 1 otherwise, 2 on bad usage,
3 on a runaway (a loop that did not end within the simulated budget).
*/
#define _DEFAULT_SOURCE
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot.h"
#include "crc32.h"
#include "fat32.h"
#include "fmt.h"
#include "port.h"
#include "port_host.h"
#include "sberr.h"
#include "sd.h"

static int failures;

#ifdef FWTEST_IO_MODEL
int iomodel_attach(const char *image, const char *kind, unsigned seed, int absent, int read_bad_crc,
                   int error_tokens);
unsigned long iomodel_violations(void);
void iomodel_report(int all);
void iomodel_detach(void);
#endif
static int io_model;                    /* --io-model: the io track's card instead of sdcard_sim */
static const char *io_kind = "sdhc";

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- unit tests ---------------- */

static void fmt_case(const char *want, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    fmt_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    CHECK(strcmp(buf, want) == 0, "format \"%s\" gave \"%s\", want \"%s\"", fmt, buf, want);
}

static int unit(void)
{
    CHECK(crc32_selftest() == 0, "crc32 test vectors (step %d)", crc32_selftest());

    static const uint8_t cmd0[5] = { 0x40, 0, 0, 0, 0 }, cmd8[5] = { 0x48, 0, 0, 1, 0xAA },
                         cmd17[5] = { 0x51, 0, 0, 0, 0 };
    CHECK(sd_crc7(cmd0, 5) == 0x4A, "CRC-7 of CMD0 is 0x%02x", sd_crc7(cmd0, 5));
    CHECK(sd_crc7(cmd8, 5) == 0x43, "CRC-7 of CMD8 is 0x%02x", sd_crc7(cmd8, 5));
    CHECK(sd_crc7(cmd17, 5) == 0x2A, "CRC-7 of CMD17(0) is 0x%02x", sd_crc7(cmd17, 5));
    uint8_t ff[512];
    memset(ff, 0xFF, sizeof ff);
    CHECK(sd_crc16(ff, 512) == 0x7FA1, "CRC-16 of 512 x 0xff is 0x%04x", sd_crc16(ff, 512));

    fmt_case("-5", "%d", -5);
    fmt_case("-2147483648", "%d", INT_MIN);
    fmt_case("4294967295", "%u", 4294967295u);
    fmt_case("0000beef", "%08x", 0xBEEFu);
    fmt_case("BEEF", "%X", 0xBEEFu);
    fmt_case("0", "%x", 0u);
    fmt_case("ab   |", "%-5s|", "ab");
    fmt_case("   42", "%5u", 42u);
    fmt_case("abc", "%.3s", "abcdef");
    fmt_case("x%y", "x%%y");
    fmt_case("A", "%c", 'A');
    fmt_case("12345678901234", "%llu", 12345678901234ull);
    fmt_case("18446744073709551615", "%llu", 18446744073709551615ull);
    fmt_case("-9223372036854775808", "%lld", (long long)(-9223372036854775807LL - 1));
    fmt_case("123456789abcdef0", "%llx", 0x123456789ABCDEF0ull);
    fmt_case("  -7", "%4d", -7);
    fmt_case("-007", "%04d", -7);
    fmt_case("(null)", "%s", (char *)NULL);
    char small[4];
    int n = fmt_snprintf(small, sizeof small, "%s", "abcdef");
    CHECK(n == 6 && strcmp(small, "abc") == 0, "truncation: n=%d buf=\"%s\"", n, small);

    struct { const char *in; const char *out; } names[] = {
        { "SURFOS.BIN", "SURFOS  BIN" }, { "boot", "BOOT       " }, { "cmdline.txt", "CMDLINE TXT" },
        { "A.B", "A       B  " }, { "12345678.123", "12345678123" }, { "~1.$$$", "~1      $$$" },
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        char o[12] = { 0 };
        int r = fat_to_83(names[i].in, (unsigned)strlen(names[i].in), o);
        CHECK(r == 0 && memcmp(o, names[i].out, 11) == 0, "8.3 of \"%s\" gave \"%.11s\" (r=%d)", names[i].in, o, r);
    }
    const char *bad[] = { "TOOLONGNAME.BIN", "X.ABCD", ".BIN", "NAME.", "A.B.C", "SP CE", "a*b", "", "x+y" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char o[12];
        CHECK(fat_to_83(bad[i], (unsigned)strlen(bad[i]), o) != 0, "\"%s\" was accepted as an 8.3 name", bad[i]);
    }
    char s[13];
    fat_83_to_str("SURFOS  BIN", 0x18, s);
    CHECK(strcmp(s, "surfos.bin") == 0, "lower-case flags: \"%s\"", s);
    fat_83_to_str("README     ", 0, s);
    CHECK(strcmp(s, "README") == 0, "no extension: \"%s\"", s);

    uint8_t csd[16] = { 0 };
    csd[0] = 0x40;
    csd[7] = 0x00; csd[8] = 0xED; csd[9] = 0x3F;      /* C_SIZE 0xED3F: a "32 GB" card */
    CHECK(sd_csd_sectors(csd) == (0xED3Fu + 1u) * 1024u, "CSD 2.0 sectors %u", sd_csd_sectors(csd));
    memset(csd, 0, sizeof csd);                        /* CSD 1.0, 2 GB: BL 10, C_SIZE 4095, MULT 7 */
    csd[5] = 0x0A; csd[6] = 0x03; csd[7] = 0xFF; csd[8] = 0xC0; csd[9] = 0x03; csd[10] = 0x80;
    CHECK(sd_csd_sectors(csd) == 4194304u, "CSD 1.0 sectors %u", sd_csd_sectors(csd));
    CHECK(sd_div_for(25000000u, 400000u) == 31, "400 kHz divider %u", sd_div_for(25000000u, 400000u));
    CHECK(sd_div_for(25000000u, 6250000u) == 1, "6.25 MHz divider %u", sd_div_for(25000000u, 6250000u));
    CHECK(sd_div_for(25000000u, 12500000u) == 0, "12.5 MHz divider");
    CHECK(sd_div_for(25000000u, 50000000u) == 0, "fastest divider");
    CHECK(sd_div_for(25000000u, 1000u) == 255, "slowest divider");
    CHECK(sd_div_hz(25000000u, 31) <= 400000u, "identification clock above 400 kHz");

    for (int e = 0; e < SB_E_COUNT; e++)
        CHECK(sb_err_name(e)[0] != '?' && sb_err_led(e) <= 15, "error %d has no name or LED code", e);
    printf("unit: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

/* ---------------- options ---------------- */

struct expect {
    const char *code, *msg, *cmdline;
    const char *logs[16];
    const char *nologs[8];
    int nlogs, nnologs;
    long flags;
};

static struct sdsim_opts card_opts;
static const char *card_path;
static const char *boot_path;
static struct expect ex = { .flags = -1 };
static const char *lines[32];
static int nlines;
static int quiet;
static uint32_t ddr_mode, ddr_size = SURF_DDR_SIZE, ddr_calib_ms = 150, ddr_flip, btn;
static unsigned long seed = 1, iterations = 200, mutations = 4;

enum {
    O_TYPE = 256, O_ABSENT, O_NOCD, O_DEAD, O_MISOLOW, O_ACMD41, O_CMD8, O_NO33, O_NOCMD59, O_NCR, O_NAC,
    O_CRCERR, O_CRCEVERY, O_TOKERR, O_TIMEOUTS, O_REMOVE, O_MINDIV, O_DDR, O_DDRSIZE, O_DDRCAL, O_DDRFLIP,
    O_BTN0, O_PATH, O_EXPECT, O_EXPMSG, O_EXPLOG, O_EXPNOLOG, O_EXPFLAGS, O_EXPCMD, O_SEED, O_ITER, O_MUT,
    O_IOMODEL,
};

static const struct option longopts[] = {
    { "card", required_argument, 0, 'C' }, { "type", required_argument, 0, O_TYPE },
    { "absent", no_argument, 0, O_ABSENT }, { "no-cd", no_argument, 0, O_NOCD },
    { "dead", no_argument, 0, O_DEAD }, { "miso-low", no_argument, 0, O_MISOLOW },
    { "acmd41-busy", required_argument, 0, O_ACMD41 }, { "cmd8-bad-echo", no_argument, 0, O_CMD8 },
    { "no-33v", no_argument, 0, O_NO33 }, { "no-cmd59", no_argument, 0, O_NOCMD59 },
    { "ncr", required_argument, 0, O_NCR }, { "nac", required_argument, 0, O_NAC },
    { "crc-errors", required_argument, 0, O_CRCERR }, { "crc-every", required_argument, 0, O_CRCEVERY },
    { "token-errors", required_argument, 0, O_TOKERR }, { "read-timeouts", required_argument, 0, O_TIMEOUTS },
    { "remove-after", required_argument, 0, O_REMOVE }, { "min-div", required_argument, 0, O_MINDIV },
    { "ddr", required_argument, 0, O_DDR }, { "ddr-size", required_argument, 0, O_DDRSIZE },
    { "ddr-calib-ms", required_argument, 0, O_DDRCAL }, { "ddr-flip", no_argument, 0, O_DDRFLIP },
    { "btn0", no_argument, 0, O_BTN0 }, { "path", required_argument, 0, O_PATH },
    { "expect", required_argument, 0, O_EXPECT }, { "expect-msg", required_argument, 0, O_EXPMSG },
    { "expect-log", required_argument, 0, O_EXPLOG }, { "expect-no-log", required_argument, 0, O_EXPNOLOG },
    { "expect-flags", required_argument, 0, O_EXPFLAGS }, { "expect-cmdline", required_argument, 0, O_EXPCMD },
    { "seed", required_argument, 0, O_SEED }, { "iterations", required_argument, 0, O_ITER },
    { "mutations", required_argument, 0, O_MUT }, { "quiet", no_argument, 0, 'q' },
    { "command", required_argument, 0, 'c' }, { "io-model", no_argument, 0, O_IOMODEL }, { 0, 0, 0, 0 },
};

static int parse_options(int argc, char **argv)
{
    sdsim_default_opts(&card_opts);
    int c;
    while ((c = getopt_long(argc, argv, "C:c:q", longopts, 0)) != -1) {
        switch (c) {
        case 'C': card_path = optarg; break;
        case 'c': if (nlines < 32) lines[nlines++] = optarg; break;
        case 'q': quiet = 1; break;
        case O_IOMODEL: io_model = 1; break;
        case O_TYPE:
            io_kind = optarg;
            if (!strcmp(optarg, "sdhc")) card_opts.type = SDSIM_SDHC;
            else if (!strcmp(optarg, "sdsc2")) card_opts.type = SDSIM_SDSC2;
            else if (!strcmp(optarg, "sdsc1")) card_opts.type = SDSIM_SDSC1;
            else if (!strcmp(optarg, "mmc")) card_opts.type = SDSIM_MMC;
            else { fprintf(stderr, "fwtest: unknown card type %s\n", optarg); return -1; }
            break;
        case O_ABSENT: card_opts.absent = 1; break;
        case O_NOCD: card_opts.no_cd = 1; break;
        case O_DEAD: card_opts.dead = 1; break;
        case O_MISOLOW: card_opts.miso_low = 1; break;
        case O_ACMD41: card_opts.acmd41_busy = atoi(optarg); break;
        case O_CMD8: card_opts.cmd8_bad_echo = 1; break;
        case O_NO33: card_opts.no_33v = 1; break;
        case O_NOCMD59: card_opts.no_cmd59 = 1; break;
        case O_NCR: card_opts.ncr = atoi(optarg); break;
        case O_NAC: card_opts.nac = atoi(optarg); break;
        case O_CRCERR: card_opts.crc_errors = atoi(optarg); break;
        case O_CRCEVERY: card_opts.crc_every = atoi(optarg); break;
        case O_TOKERR: card_opts.token_errors = atoi(optarg); break;
        case O_TIMEOUTS: card_opts.read_timeouts = atoi(optarg); break;
        case O_REMOVE: card_opts.remove_after = atoi(optarg); break;
        case O_MINDIV: card_opts.min_div = atoi(optarg); break;
        case O_DDR:
            if (!strcmp(optarg, "ok")) ddr_mode = DDR_OK;
            else if (!strcmp(optarg, "absent")) ddr_mode = DDR_ABSENT;
            else if (!strcmp(optarg, "fail")) ddr_mode = DDR_FAIL;
            else if (!strcmp(optarg, "never")) ddr_mode = DDR_NEVER;
            else { fprintf(stderr, "fwtest: unknown DDR mode %s\n", optarg); return -1; }
            break;
        case O_DDRSIZE: ddr_size = (uint32_t)strtoul(optarg, 0, 0); break;
        case O_DDRCAL: ddr_calib_ms = (uint32_t)strtoul(optarg, 0, 0); break;
        case O_DDRFLIP: ddr_flip = 1; break;
        case O_BTN0: btn |= 1; break;
        case O_PATH: boot_path = optarg; break;
        case O_EXPECT: ex.code = optarg; break;
        case O_EXPMSG: ex.msg = optarg; break;
        case O_EXPLOG: if (ex.nlogs < 16) ex.logs[ex.nlogs++] = optarg; break;
        case O_EXPNOLOG: if (ex.nnologs < 8) ex.nologs[ex.nnologs++] = optarg; break;
        case O_EXPFLAGS: ex.flags = strtol(optarg, 0, 0); break;
        case O_EXPCMD: ex.cmdline = optarg; break;
        case O_SEED: seed = strtoul(optarg, 0, 0); break;
        case O_ITER: iterations = strtoul(optarg, 0, 0); break;
        case O_MUT: mutations = strtoul(optarg, 0, 0); break;
        default: return -1;
        }
    }
    return 0;
}

static int setup_board(void)
{
    host_reset();
    host.echo = !quiet;
    host.ddr_mode = (int)ddr_mode;
    host.ddr_size = ddr_size;
    host.ddr_calib_us = ddr_calib_ms * 1000u;
    host.ddr_flip = (int)ddr_flip;
    host.buttons = btn;
    host.spi_budget = 400000000ull;
    host.time_budget_ns = 3600ull * 1000000000ull;
    if (io_model) {
#ifdef FWTEST_IO_MODEL
        if (card_opts.no_cd || card_opts.dead || card_opts.miso_low || card_opts.acmd41_busy != 3 ||
            card_opts.cmd8_bad_echo || card_opts.no_33v || card_opts.no_cmd59 || card_opts.crc_every ||
            card_opts.read_timeouts || card_opts.remove_after || card_opts.min_div || card_opts.type == SDSIM_MMC) {
            fprintf(stderr, "fwtest: that card option exists only in sdcard_sim, not in the io model\n");
            return -1;
        }
        if (iomodel_attach(card_path, io_kind, (unsigned)seed, card_opts.absent || !card_path,
                           card_opts.crc_errors, card_opts.token_errors) != 0)
            return -1;
#else
        fprintf(stderr, "fwtest: built without the io track's SD model (make IO_MODEL_DIR=...)\n");
        return -1;
#endif
    } else if (card_path || card_opts.absent || card_opts.no_cd) {
        host.card = sdsim_open(card_path, &card_opts);
        if (!host.card)
            return -1;
    }
    sb_last_err = 0;
    sb_last_msg[0] = 0;
    return 0;
}

static int check_expectations(int err)
{
    int bad = 0;
    if (ex.code && strcmp(ex.code, sb_err_name(err)) != 0) {
        printf("EXPECTATION FAILED: result %s, expected %s\n", sb_err_name(err), ex.code);
        bad++;
    }
    if (ex.msg && !strstr(sb_last_msg, ex.msg)) {
        printf("EXPECTATION FAILED: message \"%s\" lacks \"%s\"\n", sb_last_msg, ex.msg);
        bad++;
    }
    for (int i = 0; i < ex.nlogs; i++)
        if (!host.out || !strstr(host.out, ex.logs[i])) {
            printf("EXPECTATION FAILED: the log lacks \"%s\"\n", ex.logs[i]);
            bad++;
        }
    for (int i = 0; i < ex.nnologs; i++)
        if (host.out && strstr(host.out, ex.nologs[i])) {
            printf("EXPECTATION FAILED: the log contains \"%s\"\n", ex.nologs[i]);
            bad++;
        }
    return bad;
}

/* After a successful boot: the boot information and the payload in DDR are as the ABI says. */
static int check_boot_result(struct sb_state *s)
{
    int bad = 0;
    struct surf_boot_info *bi = port_bootinfo();
    struct surf_boot_info copy = *bi;
    copy.bootinfo_crc32 = 0;
#define BI(cond, ...) do { if (!(cond)) { printf("BOOTINFO: " __VA_ARGS__); printf("\n"); bad++; } } while (0)
    BI(bi->magic == SURF_BOOTINFO_MAGIC, "magic %08x", bi->magic);
    BI(bi->version == 1 && bi->size == 256, "version %u size %u", bi->version, bi->size);
    BI(bi->bootinfo_crc32 == crc32(&copy, sizeof copy), "CRC %08x", bi->bootinfo_crc32);
    BI(bi->hart_id == 0 && bi->board_id == SURF_BOARD_SIM, "hart %u board %x", bi->hart_id, bi->board_id);
    BI(bi->soc_version == SURF_SOC_VERSION && bi->map_version == SURF_MAP_VERSION, "versions");
    BI(bi->sys_hz == SURF_SYS_HZ && bi->timebase_hz == SURF_TIMEBASE_HZ, "clocks %u %u", bi->sys_hz, bi->timebase_hz);
    BI(bi->ram_base == SURF_DDR_BASE && bi->ram_size == host.ddr_size, "ram %08x %u", bi->ram_base, bi->ram_size);
    BI(bi->image_base == s->img.h.load_addr && bi->image_mem_size == s->img.h.mem_size, "image range");
    BI(bi->boot_device == SURF_BOOT_DEVICE_SD && bi->part_lba == s->vol.part_lba, "device/partition");
    BI(bi->surfboot_version == sb_version(), "SurfBoot version");
    BI(bi->cmdline[SURF_BOOTINFO_CMDLINE_LEN - 1] == 0, "cmdline not terminated");
    for (int i = 0; i < 14; i++)
        BI(bi->reserved[i] == 0, "reserved[%d] = %08x", i, bi->reserved[i]);
    uint8_t *p = port_mem(s->img.h.load_addr, s->img.h.payload_size);
    BI(crc32(p, s->img.h.payload_size) == s->img.h.payload_crc32, "payload in DDR does not match its CRC");
    if (ex.flags >= 0)
        BI(bi->boot_flags == (uint32_t)ex.flags, "boot_flags %u, expected %ld", bi->boot_flags, ex.flags);
    if (ex.cmdline)
        BI(strcmp(bi->cmdline, ex.cmdline) == 0, "cmdline \"%s\", expected \"%s\"", bi->cmdline, ex.cmdline);
#undef BI
    return bad;
}

/* With --io-model: report the model's statistics; any protocol violation is a failure. */
static int io_model_done(void)
{
#ifdef FWTEST_IO_MODEL
    if (!io_model)
        return 0;
    unsigned long v = iomodel_violations();
    iomodel_report(0);
    iomodel_detach();
    if (v) {
        printf("EXPECTATION FAILED: the io model saw %lu protocol violation(s)\n", v);
        return 1;
    }
#endif
    return 0;
}

static int run_boot(void)
{
    if (setup_board() != 0)
        return 2;
    static struct sb_state st;
    memset(&st, 0, sizeof st);
    int err = sb_boot(&st, boot_path, (btn & 1u) ? SB_BOOT_BACKUP : SB_BOOT_NORMAL);
    int bad = 0;
    if (err == SB_OK) {
        bad += check_boot_result(&st);
        printf("RESULT OK path=%s entry=0x%08x load=0x%08x payload=%u flags=0x%x cmdline=\"%s\" fragments=%u "
               "spi_bytes=%llu sim_ms=%llu\n", st.image_path, st.img.h.entry, st.img.h.load_addr,
               st.img.h.payload_size, port_bootinfo()->boot_flags, port_bootinfo()->cmdline, st.img.fragments,
               (unsigned long long)host.spi_bytes, (unsigned long long)(host.now_ns / 1000000u));
    } else {
        printf("RESULT FAIL code=%s led=%u msg=%s\n", sb_err_name(err), sb_err_led(err), sb_last_msg);
    }
    bad += check_expectations(err);
    bad += io_model_done();
    host_free();
    return bad ? 1 : 0;
}

static int run_monitor(void)
{
    if (setup_board() != 0)
        return 2;
    static struct sb_state st;
    memset(&st, 0, sizeof st);
    int last = MON_CONTINUE;
    for (int i = 0; i < nlines; i++) {
        char buf[200];
        fmt_snprintf(buf, sizeof buf, "%s", lines[i]);
        sb_log("surfboot> %s\n", buf);
        last = sb_monitor_line(&st, buf);
        if (last != MON_CONTINUE)
            break;
    }
    printf("RESULT %s last_err=%s\n", last == MON_BOOT ? "BOOT" : last == MON_RESET ? "RESET" : "CONTINUE",
           sb_err_name(sb_last_err));
    int bad = check_expectations(ex.code ? sb_last_err : 0);
    bad += io_model_done();
    host_free();
    return bad ? 1 : 0;
}

/* ---------------- fuzzing ---------------- */

static uint64_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 11);
}

struct region { uint32_t lba, count; };

static int image_read(void *ctx, uint32_t lba, void *buf)
{
    struct { uint8_t *img; uint64_t size; } *m = ctx;
    if ((uint64_t)lba * 512u + 512u > m->size)
        return sb_fail(SB_E_SD_READ, "beyond the image");
    memcpy(buf, m->img + (uint64_t)lba * 512u, 512);
    return SB_OK;
}

static int run_fuzz(void)
{
    if (!card_path) {
        fprintf(stderr, "fwtest fuzz: --card is required\n");
        return 2;
    }
    FILE *f = fopen(card_path, "rb");
    if (!f) {
        perror(card_path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > (256u << 20)) {
        fprintf(stderr, "fwtest fuzz: use an image of at most 256 MiB\n");
        fclose(f);
        return 2;
    }
    uint8_t *img = malloc(size);
    if (!img || fread(img, 1, size, f) != size) {
        fprintf(stderr, "fwtest fuzz: cannot read %s\n", card_path);
        fclose(f);
        return 2;
    }
    fclose(f);

    /* Where the metadata is, from the undamaged image. */
    struct { uint8_t *img; uint64_t size; } mem = { img, size };
    static struct fat_vol v;
    struct region regs[8];
    int nregs = 0;
    regs[nregs++] = (struct region){ 0, 1 };
    if (fat_mount(&v, image_read, &mem, (uint32_t)(size / 512u)) == SB_OK) {
        regs[nregs++] = (struct region){ v.part_lba, 1 };
        regs[nregs++] = (struct region){ v.fat_lba, v.fat_sectors < 8 ? v.fat_sectors : 8 };
        regs[nregs++] = (struct region){ fat_cluster_lba(&v, v.root_cluster), v.spc };
        struct fat_dirent d;
        if (fat_lookup(&v, "/BOOT", &d) == SB_OK && d.cluster)
            regs[nregs++] = (struct region){ fat_cluster_lba(&v, d.cluster), v.spc };
        if (fat_lookup(&v, SB_PATH_MAIN, &d) == SB_OK && d.cluster)
            regs[nregs++] = (struct region){ fat_cluster_lba(&v, d.cluster), 1 };
        if (fat_lookup(&v, SB_PATH_BACKUP, &d) == SB_OK && d.cluster)
            regs[nregs++] = (struct region){ fat_cluster_lba(&v, d.cluster), 1 };
    } else {
        fprintf(stderr, "fwtest fuzz: the undamaged image does not mount: %s\n", sb_last_msg);
        free(img);
        return 2;
    }

    unsigned long counts[SB_E_COUNT] = { 0 };
    quiet = 1;
    rng_state = seed * 0x9E3779B97F4A7C15ull + 1;
    for (unsigned long it = 0; it < iterations; it++) {
        struct { uint64_t off; uint8_t old; } undo[64];
        unsigned nm = 1 + rnd() % (unsigned)(mutations ? mutations : 1);
        if (nm > 64)
            nm = 64;
        for (unsigned k = 0; k < nm; k++) {
            struct region r = regs[rnd() % (unsigned)nregs];
            uint64_t off = ((uint64_t)r.lba + rnd() % r.count) * 512u + rnd() % 512u;
            undo[k].off = off;
            undo[k].old = img[off];
            switch (rnd() % 4) {
            case 0: img[off] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 1: img[off] = (uint8_t)rnd(); break;
            case 2: img[off] = 0x00; break;
            default: img[off] = 0xFF; break;
            }
        }
        host_reset();
        host.echo = 0;
        host.spi_budget = 200000000ull;
        host.time_budget_ns = 600ull * 1000000000ull;
        host.card = sdsim_open_mem(img, size, &card_opts);
        static struct sb_state st;
        memset(&st, 0, sizeof st);
        int err = sb_boot(&st, 0, SB_BOOT_NORMAL);
        if (err == SB_OK && check_boot_result(&st) != 0) {
            printf("fuzz: seed %lu iteration %lu booted with a bad result\n", seed, it);
            failures++;
        }
        if (err >= 0 && err < SB_E_COUNT)
            counts[err]++;
        sdsim_close(host.card);
        host.card = 0;
        for (int k = (int)nm - 1; k >= 0; k--)
            img[undo[k].off] = undo[k].old;
    }
    printf("fuzz: seed %lu, %lu runs, %lu mutations at most each; outcomes:", seed, iterations, mutations);
    for (int e = 0; e < SB_E_COUNT; e++)
        if (counts[e])
            printf(" %s=%lu", sb_err_name(e), counts[e]);
    printf("\nfuzz: %s\n", failures ? "FAIL" : "PASS (every run ended with a result)");
    free(img);
    host_free();
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: fwtest unit | boot | monitor | fuzz [options] (see fwtest.c)\n");
        return 2;
    }
    const char *mode = argv[1];
    optind = 2;
    if (parse_options(argc, argv) != 0)
        return 2;
    if (!strcmp(mode, "unit"))
        return unit();
    if (!strcmp(mode, "boot"))
        return run_boot();
    if (!strcmp(mode, "monitor"))
        return run_monitor();
    if (!strcmp(mode, "fuzz"))
        return run_fuzz();
    fprintf(stderr, "fwtest: unknown mode %s\n", mode);
    return 2;
}
