/* Host tests for Swell (user/wswell/: swell.c, its rules, and view.c, its window without the drawing; 11/05/26, the
   desktop applications, track games), built as they are with the host's C library. The pieces: seven of two, three and
   five cells (no four), each turn worked out right, four turns coming back, no two alike. The set they come from: each
   kind twice, a seed repeating it. A game: the piece coming in at the top, falling on time, moving and turning (the
   kicks off a wall and a row up, a turn that cannot be), landing and resting before it sets, a rest that sliding over
   a gap cannot stretch, Down and Space, rows washed away with their points, an empty sea's bonus, the levels, the
   swell lifting the sea with sand and two gaps, the end at the top two ways, the pause stopping the clock, the time to
   wait. Thousands of random games keeping the sea's rules. The window's model: its sizes, the cells it shows (the
   piece and its ghost), the keys in every state, the focus lost pausing a game, the washed rows' moment, closing
   during a game, the status bar, the session's best. The drawing is checked on the screen (tools/gui_apps_games.py).
   Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include "swell.h"
#include "view.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/* a shape as a bitmap of 5 x 5, for comparing turns */
static unsigned bits(const struct swell_piece *p, int dx, int dy) {
    unsigned b = 0;
    int i;
    for(i = 0; i < p->n; i++) b |= 1u << ((p->y[i] + dy) * 5 + p->x[i] + dx);
    return b;
}

static void pieces(void) {
    static const int cells[SW_KINDS] = { 2, 3, 3, 5, 5, 5, 5 };
    unsigned all[SW_KINDS][4];
    int k, t, i, j, ok = 1, distinct = 1, minx, miny;
    for(k = 0; k < SW_KINDS; k++) for(t = 0; t < 4; t++) {
        const struct swell_piece *p = swell_shape(k, t), *q = swell_shape(k, t + 1);
        if(p->n != cells[k] || p->n == 4) ok = 0;
        minx = miny = 9;
        for(i = 0; i < p->n; i++) {
            if(p->x[i] < minx) minx = p->x[i];
            if(p->y[i] < miny) miny = p->y[i];
            if(p->x[i] >= p->w || p->y[i] >= p->h) ok = 0;
            for(j = 0; j < i; j++) if(p->x[i] == p->x[j] && p->y[i] == p->y[j]) ok = 0;
        }
        if(minx || miny || p->w != q->h || p->h != q->w) ok = 0;    /* at the box's corner; a turn swaps its sides */
        all[k][t] = bits(p, 0, 0);
    }
    CHECK(ok);
    for(k = 0; k < SW_KINDS; k++) {                     /* four turns come back; no kind is another in any turn */
        CHECK(swell_shape(k, 4) == swell_shape(k, 0) && all[k][0] == bits(swell_shape(k, 8), 0, 0));
        for(j = 0; j < k; j++) for(t = 0; t < 4; t++) for(i = 0; i < 4; i++) if(all[k][t] == all[j][i]) distinct = 0;
    }
    CHECK(distinct);
    /* the shell turned clockwise: its corner goes from the bottom left to the top left */
    {
        const struct swell_piece *p = swell_shape(SW_SHELL, 1);
        CHECK(p->w == 2 && p->h == 2 && bits(p, 0, 0) == (1u << 0 | 1u << 1 | 1u << 5));
        p = swell_shape(SW_DRIFT, 1);
        CHECK(p->w == 1 && p->h == 3);
        p = swell_shape(SW_KELP, 2);                    /* upside down: the stem up, the bar at the bottom */
        CHECK(p->w == 3 && p->h == 3 && bits(p, 0, 0) == (1u << 1 | 1u << 6 | 1u << 10 | 1u << 11 | 1u << 12));
    }
    CHECK(swell_shape(-1, 0) == swell_shape(0, 0) && !strcmp(swell_names[SW_GULL], "gull"));
    CHECK(swell_interval(1) == 800 && swell_interval(0) == 800 && swell_interval(16) == 100 && swell_interval(99) == 100);
    for(k = 1, ok = 1; k < 16; k++) if(swell_interval(k + 1) >= swell_interval(k)) ok = 0;
    CHECK(ok);
}

static void the_set(void) {
    struct swell a, b;
    int counts[SW_KINDS] = { 0 }, i, ok = 1;
    swell_new(&a, 5);
    swell_new(&b, 5);
    CHECK(a.state == SW_READY && a.next == b.next && a.left == SW_SET - 1 && a.level == 1);
    counts[a.next]++;
    for(i = 1; i < SW_SET; i++) {
        swell_start(&a, 0);                             /* (the draws, through spawns) */
        a.state = SW_READY;
        counts[a.next]++;
    }
    for(i = 0; i < SW_KINDS; i++) if(counts[i] != 2) ok = 0;
    CHECK(ok);
    swell_new(&a, 5);
    swell_new(&b, 5);
    CHECK(!memcmp(a.set, b.set, sizeof(a.set)));        /* a seed is its sets */
    swell_new(&b, 6);
    CHECK(memcmp(a.set, b.set, sizeof(a.set)) != 0);
}

/* a game started at time 0 whose falling piece is the one asked for, at the top */
static void playing(struct swell *s, int kind) {
    swell_new(s, 1);
    s->next = kind;
    swell_start(s, 0);
}

/* the falling piece's cells, written in the sea as if it had set (for the tests' own fills) */
static void fill_row(struct swell *s, int y, int gap_from, int gap_to) {
    int x;
    for(x = 0; x < SW_W; x++) s->sea[y][x] = x >= gap_from && x <= gap_to ? 0 : SW_SAND;
}

static int sea_empty(const struct swell *s) {
    int x, y;
    for(y = 0; y < SW_H; y++) for(x = 0; x < SW_W; x++) if(s->sea[y][x]) return 0;
    return 1;
}

static void falling(void) {
    struct swell s;
    int i, y;
    playing(&s, SW_CREST);                              /* 2 wide, 3 tall */
    CHECK(s.state == SW_PLAYING && s.kind == SW_CREST && s.x == 5 && s.y == 0 && s.turn == 0);
    CHECK(swell_tick(&s, 799) == 0 && s.y == 0);
    CHECK(swell_tick(&s, 800) == 1 && s.y == 1);
    CHECK(swell_wait(&s, 800) == 800 && swell_wait(&s, 1500) == 100 && swell_wait(&s, 1700) == 0);
    CHECK(swell_tick(&s, 800 + 3 * 800) == 1 && s.y == 4);    /* late: every row due */
    /* to the walls */
    for(i = 0; i < 20; i++) swell_move(&s, -1, 4000);
    CHECK(s.x == 0 && swell_move(&s, -1, 4000) == 0);
    for(i = 0; i < 20; i++) swell_move(&s, 1, 4000);
    CHECK(s.x == SW_W - 2 && swell_move(&s, 1, 4000) == 0);
    /* down: a row and a point each; landed, it rests SW_LOCK_MS, then sets; the next comes in */
    y = s.y;
    CHECK(swell_down(&s, 4000) == 1 && s.y == y + 1 && s.score == 1 && s.fall_at == 4000 + 800);
    while(s.y < SW_H - 3) swell_tick(&s, s.fall_at);
    CHECK(s.y == SW_H - 3 && s.lock_at == s.rest_at && s.lock_at != 0 && s.pieces == 0);
    {
        unsigned landed = s.lock_at - SW_LOCK_MS;
        CHECK(swell_tick(&s, s.lock_at - 1) == 0 && s.pieces == 0);
        CHECK(swell_tick(&s, landed + SW_LOCK_MS) == 1 && s.pieces == 1 && s.sea[SW_H - 1][SW_W - 2] == SW_CREST + 1);
    }
    CHECK(s.y == 0 && s.lock_at == 0 && s.state == SW_PLAYING);
    /* Down on a landed piece sets it at once; Space drops it, 2 points a row */
    playing(&s, SW_DRIFT);
    s.score = 0;
    CHECK(swell_drop(&s, 100) == SW_H - 1 && s.score == 2u * (SW_H - 1) && s.pieces == 1);
    CHECK(s.sea[SW_H - 1][4] == SW_DRIFT + 1 && s.sea[SW_H - 1][5] == SW_DRIFT + 1 && s.sea[SW_H - 1][6] == SW_DRIFT + 1);
    s.next = SW_PEBBLE;
    s.kind = SW_PEBBLE;
    s.turn = 0;
    s.x = 4;
    s.y = SW_H - 2;
    CHECK(swell_down(&s, 200) == 1 && s.pieces == 2 && s.sea[SW_H - 2][4] == SW_PEBBLE + 1);
    CHECK(swell_move(&s, 1, 1) == 1 && swell_move(&s, 0, 1) == 1 && swell_down(&s, 1) == 1);  /* (a move of 0 fits) */
}

static void turning(void) {
    struct swell s;
    int i, x, y;
    playing(&s, SW_KELP);                               /* 3 x 3 */
    s.y = 5;
    x = s.x;
    y = s.y;
    for(i = 0; i < 4; i++) CHECK(swell_turn(&s, 1, 0) == 1);
    CHECK(s.turn == 0 && s.x == x && s.y == y);         /* four turns: back where it was */
    for(i = 0; i < 4; i++) swell_turn(&s, -1, 0);
    CHECK(s.turn == 0 && s.x == x && s.y == y);
    /* the drift upright against the right wall: turned flat it would stick out, so it kicks left */
    playing(&s, SW_DRIFT);
    s.y = 6;
    swell_turn(&s, 1, 0);
    CHECK(s.turn == 1 && swell_shape(SW_DRIFT, 1)->w == 1);
    while(swell_move(&s, 1, 0)) ;
    CHECK(s.x == SW_W - 1);
    CHECK(swell_turn(&s, 1, 0) == 1 && s.turn == 2 && s.x + 3 <= SW_W && s.x == SW_W - 3);
    /* in a tunnel a row high, a turn has nowhere to go: it stays flat */
    playing(&s, SW_DRIFT);
    s.y = 8;
    fill_row(&s, 7, 0, -1);
    fill_row(&s, 9, 0, -1);
    CHECK(swell_turn(&s, 1, 0) == 0 && swell_turn(&s, -1, 0) == 0 && s.turn == 0 && s.x == 4 && s.y == 8);
    /* at the top, it turns into the room above the sea */
    playing(&s, SW_DRIFT);
    CHECK(swell_turn(&s, 1, 0) == 1 && s.turn == 1 && s.y == -1 && swell_fits(&s, s.kind, 1, s.x, -1));
    /* a turn with a cell above it blocked kicks a row up when there is room */
    playing(&s, SW_DRIFT);
    s.y = 8;
    swell_turn(&s, 1, 0);                               /* upright, rows 7 to 9, column 5 */
    CHECK(s.turn == 1 && s.x == 5 && s.y == 7);
    for(i = 0; i < SW_W; i++) s.sea[8][i] = i == 5 ? 0 : SW_SAND;    /* turning flat at row 8 hits sand */
    for(i = 0; i < SW_W; i++) s.sea[9][i] = i == 5 ? 0 : SW_SAND;
    s.sea[6][4] = s.sea[6][6] = 0;
    CHECK(swell_turn(&s, 1, 0) == 1 && s.turn == 2 && s.y == 7);    /* flat on row 7: a row up from 8 */
}

static void resting(void) {
    struct swell s;
    unsigned deadline;
    int x;
    /* a ledge: the pebble lands on it, slides over the gap beside it (it could fall: no rest) and back: the rest it
       had on that row stays, so to and fro cannot keep it up */
    playing(&s, SW_PEBBLE);
    for(x = 0; x < SW_W; x++) s.sea[10][x] = x == 7 || x == 8 ? 0 : SW_SAND;
    s.x = 4;
    s.y = 9;
    swell_tick(&s, 1000);                               /* a tick: it cannot fall, it has landed */
    CHECK(s.lock_at == 1000 + SW_LOCK_MS);
    deadline = s.lock_at;
    swell_move(&s, 1, 1100);
    swell_move(&s, 1, 1150);                            /* over the gap: 7 and 8 open under it */
    CHECK(s.x == 6 && s.lock_at == deadline);           /* still on sand at 6 */
    swell_move(&s, 1, 1200);
    CHECK(s.x == 7 && s.lock_at == 0);                  /* over the gap: it can fall */
    swell_move(&s, -1, 1300);
    CHECK(s.lock_at == deadline);                       /* back on the row: the same time, not a new one */
    CHECK(swell_tick(&s, deadline) == 1 && s.pieces == 1 && s.sea[9][6] == SW_PEBBLE + 1);
    /* a lower row is a new rest */
    playing(&s, SW_PEBBLE);
    for(x = 0; x < SW_W; x++) s.sea[10][x] = x == 7 || x == 8 ? 0 : SW_SAND;
    s.x = 6;
    s.y = 9;
    swell_tick(&s, 1000);
    swell_move(&s, 1, 1100);                            /* over the gap */
    swell_tick(&s, 1000 + 800);                         /* it falls into it, to the bottom row by row */
    while(s.y < SW_H - 1 && s.pieces == 0) swell_tick(&s, s.fall_at);
    CHECK(s.y == SW_H - 1 && s.lock_at == s.rest_at && s.rest_y == SW_H - 1 && s.lock_at > 1000 + SW_LOCK_MS);
}

static void washing(void) {
    struct swell s;
    int x, y, i;
    /* one row: the drift completes it */
    playing(&s, SW_DRIFT);
    fill_row(&s, SW_H - 1, 4, 6);
    s.sea[SW_H - 2][0] = SW_SAND;                       /* something above, which comes down */
    swell_drop(&s, 10);
    CHECK(s.last_rows == 1 && s.rows == 1 && s.last_points == 10 && s.washed[SW_H - 1] && s.washed_n == 1);
    CHECK(s.sea[SW_H - 1][0] == SW_SAND && !s.sea[SW_H - 2][0]);
    /* three rows at once by the kelp upright: and the sea left empty: the bonus */
    playing(&s, SW_KELP);
    fill_row(&s, SW_H - 1, 5, 5);                       /* the stem's column */
    fill_row(&s, SW_H - 2, 5, 5);
    fill_row(&s, SW_H - 3, 4, 6);                       /* the bar */
    s.score = 0;
    swell_turn(&s, 1, 0);
    swell_turn(&s, 1, 0);                               /* stem up: bar at the bottom */
    swell_turn(&s, 1, 0);
    swell_turn(&s, 1, 0);                               /* (four turns: as it came) */
    CHECK(s.turn == 0 && s.x == 4);
    swell_drop(&s, 20);
    CHECK(s.last_rows == 3 && s.washed[SW_H - 1] && s.washed[SW_H - 2] && s.washed[SW_H - 3] && !s.washed[SW_H - 4]);
    CHECK(s.last_points == 50 + 200 && sea_empty(&s));
    CHECK(s.score == 2u * (SW_H - 3) + 250);
    /* two rows that are not neighbours: the crest fills the bottom row and the third, the row between keeps its hole
       and comes down to the bottom */
    playing(&s, SW_CREST);                              /* columns 5 and 6 for two rows, then 5 alone */
    fill_row(&s, SW_H - 1, 5, 5);
    fill_row(&s, SW_H - 2, 5, 6);
    s.sea[SW_H - 2][3] = 0;                             /* a hole the crest cannot fill */
    fill_row(&s, SW_H - 3, 5, 6);
    swell_drop(&s, 30);
    CHECK(s.pieces == 1 && s.last_rows == 2 && s.last_points == 25 && s.washed[SW_H - 1] && s.washed[SW_H - 3]);
    for(y = 0, x = 0; y < SW_H; y++) x += s.washed[y];
    CHECK(x == 2 && !s.washed[SW_H - 2]);
    CHECK(!s.sea[SW_H - 1][3] && s.sea[SW_H - 1][5] == SW_CREST + 1 && s.sea[SW_H - 1][0] == SW_SAND);
    for(y = 0, x = 0; y < SW_H - 1; y++) for(i = 0; i < SW_W; i++) x += s.sea[y][i] != 0;
    CHECK(x == 0);                                      /* nothing above it */
    /* levels: every 8 rows, faster */
    playing(&s, SW_DRIFT);
    s.rows = 7;
    fill_row(&s, SW_H - 1, 4, 6);
    s.sea[SW_H - 2][0] = SW_SAND;                       /* (not left empty: no bonus) */
    swell_drop(&s, 40);
    CHECK(s.rows == 8 && s.level == 2 && s.last_points == 10 && s.fall_at == 40 + swell_interval(2));
    fill_row(&s, SW_H - 1, 4, 6);
    s.sea[SW_H - 2][0] = SW_SAND;                       /* (not left empty: no bonus) */
    s.kind = SW_DRIFT;
    s.turn = 0;
    s.x = 4;
    s.y = 0;
    swell_drop(&s, 50);
    CHECK(s.last_points == 20);                         /* the level's times */
}

static void the_swell(void) {
    struct swell s;
    int x, gaps = 0, sand = 0;
    playing(&s, SW_PEBBLE);
    s.level = SW_SWELL_LEVEL;
    s.since_swell = SW_SWELL_PIECES - 2;
    s.sea[SW_H - 1][0] = SW_CREST + 1;
    swell_drop(&s, 1);
    CHECK(s.swells == 0 && s.since_swell == SW_SWELL_PIECES - 1);
    s.kind = SW_PEBBLE;
    s.x = 0;
    s.y = 0;
    swell_drop(&s, 2);                                  /* the 16th: the sea comes up a row */
    CHECK(s.swells == 1 && s.since_swell == 0 && s.state == SW_PLAYING);
    for(x = 0; x < SW_W; x++) {
        gaps += !s.sea[SW_H - 1][x];
        sand += s.sea[SW_H - 1][x] == SW_SAND;
    }
    CHECK(gaps == 2 && sand == SW_W - 2);
    CHECK(s.sea[SW_H - 2][0] == SW_CREST + 1 && s.sea[SW_H - 3][0] == SW_PEBBLE + 1);    /* the rest a row higher */
    /* sand pushed over the top ends the game */
    s.since_swell = SW_SWELL_PIECES - 1;
    s.sea[0][SW_W - 1] = SW_SAND;
    s.kind = SW_PEBBLE;
    s.x = 0;
    s.y = 0;
    swell_drop(&s, 10000);
    CHECK(s.state == SW_OVER && s.swells == 1);
    /* below its level, no swell */
    playing(&s, SW_PEBBLE);
    s.since_swell = SW_SWELL_PIECES + 5;
    swell_drop(&s, 1);
    CHECK(s.swells == 0);
}

static void ending_and_pause(void) {
    struct swell s;
    int x;
    playing(&s, SW_DRIFT);
    for(x = 0; x < SW_W; x++) if(x != 2) s.sea[1][x] = SW_SAND;     /* row 1 full but one: the next piece has no room */
    s.next = SW_CREST;
    CHECK(swell_drop(&s, 5000) == 0 && s.state == SW_OVER);         /* it sets where it is; the crest cannot come in */
    CHECK(swell_ms(&s, 99999) == 5000 && swell_wait(&s, 1) == 0xFFFFFFFFu);
    CHECK(swell_move(&s, 1, 1) == 0 && swell_turn(&s, 1, 1) == 0 && swell_down(&s, 1) == 0 && swell_drop(&s, 1) == 0);
    CHECK(swell_tick(&s, 99999) == 0 && swell_start(&s, 1) == 0);
    /* the pause stops the clock: the time played, the fall and the rest go on where they were */
    playing(&s, SW_GULL);
    swell_tick(&s, 800);
    CHECK(s.y == 1 && swell_pause(&s, 1, 1000) == 1 && s.state == SW_PAUSED && swell_pause(&s, 1, 1000) == 0);
    CHECK(swell_tick(&s, 50000) == 0 && s.y == 1 && swell_ms(&s, 50000) == 1000);
    CHECK(swell_move(&s, 1, 50000) == 0);
    CHECK(swell_pause(&s, 0, 61000) == 1 && s.fall_at == 1600 + 60000 && swell_ms(&s, 61500) == 1500);
    CHECK(swell_tick(&s, 61599) == 0 && swell_tick(&s, 61600) == 1 && s.y == 2);
    CHECK(swell_pause(&s, 0, 1) == 0);
}

/* where a careful player would put the falling piece: every turn and column tried where it would come to rest, rows
   washed counting for it, holes, height and a ragged top against it */
static void careful(const struct swell *s, int *turn, int *col) {
    int t, x, y, i, best = -1000000000, score, cx, filled, holes, high, h[SW_W], ragged, most;
    unsigned char sea[SW_H][SW_W];
    *turn = s->turn;
    *col = s->x;
    for(t = 0; t < 4; t++) for(x = -2; x < SW_W; x++) {
        const struct swell_piece *p = swell_shape(s->kind, t);
        if(!swell_fits(s, s->kind, t, x, 0)) continue;
        for(y = 0; swell_fits(s, s->kind, t, x, y + 1); y++) ;
        memcpy(sea, s->sea, sizeof(sea));
        for(i = 0; i < p->n; i++) if(y + p->y[i] >= 0) sea[y + p->y[i]][x + p->x[i]] = 1;
        score = 0;
        for(i = 0; i < SW_H; i++) {
            for(cx = 0, filled = 0; cx < SW_W; cx++) filled += sea[i][cx] != 0;
            if(filled == SW_W) score += 100;
        }
        for(cx = 0, holes = 0, high = 0, most = 0; cx < SW_W; cx++) {
            int top = -1;
            for(i = 0; i < SW_H; i++) {
                if(sea[i][cx] && top < 0) top = i;
                if(!sea[i][cx] && top >= 0) holes++;
            }
            h[cx] = top >= 0 ? SW_H - top : 0;
            high += h[cx];
            if(h[cx] > most) most = h[cx];
        }
        for(cx = 0, ragged = 0; cx + 1 < SW_W; cx++) ragged += h[cx] > h[cx + 1] ? h[cx] - h[cx + 1] : h[cx + 1] - h[cx];
        score -= holes * 60 + high * 2 + ragged * 4 + most * 3;
        if(score > best) {
            best = score;
            *turn = t;
            *col = x;
        }
    }
}

/* random games: random keys and the clock until each ends; after every step the sea keeps its rules. Every other game
   is played carefully (careful() above): such a game lasts some 300 pieces and 100 rows, so rows are washed away, the
   levels climb and the swell comes */
static void random_games(void) {
    struct swell s;
    unsigned r = swell_seed(77), now;
    int g, steps, ok = 1, ended = 0, x, y, full, washed = 0, swelled = 0;
    unsigned last;
    for(g = 0; g < 200; g++) {
        swell_new(&s, (unsigned)g);
        swell_start(&s, 0);
        now = 0;
        last = 0;
        for(steps = 0; steps < 20000 && s.state != SW_OVER; steps++) {
            if(g % 2 && s.state == SW_PLAYING) {        /* careful: turn, move, drop; then the clock a little */
                int turn, col, n;
                careful(&s, &turn, &col);
                for(n = 0; n < 4 && s.turn != turn; n++) swell_turn(&s, 1, now);
                for(n = 0; n < SW_W && s.x > col; n++) swell_move(&s, -1, now);
                for(n = 0; n < SW_W && s.x < col; n++) swell_move(&s, 1, now);
                swell_drop(&s, now);
                now += swell_below(&r, 300);
                swell_tick(&s, now);
            } else switch(swell_below(&r, 8)) {
            case 0: swell_move(&s, -1, now); break;
            case 1: swell_move(&s, 1, now); break;
            case 2: swell_turn(&s, 1, now); break;
            case 3: swell_turn(&s, -1, now); break;
            case 4: swell_down(&s, now); break;
            case 5: if(swell_below(&r, 4) == 0) swell_drop(&s, now); break;
            default: now += swell_below(&r, 400); swell_tick(&s, now); break;
            }
            if(s.state == SW_PLAYING && !swell_fits(&s, s.kind, s.turn, s.x, s.y)) ok = 0;
            for(y = 0; y < SW_H; y++) {
                for(x = 0, full = 1; x < SW_W; x++) {
                    if(s.sea[y][x] > SW_SAND) ok = 0;
                    if(!s.sea[y][x]) full = 0;
                }
                if(full) ok = 0;                        /* a full row is always washed away */
            }
            if(s.score < last) ok = 0;
            last = s.score;
            if(s.level != 1 + s.rows / SW_ROWS_LEVEL) ok = 0;
        }
        ended += s.state == SW_OVER;
        washed += s.rows > 0;
        swelled += s.swells > 0;
    }
    if(ended < 200 || washed < 100 || swelled < 90) printf("  random games: %d ended, %d washed rows, %d swelled\n", ended,
                                                          washed, swelled);
    CHECK(ok && ended == 200 && washed >= 100 && swelled >= 90);
}

/**** the window's model ****/

static struct sview v;

static void status_is(const char *want) {
    char t[160];
    sview_status(&v, t, sizeof(t));
    if(strcmp(t, want)) printf("  status: \"%s\"\n", t);
    CHECK(!strcmp(t, want));
}

static void window(void) {
    static const int rooms[][2] = { { 958, 457 }, { 638, 317 }, { 0, 0 } };
    int r, w, h;
    for(r = 0; r < 3; r++) {
        sview_window(rooms[r][0], rooms[r][1], &w, &h);
        CHECK(!rooms[r][0] || (w <= rooms[r][0] && h <= rooms[r][1]));
        sview_init(&v, 1);
        sview_layout(&v, w, h - SV_STATUS_H);           /* the board: the window but its status bar */
        CHECK(v.cell >= SV_CELL_MIN && v.cell <= SV_CELL && v.sx >= SV_MARGIN && v.sy >= SV_MARGIN);
        CHECK(v.sx + SW_W * v.cell <= v.px && v.px + SV_PANEL <= w);
        CHECK(v.sy + SW_H * v.cell <= h - SV_STATUS_H - SV_MARGIN);
    }
    sview_window(958, 457, &w, &h);
    CHECK(w == SW_W * 22 + SV_PANEL + 30 && h == SW_H * 22 + 20 + SV_STATUS_H);
    sview_window(638, 317, &w, &h);
    CHECK(sview_layout(&v, w, h - SV_STATUS_H) == 1 && v.cell == 15 && sview_layout(&v, w, h - SV_STATUS_H) == 0);
    sview_layout(&v, 100, 100);
    CHECK(v.cell == SV_CELL_MIN && v.sx == SV_MARGIN);
}

static void keys(void) {
    int i, n, ghost;
    char t[16];
    sview_init(&v, 2026);
    status_is("Enter starts; arrows move, Up turns, Space drops");
    CHECK(sview_key(&v, SV_LEFT, 0, 0) == 0 && v.game.state == SW_READY);
    CHECK(sview_cell(&v, 5, 0) == SV_WATER);            /* no piece yet */
    CHECK(sview_key(&v, '\r', 0, 1000) == SV_DRAW && v.game.state == SW_PLAYING);
    status_is("Up or X turns (Z back), Space drops, P pauses");
    /* the falling piece and its ghost */
    {
        const struct swell_piece *p = swell_shape(v.game.kind, v.game.turn);
        ghost = swell_ghost(&v.game);
        CHECK(sview_cell(&v, v.game.x + p->x[0], v.game.y + p->y[0]) == v.game.kind + 1);
        CHECK(sview_cell(&v, v.game.x + p->x[0], ghost + p->y[0]) == SV_GHOST && ghost == SW_H - p->h);
        CHECK(sview_cell(&v, -1, 0) == SV_WATER && sview_cell(&v, 0, SW_H) == SV_WATER);
    }
    n = v.game.x;
    CHECK(sview_key(&v, SV_LEFT, 0, 1000) == SV_DRAW && v.game.x == n - 1);
    CHECK(sview_key(&v, SV_RIGHT, 0, 1000) == SV_DRAW && v.game.x == n);
    n = v.game.turn;
    sview_key(&v, SV_UP, 0, 1000);
    sview_key(&v, 'x', 0, 1000);
    sview_key(&v, 'Z', 0, 1000);
    CHECK(v.game.turn == ((n + 1) & 3));                /* twice clockwise, once back */
    n = v.game.y;
    CHECK(sview_key(&v, SV_DOWN, 0, 1000) == SV_DRAW && v.game.y == n + 1);
    CHECK(sview_key(&v, 'q', 0, 1000) == 0);
    CHECK(sview_key(&v, 'n', 0, 1000) == SV_DRAW && v.game.state == SW_PLAYING);  /* not in the middle of a game */
    status_is("P pauses the game; then N starts a new one");
    n = v.game.pieces;
    CHECK(sview_key(&v, ' ', 0, 1000) == SV_DRAW && v.game.pieces == n + 1);
    CHECK(sview_key(&v, ' ', SV_REPEAT, 1000) == 0 && v.game.pieces == n + 1);    /* Space held: one drop */
    n = v.game.turn;
    CHECK(sview_key(&v, SV_UP, SV_REPEAT, 1000) == 0 && v.game.turn == n);        /* Up held: one turn */
    n = v.game.x;
    CHECK(sview_key(&v, SV_LEFT, SV_REPEAT, 1000) == SV_DRAW && v.game.x == n - 1);   /* Left held moves on */
    n = v.game.y;
    CHECK(sview_key(&v, SV_DOWN, SV_REPEAT | 1, 1000) == SV_DRAW && v.game.y == n + 1);
    /* P pauses and goes on; losing the focus pauses */
    CHECK(sview_key(&v, 'p', 0, 2000) == SV_DRAW && v.game.state == SW_PAUSED);
    status_is("Paused: P or Enter goes on, N starts again");
    CHECK(sview_key(&v, ' ', SV_REPEAT, 2500) == 0 && v.game.state == SW_PAUSED);    /* a held key: nothing */
    CHECK(sview_key(&v, SV_LEFT, 0, 2000) == 0 && sview_key(&v, ' ', 0, 3000) == SV_DRAW && v.game.state == SW_PLAYING);
    CHECK(sview_focus(&v, 1, 3000) == 0 && sview_focus(&v, 0, 3500) == SV_DRAW && v.game.state == SW_PAUSED);
    status_is("Paused while in the background: P goes on");
    CHECK(sview_focus(&v, 0, 3600) == 0);
    CHECK(sview_key(&v, 'P', 0, 4000) == SV_DRAW && v.game.state == SW_PLAYING);
    CHECK(swell_ms(&v.game, 4000) == 1500);             /* 1000 to 2000, 3000 to 3500 */
    /* closing during a game pauses and asks; Cancel leaves it paused; the question has the keys */
    CHECK(sview_key(&v, 27, 0, 5000) == SV_DRAW && v.asking && v.game.state == SW_PAUSED);
    CHECK(sview_key(&v, 'p', 0, 5000) == 0 && sview_answer(&v, 0) == SV_DRAW && !v.asking && v.game.state == SW_PAUSED);
    CHECK(sview_close(&v, 5000) == SV_DRAW && v.asking && sview_answer(&v, 1) == SV_CLOSE);
    v.asking = 0;
    CHECK(sview_key(&v, 'N', 0, 6000) == SV_DRAW && v.game.state == SW_PLAYING && v.game.pieces == 0);   /* paused: N */
    sview_key(&v, ' ', 0, 6000);
    sview_restart(&v, 6000);                            /* the menu's New game: at once, in the middle of one */
    CHECK(v.game.state == SW_PLAYING && v.game.pieces == 0 && v.games == 0);
    /* the wait: the next row, the second shown, the washed rows' moment */
    CHECK(sview_wait(&v, 6000) <= 800 && sview_wait(&v, 6000) > 0);
    sview_time(0, t, sizeof(t));
    CHECK(!strcmp(t, "0:00"));
    sview_time(3723000, t, sizeof(t));
    CHECK(!strcmp(t, "1:02:03"));
    /* rows washed away: a moment's flash, the status bar says what they scored */
    v.game.kind = SW_DRIFT;
    v.game.turn = 0;
    v.game.x = 0;
    v.game.y = 0;
    for(i = 0; i < SW_W; i++) v.game.sea[SW_H - 1][i] = i < 3 ? 0 : SW_SAND;
    v.game.sea[SW_H - 2][SW_W - 1] = SW_SAND;           /* (not left empty: no bonus) */
    sview_key(&v, ' ', 0, 7000);
    CHECK(v.flash_at == 7000 && sview_flashing(&v, SW_H - 1, 7000 + SV_FLASH_MS - 1) && !sview_flashing(&v, SW_H - 2, 7000));
    status_is("1 row washed away: 10 points");
    CHECK(sview_wait(&v, 7100) <= SV_FLASH_MS - 100);
    CHECK(sview_tick(&v, 7000 + SV_FLASH_MS) == SV_DRAW && !v.flash_at && !sview_flashing(&v, SW_H - 1, 7300));
}

static void ends(void) {
    int x;
    sview_init(&v, 8);
    sview_key(&v, ' ', 0, 0);
    CHECK(sview_close(&v, 10) == SV_DRAW && sview_answer(&v, 0) == SV_DRAW);
    sview_key(&v, 'p', 0, 10);
    /* the end: the top row blocked, a drop: the next cannot come in */
    for(x = 0; x < SW_W; x++) if(x != 0) v.game.sea[3][x] = SW_SAND;
    v.game.kind = SW_PEBBLE;
    v.game.turn = 0;
    v.game.x = 6;
    v.game.y = 0;
    v.game.next = SW_GULL;
    v.game.score = 120;
    v.game.rows = 3;
    sview_key(&v, ' ', 0, 2000);
    CHECK(v.game.state == SW_OVER && v.games == 1 && v.best == 124 && v.best_rows == 3);
    status_is("Over the top: 124 points, the best yet. Enter: again");
    CHECK(sview_close(&v, 2000) == SV_CLOSE && sview_key(&v, SV_LEFT, 0, 2000) == 0);
    CHECK(sview_key(&v, '\n', 0, 3000) == SV_DRAW && v.game.state == SW_PLAYING && v.game.score == 0);
    /* a lower score: the best stays; the end by the clock counts too */
    for(x = 0; x < SW_W; x++) if(x != 0) v.game.sea[3][x] = SW_SAND;
    v.game.kind = SW_PEBBLE;
    v.game.turn = 0;
    v.game.x = 6;
    v.game.y = 2;
    v.game.next = SW_GULL;
    v.game.lock_at = 0;
    v.game.fall_at = 3000 + 800;
    sview_tick(&v, 3800);                               /* it cannot fall: landed */
    CHECK(v.game.lock_at == 3800 + SW_LOCK_MS);
    CHECK(sview_tick(&v, 3800 + SW_LOCK_MS) == SV_DRAW && v.game.state == SW_OVER && v.games == 2 && v.best == 124);
    status_is("Over the top: 0 points (best 124). Enter: again");
    v.status[0] = 0;
    status_is("Over the top. Enter or N starts again");
    CHECK(sview_wait(&v, 0) == 0xFFFFFFFFu);
    CHECK(sview_key(&v, 'n', 0, 4000) == SV_DRAW && v.game.state == SW_PLAYING && v.games == 2);
}

int main(void) {
    pieces();
    the_set();
    falling();
    turning();
    resting();
    washing();
    the_swell();
    ending_and_pause();
    random_games();
    window();
    keys();
    ends();
    printf("test_wswell: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
