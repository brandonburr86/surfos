/*
SurfOS window toolkit: a list
--------------------
File: gklist.c  Date: 11/05/26
--------------------
gk_list() (gk.h): lines of UTF-8 text with a scroll bar, one of them selected, drawn as the
desktop's choosers and System Monitor draw theirs (shell/guidialog.c, guimon.c): rows of 18 pixels
on the sheet colour inside a thin line, the selected row in the pick colour while the list has the
keys and in the band colour while it has not, the scroll bar at the right. Up, Down, Page Up, Page
Down, Home and End move the selection, a letter goes to the next line that starts with it, Enter
says GK_EV_ACTIVATE; a click selects, a double-click on the same row activates it; the wheel and
the scroll bar move the view, not the selection. A line's tabs start its columns, at the x the
program gave (gk_list_columns()), each cut where the next begins. The lines are the list's own
copies; how many there may be is the program's to bound.
*/

#include <stdlib.h>
#include <string.h>
#include "gkpriv.h"

#define LIST_COLUMNS 16

struct list {
    char **row;
    int n, cap;
    int top;                            /* the first row shown */
    int grab;                           /* the scroll bar's thumb held: from where (-1: not) */
    int ncols, colx[LIST_COLUMNS];
    int click_row;                      /* the row the last press selected, for a double-click */
};

static int shown(const struct gk_widget *w) {
    int n = (w->h - 2) / GK_ROW_H;
    return n > 0 ? n : 0;
}

static struct gk_bar bar_of(const struct gk_widget *w) {
    const struct list *l = w->k;
    struct gk_bar b;
    b.total = l->n;
    b.shown = shown(w);
    b.first = l->top;
    return b;
}

static void keep_top(struct gk_widget *w) {
    struct list *l = w->k;
    struct gk_bar b = bar_of(w);
    int mx = gk_bar_max(&b);
    if(l->top > mx) l->top = mx;
    if(l->top < 0) l->top = 0;
}

static void reveal(struct gk_widget *w, int i) {
    struct list *l = w->k;
    int n = shown(w);
    if(i < 0) return;
    if(i < l->top) l->top = i;
    if(n && i >= l->top + n) l->top = i - n + 1;
    keep_top(w);
}

static void list_measure(struct gk_widget *w) {
    w->base_w = 160;
    w->base_h = 4 * GK_ROW_H + 2;
}

static void list_arrange(struct gk_widget *w) {
    keep_top(w);                                        /* a taller list shows more at the end */
}

static void list_paint(struct gk_widget *w, struct gw *win) {
    const struct list *l = w->k;
    const struct gk_theme *t = &w->ui->theme;
    struct gk_bar b = bar_of(w);
    struct gw_view keep;
    int off = (w->flags & GKW_DISABLED) != 0, i, k, n = shown(w), py, cx, end, len, rx = w->x + w->w - 1 - GK_SCROLL_W;
    unsigned ink = off ? t->grey : t->ink;
    char cell[256];
    const char *s, *tab;
    gw_rect(win, w->x, w->y, w->w, w->h, t->line);
    gw_rect(win, w->x + 1, w->y + 1, w->w - 2, w->h - 2, off ? t->band : t->sheet);
    gw_get_view(win, &keep);
    gw_clip_to(win, w->x + 1, w->y + 1, rx - w->x - 1, w->h - 2);
    for(i = l->top; i < l->n && i <= l->top + n; i++) {
        py = w->y + 1 + (i - l->top) * GK_ROW_H;
        if(i == w->value) gw_rect(win, w->x + 1, py, rx - w->x - 1, GK_ROW_H, gk_lit(w) ? t->pick : t->band);
        s = l->row[i];
        if(!l->ncols) {                                 /* no columns: the line as it is */
            gw_text_cut(win, w->x + 6, py + 1, s, (rx - 4 - w->x - 6) / 8, ink);
            continue;
        }
        for(k = 0; k <= l->ncols; k++) {                /* the columns, at the tabs; the last takes the rest */
            tab = strchr(s, '\t');
            cx = w->x + 6 + (k ? l->colx[k - 1] : 0);
            end = k < l->ncols ? w->x + 6 + l->colx[k] - 8 : rx - 4;
            len = tab && k < l->ncols ? (int)(tab - s) : (int)strlen(s);
            if(len > (int)sizeof(cell) - 1) {           /* cut at a character's start */
                len = (int)sizeof(cell) - 1;
                while(len && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
            }
            memcpy(cell, s, (size_t)len);
            cell[len] = 0;
            gw_text_cut(win, cx, py + 1, cell, (end - cx) / 8, ink);
            if(!tab || k == l->ncols) break;
            s = tab + 1;
        }
    }
    gw_set_view(win, &keep);
    gk_bar_paint(win, t, rx, w->y + 1, w->h - 2, 0, &b);
}

static void choose(struct gk_widget *w, int i) {
    struct list *l = w->k;
    if(!l->n) return;
    if(i < 0) i = 0;
    if(i >= l->n) i = l->n - 1;
    reveal(w, i);
    gk_dirty(w);
    if(i == w->value) return;
    w->value = i;
    gk_post(w->ui, GK_EV_CHANGE, w, i);
}

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int list_key(struct gk_widget *w, unsigned key, unsigned mods) {
    struct list *l = w->k;
    int page = shown(w) > 1 ? shown(w) - 1 : 1, i, k;
    if(mods & (WIN_MOD_CTRL | WIN_MOD_ALT)) return 0;  /* Ctrl or Alt and a key: a shortcut's, the window's */
    switch(key) {
    case WIN_KEY_UP: choose(w, w->value < 0 ? 0 : w->value - 1); return 1;
    case WIN_KEY_DOWN: choose(w, w->value + 1); return 1;
    case WIN_KEY_PGUP: choose(w, w->value - page); return 1;
    case WIN_KEY_PGDN: choose(w, w->value < 0 ? page : w->value + page); return 1;
    case WIN_KEY_HOME: choose(w, 0); return 1;
    case WIN_KEY_END: choose(w, l->n - 1); return 1;
    case '\r': case '\n':
        if(w->value >= 0 && w->value < l->n) gk_post(w->ui, GK_EV_ACTIVATE, w, w->value);
        return 1;
    }
    if(key > ' ' && key < 127) {                        /* the next line that starts with the letter */
        for(k = 1; k <= l->n; k++) {
            i = (w->value + k + l->n) % l->n;
            if(lower((unsigned char)l->row[i][0]) == lower((int)key)) {
                choose(w, i);
                break;
            }
        }
        return 1;
    }
    return 0;
}

static void list_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    struct list *l = w->k;
    struct gk_bar b = bar_of(w);
    int rx = w->x + w->w - 1 - GK_SCROLL_W, row;
    (void)released;
    if(pressed & WIN_BUTTON_LEFT) {
        if(x >= rx) {                                   /* the scroll bar */
            if(gk_bar_press(w->h - 2, y - w->y - 1, &b, &l->grab)) {
                l->top = b.first;
                gk_dirty(w);
            }
            return;
        }
        l->grab = -1;
        row = l->top + (y - w->y - 1) / GK_ROW_H;
        if(y < w->y + 1 || row >= l->n) return;
        if(w->ui->clicks >= 2 && row == l->click_row) {
            gk_post(w->ui, GK_EV_ACTIVATE, w, row);
            return;
        }
        l->click_row = row;
        choose(w, row);
        return;
    }
    if((buttons & WIN_BUTTON_LEFT) && l->grab >= 0 && gk_bar_drag(w->h - 2, y - w->y - 1, &b, l->grab)) {
        l->top = b.first;
        gk_dirty(w);
    }
    if(!(buttons & WIN_BUTTON_LEFT)) l->grab = -1;
}

static int list_wheel(struct gk_widget *w, int x, int y, int rows) {
    struct list *l = w->k;
    int was = l->top;
    (void)x;
    (void)y;
    {                                                   /* (the re-review, N7) in 64 bits */
        long long to = (long long)l->top + rows;
        l->top = to > 0x7FFFFFFF ? 0x7FFFFFFF : to < 0 ? 0 : (int)to;
    }
    keep_top(w);
    if(l->top != was) gk_dirty(w);
    return 1;
}

static void list_set(struct gk_widget *w, int value) {
    struct list *l = w->k;
    if(value < -1 || value >= l->n) value = -1;
    if(value == w->value) return;
    w->value = value;
    reveal(w, value);
    gk_dirty(w);
}

static const char *list_text(const struct gk_widget *w) {
    const struct list *l = w->k;
    return w->value >= 0 && w->value < l->n ? l->row[w->value] : "";
}

static void list_destroy(struct gk_widget *w) {
    struct list *l = w->k;
    int i;
    for(i = 0; i < l->n; i++) free(l->row[i]);
    free(l->row);
}

static const struct gk_class list_class = {
    .takes_focus = 1, .measure = list_measure, .arrange = list_arrange, .paint = list_paint, .key = list_key,
    .pointer = list_pointer, .wheel = list_wheel, .set_value = list_set, .get_text = list_text, .destroy = list_destroy,
};

struct gk_widget *gk_list(struct gk_widget *parent, int id) {
    struct gk_widget *w = gk_make(parent, &list_class, sizeof(struct list));
    if(!w) return NULL;
    w->id = id;
    w->value = -1;
    ((struct list *)w->k)->grab = -1;
    ((struct list *)w->k)->click_row = -1;
    w->grow_x = w->grow_y = 1;
    return w;
}

static struct list *list_of(const struct gk_widget *w) {
    return w && w->cls == &list_class ? w->k : NULL;
}

int gk_list_insert(struct gk_widget *w, int at, const char *text) {
    struct list *l = list_of(w);
    char **grown, *copy;
    if(!l) return -1;
    if(at < 0 || at > l->n) at = l->n;
    if(l->n == l->cap) {
        if(l->cap > (1 << 24) || !(grown = realloc(l->row, (size_t)(l->cap ? l->cap * 2 : 16) * sizeof(*grown)))) {
            w->ui->failed = 1;
            return -1;
        }
        l->row = grown;
        l->cap = l->cap ? l->cap * 2 : 16;
    }
    if(!(copy = gk_strdup(w->ui, text))) return -1;
    memmove(l->row + at + 1, l->row + at, (size_t)(l->n - at) * sizeof(*l->row));
    l->row[at] = copy;
    l->n++;
    if(w->value >= at) w->value++;                      /* the selection stays on its line */
    gk_dirty(w);
    return at;
}

int gk_list_add(struct gk_widget *w, const char *text) {
    return gk_list_insert(w, -1, text);
}

int gk_list_set(struct gk_widget *w, int i, const char *text) {
    struct list *l = list_of(w);
    if(!l || i < 0 || i >= l->n) return -1;
    if(gk_set_str(w, &l->row[i], text)) gk_dirty(w);
    return 0;
}

void gk_list_remove(struct gk_widget *w, int i) {
    struct list *l = list_of(w);
    if(!l || i < 0 || i >= l->n) return;
    free(l->row[i]);
    memmove(l->row + i, l->row + i + 1, (size_t)(l->n - i - 1) * sizeof(*l->row));
    l->n--;
    if(w->value > i || w->value >= l->n) w->value--;    /* on its line still, or on the line before the last's */
    keep_top(w);
    gk_dirty(w);
}

void gk_list_clear(struct gk_widget *w) {
    struct list *l = list_of(w);
    int i;
    if(!l) return;
    for(i = 0; i < l->n; i++) free(l->row[i]);
    l->n = l->top = 0;
    w->value = -1;
    gk_dirty(w);
}

int gk_list_count(const struct gk_widget *w) {
    const struct list *l = list_of(w);
    return l ? l->n : 0;
}

const char *gk_list_text(const struct gk_widget *w, int i) {
    const struct list *l = list_of(w);
    return l && i >= 0 && i < l->n ? l->row[i] : NULL;
}

void gk_list_columns(struct gk_widget *w, int n, const int *x) {
    struct list *l = list_of(w);
    int i;
    if(!l) return;
    if(n > LIST_COLUMNS) n = LIST_COLUMNS;
    for(i = 0; i < n; i++) l->colx[i] = x[i];
    l->ncols = n > 0 ? n : 0;
    gk_dirty(w);
}

void gk_list_reveal(struct gk_widget *w, int i) {
    if(!list_of(w)) return;
    reveal(w, i);
    gk_dirty(w);
}
