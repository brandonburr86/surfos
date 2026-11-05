/*
SurfOS user program: wpaint
--------------------
File: canvas.c  Date: 11/05/26
--------------------
The canvas and the memory Paint keeps. Every large block is counted against p->mem_max (paint.h
says why that bound); when one would pass it, the history gives up its oldest steps first, and
only then is what asked refused. The canvas's pixels change only through fill_span() and
copy_span(), which first save the tiles they touch into the change under way (history.c's
touch()), and then mark the pixels the window must draw again. A new canvas and a picture opened
in its place start the history again (they are not undone, as a file opened in Notepad is not); a
new size is a step of the history, which keeps the whole canvas as it was.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "paint.h"
#include "ppriv.h"

/**** memory ****/

void *pm_alloc(struct paint *p, size_t n) {
    void *q;
    if(!n) n = 1;
    while(p->mem > p->mem_max || n > p->mem_max - p->mem)
        if(!history_trim(p)) return NULL;
    q = malloc(n);
    while(!q && history_trim(p)) q = malloc(n);    /* the machine has less than the bound: the history goes first */
    if(!q) return NULL;
    p->mem += n;
    return q;
}

void pm_free(struct paint *p, void *q, size_t n) {
    if(!q) return;
    if(!n) n = 1;
    free(q);
    p->mem -= n;
}

size_t canvas_bytes(int w, int h) {
    return (size_t)w * (size_t)h * sizeof(unsigned);    /* at most 64,000,000: sizes are checked first */
}

int *rows_new(struct paint *p, int h) {
    return (int *)pm_alloc(p, (size_t)h * 2 * sizeof(int));
}

void rows_free(struct paint *p, int *rows, int h) {
    pm_free(p, rows, (size_t)h * 2 * sizeof(int));
}

struct pbuf *pbuf_new(struct paint *p, int w, int h) {
    struct pbuf *b;
    if(w < 1 || h < 1 || w > PAINT_SIDE_MAX || h > PAINT_SIDE_MAX || (unsigned)w * (unsigned)h > PAINT_PIXELS_MAX)
        return NULL;
    b = (struct pbuf *)pm_alloc(p, sizeof(*b));
    if(!b) return NULL;
    b->px = (unsigned *)pm_alloc(p, canvas_bytes(w, h));
    if(!b->px) {
        pm_free(p, b, sizeof(*b));
        return NULL;
    }
    b->refs = 1;
    b->w = w;
    b->h = h;
    return b;
}

void pbuf_put(struct paint *p, struct pbuf *b) {
    if(!b || --b->refs > 0) return;
    pm_free(p, b->px, canvas_bytes(b->w, b->h));
    pm_free(p, b, sizeof(*b));
}

/**** what the window draws again, and what the status line says ****/

void mark(struct paint *p, int x0, int y0, int x1, int y1) {
    if(x0 > x1 || y0 > y1) return;
    if(!p->dirty) {
        p->dirty = 1;
        p->dx0 = x0;
        p->dy0 = y0;
        p->dx1 = x1;
        p->dy1 = y1;
        return;
    }
    if(x0 < p->dx0) p->dx0 = x0;
    if(y0 < p->dy0) p->dy0 = y0;
    if(x1 > p->dx1) p->dx1 = x1;
    if(y1 > p->dy1) p->dy1 = y1;
}

void mark_view(struct paint *p) {
    p->view_dirty = 1;
}

void say(struct paint *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->msg, sizeof(p->msg), fmt, ap);
    va_end(ap);
}

/**** the canvas's pixels ****/

void fill_span(struct paint *p, int y, int x0, int x1, unsigned c) {
    unsigned *row;
    int x;
    if(y < 0 || y >= p->h) return;
    if(x0 < 0) x0 = 0;
    if(x1 > p->w - 1) x1 = p->w - 1;
    if(x0 > x1) return;
    touch(p, y, x0, x1);
    row = p->px + (size_t)y * (size_t)p->w;
    for(x = x0; x <= x1; x++) row[x] = c;
    mark(p, x0, y, x1, y);
}

void copy_span(struct paint *p, int y, int x, const unsigned *src, int n) {
    if(y < 0 || y >= p->h || n <= 0) return;
    if(x < 0) {                                         /* the part left of the canvas is not drawn */
        if(n <= -x) return;
        src += -x;
        n -= -x;
        x = 0;
    }
    if(x >= p->w) return;
    if(n > p->w - x) n = p->w - x;
    touch(p, y, x, x + n - 1);
    memcpy(p->px + (size_t)y * (size_t)p->w + x, src, (size_t)n * sizeof(unsigned));
    mark(p, x, y, x + n - 1, y);
}

/**** whole canvases ****/

/* what the codecs read and write: a canvas Paint makes can always be saved */
int canvas_size_ok(struct paint *p, int w, int h) {
    if(w < 1 || h < 1 || w > PAINT_SIDE_MAX || h > PAINT_SIDE_MAX) {
        say(p, "%d x %d: a canvas is 1 to %d pixels each way", w, h, PAINT_SIDE_MAX);
        return 0;
    }
    if((unsigned)w * (unsigned)h > PAINT_PIXELS_MAX) {
        say(p, "%d x %d: a canvas has at most 16,000,000 pixels (4000 x 4000)", w, h);
        return 0;
    }
    return 1;
}

void selection_fit(struct paint *p) {
    struct psel *s = &p->sel;
    if(!s->on || s->buf) return;                        /* what floats may reach past the canvas */
    if(s->x < 0) { s->w += s->x; s->x = 0; }
    if(s->y < 0) { s->h += s->y; s->y = 0; }
    if(s->x + s->w > p->w) s->w = p->w - s->x;
    if(s->y + s->h > p->h) s->h = p->h - s->y;
    if(s->w < 1 || s->h < 1) s->on = 0;
}

void canvas_install(struct paint *p, unsigned *px, int *rows, int w, int h) {
    if(p->px) pm_free(p, p->px, canvas_bytes(p->w, p->h));
    if(p->rowl) rows_free(p, p->rowl, p->h);
    p->px = px;
    p->rowl = rows;
    p->rowr = rows + h;
    p->w = w;
    p->h = h;
    p->tiles_x = tiles_across(w);
    p->tiles_y = tiles_across(h);
    selection_fit(p);
    paint_scroll_by(p, 0, 0);                           /* the scroll, kept within the new size */
    mark_view(p);
}

/* what is under way goes, unfinished: the change, what floats, the text, the shape, the drag */
static void drop_work(struct paint *p) {
    if(p->open) step_abort(p);
    pbuf_put(p, p->sel.buf);
    memset(&p->sel, 0, sizeof(p->sel));
    p->text.on = 0;
    p->shape.on = 0;
    p->drag = 0;
    p->moving = 0;
}

/* a canvas of w x h would fit were the history gone (the old canvas is still there while it is made) */
static int would_fit(const struct paint *p, size_t n) {
    size_t kept = history_kept(p), held = p->mem > kept ? p->mem - kept : 0;
    return held <= p->mem_max && n <= p->mem_max - held;
}

/* the history starts again from this canvas: the state it is in is the one saved */
static void fresh(struct paint *p) {
    p->state = p->clean = ++p->serial;
    p->sx = p->sy = 0;
    paint_scroll_by(p, 0, 0);
    mark_view(p);
}

int paint_new(struct paint *p, int w, int h) {
    unsigned *px;
    int *rows = NULL;
    size_t i, n;
    if(!canvas_size_ok(p, w, h)) return -1;
    n = canvas_bytes(w, h);
    if(!would_fit(p, n + (size_t)h * 2 * sizeof(int))) {
        say(p, "Not enough memory for a canvas of %d x %d", w, h);
        return -1;
    }
    px = (unsigned *)pm_alloc(p, n);
    if(px) rows = rows_new(p, h);
    if(!rows) {
        pm_free(p, px, n);
        say(p, "Not enough memory for a canvas of %d x %d", w, h);
        return -1;
    }
    for(i = 0; i < (size_t)w * (size_t)h; i++) px[i] = PAINT_PAPER;
    drop_work(p);
    history_clear(p);
    canvas_install(p, px, rows, w, h);
    fresh(p);
    p->msg[0] = 0;
    return 0;
}

int paint_adopt(struct paint *p, unsigned *px, int w, int h) {
    int *rows;
    size_t n;
    if(!px || !canvas_size_ok(p, w, h)) return -1;
    n = canvas_bytes(w, h);
    if(!would_fit(p, n + (size_t)h * 2 * sizeof(int))) {
        say(p, "Not enough memory for a picture of %d x %d", w, h);
        return -1;
    }
    while((p->mem > p->mem_max || n > p->mem_max - p->mem) && history_trim(p))   /* would_fit() said it ends in time */
        ;
    p->mem += n;                                        /* the decoder's block, counted from now on */
    rows = rows_new(p, h);
    if(!rows) {
        p->mem -= n;
        say(p, "Not enough memory for a picture of %d x %d", w, h);
        return -1;
    }
    drop_work(p);
    history_clear(p);
    canvas_install(p, px, rows, w, h);
    fresh(p);
    p->msg[0] = 0;
    return 0;
}

int paint_resize(struct paint *p, int w, int h) {
    unsigned *px, *row;
    const unsigned *old;
    int *rows = NULL, x, y, keep;
    size_t n;
    if(!canvas_size_ok(p, w, h)) return -1;
    if(w == p->w && h == p->h) return 0;
    paint_put_down(p);
    n = canvas_bytes(w, h);
    px = (unsigned *)pm_alloc(p, n);
    if(px) rows = rows_new(p, h);
    if(!rows) {
        pm_free(p, px, n);
        say(p, "Not enough memory for a canvas of %d x %d", w, h);
        return -1;
    }
    step_canvas(p, "Size");                             /* the whole canvas as it is, to undo it with */
    keep = w < p->w ? w : p->w;
    for(y = 0; y < h; y++) {                            /* from the top left; the new part is the second colour */
        row = px + (size_t)y * (size_t)w;
        x = 0;
        if(y < p->h) {
            old = p->px + (size_t)y * (size_t)p->w;
            memcpy(row, old, (size_t)keep * sizeof(unsigned));
            x = keep;
        }
        for(; x < w; x++) row[x] = p->bg;
    }
    canvas_install(p, px, rows, w, h);
    step_end(p);
    return 0;
}

int paint_clear(struct paint *p) {
    int y;
    paint_put_down(p);
    step_begin(p, "Clear");
    for(y = 0; y < p->h; y++) fill_span(p, y, 0, p->w - 1, p->bg);
    step_end(p);
    return 1;
}

/**** the model ****/

int paint_init(struct paint *p, int w, int h) {
    int i;
    memset(p, 0, sizeof(*p));
    p->mem_max = PAINT_MEM_MAX;
    p->undo_max = PAINT_UNDO_MAX;
    p->fill_stack = PAINT_FILL_STACK;
    p->tool = p->back_tool = PAINT_BRUSH;
    p->size = 2;                                        /* 3 pixels */
    p->text_scale = 2;
    p->fg = 0x000000;
    p->bg = PAINT_PAPER;
    for(i = 0; i < PAINT_CUSTOM; i++) p->custom[i] = PAINT_PAPER;
    p->zoom = PAINT_ZOOM_1;
    p->backdrop = 0x102E3B;                             /* the desktop's navy, as the Image Viewer's dark space */
    if(paint_new(p, w ? w : PAINT_NEW_W, h ? h : PAINT_NEW_H) < 0) {
        paint_free(p);
        return -1;
    }
    return 0;
}

void paint_free(struct paint *p) {
    drop_work(p);
    history_clear(p);
    pbuf_put(p, p->clip);
    p->clip = NULL;
    if(p->px) pm_free(p, p->px, canvas_bytes(p->w, p->h));
    if(p->rowl) rows_free(p, p->rowl, p->h);
    p->px = NULL;
    p->rowl = p->rowr = NULL;
}
