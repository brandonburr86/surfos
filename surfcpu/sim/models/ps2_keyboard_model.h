/*
SurfCPU PS/2 keyboard model
--------------------
File: ps2_keyboard_model.h Date: 10/15/26 (SurfCPU)
--------------------
A PS/2 keyboard at the pins for the simulations (MODULES.md section 7). Both lines are
open drain: tick(clk_oe, data_oe, clk_line, data_line) is called once per clk cycle with
the host's pull-downs and returns the line levels, low when the host or the keyboard
pulls them (the pull-ups make them high otherwise).

Device to host: scan code set 2 frames (start 0, eight data bits LSB first, odd parity,
stop 1) on a clock the keyboard makes at ps2_hz (10 to 16.7 kHz for a real one); the data
line changes a quarter period before each falling edge and is held a quarter period after
the rising one. A frame starts only after the bus was idle for 50 us. If the host pulls
the clock low (inhibits) before the frame's commit point, the keyboard abandons the frame
and sends it again later, as the protocol says; after it, the frame counts as delivered and
is never sent again. The commit point is chosen by `commit`: AFTER_STOP_EDGE (default) once
the stop bit's clock has fallen, the 11th edge; AFTER_PARITY_CLOCK, IBM's rule, once the
parity bit's clock (the 10th) has risen: an inhibit after that ends the frame there, without
the stop bit's clock pulse.

Host to device: when the host has pulled the clock low and then releases it with data
held low (request to send), the keyboard clocks the byte in (eleven pulses: d0..d7,
parity, stop, then its acknowledge with data low), checks parity and stop, and answers:
0xFA for commands (0xED LEDs and 0xF3/0xF0 take an argument byte, also answered 0xFA),
0xFF reset gives 0xFA then 0xAA, 0xF2 gives 0xFA 0xAB 0x83, 0xEE echoes, 0xFE resends,
a bad parity asks for a resend (0xFE). It checks the host: the inhibit must last 100 us,
the host must not pull the clock while the keyboard clocks, and must change data only
while the clock is low; mistakes are counted in stats.violations and logged.

Error injection: queue_frame() with a fault (bad parity, bad start or stop bit, a partial
frame followed by a pause, a sub-microsecond glitch on the clock), no_ack, host_stall_bits
(stop clocking a host byte after N bits), present=false.
No Verilator dependency: plain C++17.
*/
#ifndef SURFCPU_PS2_KEYBOARD_MODEL_H
#define SURFCPU_PS2_KEYBOARD_MODEL_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class Ps2KeyboardModel {
public:
    enum class Fault { NONE, PARITY, START, STOP, PARTIAL, GLITCH };

    explicit Ps2KeyboardModel(uint32_t sys_hz, uint32_t ps2_hz = 12500);

    // Once per clk cycle: the host's pull-downs in, the line levels out.
    void tick(bool clk_oe, bool data_oe, bool &clk_line, bool &data_line);

    // What to send. Names: "A".."Z", "0".."9", "SPACE", "ENTER", "BACKSPACE", "TAB", "ESC",
    // "LSHIFT", "RSHIFT", "LCTRL", "RCTRL", "LALT", "RALT", "LGUI", "RGUI", "APPS", "CAPS",
    // "UP", "DOWN", "LEFT", "RIGHT", "HOME", "END", "PGUP", "PGDN", "INSERT", "DELETE",
    // "F1".."F12", "NUM", "SCROLL", "KP0".."KP9", "KP_ENTER", "KP_SLASH", "KP_STAR",
    // "KP_MINUS", "KP_PLUS", "KP_DOT", "PRTSC", "PAUSE", and the punctuation keys "MINUS",
    // "EQUALS", "LBRACKET", "RBRACKET", "BACKSLASH", "SEMICOLON", "QUOTE", "BACKTICK",
    // "COMMA", "PERIOD", "SLASH". A false return means an unknown name.
    void type_text(const std::string &text);       // US layout; \n Enter, \b Backspace, \t Tab, \x1b Esc
    bool key_make(const std::string &name);
    bool key_break(const std::string &name);
    bool key_tap(const std::string &name);         // make then break
    bool key_hold(const std::string &name, int repeats);   // make, typematic repeats, break
    void queue_bytes(const std::vector<uint8_t> &bytes);
    void queue_frame(uint8_t byte, Fault fault = Fault::NONE, int bits = 11);   // bits < 11: partial
    void queue_pause(uint32_t micro);              // keep the line idle this long (a typing pause)

    static bool make_codes(const std::string &name, std::vector<uint8_t> &out);
    static bool break_codes(const std::string &name, std::vector<uint8_t> &out);
    static std::vector<uint8_t> text_codes(const std::string &text);   // what type_text sends

    bool idle() const;          // nothing queued, nothing being sent or received
    size_t pending() const { return resp_q_.size() + key_q_.size(); }

    // behaviour
    enum class Commit { AFTER_STOP_EDGE, AFTER_PARITY_CLOCK };
    Commit commit = Commit::AFTER_STOP_EDGE;   // when an inhibit stops causing a resend
    bool present = true;            // false: a missing keyboard, never clocks or answers
    int no_ack = 0;                 // leave out the acknowledge of the next N host bytes
    int host_stall_bits = 0;        // the next host byte: stop clocking after N bits (0 = off)
    bool answer_commands = true;
    uint32_t gap_us = 200;          // idle line after each frame it sends
    uint32_t partial_gap_us = 3000; // pause after a partial frame (longer than the host's 2 ms)
    uint32_t rts_delay_us = 100;    // from a request to send to the first clock pulse
    uint32_t bat_delay_us = 2000;   // reset: 0xFA, then 0xAA this much later

    // what it saw
    std::vector<uint8_t> host_bytes;   // host bytes with good parity and stop bit
    uint8_t leds = 0;
    bool scanning = true;              // 0xF5 stops sending key codes, 0xF4 resumes
    struct Stats {
        uint64_t frames_sent = 0, frames_aborted = 0, faults_sent = 0;
        uint64_t frames_cut_short = 0;            // delivered at an inhibit after the commit point
        uint64_t host_frames = 0, host_parity_errors = 0, host_stop_errors = 0;
        uint64_t host_clock_pulls = 0;            // times the host pulled the clock low
        uint64_t host_clock_min = 0, host_clock_max = 0;   // their lengths in ticks
        uint64_t violations = 0;
    } stats;
    std::vector<std::string> log;

    uint32_t ticks_per_bit() const { return tpb_; }
    int sending_bit() const {       // the bit of the frame being sent (0 = start), or -1
        return (s_ == S::TX_SETUP || s_ == S::TX_LOW || s_ == S::TX_HIGH) ? bit_ : -1;
    }
    uint64_t now() const { return now_; }

private:
    enum class S {
        IDLE, TX_SETUP, TX_LOW, TX_HIGH, GAP,
        RX_DELAY, RX_LOW, RX_HIGH, RX_ACK_PRE, RX_ACK_LOW, RX_ACK_HIGH, RX_STALL
    };
    struct Frame {
        uint8_t byte;
        Fault fault;
        int bits;            // 0: a pause of pause_us, not a frame
        bool response;
        uint32_t pause_us;
    };

    uint32_t sys_hz_, tpb_, quarter_, half_;
    uint64_t now_ = 0, t_ = 0;
    S s_ = S::IDLE;
    std::deque<Frame> resp_q_, key_q_;
    Frame cur_{0, Fault::NONE, 11, false, 0};
    uint16_t bits_ = 0;          // the frame's 11 bits, bit 0 first
    int bit_ = 0;
    bool glitched_ = false;
    uint32_t glitch_t_ = 0;
    bool drv_clk_ = false, drv_dat_ = false;   // the keyboard pulls the line low
    uint64_t busy_at_ = 0;       // last tick the bus was not idle
    bool host_clk_ = false, host_dat_ = false;
    bool inhibited_ = false;     // the host pulled the clock low since the bus was last idle
    bool dat_alone_ = false;     // reported: data pulled low without an inhibit
    uint64_t host_clk_since_ = 0, last_pull_ = 0;
    int rx_bit_ = 0;
    uint16_t rx_val_ = 0;
    bool rx_dat_ = false;        // host data level at the last rising edge
    bool expect_arg_ = false;
    uint8_t arg_cmd_ = 0;
    uint8_t last_sent_ = 0xAA;
    uint64_t bat_at_ = 0;

    uint64_t us(uint32_t micro) const { return static_cast<uint64_t>(sys_hz_) * micro / 1000000u; }
    void violation(const std::string &what);
    void respond(uint8_t b) { resp_q_.push_back(Frame{b, Fault::NONE, 11, true, 0}); }
    void start_frame();
    void abort_frame();
    void end_committed();
    void finish_rx();
    void command(uint8_t b);
    void step(bool clk_oe, bool data_oe);
};

#endif
