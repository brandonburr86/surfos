/*
SurfOS user program: wswell
--------------------
File: view.h    Date: 11/05/26
--------------------
Swell in its window, without the drawing (view.c): the sea and the panel beside it (the next
piece, the score, the rows, the level, the time and the session's best) laid out in the board, the
area of the window above its status bar; what each cell of the sea shows (the falling piece,
where it would land, the rows just washed away), what a key does, the pause when the window loses
the focus, closing while a game is under way, the status bar, and the session's scores, which last
while the program runs and are written nowhere. window.c draws what this says;
tests/host/test_wswell.c runs this on the host as it is.
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

struct sview {
    struct swell game;
    unsigned rng;                       /* where each new game's seed comes from */
    int cell, sx, sy;                   /* a cell's side, and the sea's top left in the board */
    int px, py;                         /* the panel's top left */
    int asking;                         /* closing asked about while a game is under way (it is paused meanwhile) */
    unsigned flash_at;                  /* when rows were last washed away */
    int games;                          /* the session's: games ended, the best score and its rows */
    unsigned best;
    int best_rows;
    char status[SV_STATUS];
};

/* what a key asks main.c to do besides drawing again */
#define SV_DRAW 1
#define SV_CLOSE 3

void sview_init(struct sview *v, unsigned seed);
void sview_new(struct sview *v);                        /* a new game, waiting for a key */
void sview_restart(struct sview *v, unsigned now);      /* a new game at once (the Game menu's: a person chose it) */
void sview_window(int max_w, int max_h, int *w, int *h);    /* a new window's content: SV_CELL cells, less to fit */
int sview_layout(struct sview *v, int w, int h);        /* in a board of w x h: 1 when that changed anything */
int sview_cell(const struct sview *v, int x, int y);
int sview_flashing(const struct sview *v, int y, unsigned now);     /* row y was just washed away */
/* a key and its modifiers (SV_REPEAT: held down, which only Left, Right and Down act on): 0, SV_DRAW or SV_CLOSE */
int sview_key(struct sview *v, int key, unsigned mods, unsigned now);
int sview_tick(struct sview *v, unsigned now);          /* what time does: SV_DRAW when anything changed */
unsigned sview_wait(const struct sview *v, unsigned now);   /* milliseconds until a tick has something to do */
int sview_focus(struct sview *v, int focused, unsigned now);    /* losing the focus pauses a game: SV_DRAW */
int sview_close(struct sview *v, unsigned now);         /* SV_CLOSE, or SV_DRAW with asking set (and paused) */
int sview_answer(struct sview *v, int close);
void sview_status(const struct sview *v, char *out, int n);
void sview_time(unsigned ms, char *out, int n);         /* 3:21, from an hour on 1:02:03 */

#endif
