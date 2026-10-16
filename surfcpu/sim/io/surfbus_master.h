/*
SurfCPU test SurfBus master
--------------------
File: surfbus_master.h Date: 10/15/26 (SurfCPU)
--------------------
A bus-functional SurfBus master (SURFBUS.md) for the Verilator I/O testbenches, standing in
for the CPU's data port in front of one slave. Blocking calls (read, write) run the clock
through the posedge callback until the response arrives; between them the bus is idle.
It plays by the master's rules (one outstanding request, fields held until accepted) and
checks the slave's: a response only for an accepted request, never in the acceptance cycle
or earlier, one per request. With back_to_back it sometimes offers the next request in the
cycle the previous response arrives (allowed by rule 6); max_gap adds seeded idle cycles.
After acceptance the request fields are scrambled, so a slave that looked at them late
would be caught. The Top type is any Verilated model with the s_req_* / s_rsp_* ports.
*/
#ifndef SURFCPU_SURFBUS_MASTER_H
#define SURFCPU_SURFBUS_MASTER_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

template <class Top>
class SurfBusMaster {
public:
    struct Rsp {
        uint32_t data = 0;
        bool err = false;
        uint64_t wait = 0;      // cycles the request waited for s_req_ready
        uint64_t latency = 0;   // cycles from acceptance to the response (1 = next cycle)
    };

    SurfBusMaster(Top *top, std::function<void()> posedge, unsigned seed = 1)
        : t_(top), posedge_(std::move(posedge)), rng_(seed) {
        t_->s_req_valid = 0;
    }

    int max_gap = 0;            // random idle cycles before each request: 0..max_gap
    bool back_to_back = true;   // sometimes present the next request in the response cycle
    uint64_t timeout = 5000000; // cycles before a request or response counts as lost

    uint64_t requests = 0, responses = 0, error_responses = 0, max_latency = 0, max_wait = 0;
    std::vector<std::string> problems;   // protocol violations by the slave

    Rsp read(uint32_t addr, uint8_t be = 0xF) { return access(false, addr, 0, be, false); }
    Rsp write(uint32_t addr, uint32_t data, uint8_t be = 0xF) { return access(true, addr, data, be, false); }
    Rsp fetch(uint32_t addr) { return access(false, addr, 0, 0xF, true); }

    uint32_t rd(uint32_t addr) {             // a read that must succeed
        Rsp r = read(addr);
        if (r.err) problem("unexpected error response reading " + hex(addr));
        return r.data;
    }
    void wr(uint32_t addr, uint32_t data) {  // a write that must succeed
        Rsp r = write(addr, data);
        if (r.err) problem("unexpected error response writing " + hex(addr));
    }

    // Let n clk cycles pass with the bus idle.
    void idle(uint64_t n) {
        close_open_cycle();
        for (uint64_t i = 0; i < n; i++) idle_cycle();
    }

    void problem(const std::string &what) {
        if (problems.size() < 50) problems.push_back(what);
    }

private:
    Top *t_;
    std::function<void()> posedge_;
    std::mt19937 rng_;
    bool open_ = false;        // we returned in the cycle a response arrived, before its posedge

    static std::string hex(uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof b, "0x%08x", v);
        return b;
    }

    void scramble() {          // request fields are don't-care while valid is low
        t_->s_req_valid = 0;
        t_->s_req_addr = static_cast<uint32_t>(rng_());
        t_->s_req_write = rng_() & 1;
        t_->s_req_be = rng_() & 0xF;
        t_->s_req_wdata = static_cast<uint32_t>(rng_());
        t_->s_req_fetch = rng_() & 1;
    }

    void idle_cycle() {
        scramble();
        t_->eval();
        if (t_->s_rsp_valid) problem("response with no request outstanding");
        posedge_();
    }

    void close_open_cycle() {
        if (!open_) return;
        open_ = false;
        scramble();
        t_->eval();
        posedge_();
    }

    Rsp access(bool write, uint32_t addr, uint32_t wdata, uint8_t be, bool fetch) {
        bool in_open = open_ && back_to_back && (rng_() & 1);
        if (!in_open) close_open_cycle();
        open_ = false;
        if (!in_open && max_gap > 0) {
            int gap = static_cast<int>(rng_() % static_cast<unsigned>(max_gap + 1));
            for (int i = 0; i < gap; i++) idle_cycle();
        }
        t_->s_req_valid = 1;
        t_->s_req_addr = addr;
        t_->s_req_write = write;
        t_->s_req_be = be;
        t_->s_req_wdata = wdata;
        t_->s_req_fetch = fetch;
        Rsp r;
        for (;;) {
            t_->eval();
            // the previous response may still show in the cycle we started in; never later
            if (t_->s_rsp_valid && !in_open) problem("response before the request was accepted");
            in_open = false;
            const bool accepted = t_->s_req_ready;
            posedge_();
            if (accepted) break;
            if (++r.wait > timeout) {
                std::fprintf(stderr, "surfbus: request to %s never accepted\n", hex(addr).c_str());
                std::exit(3);
            }
        }
        requests++;
        if (r.wait > max_wait) max_wait = r.wait;
        scramble();
        for (;;) {
            ++r.latency;
            t_->eval();
            if (t_->s_rsp_valid) {
                r.data = t_->s_rsp_rdata;
                r.err = t_->s_rsp_err;
                responses++;
                if (r.err) error_responses++;
                if (r.latency > max_latency) max_latency = r.latency;
                open_ = true;
                return r;
            }
            posedge_();
            if (r.latency > timeout) {
                std::fprintf(stderr, "surfbus: no response from %s\n", hex(addr).c_str());
                std::exit(3);
            }
        }
    }
};

#endif
