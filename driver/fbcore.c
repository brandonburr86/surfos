/*
SurfOS Framebuffer Renderer
--------------------
File: fbcore.c  Date: 10/5/26 (USB boot)
--------------------
Draws console cells into any direct-colour framebuffer: 15, 16, 24 or 32 bits per
pixel with the channel positions the boot loader reports (RGB or BGR), and a pitch
that may be wider than the visible line. Never reads the framebuffer back (it may be
uncached memory) and never writes outside the mode's visible lines. Pure code: no
kernel state, so tests/host/test_framebuffer.c runs it against a buffer in memory.
*/

#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <surfos/math64.h>
#include <sys/framebuffer.h>
#include <stdio.h>

/* the sixteen text mode colours (VGA attribute order) */
static const u8 vga_rgb[16][3] = {
    {   0,   0,   0 }, {   0,   0, 170 }, {   0, 170,   0 }, {   0, 170, 170 },
    { 170,   0,   0 }, { 170,   0, 170 }, { 170,  85,   0 }, { 170, 170, 170 },
    {  85,  85,  85 }, {  85,  85, 255 }, {  85, 255,  85 }, {  85, 255, 255 },
    { 255,  85,  85 }, { 255,  85, 255 }, { 255, 255,  85 }, { 255, 255, 255 },
};

static bool field_ok(u8 pos, u8 size, u32 bits) {
    return size >= 1 && size <= 8 && (u32)pos + size <= bits;
}

const char *fb_check_mode(const struct fb_mode *m, u64 *bytes) {
    u32 bytespp;
    u64 size;
    if(m->type != MB_FB_RGB) return "not a direct-colour (RGB) framebuffer";
    if(m->bpp != 15 && m->bpp != 16 && m->bpp != 24 && m->bpp != 32) return "unsupported bits per pixel";
    bytespp = (m->bpp + 7) / 8;
    if(m->width < FB_MIN_WIDTH || m->height < FB_MIN_HEIGHT) return "smaller than 640x400";
    if(m->width > FB_MAX_DIM || m->height > FB_MAX_DIM) return "implausibly large";
    if((u64)m->pitch < (u64)m->width * bytespp) return "pitch shorter than a line";
    if(!field_ok(m->red_pos, m->red_size, bytespp * 8) || !field_ok(m->green_pos, m->green_size, bytespp * 8) ||
       !field_ok(m->blue_pos, m->blue_size, bytespp * 8)) return "colour fields outside the pixel";
    size = (u64)m->pitch * m->height;
    if(size > FB_MAX_BYTES) return "larger than 256 MB";
    if(!m->addr || m->addr + size < m->addr) return "no address, or it wraps";
    if(bytes) *bytes = size;
    return NULL;
}

/* 10/10/26: pixel-space drawing, independent of the console grid. */
static u32 rgb_pixel(const struct fb_surface *s, u32 rgb) {
    return fb_rgb(s, (u8)(rgb >> 16), (u8)(rgb >> 8), (u8)rgb);
}

void fb_pixel_rgb(const struct fb_surface *s, i32 x, i32 y, u32 rgb) {
    if(x < 0 || y < 0) return;
    fb_fill(s, (u32)x, (u32)y, 1, 1, rgb_pixel(s, rgb));
}

/* Wide arithmetic also handles INT_MIN plus UINT_MAX without wrapping. */
static bool clip_rect(const struct fb_surface *s, i32 x, i32 y, u32 w, u32 h,
                      u32 *left, u32 *top, u32 *right, u32 *bottom) {
    i64 l = x, t = y, r = (i64)x + w, b = (i64)y + h;
    if(l < 0) l = 0;
    if(t < 0) t = 0;
    if(r > s->mode.width) r = s->mode.width;
    if(b > s->mode.height) b = s->mode.height;
    if(l >= r || t >= b) return false;
    *left = (u32)l; *top = (u32)t; *right = (u32)r; *bottom = (u32)b;
    return true;
}

void fb_rect_rgb(const struct fb_surface *s, i32 x, i32 y, u32 w, u32 h, u32 rgb) {
    u32 l, t, r, b;
    if(clip_rect(s, x, y, w, h, &l, &t, &r, &b)) fb_fill(s, l, t, r - l, b - t, rgb_pixel(s, rgb));
}

/* Sample only the visible part of the major axis: at most width or height
   iterations, even for INT_MIN..INT_MAX endpoints. Unsigned products fit u64;
   using the kernel's divider avoids a compiler-runtime dependency. Ties round
   away from the minor coordinate at the lower major endpoint. */
void fb_line_rgb(const struct fb_surface *s, i32 x0, i32 y0, i32 x1, i32 y1, u32 rgb) {
    i64 dx = (i64)x1 - x0, dy = (i64)y1 - y0, a0, a1, b0, b1, first, last, a, b;
    u32 major, minor, p = rgb_pixel(s, rgb);
    bool steep = (dy < 0 ? -dy : dy) > (dx < 0 ? -dx : dx);
    a0 = steep ? y0 : x0; a1 = steep ? y1 : x1;
    b0 = steep ? x0 : y0; b1 = steep ? x1 : y1;
    if(a0 > a1) { i64 tmp = a0; a0 = a1; a1 = tmp; tmp = b0; b0 = b1; b1 = tmp; }
    major = (u32)(a1 - a0);
    minor = (u32)(b1 < b0 ? b0 - b1 : b1 - b0);
    first = a0 < 0 ? 0 : a0;
    last = (steep ? s->mode.height : s->mode.width) - 1;
    if(a1 < last) last = a1;
    for(a = first; a <= last; a++) {
        u32 delta = major ? (u32)div64_32((u64)minor * (u32)(a - a0) + major / 2, major, NULL) : 0;
        b = b0 + (b1 < b0 ? -(i64)delta : (i64)delta);
        if(b >= 0 && b < (steep ? s->mode.width : s->mode.height))
            fb_fill(s, steep ? (u32)b : (u32)a, steep ? (u32)a : (u32)b, 1, 1, p);
    }
}

int fb_blit_rgb(const struct fb_surface *s, i32 x, i32 y, const struct fb_bitmap *image) {
    u32 l, t, r, b, xx, yy;
    u64 pixels;
    if(!image || !image->pixels || image->stride < image->width) return -1;
    if(!image->width || !image->height) return 0;
    pixels = (u64)(image->height - 1) * image->stride + image->width;
    if(pixels > image->bytes / sizeof(u32)) return -1;
    if(!clip_rect(s, x, y, image->width, image->height, &l, &t, &r, &b)) return 0;
    for(yy = t; yy < b; yy++) {
        const u32 *src = image->pixels + (u32)((i64)yy - y) * image->stride + (u32)((i64)l - x);
        for(xx = l; xx < r; xx++) fb_fill(s, xx, yy, 1, 1, rgb_pixel(s, *src++));
    }
    return 0;
}

u32 fb_rgb(const struct fb_surface *s, u8 r, u8 g, u8 b) {
    const struct fb_mode *m = &s->mode;
    return ((u32)(r >> (8 - m->red_size)) << m->red_pos) |
           ((u32)(g >> (8 - m->green_size)) << m->green_pos) |
           ((u32)(b >> (8 - m->blue_size)) << m->blue_pos);
}

/* The letters are as large as they were with a centred 80x25 screen: the largest whole
   scale at which 640x400 fits. 10/5/26: the grid then takes as many cells as the mode
   holds (more text, same letters), within FB_MAX_COLS x FB_MAX_ROWS, and the pixels left
   over become an even margin. 1920x1080: 120x33 at scale 2; 1280x800: 80x25 as before. */
void fb_setup(struct fb_surface *s, u8 *base, const struct fb_mode *m) {
    u32 i, sx, sy;
    s->base = base;
    s->mode = *m;
    s->bytespp = (m->bpp + 7) / 8;
    for(i = 0; i < 16; i++) s->palette[i] = fb_rgb(s, vga_rgb[i][0], vga_rgb[i][1], vga_rgb[i][2]);
    sx = m->width / FB_MIN_WIDTH;
    sy = m->height / FB_MIN_HEIGHT;
    s->scale = sx < sy ? sx : sy;
    if(s->scale < 1) s->scale = 1;
    s->cols = m->width / (FONT_WIDTH * s->scale);
    s->rows = m->height / (FONT_HEIGHT * s->scale);
    if(s->cols > FB_MAX_COLS) s->cols = FB_MAX_COLS;
    if(s->rows > FB_MAX_ROWS) s->rows = FB_MAX_ROWS;
    s->ox = (m->width - s->cols * FONT_WIDTH * s->scale) / 2;
    s->oy = (m->height - s->rows * FONT_HEIGHT * s->scale) / 2;
}

/* one horizontal run of the same pixel */
static void put_run(const struct fb_surface *s, u8 *p, u32 n, u32 pixel) {
    switch(s->bytespp) {
    case 4: {
        u32 *q = (u32 *)p;
        while(n--) *q++ = pixel;
        break;
    }
    case 3:
        while(n--) {
            p[0] = (u8)pixel;
            p[1] = (u8)(pixel >> 8);
            p[2] = (u8)(pixel >> 16);
            p += 3;
        }
        break;
    default: {
        u16 *q = (u16 *)p;
        while(n--) *q++ = (u16)pixel;
        break;
    }
    }
}

void fb_fill(const struct fb_surface *s, u32 x, u32 y, u32 w, u32 h, u32 pixel) {
    u32 r;
    if(x >= s->mode.width || y >= s->mode.height) return;
    if(w > s->mode.width - x) w = s->mode.width - x;
    if(h > s->mode.height - y) h = s->mode.height - y;
    for(r = 0; r < h; r++) put_run(s, s->base + (u_long)(y + r) * s->mode.pitch + (u_long)x * s->bytespp, w, pixel);
}

/* glyph rows [from, FONT_HEIGHT) of a cell; 10/25/26: the cursor alone is its last two (fb_draw_cursor()) */
static void glyph_rows(const struct fb_surface *s, u32 col, u32 row, u8 ch, u8 attr, bool cursor, u32 from) {
    u32 fg = s->palette[attr & 0x0F], bg = s->palette[(attr >> 4) & 0x07];  /* bit 7 is blink in text mode */
    u32 x0 = s->ox + col * FONT_WIDTH * s->scale, y0 = s->oy + row * FONT_HEIGHT * s->scale;
    u32 gy, gx, k;
    if(col >= s->cols || row >= s->rows) return;
    for(gy = from; gy < FONT_HEIGHT; gy++) {
        u8 bits = font8x16[ch][gy];
        if(cursor && gy >= FONT_HEIGHT - 2) bits = 0xFF;    /* an underline cursor */
        for(k = 0; k < s->scale; k++) {
            u8 *p = s->base + (u_long)(y0 + gy * s->scale + k) * s->mode.pitch + (u_long)x0 * s->bytespp;
            for(gx = 0; gx < FONT_WIDTH; gx++) {
                put_run(s, p, s->scale, (bits & (0x80 >> gx)) ? fg : bg);
                p += s->scale * s->bytespp;
            }
        }
    }
}

void fb_draw_glyph(const struct fb_surface *s, u32 col, u32 row, u8 ch, u8 attr, bool cursor) {
    glyph_rows(s, col, row, ch, attr, cursor, 0);
}

void fb_draw_cursor(const struct fb_surface *s, u32 col, u32 row, u8 ch, u8 attr, bool cursor) {
    glyph_rows(s, col, row, ch, attr, cursor, FONT_HEIGHT - 2);
}
