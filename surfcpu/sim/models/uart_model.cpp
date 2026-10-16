/*
SurfCPU UART model
--------------------
File: uart_model.cpp Date: 10/15/26 (SurfCPU)
--------------------
See uart_model.h.
*/
#include "uart_model.h"

#include <stdexcept>

UartModel::UartModel(unsigned clks_per_bit) : cpb_(clks_per_bit) {
    if (clks_per_bit < 4) throw std::invalid_argument("UartModel: clks_per_bit must be >= 4");
}

void UartModel::push(bool level, uint64_t cycles) {
    if (cycles == 0) return;
    if (segs_.empty()) seg_left_ = cycles;
    segs_.push_back({level, cycles});
}

void UartModel::send_frame(uint8_t byte, bool stop_bit) {
    push(false, cpb_);                                   // start bit
    for (int b = 0; b < 8; b++) push((byte >> b) & 1, cpb_);
    push(stop_bit, cpb_);
}

void UartModel::send(const std::string &bytes) {
    for (unsigned char c : bytes) send_frame(c, true);
}

void UartModel::send_break(unsigned bit_times) {
    push(false, uint64_t(bit_times) * cpb_);
    push(true, cpb_);
}

void UartModel::send_idle(unsigned bit_times) { push(true, uint64_t(bit_times) * cpb_); }

bool UartModel::rx_pin() const { return segs_.empty() ? true : segs_.front().level; }

std::string UartModel::take_output() {
    std::string s;
    s.swap(out_);
    return s;
}

void UartModel::tick(bool tx_pin) {
    // transmitter: the current segment has been on the line for one more cycle
    if (!segs_.empty() && --seg_left_ == 0) {
        segs_.pop_front();
        if (!segs_.empty()) seg_left_ = segs_.front().cycles;
    }

    // receiver: start on a falling edge, check the start bit half a bit later, then one
    // sample in the middle of every bit
    switch (rstate_) {
    case Rx::Idle:
        if (!tx_pin) {
            rstate_ = Rx::Start;
            rcnt_ = 1;
        }
        break;
    case Rx::Start:
        if (++rcnt_ >= cpb_ / 2) {
            if (tx_pin) {
                rstate_ = Rx::Idle;                      // a glitch
            } else {
                rstate_ = Rx::Data;
                rcnt_ = 0;
                rbit_ = 0;
                rbyte_ = 0;
            }
        }
        break;
    case Rx::Data:
        if (++rcnt_ >= cpb_) {
            rcnt_ = 0;
            rbyte_ |= uint8_t(tx_pin) << rbit_;
            if (++rbit_ == 8) rstate_ = Rx::Stop;
        }
        break;
    case Rx::Stop:
        if (++rcnt_ >= cpb_) {
            if (tx_pin) {
                out_.push_back(char(rbyte_));
                n_rx_++;
                rstate_ = Rx::Idle;
            } else {
                n_frame_++;
                if (rbyte_ == 0) n_break_++;
                rstate_ = Rx::WaitHigh;
            }
        }
        break;
    case Rx::WaitHigh:
        if (tx_pin) rstate_ = Rx::Idle;
        break;
    }
}
