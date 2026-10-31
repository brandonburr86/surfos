/*
SurfOS user program: wcharmap
--------------------
File: charmap.c Date: 10/26/26
--------------------
The Character Map's model (charmap.h), ported from the desktop's Character Map (shell/guichar.c):
the console's 256 characters (code page 437 with SurfOS's three, driver/cp437.c, which libsurf
builds) in a grid of 16 by 16. The chosen one's UTF-8 is what Copy (Enter, Ctrl+C, a double-click,
the button) puts on the desktop's clipboard, so it pastes into Notepad, whose text is UTF-8, and
into the Terminal. Codes 1 to 31 and 127 are the graphic characters the console draws there
(cp437_utf8() gives U+263A for 1, say), never control characters, and that is what is copied; 0
is the console's empty cell, which copies nothing. Go to code (Ctrl+L) opens a band of the
window's own, where the desktop's dialog was: it takes 247, 0xF7, U+224B or one character, and
only what it says it takes. No system calls: main.c draws and talks to the desktop.
*/

#include <stdio.h>
#include <string.h>
#include <surfos/cp437.h>
#include "charmap.h"

#define BAND_H 48                       /* Go to code's band, over the status bar */

void charmap_layout(int win_h, struct charmap_layout *l) {
    l->sy = win_h - CHARMAP_STATUS_H - 1;
    l->ch = l->sy >= 50 + 16 * 17 + 1 + 4 ? 17 : 16;
    l->gy = l->ch == 17 ? 50 : 45;
    l->head_y = l->gy - (l->ch == 17 ? 18 : 16);
    l->copy_y = l->sy - 8 - CHARMAP_COPY_H;
    l->band_y = win_h - 1 - BAND_H;
    l->button_y = l->band_y + 22;
    l->field_x = 10;
    l->field_w = 200;
    l->go_x = l->field_x + l->field_w + 10;
    l->cancel_x = l->go_x + 64 + 8;
}

void charmap_init(struct wcharmap *m, int win_h) {
    memset(m, 0, sizeof(*m));
    m->win_h = win_h;
    m->click_code = -1;
}

int charmap_utf8(int code, char *out) {
    if(code <= 0 || code > 255) return 0;
    return cp437_utf8((unsigned char)code, out);
}

void charmap_bytes(int code, char *out, int n) {
    char u[CP437_UTF8_MAX];
    int len = charmap_utf8(code, u);
    if(len == 1) snprintf(out, (size_t)n, "%02X", (unsigned char)u[0]);
    else if(len == 2) snprintf(out, (size_t)n, "%02X %02X", (unsigned char)u[0], (unsigned char)u[1]);
    else if(len == 3) snprintf(out, (size_t)n, "%02X %02X %02X", (unsigned char)u[0], (unsigned char)u[1], (unsigned char)u[2]);
    else snprintf(out, (size_t)n, "none");
}

static void status(struct wcharmap *m, const char *msg) {
    snprintf(m->status, sizeof(m->status), "%s", msg);
}

static void choose(struct wcharmap *m, int code) {
    m->selected = code < 0 ? 0 : code > 255 ? 255 : code;
    m->status[0] = 0;
}

int charmap_copy(struct wcharmap *m, char *out, int n) {
    char u[CP437_UTF8_MAX + 1], msg[CHARMAP_STATUS];
    int code = m->selected, len = charmap_utf8(code, u);
    if(!len) { status(m, "0x00 is the console's empty cell: nothing to copy"); return 0; }
    if(n < len) return 0;
    memcpy(out, u, (size_t)len);
    u[len] = 0;
    if(code == ' ' || code == 0xFF) snprintf(msg, sizeof(msg), "Copied a %sspace: Ctrl+V pastes it", code == ' ' ? "" : "no-break ");
    else snprintf(msg, sizeof(msg), "Copied %s (%d byte%s of UTF-8): Ctrl+V pastes it", u, len, len == 1 ? "" : "s");
    status(m, msg);
    return len;
}

static int blank(const char *s) {
    while(*s == ' ') s++;
    return !*s;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* the length of the UTF-8 character at s (avail bytes there): 1 for a byte that begins none, as the desktop's drawing
   kit reads it (shell/guidraw.c's gd_char_len()) */
static unsigned char_len(const char *s, unsigned avail) {
    unsigned char c = (unsigned char)s[0], lo = 0x80, hi = 0xBF;
    unsigned need = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 1, k;
    if(need == 1 || avail < need) return 1;
    if(c == 0xE0) lo = 0xA0;
    if(c == 0xED) hi = 0x9F;
    if(c == 0xF0) lo = 0x90;
    if(c == 0xF4) hi = 0x8F;
    if((unsigned char)s[1] < lo || (unsigned char)s[1] > hi) return 1;
    for(k = 2; k < need; k++) if(((unsigned char)s[k] & 0xC0) != 0x80) return 1;
    return need;
}

int charmap_parse(const char *s, unsigned *u) {
    unsigned v = 0, len, k;
    int n = 0, code;
    while(*s == ' ') s++;
    *u = 0;
    if((s[0] == 'U' || s[0] == 'u') && s[1] == '+') {
        for(s += 2; n < 7 && hexval(*s) >= 0; s++, n++) v = v * 16 + (unsigned)hexval(*s);
        if(!n || n > 6 || !blank(s)) return -1;
        *u = v;
        code = v <= 0xFFFF ? cp437_from_unicode(v) : -1;
        return code < 0 ? -2 : code;
    }
    if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for(s += 2; n < 3 && hexval(*s) >= 0; s++, n++) v = v * 16 + (unsigned)hexval(*s);
        return n && n <= 2 && blank(s) ? (int)v : -1;
    }
    if(*s >= '0' && *s <= '9') {
        for(; n < 4 && *s >= '0' && *s <= '9'; s++, n++) v = v * 10 + (unsigned)(*s - '0');
        return n <= 3 && v <= 255 && blank(s) ? (int)v : -1;
    }
    if(!*s) return -1;
    len = char_len(s, (unsigned)strlen(s));            /* one character: where the console has it */
    if(!blank(s + len)) return -1;
    if(len == 1) return (unsigned char)*s;              /* ASCII, or a byte that is not UTF-8: the 437 character it is */
    v = (unsigned char)s[0] & (0x7F >> len);
    for(k = 1; k < len; k++) v = v << 6 | ((unsigned char)s[k] & 0x3F);
    *u = v;
    code = cp437_from_unicode(v);
    return code < 0 ? -2 : code;
}

static void go_to(struct wcharmap *m) {
    char msg[CHARMAP_STATUS];
    unsigned u;
    int code = charmap_parse(m->go, &u);
    m->asking = 0;
    if(code >= 0) { choose(m, code); return; }
    if(code == -2) snprintf(msg, sizeof(msg), "U+%04X is not one of the console's characters", u);
    else snprintf(msg, sizeof(msg), "Not a code: 0-255, 0x00-0xFF, U+224B or a character");
    status(m, msg);
}

/* Go to code's band: what is typed there, byte by byte as the desktop's text boxes take it (the bytes of a character
   typed as UTF-8 come one by one); the text put there first is replaced by the first key */
static int band_key(struct wcharmap *m, int key) {
    size_t n = strlen(m->go);
    if(key == 27) { m->asking = 0; return CHARMAP_DRAW; }   /* Esc: nothing */
    if(key == '\r' || key == '\n') { go_to(m); return CHARMAP_DRAW; }
    if(key == 8 || key == 127) {                        /* Backspace: the last byte, or all of what was put there */
        if(m->fresh) m->go[0] = 0;
        else if(n) {
            while(n > 1 && ((unsigned char)m->go[n - 1] & 0xC0) == 0x80) n--;   /* a character's bytes */
            m->go[n - 1] = 0;
        }
        m->fresh = 0;
        return CHARMAP_DRAW;
    }
    if(key < 32 || key > 255) return 0;                /* the arrows and the rest: nothing here */
    if(m->fresh) { m->go[0] = 0; n = 0; m->fresh = 0; }
    if(n + 1 < sizeof(m->go)) { m->go[n] = (char)key; m->go[n + 1] = 0; }
    return CHARMAP_DRAW;
}

int charmap_key(struct wcharmap *m, int key) {
    int s = m->selected;
    if(m->asking) return band_key(m, key);
    switch(key) {
    case CHARMAP_LEFT: if(s > 0) s--; break;
    case CHARMAP_RIGHT: if(s < 255) s++; break;
    case CHARMAP_UP: if(s >= 16) s -= 16; break;
    case CHARMAP_DOWN: if(s < 240) s += 16; break;
    case CHARMAP_HOME: s &= 0xF0; break;                /* the row's start and end */
    case CHARMAP_END: s |= 0x0F; break;
    case CHARMAP_PGUP: s &= 0x0F; break;                /* the column's top and bottom */
    case CHARMAP_PGDN: s |= 0xF0; break;
    case '\r': case '\n': case 3: return CHARMAP_COPY;  /* Enter, Ctrl+C (Edit > Copy) */
    case 12:                                            /* Ctrl+L: Go to code, the chosen one's code put there */
        snprintf(m->go, sizeof(m->go), "0x%02X", s);
        m->asking = m->fresh = 1;
        return CHARMAP_DRAW;
    case 27: case 23: return CHARMAP_CLOSE;             /* Esc, Ctrl+W */
    default:
        if(key < 32 || key > 126) return 0;
        s = key;                                        /* a key typed: its own character */
    }
    choose(m, s);
    return CHARMAP_DRAW;
}

static int inside(int x, int y, int bx, int by, int bw, int bh) {
    return x >= bx && y >= by && x < bx + bw && y < by + bh;
}

int charmap_press(struct wcharmap *m, int x, int y, unsigned now_ms) {
    struct charmap_layout l;
    int code;
    charmap_layout(m->win_h, &l);
    if(m->asking) {                                     /* the band has the pointer too: its buttons */
        if(inside(x, y, l.go_x, l.button_y, 64, 22)) { go_to(m); return CHARMAP_DRAW; }
        if(inside(x, y, l.cancel_x, l.button_y, 80, 22)) { m->asking = 0; return CHARMAP_DRAW; }
        return 0;
    }
    if(inside(x, y, CHARMAP_PX, l.copy_y, CHARMAP_COPY_W, CHARMAP_COPY_H)) return CHARMAP_COPY;
    if(x < CHARMAP_GX || y < l.gy || x >= CHARMAP_GX + 16 * CHARMAP_CW || y >= l.gy + 16 * l.ch || y >= l.sy) return 0;
    code = (y - l.gy) / l.ch * 16 + (x - CHARMAP_GX) / CHARMAP_CW;
    choose(m, code);
    if(code == m->click_code && now_ms - m->click_ms <= CHARMAP_DOUBLE_MS) {    /* a double-click copies it */
        m->click_code = -1;
        return CHARMAP_COPY;
    }
    m->click_ms = now_ms;
    m->click_code = code;
    return CHARMAP_DRAW;
}

void charmap_notes(int code, const char **a, const char **b) {
    static const char *const ascii[] = { "NUL", "^A", "^B", "^C", "^D", "^E", "^F", "^G", "^H", "^I", "^J", "^K", "^L",
                                         "^M", "^N", "^O", "^P", "^Q", "^R", "^S", "^T", "^U", "^V", "^W", "^X", "^Y",
                                         "^Z", "^[", "^\\", "^]", "^^", "^_" };
    *a = *b = "";
    if(code == 0) { *a = "The empty cell:"; *b = "nothing to copy"; }
    else if(code == 0x10 || code == 0xF7 || code == 0xF9) { *a = "One of SurfOS's"; *b = "own characters"; }   /* 0x10 too */
    else if(code > 0 && code < 32) { *a = ascii[code]; *b = "copies the symbol"; }
    else if(code == 127) { *a = "DEL"; *b = "copies the symbol"; }
    else if(code == ' ') *a = "Space";
    else if(code == 0xFF) *a = "No-break space";
}

void charmap_status(const struct wcharmap *m, char *out, int n) {
    snprintf(out, (size_t)n, "%s", m->status[0] ? m->status : "Enter, Ctrl+C or a double-click copies it");
}
