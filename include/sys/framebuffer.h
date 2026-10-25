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
Until the kernel heap works the screen shows the top left 80x25 of that grid (from
the console's static buffer); framebuffer_grow() then takes the whole of it.
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
/* 10/25/26: the cursor's rows of a cell alone (its bottom two glyph rows): the underline, or the glyph's own rows
   again; the rest of the cell is left as drawn */
void fb_draw_cursor(const struct fb_surface *s, u32 col, u32 row, u8 ch, u8 attr, bool cursor);

/* 10/10/26: software 2D. Colours are 0x00RRGGBB, coordinates are signed;
   all drawing clips to the visible pixels and leaves pitch padding alone.
   Source pixels must be in ordinary RAM, not overlap the destination. */
struct fb_bitmap {
    const u32 *pixels;
    u32 width, height, stride;  /* stride in u32 pixels, including row padding */
    size_t bytes;              /* readable source allocation, validated before drawing */
};
void fb_pixel_rgb(const struct fb_surface *s, i32 x, i32 y, u32 rgb);
void fb_rect_rgb(const struct fb_surface *s, i32 x, i32 y, u32 w, u32 h, u32 rgb);
void fb_line_rgb(const struct fb_surface *s, i32 x0, i32 y0, i32 x1, i32 y1, u32 rgb);
int fb_blit_rgb(const struct fb_surface *s, i32 x, i32 y, const struct fb_bitmap *image);

/* Kernel callers only. One active local console/task owns a graphics session.
   begin returns -1 if unavailable/busy; draw returns -1 after ownership is lost.
   The callback is synchronous, with preemption disabled: do not sleep, allocate,
   print, retain the surface, or call these session APIs from the callback.
   Interrupts remain enabled. F1-F4 switching, death and panic revoke ownership.
   Always end on every exit path (including an interrupted wait). */
int framebuffer_graphics_begin(void);
bool framebuffer_graphics_owned(void);
int framebuffer_graphics_draw(void (*draw)(const struct fb_surface *, void *), void *arg);
void framebuffer_graphics_end(void);
void framebuffer_console_switched(void); /* console.c, after changing conActive */

/* framebuffer.c: the console's screen */
int framebuffer_init(void);     /* 0 when the boot loader's framebuffer is mapped and cleared */
void framebuffer_bind(const u8 *cells);   /* the 80x25x2 char/attribute buffer the screen shows at first */
void framebuffer_grid(u_int *cols, u_int *rows);   /* the whole grid the mode holds (after framebuffer_init) */
int framebuffer_grow(const u8 *cells);    /* show that whole grid from this cols x rows x 2 buffer; kernel heap; -1: no memory */
void framebuffer_draw_cell(u32 col, u32 row, u8 ch, u8 color);
void framebuffer_present(void); /* redraw every cell that differs from the buffer */
void framebuffer_set_cursor(u32 col, u32 row);
/* 10/25/26 (track A, A3): a present that may wait up to 20 ms, batching a burst of output into one (scrolling,
   erasing, a console switch); the immediate one stays for panics and graphics. framebuffer_flush(): a pending
   present now (before a reader waits). framebuffer_start(): the thread that does them, once there are tasks. */
void framebuffer_present_lazy(void);
void framebuffer_flush(void);
void framebuffer_start(void);
struct fb_counts {
    u32 presents;               /* passes over the whole grid */
    u32 glyphs;                 /* cells drawn whole */
    u32 cursors;                /* cursor moves drawn as two rows */
    u32 saved;                  /* presents asked for while one was pending: folded into it */
    u32 slowest_ms;             /* the longest present */
};
void framebuffer_counts(struct fb_counts *out);
bool framebuffer_active(void);
const char *framebuffer_describe(char *buf, size_t size);  /* the mode, or why there is none */
void framebuffer_panic(void);   /* panic/wipeout: take the screen back from whoever was drawing */
struct surf_task;
void framebuffer_task_died(struct surf_task *t);           /* exception kill: release it if t was drawing */

#endif
