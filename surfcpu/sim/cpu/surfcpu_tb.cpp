/*
SurfCPU core testbench
--------------------
File: surfcpu_tb.cpp Date: 10/15/26 (SurfCPU)
--------------------
build/cpu/surfcpu_tb (docs/MODULES.md section 6): runs one RISC-V ELF program on
surfcpu_core alone, under Verilator.

  surfcpu_tb --elf FILE [--trace FILE [--trace-tail N]] [--seed N] [--max-cycles N]
             [--ilat MIN:MAX] [--dlat MIN:MAX] [--signature FILE --sig-begin SYM --sig-end SYM]
             [--vcd FILE] [--rand-reset] [--quiet]

Memory: 4 MiB of RAM at 0x80000000 loaded from the ELF; the core starts at 0x80000000. Every
other address answers with a bus error, except a CLINT-compatible timer at 0x02000000
(msip +0, mtimecmp +0x4000, mtime +0xBFF8; mtime counts clock cycles) and the testbench
registers at 0x03000000 (docs/CPU.md section 9: console, interrupt control, access probes).
Fetches succeed only from RAM.

Bus timing: each port is a SurfBus slave whose req_ready waits and response latencies are
drawn from --seed: a request waits 0..(MAX-MIN) cycles for req_ready and its response comes
MIN..MAX cycles after acceptance (half of the draws take the shortest value, the rest are
uniform). req_ready is random while req_valid is low. The harness checks the master side of
the protocol on both ports (no withdrawal or change of a waiting request, one outstanding
request, the fixed fields, legal byte enables for the address) and stops with exit 3 on a
violation.

The program ends by storing to its `tohost` symbol: 1 = pass (exit 0); any other value =
fail, test number value >> 1 (exit 1). The run continues until the RVFI record of that
store, so the trace ends with it. Running past --max-cycles exits 2; a testbench error
(bad arguments or ELF, protocol violation) exits 3. --signature dumps the words between
the two symbols, one per line, in every case. --trace writes the retirement trace of
docs/MODULES.md section 5 (field formats: sim/README.md); with --trace-tail N only the last
N records are kept (in memory, written when the run ends), so a runaway program cannot
fill the disk.
*/
#include <elf.h>

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "Vsurfcpu_tb_top.h"
#include "verilated.h"
#if VM_TRACE
#include "verilated_vcd_c.h"
#endif

namespace {

constexpr uint32_t RAM_BASE   = 0x80000000u;
constexpr uint32_t RAM_SIZE   = 4u << 20;
constexpr uint32_t CLINT_BASE = 0x02000000u;
constexpr uint32_t CLINT_SIZE = 0x00010000u;
constexpr uint32_t TB_BASE    = 0x03000000u;
constexpr uint32_t TB_SIZE    = 0x00001000u;

// testbench registers (docs/CPU.md section 9), offsets from TB_BASE
constexpr uint32_t TB_CONSOLE    = 0x000;  // wo: bits 7:0 to stdout
constexpr uint32_t TB_IRQ_EXT    = 0x004;  // rw: bit 0 = irq_external level
constexpr uint32_t TB_IRQ_DELAY  = 0x008;  // rw: N > 0 sets IRQ_EXT after N cycles; reads cycles left
constexpr uint32_t TB_IRQ_RANDOM = 0x00C;  // rw: N > 0 sets IRQ_EXT with probability 1/N each cycle
constexpr uint32_t TB_PROBE_RD   = 0x010;  // rc: returns how many times it was read before; write sets the count
constexpr uint32_t TB_PROBE_WR   = 0x014;  // rw: a write counts one; reads the count
constexpr uint32_t TB_PROBE_SUM  = 0x018;  // rw: sum of the values written to PROBE_WR; write sets it
constexpr uint32_t TB_CYCLE      = 0x01C;  // ro: the testbench cycle counter (low 32 bits)

enum ExitCode { EXIT_PASS = 0, EXIT_FAIL = 1, EXIT_TIMEOUT = 2, EXIT_ERROR = 3 };

struct Options {
    std::string elf, trace, signature, sig_begin, sig_end, vcd;
    uint64_t trace_tail = 0;        // 0: the whole trace
    uint64_t seed = 1;
    uint64_t max_cycles = 10000000;
    unsigned ilat_min = 1, ilat_max = 1, dlat_min = 1, dlat_max = 1;
    bool rand_reset = false;
    bool quiet = false;
};

void usage() {
    std::fprintf(stderr,
        "usage: surfcpu_tb --elf FILE [--trace FILE [--trace-tail N]] [--seed N] [--max-cycles N]\n"
        "                  [--ilat MIN:MAX] [--dlat MIN:MAX]\n"
        "                  [--signature FILE --sig-begin SYM --sig-end SYM]\n"
        "                  [--vcd FILE] [--rand-reset] [--quiet]\n");
}

bool parse_u64(const char *s, uint64_t &out) {
    char *end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0') return false;
    out = v;
    return true;
}

bool parse_range(const char *s, unsigned &lo, unsigned &hi) {
    unsigned a = 0, b = 0;
    char extra = 0;
    if (std::sscanf(s, "%u:%u%c", &a, &b, &extra) != 2) return false;
    if (a < 1 || b < a || b > 100000) return false;
    lo = a;
    hi = b;
    return true;
}

bool parse_args(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (!a.empty() && a[0] == '+') continue;      // +verilator+... runtime options
        auto need = [&](const char *what) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "surfcpu_tb: %s needs a value\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        const char *v = nullptr;
        if (a == "--elf") { if (!(v = need("--elf"))) return false; o.elf = v; }
        else if (a == "--trace") { if (!(v = need("--trace"))) return false; o.trace = v; }
        else if (a == "--trace-tail") {
            if (!(v = need("--trace-tail")) || !parse_u64(v, o.trace_tail) || o.trace_tail == 0 ||
                o.trace_tail > 10000000) {
                std::fprintf(stderr, "surfcpu_tb: bad --trace-tail (a record count, 1..10000000)\n");
                return false;
            }
        }
        else if (a == "--signature") { if (!(v = need("--signature"))) return false; o.signature = v; }
        else if (a == "--sig-begin") { if (!(v = need("--sig-begin"))) return false; o.sig_begin = v; }
        else if (a == "--sig-end") { if (!(v = need("--sig-end"))) return false; o.sig_end = v; }
        else if (a == "--vcd") { if (!(v = need("--vcd"))) return false; o.vcd = v; }
        else if (a == "--seed") {
            if (!(v = need("--seed")) || !parse_u64(v, o.seed)) { std::fprintf(stderr, "surfcpu_tb: bad --seed\n"); return false; }
        } else if (a == "--max-cycles") {
            if (!(v = need("--max-cycles")) || !parse_u64(v, o.max_cycles)) { std::fprintf(stderr, "surfcpu_tb: bad --max-cycles\n"); return false; }
        } else if (a == "--ilat") {
            if (!(v = need("--ilat")) || !parse_range(v, o.ilat_min, o.ilat_max)) { std::fprintf(stderr, "surfcpu_tb: bad --ilat (MIN:MAX, 1 <= MIN <= MAX)\n"); return false; }
        } else if (a == "--dlat") {
            if (!(v = need("--dlat")) || !parse_range(v, o.dlat_min, o.dlat_max)) { std::fprintf(stderr, "surfcpu_tb: bad --dlat (MIN:MAX, 1 <= MIN <= MAX)\n"); return false; }
        } else if (a == "--rand-reset") { o.rand_reset = true; }
        else if (a == "--quiet") { o.quiet = true; }
        else if (a == "--help" || a == "-h") { usage(); std::exit(EXIT_ERROR); }
        else { std::fprintf(stderr, "surfcpu_tb: unknown argument %s\n", a.c_str()); return false; }
    }
    if (o.elf.empty()) { std::fprintf(stderr, "surfcpu_tb: --elf is required\n"); return false; }
    if (!o.signature.empty() && (o.sig_begin.empty() || o.sig_end.empty())) {
        std::fprintf(stderr, "surfcpu_tb: --signature needs --sig-begin and --sig-end\n");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------- memory and ELF
std::vector<uint8_t> ram(RAM_SIZE, 0);

bool in_ram(uint32_t addr) { return addr >= RAM_BASE && addr - RAM_BASE < RAM_SIZE; }

uint32_t ram_read32(uint32_t addr) {
    uint32_t o = (addr - RAM_BASE) & ~3u;
    return uint32_t(ram[o]) | uint32_t(ram[o + 1]) << 8 | uint32_t(ram[o + 2]) << 16 | uint32_t(ram[o + 3]) << 24;
}

void ram_write32(uint32_t addr, uint32_t data, unsigned be) {
    uint32_t o = (addr - RAM_BASE) & ~3u;
    for (unsigned k = 0; k < 4; k++)
        if (be & (1u << k)) ram[o + k] = uint8_t(data >> (8 * k));
}

bool load_elf(const std::string &path, std::map<std::string, uint32_t> &syms, std::string &err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < sizeof(Elf32_Ehdr)) { err = "not an ELF file"; return false; }
    Elf32_Ehdr eh;
    std::memcpy(&eh, d.data(), sizeof eh);
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) { err = "not an ELF file"; return false; }
    if (eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_RISCV) {
        err = "not a 32-bit little-endian RISC-V ELF";
        return false;
    }
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        uint64_t off = uint64_t(eh.e_phoff) + uint64_t(i) * eh.e_phentsize;
        if (off + sizeof(Elf32_Phdr) > d.size()) { err = "truncated program headers"; return false; }
        Elf32_Phdr ph;
        std::memcpy(&ph, d.data() + off, sizeof ph);
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        if (uint64_t(ph.p_offset) + ph.p_filesz > d.size() || ph.p_filesz > ph.p_memsz) {
            err = "truncated segment";
            return false;
        }
        if (ph.p_paddr < RAM_BASE || uint64_t(ph.p_paddr) + ph.p_memsz > uint64_t(RAM_BASE) + RAM_SIZE) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "segment at 0x%08x (+0x%x) is outside the 4 MiB RAM at 0x80000000",
                          unsigned(ph.p_paddr), unsigned(ph.p_memsz));
            err = buf;
            return false;
        }
        std::memcpy(&ram[ph.p_paddr - RAM_BASE], d.data() + ph.p_offset, ph.p_filesz);
    }
    for (unsigned i = 0; i < eh.e_shnum; i++) {
        uint64_t off = uint64_t(eh.e_shoff) + uint64_t(i) * eh.e_shentsize;
        if (off + sizeof(Elf32_Shdr) > d.size()) break;
        Elf32_Shdr sh;
        std::memcpy(&sh, d.data() + off, sizeof sh);
        if (sh.sh_type != SHT_SYMTAB || sh.sh_link >= eh.e_shnum) continue;
        Elf32_Shdr st;
        std::memcpy(&st, d.data() + eh.e_shoff + uint64_t(sh.sh_link) * eh.e_shentsize, sizeof st);
        for (uint64_t s = 0; s + sizeof(Elf32_Sym) <= sh.sh_size; s += sizeof(Elf32_Sym)) {
            if (uint64_t(sh.sh_offset) + s + sizeof(Elf32_Sym) > d.size()) break;
            Elf32_Sym sym;
            std::memcpy(&sym, d.data() + sh.sh_offset + s, sizeof sym);
            uint64_t name = uint64_t(st.sh_offset) + sym.st_name;
            if (sym.st_name == 0 || name >= d.size()) continue;
            std::string n(reinterpret_cast<const char *>(d.data() + name),
                          strnlen(reinterpret_cast<const char *>(d.data() + name), d.size() - name));
            syms[n] = sym.st_value;
        }
    }
    return true;
}

// ------------------------------------------------------------------------- bus ports
struct Req {
    uint32_t addr = 0, wdata = 0;
    unsigned be = 0;
    bool write = false, fetch = false;
    bool operator==(const Req &o) const {
        return addr == o.addr && wdata == o.wdata && be == o.be && write == o.write && fetch == o.fetch;
    }
};

struct Rsp {
    bool valid = false, err = false;
    uint32_t rdata = 0;
};

struct Port {
    const char *name;
    unsigned lat_min, lat_max;
    std::mt19937_64 rng;
    bool waiting = false;     // a request was offered and not accepted last cycle
    Req offered;              // ... that request
    long ready_wait = -1;     // cycles left before req_ready for the offered request (-1: not drawn)
    bool busy = false;        // an accepted request has not had its response
    uint64_t due = 0;         // ... the cycle of that response
    Req pend;
    uint64_t requests = 0, wait_cycles = 0, errors = 0;

    Port(const char *n, unsigned lo, unsigned hi, uint64_t seed) : name(n), lat_min(lo), lat_max(hi), rng(seed) {}
    // half the draws take the minimum, the others are uniform in [lo, hi]
    unsigned draw(unsigned lo, unsigned hi) {
        if (lo == hi || (rng() & 1)) return lo;
        return lo + unsigned(rng() % (hi - lo + 1));
    }
};

// ------------------------------------------------------------------------- devices
struct Devices {
    uint64_t mtime = 0;
    uint64_t mtimecmp = ~0ull;
    uint32_t msip = 0;
    uint32_t irq_ext = 0;
    uint32_t irq_delay = 0;
    uint32_t irq_random = 0;
    uint32_t probe_rd = 0, probe_wr = 0, probe_sum = 0;
    std::mt19937_64 rng;
    explicit Devices(uint64_t seed) : rng(seed) {}
};

// ------------------------------------------------------------------------- the harness
class Harness {
  public:
    Harness(const Options &o, const std::map<std::string, uint32_t> &syms, uint32_t tohost)
        : opt(o), syms(syms), tohost(tohost),
          iport("ibus", o.ilat_min, o.ilat_max, o.seed * 0x9E3779B97F4A7C15ull + 1),
          dport("dbus", o.dlat_min, o.dlat_max, o.seed * 0xC2B2AE3D27D4EB4Full + 2),
          dev(o.seed * 0x165667B19E3779F9ull + 3) {}

    int run();

  private:
    const Options &opt;
    const std::map<std::string, uint32_t> &syms;
    uint32_t tohost;
    Port iport, dport;
    Devices dev;
    std::unique_ptr<VerilatedContext> ctx;
    std::unique_ptr<Vsurfcpu_tb_top> top;
#if VM_TRACE
    std::unique_ptr<VerilatedVcdC> vcd;
#endif
    FILE *trace = nullptr;
    std::vector<std::string> tail;  // --trace-tail: the last records, a ring buffer
    uint64_t tail_next = 0;         // records written into it so far
    uint64_t cycle = 0;
    uint64_t retired = 0;
    bool tohost_seen = false;
    uint32_t tohost_value = 0;
    uint64_t tohost_cycle = 0;
    bool protocol_error = false;

    void violation(const Port &p, const char *what) {
        std::fprintf(stderr, "surfcpu_tb: PROTOCOL ERROR on %s at cycle %" PRIu64 ": %s\n", p.name, cycle, what);
        protocol_error = true;
    }
    bool mmio(uint32_t addr, bool write, uint32_t wdata, uint32_t &rdata);
    void fetch_access(const Req &r, Rsp &out);
    void data_access(const Req &r, Rsp &out);
    bool decide_ready(Port &p, bool valid, const Req &r, bool in_reset, bool rsp_now, bool is_fetch);
    void after_edge(Port &p, bool valid, bool ready, const Req &r, bool rsp_now);
    void write_trace_line();
    void dump_signature();
    void dump(uint64_t t) {
#if VM_TRACE
        if (vcd) vcd->dump(t);
#else
        (void)t;
#endif
    }
};

bool Harness::mmio(uint32_t addr, bool write, uint32_t wdata, uint32_t &rdata) {
    rdata = 0;
    if (addr >= CLINT_BASE && addr - CLINT_BASE < CLINT_SIZE) {
        uint32_t off = addr - CLINT_BASE;
        switch (off) {
            case 0x0000: if (write) dev.msip = wdata & 1; else rdata = dev.msip; return true;
            case 0x4000: if (write) dev.mtimecmp = (dev.mtimecmp & ~0xFFFFFFFFull) | wdata; else rdata = uint32_t(dev.mtimecmp); return true;
            case 0x4004: if (write) dev.mtimecmp = (dev.mtimecmp & 0xFFFFFFFFull) | uint64_t(wdata) << 32; else rdata = uint32_t(dev.mtimecmp >> 32); return true;
            case 0xBFF8: if (write) dev.mtime = (dev.mtime & ~0xFFFFFFFFull) | wdata; else rdata = uint32_t(dev.mtime); return true;
            case 0xBFFC: if (write) dev.mtime = (dev.mtime & 0xFFFFFFFFull) | uint64_t(wdata) << 32; else rdata = uint32_t(dev.mtime >> 32); return true;
            default: return false;
        }
    }
    uint32_t off = addr - TB_BASE;
    switch (off) {
        case TB_CONSOLE:
            if (write) { std::fputc(int(wdata & 0xFF), stdout); std::fflush(stdout); }
            return true;
        case TB_IRQ_EXT: if (write) dev.irq_ext = wdata & 1; else rdata = dev.irq_ext; return true;
        case TB_IRQ_DELAY: if (write) dev.irq_delay = wdata; else rdata = dev.irq_delay; return true;
        case TB_IRQ_RANDOM: if (write) dev.irq_random = wdata; else rdata = dev.irq_random; return true;
        case TB_PROBE_RD: if (write) dev.probe_rd = wdata; else rdata = dev.probe_rd++; return true;
        case TB_PROBE_WR: if (write) dev.probe_wr++; else rdata = dev.probe_wr; return true;
        case TB_PROBE_SUM: if (write) dev.probe_sum = wdata; else rdata = dev.probe_sum; return true;
        case TB_CYCLE: if (!write) rdata = uint32_t(cycle); return true;
        default: return false;
    }
}

void Harness::fetch_access(const Req &r, Rsp &out) {
    if (in_ram(r.addr)) out.rdata = ram_read32(r.addr);
    else out.err = true;
    if (out.err) iport.errors++;
}

void Harness::data_access(const Req &r, Rsp &out) {
    uint32_t a = r.addr & ~3u;
    if (in_ram(a)) {
        if (r.write) {
            ram_write32(a, r.wdata, r.be);
            if (!tohost_seen && a == (tohost & ~3u)) {
                uint32_t v = ram_read32(a);
                if (v != 0) {
                    tohost_seen = true;
                    tohost_value = v;
                    tohost_cycle = cycle;
                }
            }
        } else {
            out.rdata = ram_read32(a);
        }
    } else if ((a >= CLINT_BASE && a - CLINT_BASE < CLINT_SIZE) || (a >= TB_BASE && a - TB_BASE < TB_SIZE)) {
        if (r.be != 0xF || !mmio(a, r.write, r.wdata, out.rdata)) out.err = true;
        if (r.write && a == TB_BASE + TB_PROBE_WR && !out.err) dev.probe_sum += r.wdata;
    } else {
        out.err = true;
    }
    if (out.err) dport.errors++;
}

static bool be_legal(uint32_t addr, unsigned be) {
    unsigned off = addr & 3u;
    if (be == (1u << off)) return true;                               // byte
    if ((off & 1u) == 0 && be == (3u << off)) return true;            // halfword
    return off == 0 && be == 0xFu;                                    // word
}

bool Harness::decide_ready(Port &p, bool valid, const Req &r, bool in_reset, bool rsp_now, bool is_fetch) {
    if (in_reset) {
        if (valid) violation(p, "req_valid during reset");
        p.waiting = false;
        p.ready_wait = -1;
        return (p.rng() & 1) != 0;
    }
    if (p.waiting) {
        if (!valid) violation(p, "a waiting request was withdrawn");
        else if (!(r == p.offered)) violation(p, "a waiting request changed before it was accepted");
    }
    if (!valid) return (p.rng() & 1) != 0;                    // must be ignored without req_valid
    if (p.busy && !rsp_now) violation(p, "a second request before the previous response");
    if (is_fetch) {
        if (r.write || r.be != 0xF || r.wdata != 0 || !r.fetch) violation(p, "fetch request fields not write=0 be=1111 wdata=0 fetch=1");
        if (r.addr & 3u) violation(p, "misaligned fetch address");
    } else {
        if (r.fetch) violation(p, "data request with req_fetch = 1");
        if (!be_legal(r.addr, r.be)) violation(p, "byte enables do not match the address");
    }
    if (p.ready_wait < 0) p.ready_wait = long(p.draw(0, p.lat_max - p.lat_min));
    return p.ready_wait == 0;
}

void Harness::after_edge(Port &p, bool valid, bool ready, const Req &r, bool rsp_now) {
    if (rsp_now) p.busy = false;
    if (valid && ready) {
        p.busy = true;
        p.due = cycle + p.draw(p.lat_min, p.lat_max);
        p.pend = r;
        p.ready_wait = -1;
        p.waiting = false;
        p.requests++;
    } else if (valid) {
        p.ready_wait--;
        p.waiting = true;
        p.offered = r;
        p.wait_cycles++;
    } else {
        p.waiting = false;
    }
}

void Harness::write_trace_line() {
    if (!trace) return;
    char line[160];
    std::snprintf(line, sizeof line, "%" PRIu64 " %08x %08x %x %x %x %08x %08x %x %x %08x %08x %08x\n",
                 uint64_t(top->rvfi_order), unsigned(top->rvfi_pc_rdata), unsigned(top->rvfi_insn),
                 unsigned(top->rvfi_trap), unsigned(top->rvfi_intr), unsigned(top->rvfi_rd_addr),
                 unsigned(top->rvfi_rd_wdata), unsigned(top->rvfi_mem_addr), unsigned(top->rvfi_mem_rmask),
                 unsigned(top->rvfi_mem_wmask), unsigned(top->rvfi_mem_rdata), unsigned(top->rvfi_mem_wdata),
                 unsigned(top->rvfi_pc_wdata));
    if (opt.trace_tail == 0) {
        std::fputs(line, trace);
    } else {
        tail[tail_next % opt.trace_tail] = line;
        tail_next++;
    }
}

void Harness::dump_signature() {
    if (opt.signature.empty()) return;
    uint32_t b = syms.at(opt.sig_begin), e = syms.at(opt.sig_end);
    FILE *f = std::fopen(opt.signature.c_str(), "w");
    if (!f) { std::fprintf(stderr, "surfcpu_tb: cannot write %s\n", opt.signature.c_str()); return; }
    for (uint32_t a = b & ~3u; a < e; a += 4) std::fprintf(f, "%08x\n", in_ram(a) ? ram_read32(a) : 0u);
    std::fclose(f);
}

int Harness::run() {
    ctx.reset(new VerilatedContext);
    if (opt.rand_reset) {
        ctx->randReset(2);                       // every register starts random
        ctx->randSeed(int(opt.seed & 0x7FFFFFFF));
    }
#if VM_TRACE
    if (!opt.vcd.empty()) ctx->traceEverOn(true);
#endif
    top.reset(new Vsurfcpu_tb_top{ctx.get()});
#if VM_TRACE
    if (!opt.vcd.empty()) {
        vcd.reset(new VerilatedVcdC);
        top->trace(vcd.get(), 99);
        vcd->open(opt.vcd.c_str());
    }
#else
    if (!opt.vcd.empty()) std::fprintf(stderr, "surfcpu_tb: built without VCD support; --vcd ignored\n");
#endif
    if (!opt.trace.empty()) {
        trace = std::fopen(opt.trace.c_str(), "w");
        if (!trace) { std::fprintf(stderr, "surfcpu_tb: cannot write %s\n", opt.trace.c_str()); return EXIT_ERROR; }
        std::fprintf(trace, "# surfcpu-trace v1: order pc insn trap intr rd rd_wdata mem_addr rmask wmask mem_rdata mem_wdata pc_next\n");
        if (opt.trace_tail) tail.assign(opt.trace_tail, std::string());
        std::fprintf(trace, "# elf %s seed %" PRIu64 " ilat %u:%u dlat %u:%u\n", opt.elf.c_str(), opt.seed,
                     opt.ilat_min, opt.ilat_max, opt.dlat_min, opt.dlat_max);
    }

    const uint64_t reset_cycles = 5;
    int code = -1;
    top->clk = 0;
    top->rst = 1;
    top->eval();

    for (cycle = 0;; cycle++) {
        const bool in_reset = cycle < reset_cycles;
        top->clk = 0;
        top->rst = in_reset;

        // responses due this cycle (the slave performs the access as it answers)
        Rsp ir, dr;
        if (!in_reset && iport.busy && iport.due == cycle) { ir.valid = true; fetch_access(iport.pend, ir); }
        if (!in_reset && dport.busy && dport.due == cycle) { dr.valid = true; data_access(dport.pend, dr); }
        top->ibus_rsp_valid = ir.valid;
        top->ibus_rsp_rdata = ir.valid ? ir.rdata : 0xDEADBEEFu;   // don't-care without rsp_valid
        top->ibus_rsp_err = ir.valid && ir.err;
        top->dbus_rsp_valid = dr.valid;
        top->dbus_rsp_rdata = dr.valid ? dr.rdata : 0xDEADBEEFu;
        top->dbus_rsp_err = dr.valid && dr.err;
        top->irq_timer = !in_reset && dev.mtime >= dev.mtimecmp;
        top->irq_software = !in_reset && (dev.msip & 1);
        top->irq_external = !in_reset && (dev.irq_ext & 1);
        top->mtime = dev.mtime;
        top->eval();

        // requests (they may depend on the responses above, never on req_ready)
        Req ireq, dreq;
        bool ivalid = top->ibus_req_valid, dvalid = top->dbus_req_valid;
        ireq.addr = top->ibus_req_addr; ireq.wdata = top->ibus_req_wdata; ireq.be = top->ibus_req_be;
        ireq.write = top->ibus_req_write; ireq.fetch = top->ibus_req_fetch;
        dreq.addr = top->dbus_req_addr; dreq.wdata = top->dbus_req_wdata; dreq.be = top->dbus_req_be;
        dreq.write = top->dbus_req_write; dreq.fetch = top->dbus_req_fetch;
        bool iready = decide_ready(iport, ivalid, ireq, in_reset, ir.valid, true);
        bool dready = decide_ready(dport, dvalid, dreq, in_reset, dr.valid, false);
        top->ibus_req_ready = iready;
        top->dbus_req_ready = dready;
        top->eval();
        if (top->ibus_req_valid != ivalid || top->dbus_req_valid != dvalid) {
            violation(top->ibus_req_valid != ivalid ? iport : dport, "req_valid depends on req_ready");
        }
        dump(2 * cycle);

        // retirement
        bool finished = false;
        if (!in_reset && top->rvfi_valid) {
            write_trace_line();
            retired++;
            if (tohost_seen && top->rvfi_mem_wmask != 0 && uint32_t(top->rvfi_mem_addr) == (tohost & ~3u))
                finished = true;
        }

        top->clk = 1;
        top->eval();
        dump(2 * cycle + 1);

        after_edge(iport, ivalid, iready, ireq, ir.valid);
        after_edge(dport, dvalid, dready, dreq, dr.valid);

        // devices: the timer counts cycles; the delayed and random external interrupts
        if (!in_reset) {
            dev.mtime++;
            if (dev.irq_delay > 0 && --dev.irq_delay == 0) dev.irq_ext = 1;
            if (dev.irq_random > 0 && dev.rng() % dev.irq_random == 0) dev.irq_ext = 1;
        }

        if (protocol_error) { code = EXIT_ERROR; break; }
        if (finished) { code = tohost_value == 1 ? EXIT_PASS : EXIT_FAIL; break; }
        if (tohost_seen && cycle > tohost_cycle + 64) {
            std::fprintf(stderr, "surfcpu_tb: tohost was written but its store never retired\n");
            code = EXIT_ERROR;
            break;
        }
        if (cycle + 1 >= opt.max_cycles) { code = EXIT_TIMEOUT; break; }
    }

    top->final();
#if VM_TRACE
    if (vcd) vcd->close();
#endif
    if (trace) {
        if (opt.trace_tail) {           // the kept records, oldest first
            if (tail_next > opt.trace_tail)
                std::fprintf(trace, "# --trace-tail %" PRIu64 ": the first %" PRIu64 " records are not shown\n",
                             opt.trace_tail, tail_next - opt.trace_tail);
            uint64_t first = tail_next > opt.trace_tail ? tail_next - opt.trace_tail : 0;
            for (uint64_t i = first; i < tail_next; i++) std::fputs(tail[i % opt.trace_tail].c_str(), trace);
        }
        std::fclose(trace);
    }
    dump_signature();

    if (!opt.quiet || code != EXIT_PASS) {
        const char *what = code == EXIT_PASS ? "PASS" : code == EXIT_FAIL ? "FAIL" : code == EXIT_TIMEOUT ? "TIMEOUT" : "ERROR";
        std::fprintf(stderr, "surfcpu_tb: %s", what);
        if (code == EXIT_FAIL) std::fprintf(stderr, " (test %u, tohost 0x%08x)", tohost_value >> 1, tohost_value);
        std::fprintf(stderr, " after %" PRIu64 " cycles, %" PRIu64 " retired; seed %" PRIu64
                     " ilat %u:%u dlat %u:%u; ibus %" PRIu64 " req %" PRIu64 " wait %" PRIu64
                     " err, dbus %" PRIu64 " req %" PRIu64 " wait %" PRIu64 " err; probes rd %u wr %u\n",
                     cycle + 1, retired, opt.seed, opt.ilat_min, opt.ilat_max, opt.dlat_min, opt.dlat_max,
                     iport.requests, iport.wait_cycles, iport.errors, dport.requests, dport.wait_cycles,
                     dport.errors, dev.probe_rd, dev.probe_wr);
    }
    return code;
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        usage();
        return EXIT_ERROR;
    }
    std::map<std::string, uint32_t> syms;
    std::string err;
    if (!load_elf(opt.elf, syms, err)) {
        std::fprintf(stderr, "surfcpu_tb: %s: %s\n", opt.elf.c_str(), err.c_str());
        return EXIT_ERROR;
    }
    auto it = syms.find("tohost");
    if (it == syms.end()) {
        std::fprintf(stderr, "surfcpu_tb: %s has no tohost symbol\n", opt.elf.c_str());
        return EXIT_ERROR;
    }
    if (!in_ram(it->second)) {
        std::fprintf(stderr, "surfcpu_tb: tohost (0x%08x) is outside RAM\n", unsigned(it->second));
        return EXIT_ERROR;
    }
    if (!opt.signature.empty()) {
        for (const std::string &s : {opt.sig_begin, opt.sig_end}) {
            if (!syms.count(s)) {
                std::fprintf(stderr, "surfcpu_tb: %s has no symbol %s\n", opt.elf.c_str(), s.c_str());
                return EXIT_ERROR;
            }
        }
    }
    Harness h(opt, syms, it->second);
    return h.run();
}
