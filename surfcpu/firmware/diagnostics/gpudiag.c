/*
SurfGPU diagnostics: the GPU diagnostic
--------------------
File: gpudiag.c Date: 10/18/26 (SurfGPU), 10/25/26 (GPU2)
--------------------
A boot ROM image (build/fw/gpudiag.mem) for the GPU designs (sysinfo.FEATURES.GPU; docs/GPU.md
section 10), like bramdiag: it needs only the CPU, block RAM, the UART, the timer, VRAM and the
video block. It prints the build identity, then checks the VRAM window (walking ones, address
lines, byte lanes, March C- and address-in-address over all 300 KiB, the error at 300 KiB for
loads and stores, no fetch), the new video registers and GPAL, the engine's registers and errors
(a full FIFO and OVERFLOW, an unknown opcode, a refused target, the target and clip a reset
leaves, a fence and its interrupt at intc source gpu), then draws gpudiag's scenes
(gpuscenes.c) one after the other. For each it waits until a frame that began after the
picture was complete has been shown (FRAME_COUNT two on), and compares FRAME_CRC with the CRC
the C model computed for that scene when the ROM was built (gpuscenes_expect.h), printing both.
A design without the engine (no FEATURES.GPU, a gpu window that traps or a gpu.ID other than
"GPU1") fails: every check that could not run counts as failed, and a run that made fewer checks
than a complete one (GPUDIAG_CHECKS) fails too, so PASS means every check ran and passed. The
GFX_ values a reset leaves can only be seen by the first run after a reset: a rerun (Ctrl-R) says
so in a line under its banner and is held to GPUDIAG_RERUN_CHECKS, one fewer.
GPU2 (docs/GPU.md section 16): gpu.ID must read "GPU2"; a section checks SET_ZBUF's refusal, a
TRIANGLE with depth but no depth buffer (refused), SET_TARGET dropping the depth buffer and FLIP
waiting for the blanking; four scenes draw triangles (flat, Gouraud, depth) and FLIP; then the
cube (gpucube.c) draws 16 frames at full rate, double-buffered with FLIP, while the CPU checks the
FRAME_CRC of every frame shown against the model's CRCs of the cube's frames (a torn frame, a
frame missed or out of order fails), and the first and last as still pictures.
It ends with "GPUDIAG: PASS (n checks)" or "GPUDIAG: FAIL (...)" and the LEDs (LD0 green or red).
After a PASS the cube spins on under a result bar (MODE 3), a line a second on the UART with the
frames drawn, shown, missed and torn; after a FAIL the result shows in the top text row over the
last picture. Either way UART input is echoed, and Ctrl-R runs it all again. The text console is
not used (the scenes own the screen): everything goes to the UART.
*/
#include "diag.h"
#include "gpucube.h"
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
        diag_check(0, "no engine: gpu.ID reads 0x%08x, not 0x%08x (\"GPU2\")", engine_id, GPU_ID_VALUE);
}

/* the engine's registers and errors: overflow behind a WAIT_VBLANK, unknown opcodes, a refused
 * target and the reset target and clip, a fence with its interrupt (intc source gpu) */
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
    /* a rerun sets the target and clip a reset leaves (SET_TARGET sets the clip too): ABORT keeps
     * them, so a rerun after a run abandoned inside an engine scene would otherwise start with that
     * scene's. The first run after a reset does not: the fills and the copy after the refused target
     * below check the reset values themselves, as video_registers checks the GFX_ ones */
    if (!first_run) {
        GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_SET_TARGET, 0);
        GREG(SURF_GPU_CMD) = 0;
        GREG(SURF_GPU_CMD) = 640;
        GREG(SURF_GPU_CMD) = GPU_WH(640, 480);
        hw_sync();
    }
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
    GREG(SURF_GPU_CMD) = GPU_HDR(0x0Du, 0);                         /* the first after GPU2's */
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_SET_TARGET, 0);
    GREG(SURF_GPU_CMD) = 4002;                                      /* not a multiple of 4 */
    GREG(SURF_GPU_CMD) = 320;
    GREG(SURF_GPU_CMD) = GPU_WH(320, 240);
    hw_sync();
    st = GREG(SURF_GPU_STATUS);
    diag_check((st & GPU_STICKY) == (SURF_GPU_STATUS_BAD_OPCODE_MASK | SURF_GPU_STATUS_BAD_PARAM_MASK),
               "unknown opcode and refused target: STATUS 0x%x", st);
    /* the refused target was not taken, and the target and clip are the ones a reset leaves (base 0,
     * stride 640, 640 x 480, the clip the whole of it; on a rerun, set so above), seen wherever the
     * engine shows them: two fills and a copy, each word below set to "before", must leave "after".
     * The fill at (0, 0) lands at byte 0 (base 0, the clip's left and top edges at 0), not at byte
     * 4002 or 4000, where the refused target would have put it. The 8 x 2 fill at (636, 478) writes
     * the last four pixels of rows 478 and 479 (stride 640; the clip's right edge at 640 or past,
     * its bottom edge at 480 or past) and not row 479's first four, where its pixels 640..643 of
     * row 478 would land if the right edge were past 640 (row 479's would be past VRAM's end). The
     * 8 x 2 copy from (636, 479) to (0, 4) copies its top-left four pixels only: the rest of its
     * source lies beyond the target's width or height, so those are 640 and 480, not more. (The
     * engine clips to the target as well as to the clip, so a clip edge past the target changes
     * nothing a program can see.) */
    static const struct { uint32_t off, before, after; } reset_view[] = {
        {0, 0, 0xC3C3C3C3u},                /* fill (0, 0) 4 x 1 */
        {4, 0, 0},
        {4000, 0, 0},                       /* the refused target's (0, 0) is byte 4002 */
        {4004, 0, 0},
        {306556, 0, 0x5A5A5A5Au},           /* fill (636, 478) 8 x 2: (636, 478) */
        {306560, 0, 0},                     /* (0, 479), where (640, 478) would land */
        {307196, 0, 0x5A5A5A5Au},           /* (636, 479) */
        {2560, 0xA5A5A5A5u, 0x5A5A5A5Au},   /* copy (636, 479) to (0, 4) 8 x 2: from (636, 479) */
        {2564, 0xA5A5A5A5u, 0xA5A5A5A5u},   /* from (640, 479): beyond the target's width */
        {3200, 0xA5A5A5A5u, 0xA5A5A5A5u},   /* from (636, 480): beyond its height */
        {3204, 0xA5A5A5A5u, 0xA5A5A5A5u},
    };
    const uint32_t n_view = sizeof reset_view / sizeof reset_view[0];
    for (uint32_t i = 0; i < n_view; i++)
        W(VRAM(reset_view[i].off)) = reset_view[i].before;
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_FILL_RECT, 0xC3u);
    GREG(SURF_GPU_CMD) = GPU_XY(0, 0);
    GREG(SURF_GPU_CMD) = GPU_WH(4, 1);
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_FILL_RECT, 0x5Au);
    GREG(SURF_GPU_CMD) = GPU_XY(636, 478);
    GREG(SURF_GPU_CMD) = GPU_WH(8, 2);
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_COPY_RECT, 0);
    GREG(SURF_GPU_CMD) = GPU_XY(636, 479);
    GREG(SURF_GPU_CMD) = GPU_XY(0, 4);
    GREG(SURF_GPU_CMD) = GPU_WH(8, 2);
    hw_sync();
    uint32_t wrong = 0, w_at = 0;
    for (uint32_t i = 0; i < n_view; i++)
        if (W(VRAM(reset_view[i].off)) != reset_view[i].after && wrong++ == 0)
            w_at = i;
    diag_check(wrong == 0, "after the refused target, the fills and the copy in the reset target: the VRAM word at "
               "byte %u reads 0x%08x, not 0x%08x (%u of %u words wrong)", reset_view[w_at].off,
               W(VRAM(reset_view[w_at].off)), reset_view[w_at].after, wrong, n_view);
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

/* GPU2's commands (docs/GPU.md section 16): a refused SET_ZBUF and a TRIANGLE with depth but no
 * depth buffer (both BAD_PARAM, nothing drawn), an accepted SET_TARGET leaving no depth buffer, and
 * FLIP writing GFX_BASE only at the next vertical blanking */
static void gpu2_commands(uint32_t unused)
{
    (void)unused;
    diag_section("gpu2 commands");
    if (!engine) {
        no_engine();
        diag_section_end();
        return;
    }
    W(VRAM(8000)) = 0x5A5A5A5Au;
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_SET_TARGET, 0);              /* 64 x 64 at 4000 */
    GREG(SURF_GPU_CMD) = 4000;
    GREG(SURF_GPU_CMD) = 64;
    GREG(SURF_GPU_CMD) = GPU_WH(64, 64);
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_SET_ZBUF, 0);                /* refused: not a multiple of 4 */
    GREG(SURF_GPU_CMD) = 0x20002u;
    GREG(SURF_GPU_CMD) = 128;
    static const uint32_t tri_words[6] = {GPU_XY(0, 0), GPU_ZC(100, 0), GPU_XY(GPU_FX(64), 0), GPU_ZC(100, 0),
                                          GPU_XY(0, GPU_FX(64)), GPU_ZC(100, 0)};
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_TRIANGLE, GPU_TRI_ZWRITE | 0x77u);   /* no depth buffer: refused */
    for (int i = 0; i < 6; i++)
        GREG(SURF_GPU_CMD) = tri_words[i];
    hw_sync();
    uint32_t st = GREG(SURF_GPU_STATUS);
    diag_check((st & GPU_STICKY) == SURF_GPU_STATUS_BAD_PARAM_MASK && W(VRAM(8000)) == 0x5A5A5A5Au,
               "a refused SET_ZBUF, then a TRIANGLE with ZWRITE and no depth buffer: STATUS 0x%x, VRAM word 0x%08x",
               st, W(VRAM(8000)));
    GREG(SURF_GPU_STATUS) = GPU_STICKY;
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_SET_ZBUF, 0);                /* taken ... */
    GREG(SURF_GPU_CMD) = 0x20000u;
    GREG(SURF_GPU_CMD) = 128;
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_SET_TARGET, 0);              /* ... and dropped by a new target */
    GREG(SURF_GPU_CMD) = 4000;
    GREG(SURF_GPU_CMD) = 64;
    GREG(SURF_GPU_CMD) = GPU_WH(64, 64);
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_TRIANGLE, GPU_TRI_ZTEST | 0x77u);
    for (int i = 0; i < 6; i++)
        GREG(SURF_GPU_CMD) = tri_words[i];
    hw_sync();
    st = GREG(SURF_GPU_STATUS);
    diag_check((st & GPU_STICKY) == SURF_GPU_STATUS_BAD_PARAM_MASK && W(VRAM(8000)) == 0x5A5A5A5Au,
               "a TRIANGLE with ZTEST after SET_TARGET dropped the depth buffer: STATUS 0x%x, VRAM word 0x%08x", st,
               W(VRAM(8000)));
    GREG(SURF_GPU_STATUS) = GPU_STICKY;
    /* FLIP just after a blanking starts: GFX_BASE stays until the next one, then has the FLIP's base */
    VREG(SURF_VIDEO_GFX_BASE) = 0;
    REG(SURF_VIDEO_BASE, SURF_VIDEO_STATUS) = SURF_VIDEO_STATUS_VBLANK_EV_MASK;
    uint32_t t0 = timer_now32();
    while (!(REG(SURF_VIDEO_BASE, SURF_VIDEO_STATUS) & SURF_VIDEO_STATUS_VBLANK_EV_MASK) &&
           (uint32_t)(timer_now32() - t0) < 100000u)
        ;
    REG(SURF_VIDEO_BASE, SURF_VIDEO_STATUS) = SURF_VIDEO_STATUS_VBLANK_EV_MASK;
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_FLIP, 0);
    GREG(SURF_GPU_CMD) = 0x12C03u;                                  /* GFX_BASE keeps 0x12C00 */
    GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_FENCE, 0);
    GREG(SURF_GPU_CMD) = 0xF11F0u;
    timer_delay_us(2000);
    const uint32_t base_before = VREG(SURF_VIDEO_GFX_BASE), fence_before = GREG(SURF_GPU_FENCE);
    t0 = timer_now32();
    while (!(REG(SURF_VIDEO_BASE, SURF_VIDEO_STATUS) & SURF_VIDEO_STATUS_VBLANK_EV_MASK) &&
           (uint32_t)(timer_now32() - t0) < 100000u)
        ;
    hw_sync();
    const uint32_t base_after = VREG(SURF_VIDEO_GFX_BASE);
    diag_check(base_before == 0 && fence_before != 0xF11F0u && base_after == 0x12C00u && GREG(SURF_GPU_FENCE) == 0xF11F0u,
               "FLIP: GFX_BASE 0x%x before the next blanking (FENCE 0x%x), 0x%x after it", base_before, fence_before,
               base_after);
    VREG(SURF_VIDEO_GFX_BASE) = 0;
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

/* ---- the cube (gpucube.c, docs/GPU.md section 16.9) ---- */

/* a frame's words, recorded, then pushed while FIFO_FREE has room (the CPU never waits for it) */
static uint32_t cube_words[GPU_CUBE_FRAME_WORDS + 8];
static uint32_t cube_n, cube_pos;
static void rec_cmd(uint32_t w)
{
    if (cube_n < sizeof cube_words / sizeof cube_words[0])
        cube_words[cube_n++] = w;
}
static void rec_sync(void) {}
static const struct gpu_sink rec_sink = {rec_cmd, rec_sync, hw_vram32, hw_vram8, hw_video, hw_text};

#define CUBE_FENCE 0x0C0B0000u                          /* frame k's FENCE: CUBE_FENCE + k + 1 */

struct cube_run {
    const uint32_t *crcs;                               /* the model's CRC of each frame shown */
    uint32_t ncrcs;
    uint32_t next;                                      /* the next frame to draw */
    uint32_t last_count;                                /* FRAME_COUNT at the last look */
    uint32_t shown, missed, torn;                       /* frames counted, not looked at, no cube frame's */
    int32_t last_frame;                                 /* the frame index of the last frame shown */
    uint32_t out_of_order;
    uint32_t seen_lo, seen_hi;                          /* frames 0..63 seen (the checked run) */
};

static int32_t cube_lookup(const struct cube_run *c, uint32_t crc)
{
    for (uint32_t i = 0; i < c->ncrcs; i++)
        if (c->crcs[i] == crc)
            return (int32_t)i;
    return -1;
}

/* the frame counter: every new frame's CRC must be one of the cube's frames */
static void cube_look(struct cube_run *c)
{
    const uint32_t count = VREG(SURF_VIDEO_FRAME_COUNT) & 0xFFFFu;
    if (count == c->last_count)
        return;
    const uint32_t d = (count - c->last_count) & 0xFFFFu, crc = VREG(SURF_VIDEO_FRAME_CRC);
    c->last_count = count;
    c->shown += d;
    c->missed += d - 1u;
    const int32_t f = cube_lookup(c, crc);
    if (f < 0) {
        c->torn++;
        return;
    }
    if (f < c->last_frame && !(c->ncrcs == GPU_CUBE_PERIOD && c->last_frame - f > (int32_t)GPU_CUBE_PERIOD / 2))
        c->out_of_order++;                               /* (the free run wraps from 127 to 0) */
    c->last_frame = f;
    if (f < 32)
        c->seen_lo |= 1u << f;
    else if (f < 64)
        c->seen_hi |= 1u << (f - 32);
}

/* one step: a look at the frame counter, then the next frame's words as FIFO_FREE allows; a new
 * frame is recorded when the last one's words are all in the FIFO, until frame `until` */
static void cube_step(struct cube_run *c, uint32_t until)
{
    cube_look(c);
    if (cube_pos < cube_n) {
        uint32_t room = GREG(SURF_GPU_FIFO_FREE);
        while (room-- && cube_pos < cube_n)
            GREG(SURF_GPU_CMD) = cube_words[cube_pos++];
    } else if (c->next < until) {
        cube_n = cube_pos = 0;
        gpu_cube_frame(&rec_sink, c->next, c->next & 1u, CUBE_FENCE + c->next + 1u);
        c->next++;
    }
}

/* frame 0 into A and shown (MODE 2, or 3 with the bar), a still picture: its FRAME_CRC; 0 on time-out */
static int cube_start(struct cube_run *c, int bar, uint32_t *crc)
{
    fifo_credit = 0;
    engine_timeouts = 0;
    cube_n = cube_pos = 0;
    gpu_cube_frame(&hw_sink, 0, 0, CUBE_FENCE + 1u);
    hw_sync();
    gpu_cube_display(&hw_sink, bar);
    const int still = wait_still();
    *crc = VREG(SURF_VIDEO_FRAME_CRC);
    c->next = 1;
    c->last_count = VREG(SURF_VIDEO_FRAME_COUNT) & 0xFFFFu;
    c->shown = c->missed = c->torn = c->out_of_order = 0;
    c->last_frame = 0;
    c->seen_lo = 1;
    c->seen_hi = 0;
    return still && engine_timeouts == 0;
}

/* the checks: frame 0 as a still picture, frames 1 .. 15 at full rate (every frame shown one of the
 * cube's, in order, each of them shown, none missed, at least 30 a second), frame 15 as a still */
static void cube_check(uint32_t unused)
{
    (void)unused;
    diag_section("cube: 16 frames at full rate");
    if (!engine) {
        no_engine();
        diag_section_end();
        return;
    }
    struct cube_run c = {gpu_cube_expect_check, GPU_CUBE_CHECK_FRAMES, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t crc0 = 0;
    const int ok0 = cube_start(&c, 0, &crc0);
    diag_check(ok0 && crc0 == gpu_cube_expect_check[0], "frame 0, a still picture: FRAME_CRC 0x%08x, the model's 0x%08x%s",
               crc0, gpu_cube_expect_check[0], ok0 ? "" : " (the engine or FRAME_COUNT timed out)");
    const uint32_t frames0 = VREG(SURF_VIDEO_FRAME_COUNT) & 0xFFFFu, t0 = timer_now32();
    int late = 0;
    /* until the last frame is on the screen: its FLIP is done at a blanking, and the frame after
     * that blanking shows it */
    while (c.last_frame != (int32_t)GPU_CUBE_CHECK_FRAMES - 1) {
        cube_step(&c, GPU_CUBE_CHECK_FRAMES);
        if ((uint32_t)(timer_now32() - t0) > 1000000u) {
            late = 1;
            break;
        }
    }
    const uint32_t video_frames = ((VREG(SURF_VIDEO_FRAME_COUNT) & 0xFFFFu) - frames0) & 0xFFFFu;
    const uint32_t all = (1u << GPU_CUBE_CHECK_FRAMES) - 1u;
    diag_check(!late && video_frames <= 2u * (GPU_CUBE_CHECK_FRAMES - 1u) + 2u,
               "frames 1 to %u shown within %u video frames%s (30 a second or more: at most %u)",
               GPU_CUBE_CHECK_FRAMES - 1u, video_frames, late ? ", stopped after a second" : "",
               2u * (GPU_CUBE_CHECK_FRAMES - 1u) + 2u);
    diag_check(c.torn == 0, "%u of the %u frames shown were none of the cube's (torn)", c.torn, c.shown);
    diag_check(c.seen_lo == all && c.out_of_order == 0, "the frames shown: %08x (every one of 0..%u: %08x), %u out of order",
               c.seen_lo, GPU_CUBE_CHECK_FRAMES - 1u, all, c.out_of_order);
    diag_check(c.missed == 0, "%u frames shown were not looked at (FRAME_COUNT moved by more than one)", c.missed);
    const int still = wait_still();
    const uint32_t crc = VREG(SURF_VIDEO_FRAME_CRC);
    diag_check(still && crc == gpu_cube_expect_check[GPU_CUBE_CHECK_FRAMES - 1u],
               "frame %u, a still picture: FRAME_CRC 0x%08x, the model's 0x%08x", GPU_CUBE_CHECK_FRAMES - 1u, crc,
               gpu_cube_expect_check[GPU_CUBE_CHECK_FRAMES - 1u]);
    con_printf("(%u video frames, %u shown) ", video_frames, c.shown);
    diag_section_end();
}

/* UART output that never waits: the cube's lines go into a ring the loop drains as the UART takes
 * them (a waiting printf of a line would stop the frame checks for longer than a frame) */
static char out_ring[512];
static uint32_t out_head, out_tail;
static void ring_out(void *ctx, char ch)
{
    (void)ctx;
    if (ch == '\n')
        ring_out(ctx, '\r');
    if (((out_head + 1u) & 511u) != out_tail) {
        out_ring[out_head] = ch;
        out_head = (out_head + 1u) & 511u;
    }
}
static void ring_printf(const char *fmt, ...) FMT_PRINTF(1, 2);
static void ring_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fmt_vformat(ring_out, 0, fmt, ap);
    va_end(ap);
}
static void ring_drain(void)
{
    while (out_tail != out_head && !(REG(SURF_UART_BASE, SURF_UART_TXDATA) & SURF_UART_TXDATA_FULL_MASK)) {
        REG(SURF_UART_BASE, SURF_UART_TXDATA) = (uint8_t)out_ring[out_tail];
        out_tail = (out_tail + 1u) & 511u;
    }
}

/* after a PASS: the cube until Ctrl-R, under the result bar (MODE 3), every frame shown checked
 * against the model's CRCs of the cube's 128 frames with the bar, a line a second; the UART echo
 * goes on meanwhile */
static void cube_free_run(void)
{
    con_puts("\nUART echo: type; each character comes back with its code. Ctrl-R runs the diagnostic again.\n");
    con_printf("cube: a line a second: frames drawn (FLIPs done), shown (frames on the screen), missed (frames "
               "not checked), torn (frames that were none of the cube's %u)\n", GPU_CUBE_PERIOD);
    struct cube_run c = {gpu_cube_expect_run, GPU_CUBE_PERIOD, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t crc0 = 0;
    if (!cube_start(&c, 1, &crc0) || crc0 != gpu_cube_expect_run[0])
        con_printf("  FAIL cube frame 0 under the bar: FRAME_CRC 0x%08x, the model's 0x%08x\n", crc0,
                   gpu_cube_expect_run[0]);
    out_head = out_tail = 0;
    uint32_t sec = 0, beat = timer_now32(), echoes = 0;
    uint32_t drawn_base = 0, shown_base = 0, missed_base = 0, torn_base = 0;
    for (;;) {
        cube_step(&c, 0xFFFFFFFFu);
        ring_drain();
        if ((uint32_t)(timer_now32() - beat) >= 1000000u) {
            beat += 1000000u;
            sec++;
            REG(SURF_GPIO_BASE, SURF_GPIO_LED) ^= 0x8u;     /* heartbeat on LD7 */
            const uint32_t drawn = GREG(SURF_GPU_FENCE) - CUBE_FENCE;
            ring_printf("cube %4u s: %3u drawn, %3u shown, %u missed, %u torn (in all %u drawn, %u shown, %u missed, "
                        "%u torn)\n", sec, drawn - drawn_base, c.shown - shown_base, c.missed - missed_base,
                        c.torn - torn_base, drawn, c.shown, c.missed, c.torn);
            drawn_base = drawn;
            shown_base = c.shown;
            missed_base = c.missed;
            torn_base = c.torn;
        }
        const int ch = con_getc();
        if (ch == 0x12) {
            while (out_tail != out_head)
                ring_drain();
            GREG(SURF_GPU_CTRL) = SURF_GPU_CTRL_ABORT_MASK;
            return;
        }
        if (ch >= 0) {
            echoes++;
            ring_printf("echo %3u: '%c' 0x%02x\n", echoes, (ch >= 0x20 && ch < 0x7F) ? ch : '.', ch);
        }
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
            diag_guard(gpu2_commands, 0);
            diag_guard(scenes, 0);
            diag_guard(cube_check, 0);
            completeness();
            if (diag_failures || !engine)
                show_result();
        } else {
            completeness();                             /* identity has said why */
        }
        first_run = 0;
        diag_summary("GPUDIAG");
        int spin = !diag_failures && engine;
#ifdef GPUDIAG_TEST_LEFTOVER
        /* make test-gpu-soc only (build/fw/gpudiag-leftover.mem): leave a target and a clip as a run
         * abandoned inside an engine scene would, for the rerun to start with (and no cube, whose
         * frames would set their own) */
        spin = 0;
        if (engine) {
            GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_SET_TARGET, 0);
            GREG(SURF_GPU_CMD) = 0x10000u;
            GREG(SURF_GPU_CMD) = 320;
            GREG(SURF_GPU_CMD) = GPU_WH(320, 240);
            GREG(SURF_GPU_CMD) = GPU_HDR(GPU_OP_SET_CLIP, 0);
            GREG(SURF_GPU_CMD) = GPU_XY(40, 40);
            GREG(SURF_GPU_CMD) = GPU_XY(280, 200);
            hw_sync();
        }
#endif
        if (spin)
            cube_free_run();                            /* until Ctrl-R */
        else
            echo();
    }
}
