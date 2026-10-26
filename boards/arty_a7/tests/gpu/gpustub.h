/*
SurfOS host test: SurfGPU's driver -- the stand-in for the hardware
----------------------
File: gpustub.h Date: 10/25/26 (SurfGPU G3)
----------------------
A struct gpu_port (boards/arty_a7/gpu.h) that is a simulated SoC: the gpu registers with a
64-word command FIFO in front of the C model of the engine (sim/models/gpu_model.c: each word
taken is handed to gpu_engine_word() as surf_gpu's FIFO hands it over), the video registers with
a frame clock (640x480 at 59.94 Hz: a frame every 16,683 us, vertical blanking from 15,253 us into
it, the frame latch 1,398 us later), GPAL, FRAME_COUNT and FRAME_CRC computed by the model's
display path (gpu_display_render(), gpu_frame_crc()) from what the latch took, and VRAM shared by
the engine and the CPU's window.

Time is simulated in microseconds: every register access costs one, a yield five, a sleep its
length; the engine works through its FIFO as time passes, at 25 MHz: a word a cycle, a cycle per
four pixels it writes, a WAIT_VBLANK until the next blanking begins. Knobs make it misbehave:
an engine that takes no word, words dropped or corrupted on their way in, the CPU stalled at a
moment, a frame counter that stops, Ctrl+C at a moment, another claimant holding the driver.

It records what the tests check: every CMD word (and whether the FIFO had room and the driver
had read FIFO_FREE for it), the display registers' writes with their times and the frame latch
each one reached, ABORTs, engine words that drew into the page on the screen while it was shown.

10/26/26 (GPU2): with gpu.ID "GPU2" the engine takes SET_ZBUF, TRIANGLE and FLIP (a GPU1 calls
them unknown opcodes); FLIP writes GFX_BASE at the next vertical blanking, as surf_gpu does; and
without a pixel clock (frames_stop) no blanking ever begins, so WAIT_VBLANK and FLIP wait for ever.
*/

#ifndef _GPUSTUB_H
#define _GPUSTUB_H

#include <stdint.h>
#include "gpu_model.h"
#include "gpu.h"

#define STUB_FRAME_US   16683u          /* 800 x 525 pixel clocks at 25.175 MHz */
#define STUB_VBLANK_US  15253u          /* line 480: vertical blanking begins (STATUS.FRAME, VBLANK_EV) */
#define STUB_LATCH_US   16651u          /* line 524: the frame latch takes CTRL and the GFX_ registers */
#define STUB_CMD_LOG    (1u << 20)
#define STUB_DISP_LOG   4096u
#define STUB_NEVER      (1ull << 40)    /* a blanking that never comes (us) */

struct stub_disp {                      /* the display registers as one value */
    uint32_t mode, base, stride, scale;
};

struct stub_event {                     /* what changed what a picture shows (stub.watch) */
    uint64_t t;
    unsigned kind;
};
#define STUB_EV_CPU_VRAM    1           /* the CPU wrote VRAM through the window */
#define STUB_EV_GPAL        2           /* a GPAL entry was written */
#define STUB_EV_ENGINE_DRAW 3           /* the engine took a word that draws */

struct stub_write {                     /* a display register write */
    uint64_t t;
    uint32_t reg, value;                /* reg: SURF_VIDEO_GFX_* or 0xFFFF for CTRL.MODE */
};

struct stub {
    struct gpu_port port;
    /* the engine */
    struct gpu_engine eng;
    uint8_t vram[GPU_VRAM_BYTES];
    uint32_t fifo[64];
    unsigned head, count, depth;
    uint32_t id, info, ie, overflow;
    uint64_t eng_cycles;                /* the engine's own clock (25 per us) */
    int wait_vbl;                       /* a WAIT_VBLANK or a FLIP holds the engine */
    uint64_t vbl_until;
    int flip_pending;                   /* the wait is a FLIP's: GFX_BASE = flip_value when it ends */
    uint32_t flip_value;
    unsigned flips_done, gpu2_on_gpu1;  /* FLIPs that wrote GFX_BASE; GPU2 opcodes a GPU1 refused */
    /* the CPU's time */
    uint64_t now;
    uint64_t stall_at, stall_us;        /* once now passes stall_at, it jumps on by stall_us (outside atomic stretches) */
    int frames_stop;                    /* no pixel clock: the frame counter stays, no blanking begins */
    uint32_t frames_frozen;             /* where it stays then (7 when 0) */
    /* the video block */
    struct stub_disp reg;               /* what the registers hold now */
    uint16_t gpal[256];
    uint32_t text_pal[16];
    uint64_t ev_cleared;                /* VBLANK_EV: a blanking began after this time */
    int kept;                           /* FRAME_COUNT was read: its frame's CRC is kept */
    uint32_t kept_frame;
    struct stub_write wlog[STUB_DISP_LOG];  /* display writes, for the latch */
    unsigned nw;
    /* knobs */
    int stopped;                        /* the engine takes no word */
    uint64_t stop_at;                   /* it stops when now passes this (0: never) */
    unsigned slow;                      /* extra engine cycles for every word it takes */
    int irq_path;                       /* the port has the interrupt */
    int isr_no_wake;                    /* the handler runs (FENCE_EV cleared) but wakes nobody */
    unsigned drop_word;                 /* the CMD write with this number (1, 2, ...) is lost: OVERFLOW */
    unsigned corrupt_word;              /* this CMD write arrives with opcode 0x0F instead */
    uint32_t corrupt_value;             /* or (corrupt_value_at) arrives as this word */
    unsigned corrupt_value_at;
    uint64_t intr_at;                   /* Ctrl+C from this time on (0: never) */
    int other_holds;                    /* another claimant has the driver until other_until */
    uint64_t other_until;
    const uint32_t *other_words;        /* what it sends just before it lets go */
    unsigned other_n;
    int locked;                         /* this claimant has it */
    int text_locks;                     /* text_lock() minus text_unlock() */
    int in_atomic;
    /* records */
    unsigned long long op_px[64];       /* the pixels the engine wrote, by the opcode of the command */
    unsigned cur_op;                    /* the command the engine is in */
    uint32_t *cmd;                      /* every CMD word that reached the FIFO, in order */
    unsigned ncmd, cmd_writes;          /* words logged; CMD writes made (dropped ones too) */
    unsigned writes_full;               /* CMD writes into a full FIFO (the driver's fault, not a knob's) */
    unsigned writes_blind;              /* CMD writes beyond the room FIFO_FREE last said */
    unsigned free_said;                 /* room left from the last FIFO_FREE read */
    unsigned full_seen;                 /* FIFO_FREE reads that said 0 */
    unsigned aborts, fifo_free_reads, irq_waits, sleeps, relaxes, interrupts_sent;
    unsigned other_done;                /* the other claimant's words went in */
    unsigned cmd_at_other;              /* ncmd when they did */
    unsigned shown_draws;               /* engine words that drew into the page being shown */
    int check_tearing;                  /* look for those (the demo's layout) */
    int event;                          /* the GPU's interrupt came since irq_arm() */
    uint64_t crc_frame;                 /* the frame the last FRAME_CRC read was of */
    unsigned bad_vram;                  /* window accesses outside VRAM or unaligned */
    unsigned mode_unlocked;             /* set_mode() without the console lock or outside an atomic stretch */
    unsigned claims, claimed_twice, unlocked_free;
    uint64_t abort_at, fence_read_at;   /* the first ABORT, the first read of FENCE */
    int no_memory;                      /* alloc() fails */
    int watch;                          /* keep the events below */
    struct stub_event *ev;
    unsigned nev, ev_cap;
    const char *holder;                 /* the other claimant as the port names it ('Shell 2', pid 9 when NULL) */
    /* 10/26/26: another console's run at a moment of this one's (the test's function: what it does
       with the driver), at STUB_AT_ABORT (an ABORT was written) and STUB_AT_UNLOCK (the claim let go) */
    void (*hook)(struct stub *s, int where);
    int in_hook;
    unsigned hooks_run;
    char out[1 << 16];                  /* what the driver and gfx printed */
    unsigned nout;
    unsigned col, line_start;           /* the line being printed: its columns so far, where it began in out */
    unsigned widest;                    /* the widest line printed */
};

#define STUB_AT_ABORT   1
#define STUB_AT_UNLOCK  2

#define STUB_COLUMNS 80                 /* the console's line (GPU.md 15.2) */
/* the lines printed wider than that since the last call (any stub), and the first of them */
unsigned stub_too_wide(const char **first);

/* a fresh stand-in: GPU1, FEATURES with the GPU and IRQ, VRAM zero, GPAL RGB332, the reset values */
void stub_init(struct stub *s, int irq_path);
void stub_free(struct stub *s);
/* the display a frame shows that began at frame k's start (what latch k - 1 took) */
struct stub_disp stub_shown(const struct stub *s, uint64_t frame);
uint64_t stub_frame_of(uint64_t t);     /* the frame t is in */
struct stub_disp stub_visible(const struct stub *s, uint64_t t);    /* what the screen shows at t */
/* the RGB444 picture the display shows now (a still picture), and its FRAME_CRC */
void stub_render(const struct stub *s, const struct stub_disp *d, uint16_t *frame);
uint32_t stub_crc(const struct stub *s, const struct stub_disp *d);
/* run the engine until it is idle (or time max_us passes) */
void stub_drain(struct stub *s, uint64_t max_us);

/* gt_printf (the driver's and gfx's printf in this build) writes into the stub that is current */
void stub_current(struct stub *s);
int gt_printf(const char *fmt, ...);
/* gfx with a line of words, split as the kernel shell splits them, on driver g over stub s; the
   output in s->out (cleared first) */
int stub_gfx(struct stub *s, struct gpu *g, const char *line);

#endif
