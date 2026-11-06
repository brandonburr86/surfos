/*
SurfOS user program: wsolitaire
--------------------
File: window.c  Date: 11/05/26
--------------------
Solitaire's window (window.h), built on the toolkit (gk.h):

    the toolbar   New, Undo, drawing one or three, the moves and the time
    the table     an area this file draws: the cards as table.c lays them out; it has the keys
    the status    what the last move did, or how to play; at its right the session's record

The cards are drawn here, in SurfOS's own look: a face is paper with its rank and suit at the top
left (the suits are the console's own characters, code page 437's 3 to 6), its suit again at the
bottom right, and in its middle the suit large, or for a jack, queen or king its letter in a
panel; a back is navy with the desktop's wave mark, ≋, in teal; the table is a deep teal. The
keyboard's card has a coral frame.

Nothing is drawn but what changed: each pile has a hash of what it shows (its cards, how they fan,
the keyboard's card on it, the cards a drag took from it), and a pile whose hash changed is painted
again, with where dragged cards were and are. The Game menu (in the desktop's bar while the window
has the focus, by its shortcuts in any case) has New deal (Ctrl+N), Undo (Ctrl+Z), drawing one or
three (1, 3) and Close (Ctrl+W); Help has a line for the desktop's Help. The close button, Esc and
Ctrl+W ask first while a game is under way, Cancel first. Two timers of the toolkit: the clock on
the second while a game runs, and a card each TB_FINISH_MS while the game finishes by itself.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <surf.h>
#include "window.h"

#define C_TABLE  0x165B5F               /* a deep teal */
#define C_PLACE  0x2E7A7E               /* an empty place's outline, and what is marked in it */
#define C_FACE   0xFFFDF5               /* a card's face */
#define C_EDGE   0x8E9E9A               /* its edge */
#define C_RED    0xC8553D               /* hearts and diamonds */
#define C_BLACK  0x153D49               /* spades and clubs */
#define C_PANEL_RED 0xF6DED4            /* a face card's panel */
#define C_PANEL_BLACK 0xDCE8E6
#define C_BACK   0x102E3B               /* a back: navy */
#define C_WAVE   0x197D83               /* its waves: teal */
#define C_RIM    0xB8E9D9               /* its rim: foam */
#define C_CURSOR 0xE0603F               /* the keyboard's card */

static const unsigned char suit_glyph[4] = { 6, 3, 5, 4 };     /* code page 437: spades, hearts, clubs, diamonds */
static const char *const rank_names[14] = { "", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };

/**** drawing ****/

/* a card's shape: a rectangle with its corners cut by a pixel */
static void shape(struct gw *win, int x, int y, int w, int h, unsigned edge, unsigned inside) {
    gw_rect(win, x + 1, y, w - 2, h, edge);
    gw_rect(win, x, y + 1, w, h - 2, edge);
    gw_rect(win, x + 1, y + 1, w - 2, h - 2, inside);
}

static void back(struct gw *win, int x, int y, int w, int h) {
    struct gw_view keep;
    int i, j;
    shape(win, x, y, w, h, C_RIM, C_BACK);
    gw_get_view(win, &keep);
    gw_clip_to(win, x + 3, y + 3, w - 6, h - 6);
    for(j = y + 3; j < y + h - 3; j += 16)              /* the wave mark, in rows offset by half a mark */
        for(i = x + 3 - ((j - y) / 16 % 2) * 6; i < x + w - 3; i += 12) gw_glyph(win, i, j, 0xF7, C_WAVE, 1);
    gw_set_view(win, &keep);
}

static void face(struct gw *win, int x, int y, int w, int h, unsigned char card) {
    unsigned ink = SOL_RED(card) ? C_RED : C_BLACK;
    int rank = SOL_RANK(card), suit = SOL_SUIT(card), len = (int)strlen(rank_names[rank]), big = w >= 56 ? 2 : 1;
    shape(win, x, y, w, h, C_EDGE, C_FACE);
    gw_text(win, x + 3, y + 2, rank_names[rank], ink);
    gw_glyph(win, x + 3 + len * 8, y + 2, suit_glyph[suit], ink, 1);
    gw_glyph(win, x + w - 11, y + h - 18, suit_glyph[suit], ink, 1);
    if(rank > 10) {                                     /* a face card: its letter large in a panel of its own */
        int px = x + w / 6, py = y + 20, pw = w - 2 * (w / 6), ph = h - 38;
        if(ph < 18) return;
        gw_rect(win, px, py, pw, ph, ink);
        gw_rect(win, px + 1, py + 1, pw - 2, ph - 2, SOL_RED(card) ? C_PANEL_RED : C_PANEL_BLACK);
        gw_text_scaled(win, px + (pw - 8 * big) / 2, py + (ph - 16 * big) / 2, rank_names[rank], ink, big);
    } else {                                            /* the suit large: larger still for an ace */
        int scale = big + (rank == 1 && w >= 56);
        if(h < 22 + 16 * scale) scale = 1;
        gw_glyph(win, x + (w - 8 * scale) / 2, y + (h - 16 * scale) / 2 + 3, suit_glyph[suit], ink, scale);
    }
}

static void card(struct gw *win, const struct table *t, int x, int y, unsigned char c) {
    if(c & SOL_UP) face(win, x, y, t->cw, t->ch, c);
    else back(win, x, y, t->cw, t->ch);
}

/* an empty place: its outline; the stock's shows the wave when the waste can be taken back, a foundation's an A */
static void place(struct gw *win, const struct table *t, int pile, int x, int y) {
    gw_frame(win, x, y, t->cw, t->ch, C_PLACE);
    gw_frame(win, x + 1, y + 1, t->cw - 2, t->ch - 2, C_PLACE);
    if(pile == SOL_STOCK && t->game.pile[SOL_WASTE].n)
        gw_glyph(win, x + (t->cw - 16) / 2, y + (t->ch - 32) / 2, 0xF7, C_PLACE, 2);
    else if(SOL_IS_FOUNDATION(pile)) gw_text_scaled(win, x + (t->cw - 16) / 2, y + (t->ch - 32) / 2, "A", C_PLACE, 2);
}

static void frame(struct gw *win, const struct table *t, int x, int y) {
    gw_frame(win, x - 2, y - 2, t->cw + 4, t->ch + 4, C_CURSOR);
    gw_frame(win, x - 1, y - 1, t->cw + 2, t->ch + 2, C_CURSOR);
}

/* the table laid out for the board as it is now: 1 when that changed */
static int fit(struct swin *s, int w, int h) {
    if(w == s->bw && h == s->bh) return 0;
    s->bw = w;
    s->bh = h;
    table_layout(&s->t, w, h);
    return 1;
}

static void draw(struct gk_widget *area, struct gw *win, const struct gk_view *vw, void *data) {
    struct swin *s = data;
    struct table *t = &s->t;
    int p, i, x, y, n, first, dp = -1, di = 0, dn = 0, down, up;
    (void)area;
    fit(s, vw->w, vw->h);
    gw_rect(win, vw->cx, vw->cy, vw->cw, vw->ch, C_TABLE);
    if(!table_dragged(t, &dp, &di, &dn)) dp = -1;
    for(p = 0; p < SOL_PILES; p++) {
        const struct sol_pile *q = &t->game.pile[p];
        n = q->n - (p == dp ? dn : 0);
        table_card_xy(t, p, 0, &x, &y);
        if(p != SOL_WASTE || !n) place(win, t, p, x, y);
        first = SOL_IS_TABLEAU(p) ? 0 : p == SOL_WASTE && t->game.draw == 3 ? n - 3 : n - 1;   /* what shows of the rest */
        for(i = first > 0 ? first : 0; i < n; i++) {
            table_card_xy(t, p, i, &x, &y);
            card(win, t, x, y, q->card[i]);
        }
        if(t->cursor && s->keys && t->cur_pile == p) {    /* (an undo may have left it past the pile's top) */
            i = t->cur_index < q->n ? t->cur_index : q->n - 1;
            table_card_xy(t, p, i > 0 ? i : 0, &x, &y);
            frame(win, t, x, y);
        }
    }
    if(dp < 0) return;                                  /* the cards dragged, over everything */
    table_fan(t, dp, &down, &up);
    for(i = 0; i < dn; i++)
        card(win, t, t->drag_x - t->grab_x, t->drag_y - t->grab_y + i * up, t->game.pile[dp].card[di + i]);
}

/**** what changed ****/

#define MIX(v) (h = (h ^ (unsigned)(v)) * 16777619u)

/* what a pile shows, as a number: its cards, its fan, the keyboard's card on it, the cards a drag took */
static unsigned pile_hash(const struct swin *s, int pile) {
    const struct table *t = &s->t;
    const struct sol_pile *q = &t->game.pile[pile];
    unsigned h = 2166136261u;
    int i, dp, di, dn, down, up;
    table_fan(t, pile, &down, &up);
    MIX(q->n);
    MIX(down);
    MIX(up);
    MIX(t->game.draw);
    for(i = 0; i < q->n; i++) MIX(q->card[i]);
    if(table_dragged(t, &dp, &di, &dn) && dp == pile) MIX(1000 + dn);
    if(t->cursor && s->keys && t->cur_pile == pile) MIX(2000 + t->cur_index);
    if(pile == SOL_STOCK) MIX(t->game.pile[SOL_WASTE].n > 0);
    return h;
}

/* the part of the board a pile may cover: its place, the waste's fan, a tableau's column to the bottom; with the
   keyboard's frame around it */
static void extent(const struct swin *s, int pile, int *x, int *y, int *w, int *h) {
    const struct table *t = &s->t;
    table_card_xy(t, pile, 0, x, y);
    *w = t->cw + (pile == SOL_WASTE ? 2 * TB_FAN : 0);
    *h = SOL_IS_TABLEAU(pile) ? s->bh - *y : t->ch;
    *x -= 2;
    *y -= 2;
    *w += 4;
    *h += 4;
}

static void set_text(struct gk_widget *w, const char *s) {
    if(strcmp(gk_text(w), s)) gk_set_text(w, s);
}

static void refresh(struct swin *s, unsigned now) {
    struct table *t = &s->t;
    struct gk_view vw;
    char b[TB_STATUS + 16], time[24];
    int p, x, y, w, h, dp, di, dn, down, up, all, undo;
    unsigned hs, ms;
    gk_area_view(s->table, &vw);
    all = fit(s, vw.w, vw.h);
    if(all) gk_redraw(s->table);
    for(p = 0; p < SOL_PILES; p++) {
        hs = pile_hash(s, p);
        if(hs == s->shown[p] && !all) continue;
        s->shown[p] = hs;
        extent(s, p, &x, &y, &w, &h);
        if(!all) gk_damage(s->table, x, y, w, h);
    }
    w = 0;
    if(table_dragged(t, &dp, &di, &dn)) {               /* where the dragged cards were, and are */
        table_fan(t, dp, &down, &up);
        x = t->drag_x - t->grab_x;
        y = t->drag_y - t->grab_y;
        w = t->cw;
        h = t->ch + (dn - 1) * up;
    }
    if(w != s->drag_w || (w && (x != s->drag_x || y != s->drag_y || h != s->drag_h))) {
        if(s->drag_w) gk_damage(s->table, s->drag_x, s->drag_y, s->drag_w, s->drag_h);
        if(w) gk_damage(s->table, x, y, w, h);
        s->drag_x = x;
        s->drag_y = y;
        s->drag_w = w;
        s->drag_h = w ? h : 0;
    }
    snprintf(b, sizeof(b), "Moves %d", t->game.moves);
    set_text(s->moves, b);
    ms = sol_ms(&t->game, now);
    table_time(ms, time, sizeof(time));
    snprintf(b, sizeof(b), "Time %s", time);
    set_text(s->clock, b);
    table_status(t, b, sizeof(b));
    set_text(s->status, b);
    table_record(t, b, sizeof(b));
    gk_status_right(s->status, b);
    undo = sol_can_undo(&t->game) && !t->finishing;
    if(gk_enabled(s->undo) != undo) gk_enable(s->undo, undo);
    gk_menu_enable(s->ui, MENU_UNDO, undo);
    if(gk_value(s->draw) != (t->draw == 3)) gk_set_value(s->draw, t->draw == 3);
    gk_menu_check(s->ui, MENU_ONE, t->draw == 1);
    gk_menu_check(s->ui, MENU_THREE, t->draw == 3);
    if(t->game.started && !t->game.won) gk_timer(s->ui, TIMER_CLOCK, (int)(1000 - ms % 1000), 0);
    else gk_timer_stop(s->ui, TIMER_CLOCK);
    if(t->finishing) {
        if(!s->finish_timer) gk_timer(s->ui, TIMER_FINISH, TB_FINISH_MS, 1);
        s->finish_timer = 1;
    } else if(s->finish_timer) {
        gk_timer_stop(s->ui, TIMER_FINISH);
        s->finish_timer = 0;
    }
}

static void new_deal(struct swin *s, int draw) {
    table_new(&s->t, draw);
    gk_focus(s->table);
}

static void ask(struct swin *s) {
    gk_confirm(s->ui, BOX_CLOSE, "Close Solitaire?", "A game is under way: it ends with the window.", "", "Cancel",
               "Close", NULL);
}

static int closing(struct swin *s) {
    if(table_close(&s->t) == TB_CLOSE) return 0;
    ask(s);
    return 1;
}

int sw_event(struct swin *s, const struct gk_event *ev) {
    unsigned now = uptime();
    switch(ev->type) {
    case GK_EV_CLOSE:
        if(!closing(s)) return 0;
        break;
    case GK_EV_ANSWER:
        if(ev->id == BOX_CLOSE && table_answer(&s->t, ev->value == 1) == TB_CLOSE) return 0;
        break;
    case GK_EV_CLICK:
        if(ev->id == ID_NEW) new_deal(s, s->t.draw);
        else if(ev->id == ID_UNDO) {
            table_undo(&s->t, now);
            gk_focus(s->table);
        }
        break;
    case GK_EV_CHANGE:
        if(ev->id == ID_DRAW) new_deal(s, ev->value ? 3 : 1);
        break;
    case GK_EV_MENU:
        switch(ev->id) {
        case MENU_NEW: new_deal(s, s->t.draw); break;
        case MENU_UNDO: table_undo(&s->t, now); break;
        case MENU_ONE: new_deal(s, 1); break;
        case MENU_THREE: new_deal(s, 3); break;
        case MENU_CLOSE: if(!closing(s)) return 0; break;
        case MENU_HELP:
            gk_message(s->ui, BOX_HELP, "Playing Solitaire", "Build the foundations up from the aces, by suit.",
                       "Drag cards, or click one to send it where it goes.");
            break;
        }
        break;
    case GK_EV_KEY:
        if(ev->w && ev->w != s->table) break;
        if(table_key(&s->t, ev->value, ev->mods, now) == TB_CLOSE) return 0;
        if(s->t.asking && !gk_box_open(s->ui)) ask(s);  /* Esc or Ctrl+W during a game */
        break;
    case GK_EV_POINTER:
        if(ev->w == s->table) table_pointer(&s->t, ev->x, ev->y, ev->buttons, (unsigned)ev->value, now);
        break;
    case GK_EV_TIMER:
        if(ev->id == TIMER_FINISH) table_tick(&s->t, now);
        break;
    case GK_EV_FOCUS:                                   /* the keys gone (another window, a question): a drag goes back */
        if(ev->w != s->table) break;
        s->keys = ev->value;
        if(!ev->value) table_cancel(&s->t);
        break;
    case GK_EV_THEME:
        gk_redraw(s->table);
        break;
    }
    refresh(s, now);
    return 1;
}

int sw_open(struct swin *s, unsigned seed) {
    struct gk_widget *root, *bar;
    int i;
    memset(s, 0, sizeof(*s));
    table_init(&s->t, seed);
    s->ui = gk_open_limits("Solitaire", TB_W, TB_BAR_H + TB_H + TB_STATUS_H, 7 * TB_CARD_MIN + 6 * TB_GAP + 2 * TB_MARGIN,
                           TB_BAR_H + TB_STATUS_H + 2 * (TB_CARD_MIN * 4 / 3) + TB_ROW_GAP + 2 * TB_MARGIN + 24, 0, 0,
                           WIN_RESIZABLE);
    if(!s->ui) return -1;
    root = gk_root(s->ui);
    bar = gk_hbox(root, 10);
    gk_pad(bar, 5);
    gk_button(bar, "New", ID_NEW);
    s->undo = gk_button(bar, "Undo", ID_UNDO);
    s->draw = gk_radio(bar, ID_DRAW, 1);
    gk_radio_add(s->draw, "Draw one");
    gk_radio_add(s->draw, "Draw three");
    gk_spacer(bar);
    s->moves = gk_label(bar, "Moves 0");
    gk_size(s->moves, 9 * 8, 0);
    s->clock = gk_label(bar, "Time 0:00");
    gk_size(s->clock, 10 * 8, 0);
    s->table = gk_area(root, ID_TABLE, GK_FOCUSABLE, draw, s);
    s->status = gk_status(root);
    i = gk_menu(s->ui, "Game");
    gk_menu_item(s->ui, i, MENU_NEW, "New deal", GK_CTRL | 'N', 0);
    gk_menu_item(s->ui, i, MENU_UNDO, "Undo", GK_CTRL | 'Z', GK_ITEM_OFF);
    gk_menu_line(s->ui, i);
    gk_menu_item(s->ui, i, MENU_ONE, "Draw one", '1', GK_ITEM_RADIO | GK_ITEM_ON);
    gk_menu_item(s->ui, i, MENU_THREE, "Draw three", '3', GK_ITEM_RADIO);
    gk_menu_line(s->ui, i);
    gk_menu_item(s->ui, i, MENU_CLOSE, "Close", GK_CTRL | 'W', 0);
    i = gk_menu(s->ui, "Help");
    gk_menu_item(s->ui, i, MENU_HELP, "Playing Solitaire", 0, 0);
    gk_focus(s->table);
    refresh(s, uptime());
    if(!gk_ok(s->ui)) {
        gk_close(s->ui);
        s->ui = NULL;
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void sw_close(struct swin *s) {
    gk_close(s->ui);
    s->ui = NULL;
}
