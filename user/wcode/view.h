/*
SurfOS user program: wcode
--------------------
File: view.h    Date: 11/05/26
--------------------
What the Code Editor's text area shows of a file (view.c), in cells of the console's font: the
rows from the first line shown, the columns from the first column shown, each cell a character
with its class (its colour) and what is behind it (the selection, the bracket that matches the
one at the caret). Lines do not wrap; the view follows the caret down and sideways. A cell is
also where a click lands: a row and a column give a position in the text. main.c draws the cells
and the gutter's line numbers; nothing here draws.
*/
#ifndef _WCODE_VIEW_H
#define _WCODE_VIEW_H

#include "doc.h"

#define WC_ROW_CELLS 512                /* columns a row has, at most */

enum { WC_BG_NONE, WC_BG_SEL, WC_BG_BRACKET, WC_BG_NOMATCH };

struct wc_cell {
    unsigned cp;                        /* a code point (WC_BAD | b for a byte that is no UTF-8), 0 past the text */
    unsigned char cls, bg;
};

struct wc_view {
    unsigned long budget;               /* bytes the colouring may tokenize for a screenful */
    int exact;                          /* the last screenful's colours were all known */
    unsigned br_a, br_b;                /* the bracket at the caret and its match (WC_NONE: none) */
};

void wc_view_init(struct wc_view *v);
/* the first line and column shown, moved so that the caret is in rows x cols */
void wc_view_follow(struct wc_doc *d, unsigned rows, unsigned cols);
/* the cells of rows x cols from the first line and column shown; *lines: how many rows have a line */
void wc_view_render(struct wc_doc *d, struct wc_view *v, struct wc_cell *cells, unsigned rows, unsigned cols,
                    unsigned *lines);
unsigned wc_view_hit(struct wc_doc *d, unsigned row, unsigned col);     /* the position a click lands on */
int wc_view_caret(struct wc_doc *d, unsigned rows, unsigned cols, unsigned *row, unsigned *col);  /* 0 out of view */
void wc_view_scroll(struct wc_doc *d, int rows, int cols, unsigned page);   /* the wheel and the scroll bars */
unsigned wc_view_digits(const struct wc_doc *d);                       /* the line numbers' widest */
unsigned wc_view_width(struct wc_doc *d, unsigned rows);               /* the widest line shown, in cells */
/* the status bar's text: line, column, the selection, the language, the line ends, the indentation */
void wc_view_status(struct wc_doc *d, char *out, unsigned size);

#endif
