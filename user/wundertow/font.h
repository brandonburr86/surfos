/*
SurfOS Undertow's window: the font
----------------------
File: font.h  Date: 10/25/26
----------------------
The console's 8x16 font (driver/font8x16.c: code page 437's 256 characters, Spleen 8x16), drawn at a whole
scale. Every character is one or more cells of it: itself when the console has it, else Undertow's ASCII
stand-in (ut_ascii_for(): "EUR", "--", "...", "?"); combining marks and the zero-width joiners take none.
Layout and painting measure and draw through these calls only, so a font of other sizes, or proportional
glyphs, changes nothing else.
*/

#ifndef WUNDERTOW_FONT_H
#define WUNDERTOW_FONT_H

#include <stddef.h>

#define WF_W        8                   /* a cell's width and height at scale 1 */
#define WF_H        16
#define WF_ASCENT   12                  /* rows above the baseline */
#define WF_UNDER    13                  /* the underline's row */
#define WF_CELLS_MAX 8                  /* cells of one character at most (a stand-in) */

extern const unsigned char font8x16[256][16];

/* The cells (code page 437 indexes) that show the character cp: how many, in out (WF_CELLS_MAX room) */
int wf_cells(unsigned int cp, unsigned char *out);

/* The cells of n bytes of UTF-8 */
size_t wf_count(const char *s, size_t n);

/* How many of the n bytes of s fit in width pixels at scale (whole characters; *cells gets their cells) */
size_t wf_fit(const char *s, size_t n, int width, int scale, size_t *cells);

/* 11/1/26: the width in pixels of n bytes of UTF-8 of a laid-out item, given its scale and style: anything that needs
   a place inside a text item (a match of find, a focus box) measures it through this */
int wf_width(const char *s, size_t n, int scale, unsigned int style);

#endif
