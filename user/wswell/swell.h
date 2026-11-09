/*
SurfOS user program: wswell
--------------------
File: swell.h   Date: 11/05/26
--------------------
Swell's rules (swell.c), a falling-blocks game of SurfOS's own. The sea is 12 cells wide and 18
deep. Its seven pieces are SurfOS's: two of two and three cells (the pebble, the drift and the
shell), and four of five (the crest, the reef, the gull and the kelp), drawn from a shuffled set
holding each twice. A piece falls a row at a time, faster each level; it moves and turns as keys
come, each key a step, so holding one plays by the keyboard's repeat (keys come as typed, with no
release). Landed, it rests for a moment before it sets, so it can still be slid. A full row is
washed away. From level 3 on, every 16 pieces the swell lifts the whole sea a row and leaves a row
of sand with two gaps at the bottom; a piece that cannot come in or sets above the sea, or sand
pushed over the top, ends the game. The time is the milliseconds main.c hands in, and the generator is seeded, so a
test repeats a game. No system calls and no drawing: tests/host/test_wswell.c runs it on the host
as it is; view.c is the window's side.
*/
#ifndef _WSWELL_SWELL_H
#define _WSWELL_SWELL_H

#define SW_W 12                         /* the sea's columns and rows */
#define SW_H 18
#define SW_KINDS 7                      /* the pieces */
#define SW_CELLS 5                      /* a piece's cells, at most */
#define SW_SAND (SW_KINDS + 1)          /* a cell the swell brought up (the pieces' cells are kind + 1) */
#define SW_SET (2 * SW_KINDS)           /* the shuffled set the pieces come from: each twice */
#define SW_LOCK_MS 450                  /* a landed piece rests this long before it sets */
#define SW_SWELL_LEVEL 3                /* the swell begins at this level */
#define SW_SWELL_PIECES 16              /* and lifts the sea every this many pieces */
#define SW_ROWS_LEVEL 8                 /* rows washed away for each level */
#define SW_NO_REST (-1000)              /* "has not rested": no row a piece reaches (it climbs at most a few rows above the sea) */

/* the pieces, by name (swell_names[]) */
#define SW_PEBBLE 0
#define SW_DRIFT 1
#define SW_SHELL 2
#define SW_CREST 3
#define SW_REEF 4
#define SW_GULL 5
#define SW_KELP 6
extern const char *const swell_names[SW_KINDS];

/* the states */
#define SW_READY 0                      /* before the first piece: a new game waits for a key */
#define SW_PLAYING 1
#define SW_PAUSED 2
#define SW_OVER 3

struct swell_piece { int n, w, h; signed char x[SW_CELLS], y[SW_CELLS]; };

struct swell {
    unsigned char sea[SW_H][SW_W];      /* 0 for water, kind + 1 for a piece's cell, SW_SAND */
    int state;
    int kind, turn, x, y;               /* the falling piece: its kind, its quarter turns, its box's top left */
    int next;                           /* the piece after it */
    unsigned char set[SW_SET];          /* the shuffled set, and how many of it are left */
    int left;
    unsigned rng;
    unsigned score;
    int rows, level, pieces;            /* rows washed away, the level, pieces set */
    int since_swell;                    /* pieces set since the swell last lifted the sea (from its level on) */
    int swells;                         /* times it did */
    unsigned fall_at;                   /* when the piece falls its next row */
    unsigned lock_at;                   /* landed: when it sets; 0 while it can fall */
    int rest_low;                       /* the lowest row its lowest cell rested at (SW_NO_REST before it landed), */
    unsigned rest_at;                   /* when it was to set there, */
    int deepest;                        /* and the lowest row that cell has been down to: Down and Space pay below it */
    unsigned paused_at, played;         /* the pause's start; the milliseconds played, pauses not counted */
    unsigned since;                     /* when the count of played last went on */
    unsigned char washed[SW_H];         /* the rows the last setting washed away (1 each), for the window */
    int washed_n;
    int last_rows;                      /* how many that was, and what it scored */
    unsigned last_points;
};

/* the generator (Marsaglia's xorshift32), as in Mines */
unsigned swell_seed(unsigned seed);
unsigned swell_next(unsigned *rng);
unsigned swell_below(unsigned *rng, unsigned n);

/* a piece's cells at a turn (0 to 3, clockwise), relative to its box's top left */
const struct swell_piece *swell_shape(int kind, int turn);
unsigned swell_interval(int level);     /* the milliseconds between rows at a level */

void swell_new(struct swell *s, unsigned seed);          /* an empty sea, ready */
int swell_start(struct swell *s, unsigned now);          /* the first piece comes in: 1 when it did */
int swell_fits(const struct swell *s, int kind, int turn, int x, int y);
int swell_move(struct swell *s, int dx, unsigned now);   /* a column left or right: 1 when it moved */
int swell_turn(struct swell *s, int dir, unsigned now);  /* a quarter turn, 1 clockwise or -1: 1 when it turned */
int swell_down(struct swell *s, unsigned now);           /* a row down by a key: a point for a new row; landed, it sets */
int swell_drop(struct swell *s, unsigned now);           /* straight down and set: 2 points a new row; the rows */
int swell_tick(struct swell *s, unsigned now);           /* what time does: 1 when anything changed */
unsigned swell_wait(const struct swell *s, unsigned now);    /* milliseconds until a tick has something to do */
int swell_pause(struct swell *s, int on, unsigned now);  /* paused or going on: 1 when that changed */
int swell_ghost(const struct swell *s);                  /* the row the piece would come to rest at */
unsigned swell_ms(const struct swell *s, unsigned now);  /* the time played */

#endif
