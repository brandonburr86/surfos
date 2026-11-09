/*
SurfOS user program: wmines
--------------------
File: mines.h   Date: 11/05/26
--------------------
Mines' rules (mines.c): a field of covered cells, some of them mines. The mines are laid at the
first reveal, never under it (nor around it, where the field has room), so the first reveal is
always safe. A reveal opens a cell; a cell with no mine around it opens its neighbours, and theirs,
through a queue of cells, never by recursion. Flags mark the cells the player takes for mines;
chording a revealed number whose flags are all placed opens the rest around it. The timer runs
from the first reveal to the end, on the milliseconds main.c hands in. The generator is seeded,
so a test repeats a field. No system calls and no drawing: tests/host/test_wmines.c runs it on the
host as it is. The window's side (the layout, the keys, the pointer, the session's scores) is
view.c.
*/
#ifndef _WMINES_MINES_H
#define _WMINES_MINES_H

#define MINES_MAX_W 32                  /* the largest field the rules take */
#define MINES_MAX_H 20
#define MINES_MAX (MINES_MAX_W * MINES_MAX_H)

/* the three fields the window offers (mines_sizes[]) */
#define MINES_SMALL 0
#define MINES_MEDIUM 1
#define MINES_LARGE 2
#define MINES_SIZES 3
struct mines_size { const char *name; int w, h, mines; };
extern const struct mines_size mines_sizes[MINES_SIZES];

/* a cell's bits */
#define MINE_HERE 0x01                  /* a mine is laid here */
#define MINE_OPEN 0x02                  /* revealed */
#define MINE_FLAG 0x04                  /* flagged by the player */
#define MINE_BLAST 0x08                 /* a mine revealed: the game was lost here */

/* the game's states */
#define MINES_READY 0                   /* nothing revealed yet: the mines are laid at the first reveal */
#define MINES_PLAYING 1
#define MINES_WON 2
#define MINES_LOST 3

struct mines {
    int w, h, mines;                    /* the field, and how many mines it holds */
    int state;
    unsigned char cell[MINES_MAX];      /* row by row: MINE_* */
    unsigned char near[MINES_MAX];      /* mines among the eight neighbours, once they are laid */
    int open;                           /* cells revealed */
    int flags;                          /* cells flagged */
    unsigned rng;                       /* the generator's state (xorshift32, never 0) */
    unsigned start, end;                /* the timer: the milliseconds at the first reveal, and at the end */
};

/* what a reveal, a chord or a flag did */
#define MINES_NONE 0                    /* nothing: no cell there, or one where it does nothing */
#define MINES_MOVED 1                   /* cells changed */
#define MINES_ENDED 2                   /* cells changed and the game is over: won or lost (state says) */

/* the generator (Marsaglia's xorshift32): a state from any seed, its next number (never 0), and a number below n
   (n from 1) without the modulo's bias */
unsigned mines_seed(unsigned seed);
unsigned mines_next(unsigned *rng);
unsigned mines_below(unsigned *rng, unsigned n);

/* a new field of w x h with mines mines, nothing laid yet: 0, or -1 when the numbers are none the rules take
   (2 to MINES_MAX_W by 2 to MINES_MAX_H, 1 mine to all the cells but one) */
int mines_new(struct mines *m, int w, int h, int mines, unsigned seed);
int mines_reveal(struct mines *m, int x, int y, unsigned now);
int mines_chord(struct mines *m, int x, int y, unsigned now);
int mines_flag(struct mines *m, int x, int y);
int mines_flags_around(const struct mines *m, int x, int y);
int mines_left(const struct mines *m);                  /* mines less flags: below 0 when too many are placed */
unsigned mines_ms(const struct mines *m, unsigned now); /* the timer's milliseconds */
int mines_over(const struct mines *m);                  /* won or lost */

#endif
