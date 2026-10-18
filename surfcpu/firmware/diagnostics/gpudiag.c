/*
SurfGPU diagnostics: the GPU diagnostic
--------------------
File: gpudiag.c Date: 10/18/26 (SurfGPU)
--------------------
A boot ROM image (build/fw/gpudiag.mem) for the GPU designs (sysinfo.FEATURES.GPU; docs/GPU.md
section 10), like bramdiag: it needs only the CPU, block RAM, the UART, the timer, VRAM and the
video block. It prints the build identity, then checks the VRAM window (walking ones, address
lines, byte lanes, March C- and address-in-address over all 300 KiB, the error at 300 KiB for
loads and stores, no fetch), the new video registers and GPAL, the engine's registers and errors
(a full FIFO and OVERFLOW, an unknown opcode, a refused target, a fence and its interrupt at intc
source gpu), then draws gpudiag's scenes
(gpuscenes.c) one after the other. For each it waits until a frame that began after the
picture was complete has been shown (FRAME_COUNT two on), and compares FRAME_CRC with the CRC
the C model computed for that scene when the ROM was built (gpuscenes_expect.h), printing both.
A design without the engine (no FEATURES.GPU, a gpu window that traps or a gpu.ID other than
"GPU1") fails: every check that could not run counts as failed, and a run that made fewer checks
than a complete one (GPUDIAG_CHECKS) fails too, so PASS means every check ran and passed. The
GFX_ values a reset leaves can only be seen by the first run after a reset: a rerun (Ctrl-R) says
so in a line under its banner and is held to GPUDIAG_RERUN_CHECKS, one fewer.
It ends with "GPUDIAG: PASS (n checks)" or "GPUDIAG: FAIL (...)", the LEDs (LD0 green or red),
the result in the top text row over the last picture, then echoes UART input; Ctrl-R runs it
all again. The text console is not used (the scenes own the screen): everything goes to the UART.
*/
#include "diag.h"
#include "gpuscenes.h"
#include "gpuscenes_expect.h"
#include "surfcpu/gpu.h"
#include "version.h"                    /* the build identity (fw_build.h) */

#define W(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define H(addr) (*(volatile uint16_t *)(uintptr_t)(addr))
#define B8(addr) (*(volatile uint8_t *)(uintptr_t)(addr))
#define VRAM(off) (SURF_VRAM_BASE + (off))
#define VREG(off) REG(SURF_VIDEO_BASE, off)

/* ---- expected traps: recorded and stepped over (bramdiag's way) ---- */
static volatile int expecting, resume_at_ra;
static volatile uint32_t got_count, got_cause, got_tval, got_epc;

static int gpudiag_hook(struct trap_frame *f)
{
    if ((f->mcause & MCAUSE_INT) || !expecting)
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

static void trapped(const char *what, uint32_t cause, uint32_t tval)
{
    expecting = 0;
    diag_check(got_count == 1 && got_cause == cause && got_tval == tval,
               "%s: %u trap(s), mcause 0x%08x mtval 0x%08x; expected mcause 0x%08x mtval 0x%08x", what, got_count,
               got_cause, got_tval, cause, tval);
}

static int first_run = 1;

/* ---- sections ---- */

static void identity(uint32_t unused)
{
    (void)unused;
    uint32_t board = sysinfo(SURF_SYSINFO_BOARD_ID), soc = sysinfo(SURF_SYSINFO_SOC_VERSION);
    con_printf("board %s (0x%x), SoC %u.%u.%u, memory map %u, sys %u Hz, pixel clock %u Hz\n", fw_board_name(board),
               board, soc >> 16, (soc >> 8) & 0xFFu, soc & 0xFFu, sysinfo(SURF_SYSINFO_MAP_VERSION),
               sysinfo(SURF_SYSINFO_SYS_HZ), sysinfo(SURF_SYSINFO_PIX_HZ));
    con_printf("bitstream build %08x flags %x, features %03x\n", sysinfo(SURF_SYSINFO_BUILD_ID),
               sysinfo(SURF_SYSINFO_BUILD_FLAGS), sysinfo(SURF_SYSINFO_FEATURES));
    diag_section("identity");
    diag_check(sysinfo(SURF_SYSINFO_MAGIC) == 0x46525553u, "sysinfo.MAGIC is 0x%08x", sysinfo(SURF_SYSINFO_MAGIC));
    diag_check(sysinfo(SURF_SYSINFO_MAP_VERSION) == SURF_MAP_VERSION, "memory map %u, this program expects %u",
               sysinfo(SURF_SYSINFO_MAP_VERSION), SURF_MAP_VERSION);
    diag_check(soc == SURF_SOC_VERSION, "SoC version 0x%08x, this program expects 0x%08x", soc, SURF_SOC_VERSION);
    diag_check(sysinfo(SURF_SYSINFO_FEATURES) & SURF_SYSINFO_FEATURES_GPU_MASK,
               "sysinfo.FEATURES has no GPU: this bitstream has no VRAM or graphics");
    diag_check(sysinfo(SURF_SYSINFO_FEATURES) & SURF_SYSINFO_FEATURES_VIDEO_MASK, "FEATURES lacks the video block");
    diag_section_end();
}

static void vram_window(uint32_t unused)
{
    (void)unused;
    const uint32_t lo = VRAM(0), hi = VRAM(SURF_VRAM_BYTES);
    diag_section("VRAM window (March C-)");
    for (int b = 0; b < 32; b++) {
        W(lo) = 1u << b;
        if (!diag_check(W(lo) == 1u << b, "walking one at 0x%08x bit %d reads 0x%08x", lo, b, W(lo)))
            break;
    }
    for (uint32_t k = 4; lo + k < hi; k <<= 1)
        W(lo + k) = k * 0x9E3779B1u;
    W(lo) = 0x13579BDFu;
    for (uint32_t k = 4; lo + k < hi; k <<= 1)
        diag_check(W(lo + k) == k * 0x9E3779B1u, "address line: 0x%08x reads 0x%08x", lo + k, W(lo + k));
    diag_check(W(lo) == 0x13579BDFu, "address line test: the base word changed");
    /* byte lanes */
    W(lo + 8) = 0x11223344u;
    B8(lo + 9) = 0xAA;
    H(lo + 10) = 0xBBCC;
    diag_check(W(lo + 8) == 0xBBCCAA44u && B8(lo + 11) == 0xBB && H(lo + 8) == 0xAA44,
               "byte lanes: 0x%08x", W(lo + 8));
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
    /* the end of VRAM: the last word works, 300 KiB and above is an error, and changes nothing */
    W(hi - 4) = 0xCAFEF00Du;
    expect_trap(0);
    uint32_t r = probe_load_word(hi, 0x5555);
    trapped("load at 300 KiB", 5, hi);
    diag_check(r == 0x5555, "a faulting load wrote its destination");
    expect_trap(0);
    probe_store_word(hi, 0xFFFFFFFFu);
    trapped("store at 300 KiB", 7, hi);
    expect_trap(0);
    probe_load_byte(VRAM(0x7FFFF), 0);
    trapped("byte load at the window's top", 5, VRAM(0x7FFFF));
    diag_check(W(hi - 4) == 0xCAFEF00Du && W(lo) == (lo ^ 0xA5A5A5A5u), "a faulting store changed VRAM");
    expect_trap(1);
    probe_jump(lo);
    trapped("fetch from VRAM", 1, lo);
    con_printf("(%u KiB) ", SURF_VRAM_BYTES >> 10);
    diag_section_end();
}

static void video_registers(uint32_t unused)
{
    (void)unused;
    diag_section("video graphics registers, GPAL");
    if (first_run)                                  /* a rerun finds the last run's values */
        diag_check(VREG(SURF_VIDEO_GFX_BASE) == 0 && VREG(SURF_VIDEO_GFX_STRIDE) == 640 &&
                   VREG(SURF_VIDEO_GFX_SCALE) == 1, "GFX_BASE/STRIDE/SCALE after reset: %x %x %x",
                   VREG(SURF_VIDEO_GFX_BASE), VREG(SURF_VIDEO_GFX_STRIDE), VREG(SURF_VIDEO_GFX_SCALE));
    VREG(SURF_VIDEO_GFX_BASE) = ~0u;
    VREG(SURF_VIDEO_GFX_STRIDE) = ~0u;
    VREG(SURF_VIDEO_GFX_SCALE) = ~0u;
    diag_check(VREG(SURF_VIDEO_GFX_BASE) == 0x7FFFCu && VREG(SURF_VIDEO_GFX_STRIDE) == 0xFFFCu &&
               VREG(SURF_VIDEO_GFX_SCALE) == 3, "GFX_ field masks: %x %x %x", VREG(SURF_VIDEO_GFX_BASE),
               VREG(SURF_VIDEO_GFX_STRIDE), VREG(SURF_VIDEO_GFX_SCALE));
    VREG(SURF_VIDEO_GFX_BASE) = 0;
    VREG(SURF_VIDEO_GFX_STRIDE) = 640;
    VREG(SURF_VIDEO_GFX_SCALE) = 1;
    uint32_t bad = 0;
    for (uint32_t n = 0; n < 256; n++)
        VREG(SURF_VIDEO_GPAL + 4 * n) = (n * 0x9E3u) ^ 0xFFFFF000u;
    for (uint32_t n = 0; n < 256; n++)
        bad += VREG(SURF_VIDEO_GPAL + 4 * n) != ((n * 0x9E3u) & 0xFFFu);
    diag_check(bad == 0, "GPAL: %u entries did not hold their value", bad);
    uint32_t c0 = VREG(SURF_VIDEO_FRAME_COUNT);
    timer_delay_us(50000);
    uint32_t frames = (VREG(SURF_VIDEO_FRAME_COUNT) - c0) & 0xFFFFu;
    diag_check(frames >= 2 && frames <= 4, "FRAME_COUNT counted %u frames in 50 ms", frames);
    con_printf("(%u frames in 50 ms) ", frames);
    diag_section_end();
}

/* ---- the scenes, through the hardware ---- */

static uint32_t engine_timeouts, fifo_credit;

/* a CMD word: FIFO_FREE is read only when the words known to fit are used up (it only grows
 * while nothing is written) */
static void hw_cmd(uint32_t w)
{
    uint32_t t0 = timer_now32();
    while (fifo_credit == 0) {
        fifo_credit = REG(SURF_GPU_BASE, SURF_GPU_FIFO_FREE);
        if (fifo_credit == 0 && (uint32_t)(timer_now32() - t0) > 100000u) {
            engine_timeouts++;
            return;
        }
    }
    fifo_credit--;
    REG(SURF_GPU_BASE, SURF_GPU_CMD) = w;
}

static void hw_sync(void)
{
    uint32_t t0 = timer_now32();
    while (REG(SURF_GPU_BASE, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK)
        if ((uint32_t)(timer_now32() - t0) > 200000u) {
            engine_timeouts++;
            return;
        }
}

static void hw_vram32(uint32_t off, uint32_t v) { W(VRAM(off)) = v; }
static void hw_vram8(uint32_t off, uint32_t v) { B8(VRAM(off)) = (uint8_t)v; }
static void hw_video(uint32_t off, uint32_t v) { VREG(off) = v; }
static void hw_text(uint32_t cell, uint32_t v) { W(SURF_TEXTRAM_BASE + 4u * cell) = v; }

static const struct gpu_sink hw_sink = {hw_cmd, hw_sync, hw_vram32, hw_vram8, hw_video, hw_text};

/* wait until a frame that began after now has been shown (docs/GPU.md section 3); 0 on time-out */
static int wait_still(void)
{
    uint32_t c0 = VREG(SURF_VIDEO_FRAME_COUNT), t0 = timer_now32();
    while (((VREG(SURF_VIDEO_FRAME_COUNT) - c0) & 0xFFFFu) < 2)
        if ((uint32_t)(timer_now32() - t0) > 200000u)
            return 0;
    return 1;
}

#define GREG(off) REG(SURF_GPU_BASE, off)
#define GPU_STICKY (SURF_GPU_STATUS_OVERFLOW_MASK | SURF_GPU_STATUS_BAD_OPCODE_MASK | \
                    SURF_GPU_STATUS_BAD_PARAM_MASK | SURF_GPU_STATUS_FENCE_EV_MASK)

/* whether the gpu window answers with the engine's ID, probed once per run (a trap is expected
 * where the window is unmapped) */
static int engine, engine_trapped;
static uint32_t engine_id, engine_cause;

static void probe_engine(void)
{
    expect_trap(0);
    engine_id = probe_load_word(SURF_GPU_BASE + SURF_GPU_ID, 0);
    expecting = 0;
    engine_trapped = got_count != 0;
    engine_cause = got_cause;
    engine = !engine_trapped && engine_id == GPU_ID_VALUE;
}

static void no_engine(void)
{
    if (engine_trapped)
        diag_check(0, "no engine: a load of gpu.ID at 0x%08x traps (mcause 0x%08x)", SURF_GPU_BASE + SURF_GPU_ID,
                   engine_cause);
    else
        diag_check(0, "no engine: gpu.ID reads 0x%08x, not 0x%08x (\"GPU1\")", engine_id, GPU_ID_VALUE);
}

/* the engine's registers and errors: overflow behind a WAIT_VBLANK, unknown opcodes, a refused
 * target, a fence with its interrupt (intc source gpu) */
static void gpu_registers(uint32_t unused)
{
    (void)unused;
    diag_section("gpu registers and errors");
    if (!engine) {
        no_engine();
        diag_section_end();
        return;
    }
    GREG(SURF_GPU_CTRL) = SURF_GPU_CTRL_ABORT_MASK;
    GREG(SURF_GPU_STATUS) = GPU_STICKY;
    GREG(SURF_GPU_IE) = 0;
    fifo_credit = 0;
    diag_check(GREG(SURF_GPU_ID) == GPU_ID_VALUE, "gpu.ID 0x%08x", GREG(SURF_GPU_ID));
    diag_check(GREG(SURF_GPU_INFO) == (300u << SURF_GPU_INFO_VRAM_KIB_LSB | 64u), "gpu.INFO 0x%08x", GREG(SURF_GPU_INFO));
    diag_check(GREG(SURF_GPU_FIFO_FREE) == 64 && GREG(SURF_GPU_STATUS) == 0, "idle: FIFO_FREE %u, STATUS 0x%x",
               GREG(SURF_GPU_FIFO_FREE), GREG(SURF_GPU_STATUS));
    /* a full FIFO: the engine waits for the next vertical blanking meanwhile */
    REG(SURF_VIDEO_BASE, SURF_VIDEO_STATUS) = SURF_VIDEO_STATUS_VBLANK_EV_MASK;
    uint32_t t0 = timer_now32();
    while (!(REG(SURF_VIDEO_BASE, SURF_VIDEO_STATUS) & SURF_VIDEO_STATUS_VBLANK_EV_MASK) &&
           (uint32_t)(timer_now32() - t0) < 100000u)
        ;
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_WAIT_VBLANK, 0);
    for (uint32_t i = 0; i < 32; i++) {
        GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_FENCE, 0);
        GREG(SURF_GPU_CMD) = 0x500u + i;
    }
    uint32_t free_full = GREG(SURF_GPU_FIFO_FREE);
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_FENCE, 0);                  /* dropped */
    uint32_t st = GREG(SURF_GPU_STATUS);
    diag_check(free_full == 0 && (st & SURF_GPU_STATUS_OVERFLOW_MASK) && (st & SURF_GPU_STATUS_BUSY_MASK),
               "a full FIFO: FIFO_FREE %u, STATUS 0x%x", free_full, st);
    hw_sync();
    diag_check(GREG(SURF_GPU_FENCE) == 0x51Fu && GREG(SURF_GPU_FIFO_FREE) == 64,
               "after the vertical blanking: FENCE 0x%x, FIFO_FREE %u", GREG(SURF_GPU_FENCE), GREG(SURF_GPU_FIFO_FREE));
    GREG(SURF_GPU_STATUS) = SURF_GPU_STATUS_OVERFLOW_MASK;
    /* an unknown opcode, a refused target: sticky, nothing else changes */
    GREG(SURF_GPU_CMD) = GPU_HDR(0x0Au, 0);
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_SET_TARGET, 0);
    GREG(SURF_GPU_CMD) = 4002;                                      /* not a multiple of 4 */
    GREG(SURF_GPU_CMD) = 320;
    GREG(SURF_GPU_CMD) = GPU_WH(320, 240);
    hw_sync();
    st = GREG(SURF_GPU_STATUS);
    diag_check((st & GPU_STICKY) == (SURF_GPU_STATUS_BAD_OPCODE_MASK | SURF_GPU_STATUS_BAD_PARAM_MASK),
               "unknown opcode and refused target: STATUS 0x%x", st);
    /* the refused target was not taken: a fill at (0, 0) lands in the target a reset leaves (0, not
     * byte 4002 or 4000, where the refused one would have put it) */
    W(VRAM(0)) = 0;
    W(VRAM(4)) = 0;
    W(VRAM(4000)) = 0;
    W(VRAM(4004)) = 0;
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_FILL_RECT, 0xC3u);
    GREG(SURF_GPU_CMD) = GPU_XY(0, 0);
    GREG(SURF_GPU_CMD) = GPU_WH(4, 1);
    hw_sync();
    diag_check(W(VRAM(0)) == 0xC3C3C3C3u && W(VRAM(4)) == 0 && W(VRAM(4000)) == 0 && W(VRAM(4004)) == 0,
               "after the refused target a fill at (0, 0) left VRAM words 0x%08x 0x%08x at 0, 0x%08x 0x%08x at 4000",
               W(VRAM(0)), W(VRAM(4)), W(VRAM(4000)), W(VRAM(4004)));
    /* the error interrupt, with the errors still sticky: only IE.ERR lets it through */
    GREG(SURF_GPU_IE) = SURF_GPU_IE_ERR_MASK;
    uint32_t p_err = REG(SURF_INTC_BASE, SURF_INTC_PENDING) & (1u << SURF_IRQ_GPU);
    GREG(SURF_GPU_IE) = SURF_GPU_IE_FENCE_MASK;
    uint32_t p_fence = REG(SURF_INTC_BASE, SURF_INTC_PENDING) & (1u << SURF_IRQ_GPU);
    GREG(SURF_GPU_IE) = 0;
    uint32_t p_none = REG(SURF_INTC_BASE, SURF_INTC_PENDING) & (1u << SURF_IRQ_GPU);
    diag_check(p_err && !p_fence && !p_none, "errors sticky: the gpu interrupt %s with IE.ERR, %s with IE.FENCE "
               "alone, %s with neither", p_err ? "on" : "off", p_fence ? "on" : "off", p_none ? "on" : "off");
    GREG(SURF_GPU_STATUS) = GPU_STICKY;
    /* a fence with its interrupt, through the interrupt controller */
    GREG(SURF_GPU_IE) = SURF_GPU_IE_FENCE_MASK;
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_FENCE, GPU_FENCE_IRQ);
    GREG(SURF_GPU_CMD) = 0xC0FFEEu;
    hw_sync();
    uint32_t pend = REG(SURF_INTC_BASE, SURF_INTC_PENDING);
    diag_check(GREG(SURF_GPU_FENCE) == 0xC0FFEEu && (pend & (1u << SURF_IRQ_GPU)),
               "FENCE with IRQ: FENCE 0x%x, intc.PENDING 0x%x", GREG(SURF_GPU_FENCE), pend);
    GREG(SURF_GPU_STATUS) = SURF_GPU_STATUS_FENCE_EV_MASK;
    pend = REG(SURF_INTC_BASE, SURF_INTC_PENDING);
    diag_check(!(pend & (1u << SURF_IRQ_GPU)), "the gpu interrupt stays after FENCE_EV is cleared");
    GREG(SURF_GPU_IE) = 0;
    /* ABORT empties the FIFO and stops the running command at once, and nothing queued runs later:
     * just after a blanking starts a WAIT_VBLANK waits 16 ms, with a fill and a fence behind it */
    W(VRAM(8)) = 0;
    REG(SURF_VIDEO_BASE, SURF_VIDEO_STATUS) = SURF_VIDEO_STATUS_VBLANK_EV_MASK;
    t0 = timer_now32();
    while (!(REG(SURF_VIDEO_BASE, SURF_VIDEO_STATUS) & SURF_VIDEO_STATUS_VBLANK_EV_MASK) &&
           (uint32_t)(timer_now32() - t0) < 100000u)
        ;
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_WAIT_VBLANK, 0);
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_FILL_RECT, 0x99u);
    GREG(SURF_GPU_CMD) = GPU_XY(8, 0);
    GREG(SURF_GPU_CMD) = GPU_WH(4, 1);
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_FENCE, 0);
    GREG(SURF_GPU_CMD) = 0xAB0u;
    uint32_t queued = 64u - GREG(SURF_GPU_FIFO_FREE);
    GREG(SURF_GPU_CTRL) = SURF_GPU_CTRL_ABORT_MASK;
    uint32_t free_after = GREG(SURF_GPU_FIFO_FREE);
    st = GREG(SURF_GPU_STATUS);
    diag_check(queued >= 5 && free_after == 64 && !(st & SURF_GPU_STATUS_BUSY_MASK),
               "ABORT behind a WAIT_VBLANK: %u words queued, then FIFO_FREE %u, STATUS 0x%x", queued, free_after, st);
    timer_delay_us(40000);                                          /* two blankings */
    diag_check(GREG(SURF_GPU_FENCE) != 0xAB0u && W(VRAM(8)) == 0,
               "a command queued before the ABORT ran later: FENCE 0x%x, VRAM word 0x%08x", GREG(SURF_GPU_FENCE),
               W(VRAM(8)));
    /* BUSY until the last write: a WRITE_RECT of one group waits for its data word, busy */
    W(VRAM(16)) = 0;
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_WRITE_RECT, 0);
    GREG(SURF_GPU_CMD) = GPU_XY(16, 0);
    GREG(SURF_GPU_CMD) = GPU_WH(4, 1);
    timer_delay_us(100);
    uint32_t st_wait = GREG(SURF_GPU_STATUS);
    GREG(SURF_GPU_CMD) = 0x44332211u;
    hw_sync();
    st = GREG(SURF_GPU_STATUS);
    diag_check((st_wait & SURF_GPU_STATUS_BUSY_MASK) && !(st & SURF_GPU_STATUS_BUSY_MASK) && W(VRAM(16)) == 0x44332211u,
               "WRITE_RECT waiting for its last data word: STATUS 0x%x; with it 0x%x, VRAM word 0x%08x", st_wait, st,
               W(VRAM(16)));
    diag_check(engine_timeouts == 0, "the engine did not finish in time");
    diag_section_end();
}

static void scenes(uint32_t unused)
{
    (void)unused;
    diag_section("scenes");
    diag_check(gpu_scene_count == GPU_SCENE_COUNT, "%u scenes, %u expected CRCs", gpu_scene_count, GPU_SCENE_COUNT);
    con_printf("%u, engine %s\n", gpu_scene_count, engine ? "present" : "absent");
    for (unsigned n = 0; n < gpu_scene_count && n < GPU_SCENE_COUNT; n++) {
        char name[48];
        fmt_snprintf(name, sizeof name, "scene %2u: %s", n, gpu_scenes[n].name);
        diag_section(name);
        if (gpu_scenes[n].engine && !engine) {         /* skipped, and failed */
            diag_check(0, "skipped: no engine");
            diag_section_end();
            continue;
        }
        engine_timeouts = 0;
        fifo_credit = 0;
        gpu_scenes[n].draw(&hw_sink);
        if (gpu_scenes[n].engine)
            hw_sync();
        diag_check(engine_timeouts == 0, "the engine did not take or finish the commands in time");
        if (n == 1) {
            /* the FRAME pair (docs/GPU.md section 3): FRAME_COUNT was read on scene 0's picture, then
             * scene 1 drawn; two frames of it later, with a STATUS read between, FRAME_CRC is still
             * the counted frame's (scene 0's), and a FRAME_CRC read after that the newest */
            timer_delay_us(50000);
            (void)VREG(SURF_VIDEO_STATUS);
            uint32_t c_pair = VREG(SURF_VIDEO_FRAME_CRC), c_new = VREG(SURF_VIDEO_FRAME_CRC);
            diag_check(c_pair == gpu_scene_expect[0], "FRAME_COUNT read on scene 0, FRAME_CRC on scene 1: 0x%08x, "
                       "scene 0's 0x%08x", c_pair, gpu_scene_expect[0]);
            diag_check(c_new == gpu_scene_expect[1], "a lone FRAME_CRC read after the pair: 0x%08x, the newest "
                       "frame's 0x%08x", c_new, gpu_scene_expect[1]);
        }
        int still = wait_still();
        diag_check(still, "FRAME_COUNT did not move in 200 ms");
        uint32_t crc = VREG(SURF_VIDEO_FRAME_CRC);
        diag_check(crc == gpu_scene_expect[n], "FRAME_CRC 0x%08x, the model's 0x%08x", crc, gpu_scene_expect[n]);
        con_printf("crc %08x ", crc);
        if (n == 0)
            (void)VREG(SURF_VIDEO_FRAME_COUNT);         /* the first half of scene 1's FRAME pair */
        diag_section_end();
    }
}

/* a run that made fewer (or more) checks than a complete one fails */
static void completeness(void)
{
    uint32_t ran = diag_checks, want = first_run ? GPUDIAG_CHECKS : GPUDIAG_RERUN_CHECKS;
    if (ran == want)
        return;
    diag_section("completeness");
    diag_check(0, "%u checks ran; a complete %s makes %u", ran, first_run ? "run" : "rerun", want);
    diag_section_end();
}

/* the result in the top text row, over the last picture (MODE 3: an opaque bar) */
static void show_result(void)
{
    char line[81];
    int n = fmt_snprintf(line, sizeof line, " GPUDIAG: %s (%u checks, %u failed) ", diag_failures ? "FAIL" : "PASS",
                         diag_checks, diag_failures);
    uint32_t attr = (diag_failures ? 4u : 2u) << 12 | 15u << 8;
    for (int c = 0; c < 80; c++)
        W(SURF_TEXTRAM_BASE + 4u * c) = attr | (uint8_t)(c < n ? line[c] : ' ');
    VREG(SURF_VIDEO_SCROLL) = 0;
    VREG(SURF_VIDEO_CTRL) = SURF_VIDEO_CTRL_ENABLE_MASK | (3u << SURF_VIDEO_CTRL_MODE_LSB);
}

static void echo(void)
{
    con_puts("\nUART echo: type; each character comes back with its code. Ctrl-R runs the diagnostic again.\n");
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
    fw_trap_hook = gpudiag_hook;
    for (;;) {
        led_set(0x1, 0x004);
        diag_checks = diag_failures = 0;
        con_printf("\nSurfGPU diagnostic %s (build %08x%s)\n", DIAG_VERSION, (unsigned)FW_BUILD_ID,
                   FW_BUILD_DIRTY ? ", dirty tree" : "");
        if (!first_run)
            con_printf("a rerun (Ctrl-R): the GFX_ values a reset leaves are checked by the first run after a "
                       "reset only, so a complete rerun makes %u checks\n", GPUDIAG_RERUN_CHECKS);
        diag_guard(identity, 0);
        if (sysinfo(SURF_SYSINFO_FEATURES) & SURF_SYSINFO_FEATURES_GPU_MASK) {
            led_set(0x3, 0x004);
            diag_guard(vram_window, 0);
            diag_guard(video_registers, 0);
            led_set(0x7, 0x004);
            probe_engine();
            diag_guard(gpu_registers, 0);
            diag_guard(scenes, 0);
            completeness();
            show_result();
        } else {
            completeness();                             /* identity has said why */
        }
        first_run = 0;
        diag_summary("GPUDIAG");
        echo();
    }
}
