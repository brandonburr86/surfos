/*
SurfOS window toolkit: an area a program draws
--------------------
File: gkarea.c  Date: 11/05/26
--------------------
gk_area() (gk.h): a widget whose pixels are the program's: a spreadsheet's grid, an editor's text,
a canvas, a game's board, a chat's messages. It sits in the layout as any widget does, and whenever
any of it is to be painted the toolkit calls its draw function with the window's drawing (gui.h)
moved to the area's top left and clipped to it and to what is being painted (struct gk_view says
which part that is, so a large area need draw only that). It hears the pointer over it, and anywhere
while a button pressed in it is held, in its own coordinates, and with GK_FOCUSABLE the keys while it
has the focus (all of them, but Tab and Shift+Tab unless GK_TAKES_TAB).

Scrolling is in the program's units (rows, columns, pixels): gk_area_extent() says how many there
are each way and how many pixels each is, and the toolkit shows a scroll bar each way that does not
fit (the desktop's: the band colour, a muted thumb), keeps the positions within what can be shown,
and moves them by the bars and the wheel (three rows a notch, as the desktop's windows scroll, or
48 pixels of an area that scrolls by pixels; across with Shift), telling the program GK_EV_SCROLL.
The wheel with Ctrl is the program's (GK_EV_WHEEL with its mods: a zoom, as a rule). The program draws
from its positions, and moves them itself (gk_scroll_to()) to keep what it shows in view.
*/

#include <stdlib.h>
#include <string.h>
#include "gkpriv.h"

struct area {
    gk_draw_fn *draw;
    void *data;
    int tx, ux, ty, uy;                 /* the units across and down, and their pixels */
    int sx, sy;                         /* the first unit shown each way */
    int vw, vh;                         /* the view, without the bars */
    int vbar, hbar;                     /* which bars show */
    int grab, held;                     /* a bar's thumb held: from where; held 1 down, 2 across, 0 the program's */
};

static void fit(struct gk_widget *w) {
    struct area *a = w->k;
    int i, page;
    a->vw = w->w;
    a->vh = w->h;
    a->vbar = a->hbar = 0;
    for(i = 0; i < 2; i++) {                            /* a bar each way takes room from the other */
        a->vbar = a->ty > 0 && (long long)a->ty * a->uy > a->vh;
        a->hbar = a->tx > 0 && (long long)a->tx * a->ux > a->vw;
        a->vw = w->w - (a->vbar ? GK_SCROLL_W : 0);
        a->vh = w->h - (a->hbar ? GK_SCROLL_W : 0);
        if(a->vw < 0) a->vw = 0;
        if(a->vh < 0) a->vh = 0;
    }
    page = a->vh / a->uy;
    if(a->sy > a->ty - page) a->sy = a->ty - page;
    if(a->sy < 0) a->sy = 0;
    page = a->vw / a->ux;
    if(a->sx > a->tx - page) a->sx = a->tx - page;
    if(a->sx < 0) a->sx = 0;
}

static struct gk_bar bar(const struct area *a, int across) {
    struct gk_bar b;
    b.total = across ? a->tx : a->ty;
    b.shown = across ? a->vw / a->ux : a->vh / a->uy;
    b.first = across ? a->sx : a->sy;
    return b;
}

static void area_arrange(struct gk_widget *w) {
    fit(w);
}

static void area_paint(struct gk_widget *w, struct gw *win) {
    struct area *a = w->k;
    const struct gk_theme *t = &w->ui->theme;
    struct gw_view keep, now;
    struct gk_view v;
    struct gk_bar b;
    gw_get_view(win, &keep);
    if(a->vbar) {
        b = bar(a, 0);
        gk_bar_paint(win, t, w->x + a->vw, w->y, a->vh, 0, &b);
    }
    if(a->hbar) {
        b = bar(a, 1);
        gk_bar_paint(win, t, w->x, w->y + a->vh, a->vw, 1, &b);
    }
    if(a->vbar && a->hbar) gw_rect(win, w->x + a->vw, w->y + a->vh, GK_SCROLL_W, GK_SCROLL_W, t->band);
    if(!a->draw || a->vw <= 0 || a->vh <= 0) return;
    gw_clip_to(win, w->x, w->y, a->vw, a->vh);
    gw_origin(win, w->x, w->y);
    gw_get_view(win, &now);
    if(now.r > now.l && now.b > now.t) {
        v.w = a->vw;
        v.h = a->vh;
        v.sx = a->sx;
        v.sy = a->sy;
        v.cx = now.l - now.ox;
        v.cy = now.t - now.oy;
        v.cw = now.r - now.l;
        v.ch = now.b - now.t;
        a->draw(w, win, &v, a->data);
    }
    gw_set_view(win, &keep);
}

static void scrolled(struct gk_widget *w) {
    struct area *a = w->k;
    struct gk_event *e = gk_post_ev(w->ui, GK_EV_SCROLL, w, 0);
    if(e) {
        e->x = a->sx;
        e->y = a->sy;
    }
    gk_dirty(w);
}

static void area_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    struct area *a = w->k;
    struct gk_event *e;
    struct gk_bar b;
    int rx = x - w->x, ry = y - w->y;
    (void)released;
    if(pressed && !(buttons & ~pressed)) {              /* the first button down: on a bar, or the program's */
        a->held = 0;
        if(a->vbar && rx >= a->vw && ry < a->vh) a->held = 1;
        else if(a->hbar && ry >= a->vh && rx < a->vw) a->held = 2;
        else if(rx >= a->vw || ry >= a->vh) a->held = 3;    /* the corner between the bars: nobody's */
        if(a->held == 1 || a->held == 2) {
            b = bar(a, a->held == 2);
            if((pressed & WIN_BUTTON_LEFT) && gk_bar_press(a->held == 2 ? a->vw : a->vh, a->held == 2 ? rx : ry, &b,
                                                            &a->grab)) {
                if(a->held == 2) a->sx = b.first;
                else a->sy = b.first;
                scrolled(w);
            }
            return;
        }
    }
    if(a->held == 1 || a->held == 2) {                  /* the thumb, dragged */
        b = bar(a, a->held == 2);
        if((buttons & WIN_BUTTON_LEFT) && gk_bar_drag(a->held == 2 ? a->vw : a->vh, a->held == 2 ? rx : ry, &b, a->grab)) {
            if(a->held == 2) a->sx = b.first;
            else a->sy = b.first;
            scrolled(w);
        }
        if(!buttons) a->held = 0;
        return;
    }
    if(a->held == 3) {
        if(!buttons) a->held = 0;
        return;
    }
    if(!(e = gk_post_ev(w->ui, GK_EV_POINTER, w, (int)pressed))) return;
    e->x = rx;
    e->y = ry;
    e->buttons = buttons;
    e->mods = w->ui->pmods;
    e->clicks = (pressed & WIN_BUTTON_LEFT) ? w->ui->clicks : 0;
}

static int area_key(struct gk_widget *w, unsigned key, unsigned mods) {
    struct gk_event *e;
    if(!(w->opts & GK_FOCUSABLE)) return 0;
    if((e = gk_post_ev(w->ui, GK_EV_KEY, w, (int)key))) e->mods = mods;
    return 1;
}

/* the wheel scrolls down, or across with Shift; with Ctrl (a zoom, as a rule), or when the area scrolls not that way or
   asked for it (GK_OWN_WHEEL), it is the program's */
static int area_wheel(struct gk_widget *w, int x, int y, int rows) {
    struct area *a = w->k;
    struct gk_event *e;
    unsigned mods = w->ui->pmods;
    int across = (mods & WIN_MOD_SHIFT) != 0, wx = a->sx, wy = a->sy, unit = across ? a->ux : a->uy;
    if((w->opts & GK_OWN_WHEEL) || (mods & WIN_MOD_CTRL) || (across ? a->tx : a->ty) <= 0) {
        if((e = gk_post_ev(w->ui, GK_EV_WHEEL, w, rows))) {
            e->x = x - w->x;
            e->y = y - w->y;
            e->mods = mods;
        }
        return 1;
    }
    {                                                   /* (the re-review, N7) in 64 bits, held to the int's range */
        long long to = (long long)(across ? a->sx : a->sy) + (long long)rows * (unit >= 16 ? 1 : 16 / unit);
        to = to > 0x7FFFFFFF ? 0x7FFFFFFF : to < -0x7FFFFFFF ? -0x7FFFFFFF : to;
        if(across) a->sx = (int)to;
        else a->sy = (int)to;
    }
    fit(w);
    if(a->sx != wx || a->sy != wy) scrolled(w);
    return 1;
}

static void area_measure(struct gk_widget *w) {
    w->base_w = w->base_h = 0;
}

/* the focus changes nothing the toolkit draws: the program hears GK_EV_FOCUS and paints what it shows of it */
static void area_lit(const struct gk_widget *w, struct gk_rect *r) {
    r->x = w->x;
    r->y = w->y;
    r->w = r->h = 0;
}

const struct gk_class gk_area_class = {
    .measure = area_measure, .arrange = area_arrange, .paint = area_paint, .key = area_key, .pointer = area_pointer,
    .wheel = area_wheel, .lit_part = area_lit,
};

struct gk_widget *gk_area(struct gk_widget *parent, int id, unsigned flags, gk_draw_fn *draw, void *data) {
    struct gk_widget *w = gk_make(parent, &gk_area_class, sizeof(struct area));
    struct area *a;
    if(!w) return NULL;
    a = w->k;
    a->draw = draw;
    a->data = data;
    a->ux = a->uy = 1;
    a->grab = -1;
    w->id = id;
    w->opts = flags & (GK_FOCUSABLE | GK_TAKES_TAB | GK_OWN_WHEEL);
    w->grow_x = w->grow_y = 1;
    return w;
}

static struct area *area_of(const struct gk_widget *w) {
    return w && w->cls == &gk_area_class ? w->k : NULL;
}

void gk_area_extent(struct gk_widget *w, int total_x, int unit_x, int total_y, int unit_y) {
    struct area *a = area_of(w);
    int vbar, hbar;
    if(!a) return;
    a->tx = total_x > 0 ? total_x : 0;
    a->ty = total_y > 0 ? total_y : 0;
    a->ux = unit_x > 0 ? unit_x : 1;
    a->uy = unit_y > 0 ? unit_y : 1;
    vbar = a->vbar;
    hbar = a->hbar;
    fit(w);
    if(vbar != a->vbar || hbar != a->hbar) gk_dirty(w);    /* a bar came or went: the whole of it */
    else {                                              /* the bars' thumbs only */
        if(a->vbar) gk_dirty_rect(w->ui, w->x + a->vw, w->y, GK_SCROLL_W, a->vh);
        if(a->hbar) gk_dirty_rect(w->ui, w->x, w->y + a->vh, a->vw, GK_SCROLL_W);
    }
}

void gk_area_view(const struct gk_widget *w, struct gk_view *v) {
    const struct area *a = area_of(w);
    memset(v, 0, sizeof(*v));
    if(!a) return;
    v->w = v->cw = a->vw;
    v->h = v->ch = a->vh;
    v->sx = a->sx;
    v->sy = a->sy;
}

void gk_scroll_to(struct gk_widget *w, int sx, int sy) {
    struct area *a = area_of(w);
    if(!a) return;
    if(sx == a->sx && sy == a->sy) return;
    a->sx = sx;
    a->sy = sy;
    fit(w);
    gk_dirty(w);
}

/* the least scrolling that shows units ux..ux+uw-1 across and uy..uy+uh-1 down: those that do not fit show from their
   start */
void gk_area_reveal(struct gk_widget *w, int ux, int uy, int uw, int uh) {
    struct area *a = area_of(w);
    int sx, sy, px, py;
    if(!a) return;
    px = a->vw / a->ux;
    py = a->vh / a->uy;
    sx = a->sx;
    sy = a->sy;
    if(uw < 1) uw = 1;
    if(uh < 1) uh = 1;
    if(ux + uw > sx + px) sx = ux + uw - px;
    if(ux < sx) sx = ux;
    if(uy + uh > sy + py) sy = uy + uh - py;
    if(uy < sy) sy = uy;
    gk_scroll_to(w, sx, sy);
}

void gk_damage(struct gk_widget *w, int x, int y, int width, int height) {
    const struct area *a = area_of(w);
    if(!w) return;
    if(!gk_clip_rect(&x, &y, &width, &height, a ? a->vw : w->w, a ? a->vh : w->h)) return;   /* in its view */
    if(gk_shown_now(w)) gk_dirty_rect(w->ui, w->x + x, w->y + y, width, height);
}
