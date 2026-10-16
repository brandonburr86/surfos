/*
SurfCPU SD card model tests
--------------------
File: test_sdcard_model.cpp Date: 10/15/26 (SurfCPU)
--------------------
SdCardModel on its own, driven by a software SPI master (mode 0, any half period) and the
test SD host of sd_host.h: CRCs against known values and an independent implementation,
the power-up and SPI-mode entry rules, initialization of SDHC, SDSC v2 and SDSC v1 cards
(and the ways it must fail), CSD/CID, single and multiple block reads and writes, CMD12's
stuff byte, the end of the card, SDSC byte addressing and block lengths, busy, CRC
checking, every injected fault, card absence and removal, and that writes reach the image
file only on flush().

    test_sdcard_model <work directory> [seed]
*/
#include "../models/sdcard_model.h"
#include "sd_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
        }                                                                  \
    } while (0)

struct SoftSpi {
    SdCardModel &card;
    int half;
    bool cs_n = true, mosi = true, miso = true;
    uint64_t ticks = 0;
    SoftSpi(SdCardModel &c, int h) : card(c), half(h) {}
    void tick(bool sclk) {
        miso = card.tick(cs_n, sclk, mosi);
        ticks++;
    }
    uint8_t xfer(uint8_t out) {
        uint8_t in = 0;
        for (int b = 7; b >= 0; b--) {
            mosi = (out >> b) & 1;
            for (int i = 0; i < half; i++) tick(false);
            in = static_cast<uint8_t>(in << 1 | (miso ? 1 : 0));   // sampled as SCLK rises
            for (int i = 0; i < half; i++) tick(true);
        }
        mosi = true;
        return in;
    }
    void cs(bool assert) {
        tick(false);
        cs_n = !assert;
        tick(false);
    }
    SdHost host() {
        return SdHost([this](uint8_t b) { return xfer(b); }, [this](bool a) { cs(a); });
    }
};

static std::string g_dir;

static std::string make_image(const std::string &name, uint64_t blocks, unsigned seed) {
    std::string path = g_dir + "/" + name;
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::printf("cannot create %s\n", path.c_str());
        std::exit(2);
    }
    std::mt19937 rng(seed);
    std::vector<uint8_t> blk(512);
    for (uint64_t lba = 0; lba < blocks; lba++) {
        for (int i = 0; i < 512; i++) blk[i] = static_cast<uint8_t>(rng());
        blk[0] = static_cast<uint8_t>(lba);                 // each block names itself
        blk[1] = static_cast<uint8_t>(lba >> 8);
        blk[2] = static_cast<uint8_t>(lba >> 16);
        std::fwrite(blk.data(), 1, 512, f);
    }
    std::fclose(f);
    return path;
}

static bool file_block(const std::string &path, uint64_t lba, uint8_t *buf) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, static_cast<long>(lba * 512), SEEK_SET);
    bool ok = std::fread(buf, 1, 512, f) == 512;
    std::fclose(f);
    return ok;
}

static void test_crc() {
    const uint8_t cmd0[5] = {0x40, 0, 0, 0, 0};
    const uint8_t cmd8[5] = {0x48, 0, 0, 0x01, 0xAA};
    const uint8_t cmd17[5] = {0x51, 0, 0, 0, 0};
    CHECK(SdCardModel::crc7(cmd0, 5) == 0x4A, "CRC7(CMD0) = %02x, want 4a (0x95 on the wire)", SdCardModel::crc7(cmd0, 5));
    CHECK(SdCardModel::crc7(cmd8, 5) == 0x43, "CRC7(CMD8 1AA) = %02x, want 43 (0x87)", SdCardModel::crc7(cmd8, 5));
    CHECK(SdCardModel::crc7(cmd17, 5) == 0x2A, "CRC7(CMD17 0) = %02x, want 2a (0x55)", SdCardModel::crc7(cmd17, 5));
    std::vector<uint8_t> ff(512, 0xFF);
    CHECK(SdCardModel::crc16(ff.data(), 512) == 0x7FA1, "CRC16(512 x FF) = %04x, want 7fa1", SdCardModel::crc16(ff.data(), 512));
    const uint8_t s[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(SdCardModel::crc16(s, 9) == 0x31C3, "CRC16(\"123456789\") = %04x, want 31c3 (XModem)", SdCardModel::crc16(s, 9));
    std::mt19937 rng(99);
    int mism = 0;
    for (int n = 0; n < 200; n++) {
        std::vector<uint8_t> d(1 + rng() % 600);
        for (auto &b : d) b = static_cast<uint8_t>(rng());
        if (SdCardModel::crc16(d.data(), d.size()) != SdHost::crc16(d.data(), d.size())) mism++;
        if (SdCardModel::crc7(d.data(), d.size()) != SdHost::crc7(d.data(), d.size())) mism++;
    }
    CHECK(mism == 0, "model and host CRCs disagree %d times", mism);
}

static void test_powerup_and_spi_entry(unsigned seed) {
    SdCardOptions o;
    o.seed = seed;
    SdCardModel card("", o);
    SoftSpi spi(card, 1);
    SdHost h = spi.host();
    // CMD0 without the power-up clocks: ignored
    spi.cs(true);
    uint8_t r = h.cmd(0, 0);
    CHECK(r == 0xFF && card.stats.ignored_powerup == 1, "CMD0 before power-up clocks: r1 %02x, ignored %llu", r,
          (unsigned long long)card.stats.ignored_powerup);
    spi.cs(false);
    for (int i = 0; i < 10; i++) spi.xfer(0xFF);
    spi.cs(true);
    // before CMD0 the card ignores everything else, and CMD0 with a bad CRC
    r = h.cmd(8, 0x1AA);
    CHECK(r == 0xFF && card.stats.ignored_sdmode == 1, "CMD8 in SD mode: r1 %02x", r);
    r = h.cmd(0, 0, true);
    CHECK(r == 0xFF && !card.in_spi_mode(), "CMD0 with a bad CRC: r1 %02x", r);
    r = h.cmd(0, 0);
    CHECK(r == 0x01 && card.in_spi_mode(), "CMD0: r1 %02x", r);
    // in idle state: CMD8 with a bad CRC, illegal commands
    r = h.cmd(8, 0x1AA, true);
    CHECK(r == 0x09, "CMD8 with a bad CRC: r1 %02x, want 09", r);
    r = h.cmd(17, 0);
    CHECK(r == 0x05, "CMD17 in idle state: r1 %02x, want 05", r);
    r = h.cmd(58, 0);
    uint32_t ocr = h.get32();
    CHECK(r == 0x01 && ocr == 0x00FF8000u, "CMD58 before init: r1 %02x ocr %08x", r, ocr);
    spi.cs(false);
}

static void test_init_kinds(unsigned seed) {
    struct Case { SdCardOptions::Kind kind; bool cmd8; bool hcs; bool ok; bool sdhc; const char *name; };
    const Case cases[] = {
        {SdCardOptions::Kind::SDHC, true, true, true, true, "SDHC"},
        {SdCardOptions::Kind::SDHC, false, true, false, false, "SDHC without CMD8"},
        {SdCardOptions::Kind::SDHC, true, false, false, false, "SDHC with HCS=0"},
        {SdCardOptions::Kind::SDSC_V2, true, true, true, false, "SDSC v2"},
        {SdCardOptions::Kind::SDSC_V2, true, false, true, false, "SDSC v2 with HCS=0"},
        {SdCardOptions::Kind::SDSC_V1, true, true, true, false, "SDSC v1"},
    };
    for (const Case &c : cases) {
        SdCardOptions o;
        o.kind = c.kind;
        o.seed = seed;
        o.acmd41_busy = 2 + seed % 4;
        SdCardModel card("", o);
        SoftSpi spi(card, 1 + seed % 3);
        SdHost h = spi.host();
        h.acmd41_tries = 20;
        bool ok = h.init(c.cmd8, c.hcs);
        CHECK(ok == c.ok, "%s: init %s (%s)", c.name, ok ? "succeeded" : "failed", h.err.c_str());
        if (ok) {
            CHECK(h.sdhc == c.sdhc, "%s: CCS %d", c.name, h.sdhc);
            CHECK(card.initialized(), "%s: model not initialized", c.name);
            if (c.kind == SdCardOptions::Kind::SDHC)
                CHECK(h.ocr == 0xC0FF8000u, "%s: OCR %08x", c.name, h.ocr);
            CHECK(h.v2 == (c.kind != SdCardOptions::Kind::SDSC_V1), "%s: v2 %d", c.name, h.v2);
            CHECK(card.stats.acmds[41] == static_cast<uint64_t>(o.acmd41_busy + 1), "%s: %llu ACMD41s", c.name,
                  (unsigned long long)card.stats.acmds[41]);
        }
        CHECK(card.stats.violations == 0, "%s: %llu violations", c.name, (unsigned long long)card.stats.violations);
    }
}

static void test_registers(unsigned seed) {
    const uint64_t sizes[] = {16384, 8192, 1u << 21};
    for (int kind = 0; kind < 3; kind++) {
        SdCardOptions o;
        o.kind = kind == 0 ? SdCardOptions::Kind::SDHC : SdCardOptions::Kind::SDSC_V2;
        o.blank_blocks = sizes[kind];
        o.seed = seed;
        SdCardModel card("", o);
        SoftSpi spi(card, 1);
        SdHost h = spi.host();
        CHECK(h.init(), "registers: init (%s)", h.err.c_str());
        uint8_t csd[16], cid[16];
        CHECK(h.read_reg(9, csd), "CMD9: %s", h.err.c_str());
        CHECK(SdHost::csd_blocks(csd) == card.blocks(), "CSD capacity %llu blocks, card %llu",
              (unsigned long long)SdHost::csd_blocks(csd), (unsigned long long)card.blocks());
        CHECK((csd[0] >> 6) == (kind == 0 ? 1 : 0), "CSD structure %d", csd[0] >> 6);
        CHECK(h.read_reg(10, cid), "CMD10: %s", h.err.c_str());
        CHECK(std::memcmp(cid + 3, "SURFC", 5) == 0, "CID product name");
        uint8_t r2 = 0xFF;
        CHECK(h.status(r2) && r2 == 0, "CMD13: r2 %02x", r2);
        spi.cs(true);
        uint8_t r = h.cmd(51, 0);                    // ACMD51 needs CMD55 first: CMD51 alone is illegal
        CHECK(r == 0x04, "CMD51 without CMD55: r1 %02x", r);
        r = h.acmd(51, 0);
        uint8_t scr[8];
        CHECK(r == 0x00 && h.get_block(scr, 8) && scr[0] == 0x02, "ACMD51: r1 %02x scr0 %02x", r, scr[0]);
        r = h.cmd(60, 0);
        CHECK(r == 0x04, "unknown CMD60: r1 %02x", r);
        spi.cs(false);
    }
}

static void test_read_write(unsigned seed) {
    const uint64_t blocks = 16384;
    std::string img = make_image("model-sdhc.img", blocks, seed);
    SdCardOptions o;
    o.seed = seed;
    o.ncr_min = 1;
    o.ncr_max = 8;
    o.nac_min = 0;
    o.nac_max = 20;
    SdCardModel card(img, o);
    CHECK(card.error().empty() && card.blocks() == blocks, "image: %s, %llu blocks", card.error().c_str(),
          (unsigned long long)card.blocks());
    SoftSpi spi(card, 1);
    SdHost h = spi.host();
    CHECK(h.init(), "init: %s", h.err.c_str());
    std::mt19937 rng(seed);
    uint8_t buf[512 * 8], ref[512];
    // single blocks: first, last, random
    const uint32_t lbas[] = {0, 1, static_cast<uint32_t>(blocks - 1), static_cast<uint32_t>(rng() % blocks)};
    for (uint32_t lba : lbas) {
        bool ok = h.read_block(lba, buf);
        file_block(img, lba, ref);
        CHECK(ok && std::memcmp(buf, ref, 512) == 0, "CMD17 lba %u: %s", lba, h.err.c_str());
    }
    // multiple blocks, CMD12
    uint32_t start = static_cast<uint32_t>(rng() % (blocks - 8));
    bool ok = h.read_multi(start, 8, buf);
    int bad = 0;
    for (int i = 0; i < 8; i++) {
        file_block(img, start + i, ref);
        if (std::memcmp(buf + 512 * i, ref, 512)) bad++;
    }
    CHECK(ok && bad == 0, "CMD18 at %u: %s, %d bad blocks", start, h.err.c_str(), bad);
    // past the end
    spi.cs(true);
    uint8_t r = h.cmd(17, static_cast<uint32_t>(blocks));
    CHECK(r == 0x40, "CMD17 past the end: r1 %02x, want 40", r);
    spi.cs(false);
    spi.cs(true);
    r = h.cmd(18, static_cast<uint32_t>(blocks - 1));
    bool got_last = r == 0 && h.get_block(buf, 512);
    bool got_more = h.get_block(buf, 512);
    CHECK(got_last && !got_more && h.last_token == 0x08, "CMD18 into the end: last %d, then token %02x", got_last,
          h.last_token);
    r = h.cmd(12, 0, false, false);
    CHECK(r == 0x00 && h.wait_ready(), "CMD12 after the out-of-range token: r1 %02x", r);
    spi.cs(false);
    // writes stay in memory until flush
    uint8_t w[512 * 4];
    for (auto &b : w) b = static_cast<uint8_t>(rng());
    const uint32_t wl = 100 + rng() % 1000;
    CHECK(h.write_block(wl, w), "CMD24: %s", h.err.c_str());
    CHECK(h.read_block(wl, buf) && std::memcmp(buf, w, 512) == 0, "read back after CMD24");
    file_block(img, wl, ref);
    CHECK(std::memcmp(ref, w, 512) != 0, "image file changed before flush()");
    CHECK(h.write_multi(wl + 10, 4, w), "CMD25: %s", h.err.c_str());
    CHECK(h.read_multi(wl + 10, 4, buf) && std::memcmp(buf, w, 2048) == 0, "read back after CMD25");
    CHECK(card.dirty_blocks() == 5 && card.stats.blocks_written == 5, "%zu dirty, %llu written", card.dirty_blocks(),
          (unsigned long long)card.stats.blocks_written);
    CHECK(card.flush() && card.dirty_blocks() == 0, "flush: %s", card.error().c_str());
    file_block(img, wl + 12, ref);
    CHECK(std::memcmp(ref, w + 1024, 512) == 0, "image file after flush()");
    CHECK(h.read_block(wl + 12, buf) && std::memcmp(buf, w + 1024, 512) == 0, "read after flush");
    CHECK(card.stats.violations == 0, "%llu violations", (unsigned long long)card.stats.violations);
}

static void test_stuff_byte_and_busy(unsigned seed) {
    SdCardOptions o;
    o.seed = seed;
    o.nac_min = o.nac_max = 1;
    o.ncr_min = o.ncr_max = 1;
    o.write_busy_min = o.write_busy_max = 3000;
    SdCardModel card("", o);
    uint8_t pattern[512];
    for (int i = 0; i < 512; i++) pattern[i] = static_cast<uint8_t>(i * 7 + 3);
    for (int lba = 0; lba < 4; lba++) card.write_block(lba, pattern);
    SoftSpi spi(card, 2);
    SdHost h = spi.host();
    CHECK(h.init(), "init: %s", h.err.c_str());
    // CMD12 during CMD18: the byte after the command is the data byte that was next (junk)
    spi.cs(true);
    uint8_t r = h.cmd(18, 0);
    uint8_t blk[512];
    CHECK(r == 0 && h.get_block(blk, 512), "CMD18");
    // block 1 follows at once: one NAC filler, the token, data; CMD12 goes out meanwhile
    uint8_t f[6] = {0x4C, 0, 0, 0, 0, 0};
    f[5] = static_cast<uint8_t>(SdHost::crc7(f, 5) << 1 | 1);
    uint8_t seen[6];
    for (int i = 0; i < 6; i++) seen[i] = spi.xfer(f[i]);
    uint8_t stuff = spi.xfer(0xFF);
    uint8_t r1 = spi.xfer(0xFF);
    CHECK(seen[0] == 0xFF && seen[1] == 0xFE && seen[2] == pattern[0] && seen[5] == pattern[3],
          "data during CMD12: %02x %02x %02x .. %02x", seen[0], seen[1], seen[2], seen[5]);
    CHECK(stuff == pattern[4], "stuff byte %02x, want the next data byte %02x", stuff, pattern[4]);
    CHECK(r1 == 0x00, "R1 after the stuff byte: %02x", r1);
    int busy_bytes = 0;
    while (spi.xfer(0xFF) == 0x00 && busy_bytes < 100000) busy_bytes++;
    CHECK(busy_bytes > 0, "no busy after CMD12");
    spi.cs(false);
    // busy after a write lasts the configured time, and commands sent during it are ignored
    spi.cs(true);
    r = h.cmd(24, 7);
    CHECK(r == 0, "CMD24: %02x", r);
    spi.xfer(0xFF);
    spi.xfer(0xFE);
    for (int i = 0; i < 512; i++) spi.xfer(pattern[i]);
    uint16_t crc = SdHost::crc16(pattern, 512);
    spi.xfer(static_cast<uint8_t>(crc >> 8));
    spi.xfer(static_cast<uint8_t>(crc));
    uint8_t resp = spi.xfer(0xFF);
    CHECK(resp == 0xE5, "data response %02x, want e5", resp);
    uint64_t t0 = spi.ticks;
    uint8_t b = spi.xfer(0xFF);
    CHECK(b == 0x00, "not busy right after the data response: %02x", b);
    r = h.cmd(13, 0, false, false);                  // during busy: ignored
    CHECK(r == 0x00 && card.stats.ignored_busy == 1, "command during busy: r1 %02x, ignored %llu", r,
          (unsigned long long)card.stats.ignored_busy);
    while (spi.xfer(0xFF) == 0x00) {}
    uint64_t busy_ticks = spi.ticks - t0;
    CHECK(busy_ticks >= 3000 && busy_ticks < 3000 + 16 * 4 * 2 + 200, "busy lasted %llu ticks, want about 3000",
          (unsigned long long)busy_ticks);
    spi.cs(false);
    CHECK(card.stats.violations == 1, "violations %llu, want 1 (the command during busy)",
          (unsigned long long)card.stats.violations);
}

static void test_sdsc_addressing(unsigned seed) {
    SdCardOptions o;
    o.kind = SdCardOptions::Kind::SDSC_V2;
    o.seed = seed;
    o.blank_blocks = 8192;
    SdCardModel card("", o);
    uint8_t pattern[512];
    for (int i = 0; i < 512; i++) pattern[i] = static_cast<uint8_t>(i ^ 0x5A);
    card.write_block(3, pattern);
    SoftSpi spi(card, 1);
    SdHost h = spi.host();
    CHECK(h.init() && !h.sdhc, "SDSC init: %s", h.err.c_str());
    uint8_t buf[512];
    CHECK(h.read_block(3, buf) && std::memcmp(buf, pattern, 512) == 0, "SDSC CMD17 at byte address 1536");
    spi.cs(true);
    CHECK(h.cmd(16, 16) == 0x00, "CMD16 16");
    uint8_t r = h.cmd(17, 3 * 512 + 32);
    CHECK(r == 0 && h.get_block(buf, 16) && std::memcmp(buf, pattern + 32, 16) == 0, "16-byte read at offset 32");
    r = h.cmd(17, 3 * 512 + 504);
    CHECK(r == 0x20, "read across a block boundary: r1 %02x, want 20", r);
    r = h.cmd(24, 4 * 512);
    CHECK(r == 0x40, "write with block length 16: r1 %02x, want 40", r);
    CHECK(h.cmd(16, 512) == 0x00, "CMD16 512");
    r = h.cmd(24, 4 * 512 + 4);
    CHECK(r == 0x20, "misaligned write: r1 %02x, want 20", r);
    r = h.cmd(16, 1024);
    CHECK(r == 0x40, "CMD16 1024: r1 %02x, want 40", r);
    spi.cs(false);
    CHECK(h.write_block(5, pattern), "SDSC CMD24: %s", h.err.c_str());
    card.read_block(5, buf);
    CHECK(std::memcmp(buf, pattern, 512) == 0, "SDSC write landed in block 5");
}

static void test_crc_on(unsigned seed) {
    SdCardOptions o;
    o.seed = seed;
    SdCardModel card("", o);
    SoftSpi spi(card, 1);
    SdHost h = spi.host();
    CHECK(h.init(), "init");
    spi.cs(true);
    CHECK(h.cmd(13, 0, true) == 0x00, "bad CRC ignored while CRC checking is off");
    h.x(0xFF);
    CHECK(h.cmd(59, 1) == 0x00, "CMD59 1");
    uint8_t r = h.cmd(13, 0, true);
    CHECK(r == 0x08, "bad CRC with checking on: r1 %02x, want 08", r);
    spi.cs(false);
    uint8_t blk[512];
    for (int i = 0; i < 512; i++) blk[i] = static_cast<uint8_t>(i);
    CHECK(!h.write_block(9, blk, true) && h.last_resp == 0xEB, "bad data CRC: response %02x, want eb", h.last_resp);
    CHECK(h.write_block(9, blk), "good data CRC: %s", h.err.c_str());
    CHECK(card.stats.write_crc_errors == 1, "write CRC errors %llu", (unsigned long long)card.stats.write_crc_errors);
}

static void test_faults(unsigned seed) {
    SdCardOptions o;
    o.seed = seed;
    SdCardModel card("", o);
    SoftSpi spi(card, 1);
    SdHost h = spi.host();
    CHECK(h.init(), "init");
    uint8_t buf[512] = {0}, r2 = 0;
    card.faults.no_response = 1;
    spi.cs(true);
    CHECK(h.cmd(13, 0) == 0xFF, "no_response: got an answer");
    spi.cs(false);
    card.faults.cmd_crc_error = 1;
    spi.cs(true);
    CHECK(h.cmd(13, 0) == 0x08, "cmd_crc_error: r1 %02x", h.last_r1);
    spi.cs(false);
    card.faults.read_error_token = 1;
    card.faults.error_token = 0x01;
    CHECK(!h.read_block(0, buf) && h.last_token == 0x01, "read_error_token: token %02x", h.last_token);
    CHECK(h.status(r2) && (r2 & 0x04), "CMD13 after a read error: %02x", r2);
    card.faults.read_bad_crc = 1;
    CHECK(!h.read_block(0, buf) && h.err == "data CRC16 mismatch", "read_bad_crc: %s", h.err.c_str());
    CHECK(h.read_block(0, buf), "read after the faults");
    card.faults.write_crc_reject = 1;
    CHECK(!h.write_block(1, buf) && h.last_resp == 0xEB, "write_crc_reject: %02x", h.last_resp);
    card.faults.write_error = 1;
    CHECK(!h.write_block(1, buf) && h.last_resp == 0xED, "write_error: %02x", h.last_resp);
    CHECK(h.status(r2) && (r2 & 0x04), "CMD13 after a write error: %02x", r2);
    CHECK(h.write_block(1, buf), "write after the faults: %s", h.err.c_str());
    SdCardOptions o2;
    o2.seed = seed;
    SdCardModel stuck("", o2);
    stuck.faults.init_never_ready = true;
    SoftSpi spi2(stuck, 1);
    SdHost h2 = spi2.host();
    h2.acmd41_tries = 30;
    CHECK(!h2.init() && h2.err == "ACMD41: card never ready", "init_never_ready: %s", h2.err.c_str());
}

static void test_absent_and_removal(unsigned seed) {
    SdCardOptions o;
    o.seed = seed;
    o.present = false;
    SdCardModel none("", o);
    SoftSpi spi(none, 1);
    SdHost h = spi.host();
    CHECK(none.cd_n() && !h.init() && h.err == "CMD0: no idle response", "absent card: %s", h.err.c_str());
    for (int i = 0; i < 100; i++) CHECK(none.tick(false, i & 1, false), "absent card drove MISO low");
    SdCardModel card("", SdCardOptions());
    SoftSpi spi2(card, 1);
    SdHost h2 = spi2.host();
    CHECK(h2.init(), "init before removal");
    card.set_present(false);
    uint8_t buf[512];
    CHECK(!h2.read_block(0, buf), "read from a removed card succeeded");
    card.set_present(true);
    CHECK(!h2.read_block(0, buf), "a reinserted card answered without initialization");
    CHECK(h2.init() && h2.read_block(0, buf), "init after reinsertion: %s", h2.err.c_str());
    SdCardModel bad("/nonexistent/card.img", SdCardOptions());
    CHECK(!bad.error().empty() && bad.cd_n(), "a missing image file must leave the card absent");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("usage: %s <work directory> [seed]\n", argv[0]);
        return 2;
    }
    g_dir = argv[1];
    unsigned seed = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 0)) : 1;
    test_crc();
    test_powerup_and_spi_entry(seed);
    test_init_kinds(seed);
    test_registers(seed);
    test_read_write(seed);
    test_stuff_byte_and_busy(seed);
    test_sdsc_addressing(seed);
    test_crc_on(seed);
    test_faults(seed);
    test_absent_and_removal(seed);
    if (failures) {
        std::printf("test_sdcard_model: FAIL (%d of %d checks failed, seed %u)\n", failures, checks, seed);
        return 1;
    }
    std::printf("test_sdcard_model: PASS (%d checks, seed %u)\n", checks, seed);
    return 0;
}
