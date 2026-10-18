/*
SurfOS Desktop Drawing Kit
--------------------
File: guidraw.c Date: 10/17/26
--------------------
Rectangles, text, buttons, scroll bars, the wave and the desktop's icons, all
clipped to a rectangle (struct gui_draw), so a window's text never spills out
of it. Text is drawn with the console's 8x16 code page 437 font; names, which
are UTF-8, go through the console's decoder (driver/cp437.c).
*/
#include "guipriv.h"
#include <sys/cp437.h>
#include <string.h>
#include <stdio.h>

void gd_init(struct gui_draw *d, const struct fb_surface *s, int width, int height) {
    d->s = s; d->l = 0; d->t = 0; d->r = width; d->b = height;
}

/* the part of d's rectangle that is also inside x, y, w, h */
struct gui_draw gd_clip(const struct gui_draw *d, int x, int y, int w, int h) {
    struct gui_draw c = *d;
    if(x > c.l) c.l = x;
    if(y > c.t) c.t = y;
    if(x + w < c.r) c.r = x + w;
    if(y + h < c.b) c.b = y + h;
    if(c.r < c.l) c.r = c.l;
    if(c.b < c.t) c.b = c.t;
    return c;
}

void gd_rect(const struct gui_draw *d, int x, int y, int w, int h, u32 c) {
    int r = x + w, b = y + h;
    if(x < d->l) x = d->l;
    if(y < d->t) y = d->t;
    if(r > d->r) r = d->r;
    if(b > d->b) b = d->b;
    if(r > x && b > y) fb_rect_rgb(d->s, x, y, r - x, b - y, c);
}

void gd_frame(const struct gui_draw *d, int x, int y, int w, int h, u32 c) {
    gd_rect(d, x, y, w, 1, c); gd_rect(d, x, y + h - 1, w, 1, c);
    gd_rect(d, x, y, 1, h, c); gd_rect(d, x + w - 1, y, 1, h, c);
}

/* one glyph, each row as runs of lit pixels */
void gd_glyph(const struct gui_draw *d, int x, int y, u8 ch, u32 c, int scale) {
    const u8 *glyph = font8x16[ch];
    int row, col, start;
    if(x >= d->r || y >= d->b || x + 8 * scale <= d->l || y + 16 * scale <= d->t) return;
    for(row = 0; row < 16; row++) {
        u8 bits = glyph[row];
        for(col = 0; col < 8;) {
            if(!(bits & (0x80 >> col))) { col++; continue; }
            start = col;
            while(col < 8 && (bits & (0x80 >> col))) col++;
            gd_rect(d, x + start * scale, y + row * scale, (col - start) * scale, scale, c);
        }
    }
}

int gd_text_scaled(const struct gui_draw *d, int x, int y, const char *s, u32 c, int scale) {
    while(*s) { gd_glyph(d, x, y, (u8)*s++, c, scale); x += 8 * scale; }
    return x;
}

int gd_text(const struct gui_draw *d, int x, int y, const char *s, u32 c) {
    return gd_text_scaled(d, x, y, s, c, 1);
}

/* the cells of a UTF-8 string, into out (at most max); how many there are in all */
static int decode(const char *s, u8 *out, int max) {
    struct utf8_dec dec;
    u8 cells[UTF8_OUT_MAX];
    int n = 0, k, i;
    memset(&dec, 0, sizeof(dec));
    for(;; s++) {
        k = utf8_feed(&dec, (u8)*s, cells);
        if(!*s) k--;                                    /* the terminator comes out last: not a cell */
        for(i = 0; i < k; i++, n++) if(n < max) out[n] = cells[i];
        if(!*s) return n;
    }
}

/* The bytes of the character at s: a whole UTF-8 sequence by the console's rules (driver/cp437.c:
   lead bytes C2-F4, no overlong forms, surrogates or values above U+10FFFF), or else one byte,
   which is drawn as the 437 character it is, as on the console. Notepad steps through its text
   with this and draws with gd_char(), so the two always agree. */
u32 gd_char_len(const char *s, u32 avail) {
    u8 c = (u8)s[0], lo = 0x80, hi = 0xBF;
    u32 need = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 1, k;
    if(need == 1 || avail < need) return 1;
    if(c == 0xE0) lo = 0xA0;
    if(c == 0xED) hi = 0x9F;
    if(c == 0xF0) lo = 0x90;
    if(c == 0xF4) hi = 0x8F;
    if((u8)s[1] < lo || (u8)s[1] > hi) return 1;
    for(k = 2; k < need; k++) if(((u8)s[k] & 0xC0) != 0x80) return 1;
    return need;
}

u8 gd_char(const char *s, u32 avail, u32 *len) {
    u8 c = (u8)s[0];
    u32 k, u;
    int index;
    *len = gd_char_len(s, avail);
    if(*len == 1) return c;
    u = c & (0x7F >> *len);
    for(k = 1; k < *len; k++) u = u << 6 | ((u8)s[k] & 0x3F);
    index = cp437_from_unicode(u);
    return index < 0 ? '?' : (u8)index;
}

int gd_cells(const char *s) {
    return decode(s, NULL, 0);
}

/* a name in at most cells cells: a longer one ends in … */
int gd_name(const struct gui_draw *d, int x, int y, const char *s, int cells, u32 c) {
    u8 out[160];
    int n, i, shown;
    if(cells <= 0) return x;
    if(cells > (int)sizeof(out)) cells = sizeof(out);
    n = decode(s, out, sizeof(out));
    shown = n > cells ? cells - 1 : n;
    for(i = 0; i < shown; i++, x += 8) gd_glyph(d, x, y, out[i], c, 1);
    if(n > cells) { gd_glyph(d, x, y, 0xF9, c, 1); x += 8; }
    return x;
}

void gd_button(const struct gui_draw *d, int x, int y, int w, int h, const char *label, u32 bg, u32 fg) {
    gd_rect(d, x, y, w, h, bg);
    gd_text(d, x + (w - (int)strlen(label) * 8) / 2, y + (h - 16) / 2, label, fg);
}

void gd_wave(const struct gui_draw *d, int x, int y, int w, int h, u32 c) {
    int i;
    for(i = 0; i < w; i++) {
        int t = i % 80, py = (t < 20 ? t : t < 60 ? 40 - t : t - 80) * h / 40;
        gd_rect(d, x + i, y + py, 1, 3, c);
    }
}

void gd_thumb(int h, int first, int shown, int total, int *top, int *size) {
    if(total <= shown || total <= 0) { *top = 0; *size = h; return; }
    *size = h * shown / total;                         /* small numbers: no 64-bit division in the kernel */
    if(*size < 16) *size = 16;
    if(*size > h) *size = h;
    if(first > total - shown) first = total - shown;
    if(first < 0) first = 0;
    *top = (h - *size) * first / (total - shown);
}

int gd_scroll_to(int h, int y, int shown, int total) {
    int top, size, first;
    if(total <= shown) return 0;
    gd_thumb(h, 0, shown, total, &top, &size);
    if(h <= size) return 0;
    first = y * (total - shown) / (h - size);
    return first < 0 ? 0 : first > total - shown ? total - shown : first;
}

void gd_scrollbar(const struct gui_draw *d, int x, int y, int h, int first, int shown, int total) {
    int top, size;
    gd_rect(d, x, y, SCROLL_W, h, BAND);
    gd_rect(d, x, y, 1, h, LINE);
    if(total <= shown) return;
    gd_thumb(h, first, shown, total, &top, &size);
    gd_rect(d, x + 2, y + top + 1, SCROLL_W - 3, size - 2, MUTED);
}

/* 16x16 pictures; a letter is a colour of the palette, '.' is see-through */
static const char *const icons[][16] = {
    [ICON_FOLDER] = {
        "................", "................", ".KKKKK..........", "KSSSSSK.........",
        "KSSSSSSKKKKKKKK.", "KYYYYYYYYYYYYYYK", "KSSSSSSSSSSSSSSK", "KSSSSSSSSSSSSSSK",
        "KSSSSSSSSSSSSSSK", "KSSSSSSSSSSSSSSK", "KSSSSSSSSSSSSSSK", "KSSSSSSSSSSSSSSK",
        "KSSSSSSSSSSSSSSK", ".KKKKKKKKKKKKKK.", "................", "................" },
    [ICON_OPEN_FOLDER] = {
        "................", "................", ".KKKKK..........", "KSSSSSK.........",
        "KSSSSSSKKKKKKK..", "KSSSSSSSSSSSSSK.", "KSSKKKKKKKKKKKKK", "KSKYYYYYYYYYYYYK",
        "KSKYYYYYYYYYYYK.", "KKYYYYYYYYYYYYK.", "KKYYYYYYYYYYYK..", "KYYYYYYYYYYYYK..",
        "KYYYYYYYYYYYYK..", ".KKKKKKKKKKKKK..", "................", "................" },
    [ICON_FILE] = {
        "................", "...KKKKKKKK.....", "...KPPPPPPKK....", "...KPPPPPPKPK...",
        "...KPPPPPPKKKK..", "...KPPPPPPPPPK..", "...KPPPPPPPPPK..", "...KPPPPPPPPPK..",
        "...KPPPPPPPPPK..", "...KPPPPPPPPPK..", "...KPPPPPPPPPK..", "...KPPPPPPPPPK..",
        "...KPPPPPPPPPK..", "...KKKKKKKKKKK..", "................", "................" },
    [ICON_TEXT] = {
        "................", "...KKKKKKKK.....", "...KPPPPPPKK....", "...KPTTTTPKPK...",
        "...KPPPPPPKKKK..", "...KPTTTTTTTPK..", "...KPPPPPPPPPK..", "...KPTTTTTTTPK..",
        "...KPPPPPPPPPK..", "...KPTTTTTPPPK..", "...KPPPPPPPPPK..", "...KPTTTTTTTPK..",
        "...KPPPPPPPPPK..", "...KKKKKKKKKKK..", "................", "................" },
    [ICON_DRIVE] = {
        "................", "................", "................", "................",
        "..KKKK..........", "..KMMKKKKKKKKKK.", "..KMKKTTTTTTTTTK", "..KMMKTFFFFFTTTK",
        "..KMKKTFFFFFTTTK", "..KMMKTTTTTTTCTK", "..KKKKKKKKKKKKK.", "................",
        "................", "................", "................", "................" },
    [ICON_NOTEPAD] = {
        "................", "...K.K.K.K.K....", "..KKKKKKKKKKKK..", "..KCCCCCCCCCCK..",
        "..KPPPPPPPPPPK..", "..KPTTTTTTTTPK..", "..KPPPPPPPPPPK..", "..KPTTTTTTTTPK..",
        "..KPPPPPPPPPPK..", "..KPTTTTTTPPPK..", "..KPPPPPPPPPPK..", "..KPTTTTTTTTPK..",
        "..KPPPPPPPPPPK..", "..KKKKKKKKKKKK..", "................", "................" },
    [ICON_FILES] = {
        "................", "................", ".KKKKK..........", "KSSSSSK.........",
        "KSSSSSSKKKKKKKK.", "KYYYYYYYYYYYYYYK", "KSSSSSSSSSSSSSSK", "KSSTTSSSTTSSSTTK",
        "KSTSSTSTSSTSTSSK", "KSSSSSSSSSSSSSSK", "KSSTTSSSTTSSSTTK", "KSTSSTSTSSTSTSSK",
        "KSSSSSSSSSSSSSSK", ".KKKKKKKKKKKKKK.", "................", "................" },
    [ICON_CALC] = {
        "................", "..KKKKKKKKKKKK..", "..KMMMMMMMMMMK..", "..KMFFFFFFFFMK..",
        "..KMFFFFFFFFMK..", "..KMMMMMMMMMMK..", "..KMPPMPPMTTMK..", "..KMPPMPPMTTMK..",
        "..KMMMMMMMMMMK..", "..KMPPMPPMTTMK..", "..KMPPMPPMTTMK..", "..KMMMMMMMMMMK..",
        "..KMPPMPPMCCMK..", "..KMMMMMMMMMMK..", "..KKKKKKKKKKKK..", "................" },
    [ICON_SKETCH] = {
        "................", ".....KKKKKK.....", "...KKPPPPPPKK...", "..KPPCCPPPBBPK..",
        ".KPPPCCPPPBBPPK.", ".KPPPPPPPPPPPPK.", "KPSSPPPPPPPPPPPK", "KPSSPPPPPKKPPPPK",
        "KPPPPPPPK..KPPPK", "KPPTTPPPK..KPPK.", ".KPTTPPPPKKPPPK.", ".KPPPPPPPPPPPK..",
        "..KKPPPPPPPKK...", "....KKKKKKK.....", "................", "................" },
    [ICON_WAVE] = {
        "................", "................", "................", "..TT....TT....TT",
        ".T..T..T..T..T..", "T....TT....TT...", "................", "..TT....TT....TT",
        ".T..T..T..T..T..", "T....TT....TT...", "................", "..TT....TT....TT",
        ".T..T..T..T..T..", "T....TT....TT...", "................", "................" },
    [ICON_HELP] = {
        "................", ".....TTTTTT.....", "...TTTTTTTTTT...", "..TTTPPPPPPTTT..",
        "..TTPPTTTTPPTT..", ".TTTTTTTTTPPTTT.", ".TTTTTTTTPPTTTT.", ".TTTTTTTPPTTTTT.",
        ".TTTTTTPPTTTTTT.", ".TTTTTTPPTTTTTT.", "..TTTTTTTTTTTT..", "..TTTTTPPTTTTT..",
        "...TTTTPPTTTT...", ".....TTTTTT.....", "................", "................" },
    [ICON_GEAR] = {
        "................", "......KKKK......", "...KK.KTTK.KK...", "..KTTKKTTKKTTK..",
        "..KTTTTTTTTTTK..", "...KTTTKKTTTK...", ".KKKTTKPPKTTKKK.", ".KTTTTKPPKTTTTK.",
        ".KTTTTKPPKTTTTK.", ".KKKTTKPPKTTKKK.", "...KTTTKKTTTK...", "..KTTTTTTTTTTK..",
        "..KTTKKTTKKTTK..", "...KK.KTTK.KK...", "......KKKK......", "................" },
};

static u32 paint(char c) {
    switch(c) {
    case 'K': return INK;
    case 'N': return NAVY;
    case 'T': return TEAL;
    case 'F': return FOAM;
    case 'P': return 0xFFFDF5;
    case 'C': return CORAL;
    case 'S': return SAND;
    case 'Y': return 0xE8D08A;
    case 'B': return SKY;
    case 'M': return MUTED;
    default: return PAPER;
    }
}

/* tint: every pixel in that colour (the wave on a teal title bar), or 0 for the icon's own */
static void icon(const struct gui_draw *d, int x, int y, int which, int scale, u32 tint) {
    int row, col, start;
    if(which < 0 || which >= (int)(sizeof(icons) / sizeof(icons[0]))) return;
    for(row = 0; row < 16; row++) {
        const char *p = icons[which][row];
        for(col = 0; col < 16;) {
            if(p[col] == '.') { col++; continue; }
            start = col;
            while(col < 16 && p[col] == p[start]) col++;
            gd_rect(d, x + start * scale, y + row * scale, (col - start) * scale, scale, tint ? tint : paint(p[start]));
        }
    }
}

void gd_icon(const struct gui_draw *d, int x, int y, int which, int scale) {
    icon(d, x, y, which, scale, 0);
}

void gd_icon_tint(const struct gui_draw *d, int x, int y, int which, int scale, u32 tint) {
    icon(d, x, y, which, scale, tint);
}
