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

u32 fb_rgb(const struct fb_surface *s, u8 r, u8 g, u8 b) {
    const struct fb_mode *m = &s->mode;
    return ((u32)(r >> (8 - m->red_size)) << m->red_pos) |
           ((u32)(g >> (8 - m->green_size)) << m->green_pos) |
           ((u32)(b >> (8 - m->blue_size)) << m->blue_pos);
}

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
    s->ox = (m->width - FB_MIN_WIDTH * s->scale) / 2;
    s->oy = (m->height - FB_MIN_HEIGHT * s->scale) / 2;
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

void fb_draw_glyph(const struct fb_surface *s, u32 col, u32 row, u8 ch, u8 attr, bool cursor) {
    u32 fg = s->palette[attr & 0x0F], bg = s->palette[(attr >> 4) & 0x07];  /* bit 7 is blink in text mode */
    u32 x0 = s->ox + col * FONT_WIDTH * s->scale, y0 = s->oy + row * FONT_HEIGHT * s->scale;
    u32 gy, gx, k;
    if(col >= FB_COLS || row >= FB_ROWS) return;
    for(gy = 0; gy < FONT_HEIGHT; gy++) {
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
