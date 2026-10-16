/*
SurfCPU UART model
--------------------
File: uart_model.h Date: 10/15/26 (SurfCPU)
--------------------
The far end of the SoC's serial port in simulation (docs/MODULES.md section 7): an 8N1
transmitter driving the SoC's rx pin and a receiver decoding its tx pin, both at
clks_per_bit sys_clk cycles per bit (round(sys_hz / 115200) = 217 for the reset divisor).
Plain C++17, no Verilator inside: call tick() once per clk cycle with the tx level of that
cycle, and drive the SoC's rx with rx_pin(). The receiver samples each bit in its middle,
reports a stop bit of 0 as a framing error (a break when the data were all zero too) and
then waits for the line to return to 1. Test helpers send frames with a bad stop bit, breaks
and idle gaps.
*/
#ifndef SURFCPU_UART_MODEL_H
#define SURFCPU_UART_MODEL_H

#include <cstdint>
#include <deque>
#include <string>

class UartModel {
public:
    explicit UartModel(unsigned clks_per_bit);

    // One clk cycle: tx_pin is the SoC's tx level in this cycle (the model's receive line).
    void tick(bool tx_pin);
    // The level the model drives on the SoC's rx pin in the current cycle (1 when idle).
    bool rx_pin() const;

    // Queue bytes to send to the SoC, 8N1, one frame after another.
    void send(const std::string &bytes);
    // Everything received from the SoC since the last call.
    std::string take_output();

    // ---- helpers for tests
    void send_frame(uint8_t byte, bool stop_bit);    // a frame with a chosen stop bit
    void send_break(unsigned bit_times);             // the line low for this many bit times
    void send_idle(unsigned bit_times);              // the line high (a gap) for this long
    bool sending() const { return !segs_.empty(); }
    unsigned clks_per_bit() const { return cpb_; }
    uint64_t bytes_received() const { return n_rx_; }
    uint64_t framing_errors() const { return n_frame_; }
    uint64_t breaks() const { return n_break_; }

private:
    struct Segment {
        bool level;
        uint64_t cycles;
    };
    void push(bool level, uint64_t cycles);

    unsigned cpb_;
    // transmitter: a queue of line levels with their durations
    std::deque<Segment> segs_;
    uint64_t seg_left_ = 0;
    // receiver
    enum class Rx { Idle, Start, Data, Stop, WaitHigh };
    Rx rstate_ = Rx::Idle;
    unsigned rcnt_ = 0;
    unsigned rbit_ = 0;
    uint8_t rbyte_ = 0;
    std::string out_;
    uint64_t n_rx_ = 0, n_frame_ = 0, n_break_ = 0;
};

#endif
