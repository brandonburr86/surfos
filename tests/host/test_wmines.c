/* Host tests for Mines (user/wmines/: mines.c, its rules, and view.c, its window without the drawing; 11/05/26, the
   desktop applications, track games), built as they are with the host's C library. The generator: a seed repeats,
   numbers stay below their bound and come evenly. The rules: fields the rules refuse; the first reveal never a mine,
   and its neighbours clear when the field has room, on thousands of seeded fields of every size and on crowded ones;
   the numbers; a reveal opening an area through the queue, also a spiral corridor that would recurse 300 deep and the
   largest field with one mine; flags (never opened, never revealed, counted, too many); chords (opening, refused,
   a wrong flag going off); the win (every mine flagged) and the loss; the timer, also past 2^32 milliseconds; and
   thousands of random games against a count of the field's own. The window's model: its sizes on the laptop's and
   the smallest desktop's room, the layout and the cell under the pointer, a press taken back, the pressed cells,
   flags and chords by the buttons, the keyboard's cursor, closing during a game, and the session's scores. The
   review of 11/05/26: a new game asked for during one (N, 1 to 3, the toolbar, the menu) asked first, the game kept
   on Cancel and counted as played on yes, nothing more taken while a question is up; a field won in no time keeping
   its best; the smallest window holding the large field. The drawing is checked on the screen
   (tools/gui_apps_games.py). Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include "mines.h"
#include "view.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/* a field laid by hand: the mines where the picture has '*', the rest clear, the game under way */
static void field(struct mines *m, const char *const *rows, int h) {
    int w = (int)strlen(rows[0]), x, y, dx, dy, n = 0, i;
    CHECK(mines_new(m, w, h, 1, 1) == 0);
    for(y = 0; y < h; y++) for(x = 0; x < w; x++) if(rows[y][x] == '*') { m->cell[y * w + x] |= MINE_HERE; n++; }
    m->mines = n;
    for(i = 0; i < w * h; i++) {
        x = i % w;
        y = i / w;
        for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++)
            if((dx || dy) && x + dx >= 0 && y + dy >= 0 && x + dx < w && y + dy < h && rows[y + dy][x + dx] == '*')
                m->near[i]++;
    }
    m->state = MINES_PLAYING;
    m->start = 0;
}

/* the field's own counts agree with what the game says it holds */
static int consistent(const struct mines *m) {
    int i, open = 0, flags = 0, mines = 0, x, y, dx, dy, near;
    for(i = 0; i < m->w * m->h; i++) {
        open += (m->cell[i] & MINE_OPEN) && !(m->cell[i] & MINE_HERE);
        flags += !!(m->cell[i] & MINE_FLAG);
        mines += !!(m->cell[i] & MINE_HERE);
        if((m->cell[i] & MINE_FLAG) && (m->cell[i] & MINE_OPEN)) return 0;
        if((m->cell[i] & MINE_BLAST) && !(m->cell[i] & MINE_HERE)) return 0;
        if(m->state == MINES_READY) continue;
        x = i % m->w;
        y = i / m->w;
        near = 0;
        for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++)
            if((dx || dy) && x + dx >= 0 && y + dy >= 0 && x + dx < m->w && y + dy < m->h &&
               (m->cell[(y + dy) * m->w + x + dx] & MINE_HERE)) near++;
        if(!(m->cell[i] & MINE_HERE) && near != m->near[i]) return 0;
    }
    return open == m->open && flags == m->flags && (m->state == MINES_READY ? mines == 0 : mines == m->mines);
}

/* an open cell with no mine around it has every neighbour open or flagged: the area opened all the way */
static int areas_whole(const struct mines *m) {
    int i, x, y, dx, dy;
    for(i = 0; i < m->w * m->h; i++) {
        if(!(m->cell[i] & MINE_OPEN) || m->near[i] || (m->cell[i] & MINE_HERE)) continue;
        x = i % m->w;
        y = i / m->w;
        for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++)
            if(x + dx >= 0 && y + dy >= 0 && x + dx < m->w && y + dy < m->h &&
               !(m->cell[(y + dy) * m->w + x + dx] & (MINE_OPEN | MINE_FLAG))) return 0;
    }
    return 1;
}

static void generator(void) {
    unsigned a = mines_seed(42), b = mines_seed(42), z = mines_seed(0x6D2B79F5u), counts[10] = { 0 };
    int i, same = 1, below = 1, even = 1;
    CHECK(mines_seed(0) != 0 && z != 0);                /* the seed that would make 0 does not */
    for(i = 0; i < 1000; i++) if(mines_next(&a) != mines_next(&b) || mines_next(&z) == 0) same = 0;
    CHECK(same);
    a = mines_seed(7);
    for(i = 0; i < 100000; i++) {
        unsigned v = mines_below(&a, 10);
        if(v >= 10) below = 0;
        else counts[v]++;
    }
    for(i = 0; i < 10; i++) if(counts[i] < 9500 || counts[i] > 10500) even = 0;   /* 10000 each, give or take */
    CHECK(below && even);
    CHECK(mines_below(&a, 1) == 0 && mines_below(&a, 0) == 0);
    for(i = 0, below = 1; i < 1000; i++) if(mines_below(&a, 0xFFFFFFFFu) >= 0xFFFFFFFFu) below = 0;
    CHECK(below);
}

static void refused(void) {
    struct mines m;
    CHECK(mines_new(&m, 1, 5, 1, 0) < 0 && mines_new(&m, 5, 1, 1, 0) < 0);
    CHECK(mines_new(&m, MINES_MAX_W + 1, 5, 1, 0) < 0 && mines_new(&m, 5, MINES_MAX_H + 1, 1, 0) < 0);
    CHECK(mines_new(&m, 5, 5, 0, 0) < 0 && mines_new(&m, 5, 5, 25, 0) < 0 && mines_new(&m, 5, 5, -3, 0) < 0);
    CHECK(mines_new(&m, 5, 5, 24, 0) == 0 && m.state == MINES_READY && mines_left(&m) == 24);
    CHECK(mines_new(&m, MINES_MAX_W, MINES_MAX_H, MINES_MAX - 1, 0) == 0);
    CHECK(mines_reveal(&m, -1, 0, 0) == MINES_NONE && mines_reveal(&m, 0, MINES_MAX_H, 0) == MINES_NONE);
    CHECK(mines_flag(&m, MINES_MAX_W, 0) == MINES_NONE && mines_chord(&m, 0, -1, 0) == MINES_NONE);
    CHECK(m.state == MINES_READY);
}

/* the first reveal: never a mine; its neighbours clear where the rest of the field holds every mine */
static void first_reveal(void) {
    struct mines m;
    unsigned pick = mines_seed(99);
    int s, seed, x, y, dx, dy, ok = 1, clear = 1, area = 1, r;
    for(s = 0; s < MINES_SIZES; s++) for(seed = 0; seed < 2000; seed++) {
        const struct mines_size *z = &mines_sizes[s];
        mines_new(&m, z->w, z->h, z->mines, (unsigned)seed);
        x = (int)mines_below(&pick, (unsigned)z->w);
        y = (int)mines_below(&pick, (unsigned)z->h);
        r = mines_reveal(&m, x, y, 1000);
        if(r == MINES_NONE || m.state == MINES_LOST || !consistent(&m)) ok = 0;
        for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++)
            if(x + dx >= 0 && y + dy >= 0 && x + dx < z->w && y + dy < z->h && (m.cell[(y + dy) * z->w + x + dx] & MINE_HERE))
                clear = 0;
        if(m.near[y * z->w + x] || !areas_whole(&m)) area = 0;   /* so the first reveal always opens an area */
    }
    CHECK(ok && clear && area);
    /* 3 x 3 with 8 mines: only the cell itself is kept clear, and opening it wins */
    for(seed = 0; seed < 50; seed++) {
        mines_new(&m, 3, 3, 8, (unsigned)seed);
        r = mines_reveal(&m, 1, 1, 5);
        CHECK(r == MINES_ENDED && m.state == MINES_WON && m.open == 1 && m.near[4] == 8 && m.flags == 8);
    }
    /* a corner of a 4 x 4 with 12 mines: its three neighbours are kept clear too (16 - 4 cells hold 12) */
    for(seed = 0, clear = 1; seed < 200; seed++) {
        mines_new(&m, 4, 4, 12, (unsigned)seed);
        mines_reveal(&m, 0, 0, 5);
        if((m.cell[1] | m.cell[4] | m.cell[5]) & MINE_HERE) clear = 0;
        CHECK(m.state == MINES_WON);                    /* four clear cells, all opened from the corner */
    }
    CHECK(clear);
    /* 13 mines: no room for the neighbours, only the corner is clear */
    for(seed = 0, ok = 1; seed < 200; seed++) {
        mines_new(&m, 4, 4, 13, (unsigned)seed);
        mines_reveal(&m, 0, 0, 5);
        if(m.state == MINES_LOST || !consistent(&m)) ok = 0;
    }
    CHECK(ok);
    /* the same seed and first reveal lay the same field; another seed another */
    {
        struct mines a, b;
        mines_new(&a, 16, 12, 32, 1234);
        mines_new(&b, 16, 12, 32, 1234);
        mines_reveal(&a, 3, 4, 0);
        mines_reveal(&b, 3, 4, 0);
        CHECK(!memcmp(a.cell, b.cell, sizeof(a.cell)));
        mines_new(&b, 16, 12, 32, 1235);
        mines_reveal(&b, 3, 4, 0);
        CHECK(memcmp(a.cell, b.cell, sizeof(a.cell)) != 0);
    }
}

static void opening(void) {
    static const char *const small[] = {
        "....*",
        ".....",
        "**...",
        ".....",
    };
    /* corridors three cells wide between walls of mines, joined at alternate ends: their middle rows are cells with
       no mine around them, one long winding area that a recursive opening would follow 150 calls deep */
    static const char *serpent[MINES_MAX_H];
    static char rows[MINES_MAX_H][MINES_MAX_W + 1];
    struct mines m;
    int x, y, n, walls = 0;
    field(&m, small, 4);
    CHECK(m.mines == 3 && m.near[3] == 1 && m.near[6] == 2 && m.near[0] == 0 && m.near[2 * 5 + 3] == 0);
    CHECK(mines_reveal(&m, 4, 3, 100) == MINES_MOVED);  /* the bottom right: an area */
    CHECK(consistent(&m) && areas_whole(&m));
    CHECK(!(m.cell[0] & MINE_OPEN) && !(m.cell[5] & MINE_OPEN));       /* behind the numbers, closed */
    CHECK((m.cell[2 * 5 + 2] & MINE_OPEN) && m.near[2 * 5 + 2] == 1);
    CHECK(mines_reveal(&m, 4, 3, 100) == MINES_NONE);   /* open already */
    n = m.open;
    CHECK(n == 9 && mines_reveal(&m, 0, 0, 200) == MINES_MOVED && m.open == n + 6);   /* the top left's area */
    CHECK((m.cell[0] & MINE_OPEN) && (m.cell[1] & MINE_OPEN) && (m.cell[5] & MINE_OPEN) && (m.cell[6] & MINE_OPEN));
    CHECK(m.state == MINES_PLAYING);
    CHECK(mines_reveal(&m, 0, 3, 250) == MINES_MOVED && m.open == 16);   /* a 2: itself alone */
    CHECK(mines_reveal(&m, 1, 3, 300) == MINES_ENDED && m.state == MINES_WON && m.flags == 3 && mines_left(&m) == 0);
    CHECK(mines_ms(&m, 99999) == 300 && consistent(&m));
    /* the serpent on the largest field: walls on rows 3, 7, 11 and 15, each open at one end, three cells wide */
    for(y = 0; y < MINES_MAX_H; y++) {
        for(x = 0; x < MINES_MAX_W; x++) {
            int gap = (y / 4) % 2 ? x < 3 : x >= MINES_MAX_W - 3;
            rows[y][x] = y % 4 == 3 && y < 16 && !gap ? '*' : '.';
            walls += rows[y][x] == '*';
        }
        rows[y][MINES_MAX_W] = 0;
        serpent[y] = rows[y];
    }
    field(&m, serpent, MINES_MAX_H);
    CHECK(walls == 4 * (MINES_MAX_W - 3) && m.mines == walls);
    CHECK(mines_reveal(&m, 0, 0, 1) == MINES_ENDED && m.state == MINES_WON);  /* one reveal opens every clear cell */
    CHECK(m.open == MINES_MAX - walls && consistent(&m) && areas_whole(&m));
    /* the largest field with one mine in a corner: the far corner opens everything, through the queue */
    mines_new(&m, MINES_MAX_W, MINES_MAX_H, 1, 3);
    m.cell[0] = MINE_HERE;
    m.near[1] = m.near[MINES_MAX_W] = m.near[MINES_MAX_W + 1] = 1;
    m.state = MINES_PLAYING;
    CHECK(mines_reveal(&m, MINES_MAX_W - 1, MINES_MAX_H - 1, 7) == MINES_ENDED && m.state == MINES_WON);
    CHECK(m.open == MINES_MAX - 1 && consistent(&m) && (m.cell[0] & MINE_FLAG));
}

static void flags_and_chords(void) {
    static const char *const f[] = {
        "*....",
        ".....",
        "..*..",
        ".....",
    };
    struct mines m;
    field(&m, f, 4);
    CHECK(mines_flag(&m, 0, 0) == MINES_MOVED && m.flags == 1 && mines_left(&m) == 1);
    CHECK(mines_reveal(&m, 0, 0, 1) == MINES_NONE);     /* a flag is never revealed */
    CHECK(mines_flag(&m, 0, 0) == MINES_MOVED && m.flags == 0);
    CHECK(mines_flag(&m, 4, 0) == MINES_MOVED);         /* a flag where no mine is: the area stops at it */
    CHECK(mines_reveal(&m, 4, 3, 1) == MINES_MOVED);
    CHECK(!(m.cell[4] & MINE_OPEN) && (m.cell[4] & MINE_FLAG) && (m.cell[3] & MINE_OPEN) && consistent(&m));
    CHECK(mines_flag(&m, 4, 3) == MINES_NONE);          /* an open cell takes no flag */
    CHECK(mines_flag(&m, 0, 3) == MINES_MOVED && mines_flag(&m, 1, 3) == MINES_MOVED);
    CHECK(mines_left(&m) == -1);                        /* more flags than mines */
    /* chords: 1,1 is a 2 (the corner and the middle); with one flag nothing happens, with both it opens the rest */
    field(&m, f, 4);
    mines_reveal(&m, 1, 1, 1);
    CHECK(m.near[1 * 5 + 1] == 2 && m.open == 1);
    CHECK(mines_chord(&m, 1, 1, 2) == MINES_NONE);
    mines_flag(&m, 0, 0);
    CHECK(mines_chord(&m, 1, 1, 2) == MINES_NONE);
    mines_flag(&m, 2, 2);
    CHECK(mines_chord(&m, 1, 1, 2) == MINES_MOVED && consistent(&m));
    CHECK((m.cell[1] & MINE_OPEN) && (m.cell[5] & MINE_OPEN) && (m.cell[2 * 5 + 1] & MINE_OPEN));
    CHECK(mines_chord(&m, 1, 1, 2) == MINES_NONE);      /* nothing covered is left around it */
    CHECK(mines_chord(&m, 4, 3, 2) == MINES_NONE);      /* not open */
    /* a wrong flag: the chord sets off the mine it left */
    field(&m, f, 4);
    mines_reveal(&m, 1, 1, 1);
    mines_flag(&m, 0, 0);
    mines_flag(&m, 2, 1);                               /* wrong: the mine is at 2,2 */
    CHECK(mines_chord(&m, 1, 1, 9) == MINES_ENDED && m.state == MINES_LOST && m.end == 9);
    CHECK((m.cell[2 * 5 + 2] & MINE_BLAST) && (m.cell[2 * 5 + 2] & MINE_OPEN) && consistent(&m));
    CHECK(mines_flag(&m, 4, 0) == MINES_NONE && mines_reveal(&m, 4, 0, 10) == MINES_NONE);   /* over */
    /* a chord with zero around it does nothing: there is no number to satisfy */
    field(&m, f, 4);
    mines_reveal(&m, 4, 0, 1);
    CHECK(m.near[4] == 0 && mines_chord(&m, 4, 0, 1) == MINES_NONE);
}

static void timer_and_loss(void) {
    static const char *const f[] = { "*...", "....", "...." };
    struct mines m;
    mines_new(&m, 4, 3, 1, 5);
    CHECK(mines_ms(&m, 123456) == 0);                   /* nothing revealed: nothing counted */
    mines_flag(&m, 2, 2);
    CHECK(m.state == MINES_READY && mines_ms(&m, 5000) == 0);   /* a flag does not start it */
    field(&m, f, 3);
    m.start = 0xFFFFF000u;                              /* the milliseconds since boot pass 2^32 */
    CHECK(mines_ms(&m, 0x1000u) == 0x2000u);
    CHECK(mines_reveal(&m, 0, 0, 0x2000u) == MINES_ENDED && m.state == MINES_LOST);
    CHECK(mines_ms(&m, 0x9999u) == 0x3000u && (m.cell[0] & MINE_BLAST) && m.open == 0);
    CHECK(mines_over(&m) && mines_chord(&m, 1, 1, 1) == MINES_NONE);
}

/* random games: random reveals, flags and chords until each ends; the counts always the field's own. Every other game
   only reveals, and its areas are whole after each: a flag taken away later leaves its cell covered beside an area
   (as it should: nothing opens by itself), so the games with flags are not held to that */
static void random_games(void) {
    struct mines m;
    unsigned r = mines_seed(2026);
    int g, steps, ok = 1, won = 0, lost = 0, act, x, y;
    for(g = 0; g < 3000; g++) {
        const struct mines_size *z = &mines_sizes[g % MINES_SIZES];
        mines_new(&m, z->w, z->h, z->mines, (unsigned)g * 7919u);
        for(steps = 0; steps < 4000 && !mines_over(&m); steps++) {
            x = (int)mines_below(&r, (unsigned)z->w);
            y = (int)mines_below(&r, (unsigned)z->h);
            act = g % 2 ? 0 : (int)mines_below(&r, 10);
            if(act < 6) mines_reveal(&m, x, y, (unsigned)steps);
            else if(act < 8) mines_flag(&m, x, y);
            else mines_chord(&m, x, y, (unsigned)steps);
            if(!consistent(&m) || (g % 2 && !areas_whole(&m))) ok = 0;
        }
        won += m.state == MINES_WON;
        lost += m.state == MINES_LOST;
        if(m.state == MINES_WON && (m.flags != m.mines || m.open != z->w * z->h - z->mines)) ok = 0;
    }
    if(won + lost <= 2900 || !lost) printf("  random games: %d won, %d lost of 3000\n", won, lost);
    CHECK(ok && won + lost > 2900 && lost > 0);
}

/**** the window's model ****/

static struct mview v;

/* the pointer: a press and a release of a button at a cell's middle */
static int at_x(int cx) { return v.fx + cx * v.cell + v.cell / 2; }
static int at_y(int cy) { return v.fy + cy * v.cell + v.cell / 2; }
static int click(unsigned button, int cx, int cy, unsigned now) {
    int r = mview_pointer(&v, at_x(cx), at_y(cy), button, button, now);
    return r | mview_pointer(&v, at_x(cx), at_y(cy), 0, 0, now);
}

static void status_is(const char *want) {
    char t[160];
    mview_status(&v, t, sizeof(t));
    if(strcmp(t, want)) printf("  status: \"%s\"\n", t);
    CHECK(!strcmp(t, want));
}

/* every clear cell of the view's game revealed, through clicks */
static void clear_all(unsigned now) {
    int x, y;
    for(y = 0; y < v.game.h; y++) for(x = 0; x < v.game.w; x++)
        if(!mines_over(&v.game) && !(v.game.cell[y * v.game.w + x] & (MINE_HERE | MINE_OPEN))) click(MV_BUTTON_LEFT, x, y, now);
}

/* the window for a field within the desktop's room, and its board (what is left by the toolbar and the status bar)
   laid out */
static void board(int size, int max_w, int max_h, int *w, int *h) {
    mview_window(size, max_w, max_h, w, h);
    mview_layout(&v, *w, *h - MV_BAR_H - MV_STATUS_H);
}

static void sizes(void) {
    static const int rooms[][2] = { { 958, 457 }, { 638, 317 }, { 0, 0 }, { 1918, 997 } };
    int s, r, w, h, cx, cy, ok = 1, bh;
    for(r = 0; r < 4; r++) for(s = 0; s < MINES_SIZES; s++) {
        mview_init(&v, 1);
        mview_new(&v, s);
        board(s, rooms[r][0], rooms[r][1], &w, &h);
        bh = h - MV_BAR_H - MV_STATUS_H;
        if(rooms[r][0] && (w > rooms[r][0] || h > rooms[r][1])) ok = 0;
        if(v.cell < MV_CELL_MIN || v.cell > (w == MV_MIN_W || h == MV_MIN_H ? MV_CELL_MAX : MV_CELL)) ok = 0;
        if(v.fx < MV_MARGIN || v.fx + v.game.w * v.cell > w - MV_MARGIN) ok = 0;
        if(v.fy < MV_MARGIN || v.fy + v.game.h * v.cell > bh - MV_MARGIN) ok = 0;
        for(cy = 0; cy < v.game.h; cy++) for(cx = 0; cx < v.game.w; cx++) {
            int x, y;
            if(!mview_cell_at(&v, at_x(cx), at_y(cy), &x, &y) || x != cx || y != cy) ok = 0;
        }
    }
    CHECK(ok);
    mview_window(MINES_LARGE, 958, 457, &w, &h);
    CHECK(w == 28 * 24 + 20 && h == MV_BAR_H + 15 * 24 + 20 + MV_STATUS_H);    /* the laptop: 24-pixel cells */
    mview_window(MINES_SMALL, 958, 457, &w, &h);
    CHECK(w == MV_MIN_W);                               /* the toolbar's width */
    board(MINES_LARGE, 638, 317, &w, &h);
    CHECK(h <= 317 && w == MV_MIN_W && v.cell == 16);  /* the smallest desktop: 16-pixel cells, the toolbar's width */
    CHECK(MV_MIN_W >= 28 * MV_CELL_MIN + 2 * MV_MARGIN && h == MV_MIN_H);  /* the smallest window holds the large field */
    mview_window(MINES_SMALL, 300, 200, &w, &h);
    CHECK(w == MV_MIN_W && h == MV_MIN_H);              /* never under the smallest */
    CHECK(!mview_cell_at(&v, v.fx - 1, at_y(0), &cx, &cy) && !mview_cell_at(&v, at_x(0), v.fy - 1, &cx, &cy));
    CHECK(!mview_cell_at(&v, v.fx + v.game.w * v.cell, at_y(0), &cx, &cy));
    CHECK(!mview_cell_at(&v, at_x(0), v.fy + v.game.h * v.cell, &cx, &cy));
    mview_layout(&v, 100, 100);                         /* too small: the top left shows */
    CHECK(v.cell == MV_CELL_MIN && v.fx == MV_MARGIN && v.fy == MV_MARGIN && mview_layout(&v, 100, 100) == 0);
    mview_new(&v, MINES_SMALL);                         /* a larger board: the small field's cells as large as allowed */
    mview_layout(&v, 900, 500);
    CHECK(v.cell == MV_CELL_MAX && v.fx == (900 - 10 * MV_CELL_MAX) / 2 && v.fy == (500 - 8 * MV_CELL_MAX) / 2);
}

static void pointer(void) {
    int w, h, x, y, n;
    mview_init(&v, 77);
    board(MINES_SMALL, 958, 457, &w, &h);
    status_is("10 mines in 10 by 8: click a cell to begin");
    /* a press shows its cell pressed; moving off the field and letting go takes it back */
    CHECK(mview_pointer(&v, at_x(4), at_y(3), MV_BUTTON_LEFT, MV_BUTTON_LEFT, 10) == MV_DRAW);
    CHECK(mview_look(&v, 4, 3) == MV_PRESSED && mview_look(&v, 5, 3) == MV_COVERED);
    CHECK(mview_pointer(&v, at_x(5), at_y(3), MV_BUTTON_LEFT, 0, 10) == MV_DRAW);  /* it follows the pointer */
    CHECK(mview_look(&v, 4, 3) == MV_COVERED && mview_look(&v, 5, 3) == MV_PRESSED);
    CHECK(mview_pointer(&v, v.fx - 5, at_y(3), MV_BUTTON_LEFT, 0, 10) == MV_DRAW && mview_look(&v, 5, 3) == MV_COVERED);
    mview_pointer(&v, v.fx - 5, at_y(3), 0, 0, 10);
    CHECK(v.game.state == MINES_READY && v.played[MINES_SMALL] == 0);
    /* a right click flags, again unflags; then a click reveals: the game starts */
    CHECK(click(MV_BUTTON_RIGHT, 0, 0, 20) == MV_DRAW && mview_look(&v, 0, 0) == MV_FLAG && mines_left(&v.game) == 9);
    click(MV_BUTTON_RIGHT, 0, 0, 20);
    CHECK(mview_look(&v, 0, 0) == MV_COVERED);
    CHECK(click(MV_BUTTON_LEFT, 4, 3, 1000) == MV_DRAW && v.game.state == MINES_PLAYING && v.played[0] == 1);
    CHECK(mview_look(&v, 4, 3) == MV_OPEN);             /* the first reveal: no mine around it */
    status_is("Right-click flags; click a number to clear around");
    /* a number: held, the covered cells around it show pressed; its flags placed, a click there chords */
    for(n = -1, y = 0; y < v.game.h && n < 0; y++) for(x = 0; x < v.game.w; x++) {
        int i = y * v.game.w + x, dx, dy, covered = 0;
        if(!(v.game.cell[i] & MINE_OPEN) || !v.game.near[i]) continue;
        for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++)
            if(x + dx >= 0 && y + dy >= 0 && x + dx < v.game.w && y + dy < v.game.h &&
               !(v.game.cell[(y + dy) * v.game.w + x + dx] & (MINE_OPEN | MINE_HERE))) covered++;
        if(covered) { n = i; break; }
    }
    CHECK(n >= 0);
    if(n >= 0) {
        int nx = n % v.game.w, ny = n / v.game.w, dx, dy, shown = 0, opened;
        mview_pointer(&v, at_x(nx), at_y(ny), MV_BUTTON_LEFT, MV_BUTTON_LEFT, 1100);
        for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++)
            if((dx || dy) && nx + dx >= 0 && ny + dy >= 0 && nx + dx < v.game.w && ny + dy < v.game.h &&
               !(v.game.cell[(ny + dy) * v.game.w + nx + dx] & MINE_OPEN)) shown += mview_look(&v, nx + dx, ny + dy) == MV_PRESSED;
        CHECK(shown > 0 && mview_look(&v, nx, ny) == MV_OPEN + v.game.near[n]);
        mview_pointer(&v, at_x(nx), at_y(ny), 0, 0, 1100);
        for(dy = -1; dy <= 1; dy++) for(dx = -1; dx <= 1; dx++)  /* the right flags, then the click: it chords */
            if(nx + dx >= 0 && ny + dy >= 0 && nx + dx < v.game.w && ny + dy < v.game.h &&
               (v.game.cell[(ny + dy) * v.game.w + nx + dx] & MINE_HERE) && !(v.game.cell[(ny + dy) * v.game.w + nx + dx] & MINE_FLAG))
                click(MV_BUTTON_RIGHT, nx + dx, ny + dy, 1200);
        opened = v.game.open;
        click(MV_BUTTON_LEFT, nx, ny, 1300);
        CHECK(v.game.open > opened || v.game.state == MINES_WON);
        opened = v.game.open;
        click(MV_BUTTON_MIDDLE, nx, ny, 1300);          /* the middle button chords too: nothing left to open */
        CHECK(v.game.open == opened);
    }
    /* a click on a flag does nothing; Right while the left is held does nothing either */
    for(y = 0; y < v.game.h && v.game.state == MINES_PLAYING; y++) for(x = 0; x < v.game.w; x++)
        if(v.game.cell[y * v.game.w + x] & MINE_FLAG) {
            n = v.game.open;
            click(MV_BUTTON_LEFT, x, y, 1400);
            CHECK(v.game.open == n && v.game.state == MINES_PLAYING);
            y = v.game.h;
            break;
        }
    mview_pointer(&v, at_x(0), at_y(0), MV_BUTTON_LEFT, MV_BUTTON_LEFT, 1500);
    n = v.game.flags;
    mview_pointer(&v, at_x(0), at_y(0), MV_BUTTON_LEFT | MV_BUTTON_RIGHT, MV_BUTTON_RIGHT, 1500);
    CHECK(v.game.flags == n);
    mview_pointer(&v, v.fx - 9, at_y(0), MV_BUTTON_RIGHT, 0, 1500);
    mview_pointer(&v, v.fx - 9, at_y(0), 0, 0, 1500);
    /* a press whose release the board never saw (the window lost the focus): forgotten, no reveal */
    n = v.game.open;
    mview_pointer(&v, at_x(0), at_y(0), MV_BUTTON_LEFT, MV_BUTTON_LEFT, 1500);
    CHECK(v.held && mview_cancel(&v) == MV_DRAW && !v.held && v.px == -1 && mview_cancel(&v) == 0);
    mview_pointer(&v, at_x(0), at_y(0), 0, 0, 1500);
    CHECK(v.game.open == n);
    mview_pointer(&v, at_x(0), at_y(0), MV_BUTTON_LEFT, MV_BUTTON_LEFT, 1500);
    v.buttons = 0;                                      /* (as if the release had been lost) */
    CHECK(mview_pointer(&v, at_x(1), at_y(0), 0, 0, 1500) == MV_DRAW && !v.held && v.game.open == n);
    /* a press made elsewhere and held as the pointer comes in is no press here; the next one is */
    mview_pointer(&v, -1, 0, MV_BUTTON_LEFT, 0, 1500);
    n = v.game.open;
    mview_pointer(&v, at_x(0), at_y(0), MV_BUTTON_LEFT, MV_BUTTON_LEFT, 1500);
    CHECK(v.held && v.px == 0 && v.py == 0);
    mview_pointer(&v, v.fx - 9, at_y(0), 0, 0, 1500);
    /* clicks win it: the session counts it, and the status bar says the time */
    clear_all(43200);
    CHECK(v.game.state == MINES_WON && v.won[0] == 1 && v.best[0] == 42200);
    status_is("Cleared in 0:42: the best this session");
    {
        char t[64];
        mview_record(&v, t, sizeof(t));
        CHECK(!strcmp(t, "Won 1 of 1, best 0:42"));
    }
    CHECK(click(MV_BUTTON_LEFT, 0, 0, 50000) == MV_DRAW && v.game.state == MINES_WON);   /* over: nothing more */
    CHECK(mview_pointer(&v, at_x(0), at_y(0), MV_BUTTON_LEFT, MV_BUTTON_LEFT, 1) == MV_DRAW && !v.held);
    mview_pointer(&v, at_x(0), at_y(0), 0, 0, 1);
}

static void keyboard(void) {
    int w, h, x, y;
    mview_init(&v, 3);
    board(MINES_SMALL, 958, 457, &w, &h);
    CHECK(!v.cursor && v.cx == 0 && v.cy == 0);
    CHECK(mview_key(&v, MV_RIGHT, 0, 0) == MV_DRAW && v.cursor && v.cx == 0);  /* the first key shows it */
    mview_key(&v, MV_RIGHT, 0, 0);
    mview_key(&v, MV_DOWN, 0, 0);
    CHECK(v.cx == 1 && v.cy == 1);
    mview_key(&v, MV_END, 0, 0);
    mview_key(&v, MV_PGDN, 0, 0);
    CHECK(v.cx == 9 && v.cy == 7);
    mview_key(&v, MV_RIGHT, 0, 0);
    mview_key(&v, MV_DOWN, 0, 0);
    CHECK(v.cx == 9 && v.cy == 7);                      /* the edges hold it */
    mview_key(&v, MV_HOME, 0, 0);
    mview_key(&v, MV_PGUP, 0, 0);
    mview_key(&v, MV_LEFT, 0, 0);
    mview_key(&v, MV_UP, 0, 0);
    CHECK(v.cx == 0 && v.cy == 0);
    CHECK(mview_key(&v, 'f', 0, 0) == MV_DRAW && (v.game.cell[0] & MINE_FLAG));
    CHECK(mview_key(&v, 'f', MV_REPEAT, 0) == 0 && (v.game.cell[0] & MINE_FLAG));    /* F held: no more */
    CHECK(mview_key(&v, 'F', 0, 0) == MV_DRAW && !(v.game.cell[0] & MINE_FLAG));
    CHECK(mview_key(&v, MV_RIGHT, MV_REPEAT, 0) == MV_DRAW && v.cx == 1);          /* an arrow held moves on */
    CHECK(mview_key(&v, ' ', MV_REPEAT | 1, 0) == 0 && v.game.state == MINES_READY);
    mview_key(&v, MV_LEFT, 0, 0);
    CHECK(mview_key(&v, 27, 0, 0) == MV_CLOSE);            /* no game under way: Esc closes */
    CHECK(mview_key(&v, ' ', 0, 500) == MV_DRAW && v.game.state == MINES_PLAYING && (v.game.cell[0] & MINE_OPEN));
    CHECK(mview_key(&v, 'x', 0, 0) == 0);
    /* Esc or Ctrl+W during a game asks; the question has the keys and the pointer until it is answered */
    CHECK(mview_key(&v, 23, 0, 0) == MV_DRAW && v.asking);
    CHECK(mview_key(&v, MV_RIGHT, 0, 0) == 0 && click(MV_BUTTON_LEFT, 5, 5, 600) == 0 && v.game.open > 0);
    CHECK(mview_answer(&v, 0) == MV_DRAW && !v.asking && v.game.state == MINES_PLAYING);
    CHECK(mview_close(&v) == MV_DRAW && v.asking && mview_answer(&v, 1) == MV_CLOSE);
    /* the keys win a game: Space on every clear cell, the cursor moved there */
    for(y = 0; y < v.game.h; y++) for(x = 0; x < v.game.w; x++)
        if(!mines_over(&v.game) && !(v.game.cell[y * v.game.w + x] & (MINE_HERE | MINE_OPEN))) {
            v.cx = x;
            v.cy = y;
            mview_key(&v, '\r', 0, 90500);
        }
    CHECK(v.game.state == MINES_WON && v.best[0] == 90000 && v.won[0] == 1 && v.played[0] == 1);
    status_is("Cleared in 1:30: the best this session");
    CHECK(mview_close(&v) == MV_CLOSE);                 /* over: no question */
    /* N: the same field again, its seed the next; 1 to 3 another field, and the window's size with it */
    CHECK(mview_key(&v, 'n', 0, 0) == MV_DRAW && v.game.state == MINES_READY && v.size == MINES_SMALL);
    CHECK(mview_key(&v, '3', 0, 0) == MV_RESIZE && v.size == MINES_LARGE && v.game.w == 28 && v.game.h == 15);
    CHECK(mview_key(&v, '2', 0, 0) == MV_RESIZE && v.size == MINES_MEDIUM && v.game.mines == 32 && v.cx <= 15);
    status_is("32 mines in 16 by 12: click a cell to begin");
    {
        char t[64];
        mview_record(&v, t, sizeof(t));
        CHECK(!strcmp(t, "Won 0 of 0"));
    }
    CHECK(v.best[0] == 90000);                          /* the session's scores stay */
}

static void scores_and_loss(void) {
    int w, h, x, y, i;
    char t[16];
    mview_time(0, t, sizeof(t));
    CHECK(!strcmp(t, "0:00"));
    mview_time(3599999, t, sizeof(t));
    CHECK(!strcmp(t, "59:59"));
    mview_time(3723000, t, sizeof(t));
    CHECK(!strcmp(t, "1:02:03"));
    mview_init(&v, 11);
    board(MINES_SMALL, 958, 457, &w, &h);
    click(MV_BUTTON_LEFT, 0, 0, 0);
    clear_all(30000);
    CHECK(v.best[0] == 30000);
    mview_new(&v, MINES_SMALL);                         /* slower: the best stays */
    click(MV_BUTTON_LEFT, 0, 0, 0);
    clear_all(45000);
    status_is("Cleared in 0:45 (the best: 0:30)");
    CHECK(v.best[0] == 30000);
    mview_new(&v, MINES_SMALL);                         /* a loss: it counts as played */
    click(MV_BUTTON_LEFT, 5, 4, 0);
    for(i = 0; i < 80 && v.game.state == MINES_PLAYING; i++)
        if(v.game.cell[i] & MINE_HERE) {
            if(i != 0) click(MV_BUTTON_RIGHT, 0, 0, 1);  /* a wrong flag, if 0,0 holds no mine */
            click(MV_BUTTON_LEFT, i % 10, i / 10, 12000);
        }
    CHECK(v.game.state == MINES_LOST && v.played[0] == 3 && v.won[0] == 2);
    status_is("A mine went off after 0:12. N: another field");
    {
        char t[64];
        mview_record(&v, t, sizeof(t));
        CHECK(!strcmp(t, "Won 2 of 3, best 0:30"));
    }
    for(y = 0, i = 0; y < v.game.h; y++) for(x = 0; x < v.game.w; x++) {
        unsigned char c = v.game.cell[y * v.game.w + x];
        int look = mview_look(&v, x, y);
        if(c & MINE_BLAST) i += look == MV_BLAST;
        else if((c & MINE_HERE) && !(c & MINE_FLAG)) i += look == MV_MINE;
        else if((c & MINE_FLAG) && !(c & MINE_HERE)) i += look == MV_WRONG;
        else if(c & MINE_OPEN) i += look == MV_OPEN + v.game.near[y * v.game.w + x];
        else i += look == MV_COVERED || look == MV_FLAG;
    }
    CHECK(i == 80);
    CHECK(mview_close(&v) == MV_CLOSE && mview_look(&v, -1, 0) == MV_COVERED);
}

/* the review of 11/05/26: N, 1 to 3, New and the sizes threw a game under way away unasked. Now a new game asked for
   during one is asked about, Cancel first: Cancel keeps it, yes starts the new one (the game left counted as played,
   from its first reveal); the question has the keys and the pointer, and another new game or a second question asked
   for meanwhile is not taken, but the close button's question takes the new game's place */
static void new_game_asked(void) {
    int w, h, open;
    char t[64];
    mview_init(&v, 21);
    board(MINES_SMALL, 958, 457, &w, &h);
    CHECK(mview_request(&v, MINES_SMALL) == MV_DRAW && v.game.state == MINES_READY && !v.asking);  /* none under way */
    CHECK(mview_request(&v, MINES_MEDIUM) == MV_RESIZE && v.size == MINES_MEDIUM && !v.asking);
    CHECK(mview_key(&v, '1', 0, 0) == MV_RESIZE && v.size == MINES_SMALL);
    board(MINES_SMALL, 958, 457, &w, &h);
    click(MV_BUTTON_LEFT, 4, 3, 1000);
    open = v.game.open;
    CHECK(v.game.state == MINES_PLAYING && v.played[0] == 1 && open > 0);
    CHECK(mview_key(&v, 'n', 0, 2000) == MV_DRAW && v.asking == MV_ASK_NEW && v.ask_size == MINES_SMALL);
    CHECK(mview_key(&v, '3', 0, 2000) == 0 && mview_request(&v, MINES_LARGE) == 0 && v.ask_size == MINES_SMALL);
    CHECK(click(MV_BUTTON_LEFT, 0, 0, 2000) == 0 && v.game.open == open);
    CHECK(mview_answer(&v, 0) == MV_DRAW && !v.asking && v.game.state == MINES_PLAYING && v.game.open == open);
    CHECK(mview_answer(&v, 1) == 0 && v.game.state == MINES_PLAYING);      /* no question up: nothing */
    /* 3 during the game: asked; yes: the large field, the window's size with it, the game left counted */
    CHECK(mview_key(&v, '3', 0, 3000) == MV_DRAW && v.asking == MV_ASK_NEW && v.ask_size == MINES_LARGE);
    CHECK(mview_answer(&v, 1) == MV_RESIZE && v.size == MINES_LARGE && v.game.state == MINES_READY && !v.asking);
    CHECK(v.played[0] == 1 && v.won[0] == 0);
    v.size = MINES_SMALL;
    mview_record(&v, t, sizeof(t));
    CHECK(!strcmp(t, "Won 0 of 1"));
    v.size = MINES_LARGE;
    /* the same field again by N, asked, yes: no new size */
    board(MINES_LARGE, 958, 457, &w, &h);
    click(MV_BUTTON_LEFT, 10, 7, 4000);
    CHECK(v.game.state == MINES_PLAYING && mview_request(&v, MINES_LARGE) == MV_DRAW && v.asking == MV_ASK_NEW);
    CHECK(mview_close(&v) == MV_DRAW && v.asking == MV_ASK_CLOSE);  /* the close button: its question instead */
    CHECK(mview_answer(&v, 0) == MV_DRAW && v.game.state == MINES_PLAYING);
    CHECK(mview_key(&v, 'N', 0, 5000) == MV_DRAW && mview_answer(&v, 1) == MV_DRAW && v.size == MINES_LARGE);
    CHECK(v.game.state == MINES_READY && v.played[2] == 1);
}

/* the review of 11/05/26: a field won in no time (its first reveal opened it all) kept 0 as its best, which also meant
   "none won yet": the record showed no best, and the next, slower, win took its place. The count of wins says it */
static void no_time(void) {
    int w, h;
    unsigned seed;
    char t[64];
    for(seed = 1; seed < 200; seed++) {                 /* one mine where the first reveal opens all the rest */
        struct mines m;
        mines_new(&m, 10, 8, 1, seed);
        mines_reveal(&m, 0, 0, 0);
        if(m.state == MINES_WON) break;
    }
    CHECK(seed < 200);
    mview_init(&v, 5);
    board(MINES_SMALL, 958, 457, &w, &h);
    CHECK(mines_new(&v.game, 10, 8, 1, seed) == 0);
    CHECK(click(MV_BUTTON_LEFT, 0, 0, 1000) == MV_DRAW && v.game.state == MINES_WON && v.best[0] == 0 && v.won[0] == 1);
    status_is("Cleared in 0:00: the best this session");
    mview_record(&v, t, sizeof(t));
    CHECK(!strcmp(t, "Won 1 of 1, best 0:00"));
    mview_new(&v, MINES_SMALL);                         /* a slower win: the best stays */
    click(MV_BUTTON_LEFT, 0, 0, 0);
    if(v.game.state == MINES_PLAYING) clear_all(5000);
    CHECK(v.game.state == MINES_WON && v.won[0] == 2 && v.best[0] == 0);
    mview_record(&v, t, sizeof(t));
    CHECK(!strcmp(t, "Won 2 of 2, best 0:00"));
}

int main(void) {
    generator();
    refused();
    first_reveal();
    opening();
    flags_and_chords();
    timer_and_loss();
    random_games();
    sizes();
    pointer();
    keyboard();
    scores_and_loss();
    new_game_asked();
    no_time();
    printf("test_wmines: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
