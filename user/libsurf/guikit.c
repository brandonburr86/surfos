/*
SurfOS user library: libgui's side of the drawing kit
--------------------
File: guikit.c  Date: 10/25/26
--------------------
The desktop's drawing kit (shell/guidraw.c, built into libsurf by guidraw.c with fbcore.c and
font8x16.c) on a window's image: a surface of 32-bit pixels, 0x00RRGGBB, as the desktop's own
frame is (shell/gui.c), and a clip of the whole image. This file sees the kernel's headers and
not the library's; gui.c reaches it through guikit.h's plain types. Text is UTF-8, shown with the
console's characters (code page 437, '?' for a character it lacks), 8x16 pixels each.
10/26/26 (stage C12): one of those characters by its code, at a scale (the Character Map's grid).
11/05/26 (the desktop applications, track kit): every drawing goes through a view, an origin added
to its coordinates and a clip (gd_clip()'s rectangle, kept inside the image), so that the toolkit
(gk.h) draws a widget, or lets a program draw an area of its own, in coordinates from the widget's
top left and never outside it. guikit_setup() gives the whole image with its origin at 0, 0, as
before, so a program that never sets a view draws as it did. A button's label is UTF-8 now too.
*/

#include <surfos/types.h>
#include <sys/guidraw.h>
#include <string.h>
#include "guikit.h"

/* d: the image; c: the view's clip, inside d; ox, oy: the view's origin */
struct guikit { struct fb_surface s; struct gui_draw d, c; int ox, oy; };

unsigned guikit_size(void) {
    return sizeof(struct guikit);
}

void guikit_setup(void *kit, unsigned *pixels, int w, int h) {
    struct guikit *k = (struct guikit *)kit;
    struct fb_mode mode;
    memset(&mode, 0, sizeof(mode));
    mode.addr = 1;
    mode.width = (u32)w;
    mode.height = (u32)h;
    mode.pitch = (u32)w * 4;
    mode.bpp = 32;
    mode.type = 1;
    mode.red_pos = 16;
    mode.green_pos = 8;
    mode.blue_pos = 0;
    mode.red_size = mode.green_size = mode.blue_size = 8;
    fb_setup(&k->s, (u8 *)pixels, &mode);
    gd_init(&k->d, &k->s, w, h);
    k->c = k->d;
    k->ox = k->oy = 0;
}

/* 11/05/26: the view */
void guikit_get_view(const void *kit, int *ox, int *oy, int *l, int *t, int *r, int *b) {
    const struct guikit *k = (const struct guikit *)kit;
    *ox = k->ox;
    *oy = k->oy;
    *l = k->c.l;
    *t = k->c.t;
    *r = k->c.r;
    *b = k->c.b;
}

void guikit_set_view(void *kit, int ox, int oy, int l, int t, int r, int b) {
    struct guikit *k = (struct guikit *)kit;
    k->ox = ox;
    k->oy = oy;
    k->c = gd_clip(&k->d, l, t, r - l, b - t);          /* never outside the image, and never inside out */
}

void guikit_rect(void *kit, int x, int y, int w, int h, unsigned color) {
    struct guikit *k = (struct guikit *)kit;
    gd_rect(&k->c, x + k->ox, y + k->oy, w, h, color);
}

void guikit_frame(void *kit, int x, int y, int w, int h, unsigned color) {
    struct guikit *k = (struct guikit *)kit;
    gd_frame(&k->c, x + k->ox, y + k->oy, w, h, color);
}

void guikit_line(void *kit, int x0, int y0, int x1, int y1, unsigned color) {
    struct guikit *k = (struct guikit *)kit;
    gd_line(&k->c, x0 + k->ox, y0 + k->oy, x1 + k->ox, y1 + k->oy, color);
}

int guikit_text(void *kit, int x, int y, const char *utf8, unsigned color) {
    return guikit_text_scaled(kit, x, y, utf8, color, 1);
}

/* 10/26/26: the same at scale (each character's pixels scale x scale) */
int guikit_text_scaled(void *kit, int x, int y, const char *utf8, unsigned color, int scale) {
    struct guikit *k = (struct guikit *)kit;
    u32 n = (u32)strlen(utf8), len;
    u8 ch;
    if(scale < 1) scale = 1;
    while(n) {
        ch = gd_char(utf8, n, &len);
        gd_glyph(&k->c, x + k->ox, y + k->oy, ch, color, scale);
        x += 8 * scale;
        utf8 += len;
        n -= len;
    }
    return x;
}

/* 10/26/26: the console's character code (code page 437, 0-255), its pixels scale x scale */
void guikit_glyph(void *kit, int x, int y, unsigned code, unsigned color, int scale) {
    struct guikit *k = (struct guikit *)kit;
    gd_glyph(&k->c, x + k->ox, y + k->oy, (u8)code, color, scale < 1 ? 1 : scale);
}

int guikit_cells(const char *utf8) {
    u32 n = (u32)strlen(utf8), len;
    int cells = 0;
    for(; n; cells++, utf8 += len, n -= len) len = gd_char_len(utf8, n);
    return cells;
}

/* 11/05/26: the character at s, as guikit_text() draws it: its code and its bytes */
int guikit_char(const char *s, int avail, int *len) {
    u32 n;
    u8 ch;
    if(avail <= 0) {
        *len = 0;
        return 0;
    }
    ch = gd_char(s, (u32)avail, &n);
    *len = (int)n;
    return ch;
}

/* 11/05/26: at most cells characters; a text that has more shows cells - 1 of them and a dot, as the desktop's names
   end (gd_name()), with guikit_text()'s characters, so that what fits is drawn as guikit_text() draws it */
int guikit_text_cut(void *kit, int x, int y, const char *utf8, int cells, unsigned color) {
    struct guikit *k = (struct guikit *)kit;
    u32 n = (u32)strlen(utf8), len;
    int shown = guikit_cells(utf8), i;
    u8 ch;
    if(cells <= 0) return x;
    if(shown > cells) shown = cells - 1;
    for(i = 0; i < shown; i++) {
        ch = gd_char(utf8, n, &len);
        gd_glyph(&k->c, x + k->ox, y + k->oy, ch, color, 1);
        x += 8;
        utf8 += len;
        n -= len;
    }
    if(n) {
        gd_glyph(&k->c, x + k->ox, y + k->oy, 0xF9, color, 1);
        x += 8;
    }
    return x;
}

void guikit_blit(void *kit, int x, int y, const unsigned *pixels, int w, int h, int stride) {
    struct guikit *k = (struct guikit *)kit;
    gd_blit(&k->c, x + k->ox, y + k->oy, pixels, w, h, stride);
}

/* 11/05/26: a button's label is UTF-8, as every other text of libgui's (the desktop's gd_button() draws its own labels'
   bytes as code page 437's: the same for the ASCII every program gave it so far), centred by its characters */
void guikit_button(void *kit, int x, int y, int w, int h, const char *label, unsigned bg, unsigned fg) {
    guikit_rect(kit, x, y, w, h, bg);
    guikit_text(kit, x + (w - guikit_cells(label) * 8) / 2, y + (h - 16) / 2, label, fg);
}
