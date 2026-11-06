/*
SurfOS user program: wswell
--------------------
File: window.c  Date: 11/05/26
--------------------
Swell's window (window.h), built on the toolkit (gk.h): the board is an area this file draws, the
sea at its left and the panel at its right (the next piece, the score, the rows, the level, the
time, the session's best); under it the status bar says what happened, or how to play.

The look is SurfOS's own: the sea is deep water with a faint grid, its pieces the desktop's sea
colours (sand, sky, coral, foam, kelp, lilac, sea green), each cell a block with a light crest and
a dark foot; sand the swell brought up is darker and speckled; where the falling piece would come
to rest is its outline; rows washed away show foam for a moment. A band across the sea says when
the game waits to start, is paused, or is over.

Nothing is drawn but what changed: the window keeps each cell's look as it shows it, and the
panel's numbers, and marks only what changed (gk_damage()), so a piece falling a row presents its
cells, and the clock its line. One timer of the toolkit wakes the game when it has something to
do (the next row, the end of a rest, the end of the washed rows' moment, the next second shown).
The board has the keys; losing them (another window in front, a question over this one) pauses
the game. The Game menu (in the desktop's bar while the window has the focus, by its shortcuts in
any case) has New game (Ctrl+N), Pause (P, checked while paused) and Close (Ctrl+W); Help has a
line for the desktop's Help. The close button, Esc and Ctrl+W ask first while a game is under
way, Cancel first, the game paused meanwhile. A click on the board starts a game that waits, or a
new one once it is over; a pause goes on only by a key, so a click that brings the window back
lets nothing fall.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <surf.h>
#include "window.h"

#define C_WATER  0x0F2F3B               /* the sea */
#define C_GRID   0x15394A               /* its grid */
#define C_SAND   0x9C8452               /* sand the swell brought up */
#define C_SPECK  0x7E6A40
#define C_FLASH  0xE8F6F1               /* a row washed away, for a moment */
#define C_BANNER 0x0A2029               /* the band across the sea */

/* the pieces' colours, by kind: pebble, drift, shell, crest, reef, gull, kelp */
static const unsigned colors[SW_KINDS] = { 0xD0B056, 0x659ECB, 0xEE906D, 0xB8E9D9, 0x7ED48C, 0xB48CC8, 0x3FB8AF };
static const char *const panel_names[SW_PANEL_LINES] = { "Score", "Rows", "Level", "Time", "Best" };

/**** drawing ****/

static unsigned shade(unsigned c, int up) {         /* a channel halfway to white, or two thirds of itself */
    unsigned r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
    if(up) { r += (255 - r) / 2; g += (255 - g) / 2; b += (255 - b) / 2; }
    else { r = r * 2 / 3; g = g * 2 / 3; b = b * 2 / 3; }
    return r << 16 | g << 8 | b;
}

static void block(struct gw *win, int x, int y, int s, unsigned c) {
    gw_rect(win, x + 1, y + 1, s - 2, s - 2, c);
    gw_rect(win, x + 1, y + 1, s - 2, 1, shade(c, 1));
    gw_rect(win, x + 1, y + 1, 1, s - 2, shade(c, 1));
    gw_rect(win, x + 1, y + s - 2, s - 2, 1, shade(c, 0));
    gw_rect(win, x + s - 2, y + 2, 1, s - 3, shade(c, 0));
}

/* a cell's look as the window shows it: water 0, a piece's kind + 1, sand, the ghost of the falling piece's kind
   (SV_GHOST + its kind, so a new piece's ghost is painted again), and SW_FLASHED while its row shows washed away */
static int look_of(const struct swwin *w, int x, int y, unsigned now) {
    int c = sview_cell(&w->v, x, y);
    if(c == SV_GHOST) c += w->v.game.kind;
    if(sview_flashing(&w->v, y, now)) c |= SW_FLASHED;
    return c;
}

static void cell(struct gw *win, const struct swwin *w, int cx, int cy, int look) {
    int s = w->v.cell, x = w->v.sx + cx * s, y = w->v.sy + cy * s;
    gw_rect(win, x, y, s, s, C_GRID);
    gw_rect(win, x + 1, y + 1, s - 1, s - 1, C_WATER);
    if(look & SW_FLASHED) gw_rect(win, x, y, s, s, C_FLASH);
    else if(look >= 1 && look <= SW_KINDS) block(win, x, y, s, colors[look - 1]);
    else if(look == SW_SAND) {
        block(win, x, y, s, C_SAND);
        gw_rect(win, x + s / 3, y + s / 3, 1, 1, C_SPECK);
        gw_rect(win, x + 2 * s / 3, y + s / 2, 1, 1, C_SPECK);
        gw_rect(win, x + s / 2, y + 3 * s / 4, 1, 1, C_SPECK);
    } else if(look >= SV_GHOST && look < SV_GHOST + SW_KINDS) gw_frame(win, x + 2, y + 2, s - 4, s - 4, colors[look - SV_GHOST]);
}

/* the panel's numbers, as text */
static void panel_text(const struct swwin *w, int i, unsigned now, char *out, int n) {
    const struct swell *s = &w->v.game;
    char t[24];
    switch(i) {
    case 0: snprintf(out, (size_t)n, "%u", s->score); break;
    case 1: snprintf(out, (size_t)n, "%d", s->rows); break;
    case 2: snprintf(out, (size_t)n, "%d", s->level); break;
    case 3: sview_time(swell_ms(s, now), t, sizeof(t)); snprintf(out, (size_t)n, "%s", t); break;
    default: snprintf(out, (size_t)n, "%u", w->v.best); break;
    }
}

/* the panel's parts: the next piece's box, and each number's line */
static int preview_cell(const struct swwin *w) {
    return w->v.cell < 18 ? w->v.cell : 18;
}

static void preview_box(const struct swwin *w, int *x, int *y, int *size) {
    *size = 4 * preview_cell(w) + 8;
    *x = w->v.px;
    *y = w->v.py + 20;
}

static int line_y(const struct swwin *w, int i) {
    int x, y, size;
    preview_box(w, &x, &y, &size);
    return y + size + 12 + i * 36;
}

static void draw_panel(struct gw *win, const struct swwin *w, const struct gk_theme *t, unsigned now) {
    const struct swell_piece *p = swell_shape(w->v.game.next, 0);
    char b[24];
    int x, y, size, pc = preview_cell(w), i, ox, oy;
    gw_text(win, w->v.px, w->v.py, "Next", t->muted);
    preview_box(w, &x, &y, &size);
    gw_rect(win, x, y, size, size, t->line);
    gw_rect(win, x + 1, y + 1, size - 2, size - 2, C_WATER);
    ox = x + (size - p->w * pc) / 2;
    oy = y + (size - p->h * pc) / 2;
    for(i = 0; i < p->n; i++) block(win, ox + p->x[i] * pc, oy + p->y[i] * pc, pc, colors[w->v.game.next]);
    for(i = 0; i < SW_PANEL_LINES; i++) {
        y = line_y(w, i);
        gw_text(win, w->v.px, y, panel_names[i], t->muted);
        panel_text(w, i, now, b, sizeof(b));
        gw_text(win, w->v.px, y + 16, b, t->ink);
    }
}

static void banner(struct gw *win, const struct swwin *w) {
    static const char *const says[] = { "Enter starts", "", "Paused", "Over the top" };
    const char *s = says[w->v.game.state & 3];
    int sea_w = SW_W * w->v.cell, y = w->v.sy + SW_H * w->v.cell / 2 - 20;
    if(!*s) return;
    gw_rect(win, w->v.sx, y, sea_w, 40, C_BANNER);
    gw_text(win, w->v.sx + (sea_w - gw_text_width(s)) / 2, y + 12, s, C_FLASH);
}

/* the sea laid out for the board as it is now: 1 when that changed */
static int fit(struct swwin *w, int bw, int bh) {
    if(bw == w->bw && bh == w->bh) return 0;
    w->bw = bw;
    w->bh = bh;
    sview_layout(&w->v, bw, bh);
    return 1;
}

static void draw(struct gk_widget *area, struct gw *win, const struct gk_view *vw, void *data) {
    struct swwin *w = data;
    const struct gk_theme *t = gk_theme(w->ui);
    unsigned now = uptime();
    int s, x, y, x0, y0, x1, y1;
    (void)area;
    fit(w, vw->w, vw->h);
    s = w->v.cell;
    gw_rect(win, vw->cx, vw->cy, vw->cw, vw->ch, t->paper);
    gw_frame(win, w->v.sx - 2, w->v.sy - 2, SW_W * s + 4, SW_H * s + 4, t->frame);
    gw_frame(win, w->v.sx - 1, w->v.sy - 1, SW_W * s + 2, SW_H * s + 2, C_GRID);
    x0 = vw->cx > w->v.sx ? (vw->cx - w->v.sx) / s : 0;  /* the cells that meet the part painted */
    y0 = vw->cy > w->v.sy ? (vw->cy - w->v.sy) / s : 0;
    x1 = vw->cx + vw->cw - 1 < w->v.sx ? -1 : (vw->cx + vw->cw - 1 - w->v.sx) / s;
    y1 = vw->cy + vw->ch - 1 < w->v.sy ? -1 : (vw->cy + vw->ch - 1 - w->v.sy) / s;
    for(y = y0; y <= y1 && y < SW_H; y++) for(x = x0; x <= x1 && x < SW_W; x++) cell(win, w, x, y, look_of(w, x, y, now));
    banner(win, w);
    if(vw->cx + vw->cw > w->v.px) draw_panel(win, w, t, now);
}

/**** what changed ****/

static void set_text(struct gk_widget *g, const char *s) {
    if(strcmp(gk_text(g), s)) gk_set_text(g, s);
}

static void refresh(struct swwin *w, unsigned now) {
    struct gk_view vw;
    char b[SV_STATUS + 16];
    int x, y, look, i, all, s, size;
    unsigned wait;
    gk_area_view(w->sea, &vw);
    all = fit(w, vw.w, vw.h);
    if(all) gk_redraw(w->sea);
    s = w->v.cell;
    if(w->shown_state != w->v.game.state) {             /* the band came or went: the sea again */
        w->shown_state = w->v.game.state;
        if(!all) gk_damage(w->sea, w->v.sx, w->v.sy, SW_W * s, SW_H * s);
    }
    for(y = 0; y < SW_H; y++) for(x = 0; x < SW_W; x++) {
        look = look_of(w, x, y, now);
        if(look == w->shown[y][x] && !all) continue;
        w->shown[y][x] = (unsigned char)look;
        if(!all) gk_damage(w->sea, w->v.sx + x * s, w->v.sy + y * s, s, s);
    }
    if(w->shown_next != w->v.game.next) {
        w->shown_next = w->v.game.next;
        preview_box(w, &x, &y, &size);
        if(!all) gk_damage(w->sea, x, y, size, size);
    }
    for(i = 0; i < SW_PANEL_LINES; i++) {
        char line[sizeof(w->panel[0])];
        panel_text(w, i, now, line, sizeof(line));
        if(!strcmp(line, w->panel[i]) && !all) continue;
        memcpy(w->panel[i], line, sizeof(line));
        if(!all) gk_damage(w->sea, w->v.px, line_y(w, i) + 16, SV_PANEL, 16);
    }
    sview_status(&w->v, b, sizeof(b));
    set_text(w->status, b);
    gk_menu_check(w->ui, MENU_PAUSE, w->v.game.state == SW_PAUSED);
    gk_menu_enable(w->ui, MENU_PAUSE, w->v.game.state == SW_PLAYING || w->v.game.state == SW_PAUSED);
    wait = sview_wait(&w->v, now);
    if(wait == 0xFFFFFFFFu) gk_timer_stop(w->ui, TIMER_SEA);
    else gk_timer(w->ui, TIMER_SEA, wait < 1 ? 1 : wait > 60000 ? 60000 : (int)wait, 0);
}

static void ask(struct swwin *w) {
    gk_confirm(w->ui, BOX_CLOSE, "Close Swell?", "A game is under way: it ends with the window.", "", "Cancel",
               "Close", NULL);
}

static int closing(struct swwin *w, unsigned now) {
    if(sview_close(&w->v, now) == SV_CLOSE) return 0;
    ask(w);
    return 1;
}

int ww_event(struct swwin *w, const struct gk_event *ev) {
    unsigned now = uptime();
    switch(ev->type) {
    case GK_EV_CLOSE:
        if(!closing(w, now)) return 0;
        break;
    case GK_EV_ANSWER:
        if(ev->id == BOX_CLOSE && sview_answer(&w->v, ev->value == 1) == SV_CLOSE) return 0;
        break;
    case GK_EV_MENU:
        switch(ev->id) {
        case MENU_NEW: sview_restart(&w->v, now); break;
        case MENU_PAUSE: sview_key(&w->v, 'p', 0, now); break;
        case MENU_CLOSE: if(!closing(w, now)) return 0; break;
        case MENU_HELP:
            gk_message(w->ui, BOX_HELP, "Playing Swell", "Fill rows across the sea: full ones wash away.",
                       "Arrows move, Up turns, Space drops, P pauses.");
            break;
        }
        break;
    case GK_EV_KEY:
        if(ev->w && ev->w != w->sea) break;
        if(sview_key(&w->v, ev->value, ev->mods, now) == SV_CLOSE) return 0;
        if(w->v.asking && !gk_box_open(w->ui)) ask(w);  /* Esc or Ctrl+W during a game */
        break;
    case GK_EV_POINTER:                                 /* a click: a game that waits, or a new one after the end */
        if(ev->w == w->sea && (ev->value & WIN_BUTTON_LEFT) &&
           (w->v.game.state == SW_READY || w->v.game.state == SW_OVER)) sview_key(&w->v, '\r', 0, now);
        break;
    case GK_EV_TIMER:
        if(ev->id == TIMER_SEA) sview_tick(&w->v, now);
        break;
    case GK_EV_FOCUS:
        if(ev->w == w->sea) sview_focus(&w->v, ev->value, now);
        break;
    case GK_EV_THEME:
        gk_redraw(w->sea);
        break;
    }
    refresh(w, now);
    return 1;
}

int ww_open(struct swwin *w, unsigned seed) {
    struct gk_widget *root;
    int width, height, i;
    memset(w, 0, sizeof(*w));
    sview_init(&w->v, seed);
    sview_window(0, 0, &width, &height);
    w->ui = gk_open_limits("Swell", width, height, SW_W * SV_CELL_MIN + SV_PANEL + 3 * SV_MARGIN,
                           SW_H * SV_CELL_MIN + 2 * SV_MARGIN + SV_STATUS_H, 0, 0, WIN_RESIZABLE);
    if(!w->ui) return -1;
    root = gk_root(w->ui);
    w->sea = gk_area(root, ID_SEA, GK_FOCUSABLE, draw, w);
    w->status = gk_status(root);
    i = gk_menu(w->ui, "Game");
    gk_menu_item(w->ui, i, MENU_NEW, "New game", GK_CTRL | 'N', 0);
    gk_menu_item(w->ui, i, MENU_PAUSE, "Pause", 'P', GK_ITEM_CHECK | GK_ITEM_OFF);
    gk_menu_line(w->ui, i);
    gk_menu_item(w->ui, i, MENU_CLOSE, "Close", GK_CTRL | 'W', 0);
    i = gk_menu(w->ui, "Help");
    gk_menu_item(w->ui, i, MENU_HELP, "Playing Swell", 0, 0);
    gk_focus(w->sea);
    w->shown_state = -1;
    w->shown_next = -1;
    refresh(w, uptime());
    if(!gk_ok(w->ui)) {
        gk_close(w->ui);
        w->ui = NULL;
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void ww_close(struct swwin *w) {
    gk_close(w->ui);
    w->ui = NULL;
}
