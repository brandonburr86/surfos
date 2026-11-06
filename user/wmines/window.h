/*
SurfOS user program: wmines
--------------------
File: window.h  Date: 11/05/26
--------------------
Mines' window (window.c) on the toolkit (gk.h): a toolbar (New, the field's size, the mines left
and the time), the board a program draws (gk_area) and a status bar, with the Game menu and a line
in the desktop's Help. Its logic is apart from its loop (main.c), as user/wkit/'s is, so that
tests/host/test_wmines_window.c drives it on the fake desktop (tests/host/gk_fake.c) as the screen
checks (tools/gui_apps_games.py) drive it on the desktop. The game itself is view.c and mines.c.
*/
#ifndef _WMINES_WINDOW_H
#define _WMINES_WINDOW_H

#include <gk.h>
#include "view.h"

/* the ids of what the window hears from */
enum {
    ID_NEW = 1, ID_SIZE, ID_BOARD,
    BOX_CLOSE = 100, BOX_HELP,
    TIMER_CLOCK = 200,
    MENU_NEW = 300, MENU_SIZE, MENU_CLOSE = MENU_SIZE + MINES_SIZES, MENU_HELP,
};

#define MW_CURSOR 0x80                  /* a cell's look with the keyboard's cursor on it */

struct mwin {
    struct gk *ui;
    struct gk_widget *board, *size, *left, *clock, *status;
    struct mview v;
    int keys;                           /* the board has the keys (GK_EV_FOCUS): the cursor shows */
    int bw, bh;                         /* the board's size the field is laid out for */
    int relayout;                       /* the field changed: laid out again at the next look */
    unsigned char shown[MINES_MAX];     /* each cell's look as the window shows it: only what changes is painted */
};

/* the window's content: the large field with MV_CELL cells (the desktop makes it smaller where it must) */
int mw_open(struct mwin *m, unsigned seed);     /* 0, or -1 with errno (ENODEV: no desktop here) */
int mw_event(struct mwin *m, const struct gk_event *ev);  /* 0 once the window is to close */
void mw_close(struct mwin *m);

#endif
