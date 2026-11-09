/*
SurfOS window toolkit: the small widgets
--------------------
File: gkwidget.c Date: 11/05/26
--------------------
Labels, buttons, check boxes and switches, radio groups, separators and spacers, status bars,
scroll bars and tabs (gk.h), drawn as the desktop draws its own windows (shell/guidraw.c,
guicore.c, guimon.c): buttons light, the colours of shell/guidraw.c's palette, the desktop's
toolbars and the tab of the page shown lit in teal, its status bars in the band colour with a line
above, its scroll bars with a muted thumb. Check boxes, switches and radio buttons the desktop has
none of are drawn here in the same colours. A widget with the focus is lit (a button in teal, as a
dialog's chosen button) or framed in teal, while the window has the desktop's focus.
*/

#include <stdlib.h>
#include <string.h>
#include "gkpriv.h"

static int max(int a, int b) { return a > b ? a : b; }
static int min(int a, int b) { return a < b ? a : b; }

/**** drawing the widgets share ****/

void gk_focus_ring(struct gw *win, const struct gk_theme *t, int x, int y, int w, int h) {
    gw_frame(win, x, y, w, h, t->teal);
}

/* a sheet's border, teal when it has the keys (the desktop's text box), and its inside */
void gk_frame_in(struct gw *win, const struct gk_theme *t, int x, int y, int w, int h, int lit) {
    gw_rect(win, x, y, w, h, lit ? t->teal : t->line);
    gw_rect(win, x + 1, y + 1, w - 2, h - 2, t->sheet);
}

/* lines of text (apart at \n): how many, and the widest's cells */
int gk_lines(const char *text, int *widest) {
    int n = 1, cells = 0, len;
    *widest = 0;
    for(; *text; text += len) {
        if(*text == '\n') {
            n++;
            cells = 0;
            len = 1;
            continue;
        }
        gw_char(text, (int)strlen(text), &len);
        if(++cells > *widest) *widest = cells;
    }
    return n;
}

/* each line from x, y, 16 pixels apart, cut to the cells of w */
void gk_draw_lines(struct gw *win, int x, int y, int w, const char *text, unsigned color) {
    char line[512];
    int n, len, cells = w / 8, k;
    while(1) {
        for(n = 0; text[n] && text[n] != '\n'; n++) ;
        for(k = 0; k < n && k < (int)sizeof(line) - 4; k += len) {   /* whole characters only */
            gw_char(text + k, n - k, &len);
            if(k + len > (int)sizeof(line) - 1) break;
        }
        memcpy(line, text, (size_t)k);
        line[k] = 0;
        gw_text_cut(win, x, y, line, cells, color);
        if(!text[n]) return;
        text += n + 1;
        y += 16;
    }
}

int gk_text_x(const char *s, int cells) {
    int n = gw_text_width(s) / 8;
    return n < cells ? n : cells;
}

/* a disc of rows wide less inset[row] at each end */
static void disc(struct gw *win, int x, int y, int size, const unsigned char *inset, unsigned color) {
    int i;
    for(i = 0; i < size; i++) gw_rect(win, x + inset[i], y + i, size - 2 * inset[i], 1, color);
}

static const unsigned char disc12[12] = { 4, 2, 1, 1, 0, 0, 0, 0, 1, 1, 2, 4 };
static const unsigned char disc10[10] = { 3, 1, 1, 0, 0, 0, 0, 1, 1, 3 };
static const unsigned char disc6[6] = { 1, 0, 0, 0, 0, 1 };
static const unsigned char pill14[14] = { 4, 2, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 2, 4 };

/**** scroll bars' arithmetic (the desktop's gd_thumb() and gd_scroll_to(), in 64 bits) ****/

int gk_bar_max(const struct gk_bar *b) {
    return b->total > b->shown ? b->total - b->shown : 0;
}

void gk_bar_thumb(int len, const struct gk_bar *b, int *top, int *size) {
    long long s;
    int first = b->first, mx = gk_bar_max(b);
    if(!mx || len <= 0) {
        *top = 0;
        *size = len > 0 ? len : 0;
        return;
    }
    s = gk_div64((long long)len * (b->shown > 0 ? b->shown : 0), b->total);
    if(s < 16) s = 16;
    if(s > len) s = len;
    *size = (int)s;
    first = first < 0 ? 0 : first > mx ? mx : first;
    *top = (int)gk_div64((long long)(len - s) * first, mx);
}

int gk_bar_first(int len, int pos, const struct gk_bar *b) {
    int top, size, mx = gk_bar_max(b);
    long long f;
    if(!mx) return 0;
    gk_bar_thumb(len, b, &top, &size);
    if(len <= size) return 0;
    if(pos <= 0) return 0;
    f = gk_div64((long long)pos * mx + (len - size) / 2, len - size);
    return f < 0 ? 0 : f > mx ? mx : (int)f;
}

void gk_bar_paint(struct gw *win, const struct gk_theme *t, int x, int y, int len, int across, const struct gk_bar *b) {
    int top, size;
    if(across) {
        gw_rect(win, x, y, len, GK_SCROLL_W, t->band);
        gw_rect(win, x, y, len, 1, t->line);
    } else {
        gw_rect(win, x, y, GK_SCROLL_W, len, t->band);
        gw_rect(win, x, y, 1, len, t->line);
    }
    if(!gk_bar_max(b)) return;
    gk_bar_thumb(len, b, &top, &size);
    if(across) gw_rect(win, x + top + 1, y + 2, size - 2, GK_SCROLL_W - 3, t->muted);
    else gw_rect(win, x + 2, y + top + 1, GK_SCROLL_W - 3, size - 2, t->muted);
}

static int set_first(struct gk_bar *b, int first) {
    int mx = gk_bar_max(b);
    first = first < 0 ? 0 : first > mx ? mx : first;
    if(first == b->first) return 0;
    b->first = first;
    return 1;
}

int gk_bar_press(int len, int pos, struct gk_bar *b, int *grab) {
    int top, size;
    *grab = -1;
    if(!gk_bar_max(b)) return 0;
    gk_bar_thumb(len, b, &top, &size);
    if(pos >= top && pos < top + size) {                /* the thumb: dragged from where it was taken */
        *grab = pos - top;
        return 0;
    }
    return set_first(b, b->first + (pos < top ? -b->shown : b->shown));    /* a page before or after it */
}

int gk_bar_drag(int len, int pos, struct gk_bar *b, int grab) {
    if(grab < 0) return 0;
    return set_first(b, gk_bar_first(len, pos - grab, b));
}

/**** labels ****/

struct label { unsigned color; };

static void label_measure(struct gk_widget *w) {
    int widest, n = gk_lines(w->text ? w->text : "", &widest);
    w->base_w = widest * 8;
    w->base_h = n == 1 ? GK_LINE_H : n * 16 + 6;
}

static void label_paint(struct gk_widget *w, struct gw *win) {
    const struct label *l = w->k;
    const struct gk_theme *t = &w->ui->theme;
    unsigned c = w->flags & GKW_DISABLED ? t->grey : l->color & 0xFF000000u ? t->ink : l->color;
    int n, widest;
    if(!w->text) return;
    n = gk_lines(w->text, &widest);
    gk_draw_lines(win, w->x, w->y + (n == 1 ? (w->h - 16) / 2 : 3), w->w, w->text, c);
}

static const struct gk_class label_class = {
    .measure = label_measure, .paint = label_paint,
};

struct gk_widget *gk_label(struct gk_widget *parent, const char *text) {
    struct gk_widget *w = gk_make(parent, &label_class, sizeof(struct label));
    if(!w) return NULL;
    ((struct label *)w->k)->color = GK_INK;
    gk_set_text(w, text);
    return w;
}

void gk_label_color(struct gk_widget *w, unsigned color) {
    if(!w || w->cls != &label_class) return;
    ((struct label *)w->k)->color = color;
    gk_dirty(w);
}

/**** buttons ****/

static void button_measure(struct gk_widget *w) {
    w->base_w = max(64, gw_text_width(w->text ? w->text : "") + 20);
    w->base_h = GK_BUTTON_H;
}

static void button_paint(struct gk_widget *w, struct gw *win) {
    const struct gk_theme *t = &w->ui->theme;
    int off = (w->flags & GKW_DISABLED) != 0, pressed = (w->flags & GKW_PRESSED) != 0, lit = gk_lit(w);
    unsigned bg = off ? t->band : pressed ? t->frame : lit ? t->teal : t->shell;
    unsigned fg = off ? t->grey : pressed || lit ? t->on_teal : t->ink;
    const char *s = w->text ? w->text : "";
    int cells = (w->w - 8) / 8, n = gk_text_x(s, cells);
    gw_rect(win, w->x, w->y, w->w, w->h, bg);
    gw_text_cut(win, w->x + (w->w - n * 8) / 2, w->y + (w->h - 16) / 2, s, cells, fg);
}

static int button_key(struct gk_widget *w, unsigned key, unsigned mods) {
    if((key != ' ' && key != '\r' && key != '\n') || (mods & (WIN_MOD_CTRL | WIN_MOD_ALT))) return 0;
    if(mods & WIN_MOD_REPEAT) return 1;                 /* once a press: a held key clicks once */
    gk_post(w->ui, GK_EV_CLICK, w, 0);
    return 1;
}

static int inside(const struct gk_widget *w, int x, int y) {
    return x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h;
}

/* held down while the pointer is over it; clicked when the button is let go over it */
static void button_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    unsigned was = w->flags & GKW_PRESSED;
    if(pressed & WIN_BUTTON_LEFT) w->flags |= GKW_PRESSED;
    else if(buttons & WIN_BUTTON_LEFT) {
        if(inside(w, x, y) && w->ui->capture == w) w->flags |= GKW_PRESSED;
        else w->flags &= ~GKW_PRESSED;
    }
    if(released & WIN_BUTTON_LEFT) {
        if(was && inside(w, x, y)) gk_post(w->ui, GK_EV_CLICK, w, 0);
        w->flags &= ~GKW_PRESSED;
    }
    if(was != (w->flags & GKW_PRESSED)) gk_dirty(w);
}

static const struct gk_class button_class = {
    .takes_focus = 1, .measure = button_measure, .paint = button_paint, .key = button_key, .pointer = button_pointer,
};

struct gk_widget *gk_button(struct gk_widget *parent, const char *label, int id) {
    struct gk_widget *w = gk_make(parent, &button_class, 0);
    if(!w) return NULL;
    w->id = id;
    gk_set_text(w, label);
    return w;
}

/**** check boxes and switches ****/

struct check { int look; };             /* 0 a box, 1 a switch */

static void check_measure(struct gk_widget *w) {
    const struct check *c = w->k;
    w->base_w = (c->look ? 26 : 14) + 6 + gw_text_width(w->text ? w->text : "") + 2;
    w->base_h = GK_LINE_H;
}

static void check_paint(struct gk_widget *w, struct gw *win) {
    const struct check *c = w->k;
    const struct gk_theme *t = &w->ui->theme;
    int off = (w->flags & GKW_DISABLED) != 0, x = w->x, y = w->y + (w->h - 14) / 2, mark = c->look ? 26 : 14;
    int cells = (w->w - mark - 8) / 8;
    unsigned edge = off ? t->line : t->muted;
    if(c->look) {                                       /* a pill: teal and its knob at the right when on */
        disc(win, x, y, 14, pill14, off ? t->band : w->value ? t->teal : edge);
        gw_rect(win, x + 7, y, 12, 14, off ? t->band : w->value ? t->teal : edge);
        disc(win, x + 12, y, 14, pill14, off ? t->band : w->value ? t->teal : edge);
        disc(win, x + (w->value ? 14 : 2), y + 2, 10, disc10, off ? t->sheet : t->on_teal);
    } else {
        gw_rect(win, x, y, 14, 14, w->value && !off ? t->teal : edge);
        gw_rect(win, x + 1, y + 1, 12, 12, w->value && !off ? t->teal : off ? t->band : t->sheet);
        if(w->value) {                                  /* a tick, two pixels thick */
            gw_line(win, x + 3, y + 7, x + 5, y + 9, off ? t->grey : t->on_teal);
            gw_line(win, x + 3, y + 8, x + 5, y + 10, off ? t->grey : t->on_teal);
            gw_line(win, x + 5, y + 9, x + 10, y + 4, off ? t->grey : t->on_teal);
            gw_line(win, x + 5, y + 10, x + 10, y + 5, off ? t->grey : t->on_teal);
        }
    }
    gw_text_cut(win, x + mark + 6, w->y + (w->h - 16) / 2, w->text ? w->text : "", cells, off ? t->grey : t->ink);
    if(gk_lit(w))
        gk_focus_ring(win, t, x + mark + 3, w->y + (w->h - 16) / 2 - 2,
                      gk_text_x(w->text ? w->text : "", cells) * 8 + 6, 20);
}

static void check_toggle(struct gk_widget *w) {
    w->value = !w->value;
    gk_dirty(w);
    gk_post(w->ui, GK_EV_CHANGE, w, w->value);
}

static int check_key(struct gk_widget *w, unsigned key, unsigned mods) {
    if(key != ' ' || (mods & (WIN_MOD_CTRL | WIN_MOD_ALT))) return 0;
    if(mods & WIN_MOD_REPEAT) return 1;                 /* once a press */
    check_toggle(w);
    return 1;
}

static void check_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    (void)buttons;
    if(pressed & WIN_BUTTON_LEFT) w->flags |= GKW_PRESSED;
    if(released & WIN_BUTTON_LEFT) {
        if((w->flags & GKW_PRESSED) && inside(w, x, y)) check_toggle(w);
        w->flags &= ~GKW_PRESSED;
    }
}

static void check_set(struct gk_widget *w, int value) {
    value = value != 0;
    if(value == w->value) return;
    w->value = value;
    gk_dirty(w);
}

static const struct gk_class check_class = {
    .takes_focus = 1, .measure = check_measure, .paint = check_paint, .key = check_key, .pointer = check_pointer,
    .set_value = check_set,
};

static struct gk_widget *check(struct gk_widget *parent, const char *label, int id, int on, int look) {
    struct gk_widget *w = gk_make(parent, &check_class, sizeof(struct check));
    if(!w) return NULL;
    ((struct check *)w->k)->look = look;
    w->id = id;
    w->value = on != 0;
    gk_set_text(w, label);
    return w;
}

struct gk_widget *gk_check(struct gk_widget *parent, const char *label, int id, int on) {
    return check(parent, label, id, on, 0);
}

struct gk_widget *gk_switch(struct gk_widget *parent, const char *label, int id, int on) {
    return check(parent, label, id, on, 1);
}

/**** radio groups ****/

#define RADIO_MAX 32
struct radio { char *label[RADIO_MAX]; int n; };

/* the options' places: x, width of option i */
static int option_w(const struct radio *r, int i) {
    return 18 + gw_text_width(r->label[i]) + 2;
}

static void radio_measure(struct gk_widget *w) {
    const struct radio *r = w->k;
    int i;
    for(i = 0; i < r->n; i++) {
        if(w->across) w->base_w += option_w(r, i) + (i ? 16 : 0);
        else w->base_w = max(w->base_w, option_w(r, i));
    }
    w->base_h = w->across ? GK_LINE_H : r->n * GK_LINE_H;
}

static int option_at(const struct gk_widget *w, int x, int y) {
    const struct radio *r = w->k;
    int i, at = w->x;
    if(!w->across) {
        i = (y - w->y) / GK_LINE_H;
        return y >= w->y && i < r->n && x < w->x + option_w(r, i) ? i : -1;
    }
    for(i = 0; i < r->n; at += option_w(r, i) + 16, i++) if(x >= at && x < at + option_w(r, i)) return i;
    return -1;
}

static void radio_paint(struct gk_widget *w, struct gw *win) {
    const struct radio *r = w->k;
    const struct gk_theme *t = &w->ui->theme;
    int off = (w->flags & GKW_DISABLED) != 0, i, x = w->x, y = w->y, ow;
    for(i = 0; i < r->n; i++) {
        ow = option_w(r, i);
        disc(win, x, y + 5, 12, disc12, off ? t->line : i == w->value ? t->teal : t->muted);
        disc(win, x + 1, y + 6, 10, disc10, off ? t->band : t->sheet);
        if(i == w->value) disc(win, x + 3, y + 8, 6, disc6, off ? t->grey : t->teal);
        gw_text_cut(win, x + 18, y + 3, r->label[i], max(0, (w->x + w->w - x - 18) / 8), off ? t->grey : t->ink);
        if(gk_lit(w) && i == (w->value < 0 ? 0 : w->value))
            gk_focus_ring(win, t, x + 15, y + 1, ow - 12, 20);
        if(w->across) x += ow + 16;
        else y += GK_LINE_H;
    }
}

static void radio_choose(struct gk_widget *w, int i) {
    const struct radio *r = w->k;
    if(i < 0 || i >= r->n || i == w->value) return;
    w->value = i;
    gk_dirty(w);
    gk_post(w->ui, GK_EV_CHANGE, w, i);
}

static int radio_key(struct gk_widget *w, unsigned key, unsigned mods) {
    if(mods & (WIN_MOD_CTRL | WIN_MOD_ALT)) return 0;
    if((mods & WIN_MOD_REPEAT) && (key == ' ' || (key >= WIN_KEY_UP && key <= WIN_KEY_RIGHT))) return 1;  /* once */
    if(key == WIN_KEY_UP || key == WIN_KEY_LEFT) radio_choose(w, w->value <= 0 ? 0 : w->value - 1);
    else if(key == WIN_KEY_DOWN || key == WIN_KEY_RIGHT) radio_choose(w, w->value + 1);
    else if(key == ' ') radio_choose(w, w->value < 0 ? 0 : w->value);
    else return 0;
    return 1;
}

static void radio_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    (void)buttons;
    (void)released;
    if(pressed & WIN_BUTTON_LEFT) radio_choose(w, option_at(w, x, y));
}

static void radio_set(struct gk_widget *w, int value) {
    const struct radio *r = w->k;
    if(value < -1 || value >= r->n || value == w->value) return;
    w->value = value;
    gk_dirty(w);
}

static void radio_destroy(struct gk_widget *w) {
    struct radio *r = w->k;
    int i;
    for(i = 0; i < r->n; i++) free(r->label[i]);
}

static const struct gk_class radio_class = {
    .takes_focus = 1, .measure = radio_measure, .paint = radio_paint, .key = radio_key, .pointer = radio_pointer,
    .set_value = radio_set, .destroy = radio_destroy,
};

struct gk_widget *gk_radio(struct gk_widget *parent, int id, int across) {
    struct gk_widget *w = gk_make(parent, &radio_class, sizeof(struct radio));
    if(!w) return NULL;
    w->id = id;
    w->across = across != 0;
    return w;
}

int gk_radio_add(struct gk_widget *w, const char *label) {
    struct radio *r;
    if(!w || w->cls != &radio_class) return -1;
    r = w->k;
    if(r->n == RADIO_MAX || !(r->label[r->n] = gk_strdup(w->ui, label))) return -1;
    gk_relayout(w);
    gk_dirty(w);
    return r->n++;
}

/**** separators and spacers ****/

static void separator_measure(struct gk_widget *w) {
    w->base_w = w->base_h = 1;
}

static void separator_paint(struct gk_widget *w, struct gw *win) {
    if(w->w >= w->h) gw_rect(win, w->x, w->y + w->h / 2, w->w, 1, w->ui->theme.line);
    else gw_rect(win, w->x + w->w / 2, w->y, 1, w->h, w->ui->theme.line);
}

static const struct gk_class separator_class = {
    .measure = separator_measure, .paint = separator_paint,
};

struct gk_widget *gk_separator(struct gk_widget *parent) {
    struct gk_widget *w = gk_make(parent, &separator_class, 0);
    if(w) w->align = GK_FILL;
    return w;
}

static const struct gk_class spacer_class = { 0 };

struct gk_widget *gk_spacer(struct gk_widget *parent) {
    struct gk_widget *w = gk_make(parent, &spacer_class, 0);
    if(w) w->grow_x = w->grow_y = 1;
    return w;
}

/**** status bars ****/

struct status { char *right; };

static void status_measure(struct gk_widget *w) {
    w->base_w = 0;
    w->base_h = GK_STATUS_H;
}

static void status_paint(struct gk_widget *w, struct gw *win) {
    const struct status *s = w->k;
    const struct gk_theme *t = &w->ui->theme;
    const char *right = s->right ? s->right : "";
    /* clear of the window's resize grip when the bar reaches its bottom right corner */
    int grip = w->ui->resizable && w->x + w->w >= w->ui->win->w && w->y + w->h >= w->ui->win->h ? 24 : 10;
    int rx = w->x + w->w - grip - gw_text_width(right);
    gw_rect(win, w->x, w->y, w->w, w->h, t->band);
    gw_rect(win, w->x, w->y, w->w, 1, t->line);
    if(*right) gw_text(win, rx, w->y + 3, right, t->muted);
    gw_text_cut(win, w->x + 10, w->y + 3, w->text ? w->text : "", max(0, (*right ? rx - 8 : rx) - w->x - 10) / 8, t->ink);
}

static void status_destroy(struct gk_widget *w) {
    free(((struct status *)w->k)->right);
}

static void status_set(struct gk_widget *w, const char *text) {
    if(gk_set_str(w, &w->text, text)) gk_dirty(w);      /* its size never changes */
}

static const struct gk_class status_class = {
    .measure = status_measure, .paint = status_paint, .destroy = status_destroy, .set_text = status_set,
};

struct gk_widget *gk_status(struct gk_widget *parent) {
    struct gk_widget *w = gk_make(parent, &status_class, sizeof(struct status));
    if(w) w->grow_x = 1;
    return w;
}

void gk_status_right(struct gk_widget *w, const char *text) {
    if(!w || w->cls != &status_class) return;
    if(gk_set_str(w, &((struct status *)w->k)->right, text)) gk_dirty(w);
}

/**** scroll bars on their own ****/

struct sbar { struct gk_bar b; int grab; };

static void sbar_measure(struct gk_widget *w) {
    w->base_w = w->across ? 48 : GK_SCROLL_W;
    w->base_h = w->across ? GK_SCROLL_W : 48;
}

static void sbar_paint(struct gk_widget *w, struct gw *win) {
    const struct sbar *s = w->k;
    gk_bar_paint(win, &w->ui->theme, w->x, w->y, w->across ? w->w : w->h, w->across, &s->b);
}

static void sbar_moved(struct gk_widget *w) {
    struct sbar *s = w->k;
    w->value = s->b.first;
    gk_dirty(w);
    gk_post(w->ui, GK_EV_CHANGE, w, w->value);
}

static void sbar_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    struct sbar *s = w->k;
    int len = w->across ? w->w : w->h, pos = w->across ? x - w->x : y - w->y;
    (void)released;
    if(pressed & WIN_BUTTON_LEFT) {
        if(gk_bar_press(len, pos, &s->b, &s->grab)) sbar_moved(w);
    } else if((buttons & WIN_BUTTON_LEFT) && gk_bar_drag(len, pos, &s->b, s->grab)) sbar_moved(w);
    if(!(buttons & WIN_BUTTON_LEFT)) s->grab = -1;
}

static int sbar_wheel(struct gk_widget *w, int x, int y, int rows) {
    struct sbar *s = w->k;
    (void)x;
    (void)y;
    if(!gk_bar_max(&s->b)) return 0;
    if(set_first(&s->b, s->b.first + rows)) sbar_moved(w);
    return 1;
}

static void sbar_set(struct gk_widget *w, int value) {
    struct sbar *s = w->k;
    if(set_first(&s->b, value)) {
        w->value = s->b.first;
        gk_dirty(w);
    }
}

static const struct gk_class sbar_class = {
    .measure = sbar_measure, .paint = sbar_paint, .pointer = sbar_pointer, .wheel = sbar_wheel, .set_value = sbar_set,
};

struct gk_widget *gk_scrollbar(struct gk_widget *parent, int id, int across) {
    struct gk_widget *w = gk_make(parent, &sbar_class, sizeof(struct sbar));
    if(!w) return NULL;
    w->id = id;
    w->across = across != 0;
    ((struct sbar *)w->k)->grab = -1;
    if(across) w->grow_x = 1;
    else w->grow_y = 1;
    return w;
}

void gk_scroll_set(struct gk_widget *w, int total, int shown, int first) {
    struct sbar *s;
    if(!w || w->cls != &sbar_class) return;
    s = w->k;
    s->b.total = total > 0 ? total : 0;
    s->b.shown = shown > 0 ? shown : 0;
    s->b.first = 0;
    set_first(&s->b, first);
    w->value = s->b.first;
    gk_dirty(w);
}

/**** tabs ****/

#define TAB_CELLS 24                    /* a label's cells at most: a longer one ends in a dot */
#define TAB_ARROWS 36                   /* the two arrows when the tabs do not all fit */

struct tabs { char **label; int n, cap, first; };   /* the labels; the first tab shown */

static int tab_w(const struct gk_widget *w, int i) {
    const struct tabs *t = w->k;
    return min(gw_text_width(t->label[i]) / 8, TAB_CELLS) * 8 + 14 + ((w->opts & GK_CLOSABLE) ? 14 : 0);
}

/* the tabs that fit from first on: the last one shown, and whether the arrows are needed */
static int tabs_fit(const struct gk_widget *w, int *arrows) {
    const struct tabs *t = w->k;
    int i, at = 8, room = w->w - 8, last = t->first - 1, all = 8;
    for(i = 0; i < t->n; i++) all += tab_w(w, i) + 4;
    *arrows = t->first > 0 || all > room;
    if(*arrows) room -= TAB_ARROWS;
    for(i = t->first; i < t->n; i++) {
        if(at + tab_w(w, i) > room && i > t->first) break;
        at += tab_w(w, i) + 4;
        last = i;
    }
    return last;
}

/* the page shown has its tab in view */
static void tabs_reveal(struct gk_widget *w) {
    struct tabs *t = w->k;
    int arrows;
    if(w->value < t->first) t->first = w->value;
    while(t->first < w->value && tabs_fit(w, &arrows) < w->value) t->first++;
    if(t->first < 0) t->first = 0;
}

static void tabs_mark(struct gk_widget *w) {
    struct gk_widget *c;
    int i = 0;
    for(c = w->child; c; c = c->next, i++) {
        if(i == w->value) c->flags &= ~GKW_AWAY;
        else c->flags |= GKW_AWAY;
    }
}

static void tabs_measure(struct gk_widget *w) {
    struct gk_widget *c;
    for(c = w->child; c; c = c->next) {
        if(c->flags & GKW_HIDDEN) continue;
        w->base_w = max(w->base_w, c->nat_w);
        w->base_h = max(w->base_h, c->nat_h);
    }
    w->base_h += GK_TABS_H;
}

static void tabs_arrange(struct gk_widget *w) {
    struct gk_widget *c;
    tabs_mark(w);
    tabs_reveal(w);
    for(c = w->child; c; c = c->next)
        if(!(c->flags & GKW_HIDDEN)) gk_lay(c, w->x, w->y + GK_TABS_H, w->w, w->h - GK_TABS_H);
}

static void tabs_paint(struct gk_widget *w, struct gw *win) {
    const struct tabs *tb = w->k;
    const struct gk_theme *t = &w->ui->theme;
    int off = (w->flags & GKW_DISABLED) != 0, arrows, last = tabs_fit(w, &arrows), i, at = w->x + 8, tw, on;
    gw_rect(win, w->x, w->y, w->w, GK_TABS_H - 1, t->band);
    gw_rect(win, w->x, w->y + GK_TABS_H - 1, w->w, 1, t->line);
    for(i = tb->first; i <= last; at += tw + 4, i++) {
        tw = tab_w(w, i);
        on = i == w->value;
        gw_rect(win, at, w->y + 4, tw, 22, off ? t->band : on ? t->teal : t->shell);
        gw_text_cut(win, at + 7, w->y + 7, tb->label[i], TAB_CELLS, off ? t->grey : on ? t->on_teal : t->ink);
        if(w->opts & GK_CLOSABLE) gw_text(win, at + tw - 14, w->y + 7, "x", on && !off ? t->foam : t->muted);
        if(on && gk_lit(w)) gw_frame(win, at - 2, w->y + 2, tw + 4, 26, t->frame);
    }
    if(arrows) {                                        /* ◄ ► at the right: more tabs that way */
        at = w->x + w->w - TAB_ARROWS;
        gw_rect(win, at, w->y + 4, 16, 22, tb->first > 0 ? t->shell : t->band);
        gw_glyph(win, at + 4, w->y + 7, 0x11, tb->first > 0 ? t->ink : t->muted, 1);
        gw_rect(win, at + 18, w->y + 4, 16, 22, last < tb->n - 1 ? t->shell : t->band);
        gw_glyph(win, at + 22, w->y + 7, 0x10, last < tb->n - 1 ? t->ink : t->muted, 1);
    }
}

static void tabs_choose(struct gk_widget *w, int i) {
    struct tabs *t = w->k;
    if(i < 0 || i >= t->n || i == w->value) return;
    w->value = i;
    tabs_mark(w);
    tabs_reveal(w);
    gk_dirty(w);
    gk_post(w->ui, GK_EV_CHANGE, w, i);
}

static int tabs_key(struct gk_widget *w, unsigned key, unsigned mods) {
    if(mods & (WIN_MOD_CTRL | WIN_MOD_ALT)) return 0;
    if((mods & WIN_MOD_REPEAT) && key >= WIN_KEY_LEFT && key <= WIN_KEY_END) return 1;   /* once a press */
    if(key == WIN_KEY_LEFT) tabs_choose(w, w->value - 1);
    else if(key == WIN_KEY_RIGHT) tabs_choose(w, w->value + 1);
    else if(key == WIN_KEY_HOME) tabs_choose(w, 0);
    else if(key == WIN_KEY_END) tabs_choose(w, ((struct tabs *)w->k)->n - 1);
    else return 0;
    return 1;
}

static void tabs_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    struct tabs *t = w->k;
    int arrows, last = tabs_fit(w, &arrows), i, at = w->x + 8, tw;
    (void)buttons;
    (void)released;
    if(!(pressed & WIN_BUTTON_LEFT) || y < w->y + 4 || y >= w->y + 26) return;
    if(arrows && x >= w->x + w->w - TAB_ARROWS) {
        if(x < w->x + w->w - TAB_ARROWS + 16) {
            if(t->first > 0) t->first--;
        } else if(last < t->n - 1) t->first++;
        gk_dirty(w);
        return;
    }
    for(i = t->first; i <= last; at += tw + 4, i++) {
        tw = tab_w(w, i);
        if(x < at || x >= at + tw) continue;
        if((w->opts & GK_CLOSABLE) && x >= at + tw - 16) gk_post(w->ui, GK_EV_TAB_CLOSE, w, i);
        else tabs_choose(w, i);
        return;
    }
}

static const char *tabs_text(const struct gk_widget *w) {
    const struct tabs *t = w->k;
    return w->value >= 0 && w->value < t->n ? t->label[w->value] : "";
}

static void tabs_set(struct gk_widget *w, int value) {
    struct tabs *t = w->k;
    if(value < 0 || value >= t->n || value == w->value) return;
    w->value = value;
    tabs_mark(w);
    tabs_reveal(w);
    gk_dirty(w);
}

static void tabs_destroy(struct gk_widget *w) {
    struct tabs *t = w->k;
    int i;
    for(i = 0; i < t->n; i++) free(t->label[i]);
    free(t->label);
}

/* the focus changes the strip alone */
static void tabs_lit(const struct gk_widget *w, struct gk_rect *r) {
    r->x = w->x;
    r->y = w->y;
    r->w = w->w;
    r->h = GK_TABS_H;
}

static const struct gk_class tabs_class = {
    .takes_focus = 1, .measure = tabs_measure, .arrange = tabs_arrange, .paint = tabs_paint, .key = tabs_key,
    .pointer = tabs_pointer, .get_text = tabs_text, .set_value = tabs_set, .destroy = tabs_destroy,
    .lit_part = tabs_lit,
};

struct gk_widget *gk_tabs(struct gk_widget *parent, int id, unsigned flags) {
    struct gk_widget *w = gk_make(parent, &tabs_class, sizeof(struct tabs));
    if(!w) return NULL;
    w->id = id;
    w->opts = flags & GK_CLOSABLE;
    w->grow_x = w->grow_y = 1;
    return w;
}

struct gk_widget *gk_tab(struct gk_widget *w, const char *label) {
    struct tabs *t;
    struct gk_widget *page;
    char **grown, *copy;
    if(!w || w->cls != &tabs_class) return NULL;
    t = w->k;
    if(t->n == t->cap) {
        if(!(grown = realloc(t->label, (size_t)(t->cap ? t->cap * 2 : 8) * sizeof(*grown)))) {
            w->ui->failed = 1;
            return NULL;
        }
        t->label = grown;
        t->cap = t->cap ? t->cap * 2 : 8;
    }
    if(!(copy = gk_strdup(w->ui, label))) return NULL;
    if(!(page = gk_vbox(w, 0))) {
        free(copy);
        return NULL;
    }
    t->label[t->n++] = copy;
    tabs_mark(w);
    gk_dirty(w);
    return page;
}

void gk_tab_label(struct gk_widget *w, int i, const char *label) {
    struct tabs *t;
    if(!w || w->cls != &tabs_class) return;
    t = w->k;
    if(i >= 0 && i < t->n && gk_set_str(w, &t->label[i], label)) gk_dirty(w);
}

int gk_tab_count(const struct gk_widget *w) {
    return w && w->cls == &tabs_class ? ((const struct tabs *)w->k)->n : 0;
}

void gk_tab_remove(struct gk_widget *w, int i) {
    struct tabs *t;
    struct gk_widget *page;
    int k;
    if(!w || w->cls != &tabs_class || w->ui->painting) return;
    t = w->k;
    if(i < 0 || i >= t->n) return;
    for(page = w->child, k = 0; page && k < i; k++) page = page->next;
    free(t->label[i]);
    memmove(t->label + i, t->label + i + 1, (size_t)(t->n - i - 1) * sizeof(*t->label));
    t->n--;
    if(w->value > i || w->value >= t->n) w->value = w->value > 0 ? w->value - 1 : 0;
    if(page) gk_remove(page);
    if(t->first >= t->n) t->first = t->n > 0 ? t->n - 1 : 0;
    tabs_mark(w);
    gk_dirty(w);
}
