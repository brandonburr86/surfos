/*
SurfGPU reference model
--------------------
File: gpu_model.h Date: 10/18/26 (SurfGPU)
--------------------
The C reference model of docs/GPU.md: one implementation, used by the Verilator tests of
surf_video, surf_vram and surf_gpu, by surfsim's checks and by the firmware's host tools (the
expected FRAME_CRCs of gpudiag's scenes). Plain C99, no simulator dependency.

The display path (docs/GPU.md section 3, IO.md section 3): gpu_display_render() makes the
640x480 RGB444 frame surf_video puts on the pins in every mode (the test pattern, text, the
graphics modes with GPAL, GFX_BASE, GFX_STRIDE, GFX_SCALE and the text overlay), from the
settings a frame latch took, VRAM and text RAM as they are during the frame (a still
picture), and the font. gpu_frame_crc() is FRAME_CRC of such a frame.

The engine (docs/GPU.md section 5): struct gpu_engine takes command words one at a time,
exactly as surf_gpu's FIFO hands them over, and draws on a 307,200-byte VRAM array; it keeps
the target, the clip, FENCE and the sticky status bits as the hardware does. It has no timing:
WAIT_VBLANK does nothing in it.
*/
#ifndef SURFGPU_GPU_MODEL_H
#define SURFGPU_GPU_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "surfcpu/gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPU_VRAM_BYTES SURF_VRAM_BYTES   /* surf_vram's size */
#define GPU_SCREEN_W   640
#define GPU_SCREEN_H   480
#define GPU_TEXT_COLS  80
#define GPU_TEXT_ROWS  30

/* ------------------------------------------------------------------ the display path */

struct gpu_display {
    int enable;                 /* CTRL.ENABLE */
    int mode;                   /* CTRL.MODE: 0 bars, 1 text, 2 graphics, 3 graphics + text */
    int cursor;                 /* the cursor shows in this frame (CURSOR_EN, and the blink phase) */
    int cur_col, cur_row;       /* CURSOR */
    int scroll;                 /* SCROLL as written (30 and 31 act as 0) */
    uint16_t pal[16];           /* PALETTE, RGB444 */
    uint32_t gfx_base;          /* GFX_BASE (bits 1:0 ignored) */
    uint32_t gfx_stride;        /* GFX_STRIDE (bits 1:0 ignored) */
    uint32_t gfx_scale;         /* GFX_SCALE: 2 doubles, anything else is 1 */
    int has_gfx;                /* surf_video's HAS_GFX: without it modes 2 and 3 are black */
    uint16_t gpal[256];         /* GPAL, RGB444 */
};

/* the values after a reset (GPAL: its configuration contents, RGB332 widened) */
void gpu_display_reset(struct gpu_display *d, int has_gfx);
uint16_t gpu_gpal_initial(unsigned n);
/* the colour-bar test pattern at (x, y) */
uint16_t gpu_test_pattern(int x, int y);
/* the frame: vram (GPU_VRAM_BYTES, or NULL: all zero), textram (cells 0..2399, NULL: all
 * zero), font (256 glyphs of 16 rows, MSB = leftmost pixel); frame gets 640 x 480 RGB444 */
void gpu_display_render(const struct gpu_display *d, const uint8_t *vram, const uint32_t *textram,
                        const uint8_t font[256][16], uint16_t *frame);
/* zlib's crc32 (crc = 0 to start) and FRAME_CRC of a 640 x 480 frame */
uint32_t gpu_crc32(uint32_t crc, const void *buf, size_t n);
uint32_t gpu_frame_crc(const uint16_t *frame);

/* ------------------------------------------------------------------ the engine */

/* opcodes and header fields: firmware/include/surfcpu/gpu.h (GPU_OP_*, GPU_COPY_KEY, ...) */
/* STATUS bits */
#define GPU_ST_BUSY         (1u << 0)
#define GPU_ST_OVERFLOW     (1u << 1)
#define GPU_ST_BAD_OPCODE   (1u << 2)
#define GPU_ST_BAD_PARAM    (1u << 3)
#define GPU_ST_FENCE_EV     (1u << 4)

struct gpu_engine {
    uint8_t *vram;              /* GPU_VRAM_BYTES, the caller's */
    /* the target and clip */
    uint32_t base, stride, tw, th;
    uint32_t cx0, cy0, cx1, cy1;
    /* registers the command stream changes */
    uint32_t fence;
    uint32_t sticky;            /* GPU_ST_BAD_OPCODE | GPU_ST_BAD_PARAM | GPU_ST_FENCE_EV */
    unsigned fences;            /* FENCE commands completed (for tests) */
    /* the command being collected */
    uint32_t hdr, p[3];
    unsigned np, need;          /* parameter words held, needed */
    int in_cmd;
    /* EXPAND, WRITE_RECT: the data stream */
    uint64_t data_left;         /* pixels still to come */
    uint32_t dx, dy, dw, dh;    /* the rectangle (dx, dy signed 16-bit), as given */
    uint64_t pix_done;          /* pixels of it taken */
    unsigned long long writes;  /* pixel writes (for tests) */
};

void gpu_engine_init(struct gpu_engine *g, uint8_t *vram);  /* the state after a reset */
void gpu_engine_word(struct gpu_engine *g, uint32_t w);      /* one CMD word, as the FIFO gives it */
void gpu_engine_abort(struct gpu_engine *g);                 /* CTRL.ABORT */
int gpu_engine_idle(const struct gpu_engine *g);             /* no command partly taken */

#ifdef __cplusplus
}
#endif

#endif
