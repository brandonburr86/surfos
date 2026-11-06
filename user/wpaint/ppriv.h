/*
SurfOS user program: wpaint
--------------------
File: ppriv.h   Date: 11/05/26
--------------------
What the model's files (canvas.c, history.c, draw.c, tools.c, view.c) share and main.c does not
need: the counted memory, the two calls that change the canvas's pixels, the history's steps, and
the shapes, the flood fill and the text as rows of pixels.
*/
#ifndef _WPAINT_PPRIV_H
#define _WPAINT_PPRIV_H

#include "paint.h"

#define PAINT_DAB_MAX 32                /* the largest brush (paint_sizes[]) fits in this */

/* canvas.c: memory counted against p->mem_max. pm_room() makes room for n bytes more by giving up the history's
   oldest steps, as many as that needs, and only when that is enough: 1, or 0 with every step kept. pm_alloc() is
   pm_room() and the block (NULL: no room); a change of several blocks asks pm_room() for all of them first. Pixels
   shared by the clipboard and a floating selection */
int pm_room(struct paint *p, size_t n);
void *pm_alloc(struct paint *p, size_t n);
void pm_free(struct paint *p, void *q, size_t n);
struct pbuf *pbuf_new(struct paint *p, int w, int h);
void pbuf_put(struct paint *p, struct pbuf *b);
/* the only writes to the canvas: a run of one colour, and a row of pixels; both clipped, both saving the tiles
   they touch into the change under way first */
void fill_span(struct paint *p, int y, int x0, int x1, unsigned c);
void copy_span(struct paint *p, int y, int x, const unsigned *src, int n);
void mark(struct paint *p, int x0, int y0, int x1, int y1);    /* the window draws these canvas pixels again */
void mark_view(struct paint *p);                                /* and all of the view */
void say(struct paint *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
size_t canvas_bytes(int w, int h);
int canvas_size_ok(struct paint *p, int w, int h);              /* 1, or 0 with p->msg saying why */
/* a canvas of w x h made elsewhere (history.c's undo of a new size): px and its rows, both from pm_alloc() */
void canvas_install(struct paint *p, unsigned *px, int *rows, int w, int h);
int *rows_new(struct paint *p, int h);
void rows_free(struct paint *p, int *rows, int h);
void selection_fit(struct paint *p);                            /* the selection after the canvas's size changed */

/* history.c */
void tile_rect(int w, int h, int t, int *x, int *y, int *tw, int *th);
int tiles_across(int w);
int step_begin(struct paint *p, const char *what);              /* the change under way, for the tiles it touches */
int step_canvas(struct paint *p, const char *what);             /* one that holds the whole canvas (a new size) */
void step_end(struct paint *p);                                 /* kept, unless nothing changed */
void step_abort(struct paint *p);                               /* the canvas as it was, and the step gone */
void touch(struct paint *p, int y, int x0, int x1);
void history_clear(struct paint *p);
int history_trim(struct paint *p);      /* a step gone (a redo first while a change is under way, else the oldest undo):
                                           1, or 0 when none is left */
size_t history_kept(const struct paint *p);                     /* the bytes of the steps kept */

/* tools.c: Undo's part while a change is under way: typed text dropped, a floating selection put back or a paste
   dropped. 1 when it did that */
int work_undo(struct paint *p);

/* draw.c */
int ellipse_span(int w, int h, int r, int *a, int *b);          /* row r of the ellipse in a box of w x h */
void dab_row(int size, int round, int r, int *l, int *rr);      /* row r of a dab, from its middle */
int line_rows(struct paint *p, int x0, int y0, int x1, int y1, int size, int round, int *top, int *bottom);
void stroke(struct paint *p, int x0, int y0, int x1, int y1, int size, int round, unsigned c);
void shape_set(struct paint *p, int tool, int x0, int y0, int x1, int y1, int constrain);
int shape_row(const struct paint *p, const struct pshape *s, int y, int x[4]);
void shape_bounds(const struct pshape *s, int *x0, int *y0, int *x1, int *y1);
void shape_draw(struct paint *p, const struct pshape *s);
int flood_fill(struct paint *p, int x, int y, unsigned c);      /* pixels filled, or -1 without the memory */
void text_size(const struct ptext *t, int scale, int *w, int *h);
void text_draw(struct paint *p, const struct ptext *t, int scale, unsigned c);
int text_lines(const struct ptext *t, int *start, int *len, int max);   /* each line's cells */

#endif
