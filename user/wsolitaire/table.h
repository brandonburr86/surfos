/*
SurfOS user program: wsolitaire
--------------------
File: table.h   Date: 11/05/26
--------------------
Solitaire in its window, without the drawing (table.c): the table laid out in the board, the area
of the window between its toolbar and its status bar (the stock, the waste and the four
foundations in the top row, the seven tableau piles below, the cards as large as the room allows),
where each card lies, the card or pile under the pointer, a press that becomes a drag or a click,
the keyboard's cursor, the game finishing by itself a card a tick, the status bar, closing while a
game is under way, and the session's scores (games played and won, the best time and the fewest
moves, for each way of drawing), which last while the program runs and are written nowhere.
window.c draws what this says; tests/host/test_wsolitaire.c runs this on the host as it is.
*/
#ifndef _WSOLITAIRE_TABLE_H
#define _WSOLITAIRE_TABLE_H

#include "klondike.h"

#define TB_BAR_H 34                     /* the window's toolbar above the board, its status bar below it (gk.h's) */
#define TB_STATUS_H 22
#define TB_MARGIN 10
#define TB_GAP 8                        /* between the columns */
#define TB_ROW_GAP 14                   /* between the top row and the tableau */
#define TB_CARD_W 72                    /* a card's width in a large window (its height is a third more) */
#define TB_CARD_MIN 40
#define TB_DOWN 8                       /* the edge of a face-down card showing under the next, at most */
#define TB_UP 22                        /* and of a face-up card: its rank and suit */
#define TB_DOWN_MIN 3                   /* as little as a tall pile in a short window leaves */
#define TB_UP_MIN 4
#define TB_FAN 16                       /* drawing three: the waste's top cards side by side */
#define TB_DRAG 4                       /* the pixels a press moves before it is a drag */
#define TB_FINISH_MS 60                 /* finishing by itself: a card each */
#define TB_W 600                        /* a new window's board: the table with 72-pixel cards */
#define TB_H 380
#define TB_STATUS 128

/* the keys that are no bytes (surfos/win.h's WIN_KEY_*), a key held down (WIN_MOD_REPEAT), the pointer's left button */
#define TB_UPKEY 0x100
#define TB_DOWNKEY 0x101
#define TB_LEFT 0x102
#define TB_RIGHT 0x103
#define TB_REPEAT 8
#define TB_BUTTON_LEFT 1

struct table {
    struct klondike game;
    int draw;                           /* the next game's way of drawing: 1 or 3 */
    unsigned rng;                       /* where each new game's seed comes from */
    int w, h;                           /* the board's size */
    int cw, ch;                         /* a card */
    int col[7];                         /* the columns' left edges */
    int top_y, tab_y, bottom;           /* the top row's top, the tableau's, and how low a tableau card may reach */
    unsigned buttons;                   /* the pointer's buttons as last seen */
    int press_pile, press_index;        /* a left press on cards that can be taken, or on the stock (-1: none) */
    int press_x, press_y;               /* where it was, and where in the card it took hold */
    int grab_x, grab_y;
    int dragging, drag_x, drag_y;       /* it moved: the cards follow the pointer, which is at drag_x, drag_y */
    int cur_pile, cur_index, cursor;    /* the keyboard's card (index -1: an empty pile), shown from the first key */
    int finishing;                      /* the game plays itself out (table_tick()) */
    unsigned last_step;                 /* when it last did */
    int asking;                         /* closing asked about while a game is under way: main.c's question */
    int played[2], won[2];              /* the session's, drawing one [0] and three [1] */
    unsigned best_ms[2];                /* 0: none won yet */
    int best_moves[2];
    char status[TB_STATUS];
};

/* what a key or the pointer asks main.c to do besides drawing again */
#define TB_DRAW 1
#define TB_CLOSE 3

void table_init(struct table *t, unsigned seed);
void table_new(struct table *t, int draw);              /* a new deal, drawing 1 or 3 */
void table_layout(struct table *t, int w, int h);
/* where card index of pile lies (for an empty pile, index 0: its place), and how far a tableau pile fans */
void table_card_xy(const struct table *t, int pile, int index, int *x, int *y);
void table_fan(const struct table *t, int pile, int *down, int *up);
/* the card under the pointer: its pile and index (-1: the place of an empty pile, or under the stock): 0 for none */
int table_hit(const struct table *t, int x, int y, int *pile, int *index);
int table_drop(const struct table *t, int x, int y);    /* the pile a drag lets go over, or -1 */
/* a key and its modifiers (TB_REPEAT: held down, which moves the keyboard's card and does nothing else): 0, TB_DRAW or
   TB_CLOSE */
int table_key(struct table *t, int key, unsigned mods, unsigned now);
/* the pointer at x, y in the board, these buttons held, pressed of them pressed now (the toolkit says which) */
int table_pointer(struct table *t, int x, int y, unsigned buttons, unsigned pressed, unsigned now);
/* the board lost the pointer (the window lost the focus, or a question came over it): a press or a drag is forgotten
   and the cards stay where they were: TB_DRAW when there was one */
int table_cancel(struct table *t);
int table_tick(struct table *t, unsigned now);          /* finishing: the next card when its time came; TB_DRAW */
int table_undo(struct table *t, unsigned now);
int table_close(struct table *t);
int table_answer(struct table *t, int close);
/* the cards a drag carries: from index of pile up (n: how many); 0 when nothing is dragged */
int table_dragged(const struct table *t, int *pile, int *index, int *n);
void table_status(const struct table *t, char *out, int n);    /* what the last move said, or how to play */
void table_record(const struct table *t, char *out, int n);    /* the session's: "Won 1 of 3, best 3:21" */
void table_time(unsigned ms, char *out, int n);         /* 3:21, from an hour on 1:02:03 */
const char *table_card_name(unsigned char card, char *out, int n);    /* "the queen of hearts" */

#endif
