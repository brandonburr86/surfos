/*
SurfOS user program: wswell
--------------------
File: view.h    Date: 11/05/26
--------------------
Swell in its window, without the drawing (view.c): the sea and the panel beside it (the next
piece, the score, the rows, the level, the time and the session's best) laid out in the board, the
area of the window above its status bar; what each cell of the sea shows (the falling piece,
where it would land, the rows just washed away), what a key does, the pause when the window loses
the focus, the questions before closing or starting anew while a game is under way, the status
bar, and the session's scores, which last while the program runs and are written nowhere.
window.c draws what this says; tests/host/test_wswell.c runs this on the host as it is.
*/
#ifndef _WSWELL_VIEW_H
#define _WSWELL_VIEW_H

#include "swell.h"

#define SV_CELL 22                      /* a cell's side in a new window */
#define SV_CELL_MIN 12                  /* and as small or large as the window makes it */
#define SV_CELL_MAX 32
#define SV_MARGIN 10
#define SV_PANEL 176                    /* the panel right of the sea */
#define SV_STATUS_H 22
#define SV_PREVIEW 18                   /* the next piece's cells in the panel: the sea's, at most this */
#define SV_LINES 5                      /* the panel's numbers under it: the score, the rows, the level, the time, the best */
#define SV_LINE_H 36                    /* each a name and its value under it (16 each), and room */
/* the smallest window (the review of 11/05/26): as wide as the toolkit's boxes need for lines of 50 characters (a
   box is 16 narrower than the window and shows (its width - 32) / 8), and with a board 290 high, where the sea's cells
   are 15 and the panel's next piece and five lines beside it end above the board's foot (216 + 4 times the preview's
   cell under the sea's top); test_wswell.c tries every size from these up */
#define SV_MIN_W 448
#define SV_MIN_H 312
#define SV_FLASH_MS 200                 /* rows washed away show this long */
#define SV_STATUS 112

/* the keys that are no bytes (surfos/win.h's WIN_KEY_*), and a key held down (WIN_MOD_REPEAT) */
#define SV_UP 0x100
#define SV_DOWN 0x101
#define SV_LEFT 0x102
#define SV_RIGHT 0x103
#define SV_REPEAT 8

/* what a cell shows (sview_cell()): water, a piece's cell (kind + 1, the falling piece's too), sand, and where the
   falling piece would come to rest */
#define SV_WATER 0
#define SV_GHOST (SW_SAND + 1)

/* the questions main.c asks while a game is under way (it is paused meanwhile), Cancel first */
#define SV_ASK_CLOSE 1                  /* closing the window */
#define SV_ASK_NEW 2                    /* a new game in its place (N, the Game menu's New game) */

struct sview {
    struct swell game;
    unsigned rng;                       /* where each new game's seed comes from */
    int cell, sx, sy;                   /* a cell's side, and the sea's top left in the board */
    int px, py;                         /* the panel's top left */
    int asking;                         /* the question asked (SV_ASK_*), 0 for none */
    unsigned flash_at;                  /* when rows were last washed away */
    int games;                          /* the session's: games ended (over the top, or given up for a new one), */
    unsigned best;                      /* the best score among them, and its rows */
    int best_rows;
    char status[SV_STATUS];
};

/* what a key asks main.c to do besides drawing again */
#define SV_DRAW 1
#define SV_CLOSE 3

void sview_init(struct sview *v, unsigned seed);
void sview_new(struct sview *v);                        /* a new game, waiting for a key */
/* a new game asked for (N, the Game menu's New game): it starts at once when none is under way; one under way is
   paused and asking set (main.c asks); nothing while a question is up: 0, or SV_DRAW */
int sview_request(struct sview *v, unsigned now);
/* a new window's content: SV_CELL cells, less to fit within max_w x max_h (0: no limit), never under SV_MIN_W x SV_MIN_H */
void sview_window(int max_w, int max_h, int *w, int *h);
int sview_layout(struct sview *v, int w, int h);        /* in a board of w x h: 1 when that changed anything */
/* the panel as laid out: the next piece's box under the word Next (its top left and side, and its cells' side), and
   the top of number line i (its name; the value 16 under it) */
void sview_preview(const struct sview *v, int *x, int *y, int *size, int *cell);
int sview_line_y(const struct sview *v, int i);
int sview_cell(const struct sview *v, int x, int y);
int sview_flashing(const struct sview *v, int y, unsigned now);     /* row y was just washed away */
/* a key and its modifiers (SV_REPEAT: held down, which only Left, Right and Down act on): 0, SV_DRAW or SV_CLOSE */
int sview_key(struct sview *v, int key, unsigned mods, unsigned now);
int sview_tick(struct sview *v, unsigned now);          /* what time does: SV_DRAW when anything changed */
unsigned sview_wait(const struct sview *v, unsigned now);   /* milliseconds until a tick has something to do */
int sview_focus(struct sview *v, int focused, unsigned now);    /* losing the focus pauses a game: SV_DRAW */
int sview_close(struct sview *v, unsigned now);         /* SV_CLOSE, or SV_DRAW with asking set (and paused) */
/* the question answered, yes (Close, New game) or not: SV_CLOSE, or SV_DRAW (a new game, or the game paused) */
int sview_answer(struct sview *v, int yes, unsigned now);
void sview_status(const struct sview *v, char *out, int n);
void sview_time(unsigned ms, char *out, int n);         /* 3:21, from an hour on 1:02:03 */

#endif
