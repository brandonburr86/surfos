/* Host tests for Swell's window (user/wswell/window.c; 11/05/26, the desktop applications, track games) on the fake
   desktop (tests/host/gk_fake.c), driven as a person drives it and as the screen checks (tools/gui_apps_games.py) do:
   no desktop here; the window's size on the laptop's desktop and the smallest; the sea, its band and the panel
   drawn; Enter starting a game, the piece and its ghost drawn; the arrows moving it, held or pressed, presenting a few
   cells; the timer letting it fall a row; Space dropping it, the score following, a held Space dropping one; a row
   washed away showing foam for a moment; P and the menu pausing, the menu's check following; the focus lost pausing
   it; the end at the top, its band and the session's best; the close button and Esc asking during a game, Cancel
   first, Close ending it; New game and Help from the menu. Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <gk.h>
#include "gk_fake.h"
#include "window.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

#define C_WATER 0x0F2F3B                /* window.c's colours */
#define C_FLASH 0xE8F6F1
#define C_SAND 0x9C8452
static const unsigned colors[SW_KINDS] = { 0xD0B056, 0x659ECB, 0xEE906D, 0xB8E9D9, 0x7ED48C, 0xB48CC8, 0x3FB8AF };

static struct swwin w;
static int closed;

static int run(void) {
    struct gk_event ev;
    while(gk_next(w.ui, &ev, 0) > 0) if(!ww_event(&w, &ev)) return !(closed = 1);
    return 1;
}

/* a cell of the sea on the desktop's picture of the window: its top left */
static void cell_xy(int x, int y, int *px, int *py) {
    int bx, by, bw, bh;
    gk_bounds(w.sea, &bx, &by, &bw, &bh);
    *px = bx + w.v.sx + x * w.v.cell;
    *py = by + w.v.sy + y * w.v.cell;
}

static unsigned middle(int x, int y) {
    int px, py;
    cell_xy(x, y, &px, &py);
    return gkf_pixel(px + w.v.cell / 2, py + w.v.cell / 2);
}

static int status_says(const char *s) {
    return gkf_text(10, gkf_h() - GK_STATUS_H + 3, s, GW_INK);
}

static int find_text(const char *s, unsigned color) {
    int x, y;
    return gkf_find(s, color, &x, &y);
}

/* the falling piece's cells all show its colour */
static int piece_shown(void) {
    const struct swell_piece *p = swell_shape(w.v.game.kind, w.v.game.turn);
    int i;
    for(i = 0; i < p->n; i++)
        if(w.v.game.y + p->y[i] >= 0 && middle(w.v.game.x + p->x[i], w.v.game.y + p->y[i]) != colors[w.v.game.kind]) return 0;
    return 1;
}

static int presented_area(void) {
    int x, y, pw, ph, i, n, area = 0;
    n = gkf_presented(&x, &y, &pw, &ph);
    for(i = 0; i < n; i++) {
        gkf_presented_rect(i, &x, &y, &pw, &ph);
        area += pw * ph;
    }
    return area;
}

int main(void) {
    int width, height, x, y, n, i, px, py;
    setvbuf(stdout, NULL, _IOLBF, 0);
    gkf_reset(0, 0);
    gkf_refuse(ENODEV);
    CHECK(ww_open(&w, 1) < 0 && errno == ENODEV);
    CHECK(ww_open(&w, 2026) == 0 && !strcmp(gkf_title(), "Swell") && gkf_resizable());
    sview_window(0, 0, &width, &height);
    CHECK(gkf_w() == width && gkf_h() == height && width == 470 && height == 438);
    gkf_focus(1);
    CHECK(run() && w.v.cell == SV_CELL);
    CHECK(find_text("Enter starts", C_FLASH) && find_text("Next", GW_MUTED) && find_text("Score", GW_MUTED));
    CHECK(status_says("Enter starts; arrows move, Up turns, Space drops") && middle(0, 0) == C_WATER);
    CHECK(gkf_menu_find("New game") >= 0 && (gkf_menu_entry(gkf_menu_find("Pause"))->flags & WIN_MENU_DISABLED));
    CHECK(gkf_menu_find("Playing Swell") >= 0);
    /* Enter: the game starts, the piece at the top, its ghost at the bottom */
    gkf_key('\r', 0);
    CHECK(run() && w.v.game.state == SW_PLAYING && piece_shown() && !find_text("Enter starts", C_FLASH));
    {
        const struct swell_piece *p = swell_shape(w.v.game.kind, 0);
        cell_xy(w.v.game.x + p->x[0], SW_H - p->h + p->y[0], &px, &py);
        CHECK(gkf_pixel(px + 2, py + w.v.cell / 2) == colors[w.v.game.kind]);   /* the ghost's outline */
        CHECK(gkf_pixel(px + w.v.cell / 2, py + w.v.cell / 2) == C_WATER);
    }
    CHECK(!(gkf_menu_entry(gkf_menu_find("Pause"))->flags & WIN_MENU_DISABLED));
    /* the arrows, pressed and held: a few cells presented */
    presented_area();
    x = w.v.game.x;
    gkf_key(WIN_KEY_LEFT, 0);
    gkf_key(WIN_KEY_LEFT, WIN_MOD_REPEAT);
    CHECK(run() && w.v.game.x == x - 2 && piece_shown());
    CHECK(presented_area() < width * height / 3);
    gkf_key(WIN_KEY_RIGHT, 0);
    CHECK(run() && w.v.game.x == x - 1);
    /* the timer: the piece falls a row */
    y = w.v.game.y;
    gkf_advance(swell_interval(1));
    CHECK(run() && w.v.game.y == y + 1 && piece_shown());
    /* Space drops it: the score, and a held Space drops no more */
    n = w.v.game.pieces;
    gkf_key(' ', 0);
    gkf_key(' ', WIN_MOD_REPEAT);
    CHECK(run() && w.v.game.pieces == n + 1 && w.v.game.score > 0);
    {
        char b[16];
        snprintf(b, sizeof(b), "%u", w.v.game.score);
        CHECK(find_text(b, GW_INK));
    }
    /* a row washed away: foam for a moment, then the sea again */
    for(i = 0; i < SW_W; i++) w.v.game.sea[SW_H - 1][i] = i < 3 ? 0 : SW_SAND;
    w.v.game.sea[SW_H - 2][SW_W - 1] = SW_SAND;
    w.v.game.kind = SW_DRIFT;
    w.v.game.turn = 0;
    w.v.game.x = 0;
    w.v.game.y = 0;
    gk_redraw(w.sea);
    run();
    gkf_key(' ', 0);
    CHECK(run() && w.v.game.rows == 1 && middle(5, SW_H - 1) == C_FLASH && status_says("1 row washed away: 10 points"));
    gkf_advance(SV_FLASH_MS);
    CHECK(run() && middle(5, SW_H - 1) != C_FLASH && middle(SW_W - 1, SW_H - 1) == C_SAND);
    /* P pauses: the band and the menu's check; the menu's Pause goes on */
    gkf_key('p', 0);
    CHECK(run() && w.v.game.state == SW_PAUSED && find_text("Paused", C_FLASH));
    CHECK(gkf_menu_entry(gkf_menu_find("Pause"))->flags & WIN_MENU_CHECKED);
    CHECK(gkf_menu_choose(MENU_PAUSE) && run() && w.v.game.state == SW_PLAYING && !find_text("Paused", C_FLASH));
    /* the focus lost pauses it, and it waits for P */
    gkf_focus(0);
    CHECK(run() && w.v.game.state == SW_PAUSED && status_says("Paused while in the background: P goes on"));
    gkf_focus(1);
    CHECK(run() && w.v.game.state == SW_PAUSED);
    {
        int bx, by, bw, bh;                             /* a click to bring it back lets nothing fall */
        gk_bounds(w.sea, &bx, &by, &bw, &bh);
        gkf_advance(600);
        gkf_click(bx + w.v.sx + 30, by + w.v.sy + 30);
        CHECK(run() && w.v.game.state == SW_PAUSED);
    }
    gkf_key('P', WIN_MOD_SHIFT);
    CHECK(run() && w.v.game.state == SW_PLAYING);
    /* Esc during a game: paused, asked, Cancel first; the game waits paused */
    gkf_key(27, 0);
    CHECK(run() && gk_box_open(w.ui) && w.v.game.state == SW_PAUSED && find_text("Close Swell?", GW_PAPER));
    gkf_key('\r', 0);
    CHECK(run() && !gk_box_open(w.ui) && w.v.game.state == SW_PAUSED && !w.v.asking);
    gkf_key('\r', 0);
    CHECK(run() && w.v.game.state == SW_PLAYING);
    /* the end at the top: the band, the status, the session's best; Enter starts again */
    for(y = 3; y < SW_H; y++) for(i = 0; i < SW_W; i++) w.v.game.sea[y][i] = i == 0 ? 0 : SW_SAND;
    w.v.game.kind = SW_PEBBLE;
    w.v.game.turn = 0;
    w.v.game.x = 6;
    w.v.game.y = 0;
    w.v.game.next = SW_GULL;
    gkf_key(' ', 0);
    CHECK(run() && w.v.game.state == SW_OVER && find_text("Over the top", C_FLASH) && w.v.games == 1);
    {
        char b[80];
        snprintf(b, sizeof(b), "Over the top: %u points, the best yet. Enter: again", w.v.game.score);
        CHECK(status_says(b) && w.v.best == w.v.game.score);
    }
    gkf_close();                                        /* over: closes at once */
    CHECK(!run() && closed);
    ww_close(&w);
    CHECK(!gkf_open());
    /* the close button during a game asks; Close ends it. The menu's New game and Help */
    gkf_reset(0, 0);
    closed = 0;
    CHECK(ww_open(&w, 5) == 0);
    gkf_focus(1);
    run();
    {
        int bx, by, bw, bh;
        gk_bounds(w.sea, &bx, &by, &bw, &bh);
        gkf_click(bx + w.v.sx + 20, by + w.v.sy + 20);  /* a click starts the game that waits */
    }
    CHECK(run() && w.v.game.state == SW_PLAYING);
    gkf_key(' ', 0);
    run();
    n = w.v.game.pieces;
    CHECK(gkf_menu_choose(MENU_NEW) && run() && w.v.game.state == SW_PLAYING && w.v.game.pieces == 0 && n == 1);
    CHECK(gkf_menu_choose(MENU_HELP) && run() && gk_asking(w.ui) == BOX_HELP && w.v.game.state == SW_PAUSED);
    gkf_key('\r', 0);
    CHECK(run() && !gk_box_open(w.ui));
    gkf_key('p', 0);
    gkf_close();
    CHECK(run() && gk_box_open(w.ui));
    gkf_key(WIN_KEY_RIGHT, 0);                          /* to Close */
    gkf_key('\r', 0);
    CHECK(!run() && closed);
    ww_close(&w);
    /* the smallest desktop: the window as tall as it allows, the sea within */
    gkf_reset(638, 317);
    CHECK(ww_open(&w, 9) == 0 && gkf_w() == width && gkf_h() == 317);
    gkf_focus(1);
    CHECK(run() && w.v.cell == 15);
    cell_xy(SW_W - 1, SW_H - 1, &px, &py);
    CHECK(py + w.v.cell <= 317 - GK_STATUS_H && middle(SW_W - 1, SW_H - 1) == C_WATER && find_text("Best", GW_MUTED));
    ww_close(&w);
    gkf_reset(0, 0);
    printf("test_wswell_window: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
