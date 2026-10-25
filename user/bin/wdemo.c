/*
SurfOS user program: wdemo
--------------------
File: wdemo.c   Date: 10/25/26
--------------------
A window on the desktop through libgui (gui.h): colour bars, and under them the window's size
and the last event it got, as text. It resizes with the window, and the close button ends it.
For trying programs' windows by hand and for the desktop's screen tests (milestone M12, stage C5
of docs/superpowers/plans/2026-10-25-app-windows.md).

wdemo         as above
wdemo hang    the same, but it pays no heed to the close button: the desktop asks to end it
wdemo crash   it faults after its first present: only its window goes
*/

#include <stdio.h>
#include <string.h>
#include <surf.h>
#include <gui.h>

static const unsigned bars[] = { GW_CORAL, GW_SAND, 0x7ED48C, GW_TEAL, GW_SKY, 0x9B5FA7, GW_PAPER, GW_INK };
#define BARS (int)(sizeof(bars) / sizeof(bars[0]))

static void describe(const struct win_event *e, char *out, size_t n) {
    switch(e->type) {
    case WIN_EV_KEY: snprintf(out, n, "last: key %u", e->value); break;
    case WIN_EV_POINTER: snprintf(out, n, "last: pointer %d,%d buttons %u", e->x, e->y, e->value); break;
    case WIN_EV_WHEEL: snprintf(out, n, "last: wheel %d", (int)e->value); break;
    case WIN_EV_FOCUS: snprintf(out, n, "last: focus %u", e->value); break;
    case WIN_EV_RESIZE: snprintf(out, n, "last: size %dx%d", e->x, e->y); break;
    case WIN_EV_CLOSE: snprintf(out, n, "last: close"); break;
    default: snprintf(out, n, "last: event %u", e->type); break;
    }
}

/* the bars over the top two thirds, the size and the last event in the third below */
static void draw(struct gw *win, const char *last) {
    char size[32];
    int i, h = win->h * 2 / 3;
    gw_fill(win, GW_NAVY);
    for(i = 0; i < BARS; i++) gw_rect(win, i * win->w / BARS, 0, (i + 1) * win->w / BARS - i * win->w / BARS, h, bars[i]);
    snprintf(size, sizeof(size), "wdemo %dx%d", win->w, win->h);
    gw_text(win, 8, h + 8, size, GW_FOAM);
    gw_text(win, 8, h + 28, last, GW_PAPER);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    struct gw *win;
    struct win_event ev;
    char last[80] = "last: nothing yet";
    int hang = !strcmp(mode, "hang"), crash = !strcmp(mode, "crash");
    if(argc > 1 && !hang && !crash) {
        fprintf(stderr, "usage: wdemo [hang | crash]\n");
        return 2;
    }
    win = gw_open(hang ? "wdemo hang" : crash ? "wdemo crash" : "wdemo", 360, 240, WIN_RESIZABLE, GW_NAVY);
    if(!win) {
        fprintf(stderr, "wdemo: no window: %s\n", strerror(errno));
        return 1;
    }
    draw(win, last);
    gw_present_all(win);
    if(crash) *(volatile int *)0 = 1;                   /* a fault in ring 3: this program ends, its window goes */
    while(gw_next(win, &ev, -1) > 0) {
        describe(&ev, last, sizeof(last));
        if(ev.type == WIN_EV_CLOSE && !hang) break;
        draw(win, last);
        gw_present_all(win);
    }
    gw_close(win);
    return 0;
}
