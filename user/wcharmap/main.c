/*
SurfOS user program: wcharmap
--------------------
File: main.c    Date: 10/26/26
--------------------
The desktop's Character Map as a program with a window of its own (libgui, gui.h; the model is
charmap.c): the console's 256 characters in a grid of 16 by 16, drawn with the console's own font
(gw_glyph()), the chosen one large with its code, its Unicode code point and its UTF-8 bytes, at
the places the desktop drew its own Character Map. The desktop's Apps menu and its tile start it
(shell/guicore.c's launcher stub), and bring its window back when it runs already. Copy (Enter,
Ctrl+C or Edit > Copy, a double-click, the button) puts the chosen character's UTF-8 on the
desktop's clipboard (gw_clip(), while the window has the focus); Ctrl+L opens Go to code, a band
of the window's own; Esc, Ctrl+W or the close button end it. A fault in it costs only its window.
Stage C12 of docs/superpowers/plans/2026-10-25-app-windows.md, after the Clock (wclock).
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <surf.h>
#include <gui.h>
#include <surfos/cp437.h>
#include "charmap.h"

/* the window's coordinates, as the desktop's Character Map drew in them, onto the content's: a pixel of border at
   the left and the title bar above */
#define X0 (-1)
#define Y0 (-CHARMAP_TITLE_H)
#define W (CHARMAP_W + 2)
#define GX CHARMAP_GX
#define CW CHARMAP_CW
#define PX CHARMAP_PX

/* at most cells characters, the last a dot when it goes on (the desktop's gd_name()) */
static void name(struct gw *win, int x, int y, const char *s, int cells, unsigned color) {
    char b[CHARMAP_STATUS + 4];
    int n = gw_text_width(s) / 8, k, len;
    if(n <= cells) { gw_text(win, x, y, s, color); return; }
    for(k = len = 0; s[len] && k < cells - 1; k++)      /* cells - 1 characters, then the dot */
        do len++; while(((unsigned char)s[len] & 0xC0) == 0x80);
    snprintf(b, sizeof(b), "%.*s\xe2\x88\x99", len, s);
    gw_text(win, x, y, b, color);
}

/* Go to code's band, over the status bar: what it takes, its text (lit while the first key typed replaces it), Go
   and Cancel */
static void band(struct gw *win, const struct wcharmap *m, const struct charmap_layout *l) {
    int fx = X0 + l->field_x, fy = Y0 + l->button_y, tw = gw_text_width(m->go);
    gw_rect(win, X0 + 1, Y0 + l->band_y, W - 2, m->win_h - 1 - l->band_y, GW_BAND);
    gw_rect(win, X0 + 1, Y0 + l->band_y, W - 2, 1, GW_LINE);
    gw_text(win, X0 + 10, Y0 + l->band_y + 4, "Go to code: 247, 0xF7, U+224B or a character", GW_INK);
    gw_rect(win, fx, fy, l->field_w, 22, GW_LINE);
    gw_rect(win, fx + 1, fy + 1, l->field_w - 2, 20, CHARMAP_SHEET);
    if(m->fresh) gw_rect(win, fx + 5, fy + 3, tw + 2, 16, GW_TEAL);
    gw_text(win, fx + 6, fy + 3, m->go, m->fresh ? GW_PAPER : GW_INK);
    if(!m->fresh) gw_rect(win, fx + 6 + tw, fy + 3, 1, 16, GW_INK);     /* where the next character goes */
    gw_button(win, X0 + l->go_x, fy, 64, 22, "Go", GW_TEAL, GW_PAPER);
    gw_button(win, X0 + l->cancel_x, fy, 80, 22, "Cancel", GW_SHELL, GW_INK);
}

static void draw(struct gw *win, const struct wcharmap *m, int focused) {
    struct charmap_layout l;
    char b[CHARMAP_STATUS + 16];
    const char *a, *z;
    int sel = m->selected & 0xFF, i, x, y;
    charmap_layout(m->win_h, &l);
    gw_fill(win, GW_PAPER);
    for(i = 0; i < 16; i++) {                           /* the code's low digit above, its high digit beside */
        snprintf(b, sizeof(b), "%X", i);
        gw_text(win, X0 + GX + i * CW + 5, Y0 + l.head_y, b, GW_MUTED);
        gw_text(win, X0 + GX - 16, Y0 + l.gy + i * l.ch + 1, b, GW_MUTED);
    }
    gw_rect(win, X0 + GX, Y0 + l.gy, 16 * CW + 1, 16 * l.ch + 1, GW_LINE);
    for(i = 0; i < 256; i++) {                          /* (cells of 16 rows: a glyph's last row is on the line below) */
        x = X0 + GX + i % 16 * CW;
        y = Y0 + l.gy + i / 16 * l.ch;
        gw_rect(win, x + 1, y + 1, CW - 1, l.ch - 1, i == sel ? (focused ? GW_TEAL : GW_MUTED) : CHARMAP_SHEET);
        gw_glyph(win, x + 5, y + 1, (unsigned)i, i == sel ? GW_PAPER : GW_INK, 1);
    }
    x = X0 + PX + (CHARMAP_PW - 66) / 2;                /* the chosen one, large */
    gw_rect(win, x, Y0 + CHARMAP_BOX_Y, 66, 106, GW_LINE);
    gw_rect(win, x + 1, Y0 + CHARMAP_BOX_Y + 1, 64, 104, CHARMAP_SHEET);
    gw_glyph(win, x + 9, Y0 + CHARMAP_BOX_Y + 5, (unsigned)sel, GW_INK, CHARMAP_BIG);
    x = X0 + PX;
    y = Y0 + CHARMAP_INFO_Y;
    gw_text(win, x, y, "Code", GW_MUTED);
    snprintf(b, sizeof(b), "%d", sel);
    gw_text(win, x + 64, y, b, GW_INK);
    gw_text(win, x, y + 18, "Hex", GW_MUTED);
    snprintf(b, sizeof(b), "0x%02X", sel);
    gw_text(win, x + 64, y + 18, b, GW_INK);
    gw_text(win, x, y + 36, "Unicode", GW_MUTED);
    snprintf(b, sizeof(b), "U+%04X", cp437_unicode[sel]);
    gw_text(win, x + 64, y + 36, b, GW_INK);
    gw_text(win, x, y + 54, "UTF-8", GW_MUTED);
    charmap_bytes(sel, b, sizeof(b));
    gw_text(win, x + 64, y + 54, b, GW_INK);
    charmap_notes(sel, &a, &z);
    gw_text(win, x, y + 80, a, GW_MUTED);
    gw_text(win, x, y + 98, z, GW_MUTED);
    gw_button(win, x, Y0 + l.copy_y, CHARMAP_COPY_W, CHARMAP_COPY_H, "Copy", sel ? GW_TEAL : GW_BAND, sel ? GW_PAPER : GW_MUTED);
    if(m->asking) { band(win, m, &l); return; }
    gw_rect(win, X0 + 1, Y0 + l.sy, W - 2, CHARMAP_STATUS_H, GW_BAND);
    gw_rect(win, X0 + 1, Y0 + l.sy, W - 2, 1, GW_LINE);
    charmap_status(m, b, sizeof(b));
    name(win, X0 + 10, Y0 + l.sy + 3, b, (W - 20) / 8, GW_INK);
}

int main(void) {
    struct wcharmap m;
    struct win_event ev;
    struct gw *win;
    char text[CP437_UTF8_MAX];
    unsigned buttons = 0;
    int r, k, n, focused = -1;
    win = gw_open("Character Map", CHARMAP_W, CHARMAP_H, 0, GW_PAPER);
    if(!win) {
        fprintf(stderr, "wcharmap: no window: %s\n", strerror(errno));
        return 1;
    }
    charmap_init(&m, win->h + CHARMAP_TITLE_H + 1);     /* as high as the desktop let it be: the layout follows */
    for(k = CHARMAP_DRAW;;) {
        if(k || focused != win->focused) {              /* the chosen cell lit in teal while the window has the focus */
            focused = win->focused;
            draw(win, &m, focused);
            if(gw_present_all(win) < 0 && errno == EPIPE) break;
        }
        r = gw_next(win, &ev, -1);
        if(r < 0) break;                                /* the desktop let it go, or a kill */
        k = 0;
        if(r == 0) continue;
        if(ev.type == WIN_EV_CLOSE) break;
        if(ev.type == WIN_EV_KEY) k = charmap_key(&m, (int)ev.value);
        else if(ev.type == WIN_EV_POINTER) {            /* a press of the left button, in the window's coordinates */
            if((ev.value & WIN_BUTTON_LEFT) && !(buttons & WIN_BUTTON_LEFT))
                k = charmap_press(&m, ev.x - X0, ev.y - Y0, uptime());
            buttons = ev.value;
        }
        if(k == CHARMAP_CLOSE) break;
        if(k == CHARMAP_COPY && (n = charmap_copy(&m, text, sizeof(text))) > 0 && gw_clip(win, text, (unsigned)n) < 0)
            snprintf(m.status, sizeof(m.status), "%s", errno == EACCES ? "Not copied: the window must have the focus" :
                     "Not copied");
    }
    gw_close(win);
    return 0;
}
