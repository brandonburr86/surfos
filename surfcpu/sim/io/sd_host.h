/*
SurfCPU test SD host
--------------------
File: sd_host.h Date: 10/15/26 (SurfCPU)
--------------------
The SD card protocol as firmware does it (SPI mode), over two callbacks: xfer() exchanges
one byte and cs() asserts or releases chip select. The I/O tests use it twice: with a
software SPI master straight against SdCardModel (test_sdcard_model.cpp), and through
surf_spi's registers on the SurfBus (test_spi.cpp), so the same sequence checks the model
and the controller. CRC7/CRC16 are computed here independently of the model's own.

Every function returns false on failure and leaves the reason in err.
*/
#ifndef SURFCPU_SD_HOST_H
#define SURFCPU_SD_HOST_H

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

class SdHost {
public:
    using Xfer = std::function<uint8_t(uint8_t)>;
    using Cs = std::function<void(bool)>;

    SdHost(Xfer xfer, Cs cs) : xfer_(std::move(xfer)), cs_(std::move(cs)) {}

    // limits, in bytes clocked
    int resp_wait = 10;          // for an R1 (NCR is at most 8)
    int token_wait = 2000;       // for a read data token
    long busy_wait = 2000000;    // for the end of busy
    int acmd41_tries = 50;

    std::string err;
    bool sdhc = false;           // block addressing (CCS)
    bool v2 = false;             // answered CMD8
    uint32_t ocr = 0;
    uint8_t last_r1 = 0xFF, last_token = 0xFF, last_resp = 0xFF;
    uint64_t bytes = 0;          // bytes exchanged
    std::vector<std::string> trace;

    static uint8_t crc7(const uint8_t *p, size_t n) {
        unsigned crc = 0;                              // bit-serial, MSB first
        for (size_t i = 0; i < n; i++)
            for (int b = 7; b >= 0; b--) {
                unsigned top = (crc >> 6) & 1;
                crc = (crc << 1) & 0x7F;
                if (top ^ ((p[i] >> b) & 1)) crc ^= 0x09;
            }
        return static_cast<uint8_t>(crc);
    }
    static uint16_t crc16(const uint8_t *p, size_t n) {
        uint16_t crc = 0;                              // table-free byte-wise form
        for (size_t i = 0; i < n; i++) {
            uint16_t x = static_cast<uint16_t>(((crc >> 8) ^ p[i]) & 0xFF);
            x ^= x >> 4;
            crc = static_cast<uint16_t>((crc << 8) ^ (x << 12) ^ (x << 5) ^ x);
        }
        return crc;
    }

    uint8_t x(uint8_t b) { bytes++; return xfer_(b); }
    void select() { cs_(true); }
    void deselect() { cs_(false); x(0xFF); }       // one more byte so the card lets go of MISO

    bool fail(const std::string &why) { err = why; trace.push_back("FAIL " + why); return false; }

    // Wait until the card does not hold MISO low (busy), as FatFs does before commands.
    bool wait_ready() {
        for (long i = 0; i < busy_wait; i++)
            if (x(0xFF) == 0xFF) return true;
        return fail("card stays busy");
    }

    // Send a command; returns its R1 (0xFF: no response). CMD12 skips the stuff byte.
    uint8_t cmd(int idx, uint32_t arg, bool bad_crc = false, bool ready_first = true) {
        if (ready_first && idx != 0 && !wait_ready()) return 0xFF;
        uint8_t f[6] = {static_cast<uint8_t>(0x40 | idx), static_cast<uint8_t>(arg >> 24),
                        static_cast<uint8_t>(arg >> 16), static_cast<uint8_t>(arg >> 8),
                        static_cast<uint8_t>(arg), 0};
        f[5] = static_cast<uint8_t>(crc7(f, 5) << 1 | 1);
        if (bad_crc) f[5] ^= 0x02;
        for (uint8_t b : f) x(b);
        if (idx == 12) x(0xFF);                        // the stuff byte
        uint8_t r = 0xFF;
        for (int i = 0; i < resp_wait; i++) {
            r = x(0xFF);
            if (!(r & 0x80)) break;
        }
        last_r1 = r;
        char t[64];
        std::snprintf(t, sizeof t, "CMD%d %08x -> %02x", idx, arg, r);
        trace.push_back(t);
        return r;
    }
    uint8_t acmd(int idx, uint32_t arg) {
        uint8_t r = cmd(55, 0);
        if (r & 0xFE) return r;
        return cmd(idx, arg);
    }
    uint32_t get32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v = v << 8 | x(0xFF);
        return v;
    }

    // Power-up clocks, CMD0, CMD8, ACMD41 until ready, CMD58, CMD16 for SDSC.
    bool init(bool send_cmd8 = true, bool hcs = true) {
        deselect();
        for (int i = 0; i < 10; i++) x(0xFF);           // 80 clocks with CS high
        select();
        uint8_t r = 0xFF;
        for (int i = 0; i < 4 && r != 0x01; i++) r = cmd(0, 0);
        if (r != 0x01) { deselect(); return fail("CMD0: no idle response"); }
        v2 = false;
        if (send_cmd8) {
            r = cmd(8, 0x1AA);
            if (r == 0x01) {
                uint32_t r7 = get32();
                if ((r7 & 0xFFF) != 0x1AA) { deselect(); return fail("CMD8: bad echo"); }
                v2 = true;
            } else if (!(r & 0x04)) {
                deselect();
                return fail("CMD8: unexpected R1");
            }
        }
        int tries = 0;
        for (;;) {
            r = acmd(41, (v2 && hcs) ? 0x40000000u : 0);
            if (r == 0x00) break;
            if (r != 0x01) { deselect(); return fail("ACMD41: error R1"); }
            if (++tries >= acmd41_tries) { deselect(); return fail("ACMD41: card never ready"); }
        }
        sdhc = false;
        if (v2) {
            r = cmd(58, 0);
            if (r != 0x00) { deselect(); return fail("CMD58 failed"); }
            ocr = get32();
            if (!(ocr & 0x80000000u)) { deselect(); return fail("CMD58: power-up bit clear"); }
            sdhc = (ocr & 0x40000000u) != 0;
        }
        if (!sdhc && cmd(16, 512) != 0x00) { deselect(); return fail("CMD16 failed"); }
        deselect();
        return true;
    }

    uint32_t addr_of(uint32_t lba) const { return sdhc ? lba : lba * 512u; }

    // A data block after a read command's R1: token, n bytes, CRC16 checked.
    bool get_block(uint8_t *buf, size_t n) {
        uint8_t t = 0xFF;
        for (int i = 0; i < token_wait; i++) {
            t = x(0xFF);
            if (t != 0xFF) break;
        }
        last_token = t;
        if (t != 0xFE) {
            char e[48];
            std::snprintf(e, sizeof e, "data token %02x", t);
            return fail(e);
        }
        for (size_t i = 0; i < n; i++) buf[i] = x(0xFF);
        uint16_t crc = static_cast<uint16_t>(x(0xFF) << 8);
        crc |= x(0xFF);
        if (crc != crc16(buf, n)) return fail("data CRC16 mismatch");
        return true;
    }

    bool read_reg(int idx, uint8_t *reg16) {
        select();
        uint8_t r = cmd(idx, 0);
        bool ok = r == 0x00 && get_block(reg16, 16);
        deselect();
        if (r != 0x00) return fail("CMD9/10 R1");
        if (ok && (reg16[15] >> 1) != crc7(reg16, 15)) return fail("register CRC7 mismatch");
        return ok;
    }

    // Capacity from the CSD, in 512-byte blocks.
    static uint64_t csd_blocks(const uint8_t *c) {
        auto bits = [&](int msb, int lsb) {
            uint64_t v = 0;
            for (int b = msb; b >= lsb; b--) v = v << 1 | ((c[15 - b / 8] >> (b % 8)) & 1);
            return v;
        };
        if (bits(127, 126) == 1) return (bits(69, 48) + 1) * 1024;
        uint64_t c_size = bits(73, 62), mult = bits(49, 47), bl_len = bits(83, 80);
        return ((c_size + 1) << (mult + 2)) * (1ull << bl_len) / 512;
    }

    bool read_block(uint32_t lba, uint8_t *buf) {
        select();
        uint8_t r = cmd(17, addr_of(lba));
        if (r != 0x00) { deselect(); return fail("CMD17 R1 " + hex(r)); }
        bool ok = get_block(buf, 512);
        deselect();
        return ok;
    }

    bool read_multi(uint32_t lba, int n, uint8_t *buf) {
        select();
        uint8_t r = cmd(18, addr_of(lba));
        if (r != 0x00) { deselect(); return fail("CMD18 R1 " + hex(r)); }
        bool ok = true;
        for (int i = 0; i < n && ok; i++) ok = get_block(buf + 512 * i, 512);
        r = cmd(12, 0, false, false);
        if (r != 0x00) { deselect(); return fail("CMD12 R1 " + hex(r)); }
        bool rdy = wait_ready();                          // R1b busy
        deselect();
        return ok && rdy;
    }

    // Data block for a write: token, data, CRC, response token, busy.
    bool put_block(uint8_t token, const uint8_t *buf, bool bad_crc = false) {
        x(0xFF);                                          // Nwr: at least one byte
        x(token);
        for (int i = 0; i < 512; i++) x(buf[i]);
        uint16_t crc = crc16(buf, 512);
        if (bad_crc) crc ^= 0x8000;
        x(static_cast<uint8_t>(crc >> 8));
        x(static_cast<uint8_t>(crc));
        uint8_t resp = x(0xFF);
        last_resp = resp;
        if ((resp & 0x1F) != 0x05) return fail("data response " + hex(resp));
        return wait_ready();
    }

    bool write_block(uint32_t lba, const uint8_t *buf, bool bad_crc = false) {
        select();
        uint8_t r = cmd(24, addr_of(lba));
        if (r != 0x00) { deselect(); return fail("CMD24 R1 " + hex(r)); }
        bool ok = put_block(0xFE, buf, bad_crc);
        if (!ok) wait_ready();
        deselect();
        return ok;
    }

    bool write_multi(uint32_t lba, int n, const uint8_t *buf) {
        select();
        uint8_t r = cmd(25, addr_of(lba));
        if (r != 0x00) { deselect(); return fail("CMD25 R1 " + hex(r)); }
        bool ok = true;
        for (int i = 0; i < n && ok; i++) ok = put_block(0xFC, buf + 512 * i);
        x(0xFD);                                          // stop tran token
        x(0xFF);                                          // Nbr
        bool rdy = wait_ready();
        deselect();
        return ok && rdy;
    }

    bool status(uint8_t &r2) {
        select();
        uint8_t r = cmd(13, 0);
        r2 = x(0xFF);
        deselect();
        if (r & 0x80) return fail("CMD13: no response");
        return true;
    }

    static std::string hex(unsigned v) {
        char b[8];
        std::snprintf(b, sizeof b, "%02x", v & 0xFF);
        return b;
    }

private:
    Xfer xfer_;
    Cs cs_;
};

#endif
