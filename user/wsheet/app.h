/*
SurfOS user program: wsheet
--------------------
File: app.h     Date: 11/05/26
--------------------
The Spreadsheet's window (app.c), on the toolkit (gk.h): a formula bar (the cell's name, which takes
a name or a range to go to, and what the cell holds, to edit), the grid the program draws (an area:
view.c is what it shows), a status bar, and the menus File, Edit, Sheet, Format and the program's
part of the desktop's Help. Its logic is apart from its loop (main.c) and from the files' system calls
(io.c, through struct sheet_io), so that tests/host/test_wsheet_win.c drives it on the fake desktop
(tests/host/gk_fake.c) with files in memory, as the screen checks (tools/gui_apps_sheet.py) drive it
on the desktop with files on a stick.
*/
#ifndef _WSHEET_APP_H
#define _WSHEET_APP_H

#include <gk.h>
#include "sheet.h"
#include "view.h"

#define APP_W 760                       /* the window's content when it opens, and its least */
#define APP_H 440
#define APP_MIN_W 360
#define APP_MIN_H 200
#define APP_CHUNK 65536                 /* a file is read this much at a time */
#define APP_CHUNKS 4                    /* pieces read between two looks at the window's events */
#define APP_MSG 200

/* the files, as main.c reaches them (io.c) and the tests fake them: a handle, its bytes; -1 and errno */
struct sheet_io {
    void *ctx;
    int (*open)(void *ctx, const char *path, long *size);  /* a file to read: a handle, or -1 (errno) */
    int (*read)(void *ctx, int h, char *buf, int n);        /* bytes read, 0 at its end, -1 (errno) */
    void (*close)(void *ctx, int h);
    /* the sheet written to path as kind (SH_NATIVE, SH_CSV): 0, or -1 with why */
    int (*save)(void *ctx, const char *path, const struct sheet *s, int kind, char *why, int size);
};

extern const struct sheet_io io_files;  /* io.c's: the system calls (SurfOS only) */

/* the ids of what the window hears from */
enum {
    ID_NAME = 1, ID_FORMULA, ID_GRID,
    BOX_SAVE = 100, BOX_CSV, BOX_WIDTH, BOX_FAILED, BOX_KEYS, BOX_ABOUT,
    MENU_NEW = 200, MENU_OPEN, MENU_SAVE, MENU_SAVE_AS, MENU_CLOSE,
    MENU_UNDO, MENU_REDO, MENU_CUT, MENU_COPY, MENU_PASTE, MENU_DELETE, MENU_SELECT_ALL, MENU_GOTO, MENU_EDIT,
    MENU_INS_ROWS, MENU_INS_COLS, MENU_DEL_ROWS, MENU_DEL_COLS, MENU_WIDTH, MENU_WIDTH_DEFAULT,
    MENU_GENERAL, MENU_FIXED, MENU_PERCENT, MENU_MORE, MENU_FEWER,
    MENU_KEYS, MENU_ABOUT,
    CHOOSE_OPEN = 300, CHOOSE_SAVE,
};

/* what typing is going into: nothing, the active cell (typed in the grid: an arrow moves on), the formula bar */
enum { E_NONE, E_TYPING, E_FIELD };
/* what waits for a question's answer or a save: the change that asked */
enum { AFTER_NONE, AFTER_NEW, AFTER_OPEN, AFTER_CLOSE };

struct app {
    struct gk *ui;
    struct gk_widget *name, *formula, *grid, *status;
    struct sheet *s;
    struct view v;
    struct sheet_io io;
    char path[WIN_PATH_MAX];            /* the sheet's file; "" for one never saved */
    int editing, edit_row, edit_col;    /* E_*, and the cell it goes into */
    char edit[SH_INPUT_MAX + 2];        /* E_TYPING's text: a text's ' and SH_INPUT_MAX bytes */
    int edit_len;
    struct sh_clip *clip;               /* the last copy */
    unsigned clip_seen;                 /* the desktop's clipboard as the copy left it, when its text was too long */
    int clip_known;
    struct sh_reader *rd;               /* a file being read */
    int rd_h;
    long rd_size, rd_done;
    char rd_path[WIN_PATH_MAX];
    char save_path[WIN_PATH_MAX];       /* a Save as waiting for CSV's question */
    int after;                          /* AFTER_*: what a save or a question was for */
    unsigned lossy;                     /* SH_LOSE_*: what the last save, a CSV's, left out of the file */
    int all;                            /* the whole grid is to be painted again */
    char msg[APP_MSG];                  /* the status bar's message, until the next change */
    char title[WIN_TITLE_MAX];          /* the window's title as last set */
    struct {                            /* the selection's status as last worked out, and for what */
        const struct sheet *s;
        unsigned state;
        int row, col, erow, ecol;
        char text[SH_WHY];
    } said;
    int quit;                           /* the window is to close */
};

/* the window, and the file at path read into it (NULL: a new sheet): 0, or -1 with errno (ENODEV: no desktop) */
int app_open(struct app *a, const struct sheet_io *io, const char *path);
int app_event(struct app *a, const struct gk_event *ev);       /* 0 once the window is to close */
int app_reading(const struct app *a);                          /* a file is being read: main.c calls app_read */
void app_read(struct app *a);
void app_close(struct app *a);

#endif
