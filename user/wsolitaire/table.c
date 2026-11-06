/*
SurfOS user program: wsolitaire
--------------------
File: table.c   Date: 11/05/26
--------------------
Solitaire in its window, without the drawing (table.h). The board is the window's area between its
toolbar and its status bar; everything here is in its coordinates. The top row holds the stock, the waste
(drawing three, its top three cards side by side) and, after a gap, the four foundations; the
seven tableau piles are below, each fanning its face-down cards by their edges and its face-up
cards by their rank and suit, closer together when a pile would pass the bottom of the window.
The cards are as wide as seven columns allow, at most TB_CARD_W, a third taller than wide, and
smaller still when the window is too short for the two rows.

The pointer: a left press on cards that can be taken holds them; moved TB_DRAG pixels it is a
drag, and the cards follow the pointer until the button is let go over a pile they fit (the pile
under the pointer, or under the middle of the top card dragged), or go back. A press let go
without moving is a click, which sends the cards where they can go (sol_target()), and a click on
the stock deals. The keyboard: the arrows choose a card (up and down along a face-up run too),
Enter sends it where it can go, Space deals, U, Backspace or Ctrl+Z undo, N deals again, 1 and 3
deal anew drawing one or three, Esc or Ctrl+W close, asking first while a game is under way. A key
held down comes again (the keyboard's repeat): the arrows move on with it, and nothing else repeats,
so a held Space deals once.

When a move leaves the game able to finish by itself, it does: a card each TB_FINISH_MS, the
window drawing each (table_tick()). A game counts as played from its first move; the session
keeps the games played and won, the best time and the fewest moves of a win, drawing one and
drawing three apart, while the program runs.
*/
#include <stdio.h>
#include <string.h>
#include "table.h"

static const char *const ranks[] = { "", "ace", "2", "3", "4", "5", "6", "7", "8", "9", "10", "jack", "queen", "king" };
static const char *const suits[] = { "spades", "hearts", "clubs", "diamonds" };

static void status(struct table *t, const char *msg) {
    snprintf(t->status, sizeof(t->status), "%s", msg);
}

static int inside(int x, int y, int bx, int by, int bw, int bh) {
    return x >= bx && y >= by && x < bx + bw && y < by + bh;
}

void table_time(unsigned ms, char *out, int n) {
    unsigned s = ms / 1000;
    if(s >= 3600) snprintf(out, (size_t)n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, (size_t)n, "%u:%02u", s / 60, s % 60);
}

const char *table_card_name(unsigned char card, char *out, int n) {
    snprintf(out, (size_t)n, "the %s of %s", ranks[SOL_RANK(card)], suits[SOL_SUIT(card)]);
    return out;
}

void table_new(struct table *t, int draw) {
    t->draw = draw == 3 ? 3 : 1;
    sol_new(&t->game, t->draw, sol_next(&t->rng));
    t->press_pile = -1;
    t->dragging = t->finishing = t->asking = 0;
    t->cur_pile = SOL_STOCK;
    t->cur_index = t->game.pile[SOL_STOCK].n - 1;
    t->status[0] = 0;
}

void table_init(struct table *t, unsigned seed) {
    memset(t, 0, sizeof(*t));
    t->rng = sol_seed(seed);            /* each deal's seed is this generator's next number */
    table_new(t, 1);
    table_layout(t, TB_W, TB_H);
}

/**** where everything lies ****/

void table_layout(struct table *t, int w, int h) {
    int cw = (w - 2 * TB_MARGIN - 6 * TB_GAP) / 7, room = h - 2 * TB_MARGIN, x0, i;
    if(cw > TB_CARD_W) cw = TB_CARD_W;
    /* the two rows, and a tableau pile of six face-down cards and seven face-up as close as they go and still read */
    while(cw > TB_CARD_MIN && 2 * (cw * 4 / 3) + TB_ROW_GAP + 6 * TB_DOWN_MIN + 6 * TB_UP > room) cw--;
    if(cw < TB_CARD_MIN) cw = TB_CARD_MIN;
    t->w = w;
    t->h = h;
    t->cw = cw;
    t->ch = cw * 4 / 3;
    x0 = (w - 7 * cw - 6 * TB_GAP) / 2;
    if(x0 < TB_MARGIN) x0 = TB_MARGIN;  /* a window too narrow shows the left of the table */
    for(i = 0; i < 7; i++) t->col[i] = x0 + i * (cw + TB_GAP);
    t->top_y = TB_MARGIN;
    t->tab_y = t->top_y + t->ch + TB_ROW_GAP;
    t->bottom = h - TB_MARGIN;
}

/* every card but a pile's last makes a step: a face-down one of TB_DOWN, a face-up one of TB_UP, both less when the
   pile would pass the bottom (the face-down steps first, as their cards cannot be read anyway) */
void table_fan(const struct table *t, int pile, int *down, int *up) {
    const struct sol_pile *p;
    int nd = 0, nu, room, i;
    *down = TB_DOWN;
    *up = TB_UP;
    if(!SOL_IS_TABLEAU(pile)) return;
    p = &t->game.pile[pile];
    if(p->n < 2) return;
    for(i = 0; i < p->n - 1; i++) nd += !(p->card[i] & SOL_UP);
    nu = p->n - 1 - nd;
    room = t->bottom - t->tab_y - t->ch;
    if(nd * TB_DOWN + nu * TB_UP <= room) return;
    if(nd * TB_DOWN_MIN + nu * TB_UP <= room) {
        *down = (room - nu * TB_UP) / nd;
        return;
    }
    *down = TB_DOWN_MIN;
    if(nu) *up = (room - nd * TB_DOWN_MIN) / nu;
    if(*up < TB_UP_MIN) *up = TB_UP_MIN;
}

void table_card_xy(const struct table *t, int pile, int index, int *x, int *y) {
    const struct sol_pile *p = &t->game.pile[pile];
    int down, up, i, first;
    *y = t->top_y;
    if(pile == SOL_STOCK) { *x = t->col[0]; return; }
    if(pile == SOL_WASTE) {             /* drawing three: the top three side by side, the newest on the right */
        *x = t->col[1];
        first = p->n > 3 ? p->n - 3 : 0;
        if(t->game.draw == 3 && index > first) *x += (index - first) * TB_FAN;
        return;
    }
    if(SOL_IS_FOUNDATION(pile)) { *x = t->col[3 + pile - SOL_FOUNDATION]; return; }
    *x = t->col[pile - SOL_TABLEAU];
    *y = t->tab_y;
    table_fan(t, pile, &down, &up);
    for(i = 0; i < index && i < p->n; i++) *y += p->card[i] & SOL_UP ? up : down;
}

int table_hit(const struct table *t, int x, int y, int *pile, int *index) {
    int p, i, cx, cy;
    for(p = 0; p < SOL_PILES; p++) {
        for(i = t->game.pile[p].n - 1; i >= 0; i--) {   /* the top card first: it covers the rest */
            table_card_xy(t, p, i, &cx, &cy);
            if(inside(x, y, cx, cy, t->cw, t->ch)) {
                *pile = p;
                *index = i;
                return 1;
            }
        }
        table_card_xy(t, p, 0, &cx, &cy);
        if(!t->game.pile[p].n && inside(x, y, cx, cy, t->cw, t->ch)) {
            *pile = p;
            *index = -1;
            return 1;
        }
    }
    return 0;
}

/* a foundation's place, or a tableau's whole column down to the bottom, half the gap wider each side */
int table_drop(const struct table *t, int x, int y) {
    int p, cx, cy;
    for(p = SOL_FOUNDATION; p < SOL_PILES; p++) {
        table_card_xy(t, p, 0, &cx, &cy);
        if(SOL_IS_FOUNDATION(p) && inside(x, y, cx - TB_GAP / 2, cy - TB_GAP / 2, t->cw + TB_GAP, t->ch + TB_GAP))
            return p;
        if(SOL_IS_TABLEAU(p) && inside(x, y, cx - TB_GAP / 2, cy, t->cw + TB_GAP, t->bottom - cy + TB_MARGIN)) return p;
    }
    return -1;
}

int table_dragged(const struct table *t, int *pile, int *index, int *n) {
    if(!t->dragging || t->press_pile < 0) return 0;
    *pile = t->press_pile;
    *index = t->press_index;
    *n = t->game.pile[*pile].n - *index;
    return 1;
}

/**** the game's moves, and the session ****/

/* a game was just won: the session's scores, and what the status bar says */
static void ended(struct table *t, unsigned now) {
    char time[24], best[24], msg[TB_STATUS];
    int d = t->draw == 3, moves = t->game.moves;
    unsigned ms = sol_ms(&t->game, now);
    t->won[d]++;
    table_time(ms, time, sizeof(time));
    if(!t->best_ms[d] || ms < t->best_ms[d] || moves < t->best_moves[d]) {
        if(!t->best_ms[d] || ms < t->best_ms[d]) t->best_ms[d] = ms;
        if(!t->best_moves[d] || moves < t->best_moves[d]) t->best_moves[d] = moves;
        snprintf(msg, sizeof(msg), "Won in %s with %d moves: the best yet", time, moves);
    } else {
        table_time(t->best_ms[d], best, sizeof(best));
        snprintf(msg, sizeof(msg), "Won in %s with %d moves (best %s, %d)", time, moves, best, t->best_moves[d]);
    }
    status(t, msg);
}

/* after the player's move or deal: the first counts the game as played; the last may win it, or leave it able to finish
   by itself */
static void after(struct table *t, int was_started, unsigned now) {
    if(!was_started && t->game.started) t->played[t->draw == 3]++;
    if(t->game.won) ended(t, now);
    else if(sol_finishing(&t->game)) {
        t->finishing = 1;
        t->last_step = now;
        status(t, "Every card is up: the rest go home");
    }
}

static void deal(struct table *t, unsigned now) {
    int was = t->game.started;
    if(!sol_deal(&t->game, now)) {
        status(t, "The stock and the waste are empty");
        return;
    }
    t->status[0] = 0;
    after(t, was, now);
}

/* a click on index of pile (or Enter on the keyboard's card): the cards go where they can */
static void send(struct table *t, int pile, int index, unsigned now) {
    char name[40], msg[TB_STATUS];
    int to, was = t->game.started;
    if(pile == SOL_STOCK) {
        deal(t, now);
        return;
    }
    if(!sol_can_take(&t->game, pile, index)) return;
    to = sol_target(&t->game, pile, index);
    if(to < 0) {
        snprintf(msg, sizeof(msg), "%s has nowhere to go", table_card_name(t->game.pile[pile].card[index], name,
                 sizeof(name)));
        msg[0] = (char)(msg[0] - 'a' + 'A');
        status(t, msg);
        return;
    }
    sol_move(&t->game, pile, index, to, now);
    t->status[0] = 0;
    after(t, was, now);
}

int table_undo(struct table *t, unsigned now) {
    if(t->finishing) return 0;
    if(!sol_undo(&t->game, now)) status(t, t->game.won ? "The game is won: N deals again" : "Nothing to undo");
    else t->status[0] = 0;
    t->press_pile = -1;
    t->dragging = 0;
    return TB_DRAW;
}

int table_cancel(struct table *t) {
    int was = t->press_pile >= 0 || t->dragging;
    t->press_pile = -1;
    t->dragging = 0;
    t->buttons = 0;
    return was ? TB_DRAW : 0;
}

int table_tick(struct table *t, unsigned now) {
    if(!t->finishing || (int)(now - t->last_step) < TB_FINISH_MS) return 0;
    t->last_step = now;
    if(!sol_finish_step(&t->game, now) || t->game.won) {
        t->finishing = 0;
        if(t->game.won) ended(t, now);
    }
    return TB_DRAW;
}

int table_close(struct table *t) {
    if(!t->game.started || t->game.won) return TB_CLOSE;
    t->asking = 1;                      /* a game under way: main.c asks, Cancel first */
    return TB_DRAW;
}

int table_answer(struct table *t, int close) {
    t->asking = 0;
    return close ? TB_CLOSE : TB_DRAW;
}

/**** the pointer ****/

int table_pointer(struct table *t, int x, int y, unsigned buttons, unsigned pressed, unsigned now) {
    unsigned was = t->buttons, down = pressed & buttons, up = was & ~buttons;
    int pile, index, cx, cy, to, r = 0, d;
    t->buttons = buttons;
    if(t->press_pile >= 0 && !(buttons & TB_BUTTON_LEFT) && !(up & TB_BUTTON_LEFT))
        r = table_cancel(t);            /* a release never seen: a drag starts only from a press seen here */
    if(t->asking || t->finishing) {     /* the question, or the finish, has the pointer: a drag goes back */
        if(t->dragging || t->press_pile >= 0) r = TB_DRAW;
        t->press_pile = -1;
        t->dragging = 0;
        return r;
    }
    if(down & TB_BUTTON_LEFT) {
        t->press_pile = -1;
        t->dragging = 0;
        if(table_hit(t, x, y, &pile, &index) && (pile == SOL_STOCK || sol_can_take(&t->game, pile, index))) {
            t->press_pile = pile;
            t->press_index = index;
            t->press_x = x;
            t->press_y = y;
            table_card_xy(t, pile, index < 0 ? 0 : index, &cx, &cy);
            t->grab_x = x - cx;
            t->grab_y = y - cy;
            t->cur_pile = pile;         /* the keyboard's card follows, hidden until a key is used */
            t->cur_index = index;
            t->cursor = 0;
        }
        r = TB_DRAW;
    }
    if((buttons & TB_BUTTON_LEFT) && t->press_pile > SOL_STOCK) {
        d = (x > t->press_x ? x - t->press_x : t->press_x - x) + (y > t->press_y ? y - t->press_y : t->press_y - y);
        if(!t->dragging && d >= TB_DRAG) t->dragging = 1;
        if(t->dragging && (r || x != t->drag_x || y != t->drag_y)) {
            t->drag_x = x;
            t->drag_y = y;
            r = TB_DRAW;
        }
    }
    if(up & TB_BUTTON_LEFT) {
        pile = t->press_pile;
        if(pile < 0) return r;
        if(t->dragging) {               /* let go: over a pile it fits, or by the dragged card's middle, or back */
            int was_started = t->game.started;
            to = table_drop(t, x, y);
            if(!sol_can_put(&t->game, pile, t->press_index, to))
                to = table_drop(t, x - t->grab_x + t->cw / 2, y - t->grab_y + t->ch / 2);
            t->dragging = 0;
            t->press_pile = -1;
            if(sol_move(&t->game, pile, t->press_index, to, now)) {
                t->status[0] = 0;
                after(t, was_started, now);
            }
            return TB_DRAW;
        }
        t->press_pile = -1;             /* a click: on the stock it deals, elsewhere the cards go where they can */
        if(pile == SOL_STOCK) {
            if(table_hit(t, x, y, &cx, &cy) && cx == SOL_STOCK) deal(t, now);
        } else send(t, pile, t->press_index, now);
        return TB_DRAW;
    }
    return r;
}

/**** the keyboard ****/

/* the keyboard's card on pile: its top card (-1 when empty) */
static void choose(struct table *t, int pile) {
    t->cur_pile = pile;
    t->cur_index = t->game.pile[pile].n - 1;
}

/* the top row's places by column: the stock, the waste, the gap (the waste's), the four foundations */
static int top_pile(int column) {
    return column == 0 ? SOL_STOCK : column <= 2 ? SOL_WASTE : SOL_FOUNDATION + column - 3;
}

static int column_of(int pile) {
    return pile == SOL_STOCK ? 0 : pile == SOL_WASTE ? 1 : SOL_IS_FOUNDATION(pile) ? 3 + pile - SOL_FOUNDATION :
           pile - SOL_TABLEAU;
}

static void cursor_key(struct table *t, int key) {
    const struct sol_pile *p = &t->game.pile[t->cur_pile];
    int c = column_of(t->cur_pile), tab = SOL_IS_TABLEAU(t->cur_pile);
    if(t->cur_index >= p->n) t->cur_index = p->n - 1;   /* the pile changed under it */
    switch(key) {
    case TB_LEFT:
        if(tab && c > 0) choose(t, t->cur_pile - 1);
        else if(!tab && c > 0) choose(t, top_pile(c == 3 ? 1 : c - 1));
        break;
    case TB_RIGHT:
        if(tab && c < 6) choose(t, t->cur_pile + 1);
        else if(!tab && c < 6) choose(t, top_pile(c == 1 ? 3 : c + 1));
        break;
    case TB_UPKEY:                      /* deeper into a face-up run, then up to the top row */
        if(tab && t->cur_index > 0 && sol_can_take(&t->game, t->cur_pile, t->cur_index - 1)) t->cur_index--;
        else if(tab) choose(t, top_pile(c));
        break;
    case TB_DOWNKEY:                    /* back along the run, or down from the top row */
        if(tab && t->cur_index < p->n - 1) t->cur_index++;
        else if(!tab) choose(t, SOL_TABLEAU + c);
        break;
    }
}

int table_key(struct table *t, int key, unsigned mods, unsigned now) {
    if(t->asking) return 0;             /* the question has the keys */
    if((mods & TB_REPEAT) && (key < TB_UPKEY || key > TB_RIGHT)) return 0;     /* only the arrows move on a repeat */
    switch(key) {
    case 27: case 23: return table_close(t);           /* Esc, Ctrl+W */
    case 'n': case 'N': table_new(t, t->draw); return TB_DRAW;
    case '1': table_new(t, 1); return TB_DRAW;
    case '3': table_new(t, 3); return TB_DRAW;
    }
    if(t->finishing) return 0;
    switch(key) {
    case 'u': case 'U': case 127: case 8: case 26: return table_undo(t, now);  /* U, Backspace (127; 8 from an older
                                                                                desktop), Ctrl+Z */
    case ' ': case 'd': case 'D':
        deal(t, now);
        return TB_DRAW;
    case '\r': case '\n':
        if(!t->cursor) { t->cursor = 1; return TB_DRAW; }   /* the first key shows the card it is on */
        send(t, t->cur_pile, t->cur_index, now);
        if(t->cur_index >= t->game.pile[t->cur_pile].n || t->cur_pile == SOL_STOCK) choose(t, t->cur_pile);
        return TB_DRAW;
    case TB_LEFT: case TB_RIGHT: case TB_UPKEY: case TB_DOWNKEY:
        if(t->cursor) cursor_key(t, key);
        t->cursor = 1;
        return TB_DRAW;
    }
    return 0;
}

/**** the status bar ****/

/* (short: the status bar has the session's record at its right, and 640 x 400 is the smallest desktop) */
void table_status(const struct table *t, char *out, int n) {
    if(t->status[0]) snprintf(out, (size_t)n, "%s", t->status);
    else if(t->game.won) snprintf(out, (size_t)n, "N deals again; 1 or 3 draws one or three");
    else snprintf(out, (size_t)n, "Drag a card, or click it; Space deals");
}

void table_record(const struct table *t, char *out, int n) {
    char time[24];
    int d = t->draw == 3;
    if(!t->best_ms[d]) {
        snprintf(out, (size_t)n, "Won %d of %d", t->won[d], t->played[d]);
        return;
    }
    table_time(t->best_ms[d], time, sizeof(time));
    snprintf(out, (size_t)n, "Won %d of %d, best %s", t->won[d], t->played[d], time);
}
