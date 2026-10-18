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
10/18/26: laid out for the height the window gets (on a 640x400 screen the status bar covered the last
row); Go to code takes only what it says it takes.
*/
#include "gui_tools.h"
#include <sys/cp437.h>
#include <string.h>
#include <stdio.h>

#define SHEET 0xFFFDF5                  /* the grid's background, as Notepad's text */
#define GX 26                           /* the grid's left (window-relative), and each cell's width */
#define CW 17
#define PX 312                          /* the panel of the chosen character, beside the grid */
#define PW 138
#define BOX_Y 34                        /* the chosen character, large */
#define BIG 6
#define INFO_Y 148                      /* its code, code point and bytes, and what is special about it */
#define ANS_GOTO 1                      /* Go to code's answer (gui_app_act) */

/* 10/18/26: the grid's top and its cells' height, the code's digits above it, the status bar, the Copy button: for the
   window's height, 360 pixels, or 326 on a 640x400 screen (gui_init's room less 20), where the cells are a pixel lower
   so that the status bar covers none of them */
struct map_layout { int gy, ch, head_y, sy, copy_y; };

static void layout(const struct gui_state *g, struct map_layout *l) {
    const struct gui_window *w = &g->win[GUI_CHARMAP];
    l->sy = w->h - STATUS_H - 1;
    l->ch = l->sy >= 50 + 16 * 17 + 1 + 4 ? 17 : 16;
    l->gy = l->ch == 17 ? 50 : 45;
    l->head_y = l->gy - (l->ch == 17 ? 18 : 16);
    l->copy_y = l->sy - 8 - 26;
}

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

static bool blank(const char *s) {
    while(*s == ' ') s++;
    return !*s;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* the code a text names: 247 (three digits at most), 0xF7 (two hex digits), U+224B (six at most), or one character
   (not a digit or a space), with spaces around it: 0-255, -1 when it names none, -2 for a code point the console
   lacks (*u: it) */
static int parse_code(const char *s, u32 *u) {
    u32 v = 0, len, k;
    int n = 0, code;
    while(*s == ' ') s++;
    *u = 0;
    if((s[0] == 'U' || s[0] == 'u') && s[1] == '+') {
        for(s += 2; n < 7 && hexval(*s) >= 0; s++, n++) v = v * 16 + (u32)hexval(*s);
        if(!n || n > 6 || !blank(s)) return -1;
        *u = v;
        code = v <= 0xFFFF ? cp437_from_unicode(v) : -1;
        return code < 0 ? -2 : code;
    }
    if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for(s += 2; n < 3 && hexval(*s) >= 0; s++, n++) v = v * 16 + (u32)hexval(*s);
        return n && n <= 2 && blank(s) ? (int)v : -1;
    }
    if(*s >= '0' && *s <= '9') {
        for(; n < 4 && *s >= '0' && *s <= '9'; s++, n++) v = v * 10 + (u32)(*s - '0');
        return n <= 3 && v <= 255 && blank(s) ? (int)v : -1;
    }
    if(!*s) return -1;
    len = gd_char_len(s, strlen(s));                    /* one character: where the console has it */
    if(!blank(s + len)) return -1;
    if(len == 1) return (u8)*s;                         /* ASCII, or a byte that is not UTF-8: the 437 character it is */
    v = (u8)s[0] & (0x7F >> len);
    for(k = 1; k < len; k++) v = v << 6 | ((u8)s[k] & 0x3F);
    *u = v;
    code = cp437_from_unicode(v);
    return code < 0 ? -2 : code;
}

static void go_to(struct gui_state *g, const char *text) {
    char msg[GUI_STATUS];
    u32 u;
    int code = parse_code(text, &u);
    if(code >= 0) { choose(g, code); return; }
    if(code == -2) snprintf(msg, sizeof(msg), "U+%04X is not one of the console's characters", u);
    else strlcpy(msg, "Not a code: 0-255, 0x00-0xFF, U+224B or a character", sizeof(msg));
    status(g, msg);
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
    struct map_layout l;
    int code;
    layout(g, &l);
    if(gui_inside(x, y, PX, l.copy_y, 96, 26)) { copy(g); return; }
    if(x < GX || y < l.gy || x >= GX + 16 * CW || y >= l.gy + 16 * l.ch || y >= l.sy) return;
    code = (y - l.gy) / l.ch * 16 + (x - GX) / CW;
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
    else if(code == 0x10 || code == 0xF7 || code == 0xF9) { *a = "One of SurfOS's"; *b = "own characters"; }   /* 0x10 too */
    else if(code < 32) { *a = ascii[code]; *b = "copies the symbol"; }
    else if(code == 127) { *a = "DEL"; *b = "copies the symbol"; }
    else if(code == ' ') *a = "Space";
    else if(code == 0xFF) *a = "No-break space";
}

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_CHARMAP];
    const struct gui_charmap *m = &g->charmap;
    bool focused = g->focus == GUI_CHARMAP && !g->dialog.kind && !g->menu;
    char b[GUI_STATUS + 16], u[CP437_UTF8_MAX];
    const char *a, *z;
    struct map_layout l;
    int x0 = w->x, y0 = w->y, sel = m->selected & 0xFF, i, x, y, n;
    layout(g, &l);
    for(i = 0; i < 16; i++) {                           /* the code's low digit above, its high digit beside */
        snprintf(b, sizeof(b), "%X", i);
        gd_text(d, x0 + GX + i * CW + 5, y0 + l.head_y, b, MUTED);
        gd_text(d, x0 + GX - 16, y0 + l.gy + i * l.ch + 1, b, MUTED);
    }
    gd_rect(d, x0 + GX, y0 + l.gy, 16 * CW + 1, 16 * l.ch + 1, LINE);
    for(i = 0; i < 256; i++) {                          /* (cells of 16 rows: a glyph's last row is on the line below) */
        x = x0 + GX + i % 16 * CW;
        y = y0 + l.gy + i / 16 * l.ch;
        gd_rect(d, x + 1, y + 1, CW - 1, l.ch - 1, i == sel ? (focused ? TEAL : MUTED) : SHEET);
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
    gd_button(d, x, y0 + l.copy_y, 96, 26, "Copy", sel ? TEAL : BAND, sel ? PAPER : MUTED);
    gd_rect(d, x0 + 1, y0 + l.sy, w->w - 2, STATUS_H, BAND);
    gd_rect(d, x0 + 1, y0 + l.sy, w->w - 2, 1, LINE);
    gd_name(d, x0 + 10, y0 + l.sy + 3, m->status[0] ? m->status : "Enter, Ctrl+C or a double-click copies it", (w->w - 20) / 8, INK);
}

const struct gui_app gui_app_charmap = {
    .title = "Character Map", .icon = ICON_CHARMAP, .shortcut = "", .w = 460, .h = 360,
    .resizable = false, .arrows = true, .keys = false,
    .opened = opened, .render = render, .key = key, .press = press, .answer = answer,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .can_edit = can_edit, .edit = edit, .goto_label = "Go to code\xF9",
};
