/*
SurfOS board: SurfCPU on the Arty A7 -- the GPU driver
----------------------
File: gpu.h     Date: 10/25/26 (SurfGPU G3)
----------------------
SurfGPU (surfcpu/docs/GPU.md): 300 KiB of VRAM at 0x3000_0000, the graphics modes of the video
block (CTRL.MODE 2 and 3, GFX_BASE, GFX_STRIDE, GFX_SCALE, the 256-entry palette GPAL, FRAME_CRC)
and the drawing engine at 0x1000_9000, a 64-word command FIFO in front of it. Present when
sysinfo.FEATURES.GPU is set and gpu.ID reads "GPU1" (0x31555047) or "GPU2" (0x32555047, the
command set of milestone G4, which only adds commands); anything else is no GPU.

gpu.c is the driver itself, pure code over struct gpu_port: on the Arty the port is the hardware
(boards/arty_a7/gpuhw.c: the registers, the GPU's interrupt, the kernel's waits and its console
lock), in the host test a stand-in that hands the command words to the C model of the engine
(boards/arty_a7/tests/gpu/, sim/models/gpu_model.c). Its rules:

  * A command word goes to CMD only while FIFO_FREE says there is room: FIFO_FREE is read, that
    many words are written, then it is read again. Two seconds without a word taken is a stopped
    engine: ABORT, said, GPU_ETIMEOUT.
  * A submission that must be finished ends with a FENCE whose value the driver counts; the wait
    for it polls for a short while, then sleeps on the GPU's interrupt (IE.FENCE, intc source 5)
    or, without one, with sleeps. Two seconds without progress is a stopped engine, as above.
  * After a finished submission STATUS's sticky errors (OVERFLOW, BAD_OPCODE, BAD_PARAM) are read:
    any one is said in words, cleared, and the engine ABORTed back to a known state (its target
    and clip sent again, ahead of anything sent after them).
  * The driver is claimed for one command's run at a time (gpu_claim()), so the commands of two
    consoles never interleave.
  * The display: CTRL.MODE belongs to the text console's copy of CTRL (textcon.c), changed under
    the console lock through the port; GFX_BASE, GFX_STRIDE, GFX_SCALE and the mode are written
    together within a millisecond after a vertical blanking began (GPU.md section 3's frame latch
    rule), so the picture changes in one frame.
*/

#ifndef _ARTY_GPU_H
#define _ARTY_GPU_H

#include <surfos/types.h>

/* the two IDs, under names of the driver's own: surfcpu/gpu.h's GPU_ID_VALUE is the newest command
   set's ID, which G4 changes to "GPU2" */
#define GPUDRV_ID_GPU1  0x31555047u     /* gpu.ID "GPU1": GPU.md's command set (G2) */
#define GPUDRV_ID_GPU2  0x32555047u     /* "GPU2": G4's, a superset; the driver uses GPU1's commands */

#define GPU_OK          0
#define GPU_EABSENT    -1               /* no GPU in this bitstream */
#define GPU_ETIMEOUT   -2               /* a stopped engine: two seconds without progress (ABORTed) */
#define GPU_EINTR      -3               /* Ctrl+C or a kill: the engine was stopped (ABORT) */
#define GPU_EERROR     -4               /* the engine reported an error (said, cleared, ABORTed) */
#define GPU_EARG       -5               /* refused before anything was sent */
#define GPU_ENOMEM     -6

#define GPU_TIMEOUT_US  2000000u        /* without a word taken or a command finished: stopped */
#define GPU_SPIN_US     2000u           /* a fence is polled this long before the driver sleeps (spin_us) */
#define GPU_LATCH_GAP_US 1000u          /* the frame latch rule: a blanking seen less than this after it began */
#define GPU_LATCH_LIMIT_US 250000u      /* how long a display change looks for a blanking */

/* video CTRL.MODE */
#define GPU_MODE_BARS    0
#define GPU_MODE_TEXT    1
#define GPU_MODE_GFX     2
#define GPU_MODE_OVERLAY 3

/* what the display shows: CTRL.MODE and the GFX_ registers */
struct gpu_screen {
    u32 mode;                           /* GPU_MODE_* */
    u32 base, stride, scale;            /* GFX_BASE, GFX_STRIDE (multiples of 4), GFX_SCALE (1 or 2) */
};

/* the named palettes (gpu_pal_make()) */
#define GPU_PAL_RGB332  0               /* what the configuration leaves: n = RRRGGGBB */
#define GPU_PAL_GREY    1               /* 16 greys, 16 entries each: n >> 4 */
#define GPU_PAL_RAMPS   2               /* 8 hues x 32 shades, black to full (G4's Gouraud) */

/* What the driver needs of the machine. Register offsets are soc.h's SURF_GPU_* and SURF_VIDEO_*,
   VRAM offsets bytes from SURF_VRAM_BASE. */
struct gpu_port {
    void *ctx;
    u32 (*rd)(void *ctx, u32 off);                      /* a gpu register */
    void (*wr)(void *ctx, u32 off, u32 v);
    u32 (*vrd)(void *ctx, u32 off);                     /* a video register */
    void (*vwr)(void *ctx, u32 off, u32 v);
    /* words of VRAM through the CPU's window, in a tight loop (off and the length multiples of 4) */
    void (*vram_read)(void *ctx, u32 off, u32 *dst, u32 words);
    void (*vram_write)(void *ctx, u32 off, const u32 *src, u32 words);
    u8 (*vram_rd8)(void *ctx, u32 off);                 /* a VRAM byte */
    void (*vram_wr8)(void *ctx, u32 off, u8 v);
    u64 (*us)(void *ctx);                               /* microseconds, from any start */
    void (*relax)(void *ctx);                           /* between two polls: other tasks may run */
    int (*sleep)(void *ctx, u32 ms);                    /* 0, or GPU_EINTR when Ctrl+C cut it short */
    bool (*interrupted)(void *ctx);                     /* Ctrl+C (or a kill) reached the command */
    /* the GPU's interrupt (FENCE_EV); NULL when the board's interrupt path has none */
    void (*irq_arm)(void *ctx);                         /* forget what came so far */
    int (*irq_wait)(void *ctx, u32 ms);                 /* until one since irq_arm(), or ms: 0, GPU_EINTR */
    /* a stretch of a few register accesses that nothing may come between (interrupts off) */
    u_long (*atomic_begin)(void *ctx);
    void (*atomic_end)(void *ctx, u_long flags);
    /* CTRL.MODE, through the text console's copy of CTRL (textcon.c): text_lock() takes the console
       lock, set_mode() is called with it held and inside atomic_begin() */
    int (*text_lock)(void *ctx);
    void (*text_unlock)(void *ctx, int how);
    u32 (*get_mode)(void *ctx);
    void (*set_mode)(void *ctx, u32 mode);
    /* the claim: one command's run at a time */
    bool (*trylock)(void *ctx);
    void (*unlock)(void *ctx);
    const char *(*holder)(void *ctx, char *buf, u_int size);   /* who has it, in words */
    /* memory for the demo's copy of VRAM; NULL when there is none */
    void *(*alloc)(void *ctx, u32 bytes);
    void (*free)(void *ctx, void *p);
};

struct gpu {
    const struct gpu_port *port;
    bool present;                       /* FEATURES.GPU and a known gpu.ID */
    bool irq;                           /* fences wait on the GPU's interrupt */
    const char *why;                    /* why there is no GPU */
    char why_buf[72];                   /* (with the value read) */
    u32 features;                       /* sysinfo.FEATURES */
    u32 id, fifo_depth, vram_bytes;
    u32 credit;                         /* FIFO words known to be free */
    u32 fence_sent;                     /* the last FENCE value sent */
    /* the engine's target and clip as the driver last set them: an ABORT keeps them, and they are
       sent again after one in case it cut a SET_TARGET or SET_CLIP short */
    u32 t_base, t_stride, t_w, t_h;
    u32 c_x0, c_y0, c_x1, c_y1;
    u32 spin_us;                        /* a fence is polled this long before the wait sleeps (GPU_SPIN_US) */
    bool latched;                       /* the last display change caught the frame latch */
    bool keep_going;                    /* waits go on through Ctrl+C (the demo putting the display back) */
    bool restoring;                     /* inside gpu_abort()'s resend of the target and clip */
    bool intr_aborted;                  /* this command's Ctrl+C stopped the engine (ABORT) */
    /* what happened, for gfx */
    u_long words, fences, irqs, irq_waits, sleeps, timeouts, aborts, errors, ctrl_c;
    u_long overflows, bad_opcodes, bad_params, display_changes, latch_misses;
    u32 last_error;                     /* the sticky bits of the last error */
};

/* the driver: FEATURES is sysinfo.FEATURES; GPU_OK when there is a GPU, else GPU_EABSENT with why set */
int gpu_probe(struct gpu *g, const struct gpu_port *port, u32 features);
const char *gpu_id_name(u32 id);        /* "GPU1", "GPU2" or NULL */

int gpu_claim(struct gpu *g, const char *what);    /* GPU_OK, or GPU_EINTR (said) */
void gpu_release(struct gpu *g);

/* command words, by the FIFO_FREE rule; then a fence and the error check: what names the command
   in what is said */
int gpu_put(struct gpu *g, const u32 *w, u_int n, const char *what);
int gpu_sync(struct gpu *g, const char *what);
int gpu_fence_send(struct gpu *g, const char *what, u32 *value);
int gpu_fence_wait(struct gpu *g, u32 value, const char *what);
int gpu_check(struct gpu *g, const char *what);    /* the sticky errors: GPU_OK or GPU_EERROR */
int gpu_abort(struct gpu *g, const char *what);    /* ABORT, then the target and clip queued again */
int gpu_reset(struct gpu *g, const char *what);    /* ABORT, sticky bits cleared, the reset target */
bool gpu_busy(struct gpu *g);

/* drawing, in target coordinates (GPU.md section 5: signed x, y; the clip cuts) */
int gpu_set_target(struct gpu *g, u32 base, u32 stride, u32 w, u32 h, const char *what);
int gpu_set_clip(struct gpu *g, u32 x0, u32 y0, u32 x1, u32 y1, const char *what);
int gpu_fill(struct gpu *g, i32 x, i32 y, u32 w, u32 h, u8 c, const char *what);
int gpu_line(struct gpu *g, i32 x0, i32 y0, i32 x1, i32 y1, u8 c, const char *what);
int gpu_rect(struct gpu *g, i32 x, i32 y, u32 w, u32 h, u8 c, const char *what);    /* an outline: four LINEs */
int gpu_copy(struct gpu *g, i32 sx, i32 sy, i32 dx, i32 dy, u32 w, u32 h, int key, const char *what); /* key < 0: none */
int gpu_expand(struct gpu *g, i32 x, i32 y, u32 w, u32 h, u8 fg, int bg, const u32 *bits, const char *what); /* bg < 0: transparent */
int gpu_text(struct gpu *g, i32 x, i32 y, u8 fg, int bg, const u8 *cells, u_int n, const char *what);  /* code page 437 cells */
int gpu_wait_vblank(struct gpu *g, const char *what);
u32 gpu_text_width(u_int n);           /* pixels of n cells */

/* the display */
int gpu_screen_get(struct gpu *g, struct gpu_screen *d);
int gpu_screen_set(struct gpu *g, const struct gpu_screen *d, const char *what);
const char *gpu_mode_name(u32 mode);

/* the palette */
void gpu_pal_make(int which, u16 *pal);                    /* 256 RGB444 entries */
void gpu_pal_read(struct gpu *g, u16 *pal);
void gpu_pal_write(struct gpu *g, const u16 *pal);
void gpu_pal_set(struct gpu *g, u32 n, u16 rgb);
const char *gpu_pal_describe(struct gpu *g, char *buf, u_int size);

/* FRAME_COUNT and FRAME_CRC of a frame that began after the call (GPU.md section 3's still picture) */
int gpu_still_crc(struct gpu *g, u32 *count, u32 *crc, const char *what);

/* one pixel of the target through the CPU's VRAM window */
int gpu_pixel_put(struct gpu *g, u32 x, u32 y, u8 c, const char *what);
int gpu_pixel_get(struct gpu *g, u32 x, u32 y, u8 *c, const char *what);

/* the registers as they are now (FRAME_COUNT, then FRAME_CRC: a consistent pair) */
struct gpu_state {
    u32 status, ie, fifo_free, fence, frame_count, frame_crc;
};
void gpu_state_read(struct gpu *g, struct gpu_state *s);

/* for the command's longer runs (gfx.c: bench, demo) */
u64 gpu_now_us(struct gpu *g);
bool gpu_interrupted(struct gpu *g);               /* Ctrl+C, unless keep_going */
void gpu_flip(struct gpu *g, u32 base);            /* GFX_BASE alone: the next frame latch takes it whole */
int gpu_vram_read(struct gpu *g, u32 off, u32 *dst, u32 words);      /* GPU_EARG outside VRAM */
int gpu_vram_write(struct gpu *g, u32 off, const u32 *src, u32 words);
void *gpu_alloc(struct gpu *g, u32 bytes);         /* NULL when there is none */
void gpu_free(struct gpu *g, void *p);

/* boards/arty_a7/gfx.c: the gfx command on a driver (CMD_OK, CMD_FAIL, CMD_USAGE) */
int gfx_command(struct gpu *g, int argc, char **argv);

#endif
