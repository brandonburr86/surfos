/*
SurfOS user program: wmines
--------------------
File: window.c  Date: 11/05/26
--------------------
Mines' window (window.h), built on the toolkit (gk.h):

    the toolbar   New, the field's size (Small, Medium, Large), the mines left and the time
    the board     an area this file draws: the field in the middle, its cells as large as the
                  room allows (view.c's layout); it has the keys
    the status    what the last move did, or how to play; at its right the session's record of
                  the field

The look is SurfOS's own: covered cells are sand, open ones the paper of the desktop's windows,
the numbers in the desktop's sea colours, a flag a teal pennant, a mine a navy depth charge (an
upright drum with two teal hoops and a stub on top), and the one that went off a splash: the drum
in a ring of foam on sky blue (the review of 11/05/26 asked for a mine of SurfOS's own: the round
mine with spikes, a square glint and a red cell were another game's). Nothing is drawn but what
changed: the window keeps each cell's look as it shows it, and after every event marks only the
cells whose look changed (gk_damage()), so a click presents a few cells, and the timer presents
its label and nothing else.

The Game menu (in the desktop's bar while the window has the focus, by its shortcuts in any case)
has New field (Ctrl+N), the three sizes (1 to 3) and Close (Ctrl+W); Help has a line for the
desktop's Help. The close button, Esc and Ctrl+W, and a new field however it is asked for (N, 1
to 3, New, the sizes' buttons, the menu), ask first while a game is under way, Cancel first:
view.c says which question, and the box on the screen follows it (a new box takes the place of
one open, which is answered as cancelled). The toolkit's timer moves the clock on the second
while a game runs.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <surf.h>
#include "window.h"

/* the board's own colours, from the desktop's palette */
#define C_COVER  0xDCCB9E               /* sand: a covered cell */
#define C_PRESS  0xC5B282               /* and pressed */
#define C_OPEN   0xF7F3E8               /* an open cell */
#define C_SPLASH 0x659ECB               /* sky: the cell of the mine that went off */
#define C_RING   0xE8F6F1               /* foam: the splash's ring */
#define C_MINE   0x102E3B               /* navy: a mine */
#define C_HOOP   0x197D83               /* teal: its hoops */
#define C_POLE   0x153D49               /* ink: a flag's pole */
#define C_FLAG   0x197D83               /* teal: its pennant */
#define C_WRONG  0xC8553D               /* a flag where no mine was: crossed out */
#define C_CURSOR 0xE0603F               /* the keyboard's cursor */

/* the numbers' colours: teal, sky, coral, navy, lilac, kelp, ink, muted */
static const unsigned digits[9] = { 0, 0x197D83, 0x3A78B5, 0xC8553D, 0x102E3B, 0x8A4F9E, 0x2E8B6E, 0x153D49,
                                    0x6C888C };
static const char *const size_labels[MINES_SIZES] = { "Small field (10 by 8)", "Medium field (16 by 12)",
                                                      "Large field (28 by 15)" };

/**** drawing ****/

static int isqrt(int n) {
    int r = 0;
    while((r + 1) * (r + 1) <= n) r++;
    return r;
}

static void disc(struct gw *win, int cx, int cy, int r, unsigned color) {
    int dy, half;
    for(dy = -r; dy <= r; dy++) {
        half = isqrt(r * r - dy * dy);
        gw_rect(win, cx - half, cy + dy, 2 * half + 1, 1, color);
    }
}

/* a depth charge: an upright drum, its corners cut, two teal hoops round it and a stub on top */
static void mine(struct gw *win, int x, int y, int s) {
    int w = s * 2 / 5, h = s / 2, left = x + (s - w) / 2, top = y + (s - h) / 2 + s / 16, band = s >= 30 ? 2 : 1;
    gw_rect(win, left + 1, top, w - 2, h, C_MINE);
    gw_rect(win, left, top + 1, w, h - 2, C_MINE);
    gw_rect(win, left, top + h / 4, w, band, C_HOOP);
    gw_rect(win, left, top + h - h / 4 - band, w, band, C_HOOP);
    gw_rect(win, x + s / 2 - band, top - s / 8, 2 * band, s / 8, C_MINE);
}

/* the mine that went off: a ring of foam round it, on the sky blue of its cell */
static void splash(struct gw *win, int x, int y, int s) {
    int cx = x + s / 2, cy = y + s / 2, r = s * 2 / 5;
    disc(win, cx, cy, r, C_RING);
    disc(win, cx, cy, r - (s >= 30 ? 3 : 2), C_SPLASH);
}

/* a pennant on a pole; a wrong one crossed out */
static void flag(struct gw *win, int x, int y, int s, int wrong) {
    int px = x + s * 3 / 8, top = y + s / 5, bottom = y + s - s / 5, h = s * 3 / 10, w = s * 2 / 5, half = h / 2, i, d;
    gw_rect(win, px, top, 2, bottom - top, C_POLE);
    gw_rect(win, px - s / 6, bottom - 2, s / 3 + 2, 2, C_POLE);
    for(i = 0; i < h; i++) {
        d = i < half ? i : h - 1 - i;
        gw_rect(win, px + 2, top + i, 1 + (d + 1) * w / (half + 1), 1, C_FLAG);
    }
    if(!wrong) return;
    for(i = 0; i < 2; i++) {
        gw_line(win, x + 3 + i, y + 3, x + s - 3 + i, y + s - 3, C_WRONG);
        gw_line(win, x + s - 3 + i, y + 3, x + 3 + i, y + s - 3, C_WRONG);
    }
}

/* a cell's look as the window shows it: view.c's, and the cursor where the keys would act */
static int look_of(const struct mwin *m, int x, int y) {
    const struct mview *v = &m->v;
    return mview_look(v, x, y) | (v->cursor && m->keys && x == v->cx && y == v->cy ? MW_CURSOR : 0);
}

static void cell(struct gw *win, const struct mwin *m, int cx, int cy, int look) {
    const struct mview *v = &m->v;
    int s = v->cell, x = v->fx + cx * s, y = v->fy + cy * s, base = look & ~MW_CURSOR, scale = s >= 34 ? 2 : 1;
    unsigned bg = base == MV_PRESSED ? C_PRESS : base == MV_BLAST ? C_SPLASH : base >= MV_OPEN && base <= MV_MINE ? C_OPEN :
                  C_COVER;
    char digit[2];
    gw_rect(win, x + 1, y + 1, s - 1, s - 1, bg);   /* the grid's line is its left and top */
    if(base > MV_OPEN && base < MV_MINE) {
        digit[0] = (char)('0' + base - MV_OPEN);
        digit[1] = 0;
        gw_text_scaled(win, x + 1 + (s - 1 - 8 * scale) / 2, y + 1 + (s - 1 - 16 * scale) / 2, digit,
                       digits[base - MV_OPEN], scale);
    } else if(base == MV_MINE || base == MV_BLAST) {
        if(base == MV_BLAST) splash(win, x, y, s);
        mine(win, x, y, s);
    }
    else if(base == MV_FLAG || base == MV_WRONG) flag(win, x, y, s, base == MV_WRONG);
    if(look & MW_CURSOR) {
        gw_frame(win, x + 1, y + 1, s - 1, s - 1, C_CURSOR);
        gw_frame(win, x + 2, y + 2, s - 3, s - 3, C_CURSOR);
    }
}

/* the field laid out for the board as it is now (it may have a new size, or the field may be another) */
static int fit(struct mwin *m, int w, int h) {
    int moved;
    if(w == m->bw && h == m->bh && !m->relayout) return 0;
    moved = mview_layout(&m->v, w, h) || m->relayout;
    m->bw = w;
    m->bh = h;
    m->relayout = 0;
    return moved;
}

/* the board: what is to be painted of it, the cells that meet that part */
static void draw(struct gk_widget *area, struct gw *win, const struct gk_view *vw, void *data) {
    struct mwin *m = data;
    const struct gk_theme *t = gk_theme(m->ui);
    const struct mview *v = &m->v;
    int s, x0, y0, x1, y1, x, y;
    (void)area;
    fit(m, vw->w, vw->h);
    s = v->cell;
    gw_rect(win, vw->cx, vw->cy, vw->cw, vw->ch, t->paper);
    gw_rect(win, v->fx, v->fy, v->game.w * s + 1, v->game.h * s + 1, t->line);
    if(vw->cx + vw->cw <= v->fx || vw->cy + vw->ch <= v->fy) return;
    x0 = vw->cx > v->fx ? (vw->cx - v->fx) / s : 0;
    y0 = vw->cy > v->fy ? (vw->cy - v->fy) / s : 0;
    x1 = (vw->cx + vw->cw - 1 - v->fx) / s;
    y1 = (vw->cy + vw->ch - 1 - v->fy) / s;
    if(x1 >= v->game.w) x1 = v->game.w - 1;
    if(y1 >= v->game.h) y1 = v->game.h - 1;
    for(y = y0; y <= y1; y++) for(x = x0; x <= x1; x++) cell(win, m, x, y, look_of(m, x, y));
}

/**** what changed ****/

static void set_text(struct gk_widget *w, const char *s) {
    if(strcmp(gk_text(w), s)) gk_set_text(w, s);
}

/* after every event: the cells whose look changed painted again, the counters, the status bar, the size shown in the
   toolbar and the menu, and the clock's next tick while a game runs */
static void refresh(struct mwin *m, unsigned now) {
    struct mview *v = &m->v;
    struct gk_view vw;
    char b[MV_STATUS + 16], t[24];
    int x, y, look, s = v->size, all;
    unsigned ms;
    gk_area_view(m->board, &vw);
    all = fit(m, vw.w, vw.h);
    if(all) gk_redraw(m->board);
    for(y = 0; y < v->game.h; y++) for(x = 0; x < v->game.w; x++) {
        look = look_of(m, x, y);
        if(look == m->shown[y * v->game.w + x] && !all) continue;
        m->shown[y * v->game.w + x] = (unsigned char)look;
        if(!all) gk_damage(m->board, v->fx + x * v->cell, v->fy + y * v->cell, v->cell + 1, v->cell + 1);
    }
    snprintf(b, sizeof(b), "Mines %d", mines_left(&v->game));
    set_text(m->left, b);
    ms = mines_ms(&v->game, now);
    mview_time(ms, t, sizeof(t));
    snprintf(b, sizeof(b), "Time %s", t);
    set_text(m->clock, b);
    mview_status(v, b, sizeof(b));
    set_text(m->status, b);
    mview_record(v, b, sizeof(b));
    gk_status_right(m->status, b);
    if(gk_value(m->size) != s) gk_set_value(m->size, s);
    for(x = 0; x < MINES_SIZES; x++) gk_menu_check(m->ui, MENU_SIZE + x, x == s);
    if(v->game.state == MINES_PLAYING) gk_timer(m->ui, TIMER_CLOCK, (int)(1000 - ms % 1000), 0);
    else gk_timer_stop(m->ui, TIMER_CLOCK);
}

/* a new game of that field asked for, at once or after the question; the board has the keys again (a click on the
   toolbar took them), and the box, if one comes, gives them back to it */
static void new_game(struct mwin *m, int size) {
    if(mview_request(&m->v, size) == MV_RESIZE) m->relayout = 1;
    gk_focus(m->board);
}

/* the question view.c asked for, Cancel first, and the box it is in */
static int box_of(int asking) {
    return asking == MV_ASK_NEW ? BOX_NEW : BOX_CLOSE;
}

static void ask(struct mwin *m) {
    if(m->v.asking == MV_ASK_NEW)
        gk_confirm(m->ui, BOX_NEW, "Start a new field?", "A game is under way: a new field ends it.", "", "Cancel",
                   "New field", NULL);
    else gk_confirm(m->ui, BOX_CLOSE, "Close Mines?", "A game is under way: it ends with the window.", "", "Cancel",
                    "Close", NULL);
}

static void help(struct mwin *m) {
    gk_message(m->ui, BOX_HELP, "Playing Mines", MW_HELP_1, MW_HELP_2);
}

int mw_event(struct mwin *m, const struct gk_event *ev) {
    unsigned now = uptime();
    int r;
    switch(ev->type) {
    case GK_EV_CLOSE:
        if(mview_close(&m->v) == MV_CLOSE) return 0;
        break;
    case GK_EV_ANSWER:                                  /* the question on the screen (an older one's answer is not) */
        if(m->v.asking && ev->id == box_of(m->v.asking)) {
            r = mview_answer(&m->v, ev->value == 1);
            if(r == MV_CLOSE) return 0;
            if(r == MV_RESIZE) m->relayout = 1;
        }
        break;
    case GK_EV_CLICK:
        if(ev->id == ID_NEW) new_game(m, m->v.size);
        break;
    case GK_EV_CHANGE:
        if(ev->id == ID_SIZE) new_game(m, ev->value);
        break;
    case GK_EV_MENU:
        if(ev->id == MENU_NEW) new_game(m, m->v.size);
        else if(ev->id >= MENU_SIZE && ev->id < MENU_SIZE + MINES_SIZES) new_game(m, ev->id - MENU_SIZE);
        else if(ev->id == MENU_CLOSE && mview_close(&m->v) == MV_CLOSE) return 0;
        else if(ev->id == MENU_HELP) help(m);
        break;
    case GK_EV_KEY:
        if(ev->w && ev->w != m->board) break;
        r = mview_key(&m->v, ev->value, ev->mods, now);
        if(r == MV_RESIZE) m->relayout = 1;
        if(r == MV_CLOSE) return 0;
        break;
    case GK_EV_POINTER:
        if(ev->w == m->board) mview_pointer(&m->v, ev->x, ev->y, ev->buttons, (unsigned)ev->value, now);
        break;
    case GK_EV_FOCUS:                                   /* the keys gone (another window, a question): a press too */
        if(ev->w != m->board) break;
        m->keys = ev->value;
        if(!ev->value) mview_cancel(&m->v);
        break;
    case GK_EV_THEME:
        gk_redraw(m->board);
        break;
    }
    if(m->v.asking != m->asked) {                       /* a question asked, or answered: the box follows */
        m->asked = m->v.asking;
        if(m->asked) ask(m);
    }
    refresh(m, now);
    return 1;
}

int mw_open(struct mwin *m, unsigned seed) {
    struct gk_widget *root, *bar;
    int w, h, i;
    memset(m, 0, sizeof(*m));
    mview_init(&m->v, seed);
    mview_window(MINES_LARGE, 0, 0, &w, &h);
    m->ui = gk_open_limits("Mines", w, h, MV_MIN_W, MV_MIN_H, 0, 0, WIN_RESIZABLE);
    if(!m->ui) return -1;
    root = gk_root(m->ui);
    bar = gk_hbox(root, 12);
    gk_pad(bar, 5);
    gk_button(bar, "New", ID_NEW);
    m->size = gk_radio(bar, ID_SIZE, 1);
    for(i = 0; i < MINES_SIZES; i++) gk_radio_add(m->size, mines_sizes[i].name);
    gk_spacer(bar);
    m->left = gk_label(bar, "Mines 10");
    gk_size(m->left, 10 * 8, 0);                        /* room for "Mines -100": flags are not limited */
    m->clock = gk_label(bar, "Time 0:00");
    gk_size(m->clock, 12 * 8, 0);                       /* and "Time 1:02:03" */
    m->board = gk_area(root, ID_BOARD, GK_FOCUSABLE, draw, m);
    m->status = gk_status(root);
    i = gk_menu(m->ui, "Game");
    gk_menu_item(m->ui, i, MENU_NEW, "New field", GK_CTRL | 'N', 0);
    gk_menu_line(m->ui, i);
    for(w = 0; w < MINES_SIZES; w++)
        gk_menu_item(m->ui, i, MENU_SIZE + w, size_labels[w], (unsigned)('1' + w), GK_ITEM_RADIO | (w ? 0 : GK_ITEM_ON));
    gk_menu_line(m->ui, i);
    gk_menu_item(m->ui, i, MENU_CLOSE, "Close", GK_CTRL | 'W', 0);
    i = gk_menu(m->ui, "Help");
    gk_menu_item(m->ui, i, MENU_HELP, "Playing Mines", 0, 0);
    gk_focus(m->board);
    memset(m->shown, 0, sizeof(m->shown));
    refresh(m, uptime());
    if(!gk_ok(m->ui)) {
        gk_close(m->ui);
        m->ui = NULL;
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void mw_close(struct mwin *m) {
    gk_close(m->ui);
    m->ui = NULL;
}
