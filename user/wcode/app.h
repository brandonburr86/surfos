/*
SurfOS user program: wcode
--------------------
File: app.h     Date: 11/05/26
--------------------
The Code Editor as a whole (app.c): its tabs, one document a file, and what a key, a menu item, a
click or the wheel does to them; the window (main.c) draws and asks the desktop for what this
says it needs (a chooser, the clipboard, a question), then hands the answer back. Reading and
saving go through the callbacks in struct wc_app_io (fileio.c in the program), and find in
files through struct wc_fsops (grep.h), so tests/host/test_wcode.c drives all of it on the host.

The unsaved-changes guard is here, on every way out but one: closing a tab, quitting (Ctrl+Q, the
File menu, the window's close button) ask about each file with unsaved changes in turn, Save,
Don't save or Cancel, and a file never saved asks for its name first. The way out it cannot
guard is the desktop's end, which takes the window before anything can be asked: main.c then
puts each such file's text beside it (wc_file_rescue()).
*/
#ifndef _WCODE_APP_H
#define _WCODE_APP_H

#include "doc.h"
#include "find.h"
#include "grep.h"
#include "view.h"

#define WC_TABS 8                       /* files open at once */
#define WC_GOTO_MAX 32

/* the keys that are no bytes and the modifiers, with surfos/win.h's numbers (11/05/26, desk's ABI): main.c hands them
   in as the desktop sends them. Tab, Enter, Esc and Backspace are their bytes 9, 13, 27 and 127, and with Ctrl held
   WC_KEY_TAB .. WC_KEY_BACKSPACE; a Ctrl+letter is its control byte with WC_MOD_CTRL (Ctrl+H 8 is no Backspace) */
#define WC_KEY_UP        0x100
#define WC_KEY_DOWN      0x101
#define WC_KEY_LEFT      0x102
#define WC_KEY_RIGHT     0x103
#define WC_KEY_HOME      0x104
#define WC_KEY_END       0x105
#define WC_KEY_PGUP      0x106
#define WC_KEY_PGDN      0x107
#define WC_KEY_DELETE    0x108
#define WC_KEY_INSERT    0x109
#define WC_KEY_BACKTAB   0x10a
#define WC_KEY_TAB       0x10c
#define WC_KEY_ENTER     0x10d
#define WC_KEY_ESC       0x10e
#define WC_KEY_BACKSPACE 0x10f
#define WC_KEY_F(n)      (0x110 + (n))
#define WC_MOD_SHIFT     1
#define WC_MOD_ALT       2
#define WC_MOD_CTRL      4
#define WC_MOD_REPEAT    8

/* what keys and the menus ask for */
enum {
    WC_CMD_NONE,
    WC_CMD_NEW, WC_CMD_OPEN, WC_CMD_SAVE, WC_CMD_SAVE_AS, WC_CMD_CLOSE, WC_CMD_QUIT,
    WC_CMD_UNDO, WC_CMD_REDO, WC_CMD_CUT, WC_CMD_COPY, WC_CMD_PASTE, WC_CMD_DELETE, WC_CMD_SELECT_ALL,
    WC_CMD_FIND, WC_CMD_FIND_NEXT, WC_CMD_FIND_PREV, WC_CMD_REPLACE, WC_CMD_REPLACE_ONE, WC_CMD_REPLACE_ALL,
    WC_CMD_FIND_FILES, WC_CMD_GOTO, WC_CMD_BRACKET, WC_CMD_COMMENT, WC_CMD_INDENT, WC_CMD_OUTDENT,
    WC_CMD_NEXT_TAB, WC_CMD_PREV_TAB,
    WC_CMD_LANG,                        /* + a language: WC_CMD_LANG + WC_LANG_C ... */
    WC_CMD_TAB = WC_CMD_LANG + WC_LANGS,    /* + a tab's number: to it */
    WC_CMDS = WC_CMD_TAB + WC_TABS
};

/* what the window does after a call (bits) */
#define WC_DO_DRAW    0x001             /* draw again */
#define WC_DO_COPY    0x002             /* the text wc_app_clip() gives to the clipboard, then wc_app_copied() */
#define WC_DO_PASTE   0x004             /* the clipboard's text to wc_app_paste() */
#define WC_DO_OPEN    0x008             /* the Open chooser, its answer to wc_app_open() */
#define WC_DO_SAVE_AS 0x010             /* the Save as chooser for the file in front, its answer to wc_app_save_as() */
#define WC_DO_ASK     0x020             /* the unsaved-changes question about wc_app_asking(), to wc_app_answer() */
#define WC_DO_FIND    0x040             /* the find bar, with its box focused */
#define WC_DO_REPLACE 0x080             /* the find bar with the replacement's box */
#define WC_DO_GOTO    0x100             /* the go-to-line prompt, its answer to wc_app_goto() */
#define WC_DO_FILES   0x200             /* the find-in-files band */
#define WC_DO_QUIT    0x400             /* the program ends */
#define WC_DO_TITLE   0x800             /* the window's title is another */
#define WC_DO_IDLE    0x1000            /* more to do between events: call wc_app_idle() again soon */
#define WC_DO_DISK    0x2000            /* 11/09/26: the file changed on the disk since: wc_app_disk() asks what to do */

enum { WC_ANSWER_SAVE, WC_ANSWER_DISCARD, WC_ANSWER_CANCEL };
enum { WC_DISK_OVER, WC_DISK_SAVE_AS, WC_DISK_CANCEL };   /* save over it, save it elsewhere, neither */
enum { WC_JOB_NONE, WC_JOB_FIND, WC_JOB_ALL };      /* 11/09/26: a search going on between events */

struct wc_app_io {
    int (*read)(struct wc_doc *d, const char *path);    /* 0, or -1 with d->msg */
    int (*save)(struct wc_doc *d, const char *to);      /* 0, 1 (a copy beside), -1; d->msg */
};

struct wc_app {
    struct wc_pool pool;
    struct wc_doc *docs[WC_TABS];
    int n, cur;
    unsigned untitled;                  /* the next new file's number */
    const struct wc_app_io *io;
    const struct wc_fsops *fs;
    struct wc_find find;
    struct wc_grep grep;
    struct wc_view view;
    unsigned rows, cols;                /* the text area, in cells */
    char utf8[4];                       /* a character typed a byte at a time */
    unsigned nutf8;
    int closing;                        /* WC_CLOSE_TAB or WC_CLOSE_ALL while the guard asks */
    int asking;                         /* the tab it asks about, -1 for none */
    int after_save_as;                  /* the Save as chooser was for the guard: go on after it */
    int cut;                            /* the copy asked for is a cut */
    int discard[WC_TABS];               /* Don't save was the answer, for the files of a quit not yet ended */
    unsigned clicks, click_pos;         /* the last press, for double and triple clicks */
    char status[WC_MSG];                /* for the status bar, until the next key */
    int job;                            /* 11/09/26 (the review, C3): WC_JOB_*, in the file in front */
    unsigned long step;                 /* its work at a time (0: WC_FIND_STEP) */
    char cwd[WC_PATH];                  /* the program's folder, for a path not from / ("" is /) */
};

enum { WC_CLOSE_NONE, WC_CLOSE_TAB, WC_CLOSE_ALL };

int wc_app_init(struct wc_app *a, const struct wc_app_io *io, const struct wc_fsops *fs);   /* one new file */
void wc_app_free(struct wc_app *a);
struct wc_doc *wc_app_doc(struct wc_app *a);                /* the file in front */
void wc_app_size(struct wc_app *a, unsigned rows, unsigned cols);   /* the text area's cells */

int wc_app_cmd(struct wc_app *a, int cmd);
int wc_app_key(struct wc_app *a, unsigned key, unsigned mods);
/* the left button in the text area at a cell (row, col; col < 0: in the line numbers), clicks 1 to 3, extend with
   Shift; then the pointer moving while it is held (rows past the edges scroll) */
int wc_app_press(struct wc_app *a, int row, int col, unsigned clicks, int extend);
int wc_app_drag(struct wc_app *a, int row, int col);
int wc_app_wheel(struct wc_app *a, int rows);
int wc_app_idle(struct wc_app *a);                          /* between events: searches, find in files, colouring */
/* 11/09/26 (the review, C3): a person's key, click or choice while a search goes on (wc_app_key(), wc_app_cmd() and
   wc_app_press() call it too): a search stops, as does Replace all still counting, and Esc is then used up (esc);
   while Replace all changes the file the input waits (-1), and Esc takes back what it did, or says it cannot (a
   change too large to undo). Otherwise what to do (0 when no search was going on) */
int wc_app_interrupt(struct wc_app *a, int esc);
int wc_app_busy(const struct wc_app *a);                    /* Replace all changing the file: input waits */

/* answers */
int wc_app_open(struct wc_app *a, const char *path);         /* a chooser's file, or the program's argument */
int wc_app_new_file(struct wc_app *a, const char *path);     /* a file not there yet: made by its first save */
int wc_app_save_as(struct wc_app *a, const char *path);      /* NULL: cancelled */
int wc_app_answer(struct wc_app *a, int answer);
int wc_app_disk(struct wc_app *a, int answer);               /* 11/09/26: WC_DISK_*, the answer to WC_DO_DISK */
/* a path in its one form (wc_path_canon()), one not from / taken from a->cwd: 0, or -1 when it does not fit */
int wc_app_path(const struct wc_app *a, const char *path, char *out, unsigned size);
int wc_app_paste(struct wc_app *a, const char *text, unsigned n);
int wc_app_copied(struct wc_app *a, int ok);                 /* the clipboard took the text (1) or not */
int wc_app_goto(struct wc_app *a, const char *text);         /* "120" or "120:8" */
int wc_app_files(struct wc_app *a, const char *folder);      /* find in files from folder, with a->grep's choices */
int wc_app_hit(struct wc_app *a, unsigned i);                /* a result of find in files, opened at its match */

/* what the window shows */
unsigned wc_app_clip(struct wc_app *a, char *out, unsigned size);  /* the selection, for WC_DO_COPY */
int wc_app_asking(struct wc_app *a, char *question, unsigned size);    /* the tab asked about, -1 for none */
void wc_app_tab_label(struct wc_app *a, int i, char *out, unsigned size);
void wc_app_title(struct wc_app *a, char *out, unsigned size);
void wc_app_status(struct wc_app *a, char *out, unsigned size);
void wc_app_find_seed(struct wc_app *a);                     /* the selection, one line of it, as what Find takes */
int wc_app_unsafe(const struct wc_app *a);                   /* files with unsaved changes */

#endif
