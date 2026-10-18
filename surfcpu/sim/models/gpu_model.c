/*
SurfGPU reference model
--------------------
File: gpu_model.c Date: 10/18/26 (SurfGPU)
--------------------
The display path and the engine of gpu_model.h, written from docs/GPU.md sections 3 and 5
pixel by pixel (never from the RTL): a destination pixel is drawn exactly when the document
says so, with no notion of words, lanes or cycles.
*/
#include "gpu_model.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ display */

/* the 16 CGA colours, the text palette after a reset */
static const uint16_t cga[16] = {0x000, 0x00A, 0x0A0, 0x0AA, 0xA00, 0xA0A, 0xA50, 0xAAA,
                                 0x555, 0x55F, 0x5F5, 0x5FF, 0xF55, 0xF5F, 0xFF5, 0xFFF};

uint16_t gpu_gpal_initial(unsigned n)
{
    unsigned r = (n >> 5) & 7, g = (n >> 2) & 7, b = n & 3;
    return (uint16_t)(((r << 1 | r >> 2) << 8) | ((g << 1 | g >> 2) << 4) | (b << 2 | b));
}

void gpu_display_reset(struct gpu_display *d, int has_gfx)
{
    memset(d, 0, sizeof *d);
    d->enable = 1;
    d->mode = 0;
    memcpy(d->pal, cga, sizeof cga);
    d->gfx_base = 0;
    d->gfx_stride = 640;
    d->gfx_scale = 1;
    d->has_gfx = has_gfx;
    for (unsigned n = 0; n < 256; n++)
        d->gpal[n] = gpu_gpal_initial(n);
}

uint16_t gpu_test_pattern(int x, int y)
{
    if (x == 0 || x == GPU_SCREEN_W - 1 || y == 0 || y == GPU_SCREEN_H - 1)
        return 0xFFF;
    if (y < 360) {
        int bar = x / 80;
        return (uint16_t)(((bar & 2) ? 0 : 0xF00) | ((bar & 4) ? 0 : 0x0F0) | ((bar & 1) ? 0 : 0x00F));
    }
    int lv = x / 40;
    if (y < 390)
        return (uint16_t)(lv << 8 | lv << 4 | lv);
    if (y < 420)
        return (uint16_t)(lv << 8);
    if (y < 450)
        return (uint16_t)(lv << 4);
    return (uint16_t)lv;
}

void gpu_display_render(const struct gpu_display *d, const uint8_t *vram, const uint32_t *textram,
                        const uint8_t font[256][16], uint16_t *frame)
{
    const int scroll = d->scroll >= GPU_TEXT_ROWS ? 0 : d->scroll;
    const uint32_t base = d->gfx_base & 0x7FFFCu, stride = d->gfx_stride & 0xFFFCu;
    const int s = d->gfx_scale == 2 ? 2 : 1;
    for (int y = 0; y < GPU_SCREEN_H; y++) {
        for (int x = 0; x < GPU_SCREEN_W; x++) {
            uint16_t c = 0;
            /* the text layer */
            int col = x / 8, srow = y / 16, gy = y % 16;
            int trow = (srow + scroll) % GPU_TEXT_ROWS;
            uint32_t cell = textram ? textram[trow * GPU_TEXT_COLS + col] : 0;
            unsigned fg = (cell >> 8) & 15, bg = (cell >> 12) & 15;
            int on = (font[cell & 0xFF][gy] >> (7 - x % 8)) & 1;
            if (d->cursor && col == d->cur_col && srow == d->cur_row && gy >= 14)
                on = 1;
            uint16_t txt = d->pal[on ? fg : bg];
            /* the graphics layer */
            uint32_t a = (base + (uint32_t)(y / s) * stride + (uint32_t)(x / s)) & 0x7FFFFu;
            uint8_t v = (vram && a < GPU_VRAM_BYTES) ? vram[a] : 0;
            uint16_t gfx = d->gpal[v];
            if (!d->enable)
                c = 0;
            else if (d->mode == 0)
                c = gpu_test_pattern(x, y);
            else if (d->mode == 1)
                c = txt;
            else if (!d->has_gfx)
                c = 0;
            else if (d->mode == 2)
                c = gfx;
            else
                c = (on || bg != 0) ? txt : gfx;
            frame[y * GPU_SCREEN_W + x] = c;
        }
    }
}

uint32_t gpu_crc32(uint32_t crc, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

uint32_t gpu_frame_crc(const uint16_t *frame)
{
    uint32_t crc = 0;
    uint8_t b[2 * GPU_SCREEN_W];
    for (int y = 0; y < GPU_SCREEN_H; y++) {
        for (int x = 0; x < GPU_SCREEN_W; x++) {
            uint16_t p = frame[y * GPU_SCREEN_W + x];
            b[2 * x] = (uint8_t)(p & 0xFF);
            b[2 * x + 1] = (uint8_t)(p >> 8);
        }
        crc = gpu_crc32(crc, b, sizeof b);
    }
    return crc;
}

/* ------------------------------------------------------------------ engine */

static int32_t sx(uint32_t w) { return (int16_t)(w & 0xFFFF); }
static int32_t sy(uint32_t w) { return (int16_t)(w >> 16); }
static uint32_t ux(uint32_t w) { return w & 0xFFFF; }
static uint32_t uy(uint32_t w) { return w >> 16; }

void gpu_engine_init(struct gpu_engine *g, uint8_t *vram)
{
    memset(g, 0, sizeof *g);
    g->vram = vram;
    g->base = 0;
    g->stride = 640;
    g->tw = 640;
    g->th = 480;
    g->cx1 = 640;
    g->cy1 = 480;
}

/* inside the effective clip (the clip and the target) */
static int inside(const struct gpu_engine *g, int64_t x, int64_t y)
{
    int64_t x1 = g->cx1 < g->tw ? g->cx1 : g->tw, y1 = g->cy1 < g->th ? g->cy1 : g->th;
    return x >= (int64_t)g->cx0 && x < x1 && y >= (int64_t)g->cy0 && y < y1;
}

static int in_target(const struct gpu_engine *g, int64_t x, int64_t y)
{
    return x >= 0 && x < (int64_t)g->tw && y >= 0 && y < (int64_t)g->th;
}

static size_t at(const struct gpu_engine *g, int64_t x, int64_t y)
{
    return (size_t)(g->base + (uint64_t)y * g->stride + (uint64_t)x);
}

static void put(struct gpu_engine *g, int64_t x, int64_t y, uint8_t c)
{
    if (inside(g, x, y)) {
        g->vram[at(g, x, y)] = c;
        g->writes++;
    }
}

static unsigned params(unsigned op)
{
    switch (op) {
    case GPU_OP_NOP: return 0;
    case GPU_OP_SET_TARGET: return 3;
    case GPU_OP_SET_CLIP: return 2;
    case GPU_OP_FILL_RECT: return 2;
    case GPU_OP_COPY_RECT: return 3;
    case GPU_OP_LINE: return 2;
    case GPU_OP_EXPAND: return 2;
    case GPU_OP_WRITE_RECT: return 2;
    case GPU_OP_FENCE: return 1;
    case GPU_OP_WAIT_VBLANK: return 0;
    default: return 99;
    }
}

static void set_target(struct gpu_engine *g)
{
    uint32_t base = g->p[0], stride = g->p[1], w = ux(g->p[2]), h = uy(g->p[2]);
    int ok = (base & 3) == 0 && (stride & 3) == 0 && base < GPU_VRAM_BYTES && stride < 65536u &&
             w < 32768u && h < 32768u;
    if (ok && w && h)
        ok = (w <= stride || h == 1) && (uint64_t)base + (uint64_t)(h - 1) * stride + w <= GPU_VRAM_BYTES;
    if (!ok) {
        g->sticky |= GPU_ST_BAD_PARAM;
        return;
    }
    g->base = base;
    g->stride = stride;
    g->tw = w;
    g->th = h;
    g->cx0 = g->cy0 = 0;
    g->cx1 = w;
    g->cy1 = h;
}

static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

/* the effective clip [*x0, *x1) x [*y0, *y1) */
static void eclip(const struct gpu_engine *g, int64_t *x0, int64_t *y0, int64_t *x1, int64_t *y1)
{
    *x0 = g->cx0;
    *y0 = g->cy0;
    *x1 = min64(g->cx1, g->tw);
    *y1 = min64(g->cy1, g->th);
}

/* every pixel of the rectangle inside the clip: walk only the part that can be inside it (the
 * rectangle may be 65535 x 65535) */
static void fill_rect(struct gpu_engine *g)
{
    int64_t x = sx(g->p[0]), y = sy(g->p[0]), w = ux(g->p[1]), h = uy(g->p[1]);
    int64_t cx0, cy0, cx1, cy1;
    eclip(g, &cx0, &cy0, &cx1, &cy1);
    uint8_t c = (uint8_t)g->hdr;
    for (int64_t py = max64(y, cy0); py < min64(y + h, cy1); py++)
        for (int64_t px = max64(x, cx0); px < min64(x + w, cx1); px++)
            put(g, px, py, c);
}

static uint8_t snapshot[GPU_VRAM_BYTES];

/* destination pixel (px, py) is written when inside the clip and its source is inside the
 * target, with the source's value as it was before the command (and not the key colour) */
static void copy_rect(struct gpu_engine *g)
{
    int64_t x0 = sx(g->p[0]), y0 = sy(g->p[0]), x1 = sx(g->p[1]), y1 = sy(g->p[1]);
    int64_t w = ux(g->p[2]), h = uy(g->p[2]);
    int key = (g->hdr & GPU_COPY_KEY) != 0;
    uint8_t keyc = (uint8_t)g->hdr;
    if (!w || !h)
        return;
    memcpy(snapshot, g->vram, GPU_VRAM_BYTES);
    int64_t cx0, cy0, cx1, cy1;
    eclip(g, &cx0, &cy0, &cx1, &cy1);
    for (int64_t py = max64(y1, cy0); py < min64(y1 + h, cy1); py++)
        for (int64_t px = max64(x1, cx0); px < min64(x1 + w, cx1); px++) {
            int64_t qx = px - x1 + x0, qy = py - y1 + y0;      /* its source */
            if (!in_target(g, qx, qy))
                continue;
            uint8_t v = snapshot[at(g, qx, qy)];
            if (key && v == keyc)
                continue;
            put(g, px, py, v);
        }
}

static void line(struct gpu_engine *g)
{
    int32_t x0 = sx(g->p[0]), y0 = sy(g->p[0]), x1 = sx(g->p[1]), y1 = sy(g->p[1]);
    uint8_t c = (uint8_t)g->hdr;
    int32_t dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int32_t stx = x0 < x1 ? 1 : -1, sty = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        put(g, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += stx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sty;
        }
    }
}

static void stream_start(struct gpu_engine *g)
{
    g->dx = g->p[0] & 0xFFFF;
    g->dy = g->p[0] >> 16;
    g->dw = ux(g->p[1]);
    g->dh = uy(g->p[1]);
    g->pix_done = 0;
    g->data_left = (uint64_t)g->dw * g->dh;
}

/* one data word of EXPAND or WRITE_RECT */
static void stream_word(struct gpu_engine *g, uint32_t w)
{
    unsigned op = g->hdr >> 24, per = op == GPU_OP_EXPAND ? 32 : 4;
    int64_t x = (int16_t)g->dx, y = (int16_t)g->dy;
    for (unsigned k = 0; k < per && g->data_left; k++) {
        uint64_t i = g->pix_done;
        int64_t px = x + (int64_t)(i % g->dw), py = y + (int64_t)(i / g->dw);
        if (op == GPU_OP_EXPAND) {
            int bit = (w >> (31 - k)) & 1;
            if (bit)
                put(g, px, py, (uint8_t)g->hdr);
            else if (!(g->hdr & GPU_EXPAND_TRANSP))
                put(g, px, py, (uint8_t)(g->hdr >> 8));
        } else {
            put(g, px, py, (uint8_t)(w >> (8 * k)));
        }
        g->pix_done++;
        g->data_left--;
    }
}

static void execute(struct gpu_engine *g)
{
    g->in_cmd = 0;
    switch (g->hdr >> 24) {
    case GPU_OP_SET_TARGET: set_target(g); break;
    case GPU_OP_SET_CLIP:
        g->cx0 = ux(g->p[0]);
        g->cy0 = uy(g->p[0]);
        g->cx1 = ux(g->p[1]);
        g->cy1 = uy(g->p[1]);
        break;
    case GPU_OP_FILL_RECT: fill_rect(g); break;
    case GPU_OP_COPY_RECT: copy_rect(g); break;
    case GPU_OP_LINE: line(g); break;
    case GPU_OP_EXPAND:
    case GPU_OP_WRITE_RECT: stream_start(g); break;
    case GPU_OP_FENCE:
        g->fence = g->p[0];
        g->fences++;
        if (g->hdr & GPU_FENCE_IRQ)
            g->sticky |= GPU_ST_FENCE_EV;
        break;
    default: break;                         /* NOP, WAIT_VBLANK */
    }
}

void gpu_engine_word(struct gpu_engine *g, uint32_t w)
{
    if (g->data_left) {
        stream_word(g, w);
        return;
    }
    if (!g->in_cmd) {
        unsigned need = params(w >> 24);
        if (need == 99) {
            g->sticky |= GPU_ST_BAD_OPCODE;
            return;
        }
        g->hdr = w;
        g->np = 0;
        g->need = need;
        g->in_cmd = 1;
        if (need == 0)
            execute(g);
        return;
    }
    g->p[g->np++] = w;
    if (g->np == g->need)
        execute(g);
}

void gpu_engine_abort(struct gpu_engine *g)
{
    g->in_cmd = 0;
    g->data_left = 0;
}

int gpu_engine_idle(const struct gpu_engine *g)
{
    return !g->in_cmd && !g->data_left;
}
