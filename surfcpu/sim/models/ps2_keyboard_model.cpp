/*
SurfCPU PS/2 keyboard model
--------------------
File: ps2_keyboard_model.cpp Date: 10/15/26 (SurfCPU)
--------------------
The keyboard of ps2_keyboard_model.h: a state machine stepped once per clk tick, with a
countdown t_ for the phase it is in. Bit timing of a frame it sends, for a bit period T:
data changes, T/4 later the clock falls, T/2 later it rises, T/4 later the next bit. When
it clocks a host byte in: clock low T/2 (the host changes data), release (the keyboard
samples data on that rising edge), high T/2.
*/
#include "ps2_keyboard_model.h"

#include <cctype>
#include <cstdio>

namespace {

struct KeyDef {
    const char *name;
    uint8_t code;
    bool ext;      // E0 prefix
};

const KeyDef kKeys[] = {
    {"A", 0x1C, false}, {"B", 0x32, false}, {"C", 0x21, false}, {"D", 0x23, false},
    {"E", 0x24, false}, {"F", 0x2B, false}, {"G", 0x34, false}, {"H", 0x33, false},
    {"I", 0x43, false}, {"J", 0x3B, false}, {"K", 0x42, false}, {"L", 0x4B, false},
    {"M", 0x3A, false}, {"N", 0x31, false}, {"O", 0x44, false}, {"P", 0x4D, false},
    {"Q", 0x15, false}, {"R", 0x2D, false}, {"S", 0x1B, false}, {"T", 0x2C, false},
    {"U", 0x3C, false}, {"V", 0x2A, false}, {"W", 0x1D, false}, {"X", 0x22, false},
    {"Y", 0x35, false}, {"Z", 0x1A, false},
    {"1", 0x16, false}, {"2", 0x1E, false}, {"3", 0x26, false}, {"4", 0x25, false},
    {"5", 0x2E, false}, {"6", 0x36, false}, {"7", 0x3D, false}, {"8", 0x3E, false},
    {"9", 0x46, false}, {"0", 0x45, false},
    {"BACKTICK", 0x0E, false}, {"MINUS", 0x4E, false}, {"EQUALS", 0x55, false},
    {"LBRACKET", 0x54, false}, {"RBRACKET", 0x5B, false}, {"BACKSLASH", 0x5D, false},
    {"SEMICOLON", 0x4C, false}, {"QUOTE", 0x52, false}, {"COMMA", 0x41, false},
    {"PERIOD", 0x49, false}, {"SLASH", 0x4A, false},
    {"SPACE", 0x29, false}, {"ENTER", 0x5A, false}, {"BACKSPACE", 0x66, false},
    {"TAB", 0x0D, false}, {"ESC", 0x76, false}, {"CAPS", 0x58, false},
    {"LSHIFT", 0x12, false}, {"RSHIFT", 0x59, false}, {"LCTRL", 0x14, false},
    {"LALT", 0x11, false}, {"RCTRL", 0x14, true}, {"RALT", 0x11, true},
    {"LGUI", 0x1F, true}, {"RGUI", 0x27, true}, {"APPS", 0x2F, true},
    {"F1", 0x05, false}, {"F2", 0x06, false}, {"F3", 0x04, false}, {"F4", 0x0C, false},
    {"F5", 0x03, false}, {"F6", 0x0B, false}, {"F7", 0x83, false}, {"F8", 0x0A, false},
    {"F9", 0x01, false}, {"F10", 0x09, false}, {"F11", 0x78, false}, {"F12", 0x07, false},
    {"UP", 0x75, true}, {"DOWN", 0x72, true}, {"LEFT", 0x6B, true}, {"RIGHT", 0x74, true},
    {"INSERT", 0x70, true}, {"HOME", 0x6C, true}, {"PGUP", 0x7D, true},
    {"DELETE", 0x71, true}, {"END", 0x69, true}, {"PGDN", 0x7A, true},
    {"NUM", 0x77, false}, {"SCROLL", 0x7E, false},
    {"KP_SLASH", 0x4A, true}, {"KP_STAR", 0x7C, false}, {"KP_MINUS", 0x7B, false},
    {"KP_PLUS", 0x79, false}, {"KP_ENTER", 0x5A, true}, {"KP_DOT", 0x71, false},
    {"KP0", 0x70, false}, {"KP1", 0x69, false}, {"KP2", 0x72, false}, {"KP3", 0x7A, false},
    {"KP4", 0x6B, false}, {"KP5", 0x73, false}, {"KP6", 0x74, false}, {"KP7", 0x6C, false},
    {"KP8", 0x75, false}, {"KP9", 0x7D, false},
};

const KeyDef *find_key(const std::string &name) {
    for (const KeyDef &k : kKeys)
        if (name == k.name) return &k;
    return nullptr;
}

// the key and shift state that type a character on a US keyboard
bool char_key(char c, std::string &name, bool &shift) {
    static const char *const unshifted = "`1234567890-=[]\\;',./";
    static const char *const shifted = "~!@#$%^&*()_+{}|:\"<>?";
    static const char *const names[] = {"BACKTICK", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
                                        "MINUS", "EQUALS", "LBRACKET", "RBRACKET", "BACKSLASH",
                                        "SEMICOLON", "QUOTE", "COMMA", "PERIOD", "SLASH"};
    shift = false;
    if (c >= 'a' && c <= 'z') { name = std::string(1, static_cast<char>(c - 32)); return true; }
    if (c >= 'A' && c <= 'Z') { name = std::string(1, c); shift = true; return true; }
    for (int i = 0; unshifted[i]; i++) {
        if (c == unshifted[i]) { name = names[i]; return true; }
        if (c == shifted[i]) { name = names[i]; shift = true; return true; }
    }
    switch (c) {
    case ' ': name = "SPACE"; return true;
    case '\n': name = "ENTER"; return true;
    case '\b': name = "BACKSPACE"; return true;
    case '\t': name = "TAB"; return true;
    case '\x1b': name = "ESC"; return true;
    default: return false;
    }
}

int parity_bit(uint8_t b) {      // odd parity: the bit that makes the count of ones odd
    int ones = 0;
    for (int i = 0; i < 8; i++) ones += (b >> i) & 1;
    return (ones & 1) ? 0 : 1;
}

}  // namespace

Ps2KeyboardModel::Ps2KeyboardModel(uint32_t sys_hz, uint32_t ps2_hz) : sys_hz_(sys_hz) {
    if (ps2_hz == 0) ps2_hz = 12500;
    tpb_ = sys_hz / ps2_hz;
    if (tpb_ < 8) tpb_ = 8;
    quarter_ = tpb_ / 4;
    half_ = tpb_ / 2;
}

bool Ps2KeyboardModel::make_codes(const std::string &name, std::vector<uint8_t> &out) {
    if (name == "PRTSC") {
        out.insert(out.end(), {0xE0, 0x12, 0xE0, 0x7C});
        return true;
    }
    if (name == "PAUSE") {
        out.insert(out.end(), {0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77});
        return true;
    }
    const KeyDef *k = find_key(name);
    if (!k) return false;
    if (k->ext) out.push_back(0xE0);
    out.push_back(k->code);
    return true;
}

bool Ps2KeyboardModel::break_codes(const std::string &name, std::vector<uint8_t> &out) {
    if (name == "PRTSC") {
        out.insert(out.end(), {0xE0, 0xF0, 0x7C, 0xE0, 0xF0, 0x12});
        return true;
    }
    if (name == "PAUSE") return true;          // Pause has no break code
    const KeyDef *k = find_key(name);
    if (!k) return false;
    if (k->ext) out.push_back(0xE0);
    out.push_back(0xF0);
    out.push_back(k->code);
    return true;
}

std::vector<uint8_t> Ps2KeyboardModel::text_codes(const std::string &text) {
    std::vector<uint8_t> out;
    for (char c : text) {
        std::string name;
        bool shift = false;
        if (!char_key(c, name, shift)) continue;
        if (shift) make_codes("LSHIFT", out);
        make_codes(name, out);
        break_codes(name, out);
        if (shift) break_codes("LSHIFT", out);
    }
    return out;
}

void Ps2KeyboardModel::type_text(const std::string &text) { queue_bytes(text_codes(text)); }

bool Ps2KeyboardModel::key_make(const std::string &name) {
    std::vector<uint8_t> b;
    if (!make_codes(name, b)) return false;
    queue_bytes(b);
    return true;
}

bool Ps2KeyboardModel::key_break(const std::string &name) {
    std::vector<uint8_t> b;
    if (!break_codes(name, b)) return false;
    queue_bytes(b);
    return true;
}

bool Ps2KeyboardModel::key_tap(const std::string &name) { return key_make(name) && key_break(name); }

bool Ps2KeyboardModel::key_hold(const std::string &name, int repeats) {
    std::vector<uint8_t> b;
    if (!make_codes(name, b)) return false;
    for (int i = 0; i <= repeats; i++) queue_bytes(b);
    return key_break(name);
}

void Ps2KeyboardModel::queue_bytes(const std::vector<uint8_t> &bytes) {
    for (uint8_t b : bytes) key_q_.push_back(Frame{b, Fault::NONE, 11, false, 0});
}

void Ps2KeyboardModel::queue_pause(uint32_t micro) { key_q_.push_back(Frame{0, Fault::NONE, 0, false, micro}); }

void Ps2KeyboardModel::queue_frame(uint8_t byte, Fault fault, int bits) {
    if (bits < 1 || bits > 11) bits = 11;
    if (fault != Fault::PARTIAL) bits = 11;
    key_q_.push_back(Frame{byte, fault, bits, false, 0});
}

bool Ps2KeyboardModel::idle() const {
    return s_ == S::IDLE && resp_q_.empty() && key_q_.empty() && (bat_at_ == 0);
}

void Ps2KeyboardModel::violation(const std::string &what) {
    stats.violations++;
    log.push_back("VIOLATION @" + std::to_string(now_) + ": " + what);
}

void Ps2KeyboardModel::tick(bool clk_oe, bool data_oe, bool &clk_line, bool &data_line) {
    ++now_;
    // the host's clock pulls: how long each one lasts
    if (clk_oe && !host_clk_) host_clk_since_ = now_;
    if (!clk_oe && host_clk_) {
        last_pull_ = now_ - host_clk_since_;
        stats.host_clock_pulls++;
        if (stats.host_clock_min == 0 || last_pull_ < stats.host_clock_min) stats.host_clock_min = last_pull_;
        if (last_pull_ > stats.host_clock_max) stats.host_clock_max = last_pull_;
    }
    if (clk_oe) inhibited_ = true;
    if (present) step(clk_oe, data_oe);
    host_clk_ = clk_oe;
    host_dat_ = data_oe;
    clk_line = !(clk_oe || drv_clk_);
    data_line = !(data_oe || drv_dat_);
    if (!clk_line || !data_line) busy_at_ = now_;
}

void Ps2KeyboardModel::start_frame() {
    if (!resp_q_.empty()) {
        cur_ = resp_q_.front();
        resp_q_.pop_front();
    } else {
        cur_ = key_q_.front();
        key_q_.pop_front();
    }
    if (cur_.bits == 0) {                            // a pause
        s_ = S::GAP;
        t_ = us(cur_.pause_us);
        return;
    }
    const int p = parity_bit(cur_.byte) ^ (cur_.fault == Fault::PARITY ? 1 : 0);
    const int start = cur_.fault == Fault::START ? 1 : 0;
    const int stop = cur_.fault == Fault::STOP ? 0 : 1;
    bits_ = static_cast<uint16_t>(start | cur_.byte << 1 | p << 9 | stop << 10);
    bit_ = 0;
    glitched_ = false;
    drv_dat_ = !(bits_ & 1);
    s_ = S::TX_SETUP;
    t_ = quarter_;
}

void Ps2KeyboardModel::abort_frame() {
    drv_clk_ = false;
    drv_dat_ = false;
    stats.frames_aborted++;
    log.push_back("frame " + std::to_string(cur_.byte) + " abandoned: host inhibit at bit " + std::to_string(bit_));
    if (cur_.fault != Fault::PARTIAL) {             // send it again later
        if (cur_.response) resp_q_.push_front(cur_);
        else key_q_.push_front(cur_);
    }
    s_ = S::IDLE;
}

void Ps2KeyboardModel::step(bool clk_oe, bool data_oe) {
    if (t_ > 0) --t_;
    switch (s_) {
    case S::IDLE: {
        if (clk_oe) {                                // inhibited
            inhibited_ = true;
            break;
        }
        if (data_oe) {                               // request to send: clock released, data low
            if (!inhibited_) {
                if (!dat_alone_) violation("host pulled data low without inhibiting the clock first");
                dat_alone_ = true;
                break;
            }
            inhibited_ = false;
            if (last_pull_ < us(100))
                violation("request to send after a clock inhibit of only " + std::to_string(last_pull_) + " ticks");
            s_ = S::RX_DELAY;
            t_ = us(rts_delay_us);
            break;
        }
        inhibited_ = false;                          // clock and data released: inhibit over
        dat_alone_ = false;
        if (bat_at_ && now_ >= bat_at_) {
            bat_at_ = 0;
            respond(0xAA);
        }
        const bool have = !resp_q_.empty() || (!key_q_.empty() && scanning);
        if (have && now_ - busy_at_ >= us(50)) start_frame();
        break;
    }
    case S::TX_SETUP:                                // data set, clock high
        if (clk_oe) { abort_frame(); break; }
        if (cur_.fault == Fault::GLITCH && bit_ == 4) {
            // a 0.2 us spike on the clock halfway through the setup time, far below the
            // host's filter; the bit timing is not changed
            if (!glitched_ && t_ == quarter_ / 2) {
                glitched_ = true;
                drv_clk_ = true;
                glitch_t_ = sys_hz_ / 5000000u + 1;
            } else if (glitch_t_ > 0 && --glitch_t_ == 0) {
                drv_clk_ = false;
            }
        }
        if (t_ == 0) {
            drv_clk_ = true;                         // falling edge: the host samples
            s_ = S::TX_LOW;
            t_ = half_;
        }
        break;
    case S::TX_LOW:
        if (t_ == 0) {
            drv_clk_ = false;
            if (clk_oe && bit_ < 10) { abort_frame(); break; }
            s_ = S::TX_HIGH;
            t_ = quarter_;
        }
        break;
    case S::TX_HIGH:
        if (clk_oe && bit_ < 10) { abort_frame(); break; }
        if (t_ == 0) {
            bit_++;
            if (bit_ >= cur_.bits) {
                drv_dat_ = false;
                if (cur_.bits == 11) {
                    stats.frames_sent++;
                    last_sent_ = cur_.byte;
                }
                if (cur_.fault != Fault::NONE) stats.faults_sent++;
                s_ = S::GAP;
                t_ = us(cur_.bits == 11 ? gap_us : partial_gap_us);
                break;
            }
            drv_dat_ = !((bits_ >> bit_) & 1);
            s_ = S::TX_SETUP;
            t_ = quarter_;
        }
        break;
    case S::GAP:
        if (clk_oe || t_ == 0) s_ = S::IDLE;
        break;
    case S::RX_DELAY:
        if (!data_oe || clk_oe) {                    // the host gave up its request
            if (!data_oe) log.push_back("request to send withdrawn");
            s_ = S::IDLE;
            break;
        }
        if (t_ == 0) {
            rx_bit_ = 0;
            rx_val_ = 0;
            drv_clk_ = true;
            s_ = S::RX_LOW;
            t_ = half_;
        }
        break;
    case S::RX_LOW:                                  // the host sets data now
        if (t_ == 0) {
            drv_clk_ = false;                        // rising edge: sample
            if (clk_oe) violation("host pulled the clock while the keyboard was clocking");
            rx_dat_ = data_oe;
            if (!data_oe) rx_val_ |= static_cast<uint16_t>(1u << rx_bit_);
            rx_bit_++;
            s_ = S::RX_HIGH;
            t_ = rx_bit_ == 10 ? quarter_ : half_;   // after the stop bit: the acknowledge a quarter on
        }
        break;
    case S::RX_HIGH:
        if (data_oe != rx_dat_) {
            violation("host changed data while the clock was high (bit " + std::to_string(rx_bit_ - 1) + ")");
            rx_dat_ = data_oe;
        }
        if (t_ == 0) {
            if (rx_bit_ < 10) {
                drv_clk_ = true;
                s_ = S::RX_LOW;
                t_ = half_;
            } else {                                 // stop bit in: acknowledge
                bool ack = true;
                if (no_ack > 0) {
                    no_ack--;
                    ack = false;
                    log.push_back("acknowledge left out (injected)");
                }
                drv_dat_ = ack;
                s_ = S::RX_ACK_PRE;
                t_ = quarter_;
            }
        }
        break;
    case S::RX_ACK_PRE:
        if (t_ == 0) {
            drv_clk_ = true;
            s_ = S::RX_ACK_LOW;
            t_ = half_;
        }
        break;
    case S::RX_ACK_LOW:
        if (t_ == 0) {
            drv_clk_ = false;
            s_ = S::RX_ACK_HIGH;
            t_ = quarter_;
        }
        break;
    case S::RX_ACK_HIGH:
        if (t_ == 0) {
            drv_dat_ = false;
            finish_rx();
            s_ = S::GAP;
            t_ = us(gap_us);
        }
        break;
    }
}

void Ps2KeyboardModel::finish_rx() {
    const uint8_t b = static_cast<uint8_t>(rx_val_ & 0xFF);
    const int p = (rx_val_ >> 8) & 1;
    const int stop = (rx_val_ >> 9) & 1;
    char t[64];
    std::snprintf(t, sizeof t, "host byte %02x parity %d stop %d", b, p, stop);
    log.push_back(t);
    if (!stop) {
        stats.host_stop_errors++;
        violation("host stop bit was 0");
        respond(0xFE);
        return;
    }
    if (p != parity_bit(b)) {
        stats.host_parity_errors++;
        respond(0xFE);                               // resend, please
        return;
    }
    stats.host_frames++;
    host_bytes.push_back(b);
    command(b);
}

void Ps2KeyboardModel::command(uint8_t b) {
    if (!answer_commands) return;
    if (expect_arg_) {
        expect_arg_ = false;
        if (arg_cmd_ == 0xED) leds = b & 7;
        respond(0xFA);
        if (arg_cmd_ == 0xF0 && b == 0) respond(0x02);   // the current scan code set
        return;
    }
    switch (b) {
    case 0xFF:                                       // reset: acknowledge, then the self-test result
        respond(0xFA);
        key_q_.clear();
        scanning = true;
        leds = 0;
        bat_at_ = now_ + us(bat_delay_us);
        break;
    case 0xFE:
        respond(last_sent_);
        break;
    case 0xEE:
        respond(0xEE);
        break;
    case 0xF2:
        respond(0xFA);
        respond(0xAB);
        respond(0x83);
        break;
    case 0xED:
    case 0xF3:
    case 0xF0:
        respond(0xFA);
        expect_arg_ = true;
        arg_cmd_ = b;
        break;
    case 0xF4:
        respond(0xFA);
        scanning = true;
        break;
    case 0xF5:
        respond(0xFA);
        scanning = false;
        break;
    default:
        respond(0xFA);
        break;
    }
}
