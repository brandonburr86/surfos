/*
SurfCPU diagnostics: the milestone 3 BRAM program
--------------------
File: bramdiag.c Date: 10/16/26 (SurfCPU)
--------------------
A boot ROM image (build/fw/bramdiag.mem, in place of SurfBoot) that needs nothing but the
CPU, block RAM, the UART and the timer, for the first runs of SurfCPU on the board
(brief section 10, milestone 3). It prints the build identity, then checks: the identity
registers; arithmetic, the M extension, branches, loads and stores, CSRs and counters;
scratch RAM (data and address lines, byte lanes, March C-) and the spare text RAM; every
exception kind, each caught and recovered from (illegal instructions, misaligned and
faulting loads, stores and fetches, misaligned jump and branch targets, ECALL, EBREAK);
the software, timer and external interrupts with their priority, MIE gating and WFI, the
timer ticking at the right rate; FENCE.I with code written into scratch RAM; and, when the
bitstream has a DDR controller, its initialization when the controller waits for software
(sysinfo.DDR_STATUS.SWINIT: LiteDRAM without its own CPU; common/ddrinit.c, with every lane's
scan of bitslips and delays printed), its calibration, and the DDR tests including code
copied from the ROM into DDR and run there (milestone 4). It ends with "BRAMDIAG: PASS (n checks)" or
"BRAMDIAG: FAIL (...)" and the LEDs (LD0 green or red), then echoes UART input; Ctrl-R runs
everything again. Its data and stack use the top 8 KiB of scratch RAM; the 56 KiB below are
tested.
*/
#include <string.h>
#include "crc32.h"
#include "ddrinit.h"
#include "diag.h"
#include "version.h"

extern char __diag_scratch_lo[], __diag_scratch_hi[];
#define SCRATCH_LO ((uint32_t)(uintptr_t)__diag_scratch_lo)
#define SCRATCH_HI ((uint32_t)(uintptr_t)__diag_scratch_hi)
#define W(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* ---- the trap hook: expected exceptions are recorded and stepped over ---- */
static volatile int expecting, resume_at_ra, irq_mode;
static volatile uint32_t got_count, got_cause, got_tval, got_epc;
static volatile uint32_t irq_order[8], irq_n, ticks, next_seen;

enum { IRQ_NONE, IRQ_SOFT, IRQ_TICKS, IRQ_EXT, IRQ_PRIO };

static int diag_hook(struct trap_frame *f)
{
    if (f->mcause & MCAUSE_INT) {
        uint32_t c = f->mcause & 0x1Fu;
        if (irq_mode == IRQ_NONE)
            return 0;
        if (irq_n < 8)
            irq_order[irq_n] = c;
        irq_n++;
        switch (c) {
        case 3:
            REG(SURF_TIMER_BASE, SURF_TIMER_MSIP) = 0;
            return 1;
        case 7:
            ticks++;
            if (irq_mode == IRQ_TICKS && ticks < 10)
                timer_set_cmp(timer_now() + 1000u);
            else
                timer_cmp_off();
            return 1;
        case 11:
            next_seen = REG(SURF_INTC_BASE, SURF_INTC_NEXT);
            REG(SURF_UART_BASE, SURF_UART_CTRL) = 0;
            return 1;
        default:
            return 0;
        }
    }
    if (!expecting)
        return 0;
    got_count++;
    got_cause = f->mcause;
    got_tval = f->mtval;
    got_epc = f->mepc;
    expecting = 0;
    f->mepc = resume_at_ra ? f->x[1] : f->mepc + 4u;
    return 1;
}

static void expect_trap(int ra)
{
    got_count = 0;
    got_cause = got_tval = got_epc = 0xDEADDEADu;
    resume_at_ra = ra;
    expecting = 1;
}

static void trapped(const char *what, uint32_t cause, uint32_t tval, uint32_t epc)
{
    expecting = 0;
    diag_check(got_count == 1 && got_cause == cause && got_tval == tval && got_epc == epc,
               "%s: %u trap(s), mcause 0x%08x mtval 0x%08x mepc 0x%08x; expected mcause 0x%08x mtval 0x%08x mepc "
               "0x%08x", what, got_count, got_cause, got_tval, got_epc, cause, tval, epc);
}

static void no_trap(const char *what)
{
    expecting = 0;
    diag_check(got_count == 0, "%s trapped (mcause 0x%08x)", what, got_cause);
}

#define A(p) ((uint32_t)(uintptr_t)(p))

/* ---- sections ---- */

static void identity(uint32_t unused)
{
    (void)unused;
    uint32_t board = sysinfo(SURF_SYSINFO_BOARD_ID), soc = sysinfo(SURF_SYSINFO_SOC_VERSION);
    con_printf("board %s (0x%x), SoC %u.%u.%u, memory map %u, sys %u Hz, timebase %u Hz\n", fw_board_name(board),
               board, soc >> 16, (soc >> 8) & 0xFFu, soc & 0xFFu, sysinfo(SURF_SYSINFO_MAP_VERSION),
               sysinfo(SURF_SYSINFO_SYS_HZ), REG(SURF_TIMER_BASE, SURF_TIMER_TIMEBASE));
    con_printf("bitstream build %08x flags %x, features %02x, DDR status %x size %u MiB\n",
               sysinfo(SURF_SYSINFO_BUILD_ID), sysinfo(SURF_SYSINFO_BUILD_FLAGS), sysinfo(SURF_SYSINFO_FEATURES),
               sysinfo(SURF_SYSINFO_DDR_STATUS), sysinfo(SURF_SYSINFO_DDR_SIZE) >> 20);
    diag_section("identity");
    diag_check(sysinfo(SURF_SYSINFO_MAGIC) == 0x46525553u, "sysinfo.MAGIC is 0x%08x", sysinfo(SURF_SYSINFO_MAGIC));
    diag_check(sysinfo(SURF_SYSINFO_MAP_VERSION) == SURF_MAP_VERSION, "memory map %u, this program expects %u",
               sysinfo(SURF_SYSINFO_MAP_VERSION), SURF_MAP_VERSION);
    diag_check(soc == csr_read(mimpid), "mimpid 0x%08x and sysinfo.SOC_VERSION 0x%08x differ", csr_read(mimpid), soc);
    diag_check(sysinfo(SURF_SYSINFO_SYS_HZ) != 0, "sysinfo.SYS_HZ is 0");
    diag_check(REG(SURF_TIMER_BASE, SURF_TIMER_TIMEBASE) == SURF_TIMEBASE_HZ, "timer.TIMEBASE is %u",
               REG(SURF_TIMER_BASE, SURF_TIMER_TIMEBASE));
    diag_check(sysinfo(SURF_SYSINFO_FEATURES) & SURF_SYSINFO_FEATURES_UART_MASK, "FEATURES lacks the UART");
    REG(SURF_SYSINFO_BASE, SURF_SYSINFO_SCRATCH) = 0x5A5AA5A5u;
    diag_check(sysinfo(SURF_SYSINFO_SCRATCH) == 0x5A5AA5A5u, "sysinfo.SCRATCH does not hold a value");
    diag_section_end();
}

static void cpu_sections(uint32_t step)
{
    switch (step) {
    case 0: cpu_test_alu(); break;
    case 1: cpu_test_muldiv(); break;
    case 2: cpu_test_branches(); break;
    case 3: cpu_test_loadstore(SCRATCH_LO); break;
    default: cpu_test_csrs(); break;
    }
}

static void scratch_ram(uint32_t unused)
{
    (void)unused;
    uint32_t lo = SCRATCH_LO, hi = SCRATCH_HI;
    diag_section("scratch RAM (March C-)");
    for (int b = 0; b < 32; b++) {
        W(lo) = 1u << b;
        if (!diag_check(W(lo) == 1u << b, "walking one at 0x%08x bit %d reads 0x%08x", lo, b, W(lo)))
            break;
    }
    for (uint32_t k = 4; lo + k < hi; k <<= 1)
        W(lo + k) = k * 0x9E3779B1u;
    W(lo) = 0x13579BDFu;
    for (uint32_t k = 4; lo + k < hi; k <<= 1)
        diag_check(W(lo + k) == k * 0x9E3779B1u, "address line test: 0x%08x reads 0x%08x", lo + k, W(lo + k));
    diag_check(W(lo) == 0x13579BDFu, "address line test: the base word changed");
    uint32_t bad = 0;
    for (uint32_t a = lo; a < hi; a += 4) W(a) = 0;                                       /* up w0 */
    for (uint32_t a = lo; a < hi; a += 4) { bad += W(a) != 0; W(a) = ~0u; }               /* up r0 w1 */
    for (uint32_t a = lo; a < hi; a += 4) { bad += W(a) != ~0u; W(a) = 0; }               /* up r1 w0 */
    for (uint32_t a = hi - 4; a >= lo; a -= 4) { bad += W(a) != 0; W(a) = ~0u; }          /* down r0 w1 */
    for (uint32_t a = hi - 4; a >= lo; a -= 4) { bad += W(a) != ~0u; W(a) = 0; }          /* down r1 w0 */
    for (uint32_t a = hi - 4; a >= lo; a -= 4) bad += W(a) != 0;                          /* down r0 */
    diag_check(bad == 0, "March C-: %u wrong reads in [0x%08x, 0x%08x)", bad, lo, hi);
    for (uint32_t a = lo; a < hi; a += 4) W(a) = a ^ 0xA5A5A5A5u;
    bad = 0;
    for (uint32_t a = lo; a < hi; a += 4) bad += W(a) != (a ^ 0xA5A5A5A5u);
    diag_check(bad == 0, "address-in-address: %u wrong words", bad);
    con_printf("(%u KiB) ", (hi - lo) >> 10);
    diag_section_end();

    diag_section("text RAM (cells 2400-4095)");
    bad = 0;
    for (uint32_t c = 2400; c < 4096; c++) W(SURF_TEXTRAM_BASE + 4 * c) = c * 0x01000193u;
    for (uint32_t c = 2400; c < 4096; c++) bad += W(SURF_TEXTRAM_BASE + 4 * c) != c * 0x01000193u;
    diag_check(bad == 0, "%u text RAM cells wrong", bad);
    diag_section_end();
}

static void exceptions(uint32_t unused)
{
    (void)unused;
    uint32_t ram = SCRATCH_LO, r;
    diag_section("exceptions, each recovered");
    expect_trap(0);
    r = probe_illegal_zero(0x5555);
    trapped("illegal 0x00000000", 2, 0, A(probe_illegal_zero_at));
    diag_check(r == 0x5555, "registers changed across the trap");
    expect_trap(0);
    probe_illegal_ones(0);
    trapped("illegal 0xffffffff", 2, 0xFFFFFFFFu, A(probe_illegal_ones));
    expect_trap(0);
    probe_csr_write_ro(0);
    trapped("write to read-only mhartid", 2, 0xF1401073u, A(probe_csr_write_ro));
    expect_trap(0);
    r = probe_csr_unknown(0x77);
    trapped("unknown CSR (satp)", 2, 0x18002573u, A(probe_csr_unknown));
    diag_check(r == 0x77, "csrr of an unknown CSR wrote its destination");
    expect_trap(0);
    r = probe_slli_shamt5(0x88);
    trapped("SLLI with imm[5] = 1", 2, 0x02051513u, A(probe_slli_shamt5));
    diag_check(r == 0x88, "the illegal SLLI wrote its destination");
    expect_trap(0);
    probe_sret(0);
    trapped("SRET", 2, 0x10200073u, A(probe_sret));
    expect_trap(0);
    probe_ecall();
    trapped("ECALL", 11, 0, A(probe_ecall));
    expect_trap(0);
    probe_ebreak();
    trapped("EBREAK", 3, A(probe_ebreak_at), A(probe_ebreak_at));

    W(ram) = 0x11223344u;
    W(ram + 4) = 0x55667788u;
    expect_trap(0);
    r = probe_load_word(ram + 1, 0x99);
    trapped("misaligned LW", 4, ram + 1, A(probe_load_word));
    diag_check(r == 0x99, "a misaligned load wrote its destination");
    expect_trap(0);
    probe_load_word(ram + 2, 0);
    trapped("LW at +2", 4, ram + 2, A(probe_load_word));
    expect_trap(0);
    probe_load_half(ram + 3, 0);
    trapped("misaligned LH", 4, ram + 3, A(probe_load_half));
    expect_trap(0);
    r = probe_load_half(ram + 2, 0);
    no_trap("aligned LH");
    diag_check(r == 0x1122u, "LH at +2 read 0x%08x", r);
    expect_trap(0);
    probe_store_word(ram + 2, 0xFFFFFFFFu);
    trapped("misaligned SW", 6, ram + 2, A(probe_store_word));
    expect_trap(0);
    probe_store_half(ram + 1, 0xFFFFu);
    trapped("misaligned SH", 6, ram + 1, A(probe_store_half));
    diag_check(W(ram) == 0x11223344u && W(ram + 4) == 0x55667788u, "a trapping store changed memory");

    expect_trap(0);
    r = probe_load_word(0x30000000u, 0xAB);
    trapped("load from an unmapped address", 5, 0x30000000u, A(probe_load_word));
    diag_check(r == 0xAB, "a faulting load wrote its destination");
    expect_trap(0);
    probe_load_word(SURF_UART_BASE + 0xFF0u, 0);
    trapped("load from a hole in an MMIO window", 5, SURF_UART_BASE + 0xFF0u, A(probe_load_word));
    expect_trap(0);
    probe_load_byte(SURF_UART_BASE + SURF_UART_STATUS, 0);
    trapped("byte load from an MMIO register", 5, SURF_UART_BASE + SURF_UART_STATUS, A(probe_load_byte));
    expect_trap(0);
    probe_load_word(SURF_ETH_BASE, 0);
    trapped("load from the reserved eth window", 5, SURF_ETH_BASE, A(probe_load_word));
    uint32_t rom_word = W(0x100);
    expect_trap(0);
    probe_store_word(0x100, ~rom_word);
    trapped("store to the boot ROM", 7, 0x100, A(probe_store_word));
    diag_check(W(0x100) == rom_word, "a store changed the boot ROM");
    expect_trap(0);
    probe_store_half(SURF_GPIO_BASE + SURF_GPIO_LED, 0xF);
    trapped("halfword store to an MMIO register", 7, SURF_GPIO_BASE + SURF_GPIO_LED, A(probe_store_half));

    expect_trap(1);
    probe_jump(0x30000000u);
    trapped("fetch from an unmapped address", 1, 0x30000000u, 0x30000000u);
    expect_trap(1);
    probe_jump(SURF_UART_BASE);
    trapped("fetch from MMIO", 1, SURF_UART_BASE, SURF_UART_BASE);
    expect_trap(1);
    probe_jump(SURF_TEXTRAM_BASE);
    trapped("fetch from text RAM", 1, SURF_TEXTRAM_BASE, SURF_TEXTRAM_BASE);
    expect_trap(0);
    r = probe_misaligned_jump(0xABC);
    trapped("JALR to a 2-aligned target", 0, A(probe_misaligned_target) + 2u, A(probe_misaligned_jump_at));
    diag_check(r == 0xABC, "the misaligned JALR wrote its link register");
    expect_trap(0);
    probe_misaligned_branch();
    trapped("branch to a 2-aligned target", 0, A(probe_misaligned_branch_at) + 6u, A(probe_misaligned_branch_at));
    diag_section_end();
}

static void interrupts(uint32_t unused)
{
    (void)unused;
    diag_section("interrupts, timer and WFI");
    /* the timer runs at TIMEBASE against sys_clk */
    uint32_t c0 = csr_read(mcycle), t0 = timer_now32();
    while ((uint32_t)(timer_now32() - t0) < 10000u)
        ;
    uint32_t cyc = csr_read(mcycle) - c0, per_us = cyc / 10000u, want = sysinfo(SURF_SYSINFO_SYS_HZ) / 1000000u;
    diag_check(per_us + 1u >= want && per_us <= want + 1u + want / 10u, "%u cycles per timer microsecond, expected %u",
               per_us, want);

    irq_mode = IRQ_SOFT;
    irq_n = 0;
    csr_set(mie, MIE_MSIE);
    REG(SURF_TIMER_BASE, SURF_TIMER_MSIP) = 1;
    csr_set(mstatus, MSTATUS_MIE);
    __asm__ volatile ("nop\n\tnop");
    csr_clear(mstatus, MSTATUS_MIE);
    csr_clear(mie, MIE_MSIE);
    diag_check(irq_n == 1 && irq_order[0] == 3, "software interrupt: %u taken, first cause %u", irq_n, irq_order[0]);

    irq_mode = IRQ_TICKS;
    irq_n = ticks = 0;
    uint64_t start = timer_now();
    timer_set_cmp(start + 1000u);
    csr_set(mie, MIE_MTIE);
    csr_set(mstatus, MSTATUS_MIE);
    while (ticks < 10 && timer_now() - start < 100000u)
        fw_wfi();
    csr_clear(mstatus, MSTATUS_MIE);
    csr_clear(mie, MIE_MTIE);
    timer_cmp_off();
    uint32_t elapsed = (uint32_t)(timer_now() - start);
    diag_check(ticks == 10 && elapsed >= 9500u && elapsed <= 15000u, "timer: %u ticks of 1 ms in %u us", ticks, elapsed);
    con_printf("(10 timer ticks in %u us) ", elapsed);

    irq_mode = IRQ_EXT;
    irq_n = 0;
    next_seen = 0xFFu;
    uart_drain(100000);
    REG(SURF_INTC_BASE, SURF_INTC_ENABLE) = 1u << SURF_IRQ_UART;
    REG(SURF_UART_BASE, SURF_UART_CTRL) = SURF_UART_CTRL_TX_IE_MASK;      /* TX FIFO empty: pending now */
    csr_set(mie, MIE_MEIE);
    csr_set(mstatus, MSTATUS_MIE);
    __asm__ volatile ("nop\n\tnop");
    csr_clear(mstatus, MSTATUS_MIE);
    csr_clear(mie, MIE_MEIE);
    REG(SURF_INTC_BASE, SURF_INTC_ENABLE) = 0;
    diag_check(irq_n == 1 && irq_order[0] == 11 && next_seen == SURF_IRQ_UART,
               "external interrupt: %u taken, cause %u, intc.NEXT %u", irq_n, irq_order[0], next_seen);

    irq_mode = IRQ_PRIO;
    irq_n = 0;
    uart_drain(100000);
    REG(SURF_TIMER_BASE, SURF_TIMER_MSIP) = 1;
    timer_set_cmp(0);
    REG(SURF_INTC_BASE, SURF_INTC_ENABLE) = 1u << SURF_IRQ_UART;
    REG(SURF_UART_BASE, SURF_UART_CTRL) = SURF_UART_CTRL_TX_IE_MASK;
    csr_set(mie, MIE_MSIE | MIE_MTIE | MIE_MEIE);
    uint32_t pend = csr_read(mip) & 0x888u;
    fw_wfi();                                       /* pending and enabled: WFI completes at once */
    diag_check(irq_n == 0, "an interrupt was taken with mstatus.MIE = 0");
    diag_check(pend == 0x888u, "mip 0x%03x: MSIP, MTIP and MEIP should all be pending", pend);
    csr_set(mstatus, MSTATUS_MIE);
    __asm__ volatile ("nop\n\tnop\n\tnop\n\tnop");
    csr_clear(mstatus, MSTATUS_MIE);
    csr_write(mie, 0);
    REG(SURF_INTC_BASE, SURF_INTC_ENABLE) = 0;
    REG(SURF_UART_BASE, SURF_UART_CTRL) = 0;
    REG(SURF_TIMER_BASE, SURF_TIMER_MSIP) = 0;
    timer_cmp_off();
    diag_check(irq_n == 3 && irq_order[0] == 11 && irq_order[1] == 3 && irq_order[2] == 7,
               "priority: %u taken, order %u %u %u (expected 11 3 7)", irq_n, irq_order[0], irq_order[1], irq_order[2]);
    irq_mode = IRQ_NONE;
    diag_section_end();
}

typedef uint32_t (*probe_fn)(uint32_t);

static void fence_i(uint32_t unused)
{
    (void)unused;
    diag_section("FENCE.I, code written to RAM");
    uint32_t at = SCRATCH_LO + 256u;
    uint32_t n = A(ddr_probe_end) - A(ddr_probe_start), imm = A(ddr_probe_imm) - A(ddr_probe_start);
    memcpy((void *)(uintptr_t)at, ddr_probe_start, n);
    fw_fence_i();
    uint32_t r = ((probe_fn)(uintptr_t)at)(7);
    diag_check(r == 7u * 3u + 0x234u, "code copied to scratch RAM returned 0x%08x", r);
    W(at + imm) = (W(at + imm) & 0x000FFFFFu) | (0x111u << 20);
    fw_fence_i();
    r = ((probe_fn)(uintptr_t)at)(7);
    diag_check(r == 7u * 3u + 0x111u, "after patching and FENCE.I it returned 0x%08x", r);
    diag_section_end();
}

static uint32_t ddrc_read(uint32_t off) { return REG(SURF_DDRC_BASE, off); }
static void ddrc_write(uint32_t off, uint32_t value) { REG(SURF_DDRC_BASE, off) = value; }

static const struct ddrinit_ops diag_ddrc = { ddrc_read, ddrc_write, timer_now32, con_puts, 1 };

/* SWINIT: initialize the controller and level the read path, printing every lane's scan
 * (one line per bitslip: a 1 for each of the 32 delays that read the pattern back). */
static int ddr_software_init(void)
{
    static struct ddrinit_result r;
    con_puts("DDR initialization (DDR_STATUS.SWINIT), read leveling:\n");
    ddrinit_run(&diag_ddrc, &r);
    for (int m = 0; m < DDRINIT_MODULES; m++) {
        char d[64];
        ddrinit_describe(&r, m, d, sizeof d);
        con_printf("  %s\n", d);
    }
    con_printf("  %u ms\n", r.us / 1000u);
    diag_section("DDR initialization");
    for (int m = 0; r.leveling && m < DDRINIT_MODULES; m++)
        diag_check(r.lane[m].window >= 0, "lane %d: no read delay works at bitslip %d, the best", m,
                   r.lane[m].bitslip);
    diag_check(r.ok, "read leveling did not succeed: init_error written");
    diag_section_end();
    return r.ok;
}

static void ddr(uint32_t unused)
{
    (void)unused;
    uint32_t st = sysinfo(SURF_SYSINFO_DDR_STATUS);
    if (!(st & SURF_SYSINFO_DDR_STATUS_PRESENT_MASK)) {
        con_puts("DDR                                skipped: no DDR controller in this bitstream\n");
        return;
    }
    if ((st & SURF_SYSINFO_DDR_STATUS_SWINIT_MASK) && !ddr_software_init())
        return;
    diag_section("DDR calibration");
    int ok = ddr_wait_calibration(2000000u);
    diag_check(ok, "calibration did not succeed within 2 s (DDR_STATUS 0x%08x)", sysinfo(SURF_SYSINFO_DDR_STATUS));
    diag_section_end();
    if (!ok)
        return;
    uint32_t size = sysinfo(SURF_SYSINFO_DDR_SIZE);
    struct ddr_area a = { SURF_DDR_BASE, size, SURF_DDR_BASE, SURF_DDR_BASE + size };
    ddr_test_data_lines(&a);
    ddr_test_byte_lanes(&a);
    ddr_test_address_lines(&a);
    ddr_test_boundaries(&a);
    ddr_test_patterns(&a, 1u << 20, 1);
    ddr_test_copies(&a, 256u << 10, 4);
    ddr_test_exec(&a);
}

static void echo(void)
{
    con_puts("\nUART echo: type; each character comes back with its code. Ctrl-R runs the diagnostics again.\n");
    uint32_t count = 0, beat = timer_now32();
    for (;;) {
        if ((uint32_t)(timer_now32() - beat) >= 500000u) {      /* heartbeat on LD7 */
            beat = timer_now32();
            REG(SURF_GPIO_BASE, SURF_GPIO_LED) ^= 0x8u;
        }
        int c = con_getc();
        if (c < 0)
            continue;
        if (c == 0x12)
            return;
        count++;
        con_printf("echo %3u: '%c' 0x%02x\n", count, (c >= 0x20 && c < 0x7F) ? c : '.', c);
    }
}

int main(void)
{
    video_init();
    fw_trap_hook = diag_hook;
    for (;;) {
        led_set(0x1, 0x004);
        diag_checks = diag_failures = 0;
        con_printf("\nSurfCPU BRAM diagnostics %s (build %08x%s)\n", DIAG_VERSION, (unsigned)FW_BUILD_ID,
                   FW_BUILD_DIRTY ? ", dirty tree" : "");
        diag_guard(identity, 0);
        for (uint32_t i = 0; i < 5; i++)
            diag_guard(cpu_sections, i);
        led_set(0x3, 0x004);
        diag_guard(scratch_ram, 0);
        diag_guard(exceptions, 0);
        led_set(0x7, 0x004);
        diag_guard(interrupts, 0);
        diag_guard(fence_i, 0);
        diag_guard(ddr, 0);
        diag_summary("BRAMDIAG");
        echo();
    }
}
