/*
SurfOS Undertow's window: the font
----------------------
File: font.c  Date: 10/25/26
----------------------
font.h's measuring: characters to the console's cells, as Undertow's text view shows them on the console.
*/

#include <stddef.h>
#include <surfos/cp437.h>
#include "undertow.h"
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

size_t wf_fit(const char *s, size_t n, int width, int scale, size_t *cells_out) {
    unsigned char cells[WF_CELLS_MAX];
    unsigned int cp;
    size_t i = 0, k, total = 0;
    long room = width / (WF_W * (scale < 1 ? 1 : scale));
    while(i < n) {
        size_t c;
        if((unsigned char)s[i] < 0x80) {
            k = 1;
            c = (unsigned char)s[i] >= 0x20 && s[i] != 0x7f ? 1 : (size_t)wf_cells((unsigned char)s[i], cells);
        } else {
            k = ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
            c = (size_t)wf_cells(cp, cells);
        }
        if((long)(total + c) > room) break;
        total += c;
        i += k;
    }
    if(cells_out) *cells_out = total;
    return i;
}
