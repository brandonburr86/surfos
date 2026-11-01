/*
SurfOS host test: the text screen
--------------------
File: test_textscreen.c   Date: 10/15/26 (SurfCPU)
--------------------
boards/arty_a7/textscreen.c: the cell format, present() writing only what differs,
the hardware scroll across the 30-row wrap, random changes against what the display shows.
*/

#include <string.h>
#include "harness.h"
#include "textscreen.h"

/**** the text screen ****/

static u32 ram[TS_COLS * TS_ROWS];
static u_int scroll_reg;
static void set_scroll(u_int r) { scroll_reg = r; }

/* what the display shows at display row r, column c: text row (r + SCROLL) mod 30 */
static u32 shown(u_int r, u_int c) {
    return ram[((r + scroll_reg) % TS_ROWS) * TS_COLS + c];
}

static int screen_matches(const u8 *cells, u_int cols, u_int rows) {
    u_int r, c;
    for(r = 0; r < rows; r++)
        for(c = 0; c < cols; c++)
            if(shown(r, c) != textscreen_cell(cells[(r * cols + c) * 2], cells[(r * cols + c) * 2 + 1])) return 0;
    return 1;
}

static void test_textscreen(unsigned seed) {
    (void)seed;
    static u8 cells[TS_COLS * TS_ROWS * 2];
    struct textscreen s = { ram, 0, set_scroll, 0, 0 };
    u_int i, r, round;
    u_long before;

    check(textscreen_cell('A', 0x1E) == (0x41 | 0xE << 8 | 0x1 << 12), "cell: glyph 7:0, foreground 11:8, background 15:12");
    check(textscreen_cell('x', 0xF0) == ('x' | 0x0 << 8 | 0x7 << 12), "cell: the blink bit (7) is not a background colour");
    textscreen_clear(&s, 0x07);
    for(i = 0; i < sizeof(cells); i += 2) { cells[i] = ' '; cells[i + 1] = 0x07; }
    before = s.writes;
    textscreen_present(&s, cells, TS_COLS, TS_ROWS);
    check(s.writes == before && s.scrolls == 0, "present of an unchanged screen writes nothing");
    for(r = 0; r < TS_ROWS; r++) for(i = 0; i < TS_COLS; i++) { cells[(r * TS_COLS + i) * 2] = (u8)('a' + (r + i) % 26); cells[(r * TS_COLS + i) * 2 + 1] = (u8)(r % 16); }
    textscreen_present(&s, cells, TS_COLS, TS_ROWS);
    check(screen_matches(cells, TS_COLS, TS_ROWS), "present: the screen shows the cells");
    for(round = 0; round < 45; round++) {           /* scrolls past the 30-row wrap */
        memmove(cells, cells + TS_COLS * 2, (TS_ROWS - 1) * TS_COLS * 2);
        for(i = 0; i < TS_COLS; i++) { cells[((TS_ROWS - 1) * TS_COLS + i) * 2] = (u8)('0' + (round + i) % 10); cells[((TS_ROWS - 1) * TS_COLS + i) * 2 + 1] = 0x0F; }
        before = s.writes;
        textscreen_present(&s, cells, TS_COLS, TS_ROWS);
        if(!screen_matches(cells, TS_COLS, TS_ROWS) || s.writes - before > TS_COLS) break;
    }
    check(round == 45 && s.scrolls == 45 && scroll_reg == 45 % TS_ROWS,
          "a scrolled console moves SCROLL on and rewrites one row, 45 times across the wrap");
    for(round = 0; round < 200; round++) {          /* random changes, some scrolls, a smaller grid now and then */
        u_int rows = (rnd() % 8 == 0) ? 25 : TS_ROWS, n = rnd() % 50;
        if(rnd() % 3 == 0) memmove(cells, cells + TS_COLS * 2, (TS_ROWS - 1) * TS_COLS * 2);
        for(i = 0; i < n; i++) { u_int k = rnd() % (TS_COLS * TS_ROWS); cells[2 * k] = (u8)rnd(); cells[2 * k + 1] = (u8)rnd(); }
        textscreen_present(&s, cells, TS_COLS, rows);
        if(!screen_matches(cells, TS_COLS, rows)) break;
        if(rnd() % 4 == 0) {                        /* one cell drawn alone, as kernel/console.c does */
            u_int c = rnd() % TS_COLS, rr = rnd() % rows;
            cells[(rr * TS_COLS + c) * 2] = 'Q';
            textscreen_put(&s, c, rr, 'Q', cells[(rr * TS_COLS + c) * 2 + 1]);
            if(!screen_matches(cells, TS_COLS, rows)) break;
        }
    }
    check(round == 200, "200 random rounds of changes, scrolls, single cells and an 80x25 grid: the screen always matches");
}

TEST(test_textscreen)
