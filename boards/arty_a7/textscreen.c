/*
SurfOS board: SurfCPU on the Arty A7 -- the text screen's logic
--------------------
File: textscreen.c  Date: 10/15/26 (SurfCPU)
--------------------
See textscreen.h. Pure code: the cells it reads are RAM, the cells it writes are whatever
s->ram points at (text RAM on the board, an array in the host test).
*/

#include "textscreen.h"

void textscreen_clear(struct textscreen *s, u8 attr) {
    u_int i;
    for(i = 0; i < TS_COLS * TS_ROWS; i++) s->ram[i] = textscreen_cell(' ', attr);
    s->scroll = 0;
    if(s->set_scroll) s->set_scroll(0);
}

void textscreen_put(struct textscreen *s, u_int col, u_int row, u8 ch, u8 attr) {
    if(col >= TS_COLS || row >= TS_ROWS) return;
    *textscreen_at(s, row, col) = textscreen_cell(ch, attr);
    s->writes++;
}

/* One pass over the new picture: is it the screen as it is (nothing to do), or the screen
   moved up one row (new row r is shown row r + 1, for every row but the last)? Only for a
   whole screen: a smaller grid leaves rows the console does not own. Ends as soon as both
   answers are no. */
static void compare(struct textscreen *s, const u8 *cells, u_int cols, u_int rows, bool *same, bool *shifted) {
    u_int r, c;
    *same = true;
    *shifted = rows == TS_ROWS && cols == TS_COLS;
    for(r = 0; r < rows && (*same || *shifted); r++) {
        const u8 *src = cells + r * cols * 2;
        for(c = 0; c < cols && (*same || *shifted); c++) {
            u32 want = textscreen_cell(src[2 * c], src[2 * c + 1]);
            if(*same && *textscreen_at(s, r, c) != want) *same = false;
            if(*shifted && r + 1 < rows && *textscreen_at(s, r + 1, c) != want) *shifted = false;
        }
    }
}

void textscreen_present(struct textscreen *s, const u8 *cells, u_int cols, u_int rows) {
    u_int r, c;
    bool same, shifted;
    if(cols > TS_COLS) cols = TS_COLS;
    if(rows > TS_ROWS) rows = TS_ROWS;
    compare(s, cells, cols, rows, &same, &shifted);
    if(same) return;
    if(shifted) {                                   /* the old top row becomes the new bottom row: */
        s->scroll = (s->scroll + 1) % TS_ROWS;      /* written first, then SCROLL moves on (IO.md's */
        s->scrolls++;                               /* order; the register is taken once per frame) */
    }
    for(r = shifted ? rows - 1 : 0; r < rows; r++) {
        const u8 *src = cells + r * cols * 2;
        for(c = 0; c < cols; c++) {
            volatile u32 *p = textscreen_at(s, r, c);
            u32 want = textscreen_cell(src[2 * c], src[2 * c + 1]);
            if(*p != want) {
                *p = want;
                s->writes++;
            }
        }
    }
    if(shifted && s->set_scroll) s->set_scroll(s->scroll);
}
