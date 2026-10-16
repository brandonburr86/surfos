/*
SurfCPU firmware tests: SurfBoot against the io track's SD card model
--------------------
File: iomodel_glue.cpp Date: 10/15/26 (SurfCPU)
--------------------
Puts sim/models/sdcard_model (the io track's pin-level model, the one the RTL system
simulation uses) behind port_host.c's SPI calls, so SurfBoot's own sd.c, fat32.c and boot.c
run against it: each byte is clocked in SPI mode 0 with SCLK = sys_clk / (2 (DIV + 1)), as
surf_spi makes it (MOSI set while SCLK is low, MISO sampled just before SCLK rises, the card
sampling MOSI on the rising edge). The model counts protocol mistakes (stats.violations);
fwtest --io-model fails when there is one. Built only when the model's source is in the
tree (mk/area-fw.mk, IO_MODEL_DIR).
*/
#include <cstdio>
#include <string>
#include "sdcard_model.h"

extern "C" {
#include "port_host.h"
int iomodel_attach(const char *image, const char *kind, unsigned seed, int absent, int read_bad_crc,
                   int error_tokens);
unsigned long iomodel_violations(void);
void iomodel_report(int all);
void iomodel_detach(void);
}

static SdCardModel *card;
static bool cs_n = true;

static uint8_t io_xfer(int cs, uint8_t out)
{
    bool want = !cs;
    if (want != cs_n) {                             /* chip select changes with SCLK low */
        card->tick(cs_n, false, true);
        cs_n = want;
        card->tick(cs_n, false, true);
    }
    int half = static_cast<int>(host.spi_div) + 1;
    uint8_t in = 0;
    bool miso = true;
    for (int b = 7; b >= 0; b--) {
        bool mosi = (out >> b) & 1;
        for (int i = 0; i < half; i++)
            miso = card->tick(cs_n, false, mosi);
        in = static_cast<uint8_t>(in << 1 | (miso ? 1 : 0));
        for (int i = 0; i < half; i++)
            card->tick(cs_n, true, mosi);
    }
    return in;
}

static int io_present(void) { return card->cd_n() ? 0 : 1; }

int iomodel_attach(const char *image, const char *kind, unsigned seed, int absent, int read_bad_crc,
                   int error_tokens)
{
    SdCardOptions o;
    std::string k = kind ? kind : "sdhc";
    o.kind = k == "sdsc2" ? SdCardOptions::Kind::SDSC_V2 : k == "sdsc1" ? SdCardOptions::Kind::SDSC_V1
           : SdCardOptions::Kind::SDHC;
    o.seed = seed;
    o.present = !absent;
    card = new SdCardModel(image ? image : "", o);
    if (!card->error().empty()) {
        std::fprintf(stderr, "iomodel: %s\n", card->error().c_str());
        return -1;
    }
    card->faults.read_bad_crc = read_bad_crc;
    card->faults.read_error_token = error_tokens;
    card->faults.error_token = 0x08;
    cs_n = true;
    host.xfer_hook = io_xfer;
    host.present_hook = io_present;
    return 0;
}

unsigned long iomodel_violations(void) { return card ? static_cast<unsigned long>(card->stats.violations) : 0; }

void iomodel_report(int all)
{
    if (!card)
        return;
    const SdCardModel::Stats &s = card->stats;
    std::printf("IOMODEL commands CMD0 %llu CMD8 %llu CMD9 %llu CMD10 %llu CMD16 %llu CMD17 %llu CMD58 %llu CMD59 "
                "%llu ACMD41 %llu; blocks read %llu; ignored: power-up %llu busy %llu sd-mode %llu; command CRC "
                "errors %llu; violations %llu\n",
                (unsigned long long)s.cmds[0], (unsigned long long)s.cmds[8], (unsigned long long)s.cmds[9],
                (unsigned long long)s.cmds[10], (unsigned long long)s.cmds[16], (unsigned long long)s.cmds[17],
                (unsigned long long)s.cmds[58], (unsigned long long)s.cmds[59], (unsigned long long)s.acmds[41],
                (unsigned long long)s.blocks_read, (unsigned long long)s.ignored_powerup,
                (unsigned long long)s.ignored_busy, (unsigned long long)s.ignored_sdmode,
                (unsigned long long)s.cmd_crc_errors, (unsigned long long)s.violations);
    for (const std::string &line : card->log)
        if (all || line.rfind("VIOLATION", 0) == 0)
            std::printf("IOMODEL %s\n", line.c_str());
}

void iomodel_detach(void)
{
    host.xfer_hook = nullptr;
    host.present_hook = nullptr;
    delete card;
    card = nullptr;
}
