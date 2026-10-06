/*
SurfOS Framebuffer Console
----------------------
File: framebuffer.h Date: 10/5/26 (USB boot)
----------------------
On a UEFI machine there is no VGA text mode: the boot loader leaves a linear
framebuffer, and the console's cells (the same char/attribute buffer as 0xB8000)
are drawn into it with an 8x16 font in the VGA palette, scaled by a whole number.
driver/fbcore.c is the renderer (pure, tested on the host by
tests/host/test_framebuffer.c); driver/framebuffer.c maps the boot loader's
framebuffer and keeps the screen in step with the console.

10/5/26: more text, same letters. The scale is still the largest one at which 80x25
cells fit, but the grid grows to fill the screen (1920x1080: 120x33 at scale 2), up
to FB_MAX_COLS x FB_MAX_ROWS; the pixels left over are split evenly as a margin.
*/

#ifndef _SYS_FRAMEBUFFER_H
#define _SYS_FRAMEBUFFER_H

#include <surfos/types.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16
#define FB_MIN_WIDTH  640                       /* 80x25 cells at scale 1: the smallest mode taken */
#define FB_MIN_HEIGHT 400
#define FB_MAX_COLS   240                       /* the grid's limit, for very wide or tall panels */
#define FB_MAX_ROWS   100                       /* (the console's buffers hold this many, console.h) */
#define FB_MAX_DIM    16384
#define FB_MAX_BYTES  0x10000000ULL             /* the framebuffer window is 256 MB */

extern const u8 font8x16[256][FONT_HEIGHT];

/* a mode the renderer can draw into */
struct fb_mode {
    u64 addr;                   /* physical (kept 64-bit: it may be above 4 GB) */
    u32 width, height, pitch;   /* pixels, pixels, bytes per line */
    u32 bpp;                    /* 15, 16, 24 or 32 */
    u32 type;                   /* MB_FB_RGB only */
    u8 red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
};

struct fb_surface {
    u8 *base;                   /* where the first pixel is mapped */
    struct fb_mode mode;
    u32 bytespp;
    u32 palette[16];            /* the VGA colours in this pixel format */
    u32 scale;                  /* a cell is 8*scale x 16*scale pixels (80x25 of them fit) */
    u32 cols, rows;             /* the text grid: as many cells as fit, at most FB_MAX_COLS x FB_MAX_ROWS */
    u32 ox, oy;                 /* top-left pixel of the text area: the margin left over, split evenly */
};

/* fbcore.c: NULL when the mode is drawable, otherwise why not; *bytes = pitch * height */
const char *fb_check_mode(const struct fb_mode *m, u64 *bytes);
void fb_setup(struct fb_surface *s, u8 *base, const struct fb_mode *m);   /* m must have passed fb_check_mode */
u32 fb_rgb(const struct fb_surface *s, u8 r, u8 g, u8 b);
void fb_fill(const struct fb_surface *s, u32 x, u32 y, u32 w, u32 h, u32 pixel);  /* clipped to the surface */
void fb_draw_glyph(const struct fb_surface *s, u32 col, u32 row, u8 ch, u8 attr, bool cursor); /* ignored outside the grid */

/* framebuffer.c: the console's screen */
int framebuffer_init(void);     /* 0 when the boot loader's framebuffer is mapped and cleared */
void framebuffer_grid(u_int *cols, u_int *rows);   /* its text grid (after framebuffer_init) */
void framebuffer_bind(const u8 *cells);   /* the cols x rows x 2 char/attribute buffer the screen shows */
void framebuffer_draw_cell(u32 col, u32 row, u8 ch, u8 color);
void framebuffer_present(void); /* redraw every cell that differs from the buffer */
void framebuffer_set_cursor(u32 col, u32 row);
bool framebuffer_active(void);
const char *framebuffer_describe(char *buf, size_t size);  /* the mode, or why there is none */
void framebuffer_panic(void);   /* panic/wipeout: take the screen back from whoever was drawing */
struct surf_task;
void framebuffer_task_died(struct surf_task *t);           /* exception kill: release it if t was drawing */

#endif
