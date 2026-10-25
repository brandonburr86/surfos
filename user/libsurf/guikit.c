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
*/

#include <surfos/types.h>
#include <sys/guidraw.h>
#include <string.h>
#include "guikit.h"

struct guikit { struct fb_surface s; struct gui_draw d; };

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
}

void guikit_rect(void *kit, int x, int y, int w, int h, unsigned color) {
    gd_rect(&((struct guikit *)kit)->d, x, y, w, h, color);
}

void guikit_frame(void *kit, int x, int y, int w, int h, unsigned color) {
    gd_frame(&((struct guikit *)kit)->d, x, y, w, h, color);
}

void guikit_line(void *kit, int x0, int y0, int x1, int y1, unsigned color) {
    gd_line(&((struct guikit *)kit)->d, x0, y0, x1, y1, color);
}

int guikit_text(void *kit, int x, int y, const char *utf8, unsigned color) {
    struct guikit *k = (struct guikit *)kit;
    u32 n = (u32)strlen(utf8), len;
    u8 ch;
    while(n) {
        ch = gd_char(utf8, n, &len);
        gd_glyph(&k->d, x, y, ch, color, 1);
        x += 8;
        utf8 += len;
        n -= len;
    }
    return x;
}

int guikit_cells(const char *utf8) {
    u32 n = (u32)strlen(utf8), len;
    int cells = 0;
    for(; n; cells++, utf8 += len, n -= len) len = gd_char_len(utf8, n);
    return cells;
}

void guikit_blit(void *kit, int x, int y, const unsigned *pixels, int w, int h, int stride) {
    gd_blit(&((struct guikit *)kit)->d, x, y, pixels, w, h, stride);
}

void guikit_button(void *kit, int x, int y, int w, int h, const char *label, unsigned bg, unsigned fg) {
    gd_button(&((struct guikit *)kit)->d, x, y, w, h, label, bg, fg);
}
