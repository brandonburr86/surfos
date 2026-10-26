/*
SurfOS board: SurfCPU on the Arty A7 -- gfx cube
--------------------
File: gfxcube.c Date: 10/26/26 (SurfGPU, the cube in SurfOS)
--------------------
gpudiag's spinning cube (surfcpu/docs/GPU.md 16.9) in SurfOS, on a GPU2 (GPU.md 15.8). The frames
are gpucube.c's own: gpu_cube_frame(), compiled into the kernel from where it is, unchanged,
records each frame's words through a gpu_sink, and they go to the engine through the driver
(gpu_pump(): the FIFO_FREE rule, never waiting, so that every frame shown can be looked at between
the words). A frame is 105 words: the depth buffer cleared, the picture cleared, SET_ZBUF, twelve
Gouraud triangles with depth, a FLIP to the picture drawn and a FENCE (the driver's next value):
320x240 double-buffered with the depth buffer, GPU.md 16.6's layout, all of VRAM; frame k is drawn
into picture A (k even) or B (k odd).

The display is set up through the driver and textcon, never by gpucube.c's own writes: CTRL.MODE 2
through textcon's copy of CTRL, GFX_STRIDE 320 and GFX_SCALE 2 at the frame latch, and the palette
gpu_cube_display() makes, taken from the GPAL writes it hands the sink (which reach nothing) and
loaded by the driver. Text RAM, the text palette, SCROLL and the cursor stay the console's, so the
console keeps working, and comes back whole: gfx_keep_begin() and gfx_keep_end() keep and put back
the display, the palette, the target, the clip, the depth buffer and VRAM, as gfx demo does, with
the console on the screen while VRAM or the palette changes.

Every FRAME_COUNT that moved has its FRAME_CRC (GPU.md section 3's pair: that frame's) looked up
in gfx_cube_expect[] (gfxcube_expect.h, generated at build time by boards/arty_a7/gen/
gfxcube_expect.c from the same gpucube.c and the C model with this display, never from the
driver). The looks are made from the timer interrupt, every tick (10 ms, less than a frame's
16.7): each FRAME_COUNT that moved, with its FRAME_CRC, goes into a small ring that the run
empties as it goes, so no frame passes unseen while the run prints its line (a line costs SurfCPU
about a frame: the console compares its whole screen) or other tasks run. On a port without the
tick the run looks from its loop. A frame whose CRC is none of the turn's is torn: not a whole
drawn frame. When the turn's 128 pictures all differ their order is checked as well: frame f shown
d frames after frame l must have (f - l) mod 128 <= d, since a FLIP waits for a blanking, and the
cube must turn with its FLIPs (the review of 58ac7b3's M2): each look also reads FENCE, and the
frame shown must be the one FLIPped last or the one before (in_step(), below), and at the end the
frames shown must have gone on as many frames as the FLIPs done. A frame shown that no look saw (FRAME_COUNT moved by more than one between two looks) was not checked: it
is missed (the review of 58ac7b3's L1), and no verdict of all whole is given while any is. A line
a second says the frames drawn (FLIPs done after frame 0's), shown (video frames) and missed, and
whether all were whole (and in order); at the end the frame rate (the frames drawn over the time
since the blanking at which the cube's display began: never above the display's 59.94, the
review's M1) and the same for the whole run. Ctrl+C, the time running out, a stopped engine and an
error each end it with the engine idle and everything put back; it fails when a frame shown was
torn, out of order or missed.

The sink has no context pointer and no error return: it only records, into the run's own record,
which `rec` points to while the run holds the driver's claim (one gfx cube at a time, as heaptest
and selftest claim theirs; it is cleared before the claim is let go). Errors and Ctrl+C are the
driver's, said and handled where the run sends the words.
*/

#include <surfos/types.h>
#include <stdio.h>
#include <string.h>
#include <surfcpu/soc.h>
#include <surfcpu/gpu.h>
#include "gpu.h"
#include "../../surfcpu/firmware/diagnostics/gpucube.h"
#include "gfxcube_expect.h"

#define REC_WORDS (GPU_CUBE_FRAME_WORDS + 16)

/* one frame's words, and gpu_cube_display()'s palette, as the sink records them */
struct cube_rec {
    u32 w[REC_WORDS];
    u_int n;
    bool odd;                           /* more words than room, or VRAM written through the sink */
    u16 pal[256];
};

static struct cube_rec *rec;            /* the run's, under the driver's claim (above) */

static void rec_cmd(uint32_t w) {
    if(!rec) return;
    if(rec->n < REC_WORDS) rec->w[rec->n++] = w;
    else rec->odd = true;
}

static void rec_sync(void) {}

static void rec_vram(uint32_t off, uint32_t v) {
    (void)off;
    (void)v;
    if(rec) rec->odd = true;            /* gpucube.c draws nothing through the CPU's window */
}

static void rec_video(uint32_t off, uint32_t v) {
    if(rec && off >= SURF_VIDEO_GPAL && off < SURF_VIDEO_GPAL + 4u * SURF_VIDEO_GPAL_COUNT)
        rec->pal[(off - SURF_VIDEO_GPAL) / 4] = (u16)(v & 0xFFFu);
}

static void rec_text(uint32_t cell, uint32_t v) {
    (void)cell;
    (void)v;
}

static const struct gpu_sink sink = { rec_cmd, rec_sync, rec_vram, rec_vram, rec_video, rec_text };

/* The looks: FRAME_COUNT read every tick, from the timer interrupt; when it moved, it and that
   frame's FRAME_CRC go into the ring with FENCE as it read just before and just after the two (the
   FLIPs done then: the check's in-step rule), which the run empties (one writer, the interrupt,
   one reader, the run, on one CPU: an entry is written before head moves on, read before tail
   does). A full ring drops the look, which the check then counts as a frame not looked at. */
#define LOOKS 16

struct cube_look {
    u32 count, crc, fb, fa;             /* FRAME_COUNT, FRAME_CRC; FENCE before and after them */
    u64 t;                              /* when */
};

struct cube_looks {
    struct gpu *g;
    u32 count;                          /* FRAME_COUNT at the last look (the interrupt's own) */
    volatile u32 head, tail, dropped;
    volatile struct cube_look l[LOOKS];
};

/* a look: false when FRAME_COUNT has not moved from since */
static bool look(struct gpu *g, u32 since, struct cube_look *l) {
    l->fb = gpu_fence_now(g);
    if(!gpu_frame_next(g, since, &l->count, &l->crc)) return false;
    l->fa = gpu_fence_now(g);
    l->t = gpu_now_us(g);
    return true;
}

static void look_tick(void *arg) {
    struct cube_looks *ls = arg;
    struct cube_look l;
    if(!look(ls->g, ls->count, &l)) return;
    ls->count = l.count;
    if(ls->head - ls->tail >= LOOKS) {
        ls->dropped++;
        return;
    }
    ls->l[ls->head % LOOKS].count = l.count;
    ls->l[ls->head % LOOKS].crc = l.crc;
    ls->l[ls->head % LOOKS].fb = l.fb;
    ls->l[ls->head % LOOKS].fa = l.fa;
    ls->l[ls->head % LOOKS].t = l.t;
    ls->head++;
}

/**** the frames shown ****/

void gfx_cube_check_start(struct gfx_cube_check *c, const u32 *crcs, u32 n, bool ordered, u32 count, i32 first,
                          u32 fence0) {
    memset(c, 0, sizeof(*c));
    c->crcs = crcs;
    c->n = n;
    c->ordered = ordered;
    c->count = c->last_count = count & SURF_VIDEO_FRAME_COUNT_COUNT_MASK;
    c->last = first;
    c->fence0 = fence0;
}

/* Is frame f of the turn in step with the FLIPs? The frame a look counts showed what the latch
   took after the blanking before it, the frame FLIPped last by then: of the FLIPs done (counted
   after frame 0's) at the look, the last or the one before (a FLIP a blanking at most). FENCE read
   before the pair, fb, can only be fewer; FENCE read after it, fa, at most one fewer (a FLIP done
   whose FENCE the CPU had not yet sent, say while it printed a line). So f is one of fb - 1 to
   fa + 1: the same frame again is in order only while at most one FLIP has come since (the review
   of 58ac7b3's M2: a cube that stood still while its FLIPs went on was "all whole, in order") */
static bool in_step(const struct gfx_cube_check *c, u32 f, u32 fb, u32 fa) {
    i32 lo = (i32)(fb - c->fence0) - 1, hi = (i32)(fa - c->fence0) + 1, j;
    if(lo < 0) lo = 0;
    if(hi - lo + 1 >= (i32)c->n) return true;
    for(j = lo; j <= hi; j++)
        if((u32)j % c->n == f) return true;
    return false;
}

/* frame f of the turn shown d frames after the last whole one, l: in order when (f - l) mod n <= d
   (a FLIP waits for a blanking, so the turn moves on a frame a frame at most) and f is in step with
   the FLIPs (above) */
void gfx_cube_check_frame(struct gfx_cube_check *c, u32 count, u32 crc, u32 fence_before, u32 fence_after) {
    u32 d = (count - c->count) & SURF_VIDEO_FRAME_COUNT_COUNT_MASK, dl = (count - c->last_count) & SURF_VIDEO_FRAME_COUNT_COUNT_MASK, f;
    if(!d) return;
    c->count = count & SURF_VIDEO_FRAME_COUNT_COUNT_MASK;
    c->shown += d;
    c->checked++;
    for(f = 0; f < c->n && c->crcs[f] != crc; f++) {}
    if(f == c->n) {                                     /* none of the turn's frames: torn */
        if(!c->torn++) {
            c->torn_count = count;
            c->torn_crc = crc;
        }
        return;
    }
    if(c->ordered && c->last >= 0) {
        u32 step = (f + c->n - (u32)c->last) % c->n;
        if(step > dl || !in_step(c, f, fence_before, fence_after)) c->disorder++;
        c->advance += step;
    }
    c->last = (i32)f;
    c->last_count = c->count;
}

/* all whole (and in order), or how many were not; frames missed (shown, not looked at) were not
   checked, so with any the verdict is the counts */
static const char *verdict(const struct gfx_cube_check *c, u_long missed, u_long torn, u_long disorder, char *buf,
                           u_int size) {
    if(!missed && !torn && !disorder) return c->ordered ? "all whole, in order" : "all whole";
    snprintf(buf, size, "%lu torn, %lu out of order", torn, disorder);
    return buf;
}

/**** the run ****/

/* frame k recorded, its FENCE the driver's next value (*fence) */
static int record(struct gpu *g, struct cube_rec *r, u32 k, u32 *fence, const char *what) {
    r->n = 0;
    *fence = gpu_fence_next(g);
    gpu_cube_frame(&sink, k, k & 1u, *fence);
    if(r->odd || r->n != GPU_CUBE_FRAME_WORDS) {
        printf("    %s: gpucube.c's frame is not the %u words this kernel was built for\n", what,
               (u_int)GPU_CUBE_FRAME_WORDS);
        return GPU_EARG;
    }
    return GPU_OK;
}

int gfx_cube_run(struct gpu *g, u32 seconds, const char *what) {
    struct cube_rec r;
    struct gfx_keep keep;
    struct gfx_cube_check c;
    struct cube_looks looks;
    struct cube_look lk;
    struct gpu_screen shown;
    char buf[48];
    u32 k = 1, pos = 0, v0 = 0, vbase = 0, vlast = 0, fence = 0, f0, count, crc, sec = 0, drawn = 0, ends = 0;
    u32 ends_done = 0;
    u_long last_drawn = 0, last_shown = 0, last_missed = 0, last_torn = 0, last_dis = 0;
    u64 t0 = 0, tick = 0, now = 0, secs = (u64)seconds * 1000000u, done_at = 0, seen_at = 0;
    bool ordered = GFX_CUBE_PICTURES == GFX_CUBE_FRAMES, intr = false, skip = false, ran = false, ticking = false;
    bool stood = false;
    u_int taken;
    int rc, r2;
    memset(&r, 0, sizeof(r));
    rec = &r;
    gpu_cube_display(&sink, 0);                         /* only its GPAL is kept: nothing reaches the hardware */
    gfx_cube_check_start(&c, gfx_cube_expect, GFX_CUBE_FRAMES, ordered, 0, 0, 0);
    rc = gfx_keep_begin(g, &keep, SURF_VRAM_BYTES, r.pal, what);
    /* frame 0 into picture A, and once its FLIP is done the display: mode 2, 320x240 doubled (the
       wait is gpu_sync()'s, whose FENCE asks for the interrupt: gpucube.c's does not, and a wait on
       it could only end by its 10 ms steps). That FENCE comes after frame 0's, so frame k's is
       vbase + k and the FLIPs done since frame 0's are FENCE - vbase (the review of 58ac7b3's M1:
       counted from frame 0's own FENCE, the sync's was one more). The first lines are said before
       the display changes, while the console is on the screen, so that no frame waits for them */
    if(!rc) rc = record(g, &r, 0, &v0, what);
    if(!rc) rc = gpu_put(g, r.w, r.n, what);
    if(!rc) rc = gpu_sync(g, what);
    vbase = g->fence_sent;
    if(!rc) {
        printf("    %s: %lu s at 320x240 with depth, a FLIP a frame (Ctrl+C ends)\n", what, (u_long)seconds);
        if(!ordered)
            printf("    %s: %lu of a turn's %lu frames are pictures of their own: each\n"
                   "    frame shown is checked to be one of them, not their order\n", what, (u_long)GFX_CUBE_PICTURES,
                   (u_long)GFX_CUBE_FRAMES);
    }
    shown.mode = GPU_MODE_GFX;
    shown.base = GPU_CUBE_A;
    shown.stride = 320;
    shown.scale = 2;
    if(!rc) rc = gpu_screen_set(g, &shown, what);
    if(!rc) {
        ran = true;
        /* the time counts from just before the blanking at which the cube's display began (the
           latch's last sample before it): FLIP k comes at the k-th blanking after it or later, so
           the rate, the FLIPs done since frame 0's over that time, is never above the display's */
        t0 = tick = g->latched ? g->latch_us : gpu_now_us(g);
        gpu_frame_next(g, 0x10000u, &count, &crc);     /* the frames after this one are the cube's */
        gfx_cube_check_start(&c, gfx_cube_expect, GFX_CUBE_FRAMES, ordered, count, 0, vbase);
        skip = !g->latched;                             /* the display changed off the latch: a mixed frame */
        memset(&looks, 0, sizeof(looks));
        looks.g = g;
        looks.count = c.count;
        ticking = gpu_tick_start(g, look_tick, &looks);
        vlast = fence = vbase;
        r.n = 0;
    }
    while(!rc) {
        bool seen = false;
        for(;;) {                                       /* the frames looked at since: the tick's, or here */
            if(ticking) {
                if(looks.tail == looks.head) break;
                lk.count = looks.l[looks.tail % LOOKS].count;
                lk.crc = looks.l[looks.tail % LOOKS].crc;
                lk.fb = looks.l[looks.tail % LOOKS].fb;
                lk.fa = looks.l[looks.tail % LOOKS].fa;
                lk.t = looks.l[looks.tail % LOOKS].t;
                looks.tail++;
            } else if(!look(g, c.count, &lk)) {
                break;
            }
            if(skip) c.count = c.last_count = lk.count & SURF_VIDEO_FRAME_COUNT_COUNT_MASK;
            else gfx_cube_check_frame(&c, lk.count, lk.crc, lk.fb, lk.fa);
            if(!seen_at && (i32)(lk.fa - vlast) >= 0) seen_at = lk.t;      /* the last frame recorded, done */
            skip = false;
            seen = true;
            ends++;
            if(!ticking) break;
        }
        now = gpu_now_us(g);
        drawn = (i32)(fence - vbase) > 0 ? fence - vbase : 0;   /* FLIPs done since frame 0's */
        if(seen && now - tick >= 1000000u) {            /* a line a second, just after a frame was looked at */
            tick += 1000000u;
            sec++;
            printf("    cube %lu s: %lu drawn, %lu shown, %lu missed: %s\n", (u_long)sec, (u_long)drawn - last_drawn,
                   c.shown - last_shown, c.shown - c.checked - last_missed,
                   verdict(&c, c.shown - c.checked - last_missed, c.torn - last_torn, c.disorder - last_dis, buf,
                           sizeof(buf)));
            last_drawn = drawn;
            last_shown = c.shown;
            last_missed = c.shown - c.checked;
            last_torn = c.torn;
            last_dis = c.disorder;
        }
        if(gpu_interrupted(g)) {
            intr = true;
            break;
        }
        f0 = fence;
        if(pos < r.n) {                                 /* this frame's words, as the FIFO takes them */
            rc = gpu_pump(g, r.w + pos, r.n - pos, &taken, &fence, what);
            pos += taken;
        } else if(now - t0 < secs) {                    /* the next frame */
            rc = record(g, &r, k++, &vlast, what);
            seen_at = 0;
            pos = 0;
        } else if((i32)(fence - vlast) < 0) {           /* time is up: the last frame's FLIP and FENCE */
            rc = gpu_pump(g, r.w, 0, &taken, &fence, what);
        } else {                                        /* all drawn: looked at until the last is shown */
            if(!done_at) {
                done_at = now;
                ends_done = ends;
            }
            if(ends >= ends_done + 2 || now - done_at >= 200000u) break;
            rc = gpu_pump(g, r.w, 0, &taken, &fence, what);
        }
        if(!rc && fence != f0) rc = gpu_check(g, what);    /* a frame done: the engine's errors */
    }
    drawn = (i32)(fence - vbase) > 0 ? fence - vbase : 0;   /* as FENCE last read */
    if(ticking) gpu_tick_stop(g);                       /* before `looks` goes */
    if(rc == GPU_EINTR || rc == GPU_ECANCEL) intr = true;
    /* a run that ran its time saw the frame of its last FLIP: the frames shown went on as many
       frames as the FLIPs done (so, none missed, each of them was shown: the review's M2) */
    stood = ran && !rc && !intr && ordered && c.advance != drawn;
    if(stood) c.disorder++;
    /* the rate's time ends where the last FLIP was first seen done, by the run or by a look (FENCE read
       just after it, within a tick: the run may have been printing its line then) */
    if(ran) {
        now = done_at ? done_at : gpu_now_us(g);
        if(seen_at && seen_at < now) now = seen_at;
    }
    r2 = gfx_keep_end(g, &keep, rc, intr, what);
    rec = NULL;
    if(ran) {
        u64 us = now - t0, rate10 = us ? (u64)drawn * 10000000u / us : 0;
        printf("    %s: %lu drawn in %lu.%02lu s, %lu.%lu a second; %s\n", what, (u_long)drawn, (u_long)(us / 1000000u),
               (u_long)(us % 1000000u / 10000u), (u_long)(rate10 / 10), (u_long)(rate10 % 10),
               r2 ? "display NOT put back" : "display back as it was");
        if(stood)
            printf("    %s: the frames shown went %lu on for %lu FLIPs\n", what, c.advance, (u_long)drawn);
        printf("    %s: %lu shown, %lu missed: %s\n", what, c.shown, c.shown - c.checked,
               verdict(&c, c.shown - c.checked, c.torn, c.disorder, buf, sizeof(buf)));
        if(c.torn)
            printf("    %s: the first torn: frame %lu, FRAME_CRC %08lx, none of the turn's\n", what,
                   (u_long)c.torn_count, (u_long)c.torn_crc);
    }
    if(intr) {
        printf("    %s: ended by Ctrl+C\n", what);
        return GPU_EINTR;
    }
    if(rc) return rc;
    return c.torn || c.disorder || c.shown != c.checked || r2 ? GPU_EERROR : GPU_OK;
}
