/*
SurfOS user program: wsheet
--------------------
File: edit.c    Date: 11/05/26
--------------------
Every change to a sheet, and its history.

A change is built first, as a step, and only then made: everything it needs (the new contents,
their formulas compiled, the history's room) is asked for before any cell changes, so a change
that cannot get its memory changes nothing. A step is a list of cells and what each is to hold;
making it swaps each cell's content with the one in the list, so that afterwards the list holds
what the cells held. Undo is the same swap again, and so is Redo: nothing is copied either way.
A step that inserts or deletes rows or columns has a part before the move (the cells that go,
and the widths of columns that go or are pushed off the end) and a part after it (the formulas
that refer across it, rewritten by formula.c).

Swapping a content into a place that had no cell makes one, and that may ask for memory: when it
cannot, the cells already swapped are swapped back, which never asks for memory (a row keeps its
room until the change ends), and the change, Undo or Redo is refused. A step's cells are listed
once each, so the swaps do not depend on their order.

The history keeps 100 steps and 8 MB at most; a change bigger than that is made and cannot be
undone. A sheet is modified when its state is not the one the file holds: every step has a serial
number, and undoing back to the saved state makes it unmodified again.

Copy keeps cells with their contents and formats, and their values as text, a tab between cells
and a line end between rows, for the desktop's clipboard (128 KB at most, past which only the
Spreadsheet's own copy is made). Paste puts them back with their formulas' relative references
moved by as far as the cells moved: a reference that would leave the sheet becomes #REF!. Text
pasted from elsewhere is read as tab-separated cells and each one taken as typed, into the cells'
own formats.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sheetpriv.h"

#define HIST_STEPS 100                  /* steps kept */
#define HIST_BYTES (8L * 1024 * 1024)   /* and the memory they may hold */
#define CLIP_TEXT_MAX (128 * 1024)      /* the desktop's clipboard (WIN_CLIP_MAX): longer text is not made */
#define PASTE_TEXT_MAX (4 * 1024 * 1024)

enum { AXIS_NONE, AXIS_ROWS, AXIS_COLS };

struct list {                           /* one part of a step */
    struct change *v;
    int n, cap;
    struct wchange *w;
    int nw, capw;
};

struct step {
    unsigned serial, prev;              /* the state it makes, and the one before it */
    const char *label;
    struct sh_range where;
    struct list a, b;                   /* swapped before the rows or columns move, and after */
    int axis, at, n;                    /* rows or columns from at move by n (n < 0: -n of them go) */
    long bytes;
};

struct clipcell {
    int r, c;                           /* from the copy's top left */
    struct content ct;
};

struct sh_clip {
    int rows, cols, r0, c0;             /* its size, and where it was copied from */
    int n;
    struct clipcell *cells;             /* row by row */
    char *text;                         /* the values as text; NULL when longer than CLIP_TEXT_MAX */
    int len;
};

static int nomem(struct sheet *s, const char *what) {
    sheet_set_why(s, "Not enough memory %s", what);
    return -1;
}

static int range_ok(struct sh_range r) {
    return r.r1 >= 0 && r.c1 >= 0 && r.r2 < SH_ROWS && r.c2 < SH_COLS && r.r1 <= r.r2 && r.c1 <= r.c2;
}

static struct sh_range span(int r1, int c1, int r2, int c2) {
    struct sh_range r;
    r.r1 = r1;
    r.c1 = c1;
    r.r2 = r2;
    r.c2 = c2;
    return r;
}

/**** steps ****/

static long content_bytes(const struct content *c) {
    long n = (long)sizeof(struct change);
    if(c->kind == K_TEXT) n += c->len + 1;
    if(c->kind == K_FORMULA)
        n += (long)sizeof(struct formula) + c->f->len + 1 + (long)c->f->ncode * (long)sizeof(struct ins);
    return n;
}

static struct step *step_new(const char *label, struct sh_range where) {
    struct step *st = mem_zalloc(sizeof(*st));
    if(!st) return NULL;
    st->label = label;
    st->where = where;
    st->bytes = (long)sizeof(*st);
    return st;
}

static void list_free(struct list *l) {
    int i;
    for(i = 0; i < l->n; i++) content_free(&l->v[i].other);
    mem_free(l->v);
    mem_free(l->w);
    memset(l, 0, sizeof(*l));
}

static void step_free(struct step *st) {
    if(!st) return;
    list_free(&st->a);
    list_free(&st->b);
    mem_free(st);
}

/* the cell r, c is to hold ct, which the step takes (or frees, when there is no memory): 0 or -1 */
static int list_add(struct step *st, struct list *l, int r, int c, struct content *ct) {
    if(l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        struct change *v = mem_realloc(l->v, (size_t)cap * sizeof(*v));
        if(!v) {
            content_free(ct);
            return -1;
        }
        l->v = v;
        l->cap = cap;
    }
    st->bytes += content_bytes(ct);
    l->v[l->n].row = r;
    l->v[l->n].col = c;
    l->v[l->n].other = *ct;
    l->n++;
    memset(ct, 0, sizeof(*ct));
    return 0;
}

static int list_width(struct step *st, struct list *l, int col, int px) {
    if(l->nw == l->capw) {
        int cap = l->capw ? l->capw * 2 : 8;
        struct wchange *w = mem_realloc(l->w, (size_t)cap * sizeof(*w));
        if(!w) return -1;
        l->w = w;
        l->capw = cap;
    }
    l->w[l->nw].col = col;
    l->w[l->nw].other = px;
    l->nw++;
    st->bytes += (long)sizeof(struct wchange);
    return 0;
}

static void swap_widths(struct sheet *s, struct list *l) {
    int i, t;
    for(i = 0; i < l->nw; i++) {
        t = s->width[l->w[i].col];
        s->width[l->w[i].col] = (unsigned short)l->w[i].other;
        l->w[i].other = t;
    }
}

/* the step made, or taken back: the formulas it holds now are out of the sheet, and their values with them */
static void forget_values(struct step *st) {
    int i;
    for(i = 0; i < st->a.n; i++) content_forget_value(&st->a.v[i].other);
    for(i = 0; i < st->b.n; i++) content_forget_value(&st->b.v[i].other);
}

static void move(struct sheet *s, int axis, int at, int n) {
    if(axis == AXIS_ROWS) shift_rows(s, at, n);
    else if(axis == AXIS_COLS) shift_cols(s, at, n);
}

/* part b only swaps formulas that stay where they are: it never makes or takes a cell, so it cannot fail */
static void swap_all(struct sheet *s, struct list *l, int backward) {
    int i;
    if(backward) for(i = l->n - 1; i >= 0; i--) cell_swap(s, l->v[i].row, l->v[i].col, &l->v[i].other);
    else for(i = 0; i < l->n; i++) cell_swap(s, l->v[i].row, l->v[i].col, &l->v[i].other);
}

static int by_number(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* the rows a part will make cells in, made empty beforehand in one merge: a step of many cells (a whole column's
   format, a big paste) would otherwise move the rows after each new one, once per row. 0, or -1 with nothing done
   but empty rows, which drop_empty_rows() takes */
static int make_rows(struct sheet *s, const struct list *l) {
    struct row *rows;
    int *need, i, j, k = 0, n, found;
    if(l->n < 64) return 0;                     /* a few: cell_swap() makes them */
    need = mem_alloc((size_t)l->n * sizeof(*need));
    if(!need) return -1;
    for(i = 0; i < l->n; i++) {
        row_index(s, l->v[i].row, &found);
        if(!found && !content_absent(&l->v[i].other)) need[k++] = l->v[i].row;
    }
    qsort(need, (size_t)k, sizeof(*need), by_number);
    for(i = n = 0; i < k; i++) if(!n || need[i] != need[n - 1]) need[n++] = need[i];
    if(!n) {
        mem_free(need);
        return 0;
    }
    rows = mem_alloc((size_t)(s->nrows + n) * sizeof(*rows));
    if(!rows) {
        mem_free(need);
        return -1;
    }
    for(i = j = k = 0; i < s->nrows || j < n; k++) {
        if(j >= n || (i < s->nrows && s->rows[i].r < need[j])) rows[k] = s->rows[i++];
        else {
            memset(&rows[k], 0, sizeof(rows[k]));
            rows[k].r = need[j++];
        }
    }
    mem_free(s->rows);
    mem_free(need);
    s->rows = rows;
    s->nrows = s->caprows = k;
    return 0;
}

/* the step made: 0; or -1 with the sheet as it was */
static int step_forward(struct sheet *s, struct step *st) {
    int i;
    if(make_rows(s, &st->a) < 0) return -1;
    for(i = 0; i < st->a.n; i++)
        if(cell_swap(s, st->a.v[i].row, st->a.v[i].col, &st->a.v[i].other) < 0) {
            while(--i >= 0) cell_swap(s, st->a.v[i].row, st->a.v[i].col, &st->a.v[i].other);
            drop_empty_rows(s);
            return -1;
        }
    swap_widths(s, &st->a);
    move(s, st->axis, st->at, st->n);
    swap_all(s, &st->b, 0);
    swap_widths(s, &st->b);
    drop_empty_rows(s);
    forget_values(st);
    return 0;
}

/* the step taken back: 0; or -1 with the sheet as it was */
static int step_backward(struct sheet *s, struct step *st) {
    int i = st->a.n - 1;
    swap_widths(s, &st->b);
    swap_all(s, &st->b, 1);
    move(s, st->axis, st->at, -st->n);
    if(make_rows(s, &st->a) == 0)
        for(; i >= 0; i--)
            if(cell_swap(s, st->a.v[i].row, st->a.v[i].col, &st->a.v[i].other) < 0) break;
    if(i >= 0) {                                /* no memory: everything back as it was */
        for(i++; i < st->a.n; i++) cell_swap(s, st->a.v[i].row, st->a.v[i].col, &st->a.v[i].other);
        move(s, st->axis, st->at, st->n);
        swap_all(s, &st->b, 0);
        swap_widths(s, &st->b);
        drop_empty_rows(s);
        return -1;
    }
    swap_widths(s, &st->a);
    drop_empty_rows(s);
    forget_values(st);
    return 0;
}

void hist_free(struct sheet *s) {
    int i;
    for(i = 0; i < s->nhist; i++) step_free(s->hist[i]);
    mem_free(s->hist);
    s->hist = NULL;
    s->nhist = s->caphist = s->pos = 0;
    s->hist_bytes = 0;
}

/* room in the history for one more step at pos */
static int hist_room(struct sheet *s) {
    struct step **h;
    int cap;
    if(s->pos < s->caphist) return 0;
    cap = s->caphist ? s->caphist * 2 : 16;
    h = mem_realloc(s->hist, (size_t)cap * sizeof(*h));
    if(!h) return -1;
    s->hist = h;
    s->caphist = cap;
    return 0;
}

static void hist_push(struct sheet *s, struct step *st) {
    int i;
    for(i = s->pos; i < s->nhist; i++) {        /* what was undone cannot be redone after a new change */
        s->hist_bytes -= s->hist[i]->bytes;
        step_free(s->hist[i]);
    }
    s->nhist = s->pos;
    st->prev = s->state;
    st->serial = ++s->serials;
    s->state = st->serial;
    s->hist[s->nhist++] = st;
    s->pos = s->nhist;
    s->hist_bytes += st->bytes;
    if(st->bytes > HIST_BYTES) {                /* too big to keep: made, and the history starts again */
        hist_free(s);
        return;
    }
    while(s->nhist > HIST_STEPS || s->hist_bytes > HIST_BYTES) {
        s->hist_bytes -= s->hist[0]->bytes;
        step_free(s->hist[0]);
        memmove(s->hist, s->hist + 1, (size_t)(s->nhist - 1) * sizeof(*s->hist));
        s->nhist--;
        s->pos--;
    }
}

/* the step made and kept: 0, or -1 with nothing changed */
static int finish(struct sheet *s, struct step *st) {
    struct cell *c;
    int i;
    if(!st->a.n && !st->b.n && !st->a.nw && !st->b.nw && st->axis == AXIS_NONE) {
        step_free(st);                          /* nothing to change */
        return 0;
    }
    for(i = 0; i < st->a.n; i++)                /* made, the step holds what the cells hold now */
        if((c = cell_at(s, st->a.v[i].row, st->a.v[i].col))) st->bytes += content_bytes(&c->c);
    for(i = 0; i < st->b.n; i++) st->bytes += content_bytes(&st->b.v[i].other);    /* a formula's text, about */
    if(hist_room(s) < 0 || step_forward(s, st) < 0) {
        step_free(st);
        return nomem(s, "for the change");
    }
    hist_push(s, st);
    recalc(s);
    return 0;
}

int sh_undo(struct sheet *s, struct sh_range *where) {
    struct step *st;
    s->why[0] = 0;
    if(!s->pos) {
        sheet_set_why(s, "Nothing to undo");
        return -1;
    }
    st = s->hist[s->pos - 1];
    if(step_backward(s, st) < 0) return nomem(s, "to undo");
    s->pos--;
    s->state = st->prev;
    if(where) *where = st->where;
    recalc(s);
    return 0;
}

int sh_redo(struct sheet *s, struct sh_range *where) {
    struct step *st;
    s->why[0] = 0;
    if(s->pos >= s->nhist) {
        sheet_set_why(s, "Nothing to redo");
        return -1;
    }
    st = s->hist[s->pos];
    if(step_forward(s, st) < 0) return nomem(s, "to redo");
    s->pos++;
    s->state = st->serial;
    if(where) *where = st->where;
    recalc(s);
    return 0;
}

const char *sh_undo_label(const struct sheet *s) {
    return s->pos ? s->hist[s->pos - 1]->label : NULL;
}

const char *sh_redo_label(const struct sheet *s) {
    return s->pos < s->nhist ? s->hist[s->pos]->label : NULL;
}

/**** the changes ****/

static int full(struct sheet *s, long more) {
    if(s->ncells + more <= SH_CELLS_MAX) return 0;
    sheet_set_why(s, "The sheet is full: it keeps %d cells at most", SH_CELLS_MAX);
    return 1;
}

int sh_set(struct sheet *s, int row, int col, const char *text, int len) {
    struct content ct;
    struct step *st;
    struct cell *c;
    s->why[0] = 0;
    if(row < 0 || row >= SH_ROWS || col < 0 || col >= SH_COLS || len < 0) {
        sheet_set_why(s, "No such cell");
        return -1;
    }
    if(len > SH_INPUT_MAX) {
        sheet_set_why(s, "A cell holds %d bytes at most", SH_INPUT_MAX);
        return -1;
    }
    if(len && memchr(text, 0, (size_t)len)) {
        sheet_set_why(s, "A cell cannot hold a NUL character");
        return -1;
    }
    c = cell_at(s, row, col);
    if(content_parse(&ct, text, len, c ? c->c.fmt : SH_GENERAL) < 0) return nomem(s, "for the cell");
    if(c ? content_equal(&c->c, &ct) : content_absent(&ct)) {
        content_free(&ct);
        return 0;
    }
    if(!c && full(s, 1)) {
        content_free(&ct);
        return -1;
    }
    st = step_new("typing", span(row, col, row, col));
    if(!st) {
        content_free(&ct);
        return nomem(s, "for the change");
    }
    if(list_add(st, &st->a, row, col, &ct) < 0) {
        step_free(st);
        return nomem(s, "for the change");
    }
    return finish(s, st);
}

int sh_clear(struct sheet *s, struct sh_range r) {
    struct content ct;
    struct step *st;
    struct walk w;
    struct cell *c;
    int row;
    s->why[0] = 0;
    if(!range_ok(r)) {
        sheet_set_why(s, "No such cells");
        return -1;
    }
    st = step_new("clearing", r);
    if(!st) return nomem(s, "for the change");
    walk_start(s, &w, r.r1, r.c1, r.r2, r.c2, 0);
    while((c = walk_next(s, &w, &row, NULL))) {
        if(c->c.kind == K_NONE) continue;
        memset(&ct, 0, sizeof(ct));
        ct.fmt = c->c.fmt;                      /* the format stays */
        if(list_add(st, &st->a, row, c->col, &ct) < 0) {
            step_free(st);
            return nomem(s, "for the change");
        }
    }
    return finish(s, st);
}

static int fmt_ok(int fmt) {
    int kind = SH_FMT_KIND(fmt), d = SH_FMT_DECIMALS(fmt);
    if(fmt < 0 || fmt > 0xFF) return 0;
    if(kind == SH_GENERAL) return d == 0;
    return (kind == SH_FIXED(0) || kind == SH_PERCENT(0)) && d <= SH_DECIMALS_MAX;
}

int sh_set_format(struct sheet *s, struct sh_range r, int fmt) {
    struct content ct;
    struct step *st;
    struct cell *c;
    long made = 0;
    int row, col;
    s->why[0] = 0;
    if(!range_ok(r) || !fmt_ok(fmt)) {
        sheet_set_why(s, "No such cells or format");
        return -1;
    }
    if((long long)(r.r2 - r.r1 + 1) * (r.c2 - r.c1 + 1) > SH_AREA_MAX) {
        sheet_set_why(s, "A format is set on %d cells at most at once: a whole column", SH_AREA_MAX);
        return -1;
    }
    st = step_new("format", r);
    if(!st) return nomem(s, "for the change");
    for(row = r.r1; row <= r.r2; row++)
        for(col = r.c1; col <= r.c2; col++) {
            c = cell_at(s, row, col);
            if(c) {
                if(c->c.fmt == fmt) continue;
                if(content_copy(&ct, &c->c) < 0) goto fail;
            } else {
                if(fmt == SH_GENERAL) continue;
                memset(&ct, 0, sizeof(ct));
                made++;
            }
            ct.fmt = (unsigned char)fmt;
            if(list_add(st, &st->a, row, col, &ct) < 0) goto fail;
        }
    if(full(s, made)) {
        step_free(st);
        return -1;
    }
    return finish(s, st);
fail:
    step_free(st);
    return nomem(s, "for the change");
}

int sh_set_width(struct sheet *s, int c1, int c2, int px) {
    struct step *st;
    int c;
    s->why[0] = 0;
    if(c1 < 0 || c2 >= SH_COLS || c1 > c2) {
        sheet_set_why(s, "No such columns");
        return -1;
    }
    if(px < SH_WIDTH_MIN) px = SH_WIDTH_MIN;
    if(px > SH_WIDTH_MAX) px = SH_WIDTH_MAX;
    st = step_new("column width", span(0, c1, SH_ROWS - 1, c2));
    if(!st) return nomem(s, "for the change");
    for(c = c1; c <= c2; c++)
        if(s->width[c] != px && list_width(st, &st->a, c, px) < 0) {
            step_free(st);
            return nomem(s, "for the change");
        }
    return finish(s, st);
}

/* any cell at from or past it, along the axis */
static int cells_from(const struct sheet *s, int axis, int from) {
    int i;
    if(axis == AXIS_ROWS) return s->nrows && s->rows[s->nrows - 1].r >= from;
    for(i = 0; i < s->nrows; i++)
        if(s->rows[i].n && s->rows[i].cells[s->rows[i].n - 1].col >= from) return 1;
    return 0;
}

/* rows (or columns) from at move by n: n > 0 inserts n there, n < 0 deletes -n from at */
static int structural(struct sheet *s, int axis, int at, int n, const char *label) {
    static char buf[SH_INPUT_MAX];
    int limit = axis == AXIS_ROWS ? SH_ROWS : SH_COLS, m = n < 0 ? -n : n, i, j, r, c, len;
    const char *what = axis == AXIS_ROWS ? "rows" : "columns";
    struct content ct;
    struct step *st;
    struct walk w;
    struct cell *cl;
    s->why[0] = 0;
    if(at < 0 || m < 1 || at + m > limit) {
        sheet_set_why(s, "No such %s", what);
        return -1;
    }
    if(n > 0 && cells_from(s, axis, limit - m)) {
        sheet_set_why(s, "Inserting would push cells off the end of the sheet: clear its last %d %s first", m, what);
        return -1;
    }
    st = step_new(label, axis == AXIS_ROWS ? span(at, 0, at + m - 1, SH_COLS - 1) :
                                             span(0, at, SH_ROWS - 1, at + m - 1));
    if(!st) return nomem(s, "for the change");
    st->axis = axis;
    st->at = at;
    st->n = n;
    if(n < 0) {                                 /* the cells that go */
        if(axis == AXIS_ROWS) walk_start(s, &w, at, 0, at + m - 1, SH_COLS - 1, 0);
        else walk_start(s, &w, 0, at, SH_ROWS - 1, at + m - 1, 0);
        while((cl = walk_next(s, &w, &r, NULL))) {
            memset(&ct, 0, sizeof(ct));
            if(list_add(st, &st->a, r, cl->col, &ct) < 0) goto fail;
        }
    }
    if(axis == AXIS_COLS)                       /* the widths that go, or are pushed off the end */
        for(c = n < 0 ? at : limit - m; c < (n < 0 ? at + m : limit); c++)
            if(s->width[c] != SH_WIDTH_DEFAULT && list_width(st, &st->a, c, SH_WIDTH_DEFAULT) < 0) goto fail;
    for(i = 0; i < s->nrows; i++)               /* the formulas that stay, rewritten where they will be */
        for(j = 0; j < s->rows[i].n; j++) {
            struct content *f = &s->rows[i].cells[j].c;
            int pos;
            if(f->kind != K_FORMULA) continue;
            r = s->rows[i].r;
            c = s->rows[i].cells[j].col;
            pos = axis == AXIS_ROWS ? r : c;
            if(n < 0 && pos >= at && pos < at + m) continue;
            len = fx_rewrite(f->f->src, f->f->len, axis == AXIS_ROWS ? FX_ROWS : FX_COLS, at, n, buf, sizeof(buf));
            if(len == -2) {
                step_free(st);
                sheet_set_why(s, "A formula would grow past %d bytes", SH_INPUT_MAX - 1);
                return -1;
            }
            if(len < 0 || (len == f->f->len && !memcmp(buf, f->f->src, (size_t)len))) continue;
            if(content_formula(&ct, buf, len, f->fmt) < 0) goto fail;
            if(pos >= at) pos = n > 0 ? pos + n : pos - m;
            if(list_add(st, &st->b, axis == AXIS_ROWS ? pos : r, axis == AXIS_ROWS ? c : pos, &ct) < 0) goto fail;
        }
    return finish(s, st);
fail:
    step_free(st);
    return nomem(s, "for the change");
}

int sh_insert_rows(struct sheet *s, int at, int n) {
    return structural(s, AXIS_ROWS, at, n > 0 ? n : 0, "inserting rows");
}

int sh_delete_rows(struct sheet *s, int at, int n) {
    return structural(s, AXIS_ROWS, at, n > 0 ? -n : 0, "deleting rows");
}

int sh_insert_cols(struct sheet *s, int at, int n) {
    return structural(s, AXIS_COLS, at, n > 0 ? n : 0, "inserting columns");
}

int sh_delete_cols(struct sheet *s, int at, int n) {
    return structural(s, AXIS_COLS, at, n > 0 ? -n : 0, "deleting columns");
}

/**** the clipboard ****/

struct text {                           /* a text that grows, up to a limit */
    char *s;
    int len, cap, max, over;
};

static void text_add(struct text *t, const char *s, int n) {
    if(t->over) return;
    if(t->len + n > t->max) {
        t->over = 1;
        return;
    }
    if(t->len + n + 1 > t->cap) {
        int cap = t->cap ? t->cap : 256;
        char *p;
        while(cap < t->len + n + 1) cap *= 2;
        p = mem_realloc(t->s, (size_t)cap);
        if(!p) {
            t->over = 2;
            return;
        }
        t->s = p;
        t->cap = cap;
    }
    memcpy(t->s + t->len, s, (size_t)n);
    t->len += n;
    t->s[t->len] = 0;
}

/* a field with a tab, a line end or a quote in it goes in quotes, its quotes doubled (as CSV does, with tabs) */
static void field_add(struct text *t, const char *s, int n) {
    int i, start;
    if(!memchr(s, '\t', (size_t)n) && !memchr(s, '\n', (size_t)n) && !memchr(s, '\r', (size_t)n) &&
       !memchr(s, '"', (size_t)n)) {
        text_add(t, s, n);
        return;
    }
    text_add(t, "\"", 1);
    for(i = start = 0; i < n; i++)
        if(s[i] == '"') {
            text_add(t, s + start, i + 1 - start);
            text_add(t, "\"", 1);
            start = i + 1;
        }
    text_add(t, s + start, n - start);
    text_add(t, "\"", 1);
}

/* the range's values as they show, without the width's limit: tab-separated text, or NULL when it is too long */
static char *clip_text(const struct sheet *s, struct sh_range r, int *len) {
    static char buf[SH_TEXT_MAX + 64];
    struct text t;
    int row, col, n, align;
    memset(&t, 0, sizeof(t));
    t.max = CLIP_TEXT_MAX;
    text_add(&t, "", 0);
    for(row = r.r1; row <= r.r2 && !t.over; row++) {
        if(row > r.r1) text_add(&t, "\n", 1);
        for(col = r.c1; col <= r.c2 && !t.over; col++) {
            if(col > r.c1) text_add(&t, "\t", 1);
            n = sh_display(s, row, col, 0, buf, sizeof(buf), &align);
            if(n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
            field_add(&t, buf, n);
        }
    }
    if(t.over || !t.s) {
        mem_free(t.s);
        *len = 0;
        return NULL;
    }
    *len = t.len;
    return t.s;
}

void sh_clip_free(struct sh_clip *c) {
    int i;
    if(!c) return;
    for(i = 0; i < c->n; i++) content_free(&c->cells[i].ct);
    mem_free(c->cells);
    mem_free(c->text);
    mem_free(c);
}

struct sh_clip *sh_copy(struct sheet *s, struct sh_range r) {
    struct sh_clip *c;
    struct walk w;
    struct cell *cl;
    int row, n = 0;
    s->why[0] = 0;
    if(!range_ok(r)) {
        sheet_set_why(s, "No such cells");
        return NULL;
    }
    if((long long)(r.r2 - r.r1 + 1) * (r.c2 - r.c1 + 1) > SH_COPY_MAX) {
        sheet_set_why(s, "Copy takes %d cells at most: 16 whole columns", SH_COPY_MAX);
        return NULL;
    }
    walk_start(s, &w, r.r1, r.c1, r.r2, r.c2, 0);
    while(walk_next(s, &w, &row, NULL)) n++;
    c = mem_zalloc(sizeof(*c));
    if(!c) goto fail;
    c->rows = r.r2 - r.r1 + 1;
    c->cols = r.c2 - r.c1 + 1;
    c->r0 = r.r1;
    c->c0 = r.c1;
    c->cells = mem_alloc((size_t)(n ? n : 1) * sizeof(*c->cells));
    if(!c->cells) goto fail;
    walk_start(s, &w, r.r1, r.c1, r.r2, r.c2, 0);
    while((cl = walk_next(s, &w, &row, NULL)) && c->n < n) {
        if(content_copy(&c->cells[c->n].ct, &cl->c) < 0) goto fail;
        c->cells[c->n].r = row - r.r1;
        c->cells[c->n].c = cl->col - r.c1;
        c->n++;
    }
    c->text = clip_text(s, r, &c->len);
    return c;
fail:
    sh_clip_free(c);
    nomem(s, "to copy");
    return NULL;
}

const char *sh_clip_text(const struct sh_clip *c, int *len) {
    *len = c->len;
    return c->text;
}

void sh_clip_size(const struct sh_clip *c, int *rows, int *cols) {
    *rows = c->rows;
    *cols = c->cols;
}

/* the clip's cell at r, c from its top left, or NULL */
static const struct clipcell *clip_at(const struct sh_clip *c, int r, int col) {
    int lo = 0, hi = c->n, mid;
    while(lo < hi) {
        mid = lo + (hi - lo) / 2;
        if(c->cells[mid].r < r || (c->cells[mid].r == r && c->cells[mid].c < col)) lo = mid + 1;
        else hi = mid;
    }
    return lo < c->n && c->cells[lo].r == r && c->cells[lo].c == col ? &c->cells[lo] : NULL;
}

static int fits(struct sheet *s, int row, int col, int rows, int cols) {
    if(row >= 0 && col >= 0 && row + rows <= SH_ROWS && col + cols <= SH_COLS) return 1;
    sheet_set_why(s, "The paste would go past the edge of the sheet");
    return 0;
}

int sh_paste(struct sheet *s, const struct sh_clip *clip, int row, int col, struct sh_range *where) {
    static char buf[SH_INPUT_MAX];
    struct content ct;
    struct step *st;
    struct walk w;
    struct cell *cl;
    long made = 0;
    int r, i, len;
    s->why[0] = 0;
    if(!fits(s, row, col, clip->rows, clip->cols)) return -1;
    st = step_new("paste", span(row, col, row + clip->rows - 1, col + clip->cols - 1));
    if(!st) return nomem(s, "to paste");
    walk_start(s, &w, row, col, row + clip->rows - 1, col + clip->cols - 1, 0);
    while((cl = walk_next(s, &w, &r, NULL))) {  /* cells the copy has none for are emptied, formats too */
        if(clip_at(clip, r - row, cl->col - col)) continue;
        memset(&ct, 0, sizeof(ct));
        made--;
        if(list_add(st, &st->a, r, cl->col, &ct) < 0) goto fail;
    }
    for(i = 0; i < clip->n; i++) {
        const struct content *from = &clip->cells[i].ct;
        int tr = row + clip->cells[i].r, tc = col + clip->cells[i].c;
        if(from->kind == K_FORMULA) {
            len = fx_rewrite(from->f->src, from->f->len, FX_MOVE, row - clip->r0, col - clip->c0, buf, sizeof(buf));
            if(len == -2) {
                step_free(st);
                sheet_set_why(s, "A formula would grow past %d bytes", SH_INPUT_MAX - 1);
                return -1;
            }
            if(len == -1) {
                memcpy(buf, from->f->src, (size_t)from->f->len);
                len = from->f->len;
            }
            if(content_formula(&ct, buf, len, from->fmt) < 0) goto fail;
        } else if(content_copy(&ct, from) < 0) goto fail;
        cl = cell_at(s, tr, tc);
        if(cl && content_equal(&cl->c, &ct)) {
            content_free(&ct);
            continue;
        }
        if(!cl) made++;
        if(list_add(st, &st->a, tr, tc, &ct) < 0) goto fail;
    }
    if(full(s, made)) {
        step_free(st);
        return -1;
    }
    if(where) *where = st->where;
    return finish(s, st);
fail:
    step_free(st);
    return nomem(s, "to paste");
}

/* text pasted from elsewhere: its fields as the dsv reader gives them */
struct pasted {
    struct sheet *s;
    struct step *st;
    int row, col;
    long made;
    int nomem;
};

static int pasted_field(void *ctx, int r, int c, const char *text, int len) {
    struct pasted *p = ctx;
    struct content ct;
    struct cell *cl;
    int tr = p->row + r, tc = p->col + c;
    if(tr >= SH_ROWS || tc >= SH_COLS) return 0;        /* fits() refuses it at the end */
    cl = cell_at(p->s, tr, tc);
    if(content_parse(&ct, text, len, cl ? cl->c.fmt : SH_GENERAL) < 0) {
        p->nomem = 1;
        return -1;
    }
    if(cl ? content_equal(&cl->c, &ct) : content_absent(&ct)) {
        content_free(&ct);
        return 0;
    }
    if(!cl) p->made++;
    else if(content_absent(&ct)) p->made--;
    if(list_add(p->st, &p->st->a, tr, tc, &ct) < 0) {
        p->nomem = 1;
        return -1;
    }
    return 0;
}

int sh_paste_text(struct sheet *s, const char *text, int len, int row, int col, struct sh_range *where) {
    struct pasted p;
    struct dsv d;
    int rows, cols, r;
    s->why[0] = 0;
    if(len < 0 || len > PASTE_TEXT_MAX) {
        sheet_set_why(s, "The text is too long to paste");
        return -1;
    }
    memset(&p, 0, sizeof(p));
    p.s = s;
    p.row = row;
    p.col = col;
    p.st = step_new("paste", span(row, col, row, col));
    if(!p.st || dsv_init(&d, '\t', pasted_field, &p) < 0) {
        step_free(p.st);
        return nomem(s, "to paste");
    }
    r = dsv_feed(&d, text, len);
    if(!r) r = dsv_end(&d);
    rows = d.rows;
    cols = d.cols;
    if(r < 0) {
        if(p.nomem) nomem(s, "to paste");
        else sheet_set_why(s, "%s", d.why);
        dsv_free(&d);
        step_free(p.st);
        return -1;
    }
    dsv_free(&d);
    if(!rows || !cols) {
        step_free(p.st);
        return 0;
    }
    if(!fits(s, row, col, rows, cols) || full(s, p.made)) {
        step_free(p.st);
        return -1;
    }
    p.st->where = span(row, col, row + rows - 1, col + cols - 1);
    if(where) *where = p.st->where;
    return finish(s, p.st);
}
