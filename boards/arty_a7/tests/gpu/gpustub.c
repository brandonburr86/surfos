/*
SurfOS host test: SurfGPU's driver -- the stand-in for the hardware
--------------------
File: gpustub.c Date: 10/25/26 (SurfGPU G3)
--------------------
See gpustub.h. Written from surfcpu/docs/GPU.md sections 3 to 5 and MEMORY-MAP.md: what each
register answers, when the engine takes a word, when a frame latch takes the display registers.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "surfcpu/soc.h"
#include "gpustub.h"
#include "../../../../shell/shell.h"

extern const uint8_t font8x16[256][16];        /* driver/font8x16.c */

static struct stub *cur;
static uint16_t picture[GPU_SCREEN_W * GPU_SCREEN_H];

void stub_current(struct stub *s) { cur = s; }

/**** the frame clock ****/

uint64_t stub_frame_of(uint64_t t) { return t / STUB_FRAME_US; }
static uint64_t vblank_of(uint64_t k) { return k * STUB_FRAME_US + STUB_VBLANK_US; }
static uint64_t latch_of(uint64_t k) { return k * STUB_FRAME_US + STUB_LATCH_US; }

/* how many blankings have begun at time t: STATUS.FRAME and FRAME_COUNT (a frame's CRC is taken as
   its blanking begins) */
static uint64_t blankings(const struct stub *s, uint64_t t) {
    if(s->frames_stop) return s->frames_frozen ? s->frames_frozen : 7;
    return t >= STUB_VBLANK_US ? (t - STUB_VBLANK_US) / STUB_FRAME_US + 1 : 0;
}

/* the first blanking that begins after t; none without a pixel clock (10/26/26) */
static uint64_t next_vblank(const struct stub *s, uint64_t t) {
    uint64_t k = stub_frame_of(t);
    if(s->frames_stop) return STUB_NEVER;
    return vblank_of(k) > t ? vblank_of(k) : vblank_of(k + 1);
}

static void apply(struct stub_disp *d, const struct stub_write *w) {
    if(w->reg == SURF_VIDEO_GFX_BASE) d->base = w->value & SURF_VIDEO_GFX_BASE_BASE_MASK;
    else if(w->reg == SURF_VIDEO_GFX_STRIDE) d->stride = w->value & SURF_VIDEO_GFX_STRIDE_STRIDE_MASK;
    else if(w->reg == SURF_VIDEO_GFX_SCALE) d->scale = w->value & SURF_VIDEO_GFX_SCALE_SCALE_MASK;
    else d->mode = w->value;
}

/* frame k shows what the latch before it took: writes that reached it (a microsecond's crossing) */
struct stub_disp stub_shown(const struct stub *s, uint64_t k) {
    struct stub_disp d = { 1, 0, 640, 1 };
    uint64_t cut = k ? latch_of(k - 1) : 0;
    unsigned i;
    for(i = 0; i < s->nw && s->wlog[i].t + 1 <= cut; i++) apply(&d, &s->wlog[i]);
    return d;
}

static void log_write_at(struct stub *s, uint64_t t, uint32_t reg, uint32_t v) {
    struct stub_write w = { t, reg, v };
    if(s->nw < STUB_DISP_LOG) s->wlog[s->nw++] = w;
    apply(&s->reg, &w);
}

static void log_write(struct stub *s, uint32_t reg, uint32_t v) { log_write_at(s, s->now, reg, v); }

/**** the picture ****/

static void render(const uint8_t *vram, const uint16_t *gpal, const struct stub_disp *d, uint16_t *frame) {
    struct gpu_display gd;
    gpu_display_reset(&gd, 1);
    gd.mode = (int)d->mode;
    gd.gfx_base = d->base;
    gd.gfx_stride = d->stride;
    gd.gfx_scale = d->scale;
    memcpy(gd.gpal, gpal, sizeof(gd.gpal));
    gpu_display_render(&gd, vram, NULL, font8x16, frame);
}

void stub_render(const struct stub *s, const struct stub_disp *d, uint16_t *frame) {
    render(s->vram, s->gpal, d, frame);
}

uint32_t stub_crc(const struct stub *s, const struct stub_disp *d) {
    stub_render(s, d, picture);
    return gpu_frame_crc(picture);
}

/* frame k's last line was sent: VRAM, GPAL and its display kept as they are now (FRAME_CRC is this
   frame's, whatever the engine draws next), and with log_frames its CRC logged */
static void frame_end(struct stub *s, uint64_t k) {
    struct stub_frame *f = &s->frame[k % STUB_FRAMES];
    f->k = k;
    f->valid = 1;
    f->d = stub_shown(s, k);
    memcpy(f->gpal, s->gpal, sizeof(f->gpal));
    memcpy(f->vram, s->vram, sizeof(f->vram));
    if(s->log_frames && s->nflog < STUB_FRAME_LOG) {
        render(f->vram, f->gpal, &f->d, picture);
        s->flog[s->nflog].k = k;
        s->flog[s->nflog].crc = gpu_frame_crc(picture);
        s->flog[s->nflog].d = f->d;
        s->nflog++;
    }
}

/* FRAME_CRC of frame k: as it ended (the frame kept), or, when the clock jumped past it, of what is
   there now */
static uint32_t frame_crc(struct stub *s, uint64_t k) {
    const struct stub_frame *f = &s->frame[k % STUB_FRAMES];
    struct stub_disp d;
    if(f->valid && f->k == k) {
        render(f->vram, f->gpal, &f->d, picture);
        return gpu_frame_crc(picture);
    }
    d = stub_shown(s, k);
    return stub_crc(s, &d);
}

/* what the screen shows at t: during a frame's lines, what its latch took; in its blanking, what the
   next frame will show (a change then is seen first in that one) */
struct stub_disp stub_visible(const struct stub *s, uint64_t t) {
    uint64_t k = stub_frame_of(t);
    return stub_shown(s, t % STUB_FRAME_US < STUB_VBLANK_US ? k : k + 1);
}

static void note(struct stub *s, unsigned kind, uint64_t t) {
    if(!s->watch) return;
    if(s->nev == s->ev_cap) {
        unsigned cap = s->ev_cap ? 2 * s->ev_cap : 4096;
        struct stub_event *e = realloc(s->ev, cap * sizeof(*e));
        if(!e) return;
        s->ev = e;
        s->ev_cap = cap;
    }
    s->ev[s->nev].t = t;
    s->ev[s->nev].kind = kind;
    s->nev++;
}

/**** the engine ****/

/* does this word make the engine draw (the last parameter of a FILL, COPY, LINE or TRIANGLE, or
   data)? */
static int draws(const struct gpu_engine *e) {
    unsigned op;
    if(e->data_left) return 1;
    if(!e->in_cmd) return 0;
    op = e->hdr >> 24;
    return (op == GPU_OP_FILL_RECT || op == GPU_OP_COPY_RECT || op == GPU_OP_LINE || op == GPU_OP_TRIANGLE) &&
           e->np + 1 == e->need;
}

/* the 320 x 240 double-buffered layouts (the demo's and the cube's: pages at 0 and 76,800, stride
   320, doubled): a word that draws while a page is on the screen must not reach that page's bytes,
   the bytes of the rows the clip lets it reach in its target (10/26/26: by bytes, so a target of
   another layout, the cube's depth buffer, is judged too) */
static void tearing(struct stub *s) {
    uint64_t t = s->eng_cycles / 25, k = stub_frame_of(t), lo, hi;
    struct stub_disp d;
    uint32_t y0, y1, x1;
    if(t % STUB_FRAME_US >= STUB_VBLANK_US || !draws(&s->eng)) return;   /* blanking: nothing is shown */
    d = stub_shown(s, k);
    if(d.mode != 2 || d.stride != 320 || d.scale != 2 || (d.base != 0 && d.base != 76800)) return;
    y0 = s->eng.cy0;
    y1 = s->eng.cy1 < s->eng.th ? s->eng.cy1 : s->eng.th;
    x1 = s->eng.cx1 < s->eng.tw ? s->eng.cx1 : s->eng.tw;
    if(y0 >= y1 || s->eng.cx0 >= x1) return;
    lo = s->eng.base + (uint64_t)y0 * s->eng.stride + s->eng.cx0;
    hi = s->eng.base + (uint64_t)(y1 - 1) * s->eng.stride + x1;
    if(lo < d.base + 76800ull && hi > d.base) s->shown_draws++;
}

static void take(struct stub *s, uint32_t w) {
    unsigned long long before = s->eng.writes, tri_px = s->eng.tri_pixels, tri_cmds = s->eng.tri_cmds;
    unsigned flips = s->eng.flips, op = w >> 24;
    if(gpu_engine_idle(&s->eng) && op >= GPU_OP_SET_ZBUF && op <= GPU_OP_FLIP && s->id != GPUDRV_ID_GPU2) {
        s->eng.sticky |= GPU_ST_BAD_OPCODE;             /* a GPU1 has no such command: the word is dropped */
        s->gpu2_on_gpu1++;
        s->eng_cycles += 1;
        return;
    }
    if(gpu_engine_idle(&s->eng) && op == GPU_OP_WAIT_VBLANK) {
        s->wait_vbl = 1;
        s->vbl_until = next_vblank(s, s->eng_cycles / 25);
    }
    if(s->check_tearing) tearing(s);
    if(draws(&s->eng)) note(s, STUB_EV_ENGINE_DRAW, s->eng_cycles / 25);
    if(gpu_engine_idle(&s->eng)) s->cur_op = op & 63;  /* a header */
    gpu_engine_word(&s->eng, w);
    s->op_px[s->cur_op] += s->eng.writes - before;
    s->eng_cycles += 1 + s->slow + (s->eng.writes - before) / 4;
    if(s->eng.tri_cmds != tri_cmds)                     /* GPU.md 16.5 and 16.8: a setup, then a cycle */
        s->eng_cycles += 247 + (s->eng.tri_pixels - tri_px) * 6 / 4;   /* a group, six with depth: about */
    if(s->eng.flips != flips) {                         /* FLIP: GFX_BASE at the next blanking, then on */
        s->wait_vbl = 1;
        s->vbl_until = next_vblank(s, s->eng_cycles / 25);
        s->flip_pending = 1;
        s->flip_value = s->eng.flip_base;
    }
}

/* the engine's work until the CPU's now: a word a cycle when it is free */
static void run(struct stub *s) {
    uint64_t limit = s->now * 25;
    for(;;) {
        if(s->wait_vbl) {
            if(s->vbl_until * 25 > limit) break;
            if(s->eng_cycles < s->vbl_until * 25) s->eng_cycles = s->vbl_until * 25;
            s->wait_vbl = 0;
            if(s->flip_pending) {                       /* the cycle after the blanking's pulse */
                log_write_at(s, s->vbl_until, SURF_VIDEO_GFX_BASE, s->flip_value);
                s->flip_pending = 0;
                s->flips_done++;
            }
        }
        if(!s->count || s->stopped || s->eng_cycles >= limit) break;
        take(s, s->fifo[s->head]);
        s->head = (s->head + 1) % 64;
        s->count--;
    }
    if(!s->count && !s->wait_vbl && s->eng_cycles < limit) s->eng_cycles = limit;
    /* the interrupt: the kernel's handler clears FENCE_EV and wakes the waiter */
    if(s->irq_path && (s->ie & SURF_GPU_IE_FENCE_MASK) && (s->eng.sticky & GPU_ST_FENCE_EV)) {
        s->eng.sticky &= ~GPU_ST_FENCE_EV;
        s->interrupts_sent++;
        if(!s->isr_no_wake) s->event = 1;
    }
}

static void push(struct stub *s, uint32_t w) {
    if(s->ncmd < STUB_CMD_LOG) s->cmd[s->ncmd++] = w;
    s->fifo[(s->head + s->count) % 64] = w;
    s->count++;
}

/* time passes on the CPU's side; each frame that ends on the way is kept as it ended, with the
   engine's work up to then (10/26/26) */
static void tick(struct stub *s, uint64_t us) {
    uint64_t to = s->now + us, b;
    if(s->stall_at && to >= s->stall_at && !s->in_atomic) {        /* an interrupt or another task: */
        to += s->stall_us;                                          /* never inside an atomic stretch */
        s->stall_at = 0;
    }
    if(s->frames_stop_at && to >= s->frames_stop_at) {
        if(s->frames_stop_at > s->now) {
            s->now = s->frames_stop_at;
            run(s);
        }
        s->frames_frozen = (uint32_t)blankings(s, s->now);
        s->frames_stop = 1;
        s->frames_stop_at = 0;
    }
    b = blankings(s, s->now);
    if(!s->frames_stop && b > s->frames_seen + 2) s->frames_seen = b - 1;  /* the clock was moved on: the last */
    while(!s->frames_stop && vblank_of(s->frames_seen) <= to) {
        uint64_t vb = vblank_of(s->frames_seen);
        if(vb > s->now) {
            s->now = vb;
            run(s);
        }
        frame_end(s, s->frames_seen++);
    }
    s->now = to;
    if(s->poke_at && s->now >= s->poke_at) {                       /* a byte of the picture on the screen */
        struct stub_disp d = stub_visible(s, s->now);
        s->vram[(d.base + s->poke_off) % GPU_VRAM_BYTES] ^= 0xFF;
        s->poke_at = 0;
    }
    if(s->stop_at && s->now >= s->stop_at) {
        s->stopped = 1;
        s->stop_at = 0;
    }
    if(s->other_holds && !s->other_done && s->now + 1000 >= s->other_until) {
        unsigned i;
        s->cmd_at_other = s->ncmd;
        for(i = 0; i < s->other_n; i++) {           /* the other claimant's last words, while it holds */
            while(s->count == s->depth) {
                s->now++;
                run(s);
            }
            push(s, s->other_words[i]);
            run(s);
        }
        s->other_done = 1;
    }
    run(s);
}

void stub_drain(struct stub *s, uint64_t max_us) {
    uint64_t end = s->now + max_us;
    while(s->now < end && (s->count || s->wait_vbl || s->eng_cycles > s->now * 25)) tick(s, 10);
}

static int interrupted(struct stub *s) {
    return s->intr_at && s->now >= s->intr_at;
}

/* the test's other console, never inside itself */
static void hook(struct stub *s, int where) {
    if(!s->hook || s->in_hook) return;
    s->in_hook = 1;
    s->hook(s, where);
    s->hooks_run++;
    s->in_hook = 0;
}

/**** the port ****/

static uint32_t p_rd(void *c, uint32_t off) {
    struct stub *s = c;
    uint32_t v = 0;
    tick(s, 1);
    switch(off) {
    case SURF_GPU_ID: v = s->id; break;
    case SURF_GPU_STATUS:
        v = s->overflow | (s->eng.sticky & (GPU_ST_BAD_OPCODE | GPU_ST_BAD_PARAM | GPU_ST_FENCE_EV));
        if(s->count || s->wait_vbl || s->eng_cycles > s->now * 25 || !gpu_engine_idle(&s->eng)) v |= GPU_ST_BUSY;
        break;
    case SURF_GPU_IE: v = s->ie; break;
    case SURF_GPU_FIFO_FREE:
        v = s->depth - s->count;
        s->fifo_free_reads++;
        s->free_said = v;
        if(!v) s->full_seen++;
        break;
    case SURF_GPU_FENCE:
        v = s->eng.fence;
        if(!s->fence_read_at) s->fence_read_at = s->now;
        break;
    case SURF_GPU_INFO: v = s->info; break;
    }
    return v;
}

static void p_wr(void *c, uint32_t off, uint32_t v) {
    struct stub *s = c;
    tick(s, 1);
    switch(off) {
    case SURF_GPU_STATUS:
        if(v & GPU_ST_OVERFLOW) s->overflow = 0;
        s->eng.sticky &= ~(v & (GPU_ST_BAD_OPCODE | GPU_ST_BAD_PARAM | GPU_ST_FENCE_EV));
        break;
    case SURF_GPU_IE: s->ie = v & 3; break;
    case SURF_GPU_CTRL:
        if(v & SURF_GPU_CTRL_ABORT_MASK) {
            s->count = 0;
            s->wait_vbl = 0;
            s->flip_pending = 0;                        /* a FLIP waiting for its blanking writes nothing */
            gpu_engine_abort(&s->eng);
            s->eng_cycles = s->now * 25;
            s->aborts++;
            if(!s->abort_at) s->abort_at = s->now;
            hook(s, STUB_AT_ABORT);
        }
        break;
    case SURF_GPU_CMD:
        s->cmd_writes++;
        if(s->free_said) s->free_said--;
        else s->writes_blind++;
        if(s->cmd_writes == s->drop_word) {         /* a knob: as if the FIFO had been full */
            s->overflow = GPU_ST_OVERFLOW;
            break;
        }
        if(s->cmd_writes == s->corrupt_word) v = (v & 0x00FFFFFFu) | 0x0F000000u;
        if(s->cmd_writes == s->corrupt_value_at) v = s->corrupt_value;
        if(s->count == s->depth) {
            s->writes_full++;
            s->overflow = GPU_ST_OVERFLOW;
            break;
        }
        push(s, v);
        run(s);
        break;
    }
}

static uint32_t p_vrd(void *c, uint32_t off) {
    struct stub *s = c;
    uint64_t b;
    tick(s, 1);
    b = blankings(s, s->now);
    if(off == SURF_VIDEO_STATUS) {
        uint32_t v = (uint32_t)(b & 0xFFFF) << SURF_VIDEO_STATUS_FRAME_LSB;
        if(!s->frames_stop && s->now % STUB_FRAME_US >= STUB_VBLANK_US) v |= SURF_VIDEO_STATUS_VBLANK_MASK;
        if(b > blankings(s, s->ev_cleared)) v |= SURF_VIDEO_STATUS_VBLANK_EV_MASK;
        return v;
    }
    if(off == SURF_VIDEO_GFX_BASE) return s->reg.base;
    if(off == SURF_VIDEO_GFX_STRIDE) return s->reg.stride;
    if(off == SURF_VIDEO_GFX_SCALE) return s->reg.scale;
    if(off == SURF_VIDEO_FRAME_COUNT) {
        s->kept = 1;
        s->kept_frame = (uint32_t)b;
        return (uint32_t)(b & 0xFFFF);
    }
    if(off == SURF_VIDEO_FRAME_CRC) {
        uint64_t n = s->kept ? s->kept_frame : b;   /* the kept frame, else the newest */
        s->kept = 0;
        if(!n) return 0;
        s->crc_frame = n - 1;
        return frame_crc(s, n - 1);
    }
    if(off >= SURF_VIDEO_GPAL && off < SURF_VIDEO_GPAL + 4 * 256) return s->gpal[(off - SURF_VIDEO_GPAL) / 4];
    return 0;
}

static void p_vwr(void *c, uint32_t off, uint32_t v) {
    struct stub *s = c;
    tick(s, 1);
    if(off == SURF_VIDEO_STATUS && (v & SURF_VIDEO_STATUS_VBLANK_EV_MASK)) s->ev_cleared = s->now;
    else if(off == SURF_VIDEO_GFX_BASE || off == SURF_VIDEO_GFX_STRIDE || off == SURF_VIDEO_GFX_SCALE) log_write(s, off, v);
    else if(off >= SURF_VIDEO_GPAL && off < SURF_VIDEO_GPAL + 4 * 256) {
        note(s, STUB_EV_GPAL, s->now);
        s->gpal[(off - SURF_VIDEO_GPAL) / 4] = v & 0xFFF;
    }
}

/* a word a microsecond, as through the window */
static void p_vram_read(void *c, uint32_t off, uint32_t *dst, uint32_t words) {
    struct stub *s = c;
    for(; words; words--, off += 4) {
        tick(s, 1);
        if(off + 4 > GPU_VRAM_BYTES || (off & 3)) {
            s->bad_vram++;
            *dst++ = 0;
            continue;
        }
        *dst++ = (uint32_t)s->vram[off] | (uint32_t)s->vram[off + 1] << 8 | (uint32_t)s->vram[off + 2] << 16 |
                 (uint32_t)s->vram[off + 3] << 24;
    }
}

static void p_vram_write(void *c, uint32_t off, const uint32_t *src, uint32_t words) {
    struct stub *s = c;
    for(; words; words--, off += 4) {
        uint32_t v = *src++;
        tick(s, 1);
        if(off + 4 > GPU_VRAM_BYTES || (off & 3)) {
            s->bad_vram++;
            continue;
        }
        note(s, STUB_EV_CPU_VRAM, s->now);
        s->vram[off] = (uint8_t)v;
        s->vram[off + 1] = (uint8_t)(v >> 8);
        s->vram[off + 2] = (uint8_t)(v >> 16);
        s->vram[off + 3] = (uint8_t)(v >> 24);
    }
}

static uint8_t p_vram_rd8(void *c, uint32_t off) {
    struct stub *s = c;
    tick(s, 1);
    if(off >= GPU_VRAM_BYTES) {
        s->bad_vram++;
        return 0;
    }
    return s->vram[off];
}

static void p_vram_wr8(void *c, uint32_t off, uint8_t v) {
    struct stub *s = c;
    tick(s, 1);
    if(off >= GPU_VRAM_BYTES) {
        s->bad_vram++;
        return;
    }
    note(s, STUB_EV_CPU_VRAM, s->now);
    s->vram[off] = v;
}

static uint64_t p_us(void *c) {
    struct stub *s = c;
    tick(s, 1);
    return s->now;
}

static void p_relax(void *c) {
    struct stub *s = c;
    s->relaxes++;
    tick(s, 5);
}

/* sleep_intr(): at once when Ctrl+C has come, cut short when it comes */
static int p_sleep(void *c, uint32_t ms) {
    struct stub *s = c;
    uint64_t end = s->now + (uint64_t)ms * 1000;
    s->sleeps++;
    if(interrupted(s)) {
        tick(s, 1);
        return GPU_EINTR;
    }
    if(s->intr_at && s->intr_at < end) {
        tick(s, s->intr_at - s->now);
        return GPU_EINTR;
    }
    tick(s, end - s->now);
    return 0;
}

static bool p_interrupted(void *c) { return (bool)interrupted((struct stub *)c); }

static void p_irq_arm(void *c) {
    struct stub *s = c;
    s->event = 0;
}

/* event_wait_timeout(): 1 when the interrupt came (before or during the wait), 0 when ms passed */
static int p_irq_wait(void *c, uint32_t ms) {
    struct stub *s = c;
    uint64_t end = s->now + (uint64_t)ms * 1000;
    s->irq_waits++;
    while(!s->event && s->now < end) {
        if(interrupted(s)) return GPU_EINTR;
        tick(s, 10);
    }
    return s->event ? 1 : 0;
}

static u_long p_atomic_begin(void *c) {
    struct stub *s = c;
    s->in_atomic++;
    return 1;
}

/* interrupts on again: a stall that came meanwhile (an interrupt, another task) happens now */
static void p_atomic_end(void *c, u_long flags) {
    struct stub *s = c;
    (void)flags;
    s->in_atomic--;
    if(!s->in_atomic) tick(s, 0);
}

static int p_text_lock(void *c) {
    struct stub *s = c;
    s->text_locks++;
    return 1;
}

static void p_text_unlock(void *c, int how) {
    struct stub *s = c;
    (void)how;
    s->text_locks--;
}

static uint32_t p_get_mode(void *c) { return ((struct stub *)c)->reg.mode; }

static void p_set_mode(void *c, uint32_t mode) {
    struct stub *s = c;
    if(s->text_locks <= 0 || s->in_atomic <= 0) s->mode_unlocked++;
    tick(s, 1);
    log_write(s, 0xFFFF, mode);
}

static bool p_trylock(void *c) {
    struct stub *s = c;
    if(s->other_holds) {
        if(s->now < s->other_until) return false;
        s->other_holds = 0;
    }
    if(s->locked) {
        s->claimed_twice++;
        return false;
    }
    s->locked = 1;
    s->claims++;
    return true;
}

static void p_unlock(void *c) {
    struct stub *s = c;
    if(!s->locked) s->unlocked_free++;
    s->locked = 0;
    hook(s, STUB_AT_UNLOCK);
}

static const char *p_holder(void *c, char *buf, u_int size) {
    struct stub *s = c;
    snprintf(buf, size, "%s", s->holder ? s->holder : "'Shell 2', pid 9");
    return buf;
}

static void *p_alloc(void *c, uint32_t bytes) {
    struct stub *s = c;
    return s->no_memory ? NULL : malloc(bytes);
}

static void p_free(void *c, void *p) {
    (void)c;
    free(p);
}

void stub_init(struct stub *s, int irq_path) {
    static const struct gpu_port port = {
        NULL, p_rd, p_wr, p_vrd, p_vwr, p_vram_read, p_vram_write, p_vram_rd8, p_vram_wr8, p_us, p_relax, p_sleep,
        p_interrupted, p_irq_arm, p_irq_wait, p_atomic_begin, p_atomic_end, p_text_lock, p_text_unlock, p_get_mode,
        p_set_mode, p_trylock, p_unlock, p_holder, p_alloc, p_free
    };
    unsigned n;
    uint32_t *log = s->cmd ? s->cmd : malloc(STUB_CMD_LOG * sizeof(uint32_t));
    free(s->ev);
    memset(s, 0, sizeof(*s));
    s->cmd = log;
    s->port = port;
    s->port.ctx = s;
    if(!irq_path) {
        s->port.irq_arm = NULL;
        s->port.irq_wait = NULL;
    }
    s->irq_path = irq_path;
    gpu_engine_init(&s->eng, s->vram);
    s->depth = 64;
    s->id = GPUDRV_ID_GPU1;
    s->info = 64u | 300u << 16;
    s->reg.mode = 1;                            /* textcon.c left text */
    s->reg.base = 0;
    s->reg.stride = 640;
    s->reg.scale = 1;
    for(n = 0; n < 256; n++) s->gpal[n] = gpu_gpal_initial(n);
    s->now = 1000;
    s->eng_cycles = s->now * 25;
    s->ev_cleared = 0;
    cur = s;
}

void stub_free(struct stub *s) {
    free(s->cmd);
    s->cmd = NULL;
    free(s->ev);
    s->ev = NULL;
    s->nev = s->ev_cap = 0;
}

/**** gfx with a line ****/

/* every line printed is measured in the console's columns (a UTF-8 sequence is one) */
static unsigned too_wide;
static char wide_first[240];

unsigned stub_too_wide(const char **first) {
    unsigned n = too_wide;
    if(first) *first = wide_first;
    too_wide = 0;
    return n;
}

static void measure(struct stub *s, unsigned from) {
    unsigned i;
    if(s->line_start > from) {                  /* out was cleared */
        s->line_start = from;
        s->col = 0;
    }
    for(i = from; i < s->nout; i++) {
        unsigned char ch = (unsigned char)s->out[i];
        if(ch != '\n') {
            if((ch & 0xC0) != 0x80) s->col++;
            continue;
        }
        if(s->col > s->widest) s->widest = s->col;
        if(s->col > STUB_COLUMNS && !too_wide++)
            snprintf(wide_first, sizeof(wide_first), "%.*s", (int)(i - s->line_start), s->out + s->line_start);
        s->col = 0;
        s->line_start = i + 1;
    }
}

int gt_printf(const char *fmt, ...) {
    va_list ap;
    int n = 0;
    unsigned from;
    if(!cur) return 0;
    from = cur->nout;
    va_start(ap, fmt);
    if(cur->nout < sizeof(cur->out) - 1) {
        n = vsnprintf(cur->out + cur->nout, sizeof(cur->out) - cur->nout, fmt, ap);
        if(n > 0) cur->nout += (unsigned)n;
        if(cur->nout > sizeof(cur->out) - 1) cur->nout = sizeof(cur->out) - 1;
    }
    va_end(ap);
    measure(cur, from);
    if(getenv("GPU_TEST_VERBOSE")) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    return n;
}

/* the words as the kernel shell splits them (arch/riscv32/kshell.c's split_args(): spaces and tabs,
   SHELL_MAX_ARGS words at most, the rest dropped) */
int stub_gfx(struct stub *s, struct gpu *g, const char *line) {
    char buf[SHELL_LINE_LEN + 1], *argv[SHELL_MAX_ARGS + 1], *save = NULL, *t;
    int argc = 0;
    snprintf(buf, sizeof(buf), "%s", line);
    for(t = strtok_r(buf, " \t", &save); t && argc < SHELL_MAX_ARGS; t = strtok_r(NULL, " \t", &save)) argv[argc++] = t;
    argv[argc] = NULL;
    cur = s;
    s->nout = 0;
    s->out[0] = 0;
    s->col = s->line_start = 0;
    if(getenv("GPU_TEST_VERBOSE")) printf("> %s\n", line);
    return gfx_command(g, argc, argv);
}
