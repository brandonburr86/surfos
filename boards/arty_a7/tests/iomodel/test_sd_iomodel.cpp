/*
SurfOS host test: the SD driver against the io track's card model
--------------------
File: test_sd_iomodel.cpp  Date: 10/15/26 (SurfCPU)
--------------------
boards/arty_a7/sdcard.c, the driver the Arty kernel links, against SdCardModel
(surfcpu/sim/models/sdcard_model.{h,cpp}, the io track's bit-level card, the one surf_spi is
tested with), through a software SPI master in mode 0 with surf_spi's timing: a half period
of DIV + 1 clk cycles, 32 for identification (DIV 31, 390.625 kHz) and 1 for data (DIV 0,
12.5 MHz), a millisecond being 25,000 clk cycles. Three kinds of card and several seeds:
initialization, 200 random reads and writes checked against a shadow copy and the card's
backdoor, every injected fault the model has (a bad read CRC, a rejected write, a read error
token, a missing response, a write error, a slow card, an initialization that never ends,
no card), and the model's count of host protocol mistakes, which must stay at zero.

10/16/26 (milestone 8): the faults a write can meet, as the kernel meets them: a write error
on the first block of a multiple-block write (the stop token, a card error, nothing of it
written), a write command without a response, and the card taken out in the middle of a
multiple-block write (set_present(false) after a number of bytes): the write fails within
the driver's bounds, card detect then fails every transfer at once (SD_ENOCARD), the card
put back has lost its power and answers nothing until it is initialized again, and what was
written before it went is still on it.

    test_sd_iomodel [seed...]       (make -C surfcpu test-os-iomodel)
*/

#include "sdcard_model.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" {
int glue_init(void);
int glue_read(unsigned int lba, unsigned int count, unsigned char *buf);
int glue_write(unsigned int lba, unsigned int count, const unsigned char *buf);
unsigned long long glue_blocks(void);
int glue_block_addr(void);
int glue_v2(void);
int glue_crc_on(void);
unsigned long glue_crc_retries(void);
const char *glue_why(void);
const char *glue_strerror(int err);
}

#define SD_OK        0
#define SD_ENOCARD  -1
#define SD_ETIMEOUT -2
#define SD_EIO      -4

static int checks, failures;
static void check(bool ok, const std::string &what) {
    checks++;
    if (!ok) {
        failures++;
        std::printf("FAIL %s\n", what.c_str());
    }
}

/* the card and surf_spi's timing, as test_sdcard_model.cpp's SoftSpi */
static SdCardModel *card;
static int half = 32;
static bool cs_n = true, mosi = true, miso = true;
static unsigned long long ticks;
static unsigned long long xfers, pull_at;       /* 10/16/26: the card comes out after byte pull_at */

static void tick(bool sclk) {
    miso = card->tick(cs_n, sclk, mosi);
    ticks++;
}

extern "C" unsigned char io_xfer(unsigned char out) {
    unsigned char in = 0;
    if (pull_at && ++xfers == pull_at) card->set_present(false);
    for (int b = 7; b >= 0; b--) {
        mosi = (out >> b) & 1;
        for (int i = 0; i < half; i++) tick(false);
        in = static_cast<unsigned char>(in << 1 | (miso ? 1 : 0));    /* sampled as SCLK rises */
        for (int i = 0; i < half; i++) tick(true);
    }
    mosi = true;
    return in;
}

extern "C" void io_select(int on) {
    tick(false);
    cs_n = !on;
    tick(false);
}

extern "C" void io_slow(int slow) { half = slow ? 32 : 1; }
extern "C" int io_present(void) { return !card->cd_n(); }
extern "C" unsigned int io_ms(void) { return static_cast<unsigned int>(ticks / 25000); }

static const char *kind_name(SdCardOptions::Kind k) {
    return k == SdCardOptions::Kind::SDHC ? "SDHC" : k == SdCardOptions::Kind::SDSC_V2 ? "SDSC v2" : "SDSC v1";
}

static void run_kind(SdCardOptions::Kind kind, unsigned seed) {
    SdCardOptions opt;
    opt.kind = kind;
    opt.seed = seed;
    opt.ncr_max = 8;
    opt.nac_max = 40;
    std::string tag = std::string(kind_name(kind)) + " seed " + std::to_string(seed) + ": ";
    SdCardModel model("", opt);
    card = &model;
    half = 32;
    cs_n = true;
    ticks = 0;
    std::mt19937 rng(seed);
    uint64_t blocks = model.blocks();
    std::vector<uint8_t> shadow(2048 * 512), buf(64 * 512), back(512);
    for (uint64_t lba = 0; lba < 2048; lba++) {                     /* the first 1 MB has contents */
        for (int i = 0; i < 512; i++) shadow[lba * 512 + i] = static_cast<uint8_t>(rng());
        model.write_block(lba, &shadow[lba * 512]);
    }
    int r = glue_init();
    check(r == SD_OK && glue_blocks() == blocks && glue_block_addr() == (kind == SdCardOptions::Kind::SDHC) &&
              glue_v2() == (kind != SdCardOptions::Kind::SDSC_V1),
          tag + "initializes (" + glue_strerror(r) + ", " + glue_why() + ")");
    if (r != SD_OK) return;
    bool ok = true;
    for (int op = 0; op < 200 && ok; op++) {
        unsigned count = 1 + rng() % ((rng() % 4 == 0) ? 64 : 4), lba = rng() % (2048 - count);
        if (rng() % 3 == 0) {
            for (unsigned i = 0; i < count * 512; i++) buf[i] = static_cast<uint8_t>(rng());
            ok = glue_write(lba, count, buf.data()) == SD_OK;
            std::memcpy(&shadow[lba * 512], buf.data(), count * 512);
            for (unsigned i = 0; i < count && ok; i++)
                ok = model.read_block(lba + i, back.data()) && !std::memcmp(back.data(), &shadow[(lba + i) * 512], 512);
        } else {
            ok = glue_read(lba, count, buf.data()) == SD_OK && !std::memcmp(buf.data(), &shadow[lba * 512], count * 512);
        }
        if (!ok) std::printf("  op %d at %u x %u: %s\n", op, lba, count, glue_why());
    }
    check(ok, tag + "200 random reads and writes of 1-64 blocks match the shadow and the card's backdoor");

    model.faults.read_bad_crc = 1;
    unsigned long retries = glue_crc_retries();
    check(glue_read(100, 4, buf.data()) == SD_OK && glue_crc_retries() == retries + 1 &&
              !std::memcmp(buf.data(), &shadow[100 * 512], 4 * 512), tag + "a block with a bad CRC is read again");
    model.faults.write_crc_reject = 1;
    for (int i = 0; i < 2 * 512; i++) buf[i] = static_cast<uint8_t>(i * 3);
    r = glue_write(200, 2, buf.data());
    std::memcpy(&shadow[200 * 512], buf.data(), 2 * 512);
    check(r == SD_OK && model.read_block(201, back.data()) && !std::memcmp(back.data(), &shadow[201 * 512], 512),
          tag + "a written block rejected for its CRC is sent again (CMD25, the stop token)");
    model.faults.read_error_token = 1;
    check(glue_read(300, 1, buf.data()) == SD_EIO, tag + "a read error token is a card error");
    model.faults.no_response = 1;
    check(glue_read(301, 1, buf.data()) == SD_ETIMEOUT, tag + "a command without a response times out");
    model.faults.write_error = 1;
    check(glue_write(302, 1, buf.data()) == SD_EIO, tag + "a write error response is a card error");
    model.faults.busy_extra = 200000;
    r = glue_write(303, 1, buf.data());
    check(r == SD_OK, tag + "a slow card (8 ms more busy) is waited for (" + glue_strerror(r) + ", " + glue_why() + ")");
    std::memcpy(&shadow[303 * 512], buf.data(), 512);
    check(glue_read(0, 8, buf.data()) == SD_OK && !std::memcmp(buf.data(), shadow.data(), 8 * 512),
          tag + "the card works on after every fault");
    check(glue_read(static_cast<unsigned int>(blocks - 1), 2, buf.data()) != SD_OK, tag + "a read past the end is refused");

    /* 10/16/26: the faults of a write (milestone 8) */
    for (int i = 0; i < 4 * 512; i++) buf[i] = static_cast<uint8_t>(i * 5 + 1);
    model.faults.write_error = 1;
    r = glue_write(400, 4, buf.data());
    ok = r == SD_EIO;
    for (unsigned i = 0; i < 4 && ok; i++) ok = model.read_block(400 + i, back.data()) && !std::memcmp(back.data(), &shadow[(400 + i) * 512], 512);
    check(ok, tag + "a write error on the first block of a multiple write: a card error, none of it written (" + glue_strerror(r) + ")");
    model.faults.no_response = 1;
    r = glue_write(404, 2, buf.data());
    check(r == SD_ETIMEOUT, tag + "a write command without a response times out (" + std::string(glue_strerror(r)) + ")");
    r = glue_write(400, 4, buf.data());
    std::memcpy(&shadow[400 * 512], buf.data(), 4 * 512);
    std::vector<uint8_t> got(16 * 512), pulled(16 * 512);
    check(r == SD_OK && glue_read(400, 4, got.data()) == SD_OK && !std::memcmp(got.data(), &shadow[400 * 512], 4 * 512),
          tag + "and the same write goes through afterwards");
    for (int i = 0; i < 16 * 512; i++) pulled[i] = static_cast<uint8_t>(i * 11 + 7);
    uint64_t w0 = model.stats.blocks_written;
    xfers = 0;
    pull_at = 8 * 520;                          /* some blocks into sixteen */
    unsigned long long t0 = ticks;
    r = glue_write(500, 16, pulled.data());
    pull_at = 0;
    unsigned stored = static_cast<unsigned>(model.stats.blocks_written - w0);
    check(r != SD_OK && stored > 0 && stored < 16 && (ticks - t0) / 25000 < 1100,
          tag + "taken out after " + std::to_string(stored) + " of 16 blocks: the write fails (" + glue_strerror(r) + ", " +
          glue_why() + ") within the driver's bounds (" + std::to_string((ticks - t0) / 25000) + " ms)");
    check(glue_read(0, 1, got.data()) == SD_ENOCARD && glue_write(0, 1, got.data()) == SD_ENOCARD,
          tag + "while it is out, card detect fails every read and write at once");
    model.set_present(true);
    r = glue_read(0, 1, got.data());
    check(r != SD_OK, tag + "put back, it lost its power: nothing it answers is taken for data (" + std::string(glue_strerror(r)) + ")");
    r = glue_init();
    ok = r == SD_OK && glue_read(0, 8, got.data()) == SD_OK && !std::memcmp(got.data(), shadow.data(), 8 * 512);
    for (unsigned i = 0; i < stored && ok; i++) ok = model.read_block(500 + i, back.data()) && !std::memcmp(back.data(), &pulled[i * 512], 512);
    check(ok, tag + "initialized again it works, and the " + std::to_string(stored) + " blocks written before it went are on it");
    check(model.stats.violations == 0, tag + "no protocol mistakes by the driver (" + std::to_string(model.stats.violations) + ")");
    if (model.stats.violations)
        for (size_t i = 0; i < model.log.size(); i++)
            if (model.log[i].compare(0, 9, "VIOLATION") == 0)     /* the mistake and what came before it */
                for (size_t j = i < 6 ? 0 : i - 6; j <= i; j++) std::printf("    %s\n", model.log[j].c_str());
}

int main(int argc, char **argv) {
    std::vector<unsigned> seeds = {1, 7, 123};
    if (argc > 1) {
        seeds.clear();
        for (int i = 1; i < argc; i++) seeds.push_back(static_cast<unsigned>(std::strtoul(argv[i], nullptr, 0)));
    }
    for (unsigned seed : seeds) {
        run_kind(SdCardOptions::Kind::SDHC, seed);
        run_kind(SdCardOptions::Kind::SDSC_V2, seed);
        run_kind(SdCardOptions::Kind::SDSC_V1, seed);
    }
    {   /* no card, and an initialization that never finishes */
        SdCardOptions opt;
        opt.present = false;
        SdCardModel model("", opt);
        card = &model;
        half = 32;
        ticks = 0;
        check(glue_init() == SD_ENOCARD, "no card in the socket");
    }
    {
        SdCardModel model("", SdCardOptions());
        model.faults.init_never_ready = true;
        card = &model;
        half = 32;
        ticks = 0;
        check(glue_init() == SD_ETIMEOUT, "an initialization that never finishes times out (1 s)");
    }
    std::printf("test_sd_iomodel: %d of %d checks passed (SdCardModel, seeds", checks - failures, checks);
    for (unsigned s : seeds) std::printf(" %u", s);
    std::printf(")\n");
    return failures ? 1 : 0;
}
