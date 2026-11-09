/*
SurfOS user program: wcode
--------------------
File: window.h  Date: 11/05/26
--------------------
The Code Editor's window (window.c), on the window toolkit (gk.h): a tab for each file, the text
with its line numbers and scroll bars, a find bar, find in files with its list of what it found,
the status bar, and the menus in the desktop's bar. It is apart from the program's loop
(main.c), so that tests/host/test_wcode_win.c drives it on the fake desktop (tests/host/gk_fake.c)
as the screen checks drive it on the desktop (tools/gui_apps_code.py). What a key or a click does
to the files is app.c's; this draws, and asks the desktop for what app.c says it needs.
*/
#ifndef _WCODE_WINDOW_H
#define _WCODE_WINDOW_H

#include <gk.h>
#include "app.h"

#define WC_WIN_W 800                    /* the window's content as it opens (the desktop may give less) */
#define WC_WIN_H 440
#define WC_CW    8                      /* a cell of the console's font */
#define WC_CH    16

/* the ids of what the window hears from (the menus' items are app.c's commands, WC_CMD_*) */
enum {
    ID_TABS = 1, ID_TEXT, ID_FIND, ID_WITH, ID_CASE, ID_WORD, ID_REGEX, ID_PREV, ID_NEXT, ID_REPLACE_ONE,
    ID_REPLACE_ALL, ID_FIND_CLOSE, ID_FILES_WHAT, ID_FILES_IN, ID_FILES_NAMES, ID_FILES_FOLDER, ID_FILES_GO,
    ID_FILES_CLOSE, ID_FILES_LIST,
    BOX_SAVE = 100, BOX_GOTO, BOX_ABOUT, BOX_DISK, BOX_ALL,
    CHOOSE_OPEN = 200, CHOOSE_SAVE, CHOOSE_FOLDER,
    MENU_LANGUAGE = 300, MENU_ABOUT,
    TIMER_DRAG = 400,
};

struct wc_window {
    struct gk *ui;
    struct wc_app app;
    struct gk_widget *tabs, *findbar, *find, *with, *replace_row, *icase, *word, *regex, *text, *files, *fwhat, *fin,
                     *fnames, *fgo, *flist, *fsum, *status;
    struct wc_cell *cells, *drawn;      /* the rows as rendered now, and as they were last drawn */
    unsigned rows, cols, gutter;        /* the text in cells (its columns past the line numbers' gutter cells) */
    unsigned top, left, lines;          /* where the cells come from; the rows with a line */
    int crow, ccol;                     /* the caret's cell as drawn, -1 out of view */
    unsigned cline;                     /* the caret's line */
    int doc_drawn;                      /* the tab whose text is drawn */
    int focused;                        /* the text has the keys */
    int idle;                           /* more to do between events */
    int relayout;                       /* the layout changed (a new size, a bar shown): the toolkit lays it out first */
    int greyed;                         /* 11/09/26: the find bar's fields greyed, Replace all changing the file */
    int dragging, drag_x, drag_y;       /* the left button held in the text, and where the pointer is */
    unsigned listed;                    /* find in files' lines in its list */
    char dir[WC_PATH];                  /* the folder of the last file chosen, where a chooser starts */
    char right[96], title[WC_PATH + 24];    /* the status bar's right part and the title, as shown */
    char *clip;                         /* the clipboard's text: WIN_CLIP_MAX bytes and a NUL */
};

/* the window, with one new file: 0, or -1 with errno (ENODEV: this terminal is no desktop's; ENOMEM) */
int wc_window_open(struct wc_window *w, const struct wc_app_io *io, const struct wc_fsops *fs);
int wc_window_path(struct wc_window *w, const char *path, int exists);  /* an argument: opened, or a new file */
int wc_window_event(struct wc_window *w, const struct gk_event *ev);    /* 0 once the program is to end */
int wc_window_idle(struct wc_window *w);                                /* between events: 1 while more is left */
void wc_window_close(struct wc_window *w);

#endif
