/*
SurfOS board: SurfCPU on the Arty A7 -- the text screen's logic
----------------------
File: textscreen.h  Date: 10/15/26 (SurfCPU)
----------------------
What boards/arty_a7/textcon.c does with the console's cells, kept free of hardware so it
is tested on a host (boards/arty_a7/tests/test_arty.c). surf_video (MEMORY-MAP.md) shows
80x30 cells of 32 bits: glyph 7:0, foreground 11:8, background 15:12 (palette indices),
and display row r shows text row (r + SCROLL) mod 30. The console (kernel/console.c) keeps
VGA cells: a character byte and an attribute byte (foreground 3:0, background 6:4, bit 7
blink, not drawn, as the framebuffer console does).

textscreen_present() makes the screen show a whole grid of console cells, writing only
the cells that differ. When the new picture is the old one moved up by a row (the
console scrolled), it moves SCROLL on by one instead and rewrites only the last row:
the hardware scroll.
*/

#ifndef _TEXTSCREEN_H
#define _TEXTSCREEN_H

#include <surfos/types.h>

#define TS_COLS 80
#define TS_ROWS 30

struct textscreen {
    volatile u32 *ram;              /* cells 0..2399 of text RAM */
    u_int scroll;                   /* the SCROLL register's value */
    void (*set_scroll)(u_int row);  /* writes the register */
    u_long writes, scrolls;         /* statistics (and for the tests) */
};

static inline u32 textscreen_cell(u8 ch, u8 attr) {
    return (u32)ch | ((u32)(attr & 0x0F) << 8) | ((u32)((attr >> 4) & 0x07) << 12);
}

/* the text RAM cell that display row, column shows now */
static inline volatile u32 *textscreen_at(struct textscreen *s, u_int row, u_int col) {
    return s->ram + ((row + s->scroll) % TS_ROWS) * TS_COLS + col;
}

void textscreen_clear(struct textscreen *s, u8 attr);
void textscreen_put(struct textscreen *s, u_int col, u_int row, u8 ch, u8 attr);
/* cells: cols x rows x 2 (character, attribute), rows <= TS_ROWS, cols <= TS_COLS */
void textscreen_present(struct textscreen *s, const u8 *cells, u_int cols, u_int rows);

#endif
