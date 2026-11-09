/*
SurfOS user program: wmines
--------------------
File: view.h    Date: 11/05/26
--------------------
Mines in its window, without the drawing (view.c): the field laid out in the board, the area of
the window between its toolbar and its status bar; what each cell looks like, the keyboard's
cursor, what a key or the pointer does, the status bar's line, the questions before closing or
starting anew while a game is under way, and the session's scores (the best time of each field,
the games played and won), which last while the program runs and are never written anywhere. window.c draws what this says and talks to the
desktop through the toolkit (gk.h); tests/host/test_wmines.c runs this on the host as it is.
*/
#ifndef _WMINES_VIEW_H
#define _WMINES_VIEW_H

#include "mines.h"

#define MV_CELL 24                      /* a cell's side in a new window, in the desktop's pixels */
#define MV_CELL_MIN 16                  /* and as small or large as a resized window makes it */
#define MV_CELL_MAX 40
#define MV_BAR_H 34                     /* the window's toolbar and status bar (gk.h's), and the room around the field */
#define MV_STATUS_H 22
#define MV_MARGIN 10
/* the smallest window (the review of 11/05/26): the toolbar at its natural width (New 64, the sizes' buttons 220, the
   counters 80 and 96, which hold "Mines -100" and "Time 1:02:03", 12 apart and 5 in from the edges, with the spacer's
   two gaps), which is wider than the large field's 28 cells of MV_CELL_MIN and its margins (468); as tall as those
   cells' 15 rows need under the toolbar and over the status bar. A box's lines of 50 characters fit in it (gk.h's
   boxes show (the window's width - 48) / 8). test_wmines_window.c opens the window there */
#define MV_MIN_W 518
#define MV_MIN_H (MV_BAR_H + 15 * MV_CELL_MIN + 2 * MV_MARGIN + MV_STATUS_H)
#define MV_STATUS 112

/* the keys that are no bytes (surfos/win.h's WIN_KEY_*) */
#define MV_UP 0x100
#define MV_DOWN 0x101
#define MV_LEFT 0x102
#define MV_RIGHT 0x103
#define MV_HOME 0x104
#define MV_END 0x105
#define MV_PGUP 0x106
#define MV_PGDN 0x107

#define MV_REPEAT 8                     /* a key held down, coming again (surfos/win.h's WIN_MOD_REPEAT) */

/* the pointer's buttons (surfos/win.h's WIN_BUTTON_*) */
#define MV_BUTTON_LEFT 1
#define MV_BUTTON_RIGHT 2
#define MV_BUTTON_MIDDLE 4

/* what a cell looks like (mview_look()): covered, pressed while the left button is held on it (or around a number
   being chorded), flagged, open with its number (MV_OPEN + 0 to 8), and at a loss the mines, the one that went
   off and the flags that were wrong */
#define MV_COVERED 0
#define MV_PRESSED 1
#define MV_FLAG 2
#define MV_OPEN 3
#define MV_MINE (MV_OPEN + 9)
#define MV_BLAST (MV_MINE + 1)
#define MV_WRONG (MV_MINE + 2)

struct mview {
    struct mines game;
    int size;                           /* the game's field: MINES_SMALL, MINES_MEDIUM or MINES_LARGE */
    unsigned rng;                       /* where each new game's seed comes from */
    int fx, fy, cell;                   /* the field's top left in the board, and a cell's side */
    int cx, cy, cursor;                 /* the keyboard's cursor, and whether it shows (the keyboard moved it) */
    unsigned buttons;                   /* the pointer's buttons as last seen */
    int held;                           /* a left press began on the field and is held */
    int px, py;                         /* the cell under it (-1: off the field) */
    int asking;                         /* the question main.c asks while a game is under way (MV_ASK_*), 0 none */
    int ask_size;                       /* the field a new game was asked for in */
    unsigned best[MINES_SIZES];         /* the session's: each field's best time in ms, once won[] says one was won */
    int played[MINES_SIZES], won[MINES_SIZES];
    char status[MV_STATUS];             /* what the last move said, until the next */
};

/* the questions, Cancel first */
#define MV_ASK_CLOSE 1                  /* closing the window */
#define MV_ASK_NEW 2                    /* a new game in its place (N, 1 to 3, New, the sizes, the Game menu) */

/* what a key or the pointer asks main.c to do besides drawing again */
#define MV_DRAW 1                       /* draw again */
#define MV_RESIZE 2                     /* another field: the window should take mview_window()'s size */
#define MV_CLOSE 3                      /* the window closes */

void mview_init(struct mview *v, unsigned seed);
void mview_new(struct mview *v, int size);              /* a new game: of that field, the seed from v->rng */
/* a new game of that field asked for: at once when none is under way (MV_DRAW, or MV_RESIZE for another field);
   with one under way, asking set (MV_DRAW: main.c asks); nothing while a question is up (0) */
int mview_request(struct mview *v, int size);
/* the window's content size for a field, as large as MV_CELL cells make it within max_w x max_h (0: no limit) */
void mview_window(int size, int max_w, int max_h, int *w, int *h);
/* the field laid out in the board of w x h, in the middle: 1 when that changed anything */
int mview_layout(struct mview *v, int w, int h);
int mview_cell_at(const struct mview *v, int x, int y, int *cx, int *cy);
int mview_look(const struct mview *v, int x, int y);
/* a key and its modifiers (MV_REPEAT: held down, which moves the cursor and does nothing else): 0, or MV_DRAW,
   MV_RESIZE, MV_CLOSE */
int mview_key(struct mview *v, int key, unsigned mods, unsigned now);
/* the pointer at x, y in the board with these buttons held, pressed of them pressed now (the toolkit says which:
   a press made elsewhere is never taken for one here); a button let go is one held at the last call and not now */
int mview_pointer(struct mview *v, int x, int y, unsigned buttons, unsigned pressed, unsigned now);
/* the board lost the pointer (the window lost the focus, or a question came over it): a press held is forgotten, and
   reveals nothing: MV_DRAW when one was */
int mview_cancel(struct mview *v);
int mview_close(struct mview *v);                       /* MV_CLOSE, or MV_DRAW with asking set */
/* the question answered, yes (Close, New field) or not: MV_CLOSE, MV_RESIZE or MV_DRAW (0: there was none) */
int mview_answer(struct mview *v, int yes);
void mview_status(const struct mview *v, char *out, int n);
void mview_record(const struct mview *v, char *out, int n);     /* the session's for the field: "Won 1 of 3, best 0:41" */
void mview_time(unsigned ms, char *out, int n);         /* 0:42, from an hour on 1:02:03 */

#endif
