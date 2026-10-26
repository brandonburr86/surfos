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

Until the kernel keeps a program's floating-point registers across task switches (track B's
B2), a program with a window must not use floating point. Nothing here does.
10/26/26 (stage C12, the desktop's applications as programs): text for the desktop's clipboard
(gw_clip(), while the window has the focus), a window asking to be seen (gw_alert()), and text
drawn at a scale (gw_text_scaled(): the Clock's large digits).
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
void gw_blit(struct gw *win, int x, int y, const unsigned *pixels, int w, int h, int stride);
void gw_button(struct gw *win, int x, int y, int w, int h, const char *label, unsigned bg, unsigned fg);

#endif
