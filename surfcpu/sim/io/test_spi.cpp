/*
SurfCPU surf_spi testbench
--------------------
File: test_spi.cpp Date: 10/15/26 (SurfCPU)
--------------------
Verilator harness for surf_spi (top module) with a SurfBus master standing in for the CPU
and, on the pins, either a loopback (MISO = MOSI) or SdCardModel. Checks:
  * reset values (DIV for 400 kHz, CS released, SCLK low, MOSI high) and the register rules:
    error responses for unknown offsets, partial words and fetches, with no side effect, also
    with live state (DONE and OVR set, DIV not at its reset value);
  * mode 0 at the pins for every DIV tried: SCLK idles low, MOSI never changes on a rising
    edge, half periods of exactly DIV + 1 cycles, 8 rising edges per byte, CS changing only
    with SCLK low; a byte takes 16 x (DIV + 1) cycles and the waiting DATA read returns it;
  * STATUS (BUSY, CARD from cd_n, DONE and OVR sticky with write-1-to-clear), the interrupt,
    DIV written during a transfer applying to the next one;
  * with the SD card model: the whole initialization (power-up clocks, CMD0, CMD8, ACMD41,
    CMD58) at the reset DIV, then a fast DIV, CSD capacity and CID, single and multiple
    block reads (CMD17, CMD18 + CMD12) against the image file, single and multiple block
    writes (CMD24, CMD25 + stop token) with busy, read back, CMD13; an SDSC card (byte
    addresses) and an SD v1 card; injected errors; a card that is absent; interrupt-driven
    byte exchanges.

    Vsurf_spi --work DIR [--seed N]
*/
#include "Vsurf_spi.h"
#include "verilated.h"

#include "sdcard_model.h"
#include "sd_host.h"
#include "surfbus_master.h"
#include "surfcpu/soc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

static const uint32_t B = SURF_SPI_BASE;

struct Tb {
    Vsurf_spi *top;
    SdCardModel *card = nullptr;   // nullptr: loopback when loopback is set, else MISO high
    bool loopback = false;
    bool cd_n = false;
    uint64_t cycle = 0;

    // pin monitor
    bool p_sclk = false, p_mosi = true, p_cs_n = true;
    uint64_t last_edge = 0;
    int expect_half = 0;           // DIV + 1 of the running transfer (0 = do not check)
    uint64_t rising = 0, mosi_on_rise = 0, cs_with_sclk = 0, bad_half = 0, idle_sclk_high = 0;

    explicit Tb(Vsurf_spi *t) : top(t) {}

    void posedge() {
        top->clk = 1;
        top->eval();
        cycle++;
        // the pins after the edge: the card (or loopback) answers before the next edge
        bool miso = true;
        if (card) miso = card->tick(top->cs_n, top->sclk, top->mosi);
        else if (loopback) miso = top->mosi;
        top->miso = miso;
        top->cd_n = card ? card->cd_n() : cd_n;
        monitor();
        top->clk = 0;
        top->eval();
    }

    void monitor() {
        const bool sclk = top->sclk, mosi = top->mosi, cs_n = top->cs_n;
        if (sclk != p_sclk) {
            // every edge of a byte but its first comes DIV + 1 cycles after the one before
            const bool first_of_byte = sclk && (rising % 8) == 0;
            if (expect_half && !first_of_byte && cycle - last_edge != static_cast<uint64_t>(expect_half)) bad_half++;
            last_edge = cycle;
            if (sclk) {
                rising++;
                if (mosi != p_mosi) mosi_on_rise++;
            }
        }
        if (cs_n != p_cs_n && (sclk || p_sclk)) cs_with_sclk++;
        p_sclk = sclk;
        p_mosi = mosi;
        p_cs_n = cs_n;
    }

    void reset() {
        top->rst = 1;
        for (int i = 0; i < 5; i++) posedge();
        top->rst = 0;
        posedge();
    }
};

using Bus = SurfBusMaster<Vsurf_spi>;

// ------------------------------------------------------------------ register rules

static void test_reset_and_errors(Tb &tb, Bus &bus) {
    CHECK(tb.top->cs_n == 1 && tb.top->sclk == 0 && tb.top->mosi == 1, "pins after reset: cs_n %d sclk %d mosi %d",
          tb.top->cs_n, tb.top->sclk, tb.top->mosi);
    const uint32_t div = bus.rd(B + SURF_SPI_DIV);
    // smallest DIV with SYS_HZ / (2 (DIV + 1)) <= 400 kHz at 25 MHz: 31 (390.625 kHz)
    CHECK(div == 31, "DIV after reset %u, want 31", div);
    CHECK(SURF_SYS_HZ / (2 * (div + 1)) <= 400000 && SURF_SYS_HZ / (2 * div) > 400000, "DIV %u is not the smallest", div);
    CHECK(bus.rd(B + SURF_SPI_CTRL) == 0, "CTRL after reset");
    uint32_t st = bus.rd(B + SURF_SPI_STATUS);
    CHECK(st == SURF_SPI_STATUS_CARD_MASK, "STATUS after reset %08x (card present)", st);
    CHECK(tb.top->irq == 0, "irq after reset");
    // errors: unknown offsets, partial words, fetches; none of them may do anything
    const uint32_t bad_offs[] = {0x010, 0x014, 0x100, 0xFFC, 0x002, 0x001};
    for (uint32_t off : bad_offs) {
        CHECK(bus.read(B + off).err, "read of offset %03x not an error", off);
        CHECK(bus.write(B + off, 0xFF).err, "write of offset %03x not an error", off);
    }
    const uint8_t bes[] = {0x1, 0x3, 0xC, 0x8, 0x0, 0x7};
    for (uint8_t be : bes) {
        CHECK(bus.write(B + SURF_SPI_DATA, 0x55, be).err, "partial write (be %x) not an error", be);
        CHECK(bus.read(B + SURF_SPI_STATUS, be).err, "partial read (be %x) not an error", be);
        CHECK(bus.write(B + SURF_SPI_CTRL, 1, be).err, "partial CTRL write (be %x) not an error", be);
    }
    CHECK(bus.fetch(B + SURF_SPI_DATA).err, "fetch not an error");
    bus.idle(4);
    CHECK(bus.rd(B + SURF_SPI_STATUS) == SURF_SPI_STATUS_CARD_MASK, "an erroneous access had an effect");
    CHECK(bus.rd(B + SURF_SPI_CTRL) == 0 && tb.top->cs_n == 1, "an erroneous CTRL write had an effect");
    CHECK(tb.rising == 0, "SCLK moved without a transfer");
    // card detect, synchronized
    tb.cd_n = true;
    bus.idle(4);
    CHECK((bus.rd(B + SURF_SPI_STATUS) & SURF_SPI_STATUS_CARD_MASK) == 0, "STATUS.CARD with cd_n high");
    tb.cd_n = false;
    bus.idle(4);
    CHECK(bus.rd(B + SURF_SPI_STATUS) & SURF_SPI_STATUS_CARD_MASK, "STATUS.CARD with cd_n low");
    // CTRL.CS drives the pin; read back
    bus.wr(B + SURF_SPI_CTRL, SURF_SPI_CTRL_CS_MASK);
    CHECK(tb.top->cs_n == 0 && bus.rd(B + SURF_SPI_CTRL) == SURF_SPI_CTRL_CS_MASK, "CS assert");
    bus.wr(B + SURF_SPI_CTRL, 0);
    CHECK(tb.top->cs_n == 1, "CS release");
    bus.wr(B + SURF_SPI_DIV, 0xFFFFFF07);
    CHECK(bus.rd(B + SURF_SPI_DIV) == 7, "DIV keeps 8 bits");
}

// ------------------------------------------------------------------ timing in loopback

static void test_loopback(Tb &tb, Bus &bus, unsigned seed) {
    std::mt19937 rng(seed);
    tb.loopback = true;
    const int divs[] = {0, 1, 2, 3, 7, 31, 255};
    uint64_t bytes = 0;
    for (int div : divs) {
        bus.wr(B + SURF_SPI_DIV, static_cast<uint32_t>(div));
        tb.expect_half = div + 1;
        const int n = div >= 31 ? 4 : 24;
        for (int i = 0; i < n; i++) {
            const uint8_t v = static_cast<uint8_t>(rng());
            const uint64_t r0 = tb.rising;
            bus.wr(B + SURF_SPI_DATA, v);
            const uint64_t t0 = tb.cycle;
            Bus::Rsp r = bus.read(B + SURF_SPI_DATA);
            const uint64_t took = tb.cycle - t0;
            CHECK(!r.err && (r.data & 0xFF) == v && (r.data >> 8) == 0, "loopback DIV %d: sent %02x got %08x", div, v, r.data);
            CHECK(tb.rising - r0 == 8, "DIV %d: %llu rising edges in a byte", div, (unsigned long long)(tb.rising - r0));
            // the read waits for the end: 16 (DIV + 1) cycles from the write, give or take the bus
            const uint64_t want = 16ull * (div + 1);
            CHECK(took + 1 >= want && took <= want + 2, "DIV %d: byte took %llu cycles, want %llu", div,
                  (unsigned long long)took, (unsigned long long)want);
            CHECK(r.latency <= want + 1, "DIV %d: DATA read latency %llu > 16 (DIV + 1)", div,
                  (unsigned long long)r.latency);
            CHECK(tb.top->sclk == 0, "SCLK not low after a byte");
            bytes++;
        }
        // the waveform at the pins: idle MOSI high
        bus.idle(3);
        CHECK(tb.top->mosi == 1 && tb.top->sclk == 0, "idle pins: mosi %d sclk %d", tb.top->mosi, tb.top->sclk);
    }
    CHECK(tb.bad_half == 0, "%llu half periods of the wrong length", (unsigned long long)tb.bad_half);
    CHECK(tb.mosi_on_rise == 0, "MOSI changed on %llu rising edges", (unsigned long long)tb.mosi_on_rise);
    tb.expect_half = 0;

    // DONE, OVR, the interrupt
    bus.wr(B + SURF_SPI_DIV, 3);
    bus.wr(B + SURF_SPI_STATUS, 0xF);                  // clear the stickies
    uint32_t st = bus.rd(B + SURF_SPI_STATUS);
    CHECK((st & (SURF_SPI_STATUS_DONE_MASK | SURF_SPI_STATUS_OVR_MASK)) == 0, "stickies not cleared: %08x", st);
    bus.wr(B + SURF_SPI_DATA, 0xA5);
    st = bus.rd(B + SURF_SPI_STATUS);
    CHECK(st & SURF_SPI_STATUS_BUSY_MASK, "BUSY during a transfer: %08x", st);
    bus.wr(B + SURF_SPI_DATA, 0x3C);                   // ignored: sets OVR
    CHECK(bus.rd(B + SURF_SPI_STATUS) & SURF_SPI_STATUS_OVR_MASK, "OVR after a write during a transfer");
    CHECK((bus.rd(B + SURF_SPI_DATA) & 0xFF) == 0xA5, "the ignored write changed the byte");
    st = bus.rd(B + SURF_SPI_STATUS);
    CHECK((st & SURF_SPI_STATUS_DONE_MASK) && !(st & SURF_SPI_STATUS_BUSY_MASK), "DONE after the transfer: %08x", st);
    CHECK(tb.top->irq == 0, "irq without DONE_IE");
    // live state for the error rules: DONE and OVR set, DIV = 2. Refused writes (partial words)
    // and fetches must not clear a sticky bit, change DIV or CTRL, or start a transfer.
    bus.wr(B + SURF_SPI_DIV, 2);
    const uint32_t live = bus.rd(B + SURF_SPI_STATUS);
    CHECK((live & (SURF_SPI_STATUS_DONE_MASK | SURF_SPI_STATUS_OVR_MASK)) ==
              (SURF_SPI_STATUS_DONE_MASK | SURF_SPI_STATUS_OVR_MASK), "DONE and OVR not both set: %08x", live);
    const uint8_t pbes[] = {0x1, 0x3, 0x7, 0xE, 0xC, 0x0};
    for (uint8_t be : pbes) {
        CHECK(bus.write(B + SURF_SPI_STATUS, 0xFFFFFFFF, be).err, "partial STATUS write (be %x) not an error", be);
        CHECK(bus.write(B + SURF_SPI_DIV, 0x55, be).err, "partial DIV write (be %x) not an error", be);
        CHECK(bus.write(B + SURF_SPI_CTRL, SURF_SPI_CTRL_CS_MASK | SURF_SPI_CTRL_DONE_IE_MASK, be).err,
              "partial CTRL write (be %x) not an error", be);
        CHECK(bus.write(B + SURF_SPI_DATA, 0x77, be).err, "partial DATA write (be %x) not an error", be);
    }
    CHECK(bus.fetch(B + SURF_SPI_STATUS).err && bus.fetch(B + SURF_SPI_DIV).err, "fetches not errors");
    bus.idle(2);
    CHECK(bus.rd(B + SURF_SPI_STATUS) == live && bus.rd(B + SURF_SPI_DIV) == 2 && bus.rd(B + SURF_SPI_CTRL) == 0 &&
              tb.top->cs_n == 1 && tb.top->irq == 0,
          "refused writes changed STATUS %08x (was %08x), DIV %u or CTRL %08x", bus.rd(B + SURF_SPI_STATUS), live,
          bus.rd(B + SURF_SPI_DIV), bus.rd(B + SURF_SPI_CTRL));
    CHECK((bus.rd(B + SURF_SPI_DATA) & 0xFF) == 0xA5, "a refused DATA write started a transfer");
    bus.wr(B + SURF_SPI_DIV, 3);
    bus.wr(B + SURF_SPI_CTRL, SURF_SPI_CTRL_DONE_IE_MASK);
    bus.idle(1);
    CHECK(tb.top->irq == 1, "irq with DONE and DONE_IE");
    bus.wr(B + SURF_SPI_STATUS, SURF_SPI_STATUS_DONE_MASK);
    bus.idle(1);
    CHECK(tb.top->irq == 0, "irq after clearing DONE");
    CHECK(bus.rd(B + SURF_SPI_STATUS) & SURF_SPI_STATUS_OVR_MASK, "clearing DONE cleared OVR");
    bus.wr(B + SURF_SPI_STATUS, SURF_SPI_STATUS_OVR_MASK);
    CHECK((bus.rd(B + SURF_SPI_STATUS) & SURF_SPI_STATUS_OVR_MASK) == 0, "OVR not cleared");
    // reading DATA when idle returns the last byte at once
    Bus::Rsp r = bus.read(B + SURF_SPI_DATA);
    CHECK((r.data & 0xFF) == 0xA5 && r.latency == 1, "idle DATA read: %08x after %llu cycles", r.data,
          (unsigned long long)r.latency);
    // a DIV written during a transfer applies to the next one
    bus.wr(B + SURF_SPI_DIV, 9);
    bus.wr(B + SURF_SPI_DATA, 0x11);
    uint64_t t0 = tb.cycle;
    bus.wr(B + SURF_SPI_DIV, 1);
    bus.rd(B + SURF_SPI_DATA);
    CHECK(tb.cycle - t0 + 1 >= 160, "DIV changed mid-transfer shortened it (%llu cycles)",
          (unsigned long long)(tb.cycle - t0));
    bus.wr(B + SURF_SPI_DATA, 0x22);
    t0 = tb.cycle;
    bus.rd(B + SURF_SPI_DATA);
    CHECK(tb.cycle - t0 <= 34, "the next transfer did not use the new DIV (%llu cycles)",
          (unsigned long long)(tb.cycle - t0));
    bus.wr(B + SURF_SPI_CTRL, 0);
    bus.wr(B + SURF_SPI_STATUS, 0xF);
    tb.loopback = false;
    std::printf("loopback: %llu bytes at DIV 0..255, %llu rising edges, max DATA wait %llu cycles\n",
                (unsigned long long)bytes, (unsigned long long)tb.rising, (unsigned long long)bus.max_latency);
}

// ------------------------------------------------------------------ with the SD card model

static std::string g_work;

static std::string make_image(const std::string &name, uint64_t blocks, unsigned seed) {
    std::string path = g_work + "/" + name;
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::printf("cannot create %s\n", path.c_str());
        std::exit(2);
    }
    std::mt19937 rng(seed);
    std::vector<uint8_t> blk(512);
    for (uint64_t lba = 0; lba < blocks; lba++) {
        for (auto &b : blk) b = static_cast<uint8_t>(rng());
        blk[0] = static_cast<uint8_t>(lba);
        blk[1] = static_cast<uint8_t>(lba >> 8);
        std::fwrite(blk.data(), 1, 512, f);
    }
    std::fclose(f);
    return path;
}

static bool file_block(const std::string &path, uint64_t lba, uint8_t *buf) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, static_cast<long>(lba * 512), SEEK_SET);
    const bool ok = std::fread(buf, 1, 512, f) == 512;
    std::fclose(f);
    return ok;
}

struct BusHost {
    Tb &tb;
    Bus &bus;
    bool use_irq = false;
    uint32_t ctrl = 0;
    uint64_t irq_waits = 0;
    BusHost(Tb &t, Bus &b) : tb(t), bus(b) {}
    uint8_t xfer(uint8_t v) {
        bus.wr(B + SURF_SPI_DATA, v);
        if (use_irq) {                                   // wait for the interrupt, then clear DONE
            uint64_t n = 0;
            while (!tb.top->irq && n < 100000) {
                bus.idle(1);
                n++;
            }
            irq_waits += n;
            bus.wr(B + SURF_SPI_STATUS, SURF_SPI_STATUS_DONE_MASK);
        }
        return static_cast<uint8_t>(bus.rd(B + SURF_SPI_DATA));
    }
    void cs(bool a) {
        ctrl = (ctrl & ~SURF_SPI_CTRL_CS_MASK) | (a ? SURF_SPI_CTRL_CS_MASK : 0);
        bus.wr(B + SURF_SPI_CTRL, ctrl);
    }
    SdHost host() {
        return SdHost([this](uint8_t b) { return xfer(b); }, [this](bool a) { cs(a); });
    }
};

static void sd_session(Tb &tb, Bus &bus, SdCardOptions::Kind kind, unsigned seed, int fast_div, bool irq) {
    const bool sdhc = kind == SdCardOptions::Kind::SDHC;
    const char *kname = sdhc ? "SDHC" : kind == SdCardOptions::Kind::SDSC_V2 ? "SDSC v2" : "SDSC v1";
    const uint64_t blocks = sdhc ? 16384 : 8192;
    const std::string img = make_image(std::string("spi-") + (sdhc ? "sdhc" : "sdsc") + "-" + std::to_string(seed) + ".img",
                                       blocks, seed);
    SdCardOptions o;
    o.kind = kind;
    o.seed = seed;
    o.ncr_min = 1;
    o.ncr_max = 1 + static_cast<int>(seed % 8);
    o.nac_min = 0;
    o.nac_max = 2 + static_cast<int>(seed % 30);
    o.acmd41_busy = 1 + static_cast<int>(seed % 5);
    o.write_busy_min = 300;
    o.write_busy_max = 3000;
    SdCardModel card(img, o);
    tb.card = &card;
    bus.wr(B + SURF_SPI_DIV, 31);                         // identification speed
    BusHost bh(tb, bus);
    bh.use_irq = irq;
    if (irq) bh.ctrl = SURF_SPI_CTRL_DONE_IE_MASK;
    bus.wr(B + SURF_SPI_CTRL, bh.ctrl);
    SdHost h = bh.host();
    const uint64_t c0 = tb.cycle;
    const bool ok = h.init();
    CHECK(ok, "%s: initialization failed: %s", kname, h.err.c_str());
    const uint64_t init_cycles = tb.cycle - c0;
    CHECK(h.sdhc == sdhc && h.v2 == (kind != SdCardOptions::Kind::SDSC_V1), "%s: CCS %d v2 %d", kname, h.sdhc, h.v2);
    bus.wr(B + SURF_SPI_DIV, static_cast<uint32_t>(fast_div));
    uint8_t reg[16];
    CHECK(h.read_reg(9, reg) && SdHost::csd_blocks(reg) == blocks, "%s: CSD capacity %llu (%s)", kname,
          (unsigned long long)SdHost::csd_blocks(reg), h.err.c_str());
    CHECK(h.read_reg(10, reg) && std::memcmp(reg + 3, "SURFC", 5) == 0, "%s: CID (%s)", kname, h.err.c_str());
    std::mt19937 rng(seed * 7 + 1);
    uint8_t buf[512 * 8], ref[512];
    const uint32_t lbas[] = {0, 1, static_cast<uint32_t>(blocks - 1), static_cast<uint32_t>(rng() % blocks)};
    int bad = 0;
    for (uint32_t lba : lbas) {
        if (!h.read_block(lba, buf)) {
            CHECK(false, "%s: CMD17 %u: %s", kname, lba, h.err.c_str());
            continue;
        }
        file_block(img, lba, ref);
        if (std::memcmp(buf, ref, 512)) bad++;
    }
    CHECK(bad == 0, "%s: %d single blocks differ from the image", kname, bad);
    const uint32_t start = static_cast<uint32_t>(rng() % (blocks - 8));
    CHECK(h.read_multi(start, 8, buf), "%s: CMD18: %s", kname, h.err.c_str());
    bad = 0;
    for (int i = 0; i < 8; i++) {
        file_block(img, start + i, ref);
        if (std::memcmp(buf + 512 * i, ref, 512)) bad++;
    }
    CHECK(bad == 0, "%s: %d of 8 CMD18 blocks differ", kname, bad);
    // writes: single, then multiple, then read back both ways
    uint8_t w[512 * 4];
    for (auto &b : w) b = static_cast<uint8_t>(rng());
    const uint32_t wl = 64 + rng() % 4000;
    CHECK(h.write_block(wl, w), "%s: CMD24: %s", kname, h.err.c_str());
    CHECK(h.read_block(wl, buf) && std::memcmp(buf, w, 512) == 0, "%s: read back CMD24", kname);
    CHECK(h.write_multi(wl + 8, 4, w), "%s: CMD25: %s", kname, h.err.c_str());
    CHECK(h.read_multi(wl + 8, 4, buf) && std::memcmp(buf, w, 2048) == 0, "%s: read back CMD25", kname);
    for (int i = 0; i < 4; i++) {
        CHECK(h.read_block(wl + 8 + i, buf) && std::memcmp(buf, w + 512 * i, 512) == 0, "%s: CMD17 of CMD25 block %d",
              kname, i);
    }
    uint8_t r2 = 0xFF;
    CHECK(h.status(r2) && r2 == 0, "%s: CMD13 r2 %02x", kname, r2);
    file_block(img, wl, ref);
    CHECK(std::memcmp(ref, w, 512) != 0 && card.dirty_blocks() == 5, "%s: writes must stay in memory until flush",
          kname);
    // injected errors, seen through the controller
    card.faults.write_crc_reject = 1;
    CHECK(!h.write_block(wl, w) && h.last_resp == 0xEB, "%s: CRC-rejected write: response %02x", kname, h.last_resp);
    card.faults.read_error_token = 1;
    card.faults.error_token = 0x08;
    CHECK(!h.read_block(0, buf) && h.last_token == 0x08, "%s: read error token %02x", kname, h.last_token);
    card.faults.no_response = 1;
    tb.top->eval();
    {
        h.select();
        uint8_t r = h.cmd(13, 0);
        h.deselect();
        CHECK(r == 0xFF, "%s: no-response fault answered %02x", kname, r);
    }
    CHECK(h.read_block(1, buf), "%s: read after the faults: %s", kname, h.err.c_str());
    CHECK(card.stats.violations == 0, "%s: %llu protocol violations, first: %s", kname,
          (unsigned long long)card.stats.violations, card.log.empty() ? "" : card.log.back().c_str());
    std::printf("%s seed %u: init %llu cycles at DIV 31, then DIV %d%s: %llu blocks read, %llu written, %llu bytes, "
                "%llu commands, NCR <= %d, NAC <= %d\n",
                kname, seed, (unsigned long long)init_cycles, fast_div, irq ? " (interrupt driven)" : "",
                (unsigned long long)card.stats.blocks_read, (unsigned long long)card.stats.blocks_written,
                (unsigned long long)h.bytes, (unsigned long long)card.log.size(), o.ncr_max, o.nac_max);
    bus.wr(B + SURF_SPI_CTRL, 0);
    bus.wr(B + SURF_SPI_STATUS, 0xF);
    tb.card = nullptr;
}

static void test_absent(Tb &tb, Bus &bus, unsigned seed) {
    SdCardOptions o;
    o.seed = seed;
    o.present = false;
    SdCardModel card("", o);
    tb.card = &card;
    bus.idle(4);
    CHECK((bus.rd(B + SURF_SPI_STATUS) & SURF_SPI_STATUS_CARD_MASK) == 0, "absent card: STATUS.CARD set");
    bus.wr(B + SURF_SPI_DIV, 31);
    BusHost bh(tb, bus);
    SdHost h = bh.host();
    CHECK(!h.init() && h.err == "CMD0: no idle response", "absent card: init gave '%s'", h.err.c_str());
    // inserted later: the controller sees it and the card initializes
    card.set_present(true);
    bus.idle(4);
    CHECK(bus.rd(B + SURF_SPI_STATUS) & SURF_SPI_STATUS_CARD_MASK, "inserted card: STATUS.CARD clear");
    CHECK(h.init(), "inserted card: init %s", h.err.c_str());
    tb.card = nullptr;
    bus.wr(B + SURF_SPI_CTRL, 0);
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    unsigned seed = 1;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--work") && i + 1 < argc) g_work = argv[++i];
    }
    if (g_work.empty()) {
        std::printf("usage: %s --work DIR [--seed N]\n", argv[0]);
        return 2;
    }
    auto top = std::make_unique<Vsurf_spi>();
    Tb tb(top.get());
    top->clk = 0;
    top->miso = 1;
    top->cd_n = 0;
    top->s_req_valid = 0;
    top->eval();
    tb.reset();
    Bus bus(top.get(), [&] { tb.posedge(); }, seed);
    bus.max_gap = static_cast<int>(seed % 4);

    test_reset_and_errors(tb, bus);
    test_loopback(tb, bus, seed);
    const int fast = static_cast<int>(seed % 4);           // DIV 0..3: 12.5 to 3.125 MHz
    sd_session(tb, bus, SdCardOptions::Kind::SDHC, seed, fast, false);
    sd_session(tb, bus, SdCardOptions::Kind::SDSC_V2, seed + 100, (fast + 1) % 4, false);
    sd_session(tb, bus, SdCardOptions::Kind::SDSC_V1, seed + 200, (fast + 2) % 4, false);
    sd_session(tb, bus, SdCardOptions::Kind::SDHC, seed + 300, 0, true);
    test_absent(tb, bus, seed);

    CHECK(tb.cs_with_sclk == 0, "CS changed while SCLK was high %llu times", (unsigned long long)tb.cs_with_sclk);
    CHECK(bus.problems.empty(), "SurfBus protocol: %s", bus.problems.empty() ? "" : bus.problems[0].c_str());
    std::printf("bus: %llu requests, %llu error responses, max wait for ready %llu, max latency %llu cycles\n",
                (unsigned long long)bus.requests, (unsigned long long)bus.error_responses,
                (unsigned long long)bus.max_wait, (unsigned long long)bus.max_latency);
    top->final();
    if (failures) {
        std::printf("test_spi: FAIL (%d of %d checks failed, seed %u, %llu cycles)\n", failures, checks, seed,
                    (unsigned long long)tb.cycle);
        return 1;
    }
    std::printf("test_spi: PASS (%d checks, seed %u, %llu cycles)\n", checks, seed, (unsigned long long)tb.cycle);
    return 0;
}
