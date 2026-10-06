/*
SurfOS Code Page 437
--------------------
File: cp437.c   Date: 10/6/26
--------------------
The console's character set (include/sys/cp437.h): the 256 characters of code page 437,
the set the VGA card's own font draws, with SurfOS's ▸ ≋ … in three places whose VGA
glyph is close; the UTF-8 decoder that turns what programs print into those cells; and
the way back, a cell's character as UTF-8, for the serial terminal's repaint.

tools/bdf2c.py reads cp437_unicode[] below to build the framebuffer's font
(driver/font8x16.c) in the same order, so the screen, the font and the serial port
agree on what a cell is. Pure code without kernel state: tests/host/test_cp437.c runs it
on the host.
*/

#include <surfos/types.h>
#include <sys/cp437.h>

/* index -> Unicode. 0x01-0x1F and 0x7F are the graphic characters VGA text mode shows
   there (a cell is never a control character); 0x10, 0xF7 and 0xF9 are SurfOS's. */
const u16 cp437_unicode[256] = {
    /* 0x00 */ 0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    /* 0x08 */ 0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    /* 0x10 */ 0x25B8, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,   /* 0x10: ▸, VGA ► */
    /* 0x18 */ 0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
    /* 0x20 */ 0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    /* 0x28 */ 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    /* 0x30 */ 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    /* 0x38 */ 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    /* 0x40 */ 0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    /* 0x48 */ 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    /* 0x50 */ 0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    /* 0x58 */ 0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    /* 0x60 */ 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    /* 0x68 */ 0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    /* 0x70 */ 0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    /* 0x78 */ 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x2302,
    /* 0x80 */ 0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    /* 0x88 */ 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    /* 0x90 */ 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    /* 0x98 */ 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    /* 0xA0 */ 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    /* 0xA8 */ 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    /* 0xB0 */ 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    /* 0xB8 */ 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    /* 0xC0 */ 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    /* 0xC8 */ 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    /* 0xD0 */ 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    /* 0xD8 */ 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    /* 0xE0 */ 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    /* 0xE8 */ 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    /* 0xF0 */ 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x224B,   /* 0xF7: ≋, VGA ≈ */
    /* 0xF8 */ 0x00B0, 0x2026, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,   /* 0xF9: …, VGA ∙ */
};

/* Other characters drawn in the same places: what VGA text mode shows at SurfOS's three,
   the bigger triangles for ▸, and the Greek letters code page 437 shares with ß, µ and Ω.
   10/6/26: and the typographic quotes and dashes of UTF-8 text, as their ASCII forms. */
static const struct { u16 u; u8 index; } aliases[] = {
    { 0x25BA, 0x10 }, { 0x25B6, 0x10 },         /* ► ▶ */
    { 0x2248, 0xF7 },                           /* ≈ */
    { 0x2219, 0xF9 },                           /* ∙ */
    { 0x03B2, 0xE1 },                           /* β */
    { 0x03BC, 0xE6 },                           /* μ */
    { 0x2126, 0xEA },                           /* Ω, the ohm sign */
    { 0x2018, '\'' }, { 0x2019, '\'' },         /* ‘ ’ */
    { 0x201C, '"' }, { 0x201D, '"' },           /* “ ” */
    { 0x2013, '-' }, { 0x2014, '-' },           /* – — */
};
#define NALIASES (sizeof(aliases) / sizeof(aliases[0]))

int cp437_from_unicode(u32 u) {
    u_int i;
    if(u >= 0x20 && u <= 0x7E) return (int)u;
    if(u == 0) return 0;                        /* the empty cell */
    if(u < 0xA0 || u > 0xFFFF) return -1;       /* control characters; nothing in the set is above U+FFFF */
    for(i = 0x80; i < 0x100; i++) if(cp437_unicode[i] == u) return (int)i;
    for(i = 0x01; i < 0x20; i++) if(cp437_unicode[i] == u) return (int)i;
    if(cp437_unicode[0x7F] == u) return 0x7F;
    for(i = 0; i < NALIASES; i++) if(aliases[i].u == u) return aliases[i].index;
    return -1;
}

int cp437_utf8(u8 index, char *out) {
    u32 u = cp437_unicode[index];
    if(u < 0x80) {
        out[0] = index ? (char)u : ' ';
        return 1;
    }
    if(u < 0x800) {
        out[0] = (char)(0xC0 | (u >> 6));
        out[1] = (char)(0x80 | (u & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (u >> 12));
    out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
    out[2] = (char)(0x80 | (u & 0x3F));
    return 3;
}

/**** UTF-8 ****/

/* the length of the sequence a byte starts; 0 for a byte that cannot start one: a
   continuation byte, C0 and C1 (only ever overlong), F5-FF (beyond U+10FFFF) */
static int lead_length(u8 c) {
    if(c >= 0xC2 && c <= 0xDF) return 2;
    if(c >= 0xE0 && c <= 0xEF) return 3;
    if(c >= 0xF0 && c <= 0xF4) return 4;
    return 0;
}

/* May c come next in the sequence held? The second byte's range depends on the first
   (Unicode's table 3-7): no overlong forms (E0, F0), no surrogates (ED), nothing above
   U+10FFFF (F4). */
static int continues(const struct utf8_dec *d, u8 c) {
    u8 lo = 0x80, hi = 0xBF;
    if(d->have == 1) {
        switch(d->b[0]) {
        case 0xE0: lo = 0xA0; break;
        case 0xED: hi = 0x9F; break;
        case 0xF0: lo = 0x90; break;
        case 0xF4: hi = 0x8F; break;
        }
    }
    return c >= lo && c <= hi;
}

int utf8_feed(struct utf8_dec *d, u8 c, u8 *out) {
    int n = 0, i, len;
    u32 u;
    /* 10/6/26: a state no sequence leaves behind (two writers on one decoder, say) starts afresh,
       so out[] never gets more than UTF8_OUT_MAX cells */
    if(d->have && (d->need < 2 || d->need > 4 || d->have >= d->need)) d->have = 0;
    if(d->have) {
        if(continues(d, c)) {
            if(d->have + 1 < d->need) {
                d->b[d->have++] = c;
                return 0;
            }
            d->b[d->have] = c;                              /* the last byte: done, and nothing held */
            len = d->need;
            d->have = 0;
            u = d->b[0] & (0x7F >> len);
            for(i = 1; i < len; i++) u = (u << 6) | (d->b[i] & 0x3F);
            i = cp437_from_unicode(u);
            out[0] = i < 0 ? '?' : (u8)i;
            return 1;
        }
        for(i = 0; i < d->have && n < UTF8_OUT_MAX - 1; i++) out[n++] = d->b[i];   /* cut short: its bytes as they are */
        d->have = 0;
    }
    if(c >= 0x80 && (d->need = (u8)lead_length(c)) != 0) {
        d->b[0] = c;
        d->have = 1;
        return n;
    }
    out[n++] = c;                                           /* ASCII, or a byte that is not UTF-8 */
    return n;
}
