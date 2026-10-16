/*
SurfCPU SD card model
--------------------
File: sdcard_model.h Date: 10/15/26 (SurfCPU)
--------------------
A microSD card in SPI mode, at the pins, for the simulations (MODULES.md section 7):
tick(cs_n, sclk, mosi) once per clk cycle returns MISO. The card reacts to the edges of
SCLK it sees (mode 0: it samples MOSI on the rising edge and changes MISO on the falling
edge), so it works at any SCLK the host makes.

What it does, after the SD Physical Layer Simplified Specification's SPI mode:
  * power-up: commands are ignored until 74 clocks were given with CS high;
  * CMD0 (with its CRC, CS low) enters SPI mode; CMD8 (R7, CRC always checked), CMD55 +
    ACMD41 (idle for a few calls, then ready; an SDHC card stays busy if HCS is 0 or CMD8
    was never sent), CMD58 (R3: OCR with the busy and CCS bits), CMD59 (CRC on/off),
    CMD9/CMD10 (CSD v2.0 or v1.0, CID, as data blocks), CMD13 (R2), CMD16, CMD17,
    CMD18 + CMD12 (stuff byte, R1b), CMD24 and CMD25 (data tokens 0xFE / 0xFC / 0xFD,
    data response tokens 0xE5 accepted, 0xEB CRC error, 0xED write error, then busy),
    ACMD23, ACMD51 (SCR); anything else gets "illegal command";
  * responses after NCR filler bytes, read tokens after NAC filler bytes, busy periods in
    clk cycles, all seeded random within the options' limits;
  * SDHC/SDXC block addressing, or SDSC byte addressing with CMD16 block lengths;
  * CRC7 on commands, CRC16 on every data block it sends (and on received blocks when
    CRC checking is on);
  * card absent (MISO pulled up, cd_n high), and one-shot injected errors (SdFaults).

Storage is the image file (raw 512-byte blocks, as tools/mkcard.py makes them), opened read
only; writes stay in memory until flush() is called (or with write_through). An empty
image path makes a blank in-memory card of `blank_blocks` blocks. Nothing here writes a
real device. Protocol mistakes by the host are counted (stats.violations) and logged.
No Verilator dependency: plain C++17.
*/
#ifndef SURFCPU_SDCARD_MODEL_H
#define SURFCPU_SDCARD_MODEL_H

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

struct SdCardOptions {
    enum class Kind { SDHC, SDSC_V2, SDSC_V1 };
    Kind     kind            = Kind::SDHC;  // SDHC: block addressing; SDSC: byte addressing (V1: no CMD8)
    bool     present         = true;        // false: no card in the socket
    unsigned seed            = 1;
    int      ncr_min         = 1;           // filler bytes before a command's response (spec 1..8)
    int      ncr_max         = 2;
    int      nac_min         = 1;           // filler bytes before each read data token
    int      nac_max         = 4;
    int      acmd41_busy     = 3;           // ACMD41 calls answered "idle" before the card is ready
    uint64_t write_busy_min  = 200;         // busy after an accepted block, in tick()s (clk cycles)
    uint64_t write_busy_max  = 1000;
    uint64_t stop_busy       = 100;         // busy after CMD12 and after the stop token
    unsigned powerup_clocks  = 74;          // clocks with CS high needed before the first command
    bool     write_through   = false;       // also write every accepted block to the file at once
    uint64_t blank_blocks    = 65536;       // capacity of a card without an image file (32 MiB)
    bool     verbose         = false;       // print every command on stderr
};

// One-shot error injection: each counter is used up by the next matching event.
struct SdFaults {
    int      no_response      = 0;     // the next N commands get no response at all
    int      cmd_crc_error    = 0;     // the next N commands answer "com CRC error" and do nothing
    int      read_error_token = 0;     // the next N read blocks are a data error token instead
    uint8_t  error_token      = 0x01;  // ... this one: 0x01 error, 0x02 CC, 0x04 ECC, 0x08 out of range
    int      read_bad_crc     = 0;     // the next N read blocks carry a wrong CRC16
    int      write_crc_reject = 0;     // the next N written blocks are answered "CRC error"
    int      write_error      = 0;     // the next N written blocks are answered "write error"
    uint64_t busy_extra       = 0;     // added once to the next busy period (a slow card)
    bool     init_never_ready = false; // ACMD41 never reports ready (initialization timeout)
};

class SdCardModel {
public:
    explicit SdCardModel(const std::string &image_path, const SdCardOptions &opt = SdCardOptions());
    ~SdCardModel();
    SdCardModel(const SdCardModel &) = delete;
    SdCardModel &operator=(const SdCardModel &) = delete;

    // Once per clk cycle with the host's pins; returns MISO (1 when not driven: pulled up).
    bool tick(bool cs_n, bool sclk, bool mosi);

    bool cd_n() const { return !present_; }      // card-detect switch: low = card present
    void set_present(bool present);              // insert or remove (removal loses card state)

    SdFaults faults;

    uint64_t blocks() const { return blocks_; }  // capacity in 512-byte blocks
    bool read_block(uint64_t lba, uint8_t *buf);           // backdoor; sees unflushed writes
    bool write_block(uint64_t lba, const uint8_t *buf);    // backdoor (kept in memory)
    size_t dirty_blocks() const { return dirty_.size(); }
    bool flush();                                          // write the kept blocks to the image file
    const std::string &error() const { return error_; }    // why the image could not be used

    struct Stats {
        uint64_t cmds[64] = {};        // commands executed, by index
        uint64_t acmds[64] = {};       // application commands, by index
        uint64_t blocks_read = 0, blocks_written = 0;
        uint64_t write_crc_errors = 0; // received blocks whose CRC16 was wrong
        uint64_t cmd_crc_errors = 0;   // commands rejected for their CRC7
        uint64_t ignored_powerup = 0;  // commands before the 74 power-up clocks
        uint64_t ignored_busy = 0;     // commands sent while the card was busy
        uint64_t ignored_sdmode = 0;   // commands before CMD0 put the card in SPI mode
        uint64_t violations = 0;       // host protocol mistakes (see log)
        uint64_t sclk_rising = 0;
    } stats;
    std::vector<std::string> log;      // the commands and the protocol mistakes, in order

    bool in_spi_mode() const { return !sd_mode_; }
    bool initialized() const { return initialized_; }
    bool busy_now() const { return now_ < busy_until_ || busy_pending_ != 0; }
    uint64_t now() const { return now_; }

    static uint8_t  crc7(const uint8_t *p, size_t n);   // 7-bit CRC of commands (x^7 + x^3 + 1)
    static uint16_t crc16(const uint8_t *p, size_t n);  // CRC-16-CCITT of data blocks (x^16 + x^12 + x^5 + 1)

private:
    enum class St { TRAN, READ_MULTI, READ_DONE, WAIT_TOKEN, RECV };

    SdCardOptions opt_;
    std::string   path_, error_;
    std::FILE    *file_ = nullptr;
    uint64_t      blocks_ = 0;
    std::map<uint64_t, std::array<uint8_t, 512>> dirty_;   // written blocks not yet in the file
    std::mt19937  rng_;

    bool     present_;
    uint64_t now_ = 0;
    bool     prev_sclk_ = false, prev_cs_n_ = true;
    unsigned powerup_count_ = 0;

    // card state
    bool     sd_mode_ = true;           // until CMD0 enters SPI mode
    bool     initialized_ = false;      // ACMD41 finished (out of the idle state)
    bool     app_cmd_ = false, crc_on_ = false, cmd8_seen_ = false;
    int      acmd41_calls_ = 0;
    uint32_t blocklen_ = 512;
    uint8_t  status2_ = 0;              // R2 second byte: errors since the last CMD13
    St       st_ = St::TRAN;
    bool     multi_write_ = false;
    uint64_t addr_ = 0;                 // byte address of the next block of a transfer

    // input side
    uint64_t cmd_frame_ = 0;
    int      cmd_nbits_ = 0;
    uint8_t  rx_byte_ = 0;
    int      rx_nbits_ = 0;
    std::vector<uint8_t> rx_block_;

    // output side
    std::deque<uint8_t> out_;
    int      out_bit_ = 7;
    bool     miso_ = true;
    uint64_t busy_pending_ = 0;         // busy to start once out_ has drained
    uint64_t busy_until_ = 0;

    void open_image(const std::string &path);
    int  rand_range(int lo, int hi);
    uint64_t rand_range64(uint64_t lo, uint64_t hi);
    void violation(const std::string &what);
    void note(const std::string &what);
    void reset_card();

    void on_select();
    void on_deselect();
    void on_rising(bool mosi);
    void on_falling();
    void on_byte(uint8_t b);
    void on_command(uint64_t frame);

    uint8_t r1() const { return initialized_ ? 0x00 : 0x01; }
    void respond(const std::vector<uint8_t> &bytes);          // NCR fillers, then the bytes
    void queue_data_block(const uint8_t *data, size_t n, bool bad_crc, int nac = -1);  // nac < 0: random
    void queue_read_block();                                   // the block at addr_, or an error token
    void finish_write_block();
    std::vector<uint8_t> csd() const;
    std::vector<uint8_t> cid() const;
    bool check_range(uint64_t byte_addr, uint32_t len, uint8_t &err) const;
};

#endif
