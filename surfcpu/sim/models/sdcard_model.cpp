/*
SurfCPU SD card model
--------------------
File: sdcard_model.cpp Date: 10/15/26 (SurfCPU)
--------------------
The SPI-mode SD card of sdcard_model.h. Byte timing: everything the card sends is queued
in out_ and shifted out one bit per falling SCLK edge; a command's response is queued when
its 48th bit arrives (on a rising edge), so the next falling edge, the one that ends the
host's byte, presents the first bit of the first filler byte: responses come out aligned
to the host's bytes as on a real card. Busy is time: MISO stays low for a number of clk
cycles after the queued token has gone out, whether or not the host clocks.
*/
#include "sdcard_model.h"

#include <cinttypes>
#include <cstring>

namespace {

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08" PRIx32, v);
    return b;
}

// set bits msb..lsb (numbered from the last bit of a big-endian array) to value
void set_bits(std::vector<uint8_t> &a, int msb, int lsb, uint64_t value) {
    const int total = static_cast<int>(a.size()) * 8;
    for (int bit = lsb; bit <= msb; bit++) {
        const int pos = total - 1 - bit;               // index from the first (most significant) bit
        const uint8_t mask = static_cast<uint8_t>(0x80 >> (pos % 8));
        if ((value >> (bit - lsb)) & 1) a[pos / 8] |= mask;
        else a[pos / 8] &= static_cast<uint8_t>(~mask);
    }
}

}  // namespace

uint8_t SdCardModel::crc7(const uint8_t *p, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        for (int b = 7; b >= 0; b--) {
            const int in = ((p[i] >> b) & 1) ^ ((crc >> 6) & 1);
            crc = static_cast<uint8_t>((crc << 1) & 0x7F);
            if (in) crc ^= 0x09;
        }
    }
    return crc;
}

uint16_t SdCardModel::crc16(const uint8_t *p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= static_cast<uint16_t>(p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

SdCardModel::SdCardModel(const std::string &image_path, const SdCardOptions &opt)
    : opt_(opt), path_(image_path), rng_(opt.seed), present_(opt.present) {
    if (opt_.ncr_min < 1) opt_.ncr_min = 1;
    if (opt_.ncr_max < opt_.ncr_min) opt_.ncr_max = opt_.ncr_min;
    if (opt_.nac_min < 0) opt_.nac_min = 0;
    if (opt_.nac_max < opt_.nac_min) opt_.nac_max = opt_.nac_min;
    if (opt_.write_busy_max < opt_.write_busy_min) opt_.write_busy_max = opt_.write_busy_min;
    open_image(image_path);
}

SdCardModel::~SdCardModel() {
    if (file_) std::fclose(file_);
}

void SdCardModel::open_image(const std::string &path) {
    if (path.empty()) {
        blocks_ = opt_.blank_blocks;
        return;
    }
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) {
        error_ = "cannot open card image " + path;
        present_ = false;
        return;
    }
    std::fseek(file_, 0, SEEK_END);
    const long long size = std::ftell(file_);
    blocks_ = size > 0 ? static_cast<uint64_t>(size) / 512 : 0;
    if (size % 512) note("image size is not a multiple of 512 bytes: the partial last block is not used");
    if (blocks_ == 0) {
        error_ = "card image " + path + " holds no whole block";
        present_ = false;
    }
}

int SdCardModel::rand_range(int lo, int hi) {
    if (hi <= lo) return lo;
    return std::uniform_int_distribution<int>(lo, hi)(rng_);
}

uint64_t SdCardModel::rand_range64(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return lo;
    return std::uniform_int_distribution<uint64_t>(lo, hi)(rng_);
}

void SdCardModel::violation(const std::string &what) {
    stats.violations++;
    log.push_back("VIOLATION @" + std::to_string(now_) + ": " + what);
    if (opt_.verbose) std::fprintf(stderr, "sdcard: violation: %s\n", what.c_str());
}

void SdCardModel::note(const std::string &what) {
    log.push_back("note @" + std::to_string(now_) + ": " + what);
    if (opt_.verbose) std::fprintf(stderr, "sdcard: %s\n", what.c_str());
}

// ------------------------------------------------------------------ storage

bool SdCardModel::read_block(uint64_t lba, uint8_t *buf) {
    if (lba >= blocks_) return false;
    auto it = dirty_.find(lba);
    if (it != dirty_.end()) {
        std::memcpy(buf, it->second.data(), 512);
        return true;
    }
    if (!file_) {
        std::memset(buf, 0, 512);
        return true;
    }
    if (std::fseek(file_, static_cast<long>(lba * 512), SEEK_SET) != 0) return false;
    return std::fread(buf, 1, 512, file_) == 512;
}

bool SdCardModel::write_block(uint64_t lba, const uint8_t *buf) {
    if (lba >= blocks_) return false;
    auto &blk = dirty_[lba];
    std::memcpy(blk.data(), buf, 512);
    if (opt_.write_through) return flush();
    return true;
}

bool SdCardModel::flush() {
    if (dirty_.empty()) return true;
    if (path_.empty()) {
        error_ = "no image file to flush to";
        return false;
    }
    std::FILE *w = std::fopen(path_.c_str(), "r+b");
    if (!w) {
        error_ = "cannot open " + path_ + " for writing";
        return false;
    }
    bool ok = true;
    for (const auto &kv : dirty_) {
        if (std::fseek(w, static_cast<long>(kv.first * 512), SEEK_SET) != 0 ||
            std::fwrite(kv.second.data(), 1, 512, w) != 512) {
            ok = false;
            break;
        }
    }
    ok = (std::fclose(w) == 0) && ok;
    if (ok) {
        dirty_.clear();
        if (file_) {                                  // see the new contents
            std::fclose(file_);
            file_ = std::fopen(path_.c_str(), "rb");
        }
    } else {
        error_ = "writing " + path_ + " failed";
    }
    return ok;
}

void SdCardModel::set_present(bool present) {
    if (present == present_) return;
    present_ = present;
    // a removed card loses power: it starts over when inserted again
    sd_mode_ = true;
    initialized_ = false;
    powerup_count_ = 0;
    reset_card();
    out_.clear();
    out_bit_ = 7;
    busy_pending_ = 0;
    busy_until_ = 0;
    cmd_nbits_ = 0;
    rx_nbits_ = 0;
    miso_ = true;
    note(present ? "card inserted" : "card removed");
}

void SdCardModel::reset_card() {
    initialized_ = false;
    app_cmd_ = false;
    crc_on_ = false;
    cmd8_seen_ = false;
    acmd41_calls_ = 0;
    blocklen_ = 512;
    status2_ = 0;
    st_ = St::TRAN;
    multi_write_ = false;
    rx_block_.clear();
}

// ------------------------------------------------------------------ pins

bool SdCardModel::tick(bool cs_n, bool sclk, bool mosi) {
    ++now_;
    const bool rising = sclk && !prev_sclk_;
    const bool falling = !sclk && prev_sclk_;
    prev_sclk_ = sclk;
    if (!present_) {
        prev_cs_n_ = cs_n;
        return true;
    }
    if (cs_n != prev_cs_n_) {
        prev_cs_n_ = cs_n;
        if (cs_n) on_deselect();
        else on_select();
    }
    if (rising) stats.sclk_rising++;
    if (cs_n) {
        // the power-up clocks: given with CS high (and MOSI high, the 1s of the spec)
        if (rising && mosi && powerup_count_ < opt_.powerup_clocks) powerup_count_++;
        return true;
    }
    if (rising) on_rising(mosi);
    if (falling) on_falling();
    return miso_;
}

void SdCardModel::on_select() {
    out_bit_ = 7;
    cmd_nbits_ = 0;
    rx_nbits_ = 0;
    miso_ = !(now_ < busy_until_);
}

void SdCardModel::on_deselect() {
    cmd_nbits_ = 0;
    rx_nbits_ = 0;
    if (!out_.empty()) note("CS released with " + std::to_string(out_.size()) + " bytes unsent");
    out_.clear();
    out_bit_ = 7;
    if (busy_pending_) {
        busy_until_ = now_ + busy_pending_;
        busy_pending_ = 0;
    }
    switch (st_) {
    case St::RECV:
        violation("CS released in the middle of a data block");
        break;
    case St::WAIT_TOKEN:
        if (multi_write_) violation("CS released during a multiple-block write without the stop token");
        else note("CS released before the data block of CMD24");
        break;
    case St::READ_MULTI:
    case St::READ_DONE:
        note("CS released during CMD18 without CMD12");
        break;
    case St::TRAN:
        break;
    }
    st_ = St::TRAN;
    rx_block_.clear();
}

void SdCardModel::on_rising(bool mosi) {
    const bool in_write = (st_ == St::WAIT_TOKEN || st_ == St::RECV);
    rx_byte_ = static_cast<uint8_t>((rx_byte_ << 1) | (mosi ? 1 : 0));
    if (++rx_nbits_ == 8) {
        rx_nbits_ = 0;
        if (in_write) on_byte(rx_byte_);
    }
    if (in_write) return;                    // MOSI carries data, not commands
    if (cmd_nbits_ == 0) {                   // hunt for a start bit
        if (!mosi) {
            cmd_frame_ = 0;
            cmd_nbits_ = 1;
        }
        return;
    }
    cmd_frame_ = (cmd_frame_ << 1) | (mosi ? 1u : 0u);
    ++cmd_nbits_;
    if (cmd_nbits_ == 2 && !mosi) {          // the transmission bit must be 1: not a command
        cmd_nbits_ = 0;
        return;
    }
    if (cmd_nbits_ == 48) {
        cmd_nbits_ = 0;
        rx_nbits_ = 0;                       // bytes are counted from the end of the command
        on_command(cmd_frame_);
    }
}

void SdCardModel::on_falling() {
    if (out_.empty() && st_ == St::READ_MULTI) queue_read_block();
    if (!out_.empty()) {
        miso_ = (out_.front() >> out_bit_) & 1;
        if (out_bit_ == 0) {
            out_bit_ = 7;
            out_.pop_front();
        } else {
            --out_bit_;
        }
        return;
    }
    if (busy_pending_) {
        busy_until_ = now_ + busy_pending_;
        busy_pending_ = 0;
    }
    miso_ = !(now_ < busy_until_);
}

// ------------------------------------------------------------------ write data

void SdCardModel::on_byte(uint8_t b) {
    if (st_ == St::RECV) {
        rx_block_.push_back(b);
        if (rx_block_.size() == 514) finish_write_block();
        return;
    }
    // St::WAIT_TOKEN
    if (b == 0xFF) return;
    const uint8_t token = multi_write_ ? 0xFC : 0xFE;
    if (b == token) {
        if (!out_.empty()) violation("data token sent before the response was read");
        if (busy_now()) violation("data token sent while the card was busy");
        st_ = St::RECV;
        rx_block_.clear();
        return;
    }
    if (multi_write_ && b == 0xFD) {        // stop tran token: one byte, then busy
        if (busy_now()) violation("stop token sent while the card was busy");
        out_.push_back(0xFF);
        busy_pending_ = opt_.stop_busy + faults.busy_extra;
        faults.busy_extra = 0;
        st_ = St::TRAN;
        log.push_back("stop token");
        return;
    }
    if ((b & 0xC0) == 0x40) {               // a command instead of data: the write is given up
        note("command while waiting for a data token: write abandoned");
        st_ = St::TRAN;
        cmd_frame_ = b;
        cmd_nbits_ = 8;
        return;
    }
    violation("unexpected byte 0x" + hex32(b).substr(6) + " while waiting for a data token");
}

void SdCardModel::finish_write_block() {
    const uint16_t got = static_cast<uint16_t>(rx_block_[512] << 8 | rx_block_[513]);
    const bool crc_ok = crc16(rx_block_.data(), 512) == got;
    if (!crc_ok) stats.write_crc_errors++;
    uint8_t tok;
    uint64_t busy = 0;
    if (faults.write_crc_reject > 0) {
        faults.write_crc_reject--;
        tok = 0x0B;
        note("write block refused: CRC error (injected)");
    } else if (crc_on_ && !crc_ok) {
        tok = 0x0B;
        note("write block refused: CRC error");
    } else if (faults.write_error > 0) {
        faults.write_error--;
        tok = 0x0D;
        status2_ |= 0x04;
        busy = rand_range64(opt_.write_busy_min, opt_.write_busy_max);
        note("write error (injected)");
    } else if (addr_ + 512 > blocks_ * 512) {
        tok = 0x0D;
        status2_ |= 0x80;
        note("write past the end of the card");
    } else {
        write_block(addr_ / 512, rx_block_.data());
        stats.blocks_written++;
        tok = 0x05;
        busy = rand_range64(opt_.write_busy_min, opt_.write_busy_max);
    }
    busy += faults.busy_extra;
    faults.busy_extra = 0;
    out_.push_back(static_cast<uint8_t>(0xE0 | tok));
    busy_pending_ = busy;
    if (multi_write_) {
        st_ = St::WAIT_TOKEN;
        if (tok == 0x05) addr_ += 512;
    } else {
        st_ = St::TRAN;
    }
    rx_block_.clear();
}

// ------------------------------------------------------------------ commands

void SdCardModel::respond(const std::vector<uint8_t> &bytes) {
    const int ncr = rand_range(opt_.ncr_min, opt_.ncr_max);
    for (int i = 0; i < ncr; i++) out_.push_back(0xFF);
    for (uint8_t b : bytes) out_.push_back(b);
}

void SdCardModel::queue_data_block(const uint8_t *data, size_t n, bool bad_crc, int nac) {
    if (nac < 0) nac = rand_range(opt_.nac_min, opt_.nac_max);
    for (int i = 0; i < nac; i++) out_.push_back(0xFF);
    out_.push_back(0xFE);
    for (size_t i = 0; i < n; i++) out_.push_back(data[i]);
    uint16_t crc = crc16(data, n);
    if (bad_crc) crc ^= 0x0001;
    out_.push_back(static_cast<uint8_t>(crc >> 8));
    out_.push_back(static_cast<uint8_t>(crc));
}

void SdCardModel::queue_read_block() {
    const bool sdhc = opt_.kind == SdCardOptions::Kind::SDHC;
    const uint32_t len = sdhc ? 512 : blocklen_;
    const int nac = rand_range(opt_.nac_min, opt_.nac_max);
    if (faults.read_error_token > 0) {
        faults.read_error_token--;
        for (int i = 0; i < nac; i++) out_.push_back(0xFF);
        out_.push_back(faults.error_token);
        if (faults.error_token & 0x08) status2_ |= 0x80;
        if (faults.error_token & 0x04) status2_ |= 0x10;
        if (faults.error_token & 0x02) status2_ |= 0x08;
        if (faults.error_token & 0x01) status2_ |= 0x04;
        note("read error token (injected)");
        if (st_ == St::READ_MULTI) st_ = St::READ_DONE;
        return;
    }
    if (addr_ + len > blocks_ * 512) {     // CMD18 ran past the end of the card
        for (int i = 0; i < nac; i++) out_.push_back(0xFF);
        out_.push_back(0x08);
        status2_ |= 0x80;
        note("multiple-block read reached the end of the card: out-of-range token");
        st_ = St::READ_DONE;
        return;
    }
    uint8_t blk[512];
    read_block(addr_ / 512, blk);
    const uint32_t off = static_cast<uint32_t>(addr_ % 512);
    bool bad = false;
    if (faults.read_bad_crc > 0) {
        faults.read_bad_crc--;
        bad = true;
        note("read block with a wrong CRC (injected)");
    }
    queue_data_block(blk + off, len, bad, nac);
    stats.blocks_read++;
    addr_ += len;
}

bool SdCardModel::check_range(uint64_t a, uint32_t len, uint8_t &err) const {
    if (a + len > blocks_ * 512) {
        err = 0x40;                          // parameter error: outside the card
        return false;
    }
    if (opt_.kind != SdCardOptions::Kind::SDHC && (a % 512) + len > 512) {
        err = 0x20;                          // address error: crosses a physical block
        return false;
    }
    return true;
}

std::vector<uint8_t> SdCardModel::csd() const {
    std::vector<uint8_t> c(16, 0);
    if (opt_.kind == SdCardOptions::Kind::SDHC) {
        const uint64_t c_size = blocks_ >= 1024 ? blocks_ / 1024 - 1 : 0;   // (C_SIZE+1) x 512 KiB
        set_bits(c, 127, 126, 1);            // CSD version 2.0
        set_bits(c, 119, 112, 0x0E);         // TAAC 1 ms
        set_bits(c, 111, 104, 0x00);         // NSAC
        set_bits(c, 103, 96, 0x32);          // TRAN_SPEED 25 MHz
        set_bits(c, 95, 84, 0x5B5);          // CCC
        set_bits(c, 83, 80, 9);              // READ_BL_LEN 512
        set_bits(c, 69, 48, c_size);
        set_bits(c, 46, 46, 1);              // ERASE_BLK_EN
        set_bits(c, 45, 39, 0x7F);           // SECTOR_SIZE
        set_bits(c, 28, 26, 2);              // R2W_FACTOR
        set_bits(c, 25, 22, 9);              // WRITE_BL_LEN 512
    } else {
        // capacity = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 512: the smallest multiplier that fits
        int mult = 0;
        uint64_t c_size = 0;
        for (mult = 0; mult <= 7; mult++) {
            const uint64_t units = 1ull << (mult + 2);
            c_size = blocks_ / units;
            if (c_size <= 4096) break;
        }
        if (mult > 7) { mult = 7; c_size = 4096; }
        c_size = c_size ? c_size - 1 : 0;
        set_bits(c, 127, 126, 0);            // CSD version 1.0
        set_bits(c, 119, 112, 0x26);         // TAAC
        set_bits(c, 111, 104, 0x00);
        set_bits(c, 103, 96, 0x32);
        set_bits(c, 95, 84, 0x5F5);
        set_bits(c, 83, 80, 9);              // READ_BL_LEN 512
        set_bits(c, 79, 79, 1);              // READ_BL_PARTIAL
        set_bits(c, 73, 62, c_size);
        set_bits(c, 61, 59, 6);              // VDD_R_CURR_MIN
        set_bits(c, 58, 56, 6);              // VDD_R_CURR_MAX
        set_bits(c, 55, 53, 6);              // VDD_W_CURR_MIN
        set_bits(c, 52, 50, 6);              // VDD_W_CURR_MAX
        set_bits(c, 49, 47, static_cast<uint64_t>(mult));
        set_bits(c, 46, 46, 1);
        set_bits(c, 45, 39, 0x7F);
        set_bits(c, 28, 26, 4);
        set_bits(c, 25, 22, 9);
    }
    c[15] = static_cast<uint8_t>(crc7(c.data(), 15) << 1 | 1);
    return c;
}

std::vector<uint8_t> SdCardModel::cid() const {
    std::vector<uint8_t> c = {
        0x5C,                                // MID (made up)
        'S', 'F',                            // OID
        'S', 'U', 'R', 'F', 'C',             // PNM
        0x10,                                // PRV 1.0
        0x0B, 0xAD, 0xF0, 0x0D,              // PSN
        0x01, 0xAA,                          // reserved 4 bits, MDT = 2026-10
        0x00};
    c[15] = static_cast<uint8_t>(crc7(c.data(), 15) << 1 | 1);
    return c;
}

void SdCardModel::on_command(uint64_t frame) {
    uint8_t f[6];
    for (int i = 0; i < 6; i++) f[i] = static_cast<uint8_t>(frame >> (40 - 8 * i));
    const int idx = f[0] & 0x3F;
    const uint32_t arg = static_cast<uint32_t>(f[1]) << 24 | static_cast<uint32_t>(f[2]) << 16 |
                         static_cast<uint32_t>(f[3]) << 8 | f[4];
    const bool crc_ok = ((f[5] >> 1) == crc7(f, 5)) && (f[5] & 1);
    const bool app = app_cmd_ && !sd_mode_;
    const std::string name = std::string(app ? "ACMD" : "CMD") + std::to_string(idx) + " " + hex32(arg);
    if (opt_.verbose) std::fprintf(stderr, "sdcard: %s @%" PRIu64 "\n", name.c_str(), now_);

    if (powerup_count_ < opt_.powerup_clocks) {
        stats.ignored_powerup++;
        note(name + " ignored: only " + std::to_string(powerup_count_) + " power-up clocks so far");
        return;
    }
    if (busy_now()) {
        stats.ignored_busy++;
        violation(name + " sent while the card was busy (ignored)");
        return;
    }
    if (sd_mode_) {                          // only CMD0 with a good CRC enters SPI mode
        if (idx == 0 && crc_ok) {
            sd_mode_ = false;
            reset_card();
            stats.cmds[0]++;
            log.push_back(name);
            out_.clear();
            out_bit_ = 7;
            respond({0x01});
        } else {
            stats.ignored_sdmode++;
            note(name + " ignored: the card is not in SPI mode (CMD0 with a good CRC first)");
        }
        return;
    }
    log.push_back(name);

    if (idx == 12 && !app) {                 // stop transmission: keeps the data byte in flight
        stats.cmds[12]++;
        app_cmd_ = false;
        if (st_ == St::READ_MULTI || st_ == St::READ_DONE) {
            const uint8_t stuff = out_.empty() ? 0xFF : out_.front();
            out_.clear();
            out_bit_ = 7;
            out_.push_back(stuff);           // the stuff byte the host must skip
            const int ncr = rand_range(opt_.ncr_min, opt_.ncr_max);
            for (int i = 1; i < ncr; i++) out_.push_back(0xFF);
            out_.push_back(0x00);
            busy_pending_ = opt_.stop_busy + faults.busy_extra;
            faults.busy_extra = 0;
            st_ = St::TRAN;
        } else {
            out_.clear();
            out_bit_ = 7;
            respond({static_cast<uint8_t>(r1() | 0x04)});
        }
        return;
    }
    if (!out_.empty()) note(name + " arrived with " + std::to_string(out_.size()) + " bytes unsent (dropped)");
    out_.clear();
    out_bit_ = 7;
    if (st_ == St::READ_MULTI || st_ == St::READ_DONE) {
        violation(name + " during a multiple-block read (CMD12 first)");
        st_ = St::TRAN;
    }

    if (faults.no_response > 0) {
        faults.no_response--;
        app_cmd_ = false;
        note(name + ": no response (injected)");
        return;
    }
    if (faults.cmd_crc_error > 0) {
        faults.cmd_crc_error--;
        app_cmd_ = false;
        stats.cmd_crc_errors++;
        respond({static_cast<uint8_t>(r1() | 0x08)});
        return;
    }
    if ((crc_on_ || idx == 8) && !crc_ok) {     // CMD8's CRC is always checked
        app_cmd_ = false;
        stats.cmd_crc_errors++;
        note(name + ": bad CRC7");
        respond({static_cast<uint8_t>(r1() | 0x08)});
        return;
    }
    app_cmd_ = false;
    const bool sdhc = opt_.kind == SdCardOptions::Kind::SDHC;

    if (app) {
        stats.acmds[idx]++;
        switch (idx) {
        case 41: {
            if (initialized_) {
                respond({0x00});
                break;
            }
            acmd41_calls_++;
            const bool hcs = (arg >> 30) & 1;
            const bool can = !faults.init_never_ready && (!sdhc || (cmd8_seen_ && hcs));
            if (can && acmd41_calls_ > opt_.acmd41_busy) {
                initialized_ = true;
                respond({0x00});
            } else {
                respond({0x01});
            }
            break;
        }
        case 23:
            respond({static_cast<uint8_t>(initialized_ ? 0x00 : 0x05)});
            break;
        case 51: {
            if (!initialized_) {
                respond({0x05});
                break;
            }
            const uint8_t scr[8] = {0x02, static_cast<uint8_t>(sdhc ? 0x35 : 0x25), 0x80, 0, 0, 0, 0, 0};
            respond({0x00});
            queue_data_block(scr, 8, false);
            break;
        }
        default:
            respond({static_cast<uint8_t>(r1() | 0x04)});
            break;
        }
        return;
    }

    stats.cmds[idx]++;
    const bool idle_ok = idx == 0 || idx == 8 || idx == 55 || idx == 58 || idx == 59;
    if (!initialized_ && !idle_ok) {
        respond({0x05});                     // idle state: illegal command
        return;
    }
    switch (idx) {
    case 0:
        reset_card();
        respond({0x01});
        break;
    case 8:
        if (opt_.kind == SdCardOptions::Kind::SDSC_V1) {
            respond({static_cast<uint8_t>(r1() | 0x04)});
        } else {
            cmd8_seen_ = true;
            const uint8_t vhs = (arg >> 8) & 0x0F;
            respond({r1(), 0x00, 0x00, static_cast<uint8_t>(vhs == 1 ? 0x01 : 0x00),
                     static_cast<uint8_t>(arg & 0xFF)});
        }
        break;
    case 55:
        app_cmd_ = true;
        respond({r1()});
        break;
    case 58: {
        uint32_t ocr = 0x00FF8000;           // 2.7-3.6 V
        if (initialized_) {
            ocr |= 0x80000000u;              // power-up done
            if (sdhc) ocr |= 0x40000000u;    // CCS: block addressing
        }
        respond({r1(), static_cast<uint8_t>(ocr >> 24), static_cast<uint8_t>(ocr >> 16),
                 static_cast<uint8_t>(ocr >> 8), static_cast<uint8_t>(ocr)});
        break;
    }
    case 59:
        crc_on_ = arg & 1;
        respond({r1()});
        break;
    case 9:
    case 10: {
        const std::vector<uint8_t> reg = idx == 9 ? csd() : cid();
        respond({0x00});
        queue_data_block(reg.data(), reg.size(), false);
        break;
    }
    case 13:
        respond({0x00, status2_});
        status2_ = 0;
        break;
    case 16:
        if (arg == 0 || arg > 512) {
            respond({0x40});
        } else {
            if (!sdhc) blocklen_ = arg;      // SDHC: fixed at 512 for reads and writes
            respond({0x00});
        }
        break;
    case 17:
    case 18: {
        const uint64_t a = sdhc ? static_cast<uint64_t>(arg) * 512 : arg;
        const uint32_t len = sdhc ? 512 : blocklen_;
        uint8_t err = 0;
        if (!check_range(a, len, err)) {
            if (err == 0x40) status2_ |= 0x80;
            respond({err});
            break;
        }
        addr_ = a;
        respond({0x00});
        st_ = idx == 18 ? St::READ_MULTI : St::TRAN;
        queue_read_block();
        break;
    }
    case 24:
    case 25: {
        const uint64_t a = sdhc ? static_cast<uint64_t>(arg) * 512 : arg;
        if (!sdhc && blocklen_ != 512) {
            respond({0x40});                 // partial-block writes are not allowed
            break;
        }
        if (a % 512) {
            respond({0x20});                 // misaligned
            break;
        }
        uint8_t err = 0;
        if (!check_range(a, 512, err)) {
            status2_ |= 0x80;
            respond({err});
            break;
        }
        addr_ = a;
        multi_write_ = idx == 25;
        respond({0x00});
        st_ = St::WAIT_TOKEN;
        rx_block_.clear();
        break;
    }
    default:
        respond({static_cast<uint8_t>(r1() | 0x04)});
        break;
    }
}
