/*
SurfOS Undertow's window: the font
----------------------
File: font.c  Date: 10/25/26
----------------------
font.h's measuring: characters to the console's cells, as Undertow's text view shows them on the console, and
those cells' widths.
*/

#include <stddef.h>
#include <surfos/cp437.h>
#include "undertow.h"
#include "doc.h"
#include "font.h"

/* Combining marks: drawn on the character before them by a real font; here they take no cell */
static int combining(unsigned int cp) {
    return (cp >= 0x300 && cp <= 0x36f) || (cp >= 0x1ab0 && cp <= 0x1aff) || (cp >= 0x1dc0 && cp <= 0x1dff) ||
           (cp >= 0x20d0 && cp <= 0x20ff) || (cp >= 0xfe20 && cp <= 0xfe2f);
}

int wf_cells(unsigned int cp, unsigned char *out) {
    const char *s;
    int i, k;
    if(cp >= 0x20 && cp < 0x7f) {                   /* ASCII: itself */
        out[0] = (unsigned char)cp;
        return 1;
    }
    if(combining(cp)) return 0;
    if(cp && (k = cp437_from_unicode(cp)) > 0) {
        out[0] = (unsigned char)k;
        return 1;
    }
    s = ut_ascii_for(cp);                           /* "?" for anything it does not know */
    for(i = 0; s[i] && i < WF_CELLS_MAX; i++) out[i] = (unsigned char)s[i];
    return i;
}

size_t wf_count(const char *s, size_t n) {
    unsigned char cells[WF_CELLS_MAX];
    unsigned int cp;
    size_t i = 0, total = 0;
    while(i < n) {
        if((unsigned char)s[i] < 0x80) {            /* (the common case, without decoding) */
            total += (unsigned char)s[i] >= 0x20 && s[i] != 0x7f ? 1 : (size_t)wf_cells((unsigned char)s[i], cells);
            i++;
            continue;
        }
        i += ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        total += (size_t)wf_cells(cp, cells);
    }
    return total;
}

static const struct { unsigned char w, h, ascent, under, stroke, strike, bold; } fonts[] = {
    { 0, 0, 0, 0, 0, 0, 0 },
    { 8, 16, 12, 13, 1, 8, 1 },
    { 12, 24, 19, 20, 2, 12, 1 },
    { 16, 32, 26, 27, 2, 17, 2 }
};

static int known(int size) { return size < WF_S16 ? WF_S16 : size > WF_S32 ? WF_S32 : size; }

int wf_size(int px) { return px <= 20 ? WF_S16 : px <= 28 ? WF_S24 : WF_S32; }
int wf_cell_w(int size) { return fonts[known(size)].w; }
int wf_cell_h(int size) { return fonts[known(size)].h; }
int wf_ascent(int size) { return fonts[known(size)].ascent; }
int wf_under(int size) { return fonts[known(size)].under; }
int wf_stroke(int size) { return fonts[known(size)].stroke; }
int wf_strike(int size) { return fonts[known(size)].strike; }
int wf_bold(int size) { return fonts[known(size)].bold; }

unsigned int wf_row(int size, unsigned char g, int row) {
    switch(known(size)) {
    case WF_S16: return (unsigned int)font8x16[g][row] << 8;
    case WF_S24: return font12x24[g][row];
    }
    return font16x32[g][row];
}

/* Bold adds its smear to what has ink, and leaves what must meet its neighbours alone */
static int smears(unsigned char g) { return g != ' ' && g && g != 0xff && !(g >= 0xb0 && g <= 0xdf); }

int wf_advance(int size, unsigned char g, unsigned int style) {
    if(style & WS_MONO) return wf_cell_w(size);
    return wf_metrics[known(size) - 1][g].advance + (style & WS_BOLD && smears(g) ? wf_bold(size) : 0);
}

int wf_bearing(int size, unsigned char g, unsigned int style) {
    return style & WS_MONO ? 0 : wf_metrics[known(size) - 1][g].bearing;
}

/* The next character of s: its bytes, and its width */
static size_t advance(const char *s, size_t n, int size, unsigned int style, int *w) {
    unsigned char cells[WF_CELLS_MAX];
    unsigned int cp = (unsigned char)s[0];
    size_t k = 1;
    int c, count;
    if(cp >= 0x80) k = ut_utf8_decode((const unsigned char *)s, n, &cp);
    count = wf_cells(cp, cells);
    for(*w = 0, c = 0; c < count; c++) *w += wf_advance(size, cells[c], style);
    return k;
}

int wf_width(const char *s, size_t n, int scale, unsigned int style) {
    size_t i = 0;
    int total = 0, w;
    while(i < n) {
        i += advance(s + i, n - i, scale, style, &w);
        total += w;
    }
    return total;
}

size_t wf_fit(const char *s, size_t n, int width, int scale, unsigned int style, int *used) {
    size_t i = 0;
    int total = 0, w;
    while(i < n) {
        size_t k = advance(s + i, n - i, scale, style, &w);
        if(total + w > width) break;
        total += w;
        i += k;
    }
    if(used) *used = total;
    return i;
}
