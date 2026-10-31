/*
SurfOS user library: windows on the desktop (libgui)
--------------------
File: gui.c     Date: 10/25/26
--------------------
gui.h's windows over winopen() and winctl() (surfos/win.h): a window keeps an image of its own in
the program's memory, which the drawing kit (guikit.c) draws on and gw_present() copies to the
desktop. gw_next() reads the window's events GW_QUEUE at a time, after a poll() when it must not
wait for ever, and answers a new size itself: a new image of that size, all background, and
WIN_SETSIZE, before the event is given (when there is no memory for it the image stays as it was,
and so does the window). Milestone M12, stage C5 of docs/superpowers/plans/2026-10-25-app-windows.md.
10/26/26 (stage C12): gw_clip() (WIN_CLIP), gw_alert() (WIN_ALERT) and text at scale.
*/

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <surf.h>
#include <gui.h>
#include "guikit.h"

/* an image of w x h, all of it the window's background */
static unsigned *image(int w, int h, unsigned background) {
    unsigned *p = malloc((size_t)w * (size_t)h * 4);
    int i;
    if(!p) return NULL;
    for(i = 0; i < w * h; i++) p[i] = background;
    return p;
}

struct gw *gw_open_limits(const char *title, int w, int h, int min_w, int min_h, int max_w, int max_h,
                          unsigned flags, unsigned background) {
    struct win_open o;
    struct win_info info;
    struct gw *win;
    size_t n = title ? strlen(title) : 0;
    int fd, err = ENOMEM;
    memset(&o, 0, sizeof(o));
    o.version = WIN_VERSION;
    o.w = (unsigned short)w;
    o.h = (unsigned short)h;
    o.min_w = (unsigned short)min_w;
    o.min_h = (unsigned short)min_h;
    o.max_w = (unsigned short)max_w;
    o.max_h = (unsigned short)max_h;
    o.flags = flags;
    o.background = background;
    memcpy(o.title, title ? title : "", n < sizeof(o.title) ? n : sizeof(o.title));   /* the kernel cuts it */
    fd = winopen(&o);
    if(fd < 0) return NULL;
    win = calloc(1, sizeof(*win));
    if(win && winctl(fd, WIN_INFO, &info) < 0) err = errno;
    else if(win && (win->kit = malloc(guikit_size())) && (win->pixels = image(info.w, info.h, background))) err = 0;
    if(err) {
        if(win) {
            free(win->kit);
            free(win);
        }
        close(fd);
        errno = err;
        return NULL;
    }
    win->fd = fd;
    win->w = info.w;
    win->h = info.h;
    win->background = background;
    guikit_setup(win->kit, win->pixels, win->w, win->h);
    return win;
}

struct gw *gw_open(const char *title, int w, int h, unsigned flags, unsigned background) {
    return gw_open_limits(title, w, h, 0, 0, 0, 0, flags, background);
}

void gw_close(struct gw *win) {
    if(!win) return;
    close(win->fd);
    free(win->pixels);
    free(win->kit);
    free(win);
}

int gw_present(struct gw *win, int x, int y, int w, int h) {
    struct win_present p;
    int r;
    if(x < 0) { w += x; x = 0; }
    if(y < 0) { h += y; y = 0; }
    if(x + w > win->w) w = win->w - x;
    if(y + h > win->h) h = win->h - y;
    if(w < 0 || h < 0) w = h = 0;
    memset(&p, 0, sizeof(p));
    p.pixels = win->pixels;
    p.stride = (unsigned)win->w;
    p.x = (unsigned short)x;
    p.y = (unsigned short)y;
    p.w = (unsigned short)w;
    p.h = (unsigned short)h;
    r = winctl(win->fd, WIN_PRESENT, &p);
    if(r < 0 && errno == EPIPE) win->gone = 1;
    return r;
}

int gw_present_all(struct gw *win) {
    return gw_present(win, 0, 0, win->w, win->h);
}

int gw_title(struct gw *win, const char *title) {
    return winctl(win->fd, WIN_TITLE, (void *)title);
}

/* 10/26/26 (stage C12) */
int gw_clip(struct gw *win, const char *text, unsigned len) {
    struct win_clip c;
    c.text = text;
    c.len = len;
    return winctl(win->fd, WIN_CLIP, &c);
}

int gw_alert(struct gw *win) {
    return winctl(win->fd, WIN_ALERT, (void *)0);
}

/* a new size: a new image, all background, then the window's (nothing changes when either cannot be had) */
static void resize(struct gw *win, int w, int h) {
    unsigned *p;
    if(w == win->w && h == win->h) return;
    p = image(w, h, win->background);
    if(!p) return;
    if(winctl(win->fd, WIN_SETSIZE, (void *)(unsigned long)(((unsigned)w << 16) | (unsigned)h)) < 0) {
        free(p);
        return;
    }
    free(win->pixels);
    win->pixels = p;
    win->w = w;
    win->h = h;
    guikit_setup(win->kit, win->pixels, w, h);
}

int gw_next(struct gw *win, struct win_event *ev, int ms) {
    struct pollfd pf;
    int n;
    for(;;) {
        if(win->given < win->queued) {
            *ev = win->queue[win->given++];
            if(ev->type == WIN_EV_RESIZE) resize(win, ev->x, ev->y);
            if(ev->type == WIN_EV_FOCUS) win->focused = ev->value != 0;
            return 1;
        }
        if(win->gone) {
            errno = EPIPE;
            return -1;
        }
        if(ms >= 0) {
            pf.fd = win->fd;
            pf.events = POLLIN;
            pf.revents = 0;
            n = poll(&pf, 1, ms);
            if(n <= 0) return n;                        /* none came, or the wait was cut short */
        }
        n = (int)read(win->fd, win->queue, sizeof(win->queue));
        if(n < 0) return -1;
        if(n == 0) {                                    /* the desktop let it go */
            win->gone = 1;
            errno = EPIPE;
            return -1;
        }
        win->queued = n / (int)sizeof(struct win_event);
        win->given = 0;
    }
}

/**** drawing ****/

void gw_fill(struct gw *win, unsigned color) {
    guikit_rect(win->kit, 0, 0, win->w, win->h, color);
}

void gw_rect(struct gw *win, int x, int y, int w, int h, unsigned color) {
    guikit_rect(win->kit, x, y, w, h, color);
}

void gw_frame(struct gw *win, int x, int y, int w, int h, unsigned color) {
    guikit_frame(win->kit, x, y, w, h, color);
}

void gw_line(struct gw *win, int x0, int y0, int x1, int y1, unsigned color) {
    guikit_line(win->kit, x0, y0, x1, y1, color);
}

int gw_text(struct gw *win, int x, int y, const char *utf8, unsigned color) {
    return guikit_text(win->kit, x, y, utf8, color);
}

int gw_text_scaled(struct gw *win, int x, int y, const char *utf8, unsigned color, int scale) {
    return guikit_text_scaled(win->kit, x, y, utf8, color, scale);
}

/* 10/26/26 (stage C12) */
void gw_glyph(struct gw *win, int x, int y, unsigned code, unsigned color, int scale) {
    guikit_glyph(win->kit, x, y, code, color, scale);
}

int gw_text_width(const char *utf8) {
    return guikit_cells(utf8) * 8;
}

void gw_blit(struct gw *win, int x, int y, const unsigned *pixels, int w, int h, int stride) {
    guikit_blit(win->kit, x, y, pixels, w, h, stride);
}

void gw_button(struct gw *win, int x, int y, int w, int h, const char *label, unsigned bg, unsigned fg) {
    guikit_button(win->kit, x, y, w, h, label, bg, fg);
}
