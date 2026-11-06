/* Host tests for Solitaire's window (user/wsolitaire/window.c; 11/05/26, the desktop applications, track games) on
   the fake desktop (tests/host/gk_fake.c), driven as a person drives it and as the screen checks
   (tools/gui_apps_games.py) do: no desktop here; the window's size; the toolbar, the table and the status bar drawn,
   backs, faces and empty places; a click on the stock dealing, the moves and the clock following; a card dragged onto
   a pile, presenting only the piles it touched and where it went; a card dragged nowhere going back; a click sending a
   card where it goes; Undo by the toolbar, Ctrl+Z and the menu, greyed when there is nothing to undo; drawing three
   by the toolbar and the menu, its waste fanned; the keyboard's card framed while the table has the keys; a game that
   finishes by itself a card a tick, and the session's record; the close button and Esc asking during a game, Cancel
   first; Help; the smallest desktop. Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <gk.h>
#include "gk_fake.h"
#include "window.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

#define C_TABLE 0x165B5F                /* window.c's colours */
#define C_FACE 0xFFFDF5
#define C_BACK 0x102E3B
#define C_RIM 0xB8E9D9
#define C_CURSOR 0xE0603F

static struct swin s;
static int closed;

static int run(void) {
    struct gk_event ev;
    while(gk_next(s.ui, &ev, 0) > 0) if(!sw_event(&s, &ev)) return !(closed = 1);
    return 1;
}

/* card index of pile on the desktop's picture of the window: its top left, and a point on its visible part */
static void card_xy(int pile, int index, int *x, int *y) {
    int bx, by, bw, bh;
    gk_bounds(s.table, &bx, &by, &bw, &bh);
    table_card_xy(&s.t, pile, index, x, y);
    *x += bx;
    *y += by;
}

static void on_card(int pile, int index, int *x, int *y) {
    card_xy(pile, index, x, y);
    *x += s.t.cw / 2;
    *y += index < s.t.game.pile[pile].n - 1 && SOL_IS_TABLEAU(pile) ? 4 : s.t.ch / 2;
}

static void click_card(int pile, int index) {
    int x, y;
    on_card(pile, index, &x, &y);
    gkf_advance(600);
    gkf_click(x, y);
}

/* a drag from a card to a point, moved in steps as a hand would */
static void drag(int pile, int index, int tx, int ty) {
    int x, y;
    on_card(pile, index, &x, &y);
    gkf_advance(600);
    gkf_pointer(x, y, WIN_BUTTON_LEFT);
    gkf_pointer(x + (tx - x) / 2, y + (ty - y) / 2, WIN_BUTTON_LEFT);
    gkf_pointer(tx, ty, WIN_BUTTON_LEFT);
    gkf_pointer(tx, ty, 0);
}

static int status_says(const char *t) {
    return gkf_text(10, gkf_h() - GK_STATUS_H + 3, t, GW_INK);
}

static int find_text(const char *t, unsigned color) {
    int x, y;
    return gkf_find(t, color, &x, &y);
}

/* a move between tableau piles the deal allows: 1 with it */
static int tableau_move(int *from, int *index, int *to) {
    int p, q, i;
    for(p = SOL_TABLEAU; p < SOL_PILES; p++)
        for(i = 0; i < s.t.game.pile[p].n; i++)
            for(q = SOL_TABLEAU; q < SOL_PILES; q++)
                if(sol_can_put(&s.t.game, p, i, q) && s.t.game.pile[q].n) {
                    *from = p;
                    *index = i;
                    *to = q;
                    return 1;
                }
    return 0;
}

/* a table nearly won: the foundations all but the tens and up, which lie in runs on the tableau, the jack of spades
   face down under the ten of hearts */
static void nearly(void) {
    static const char *const runs[5] = { "KH QS JH TS", "KS QH", "KD QC JD TC", "KC QD JC TD", "JS TH" };
    static const char ranks[] = "A23456789TJQK", suits[] = "SHCD";
    struct klondike *k = &s.t.game;
    int f, r, p, i;
    memset(k->pile, 0, sizeof(k->pile));
    for(f = 0; f < 4; f++)
        for(r = 0; r < 9; r++) k->pile[SOL_FOUNDATION + f].card[k->pile[SOL_FOUNDATION + f].n++] = (unsigned char)((f * 13 + r) | SOL_UP);
    for(p = 0; p < 5; p++)
        for(i = 0; i < (int)strlen(runs[p]); i += 3) {
            int rank = (int)(strchr(ranks, runs[p][i]) - ranks), suit = (int)(strchr(suits, runs[p][i + 1]) - suits);
            k->pile[SOL_TABLEAU + p].card[k->pile[SOL_TABLEAU + p].n++] = (unsigned char)((suit * 13 + rank) | SOL_UP);
        }
    k->pile[SOL_TABLEAU + 4].card[0] &= (unsigned char)~SOL_UP;     /* one card down: no finish until it turns */
    k->started = 1;
    k->start = gkf_clock();
}

int main(void) {
    int x, y, w, h, n, from, index, to, i, px, py;
    setvbuf(stdout, NULL, _IOLBF, 0);
    gkf_reset(0, 0);
    gkf_refuse(ENODEV);
    CHECK(sw_open(&s, 1) < 0 && errno == ENODEV);
    CHECK(sw_open(&s, 4242) == 0 && !strcmp(gkf_title(), "Solitaire") && gkf_resizable());
    CHECK(gkf_w() == TB_W && gkf_h() == TB_BAR_H + TB_H + TB_STATUS_H);
    gkf_focus(1);
    CHECK(run());
    CHECK(find_text("New", GW_INK) && find_text("Draw one", GW_INK) && find_text("Draw three", GW_INK));
    CHECK(find_text("Moves 0", GW_INK) && find_text("Time 0:00", GW_INK) && status_says("Drag a card, or click it; Space deals"));
    CHECK(find_text("Won 0 of 0", GW_MUTED) && !gk_enabled(s.undo));
    CHECK(s.t.cw == TB_CARD_W);
    card_xy(SOL_STOCK, 23, &x, &y);
    CHECK(gkf_pixel(x, y + 4) == C_RIM && gkf_pixel(x + 5, y + 2) == C_BACK);  /* the stock: a back */
    card_xy(SOL_TABLEAU + 6, 6, &x, &y);
    CHECK(gkf_pixel(x + s.t.cw - 4, y + 30) == C_FACE);                         /* a face up */
    card_xy(SOL_FOUNDATION, 0, &x, &y);
    CHECK(gkf_pixel(x + s.t.cw / 2, y - 4) == C_TABLE && gkf_pixel(x, y + 10) != C_TABLE);    /* a place's outline */
    CHECK(gkf_menu_find("New deal") >= 0 && (gkf_menu_entry(gkf_menu_find("Undo"))->flags & WIN_MENU_DISABLED) &&
          (gkf_menu_entry(gkf_menu_find("Draw one"))->flags & WIN_MENU_RADIO) && gkf_menu_find("Playing Solitaire") >= 0);
    /* a click on the stock deals: a face on the waste, a move, the clock */
    click_card(SOL_STOCK, 23);
    CHECK(run() && s.t.game.pile[SOL_WASTE].n == 1 && find_text("Moves 1", GW_INK) && gk_enabled(s.undo));
    card_xy(SOL_WASTE, 0, &x, &y);
    CHECK(gkf_pixel(x + s.t.cw - 4, y + 30) == C_FACE);
    CHECK(!(gkf_menu_entry(gkf_menu_find("Undo"))->flags & WIN_MENU_DISABLED));
    gkf_advance(1000);
    CHECK(run() && find_text("Time 0:01", GW_INK));
    /* Undo: the toolbar's button, then nothing to undo: greyed */
    {
        int ux, uy;
        CHECK(gkf_find("Undo", GW_INK, &ux, &uy));
        gkf_advance(600);
        gkf_click(ux + 8, uy + 4);
    }
    CHECK(run() && s.t.game.pile[SOL_WASTE].n == 0 && s.t.game.pile[SOL_STOCK].n == 24 && !gk_enabled(s.undo));
    CHECK(gk_focused(s.ui) == s.table);
    /* a drag onto a pile it fits: it moves, and only the piles it touched (and where it went) are presented */
    while(!tableau_move(&from, &index, &to)) {          /* (a deal with none: the next) */
        sw_event(&s, &(struct gk_event){ .type = GK_EV_MENU, .id = MENU_NEW });
        run();
    }
    run();
    gkf_presented(&x, &y, &w, &h);
    n = s.t.game.pile[to].n;
    on_card(to, n - 1, &px, &py);
    drag(from, index, px, py + 10);
    CHECK(run() && s.t.game.pile[to].n > n && s.t.game.moves >= 1);
    {
        int rx, ry, rw, rh, area = 0;
        n = gkf_presented(&rx, &ry, &rw, &rh);
        for(i = 0; i < n; i++) {
            gkf_presented_rect(i, &rx, &ry, &rw, &rh);
            area += rw * rh;
        }
        CHECK(n >= 1 && area < gkf_w() * gkf_h() / 2);
    }
    /* a drag cut short by the focus going: the cards go back, and nothing stays drawn over the table */
    {
        int tx, ty;
        n = s.t.game.moves;
        i = s.t.game.pile[SOL_TABLEAU + 6].n;
        on_card(SOL_TABLEAU + 6, i - 1, &tx, &ty);
        gkf_advance(600);
        gkf_pointer(tx, ty, WIN_BUTTON_LEFT);
        gkf_pointer(tx - 200, ty + 40, WIN_BUTTON_LEFT);
        CHECK(run() && s.t.dragging && s.drag_w > 0);
        gkf_focus(0);
        CHECK(run() && !s.t.dragging && s.drag_w == 0 && s.t.game.moves == n && s.t.game.pile[SOL_TABLEAU + 6].n == i);
        CHECK(gkf_pixel(tx - 200, ty + 40) == C_TABLE);
        gkf_focus(1);
        gkf_pointer(tx - 210, ty + 40, 0);
        CHECK(run() && !s.t.dragging);
    }
    /* dragged where it does not fit, it goes back */
    n = s.t.game.moves;
    i = s.t.game.pile[SOL_TABLEAU].n;
    on_card(SOL_FOUNDATION + 3, 0, &px, &py);
    drag(SOL_TABLEAU, i - 1, px, py);
    if(SOL_RANK(s.t.game.pile[SOL_TABLEAU].card[i - 1]) != 1) CHECK(run() && s.t.game.moves == n && s.t.game.pile[SOL_TABLEAU].n == i);
    else CHECK(run());
    CHECK(!s.t.dragging && s.drag_w == 0);
    /* Ctrl+Z and the menu's Undo take moves back */
    n = s.t.game.moves;
    gkf_key(26, WIN_MOD_CTRL);
    CHECK(run() && s.t.game.moves == n + 1);
    /* drawing three: the toolbar's radio button deals anew, and a deal fans three */
    {
        int rx, ry;
        CHECK(gkf_find("Draw three", GW_INK, &rx, &ry));
        gkf_advance(600);
        gkf_click(rx - 6, ry + 6);
    }
    CHECK(run() && s.t.draw == 3 && s.t.game.draw == 3 && !s.t.game.started);
    CHECK(gkf_menu_entry(gkf_menu_find("Draw three"))->flags & WIN_MENU_RADIO);
    gkf_key(' ', 0);
    CHECK(run() && s.t.game.pile[SOL_WASTE].n == 3);
    card_xy(SOL_WASTE, 2, &x, &y);
    card_xy(SOL_WASTE, 0, &px, &py);
    CHECK(x == px + 2 * TB_FAN && gkf_pixel(x + s.t.cw - 4, y + 30) == C_FACE && gkf_pixel(px + 4, py + 30) == C_FACE);
    CHECK(gkf_menu_choose(MENU_ONE) && run() && s.t.draw == 1);
    /* the keyboard's card: framed while the table has the keys */
    gkf_key(WIN_KEY_DOWN, 0);
    gkf_key(WIN_KEY_DOWN, 0);
    CHECK(run() && s.t.cursor && s.t.cur_pile == SOL_TABLEAU);
    card_xy(SOL_TABLEAU, 0, &x, &y);
    CHECK(gkf_pixel(x - 2, y + 20) == C_CURSOR);
    gkf_focus(0);
    CHECK(run() && gkf_pixel(x - 2, y + 20) == C_TABLE);
    gkf_focus(1);
    CHECK(run() && gkf_pixel(x - 2, y + 20) == C_CURSOR);
    gkf_key(WIN_KEY_RIGHT, WIN_MOD_REPEAT);             /* an arrow held moves on */
    CHECK(run() && s.t.cur_pile == SOL_TABLEAU + 1);
    gkf_key(' ', WIN_MOD_REPEAT);                       /* Space held: one deal, not more */
    n = s.t.game.pile[SOL_WASTE].n;
    CHECK(run() && s.t.game.pile[SOL_WASTE].n == n);
    /* Esc during a game asks, Cancel first; Close from the box ends the window */
    gkf_key(' ', 0);
    gkf_key(27, 0);
    CHECK(run() && gk_box_open(s.ui) && find_text("Close Solitaire?", GW_PAPER));
    gkf_key('\r', 0);
    CHECK(run() && !gk_box_open(s.ui) && !s.t.asking);
    /* a game that finishes by itself: the ten of hearts sent home turns the last card up, then a card a tick */
    nearly();
    s.t.played[0] = 1;
    s.shown[0] = 0;
    gk_redraw(s.table);
    run();
    CHECK(!sol_finishing(&s.t.game));
    click_card(SOL_TABLEAU + 4, 1);                     /* the 10 of hearts home: the jack of spades turns up */
    CHECK(run() && s.t.finishing && status_says("Every card is up: the rest go home"));
    CHECK(!gk_enabled(s.undo));
    for(i = 0; i < 40 && !s.t.game.won; i++) {
        gkf_advance(TB_FINISH_MS);
        run();
    }
    CHECK(s.t.game.won && !s.t.finishing && s.t.won[0] == 1 && sol_home(&s.t.game) == 52 && i == 15);
    {
        char t[16], want[64];
        table_time(s.t.best_ms[0], t, sizeof(t));
        snprintf(want, sizeof(want), "Won in %s with %d moves: the best yet", t, s.t.game.moves);
        CHECK(status_says(want));
        snprintf(want, sizeof(want), "Won 1 of 1, best %s", t);
        CHECK(find_text(want, GW_MUTED));
    }
    card_xy(SOL_FOUNDATION + 1, 12, &x, &y);
    CHECK(gkf_pixel(x + s.t.cw - 4, y + 30) == C_FACE);
    /* Help; then the close button with nothing under way closes at once */
    CHECK(gkf_menu_choose(MENU_HELP) && run() && gk_asking(s.ui) == BOX_HELP);
    gkf_key(27, 0);
    run();
    gkf_close();
    CHECK(!run() && closed);
    sw_close(&s);
    CHECK(!gkf_open());
    /* during a game the close button asks; Close ends it */
    gkf_reset(0, 0);
    closed = 0;
    CHECK(sw_open(&s, 5) == 0);
    gkf_focus(1);
    run();
    click_card(SOL_STOCK, 23);
    run();
    gkf_close();
    CHECK(run() && gk_box_open(s.ui));
    gkf_key(WIN_KEY_RIGHT, 0);                          /* to Close */
    gkf_key('\r', 0);
    CHECK(!run() && closed);
    sw_close(&s);
    /* the smallest desktop: the window as tall as it allows, the cards smaller, every pile within */
    gkf_reset(638, 317);
    CHECK(sw_open(&s, 6) == 0 && gkf_w() == TB_W && gkf_h() == 317);
    gkf_focus(1);
    CHECK(run() && s.t.cw < TB_CARD_W && s.t.cw >= TB_CARD_MIN);
    card_xy(SOL_TABLEAU + 6, 6, &x, &y);
    CHECK(y + s.t.ch <= 317 - GK_STATUS_H && x + s.t.cw <= TB_W && gkf_pixel(x + s.t.cw - 4, y + s.t.ch / 2) == C_FACE);
    sw_close(&s);
    gkf_reset(0, 0);
    printf("test_wsolitaire_window: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
