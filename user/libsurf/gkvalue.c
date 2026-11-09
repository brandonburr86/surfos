/*
SurfOS window toolkit: progress bars, sliders and palettes
--------------------
File: gkvalue.c Date: 11/05/26
--------------------
Stage 2 of the toolkit (gk.h; the desktop applications, track kit). A progress bar is the desktop's meter (shell/
guimeter.c's sys_bar(): a thin line round the sheet colour, filled in teal), with its text, if any, in the middle. A
slider chooses a whole number between two, by a step: the arrows a step, Page Up and Page Down ten, Home and End the
ends, a click or a drag on its track, the wheel. A palette is a grid of cells, each a colour or a small picture (a
paint program's tools and colours), one of them chosen: a click, or the arrows while it has the focus. Each says
GK_EV_CHANGE when a person changes it.
*/

#include <stdlib.h>
#include <string.h>
#include "gkpriv.h"

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/**** progress bars ****/

struct progress { int max; };

static void progress_measure(struct gk_widget *w) {
    w->base_w = 120;
    w->base_h = 18;
}

static void progress_paint(struct gk_widget *w, struct gw *win) {
    const struct progress *p = w->k;
    const struct gk_theme *t = &w->ui->theme;
    int fill = w->w > 2 && p->max > 0 ? (int)gk_div64((long long)clamp(w->value, 0, p->max) * (w->w - 2), p->max) : 0;
    const char *s = w->text ? w->text : "";
    int cells = (w->w - 8) / 8, n = gk_text_x(s, cells);
    gw_rect(win, w->x, w->y, w->w, w->h, t->line);
    gw_rect(win, w->x + 1, w->y + 1, w->w - 2, w->h - 2, t->sheet);
    if(fill > 0) gw_rect(win, w->x + 1, w->y + 1, fill, w->h - 2, (w->flags & GKW_DISABLED) ? t->muted : t->teal);
    if(*s) {                                            /* its text: paper over the fill, ink beyond it */
        struct gw_view keep;
        int x = w->x + (w->w - n * 8) / 2, y = w->y + (w->h - 16) / 2;
        gw_get_view(win, &keep);
        gw_clip_to(win, w->x + 1, w->y, fill, w->h);
        gw_text_cut(win, x, y, s, cells, t->on_teal);
        gw_set_view(win, &keep);
        gw_clip_to(win, w->x + 1 + fill, w->y, w->w, w->h);
        gw_text_cut(win, x, y, s, cells, t->ink);
        gw_set_view(win, &keep);
    }
}

static void progress_set(struct gk_widget *w, int value) {
    const struct progress *p = w->k;
    value = clamp(value, 0, p->max);
    if(value == w->value) return;
    w->value = value;
    gk_dirty(w);
}

static void progress_text(struct gk_widget *w, const char *text) {
    if(gk_set_str(w, &w->text, text)) gk_dirty(w);
}

static const struct gk_class progress_class = {
    .measure = progress_measure, .paint = progress_paint, .set_value = progress_set, .set_text = progress_text,
};

struct gk_widget *gk_progress(struct gk_widget *parent, int max) {
    struct gk_widget *w = gk_make(parent, &progress_class, sizeof(struct progress));
    if(!w) return NULL;
    ((struct progress *)w->k)->max = max > 0 ? max : 100;
    w->grow_x = 1;
    return w;
}

/**** sliders ****/

struct slider { int min, max, step, dragging; };

#define KNOB_W 10

static void slider_measure(struct gk_widget *w) {
    w->base_w = 120;
    w->base_h = GK_LINE_H;
}

/* the knob's left for the value, on a track from x + 1 to x + w - 1 */
static int knob_x(const struct gk_widget *w) {
    const struct slider *s = w->k;
    int run = w->w - KNOB_W - 2;
    if(s->max <= s->min || run <= 0) return w->x + 1;
    return w->x + 1 + (int)gk_div64(((long long)w->value - s->min) * run, (long long)s->max - s->min);
}

static void slider_paint(struct gk_widget *w, struct gw *win) {
    const struct gk_theme *t = &w->ui->theme;
    int off = (w->flags & GKW_DISABLED) != 0, kx = knob_x(w), mid = w->y + w->h / 2, lit = gk_lit(w);
    gw_rect(win, w->x + 1, mid - 2, w->w - 2, 4, t->line);
    if(!off) gw_rect(win, w->x + 1, mid - 2, kx - w->x, 4, t->teal);
    gw_rect(win, kx, w->y + 2, KNOB_W, w->h - 4, off ? t->band : lit ? t->teal : t->shell);
    gw_frame(win, kx, w->y + 2, KNOB_W, w->h - 4, off ? t->line : lit ? t->ink : t->muted);
}

static void slider_to(struct gk_widget *w, long long v) {
    const struct slider *s = w->k;
    int value;
    if(v < s->min) v = s->min;                          /* (64 bits: gk_div64(), as libsurf links no libgcc) */
    if(s->step > 1 && v < s->max) v = s->min + gk_div64(v - s->min + s->step / 2, s->step) * s->step;
    value = v > s->max ? s->max : (int)v;
    if(value == w->value) return;
    w->value = value;
    gk_dirty(w);
    gk_post(w->ui, GK_EV_CHANGE, w, value);
}

static int slider_key(struct gk_widget *w, unsigned key, unsigned mods) {
    const struct slider *s = w->k;
    if(mods & (WIN_MOD_CTRL | WIN_MOD_ALT)) return 0;  /* Ctrl or Alt and a key: a shortcut's */
    switch(key) {
    case WIN_KEY_LEFT: case WIN_KEY_DOWN: slider_to(w, (long long)w->value - s->step); return 1;
    case WIN_KEY_RIGHT: case WIN_KEY_UP: slider_to(w, (long long)w->value + s->step); return 1;
    case WIN_KEY_PGDN: slider_to(w, (long long)w->value - 10LL * s->step); return 1;
    case WIN_KEY_PGUP: slider_to(w, (long long)w->value + 10LL * s->step); return 1;
    case WIN_KEY_HOME: slider_to(w, s->min); return 1;
    case WIN_KEY_END: slider_to(w, s->max); return 1;
    }
    return 0;
}

/* the value under x: the knob's middle there */
static int value_at(const struct gk_widget *w, int x) {
    const struct slider *s = w->k;
    int run = w->w - KNOB_W - 2, pos = x - w->x - 1 - KNOB_W / 2;
    if(run <= 0 || s->max <= s->min) return s->min;
    pos = clamp(pos, 0, run);
    return (int)(s->min + gk_div64((long long)pos * ((long long)s->max - s->min) + run / 2, run));
}

static void slider_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    struct slider *s = w->k;
    (void)y;
    (void)released;
    if(pressed & WIN_BUTTON_LEFT) s->dragging = 1;
    if(!(buttons & WIN_BUTTON_LEFT)) {
        s->dragging = 0;
        return;
    }
    if(s->dragging) slider_to(w, value_at(w, x));
}

static int slider_wheel(struct gk_widget *w, int x, int y, int rows) {
    const struct slider *s = w->k;
    (void)x;
    (void)y;
    slider_to(w, (long long)w->value - (rows > 0 ? s->step : rows < 0 ? -(long long)s->step : 0));   /* away: more */
    return 1;
}

static void slider_set(struct gk_widget *w, int value) {
    const struct slider *s = w->k;
    value = clamp(value, s->min, s->max);
    if(value == w->value) return;
    w->value = value;
    gk_dirty(w);
}

static const struct gk_class slider_class = {
    .takes_focus = 1, .measure = slider_measure, .paint = slider_paint, .key = slider_key, .pointer = slider_pointer,
    .wheel = slider_wheel, .set_value = slider_set,
};

struct gk_widget *gk_slider(struct gk_widget *parent, int id, int min, int max, int step) {
    struct gk_widget *w = gk_make(parent, &slider_class, sizeof(struct slider));
    struct slider *s;
    if(!w) return NULL;
    s = w->k;
    s->min = min;
    s->max = max > min ? max : min;
    s->step = step > 0 ? step : 1;
    w->id = id;
    w->value = min;
    w->grow_x = 1;
    return w;
}

/**** palettes ****/

#define PALETTE_MAX 256
#define ICON_MAX (32 * 32)

struct cell { unsigned color; unsigned *icon; int iw, ih; };
struct palette { struct cell cell[PALETTE_MAX]; int n, cols, cw, ch; };

static void palette_measure(struct gk_widget *w) {
    const struct palette *p = w->k;
    int rows = p->n ? (p->n + p->cols - 1) / p->cols : 1;
    w->base_w = p->cols * (p->cw + 2) + 2;
    w->base_h = rows * (p->ch + 2) + 2;
}

static int cell_at(const struct gk_widget *w, int x, int y) {
    const struct palette *p = w->k;
    int c = (x - w->x - 1) / (p->cw + 2), r = (y - w->y - 1) / (p->ch + 2), i = r * p->cols + c;
    if(x < w->x + 1 || y < w->y + 1 || c >= p->cols || i >= p->n) return -1;
    return i;
}

static void palette_paint(struct gk_widget *w, struct gw *win) {
    const struct palette *p = w->k;
    const struct gk_theme *t = &w->ui->theme;
    const struct cell *c;
    int i, x, y, k, off = (w->flags & GKW_DISABLED) != 0;
    for(i = 0; i < p->n; i++) {
        c = &p->cell[i];
        x = w->x + 1 + i % p->cols * (p->cw + 2);
        y = w->y + 1 + i / p->cols * (p->ch + 2);
        gw_rect(win, x, y, p->cw, p->ch, c->icon ? (i == w->value ? t->foam : t->shell) : c->color);
        if(c->icon) {                                   /* a picture, its see-through pixels (the top byte set) left */
            int ix = x + (p->cw - c->iw) / 2, iy = y + (p->ch - c->ih) / 2;
            for(k = 0; k < c->iw * c->ih; k++)
                if(!(c->icon[k] & 0xFF000000u)) gw_rect(win, ix + k % c->iw, iy + k / c->iw, 1, 1, c->icon[k]);
        }
        if(i == w->value) {                             /* the one chosen: framed, in ink while it has the keys */
            gw_frame(win, x - 1, y - 1, p->cw + 2, p->ch + 2, gk_lit(w) ? t->ink : t->teal);
            gw_frame(win, x, y, p->cw, p->ch, off ? t->muted : t->on_teal);
        } else if(!c->icon) gw_frame(win, x, y, p->cw, p->ch, t->line);
    }
}

static void palette_choose(struct gk_widget *w, int i) {
    const struct palette *p = w->k;
    if(i < 0 || i >= p->n || i == w->value) return;
    w->value = i;
    gk_dirty(w);
    gk_post(w->ui, GK_EV_CHANGE, w, i);
}

static int palette_key(struct gk_widget *w, unsigned key, unsigned mods) {
    const struct palette *p = w->k;
    int at = w->value < 0 ? 0 : w->value;
    if(mods & (WIN_MOD_CTRL | WIN_MOD_ALT)) return 0;
    switch(key) {
    case WIN_KEY_LEFT: palette_choose(w, at - 1); return 1;
    case WIN_KEY_RIGHT: palette_choose(w, at + 1); return 1;
    case WIN_KEY_UP: palette_choose(w, at - p->cols); return 1;
    case WIN_KEY_DOWN: palette_choose(w, at + p->cols); return 1;
    case WIN_KEY_HOME: palette_choose(w, 0); return 1;
    case WIN_KEY_END: palette_choose(w, p->n - 1); return 1;
    }
    return 0;
}

static void palette_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    (void)buttons;
    (void)released;
    if(pressed & WIN_BUTTON_LEFT) palette_choose(w, cell_at(w, x, y));
}

static void palette_set(struct gk_widget *w, int value) {
    const struct palette *p = w->k;
    if(value < -1 || value >= p->n || value == w->value) return;
    w->value = value;
    gk_dirty(w);
}

static void palette_destroy(struct gk_widget *w) {
    struct palette *p = w->k;
    int i;
    for(i = 0; i < p->n; i++) free(p->cell[i].icon);
}

static const struct gk_class palette_class = {
    .takes_focus = 1, .measure = palette_measure, .paint = palette_paint, .key = palette_key,
    .pointer = palette_pointer, .set_value = palette_set, .destroy = palette_destroy,
};

struct gk_widget *gk_palette(struct gk_widget *parent, int id, int cols, int cell_w, int cell_h) {
    struct gk_widget *w = gk_make(parent, &palette_class, sizeof(struct palette));
    struct palette *p;
    if(!w) return NULL;
    p = w->k;
    p->cols = clamp(cols, 1, PALETTE_MAX);
    p->cw = clamp(cell_w, 4, 64);
    p->ch = clamp(cell_h, 4, 64);
    w->id = id;
    w->value = -1;
    return w;
}

static struct palette *palette_of(const struct gk_widget *w) {
    return w && w->cls == &palette_class ? w->k : NULL;
}

int gk_palette_color(struct gk_widget *w, unsigned color) {
    struct palette *p = palette_of(w);
    if(!p || p->n == PALETTE_MAX) return -1;
    p->cell[p->n].color = color & 0xFFFFFF;
    gk_relayout(w);
    gk_dirty(w);
    return p->n++;
}

int gk_palette_icon(struct gk_widget *w, const unsigned *pixels, int iw, int ih) {
    struct palette *p = palette_of(w);
    unsigned *copy;
    if(!p || p->n == PALETTE_MAX || iw < 1 || ih < 1 || iw * ih > ICON_MAX || iw > p->cw || ih > p->ch) return -1;
    if(!(copy = malloc((size_t)iw * (size_t)ih * sizeof(*copy)))) {
        w->ui->failed = 1;
        return -1;
    }
    memcpy(copy, pixels, (size_t)iw * (size_t)ih * sizeof(*copy));
    p->cell[p->n].icon = copy;
    p->cell[p->n].iw = iw;
    p->cell[p->n].ih = ih;
    gk_relayout(w);
    gk_dirty(w);
    return p->n++;
}

int gk_palette_set_color(struct gk_widget *w, int i, unsigned color) {
    struct palette *p = palette_of(w);
    if(!p || i < 0 || i >= p->n || p->cell[i].icon) return -1;
    p->cell[i].color = color & 0xFFFFFF;
    gk_dirty(w);
    return 0;
}

unsigned gk_palette_get_color(const struct gk_widget *w, int i) {
    const struct palette *p = palette_of(w);
    return p && i >= 0 && i < p->n ? p->cell[i].color : 0;
}
