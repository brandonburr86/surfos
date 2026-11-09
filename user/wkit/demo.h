/*
SurfOS user program: wkit
--------------------
File: demo.h    Date: 11/05/26
--------------------
The toolkit's demonstration (gk.h; the desktop applications, track kit): a window with every
widget, its window logic apart from its loop (main.c) so that tests/host/test_wkit.c drives it on
the fake desktop (tests/host/gk_fake.c) as the screen checks (tools/gui_apps_kit.py) drive it on
the desktop. It is the pattern for the applications: open, then each event, then close.
*/
#ifndef _WKIT_DEMO_H
#define _WKIT_DEMO_H

#include <gk.h>

#define DEMO_W 600                      /* the window's content */
#define DEMO_H 360
#define DEMO_CELLS 64                   /* the drawing's cells each way */
#define DEMO_CELL 20                    /* and their pixels */

/* the ids of what the program hears from */
enum {
    ID_TABS = 1, ID_NAME, ID_SECRET, ID_WIFI, ID_JOIN, ID_SPEED, ID_GREET, ID_ASK, ID_RENAME, ID_COPY,
    ID_LIST, ID_ADD, ID_REMOVE, ID_BAR, ID_AREA, ID_CLEAR,
    ID_NOTES, ID_VOLUME, ID_COLORS, ID_TOOLS,
    BOX_GREET = 100, BOX_ASK, BOX_RENAME, BOX_CLOSE, BOX_ABOUT,
    TIMER_SECONDS = 200,
    MENU_OPEN = 300, MENU_SAVE, MENU_CLOSE, MENU_PAGE, MENU_WIFI = MENU_PAGE + 8,  /* MENU_PAGE + n: the page n */
    MENU_SPEEDS, MENU_SPEED, MENU_ABOUT = MENU_SPEED + 3,                       /* MENU_SPEED + n: the speed n */
    CHOOSE_OPEN = 400, CHOOSE_SAVE,
};

struct demo {
    struct gk *ui;
    struct gk_widget *tabs, *name, *secret, *wifi, *join, *speed, *list, *picked, *bar, *barval, *area, *status;
    struct gk_widget *notes, *minute, *volume, *colors, *tools;  /* More (stage 2) */
    int cx, cy;                         /* the drawing's cursor cell */
    int area_keys;                      /* the drawing has the keys (GK_EV_FOCUS) */
    int added;                          /* lines Add has added */
    int pages;                          /* the tabs' pages, the View menu's items */
    unsigned seconds;
    int busy, busy_lost;                /* Open was asked after a busy spell (Ctrl+Shift+O), and the events lost in it */
    unsigned char mark[DEMO_CELLS][DEMO_CELLS];
};

int demo_open(struct demo *d);          /* 0, or -1 with errno (ENODEV: no desktop here) */
int demo_event(struct demo *d, const struct gk_event *ev);     /* 0 once the window is to close */
void demo_close(struct demo *d);

#endif
