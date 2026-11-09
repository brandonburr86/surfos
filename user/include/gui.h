/*
SurfOS window library (libgui, part of libsurf)
----------------------
File: gui.h     Date: 10/25/26
----------------------
A window on the desktop for a program (surfos/win.h has the system calls and their rules;
milestone M12, docs/superpowers/plans/2026-10-25-app-windows.md), with an image of its own to
draw in. Draw with the gw_ calls below, clipped to the image, in 0x00RRGGBB colours (the
desktop's palette is GW_*), and gw_present() what changed: the desktop shows it. gw_next()
waits for the next event (struct win_event: a key, the pointer, the wheel, the focus, a new
size, the close button). A new size is handled there: the image is made again at that size,
all background, and the event is returned, so the program draws everything again and presents
it.

The drawing is the desktop's own (shell/guidraw.c): text is UTF-8 shown with the console's
characters (code page 437, '?' for any other), 8 by 16 pixels each.

Floating point (11/05/26): since robustness B2 (10/25/26) the kernel keeps each program's
floating-point registers across task switches, so a program with a window may use it. libgui and
the toolkit use none: their drawing is in whole pixels, the same on every machine.
10/26/26 (stage C12, the desktop's applications as programs): text for the desktop's clipboard
(gw_clip(), while the window has the focus), a window asking to be seen (gw_alert()), and text
drawn at a scale (gw_text_scaled(): the Clock's large digits); one of the console's characters by
its code (gw_glyph(): the Character Map's grid).
11/05/26 (the desktop applications, track kit): the view every drawing goes through (an origin and a
clip), a character's code and text cut to cells; the wrappers of track desk's calls (menus in the
desktop's bar, the clipboard read, its choosers, the theme). A program builds its window of widgets
with the toolkit, gk.h, which stands on this.
*/
#ifndef _GUI_H
#define _GUI_H

#include <surfos/win.h>

/* the desktop's colours */
#define GW_NAVY  0x102E3B
#define GW_INK   0x153D49
#define GW_TEAL  0x197D83
#define GW_FOAM  0xB8E9D9
#define GW_PAPER 0xF4F1E6
#define GW_MUTED 0x6C888C
#define GW_CORAL 0xEE906D
#define GW_SAND  0xD0B056
#define GW_SKY   0x659ECB
#define GW_SHELL 0xDEE3D8           /* light buttons */
#define GW_BAND  0xE7E2D2           /* bars and headers */
#define GW_LINE  0xC9CFC4           /* thin separators */

#define GW_QUEUE 16

/* A window and its image. Read its fields; change them only through the calls. */
struct gw {
    int fd;                         /* its descriptor: poll() it beside others (POLLIN: an event, or its end) */
    int w, h;                       /* its image's size, which is its content's */
    unsigned *pixels;               /* the image: w x h pixels, rows of w, 0x00RRGGBB */
    unsigned background;
    int focused;                    /* what the last WIN_EV_FOCUS said */
    int gone;                       /* the desktop let it go: no more events, presents fail */
    void *kit;                      /* the drawing kit's */
    struct win_event queue[GW_QUEUE];   /* events read and not yet given */
    int queued, given;
};

/* a window of w x h (as the desktop allows: gw->w and gw->h say what it got), WIN_RESIZABLE or 0, its image all
   background; NULL with errno set (ENODEV: this program's terminal is no desktop's) */
struct gw *gw_open(const char *title, int w, int h, unsigned flags, unsigned background);
/* the same, within min_w..max_w and min_h..max_h (0: no limit) */
struct gw *gw_open_limits(const char *title, int w, int h, int min_w, int min_h, int max_w, int max_h,
                          unsigned flags, unsigned background);
void gw_close(struct gw *win);

/* the desktop shows the image's rectangle (clipped to the image) or all of it: 0, or -1 with errno (EPIPE: gone) */
int gw_present(struct gw *win, int x, int y, int w, int h);
int gw_present_all(struct gw *win);
int gw_title(struct gw *win, const char *title);                /* UTF-8, cut at 63 bytes between characters */
/* 10/26/26: len bytes of text for the desktop's clipboard (1 to WIN_CLIP_MAX, up to a NUL: errno EINVAL for none),
   while the window has the focus (errno EACCES otherwise); the window asks to be seen (in front, without the focus,
   its taskbar button marked), once in WIN_ALERT_MS for the program (errno EAGAIN). 0, or -1 with errno (EINVAL from
   a kernel without them: carry on without) */
int gw_clip(struct gw *win, const char *text, unsigned len);
int gw_alert(struct gw *win);

/* the next event within ms milliseconds (ms < 0: as long as it takes): 1 with it in *ev, 0 when none came, -1 when
   the window has gone (errno EPIPE) or a call failed (errno; EINTR: a kill or Ctrl+C) */
int gw_next(struct gw *win, struct win_event *ev, int ms);

/* drawing in the image */
void gw_fill(struct gw *win, unsigned color);
void gw_rect(struct gw *win, int x, int y, int w, int h, unsigned color);
void gw_frame(struct gw *win, int x, int y, int w, int h, unsigned color);
void gw_line(struct gw *win, int x0, int y0, int x1, int y1, unsigned color);
int gw_text(struct gw *win, int x, int y, const char *utf8, unsigned color);   /* x after it */
int gw_text_scaled(struct gw *win, int x, int y, const char *utf8, unsigned color, int scale);   /* 10/26/26 */
int gw_text_width(const char *utf8);                                           /* 8 pixels a character */
/* 10/26/26: one of the console's characters by its code (code page 437, 0-255: 0x01 is ☺, 0xF7 ≋), scale x scale
   pixels to each of its 8x16 */
void gw_glyph(struct gw *win, int x, int y, unsigned code, unsigned color, int scale);
void gw_blit(struct gw *win, int x, int y, const unsigned *pixels, int w, int h, int stride);
/* a button: a rectangle of bg with its label in fg in the middle. 11/05/26: the label is UTF-8, as gw_text()'s (until
   then its bytes were drawn as code page 437's, the desktop's gd_button() way: the same for the ASCII every program
   gave it) */
void gw_button(struct gw *win, int x, int y, int w, int h, const char *label, unsigned bg, unsigned fg);

/* 11/05/26 (the desktop applications, track kit): where drawing goes, which the toolkit (gk.h) narrows to a widget or
   to an area a program draws. A view is an origin, the image's pixel that drawing's 0, 0 is, and a clip, the image's
   rectangle that drawing stays inside (l, t, r, b: r and b past its last pixel). gw_open() and every new size give the
   whole image with its origin at 0, 0. gw_clip_to() narrows the clip to a rectangle in drawing's coordinates, and
   gw_origin() moves drawing's 0, 0 by x, y; gw_get_view() and gw_set_view() keep and restore a view. */
struct gw_view { int ox, oy, l, t, r, b; };
void gw_get_view(const struct gw *win, struct gw_view *v);
void gw_set_view(struct gw *win, const struct gw_view *v);
void gw_view_reset(struct gw *win);
void gw_clip_to(struct gw *win, int x, int y, int w, int h);
void gw_origin(struct gw *win, int x, int y);
/* the console's code (code page 437) of the character at utf8, as gw_text() draws it, and its bytes in *len (of avail:
   UTF-8 by the console's rules, a byte that starts none is the 437 character it is) */
int gw_char(const char *utf8, int avail, int *len);
/* at most cells characters, a longer text's last cell a dot (as the desktop cuts names): x after it */
int gw_text_cut(struct gw *win, int x, int y, const char *utf8, int cells, unsigned color);

/* 11/05/26 (the desktop applications: track desk's calls, surfos/win.h): the window's menus in the desktop's bar
   (count entries, WIN_MENU_TITLE starting each menu; 0: none), the desktop's clipboard read into buf (its length; size 0
   only says it; -1 ENOSPC for a buf too small, EACCES without the focus), a chooser asked for (WIN_EV_DIALOG answers),
   the path it chose taken (its length), the theme and its colours (asked once, WIN_EV_THEME tells of a new one). Each
   is 0 or a length, or -1 with errno: ENOSYS from a desktop that does not do it yet, EINVAL from a kernel before it */
int gw_menu(struct gw *win, const struct win_menu_item *items, unsigned count);
int gw_paste(struct gw *win, char *buf, unsigned size);
int gw_dialog(struct gw *win, const struct win_dialog *d);
int gw_take(struct gw *win, char *buf, unsigned size);
int gw_theme(struct gw *win, struct win_theme *t);

#endif
