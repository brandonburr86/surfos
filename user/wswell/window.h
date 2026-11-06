/*
SurfOS user program: wswell
--------------------
File: window.h  Date: 11/05/26
--------------------
Swell's window (window.c) on the toolkit (gk.h): the board a program draws (gk_area: the sea and
the panel beside it) over a status bar, with the Game menu and a line in the desktop's Help. Its
logic is apart from its loop (main.c), as user/wkit/'s is, so that tests/host/test_wswell_window.c
drives it on the fake desktop (tests/host/gk_fake.c) as the screen checks (tools/gui_apps_games.py)
drive it on the desktop. The game itself is view.c and swell.c.
*/
#ifndef _WSWELL_WINDOW_H
#define _WSWELL_WINDOW_H

#include <gk.h>
#include "view.h"

/* the ids of what the window hears from */
enum {
    ID_SEA = 1,
    BOX_CLOSE = 100, BOX_HELP,
    TIMER_SEA = 200,
    MENU_NEW = 300, MENU_PAUSE, MENU_CLOSE, MENU_HELP,
};

#define SW_PANEL_LINES 5                /* the panel's numbers: score, rows, level, time, best */
#define SW_FLASHED 0x40                 /* a cell's look while its row shows washed away */

struct swwin {
    struct gk *ui;
    struct gk_widget *sea, *status;
    struct sview v;
    int bw, bh;                         /* the board's size the sea is laid out for */
    unsigned char shown[SW_H][SW_W];    /* each cell's look as the window shows it: only what changes is painted */
    int shown_state, shown_next;        /* the banner over the sea (the game's state), the next piece shown */
    char panel[SW_PANEL_LINES][24];     /* the panel's numbers as shown */
};

int ww_open(struct swwin *w, unsigned seed);    /* 0, or -1 with errno (ENODEV: no desktop here) */
int ww_event(struct swwin *w, const struct gk_event *ev);  /* 0 once the window is to close */
void ww_close(struct swwin *w);

#endif
