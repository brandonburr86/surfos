/*
SurfOS user program: wpaint
--------------------
File: history.c Date: 11/05/26
--------------------
Undo and redo of changed regions. The canvas is cut into tiles of PAINT_TILE x PAINT_TILE pixels
(smaller at its right and bottom edges). A change opens a step (step_begin()); the first time it
touches a tile, before a pixel of it is written, touch() saves the tile's pixels into the step:
as (count, colour) pairs when there are few runs, which makes a blank tile a few bytes, or as the
pixels themselves. So a stroke across a large canvas keeps only the tiles it crossed, and a blank
canvas's history costs almost nothing.

Undo takes the step out of the history and swaps: each tile's pixels now are saved, and the
step's are put back; the saved ones are what Redo puts back in turn, so one step serves both
ways. When the pixels now cannot be saved (no memory), the undo is still done, and what lies
beyond it can no longer be redone. A new size (step_canvas()) saves every tile of the canvas
before it, and its undo makes a canvas of the old size again.

The bounds: PAINT_UNDO_STEPS steps and p->undo_max bytes (the step under way counted), the
oldest step dropped first when either is passed, or when pm_alloc() needs room. A change that
alone passes the bytes, or whose tiles cannot be had, is made all the same but is lost: its tiles
go, the history before it is dropped (it cannot be reached across a change that cannot be
undone), and the status line says so.

The canvas's state moves with the history: each kept step goes from one state to another, so the
window knows (paint_modified()) whether undoing went back to what was saved.
*/
#include <stdio.h>
#include <string.h>
#include "paint.h"
#include "ppriv.h"

int tiles_across(int w) {
    return (w + PAINT_TILE - 1) / PAINT_TILE;
}

void tile_rect(int w, int h, int t, int *x, int *y, int *tw, int *th) {
    int across = tiles_across(w);
    *x = t % across * PAINT_TILE;
    *y = t / across * PAINT_TILE;
    *tw = w - *x < PAINT_TILE ? w - *x : PAINT_TILE;
    *th = h - *y < PAINT_TILE ? h - *y : PAINT_TILE;
}

/**** a tile's pixels ****/

/* tile t of a canvas of w x h into out, from pm_alloc(): 0, or -1 without the memory */
static int tile_encode(struct paint *p, const unsigned *px, int w, int h, int t, struct ptile *out) {
    const unsigned *row;
    unsigned prev = 0, count = 0, runs = 0, *q;
    int x0, y0, tw, th, x, y;
    size_t words;
    tile_rect(w, h, t, &x0, &y0, &tw, &th);
    for(y = 0; y < th; y++) {
        row = px + (size_t)(y0 + y) * (size_t)w + x0;
        for(x = 0; x < tw; x++)
            if(!runs || row[x] != prev) {
                runs++;
                prev = row[x];
            }
    }
    out->t = t;
    out->raw = runs * 2 >= (unsigned)(tw * th);
    words = out->raw ? (size_t)tw * (size_t)th : (size_t)runs * 2;
    out->data = (unsigned *)pm_alloc(p, words * sizeof(unsigned));
    out->n = out->data ? (unsigned)words : 0;
    if(!out->data) return -1;
    q = out->data;
    for(y = 0; y < th; y++) {
        row = px + (size_t)(y0 + y) * (size_t)w + x0;
        if(out->raw) {
            memcpy(q, row, (size_t)tw * sizeof(unsigned));
            q += tw;
            continue;
        }
        for(x = 0; x < tw; x++) {
            if(count && row[x] == prev) {
                count++;
                continue;
            }
            if(count) {
                *q++ = count;
                *q++ = prev;
            }
            prev = row[x];
            count = 1;
        }
    }
    if(!out->raw) {
        *q++ = count;
        *q++ = prev;
    }
    return 0;
}

/* a tile's pixels back onto a canvas of w x h (its pixels are its own, made by tile_encode() for that size) */
static void tile_decode(const struct ptile *in, unsigned *px, int w, int h) {
    const unsigned *q = in->data, *end = in->data + in->n;
    unsigned *row, left = 0, c = 0;
    int x0, y0, tw, th, x, y;
    tile_rect(w, h, in->t, &x0, &y0, &tw, &th);
    for(y = 0; y < th; y++) {
        row = px + (size_t)(y0 + y) * (size_t)w + x0;
        if(in->raw) {
            memcpy(row, q + (size_t)y * (size_t)tw, (size_t)tw * sizeof(unsigned));
            continue;
        }
        for(x = 0; x < tw; x++) {
            if(!left && end - q >= 2) {
                left = q[0];
                c = q[1];
                q += 2;
            }
            row[x] = c;
            if(left) left--;
        }
    }
}

static void tile_mark(struct paint *p, int t) {
    int x, y, tw, th;
    tile_rect(p->w, p->h, t, &x, &y, &tw, &th);
    mark(p, x, y, x + tw - 1, y + th - 1);
}

/**** steps ****/

static void tile_drop(struct paint *p, struct pstep *s, int i) {
    size_t n = (size_t)s->tiles[i].n * sizeof(unsigned);
    if(!s->tiles[i].data) return;
    pm_free(p, s->tiles[i].data, n);
    s->tiles[i].data = NULL;
    s->tiles[i].n = 0;
    s->bytes -= n;
    p->undo_bytes -= n;
}

static void tile_keep(struct paint *p, struct pstep *s, int i, const struct ptile *tile) {
    size_t n = (size_t)tile->n * sizeof(unsigned);
    s->tiles[i] = *tile;
    s->bytes += n;
    p->undo_bytes += n;
}

/* its tiles, their table and its bits go; the step itself stays */
static void step_empty(struct paint *p, struct pstep *s) {
    int i;
    size_t n;
    for(i = 0; i < s->ntiles; i++) tile_drop(p, s, i);
    if(s->tiles) {
        n = (size_t)s->cap * sizeof(struct ptile);
        pm_free(p, s->tiles, n);
        s->bytes -= n;
        p->undo_bytes -= n;
    }
    s->tiles = NULL;
    s->ntiles = s->cap = 0;
    if(s->saved) {
        n = ((size_t)tiles_across(s->w) * (size_t)tiles_across(s->h) + 7) / 8;
        pm_free(p, s->saved, n);
        s->bytes -= n;
        p->undo_bytes -= n;
    }
    s->saved = NULL;
}

static void step_free(struct paint *p, struct pstep *s) {
    step_empty(p, s);
    if(s == &p->nostep) return;
    p->undo_bytes -= s->bytes;                          /* what is left: the step itself */
    pm_free(p, s, sizeof(*s));
}

void history_clear(struct paint *p) {
    while(p->nsteps > 0) step_free(p, p->steps[--p->nsteps]);
    p->cur = 0;
}

int history_trim(struct paint *p) {
    if(p->cur > 0) {                                    /* the oldest step that can be undone */
        step_free(p, p->steps[0]);
        memmove(&p->steps[0], &p->steps[1], (size_t)(p->nsteps - 1) * sizeof(p->steps[0]));
        p->nsteps--;
        p->cur--;
        return 1;
    }
    if(p->nsteps > 0) {                                 /* the redo the furthest away */
        step_free(p, p->steps[--p->nsteps]);
        return 1;
    }
    return 0;
}

size_t history_kept(const struct paint *p) {
    size_t n = 0;
    int i;
    for(i = 0; i < p->nsteps; i++) n += p->steps[i]->bytes;
    return n;
}

/* the change under way cannot be kept: it goes on, but the history goes with it */
static void lose(struct paint *p, struct pstep *s, int memory) {
    step_empty(p, s);
    s->lost = 1;
    history_clear(p);
    if(memory) say(p, "Not enough memory to keep this change: it cannot be undone");
    else say(p, "This change is larger than the history's %u MB: it cannot be undone",
             (unsigned)(p->undo_max >> 20));
}

static void save_tile(struct paint *p, struct pstep *s, int t) {
    struct ptile tile, *table;
    size_t was, n;
    int cap;
    if(s->ntiles == s->cap) {                           /* a table twice as big */
        cap = s->cap ? s->cap * 2 : 16;
        table = (struct ptile *)pm_alloc(p, (size_t)cap * sizeof(struct ptile));
        if(!table) {
            lose(p, s, 1);
            return;
        }
        if(s->ntiles) memcpy(table, s->tiles, (size_t)s->ntiles * sizeof(struct ptile));
        was = (size_t)s->cap * sizeof(struct ptile);
        n = (size_t)cap * sizeof(struct ptile);
        if(s->tiles) pm_free(p, s->tiles, was);
        s->tiles = table;
        s->cap = cap;
        s->bytes += n - was;
        p->undo_bytes += n - was;
    }
    if(tile_encode(p, p->px, p->w, p->h, t, &tile) < 0) {
        lose(p, s, 1);
        return;
    }
    tile_keep(p, s, s->ntiles++, &tile);
    while(p->undo_bytes > p->undo_max && history_trim(p))
        ;
    if(p->undo_bytes > p->undo_max) lose(p, s, 0);
}

/* a step for the change about to be made (the one under way ends first) */
static struct pstep *step_open(struct paint *p, const char *what, int kind) {
    struct pstep *s;
    if(p->open) step_end(p);
    s = (struct pstep *)pm_alloc(p, sizeof(*s));
    if(!s) {
        s = &p->nostep;
        memset(s, 0, sizeof(*s));
        s->w = p->w;
        s->h = p->h;
        p->open = s;
        lose(p, s, 1);
        return s;
    }
    memset(s, 0, sizeof(*s));
    snprintf(s->what, sizeof(s->what), "%s", what);
    s->kind = kind;
    s->w = p->w;
    s->h = p->h;
    s->bytes = sizeof(*s);
    p->undo_bytes += s->bytes;
    p->open = s;
    return s;
}

int step_begin(struct paint *p, const char *what) {
    struct pstep *s = step_open(p, what, PSTEP_TILES);
    size_t n = ((size_t)p->tiles_x * (size_t)p->tiles_y + 7) / 8;
    if(s->lost) return -1;
    s->saved = (unsigned char *)pm_alloc(p, n);
    if(!s->saved) {
        lose(p, s, 1);
        return -1;
    }
    memset(s->saved, 0, n);
    s->bytes += n;
    p->undo_bytes += n;
    return 0;
}

int step_canvas(struct paint *p, const char *what) {
    struct pstep *s = step_open(p, what, PSTEP_CANVAS);
    int t, n = p->tiles_x * p->tiles_y;
    if(s->lost) return -1;
    s->tiles = (struct ptile *)pm_alloc(p, (size_t)n * sizeof(struct ptile));
    if(!s->tiles) {
        lose(p, s, 1);
        return -1;
    }
    s->cap = n;
    s->bytes += (size_t)n * sizeof(struct ptile);
    p->undo_bytes += (size_t)n * sizeof(struct ptile);
    for(t = 0; t < n && !s->lost; t++) save_tile(p, s, t);
    return s->lost ? -1 : 0;
}

void touch(struct paint *p, int y, int x0, int x1) {
    struct pstep *s = p->open;
    int tx, t, row;
    if(!s || s->lost || s->kind != PSTEP_TILES) return;
    row = y / PAINT_TILE * p->tiles_x;
    for(tx = x0 / PAINT_TILE; tx <= x1 / PAINT_TILE; tx++) {
        t = row + tx;
        if(s->saved[t >> 3] & (1u << (t & 7))) continue;
        s->saved[t >> 3] |= (unsigned char)(1u << (t & 7));
        save_tile(p, s, t);
        if(s->lost) return;
    }
}

void step_end(struct paint *p) {
    struct pstep *s = p->open;
    size_t n;
    p->open = NULL;
    if(!s) return;
    if(s->lost) {                                       /* made, and not kept: a state of its own */
        step_free(p, s);
        p->state = ++p->serial;
        return;
    }
    if(!s->ntiles) {                                    /* nothing was changed: the redo stays */
        step_free(p, s);
        return;
    }
    if(s->saved) {                                      /* only needed while it was open */
        n = ((size_t)tiles_across(s->w) * (size_t)tiles_across(s->h) + 7) / 8;
        pm_free(p, s->saved, n);
        s->saved = NULL;
        s->bytes -= n;
        p->undo_bytes -= n;
    }
    while(p->nsteps > p->cur) step_free(p, p->steps[--p->nsteps]);
    if(p->nsteps == PAINT_UNDO_STEPS) history_trim(p);
    s->before = p->state;
    s->after = p->state = ++p->serial;
    p->steps[p->nsteps++] = s;
    p->cur = p->nsteps;
}

void step_abort(struct paint *p) {
    struct pstep *s = p->open;
    int i;
    p->open = NULL;
    if(!s) return;
    if(s->kind == PSTEP_TILES && s->w == p->w && s->h == p->h)
        for(i = 0; i < s->ntiles; i++) {
            tile_decode(&s->tiles[i], p->px, p->w, p->h);
            tile_mark(p, s->tiles[i].t);
        }
    step_free(p, s);
}

/**** undo and redo ****/

/* the tiles' pixels now are kept, the step's put back: 0, or -1 when those now could not be kept (the step's
   are back all the same) */
static int swap_tiles(struct paint *p, struct pstep *s) {
    struct ptile now;
    int i, keep = 1;
    for(i = 0; i < s->ntiles; i++) {
        if(keep && tile_encode(p, p->px, p->w, p->h, s->tiles[i].t, &now) < 0) keep = 0;
        tile_decode(&s->tiles[i], p->px, p->w, p->h);
        tile_mark(p, s->tiles[i].t);
        tile_drop(p, s, i);
        if(keep) tile_keep(p, s, i, &now);
    }
    return keep ? 0 : -1;
}

/* a canvas of the step's size from its tiles, and the step holds the canvas now: 0; -1 when the canvas now could
   not be kept; -2 when there is no memory for the canvas (nothing changed) */
static int swap_canvas(struct paint *p, struct pstep *s) {
    struct ptile *now;
    unsigned *px;
    int *rows = NULL, i, keep = 1, w = s->w, h = s->h, n = p->tiles_x * p->tiles_y;
    size_t table = (size_t)n * sizeof(struct ptile);
    px = (unsigned *)pm_alloc(p, canvas_bytes(w, h));
    if(px) rows = rows_new(p, h);
    if(!rows) {
        pm_free(p, px, canvas_bytes(w, h));
        return -2;
    }
    now = (struct ptile *)pm_alloc(p, table);
    if(!now) keep = 0;
    for(i = 0; keep && i < n; i++)
        if(tile_encode(p, p->px, p->w, p->h, i, &now[i]) < 0) {
            while(--i >= 0) pm_free(p, now[i].data, (size_t)now[i].n * sizeof(unsigned));
            pm_free(p, now, table);
            keep = 0;
        }
    for(i = 0; i < s->ntiles; i++) tile_decode(&s->tiles[i], px, w, h);
    step_empty(p, s);
    s->w = p->w;
    s->h = p->h;
    canvas_install(p, px, rows, w, h);
    if(!keep) return -1;
    s->tiles = now;
    s->ntiles = s->cap = n;
    s->bytes += table;
    p->undo_bytes += table;
    for(i = 0; i < n; i++) {
        s->bytes += (size_t)now[i].n * sizeof(unsigned);
        p->undo_bytes += (size_t)now[i].n * sizeof(unsigned);
    }
    return 0;
}

static int swap(struct paint *p, struct pstep *s) {
    if(s->kind == PSTEP_CANVAS) return swap_canvas(p, s);
    if(s->w != p->w || s->h != p->h) return -2;        /* never: a new size between them is undone first */
    return swap_tiles(p, s);
}

static void insert(struct paint *p, int at, struct pstep *s) {
    memmove(&p->steps[at + 1], &p->steps[at], (size_t)(p->nsteps - at) * sizeof(p->steps[0]));
    p->steps[at] = s;
    p->nsteps++;
}

static struct pstep *take(struct paint *p, int at) {
    struct pstep *s = p->steps[at];
    memmove(&p->steps[at], &p->steps[at + 1], (size_t)(p->nsteps - at - 1) * sizeof(p->steps[0]));
    p->nsteps--;
    return s;
}

/* Undo while text is typed drops it; while a selection floats, it is put back (or the paste dropped): tools.c */
int paint_undo(struct paint *p) {
    struct pstep *s;
    int r;
    if(p->drag) return 0;
    if(work_undo(p)) return 1;
    if(!p->cur) {
        say(p, "Nothing to undo");
        return 1;
    }
    s = take(p, --p->cur);                              /* out of the history while it swaps: trims cannot reach it */
    r = swap(p, s);
    if(r == -2) {
        insert(p, p->cur++, s);
        say(p, "Not enough memory to undo");
        return 1;
    }
    p->state = s->before;
    p->msg[0] = 0;
    if(r < 0) {                                         /* done, but what was undone cannot come back */
        step_free(p, s);
        while(p->nsteps > p->cur) step_free(p, p->steps[--p->nsteps]);
        say(p, "Undone; not enough memory to redo it");
    } else {
        insert(p, p->cur, s);
        while(p->undo_bytes > p->undo_max && history_trim(p))
            ;
    }
    selection_fit(p);
    return 1;
}

/* not while a change is under way (text typed, a selection floating): it would have to go first */
int paint_redo(struct paint *p) {
    struct pstep *s;
    int r;
    if(p->drag || p->text.on || p->sel.buf || p->open) return 0;
    if(p->cur == p->nsteps) {
        say(p, "Nothing to redo");
        return 1;
    }
    s = take(p, p->cur);
    r = swap(p, s);
    if(r == -2) {
        insert(p, p->cur, s);
        say(p, "Not enough memory to redo");
        return 1;
    }
    p->state = s->after;
    p->msg[0] = 0;
    if(r < 0) {                                         /* done, but it cannot be undone again: nor what was before */
        step_free(p, s);
        while(p->cur > 0) {
            step_free(p, p->steps[0]);
            take(p, 0);
            p->cur--;
        }
        say(p, "Redone; not enough memory to undo it again");
    } else {
        insert(p, p->cur++, s);
        while(p->undo_bytes > p->undo_max && history_trim(p))
            ;
    }
    selection_fit(p);
    return 1;
}

int paint_can_undo(const struct paint *p) {
    return p->cur > 0 || p->text.on || p->sel.buf || p->open;
}

int paint_can_redo(const struct paint *p) {
    return p->cur < p->nsteps && !p->text.on && !p->sel.buf && !p->open;
}

const char *paint_undo_what(const struct paint *p) {
    if(p->text.on) return "Text";
    if(p->sel.buf) return p->sel.lifted ? "Move" : "Paste";
    return p->cur > 0 ? p->steps[p->cur - 1]->what : "";
}

const char *paint_redo_what(const struct paint *p) {
    return p->cur < p->nsteps ? p->steps[p->cur]->what : "";
}

int paint_modified(const struct paint *p) {
    return p->state != p->clean || p->text.on || p->sel.buf || p->open;
}

void paint_saved(struct paint *p) {
    p->clean = p->state;
}
