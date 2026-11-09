/*
SurfOS user program: wsolitaire
--------------------
File: window.h  Date: 11/05/26
--------------------
Solitaire's window (window.c) on the toolkit (gk.h): a toolbar (New, Undo, drawing one or three,
the moves and the time), the table a program draws (gk_area) and a status bar, with the Game menu
and a line in the desktop's Help. Its logic is apart from its loop (main.c), as user/wkit/'s is,
so that tests/host/test_wsolitaire_window.c drives it on the fake desktop (tests/host/gk_fake.c) as
the screen checks (tools/gui_apps_games.py) drive it on the desktop. The game itself is table.c
and klondike.c.
*/
#ifndef _WSOLITAIRE_WINDOW_H
#define _WSOLITAIRE_WINDOW_H

#include <gk.h>
#include "table.h"

/* the ids of what the window hears from */
enum {
    ID_NEW = 1, ID_UNDO, ID_DRAW, ID_TABLE,
    BOX_CLOSE = 100, BOX_HELP, BOX_NEW,
    TIMER_CLOCK = 200, TIMER_FINISH,
    MENU_NEW = 300, MENU_UNDO, MENU_ONE, MENU_THREE, MENU_CLOSE, MENU_HELP,
};

/* the Help box's lines: at most 50 characters each, which the smallest window's box shows (table.h's TB_MIN_W) */
#define SW_HELP_1 "Build the foundations up from the aces, by suit."
#define SW_HELP_2 "Drag cards, or click one to send it where it goes."

struct swin {
    struct gk *ui;
    struct gk_widget *table, *undo, *draw, *moves, *clock, *status;
    struct table t;
    int keys;                           /* the table has the keys (GK_EV_FOCUS): the keyboard's card shows */
    int bw, bh;                         /* the board's size the table is laid out for */
    unsigned shown[SOL_PILES];          /* each pile as the window shows it (a hash of what it draws): only what changes
                                           is painted again */
    int drag_x, drag_y, drag_w, drag_h; /* where the dragged cards were drawn (w 0: none) */
    int finish_timer;                   /* the timer of the finish runs */
    int asked;                          /* the question on the screen (table.h's TB_ASK_*), 0 for none */
};

int sw_open(struct swin *s, unsigned seed);     /* 0, or -1 with errno (ENODEV: no desktop here) */
int sw_event(struct swin *s, const struct gk_event *ev);  /* 0 once the window is to close */
void sw_close(struct swin *s);

#endif
