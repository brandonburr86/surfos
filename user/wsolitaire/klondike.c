/*
SurfOS user program: wsolitaire
--------------------
File: klondike.c Date: 11/05/26
--------------------
Solitaire's rules (klondike.h), Klondike as it is commonly played: the deal, the stock dealing
one or three cards to the waste, the waste taken back without limit, the foundations up by suit
from the ace, the tableau down in alternate colours with kings on empty piles, runs moving
together, and a face-down top turning up by itself (so the game needs no click for that). A card
may come back down from a foundation onto the tableau. The deck is shuffled with Durstenfeld's
form of the Fisher-Yates shuffle (Knuth, TAOCP vol. 2, algorithm 3.4.2 P) on Marsaglia's
xorshift32 ("Xorshift RNGs", 2003).

Every move is kept in a ring of the newest SOL_UNDO for Undo: what moved, from where to where,
and whether the card left on top was turned up. A click on a card sends it where it can go
(sol_target()): a single card to its foundation first, then any card or run to the next tableau
pile it fits, looking right of its own pile first; a king that already heads its pile stays.

Finishing by itself: once no card on the tableau is face down, a copy of the game is played out
by the finish's own rule (the lowest card that can go home goes; with none, the stock deals; an
empty stock takes the waste back, unless a whole pass went by with no card going home). When the
copy wins, the game itself is played out the same way, a step at a time for the window to show.
Every card is then home: of the cards not home, the lowest is at the top of its tableau pile
(everything on it would be lower), or in the stock or the waste, which the passes show. When the
copy cannot win (three at a time can hide a card on every pass), the player plays on.
*/
#include <string.h>
#include "klondike.h"

/**** the generator ****/

unsigned sol_seed(unsigned seed) {
    seed ^= 0x6D2B79F5u;                /* xorshift32 must never hold 0 */
    return seed ? seed : 0x6D2B79F5u;
}

unsigned sol_next(unsigned *rng) {
    unsigned x = *rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *rng = x;
}

unsigned sol_below(unsigned *rng, unsigned n) {
    unsigned limit, v;
    if(n <= 1) return 0;
    limit = 0xFFFFFFFFu - 0xFFFFFFFFu % n;  /* the values past the last whole multiple of n are drawn again */
    do v = sol_next(rng) - 1; while(v >= limit);
    return v % n;
}

/**** the deal ****/

static void push(struct sol_pile *p, unsigned char c) {
    if(p->n < SOL_CARDS) p->card[p->n++] = c;
}

static unsigned char pop(struct sol_pile *p) {
    return p->n ? p->card[--p->n] : 0;
}

void sol_new(struct klondike *k, int draw, unsigned seed) {
    unsigned char deck[SOL_CARDS], t;
    unsigned rng = sol_seed(seed);
    int i, j, n = 0;
    memset(k, 0, sizeof(*k));
    k->draw = draw == 3 ? 3 : 1;
    for(i = 0; i < SOL_CARDS; i++) deck[i] = (unsigned char)i;
    for(i = SOL_CARDS - 1; i > 0; i--) {
        j = (int)sol_below(&rng, (unsigned)i + 1);
        t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    for(i = 0; i < 7; i++)                  /* a row at a time, from pile i on: pile p gets p + 1 cards */
        for(j = i; j < 7; j++) push(&k->pile[SOL_TABLEAU + j], deck[n++]);
    for(j = 0; j < 7; j++) k->pile[SOL_TABLEAU + j].card[j] |= SOL_UP;
    while(n < SOL_CARDS) push(&k->pile[SOL_STOCK], deck[n++]);
}

/**** what may move where ****/

int sol_can_take(const struct klondike *k, int from, int index) {
    const struct sol_pile *p;
    int i;
    if(from < 0 || from >= SOL_PILES) return 0;
    p = &k->pile[from];
    if(index < 0 || index >= p->n || k->won) return 0;
    if(from == SOL_WASTE || SOL_IS_FOUNDATION(from)) return index == p->n - 1;
    if(!SOL_IS_TABLEAU(from)) return 0;
    for(i = index; i < p->n; i++) {         /* a face-up run, down in alternate colours (the rules keep it so) */
        if(!(p->card[i] & SOL_UP)) return 0;
        if(i > index && (SOL_RANK(p->card[i]) + 1 != SOL_RANK(p->card[i - 1]) ||
                         SOL_RED(p->card[i]) == SOL_RED(p->card[i - 1]))) return 0;
    }
    return 1;
}

/* card (with count - 1 cards on it) may go on pile to */
static int fits(const struct klondike *k, unsigned char card, int count, int to) {
    const struct sol_pile *p = &k->pile[to];
    unsigned char top = p->n ? p->card[p->n - 1] : 0;
    if(SOL_IS_FOUNDATION(to)) {
        if(count != 1) return 0;
        if(!p->n) return SOL_RANK(card) == 1;
        return SOL_SUIT(top) == SOL_SUIT(card) && SOL_RANK(card) == SOL_RANK(top) + 1;
    }
    if(SOL_IS_TABLEAU(to)) {
        if(!p->n) return SOL_RANK(card) == 13;
        return (top & SOL_UP) && SOL_RED(top) != SOL_RED(card) && SOL_RANK(card) + 1 == SOL_RANK(top);
    }
    return 0;
}

int sol_can_put(const struct klondike *k, int from, int index, int to) {
    if(to < 0 || to >= SOL_PILES || to == from || !sol_can_take(k, from, index)) return 0;
    return fits(k, k->pile[from].card[index], k->pile[from].n - index, to);
}

int sol_home(const struct klondike *k) {
    int f, n = 0;
    for(f = SOL_FOUNDATION; f < SOL_TABLEAU; f++) n += k->pile[f].n;
    return n;
}

int sol_face_down(const struct klondike *k) {
    int t, i, n = 0;
    for(t = SOL_TABLEAU; t < SOL_PILES; t++)
        for(i = 0; i < k->pile[t].n; i++) n += !(k->pile[t].card[i] & SOL_UP);
    return n;
}

/**** moves, and Undo ****/

static void record(struct klondike *k, int kind, int from, int to, int count, int flipped, unsigned now) {
    struct sol_move *m = &k->undo[k->undo_top];
    m->kind = (unsigned char)kind;
    m->from = (unsigned char)from;
    m->to = (unsigned char)to;
    m->count = (unsigned char)count;
    m->flipped = (unsigned char)flipped;
    k->undo_top = (k->undo_top + 1) % SOL_UNDO;
    if(k->undo_n < SOL_UNDO) k->undo_n++;
    k->moves++;
    if(!k->started) {                       /* the timer starts with the first move */
        k->started = 1;
        k->start = now;
    }
    if(sol_home(k) == SOL_CARDS) {
        k->won = 1;
        k->end = now;
    }
}

/* the cards from index up go from one pile to the other, in their order; a tableau's new top turns up */
static void transfer(struct klondike *k, int from, int index, int to, unsigned now) {
    struct sol_pile *a = &k->pile[from], *b = &k->pile[to];
    int count = a->n - index, i, flipped = 0;
    for(i = index; i < a->n; i++) push(b, a->card[i]);
    a->n = index;
    if(SOL_IS_TABLEAU(from) && a->n && !(a->card[a->n - 1] & SOL_UP)) {
        a->card[a->n - 1] |= SOL_UP;
        flipped = 1;
    }
    if(SOL_IS_FOUNDATION(to)) k->idle = 0;  /* a card went home: the finish may take the waste back again */
    record(k, SOL_MOVE, from, to, count, flipped, now);
}

int sol_move(struct klondike *k, int from, int index, int to, unsigned now) {
    if(!sol_can_put(k, from, index, to)) return 0;
    k->idle = 0;
    transfer(k, from, index, to, now);
    return 1;
}

static int deal(struct klondike *k, unsigned now) {
    struct sol_pile *stock = &k->pile[SOL_STOCK], *waste = &k->pile[SOL_WASTE];
    int n, i;
    if(k->won) return 0;
    if(stock->n) {                          /* one or three, each turned up onto the waste */
        n = stock->n < k->draw ? stock->n : k->draw;
        for(i = 0; i < n; i++) push(waste, (unsigned char)(pop(stock) | SOL_UP));
        record(k, SOL_DEAL, SOL_STOCK, SOL_WASTE, n, 0, now);
        return 1;
    }
    if(!waste->n) return 0;
    n = waste->n;                           /* the waste turned over: its first card is the stock's top again */
    for(i = 0; i < n; i++) push(stock, (unsigned char)(pop(waste) & ~SOL_UP));
    k->turns++;
    record(k, SOL_TURN, SOL_WASTE, SOL_STOCK, n, 0, now);
    return 1;
}

int sol_deal(struct klondike *k, unsigned now) {
    k->idle = 0;
    return deal(k, now);
}

int sol_can_undo(const struct klondike *k) {
    return k->undo_n > 0 && !k->won;
}

int sol_undo(struct klondike *k, unsigned now) {
    struct sol_move *m;
    struct sol_pile *a, *b;
    int i, at;
    if(!sol_can_undo(k)) return 0;
    k->undo_top = (k->undo_top + SOL_UNDO - 1) % SOL_UNDO;
    k->undo_n--;
    m = &k->undo[k->undo_top];
    a = &k->pile[m->from];
    b = &k->pile[m->to];
    switch(m->kind) {
    case SOL_MOVE:                          /* the run back where it was, and the card it uncovered down again */
        if(m->flipped && a->n) a->card[a->n - 1] &= (unsigned char)~SOL_UP;
        at = b->n - m->count;
        for(i = at; i < b->n; i++) push(a, b->card[i]);
        b->n = at;
        break;
    case SOL_DEAL:                          /* the cards dealt back onto the stock, face down, in their order */
        for(i = 0; i < m->count; i++) push(a, (unsigned char)(pop(b) & ~SOL_UP));
        break;
    case SOL_TURN:                          /* the stock turned over onto the waste again */
        for(i = 0; i < m->count; i++) push(a, (unsigned char)(pop(b) | SOL_UP));
        k->turns--;
        break;
    }
    k->moves++;                             /* an undo is a move: taking one back never lowers the count */
    k->idle = 0;
    (void)now;
    return 1;
}

/**** where a click sends a card ****/

int sol_target(const struct klondike *k, int from, int index) {
    const struct sol_pile *p;
    int count, i, t;
    unsigned char card;
    if(!sol_can_take(k, from, index)) return -1;
    p = &k->pile[from];
    count = p->n - index;
    card = p->card[index];
    if(count == 1 && !SOL_IS_FOUNDATION(from))
        for(t = SOL_FOUNDATION; t < SOL_TABLEAU; t++) if(fits(k, card, 1, t)) return t;
    for(i = 0; i < 7; i++) {                /* the tableau, from the pile right of its own round to its left */
        t = SOL_TABLEAU + ((SOL_IS_TABLEAU(from) ? from - SOL_TABLEAU + 1 : 0) + i) % 7;
        if(t == from || !fits(k, card, count, t)) continue;
        if(SOL_IS_TABLEAU(from) && index == 0 && !k->pile[t].n) continue;  /* a king heading its pile already */
        return t;
    }
    return -1;
}

/**** finishing by itself ****/

/* the step: the lowest card that can go home (the waste's top or a tableau's), or a deal, or the waste taken back once
   a card has gone home since the last time; 0 when there is none of these */
static int step(struct klondike *k, unsigned now) {
    int p, f, best = -1, bestf = -1, rank = 14;
    for(p = SOL_WASTE; p < SOL_PILES; p++) {
        const struct sol_pile *q = &k->pile[p];
        if(SOL_IS_FOUNDATION(p) || !q->n || SOL_RANK(q->card[q->n - 1]) >= rank) continue;
        for(f = SOL_FOUNDATION; f < SOL_TABLEAU; f++)
            if(fits(k, q->card[q->n - 1], 1, f)) {
                best = p;
                bestf = f;
                rank = SOL_RANK(q->card[q->n - 1]);
                break;
            }
    }
    if(best >= 0) {
        transfer(k, best, k->pile[best].n - 1, bestf, now);
        return 1;
    }
    if(k->pile[SOL_STOCK].n) return deal(k, now);
    if(!k->pile[SOL_WASTE].n || k->idle) return 0;
    k->idle = 1;                            /* until a card goes home: the next pass without one is the last */
    return deal(k, now);
}

int sol_finishing(const struct klondike *k) {
    static struct klondike copy;            /* a copy played out exactly as the game will be (6 KB with the ring) */
    int steps;
    if(k->won || !k->started || sol_face_down(k)) return 0;
    copy = *k;
    for(steps = 0; steps < 4 * SOL_CARDS * SOL_CARDS && !copy.won; steps++) if(!step(&copy, 0)) break;
    return copy.won;
}

int sol_finish_step(struct klondike *k, unsigned now) {
    if(k->won) return 0;
    return step(k, now);
}

unsigned sol_ms(const struct klondike *k, unsigned now) {
    if(!k->started) return 0;
    return (k->won ? k->end : now) - k->start;
}
