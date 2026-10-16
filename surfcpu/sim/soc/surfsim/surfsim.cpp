/*
SurfCPU system simulation
--------------------
File: surfsim.cpp Date: 10/15/26 (SurfCPU)
--------------------
build/sim/surfsim (docs/MODULES.md section 7): the whole surfcpu_soc in Verilator with the C++
models at its pins: the UART model on the serial port, the DDR model on the DDR port, the SD
card model on surf_spi's pins, the PS/2 keyboard model on the open-drain PS/2 lines and the
VGA capture on the video outputs.

  surfsim [--rom FILE] [--sd IMAGE] [--uart-in FILE] [--ps2-keys TEXT] [--vga-dir DIR]
          [--trace FILE] [--max-cycles N] [--expect REGEX]...
          [--send REGEX=TEXT]... [--ps2-send REGEX=TEXT]... [--fail-on REGEX]...
          [--seed N] [--quiet] [--transcript FILE] [--progress N] [--settle N]
          [--uart-loopback] [--gpio-loopback] [--sw N] [--btn N]
          [--sd-flush] [--vga-font FILE] [--vga-frames N] [--vga-after REGEX]
          [--ddr-calib N] [--ddr-fail] [--ddr-latency MIN:MAX] [--ddr-ready PCT]
          [--ddr-stall ADDR] [+plusarg ...]

Clocks: clk at the spec's sys_hz and pix_clk at its pix_hz (SURF_SYS_HZ, SURF_PIX_HZ from the
generated soc.h), scheduled in femtoseconds on two independent clocks, both held in reset for
8 of their cycles. --rom loads the boot ROM image at time 0 (+bootrom=FILE for the RTL).

UART: the SoC's output is echoed on stdout (unless --quiet) and kept in --transcript FILE; the
bytes of --uart-in are sent after reset. --expect REGEX ends the run with exit 0 once its
ECMAScript regular expression has matched the output; several --expect must match in order,
each after the previous one's match; --settle N runs N more cycles after the last one (to let
the screen catch up, say). --send REGEX=TEXT sends TEXT to the SoC's UART once REGEX
has matched the output; --ps2-send REGEX=TEXT types TEXT on the keyboard instead. Sends of
each kind go in order, each one's REGEX searched after the previous one's match, so a test can
answer a prompt with a command and the next prompt with the next. TEXT takes \r \n \t \e \b
\\ and \xHH. --fail-on REGEX ends the run with exit 1 when it matches. The patterns are tried
after every byte received, over at most the last 16 KiB (--fail-on: 4 KiB): end a pattern
with what must follow it, such as \n; . does not match the \r of CRLF line ends. Each match
and send is logged on stderr with its cycle.

SD card (--sd IMAGE, raw 512-byte blocks as tools/mkcard.py makes them): the io track's card
model on sd_sclk/sd_mosi/sd_cs_n/sd_miso with its card-detect switch on sd_cd_n; without --sd
the socket is empty. The image is opened read only; writes stay in memory unless --sd-flush
writes them back at the end.

PS/2 keyboard: the io track's model on the open-drain lines (a line is low when the SoC's
_oe or the keyboard pulls it, high otherwise); --ps2-keys TEXT is typed after reset.

VGA (--vga-dir DIR): the io track's capture model on vga_r/g/b/hs/vs, ticked every pix_clk
(pix_clk always runs; the capture adds about 1% to the run time). At the end it writes DIR/screen.ppm (the last whole
frame), DIR/screen.txt (the measured timing and the 80x30 text decoded from the pixels with
the font of --vga-font, default rtl/peripherals/surf_video_font.v) and, with --vga-frames N,
every Nth frame as DIR/frame_NNNNNN.ppm. --vga-after REGEX starts the capture only once REGEX
has matched the UART output.

DDR: the C++ DDR model, calibrated after --ddr-calib cycles (default 3000) with random ready
and latency from --seed; tb_soc's plusargs +ddr_calib= +ddr_lat_min= +ddr_lat_max= +ddr_ready=
+ddr_seed= +ddr_fail +ddr_stall= work too. Other plusargs go to the RTL (+script=FILE for the
scripted core stand-in). --trace writes the core's RVFI trace (MODULES.md section 5).

Exit status: 0 when every --expect matched, or the run finished cleanly (without --expect, also
when --max-cycles ran out); 1 on $fatal/$stop, a --fail-on match, a SurfBus protocol violation
seen by the DDR model, an SD or PS/2 protocol violation seen by the card or keyboard model (the
end of its log is printed), or --expect unmatched at $finish; 2 when --max-cycles ran out before the
--expect patterns matched. A summary (cycles, wall time, the models' counters) goes to stderr.
*/
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
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
#include "ps2_keyboard_model.h"
#include "sdcard_model.h"
#include "surfcpu/soc.h"
#include "uart_model.h"
#include "vga_capture.h"

namespace {

struct Trigger {
    std::string pattern, text;
    std::regex re;
};

struct Options {
    std::string rom, sd, uart_in, ps2_keys, vga_dir, trace, transcript;
    std::string vga_font = "rtl/peripherals/surf_video_font.v", vga_after;
    std::vector<Trigger> expects, sends, ps2_sends, fail_ons;
    uint64_t max_cycles = 0;            // 0 = no limit
    uint64_t seed = 1;
    uint64_t progress = 0;              // print a progress line every N cycles (0 = never)
    uint64_t settle = 0;                // cycles run on after the last --expect matched
    unsigned vga_frames = 0;            // also write every Nth frame
    bool quiet = false, uart_loopback = false, gpio_loopback = false, sd_flush = false;
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
                 "               [--vga-dir DIR] [--trace FILE] [--max-cycles N] [--expect REGEX]...\n"
                 "               [--send REGEX=TEXT]... [--ps2-send REGEX=TEXT]... [--fail-on REGEX]...\n"
                 "               [--seed N] [--quiet] [--transcript FILE] [--progress N] [--settle N]\n"
                 "               [--uart-loopback] [--gpio-loopback] [--sw N] [--btn N] [--sd-flush]\n"
                 "               [--vga-font FILE] [--vga-frames N] [--vga-after REGEX]\n"
                 "               [--ddr-calib N] [--ddr-fail] [--ddr-latency MIN:MAX] [--ddr-ready PCT]\n"
                 "               [--ddr-stall ADDR] [+plusarg ...]\n");
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

// C-style escapes in typed text: \r \n \t \e \b \\ \xHH
std::string unescape(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        char c = s[++i];
        switch (c) {
        case 'r': out += '\r'; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'e': out += '\x1b'; break;
        case 'b': out += '\b'; break;
        case '\\': out += '\\'; break;
        case 'x':
            if (i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
                std::isxdigit((unsigned char)s[i + 2])) {
                out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
                break;
            }
            [[fallthrough]];
        default: out += '\\'; out += c; break;
        }
    }
    return out;
}

std::regex compile(const std::string &pattern, const char *what) {
    try {
        return std::regex(pattern);
    } catch (const std::regex_error &) {
        usage((std::string(what) + " is not a valid regular expression: " + pattern).c_str());
    }
}

Trigger trigger(const std::string &arg, bool with_text, const char *what) {
    Trigger t;
    if (with_text) {
        // the first '=' ends the pattern, as in surfemu's --send TRIGGER=TEXT (a pattern
        // that needs '=' writes \x3d)
        size_t eq = arg.find('=');
        if (eq == std::string::npos) usage((std::string(what) + " needs REGEX=TEXT").c_str());
        t.pattern = arg.substr(0, eq);
        t.text = unescape(arg.substr(eq + 1));
    } else {
        t.pattern = arg;
    }
    t.re = compile(t.pattern, what);
    return t;
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
        else if (a == "--ps2-keys") o.ps2_keys = unescape(next());
        else if (a == "--vga-dir") o.vga_dir = next();
        else if (a == "--vga-font") o.vga_font = next();
        else if (a == "--vga-frames") o.vga_frames = unsigned(number(next(), "--vga-frames"));
        else if (a == "--vga-after") { o.vga_after = next(); (void)compile(o.vga_after, a.c_str()); }
        else if (a == "--trace") o.trace = next();
        else if (a == "--transcript") o.transcript = next();
        else if (a == "--max-cycles") o.max_cycles = number(next(), "--max-cycles");
        else if (a == "--progress") o.progress = number(next(), "--progress");
        else if (a == "--settle") o.settle = number(next(), "--settle");
        else if (a == "--expect") o.expects.push_back(trigger(next(), false, "--expect"));
        else if (a == "--send") o.sends.push_back(trigger(next(), true, "--send"));
        else if (a == "--ps2-send") o.ps2_sends.push_back(trigger(next(), true, "--ps2-send"));
        else if (a == "--fail-on") o.fail_ons.push_back(trigger(next(), false, "--fail-on"));
        else if (a == "--seed") o.seed = number(next(), "--seed");
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--uart-loopback") o.uart_loopback = true;
        else if (a == "--gpio-loopback") o.gpio_loopback = true;
        else if (a == "--sd-flush") o.sd_flush = true;
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

// text for a log line: control characters as escapes
std::string shown(const std::string &t) {
    std::string out;
    for (unsigned char c : t) {
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20 || c >= 0x7F) {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02x", c);
            out += b;
        } else out += char(c);
    }
    return out;
}

// an ordered list of patterns, each searched after the previous one's match
struct Sequence {
    std::vector<Trigger> *items;
    size_t next = 0;
    size_t from = 0;                // output offset where the next search starts
    bool done() const { return next >= items->size(); }
};

const size_t WINDOW = 16384;        // patterns are searched over at most this much output
const size_t FAIL_WINDOW = 4096;    // --fail-on: the newest output (a match ends the run at once)

bool search(const std::string &out, const std::regex &re, size_t from, size_t &end) {
    size_t start = std::max(from, out.size() > WINDOW ? out.size() - WINDOW : size_t(0));
    std::smatch m;
    if (!std::regex_search(out.cbegin() + long(start), out.cend(), m, re)) return false;
    end = start + size_t(m.position(0) + m.length(0));
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    Options opt = parse(argc, argv);
    const auto wall0 = std::chrono::steady_clock::now();

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

    // ---------------------------------------------------------------- the models
    const unsigned cpb = (SURF_SYS_HZ + 115200 / 2) / 115200;
    UartModel uart(cpb);
    DdrModel ddr(opt.ddr);
    for (uint32_t a : opt.ddr_stalls) ddr.stall_at(a);

    SdCardOptions sdo;
    sdo.seed = unsigned(opt.seed);
    sdo.present = !opt.sd.empty();
    SdCardModel sd(opt.sd, sdo);
    if (!opt.sd.empty() && !sd.error().empty()) {
        std::fprintf(stderr, "surfsim: --sd %s: %s\n", opt.sd.c_str(), sd.error().c_str());
        return 1;
    }

    Ps2KeyboardModel kbd(SURF_SYS_HZ);

    const bool vga_on = !opt.vga_dir.empty();
    bool vga_capturing = vga_on && opt.vga_after.empty();
    std::regex vga_after_re;
    if (!opt.vga_after.empty()) vga_after_re = std::regex(opt.vga_after);
    VgaCapture cap;
    VgaFont font;
    if (vga_on && !font.load_verilog(opt.vga_font) && !font.load_c(opt.vga_font)) {
        std::fprintf(stderr, "surfsim: --vga-font %s: no font found\n", opt.vga_font.c_str());
        return 1;
    }
    uint64_t vga_frames_written = 0, vga_last_frame = 0;

    std::string uart_input;
    if (!opt.uart_in.empty()) uart_input = read_file(opt.uart_in);

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
    FILE *transcript = nullptr;
    if (!opt.transcript.empty()) {
        transcript = std::fopen(opt.transcript.c_str(), "wb");
        if (!transcript) {
            std::fprintf(stderr, "surfsim: cannot write %s\n", opt.transcript.c_str());
            return 1;
        }
    }

    // clocks in femtoseconds, from the generated rates
    const uint64_t clk_half = 500000000000000ull / SURF_SYS_HZ;
    const uint64_t pix_half = (500000000000000ull + SURF_PIX_HZ / 2) / SURF_PIX_HZ;
    const unsigned RESET_CYCLES = 8;

    top->clk = 0;
    top->pix_clk = 0;
    top->rst = 1;
    top->pix_rst = 1;
    top->uart_rx = 1;
    top->sw = opt.sw;
    top->btn = opt.btn;
    top->sd_miso = 1;
    top->sd_cd_n = sd.cd_n();
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
    Sequence expects{&opt.expects}, sends{&opt.sends}, ps2_sends{&opt.ps2_sends};
    size_t vga_from = 0;
    std::string failed_on;
    bool matched = false, out_of_cycles = false;
    uint64_t settle_end = 0;
    size_t uart_sent = 0;                         // bytes queued to the SoC's UART receiver

    while (!ctx->gotFinish()) {
        const uint64_t t = std::min(next_clk, next_pix);
        const bool do_clk = next_clk == t, do_pix = next_pix == t;
        ctx->time(t / 1000);                       // ps for the RTL's $time

        const bool clk_rising = do_clk && !top->clk;
        if (clk_rising) {
            // the cycle that ends here, as the trace, the DDR model and the UART model see it
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
                uart_sent += uart_input.size();
                if (!opt.ps2_keys.empty()) kbd.type_text(opt.ps2_keys);
            }
            if (!top->rst) cycles++;
            // the models' answers for the next cycle (the SD card and the keyboard react to
            // the SoC's registered outputs of the cycle that starts here, docs/IO.md section 6)
            top->ddr_req_ready = ddr.req_ready();
            top->ddr_rsp_valid = ddr.rsp_valid();
            top->ddr_rsp_rdata = ddr.rsp_rdata();
            top->ddr_rsp_err = ddr.rsp_err();
            top->ddr_calib_done = ddr.calib_done();
            top->ddr_calib_fail = ddr.calib_fail();
            top->ddr_timeout = ddr.timeout();
            top->uart_rx = opt.uart_loopback ? top->uart_tx : uart.rx_pin();
            top->sd_miso = sd.tick(top->sd_cs_n, top->sd_sclk, top->sd_mosi);
            top->sd_cd_n = sd.cd_n();
            bool ps2_clk = true, ps2_dat = true;
            kbd.tick(top->ps2_clk_oe, top->ps2_data_oe, ps2_clk, ps2_dat);
            top->ps2_clk_i = ps2_clk;
            top->ps2_data_i = ps2_dat;
            if (opt.gpio_loopback) {
                top->sw = top->led;
                top->btn = top->led;
            }
            // (no eval here: the falling edge's eval, always before the next rising edge,
            // settles these inputs; nothing in the SoC uses a falling edge)

            std::string got = uart.take_output();
            if (!got.empty()) {
                if (!opt.quiet) {
                    std::fwrite(got.data(), 1, got.size(), stdout);
                    std::fflush(stdout);
                }
                if (transcript) std::fwrite(got.data(), 1, got.size(), transcript);
                output += got;
                size_t end;
                // each match and send is logged on stderr with its cycle
                while (!expects.done() && search(output, (*expects.items)[expects.next].re,
                                                 expects.from, end)) {
                    expects.from = end;
                    expects.next++;
                    std::fprintf(stderr, "surfsim: cycle %" PRIu64 ": --expect %zu of %zu matched\n",
                                 cycles, expects.next, opt.expects.size());
                }
                while (!sends.done() && search(output, (*sends.items)[sends.next].re, sends.from,
                                               end)) {
                    uart.send((*sends.items)[sends.next].text);
                    uart_sent += (*sends.items)[sends.next].text.size();
                    sends.from = end;
                    sends.next++;
                    std::fprintf(stderr, "surfsim: cycle %" PRIu64 ": --send %zu of %zu: %s\n", cycles,
                                 sends.next, opt.sends.size(), shown(opt.sends[sends.next - 1].text).c_str());
                }
                while (!ps2_sends.done() && search(output, (*ps2_sends.items)[ps2_sends.next].re,
                                                   ps2_sends.from, end)) {
                    kbd.type_text((*ps2_sends.items)[ps2_sends.next].text);
                    ps2_sends.from = end;
                    ps2_sends.next++;
                    std::fprintf(stderr, "surfsim: cycle %" PRIu64 ": --ps2-send %zu of %zu: %s\n",
                                 cycles, ps2_sends.next, opt.ps2_sends.size(),
                                 shown(opt.ps2_sends[ps2_sends.next - 1].text).c_str());
                }
                const size_t fail_from = output.size() > FAIL_WINDOW ? output.size() - FAIL_WINDOW : 0;
                for (auto &f : opt.fail_ons)
                    if (search(output, f.re, fail_from, end)) failed_on = f.pattern;
                if (vga_on && !vga_capturing && search(output, vga_after_re, vga_from, end)) {
                    vga_capturing = true;
                    vga_from = end;
                    std::fprintf(stderr, "surfsim: cycle %" PRIu64 ": VGA capture starts\n", cycles);
                }
                if (!failed_on.empty()) break;
                if (!opt.expects.empty() && expects.done() && !matched) {
                    matched = true;
                    settle_end = cycles + opt.settle;
                }
            }
            if (matched && cycles >= settle_end) break;
            if (opt.progress && cycles % opt.progress == 0 && cycles)
                std::fprintf(stderr, "surfsim: %" PRIu64 " cycles, %zu bytes out, SD %" PRIu64
                                     " blocks read\n", cycles, output.size(),
                             sd.stats.blocks_read);
            if (opt.max_cycles && cycles >= opt.max_cycles) {
                out_of_cycles = true;
                break;
            }
        }
        if (do_pix && top->pix_clk) {
            pix_edges++;
            if (pix_edges == RESET_CYCLES) top->pix_rst = 0;
            if (vga_capturing) {
                cap.tick(top->vga_r, top->vga_g, top->vga_b, top->vga_hs, top->vga_vs);
                if (opt.vga_frames && cap.frames() != vga_last_frame) {
                    vga_last_frame = cap.frames();
                    if (vga_last_frame % opt.vga_frames == 0) {
                        char name[64];
                        std::snprintf(name, sizeof name, "/frame_%06" PRIu64 ".ppm", vga_last_frame);
                        if (cap.write_ppm(opt.vga_dir + name)) vga_frames_written++;
                    }
                }
            }
        }
        if (do_clk) next_clk += clk_half;
        if (do_pix) next_pix += pix_half;
    }

    top->final();
    if (trace) std::fclose(trace);
    if (transcript) std::fclose(transcript);

    // ---------------------------------------------------------------- results
    if (vga_on) {
        if (cap.frames() == 0) {
            std::fprintf(stderr, "surfsim: VGA: no whole frame captured\n");
        } else {
            cap.write_ppm(opt.vga_dir + "/screen.ppm");
            const VgaMeasured &m = cap.measured();
            VgaText text = cap.decode_text(font);
            std::ofstream f(opt.vga_dir + "/screen.txt");
            f << "# surfsim VGA capture: " << m.frames << " frames; line " << m.h_total
              << " pixels (sync " << m.h_sync << (m.hsync_negative ? " negative" : " positive")
              << "), frame " << m.v_total << " lines (sync " << m.v_sync
              << (m.vsync_negative ? " negative" : " positive") << "), "
              << m.frame_hz(SURF_PIX_HZ) << " Hz at " << SURF_PIX_HZ << " Hz; text " << text.cols
              << "x" << text.rows << ", cursor at column " << text.cursor_col << " row "
              << text.cursor_row << ", " << text.unmatched << " unmatched cells\n";
            for (int r = 0; r < text.rows; r++) f << text.printable(r) << "\n";
        }
    }
    if (opt.sd_flush && sd.dirty_blocks()) {
        size_t n = sd.dirty_blocks();
        if (!sd.flush())
            std::fprintf(stderr, "surfsim: --sd-flush: %s\n", sd.error().c_str());
        else
            std::fprintf(stderr, "surfsim: %zu card blocks written back to %s\n", n, opt.sd.c_str());
    }

    int status = 0;
    const char *why = "finished";
    if (ctx->gotError()) {
        status = 1;
        why = "stopped by $fatal/$stop";
    } else if (!failed_on.empty()) {
        status = 1;
        why = "a --fail-on pattern matched";
    } else if (matched) {
        why = "output matched";
    } else if (out_of_cycles) {
        status = opt.expects.empty() ? 0 : 2;
        why = opt.expects.empty() ? "max-cycles reached" : "max-cycles reached before the output matched";
    } else if (!opt.expects.empty()) {
        status = 1;
        why = "finished without the expected output";
    }
    if (ddr.early_requests || ddr.protocol_errors) {
        std::fprintf(stderr, "surfsim: ERROR the SoC sent %" PRIu64 " DDR requests before "
                             "calibration and broke the handshake %" PRIu64 " times\n",
                     ddr.early_requests, ddr.protocol_errors);
        status = 1;
    }
    // the card's and the keyboard's protocol mistakes fail the run too, with their logs' ends
    auto log_tail = [](const char *who, const std::vector<std::string> &log) {
        for (size_t i = log.size() > 12 ? log.size() - 12 : 0; i < log.size(); i++)
            std::fprintf(stderr, "surfsim: %s: %s\n", who, log[i].c_str());
    };
    if (sd.stats.violations) {
        std::fprintf(stderr, "surfsim: ERROR %" PRIu64 " SD protocol violations; the card's log ends:\n",
                     sd.stats.violations);
        log_tail("card", sd.log);
        status = 1;
    }
    if (kbd.stats.violations) {
        std::fprintf(stderr, "surfsim: ERROR %" PRIu64 " PS/2 protocol violations; the keyboard's log ends:\n",
                     kbd.stats.violations);
        log_tail("keyboard", kbd.log);
        status = 1;
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    if (!failed_on.empty()) std::fprintf(stderr, "surfsim: --fail-on matched: %s\n", failed_on.c_str());
    if (!expects.done() && !opt.expects.empty())
        std::fprintf(stderr, "surfsim: --expect %zu of %zu still waiting: %s\n", expects.next + 1,
                     opt.expects.size(), opt.expects[expects.next].pattern.c_str());
    std::fprintf(stderr,
                 "surfsim: card %s: %" PRIu64 " blocks read, %" PRIu64 " written, %" PRIu64
                 " protocol violations; keyboard: %" PRIu64 " frames sent, %zu host bytes, %" PRIu64
                 " violations; VGA: %s\n",
                 opt.sd.empty() ? "absent" : opt.sd.c_str(), sd.stats.blocks_read,
                 sd.stats.blocks_written, sd.stats.violations, kbd.stats.frames_sent,
                 kbd.host_bytes.size(), kbd.stats.violations,
                 vga_on ? (std::to_string(cap.frames()) + " frames captured").c_str() : "off");
    std::fprintf(stderr,
                 "surfsim: %s after %" PRIu64 " cycles (%.3f s simulated) in %.1f s wall (%.2f M "
                 "cycles/s); UART %zu bytes in, %zu out, %" PRIu64 " framing errors; DDR %s, %" PRIu64
                 " reads %" PRIu64 " writes %" PRIu64 " fetches %" PRIu64 " errors; exit %d\n",
                 why, cycles, double(cycles) / SURF_SYS_HZ, wall, wall > 0 ? cycles / wall / 1e6 : 0.0,
                 uart_sent, output.size(), uart.framing_errors(),
                 ddr.calib_done() ? "calibrated" : (ddr.calib_fail() ? "failed" : "not calibrated"),
                 ddr.reads, ddr.writes, ddr.fetches, ddr.error_responses, status);
    return status;
}
