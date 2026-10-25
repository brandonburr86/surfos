/*
SurfGPU reference model
--------------------
File: gpu_model.c Date: 10/18/26 (SurfGPU), 10/25/26 (GPU2)
--------------------
The display path and the engine of gpu_model.h, written from docs/GPU.md sections 3, 5 and 16
pixel by pixel (never from the RTL): a destination pixel is drawn exactly when the document
says so, with no notion of lanes or cycles. TRIANGLE's coverage and values are computed for
each pixel on its own, exactly (128-bit integers where the products are wide); only the order
of its accesses follows the walk of section 16.5 (rows, then groups of four pixels: a group's
depth reads before its depth writes before its colour write), which decides the result when a
program lets the depth buffer overlap the target.
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
    static uint32_t table[256];
    static int have_table;
    if (!have_table) {                      /* the reflected polynomial, one byte at a time */
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            table[i] = c;
        }
        have_table = 1;
    }
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
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
    case GPU_OP_SET_ZBUF: return 2;
    case GPU_OP_TRIANGLE: return 6;
    case GPU_OP_FLIP: return 1;
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
    g->zvalid = 0;                          /* the depth buffer belonged to the old target */
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

/* ------------------------------------------------------------------ GPU2: depth, triangles */

/* SET_ZBUF: refused unless every pixel of the target has its 16-bit depth in VRAM at base +
 * y * stride + 2x, two lines never sharing a word (the rule of SET_TARGET with 2 bytes a pixel) */
static void set_zbuf(struct gpu_engine *g)
{
    uint32_t base = g->p[0], stride = g->p[1];
    int ok = (base & 3) == 0 && (stride & 3) == 0 && base < GPU_VRAM_BYTES && stride < 65536u;
    if (ok && g->tw && g->th)
        ok = (2 * g->tw <= stride || g->th == 1) &&
             (uint64_t)base + (uint64_t)(g->th - 1) * stride + 2u * g->tw <= GPU_VRAM_BYTES;
    if (!ok) {
        g->sticky |= GPU_ST_BAD_PARAM;
        return;
    }
    g->zbase = base;
    g->zstride = stride;
    g->zvalid = 1;
}

static int64_t area2_of(const int32_t vx[3], const int32_t vy[3])
{
    return (int64_t)(vx[1] - vx[0]) * (vy[2] - vy[0]) - (int64_t)(vy[1] - vy[0]) * (vx[2] - vx[0]);
}

/* floor(n / d) for d > 0 (C's division truncates toward zero) */
static int64_t floor_div(int64_t n, int64_t d)
{
    int64_t q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

static __int128 floor_div128(__int128 n, int64_t d)
{
    __int128 q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

int gpu_tri_setup(struct gpu_tri *t, const int32_t vx[3], const int32_t vy[3])
{
    memset(t, 0, sizeof *t);
    t->area2 = area2_of(vx, vy);
    const int ccw = t->area2 < 0;           /* counter-clockwise on the screen: 1 and 2 swapped */
    for (int i = 0; i < 3; i++) {
        const int j = (ccw && i) ? 3 - i : i;
        t->x[i] = vx[j];
        t->y[i] = vy[j];
    }
    for (int e = 0; e < 3; e++) {
        const int b = (e + 1) % 3;
        t->ex[e] = t->x[b] - t->x[e];
        t->ey[e] = t->y[b] - t->y[e];
        t->tl[e] = t->ey[e] < 0 || (t->ey[e] == 0 && t->ex[e] > 0);
    }
    return t->area2 != 0;
}

int gpu_tri_inside(const struct gpu_tri *t, int64_t px, int64_t py)
{
    if (t->area2 == 0)
        return 0;
    const int64_t cx = 16 * px + 8, cy = 16 * py + 8;     /* the pixel's centre, in 12.4 */
    for (int e = 0; e < 3; e++) {
        const int64_t E = (int64_t)t->ex[e] * (cy - t->y[e]) - (int64_t)t->ey[e] * (cx - t->x[e]);
        if (E < 0 || (E == 0 && !t->tl[e]))
            return 0;
    }
    return 1;
}

void gpu_plane_setup(struct gpu_plane *p, const int32_t vx[3], const int32_t vy[3], const uint32_t v[3])
{
    int64_t a = area2_of(vx, vy);
    const int64_t d1 = (int64_t)v[1] - (int64_t)v[0], d2 = (int64_t)v[2] - (int64_t)v[0];
    int64_t nx = d1 * (vy[2] - vy[0]) - d2 * (vy[1] - vy[0]);
    int64_t ny = d2 * (vx[1] - vx[0]) - d1 * (vx[2] - vx[0]);
    if (a < 0) {                            /* floor(n / a) = floor(-n / -a) */
        a = -a;
        nx = -nx;
        ny = -ny;
    }
    p->x0 = vx[0];
    p->y0 = vy[0];
    p->v0 = v[0];
    p->gx = a ? floor_div(nx * (int64_t)(1 << 20), a) : 0;
    p->gy = a ? floor_div(ny * (int64_t)(1 << 20), a) : 0;
    p->vmin = p->vmax = v[0];
    for (int i = 1; i < 3; i++) {
        if ((int64_t)v[i] < p->vmin) p->vmin = v[i];
        if ((int64_t)v[i] > p->vmax) p->vmax = v[i];
    }
}

uint32_t gpu_plane_value(const struct gpu_plane *p, int64_t px, int64_t py, unsigned bits, int *clamped, int *wide)
{
    const __int128 dx = (__int128)16 * px + 8 - p->x0, dy = (__int128)16 * py + 8 - p->y0;
    const __int128 v = (__int128)p->v0 * (1 << 20) + (__int128)p->gx * dx + (__int128)p->gy * dy;
    const __int128 lim = (__int128)1 << (bits - 1);
    if (wide)
        *wide = v < -lim || v >= lim;
    const int64_t t = (int64_t)floor_div128(v + (1 << 19), 1 << 20);   /* to the nearest, halves up */
    const int64_t c = t < p->vmin ? p->vmin : t > p->vmax ? p->vmax : t;
    if (clamped)
        *clamped = c != t;
    return (uint32_t)c;
}

static int32_t min3(const int32_t v[3]) { int32_t m = v[0] < v[1] ? v[0] : v[1]; return m < v[2] ? m : v[2]; }
static int32_t max3(const int32_t v[3]) { int32_t m = v[0] > v[1] ? v[0] : v[1]; return m > v[2] ? m : v[2]; }

static uint32_t zaddr(const struct gpu_engine *g, int64_t x, int64_t y)
{
    return (uint32_t)(g->zbase + (uint64_t)y * g->zstride + 2u * (uint64_t)x);
}

/* TRIANGLE (docs/GPU.md section 16): every pixel of the effective clip whose centre the triangle
 * covers is drawn, with the flat or the interpolated colour, where its depth passes */
static void triangle(struct gpu_engine *g)
{
    const uint32_t h = g->hdr;
    int32_t vx[3], vy[3];
    uint32_t vz[3], vc[3];
    for (int i = 0; i < 3; i++) {
        vx[i] = sx(g->p[2 * i]);
        vy[i] = sy(g->p[2 * i]);
        vz[i] = g->p[2 * i + 1] & 0xFFFFu;
        vc[i] = (g->p[2 * i + 1] >> 16) & 0xFFu;
    }
    const int gouraud = (h & GPU_TRI_GOURAUD) != 0, ztest = (h & GPU_TRI_ZTEST) != 0;
    const int zle = (h & GPU_TRI_ZFUNC_LE) != 0, zwrite = (h & GPU_TRI_ZWRITE) != 0;
    g->tri_cmds++;
    if ((ztest || zwrite) && !g->zvalid) {          /* depth without a depth buffer: refused */
        g->sticky |= GPU_ST_BAD_PARAM;
        return;
    }
    struct gpu_tri t;
    if (!gpu_tri_setup(&t, vx, vy)) {
        g->tri_zero++;
        return;
    }
    if ((t.area2 > 0 && (h & GPU_TRI_CULL_CW)) || (t.area2 < 0 && (h & GPU_TRI_CULL_CCW))) {
        g->tri_culled++;
        return;
    }
    struct gpu_plane pz, pc;
    gpu_plane_setup(&pz, vx, vy, vz);
    gpu_plane_setup(&pc, vx, vy, vc);
    /* the pixels whose centres can be inside (a centre at x is 16 x + 8), cut by the clip */
    int64_t cx0, cy0, cx1, cy1;
    eclip(g, &cx0, &cy0, &cx1, &cy1);
    const int64_t x0 = max64(floor_div((int64_t)min3(vx) + 7, 16), cx0);
    const int64_t x1 = min64(floor_div((int64_t)max3(vx) - 8, 16), cx1 - 1);
    const int64_t y0 = max64(floor_div((int64_t)min3(vy) + 7, 16), cy0);
    const int64_t y1 = min64(floor_div((int64_t)max3(vy) - 8, 16), cy1 - 1);
    if (x0 > x1 || y0 > y1)
        return;
    g->tri_drawn++;
    for (int64_t py = y0; py <= y1; py++) {
        for (int64_t gx4 = x0 - (x0 & 3); gx4 <= x1; gx4 += 4) {     /* the target's words */
            int cov[4], pass[4], any = 0;
            uint32_t z[4] = {0, 0, 0, 0}, c[4] = {0, 0, 0, 0};
            for (int L = 0; L < 4; L++) {
                const int64_t x = gx4 + L;
                cov[L] = x >= x0 && x <= x1 && gpu_tri_inside(&t, x, py);
                pass[L] = cov[L];
                if (!cov[L])
                    continue;
                any = 1;
                g->tri_pixels++;
                int cl = 0, wd = 0;
                if (ztest || zwrite) {
                    z[L] = gpu_plane_value(&pz, x, py, 37, &cl, &wd);
                    g->tri_clamped += (unsigned)cl;
                    g->tri_wide += (unsigned)wd;
                }
                if (gouraud) {
                    c[L] = gpu_plane_value(&pc, x, py, 29, &cl, &wd);
                    g->tri_clamped += (unsigned)cl;
                    g->tri_wide += (unsigned)wd;
                } else {
                    c[L] = h & 0xFFu;
                }
            }
            if (!any)
                continue;
            if (ztest) {                            /* the group's reads, before its writes */
                for (int L = 0; L < 4; L++) {
                    if (!cov[L])
                        continue;
                    const uint32_t a = zaddr(g, gx4 + L, py);
                    const uint32_t zb = (uint32_t)g->vram[a] | (uint32_t)g->vram[a + 1] << 8;
                    pass[L] = zle ? z[L] <= zb : z[L] < zb;
                    g->tri_zfail += (unsigned)!pass[L];
                }
            }
            if (zwrite) {                           /* lanes 0 and 1's word, then 2 and 3's */
                for (int L = 0; L < 4; L++) {
                    if (!pass[L])
                        continue;
                    const uint32_t a = zaddr(g, gx4 + L, py);
                    g->vram[a] = (uint8_t)z[L];
                    g->vram[a + 1] = (uint8_t)(z[L] >> 8);
                }
            }
            for (int L = 0; L < 4; L++) {           /* then the colour */
                if (!pass[L])
                    continue;
                g->vram[at(g, gx4 + L, py)] = (uint8_t)c[L];
                g->writes++;
            }
        }
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
    case GPU_OP_SET_ZBUF: set_zbuf(g); break;
    case GPU_OP_TRIANGLE: triangle(g); break;
    case GPU_OP_FLIP:                       /* video GFX_BASE keeps bits 18:2 */
        g->flip_base = g->p[0] & 0x7FFFCu;
        g->flips++;
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
