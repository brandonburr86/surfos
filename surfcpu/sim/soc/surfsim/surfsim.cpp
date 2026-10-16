/*
SurfCPU system simulation
--------------------
File: surfsim.cpp Date: 10/15/26 (SurfCPU)
--------------------
build/sim/surfsim (docs/MODULES.md section 7): the whole surfcpu_soc in Verilator with the
C++ models around it, the UART model on the serial pins and the DDR model on the DDR port.

  surfsim [--rom FILE] [--sd IMAGE] [--uart-in FILE] [--ps2-keys TEXT] [--vga-dir DIR]
          [--trace FILE] [--max-cycles N] [--expect REGEX]
          [--seed N] [--quiet] [--uart-loopback] [--gpio-loopback] [--sw N] [--btn N]
          [--ddr-calib N] [--ddr-fail] [--ddr-latency MIN:MAX] [--ddr-ready PCT]
          [--ddr-stall ADDR] [+plusarg ...]

--rom loads the boot ROM image at time 0 (+bootrom=FILE for the RTL). The UART model sends
the bytes of --uart-in after reset and echoes what the SoC sends on stdout (unless --quiet);
--expect ends the run with exit 0 as soon as that output matches the regular expression
(ECMAScript syntax, tried after every byte received: end the pattern with what must follow,
such as \n, and note that . does not match the \r of CRLF line ends).
--trace writes the core's RVFI retirement trace (docs/MODULES.md section 5). sys_clk runs at
spec's sys_hz and pix_clk at pix_hz, both held in reset for 8 cycles. The DDR model calibrates
after --ddr-calib cycles (default 3000) with random ready and latency from --seed; the
tb_soc plusargs +ddr_calib= +ddr_lat_min= +ddr_lat_max= +ddr_ready= +ddr_seed= +ddr_fail
+ddr_stall= are accepted too, so bus-script argument files work here unchanged. Other
plusargs go to the RTL (+script=FILE for the scripted core stand-in).
--sd, --ps2-keys and --vga-dir are accepted; their models (io track) are wired in at
integration, until then the card is absent and the keyboard and display unconnected.
Exit status: 0 when the output matched, or the run finished cleanly (without --expect, also
when --max-cycles ran out); 1 on $fatal/$stop, a SurfBus protocol violation seen by the DDR
model, or an unmatched --expect at $finish; 2 when --max-cycles ran out before --expect
matched.
*/
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "Vsurfcpu_soc.h"
#include "verilated.h"

#include "ddr_model.h"
#include "surfcpu/soc.h"
#include "uart_model.h"

namespace {

struct Options {
    std::string rom, sd, uart_in, ps2_keys, vga_dir, trace, expect;
    uint64_t max_cycles = 0;            // 0 = no limit
    uint64_t seed = 1;
    bool quiet = false, uart_loopback = false, gpio_loopback = false;
    unsigned sw = 0, btn = 0;
    DdrModelOptions ddr;
    bool ddr_seed_given = false;
    std::vector<uint32_t> ddr_stalls;
    std::vector<std::string> plusargs;
};

[[noreturn]] void usage(const char *why) {
    std::fprintf(stderr, "surfsim: %s\n", why);
    std::fprintf(stderr,
                 "usage: surfsim [--rom FILE] [--sd IMAGE] [--uart-in FILE] [--ps2-keys TEXT]\n"
                 "               [--vga-dir DIR] [--trace FILE] [--max-cycles N] [--expect REGEX]\n"
                 "               [--seed N] [--quiet] [--uart-loopback] [--gpio-loopback]\n"
                 "               [--sw N] [--btn N] [--ddr-calib N] [--ddr-fail]\n"
                 "               [--ddr-latency MIN:MAX] [--ddr-ready PCT] [--ddr-stall ADDR]\n"
                 "               [+plusarg ...]\n");
    std::exit(1);
}

uint64_t number(const std::string &s, const char *what) {
    errno = 0;
    char *end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    if (errno || end == s.c_str() || *end) usage((std::string("bad number for ") + what).c_str());
    return v;
}

int64_t snumber(const std::string &s, const char *what) {
    errno = 0;
    char *end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 0);
    if (errno || end == s.c_str() || *end) usage((std::string("bad number for ") + what).c_str());
    return v;
}

// the DDR model's plusargs, as tb_soc takes them
bool ddr_plusarg(const std::string &a, Options &o) {
    auto val = [&](const char *name, std::string &out) {
        std::string p = std::string("+") + name + "=";
        if (a.compare(0, p.size(), p) != 0) return false;
        out = a.substr(p.size());
        return true;
    };
    std::string v;
    if (val("ddr_calib", v)) o.ddr.calib_cycles = snumber(v, "+ddr_calib");
    else if (val("ddr_lat_min", v)) o.ddr.latency_min = unsigned(number(v, "+ddr_lat_min"));
    else if (val("ddr_lat_max", v)) o.ddr.latency_max = unsigned(number(v, "+ddr_lat_max"));
    else if (val("ddr_ready", v)) o.ddr.ready_percent = unsigned(number(v, "+ddr_ready"));
    else if (val("ddr_seed", v)) { o.ddr.seed = number(v, "+ddr_seed"); o.ddr_seed_given = true; }
    else if (val("ddr_stall", v)) o.ddr_stalls.push_back(uint32_t(number("0x" + v, "+ddr_stall")));
    else if (a == "+ddr_fail") o.ddr.calib_fails = true;
    else return false;
    return true;
}

Options parse(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage((a + " needs a value").c_str());
            return argv[++i];
        };
        if (a == "--rom") o.rom = next();
        else if (a == "--sd") o.sd = next();
        else if (a == "--uart-in") o.uart_in = next();
        else if (a == "--ps2-keys") o.ps2_keys = next();
        else if (a == "--vga-dir") o.vga_dir = next();
        else if (a == "--trace") o.trace = next();
        else if (a == "--max-cycles") o.max_cycles = number(next(), "--max-cycles");
        else if (a == "--expect") o.expect = next();
        else if (a == "--seed") o.seed = number(next(), "--seed");
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--uart-loopback") o.uart_loopback = true;
        else if (a == "--gpio-loopback") o.gpio_loopback = true;
        else if (a == "--sw") o.sw = unsigned(number(next(), "--sw")) & 0xF;
        else if (a == "--btn") o.btn = unsigned(number(next(), "--btn")) & 0xF;
        else if (a == "--ddr-calib") o.ddr.calib_cycles = snumber(next(), "--ddr-calib");
        else if (a == "--ddr-fail") o.ddr.calib_fails = true;
        else if (a == "--ddr-ready") o.ddr.ready_percent = unsigned(number(next(), "--ddr-ready"));
        else if (a == "--ddr-stall") o.ddr_stalls.push_back(uint32_t(number(next(), "--ddr-stall")));
        else if (a == "--ddr-latency") {
            std::string v = next();
            size_t c = v.find(':');
            if (c == std::string::npos) usage("--ddr-latency MIN:MAX");
            o.ddr.latency_min = unsigned(number(v.substr(0, c), "--ddr-latency"));
            o.ddr.latency_max = unsigned(number(v.substr(c + 1), "--ddr-latency"));
        } else if (a == "--help" || a == "-h") usage("help");
        else if (!a.empty() && a[0] == '+') {
            if (!ddr_plusarg(a, o)) o.plusargs.push_back(a);
        } else usage(("unknown option " + a).c_str());
    }
    if (!o.ddr_seed_given) o.ddr.seed = o.seed;
    return o;
}

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "surfsim: cannot read %s\n", path.c_str());
        std::exit(1);
    }
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

}  // namespace

int main(int argc, char **argv) {
    Options opt = parse(argc, argv);

    // the RTL sees the plusargs (and +bootrom= for --rom); the rest is ours
    std::vector<std::string> vargs = {"surfsim"};
    if (!opt.rom.empty()) vargs.push_back("+bootrom=" + opt.rom);
    for (auto &p : opt.plusargs) vargs.push_back(p);
    std::vector<const char *> vargv;
    for (auto &s : vargs) vargv.push_back(s.c_str());

    auto ctx = std::make_unique<VerilatedContext>();
    ctx->fatalOnError(false);                     // $fatal ends the run with exit 1, no abort
    ctx->commandArgs(int(vargv.size()), vargv.data());
    auto top = std::make_unique<Vsurfcpu_soc>(ctx.get(), "TOP");

    const unsigned cpb = (SURF_SYS_HZ + 115200 / 2) / 115200;
    UartModel uart(cpb);
    DdrModel ddr(opt.ddr);
    for (uint32_t a : opt.ddr_stalls) ddr.stall_at(a);

    std::string uart_input;
    if (!opt.uart_in.empty()) uart_input = read_file(opt.uart_in);
    if (!opt.sd.empty())
        std::fprintf(stderr, "surfsim: note: --sd %s: the SD card model is wired in at "
                             "integration; the card is absent\n", opt.sd.c_str());
    if (!opt.ps2_keys.empty())
        std::fprintf(stderr, "surfsim: note: --ps2-keys: the keyboard model is wired in at "
                             "integration\n");
    if (!opt.vga_dir.empty())
        std::fprintf(stderr, "surfsim: note: --vga-dir: the VGA capture model is wired in at "
                             "integration\n");

    std::unique_ptr<std::regex> expect;
    if (!opt.expect.empty()) {
        try {
            expect = std::make_unique<std::regex>(opt.expect);
        } catch (const std::regex_error &) {
            usage("--expect is not a valid regular expression");
        }
    }

    FILE *trace = nullptr;
    if (!opt.trace.empty()) {
        trace = std::fopen(opt.trace.c_str(), "w");
        if (!trace) {
            std::fprintf(stderr, "surfsim: cannot write %s\n", opt.trace.c_str());
            return 1;
        }
        std::fprintf(trace, "# surfcpu-trace v1: order pc insn trap intr rd rd_wdata mem_addr "
                            "rmask wmask mem_rdata mem_wdata pc_next\n");
    }

    // clocks in picoseconds
    const uint64_t clk_half = 500000000000ull / SURF_SYS_HZ;
    const uint64_t pix_half = (500000000000ull + SURF_PIX_HZ / 2) / SURF_PIX_HZ;
    const unsigned RESET_CYCLES = 8;

    top->clk = 0;
    top->pix_clk = 0;
    top->rst = 1;
    top->pix_rst = 1;
    top->uart_rx = 1;
    top->sw = opt.sw;
    top->btn = opt.btn;
    top->sd_miso = 1;
    top->sd_cd_n = 1;                             // no card (no SD model yet)
    top->ps2_clk_i = 1;
    top->ps2_data_i = 1;
    top->ddr_req_ready = 0;
    top->ddr_rsp_valid = 0;
    top->ddr_rsp_rdata = 0;
    top->ddr_rsp_err = 0;
    top->ddr_calib_done = 0;
    top->ddr_calib_fail = 0;
    top->ddr_timeout = 0;
    top->eval();

    uint64_t next_clk = clk_half, next_pix = pix_half;
    uint64_t clk_edges = 0, pix_edges = 0, cycles = 0;
    std::string output;
    bool matched = false, out_of_cycles = false;

    while (!ctx->gotFinish()) {
        const uint64_t t = std::min(next_clk, next_pix);
        const bool do_clk = next_clk == t, do_pix = next_pix == t;
        ctx->time(t);

        const bool clk_rising = do_clk && !top->clk;
        if (clk_rising) {
            // the cycle that ends here, as the models and the trace see it
#ifdef SURFCPU_RVFI
            if (trace && top->rvfi_valid)
                std::fprintf(trace,
                             "%" PRIu64 " %08x %08x %u %u %x %08x %08x %x %x %08x %08x %08x\n",
                             uint64_t(top->rvfi_order), top->rvfi_pc_rdata, top->rvfi_insn,
                             unsigned(top->rvfi_trap), unsigned(top->rvfi_intr),
                             unsigned(top->rvfi_rd_addr), top->rvfi_rd_wdata, top->rvfi_mem_addr,
                             unsigned(top->rvfi_mem_rmask), unsigned(top->rvfi_mem_wmask),
                             top->rvfi_mem_rdata, top->rvfi_mem_wdata, top->rvfi_pc_wdata);
#endif
            if (!top->rst)
                ddr.tick(top->ddr_req_valid, top->ddr_req_addr, top->ddr_req_write,
                         top->ddr_req_be, top->ddr_req_wdata, top->ddr_req_fetch);
            uart.tick(top->uart_tx);
        }
        if (do_clk) top->clk = !top->clk;
        if (do_pix) top->pix_clk = !top->pix_clk;
        top->eval();

        if (clk_rising) {
            clk_edges++;
            if (clk_edges == RESET_CYCLES) {
                top->rst = 0;
                uart.send(uart_input);
            }
            if (!top->rst) cycles++;
            // the models' outputs for the next cycle
            top->ddr_req_ready = ddr.req_ready();
            top->ddr_rsp_valid = ddr.rsp_valid();
            top->ddr_rsp_rdata = ddr.rsp_rdata();
            top->ddr_rsp_err = ddr.rsp_err();
            top->ddr_calib_done = ddr.calib_done();
            top->ddr_calib_fail = ddr.calib_fail();
            top->ddr_timeout = ddr.timeout();
            top->uart_rx = opt.uart_loopback ? top->uart_tx : uart.rx_pin();
            if (opt.gpio_loopback) {
                top->sw = top->led;
                top->btn = top->led;
            }
            top->ps2_clk_i = !top->ps2_clk_oe;     // open drain, pulled up, no keyboard yet
            top->ps2_data_i = !top->ps2_data_oe;
            top->eval();

            std::string got = uart.take_output();
            if (!got.empty()) {
                if (!opt.quiet) {
                    std::fwrite(got.data(), 1, got.size(), stdout);
                    std::fflush(stdout);
                }
                output += got;
                if (expect && std::regex_search(output, *expect)) {
                    matched = true;
                    break;
                }
            }
            if (opt.max_cycles && cycles >= opt.max_cycles) {
                out_of_cycles = true;
                break;
            }
        }
        if (do_pix && top->pix_clk) {
            pix_edges++;
            if (pix_edges == RESET_CYCLES) top->pix_rst = 0;
            // (integration) the VGA capture model ticks here
        }
        if (do_clk) next_clk += clk_half;
        if (do_pix) next_pix += pix_half;
    }

    top->final();
    if (trace) std::fclose(trace);

    int status = 0;
    const char *why = "finished";
    if (ctx->gotError()) {
        status = 1;
        why = "stopped by $fatal/$stop";
    } else if (matched) {
        why = "output matched";
    } else if (out_of_cycles) {
        status = expect ? 2 : 0;
        why = expect ? "max-cycles reached before the output matched" : "max-cycles reached";
    } else if (expect) {
        status = 1;
        why = "finished without the expected output";
    }
    if (ddr.early_requests || ddr.protocol_errors) {
        std::fprintf(stderr, "surfsim: ERROR the SoC sent %" PRIu64 " DDR requests before "
                             "calibration and broke the handshake %" PRIu64 " times\n",
                     ddr.early_requests, ddr.protocol_errors);
        status = 1;
    }
    std::fprintf(stderr,
                 "surfsim: %s after %" PRIu64 " cycles; UART %zu bytes in, %zu out, %" PRIu64
                 " framing errors; DDR %s, %" PRIu64 " reads %" PRIu64 " writes %" PRIu64
                 " fetches %" PRIu64 " errors; exit %d\n",
                 why, cycles, uart_input.size(), output.size(), uart.framing_errors(),
                 ddr.calib_done() ? "calibrated" : (ddr.calib_fail() ? "failed" : "not calibrated"),
                 ddr.reads, ddr.writes, ddr.fetches, ddr.error_responses, status);
    return status;
}
