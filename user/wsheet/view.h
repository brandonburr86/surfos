/*
SurfOS user program: wsheet
--------------------
File: view.h    Date: 11/05/26
--------------------
The grid as the window shows it, without drawing it: which rows and columns it shows from where,
the active cell and the selection, what the keys, the pointer and the wheel do to them, a column's
width dragged at its header, and the status line. main.c draws what this says and turns the
desktop's events into these calls; tests/host/test_wsheet.c runs it on the host.

The selection is the rectangle between the active cell, which typing goes into, and its other
corner, which Shift with an arrow or a drag moves (as in every spreadsheet: the active cell stays
where the selection began). A header selects whole rows or columns; the corner where the headers
meet selects everything.
*/
#ifndef _WSHEET_VIEW_H
#define _WSHEET_VIEW_H

#include "sheet.h"

#define V_ROW_H 20                      /* a row: a line of the console's font and its margins */
#define V_HEAD_H 20                     /* the column headers */
#define V_ROWHEAD_W 48                  /* the row numbers: 65536 and margins */
#define V_PAD 3                         /* a cell's text from its edges */
#define V_BORDER 3                      /* how near a column header's edge a press takes the edge */

/* keys: surfos/win.h's WIN_KEY_* for those that are no bytes; and modifiers */
#define V_UP 0x100
#define V_DOWN 0x101
#define V_LEFT 0x102
#define V_RIGHT 0x103
#define V_HOME 0x104
#define V_END 0x105
#define V_PGUP 0x106
#define V_PGDN 0x107
#define VM_SHIFT 1
#define VM_CTRL 2

enum { VD_NONE, VD_CELLS, VD_ROWS, VD_COLS, VD_WIDTH };        /* what a held button is doing */
enum { VA_NONE, VA_CELL, VA_COLHEAD, VA_ROWHEAD, VA_CORNER, VA_EDGE };  /* what is at a point */

struct view_at {
    int what, row, col;                 /* VA_EDGE: col is the column whose right edge it is */
};

struct view {
    struct sheet *s;
    int x, y, w, h;                     /* the grid's area in the window: its headers and cells */
    int top, left;                      /* the first row and column it shows */
    int row, col;                       /* the active cell */
    int erow, ecol;                     /* the selection's other corner */
    int tab_col;                        /* the column Tab moved on from, for Enter to come back to; -1 */
    int drag;                           /* VD_* */
    int drag_col, drag_x, drag_w;       /* VD_WIDTH: the column, where the press was, its width then */
    int width_now;                      /* VD_WIDTH: the width it shows while the edge is dragged */
};

void view_init(struct view *v, struct sheet *s);
void view_place(struct view *v, int x, int y, int w, int h);
int view_width(const struct view *v, int col);          /* a column's width as shown, a dragged one's too */
int view_rows_shown(const struct view *v);              /* whole rows that fit, at least 1 */
int view_cols_shown(const struct view *v);              /* whole columns that fit, at least 1 */
int view_col_x(const struct view *v, int col);          /* a shown column's left edge in the window, or -1 */
int view_row_y(const struct view *v, int row);          /* a shown row's top, or -1 */
int view_chars(const struct view *v, int col);          /* characters of 8 pixels a cell of it shows */
void view_at(const struct view *v, int x, int y, struct view_at *a);
void view_selection(const struct view *v, struct sh_range *r);
int view_selected(const struct view *v, int row, int col);
/* the x where a text in row, col must stop: its cell's right edge, or past empty cells to its right, to the grid's */
int view_text_end(const struct view *v, int row, int col);

/* moving: each returns 1 when what shows changed (draw again), 0 when nothing did */
int view_go(struct view *v, int row, int col, int extend);     /* the active cell there, or with extend the corner */
void view_select(struct view *v, struct sh_range r);    /* r selected, its top left active */
void view_select_all(struct view *v);
int view_scroll(struct view *v, int rows, int cols);
int view_show(struct view *v, int row, int col);        /* scrolled so that the cell shows */
int view_key(struct view *v, int key, int mods);        /* the arrows, Page Up and Down, Home, End: 0 for others */
int view_enter(struct view *v, int back);               /* Enter's move on: down, or up; back to Tab's column */
int view_tab(struct view *v, int back);                 /* Tab's: right, or left */
int view_press(struct view *v, int x, int y, int mods); /* the left button pressed */
int view_move(struct view *v, int x, int y);            /* the pointer moved while it is held */
int view_release(struct view *v);                       /* let go: a dragged edge's width is set (one step) */
int view_wheel(struct view *v, int rows, int mods);     /* rows, + down; with Shift, columns */
void view_status(const struct view *v, char *out, int size);   /* the cell's trouble, or the selection's numbers */

#endif
