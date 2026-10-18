/*
SurfOS Desktop: Character Map
--------------------
File: guichar.c Date: 10/17/26
--------------------
The console's 256 characters (code page 437 with SurfOS's three, driver/cp437.c) in a grid of 16 by 16,
drawn with the console's own font. The chosen one is shown large with its code, its Unicode code point
and its UTF-8 bytes. Copy (Enter, Ctrl+C, a double-click, the button) puts its UTF-8 on the desktop's
clipboard, so it pastes into Notepad, whose text is UTF-8, and into the Terminal: the laptop's keyboard
types nothing beyond ASCII. Codes 1 to 31 and 127 are the graphic characters the console draws there
(cp437_utf8() gives U+263A for 1, say), never control characters, and that is what is copied; 0 is the
console's empty cell, which copies nothing.
Track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_charmap (struct gui_app, shell/guipriv.h).
*/
#include "gui_tools.h"
#include <sys/cp437.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SHEET 0xFFFDF5                  /* the grid's background, as Notepad's text */
#define GX 26                           /* the grid: its left and top (window-relative), each cell's size */
#define GY 50
#define CW 17
#define CH 17
#define PX 312                          /* the panel of the chosen character, beside the grid */
#define PW 138
#define BOX_Y 40                        /* the chosen character, large */
#define BIG 6
#define INFO_Y 154                      /* its code, code point and bytes */
#define COPY_Y 290                      /* the Copy button */
#define ANS_GOTO 1                      /* Go to code's answer (gui_app_act) */

static const char *const help[] = {
    "#Character Map",
    "The console's 256 characters (code page 437). Click one,",
    "or move with the arrows: Home and End go to the start and",
    "the end of the row, Page Up and Page Down to the top and",
    "the bottom of the column, and a key typed to its own",
    "character. Ctrl+L goes to a code (247, 0xF7, U+224B).",
    "Enter, Ctrl+C or a double-click copies the character as",
    "UTF-8: Ctrl+V pastes it in Notepad or the Terminal.",
    "Codes 1-31 and 127 copy the symbols the console shows",
    "there; 0, the empty cell, copies nothing.",
};

int charmap_utf8(int code, char *out) {
    if(code <= 0 || code > 255) return 0;
    return cp437_utf8((u8)code, out);
}

static void status(struct gui_state *g, const char *msg) {
    strlcpy(g->charmap.status, msg, sizeof(g->charmap.status));
    g->dirty = true;
}

static void choose(struct gui_state *g, int code) {
    g->charmap.selected = gui_clamp(code, 0, 255);
    g->charmap.status[0] = 0;
    g->dirty = true;
}

static void copy(struct gui_state *g) {
    int code = g->charmap.selected, n;
    char u[CP437_UTF8_MAX + 1], msg[GUI_STATUS];
    n = charmap_utf8(code, u);
    if(!n) { status(g, "0x00 is the console's empty cell: nothing to copy"); return; }
    memcpy(g->clip, u, n);
    g->clip_len = n;
    u[n] = 0;
    if(code == ' ' || code == 0xFF) snprintf(msg, sizeof(msg), "Copied a %sspace: Ctrl+V pastes it", code == ' ' ? "" : "no-break ");
    else snprintf(msg, sizeof(msg), "Copied %s (%d byte%s of UTF-8): Ctrl+V pastes it", u, n, n == 1 ? "" : "s");
    status(g, msg);
}

/* what Go to code was given: 247, 0xF7, U+224B, or one character */
static void go_to(struct gui_state *g, const char *text) {
    char msg[GUI_STATUS], *end;
    unsigned long v;
    int k;
    u32 len;
    while(*text == ' ') text++;
    if((text[0] == 'U' || text[0] == 'u') && text[1] == '+') {
        v = strtoul(text + 2, &end, 16);
        k = end != text + 2 && v <= 0xFFFF ? cp437_from_unicode((u32)v) : -1;
        if(k < 0) {
            snprintf(msg, sizeof(msg), "U+%04lX is not one of the console's characters", v);
            status(g, msg);
            return;
        }
        choose(g, k);
        return;
    }
    if(text[0] >= '0' && text[0] <= '9') {
        v = strtoul(text, &end, text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10);
        while(*end == ' ') end++;
        if(*end || v > 255) { status(g, "A code is 0 to 255 (0x00 to 0xFF), or U+ and a code point"); return; }
        choose(g, (int)v);
        return;
    }
    if(text[0]) {                                       /* one character: where the console has it */
        len = gd_char_len(text, strlen(text));
        if(!text[len]) { choose(g, gd_char(text, len, &len)); return; }
    }
    status(g, "A code is 0 to 255 (0x00 to 0xFF), or U+ and a code point");
}

static bool can_edit(const struct gui_state *g, int act) {
    return (act == ACT_COPY && g->charmap.selected != 0) || act == ACT_GOTO;
}

static void edit(struct gui_state *g, int act) {
    char now[16];
    if(act == ACT_COPY) copy(g);
    if(act == ACT_GOTO) {
        snprintf(now, sizeof(now), "0x%02X", g->charmap.selected);
        dialog_input(g, gui_app_act(GUI_CHARMAP, ANS_GOTO), "Go to code", "A code: 247, 0xF7 or U+224B", now);
    }
}

static void answer(struct gui_state *g, int act, int button, const char *text, const char *target) {
    (void)target;
    if(act == ANS_GOTO && button == 0) go_to(g, text);
}

static void opened(struct gui_state *g) {
    g->charmap.click_code = -1;
    g->charmap.status[0] = 0;
}

static bool key(struct gui_state *g, int key) {
    int s = g->charmap.selected;
    switch(key) {
    case GUI_LEFT: if(s > 0) s--; break;
    case GUI_RIGHT: if(s < 255) s++; break;
    case GUI_UP: if(s >= 16) s -= 16; break;
    case GUI_DOWN: if(s < 240) s += 16; break;
    case GUI_HOME: s &= 0xF0; break;                    /* the row's start and end */
    case GUI_END: s |= 0x0F; break;
    case GUI_PGUP: s &= 0x0F; break;                    /* the column's top and bottom */
    case GUI_PGDN: s |= 0xF0; break;
    case '\r': case '\n': copy(g); return true;
    default:
        if(key < 32 || key > 126) return false;         /* Esc closes the window, Tab goes on */
        s = key;                                        /* a key typed: its own character */
    }
    choose(g, s);
    return true;
}

static void press(struct gui_state *g, int x, int y) {
    struct gui_charmap *m = &g->charmap;
    int code;
    if(gui_inside(x, y, PX, COPY_Y, 96, 26)) { copy(g); return; }
    if(x < GX || y < GY || x >= GX + 16 * CW || y >= GY + 16 * CH) return;
    code = (y - GY) / CH * 16 + (x - GX) / CW;
    choose(g, code);
    if(gui_double(&m->click_ms, &m->click_code, g->now, code)) copy(g);
}

/* what is special about a character, in two short lines */
static void notes(int code, const char **a, const char **b) {
    static const char *const ascii[] = { "NUL", "^A", "^B", "^C", "^D", "^E", "^F", "^G", "^H", "^I", "^J", "^K", "^L",
                                         "^M", "^N", "^O", "^P", "^Q", "^R", "^S", "^T", "^U", "^V", "^W", "^X", "^Y",
                                         "^Z", "^[", "^\\", "^]", "^^", "^_" };
    *a = *b = "";
    if(code == 0) { *a = "The empty cell:"; *b = "nothing to copy"; }
    else if(code < 32) { *a = ascii[code]; *b = "copies the symbol"; }
    else if(code == 127) { *a = "DEL"; *b = "copies the symbol"; }
    else if(code == ' ') *a = "Space";
    else if(code == 0xFF) *a = "No-break space";
    else if(code == 0x10 || code == 0xF7 || code == 0xF9) { *a = "One of SurfOS's"; *b = "own characters"; }
}

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_CHARMAP];
    const struct gui_charmap *m = &g->charmap;
    bool focused = g->focus == GUI_CHARMAP && !g->dialog.kind && !g->menu;
    char b[GUI_STATUS + 16], u[CP437_UTF8_MAX];
    const char *a, *z;
    int x0 = w->x, y0 = w->y, sel = m->selected & 0xFF, i, x, y, n, sy = w->h - STATUS_H - 1;
    for(i = 0; i < 16; i++) {                           /* the code's low digit above, its high digit beside */
        snprintf(b, sizeof(b), "%X", i);
        gd_text(d, x0 + GX + i * CW + 5, y0 + GY - 18, b, MUTED);
        gd_text(d, x0 + GX - 16, y0 + GY + i * CH + 1, b, MUTED);
    }
    gd_rect(d, x0 + GX, y0 + GY, 16 * CW + 1, 16 * CH + 1, LINE);
    for(i = 0; i < 256; i++) {
        x = x0 + GX + i % 16 * CW;
        y = y0 + GY + i / 16 * CH;
        gd_rect(d, x + 1, y + 1, CW - 1, CH - 1, i == sel ? (focused ? TEAL : MUTED) : SHEET);
        gd_glyph(d, x + 5, y + 1, (u8)i, i == sel ? PAPER : INK, 1);
    }
    x = x0 + PX + (PW - 66) / 2;                        /* the chosen one, large */
    gd_rect(d, x, y0 + BOX_Y, 66, 106, LINE);
    gd_rect(d, x + 1, y0 + BOX_Y + 1, 64, 104, SHEET);
    gd_glyph(d, x + 9, y0 + BOX_Y + 5, (u8)sel, INK, BIG);
    x = x0 + PX;
    y = y0 + INFO_Y;
    gd_text(d, x, y, "Code", MUTED);
    snprintf(b, sizeof(b), "%d", sel);
    gd_text(d, x + 64, y, b, INK);
    gd_text(d, x, y + 18, "Hex", MUTED);
    snprintf(b, sizeof(b), "0x%02X", sel);
    gd_text(d, x + 64, y + 18, b, INK);
    gd_text(d, x, y + 36, "Unicode", MUTED);
    snprintf(b, sizeof(b), "U+%04X", cp437_unicode[sel]);
    gd_text(d, x + 64, y + 36, b, INK);
    gd_text(d, x, y + 54, "UTF-8", MUTED);
    n = charmap_utf8(sel, u);
    if(n == 1) snprintf(b, sizeof(b), "%02X", (u8)u[0]);
    else if(n == 2) snprintf(b, sizeof(b), "%02X %02X", (u8)u[0], (u8)u[1]);
    else if(n == 3) snprintf(b, sizeof(b), "%02X %02X %02X", (u8)u[0], (u8)u[1], (u8)u[2]);
    else strlcpy(b, "none", sizeof(b));
    gd_text(d, x + 64, y + 54, b, INK);
    notes(sel, &a, &z);
    gd_text(d, x, y + 80, a, MUTED);
    gd_text(d, x, y + 98, z, MUTED);
    gd_button(d, x, y0 + COPY_Y, 96, 26, "Copy", sel ? TEAL : BAND, sel ? PAPER : MUTED);
    gd_rect(d, x0 + 1, y0 + sy, w->w - 2, STATUS_H, BAND);
    gd_rect(d, x0 + 1, y0 + sy, w->w - 2, 1, LINE);
    gd_name(d, x0 + 10, y0 + sy + 3, m->status[0] ? m->status : "Enter, Ctrl+C or a double-click copies it", (w->w - 20) / 8, INK);
}

const struct gui_app gui_app_charmap = {
    .title = "Character Map", .icon = ICON_CHARMAP, .shortcut = "", .w = 460, .h = 360,
    .resizable = false, .arrows = true, .keys = false,
    .opened = opened, .render = render, .key = key, .press = press, .answer = answer,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .can_edit = can_edit, .edit = edit, .goto_label = "Go to code\xF9",
};
