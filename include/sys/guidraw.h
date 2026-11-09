/*
SurfOS Desktop Drawing Kit
--------------------
File: guidraw.h Date: 10/25/26
--------------------
The desktop's drawing kit (shell/guidraw.c): rectangles, frames, glyphs and UTF-8 text in the
console's 8x16 font, buttons, scroll bars, the wave and the icons, each clipped to a rectangle
(struct gui_draw) of a surface (sys/framebuffer.h), and the SurfOS palette. Moved here from
shell/guipriv.h (milestone M12, stage C5 of docs/superpowers/plans/2026-10-25-app-windows.md)
so that libsurf builds the same file for programs' windows (user/libsurf/guidraw.c, libgui:
user/include/gui.h); the desktop's model includes it through shell/guipriv.h. It draws only:
no allocation, no state, no calls but the renderer's (driver/fbcore.c) and the console's
character set (driver/cp437.c).
*/
#ifndef _SYS_GUIDRAW_H
#define _SYS_GUIDRAW_H
#include <surfos/types.h>
#include <sys/framebuffer.h>

/* the SurfOS palette */
#define NAVY 0x102E3B
#define INK 0x153D49
#define TEAL 0x197D83
#define FOAM 0xB8E9D9
#define PAPER 0xF4F1E6
#define MUTED 0x6C888C
#define CORAL 0xEE906D
#define SAND 0xD0B056
#define SKY 0x659ECB
#define SHELL 0xDEE3D8                  /* light buttons */
#define BAND 0xE7E2D2                   /* gutters, headers, status bars */
#define LINE 0xC9CFC4                   /* thin separators */
#define SHADOW 0x0B2530
#define PICK 0xBFE3DD                   /* selected text and rows */
#define SCROLL_W 12                     /* a scroll bar's width */

/* where a drawing may go: the surface and a clipping rectangle (left, top, right, bottom) */
struct gui_draw { const struct fb_surface *s; int l, t, r, b; };
void gd_init(struct gui_draw *d, const struct fb_surface *s, int width, int height);
struct gui_draw gd_clip(const struct gui_draw *d, int x, int y, int w, int h);
void gd_rect(const struct gui_draw *d, int x, int y, int w, int h, u32 c);
void gd_frame(const struct gui_draw *d, int x, int y, int w, int h, u32 c);
void gd_glyph(const struct gui_draw *d, int x, int y, u8 ch, u32 c, int scale);
int gd_text(const struct gui_draw *d, int x, int y, const char *s, u32 c);     /* x after the text */
int gd_text_scaled(const struct gui_draw *d, int x, int y, const char *s, u32 c, int scale);
int gd_name(const struct gui_draw *d, int x, int y, const char *s, int cells, u32 c);   /* UTF-8, cut with … */
int gd_cells(const char *s);                                                    /* cells a UTF-8 name takes */
u32 gd_char_len(const char *s, u32 avail);            /* the bytes of the character at s (UTF-8 as the console has it) */
/* 11/05/26 (the review of stage 2): a name the desktop takes from a file (the registry's titles) checked by the rules
   kernel/win.c checks a program's menus by: gd_text_cells(), its cells (their code page 437 indexes into cell[] when not
   NULL, room for one a byte), or -1 for a string that is not whole UTF-8 by the console's rules (driver/cp437.c's
   utf8_feed()) or has a control character (C0, DEL, C1); gd_reads_as(), whether those cells read as one of names
   (lower case) in any case, blank cells around them not counted */
int gd_text_cells(const char *s, u8 *cell);
bool gd_reads_as(const u8 *cell, int n, const char *const *names, int count);
u8 gd_char(const char *s, u32 avail, u32 *len);       /* its glyph, and its bytes */
void gd_button(const struct gui_draw *d, int x, int y, int w, int h, const char *label, u32 bg, u32 fg);
void gd_wave(const struct gui_draw *d, int x, int y, int w, int h, u32 c);
void gd_scrollbar(const struct gui_draw *d, int x, int y, int h, int first, int shown, int total);
/* 10/25/26: an image of w x h pixels, 0x00RRGGBB in rows of stride, its top left at x, y */
void gd_blit(const struct gui_draw *d, int x, int y, const u32 *pixels, int w, int h, int stride);
/* 10/25/26: a line from x0, y0 to x1, y1, both ends included */
void gd_line(const struct gui_draw *d, int x0, int y0, int x1, int y1, u32 c);
#define ICON_FOLDER 0
#define ICON_OPEN_FOLDER 1
#define ICON_FILE 2
#define ICON_TEXT 3
#define ICON_DRIVE 4
#define ICON_NOTEPAD 5
#define ICON_FILES 6
#define ICON_CALC 7
#define ICON_SKETCH 8
#define ICON_WAVE 9
#define ICON_HELP 10
#define ICON_GEAR 11
#define ICON_TERMINAL 12                /* 10/17/26: the suite's */
#define ICON_MONITOR 13
#define ICON_DISKS 14
#define ICON_LOG 15
#define ICON_CLOCK 16
#define ICON_VIEWER 17
#define ICON_HEX 18
#define ICON_CHARMAP 19
#define ICON_PROGRAM 20                 /* 10/25/26: a program's window */
void gd_icon(const struct gui_draw *d, int x, int y, int icon, int scale);
void gd_icon_tint(const struct gui_draw *d, int x, int y, int icon, int scale, u32 tint);   /* one colour */
/* the scroll bar's thumb, for drawing and for the pointer: its top and height in a bar of h pixels */
void gd_thumb(int h, int first, int shown, int total, int *top, int *size);
int gd_scroll_to(int h, int y, int shown, int total);  /* the first row for the thumb's top at y */
#endif
