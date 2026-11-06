/* Host tests for Solitaire (user/wsolitaire/: klondike.c, its rules, and table.c, its window without the drawing;
   11/05/26, the desktop applications, track games), built as they are with the host's C library. The deal: every
   card once, the piles one to seven with their tops up, the stock, a seed repeating its deal. The stock: one or three
   at a time, the waste taken back in its order, as often as wanted. The moves: alternate colours down, kings on empty
   piles, runs, foundations up by suit, back down from a foundation, a face-down top turning up, and what is refused.
   Undo: every kind, the card turned up turned down again, the ring's limit, and thousands of random moves undone back
   to the deal. Where a click sends a card. Finishing by itself: offered when it will win, played out to the win, and
   not offered when three at a time hides the card it needs. Random and greedy games keeping the 52 cards and the
   rules. The window's model: the layout at three sizes, fans that keep a long pile above the status bar, the card
   under the pointer, clicks, drags let go over a pile or back, the keyboard, the finish a card a tick, closing during
   a game, the session's scores. The drawing is checked on the screen (tools/gui_apps_games.py). Built and run by
   `make unittest`. */
#include <stdio.h>
#include <string.h>
#include "klondike.h"
#include "table.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/* a card as written in these tests: rank (A 2-9 T J Q K) and suit (S H C D), the suit's capital meaning face up */
static unsigned char card(const char *s) {
    static const char ranks[] = "A23456789TJQK", suits[] = "SHCD", lower[] = "shcd";
    int r = (int)(strchr(ranks, s[0] >= 'a' ? s[0] - 'a' + 'A' : s[0]) - ranks), u;
    const char *up = strchr(suits, s[1]);
    u = up ? (int)(up - suits) : (int)(strchr(lower, s[1]) - lower);
    return (unsigned char)((u * 13 + r) | (up ? SOL_UP : 0));
}

/* a pile set by hand: its cards bottom first, "KS QH" */
static void pile_is(struct klondike *k, int p, const char *cards) {
    k->pile[p].n = 0;
    for(; *cards; cards += cards[2] ? 3 : 2) k->pile[p].card[k->pile[p].n++] = card(cards);
}

/* an empty table with the game under way: the tests lay out what they need */
static void bare(struct klondike *k, int draw) {
    memset(k, 0, sizeof(*k));
    k->draw = draw;
    k->started = 1;
}

static int same_piles(const struct klondike *a, const struct klondike *b) {
    int p;
    for(p = 0; p < SOL_PILES; p++)
        if(a->pile[p].n != b->pile[p].n || memcmp(a->pile[p].card, b->pile[p].card, (size_t)a->pile[p].n)) return 0;
    return 1;
}

/* the 52 cards are each somewhere once, and every pile is as the rules make it */
static int valid(const struct klondike *k) {
    int seen[SOL_CARDS] = { 0 }, p, i, n = 0, up;
    for(p = 0; p < SOL_PILES; p++) {
        const struct sol_pile *q = &k->pile[p];
        if(q->n < 0 || q->n > SOL_CARDS) return 0;
        for(i = 0, up = 0; i < q->n; i++) {
            unsigned char c = q->card[i];
            if(SOL_CARD(c) >= SOL_CARDS || seen[SOL_CARD(c)]++ || (c & ~(SOL_UP | 0x3F))) return 0;
            n++;
            if(p == SOL_STOCK && (c & SOL_UP)) return 0;
            if(p == SOL_WASTE && !(c & SOL_UP)) return 0;
            if(SOL_IS_FOUNDATION(p) && (!(c & SOL_UP) || SOL_RANK(c) != i + 1 || SOL_SUIT(c) != SOL_SUIT(q->card[0])))
                return 0;
            if(SOL_IS_TABLEAU(p)) {
                if(up && !(c & SOL_UP)) return 0;               /* nothing face down above a face-up card */
                if(up && (SOL_RANK(c) + 1 != SOL_RANK(q->card[i - 1]) || SOL_RED(c) == SOL_RED(q->card[i - 1]))) return 0;
                up = c & SOL_UP;
            }
        }
        if(SOL_IS_TABLEAU(p) && q->n && !(q->card[q->n - 1] & SOL_UP)) return 0;   /* a top is always up */
    }
    return n == SOL_CARDS;
}

static void deal(void) {
    struct klondike a, b;
    int p, i, ok = 1, differ = 0;
    sol_new(&a, 1, 12345);
    CHECK(valid(&a) && a.draw == 1 && !a.started && a.moves == 0);
    for(p = 0; p < 7; p++) {
        const struct sol_pile *q = &a.pile[SOL_TABLEAU + p];
        CHECK(q->n == p + 1);
        for(i = 0; i < q->n; i++) if(!!(q->card[i] & SOL_UP) != (i == q->n - 1)) ok = 0;
    }
    CHECK(ok && a.pile[SOL_STOCK].n == 24 && a.pile[SOL_WASTE].n == 0 && sol_home(&a) == 0 && sol_face_down(&a) == 21);
    sol_new(&b, 3, 12345);
    CHECK(b.draw == 3 && same_piles(&a, &b));           /* the seed is the deal, whatever the draw */
    sol_new(&b, 7, 12345);
    CHECK(b.draw == 1);
    for(i = 0; i < 100; i++) {
        sol_new(&b, 1, (unsigned)i);
        if(!valid(&b)) ok = 0;
        differ += !same_piles(&a, &b);
    }
    CHECK(ok && differ == 100);
    /* every card comes to every place about as often: the shuffle is even */
    {
        static int where[SOL_CARDS][SOL_CARDS];
        int s, lo = 1 << 30, hi = 0, c, n;
        for(s = 0; s < 52000; s++) {
            sol_new(&b, 1, (unsigned)s * 2654435761u);
            for(p = n = 0; p < SOL_PILES; p++)
                for(i = 0; i < b.pile[p].n; i++) where[SOL_CARD(b.pile[p].card[i])][n++]++;
        }
        for(c = 0; c < SOL_CARDS; c++) for(n = 0; n < SOL_CARDS; n++) {
            if(where[c][n] < lo) lo = where[c][n];
            if(where[c][n] > hi) hi = where[c][n];
        }
        CHECK(lo > 850 && hi < 1150);                   /* 1000 each, give or take five deviations */
    }
}

static void stock(void) {
    struct klondike k, start;
    unsigned char first[24];
    int i, n;
    sol_new(&k, 1, 99);
    start = k;
    for(i = 0; i < 24; i++) first[i] = SOL_CARD(k.pile[SOL_STOCK].card[23 - i]);  /* the order they come in */
    for(i = 0; i < 24; i++) {
        CHECK(sol_deal(&k, 100) == 1);
        if(SOL_CARD(k.pile[SOL_WASTE].card[k.pile[SOL_WASTE].n - 1]) != first[i]) break;
    }
    CHECK(i == 24 && k.pile[SOL_STOCK].n == 0 && k.pile[SOL_WASTE].n == 24 && valid(&k));
    CHECK(k.started && k.start == 100 && k.moves == 24);
    CHECK(sol_deal(&k, 200) == 1 && k.turns == 1 && k.pile[SOL_WASTE].n == 0 && valid(&k));   /* taken back */
    CHECK(sol_deal(&k, 200) == 1 && SOL_CARD(k.pile[SOL_WASTE].card[0]) == first[0]);         /* in its order */
    while(k.pile[SOL_STOCK].n) sol_deal(&k, 300);
    for(i = 0; i < 49; i++) sol_undo(&k, 400);          /* back through the deals and the turn, to the deal */
    CHECK(same_piles(&k, &start) && k.turns == 0 && k.moves == 2 * 49 && !sol_can_undo(&k));
    /* three at a time: 8 deals use the stock up; the top of the waste is each third card */
    sol_new(&k, 3, 99);
    for(n = 0; k.pile[SOL_STOCK].n; n++) {
        sol_deal(&k, 1);
        CHECK(SOL_CARD(k.pile[SOL_WASTE].card[k.pile[SOL_WASTE].n - 1]) == first[3 * n + 2]);
    }
    CHECK(n == 8 && valid(&k));
    pile_is(&k, SOL_STOCK, "5h 9c");                    /* two left: two come */
    pile_is(&k, SOL_WASTE, "");
    k.pile[SOL_TABLEAU].n = 0;                          /* (the cards' count is not this test's) */
    CHECK(sol_deal(&k, 1) && k.pile[SOL_WASTE].n == 2 && SOL_CARD(k.pile[SOL_WASTE].card[1]) == SOL_CARD(card("5H")));
    CHECK(sol_deal(&k, 1) && k.pile[SOL_STOCK].n == 2 && SOL_CARD(k.pile[SOL_STOCK].card[1]) == SOL_CARD(card("9C")));
    bare(&k, 1);
    CHECK(sol_deal(&k, 1) == 0);                        /* both empty: nothing */
}

static void moves(void) {
    struct klondike k;
    bare(&k, 1);
    pile_is(&k, SOL_TABLEAU + 0, "2s 9c 7H");
    pile_is(&k, SOL_TABLEAU + 1, "6S");
    pile_is(&k, SOL_TABLEAU + 2, "6H");
    pile_is(&k, SOL_TABLEAU + 3, "");
    pile_is(&k, SOL_TABLEAU + 4, "4d KD QC JH");
    pile_is(&k, SOL_WASTE, "AH");
    pile_is(&k, SOL_FOUNDATION + 0, "AS");
    CHECK(sol_can_put(&k, SOL_TABLEAU + 1, 0, SOL_TABLEAU + 0));      /* black 6 on red 7 */
    CHECK(!sol_can_put(&k, SOL_TABLEAU + 2, 0, SOL_TABLEAU + 0));     /* red on red */
    CHECK(!sol_can_put(&k, SOL_TABLEAU + 0, 2, SOL_TABLEAU + 1));     /* 7 on 6: up, not down */
    CHECK(!sol_can_put(&k, SOL_TABLEAU + 1, 0, SOL_TABLEAU + 3));     /* only a king on an empty pile */
    CHECK(!sol_can_take(&k, SOL_TABLEAU + 0, 1) && !sol_can_take(&k, SOL_TABLEAU + 0, 3));    /* face down; none */
    CHECK(!sol_can_take(&k, SOL_STOCK, 0) && !sol_can_take(&k, SOL_TABLEAU + 0, -1));
    CHECK(sol_move(&k, SOL_TABLEAU + 1, 0, SOL_TABLEAU + 0, 50) == 1 && k.pile[SOL_TABLEAU + 1].n == 0);
    CHECK(k.moves == 1 && k.pile[SOL_TABLEAU].n == 4);
    /* a run with a king at its head onto the empty pile; the 4 under it turns up */
    CHECK(sol_move(&k, SOL_TABLEAU + 4, 1, SOL_TABLEAU + 3, 60) == 1 && k.pile[SOL_TABLEAU + 3].n == 3);
    CHECK(k.pile[SOL_TABLEAU + 4].n == 1 && (k.pile[SOL_TABLEAU + 4].card[0] & SOL_UP));
    CHECK(k.undo[(k.undo_top + SOL_UNDO - 1) % SOL_UNDO].flipped == 1);
    /* the foundations: up by suit from the ace */
    CHECK(sol_move(&k, SOL_WASTE, 0, SOL_FOUNDATION + 1, 70) == 1 && sol_home(&k) == 2);
    pile_is(&k, SOL_WASTE, "2H");
    CHECK(!sol_can_put(&k, SOL_WASTE, 0, SOL_FOUNDATION + 0) && sol_can_put(&k, SOL_WASTE, 0, SOL_FOUNDATION + 1));
    CHECK(!sol_can_put(&k, SOL_WASTE, 0, SOL_FOUNDATION + 2));        /* a 2 on nothing */
    CHECK(!sol_can_put(&k, SOL_TABLEAU + 3, 1, SOL_FOUNDATION + 2));  /* two cards at once: never */
    pile_is(&k, SOL_WASTE, "3D 2S");
    CHECK(!sol_can_take(&k, SOL_WASTE, 0) && sol_move(&k, SOL_WASTE, 1, SOL_FOUNDATION + 0, 80));
    /* back down from a foundation: the 2 of spades onto the red 3 */
    pile_is(&k, SOL_TABLEAU + 6, "3H");
    CHECK(sol_can_take(&k, SOL_FOUNDATION + 0, 1) && !sol_can_take(&k, SOL_FOUNDATION + 0, 0));
    CHECK(sol_move(&k, SOL_FOUNDATION + 0, 1, SOL_TABLEAU + 6, 90) == 1 && k.pile[SOL_FOUNDATION].n == 1);
    CHECK(!sol_move(&k, SOL_TABLEAU + 6, 0, SOL_TABLEAU + 6, 90));   /* onto itself */
    CHECK(!sol_move(&k, SOL_TABLEAU + 6, 1, SOL_STOCK, 90) && !sol_move(&k, SOL_TABLEAU + 6, 1, SOL_WASTE, 90));
    CHECK(!sol_move(&k, SOL_TABLEAU + 6, 1, -1, 90) && !sol_move(&k, 99, 0, SOL_TABLEAU, 90));
}

static void undo(void) {
    struct klondike k, start;
    unsigned r = sol_seed(5);
    int i, p, q, n, made = 0, ok = 1;
    static struct klondike seen[400];
    bare(&k, 1);
    pile_is(&k, SOL_TABLEAU + 0, "9h 4S");
    pile_is(&k, SOL_TABLEAU + 1, "5D");
    start = k;
    sol_move(&k, SOL_TABLEAU + 0, 1, SOL_TABLEAU + 1, 1);
    CHECK((k.pile[SOL_TABLEAU].card[0] & SOL_UP) && sol_can_undo(&k));
    CHECK(sol_undo(&k, 2) == 1 && same_piles(&k, &start) && !(k.pile[SOL_TABLEAU].card[0] & SOL_UP));
    CHECK(sol_undo(&k, 2) == 0 && k.moves == 2);        /* nothing more; the undo counted */
    /* random moves from a deal, each position kept; undone one by one, each comes back */
    sol_new(&k, 3, 777);
    for(i = 0; i < 400; i++) {
        seen[i] = k;
        for(n = 0; n < 200; n++) {                      /* a random move that is allowed, or a deal */
            p = (int)sol_below(&r, SOL_PILES);
            q = (int)sol_below(&r, SOL_PILES);
            if(p == SOL_STOCK) { if(sol_deal(&k, (unsigned)i)) break; continue; }
            if(!k.pile[p].n) continue;
            if(sol_move(&k, p, (int)sol_below(&r, (unsigned)k.pile[p].n), q, (unsigned)i)) break;
        }
        if(n == 200 && !sol_deal(&k, (unsigned)i)) break;
        made++;
        if(!valid(&k)) ok = 0;
    }
    CHECK(ok && made > 300);
    for(i = made - 1; i >= 0; i--) {
        sol_undo(&k, 0);
        if(!same_piles(&k, &seen[i]) || k.turns != seen[i].turns) ok = 0;
    }
    CHECK(ok && !sol_can_undo(&k));
    /* the ring keeps the newest SOL_UNDO moves */
    sol_new(&k, 1, 3);
    for(i = 0; i < SOL_UNDO + 100; i++) sol_deal(&k, 1);
    for(n = 0; sol_undo(&k, 1); n++) ;
    CHECK(n == SOL_UNDO);
}

static void targets(void) {
    struct klondike k;
    bare(&k, 1);
    pile_is(&k, SOL_TABLEAU + 0, "8S");
    pile_is(&k, SOL_TABLEAU + 1, "KH");
    pile_is(&k, SOL_TABLEAU + 2, "8C");
    pile_is(&k, SOL_TABLEAU + 3, "");
    pile_is(&k, SOL_TABLEAU + 4, "5s 7H");
    pile_is(&k, SOL_TABLEAU + 5, "AD");
    pile_is(&k, SOL_TABLEAU + 6, "4d 2c QS");
    pile_is(&k, SOL_WASTE, "KC");
    CHECK(sol_target(&k, SOL_TABLEAU + 5, 0) == SOL_FOUNDATION);     /* an ace: home first */
    CHECK(sol_target(&k, SOL_TABLEAU + 4, 1) == SOL_TABLEAU + 0);     /* the 7 of hearts: right of its pile, round */
    pile_is(&k, SOL_TABLEAU + 5, "");
    CHECK(sol_target(&k, SOL_TABLEAU + 4, 1) == SOL_TABLEAU + 0);
    pile_is(&k, SOL_TABLEAU + 5, "8C");
    CHECK(sol_target(&k, SOL_TABLEAU + 4, 1) == SOL_TABLEAU + 5);     /* the next pile it fits, looking right */
    CHECK(sol_target(&k, SOL_TABLEAU + 1, 0) == -1);                  /* a king heading its pile stays */
    CHECK(sol_target(&k, SOL_WASTE, 0) == SOL_TABLEAU + 3);           /* a king from the waste: the empty pile */
    CHECK(sol_target(&k, SOL_TABLEAU + 6, 2) == SOL_TABLEAU + 1);     /* the queen on the red king */
    CHECK(sol_target(&k, SOL_TABLEAU + 6, 0) == -1 && sol_target(&k, SOL_STOCK, 0) == -1);
    pile_is(&k, SOL_FOUNDATION + 2, "AS 2S 3S 4S 5S 6S 7S");
    pile_is(&k, SOL_TABLEAU + 0, "8D");
    CHECK(sol_target(&k, SOL_FOUNDATION + 2, 6) == SOL_TABLEAU + 0);  /* down from a foundation, never back up */
}

/* a table where every card is up: the foundations hold all but the cards named, which lie as given */
static void nearly(struct klondike *k, int draw, const char *const *piles) {
    int f, r, p, i, c, out[SOL_CARDS] = { 0 };
    bare(k, draw);
    for(p = 0; p < SOL_PILES; p++) if(piles[p]) {
        pile_is(k, p, piles[p]);
        for(i = 0; i < k->pile[p].n; i++) out[SOL_CARD(k->pile[p].card[i])] = 1;
    }
    for(f = 0; f < 4; f++) {                            /* each suit's foundation up to its lowest card left out */
        struct sol_pile *q = &k->pile[SOL_FOUNDATION + f];
        q->n = 0;
        for(r = 1; r <= 13; r++) {
            c = f * 13 + r - 1;
            if(out[c]) break;
            q->card[q->n++] = (unsigned char)(c | SOL_UP);
        }
    }
}

static void finishing(void) {
    static const char *const easy[SOL_PILES] = {
        [SOL_TABLEAU + 0] = "KH QS JH", [SOL_TABLEAU + 1] = "KS QH JS", [SOL_TABLEAU + 2] = "KD QC JD TC",
        [SOL_TABLEAU + 3] = "KC QD JC TD", [SOL_TABLEAU + 4] = "TS", [SOL_TABLEAU + 6] = "TH",
    };
    static const char *const clubs[SOL_PILES] = {        /* every club in the stock, the ace first of a group of three */
        [SOL_STOCK] = "kc qc jc tc 9c 8c 7c 6c 5c 4c 3c 2c ac",
    };
    struct klondike k;
    int steps;
    nearly(&k, 1, easy);
    CHECK(valid(&k) && sol_home(&k) == 52 - 16);
    k.start = 1000;
    CHECK(sol_finishing(&k));
    for(steps = 0; sol_finish_step(&k, 1000 + (unsigned)steps * 60); steps++) ;
    CHECK(k.won && steps == 16 && sol_home(&k) == 52 && k.end == 1000 + 15 * 60 && valid(&k));
    CHECK(!sol_finishing(&k) && !sol_can_undo(&k) && sol_finish_step(&k, 1) == 0 && !sol_can_take(&k, SOL_FOUNDATION, 12));
    CHECK(sol_ms(&k, 999999) == 15 * 60);
    /* a face-down card: not yet */
    nearly(&k, 1, easy);
    k.pile[SOL_TABLEAU + 0].card[0] &= (unsigned char)~SOL_UP;
    CHECK(!sol_finishing(&k));
    /* the clubs in the stock: one at a time a pass finds each; three at a time the ace never comes up */
    nearly(&k, 1, clubs);
    CHECK(valid(&k) && sol_finishing(&k));
    for(steps = 0; steps < 10000 && sol_finish_step(&k, 5); steps++) ;
    CHECK(k.won);
    nearly(&k, 3, clubs);
    CHECK(!sol_finishing(&k));
    k.draw = 1;                                         /* (the same cards, one at a time) */
    CHECK(sol_finishing(&k));
    /* not started: a game nobody has moved in is not finished for them */
    nearly(&k, 1, easy);
    k.started = 0;
    CHECK(!sol_finishing(&k));
}

/* random and greedy games: the rules always hold, greedy play wins some, and finishing by itself ends them */
static void whole_games(void) {
    struct klondike k;
    unsigned r = sol_seed(31);
    int g, n, p, i, q, ok = 1, won = 0, finished = 0, moved;
    for(g = 0; g < 400; g++) {
        sol_new(&k, g % 2 ? 3 : 1, (unsigned)g);
        for(n = 0; n < 3000 && !k.won; n++) {
            if(sol_finishing(&k)) {
                while(sol_finish_step(&k, (unsigned)n)) ;
                finished++;
                break;
            }
            moved = 0;
            for(p = SOL_WASTE; p < SOL_PILES && !moved; p++)        /* greedy: a card home */
                if(k.pile[p].n && !SOL_IS_FOUNDATION(p))
                    for(q = SOL_FOUNDATION; q < SOL_TABLEAU && !moved; q++) moved = sol_move(&k, p, k.pile[p].n - 1, q, (unsigned)n);
            for(p = SOL_TABLEAU; p < SOL_PILES && !moved; p++) {    /* a run that turns a card up, or a king freed */
                for(i = 0; i < k.pile[p].n && !(k.pile[p].card[i] & SOL_UP); i++) ;
                if(i == 0 || i >= k.pile[p].n) continue;
                for(q = SOL_TABLEAU; q < SOL_PILES && !moved; q++) moved = sol_move(&k, p, i, q, (unsigned)n);
            }
            if(!moved && k.pile[SOL_WASTE].n)                       /* the waste onto the tableau */
                for(q = SOL_TABLEAU; q < SOL_PILES && !moved; q++) moved = sol_move(&k, SOL_WASTE, k.pile[SOL_WASTE].n - 1, q, (unsigned)n);
            if(!moved && sol_below(&r, 4) == 0) {                   /* now and then something random */
                p = (int)sol_below(&r, SOL_PILES);
                if(k.pile[p].n) moved = sol_move(&k, p, (int)sol_below(&r, (unsigned)k.pile[p].n), (int)sol_below(&r, SOL_PILES), (unsigned)n);
            }
            if(!moved && !sol_deal(&k, (unsigned)n)) break;
            if(!valid(&k)) ok = 0;
        }
        won += k.won;
        if(!valid(&k)) ok = 0;
    }
    if(won < 20 || !finished) printf("  whole games: %d won, %d finished by themselves\n", won, finished);
    CHECK(ok && won >= 20 && finished >= 20);
}

/**** the window's model ****/

static struct table t;

static void middle(int pile, int index, int *x, int *y) {
    table_card_xy(&t, pile, index, x, y);
    *x += t.cw / 2;
    *y += index >= 0 && SOL_IS_TABLEAU(pile) && index < t.game.pile[pile].n - 1 ? 3 : t.ch / 2;  /* its visible top */
}

static int click(int pile, int index, unsigned now) {
    int x, y;
    middle(pile, index, &x, &y);
    return table_pointer(&t, x, y, TB_BUTTON_LEFT, TB_BUTTON_LEFT, now) | table_pointer(&t, x, y, 0, 0, now);
}

static void status_is(const char *want) {
    char s[160];
    table_status(&t, s, sizeof(s));
    if(strcmp(s, want)) printf("  status: \"%s\"\n", s);
    CHECK(!strcmp(s, want));
}

static void layout(void) {
    static const int sizes[][2] = { { TB_W, TB_H }, { 958, 457 }, { 638, 317 }, { 400, 300 } };
    int s, p, i, x, y, ok = 1, pile, index;
    table_init(&t, 1);
    CHECK(t.cw == TB_CARD_W && t.ch == 96 && t.draw == 1);
    for(s = 0; s < 4; s++) {
        table_layout(&t, sizes[s][0], sizes[s][1]);
        if(t.cw < TB_CARD_MIN || t.cw > TB_CARD_W) ok = 0;
        if(s < 3 && (t.col[0] < TB_MARGIN || t.col[6] + t.cw > sizes[s][0] - TB_MARGIN)) ok = 0;
        for(p = 0; p < SOL_PILES; p++) for(i = 0; i < t.game.pile[p].n; i++) {
            table_card_xy(&t, p, i, &x, &y);
            if(s < 3 && (y + t.ch > t.bottom || y < t.top_y)) ok = 0;
            middle(p, i, &x, &y);
            if(i == t.game.pile[p].n - 1 && (!table_hit(&t, x, y, &pile, &index) || pile != p || index != i)) ok = 0;
        }
    }
    CHECK(ok);
    table_layout(&t, 638, 317);
    CHECK(t.cw >= TB_CARD_MIN && t.ch == t.cw * 4 / 3);
    /* a pile of six face down and thirteen up: within the window at every size, its top card whole */
    pile_is(&t.game, SOL_TABLEAU + 6, "2c 3c 4c 5c 6c 7c KS QH JS TH 9S 8H 7S 6H 5S 4H 3S 2H AS");
    for(s = 0; s < 3; s++) {
        table_layout(&t, sizes[s][0], sizes[s][1]);
        table_card_xy(&t, SOL_TABLEAU + 6, 18, &x, &y);
        CHECK(y + t.ch <= t.bottom);
    }
    table_layout(&t, TB_W, TB_H);
    {
        int down, up;
        table_fan(&t, SOL_TABLEAU + 6, &down, &up);
        CHECK(down < TB_DOWN && up <= TB_UP && up >= TB_UP_MIN);
        table_fan(&t, SOL_TABLEAU + 0, &down, &up);
        CHECK(down == TB_DOWN && up == TB_UP);
    }
    /* empty places: the stock once used up, a foundation */
    table_init(&t, 1);
    middle(SOL_FOUNDATION + 2, 0, &x, &y);
    CHECK(table_hit(&t, x, y, &pile, &index) && pile == SOL_FOUNDATION + 2 && index == -1);
    CHECK(table_drop(&t, x, y) == SOL_FOUNDATION + 2 && table_drop(&t, 1, 1) == -1);
    middle(SOL_TABLEAU + 3, 0, &x, &y);
    CHECK(table_drop(&t, x, t.bottom - 2) == SOL_TABLEAU + 3);    /* a column's whole height takes a drop */
    CHECK(!table_hit(&t, t.col[2] + 5, t.top_y + 5, &pile, &index));   /* the gap in the top row */
}

static void pointer(void) {
    int x, y, pile, index, n;
    char s[160];
    table_init(&t, 4242);
    status_is("Drag a card, or click it; Space deals");
    /* a click on the stock deals; on the empty stock it takes the waste back */
    CHECK(click(SOL_STOCK, t.game.pile[SOL_STOCK].n - 1, 1000) == TB_DRAW && t.game.pile[SOL_WASTE].n == 1);
    CHECK(t.played[0] == 1 && t.game.started && t.game.start == 1000);
    while(t.game.pile[SOL_STOCK].n) click(SOL_STOCK, t.game.pile[SOL_STOCK].n - 1, 1100);
    CHECK(click(SOL_STOCK, -1, 1200) == TB_DRAW && t.game.pile[SOL_STOCK].n == 24 && t.game.turns == 1);
    /* a card that has nowhere to go says so */
    table_new(&t, 1);
    bare(&t.game, 1);
    pile_is(&t.game, SOL_TABLEAU + 0, "9h 7S");
    pile_is(&t.game, SOL_TABLEAU + 1, "8D");
    pile_is(&t.game, SOL_TABLEAU + 2, "5c 9C");
    pile_is(&t.game, SOL_TABLEAU + 3, "");
    pile_is(&t.game, SOL_TABLEAU + 4, "6H");
    click(SOL_TABLEAU + 2, 1, 2000);
    status_is("The 9 of clubs has nowhere to go");
    CHECK(t.game.pile[SOL_TABLEAU + 2].n == 2);
    /* a click sends the 7 onto the 8; the 9 under it turns up */
    CHECK(click(SOL_TABLEAU + 0, 1, 2100) == TB_DRAW && t.game.pile[SOL_TABLEAU + 1].n == 2);
    CHECK(t.game.pile[SOL_TABLEAU].n == 1 && (t.game.pile[SOL_TABLEAU].card[0] & SOL_UP));
    /* a drag: the 6 of hearts held, moved, carried, and let go over the 7: it moves */
    middle(SOL_TABLEAU + 4, 0, &x, &y);
    CHECK(table_pointer(&t, x, y, TB_BUTTON_LEFT, TB_BUTTON_LEFT, 2200) == TB_DRAW && !t.dragging);
    CHECK(table_pointer(&t, x + 2, y + 1, TB_BUTTON_LEFT, 0, 2200) == 0 && !t.dragging);   /* less than TB_DRAG */
    CHECK(table_pointer(&t, x + 40, y - 60, TB_BUTTON_LEFT, 0, 2200) == TB_DRAW && t.dragging);
    CHECK(table_dragged(&t, &pile, &index, &n) && pile == SOL_TABLEAU + 4 && index == 0 && n == 1);
    middle(SOL_TABLEAU + 1, 1, &x, &y);
    table_pointer(&t, x, y + 5, TB_BUTTON_LEFT, 0, 2300);
    CHECK(table_pointer(&t, x, y + 5, 0, 0, 2300) == TB_DRAW && !t.dragging && t.game.pile[SOL_TABLEAU + 1].n == 3);
    CHECK(!table_dragged(&t, &pile, &index, &n));
    /* dragged nowhere it fits, it goes back */
    middle(SOL_TABLEAU + 2, 1, &x, &y);
    table_pointer(&t, x, y, TB_BUTTON_LEFT, TB_BUTTON_LEFT, 2400);
    table_pointer(&t, x + 100, y + 100, TB_BUTTON_LEFT, 0, 2400);
    middle(SOL_TABLEAU + 0, 0, &n, &index);             /* over the red 9: no */
    table_pointer(&t, n, index, TB_BUTTON_LEFT, 0, 2400);
    table_pointer(&t, n, index, 0, 0, 2400);
    CHECK(t.game.pile[SOL_TABLEAU + 2].n == 2 && t.game.pile[SOL_TABLEAU].n == 1);
    /* the run from the 8 dragged by its 8, let go with the pointer off any pile but the card's middle over the 9: it
       goes there, by the card's middle */
    pile_is(&t.game, SOL_TABLEAU + 5, "9S");
    middle(SOL_TABLEAU + 1, 0, &x, &y);
    table_pointer(&t, x, y, TB_BUTTON_LEFT, TB_BUTTON_LEFT, 2500);
    middle(SOL_TABLEAU + 5, 0, &n, &index);
    table_pointer(&t, n, index - t.ch / 2 + 3 - 2 * TB_MARGIN, TB_BUTTON_LEFT, 0, 2500);
    table_pointer(&t, n, index - t.ch / 2 + 3 - 2 * TB_MARGIN, 0, 0, 2500);
    CHECK(t.game.pile[SOL_TABLEAU + 5].n == 4 && t.game.pile[SOL_TABLEAU + 1].n == 0);
    /* a press on a face-down card or on nothing holds nothing */
    table_pointer(&t, 1, 1, TB_BUTTON_LEFT, TB_BUTTON_LEFT, 2600);
    CHECK(t.press_pile == -1);
    table_pointer(&t, 1, 1, 0, 0, 2600);
    /* a drag whose release the board never saw (the window lost the focus): it goes back; a move with the button held
       and no press seen here starts none */
    middle(SOL_TABLEAU + 5, 3, &x, &y);
    table_pointer(&t, x, y, TB_BUTTON_LEFT, TB_BUTTON_LEFT, 2650);
    table_pointer(&t, x + 50, y + 50, TB_BUTTON_LEFT, 0, 2650);
    CHECK(t.dragging && table_cancel(&t) == TB_DRAW && !t.dragging && t.press_pile == -1 && table_cancel(&t) == 0);
    CHECK(table_pointer(&t, x + 60, y + 60, TB_BUTTON_LEFT, 0, 2650) == 0 && !t.dragging);
    table_pointer(&t, x + 60, y + 60, 0, 0, 2650);
    CHECK(t.game.pile[SOL_TABLEAU + 5].n == 4);
    table_pointer(&t, x, y, TB_BUTTON_LEFT, TB_BUTTON_LEFT, 2660);
    t.buttons = 0;                                      /* (as if the release had been lost) */
    CHECK(table_pointer(&t, x + 3, y, 0, 0, 2660) == TB_DRAW && t.press_pile == -1);
    /* a press made elsewhere, held as the pointer comes in, is no press here */
    middle(SOL_TABLEAU + 5, 3, &x, &y);
    table_pointer(&t, -1, 5, TB_BUTTON_LEFT, 0, 2700);
    table_pointer(&t, x, y, TB_BUTTON_LEFT, 0, 2700);
    CHECK(t.press_pile == -1 && !t.dragging);
    table_pointer(&t, x, y, 0, 0, 2700);
    table_record(&t, s, sizeof(s));
    CHECK(!strcmp(s, "Won 0 of 1"));
}

static void keyboard_and_finish(void) {
    static const char *const easy[SOL_PILES] = {
        [SOL_TABLEAU + 0] = "KH QS JH", [SOL_TABLEAU + 1] = "KS QH JS", [SOL_TABLEAU + 2] = "KD QC JD TC",
        [SOL_TABLEAU + 3] = "KC QD JC TD", [SOL_TABLEAU + 4] = "ts TH",
    };
    int i;
    char s[160];
    table_init(&t, 9);
    /* the first key shows the keyboard's card: the stock; Down goes to the first pile's top, Right on along */
    CHECK(table_key(&t, TB_DOWNKEY, 0, 0) == TB_DRAW && t.cursor && t.cur_pile == SOL_STOCK);
    table_key(&t, TB_DOWNKEY, 0, 0);
    CHECK(t.cur_pile == SOL_TABLEAU && t.cur_index == 0);
    for(i = 0; i < 9; i++) table_key(&t, TB_RIGHT, 0, 0);
    CHECK(t.cur_pile == SOL_TABLEAU + 6 && t.cur_index == 6);
    table_key(&t, TB_UPKEY, 0, 0);                         /* a lone face-up card: up to the top row's foundation */
    CHECK(t.cur_pile == SOL_FOUNDATION + 3);
    for(i = 0; i < 9; i++) table_key(&t, TB_LEFT, 0, 0);
    CHECK(t.cur_pile == SOL_STOCK);
    table_key(&t, TB_RIGHT, 0, 0);
    table_key(&t, TB_RIGHT, 0, 0);
    CHECK(t.cur_pile == SOL_FOUNDATION);                /* past the gap */
    table_key(&t, TB_LEFT, 0, 0);
    CHECK(t.cur_pile == SOL_WASTE);
    /* Space deals, Enter on the stock deals too, U undoes */
    CHECK(table_key(&t, ' ', 0, 10) == TB_DRAW && t.game.pile[SOL_WASTE].n == 1 && t.game.moves == 1);
    CHECK(table_key(&t, 'u', 0, 20) == TB_DRAW && t.game.pile[SOL_WASTE].n == 0 && t.game.moves == 2);
    CHECK(table_key(&t, 26, 0, 20) == TB_DRAW && !strcmp(t.status, "Nothing to undo"));
    table_key(&t, ' ', 0, 20);
    CHECK(t.game.pile[SOL_WASTE].n == 1 && table_key(&t, 127, 0, 20) == TB_DRAW && t.game.pile[SOL_WASTE].n == 0);  /* Backspace */
    table_key(&t, TB_LEFT, 0, 0);
    CHECK(table_key(&t, '\r', 0, 30) == TB_DRAW && t.game.pile[SOL_WASTE].n == 1);
    /* Enter sends the keyboard's card where it goes: a run by its deepest card chosen with Up */
    table_new(&t, 1);
    bare(&t.game, 1);
    pile_is(&t.game, SOL_TABLEAU + 0, "4c 9S 8H 7C");
    pile_is(&t.game, SOL_TABLEAU + 1, "TD");
    t.cursor = 1;
    t.cur_pile = SOL_TABLEAU;
    t.cur_index = 3;
    table_key(&t, TB_UPKEY, 0, 0);
    table_key(&t, TB_UPKEY, 0, 0);
    CHECK(t.cur_index == 1);
    table_key(&t, TB_UPKEY, 0, 0);                         /* the face-down 4: up to the stock */
    CHECK(t.cur_pile == SOL_STOCK);
    t.cur_pile = SOL_TABLEAU;
    t.cur_index = 1;
    CHECK(table_key(&t, '\n', 0, 40) == TB_DRAW && t.game.pile[SOL_TABLEAU + 1].n == 4 && t.game.pile[SOL_TABLEAU].n == 1);
    CHECK(t.cur_index == 0);                            /* on the pile's new top */
    /* a move that leaves every card up: it finishes by itself, a card a tick, and the session counts the win */
    table_new(&t, 1);
    nearly(&t.game, 1, easy);
    t.game.start = 0;
    t.played[0] = 1;
    CHECK(valid(&t.game) && !sol_finishing(&t.game));   /* the 10 of spades face down: not yet */
    click(SOL_TABLEAU + 4, 1, 1000);                    /* the 10 of hearts home: the 10 of spades turns up */
    CHECK(t.finishing && !t.game.won && sol_home(&t.game) == 52 - 15);
    status_is("Every card is up: the rest go home");
    CHECK(table_key(&t, 'u', 0, 1000) == 0 && click(SOL_TABLEAU, 0, 1000) == 0);  /* it has the keys and the pointer */
    CHECK(table_tick(&t, 1000 + TB_FINISH_MS - 1) == 0);
    for(i = 1; t.finishing && i < 100; i++) table_tick(&t, 1000 + (unsigned)i * TB_FINISH_MS);
    CHECK(t.game.won && i == 16 && t.won[0] == 1 && t.best_ms[0] == 1000 + 15 * TB_FINISH_MS);
    table_time(t.best_ms[0], s, sizeof(s));
    CHECK(!strcmp(s, "0:01"));
    status_is("Won in 0:01 with 16 moves: the best yet");
    table_record(&t, s, sizeof(s));
    CHECK(!strcmp(s, "Won 1 of 1, best 0:01"));
    CHECK(table_undo(&t, 5000) == TB_DRAW && !strcmp(t.status, "The game is won: N deals again"));
    t.status[0] = 0;
    status_is("N deals again; 1 or 3 draws one or three");
    CHECK(table_close(&t) == TB_CLOSE);
    /* a second, slower win: the best stays */
    table_new(&t, 1);
    nearly(&t.game, 1, easy);
    t.game.start = 0;
    t.played[0]++;
    click(SOL_TABLEAU + 4, 1, 200000);
    for(i = 1; t.finishing && i < 100; i++) table_tick(&t, 200000 + (unsigned)i * TB_FINISH_MS);
    status_is("Won in 3:20 with 16 moves (best 0:01, 16)");
    CHECK(t.best_moves[0] == 16);
    /* 3 deals anew drawing three, 1 drawing one; a game under way asks before closing */
    CHECK(table_key(&t, '3', 0, 0) == TB_DRAW && t.draw == 3 && t.game.draw == 3 && !t.game.started);
    CHECK(table_close(&t) == TB_CLOSE);
    table_key(&t, ' ', 0, 5);
    CHECK(t.played[1] == 1 && table_key(&t, 27, 0, 5) == TB_DRAW && t.asking);
    CHECK(table_key(&t, ' ', 0, 5) == 0 && click(SOL_STOCK, t.game.pile[SOL_STOCK].n - 1, 5) == 0);
    CHECK(table_answer(&t, 0) == TB_DRAW && !t.asking && table_key(&t, 23, 0, 5) == TB_DRAW && t.asking);
    CHECK(table_answer(&t, 1) == TB_CLOSE);
    CHECK(table_key(&t, '1', 0, 0) == TB_DRAW && t.draw == 1 && !t.asking);
    table_record(&t, s, sizeof(s));
    CHECK(!strcmp(s, "Won 2 of 2, best 0:01"));         /* drawing one again: its record, not drawing three's */
    t.draw = 3;
    table_record(&t, s, sizeof(s));
    CHECK(!strcmp(s, "Won 0 of 1"));
    t.draw = 1;
    table_time(3723000, s, sizeof(s));
    CHECK(!strcmp(s, "1:02:03"));
    CHECK(!strcmp(table_card_name(card("QH"), s, sizeof(s)), "the queen of hearts"));
    CHECK(!strcmp(table_card_name(card("AS"), s, sizeof(s)), "the ace of spades"));
}

int main(void) {
    deal();
    stock();
    moves();
    undo();
    targets();
    finishing();
    whole_games();
    layout();
    pointer();
    keyboard_and_finish();
    printf("test_wsolitaire: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
