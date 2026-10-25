/*
SurfOS board: SurfCPU on the Arty A7 -- the GPU driver
--------------------
File: gpu.c     Date: 10/25/26 (SurfGPU G3)
--------------------
The driver of gpu.h: the FIFO_FREE rule, fences and their waits, the sticky errors, the claim,
the target and clip, the drawing commands of surfcpu/docs/GPU.md section 5 as command words,
the display and its frame latch rule, the palettes, FRAME_CRC of a still picture, and pixels
through the CPU's VRAM window. Pure code: every register access, wait and lock goes through
struct gpu_port, so boards/arty_a7/tests/gpu/ runs this file against the C model of the engine
(sim/models/gpu_model.c) on the host, and boards/arty_a7/gpuhw.c gives it the hardware.

What it prints (on the console of the command that called it, as every kernel command does) is
what went wrong at the engine: a stopped engine, the sticky errors in words, a display change
that found no vertical blanking. The caller says what it was doing.
*/

#include <surfos/types.h>
#include <stdio.h>
#include <string.h>
#include <surfcpu/soc.h>
#include <surfcpu/gpu.h>
#include <sys/framebuffer.h>            /* the console's own font (driver/font8x16.c) */
#include "gpu.h"

#define P(g)            ((g)->port)
#define CTX(g)          ((g)->port->ctx)
#define RD(g, off)      ((g)->port->rd(CTX(g), (off)))
#define WR(g, off, v)   ((g)->port->wr(CTX(g), (off), (v)))
#define VRD(g, off)     ((g)->port->vrd(CTX(g), (off)))
#define VWR(g, off, v)  ((g)->port->vwr(CTX(g), (off), (v)))
#define NOW(g)          ((g)->port->us(CTX(g)))

#define ST_ERRORS (SURF_GPU_STATUS_OVERFLOW_MASK | SURF_GPU_STATUS_BAD_OPCODE_MASK | SURF_GPU_STATUS_BAD_PARAM_MASK)
#define ST_STICKY (ST_ERRORS | SURF_GPU_STATUS_FENCE_EV_MASK)

/* the target and clip a reset leaves (GPU.md section 5), which gpu_reset() sets again */
#define RESET_W         640u
#define RESET_H         480u

const char *gpu_id_name(u32 id) {
    if(id == GPUDRV_ID_GPU1) return "GPU1";
    if(id == GPUDRV_ID_GPU2) return "GPU2";
    return NULL;
}

static bool interrupted(struct gpu *g) {
    return !g->keep_going && P(g)->interrupted(CTX(g));
}

/**** finding it ****/

int gpu_probe(struct gpu *g, const struct gpu_port *port, u32 features) {
    u32 info;
    memset(g, 0, sizeof(*g));
    g->port = port;
    g->features = features;
    if(!(features & SURF_SYSINFO_FEATURES_GPU_MASK)) {
        g->why = "sysinfo.FEATURES has no GPU (a bitstream without one)";
        return GPU_EABSENT;
    }
    if(!(features & SURF_SYSINFO_FEATURES_VIDEO_MASK)) {
        g->why = "sysinfo.FEATURES has the GPU but no video block";
        return GPU_EABSENT;
    }
    g->id = RD(g, SURF_GPU_ID);
    if(!gpu_id_name(g->id)) {
        g->why = "its gpu.ID is not GPU1 or GPU2";
        return GPU_EABSENT;
    }
    info = RD(g, SURF_GPU_INFO);
    g->fifo_depth = (info & SURF_GPU_INFO_FIFO_DEPTH_MASK) >> SURF_GPU_INFO_FIFO_DEPTH_LSB;
    g->vram_bytes = ((info & SURF_GPU_INFO_VRAM_KIB_MASK) >> SURF_GPU_INFO_VRAM_KIB_LSB) * 1024u;
    if(!g->fifo_depth || g->vram_bytes != SURF_VRAM_BYTES) {
        g->why = "gpu.INFO gives no FIFO, or not the map's 300 KiB of VRAM";
        return GPU_EABSENT;
    }
    g->irq = port->irq_arm && port->irq_wait;
    g->spin_us = GPU_SPIN_US;
    g->fence_sent = RD(g, SURF_GPU_FENCE);
    WR(g, SURF_GPU_IE, g->irq ? SURF_GPU_IE_FENCE_MASK : 0);
    g->present = true;
    return gpu_reset(g, "gpu") ? GPU_ETIMEOUT : GPU_OK;
}

/**** the claim: one command's run at a time ****/

int gpu_claim(struct gpu *g, const char *what) {
    char who[64];
    bool said = false;
    while(!P(g)->trylock(CTX(g))) {
        if(!said) {
            printf("    %s: the GPU is in use (%s): waiting; Ctrl+C gives up\n", what,
                   P(g)->holder(CTX(g), who, sizeof(who)));
            said = true;
        }
        if(P(g)->sleep(CTX(g), 20) == GPU_EINTR || P(g)->interrupted(CTX(g))) return GPU_EINTR;
    }
    return GPU_OK;
}

void gpu_release(struct gpu *g) {
    g->keep_going = false;
    P(g)->unlock(CTX(g));
}

/**** errors and a stopped engine ****/

/* ABORT, then the target and clip the driver set queued again (an ABORT keeps the engine's, but
   it may have cut a SET_TARGET or SET_CLIP short): they reach the engine before anything sent
   after them. Nothing waits here (the FIFO is empty after an ABORT), so a stopped engine costs no
   second timeout and Ctrl+C is not held up. */
int gpu_abort(struct gpu *g, const char *what) {
    u32 w[7];
    int r;
    WR(g, SURF_GPU_CTRL, SURF_GPU_CTRL_ABORT_MASK);
    g->credit = 0;
    g->aborts++;
    if(g->restoring) return GPU_OK;
    g->restoring = true;
    w[0] = GPU_HDR(GPU_OP_SET_TARGET, 0);
    w[1] = g->t_base;
    w[2] = g->t_stride;
    w[3] = GPU_WH(g->t_w, g->t_h);
    w[4] = GPU_HDR(GPU_OP_SET_CLIP, 0);
    w[5] = GPU_XY(g->c_x0, g->c_y0);
    w[6] = GPU_XY(g->c_x1, g->c_y1);
    r = gpu_put(g, w, 7, what);
    g->restoring = false;
    return r;
}

static int stopped(struct gpu *g, const char *what, const char *how, u32 waiting_for) {
    char buf[48];
    u32 st = RD(g, SURF_GPU_STATUS), ff = RD(g, SURF_GPU_FIFO_FREE), fence = RD(g, SURF_GPU_FENCE);
    g->timeouts++;
    if(waiting_for) snprintf(buf, sizeof(buf), ", waiting for %lu", (u_long)waiting_for);
    else buf[0] = 0;
    printf("    %s: the GPU %s for 2 seconds: a stopped engine; ABORT\n"
           "    (STATUS 0x%lx, FIFO_FREE %lu, FENCE %lu%s)\n", what, how, (u_long)st, (u_long)ff, (u_long)fence, buf);
    gpu_abort(g, what);
    return GPU_ETIMEOUT;
}

static int stop_intr(struct gpu *g, const char *what) {
    g->ctrl_c++;
    gpu_abort(g, what);
    return GPU_EINTR;
}

int gpu_check(struct gpu *g, const char *what) {
    char msg[300];
    u32 st;
    if(!g->present) return GPU_EABSENT;
    st = RD(g, SURF_GPU_STATUS) & ST_ERRORS;
    if(!st) return GPU_OK;
    g->errors++;
    g->last_error = st;
    if(st & SURF_GPU_STATUS_OVERFLOW_MASK) g->overflows++;
    if(st & SURF_GPU_STATUS_BAD_OPCODE_MASK) g->bad_opcodes++;
    if(st & SURF_GPU_STATUS_BAD_PARAM_MASK) g->bad_params++;
    snprintf(msg, sizeof(msg), "%s%s%s",
             (st & SURF_GPU_STATUS_OVERFLOW_MASK) ? "      OVERFLOW: a command word found the FIFO full and was dropped\n" : "",
             (st & SURF_GPU_STATUS_BAD_OPCODE_MASK) ? "      BAD_OPCODE: a command word had an unknown opcode\n" : "",
             (st & SURF_GPU_STATUS_BAD_PARAM_MASK) ? "      BAD_PARAM: a command's parameters were refused (SET_TARGET)\n" : "");
    printf("    %s: the GPU reports:\n%s    %s: cleared; the engine ABORTed, its target and clip sent again\n", what, msg,
           what);
    WR(g, SURF_GPU_STATUS, st);
    gpu_abort(g, what);
    return GPU_EERROR;
}

int gpu_reset(struct gpu *g, const char *what) {
    int r;
    if(!g->present) return GPU_EABSENT;
    WR(g, SURF_GPU_CTRL, SURF_GPU_CTRL_ABORT_MASK);
    g->credit = 0;
    WR(g, SURF_GPU_STATUS, ST_STICKY);
    r = gpu_set_target(g, 0, RESET_W, RESET_W, RESET_H, what);
    return r ? r : gpu_sync(g, what);
}

bool gpu_busy(struct gpu *g) {
    return g->present && (RD(g, SURF_GPU_STATUS) & SURF_GPU_STATUS_BUSY_MASK);
}

/**** command words ****/

/* until FIFO_FREE says there is room; two seconds without a word taken is a stopped engine */
static int room(struct gpu *g, const char *what) {
    u64 t0 = NOW(g);
    for(;;) {
        u32 f = (RD(g, SURF_GPU_FIFO_FREE) & SURF_GPU_FIFO_FREE_FREE_MASK) >> SURF_GPU_FIFO_FREE_FREE_LSB;
        if(f) {
            g->credit = f < g->fifo_depth ? f : g->fifo_depth;
            return GPU_OK;
        }
        if(NOW(g) - t0 >= GPU_TIMEOUT_US) return stopped(g, what, "took no command word", 0);
        if(interrupted(g)) return stop_intr(g, what);
        P(g)->relax(CTX(g));
    }
}

/* read FIFO_FREE, write that many words, read it again */
int gpu_put(struct gpu *g, const u32 *w, u_int n, const char *what) {
    if(!g->present) return GPU_EABSENT;
    while(n) {
        u_int k;
        if(!g->credit) {
            int r = room(g, what);
            if(r) return r;
        }
        k = n < g->credit ? n : g->credit;
        g->credit -= k;
        g->words += k;
        n -= k;
        while(k--) WR(g, SURF_GPU_CMD, *w++);
    }
    return GPU_OK;
}

int gpu_fence_send(struct gpu *g, const char *what, u32 *value) {
    u32 w[2];
    w[0] = GPU_HDR(GPU_OP_FENCE, g->irq ? GPU_FENCE_IRQ : 0);
    w[1] = ++g->fence_sent;
    *value = w[1];
    g->fences++;
    return gpu_put(g, w, 2, what);
}

/* FENCE at or past value: polled for spin_us (most submissions are done by then; a task woken
   by the interrupt runs only at the next tick, up to 10 ms later), then the GPU's interrupt or
   sleeps; two seconds in which neither FENCE nor FIFO_FREE moves is a stopped engine */
int gpu_fence_wait(struct gpu *g, u32 value, const char *what) {
    u64 t0 = NOW(g), last = t0;
    u32 fence = ~value, room_seen = 0xFFFFFFFFu;
    if(!g->present) return GPU_EABSENT;
    for(;;) {
        u32 f, ff;
        u64 now;
        if(g->irq) P(g)->irq_arm(CTX(g));
        f = RD(g, SURF_GPU_FENCE);
        if((i32)(f - value) >= 0) return GPU_OK;
        ff = RD(g, SURF_GPU_FIFO_FREE);
        now = NOW(g);
        if(f != fence || ff != room_seen) {
            fence = f;
            room_seen = ff;
            last = now;
        } else if(now - last >= GPU_TIMEOUT_US) {
            return stopped(g, what, "finished nothing", value);
        }
        if(interrupted(g)) return stop_intr(g, what);
        if(now - t0 < g->spin_us) {
            P(g)->relax(CTX(g));
        } else if(g->irq) {
            g->irq_waits++;
            if(P(g)->irq_wait(CTX(g), 10) == GPU_EINTR && interrupted(g)) return stop_intr(g, what);
        } else {
            g->sleeps++;
            if(P(g)->sleep(CTX(g), 1) == GPU_EINTR && interrupted(g)) return stop_intr(g, what);
        }
    }
}

/* every command so far done, and no error: a fence, its wait, the sticky bits (also after a
   stopped engine: an OVERFLOW is often why) */
int gpu_sync(struct gpu *g, const char *what) {
    u32 v;
    int r = gpu_fence_send(g, what, &v), c;
    if(!r) r = gpu_fence_wait(g, v, what);
    if(r == GPU_EABSENT) return r;
    c = gpu_check(g, what);
    return r ? r : c;
}

/**** drawing ****/

/* SET_TARGET's own rule (GPU.md section 5): every pixel in VRAM, no two lines in one word */
static bool target_ok(u32 base, u32 stride, u32 w, u32 h) {
    if((base & 3) || (stride & 3) || base >= SURF_VRAM_BYTES || stride >= 65536u || w >= 32768u || h >= 32768u)
        return false;
    if(!w || !h) return true;
    return (w <= stride || h == 1) && (u64)base + (u64)(h - 1) * stride + w <= SURF_VRAM_BYTES;
}

int gpu_set_target(struct gpu *g, u32 base, u32 stride, u32 w, u32 h, const char *what) {
    u32 cmd[4];
    if(!g->present) return GPU_EABSENT;
    if(!target_ok(base, stride, w, h)) return GPU_EARG;
    cmd[0] = GPU_HDR(GPU_OP_SET_TARGET, 0);
    cmd[1] = base;
    cmd[2] = stride;
    cmd[3] = GPU_WH(w, h);
    g->t_base = base;
    g->t_stride = stride;
    g->t_w = w;
    g->t_h = h;
    g->c_x0 = g->c_y0 = 0;
    g->c_x1 = w;
    g->c_y1 = h;
    return gpu_put(g, cmd, 4, what);
}

int gpu_set_clip(struct gpu *g, u32 x0, u32 y0, u32 x1, u32 y1, const char *what) {
    u32 cmd[3];
    if(!g->present) return GPU_EABSENT;
    if(x0 > 0xFFFFu || y0 > 0xFFFFu || x1 > 0xFFFFu || y1 > 0xFFFFu) return GPU_EARG;
    cmd[0] = GPU_HDR(GPU_OP_SET_CLIP, 0);
    cmd[1] = GPU_XY(x0, y0);
    cmd[2] = GPU_XY(x1, y1);
    g->c_x0 = x0;
    g->c_y0 = y0;
    g->c_x1 = x1;
    g->c_y1 = y1;
    return gpu_put(g, cmd, 3, what);
}

static bool xy_ok(i32 v) { return v >= -32768 && v <= 32767; }
static bool wh_ok(u32 v) { return v <= 0xFFFFu; }

int gpu_fill(struct gpu *g, i32 x, i32 y, u32 w, u32 h, u8 c, const char *what) {
    u32 cmd[3];
    if(!xy_ok(x) || !xy_ok(y) || !wh_ok(w) || !wh_ok(h)) return GPU_EARG;
    cmd[0] = GPU_HDR(GPU_OP_FILL_RECT, c);
    cmd[1] = GPU_XY(x, y);
    cmd[2] = GPU_WH(w, h);
    return gpu_put(g, cmd, 3, what);
}

int gpu_line(struct gpu *g, i32 x0, i32 y0, i32 x1, i32 y1, u8 c, const char *what) {
    u32 cmd[3];
    if(!xy_ok(x0) || !xy_ok(y0) || !xy_ok(x1) || !xy_ok(y1)) return GPU_EARG;
    cmd[0] = GPU_HDR(GPU_OP_LINE, c);
    cmd[1] = GPU_XY(x0, y0);
    cmd[2] = GPU_XY(x1, y1);
    return gpu_put(g, cmd, 3, what);
}

/* the outline of [x, x + w) x [y, y + h): top, right, bottom and left edges; its far corner must
   be a coordinate too (nothing wraps) */
int gpu_rect(struct gpu *g, i32 x, i32 y, u32 w, u32 h, u8 c, const char *what) {
    u32 cmd[12];
    i32 x1 = x + (i32)w - 1, y1 = y + (i32)h - 1;
    if(!xy_ok(x) || !xy_ok(y) || !wh_ok(w) || !wh_ok(h)) return GPU_EARG;
    if(!w || !h) return GPU_OK;
    if(!xy_ok(x1) || !xy_ok(y1)) return GPU_EARG;
    cmd[0] = cmd[3] = cmd[6] = cmd[9] = GPU_HDR(GPU_OP_LINE, c);
    cmd[1] = GPU_XY(x, y);
    cmd[2] = GPU_XY(x1, y);
    cmd[4] = GPU_XY(x1, y);
    cmd[5] = GPU_XY(x1, y1);
    cmd[7] = GPU_XY(x1, y1);
    cmd[8] = GPU_XY(x, y1);
    cmd[10] = GPU_XY(x, y1);
    cmd[11] = GPU_XY(x, y);
    return gpu_put(g, cmd, 12, what);
}

int gpu_copy(struct gpu *g, i32 sx, i32 sy, i32 dx, i32 dy, u32 w, u32 h, int key, const char *what) {
    u32 cmd[4];
    if(!xy_ok(sx) || !xy_ok(sy) || !xy_ok(dx) || !xy_ok(dy) || !wh_ok(w) || !wh_ok(h) || key > 255) return GPU_EARG;
    cmd[0] = GPU_HDR(GPU_OP_COPY_RECT, key >= 0 ? (GPU_COPY_KEY | (u32)key) : 0);
    cmd[1] = GPU_XY(sx, sy);
    cmd[2] = GPU_XY(dx, dy);
    cmd[3] = GPU_WH(w, h);
    return gpu_put(g, cmd, 4, what);
}

static u32 expand_hdr(u8 fg, int bg) {
    return GPU_HDR(GPU_OP_EXPAND, (u32)fg | (bg < 0 ? GPU_EXPAND_TRANSP : (u32)(bg & 0xFF) << 8));
}

/* bits: ceil(w h / 32) words, the first pixel in bit 31 */
int gpu_expand(struct gpu *g, i32 x, i32 y, u32 w, u32 h, u8 fg, int bg, const u32 *bits, const char *what) {
    u32 cmd[3];
    int r;
    if(!xy_ok(x) || !xy_ok(y) || !wh_ok(w) || !wh_ok(h) || bg > 255) return GPU_EARG;
    cmd[0] = expand_hdr(fg, bg);
    cmd[1] = GPU_XY(x, y);
    cmd[2] = GPU_WH(w, h);
    r = gpu_put(g, cmd, 3, what);
    return r ? r : gpu_put(g, bits, (u_int)(((u64)w * h + 31) / 32), what);
}

u32 gpu_text_width(u_int n) {
    return (u32)n * FONT_WIDTH;
}

/* one EXPAND of n cells, 8n x 16: row after row of the console font's bytes, four to a word, as
   GPU.md section 5 says a font row byte goes in (MSB first) */
int gpu_text(struct gpu *g, i32 x, i32 y, u8 fg, int bg, const u8 *cells, u_int n, const char *what) {
    u32 cmd[3], buf[16], word = 0;
    u_int row, k, nb = 0, bytes = 0;
    int r;
    if(!n) return GPU_OK;
    if(!xy_ok(x) || !xy_ok(y) || gpu_text_width(n) > 0xFFFFu || bg > 255) return GPU_EARG;
    cmd[0] = expand_hdr(fg, bg);
    cmd[1] = GPU_XY(x, y);
    cmd[2] = GPU_WH(gpu_text_width(n), FONT_HEIGHT);
    r = gpu_put(g, cmd, 3, what);
    for(row = 0; row < FONT_HEIGHT && !r; row++) {
        for(k = 0; k < n && !r; k++) {
            word = word << 8 | font8x16[cells[k]][row];
            if(++bytes % 4) continue;
            buf[nb++] = word;
            word = 0;
            if(nb == 16) {
                r = gpu_put(g, buf, nb, what);
                nb = 0;
            }
        }
    }
    if(!r && nb) r = gpu_put(g, buf, nb, what);        /* 16 n bytes: always whole words */
    return r;
}

int gpu_wait_vblank(struct gpu *g, const char *what) {
    u32 cmd = GPU_HDR(GPU_OP_WAIT_VBLANK, 0);
    return gpu_put(g, &cmd, 1, what);
}

/**** the display ****/

const char *gpu_mode_name(u32 mode) {
    switch(mode) {
    case GPU_MODE_BARS: return "the test pattern";
    case GPU_MODE_TEXT: return "text";
    case GPU_MODE_GFX: return "graphics";
    case GPU_MODE_OVERLAY: return "graphics with the text over it";
    }
    return "?";
}

int gpu_screen_get(struct gpu *g, struct gpu_screen *d) {
    if(!g->present) return GPU_EABSENT;
    d->mode = P(g)->get_mode(CTX(g));
    d->base = VRD(g, SURF_VIDEO_GFX_BASE) & SURF_VIDEO_GFX_BASE_BASE_MASK;
    d->stride = VRD(g, SURF_VIDEO_GFX_STRIDE) & SURF_VIDEO_GFX_STRIDE_STRIDE_MASK;
    d->scale = VRD(g, SURF_VIDEO_GFX_SCALE) & SURF_VIDEO_GFX_SCALE_SCALE_MASK;
    return GPU_OK;
}

/* GFX_BASE, GFX_STRIDE, GFX_SCALE and the mode, written together just after a vertical blanking
   began: GPU.md section 3's frame latch rule (stores between VBLANK_EV and 1.3 ms after it change
   one frame together). The blanking is caught by sampling the clock and STATUS.FRAME (it counts
   as blanking begins) as one atomic pair: when the count moved between two samples taken less
   than GPU_LATCH_GAP_US apart, the blanking began less than that ago, and the registers are
   written at once in the same atomic stretch. A pair further apart (an interrupt, another task)
   is not used. The console lock is held throughout, so no task rewrites the text console's copy
   of CTRL meanwhile. */
int gpu_screen_set(struct gpu *g, const struct gpu_screen *d, const char *what) {
    const struct gpu_port *p = P(g);
    u64 t0, prev_t = 0;
    u32 prev_f = 0;
    bool have = false, caught = false;
    int how;
    if(!g->present) return GPU_EABSENT;
    if(d->mode > GPU_MODE_OVERLAY || (d->base & 3) || d->base > SURF_VIDEO_GFX_BASE_BASE_MASK || (d->stride & 3) ||
       d->stride > SURF_VIDEO_GFX_STRIDE_STRIDE_MASK || d->scale > SURF_VIDEO_GFX_SCALE_SCALE_MASK)
        return GPU_EARG;
    how = p->text_lock(p->ctx);
    t0 = NOW(g);
    for(;;) {
        u_long flags = p->atomic_begin(p->ctx);
        u64 t = NOW(g);
        u32 f = (VRD(g, SURF_VIDEO_STATUS) & SURF_VIDEO_STATUS_FRAME_MASK) >> SURF_VIDEO_STATUS_FRAME_LSB;
        if(have && f != prev_f && t - prev_t <= GPU_LATCH_GAP_US) caught = true;
        if(caught || t - t0 >= GPU_LATCH_LIMIT_US) {
            VWR(g, SURF_VIDEO_GFX_BASE, d->base);
            VWR(g, SURF_VIDEO_GFX_STRIDE, d->stride);
            VWR(g, SURF_VIDEO_GFX_SCALE, d->scale);
            p->set_mode(p->ctx, d->mode);
            p->atomic_end(p->ctx, flags);
            break;
        }
        p->atomic_end(p->ctx, flags);
        prev_f = f;
        prev_t = t;
        have = true;
    }
    p->text_unlock(p->ctx, how);
    g->display_changes++;
    g->latched = caught;
    if(!caught) {
        g->latch_misses++;
        printf("    %s: no vertical blanking seen in %lu ms: the registers written at once\n"
               "    (the change may take two frames)\n", what, (u_long)(GPU_LATCH_LIMIT_US / 1000));
    }
    return GPU_OK;
}

/**** the palette ****/

/* the eight hues of GPU_PAL_RAMPS, RGB444 at full strength */
static const u16 ramp_hue[8] = { 0xFFF, 0xF00, 0x0F0, 0x00F, 0xFF0, 0x0FF, 0xF0F, 0xF80 };

void gpu_pal_make(int which, u16 *pal) {
    u32 n;
    for(n = 0; n < 256; n++) {
        if(which == GPU_PAL_GREY) {
            pal[n] = (u16)((n >> 4) * 0x111u);
        } else if(which == GPU_PAL_RAMPS) {
            u32 hue = ramp_hue[n >> 5], i = n & 31, r = hue >> 8, gg = (hue >> 4) & 15, b = hue & 15;
            pal[n] = (u16)(((r * i + 15) / 31) << 8 | ((gg * i + 15) / 31) << 4 | (b * i + 15) / 31);
        } else {                        /* GPU.md section 3: R4 = {R, R[2]}, G4 = {G, G[2]}, B4 = {B, B} */
            u32 r = n >> 5, gg = (n >> 2) & 7, b = n & 3;
            pal[n] = (u16)((r << 1 | r >> 2) << 8 | (gg << 1 | gg >> 2) << 4 | (b << 2 | b));
        }
    }
}

void gpu_pal_read(struct gpu *g, u16 *pal) {
    u32 n;
    for(n = 0; n < 256; n++) pal[n] = (u16)(VRD(g, SURF_VIDEO_GPAL + 4 * n) & 0xFFFu);
}

void gpu_pal_write(struct gpu *g, const u16 *pal) {
    u32 n;
    for(n = 0; n < 256; n++) VWR(g, SURF_VIDEO_GPAL + 4 * n, pal[n] & 0xFFFu);
}

void gpu_pal_set(struct gpu *g, u32 n, u16 rgb) {
    VWR(g, SURF_VIDEO_GPAL + 4 * (n & 0xFF), rgb & 0xFFFu);
}

const char *gpu_pal_describe(struct gpu *g, char *buf, u_int size) {
    static const char *const names[3] = { "rgb332 (as the configuration leaves it)", "grey (16 levels)",
                                          "ramps (8 hues of 32 shades)" };
    u16 now[256], named[256];
    u_int differ = 0, best = 256, which, n;
    gpu_pal_read(g, now);
    for(which = 0; which < 3; which++) {
        gpu_pal_make((int)which, named);
        for(n = 0, differ = 0; n < 256; n++) differ += now[n] != named[n];
        if(!differ) {
            snprintf(buf, size, "%s", names[which]);
            return buf;
        }
        if(!which) best = differ;
    }
    snprintf(buf, size, "its own (%u of 256 entries differ from rgb332)", best);
    return buf;
}

/**** frames ****/

/* GPU.md section 3, a still picture: after the last change, c0 = FRAME_COUNT; once FRAME_COUNT is
   two on, the frame that read counted began after the change, and FRAME_CRC is that frame's */
int gpu_still_crc(struct gpu *g, u32 *count, u32 *crc, const char *what) {
    u32 c0, c;
    u64 t0;
    int r = gpu_sync(g, what);
    if(r) return r;
    c0 = VRD(g, SURF_VIDEO_FRAME_COUNT) & SURF_VIDEO_FRAME_COUNT_COUNT_MASK;
    t0 = NOW(g);
    for(;;) {
        c = VRD(g, SURF_VIDEO_FRAME_COUNT) & SURF_VIDEO_FRAME_COUNT_COUNT_MASK;
        if(((c - c0) & SURF_VIDEO_FRAME_COUNT_COUNT_MASK) >= 2) break;
        if(NOW(g) - t0 >= 200000u) {
            printf("    %s: FRAME_COUNT stayed at %lu for 200 ms: no frames are being shown\n", what, (u_long)c);
            return GPU_ETIMEOUT;
        }
        if(P(g)->sleep(CTX(g), 5) == GPU_EINTR && interrupted(g)) return GPU_EINTR;
    }
    *count = c;
    *crc = VRD(g, SURF_VIDEO_FRAME_CRC);
    return GPU_OK;
}

/**** pixels through the CPU's window ****/

int gpu_pixel_put(struct gpu *g, u32 x, u32 y, u8 c, const char *what) {
    int r;
    if(!g->present) return GPU_EABSENT;
    if(x >= g->t_w || y >= g->t_h) return GPU_EARG;
    r = gpu_sync(g, what);                      /* after what was drawn before */
    if(!r) P(g)->vram_wr8(CTX(g), g->t_base + y * g->t_stride + x, c);
    return r;
}

int gpu_pixel_get(struct gpu *g, u32 x, u32 y, u8 *c, const char *what) {
    int r;
    if(!g->present) return GPU_EABSENT;
    if(x >= g->t_w || y >= g->t_h) return GPU_EARG;
    r = gpu_sync(g, what);
    if(!r) *c = P(g)->vram_rd8(CTX(g), g->t_base + y * g->t_stride + x);
    return r;
}

/**** the rest of what the command needs ****/

void gpu_state_read(struct gpu *g, struct gpu_state *s) {
    s->status = RD(g, SURF_GPU_STATUS);
    s->ie = RD(g, SURF_GPU_IE);
    s->fifo_free = (RD(g, SURF_GPU_FIFO_FREE) & SURF_GPU_FIFO_FREE_FREE_MASK) >> SURF_GPU_FIFO_FREE_FREE_LSB;
    s->fence = RD(g, SURF_GPU_FENCE);
    s->frame_count = VRD(g, SURF_VIDEO_FRAME_COUNT) & SURF_VIDEO_FRAME_COUNT_COUNT_MASK;
    s->frame_crc = VRD(g, SURF_VIDEO_FRAME_CRC);
}

u64 gpu_now_us(struct gpu *g) { return NOW(g); }
bool gpu_interrupted(struct gpu *g) { return interrupted(g); }
void gpu_flip(struct gpu *g, u32 base) { VWR(g, SURF_VIDEO_GFX_BASE, base & SURF_VIDEO_GFX_BASE_BASE_MASK); }
static bool vram_span(u32 off, u32 words) {
    return !(off & 3) && off <= SURF_VRAM_BYTES && words <= (SURF_VRAM_BYTES - off) / 4;
}

int gpu_vram_read(struct gpu *g, u32 off, u32 *dst, u32 words) {
    if(!vram_span(off, words)) return GPU_EARG;
    P(g)->vram_read(CTX(g), off, dst, words);
    return GPU_OK;
}

int gpu_vram_write(struct gpu *g, u32 off, const u32 *src, u32 words) {
    if(!vram_span(off, words)) return GPU_EARG;
    P(g)->vram_write(CTX(g), off, src, words);
    return GPU_OK;
}
void *gpu_alloc(struct gpu *g, u32 bytes) { return P(g)->alloc ? P(g)->alloc(CTX(g), bytes) : NULL; }
void gpu_free(struct gpu *g, void *p) { if(p && P(g)->free) P(g)->free(CTX(g), p); }
