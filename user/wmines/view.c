/*
SurfOS user program: wmines
--------------------
File: view.c    Date: 11/05/26
--------------------
Mines in its window, without the drawing (view.h). The field sits in the middle of the board,
the area between the window's toolbar and its status bar, its cells as large as the room allows
within MV_CELL_MIN and MV_CELL_MAX; a new window asks for MV_CELL cells, less when the desktop is
small.

The pointer: a left press on the field marks the cell under it pressed while the button is held
(the cell follows the pointer), and the release reveals it, so a press can still be taken back by
moving off the field. On an open number the release chords: the covered cells around it show
pressed meanwhile. The right button flags and unflags at its press, and the middle one chords.
The keyboard: the arrows move a cursor, which shows from the first key on, Space or Enter reveals
(chords on a number), F flags, N starts the field again, 1 to 3 choose the field, and Esc or
Ctrl+W close the window, asking first while a game is under way. A key held down comes again and
again (the keyboard's repeat): the arrows move on with it, and nothing else repeats, so a flag
held is not taken back nor a cell revealed twice.

A game counts as played from its first reveal. The best time of each field, and the games played
and won, last while the program runs; nothing is written to a file.
*/
#include <stdio.h>
#include <string.h>
#include "view.h"

static void status(struct mview *v, const char *msg) {
    snprintf(v->status, sizeof(v->status), "%s", msg);
}

void mview_time(unsigned ms, char *out, int n) {
    unsigned s = ms / 1000;
    if(s >= 3600) snprintf(out, (size_t)n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, (size_t)n, "%u:%02u", s / 60, s % 60);
}

void mview_new(struct mview *v, int size) {
    if(size < 0 || size >= MINES_SIZES) size = MINES_SMALL;
    v->size = size;
    mines_new(&v->game, mines_sizes[size].w, mines_sizes[size].h, mines_sizes[size].mines, mines_next(&v->rng));
    if(v->cx >= v->game.w) v->cx = v->game.w - 1;
    if(v->cy >= v->game.h) v->cy = v->game.h - 1;
    v->held = 0;
    v->px = v->py = -1;
    v->asking = 0;
    v->status[0] = 0;
}

void mview_init(struct mview *v, unsigned seed) {
    memset(v, 0, sizeof(*v));
    v->rng = mines_seed(seed);          /* each new game's seed is this generator's next number */
    v->px = v->py = -1;
    mview_new(v, MINES_SMALL);
}

void mview_window(int size, int max_w, int max_h, int *w, int *h) {
    const struct mines_size *s = &mines_sizes[size >= 0 && size < MINES_SIZES ? size : 0];
    int cell = MV_CELL;
    for(;; cell--) {                    /* MV_CELL, or the largest smaller cell that fits */
        *w = s->w * cell + 2 * MV_MARGIN;
        if(*w < MV_MIN_W) *w = MV_MIN_W;
        *h = MV_BAR_H + s->h * cell + 2 * MV_MARGIN + MV_STATUS_H;
        if(cell <= MV_CELL_MIN || ((!max_w || *w <= max_w) && (!max_h || *h <= max_h))) return;
    }
}

int mview_layout(struct mview *v, int w, int h) {
    int room_w = w - 2 * MV_MARGIN, room_h = h - 2 * MV_MARGIN, cell, x, y, fx, fy;
    cell = room_w / v->game.w < room_h / v->game.h ? room_w / v->game.w : room_h / v->game.h;
    if(cell < MV_CELL_MIN) cell = MV_CELL_MIN;      /* a board too small for the field shows its top left */
    if(cell > MV_CELL_MAX) cell = MV_CELL_MAX;
    x = (w - v->game.w * cell) / 2;
    y = (h - v->game.h * cell) / 2;
    fx = x > MV_MARGIN ? x : MV_MARGIN;
    fy = y > MV_MARGIN ? y : MV_MARGIN;
    if(cell == v->cell && fx == v->fx && fy == v->fy) return 0;
    v->cell = cell;
    v->fx = fx;
    v->fy = fy;
    return 1;
}

int mview_cell_at(const struct mview *v, int x, int y, int *cx, int *cy) {
    if(v->cell < 1 || x < v->fx || y < v->fy) return 0;
    *cx = (x - v->fx) / v->cell;
    *cy = (y - v->fy) / v->cell;
    return *cx < v->game.w && *cy < v->game.h;
}

/* the cells shown pressed while the left button is held: the one under it when covered, the covered ones around it
   when it is an open number (the chord about to be made) */
static int pressed(const struct mview *v, int x, int y) {
    const struct mines *m = &v->game;
    if(!v->held || v->px < 0 || mines_over(m)) return 0;
    if(m->cell[v->py * m->w + v->px] & MINE_OPEN)
        return m->near[v->py * m->w + v->px] && (x != v->px || y != v->py) && x >= v->px - 1 && x <= v->px + 1 &&
               y >= v->py - 1 && y <= v->py + 1;
    return x == v->px && y == v->py;
}

int mview_look(const struct mview *v, int x, int y) {
    const struct mines *m = &v->game;
    unsigned char c;
    if(x < 0 || y < 0 || x >= m->w || y >= m->h) return MV_COVERED;
    c = m->cell[y * m->w + x];
    if(m->state == MINES_LOST) {        /* the end of a lost game: every mine, the one that went off, wrong flags */
        if(c & MINE_BLAST) return MV_BLAST;
        if((c & MINE_HERE) && !(c & MINE_FLAG)) return MV_MINE;
        if((c & MINE_FLAG) && !(c & MINE_HERE)) return MV_WRONG;
    }
    if(c & MINE_OPEN) return MV_OPEN + m->near[y * m->w + x];
    if(c & MINE_FLAG) return MV_FLAG;
    return pressed(v, x, y) ? MV_PRESSED : MV_COVERED;
}

/* a game has just ended: the session's scores, and what the status bar says */
static void ended(struct mview *v, unsigned now) {
    char t[24], best[24], msg[MV_STATUS];
    unsigned ms = mines_ms(&v->game, now);
    int s = v->size;
    mview_time(ms, t, sizeof(t));
    if(v->game.state == MINES_WON) {
        v->won[s]++;
        if(!v->best[s] || ms < v->best[s]) {
            v->best[s] = ms;
            snprintf(msg, sizeof(msg), "Cleared in %s: the best this session", t);
        } else {
            mview_time(v->best[s], best, sizeof(best));
            snprintf(msg, sizeof(msg), "Cleared in %s (the best: %s)", t, best);
        }
    } else snprintf(msg, sizeof(msg), "A mine went off after %s. N: another field", t);
    status(v, msg);
}

/* a reveal or a chord at x, y, as the cell asks: a covered cell is revealed, an open number chorded */
static int play(struct mview *v, int x, int y, int chord, unsigned now) {
    int was = v->game.state, r;
    if(chord || (v->game.cell[y * v->game.w + x] & MINE_OPEN)) r = mines_chord(&v->game, x, y, now);
    else r = mines_reveal(&v->game, x, y, now);
    if(r == MINES_NONE) return 0;
    if(was == MINES_READY) v->played[v->size]++;
    v->status[0] = 0;
    if(r == MINES_ENDED) ended(v, now);
    return MV_DRAW;
}

int mview_close(struct mview *v) {
    if(v->game.state != MINES_PLAYING) return MV_CLOSE;
    v->asking = 1;                      /* a game under way: main.c asks, Cancel first */
    return MV_DRAW;
}

int mview_answer(struct mview *v, int close) {
    v->asking = 0;
    return close ? MV_CLOSE : MV_DRAW;
}

int mview_key(struct mview *v, int key, unsigned mods, unsigned now) {
    struct mines *m = &v->game;
    if(v->asking) return 0;             /* the question has the keys */
    if((mods & MV_REPEAT) && (key < MV_UP || key > MV_PGDN)) return 0;     /* only the cursor moves on a repeat */
    switch(key) {
    case 27: case 23: return mview_close(v);            /* Esc, Ctrl+W */
    case 'n': case 'N': mview_new(v, v->size); return MV_DRAW;
    case '1': case '2': case '3':
        mview_new(v, key - '1');
        return MV_RESIZE;
    case MV_LEFT: if(v->cx > 0 && v->cursor) v->cx--; break;
    case MV_RIGHT: if(v->cx < m->w - 1 && v->cursor) v->cx++; break;
    case MV_UP: if(v->cy > 0 && v->cursor) v->cy--; break;
    case MV_DOWN: if(v->cy < m->h - 1 && v->cursor) v->cy++; break;
    case MV_HOME: v->cx = 0; break;
    case MV_END: v->cx = m->w - 1; break;
    case MV_PGUP: v->cy = 0; break;
    case MV_PGDN: v->cy = m->h - 1; break;
    case ' ': case '\r': case '\n':
        v->cursor = 1;
        play(v, v->cx, v->cy, 0, now);
        return MV_DRAW;
    case 'f': case 'F':
        v->cursor = 1;
        if(mines_flag(m, v->cx, v->cy) != MINES_NONE) v->status[0] = 0;
        return MV_DRAW;
    default: return 0;
    }
    v->cursor = 1;                      /* the first arrow shows the cursor where it is; the next ones move it */
    return MV_DRAW;
}

int mview_pointer(struct mview *v, int x, int y, unsigned buttons, unsigned pressed, unsigned now) {
    unsigned was = v->buttons, down = pressed & buttons, up = was & ~buttons;
    int cx = -1, cy = -1, on = mview_cell_at(v, x, y, &cx, &cy), r = 0;
    v->buttons = buttons;
    if(v->asking) return 0;             /* the question has the pointer */
    if(v->held && !(buttons & MV_BUTTON_LEFT) && !(up & MV_BUTTON_LEFT)) r = mview_cancel(v);  /* a release not seen */
    if(down & MV_BUTTON_LEFT) {
        v->held = on && !mines_over(&v->game);
        if(v->held) {                   /* the keyboard's cursor follows, hidden until a key is used */
            v->cx = cx;
            v->cy = cy;
            v->cursor = 0;
        }
        r = MV_DRAW;
    }
    if(v->held) {                       /* the pressed cell follows the pointer while the button is held */
        if(!on) cx = cy = -1;
        if(cx != v->px || cy != v->py) r = MV_DRAW;
        v->px = cx;
        v->py = cy;
    }
    if(up & MV_BUTTON_LEFT) {
        if(v->held && on) play(v, cx, cy, 0, now);
        v->held = 0;
        v->px = v->py = -1;
        r = MV_DRAW;
    }
    if((down & MV_BUTTON_RIGHT) && on && !v->held && mines_flag(&v->game, cx, cy) != MINES_NONE) {
        v->status[0] = 0;
        r = MV_DRAW;
    }
    if((down & MV_BUTTON_MIDDLE) && on && !v->held && play(v, cx, cy, 1, now)) r = MV_DRAW;
    return r;
}

/* (short: the status bar has the session's record at its right, and 640 x 400 is the smallest desktop) */
int mview_cancel(struct mview *v) {
    int was = v->held;
    v->held = 0;
    v->px = v->py = -1;
    v->buttons = 0;
    return was ? MV_DRAW : 0;
}

void mview_status(const struct mview *v, char *out, int n) {
    const struct mines_size *s = &mines_sizes[v->size];
    if(v->status[0]) snprintf(out, (size_t)n, "%s", v->status);
    else if(v->game.state == MINES_READY) snprintf(out, (size_t)n, "%d mines in %d by %d: click a cell to begin",
                                                   s->mines, s->w, s->h);
    else if(v->game.state == MINES_PLAYING) snprintf(out, (size_t)n, "Right-click flags; click a number to clear around");
    else snprintf(out, (size_t)n, "N or New starts another field");
}

/* the status bar's right: the session's record of the field */
void mview_record(const struct mview *v, char *out, int n) {
    char t[24];
    int s = v->size;
    if(!v->best[s]) {
        snprintf(out, (size_t)n, "Won %d of %d", v->won[s], v->played[s]);
        return;
    }
    mview_time(v->best[s], t, sizeof(t));
    snprintf(out, (size_t)n, "Won %d of %d, best %s", v->won[s], v->played[s], t);
}
