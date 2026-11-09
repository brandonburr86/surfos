/*
SurfOS user program: wswell
--------------------
File: swell.c   Date: 11/05/26
--------------------
Swell's rules (swell.h). The pieces are given once, upright, and their quarter turns worked out
from that: a cell at x, y turns clockwise to -y, x on the screen (y grows down), and the cells are
moved back to the box's top left. A turn keeps the box's middle where it was (halves rounded
toward zero, so four turns come back to the same place), and when the turned piece does not fit
there it tries a column to the left, one to the right, a row up, two left and two right, in that
order, and stays as it was when none fits.

Time: the piece falls a row every swell_interval(level) milliseconds. When it cannot fall, it has
landed and sets SW_LOCK_MS later, unless a move or a turn lets it fall again meanwhile. A rest is
measured by the piece's lowest cell: a piece gets a new rest only when that cell comes to rest
lower than it ever rested, and otherwise keeps the time of its deepest rest, so neither sliding to
and fro over a gap nor turning it (a turn keeps the box's middle and may kick it a row up) can keep
it from setting (the review of 11/05/26). Down moves it a row and sets it when it cannot; Space
drops it to where it would rest and sets it at once. Each pays for the rows its lowest cell goes
below the deepest it had been (Down 1 point a row, Space 2), never for a row twice. A pause stops
the clock, the times of the fall and the rest with it, and the time played leaves pauses out.

Setting a piece writes its cells into the sea; each full row is washed away and the rows above it
come down. Rows washed at once score the level times 10, 25 or 50 (one, two, three: no piece is
taller), and a sea left with nothing in it 200 times the level more. Every SW_ROWS_LEVEL rows the
level goes up. From level SW_SWELL_LEVEL on, every SW_SWELL_PIECES pieces set, the swell lifts the
sea a row and leaves a row of sand with two gaps under it; anything pushed over the top ends the
game, as does a piece with no room to come in.

The pieces come from a set holding each kind twice, shuffled (Durstenfeld's form of the
Fisher-Yates shuffle, Knuth's TAOCP vol. 2, algorithm 3.4.2 P), and a new set when it is used up,
so no kind is long away; the numbers are Marsaglia's xorshift32 ("Xorshift RNGs", 2003).
*/
#include <string.h>
#include "swell.h"

const char *const swell_names[SW_KINDS] = { "pebble", "drift", "shell", "crest", "reef", "gull", "kelp" };

/* the pieces upright: cells by column and row */
static const struct { int n; signed char x[SW_CELLS], y[SW_CELLS]; } upright[SW_KINDS] = {
    { 2, { 0, 1 }, { 0, 0 } },                          /* pebble: two side by side */
    { 3, { 0, 1, 2 }, { 0, 0, 0 } },                    /* drift: three in a line */
    { 3, { 0, 0, 1 }, { 0, 1, 1 } },                    /* shell: a corner of three */
    { 5, { 0, 1, 0, 1, 0 }, { 0, 0, 1, 1, 2 } },        /* crest: a square with a cell under it */
    { 5, { 0, 2, 0, 1, 2 }, { 0, 0, 1, 1, 1 } },        /* reef: a cup */
    { 5, { 0, 0, 0, 1, 2 }, { 0, 1, 2, 2, 2 } },        /* gull: a wide corner */
    { 5, { 0, 1, 2, 1, 1 }, { 0, 0, 0, 1, 2 } },        /* kelp: a bar on a stem */
};

static struct swell_piece shapes[SW_KINDS][4];
static int shaped;

static void make_shapes(void) {
    int k, t, i, minx, miny, x;
    for(k = 0; k < SW_KINDS; k++) {
        struct swell_piece *p = &shapes[k][0];
        p->n = upright[k].n;
        for(i = 0; i < p->n; i++) {
            p->x[i] = upright[k].x[i];
            p->y[i] = upright[k].y[i];
        }
        for(t = 1; t < 4; t++) {                        /* each turn from the one before */
            struct swell_piece *q = &shapes[k][t];
            q->n = p->n;
            minx = miny = 99;
            for(i = 0; i < q->n; i++) {
                q->x[i] = (signed char)-p->y[i];
                q->y[i] = p->x[i];
                if(q->x[i] < minx) minx = q->x[i];
                if(q->y[i] < miny) miny = q->y[i];
            }
            for(i = 0; i < q->n; i++) {
                q->x[i] = (signed char)(q->x[i] - minx);
                q->y[i] = (signed char)(q->y[i] - miny);
            }
            p = q;
        }
        for(t = 0; t < 4; t++) {
            struct swell_piece *q = &shapes[k][t];
            q->w = q->h = 0;
            for(i = 0; i < q->n; i++) {
                x = q->x[i] + 1;
                if(x > q->w) q->w = x;
                if(q->y[i] + 1 > q->h) q->h = q->y[i] + 1;
            }
        }
    }
    shaped = 1;
}

const struct swell_piece *swell_shape(int kind, int turn) {
    if(!shaped) make_shapes();
    if(kind < 0 || kind >= SW_KINDS) kind = 0;
    return &shapes[kind][turn & 3];
}

/* the rows' times by level: about an eighth faster each, from 0.8 s to a tenth of a second */
unsigned swell_interval(int level) {
    static const unsigned ms[] = { 800, 700, 610, 530, 460, 400, 350, 305, 265, 230, 200, 175, 150, 130, 115, 100 };
    if(level < 1) level = 1;
    if(level > (int)(sizeof(ms) / sizeof(ms[0]))) level = (int)(sizeof(ms) / sizeof(ms[0]));
    return ms[level - 1];
}

/**** the generator ****/

unsigned swell_seed(unsigned seed) {
    seed ^= 0x6D2B79F5u;                /* xorshift32 must never hold 0 */
    return seed ? seed : 0x6D2B79F5u;
}

unsigned swell_next(unsigned *rng) {
    unsigned x = *rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *rng = x;
}

unsigned swell_below(unsigned *rng, unsigned n) {
    unsigned limit, v;
    if(n <= 1) return 0;
    limit = 0xFFFFFFFFu - 0xFFFFFFFFu % n;  /* past the last whole multiple of n: drawn again */
    do v = swell_next(rng) - 1; while(v >= limit);
    return v % n;
}

/* the next kind from the set, a new shuffled set when it is used up */
static int draw(struct swell *s) {
    int i, j;
    unsigned char t;
    if(!s->left) {
        for(i = 0; i < SW_SET; i++) s->set[i] = (unsigned char)(i % SW_KINDS);
        for(i = SW_SET - 1; i > 0; i--) {
            j = (int)swell_below(&s->rng, (unsigned)i + 1);
            t = s->set[i];
            s->set[i] = s->set[j];
            s->set[j] = t;
        }
        s->left = SW_SET;
    }
    return s->set[--s->left];
}

/**** the sea ****/

void swell_new(struct swell *s, unsigned seed) {
    memset(s, 0, sizeof(*s));
    s->rng = swell_seed(seed);
    s->level = 1;
    s->state = SW_READY;
    s->next = draw(s);                  /* shown before the first piece comes in */
}

/* the falling piece may reach above the sea (a turn at the top needs that room), never past its sides or bottom */
int swell_fits(const struct swell *s, int kind, int turn, int x, int y) {
    const struct swell_piece *p = swell_shape(kind, turn);
    int i, cx, cy;
    for(i = 0; i < p->n; i++) {
        cx = x + p->x[i];
        cy = y + p->y[i];
        if(cx < 0 || cx >= SW_W || cy >= SW_H || (cy >= 0 && s->sea[cy][cx])) return 0;
    }
    return 1;
}

static unsigned later(unsigned now, unsigned ms) {
    unsigned t = now + ms;
    return t ? t : 1;                   /* 0 means "not landed" */
}

static void over(struct swell *s, unsigned now) {
    if(s->state == SW_PLAYING) s->played += now - s->since;
    s->state = SW_OVER;
    s->lock_at = 0;
}

/* the next piece comes in at the top, in the middle; with no room for it the game is over */
static void spawn(struct swell *s, unsigned now) {
    const struct swell_piece *p;
    s->kind = s->next;
    s->next = draw(s);
    s->turn = 0;
    p = swell_shape(s->kind, 0);
    s->x = (SW_W - p->w) / 2;
    s->y = 0;
    s->fall_at = later(now, swell_interval(s->level));
    s->lock_at = 0;
    s->rest_low = SW_NO_REST;
    s->deepest = s->y + p->h - 1;
    if(!swell_fits(s, s->kind, 0, s->x, s->y)) over(s, now);
}

/* the row of the falling piece's lowest cell */
static int lowest(const struct swell *s) {
    return s->y + swell_shape(s->kind, s->turn)->h - 1;
}

/* the rows its lowest cell has gone below the deepest it had been: the points Down and Space pay for */
static int deeper(struct swell *s) {
    int low = lowest(s), rows = low > s->deepest ? low - s->deepest : 0;
    if(low > s->deepest) s->deepest = low;
    return rows;
}

int swell_start(struct swell *s, unsigned now) {
    if(s->state != SW_READY) return 0;
    s->state = SW_PLAYING;
    s->since = now;
    spawn(s, now);
    return 1;
}

/* after a fall, a move or a turn: able to fall, the piece has not landed; landed with its lowest cell lower than it
   ever rested, it rests SW_LOCK_MS from now; anywhere else it sets at the time of its deepest rest */
static void landing(struct swell *s, unsigned now) {
    int low;
    if(swell_fits(s, s->kind, s->turn, s->x, s->y + 1)) {
        s->lock_at = 0;
        return;
    }
    low = lowest(s);
    if(low > s->rest_low) {
        s->rest_low = low;
        s->rest_at = later(now, SW_LOCK_MS);
    }
    s->lock_at = s->rest_at;
}

/* the swell: the sea a row up, sand with two gaps under it; anything in the top row goes over: the game ends */
static void lift(struct swell *s, unsigned now) {
    int x, g1, g2;
    for(x = 0; x < SW_W; x++) if(s->sea[0][x]) { over(s, now); return; }
    memmove(s->sea[0], s->sea[1], sizeof(s->sea[0]) * (SW_H - 1));
    g1 = (int)swell_below(&s->rng, SW_W);
    g2 = (int)swell_below(&s->rng, SW_W - 1);
    if(g2 >= g1) g2++;                  /* two gaps, never the same */
    for(x = 0; x < SW_W; x++) s->sea[SW_H - 1][x] = x == g1 || x == g2 ? 0 : SW_SAND;
    s->swells++;
}

/* the piece sets: its cells into the sea, full rows washed away, the score, the level, the swell, the next piece */
static void set(struct swell *s, unsigned now) {
    static const unsigned points[] = { 0, 10, 25, 50, 80, 120 };
    const struct swell_piece *p = swell_shape(s->kind, s->turn);
    int i, x, y, n = 0, full, empty = 1;
    for(i = 0; i < p->n; i++) if(s->y + p->y[i] < 0) { over(s, now); return; }   /* set above the sea: over the top */
    for(i = 0; i < p->n; i++) s->sea[s->y + p->y[i]][s->x + p->x[i]] = (unsigned char)(s->kind + 1);
    s->pieces++;
    memset(s->washed, 0, sizeof(s->washed));
    for(y = SW_H - 1; y >= 0; y--) {    /* from the bottom; a row washed lets the rows above come down into it */
        for(x = 0, full = 1; x < SW_W; x++) if(!s->sea[y][x]) full = 0;
        if(!full) continue;
        memmove(s->sea[1], s->sea[0], sizeof(s->sea[0]) * (size_t)y);
        memset(s->sea[0], 0, sizeof(s->sea[0]));
        s->washed[y - n] = 1;           /* where it was before the rows washed under it let it down */
        n++;
        y++;                            /* the row that came down is looked at again */
    }
    s->washed_n = n;
    s->last_rows = n;
    s->last_points = 0;
    if(n) {
        for(y = 0; y < SW_H && empty; y++) for(x = 0; x < SW_W; x++) if(s->sea[y][x]) { empty = 0; break; }
        s->last_points = points[n < 6 ? n : 5] * (unsigned)s->level + (empty ? 200u * (unsigned)s->level : 0);
        s->score += s->last_points;
        s->rows += n;
        if(1 + s->rows / SW_ROWS_LEVEL > s->level) s->level = 1 + s->rows / SW_ROWS_LEVEL;
    }
    if(s->level >= SW_SWELL_LEVEL && ++s->since_swell >= SW_SWELL_PIECES) {
        s->since_swell = 0;
        lift(s, now);
        if(s->state == SW_OVER) return;
    }
    spawn(s, now);
}

int swell_move(struct swell *s, int dx, unsigned now) {
    if(s->state != SW_PLAYING || !swell_fits(s, s->kind, s->turn, s->x + dx, s->y)) return 0;
    s->x += dx;
    landing(s, now);
    return 1;
}

int swell_turn(struct swell *s, int dir, unsigned now) {
    static const signed char kick[][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { -2, 0 }, { 2, 0 } };
    const struct swell_piece *a, *b;
    int t, i, x, y;
    if(s->state != SW_PLAYING) return 0;
    t = (s->turn + (dir < 0 ? 3 : 1)) & 3;
    a = swell_shape(s->kind, s->turn);
    b = swell_shape(s->kind, t);
    x = s->x + (a->w - b->w) / 2;       /* the box's middle kept */
    y = s->y + (a->h - b->h) / 2;
    for(i = 0; i < (int)(sizeof(kick) / sizeof(kick[0])); i++)
        if(swell_fits(s, s->kind, t, x + kick[i][0], y + kick[i][1])) {
            s->turn = t;
            s->x = x + kick[i][0];
            s->y = y + kick[i][1];
            deeper(s);                  /* a turn can bring the lowest cell a row down: Down does not pay for it again */
            landing(s, now);
            return 1;
        }
    return 0;
}

int swell_ghost(const struct swell *s) {
    int y = s->y;
    while(swell_fits(s, s->kind, s->turn, s->x, y + 1)) y++;
    return y;
}

int swell_down(struct swell *s, unsigned now) {
    if(s->state != SW_PLAYING) return 0;
    if(!swell_fits(s, s->kind, s->turn, s->x, s->y + 1)) {
        set(s, now);                    /* landed: Down sets it */
        return 1;
    }
    s->y++;
    s->score += (unsigned)deeper(s);    /* a point for a row it had not been down to */
    s->fall_at = later(now, swell_interval(s->level));
    landing(s, now);
    return 1;
}

int swell_drop(struct swell *s, unsigned now) {
    int rows;
    if(s->state != SW_PLAYING) return 0;
    rows = swell_ghost(s) - s->y;
    s->y += rows;
    s->score += 2u * (unsigned)deeper(s);
    set(s, now);
    return rows;
}

int swell_tick(struct swell *s, unsigned now) {
    int changed = 0;
    if(s->state != SW_PLAYING) return 0;
    if(s->lock_at && (int)(now - s->lock_at) >= 0) {
        if(!swell_fits(s, s->kind, s->turn, s->x, s->y + 1)) {
            set(s, now);
            return 1;
        }
        s->lock_at = 0;
    }
    while((int)(now - s->fall_at) >= 0) {   /* every row that is due, also after a stall */
        if(!swell_fits(s, s->kind, s->turn, s->x, s->y + 1)) {
            landing(s, now);
            s->fall_at = later(now, swell_interval(s->level));
            break;
        }
        s->y++;
        deeper(s);
        s->fall_at = later(s->fall_at, swell_interval(s->level));
        landing(s, now);                /* come to rest by this row: it rests from now */
        changed = 1;
    }
    return changed;
}

unsigned swell_wait(const struct swell *s, unsigned now) {
    int fall, lock;
    if(s->state != SW_PLAYING) return 0xFFFFFFFFu;
    fall = (int)(s->fall_at - now);
    if(fall < 0) fall = 0;
    if(s->lock_at) {
        lock = (int)(s->lock_at - now);
        if(lock < 0) lock = 0;
        if(lock < fall) fall = lock;
    }
    return (unsigned)fall;
}

int swell_pause(struct swell *s, int on, unsigned now) {
    unsigned gone;
    if(on && s->state == SW_PLAYING) {
        s->state = SW_PAUSED;
        s->paused_at = now;
        s->played += now - s->since;
        return 1;
    }
    if(!on && s->state == SW_PAUSED) {  /* the clock goes on where it stopped: the fall's time and the rest's */
        gone = now - s->paused_at;
        s->fall_at = later(s->fall_at, gone);
        if(s->lock_at) s->lock_at = later(s->lock_at, gone);
        if(s->rest_low != SW_NO_REST) s->rest_at = later(s->rest_at, gone);
        s->since = now;
        s->state = SW_PLAYING;
        return 1;
    }
    return 0;
}

unsigned swell_ms(const struct swell *s, unsigned now) {
    return s->played + (s->state == SW_PLAYING ? now - s->since : 0);
}
