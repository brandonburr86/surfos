/*
SurfOS user program: wmines
--------------------
File: mines.c   Date: 11/05/26
--------------------
Mines' rules (mines.h). The field is laid only when the first cell is revealed, so that cell can
be kept clear, and with it its neighbours whenever the other cells have room for every mine: the
first reveal then always opens an area rather than a single number. Each set of cells is as
likely as any other: the mines are the first cells of a shuffle of the allowed ones (Durstenfeld's
form of the Fisher-Yates shuffle, Knuth's TAOCP vol. 2, algorithm 3.4.2 P), and the numbers come
from Marsaglia's xorshift32 ("Xorshift RNGs", Journal of Statistical Software 8(14), 2003), drawn
below a bound by rejection so that no number is likelier than another.

A cell with no mine around it opens its neighbours: a queue of cells (each put there once, as it
is opened), so the deepest field costs one array the size of the field and no recursion. Flagged
cells are never opened by the queue or a chord; a chord opens the covered cells around a number
whose flags are all placed, and a wrong flag then lets a mine go off. The game is won when every
cell without a mine is open: the mines are then flagged.
*/
#include <string.h>
#include "mines.h"

/* SurfOS's own three fields: about one cell in eight, six and five a mine */
const struct mines_size mines_sizes[MINES_SIZES] = {
    { "Small", 10, 8, 10 },
    { "Medium", 16, 12, 32 },
    { "Large", 28, 15, 80 },
};

/**** the generator ****/

unsigned mines_seed(unsigned seed) {
    seed ^= 0x6D2B79F5u;                /* any seed: xorshift32 must never hold 0, which stays 0 */
    return seed ? seed : 0x6D2B79F5u;
}

unsigned mines_next(unsigned *rng) {
    unsigned x = *rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *rng = x;
}

/* xorshift32 gives 1 to 2^32 - 1: one less, 0 to 2^32 - 2, is 2^32 - 1 values; those at or past the last whole
   multiple of n are drawn again, so every remainder is as likely */
unsigned mines_below(unsigned *rng, unsigned n) {
    unsigned limit, v;
    if(n <= 1) return 0;
    limit = 0xFFFFFFFFu - 0xFFFFFFFFu % n;
    do v = mines_next(rng) - 1; while(v >= limit);
    return v % n;
}

/**** the field ****/

static int inside(const struct mines *m, int x, int y) {
    return x >= 0 && y >= 0 && x < m->w && y < m->h;
}

int mines_over(const struct mines *m) {
    return m->state == MINES_WON || m->state == MINES_LOST;
}

int mines_new(struct mines *m, int w, int h, int mines, unsigned seed) {
    if(w < 2 || h < 2 || w > MINES_MAX_W || h > MINES_MAX_H || mines < 1 || mines > w * h - 1) return -1;
    memset(m, 0, sizeof(*m));
    m->w = w;
    m->h = h;
    m->mines = mines;
    m->state = MINES_READY;
    m->rng = mines_seed(seed);
    return 0;
}

/* the mines, at the first reveal at x0, y0: never there, nor beside it when the rest of the field holds them all */
static void lay(struct mines *m, int x0, int y0) {
    int allowed[MINES_MAX], n = 0, cells = m->w * m->h, keep = 0, i, j, k, x, y, dx, dy;
    for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++) keep += inside(m, x0 + dx, y0 + dy);
    keep = m->mines <= cells - keep;    /* room for the neighbours to stay clear */
    for(i = 0; i < cells; i++) {
        x = i % m->w;
        y = i / m->w;
        if(x == x0 && y == y0) continue;
        if(keep && x >= x0 - 1 && x <= x0 + 1 && y >= y0 - 1 && y <= y0 + 1) continue;
        allowed[n++] = i;
    }
    for(k = 0; k < m->mines; k++) {     /* the first m->mines of a shuffle of the allowed cells */
        j = k + (int)mines_below(&m->rng, (unsigned)(n - k));
        i = allowed[k];
        allowed[k] = allowed[j];
        allowed[j] = i;
        m->cell[allowed[k]] |= MINE_HERE;
    }
    for(i = 0; i < cells; i++) {
        x = i % m->w;
        y = i / m->w;
        for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++)
            if((dx || dy) && inside(m, x + dx, y + dy) && (m->cell[(y + dy) * m->w + x + dx] & MINE_HERE)) m->near[i]++;
    }
}

/* the cell at i opened; from every one with no mine around it its neighbours, through a queue that holds each cell
   once (it is opened as it is queued), so it never needs more room than the field has */
static void open_from(struct mines *m, int i) {
    int queue[MINES_MAX], head = 0, tail = 0, x, y, dx, dy, j;
    if(m->cell[i] & MINE_HERE) {        /* a mine goes off: the game is lost */
        m->cell[i] |= MINE_OPEN | MINE_BLAST;
        m->state = MINES_LOST;
        return;
    }
    m->cell[i] |= MINE_OPEN;
    m->open++;
    queue[tail++] = i;
    while(head < tail) {
        i = queue[head++];
        if(m->near[i]) continue;
        x = i % m->w;
        y = i / m->w;
        for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++) {
            if((!dx && !dy) || !inside(m, x + dx, y + dy)) continue;
            j = (y + dy) * m->w + x + dx;
            if(m->cell[j] & (MINE_OPEN | MINE_FLAG | MINE_HERE)) continue;
            m->cell[j] |= MINE_OPEN;
            m->open++;
            queue[tail++] = j;
        }
    }
}

/* after cells opened: lost when a mine went off, won when only mines are covered (they are flagged then) */
static int settle(struct mines *m, unsigned now) {
    int i;
    if(m->state == MINES_LOST) {
        m->end = now;
        return MINES_ENDED;
    }
    if(m->open < m->w * m->h - m->mines) return MINES_MOVED;
    m->state = MINES_WON;
    m->end = now;
    for(i = 0; i < m->w * m->h; i++) if(m->cell[i] & MINE_HERE) m->cell[i] |= MINE_FLAG;
    m->flags = m->mines;
    return MINES_ENDED;
}

int mines_reveal(struct mines *m, int x, int y, unsigned now) {
    int i;
    if(!inside(m, x, y) || mines_over(m)) return MINES_NONE;
    i = y * m->w + x;
    if(m->cell[i] & (MINE_OPEN | MINE_FLAG)) return MINES_NONE;
    if(m->state == MINES_READY) {       /* the first reveal: the timer starts and the mines are laid around it */
        lay(m, x, y);
        m->state = MINES_PLAYING;
        m->start = now;
    }
    open_from(m, i);
    return settle(m, now);
}

int mines_flags_around(const struct mines *m, int x, int y) {
    int n = 0, dx, dy;
    for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++)
        if((dx || dy) && inside(m, x + dx, y + dy) && (m->cell[(y + dy) * m->w + x + dx] & MINE_FLAG)) n++;
    return n;
}

int mines_chord(struct mines *m, int x, int y, unsigned now) {
    int i, j, dx, dy, opened = 0;
    if(!inside(m, x, y) || m->state != MINES_PLAYING) return MINES_NONE;
    i = y * m->w + x;
    if(!(m->cell[i] & MINE_OPEN) || !m->near[i] || mines_flags_around(m, x, y) != m->near[i]) return MINES_NONE;
    for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++) {
        if((!dx && !dy) || !inside(m, x + dx, y + dy)) continue;
        j = (y + dy) * m->w + x + dx;
        if(m->cell[j] & (MINE_OPEN | MINE_FLAG)) continue;
        open_from(m, j);                /* every covered one, also after a mine went off: all of them show */
        opened = 1;
    }
    return opened ? settle(m, now) : MINES_NONE;
}

int mines_flag(struct mines *m, int x, int y) {
    unsigned char *c;
    if(!inside(m, x, y) || mines_over(m)) return MINES_NONE;
    c = &m->cell[y * m->w + x];
    if(*c & MINE_OPEN) return MINES_NONE;
    *c ^= MINE_FLAG;
    m->flags += *c & MINE_FLAG ? 1 : -1;
    return MINES_MOVED;
}

int mines_left(const struct mines *m) {
    return m->mines - m->flags;
}

unsigned mines_ms(const struct mines *m, unsigned now) {
    if(m->state == MINES_READY) return 0;
    return (mines_over(m) ? m->end : now) - m->start;
}
