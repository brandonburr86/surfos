/*
SurfOS user program: wswell
--------------------
File: view.c    Date: 11/05/26
--------------------
Swell in its window, without the drawing (view.h). The sea is at the left, its cells as large as
the window allows within SV_CELL_MIN and SV_CELL_MAX, the panel at its right. Each cell shows the
falling piece, the sea's own cells, or where the piece would come to rest (the ghost).

The keys come as typed, each a step, with no release: holding one plays by the keyboard's repeat,
which only Left, Right and Down act on (WIN_MOD_REPEAT says a key is held): a held Space drops one
piece, not every piece after it, and a held Up turns once. Left and Right move the piece, Up or X
turns it clockwise and Z the other way, Down moves it a row (and sets it when it has landed),
Space drops it. P pauses and goes on; Enter or Space starts a
new game and goes on from a pause; N starts again once a game is paused or over (never in the
middle of one, where a stray key would lose it). Esc or Ctrl+W close the window, pausing and
asking first while a game is under way. A window that loses the focus pauses its game, since the
keys go elsewhere then; it waits for P to go on.

The session keeps the games played and the best score with its rows while the program runs.
*/
#include <stdio.h>
#include <string.h>
#include "view.h"

static void status(struct sview *v, const char *msg) {
    snprintf(v->status, sizeof(v->status), "%s", msg);
}

void sview_time(unsigned ms, char *out, int n) {
    unsigned s = ms / 1000;
    if(s >= 3600) snprintf(out, (size_t)n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, (size_t)n, "%u:%02u", s / 60, s % 60);
}

void sview_new(struct sview *v) {
    swell_new(&v->game, swell_next(&v->rng));
    v->asking = 0;
    v->flash_at = 0;
    v->status[0] = 0;
}

void sview_init(struct sview *v, unsigned seed) {
    memset(v, 0, sizeof(*v));
    v->rng = swell_seed(seed);          /* each game's seed is this generator's next number */
    sview_new(v);
    sview_layout(v, SW_W * SV_CELL + SV_PANEL + 3 * SV_MARGIN, SW_H * SV_CELL + 2 * SV_MARGIN);
}

void sview_window(int max_w, int max_h, int *w, int *h) {
    int cell = SV_CELL;
    for(;; cell--) {                    /* SV_CELL, or the largest smaller cell that fits */
        *w = SW_W * cell + SV_PANEL + 3 * SV_MARGIN;
        *h = SW_H * cell + 2 * SV_MARGIN + SV_STATUS_H;
        if(cell <= SV_CELL_MIN || ((!max_w || *w <= max_w) && (!max_h || *h <= max_h))) return;
    }
}

int sview_layout(struct sview *v, int w, int h) {
    int room_w = (w - SV_PANEL - 3 * SV_MARGIN) / SW_W, room_h = (h - 2 * SV_MARGIN) / SW_H, x, y, cell, sx, sy;
    cell = room_w < room_h ? room_w : room_h;
    if(cell < SV_CELL_MIN) cell = SV_CELL_MIN;          /* a board too small shows the sea's top left */
    if(cell > SV_CELL_MAX) cell = SV_CELL_MAX;
    x = (w - (SW_W * cell + SV_MARGIN + SV_PANEL)) / 2;
    y = (h - SW_H * cell) / 2;
    sx = x > SV_MARGIN ? x : SV_MARGIN;
    sy = y > SV_MARGIN ? y : SV_MARGIN;
    if(cell == v->cell && sx == v->sx && sy == v->sy) return 0;
    v->cell = cell;
    v->sx = sx;
    v->sy = sy;
    v->px = sx + SW_W * cell + SV_MARGIN;
    v->py = sy;
    return 1;
}

int sview_cell(const struct sview *v, int x, int y) {
    const struct swell *s = &v->game;
    const struct swell_piece *p;
    int i, ghost;
    if(x < 0 || y < 0 || x >= SW_W || y >= SW_H) return SV_WATER;
    if(s->state == SW_PLAYING || s->state == SW_PAUSED) {
        p = swell_shape(s->kind, s->turn);
        for(i = 0; i < p->n; i++) if(s->x + p->x[i] == x && s->y + p->y[i] == y) return s->kind + 1;
        if(!s->sea[y][x]) {
            ghost = swell_ghost(s);
            for(i = 0; i < p->n; i++) if(s->x + p->x[i] == x && ghost + p->y[i] == y) return SV_GHOST;
        }
    }
    return s->sea[y][x];
}

int sview_flashing(const struct sview *v, int y, unsigned now) {
    return v->flash_at && y >= 0 && y < SW_H && v->game.washed[y] && now - v->flash_at < SV_FLASH_MS;
}

/* a game has ended: the session's scores, and what the status bar says */
static void ended(struct sview *v) {
    const struct swell *s = &v->game;
    char msg[SV_STATUS];
    v->games++;
    if(s->score > v->best || v->games == 1) {
        v->best = s->score;
        v->best_rows = s->rows;
        snprintf(msg, sizeof(msg), "Over the top: %u points, the best yet. Enter: again", s->score);
    } else snprintf(msg, sizeof(msg), "Over the top: %u points (best %u). Enter: again", s->score, v->best);
    status(v, msg);
}

/* after a key or the clock: a piece that set may have washed rows away (they show a moment) or ended the game */
static void settled(struct sview *v, int pieces, unsigned now) {
    const struct swell *s = &v->game;
    char msg[SV_STATUS];
    if(s->pieces == pieces) return;
    if(s->last_rows) {
        v->flash_at = now ? now : 1;
        snprintf(msg, sizeof(msg), "%d row%s washed away: %u points", s->last_rows, s->last_rows == 1 ? "" : "s",
                 s->last_points);
        status(v, msg);
    }
    if(s->state == SW_OVER) ended(v);
}

static void begin(struct sview *v, unsigned now) {
    sview_new(v);
    swell_start(&v->game, now);
}

void sview_restart(struct sview *v, unsigned now) {
    begin(v, now);
}

int sview_key(struct sview *v, int key, unsigned mods, unsigned now) {
    struct swell *s = &v->game;
    int pieces = s->pieces, r = 0, go = key == '\r' || key == '\n' || key == ' ';
    if(v->asking) return 0;             /* the question has the keys */
    if((mods & SV_REPEAT) && key != SV_LEFT && key != SV_RIGHT && key != SV_DOWN) return 0;
    if(key == 27 || key == 23) return sview_close(v, now);     /* Esc, Ctrl+W */
    switch(s->state) {
    case SW_READY:
        if(!go && key != 'n' && key != 'N') return 0;
        swell_start(s, now);
        v->status[0] = 0;
        return SV_DRAW;
    case SW_OVER:
        if(!go && key != 'n' && key != 'N') return 0;
        begin(v, now);
        return SV_DRAW;
    case SW_PAUSED:
        if(key == 'n' || key == 'N') begin(v, now);
        else if(go || key == 'p' || key == 'P') {
            swell_pause(s, 0, now);
            v->status[0] = 0;
        } else return 0;
        return SV_DRAW;
    }
    switch(key) {
    case SV_LEFT: r = swell_move(s, -1, now); break;
    case SV_RIGHT: r = swell_move(s, 1, now); break;
    case SV_UP: case 'x': case 'X': r = swell_turn(s, 1, now); break;
    case 'z': case 'Z': r = swell_turn(s, -1, now); break;
    case SV_DOWN: r = swell_down(s, now); break;
    case ' ': swell_drop(s, now); r = 1; break;
    case 'p': case 'P':
        swell_pause(s, 1, now);
        status(v, "Paused: P or Enter goes on, N starts again");
        return SV_DRAW;
    case 'n': case 'N':
        status(v, "P pauses the game; then N starts a new one");
        return SV_DRAW;
    default: return 0;
    }
    settled(v, pieces, now);
    return r ? SV_DRAW : 0;
}

int sview_tick(struct sview *v, unsigned now) {
    int pieces = v->game.pieces, r = swell_tick(&v->game, now);
    settled(v, pieces, now);
    if(v->flash_at && now - v->flash_at >= SV_FLASH_MS) {   /* the washed rows' moment is over */
        v->flash_at = 0;
        r = 1;
    }
    return r ? SV_DRAW : 0;
}

unsigned sview_wait(const struct sview *v, unsigned now) {
    unsigned w = swell_wait(&v->game, now), t;
    if(v->flash_at) {
        t = now - v->flash_at < SV_FLASH_MS ? SV_FLASH_MS - (now - v->flash_at) : 0;
        if(t < w) w = t;
    }
    if(v->game.state == SW_PLAYING) {   /* the time shown goes on a second at a time */
        t = 1000 - swell_ms(&v->game, now) % 1000;
        if(t < w) w = t;
    }
    return w;
}

int sview_focus(struct sview *v, int focused, unsigned now) {
    if(focused || v->game.state != SW_PLAYING) return 0;
    swell_pause(&v->game, 1, now);
    status(v, "Paused while in the background: P goes on");
    return SV_DRAW;
}

int sview_close(struct sview *v, unsigned now) {
    if(v->game.state != SW_PLAYING && v->game.state != SW_PAUSED) return SV_CLOSE;
    swell_pause(&v->game, 1, now);      /* nothing falls while main.c asks */
    v->asking = 1;
    return SV_DRAW;
}

int sview_answer(struct sview *v, int close) {
    v->asking = 0;
    if(close) return SV_CLOSE;
    status(v, "Paused: P or Enter goes on, N starts again");
    return SV_DRAW;
}

void sview_status(const struct sview *v, char *out, int n) {
    const char *how;
    switch(v->game.state) {
    case SW_READY: how = "Enter starts; arrows move, Up turns, Space drops"; break;
    case SW_PLAYING: how = "Up or X turns (Z back), Space drops, P pauses"; break;
    case SW_PAUSED: how = "Paused: P or Enter goes on, N starts again"; break;
    default: how = "Over the top. Enter or N starts again"; break;
    }
    snprintf(out, (size_t)n, "%s", v->status[0] ? v->status : how);
}
