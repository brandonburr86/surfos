/*
SurfOS window toolkit: the window, the tree, layout, painting and events
--------------------
File: gkcore.c  Date: 11/05/26
--------------------
The core of gk.h (the desktop applications, track kit: docs/superpowers/plans/2026-11-05-desktop-apps.md).

A window keeps a tree of widgets (gkpriv.h). Layout measures it from the leaves up (each widget's
natural size), then places it from the root down, the boxes sharing out what is left over by their
children's weights; a widget whose place changed is painted again where it was and where it is.

Painting is by damage: whatever changed adds its rectangle to a short list (two become one when
the box around them is not much more than they are, and a full list becomes the box around all of
it; what is hidden or on a page put away adds nothing). gk_flush() paints each rectangle, the
window's background first and then every widget that meets it, each clipped to itself and to the
rectangle (gui.h's view), and presents only those rectangles: a key typed into a field presents
that field.

11/05/26, track desk's calls (gkdesk.c): the menus are sent at a flush when they changed, a key no
widget took may be a menu item's shortcut, and the events of the menus, the choosers and the theme
come here first.

gk_next() turns the window's events into the program's: the pointer goes to the widget under it,
or to the one a press began in until every button is up; the keys go to the widget with the focus,
and Tab moves the focus; what no widget takes comes back as the window's. Timers run on uptime()'s
milliseconds, and descriptors the program watches are polled beside the window's. Events for the
program wait in a queue, so that a widget's handler never calls the program.
*/

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <surf.h>
#include "gkpriv.h"

extern const struct gk_class gk_area_class;

/* the ocean theme's colours, which a desktop that does not say its theme draws its windows in (shell/guidraw.c) */
static const struct gk_theme ocean = {
    .paper = GW_PAPER, .ink = GW_INK, .muted = GW_MUTED, .teal = GW_TEAL, .on_teal = GW_PAPER, .foam = GW_FOAM,
    .coral = GW_CORAL, .shell = GW_SHELL, .band = GW_BAND, .line = GW_LINE, .sheet = 0xFFFDF5, .pick = 0xBFE3DD,
    .frame = GW_INK, .shadow = GW_NAVY, .desktop = 0x245A67, .grey = 0xA3B4B3, .which = WIN_THEME_OCEAN,
};

static int max(int a, int b) { return a > b ? a : b; }
static int min(int a, int b) { return a < b ? a : b; }

unsigned gk_now(void) {
    return uptime();
}

/* n / d for n >= 0 and d > 0 in 64 bits, a bit at a time: libsurf links no libgcc, whose __divdi3 an i386 build of a
   64-bit division calls. Scroll bars over millions of lines need it a few times a frame */
long long gk_div64(long long n, long long d) {
    unsigned long long q = 0, r = 0, u = (unsigned long long)n, v = (unsigned long long)d;
    int i;
    if(n <= 0 || d <= 0) return 0;
    for(i = 63; i >= 0; i--) {
        r = r << 1 | (u >> i & 1);
        if(r >= v) {
            r -= v;
            q |= 1ULL << i;
        }
    }
    return (long long)q;
}

/* n bytes at p zeroed through a volatile pointer, so that no optimization drops it because they are freed or go out of
   scope next: what a password field, the clipboard or a box's answer held */
void gk_wipe(void *p, size_t n) {
    volatile unsigned char *b = (volatile unsigned char *)p;
    while(n--) *b++ = 0;
}

int gk_sat(long long v) {
    return v > GK_SIZE_MAX ? GK_SIZE_MAX : v < -GK_SIZE_MAX ? -GK_SIZE_MAX : (int)v;
}

int gk_clip_rect(int *x, int *y, int *w, int *h, int cw, int ch) {
    long long l = *x, t = *y, r = l + *w, b = t + *h;
    if(l < 0) l = 0;
    if(t < 0) t = 0;
    if(r > cw) r = cw;
    if(b > ch) b = ch;
    if(r <= l || b <= t) return 0;
    *x = (int)l;
    *y = (int)t;
    *w = (int)(r - l);
    *h = (int)(b - t);
    return 1;
}

/* Ctrl and a letter (surfos/win.h's keys): its control byte, 1 for Ctrl+A to 26, with WIN_MOD_CTRL (Ctrl+I is 9 with
   it, Tab 9 without, Ctrl+Tab WIN_KEY_TAB). A desktop before the modifiers sent the control byte alone, and Backspace
   as 8, Tab as 9, Enter as 13: without WIN_MOD_CTRL those are the keys, the other bytes Ctrl and their letters */
int gk_ctrl(unsigned key, unsigned mods) {
    if(key >= 1 && key <= 26)
        return (mods & WIN_MOD_CTRL) || (key != 8 && key != 9 && key != 10 && key != 13) ? (int)key - 1 + 'a' : 0;
    if(mods & WIN_MOD_CTRL) {
        if(key >= 'a' && key <= 'z') return (int)key;
        if(key >= 'A' && key <= 'Z') return (int)key - 'A' + 'a';
    }
    return 0;
}

/* a key that types a character: one of the keyboard's, without Ctrl or Alt */
static int typing(unsigned key, unsigned mods) {
    return ((key >= ' ' && key < 0x7F) || (key >= 0x80 && key < 0x100)) && !(mods & (WIN_MOD_CTRL | WIN_MOD_ALT));
}

int gk_shift(unsigned mods) {
    return (mods & WIN_MOD_SHIFT) != 0;
}

/**** the tree ****/

struct gk_widget *gk_make_in(struct gk *ui, const struct gk_class *cls, size_t extra) {
    struct gk_widget *w = calloc(1, sizeof(*w));
    if(w && extra && !(w->k = calloc(1, extra))) {
        free(w);
        w = NULL;
    }
    if(!w) {
        ui->failed = 1;
        return NULL;
    }
    w->cls = cls;
    w->ui = ui;
    w->align = -1;                                      /* the box's own way */
    return w;
}

struct gk_widget *gk_make(struct gk_widget *parent, const struct gk_class *cls, size_t extra) {
    struct gk_widget *w;
    if(!parent) return NULL;
    if(parent->ui->painting) return NULL;               /* an area's draw does not build */
    w = gk_make_in(parent->ui, cls, extra);
    if(!w) return NULL;
    w->parent = parent;
    w->prev = parent->last;
    if(parent->last) parent->last->next = w;
    else parent->child = w;
    parent->last = w;
    gk_relayout(w);
    return w;
}

/* whatever the window keeps of w lets go of it: the focus, the pointer, events not yet given */
static void forget(struct gk *ui, struct gk_widget *w) {
    int i;
    if(ui->focus == w) ui->focus = NULL;
    if(ui->capture == w) ui->capture = NULL;
    if(ui->under == w) ui->under = NULL;
    if(ui->saved == w) ui->saved = NULL;
    if(ui->last_area_focus == w) ui->last_area_focus = NULL;
    if(ui->click_w == w) ui->click_w = NULL;
    for(i = 0; i < ui->qcount; i++)
        if(ui->queue[(ui->qhead + i) % GK_QUEUE].w == w) ui->queue[(ui->qhead + i) % GK_QUEUE].w = NULL;
}

void gk_free_tree(struct gk_widget *w) {
    struct gk_widget *c, *n;
    if(!w) return;
    for(c = w->child; c; c = n) {
        n = c->next;
        gk_free_tree(c);
    }
    forget(w->ui, w);
    if(w->cls->destroy) w->cls->destroy(w);
    free(w->k);
    free(w->text);
    free(w);
}

void gk_remove(struct gk_widget *w) {
    struct gk_widget *p;
    if(!w || !w->parent || w->ui->painting) return;     /* the root stays, and a box goes by its answer */
    p = w->parent;
    gk_dirty(w);
    if(w->prev) w->prev->next = w->next;
    else p->child = w->next;
    if(w->next) w->next->prev = w->prev;
    else p->last = w->prev;
    gk_free_tree(w);
    gk_relayout(p);
}

char *gk_strdup(struct gk *ui, const char *s) {
    size_t n = strlen(s ? s : "");
    char *p = malloc(n + 1);
    if(!p) {
        ui->failed = 1;
        return NULL;
    }
    memcpy(p, s ? s : "", n + 1);
    return p;
}

/* *slot becomes a copy of text: 1 when it changed (0 when it was the same, or there was no memory: it stays) */
int gk_set_str(struct gk_widget *w, char **slot, const char *text) {
    char *p;
    if(!text) text = "";
    if(*slot && !strcmp(*slot, text)) return 0;
    if(!*slot && !*text) return 0;
    if(!(p = gk_strdup(w->ui, text))) return 0;
    free(*slot);
    *slot = p;
    return 1;
}

/**** damage ****/

static void join(struct gk_rect *a, const struct gk_rect *b) {
    int r = max(a->x + a->w, b->x + b->w), bt = max(a->y + a->h, b->y + b->h);
    a->x = min(a->x, b->x);
    a->y = min(a->y, b->y);
    a->w = r - a->x;
    a->h = bt - a->y;
}

/* two rectangles are painted as one when the box around them is not much more than they are (one inside the other,
   or side by side), so that merging never paints much that did not change */
static int worth_joining(const struct gk_rect *a, const struct gk_rect *b) {
    struct gk_rect u = *a;
    long long sa = (long long)a->w * a->h, sb = (long long)b->w * b->h;
    join(&u, b);
    return (long long)u.w * u.h <= sa + sb + ((sa < sb ? sa : sb) >> 2);
}

void gk_dirty_rect(struct gk *ui, int x, int y, int w, int h) {
    struct gk_rect r;
    int i, merged;
    if(!gk_clip_rect(&x, &y, &w, &h, ui->win->w, ui->win->h)) return;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    do {                                                /* rectangles that meet become one */
        merged = 0;
        for(i = 0; i < ui->ndamage; i++) {
            if(!worth_joining(&ui->damage[i], &r)) continue;
            join(&r, &ui->damage[i]);
            ui->damage[i] = ui->damage[--ui->ndamage];
            merged = 1;
            break;
        }
    } while(merged);
    if(ui->ndamage == GK_DAMAGE) {                      /* too many: the box around them all */
        for(i = 0; i < ui->ndamage; i++) join(&r, &ui->damage[i]);
        ui->ndamage = 0;
    }
    ui->damage[ui->ndamage++] = r;
}

/* what is hidden, or on a page put away, is not painted: a change there costs nothing until it shows */
int gk_shown_now(const struct gk_widget *w) {
    for(; w; w = w->parent) if(w->flags & (GKW_HIDDEN | GKW_AWAY)) return 0;
    return 1;
}

void gk_dirty(struct gk_widget *w) {
    if(w && w->w > 0 && w->h > 0 && gk_shown_now(w)) gk_dirty_rect(w->ui, w->x, w->y, w->w, w->h);
}

void gk_redraw(struct gk_widget *w) {
    gk_dirty(w);
}

void gk_dirty_lit(struct gk_widget *w) {
    struct gk_rect r;
    if(!w) return;
    if(!w->cls->lit_part) {
        gk_dirty(w);
        return;
    }
    w->cls->lit_part(w, &r);
    if(gk_shown_now(w)) gk_dirty_rect(w->ui, r.x, r.y, r.w, r.h);
}

void gk_relayout(struct gk_widget *w) {
    if(w) w->ui->layout = 1;
}

/**** state ****/

int gk_reached(const struct gk_widget *w) {
    for(; w; w = w->parent) if(w->flags & (GKW_HIDDEN | GKW_AWAY | GKW_DISABLED)) return 0;
    return 1;
}

int gk_lit(const struct gk_widget *w) {
    return w->ui->focus == w && w->ui->win->focused;
}

static int takes_focus(const struct gk_widget *w) {
    return w->cls->takes_focus || (w->cls == &gk_area_class && (w->opts & GK_FOCUSABLE));
}

/* the area that has the keys now (its focus and the window's, and no box over it), told when that changes */
static void sync_keys(struct gk *ui) {
    struct gk_widget *a = ui->focus;
    if(!a || a->cls != &gk_area_class || !ui->win->focused || ui->box) a = NULL;
    if(a == ui->last_area_focus) return;
    if(ui->last_area_focus) gk_post(ui, GK_EV_FOCUS, ui->last_area_focus, 0);
    if(a) gk_post(ui, GK_EV_FOCUS, a, 1);
    ui->last_area_focus = a;
}

void gk_set_focus(struct gk *ui, struct gk_widget *w) {
    struct gk_widget *old = ui->focus;
    if(w == old) return;
    ui->focus = w;
    if(old) {
        gk_dirty_lit(old);
        if(old->cls->focus) old->cls->focus(old, 0);
    }
    if(w) {
        gk_dirty_lit(w);
        if(w->cls->focus) w->cls->focus(w, 1);
    }
    sync_keys(ui);
}

void gk_focus(struct gk_widget *w) {
    if(!w || !takes_focus(w)) return;
    if(w->ui->box) {                                    /* behind a box: it has the keys once the box goes */
        w->ui->saved = w;
        return;
    }
    gk_set_focus(w->ui, w);
}

struct gk_widget *gk_focused(const struct gk *ui) {
    return ui->focus;
}

/* the widgets that take the focus, in the tree's order, around the one that has it */
struct walk { const struct gk_widget *cur; struct gk_widget *first, *last, *before, *after; int seen; };

static void walk(struct gk_widget *w, struct walk *k) {
    struct gk_widget *c;
    if(w->flags & (GKW_HIDDEN | GKW_AWAY | GKW_DISABLED)) return;
    if(takes_focus(w)) {
        if(!k->first) k->first = w;
        if(w == k->cur) k->seen = 1;
        else if(!k->seen) k->before = w;
        else if(!k->after) k->after = w;
        k->last = w;
    }
    for(c = w->child; c; c = c->next) walk(c, k);
}

void gk_move_focus(struct gk *ui, int back) {
    struct walk k;
    memset(&k, 0, sizeof(k));
    k.cur = ui->focus;
    walk(ui->box ? ui->box : ui->root, &k);
    if(!k.seen) gk_set_focus(ui, back ? k.last : k.first);
    else gk_set_focus(ui, back ? (k.before ? k.before : k.last) : (k.after ? k.after : k.first));
}

void gk_show(struct gk_widget *w, int on) {
    if(!w || !(w->flags & GKW_HIDDEN) == !!on) return;
    if(!on) {
        gk_dirty(w);
        w->flags |= GKW_HIDDEN;
    } else w->flags &= ~GKW_HIDDEN;
    gk_relayout(w);
}

void gk_enable(struct gk_widget *w, int on) {
    if(!w || !(w->flags & GKW_DISABLED) == !!on) return;
    if(on) w->flags &= ~GKW_DISABLED;
    else w->flags |= GKW_DISABLED;
    gk_dirty(w);
}

int gk_shown(const struct gk_widget *w) { return w && !(w->flags & GKW_HIDDEN); }
int gk_enabled(const struct gk_widget *w) { return w && !(w->flags & GKW_DISABLED); }
int gk_id(const struct gk_widget *w) { return w ? w->id : 0; }
void gk_set_data(struct gk_widget *w, void *data) { if(w) w->data = data; }
void *gk_data(const struct gk_widget *w) { return w ? w->data : NULL; }

void gk_bounds(const struct gk_widget *w, int *x, int *y, int *width, int *height) {
    *x = w ? w->x : 0;
    *y = w ? w->y : 0;
    *width = w ? w->w : 0;
    *height = w ? w->h : 0;
}

void gk_size(struct gk_widget *w, int min_w, int min_h) {
    if(!w) return;
    w->min_w = min_w > 0 ? gk_sat(min_w) : 0;
    w->min_h = min_h > 0 ? gk_sat(min_h) : 0;
    gk_relayout(w);
}

void gk_grow(struct gk_widget *w, int gx, int gy) {
    if(!w) return;
    w->grow_x = gx > 0 ? (gx < 1024 ? gx : 1024) : 0;  /* (weights summed in 64 bits; a share is what counts) */
    w->grow_y = gy > 0 ? (gy < 1024 ? gy : 1024) : 0;
    gk_relayout(w);
}

void gk_align(struct gk_widget *w, int how) {
    if(!w) return;
    w->align = how >= GK_START && how <= GK_FILL ? how : -1;
    gk_relayout(w);
}

const char *gk_text(const struct gk_widget *w) {
    if(!w) return "";
    if(w->cls->get_text) return w->cls->get_text(w);
    return w->text ? w->text : "";
}

/* (the review, F7) 0 when the widget holds the text as given, 1 when it holds it changed to fit (cut, a control
   character a space), -1 when it could not take it (no memory, or while painting) */
int gk_set_text(struct gk_widget *w, const char *text) {
    int had, r;
    if(!w || w->ui->painting) return -1;
    if(!w->cls->set_text && w->cls->get_text) return -1;   /* a tab strip's, a list's: its tab's or row's (the
                                                               re-review, N5: gk_tab_label(), gk_list_set()) */
    if(!text) text = "";
    had = w->ui->failed;                                /* (N5: this call's failure, not an earlier one's) */
    w->ui->failed = 0;
    if(w->cls->set_text) w->cls->set_text(w, text);
    else if(gk_set_str(w, &w->text, text)) {
        gk_relayout(w);
        gk_dirty(w);
    }
    r = !strcmp(gk_text(w), text) ? 0 : w->ui->failed ? -1 : 1;
    w->ui->failed |= had;
    return r;
}

int gk_value(const struct gk_widget *w) {
    return w ? w->value : 0;
}

void gk_set_value(struct gk_widget *w, int value) {
    if(!w || w->ui->painting) return;
    if(w->cls->set_value) w->cls->set_value(w, value);
    else if(w->value != value) {
        w->value = value;
        gk_dirty(w);
    }
}

/**** boxes ****/

static void box_measure(struct gk_widget *w) {
    struct gk_widget *c;
    long long n = 0, along = 0, across = 0;
    for(c = w->child; c; c = c->next) {
        if(c->flags & GKW_HIDDEN) continue;
        along += w->across ? c->nat_w : c->nat_h;
        across = max(across, w->across ? c->nat_h : c->nat_w);
        n++;
    }
    if(n > 1) along += (n - 1) * w->gap;
    w->base_w = gk_sat((w->across ? along : across) + 2LL * w->pad);
    w->base_h = gk_sat((w->across ? across : along) + 2LL * w->pad);
}

/* how a child sits across its box: as it asked, or filling the box when it grows that way, or the box's way (an
   hbox's children in the middle of its height, a vbox's at its left) */
static int across_how(const struct gk_widget *box, const struct gk_widget *c) {
    if(c->align >= 0) return c->align;
    if(box->across ? c->grow_y : c->grow_x) return GK_FILL;
    return box->across ? GK_CENTER : GK_START;
}

static void box_arrange(struct gk_widget *w) {
    struct gk_widget *c;
    int across = w->across, ix = w->x + w->pad, iy = w->y + w->pad;
    int iw = max(0, w->w - 2 * w->pad), ih = max(0, w->h - 2 * w->pad);
    int room = across ? iw : ih, cross = across ? ih : iw;
    int n = 0, extra, pos, size, share, csize, cpos, how, nat, g;
    long long total = 0, weights = 0, shrinkable = 0;
    int growers = 0, given = 0, taken = 0, seen = 0;
    for(c = w->child; c; c = c->next) {
        if(c->flags & GKW_HIDDEN) continue;
        nat = across ? c->nat_w : c->nat_h;
        g = across ? c->grow_x : c->grow_y;
        total += nat;
        weights += g;
        if(g) {
            shrinkable += nat;
            growers++;
        }
        n++;
    }
    if(n > 1) total += (long long)(n - 1) * w->gap;
    extra = gk_sat(room - total);
    pos = across ? ix : iy;
    for(c = w->child; c; c = c->next) {
        if(c->flags & GKW_HIDDEN) continue;
        nat = across ? c->nat_w : c->nat_h;
        g = across ? c->grow_x : c->grow_y;
        size = nat;
        if(g) {
            seen++;
            if(extra > 0 && weights) {                  /* what is left over, by weight; the last grower the rest */
                share = seen == growers ? extra - given : (int)gk_div64((long long)extra * g, weights);
                given += share;
                size += share;
            } else if(extra < 0 && shrinkable) {        /* too little room: the growers give way first */
                share = seen == growers ? -extra - taken : (int)gk_div64((long long)-extra * nat, shrinkable);
                if(share > nat) share = nat;
                taken += share;
                size -= share;
            }
        }
        how = across_how(w, c);
        csize = how == GK_FILL ? cross : min(across ? c->nat_h : c->nat_w, cross);
        cpos = how == GK_CENTER ? (cross - csize) / 2 : how == GK_END ? cross - csize : 0;
        if(across) gk_lay(c, pos, iy + cpos, size, csize);
        else gk_lay(c, ix + cpos, pos, csize, size);
        pos = gk_sat((long long)pos + size + w->gap);
    }
}

static const struct gk_class box_class = {
    .measure = box_measure, .arrange = box_arrange,
};

static struct gk_widget *box(struct gk_widget *parent, int gap, int across) {
    struct gk_widget *w = gk_make(parent, &box_class, 0);
    if(!w) return NULL;
    w->gap = gap > 0 ? gap : 0;
    w->across = across;
    w->align = GK_FILL;                                 /* a box in a box fills it across */
    return w;
}

struct gk_widget *gk_vbox(struct gk_widget *parent, int gap) { return box(parent, gap, 0); }
struct gk_widget *gk_hbox(struct gk_widget *parent, int gap) { return box(parent, gap, 1); }

void gk_pad(struct gk_widget *w, int pad) {
    if(!w) return;
    w->pad = pad > 0 ? gk_sat(pad) : 0;
    gk_relayout(w);
}

void gk_gap(struct gk_widget *w, int gap) {
    if(!w) return;
    w->gap = gap > 0 ? gk_sat(gap) : 0;
    gk_relayout(w);
}

/* pages: every child in the whole of it, one shown (value) */
static void pages_measure(struct gk_widget *w) {
    struct gk_widget *c;
    for(c = w->child; c; c = c->next) {
        if(c->flags & GKW_HIDDEN) continue;
        w->base_w = max(w->base_w, c->nat_w);
        w->base_h = max(w->base_h, c->nat_h);
    }
    w->base_w += 2 * w->pad;
    w->base_h += 2 * w->pad;
}

static void pages_mark(struct gk_widget *w) {
    struct gk_widget *c;
    int i = 0;
    for(c = w->child; c; c = c->next, i++) {
        if(i == w->value) c->flags &= ~GKW_AWAY;
        else c->flags |= GKW_AWAY;
    }
}

static void pages_arrange(struct gk_widget *w) {
    struct gk_widget *c;
    pages_mark(w);
    for(c = w->child; c; c = c->next)
        if(!(c->flags & GKW_HIDDEN)) gk_lay(c, w->x + w->pad, w->y + w->pad, w->w - 2 * w->pad, w->h - 2 * w->pad);
}

static void pages_set(struct gk_widget *w, int value) {
    if(value == w->value) return;
    w->value = value;
    pages_mark(w);
    gk_dirty(w);
}

static const struct gk_class pages_class = {
    .measure = pages_measure, .arrange = pages_arrange, .set_value = pages_set,
};

struct gk_widget *gk_pages(struct gk_widget *parent) {
    struct gk_widget *w = gk_make(parent, &pages_class, 0);
    if(!w) return NULL;
    w->grow_x = w->grow_y = 1;
    return w;
}

/**** layout ****/

void gk_lay(struct gk_widget *w, int x, int y, int width, int height) {
    w->x = x;
    w->y = y;
    w->w = width > 0 ? width : 0;
    w->h = height > 0 ? height : 0;
    if(w->cls->arrange) w->cls->arrange(w);
}

static void measure(struct gk_widget *w) {
    struct gk_widget *c;
    for(c = w->child; c; c = c->next) if(!(c->flags & GKW_HIDDEN)) measure(c);
    w->base_w = w->base_h = 0;
    if(w->cls->measure) w->cls->measure(w);
    w->nat_w = gk_sat(max(w->base_w, w->min_w));
    w->nat_h = gk_sat(max(w->base_h, w->min_h));
}

static void keep_places(struct gk_widget *w) {
    struct gk_widget *c;
    w->ox = w->x;
    w->oy = w->y;
    w->ow = w->w;
    w->oh = w->h;
    for(c = w->child; c; c = c->next) keep_places(c);
}

static void zero_hidden(struct gk_widget *w, int hidden) {
    struct gk_widget *c;
    hidden |= w->flags & GKW_HIDDEN;
    if(hidden) w->x = w->y = w->w = w->h = 0;
    for(c = w->child; c; c = c->next) zero_hidden(c, hidden);
}

/* what moved is painted again where it was and where it is (what is inside a widget that moved, with it) */
static void moved(struct gk *ui, struct gk_widget *w) {
    struct gk_widget *c;
    if(w->x != w->ox || w->y != w->oy || w->w != w->ow || w->h != w->oh) {
        gk_dirty_rect(ui, w->ox, w->oy, w->ow, w->oh);
        gk_dirty_rect(ui, w->x, w->y, w->w, w->h);
        return;
    }
    for(c = w->child; c; c = c->next) moved(ui, c);
}

static void layout_all(struct gk *ui) {
    ui->layout = 0;
    keep_places(ui->root);
    measure(ui->root);
    gk_lay(ui->root, 0, 0, ui->win->w, ui->win->h);
    zero_hidden(ui->root, 0);
    moved(ui, ui->root);
    if(ui->box) {
        keep_places(ui->box);
        measure(ui->box);
        gk_box_layout(ui);
        zero_hidden(ui->box, 0);
        moved(ui, ui->box);
    }
}

/**** painting ****/

static int clip_to(const struct gk_widget *w, const struct gk_rect *r, struct gk_rect *out) {
    int x0 = max(w->x, r->x), y0 = max(w->y, r->y);
    int x1 = min(w->x + w->w, r->x + r->w), y1 = min(w->y + w->h, r->y + r->h);
    if(x1 <= x0 || y1 <= y0) return 0;
    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
    return 1;
}

void gk_paint_tree(struct gk *ui, struct gk_widget *w, const struct gk_rect *r) {
    struct gk_widget *c;
    struct gk_rect in;
    struct gw_view v;
    if(w->flags & (GKW_HIDDEN | GKW_AWAY)) return;
    if(!clip_to(w, r, &in)) return;
    if(w->cls->paint) {
        v.ox = v.oy = 0;
        v.l = in.x;
        v.t = in.y;
        v.r = in.x + in.w;
        v.b = in.y + in.h;
        gw_set_view(ui->win, &v);
        w->cls->paint(w, ui->win);
    }
    for(c = w->child; c; c = c->next) gk_paint_tree(ui, c, &in);
}

/* the focus left on a widget that can no longer have it (hidden, disabled, on a page put away) goes */
static void focus_check(struct gk *ui) {
    if(ui->focus && !gk_reached(ui->focus)) gk_set_focus(ui, NULL);
    sync_keys(ui);
}

int gk_flush(struct gk *ui) {
    struct gk_rect r[GK_DAMAGE];
    int i, n, rc = 0;
    if(ui->win->gone) {
        errno = EPIPE;
        return -1;
    }
    if(ui->layout) layout_all(ui);
    gk_menus_send(ui);
    focus_check(ui);
    if(ui->had_focus != ui->win->focused) {             /* the window's focus came or went: the focus is drawn so */
        ui->had_focus = ui->win->focused;
        if(ui->focus) gk_dirty_lit(ui->focus);
        sync_keys(ui);
    }
    n = ui->ndamage;
    if(!n) return 0;
    memcpy(r, ui->damage, (size_t)n * sizeof(r[0]));
    ui->ndamage = 0;
    ui->painting = 1;
    for(i = 0; i < n; i++) {
        gw_view_reset(ui->win);
        gw_rect(ui->win, r[i].x, r[i].y, r[i].w, r[i].h, ui->theme.paper);
        gk_paint_tree(ui, ui->root, &r[i]);
        if(ui->box) gk_paint_tree(ui, ui->box, &r[i]);
    }
    gw_view_reset(ui->win);
    ui->painting = 0;
    for(i = 0; i < n; i++)
        if(gw_present(ui->win, r[i].x, r[i].y, r[i].w, r[i].h) < 0 && errno == EPIPE) {
            rc = -1;
            break;
        }
    return rc;
}

/**** events for the program ****/

struct gk_event *gk_post_ev(struct gk *ui, int type, struct gk_widget *w, int value) {
    struct gk_event *e;
    struct gk_widget *top;
    int at;
    for(top = w; top && top->parent; top = top->parent) ;
    if(top && top == ui->box) return NULL;              /* (the review, F12) a box's line (a password being typed) is the
                                                           box's own: the program hears the box's answer, nothing else */
    if(ui->qcount == GK_QUEUE) return NULL;             /* a whole queue unread: no window event makes so many */
    at = (ui->qhead + ui->qcount++) % GK_QUEUE;
    e = &ui->queue[at];
    memset(e, 0, sizeof(*e));
    ui->qtext[at] = NULL;
    e->text = "";
    e->type = type;
    e->w = w;
    e->id = w ? w->id : 0;
    e->value = value;
    return e;
}

void gk_post(struct gk *ui, int type, struct gk_widget *w, int value) {
    gk_post_ev(ui, type, w, value);
}

void gk_post_text(struct gk *ui, int type, int id, int value, const char *text, int len) {
    struct gk_event *e = gk_post_ev(ui, type, NULL, value);
    char *p;
    int at;
    if(!e) return;
    e->id = id;
    if(!text) return;
    if(len < 0) len = (int)strlen(text);
    at = (ui->qhead + ui->qcount - 1) % GK_QUEUE;
    if(!(p = malloc((size_t)len + 1))) {
        ui->failed = 1;
        e->text = "";
        if(type == GK_EV_CHOSEN) e->value = 0;          /* (the review, F16) no path to give: not chosen, never an empty
                                                           one chosen; a box's answer is a Cancel */
        else if(type == GK_EV_ANSWER) e->value = -1;
        return;
    }
    memcpy(p, text, (size_t)len);
    p[len] = 0;
    ui->qtext[at] = p;
    e->text = p;
}

/* the text given with the last event goes (wiped: it may be a password a box took) */
static void drop_given(struct gk *ui) {
    if(!ui->given) return;
    gk_wipe(ui->given, strlen(ui->given));
    free(ui->given);
    ui->given = NULL;
}

/* the events a person's own key, press or choice makes (not a timer, a descriptor, the view moved, a new size, the close
   button, the desktop's answers): only in answer to them is a chooser asked for (gkdesk.c) */
static int by_hand(const struct gk_event *ev) {
    switch(ev->type) {
    case GK_EV_KEY: case GK_EV_CLICK: case GK_EV_CHANGE: case GK_EV_ACTIVATE: case GK_EV_MENU: case GK_EV_TAB_CLOSE:
    case GK_EV_ANSWER:
        return 1;
    case GK_EV_POINTER:
        return ev->value != 0;
    }
    return 0;
}

static int take(struct gk *ui, struct gk_event *ev) {
    if(!ui->qcount) return 0;
    ui->user_act = by_hand(&ui->queue[ui->qhead]);
    *ev = ui->queue[ui->qhead];
    ui->given = ui->qtext[ui->qhead];
    ui->qtext[ui->qhead] = NULL;
    ui->qhead = (ui->qhead + 1) % GK_QUEUE;
    ui->qcount--;
    return 1;
}

/**** the window's events ****/

struct gk_widget *gk_hit(struct gk_widget *w, int x, int y) {
    struct gk_widget *c, *h;
    if(w->flags & (GKW_HIDDEN | GKW_AWAY)) return NULL;
    if(x < w->x || y < w->y || x >= w->x + w->w || y >= w->y + w->h) return NULL;
    for(c = w->child; c; c = c->next) if((h = gk_hit(c, x, y))) return h;
    return w;
}

/* a key: the box's, if one is open; Tab and Shift+Tab move the focus, but from a widget that takes Tab, and Ctrl+Tab
   (with Shift back) from any, unless it is a menu item's shortcut; an area's menu shortcuts come before the area but
   for a key that types a character, and the area takes every other key; a widget's own keys come before the
   shortcuts, and what none takes is the window's */
static void key(struct gk *ui, unsigned k, unsigned mods) {
    struct gk_widget *f = ui->focus;
    struct gk_event *e;
    if(f && !gk_reached(f)) f = NULL;
    if(ui->box && gk_box_key(ui, k, mods)) return;
    if(((k == 9 && !(mods & WIN_MOD_CTRL)) || k == WIN_KEY_BACKTAB) && !(f && (f->opts & GK_TAKES_TAB))) {
        gk_move_focus(ui, k == WIN_KEY_BACKTAB || gk_shift(mods));
        return;
    }
    if(k == WIN_KEY_TAB) {
        if(!ui->box && gk_menu_key(ui, k, mods)) return;
        gk_move_focus(ui, gk_shift(mods));
        return;
    }
    if(!ui->box && f && f->cls == &gk_area_class && !typing(k, mods) && gk_menu_key(ui, k, mods)) return;
    if(f && f->cls->key && f->cls->key(f, k, mods)) return;
    if(ui->box) return;                                 /* nothing behind a box hears a key */
    if(gk_menu_key(ui, k, mods)) return;
    if((e = gk_post_ev(ui, GK_EV_KEY, NULL, (int)k))) e->mods = mods;
}

/* the area the pointer is over (none: NULL) changed: the one it left hears it once, with a place outside it */
static void hover(struct gk *ui, struct gk_widget *t, const struct win_event *e) {
    struct gk_widget *a = t && t->cls == &gk_area_class && gk_reached(t) ? t : NULL;
    struct gk_event *ev;
    if(ui->capture || a == ui->under) return;
    if(ui->under && (ev = gk_post_ev(ui, GK_EV_POINTER, ui->under, 0))) {
        ev->x = e->x - ui->under->x;
        ev->y = e->y - ui->under->y;
        if(ev->x >= 0 && ev->y >= 0 && ev->x < ui->under->w && ev->y < ui->under->h) ev->x = -1;   /* it left it */
        ev->buttons = 0;                                /* (no press of its: nothing it may drag with) */
        ev->mods = e->mods;
    }
    ui->under = a;
}

/* the pointer: a press gives the widget under it the pointer (capture) until every button is up, and a widget hears
   buttons held only while it has it, so that a drag starts only from a press the window saw over it (a button held
   since a press elsewhere, on the desktop or outside a box, is a hover) */
static void pointer(struct gk *ui, const struct win_event *e) {
    unsigned old = ui->buttons, now = e->value, pressed = now & ~old, released = old & ~now;
    struct gk_widget *layer = ui->box ? ui->box : ui->root, *t;
    ui->buttons = now;
    ui->pmods = e->mods;
    ui->px = e->x;
    ui->py = e->y;
    t = ui->capture ? ui->capture : gk_hit(layer, e->x, e->y);
    hover(ui, t, e);
    if(pressed & WIN_BUTTON_LEFT) {                     /* one click, two (a double-click), three */
        if(t && t == ui->click_w && gk_now() - ui->click_ms <= GK_DOUBLE_MS &&
           abs(e->x - ui->click_x) <= 4 && abs(e->y - ui->click_y) <= 4) ui->clicks++;
        else ui->clicks = 1;
        ui->click_w = t;
        ui->click_ms = gk_now();
        ui->click_x = e->x;
        ui->click_y = e->y;
    }
    if(pressed && t && gk_reached(t)) {
        if(takes_focus(t)) gk_set_focus(ui, t);
        ui->capture = t;
    }
    if(t && gk_reached(t) && t->cls->pointer)
        t->cls->pointer(t, e->x, e->y, t == ui->capture ? now : 0, pressed, t == ui->capture ? released : 0);
    if(!now) ui->capture = NULL;
}

static void wheel(struct gk *ui, const struct win_event *e) {
    struct gk_widget *t = gk_hit(ui->box ? ui->box : ui->root, e->x, e->y);
    ui->pmods = e->mods;                                /* Ctrl or Shift with the wheel: the widget looks */
    if(ui->capture) t = ui->capture;                    /* a held button: the wheel is that widget's */
    for(; t; t = t->parent)
        if(gk_reached(t) && t->cls->wheel && t->cls->wheel(t, e->x, e->y, (int)e->value)) return;
}

static void dispatch(struct gk *ui, const struct win_event *e) {
    struct gk_event *ev;
    switch(e->type) {
    case WIN_EV_KEY:
        key(ui, e->value, e->mods);
        break;
    case WIN_EV_POINTER:
        pointer(ui, e);
        break;
    case WIN_EV_WHEEL:
        wheel(ui, e);
        break;
    case WIN_EV_FOCUS:                                  /* gw_next() keeps win->focused: drawn at the flush */
        if(ui->focus) gk_dirty_lit(ui->focus);
        if(!ui->win->focused && ui->buttons) {          /* the desktop took the pointer: nothing stays held */
            ui->buttons = 0;
            ui->capture = NULL;
        }
        sync_keys(ui);
        break;
    case WIN_EV_RESIZE:                                 /* gw_next() made the image again: all of it, laid out */
        ui->layout = 1;
        gk_dirty_rect(ui, 0, 0, ui->win->w, ui->win->h);
        if((ev = gk_post_ev(ui, GK_EV_RESIZE, NULL, 0))) {
            ev->x = ui->win->w;
            ev->y = ui->win->h;
        }
        break;
    case WIN_EV_CLOSE:
        gk_post(ui, GK_EV_CLOSE, NULL, 0);
        break;
    case WIN_EV_MENU:                                   /* 11/05/26: an item chosen in the desktop's bar; while a box is
                                                           open, none (the menus are greyed: one chosen before that
                                                           reached the bar goes) */
        if(!ui->box && (ev = gk_post_ev(ui, GK_EV_MENU, NULL, e->x))) ev->id = (int)e->value;
        break;
    case WIN_EV_DIALOG:                                 /* a chooser answered */
        gk_dialog_answer(ui, e);
        break;
    case WIN_EV_THEME:                                  /* a new theme: its colours, all of the window again */
        gk_theme_read(ui);
        gk_dirty_rect(ui, 0, 0, ui->win->w, ui->win->h);
        gk_post(ui, GK_EV_THEME, NULL, ui->theme.which);
        break;
    }
}

/**** timers and descriptors ****/

int gk_timer(struct gk *ui, int id, int ms, int repeat) {
    struct gk_timer *t = NULL;
    int i;
    for(i = 0; i < GK_TIMERS; i++) if(ui->timers[i].on && ui->timers[i].id == id) t = &ui->timers[i];
    for(i = 0; !t && i < GK_TIMERS; i++) if(!ui->timers[i].on) t = &ui->timers[i];
    if(!t) return -1;
    if(ms < 1) ms = 1;
    t->id = id;
    t->ms = ms;
    t->repeat = repeat;
    t->on = 1;
    t->due = gk_now() + (unsigned)ms;
    return 0;
}

void gk_timer_stop(struct gk *ui, int id) {
    int i, k, n = 0;
    for(i = 0; i < GK_TIMERS; i++) if(ui->timers[i].on && ui->timers[i].id == id) ui->timers[i].on = 0;
    for(k = 0; k < ui->qcount; k++) {                   /* (the re-review, N3) its events not given yet go too: a key and
                                                           the timer due in one turn queue the key first */
        int from = (ui->qhead + k) % GK_QUEUE, to = (ui->qhead + n) % GK_QUEUE;
        if(ui->queue[from].type == GK_EV_TIMER && ui->queue[from].id == id) {
            if(ui->qtext[from]) {
                gk_wipe(ui->qtext[from], strlen(ui->qtext[from]));
                free(ui->qtext[from]);
                ui->qtext[from] = NULL;
            }
            continue;
        }
        if(from != to) {
            ui->queue[to] = ui->queue[from];
            ui->qtext[to] = ui->qtext[from];
            ui->qtext[from] = NULL;
        }
        n++;
    }
    ui->qcount = n;
}

/* the timers that are due: their events (a repeating one keeps its beat, and skips the beats it missed) */
static int fire(struct gk *ui, unsigned now) {
    struct gk_timer *t;
    struct gk_event *e;
    int i, n = 0;
    for(i = 0; i < GK_TIMERS; i++) {
        t = &ui->timers[i];
        if(!t->on || (int)(t->due - now) > 0) continue;
        if(!(e = gk_post_ev(ui, GK_EV_TIMER, NULL, 0))) break;
        e->id = t->id;
        n++;
        if(!t->repeat) t->on = 0;
        else {
            t->due += (unsigned)t->ms;
            if((int)(t->due - now) <= 0) t->due = now + (unsigned)t->ms;
        }
    }
    return n;
}

static int until_timer(const struct gk *ui, unsigned now) {
    int i, d, best = -1;
    for(i = 0; i < GK_TIMERS; i++) {
        if(!ui->timers[i].on) continue;
        d = (int)(ui->timers[i].due - now);
        if(d < 0) d = 0;
        if(best < 0 || d < best) best = d;
    }
    return best;
}

/* a watch's GK_EV_FD (x: its descriptor): one waits at most, a new one's revents merged into it, so that a program that
   never unwatches a descriptor at its end costs time, never memory. (The re-review, N2: watches stay level-triggered
   until gk_unwatch() or a close, POLLHUP or not: SurfOS's pipes and sockets say POLLIN|POLLHUP both while data is left
   and at the end, so only the program's read() can tell, and an event told once would strand what was left) */
static void fd_event(struct gk *ui, const struct gk_watch *wa, short revents) {
    struct gk_event *e;
    int k;
    for(k = 0; k < ui->qcount; k++) {
        e = &ui->queue[(ui->qhead + k) % GK_QUEUE];
        if(e->type == GK_EV_FD && e->x == wa->fd - 1 && e->id == wa->id) {
            e->value |= revents;
            return;
        }
    }
    if((e = gk_post_ev(ui, GK_EV_FD, NULL, revents))) {
        e->id = wa->id;
        e->x = wa->fd - 1;
    }
}

int gk_watch(struct gk *ui, int fd, short events, int id) {
    int i, slot = -1;
    if(fd < 0) return -1;
    for(i = 0; i < GK_WATCHES; i++) {
        if(ui->watches[i].fd == fd + 1) slot = i;       /* fd + 1: 0 is a free slot */
        else if(slot < 0 && !ui->watches[i].fd) slot = i;
    }
    if(slot < 0) return -1;
    ui->watches[slot].fd = fd + 1;
    ui->watches[slot].events = events;
    ui->watches[slot].id = id;
    return 0;
}

void gk_unwatch(struct gk *ui, int fd) {
    int i;
    for(i = 0; i < GK_WATCHES; i++) if(ui->watches[i].fd == fd + 1) ui->watches[i].fd = 0;
}

int gk_copy(struct gk *ui, const char *text, unsigned len) {
    return gw_clip(ui->win, text, len);
}



/**** the window ****/

struct gk *gk_open_limits(const char *title, int w, int h, int min_w, int min_h, int max_w, int max_h, unsigned flags) {
    struct gk *ui = calloc(1, sizeof(*ui));
    int err;
    if(!ui) {
        errno = ENOMEM;
        return NULL;
    }
    ui->win = gw_open_limits(title, w, h, min_w, min_h, max_w, max_h, flags, GW_PAPER);
    if(!ui->win) {
        err = errno;
        free(ui);
        errno = err;
        return NULL;
    }
    ui->theme = ocean;
    ui->desk_calls = gk_theme_read(ui);                 /* the desktop's own, and news of a new one from now on */
    ui->resizable = (flags & WIN_RESIZABLE) != 0;
    ui->root = gk_make_in(ui, &box_class, 0);
    if(!ui->root) {
        gw_close(ui->win);
        free(ui);
        errno = ENOMEM;
        return NULL;
    }
    ui->root->grow_x = ui->root->grow_y = 1;
    ui->layout = 1;
    gk_dirty_rect(ui, 0, 0, ui->win->w, ui->win->h);
    return ui;
}

struct gk *gk_open(const char *title, int w, int h, unsigned flags) {
    return gk_open_limits(title, w, h, 0, 0, 0, 0, flags);
}

void gk_close(struct gk *ui) {
    int i;
    if(!ui) return;
    gk_box_free(ui);
    gk_free_tree(ui->root);
    free(ui->menus);
    free(ui->later);
    drop_given(ui);
    for(i = 0; i < GK_QUEUE; i++) {
        if(!ui->qtext[i]) continue;
        gk_wipe(ui->qtext[i], strlen(ui->qtext[i]));
        free(ui->qtext[i]);
    }
    gw_close(ui->win);
    free(ui);
}

struct gw *gk_gw(struct gk *ui) { return ui->win; }
struct gk_widget *gk_root(struct gk *ui) { return ui->root; }
int gk_ok(const struct gk *ui) { return ui && !ui->failed; }
const struct gk_theme *gk_theme(const struct gk *ui) { return &ui->theme; }

/* one event the window has: 1 handled, 0 none, -1 the window has gone or the read failed */
static int window_event(struct gk *ui) {
    struct win_event e;
    int r = gw_next(ui->win, &e, 0);
    if(r > 0) dispatch(ui, &e);
    if(r > 0 && e.type == WIN_EV_KEY) {                 /* a key gone where it went (a password's character, say):
                                                           its copies, here and in gui.h's queue, zeroed */
        gk_wipe(&ui->win->queue[ui->win->given - 1], sizeof(e));
        gk_wipe(&e, sizeof(e));
    }
    return r;
}

int gk_next(struct gk *ui, struct gk_event *ev, int ms) {
    struct pollfd pf[1 + GK_WATCHES];
    unsigned start = gk_now(), now;
    int i, n, r, wait, t, slot[1 + GK_WATCHES], polled = 0, batches = 0;
    drop_given(ui);
    ui->user_act = 0;                                   /* the program has answered the event it was given */
    for(;;) {
        if(take(ui, ev)) return 1;
        if(ui->win->gone) {
            errno = EPIPE;
            return -1;
        }
        if(ui->win->given < ui->win->queued) {          /* read already: no wait for those */
            if(window_event(ui) < 0) return -1;
            continue;
        }
        if(ui->later) {                                 /* a chooser's request waits for room: those read, again */
            gk_choose_again(ui);
            if(ui->qcount) continue;
        }
        if(gk_flush(ui) < 0) {                          /* (painted before any wait, and before a timer's event:
                                                           timers due one after another never starve the window) */
            errno = EPIPE;
            return -1;
        }
        if(ui->qcount) continue;                        /* the flush told an area of the keys */
        now = gk_now();
        wait = -1;
        if(ms >= 0) {
            wait = ms - (int)(now - start);
            if(wait < 0) wait = 0;
            if(polled && !wait) return 0;               /* looked once more at the end: nothing */
        }
        t = until_timer(ui, now);
        if(t >= 0 && (wait < 0 || t < wait)) wait = t;
        pf[0].fd = ui->win->fd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        for(i = 0, n = 1; i < GK_WATCHES; i++) {
            if(!ui->watches[i].fd) continue;
            pf[n].fd = ui->watches[i].fd - 1;
            pf[n].events = ui->watches[i].events;
            pf[n].revents = 0;
            slot[n++] = i;
        }
        r = poll(pf, (nfds_t)n, wait);
        polled = !pf[0].revents || ++batches > GK_BATCHES;   /* the window had events: they are read on, its queue
                                                               drained (a few batches more) before 0 is the answer */
        if(r < 0) return -1;                            /* errno: EINTR for a kill or Ctrl+C */
        if(pf[0].revents && window_event(ui) < 0) return -1;
        for(i = 1; i < n; i++) {
            if(!pf[i].revents || !ui->watches[slot[i]].fd) continue;
            fd_event(ui, &ui->watches[slot[i]], pf[i].revents);
            if(pf[i].revents & POLLNVAL) ui->watches[slot[i]].fd = 0;   /* no descriptor: told once, the watch gone */
        }
        fire(ui, gk_now());                             /* the timers due, after the window and the descriptors were
                                                           looked at (the review: before, a timer due again at once was
                                                           all a program heard) */
        if(!r && ms >= 0 && (int)(gk_now() - start) >= ms && !ui->qcount && until_timer(ui, gk_now()) != 0) return 0;
    }
}
