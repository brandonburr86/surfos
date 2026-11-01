/*
SurfOS Undertow's window: a page laid out at a width
----------------------
File: layout.c  Date: 10/25/26
----------------------
layout.h's rules over doc.h's boxes, in two kinds of pass: measuring (the narrowest and widest a box can be:
its longest word, its longest line), which tables need for their columns, and placing, which makes the items.

A box's inline content is cut into pieces: words, the spaces between them (where a line may break, and which
a line's end drops), line breaks, and atoms (images, fields: boxes of their own). Pieces with nothing to break
between them (a word with a <sub> in it) go onto a line together. Each line's pieces are placed on a common
baseline, and the pieces of one run next to each other become one item.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "undertow.h"
#include "doc.h"
#include "font.h"
#include "layout.h"

#define INDENT      40                  /* lists, definitions, quotes */
#define SPACING     2                   /* between a table's cells, and around them */
#define RULE_GAP    8                   /* above and below <hr> */
#define COLS_MAX    64                  /* a table's columns kept; more are laid out in the last one */
#define ROWS_MAX    8192                /* a table's rows kept; more are not laid out */
#define BOX_PAD     4                   /* inside an image's or a field's box, each side */
#define MIN_WIDTH   16                  /* the narrowest a box's content gets, however deep it is */

/* A side of a box by the page's CSS, in pixels of a width w: margins 0 to 3, paddings 4 to 7, each top, right, bottom,
   left. def where it gives none. */
static int side(const struct wd_block *b, int i, int w, int def) {
    int v;
    if(!(b->given & (1 << i))) return def;
    v = i < 4 ? b->margin[i] : b->pad[i - 4];
    return v < 0 ? (int)((long)w * -v / 100) : v;
}

/**** pieces of inline content ****/

struct piece {
    int run;                            /* the run it comes from */
    unsigned int off, len;              /* WR_TEXT: its bytes in the document's text */
    int w, h;                           /* its width; an atom's height */
    unsigned char space;                /* a space a line may break at (and drops at its end) */
    unsigned char brk;                  /* a line break */
    unsigned char atom;                 /* an image or a field */
    unsigned char pre;                  /* pre text: kept as it is, broken only where it leaves the line */
};

struct pieces {                         /* the pieces of a box, one at a time */
    const struct wd_document *d;
    unsigned int r, end;                /* the run now, and past the box's last */
    unsigned int pos;                   /* the place in that run's text */
};

/* A run's size: what the page's CSS says, else what its element says of the base size, as a browser has them */
static int run_scale(const struct wd_run *u) {
    static const unsigned short ratio[] = { 256, 512, 384, 300, 256, 212, 171, 307, 213 };     /* WZ_*, in 1/256ths */
    return wf_size(u->px ? u->px : (wd_base * ratio[u->size] + 128) / 256);
}

static int field_width(const struct wd_run *u, int cells) {
    switch(u->field) {
    case WF_CHECKBOX: case WF_RADIO: return 14;
    case WF_BUTTON: return cells * WF_W + 4 * BOX_PAD;
    case WF_SELECT: return cells * WF_W + 2 * BOX_PAD + 16;
    }
    return (cells > 20 ? (cells > 48 ? 48 : cells) : 20) * WF_W + 2 * BOX_PAD;
}

static int field_height(const struct wd_run *u) {
    if(u->field == WF_CHECKBOX || u->field == WF_RADIO) return 14;
    return u->field == WF_AREA ? 2 * WF_H + 2 * BOX_PAD : WF_H + 2 * BOX_PAD;
}

/* An atom's size; images with a size given keep it (within max) */
static void atom_size(const struct wd_document *d, const struct wd_run *u, int max, int *w, int *h) {
    int cells = (int)wf_count(d->text + u->off, u->len);
    if(u->kind == WR_FIELD) {
        *w = field_width(u, cells);
        *h = field_height(u);
    } else if(u->w && u->h) {
        *w = u->w;
        *h = u->h;
        if(max > 0 && *w > max) {               /* narrower, at its shape */
            *h = (int)((long)*h * max / *w);
            *w = max;
            if(*h < 1) *h = 1;
        }
    } else {
        *w = cells * WF_W + 2 * BOX_PAD;
        *h = WF_H + 2 * BOX_PAD;
    }
}

static int next_piece(struct pieces *it, struct piece *p, int max) {
    const struct wd_document *d = it->d;
    for(;;) {
        const struct wd_run *u;
        const char *s;
        unsigned int n, k;
        if(it->r >= it->end) return 0;
        u = &d->runs[it->r];
        memset(p, 0, sizeof(*p));
        p->run = (int)it->r;
        if(u->kind != WR_TEXT) {
            it->r++;
            it->pos = 0;
            if(u->kind == WR_BR) p->brk = 1;
            else {
                p->atom = 1;
                atom_size(d, u, max, &p->w, &p->h);
            }
            return 1;
        }
        if(it->pos >= u->len) {
            it->r++;
            it->pos = 0;
            continue;
        }
        s = d->text + u->off + it->pos;
        n = u->len - it->pos;
        p->off = u->off + it->pos;
        if(u->style & WS_PRE) {                 /* the whole run, broken where it must be */
            p->pre = 1;
            p->len = n;
        } else if(s[0] == ' ' && !(u->style & WS_NOWRAP)) {
            p->space = 1;
            p->len = 1;
        } else {                                /* a word: to the next space (nobr: all of the run) */
            for(k = 0; k < n && (s[k] != ' ' || (u->style & WS_NOWRAP)); k++) ;
            p->len = k;
        }
        p->w = wf_width(s, p->len, run_scale(u), u->style);
        it->pos += p->len;
        return 1;
    }
}

/**** the layout's state ****/

struct lay {
    const struct wd_document *d;
    struct wl_layout *out;
    int failed;
    int y;                              /* where the next content goes */
    int margin;                         /* the margin waiting above it: the larger of those that met */
    int marker_on;                      /* a list item's marker waits for the item's first line */
    unsigned int marker_off, marker_len;
    int marker_right;                   /* its right end */
    /* the line being made, and the pieces with nothing to break between them that wait to go onto it */
    struct piece *line, *group;
    size_t nline, cline, ngroup, cgroup;
    int line_w, line_trail, group_w;
    int empty_scale;                    /* an empty line's (a break's) scale */
    int base;                           /* the page's base size */
    int gap;                            /* a paragraph's margin: its font's height */
};

static struct wl_item *add_item(struct lay *L, int kind, int x, int y, int w, int h) {
    struct wl_layout *o = L->out;
    struct wl_item *it;
    if(L->failed) return NULL;
    if(o->count >= WL_ITEMS_MAX) {
        o->truncated = 1;
        return NULL;
    }
    if(o->count == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 256;
        void *p = realloc(o->items, cap * sizeof(*o->items));
        if(!p) {
            L->failed = 1;
            return NULL;
        }
        o->items = p;
        o->cap = cap;
    }
    it = &o->items[o->count++];
    memset(it, 0, sizeof(*it));
    it->kind = (unsigned char)kind;
    it->scale = 1;
    it->x = x;
    it->y = y;
    it->w = w;
    it->h = h;
    it->color = WD_NONE;
    it->link = -1;
    if(x + w > o->width) o->width = x + w;
    if(y + h > o->height) o->height = y + h;
    return it;
}

static int push_piece(struct lay *L, struct piece **arr, size_t *n, size_t *cap, const struct piece *p) {
    if(*n == *cap) {
        size_t c = *cap ? *cap * 2 : 64;
        void *q = realloc(*arr, c * sizeof(**arr));
        if(!q) {
            L->failed = 1;
            return -1;
        }
        *arr = q;
        *cap = c;
    }
    (*arr)[(*n)++] = *p;
    return 0;
}

/* The space above the next content: the waiting margin, used up */
static int top(struct lay *L) {
    L->y += L->margin;
    L->margin = 0;
    return L->y;
}

static void wait_margin(struct lay *L, int m) { if(m > L->margin) L->margin = m; }

/**** lines ****/

/* An atom's height above the baseline: an image stands on it; a field's label is on it, as the text around */
static int atom_ascent(const struct wd_run *u, int h) {
    if(u->kind == WR_IMAGE) return h;
    if(u->field == WF_CHECKBOX || u->field == WF_RADIO) return h - 2;
    if(u->field == WF_AREA) return BOX_PAD + WF_ASCENT;
    return (h - WF_H) / 2 + WF_ASCENT;
}

static void piece_metrics(const struct lay *L, const struct piece *p, int *asc, int *desc) {
    const struct wd_run *u = &L->d->runs[p->run];
    int s = run_scale(u), h = wf_cell_h(s);
    if(p->atom) {
        *asc = atom_ascent(u, p->h);
        *desc = p->h - *asc;
        return;
    }
    *asc = wf_ascent(s) + (u->style & WS_SUP ? h / 4 : 0);
    *desc = h - wf_ascent(s) + (u->style & WS_SUB ? h / 4 : 0);
}

/* An item's marker, in the cells of the base size: its right end where it was to be */
static void add_marker(struct lay *L, int y) {
    int mw = (int)wf_count(L->out->marks + L->marker_off, L->marker_len) * wf_cell_w(L->base);
    struct wl_item *m = add_item(L, WI_MARK, L->marker_right - mw > 0 ? L->marker_right - mw : 0, y, mw, wf_cell_h(L->base));
    if(m) {
        m->scale = (unsigned char)L->base;
        m->off = L->marker_off;
        m->len = L->marker_len;
    }
}

/* The line made so far, at x within w, aligned (its spaces at the end left out); empty: a line of a break's
   height, when a break made it */
static void emit_line(struct lay *L, int x, int w, int align, int empty_ok) {
    int asc = 0, desc = 0, used = L->line_w - L->line_trail, cx, ytop, last;
    size_t k, n = L->nline;
    struct wl_item *prev = NULL;
    int fill = -1;
    while(n && L->line[n - 1].space) n--;       /* trailing spaces */
    if(!n && !empty_ok) {
        L->nline = 0;
        L->line_w = L->line_trail = 0;
        return;
    }
    for(k = 0; k < (n ? n : L->nline); k++) {
        int a, dd;
        piece_metrics(L, &L->line[k], &a, &dd);
        if(a > asc) asc = a;
        if(dd > desc) desc = dd;
    }
    if(!asc && !desc) {                         /* an empty line: the font's height at the break's scale */
        asc = wf_ascent(L->empty_scale);
        desc = wf_cell_h(L->empty_scale) - asc;
    }
    ytop = top(L);
    cx = x;
    if(align == WA_CENTER && used < w) cx = x + (w - used) / 2;
    else if(align == WA_RIGHT && used < w) cx = x + w - used;
    if(L->marker_on) {                          /* the item's marker, on its first line */
        add_marker(L, ytop + asc - wf_ascent(L->base));
        L->marker_on = 0;
    }
    last = -1;
    for(k = 0; k < n; k++) {
        const struct piece *p = &L->line[k];
        const struct wd_run *u = &L->d->runs[p->run];
        int a, dd;
        if(p->brk) continue;
        piece_metrics(L, p, &a, &dd);
        if(p->atom) {
            struct wl_item *it = add_item(L, u->kind == WR_IMAGE ? WI_IMAGE : WI_FIELD, cx, ytop + asc - a, p->w, p->h);
            if(it) {
                it->sub = u->field;
                it->checked = u->checked;
                it->style = u->style;
                it->color = u->color;
                it->link = u->link;
                it->off = u->off;
                it->len = u->len;
            }
            prev = NULL;
            fill = -1;
        } else if(prev && last == p->run && prev->off + prev->len == p->off) {
            prev->w += p->w;                    /* the same run, next to it */
            prev->len += p->len;
            if(fill >= 0) L->out->items[fill].w += p->w;
        } else {
            int s = run_scale(u), h = wf_cell_h(s);
            int y = ytop + asc - wf_ascent(s) - (u->style & WS_SUP ? h / 4 : 0) + (u->style & WS_SUB ? h / 4 : 0);
            struct wl_item *f = u->bg != WD_NONE ? add_item(L, WI_FILL, cx, y, p->w, h) : NULL;
            fill = -1;
            if(f) {                             /* the page's CSS: a background behind this text */
                f->color = u->bg;
                fill = (int)L->out->count - 1;
            }
            prev = add_item(L, WI_TEXT, cx, y, p->w, h);
            if(prev) {
                prev->scale = (unsigned char)s;
                prev->style = u->style;
                prev->color = u->color;
                prev->link = u->link;
                prev->off = p->off;
                prev->len = p->len;
            }
        }
        last = p->run;
        cx += p->w;
        if(prev && prev->x + prev->w > L->out->width) L->out->width = prev->x + prev->w;
    }
    L->y = ytop + asc + desc;
    if(L->y > L->out->height) L->out->height = L->y;
    L->nline = 0;
    L->line_w = L->line_trail = 0;
}

/* A piece onto the line (its width counted; a space as trailing until something follows it) */
static void on_line(struct lay *L, const struct piece *p) {
    if(p->space && !L->nline) return;           /* no line starts with a space */
    if(push_piece(L, &L->line, &L->nline, &L->cline, p)) return;
    L->line_w += p->w;
    if(p->space) L->line_trail += p->w;
    else L->line_trail = 0;
}

/* pre text, or a word too wide for any line: as much as fits on each line, the rest on the next */
static void split_onto(struct lay *L, const struct piece *p, int x, int w, int align) {
    const struct wd_run *u = &L->d->runs[p->run];
    int s = run_scale(u);
    struct piece q = *p;
    while(q.len && !L->failed) {
        size_t n;
        int used, room = w - (L->line_w - (p->pre ? 0 : L->line_trail));
        n = wf_fit(L->d->text + q.off, q.len, room, s, u->style, &used);
        if(!n && !L->nline) n = 1;              /* one at least */
        if(!n) {
            emit_line(L, x, w, align, 0);
            continue;
        }
        if(n < q.len && !L->nline) {            /* a whole character, however narrow the line */
            while(n < q.len && ((unsigned char)L->d->text[q.off + n] & 0xc0) == 0x80) n++;
        }
        {
            struct piece part = q;
            part.len = (unsigned int)n;
            part.w = wf_width(L->d->text + q.off, n, s, u->style);
            part.space = 0;
            if(L->line_trail && !p->pre) L->line_trail = 0;     /* the space before it stays: it is followed */
            push_piece(L, &L->line, &L->nline, &L->cline, &part);
            L->line_w += part.w;
        }
        q.off += (unsigned int)n;
        q.len -= (unsigned int)n;
        if(q.len) emit_line(L, x, w, align, 0);
    }
}

/* The waiting group onto the line: on this one when it fits, else on the next (split if no line holds it) */
static void place_group(struct lay *L, int x, int w, int align) {
    size_t k;
    if(!L->ngroup) return;
    if(L->nline && L->line_w - L->line_trail + L->group_w > w) emit_line(L, x, w, align, 0);
    if(L->group_w > w) {
        for(k = 0; k < L->ngroup; k++) {
            const struct piece *p = &L->group[k];
            if(L->nline && L->line_w - L->line_trail + p->w > w) emit_line(L, x, w, align, 0);
            if(p->w > w && !p->atom) split_onto(L, p, x, w, align);
            else on_line(L, p);
        }
    } else for(k = 0; k < L->ngroup; k++) on_line(L, &L->group[k]);
    L->ngroup = 0;
    L->group_w = 0;
}

static void lay_inline(struct lay *L, int box, int x, int w, int align) {
    const struct wd_block *b = &L->d->blocks[box];
    struct pieces it;
    struct piece p;
    it.d = L->d;
    it.r = b->run;
    it.end = b->run + b->nruns;
    it.pos = 0;
    L->nline = L->ngroup = 0;
    L->line_w = L->line_trail = L->group_w = 0;
    while(!L->failed && next_piece(&it, &p, w)) {
        if(p.pre) {
            place_group(L, x, w, align);
            split_onto(L, &p, x, w, align);
        } else if(p.brk) {
            place_group(L, x, w, align);
            L->empty_scale = run_scale(&L->d->runs[p.run]);
            emit_line(L, x, w, align, 1);
        } else if(p.space) {
            place_group(L, x, w, align);
            on_line(L, &p);
        } else if(p.atom) {
            place_group(L, x, w, align);
            if(push_piece(L, &L->group, &L->ngroup, &L->cgroup, &p)) break;
            L->group_w = p.w;
            place_group(L, x, w, align);
        } else {
            if(push_piece(L, &L->group, &L->ngroup, &L->cgroup, &p)) break;
            L->group_w += p.w;
        }
    }
    place_group(L, x, w, align);
    if(L->nline) emit_line(L, x, w, align, 0);
}

/**** measuring ****/

static void measure(struct lay *L, int box, int *min, int *max);

static void measure_inline(struct lay *L, int box, int *min, int *max) {
    const struct wd_block *b = &L->d->blocks[box];
    struct pieces it;
    struct piece p;
    int line = 0, trail = 0, group = 0;
    it.d = L->d;
    it.r = b->run;
    it.end = b->run + b->nruns;
    it.pos = 0;
    *min = *max = 0;
    while(next_piece(&it, &p, 0)) {
        if(p.brk || p.space || p.atom || p.pre) {
            if(group > *min) *min = group;
            group = 0;
        }
        if(p.brk) {
            if(line - trail > *max) *max = line - trail;
            line = trail = 0;
            continue;
        }
        if(p.space) {
            if(line) {
                line += p.w;
                trail += p.w;
            }
            continue;
        }
        line += p.w;
        trail = 0;
        if(p.atom || p.pre) {
            if(p.w > *min) *min = p.w;
        } else group += p.w;
    }
    if(group > *min) *min = group;
    if(line - trail > *max) *max = line - trail;
}

static int indent_of(const struct wd_block *b) {
    int q = b->flags & WBF_QUOTE ? INDENT : 0;
    return side(b, 3, 0, b->flags & WBF_INDENT ? INDENT : q) + side(b, 1, 0, q) + side(b, 5, 0, 0) +
           side(b, 7, 0, b->flags & WBF_LIST ? INDENT : 0);
}

/* a table's grid: its rows, and the cell that starts at each row and column (-1: none, or a span's) */
struct grid {
    int rows, cols;
    int *cell, *span, *row;
    int caption;
    int colmin[COLS_MAX], colmax[COLS_MAX], colw[COLS_MAX];
    int border, pad, cellpad;           /* the table's border, its cells' padding with their frames */
};

static void grid_free(struct grid *g) {
    free(g->cell);
    free(g->span);
    free(g->row);
}

static int make_grid(struct lay *L, int t, struct grid *g) {
    const struct wd_document *d = L->d;
    const struct wd_block *tb = &d->blocks[t];
    int r, c, x, n = 0;
    memset(g, 0, sizeof(*g));
    g->caption = -1;
    for(r = tb->first; r >= 0; r = d->blocks[r].next) {
        if(d->blocks[r].kind == WB_ROW && n < ROWS_MAX) n++;
        else if((d->blocks[r].flags & WBF_CAPTION) && g->caption < 0) g->caption = r;
    }
    g->rows = n;
    g->cell = malloc((size_t)(n ? n : 1) * COLS_MAX * sizeof(int));
    g->span = malloc((size_t)(n ? n : 1) * COLS_MAX * sizeof(int));
    g->row = malloc((size_t)(n ? n : 1) * sizeof(int));
    if(!g->cell || !g->span || !g->row) {
        grid_free(g);
        L->failed = 1;
        return -1;
    }
    for(x = 0; x < n * COLS_MAX; x++) g->cell[x] = -1;
    n = 0;
    for(r = tb->first; r >= 0 && n < g->rows; r = d->blocks[r].next) {
        if(d->blocks[r].kind != WB_ROW) continue;
        g->row[n] = r;
        x = 0;
        for(c = d->blocks[r].first; c >= 0; c = d->blocks[c].next) {
            int span = d->blocks[c].colspan;
            if(d->blocks[c].kind != WB_CELL) continue;
            if(x >= COLS_MAX) x = COLS_MAX - 1;  /* past the last column: in it (over what is there) */
            if(span > COLS_MAX - x) span = COLS_MAX - x;
            if(span < 1) span = 1;
            g->cell[n * COLS_MAX + x] = c;
            g->span[n * COLS_MAX + x] = span;
            x += span;
            if(x > g->cols) g->cols = x > COLS_MAX ? COLS_MAX : x;
        }
        n++;
    }
    g->border = tb->border;
    g->pad = tb->padding;
    g->cellpad = tb->padding + (tb->border ? 1 : 0);
    return 0;
}

static int extras(const struct grid *g) { return (g->cols + 1) * SPACING + 2 * g->border; }

/* The columns' narrowest and widest, from their cells (a span's over its columns) */
static void columns(struct lay *L, struct grid *g) {
    int r, c, k, pass;
    for(c = 0; c < COLS_MAX; c++) g->colmin[c] = g->colmax[c] = 0;
    for(pass = 0; pass < 2; pass++)             /* the cells of one column first, then the spans */
        for(r = 0; r < g->rows; r++)
            for(c = 0; c < g->cols; c++) {
                int cell = g->cell[r * COLS_MAX + c], span = g->span[r * COLS_MAX + c], mn, mx, smin = 0, smax = 0;
                const struct wd_block *cb;
                if(cell < 0 || (span > 1) != pass) continue;
                cb = &L->d->blocks[cell];
                measure(L, cell, &mn, &mx);
                mn += 2 * g->cellpad;
                mx += 2 * g->cellpad;
                if(cb->width > 0 && span == 1 && cb->width > mx) mx = cb->width;
                if(mx < mn) mx = mn;
                if(span == 1) {
                    if(mn > g->colmin[c]) g->colmin[c] = mn;
                    if(mx > g->colmax[c]) g->colmax[c] = mx;
                    continue;
                }
                for(k = c; k < c + span; k++) {
                    smin += g->colmin[k];
                    smax += g->colmax[k];
                }
                smin += (span - 1) * SPACING;
                smax += (span - 1) * SPACING;
                if(mn > smin) for(k = c; k < c + span; k++) g->colmin[k] += (mn - smin) / span + (k == c + span - 1 ? (mn - smin) % span : 0);
                if(mx > smax) for(k = c; k < c + span; k++) g->colmax[k] += (mx - smax) / span + (k == c + span - 1 ? (mx - smax) % span : 0);
            }
    for(c = 0; c < g->cols; c++) if(g->colmax[c] < g->colmin[c]) g->colmax[c] = g->colmin[c];
}

static void measure_table(struct lay *L, int t, int *min, int *max) {
    struct grid g;
    int c;
    *min = *max = 0;
    if(make_grid(L, t, &g)) return;
    columns(L, &g);
    for(c = 0; c < g.cols; c++) {
        *min += g.colmin[c];
        *max += g.colmax[c];
    }
    *min += extras(&g);
    *max += extras(&g);
    if(L->d->blocks[t].width > 0 && L->d->blocks[t].width > *max) *max = L->d->blocks[t].width;
    if(L->d->blocks[t].width > 0 && L->d->blocks[t].width > *min) *min = L->d->blocks[t].width;
    if(g.caption >= 0) {
        int mn, mx;
        measure(L, g.caption, &mn, &mx);
        if(mn > *min) *min = mn;
        if(mx > *max) *max = mx;
    }
    grid_free(&g);
}

static void measure(struct lay *L, int box, int *min, int *max) {
    const struct wd_block *b = &L->d->blocks[box];
    int c, mn, mx, ind = indent_of(b);
    *min = *max = 0;
    if(L->failed) return;
    if(b->kind == WB_HR) return;
    if(b->kind == WB_TABLE) {
        measure_table(L, box, min, max);
        return;
    }
    if(b->first < 0) {
        measure_inline(L, box, min, max);
        if(b->given) {
            *min += ind;
            *max += ind;
        }
        return;
    }
    for(c = b->first; c >= 0; c = L->d->blocks[c].next) {
        measure(L, c, &mn, &mx);
        if(mn > *min) *min = mn;
        if(mx > *max) *max = mx;
    }
    *min += ind;
    *max += ind;
}

/**** placing ****/

static void lay_box(struct lay *L, int box, int x, int w, int align);

static int add_mark(struct lay *L, const char *s, size_t n) {
    struct wl_layout *o = L->out;
    if(o->marks_len + n + 1 > o->marks_cap) {
        size_t cap = o->marks_cap ? o->marks_cap * 2 : 1024;
        void *p;
        while(cap < o->marks_len + n + 1) cap *= 2;
        p = realloc(o->marks, cap);
        if(!p) {
            L->failed = 1;
            return -1;
        }
        o->marks = p;
        o->marks_cap = cap;
    }
    memcpy(o->marks + o->marks_len, s, n);
    o->marks_len += n;
    o->marks[o->marks_len] = 0;
    return 0;
}

static size_t alpha_marker(char *s, long n, int upper) {    /* 1 a, 26 z, 27 aa, ... */
    char t[16];
    size_t k = 0, i;
    if(n < 1) return (size_t)snprintf(s, 16, "%ld.", n);
    while(n > 0 && k < sizeof(t)) {
        n--;
        t[k++] = (char)((upper ? 'A' : 'a') + n % 26);
        n /= 26;
    }
    for(i = 0; i < k; i++) s[i] = t[k - 1 - i];
    s[k++] = '.';
    s[k] = 0;
    return k;
}

static size_t roman_marker(char *s, long n, int upper) {
    static const struct { int v; const char *r; } table[] = {
        { 1000, "m" }, { 900, "cm" }, { 500, "d" }, { 400, "cd" }, { 100, "c" }, { 90, "xc" }, { 50, "l" },
        { 40, "xl" }, { 10, "x" }, { 9, "ix" }, { 5, "v" }, { 4, "iv" }, { 1, "i" }
    };
    size_t k = 0, i, j;
    if(n < 1 || n > 3999) return (size_t)snprintf(s, 16, "%ld.", n);
    for(i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        while(n >= table[i].v) {
            for(j = 0; table[i].r[j]; j++) s[k++] = (char)(upper ? table[i].r[j] - 32 : table[i].r[j]);
            n -= table[i].v;
        }
    s[k++] = '.';
    s[k] = 0;
    return k;
}

static void flush_marker(struct lay *L) {        /* a marker that waits, where the next line will be */
    add_marker(L, L->y + L->margin);
    L->marker_on = 0;
}

static void marker(struct lay *L, const struct wd_block *b, int right) {
    char s[24];
    size_t n;
    if(L->marker_on) flush_marker(L);
    switch(b->marker) {
    case WMK_DISC: n = (size_t)snprintf(s, sizeof(s), "\xe2\x80\xa2"); break;     /* U+2022 */
    case WMK_CIRCLE: n = (size_t)snprintf(s, sizeof(s), "\xe2\x97\x8b"); break;   /* U+25CB */
    case WMK_SQUARE: n = (size_t)snprintf(s, sizeof(s), "\xe2\x96\xa0"); break;   /* U+25A0 */
    case WMK_LOWER_ALPHA: case WMK_UPPER_ALPHA: n = alpha_marker(s, b->number, b->marker == WMK_UPPER_ALPHA); break;
    case WMK_LOWER_ROMAN: case WMK_UPPER_ROMAN: n = roman_marker(s, b->number, b->marker == WMK_UPPER_ROMAN); break;
    case WMK_DECIMAL: n = (size_t)snprintf(s, sizeof(s), "%ld.", b->number); break;
    default: return;
    }
    L->marker_off = (unsigned int)L->out->marks_len;
    if(add_mark(L, s, n)) return;
    L->marker_len = (unsigned int)n;
    L->marker_right = right;
    L->marker_on = 1;
}

/* The fills and frames of a table's cells are made before their content (painted under it) and sized after */
static void lay_table(struct lay *L, int t, int x, int w, int align) {
    const struct wd_document *d = L->d;
    const struct wd_block *tb = &d->blocks[t];
    struct grid g;
    long inner, summin = 0, summax = 0, used = 0;
    int c, r, tw, tx, ytop, y, table_fill = -1, k, explicit_w = 0;
    if(make_grid(L, t, &g)) return;
    columns(L, &g);
    for(c = 0; c < g.cols; c++) {
        summin += g.colmin[c];
        summax += g.colmax[c];
    }
    if(tb->width > 0) {
        tw = tb->width < w ? tb->width : w;
        explicit_w = 1;
    } else if(tb->width < 0) {
        tw = (int)((long)w * -tb->width / 100);
        explicit_w = 1;
    } else tw = (int)(summax + extras(&g) < w ? summax + extras(&g) : w);
    inner = tw - extras(&g);
    if(inner < 0) inner = 0;
    for(c = 0; c < g.cols; c++) {
        if(inner <= summin) g.colw[c] = g.colmin[c];
        else if(inner >= summax) g.colw[c] = g.colmax[c] + (explicit_w ? (int)(summax ? (inner - summax) * g.colmax[c] / summax
                                                                                     : (inner - summax) / g.cols) : 0);
        else g.colw[c] = g.colmin[c] + (int)((long)(g.colmax[c] - g.colmin[c]) * (inner - summin) / (summax - summin));
        used += g.colw[c];
    }
    if(g.cols && inner > summin && used < inner && (explicit_w || inner < summax)) g.colw[g.cols - 1] += (int)(inner - used);
    for(tw = extras(&g), c = 0; c < g.cols; c++) tw += g.colw[c];
    tx = x;
    if(tb->align == WA_CENTER && tw < w) tx = x + (w - tw) / 2;
    else if(tb->align == WA_RIGHT && tw < w) tx = x + w - tw;
    if(g.caption >= 0) lay_box(L, g.caption, tx, tw, WA_CENTER);
    ytop = top(L);
    if(tb->bg != WD_NONE) {
        struct wl_item *f = add_item(L, WI_FILL, tx, ytop, tw, 0);
        if(f) {
            f->color = tb->bg;
            table_fill = (int)L->out->count - 1;
        }
    }
    y = ytop + g.border + SPACING;
    for(r = 0; r < g.rows && !L->failed; r++) {
        const struct wd_block *rb = &d->blocks[g.row[r]];
        int row_fill = -1, rowh = 0, cx = tx + g.border + SPACING;
        int fills[COLS_MAX], frames[COLS_MAX];
        if(rb->bg != WD_NONE) {
            struct wl_item *f = add_item(L, WI_FILL, tx + g.border, y, tw - 2 * g.border, 0);
            if(f) {
                f->color = rb->bg;
                row_fill = (int)L->out->count - 1;
            }
        }
        for(c = 0; c < g.cols; c++) fills[c] = frames[c] = -1;     /* every column: a span skips the ones it covers */
        for(c = 0; c < g.cols; c++) {
            int cell = g.cell[r * COLS_MAX + c], span = g.span[r * COLS_MAX + c], cw = 0, h, salign;
            if(cell < 0) {
                cx += g.colw[c] + SPACING;
                continue;
            }
            for(k = c; k < c + span; k++) cw += g.colw[k];
            cw += (span - 1) * SPACING;
            if(d->blocks[cell].bg != WD_NONE) {
                struct wl_item *f = add_item(L, WI_FILL, cx, y, cw, 0);
                if(f) {
                    f->color = d->blocks[cell].bg;
                    fills[c] = (int)L->out->count - 1;
                }
            }
            if(g.border && add_item(L, WI_FRAME, cx, y, cw, 0)) frames[c] = (int)L->out->count - 1;
            L->y = y + g.cellpad;
            L->margin = 0;
            salign = d->blocks[cell].align != WA_INHERIT ? d->blocks[cell].align : rb->align != WA_INHERIT ? rb->align : align;
            {
                const struct wd_block *cb = &d->blocks[cell];
                int pl = side(cb, 7, cw, 0), pr = side(cb, 5, cw, 0);
                int ix = cx + g.cellpad + pl, iw = cw - 2 * g.cellpad - pl - pr, ch;
                if(iw < 1) iw = 1;
                L->y += side(cb, 4, cw, 0);
                if(cb->first >= 0) for(ch = cb->first; ch >= 0; ch = d->blocks[ch].next) lay_box(L, ch, ix, iw, salign);
                else if(cb->nruns) lay_inline(L, cell, ix, iw, salign);
                L->y += side(cb, 6, cw, 0);
            }
            h = L->y - y + g.cellpad;
            if(h > rowh) rowh = h;
            cx += cw + SPACING;
            c += span - 1;
        }
        if(rowh < 2 * g.cellpad + 1) rowh = 2 * g.cellpad + 1;
        if(row_fill >= 0) L->out->items[row_fill].h = rowh;
        for(c = 0; c < g.cols; c++) {
            if(fills[c] >= 0) L->out->items[fills[c]].h = rowh;
            if(frames[c] >= 0) L->out->items[frames[c]].h = rowh;
        }
        y += rowh + SPACING;
    }
    y += g.border;
    if(table_fill >= 0) L->out->items[table_fill].h = y - ytop;
    for(k = 0; k < g.border; k++) add_item(L, WI_FRAME, tx + k, ytop + k, tw - 2 * k, y - ytop - 2 * k);
    L->y = y;
    L->margin = 0;
    if(L->y > L->out->height) L->out->height = L->y;
    grid_free(&g);
}

static void lay_box(struct lay *L, int box, int x, int w, int align) {
    const struct wd_block *b = &L->d->blocks[box];
    int gap = b->flags & WBF_PARA ? L->gap : 0, quote = b->flags & WBF_QUOTE ? INDENT : 0;
    int ml = side(b, 3, w, b->flags & WBF_INDENT ? INDENT : quote), mr = side(b, 1, w, quote);
    int mt = side(b, 0, w, gap), mb = side(b, 2, w, gap);
    int pt = side(b, 4, w, 0), pb = side(b, 6, w, 0), pl = side(b, 7, w, 0), pr = side(b, 5, w, 0);
    int c, fill = -1;
    if(L->failed) return;
    if(!box) ml = mr = mt = mb = 0;             /* the root's margins are the page's: wl_layout()'s */
    if(b->align != WA_INHERIT && b->kind != WB_TABLE) align = b->align;
    if(b->kind == WB_HR) {
        int y;
        wait_margin(L, side(b, 0, w, RULE_GAP));
        y = top(L);
        ml = side(b, 3, w, 0);
        mr = side(b, 1, w, 0);
        add_item(L, WI_RULE, w - ml - mr > 0 ? x + ml : x, y, w - ml - mr > 0 ? w - ml - mr : w, 2);
        L->y = y + 2;
        wait_margin(L, side(b, 2, w, RULE_GAP));
        return;
    }
    wait_margin(L, mt);
    w -= mr;
    if(w - ml < MIN_WIDTH) ml = w > MIN_WIDTH ? w - MIN_WIDTH : 0;
    x += ml;
    w -= ml;
    if(w < MIN_WIDTH) w = MIN_WIDTH;
    if(b->kind == WB_TABLE) {
        lay_table(L, box, x, w, align);
        wait_margin(L, mb);
        return;
    }
    if(b->bg != WD_NONE) {                      /* painted under what is in it, and sized after it */
        struct wl_item *f = add_item(L, WI_FILL, x, pt ? top(L) : L->y + L->margin, w, 0);
        if(f) {
            f->color = b->bg;
            fill = (int)L->out->count - 1;
        }
    }
    if(pt) L->y = top(L) + pt;
    if(w - pl - pr < MIN_WIDTH) pl = pr = 0;
    x += pl;
    w -= pl + pr;
    if(b->kind == WB_ITEM) marker(L, b, x - 8);
    if(b->flags & WBF_LIST) {                   /* its items in its indentation */
        int li = (b->given & (16 << 3)) || w - INDENT < MIN_WIDTH ? 0 : INDENT;
        for(c = b->first; c >= 0; c = L->d->blocks[c].next) lay_box(L, c, x + li, w - li, align);
    } else if(b->first >= 0) {
        for(c = b->first; c >= 0; c = L->d->blocks[c].next) lay_box(L, c, x, w, align);
    } else if(b->nruns) lay_inline(L, box, x, w, align);
    if(L->marker_on && b->kind == WB_ITEM) {    /* an item that showed nothing: its marker alone */
        int y = top(L);
        flush_marker(L);
        L->y = y + wf_cell_h(L->base);
    }
    if(pb) L->y = top(L) + pb;
    if(fill >= 0) {                             /* the margins of what is in it meet outside it, as a browser has it */
        struct wl_item *f = &L->out->items[fill];
        if(!pt && L->out->count > (size_t)fill + 1) f->y = L->out->items[fill + 1].y;
        f->h = L->y > f->y ? L->y - f->y : 0;
    }
    wait_margin(L, mb);
}

int wl_layout(const struct wd_document *d, int width, struct wl_layout *out) {
    struct lay L;
    int ml, mr, bottom;
    if(width < 64) width = 64;
    if(width > WL_WIDTH_MAX) width = WL_WIDTH_MAX;
    memset(out, 0, sizeof(*out));
    memset(&L, 0, sizeof(L));
    L.d = d;
    L.out = out;
    out->width = width;
    L.base = L.empty_scale = wf_size(wd_base);
    L.gap = wf_cell_h(L.base);
    ml = mr = bottom = L.y = WL_MARGIN;
    if(d->block_count) {                        /* the page's margins: the body's, as the page's CSS gives them */
        const struct wd_block *r = &d->blocks[0];
        L.y = side(r, 0, width, WL_MARGIN);
        ml = side(r, 3, width, WL_MARGIN);
        mr = side(r, 1, width, WL_MARGIN);
        bottom = side(r, 2, width, WL_MARGIN);
        if(width - ml - mr < MIN_WIDTH) ml = mr = 0;
        lay_box(&L, 0, ml, width - ml - mr, WA_LEFT);
    }
    out->height = (L.y > out->height ? L.y : out->height) + bottom;
    free(L.line);
    free(L.group);
    if(L.failed) {
        wl_free(out);
        return -1;
    }
    return 0;
}

void wl_free(struct wl_layout *l) {
    if(!l) return;
    free(l->items);
    free(l->marks);
    memset(l, 0, sizeof(*l));
}

int wl_link_at(const struct wl_layout *l, int x, int y) {
    size_t i;
    for(i = 0; i < l->count; i++) {
        const struct wl_item *it = &l->items[i];
        if(it->link >= 0 && x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return it->link;
    }
    return -1;
}

int wl_offset_y(const struct wl_layout *l, size_t offset) {
    size_t i;
    for(i = 0; i < l->count; i++) {
        const struct wl_item *it = &l->items[i];
        if(it->kind != WI_MARK && it->kind != WI_FILL && it->kind != WI_FRAME && it->kind != WI_RULE &&
           it->off + it->len > offset) return it->y;
    }
    return 0;
}

/**** the dump ****/

char *wl_dump(const struct wl_layout *l, const struct wd_document *d) {
    static const char *const kinds[] = { "text", "fill", "frame", "rule", "mark", "field", "image" };
    static const char *const fields[] = { "text", "password", "button", "checkbox", "radio", "select", "area", "file" };
    size_t i, cap = 256 + l->count * 96, n = 0;
    char *s = malloc(cap);
    if(!s) return NULL;
    n += (size_t)snprintf(s + n, cap - n, "page %dx%d\n", l->width, l->height);
    for(i = 0; i < l->count; i++) {
        const struct wl_item *it = &l->items[i];
        const char *text = it->kind == WI_MARK ? l->marks + it->off : d->text + it->off;
        size_t k, len = it->kind == WI_FILL || it->kind == WI_FRAME || it->kind == WI_RULE ? 0 : it->len;
        if(cap - n < len * 4 + 128) {
            char *p;
            cap = cap * 2 + len * 4 + 128;
            p = realloc(s, cap);
            if(!p) {
                free(s);
                return NULL;
            }
            s = p;
        }
        n += (size_t)snprintf(s + n, cap - n, "%s", kinds[it->kind]);
        if(it->kind == WI_FIELD) n += (size_t)snprintf(s + n, cap - n, " %s", fields[it->sub]);
        n += (size_t)snprintf(s + n, cap - n, " %d,%d %dx%d", it->x, it->y, it->w, it->h);
        if(it->scale > WF_S16) n += (size_t)snprintf(s + n, cap - n, " s%d", wf_cell_h(it->scale));
        if(it->style & WS_BOLD) n += (size_t)snprintf(s + n, cap - n, " b");
        if(it->style & WS_ITALIC) n += (size_t)snprintf(s + n, cap - n, " i");
        if(it->style & WS_UNDER) n += (size_t)snprintf(s + n, cap - n, " u");
        if(it->color == WD_TEXT) n += (size_t)snprintf(s + n, cap - n, " color=text");
        else if(it->color != WD_NONE) n += (size_t)snprintf(s + n, cap - n, " color=%06x", it->color);
        if(it->link >= 0) n += (size_t)snprintf(s + n, cap - n, " link=%d", it->link);
        if(it->checked) n += (size_t)snprintf(s + n, cap - n, " checked");
        if(len) {
            n += (size_t)snprintf(s + n, cap - n, " \"");
            for(k = 0; k < len; k++) s[n++] = text[k] == '"' ? '\'' : text[k];
            s[n++] = '"';
            s[n] = 0;
        }
        n += (size_t)snprintf(s + n, cap - n, "\n");
    }
    return s;
}
