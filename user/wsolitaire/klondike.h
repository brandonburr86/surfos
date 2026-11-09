/*
SurfOS user program: wsolitaire
--------------------
File: klondike.h Date: 11/05/26
--------------------
Solitaire's rules (klondike.c): Klondike, drawing one card or three. A shuffled deck is dealt
into seven tableau piles, one to seven cards with the top one face up, and the rest is the stock.
The stock deals to the waste, one or three at a time, and an empty stock takes the waste back as
often as the player likes. Cards go up on four foundations by suit from the ace, and down on the
tableau in alternate colours, a king or a run from a king on an empty pile; a run of face-up cards
moves together, and a tableau's face-down top turns up by itself. Every move can be undone. Once
no card on the table is face down and the cards can all go home from there, the game finishes
by itself, a card a step (sol_finishing(), sol_finish_step()). The deck comes from a seeded
generator, so a test repeats a deal. No system calls and no drawing: tests/host/test_wsolitaire.c
runs it on the host as it is; table.c is the window's side.
*/
#ifndef _WSOLITAIRE_KLONDIKE_H
#define _WSOLITAIRE_KLONDIKE_H

/* a card: suit * 13 + rank - 1, and SOL_UP while it shows its face */
#define SOL_CARDS 52
#define SOL_UP 0x40
#define SOL_CARD(c) ((c) & 0x3F)
#define SOL_RANK(c) (SOL_CARD(c) % 13 + 1)     /* 1 the ace, 11 to 13 the jack, queen and king */
#define SOL_SUIT(c) (SOL_CARD(c) / 13)
#define SOL_RED(c) (SOL_SUIT(c) & 1)
#define SOL_SPADES 0                    /* the suits, black and red in turn */
#define SOL_HEARTS 1
#define SOL_CLUBS 2
#define SOL_DIAMONDS 3

/* the piles */
#define SOL_STOCK 0
#define SOL_WASTE 1
#define SOL_FOUNDATION 2                /* four: 2 to 5 */
#define SOL_TABLEAU 6                   /* seven: 6 to 12 */
#define SOL_PILES 13
#define SOL_IS_FOUNDATION(p) ((p) >= SOL_FOUNDATION && (p) < SOL_TABLEAU)
#define SOL_IS_TABLEAU(p) ((p) >= SOL_TABLEAU && (p) < SOL_PILES)

struct sol_pile { int n; unsigned char card[SOL_CARDS]; };     /* bottom first */

/* a move, kept for Undo: cards from a pile to another (and whether the one left on top turned up), a deal from the
   stock, or the waste taken back into it */
#define SOL_MOVE 1
#define SOL_DEAL 2
#define SOL_TURN 3
struct sol_move { unsigned char kind, from, to, count, flipped; };
#define SOL_UNDO 1024                   /* the newest moves kept */

struct klondike {
    int draw;                           /* 1 or 3 */
    struct sol_pile pile[SOL_PILES];
    int moves;                          /* moves made, Undo and the game's own finish included */
    int turns;                          /* times the waste was taken back into the stock */
    int started, won;                   /* the first move was made; every card is home */
    unsigned start, end;                /* the timer: from the first move to the last card home */
    int idle;                           /* the finish took the waste back without a card going home since */
    struct sol_move undo[SOL_UNDO];     /* a ring of the newest moves */
    int undo_top, undo_n;
};

/* the generator (Marsaglia's xorshift32), as in Mines: a state from any seed, its next number, one below n */
unsigned sol_seed(unsigned seed);
unsigned sol_next(unsigned *rng);
unsigned sol_below(unsigned *rng, unsigned n);

void sol_new(struct klondike *k, int draw, unsigned seed);  /* shuffled and dealt; draw 3, or 1 for anything else */
/* the cards from index up of pile from can be picked up: the waste's or a foundation's top, a tableau's face-up run */
int sol_can_take(const struct klondike *k, int from, int index);
int sol_can_put(const struct klondike *k, int from, int index, int to);
int sol_move(struct klondike *k, int from, int index, int to, unsigned now);   /* 1 when made */
int sol_deal(struct klondike *k, unsigned now);         /* the stock: one or three to the waste, or it back: 1 when */
int sol_target(const struct klondike *k, int from, int index);  /* where a click sends them: a pile, or -1 */
int sol_undo(struct klondike *k, unsigned now);         /* the last move taken back: 1 when one was (never after a win) */
int sol_can_undo(const struct klondike *k);
int sol_home(const struct klondike *k);                 /* cards on the foundations */
int sol_face_down(const struct klondike *k);            /* cards on the tableau still face down */
int sol_finishing(const struct klondike *k);            /* it can finish by itself from here, and has not won */
int sol_finish_step(struct klondike *k, unsigned now);  /* the next step of that: 1 when one was made */
unsigned sol_ms(const struct klondike *k, unsigned now);

#endif
