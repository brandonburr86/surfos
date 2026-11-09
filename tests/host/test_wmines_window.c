/* Host tests for Mines' window (user/wmines/window.c; 11/05/26, the desktop applications, track games) on the fake
   desktop (tests/host/gk_fake.c), driven as a person drives it and as the screen checks (tools/gui_apps_games.py)
   do: no desktop here; the window's size on the laptop's desktop and the smallest; the toolbar, the board and the
   status bar drawn; a click revealing an area, a right-click a flag, the counters and the clock following; only the
   cells that changed presented; the keyboard's cursor shown while the board has the keys, Space and F; the field's
   size by the toolbar, the Game menu and the keys, its check in the menu following; a game won by clicks and the
   session's record; a game lost, its mines shown; the close button and Esc asking during a game (Cancel first),
   Close ending it, closing at once otherwise; Help; a new size and a new theme painting the board again. The review
   of 11/05/26: a new field during a game (the sizes' buttons, Ctrl+N) asked about first, Cancel first, Cancel keeping
   the game and the size shown; the mine that went off a splash of SurfOS's own (no coral); the window at its own
   smallest size (no smaller taken): the toolbar whole, the large field within the board, the Help's and the questions'
   lines whole. Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <gk.h>
#include "gk_fake.h"
#include "window.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

#define C_COVER 0xDCCB9E                /* window.c's colours */
#define C_OPEN 0xF7F3E8
#define C_SPLASH 0x659ECB
#define C_RING 0xE8F6F1
#define C_CORAL 0xEE906D                /* (the old look's blast: no more) */
#define C_MINE 0x102E3B
#define C_HOOP 0x197D83
#define C_FLAG 0x197D83
#define C_CURSOR 0xE0603F

static struct mwin m;
static int closed;

/* every event the window has, through the program; 0 once it asked to close */
static int run(void) {
    struct gk_event ev;
    while(gk_next(m.ui, &ev, 0) > 0) if(!mw_event(&m, &ev)) return !(closed = 1);
    return 1;
}

/* a cell's top left on the desktop's picture of the window */
static void cell_xy(int x, int y, int *px, int *py) {
    int bx, by, bw, bh;
    gk_bounds(m.board, &bx, &by, &bw, &bh);
    *px = bx + m.v.fx + x * m.v.cell;
    *py = by + m.v.fy + y * m.v.cell;
}

static void click(int x, int y, unsigned button) {
    int px, py;
    cell_xy(x, y, &px, &py);
    gkf_advance(600);                   /* never a double-click */
    gkf_pointer(px + m.v.cell / 2, py + m.v.cell / 2, button);
    gkf_pointer(px + m.v.cell / 2, py + m.v.cell / 2, 0);
}

/* the cell shows color somewhere inside it */
static int shows(int x, int y, unsigned color) {
    int px, py, i, j;
    cell_xy(x, y, &px, &py);
    for(j = 1; j < m.v.cell; j++) for(i = 1; i < m.v.cell; i++) if(gkf_pixel(px + i, py + j) == color) return 1;
    return 0;
}

static unsigned corner(int x, int y) {    /* a cell's own colour, clear of what is drawn in it */
    int px, py;
    cell_xy(x, y, &px, &py);
    return gkf_pixel(px + 3, py + 3);
}

static int status_says(const char *s) {
    return gkf_text(10, gkf_h() - GK_STATUS_H + 3, s, GW_INK);
}

static int find_text(const char *s, unsigned color) {
    int x, y;
    return gkf_find(s, color, &x, &y);
}

/* a box's lines, whole (gkbox.c: the title on teal, the first line in ink, the second muted) */
static int box_says(const char *title, const char *line1, const char *line2) {
    return gk_box_open(m.ui) && find_text(title, GW_PAPER) && (!*line1 || find_text(line1, GW_INK)) &&
           (!*line2 || find_text(line2, GW_MUTED));
}

/* the question's buttons: Cancel first, the default, then the other */
static int cancel_first(const char *other) {
    int cx, cy, ox, oy;
    return gkf_find("Cancel", GW_PAPER, &cx, &cy) && gkf_find(other, GW_INK, &ox, &oy) && cx < ox && cy == oy;
}

/* where a widget ends, and starts */
static int right_of(struct gk_widget *g) {
    int x, y, w, h;
    gk_bounds(g, &x, &y, &w, &h);
    return x + w;
}

static int left_of(struct gk_widget *g) {
    int x, y, w, h;
    gk_bounds(g, &x, &y, &w, &h);
    return x;
}

/* every clear cell clicked: the game won */
static void win_it(void) {
    int x, y;
    for(y = 0; y < m.v.game.h; y++) for(x = 0; x < m.v.game.w; x++)
        if(!mines_over(&m.v.game) && !(m.v.game.cell[y * m.v.game.w + x] & (MINE_HERE | MINE_OPEN))) click(x, y, 1);
    run();
}

int main(void) {
    int x, y, w, h, n, px, py, i;
    setvbuf(stdout, NULL, _IOLBF, 0);
    gkf_reset(0, 0);
    gkf_refuse(ENODEV);
    CHECK(mw_open(&m, 1) < 0 && errno == ENODEV);
    /* the laptop's desktop: the large field's window, 24-pixel cells */
    CHECK(mw_open(&m, 2026) == 0 && !strcmp(gkf_title(), "Mines") && gkf_resizable());
    mview_window(MINES_LARGE, 0, 0, &w, &h);
    CHECK(gkf_w() == w && gkf_h() == h && w == 692 && h == 436);
    gkf_focus(1);
    CHECK(run());
    CHECK(find_text("New", GW_INK) && find_text("Small", GW_INK) && find_text("Medium", GW_INK));
    CHECK(find_text("Mines 10", GW_INK) && find_text("Time 0:00", GW_INK));
    CHECK(status_says("10 mines in 10 by 8: click a cell to begin"));
    CHECK(find_text("Won 0 of 0", GW_MUTED));
    CHECK(m.v.cell == MV_CELL_MAX && corner(0, 0) == C_COVER && corner(9, 7) == C_COVER);   /* the small field, large */
    CHECK(gkf_menu_count() > 0 && (gkf_menu_entry(gkf_menu_find("Small field (10 by 8)"))->flags & WIN_MENU_RADIO));
    CHECK(gkf_menu_find("Playing Mines") >= 0 && !strcmp(gkf_menu_entry(gkf_menu_find("New field"))->keys, "Ctrl+N"));
    /* a click: an area opens, the clock starts */
    click(4, 3, WIN_BUTTON_LEFT);
    CHECK(run() && m.v.game.state == MINES_PLAYING && corner(4, 3) == C_OPEN);
    CHECK(status_says("Right-click flags; click a number to clear around"));
    gkf_advance(1000);
    CHECK(run() && find_text("Time 0:01", GW_INK));
    /* the clock's tick presents its label and nothing else */
    gkf_presented(&x, &y, &w, &h);
    gkf_advance(1000);
    CHECK(run() && find_text("Time 0:02", GW_INK));
    n = gkf_presented(&x, &y, &w, &h);
    CHECK(n >= 1 && w * h <= 16 * 8 * 24);
    /* a flag on a covered cell next to the area: one cell and the count presented */
    for(i = -1, y = 0; y < m.v.game.h && i < 0; y++) for(x = 0; x < m.v.game.w; x++)
        if(m.v.game.cell[y * m.v.game.w + x] & MINE_HERE) { i = y * m.v.game.w + x; break; }
    CHECK(i >= 0);
    click(i % m.v.game.w, i / m.v.game.w, WIN_BUTTON_RIGHT);
    CHECK(run() && shows(i % m.v.game.w, i / m.v.game.w, C_FLAG) && find_text("Mines 9", GW_INK));
    {
        int px0, py0, pw, ph, area = 0;
        n = gkf_presented(&px0, &py0, &pw, &ph);
        for(x = 0; x < n; x++) {
            gkf_presented_rect(x, &px0, &py0, &pw, &ph);
            area += pw * ph;
        }
        CHECK(area <= 2 * (MV_CELL_MAX + 1) * (MV_CELL_MAX + 1) + 24 * 16 * 8);   /* the cell (and the pressed one), the count */
    }
    /* the keyboard: the cursor shows on the first arrow, Space reveals, F flags; without the keys it does not show */
    gkf_key(WIN_KEY_HOME, 0);
    gkf_key(WIN_KEY_PGUP, 0);
    CHECK(run() && m.v.cursor && m.v.cx == 0 && m.v.cy == 0 && shows(0, 0, C_CURSOR));
    gkf_focus(0);
    CHECK(run() && !shows(0, 0, C_CURSOR));
    gkf_focus(1);
    CHECK(run() && shows(0, 0, C_CURSOR));
    for(x = 0; x < 3; x++) gkf_key(WIN_KEY_RIGHT, x ? WIN_MOD_REPEAT : 0);  /* an arrow held: it moves on */
    CHECK(run() && m.v.cx == 3 && shows(3, 0, C_CURSOR) && !shows(0, 0, C_CURSOR));
    n = m.v.game.flags;
    gkf_key('f', 0);
    gkf_key('f', WIN_MOD_REPEAT);       /* F held: one flag, not taken back */
    CHECK(run() && m.v.game.flags == (m.v.game.cell[3] & MINE_OPEN ? n : n + 1));
    if(m.v.game.flags == n + 1) {
        gkf_key('F', WIN_MOD_SHIFT);
        CHECK(run() && m.v.game.flags == n);
    }
    /* Esc during a game asks, Cancel first; the close button too; Close ends the window */
    gkf_key(27, 0);
    CHECK(run() && gk_box_open(m.ui) && find_text("Close Mines?", GW_PAPER));
    gkf_key('\r', 0);
    CHECK(run() && !gk_box_open(m.ui) && !m.v.asking && m.v.game.state == MINES_PLAYING);
    gkf_close();
    CHECK(run() && gk_box_open(m.ui));
    gkf_key(27, 0);
    CHECK(run() && !gk_box_open(m.ui));
    /* the field's size: the toolbar's radio buttons, the menu, the keys; the menu's check follows */
    /* during the game a new size is asked about first (the review of 11/05/26), Cancel first: Cancel keeps the game and
       the size shown; New field lays the new one */
    gkf_advance(600);
    {
        int rx, ry;
        CHECK(gkf_find("Large", GW_INK, &rx, &ry));
        gkf_click(rx + 4, ry + 4);
        CHECK(run() && gk_asking(m.ui) == BOX_NEW && m.v.size == MINES_SMALL && m.v.game.state == MINES_PLAYING);
        CHECK(box_says("Start a new field?", "A game is under way: a new field ends it.", "") &&
              cancel_first("New field"));
        gkf_key('\r', 0);
        CHECK(run() && !gk_box_open(m.ui) && m.v.size == MINES_SMALL && gk_value(m.size) == MINES_SMALL);
        CHECK(m.v.game.state == MINES_PLAYING && corner(4, 3) == C_OPEN && gk_focused(m.ui) == m.board);
        gkf_advance(600);
        gkf_click(rx + 4, ry + 4);
        CHECK(run() && gk_asking(m.ui) == BOX_NEW);
        gkf_key(WIN_KEY_RIGHT, 0);                      /* to New field */
        gkf_key('\r', 0);
    }
    CHECK(run() && m.v.size == MINES_LARGE && m.v.game.w == 28 && gk_focused(m.ui) == m.board);
    CHECK(m.v.played[0] == 1 && m.v.won[0] == 0);       /* the game left counts as played */
    CHECK(m.v.cell == 24 && status_says("80 mines in 28 by 15: click a cell to begin"));
    CHECK(gkf_menu_entry(gkf_menu_find("Large field (28 by 15)"))->flags & WIN_MENU_RADIO);
    CHECK(!(gkf_menu_entry(gkf_menu_find("Small field (10 by 8)"))->flags & WIN_MENU_RADIO));
    CHECK(corner(27, 14) == C_COVER && find_text("Mines 80", GW_INK) && find_text("Time 0:00", GW_INK));
    CHECK(gkf_menu_choose(MENU_SIZE + MINES_MEDIUM) && run() && m.v.size == MINES_MEDIUM && gk_value(m.size) == 1);
    gkf_key('1', 0);
    CHECK(run() && m.v.size == MINES_SMALL && m.v.cell == MV_CELL_MAX);
    click(0, 0, WIN_BUTTON_LEFT);
    CHECK(run() && m.v.game.state == MINES_PLAYING);
    gkf_key(14, WIN_MOD_CTRL);          /* Ctrl+N: the field again, new, once asked */
    CHECK(run() && gk_asking(m.ui) == BOX_NEW && m.v.game.state == MINES_PLAYING);
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key('\r', 0);
    CHECK(run() && m.v.game.state == MINES_READY && m.v.played[0] == 2);
    /* a game won by clicks: the status bar and the session's record */
    click(5, 4, WIN_BUTTON_LEFT);
    run();
    gkf_advance(41000);
    win_it();
    CHECK(m.v.game.state == MINES_WON && m.v.won[0] == 1 && m.v.best[0] >= 41000);
    {
        char t[16], want[64];
        mview_time(m.v.best[0], t, sizeof(t));
        snprintf(want, sizeof(want), "Cleared in %s: the best this session", t);
        CHECK(status_says(want));
        snprintf(want, sizeof(want), "Won 1 of 3, best %s", t);
        CHECK(find_text(want, GW_MUTED));
    }
    for(i = 0; i < 80; i++) if(m.v.game.cell[i] & MINE_HERE) break;
    CHECK(shows(i % 10, i / 10, C_FLAG));               /* won: every mine flagged */
    gkf_close();                                        /* over: no question */
    CHECK(!run() && closed);
    mw_close(&m);
    CHECK(!gkf_open());
    /* a game lost: the mines shown, the one that went off a splash (a ring of foam on sky blue; no coral) */
    gkf_reset(0, 0);
    closed = 0;
    CHECK(mw_open(&m, 7) == 0);
    gkf_focus(1);
    run();
    click(4, 3, WIN_BUTTON_LEFT);
    run();
    for(i = 0; i < 80; i++) if(m.v.game.cell[i] & MINE_HERE) break;
    click(i % 10, i / 10, WIN_BUTTON_LEFT);
    CHECK(run() && m.v.game.state == MINES_LOST && corner(i % 10, i / 10) == C_SPLASH && shows(i % 10, i / 10, C_MINE));
    CHECK(shows(i % 10, i / 10, C_RING) && shows(i % 10, i / 10, C_HOOP) && !shows(i % 10, i / 10, C_CORAL));
    for(n = i + 1; n < 80; n++) if(m.v.game.cell[n] & MINE_HERE) break;
    CHECK(n < 80 && corner(n % 10, n / 10) == C_OPEN && shows(n % 10, n / 10, C_MINE) && shows(n % 10, n / 10, C_HOOP));
    CHECK(!shows(n % 10, n / 10, C_RING) && !shows(n % 10, n / 10, C_CORAL));
    CHECK(gkf_find("A mine went off after 0:00", GW_INK, &x, &y));
    /* Help, in the desktop's Help */
    CHECK(gkf_menu_choose(MENU_HELP) && run() && gk_box_open(m.ui) && gk_asking(m.ui) == BOX_HELP);
    gkf_key('\r', 0);
    CHECK(run() && !gk_box_open(m.ui));
    /* a new size: the field laid out again, its cells larger; a new theme: the board painted again */
    gkf_resize(900, 457);
    CHECK(run() && m.bw == 900 && m.v.cell == MV_CELL_MAX && corner(0, 0) == C_COVER);
    cell_xy(0, 0, &px, &py);
    CHECK(px == (900 - 10 * MV_CELL_MAX) / 2);
    gkf_set_theme(WIN_THEME_DUSK, NULL);
    CHECK(run() && corner(0, 0) == C_COVER);
    gkf_close();
    CHECK(!run());
    mw_close(&m);
    /* the smallest desktop (1280 x 800 at 2x): the large field still fits, at 16-pixel cells */
    gkf_reset(638, 317);
    CHECK(mw_open(&m, 9) == 0 && gkf_w() == 638 && gkf_h() == 317);
    gkf_focus(1);
    gkf_key('3', 0);
    CHECK(run() && m.v.size == MINES_LARGE && m.v.cell == MV_CELL_MIN);
    cell_xy(27, 14, &px, &py);
    CHECK(px + MV_CELL_MIN <= 638 && py + MV_CELL_MIN <= 317 - GK_STATUS_H && corner(27, 14) == C_COVER);
    mw_close(&m);
    /* the window at its own smallest size (the review of 11/05/26: the toolbar and the large field were cut, the Help's
       second line too): no smaller taken; the toolbar's widgets whole, apart, inside; the large field within the board;
       the Help's and the questions' lines whole */
    gkf_reset(0, 0);
    CHECK(mw_open(&m, 13) == 0);
    gkf_focus(1);
    gkf_resize(MV_MIN_W, MV_MIN_H);
    gkf_key('3', 0);
    CHECK(run() && gkf_w() == MV_MIN_W && gkf_h() == MV_MIN_H && m.v.size == MINES_LARGE && m.v.cell == MV_CELL_MIN);
    gkf_resize(MV_MIN_W - 1, MV_MIN_H);
    gkf_resize(MV_MIN_W, MV_MIN_H - 1);
    CHECK(run() && gkf_w() == MV_MIN_W && gkf_h() == MV_MIN_H);
    CHECK(right_of(m.clock) <= MV_MIN_W - 5 && left_of(m.clock) >= right_of(m.left) + 12);
    CHECK(left_of(m.left) >= right_of(m.size) + 12 && find_text("Time 0:00", GW_INK) && find_text("Mines 80", GW_INK));
    cell_xy(27, 14, &px, &py);
    CHECK(px + MV_CELL_MIN <= MV_MIN_W && py + MV_CELL_MIN <= MV_MIN_H - GK_STATUS_H && corner(27, 14) == C_COVER);
    CHECK(strlen(MW_HELP_1) <= 50 && strlen(MW_HELP_2) <= 50);
    CHECK(gkf_menu_choose(MENU_HELP) && run() && box_says("Playing Mines", MW_HELP_1, MW_HELP_2));
    gkf_key('\r', 0);
    click(13, 7, WIN_BUTTON_LEFT);
    CHECK(run() && m.v.game.state == MINES_PLAYING);
    gkf_key('n', 0);
    CHECK(run() && box_says("Start a new field?", "A game is under way: a new field ends it.", "") &&
          cancel_first("New field"));
    gkf_key(27, 0);
    gkf_key(27, 0);
    CHECK(run() && box_says("Close Mines?", "A game is under way: it ends with the window.", "") && cancel_first("Close"));
    mw_close(&m);
    gkf_reset(0, 0);
    printf("test_wmines_window: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
