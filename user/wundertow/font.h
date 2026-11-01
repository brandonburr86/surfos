/*
SurfOS Undertow's window: the font
----------------------
File: font.h  Date: 10/25/26
----------------------
The console's fonts (driver/font8x16.c, font12x24.c, font16x32.c: code page 437's 256 characters, Spleen, in
libsurf). Every character is one or more cells of them: itself when the console has it, else Undertow's ASCII
stand-in (ut_ascii_for(): "EUR", "--", "...", "?"); combining marks and the zero-width joiners take none.
Layout and painting measure and draw through these calls only, so nothing else knows what a glyph is.

11/01/26 (M3, stage 3.4): sizes are real sizes, never a font drawn at twice its size. A size (WF_S16, WF_S24, WF_S32) is
one of the three fonts, and wf_size() gives the nearest to what a page asks for. A wl_item's "scale" holds a size.
The base size, what text is without anything said of it, is doc.h's wd_base. The window's chrome and form fields
stay in 8x16: WF_W, WF_H and WF_ASCENT are that font's, and wp_text() draws in cells of a size.
*/

#ifndef WUNDERTOW_FONT_H
#define WUNDERTOW_FONT_H

#include <stddef.h>

#define WF_W        8                   /* a cell's width and height at scale 1 */
#define WF_H        16
#define WF_ASCENT   12                  /* rows above the baseline */
#define WF_UNDER    13                  /* the underline's row */
#define WF_CELLS_MAX 8                  /* cells of one character at most (a stand-in) */

enum { WF_S16 = 1, WF_S24, WF_S32 };    /* the sizes: their fonts' heights in pixels */

extern const unsigned char font8x16[256][16];
extern const unsigned short font12x24[256][24], font16x32[256][32];

/* The cells (code page 437 indexes) that show the character cp: how many, in out (WF_CELLS_MAX room) */
int wf_cells(unsigned int cp, unsigned char *out);

/* The cells of n bytes of UTF-8 */
size_t wf_count(const char *s, size_t n);

/* The size nearest to a font of px pixels */
int wf_size(int px);

/* A size's cell, its rows above the baseline, the row and thickness of its underline, the row of its strike-through */
int wf_cell_w(int size);
int wf_cell_h(int size);
int wf_ascent(int size);
int wf_under(int size);
int wf_stroke(int size);
int wf_strike(int size);

/* How many columns a bold glyph is wider than a plain one, by its smear to the right */
int wf_bold(int size);

/* A row of a cell of a size: the leftmost pixel in the top bit of 16 */
unsigned int wf_row(int size, unsigned char g, int row);

/* The width in pixels of n bytes of UTF-8 at a size, in a style (WS_*) */
int wf_width(const char *s, size_t n, int scale, unsigned int style);

/* How many of the n bytes of s fit in width pixels (whole characters); *used gets their width */
size_t wf_fit(const char *s, size_t n, int width, int scale, unsigned int style, int *used);

#endif
