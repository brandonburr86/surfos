/*
SurfCPU system simulation
--------------------
File: surfsim.cpp Date: 10/16/26 (SurfCPU)
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
          [--ddr-stall ADDR] [--ddr-swinit] [--sd-fault KIND=N@REGEX]... [--sd-remove [N@]REGEX]...
          [--sd-insert REGEX]... [--uart-pty] [+plusarg ...]

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

Card actions (10/16/26, for the os track's fault tests): --sd-fault KIND=N@REGEX sets the card
model's faults.KIND (SdFaults in sdcard_model.h: no_response, cmd_crc_error, read_error_token,
error_token, read_bad_crc, write_crc_reject, write_error, busy_extra, init_never_ready) to N
once REGEX has matched; N=0 clears it. --sd-remove REGEX takes the card out (card detect off,
the card's state lost, what it was given kept); with N@ it comes out right after the Nth
block it accepts from then on, in the middle of whatever the SoC is writing. --sd-insert REGEX
puts it back (powered up afresh). The actions go in the --send list, in order with the sends:
each REGEX is searched after the previous entry's match, and an action does not move that
point, so the entry after it may match the same text (arm a fault at a prompt, then type the
command at that prompt). Each is logged on stderr with its cycle.

UART on a pseudo-terminal (--uart-pty, 10/16/26, for the os track's physical test kit): the
UART also goes to a pseudo-terminal, whose path is printed on stderr ("surfsim: --uart-pty
/dev/pts/N"), so tools/uart.py and tools/board-test.py talk to the RTL system exactly as to
the board's serial port. Its far side is raw. The run starts only once a program has opened
it, so nothing the SoC prints is lost; what is typed there goes to the SoC's receive pin (with
--uart-in, --send and --ps2-send as well), and what the SoC prints goes there (and to stdout,
--expect and the transcript as always), held while nobody reads it. SIGTERM or SIGINT then
ends the run as running out of --max-cycles would, so --sd-flush keeps what the card got: a
power cut, with the card's contents kept. SIGUSR1 takes the card out and SIGUSR2 puts it back
(a test's stand-in for the owner's hands).

PS/2 keyboard: the io track's model on the open-drain lines (a line is low when the SoC's
_oe or the keyboard pulls it, high otherwise); --ps2-keys TEXT is typed after reset.

VGA (--vga-dir DIR): the io track's capture model on vga_r/g/b/hs/vs, ticked every pix_clk
(pix_clk always runs; the capture adds about 1% to the run time). At the end it writes DIR/screen.ppm (the last whole
frame), DIR/screen.txt (the measured timing and the 80x30 text decoded from the pixels with
the font of --vga-font, default rtl/peripherals/surf_video_font.v, and the cursor's cell: when
the last frame is in the cursor's dark blink phase, the cell where --settle's frames last
showed it) and, with --vga-frames N, every Nth frame as DIR/frame_NNNNNN.ppm. --vga-after REGEX starts the capture only once REGEX
has matched the UART output. --vga-crc FILE (10/18/26, the gpu track) writes a line "frame N crc
XXXXXXXX" to FILE for every whole frame captured: zlib's CRC-32 of its 640x480 pixels, each as a
little-endian 16-bit RGB444 value, which is how surf_video's FRAME_CRC is defined (docs/GPU.md
section 3); it needs no --vga-dir.

DDR: the C++ DDR model, calibrated after --ddr-calib cycles (default 3000) with random ready
and latency from --seed; tb_soc's plusargs +ddr_calib= +ddr_lat_min= +ddr_lat_max= +ddr_ready=
+ddr_seed= +ddr_fail +ddr_stall= work too. --ddr-swinit (+ddr_swinit), for an SoC built with
DDR_SWINIT=1: the ddrc port answers from LiteDRAM's register model (firmware/tests/
ddrc_model.h, lane windows as the firmware tests' --ddr swinit), each access 8 cycles after
acceptance, and the SoC sees calib_done once software has initialized the model (its
init_done without init_error), or calib_fail when it has not --ddr-calib cycles after reset
(then the bridge's limit, default 37,500,000: 1.5 s at 25 MHz); a protocol violation the
model counts fails the run. Other plusargs go to the RTL (+script=FILE for the
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

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "Vsurfcpu_soc.h"
#include "verilated.h"
#include "../../../firmware/tests/ddrc_model.h"

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
    // the card actions share the --send list (10/16/26): what to do once the pattern matched
    enum Kind { SEND, SD_FAULT, SD_REMOVE, SD_INSERT } kind = SEND;
    std::string fault;              // --sd-fault: the SdFaults member
    uint64_t value = 0;             // --sd-fault: its value; --sd-remove N@: blocks taken first
};

struct Options {
    std::string rom, sd, uart_in, ps2_keys, vga_dir, trace, transcript;
    std::string vga_font = "rtl/peripherals/surf_video_font.v", vga_after;
    std::string vga_crc;                // every whole frame's CRC to this file (10/18/26)
    std::vector<Trigger> expects, sends, ps2_sends, fail_ons;
    uint64_t max_cycles = 0;            // 0 = no limit
    uint64_t seed = 1;
    uint64_t progress = 0;              // print a progress line every N cycles (0 = never)
    uint64_t settle = 0;                // cycles run on after the last --expect matched
    unsigned vga_frames = 0;            // also write every Nth frame
    bool quiet = false, uart_loopback = false, gpio_loopback = false, sd_flush = false;
    bool uart_pty = false;              // the UART on a pseudo-terminal too (10/16/26)
    unsigned sw = 0, btn = 0;
    DdrModelOptions ddr;
    bool ddr_seed_given = false;
    bool ddr_swinit = false;            // the ddrc port and LiteDRAM's register model (10/16/26)
    bool ddr_calib_given = false;
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
                 "               [--vga-font FILE] [--vga-frames N] [--vga-after REGEX] [--vga-crc FILE]\n"
                 "               [--ddr-calib N] [--ddr-fail] [--ddr-latency MIN:MAX] [--ddr-ready PCT]\n"
                 "               [--ddr-stall ADDR] [--ddr-swinit] [--sd-fault KIND=N@REGEX]... [--sd-remove [N@]REGEX]...\n"
                 "               [--sd-insert REGEX]... [--uart-pty] [+plusarg ...]\n");
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

// --sd-fault's KIND: set one of the card model's faults (false: no such fault)
bool set_fault(SdFaults &f, const std::string &kind, uint64_t v) {
    if (kind == "no_response") f.no_response = int(v);
    else if (kind == "cmd_crc_error") f.cmd_crc_error = int(v);
    else if (kind == "read_error_token") f.read_error_token = int(v);
    else if (kind == "error_token") f.error_token = uint8_t(v);
    else if (kind == "read_bad_crc") f.read_bad_crc = int(v);
    else if (kind == "write_crc_reject") f.write_crc_reject = int(v);
    else if (kind == "write_error") f.write_error = int(v);
    else if (kind == "busy_extra") f.busy_extra = v;
    else if (kind == "init_never_ready") f.init_never_ready = v != 0;
    else return false;
    return true;
}

// --sd-fault KIND=N@REGEX, --sd-remove [N@]REGEX, --sd-insert REGEX: entries of the --send list
Trigger card_action(const std::string &arg, Trigger::Kind kind, const char *what) {
    Trigger t;
    t.kind = kind;
    std::string rest = arg;
    if (kind == Trigger::SD_FAULT) {
        size_t eq = arg.find('='), at = eq == std::string::npos ? eq : arg.find('@', eq);
        if (at == std::string::npos) usage("--sd-fault needs KIND=N@REGEX");
        t.fault = arg.substr(0, eq);
        t.value = number(arg.substr(eq + 1, at - eq - 1), what);
        SdFaults probe;
        if (!set_fault(probe, t.fault, t.value)) usage(("--sd-fault: no fault " + t.fault).c_str());
        rest = arg.substr(at + 1);
    } else if (kind == Trigger::SD_REMOVE) {
        // N@ before the pattern: the blocks the card takes before it comes out (a pattern that
        // starts with digits and @ writes \x40 for the @)
        size_t at = arg.find('@');
        if (at != std::string::npos && at > 0 &&
            std::all_of(arg.begin(), arg.begin() + long(at), [](char c) { return std::isdigit((unsigned char)c); })) {
            t.value = number(arg.substr(0, at), what);
            rest = arg.substr(at + 1);
        }
    }
    t.pattern = rest;
    t.re = compile(rest, what);
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
    if (val("ddr_calib", v)) { o.ddr.calib_cycles = snumber(v, "+ddr_calib"); o.ddr_calib_given = true; }
    else if (val("ddr_lat_min", v)) o.ddr.latency_min = unsigned(number(v, "+ddr_lat_min"));
    else if (val("ddr_lat_max", v)) o.ddr.latency_max = unsigned(number(v, "+ddr_lat_max"));
    else if (val("ddr_ready", v)) o.ddr.ready_percent = unsigned(number(v, "+ddr_ready"));
    else if (val("ddr_seed", v)) { o.ddr.seed = number(v, "+ddr_seed"); o.ddr_seed_given = true; }
    else if (val("ddr_stall", v)) o.ddr_stalls.push_back(uint32_t(number("0x" + v, "+ddr_stall")));
    else if (a == "+ddr_fail") o.ddr.calib_fails = true;
    else if (a == "+ddr_swinit") o.ddr_swinit = true;
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
        else if (a == "--vga-crc") o.vga_crc = next();
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
        else if (a == "--sd-fault") o.sends.push_back(card_action(next(), Trigger::SD_FAULT, "--sd-fault"));
        else if (a == "--sd-remove") o.sends.push_back(card_action(next(), Trigger::SD_REMOVE, "--sd-remove"));
        else if (a == "--sd-insert") o.sends.push_back(card_action(next(), Trigger::SD_INSERT, "--sd-insert"));
        else if (a == "--seed") o.seed = number(next(), "--seed");
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--uart-loopback") o.uart_loopback = true;
        else if (a == "--gpio-loopback") o.gpio_loopback = true;
        else if (a == "--sd-flush") o.sd_flush = true;
        else if (a == "--uart-pty") o.uart_pty = true;
        else if (a == "--sw") o.sw = unsigned(number(next(), "--sw")) & 0xF;
        else if (a == "--btn") o.btn = unsigned(number(next(), "--btn")) & 0xF;
        else if (a == "--ddr-calib") { o.ddr.calib_cycles = snumber(next(), "--ddr-calib"); o.ddr_calib_given = true; }
        else if (a == "--ddr-swinit") o.ddr_swinit = true;
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

// --uart-pty: SIGTERM or SIGINT ends the run as --max-cycles would
volatile std::sig_atomic_t stop_requested = 0, card_out_requested = 0, card_in_requested = 0;
void on_stop(int) { stop_requested = 1; }
void on_card(int sig) { (sig == SIGUSR1 ? card_out_requested : card_in_requested) = 1; }

// --uart-pty: a pseudo-terminal whose far side is raw; its path, or "" with the reason in why
int open_pty(std::string &path, std::string &why) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) {
        why = std::strerror(errno);
        if (m >= 0) close(m);
        return -1;
    }
    path = ptsname(m);
    int s = open(path.c_str(), O_RDWR | O_NOCTTY);       // raw: the setting outlives this open
    if (s >= 0) {
        termios t;
        if (tcgetattr(s, &t) == 0) {
            cfmakeraw(&t);
            tcsetattr(s, TCSANOW, &t);
        }
        close(s);
    }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    return m;
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
    // --ddr-swinit: the bridge's calibration limit is --ddr-calib; the data side is usable at once
    // (the SoC's gate keeps requests away until calib_done, which the register model decides)
    const int64_t swinit_limit = opt.ddr_calib_given ? opt.ddr.calib_cycles : 37500000;
    if (opt.ddr_swinit) opt.ddr.calib_cycles = 0;
    DdrModel ddr(opt.ddr);
    for (uint32_t a : opt.ddr_stalls) ddr.stall_at(a);
    struct ddrc_model ddrc;
    std::memset(&ddrc, 0, sizeof ddrc);
    ddrc.lane[0] = ddrc_lane{3, 6, 26};
    ddrc.lane[1] = ddrc_lane{5, 10, 20};
    ddrc_model_init(&ddrc);
    struct { bool busy = false, rsp = false, err = false; unsigned left = 0; uint32_t rdata = 0; } dc;
    bool swinit_ever = false, swinit_fail = false;

    SdCardOptions sdo;
    sdo.seed = unsigned(opt.seed);
    sdo.present = !opt.sd.empty();
    SdCardModel sd(opt.sd, sdo);
    if (!opt.sd.empty() && !sd.error().empty()) {
        std::fprintf(stderr, "surfsim: --sd %s: %s\n", opt.sd.c_str(), sd.error().c_str());
        return 1;
    }

    Ps2KeyboardModel kbd(SURF_SYS_HZ);

    const bool vga_on = !opt.vga_dir.empty() || !opt.vga_crc.empty();
    std::FILE *vga_crc = nullptr;
    if (!opt.vga_crc.empty() && !(vga_crc = std::fopen(opt.vga_crc.c_str(), "w"))) {
        std::fprintf(stderr, "surfsim: --vga-crc %s: cannot write\n", opt.vga_crc.c_str());
        return 1;
    }
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
    // the cursor blinks (32 frames, half of them dark): while --settle runs, every whole frame is
    // decoded and the cursor's last position kept, for a last frame in the dark half
    int seen_col = -1, seen_row = -1;
    uint64_t seen_frame = 0, settle_frame = 0;

    std::string uart_input;
    if (!opt.uart_in.empty()) uart_input = read_file(opt.uart_in);

    // --sd-remove N@: out once the card has taken this many blocks in all (0: not armed)
    uint64_t sd_out_at = 0, sd_faults_set = 0, sd_removals = 0, sd_insertions = 0;

    // --uart-pty: the run waits for a program to open the pseudo-terminal
    int pty = -1;
    std::string pty_pending;                      // output the pseudo-terminal has not taken yet
    uint64_t pty_in = 0, pty_dropped = 0;
    if (opt.uart_pty) {
        std::string path, why;
        pty = open_pty(path, why);
        if (pty < 0) {
            std::fprintf(stderr, "surfsim: --uart-pty: %s\n", why.c_str());
            return 1;
        }
        std::signal(SIGTERM, on_stop);
        std::signal(SIGINT, on_stop);
        std::signal(SIGUSR1, on_card);
        std::signal(SIGUSR2, on_card);
        std::fprintf(stderr, "surfsim: --uart-pty %s\n", path.c_str());
        std::fflush(stderr);
        for (;;) {                                // POLLHUP while nobody has the far side open
            pollfd p{pty, POLLIN, 0};
            poll(&p, 1, 100);
            if (!(p.revents & POLLHUP) || stop_requested) break;
        }
        std::fprintf(stderr, "surfsim: --uart-pty: %s\n", stop_requested ? "stopped before anyone opened it"
                                                                            : "opened: the run starts");
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
    top->ddrc_req_ready = 0;
    top->ddrc_rsp_valid = 0;
    top->ddrc_rsp_rdata = 0;
    top->ddrc_rsp_err = 0;
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
            if (opt.ddr_swinit && !top->rst) {      // the ddrc port: one access at a time
                if (dc.rsp) dc.rsp = false;         // the response was taken at this edge
                if (top->ddrc_req_valid && top->ddrc_req_ready) {
                    const uint32_t off = top->ddrc_req_addr & SURF_DDRC_MASK;
                    dc.err = top->ddrc_req_be != 0xF || (off & 3u);   // surf_ddr_csr: words only
                    dc.rdata = 0;
                    if (!dc.err && top->ddrc_req_write) ddrc_model_write(&ddrc, off, top->ddrc_req_wdata);
                    else if (!dc.err) dc.rdata = ddrc_model_read(&ddrc, off);
                    dc.busy = true;
                    dc.left = 8;
                } else if (dc.busy && --dc.left == 0) {
                    dc.busy = false;
                    dc.rsp = true;
                }
            }
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
            if (pty >= 0 && (cycles & 1023) == 0 && !top->rst) {
                // the pseudo-terminal: what was typed to the receive pin, what was printed out
                char buf[256];
                ssize_t n = read(pty, buf, sizeof buf); // (EAGAIN: nothing typed; EIO: nobody there now)
                if (n > 0) {
                    uart.send(std::string(buf, size_t(n)));
                    uart_sent += size_t(n);
                    pty_in += uint64_t(n);
                }
                if (!pty_pending.empty()) {
                    ssize_t w = write(pty, pty_pending.data(), pty_pending.size());
                    if (w > 0) pty_pending.erase(0, size_t(w));
                }
                if (card_out_requested || card_in_requested) {    // the owner's hands (SIGUSR1/2)
                    const bool in = card_in_requested;
                    card_out_requested = card_in_requested = 0;
                    sd.set_present(in);
                    (in ? sd_insertions : sd_removals)++;
                    std::fprintf(stderr, "surfsim: cycle %" PRIu64 ": %s\n", cycles,
                                 in ? "SIGUSR2: the card is in" : "SIGUSR1: the card is out");
                }
                if (stop_requested) {
                    out_of_cycles = true;
                    break;
                }
            }
            // the models' answers for the next cycle (the SD card and the keyboard react to
            // the SoC's registered outputs of the cycle that starts here, docs/IO.md section 6)
            top->ddr_req_ready = ddr.req_ready();
            top->ddr_rsp_valid = ddr.rsp_valid();
            top->ddr_rsp_rdata = ddr.rsp_rdata();
            top->ddr_rsp_err = ddr.rsp_err();
            if (opt.ddr_swinit) {                   // as surf_ddr_bridge: CALIB_FAIL sticky, limit from reset
                const bool ready = ddrc_model_ready(&ddrc);
                if (!swinit_fail && ready) swinit_ever = true;
                else if (!swinit_ever && swinit_limit >= 0 && int64_t(cycles) >= swinit_limit) swinit_fail = true;
                top->ddr_calib_done = !swinit_fail && ready;
                top->ddr_calib_fail = swinit_fail;
                top->ddrc_req_ready = !top->rst && !dc.busy && !dc.rsp;
                top->ddrc_rsp_valid = dc.rsp;
                top->ddrc_rsp_rdata = dc.rsp ? dc.rdata : 0;
                top->ddrc_rsp_err = dc.rsp && dc.err;
            } else {
                top->ddr_calib_done = ddr.calib_done();
                top->ddr_calib_fail = ddr.calib_fail();
            }
            top->ddr_timeout = ddr.timeout();
            top->uart_rx = opt.uart_loopback ? top->uart_tx : uart.rx_pin();
            top->sd_miso = sd.tick(top->sd_cs_n, top->sd_sclk, top->sd_mosi);
            if (sd_out_at && sd.stats.blocks_written >= sd_out_at) {
                // --sd-remove N@: right after the Nth block, before the card can answer for it
                sd.set_present(false);
                top->sd_miso = 1;
                sd_out_at = 0;
                sd_removals++;
                std::fprintf(stderr, "surfsim: cycle %" PRIu64 ": --sd-remove: the card is out, after block %" PRIu64
                                     " of those written\n", cycles, sd.stats.blocks_written);
            }
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
                if (pty >= 0) {
                    pty_pending += got;
                    if (pty_pending.size() > (1u << 20)) {     // nobody has read for a long while
                        pty_dropped += pty_pending.size() - (1u << 20);
                        pty_pending.erase(0, pty_pending.size() - (1u << 20));
                    }
                }
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
                    const Trigger &s = (*sends.items)[sends.next];
                    sends.next++;
                    if (s.kind == Trigger::SEND) {
                        uart.send(s.text);
                        uart_sent += s.text.size();
                        sends.from = end;
                        std::fprintf(stderr, "surfsim: cycle %" PRIu64 ": --send %zu of %zu: %s\n", cycles,
                                     sends.next, opt.sends.size(), shown(s.text).c_str());
                    } else if (s.kind == Trigger::SD_FAULT) {
                        // a card action leaves the search point where it was (see the top)
                        set_fault(sd.faults, s.fault, s.value);
                        sd_faults_set++;
                        std::fprintf(stderr, "surfsim: cycle %" PRIu64 ": --sd-fault %s=%" PRIu64 " (%zu of %zu)\n",
                                     cycles, s.fault.c_str(), s.value, sends.next, opt.sends.size());
                    } else if (s.kind == Trigger::SD_REMOVE && s.value) {
                        sd_out_at = sd.stats.blocks_written + s.value;
                        std::fprintf(stderr, "surfsim: cycle %" PRIu64 ": --sd-remove: the card comes out after %"
                                     PRIu64 " more blocks (%zu of %zu)\n", cycles, s.value, sends.next, opt.sends.size());
                    } else {
                        sd.set_present(s.kind == Trigger::SD_INSERT);
                        (s.kind == Trigger::SD_INSERT ? sd_insertions : sd_removals)++;
                        std::fprintf(stderr, "surfsim: cycle %" PRIu64 ": %s (%zu of %zu)\n", cycles,
                                     s.kind == Trigger::SD_INSERT ? "--sd-insert: the card is in"
                                                                  : "--sd-remove: the card is out",
                                     sends.next, opt.sends.size());
                    }
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
                if (cap.frames() != vga_last_frame) {
                    vga_last_frame = cap.frames();
                    if (vga_crc) {
                        uint32_t crc = 0xFFFFFFFFu;
                        for (uint16_t px : cap.frame())
                            for (int byte = 0; byte < 2; byte++) {
                                crc ^= (px >> (8 * byte)) & 0xFFu;
                                for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
                            }
                        std::fprintf(vga_crc, "frame %" PRIu64 " crc %08x\n", vga_last_frame, ~crc);
                    }
                    if (opt.vga_frames && vga_last_frame % opt.vga_frames == 0) {
                        char name[64];
                        std::snprintf(name, sizeof name, "/frame_%06" PRIu64 ".ppm", vga_last_frame);
                        if (cap.write_ppm(opt.vga_dir + name)) vga_frames_written++;
                    }
                    if (matched && settle_frame++ < 64) {
                        VgaText t = cap.decode_text(font);
                        if (t.cursor_row >= 0) {
                            seen_col = t.cursor_col;
                            seen_row = t.cursor_row;
                            seen_frame = vga_last_frame;
                        }
                    }
                }
            }
        }
        if (do_clk) next_clk += clk_half;
        if (do_pix) next_pix += pix_half;
    }

    top->final();
    if (pty >= 0) {
        if (!pty_pending.empty()) {               // what is left, if the reader takes it now
            ssize_t w = write(pty, pty_pending.data(), pty_pending.size());
            if (w > 0) pty_pending.erase(0, size_t(w));
        }
        std::fprintf(stderr, "surfsim: --uart-pty: %" PRIu64 " bytes typed%s; %zu printed bytes not taken, %" PRIu64
                             " dropped\n", pty_in, stop_requested ? "; stopped by a signal" : "",
                     pty_pending.size(), pty_dropped);
        close(pty);
    }
    if (trace) std::fclose(trace);
    if (transcript) std::fclose(transcript);

    // ---------------------------------------------------------------- results
    if (vga_crc) std::fclose(vga_crc);
    if (!opt.vga_dir.empty()) {
        if (cap.frames() == 0) {
            std::fprintf(stderr, "surfsim: VGA: no whole frame captured\n");
        } else {
            cap.write_ppm(opt.vga_dir + "/screen.ppm");
            const VgaMeasured &m = cap.measured();
            VgaText text = cap.decode_text(font);
            std::string blink;
            if (text.cursor_row < 0 && seen_row >= 0) {
                text.cursor_col = seen_col;
                text.cursor_row = seen_row;
                blink = " (dark in the last frame, its blink; seen in frame " + std::to_string(seen_frame) + ")";
            }
            std::ofstream f(opt.vga_dir + "/screen.txt");
            f << "# surfsim VGA capture: " << m.frames << " frames; line " << m.h_total
              << " pixels (sync " << m.h_sync << (m.hsync_negative ? " negative" : " positive")
              << "), frame " << m.v_total << " lines (sync " << m.v_sync
              << (m.vsync_negative ? " negative" : " positive") << "), "
              << m.frame_hz(SURF_PIX_HZ) << " Hz at " << SURF_PIX_HZ << " Hz; text " << text.cols
              << "x" << text.rows << ", cursor at column " << text.cursor_col << " row "
              << text.cursor_row << blink << ", " << text.unmatched << " unmatched cells\n";
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
        why = stop_requested ? (opt.expects.empty() ? "stopped by a signal" : "stopped by a signal before the output matched")
                             : opt.expects.empty() ? "max-cycles reached" : "max-cycles reached before the output matched";
    } else if (!opt.expects.empty()) {
        status = 1;
        why = "finished without the expected output";
    }
    if (opt.ddr_swinit) {
        std::fprintf(stderr, "surfsim: ddrc model: %u accesses, %u mode register loads, %u ZQCL, %u writes, %u reads, "
                             "%u violations; lanes at bitslip/delay %d/%d and %d/%d; %s\n", ddrc.accesses, ddrc.mrs,
                     ddrc.zqcl, ddrc.writes, ddrc.reads, ddrc.violations, ddrc.bitslip[0], ddrc.delay[0],
                     ddrc.bitslip[1], ddrc.delay[1],
                     swinit_fail ? "calibration failed (the limit passed)" : ddrc_model_ready(&ddrc) ? "initialized" : "not initialized");
        if (ddrc.violations) {
            std::fprintf(stderr, "surfsim: ERROR LiteDRAM's register model saw protocol violations\n");
            status = 1;
        }
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
    if (sd_faults_set || sd_removals || sd_insertions)
        std::fprintf(stderr, "surfsim: card actions: %" PRIu64 " faults set, taken out %" PRIu64 " times, put in %" PRIu64
                             " times\n", sd_faults_set, sd_removals, sd_insertions);
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
