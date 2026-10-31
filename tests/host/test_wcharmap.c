/* Host tests for the Character Map as a program (user/wcharmap/charmap.c, its model; stage C12 of
   docs/superpowers/plans/2026-10-25-app-windows.md), with the console's real code page 437 (driver/cp437.c, which
   libsurf builds). What the desktop's Character Map was tested for (tests/host/test_guitools.c until 10/26/26) holds
   for the program: each character copies as the UTF-8 that pastes back as itself, the keys, a click and a
   double-click, the Copy button, the empty cell, the status bar's lines, what is special about a character, the
   layout at 360 pixels and on a small screen; and Go to code, now a band of the window's own: what it takes and
   only that, typed, Backspace, Enter, Esc, its buttons. Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/cp437.h>
#include "charmap.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

static struct wcharmap m;
static unsigned now = 10000;

static int key(int k) { return charmap_key(&m, k); }
static int click(int x, int y) { return charmap_press(&m, x, y, now); }
static void type(const char *s) { while(*s) key((unsigned char)*s++); }

/* the cell of code c, its middle, for a window of win_h */
static void cell(int c, int *x, int *y) {
    struct charmap_layout l;
    charmap_layout(m.win_h, &l);
    *x = CHARMAP_GX + c % 16 * CHARMAP_CW + 8;
    *y = l.gy + c / 16 * l.ch + 8;
}

static int copied(const char *want, int len, const char *status) {
    char out[CP437_UTF8_MAX];
    int n = charmap_copy(&m, out, sizeof(out));
    return n == len && !memcmp(out, want, (size_t)len) && !strcmp(m.status, status);
}

/* a code point from its UTF-8 (1 to 3 bytes) */
static unsigned decode(const char *u, int n) {
    if(n == 1) return (unsigned char)u[0];
    if(n == 2) return ((unsigned char)u[0] & 0x1F) << 6 | ((unsigned char)u[1] & 0x3F);
    return ((unsigned char)u[0] & 0x0F) << 12 | ((unsigned char)u[1] & 0x3F) << 6 | ((unsigned char)u[2] & 0x3F);
}

static void codes(void) {
    char u[CP437_UTF8_MAX], v[CP437_UTF8_MAX], b[16];
    int c, n, all = 1;
    for(c = 1; c < 256; c++) {                          /* each pastes as itself: Notepad shows the same glyph */
        n = charmap_utf8(c, u);
        if(n < 1 || n != cp437_utf8((unsigned char)c, v) || memcmp(u, v, (size_t)n) || cp437_from_unicode(decode(u, n)) != c)
            all = 0;
    }
    CHECK(all);
    CHECK(charmap_utf8(0, u) == 0 && charmap_utf8(256, u) == 0 && charmap_utf8(-1, u) == 0);
    CHECK(charmap_utf8(0xF7, u) == 3 && !memcmp(u, "\xE2\x89\x8B", 3));          /* ≋ U+224B */
    CHECK(charmap_utf8(0x01, u) == 3 && !memcmp(u, "\xE2\x98\xBA", 3));          /* ☺ U+263A, not ^A */
    CHECK(charmap_utf8(0x14, u) == 2 && !memcmp(u, "\xC2\xB6", 2));              /* ¶ */
    CHECK(charmap_utf8(0x7F, u) == 3 && !memcmp(u, "\xE2\x8C\x82", 3));          /* ⌂ */
    CHECK(charmap_utf8('A', u) == 1 && u[0] == 'A');
    CHECK(charmap_utf8(0xDB, u) == 3 && !memcmp(u, "\xE2\x96\x88", 3));          /* █ */
    CHECK(charmap_utf8(0xFF, u) == 2 && !memcmp(u, "\xC2\xA0", 2));              /* no-break space */
    charmap_bytes(0xF7, b, sizeof(b));
    CHECK(!strcmp(b, "E2 89 8B"));
    charmap_bytes(0x14, b, sizeof(b));
    CHECK(!strcmp(b, "C2 B6"));
    charmap_bytes('A', b, sizeof(b));
    CHECK(!strcmp(b, "41"));
    charmap_bytes(0, b, sizeof(b));
    CHECK(!strcmp(b, "none"));
}

static void keys_and_clicks(void) {
    char line[CHARMAP_STATUS];
    struct charmap_layout l;
    int x, y;
    charmap_init(&m, 360);
    charmap_layout(m.win_h, &l);
    charmap_status(&m, line, sizeof(line));
    CHECK(m.selected == 0 && !strcmp(line, "Enter, Ctrl+C or a double-click copies it"));
    /* the keys */
    key(CHARMAP_RIGHT); key(CHARMAP_RIGHT); key(CHARMAP_DOWN);
    CHECK(m.selected == 18);
    key(CHARMAP_END);
    CHECK(m.selected == 31);
    key(CHARMAP_HOME);
    CHECK(m.selected == 16);
    key(CHARMAP_PGDN);
    CHECK(m.selected == 0xF0);
    key(CHARMAP_DOWN); key(CHARMAP_PGUP);
    CHECK(m.selected == 0);
    key(CHARMAP_LEFT); key(CHARMAP_UP);
    CHECK(m.selected == 0);
    CHECK(key('A') == CHARMAP_DRAW && m.selected == 'A');
    key(CHARMAP_PGDN); key(CHARMAP_END); key(CHARMAP_RIGHT); key(CHARMAP_DOWN);
    CHECK(m.selected == 255);
    CHECK(key(9) == 0 && key(0x108) == 0 && m.selected == 255);   /* Tab, Delete: nothing */
    /* a click chooses */
    cell(0xF7, &x, &y);
    CHECK(click(x, y) == CHARMAP_DRAW && m.selected == 0xF7);
    /* Copy: Ctrl+C (Edit > Copy) and Enter ask for it; its UTF-8, and the status bar says so */
    CHECK(key(3) == CHARMAP_COPY && key('\r') == CHARMAP_COPY);
    CHECK(copied("\xE2\x89\x8B", 3, "Copied \xE2\x89\x8B (3 bytes of UTF-8): Ctrl+V pastes it"));
    charmap_status(&m, line, sizeof(line));
    CHECK(!strcmp(line, m.status));
    key(CHARMAP_RIGHT);                                 /* another choice: the status bar's default again */
    CHECK(!m.status[0]);
    /* a double-click copies; one too slow does not */
    cell(0xDB, &x, &y);
    now += 1000;
    CHECK(click(x, y) == CHARMAP_DRAW);
    now += CHARMAP_DOUBLE_MS;
    CHECK(click(x, y) == CHARMAP_COPY && m.selected == 0xDB);
    CHECK(copied("\xE2\x96\x88", 3, "Copied \xE2\x96\x88 (3 bytes of UTF-8): Ctrl+V pastes it"));   /* █ */
    now += 100;
    CHECK(click(x, y) == CHARMAP_DRAW);                 /* a third: a first again */
    now += CHARMAP_DOUBLE_MS + 1;
    CHECK(click(x, y) == CHARMAP_DRAW);
    cell(0xDC, &x, &y);
    now += 10;
    CHECK(click(x, y) == CHARMAP_DRAW);                 /* another cell: no double-click */
    /* the empty cell copies nothing, and says so */
    now += 1000;
    CHECK(click(CHARMAP_GX + 5, l.gy + 5) == CHARMAP_DRAW && m.selected == 0);
    CHECK(copied("", 0, "0x00 is the console's empty cell: nothing to copy"));
    /* the Copy button, above the status bar; a space and the no-break space say what they are */
    key(' ');
    CHECK(click(CHARMAP_PX + 10, l.copy_y + 10) == CHARMAP_COPY && copied(" ", 1, "Copied a space: Ctrl+V pastes it"));
    key(CHARMAP_PGDN); key(CHARMAP_END);
    CHECK(copied("\xC2\xA0", 2, "Copied a no-break space: Ctrl+V pastes it"));
    /* outside the grid and the button: nothing */
    CHECK(click(CHARMAP_GX - 3, l.gy + 5) == 0 && click(CHARMAP_GX + 16 * CHARMAP_CW, l.gy + 5) == 0 &&
          click(CHARMAP_GX + 5, l.gy - 1) == 0 && click(CHARMAP_GX + 5, l.gy + 16 * l.ch) == 0 && m.selected == 0xFF);
    /* Esc and Ctrl+W close the window */
    CHECK(key(27) == CHARMAP_CLOSE && key(23) == CHARMAP_CLOSE);
}

static void notes(void) {
    const char *a, *b;
    charmap_notes(0, &a, &b);
    CHECK(!strcmp(a, "The empty cell:") && !strcmp(b, "nothing to copy"));
    charmap_notes(0x10, &a, &b);                        /* SurfOS's own three */
    CHECK(!strcmp(a, "One of SurfOS's") && !strcmp(b, "own characters"));
    charmap_notes(0xF7, &a, &b);
    CHECK(!strcmp(a, "One of SurfOS's"));
    charmap_notes(0xF9, &a, &b);
    CHECK(!strcmp(a, "One of SurfOS's"));
    charmap_notes(1, &a, &b);
    CHECK(!strcmp(a, "^A") && !strcmp(b, "copies the symbol"));
    charmap_notes(0x1B, &a, &b);
    CHECK(!strcmp(a, "^["));
    charmap_notes(127, &a, &b);
    CHECK(!strcmp(a, "DEL") && !strcmp(b, "copies the symbol"));
    charmap_notes(' ', &a, &b);
    CHECK(!strcmp(a, "Space") && !b[0]);
    charmap_notes(0xFF, &a, &b);
    CHECK(!strcmp(a, "No-break space"));
    charmap_notes('A', &a, &b);
    CHECK(!a[0] && !b[0]);
}

/* Go to code (Ctrl+L): a band of the window's own, the chosen code put there first */
static void go_to(void) {
    static const struct { const char *text; int code; const char *status; } cases[] = {
        { "4294967361", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },   /* 2^32 + 65 */
        { "U+100000041", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "U+41zz", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "65 66", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "0x", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "0x123", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "U+", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "1000", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "300", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { " ", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "AB", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "\xE2\x82\xAC", -2, "U+20AC is not one of the console's characters" },       /* € */
        { "\xF0\x9F\x98\x80", -2, "U+1F600 is not one of the console's characters" },  /* an emoji */
        { "U+FFFF", -2, "U+FFFF is not one of the console's characters" },
        { "U+1234", -2, "U+1234 is not one of the console's characters" },
        { "007", 7, "" }, { " 0x41 ", 'A', "" }, { "u+2588", 0xDB, "" }, { "U+00e9", 0x82, "" }, { "0x41", 0x41, "" },
        { "247", 247, "" }, { "\xC3\xA9", 0x82, "" }, { "9", 9, "" }, { "255", 255, "" }, { "0xff", 255, "" },
        { "~", '~', "" }, { "Z", 'Z', "" }, { "\xE2\x89\x8B", 0xF7, "" },
        { "U+0010FFFF", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "U+10FFFF", -2, "U+10FFFF is not one of the console's characters" },
    };
    struct charmap_layout l;
    unsigned u;
    int i, ok = 1, x, y;
    charmap_init(&m, 360);
    charmap_layout(m.win_h, &l);
    key(' ');
    CHECK(key(12) == CHARMAP_DRAW && m.asking && m.fresh && !strcmp(m.go, "0x20"));
    type("0x41");                                       /* the first key replaces what was put there */
    CHECK(!strcmp(m.go, "0x41") && !m.fresh);
    CHECK(key('\r') == CHARMAP_DRAW && !m.asking && m.selected == 0x41 && !m.status[0]);
    for(i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        key('Z');
        key(12);
        type(cases[i].text);
        key('\r');
        if(m.asking || m.selected != (cases[i].code >= 0 ? cases[i].code : 'Z') || strcmp(m.status, cases[i].status)) {
            ok = 0;
            printf("Go to code \"%s\": 0x%02X, \"%s\"\n", cases[i].text, m.selected, m.status);
        }
        if(charmap_parse(cases[i].text, &u) != cases[i].code) ok = 0;
    }
    CHECK(ok);
    /* Backspace: all of what was put there, then a character at a time (its bytes together) */
    key('Z');
    key(12);
    key(8);
    CHECK(m.asking && !m.go[0]);
    key('\r');                                          /* empty */
    CHECK(m.selected == 'Z' && !strcmp(m.status, "Not a code: 0-255, 0x00-0xFF, U+224B or a character"));
    key(12);
    key('1');
    type("\xE2\x89\x8B");
    key(8);
    CHECK(!strcmp(m.go, "1"));
    key(127);
    CHECK(!m.go[0]);
    type("U+2588");
    CHECK(key('\n') == CHARMAP_DRAW && m.selected == 0xDB);
    /* while it is open it has the keys: no arrow, no closing, no copying; Esc leaves it, choosing nothing */
    key(12);
    CHECK(key(CHARMAP_LEFT) == 0 && key(3) == 0 && key(23) == 0 && m.asking && m.selected == 0xDB);
    CHECK(key(27) == CHARMAP_DRAW && !m.asking && m.selected == 0xDB);
    /* and the pointer: its Go and Cancel; the grid behind it takes no click */
    key(12);
    type("65");
    cell(0xF7, &x, &y);
    CHECK(click(x, y) == 0 && m.selected == 0xDB && m.asking);
    CHECK(click(l.go_x + 5, l.button_y + 5) == CHARMAP_DRAW && !m.asking && m.selected == 65);
    key(12);
    type("66");
    CHECK(click(l.cancel_x + 5, l.button_y + 5) == CHARMAP_DRAW && !m.asking && m.selected == 65);
    /* a long text stops at its room */
    key(12);
    type("12345678901234567890123456789");
    CHECK(strlen(m.go) == CHARMAP_GO_MAX - 1);
    key(27);
}

/* the layout for the window's height: 360 pixels, and less on a small screen, where the cells are 16 high so that
   the status bar covers none of them; the band over the status bar */
static void layouts(void) {
    static const int heights[] = { 360, 346, 326 };
    struct charmap_layout l;
    int i, x, y;
    charmap_layout(360, &l);
    CHECK(l.ch == 17 && l.gy == 50 && l.head_y == 32 && l.sy == 337 && l.copy_y == 303);
    for(i = 0; i < 3; i++) {
        charmap_layout(heights[i], &l);
        CHECK(l.gy + 16 * l.ch < l.sy && l.copy_y + CHARMAP_COPY_H <= l.sy);   /* the grid's end and the button whole */
        CHECK(l.copy_y > CHARMAP_INFO_Y + 98 + 16);     /* below the panel's notes */
        CHECK(l.band_y + 48 == heights[i] - 1 && l.button_y + 22 <= heights[i] - 1);
        CHECK(l.cancel_x + 80 <= CHARMAP_W);
    }
    charmap_layout(326, &l);
    CHECK(l.ch == 16 && l.gy == 45 && l.head_y == 29 && l.sy == 303 && l.copy_y == 269);
    charmap_init(&m, 326);
    CHECK(click(CHARMAP_GX + 7 * 17 + 8, l.sy + 5) == 0 && m.selected == 0);   /* the status bar chooses nothing */
    CHECK(click(CHARMAP_GX + 7 * 17 + 8, l.gy + 15 * l.ch + 8) == CHARMAP_DRAW && m.selected == 0xF7);
    CHECK(click(CHARMAP_GX + 15 * 17 + 8, l.gy + 15 * l.ch + 14) == CHARMAP_DRAW && m.selected == 0xFF);
    now += 1000;
    CHECK(click(CHARMAP_GX + 2 * 17 + 8, l.gy + 14 * l.ch + 15) == CHARMAP_DRAW && m.selected == 0xE2);  /* row E's last */
    CHECK(click(CHARMAP_PX + 10, l.copy_y + 10) == CHARMAP_COPY && copied("\xCE\x93", 2,
          "Copied \xCE\x93 (2 bytes of UTF-8): Ctrl+V pastes it"));                  /* Γ */
    cell(0x41, &x, &y);
    CHECK(y < l.sy);
}

int main(void) {
    codes();
    keys_and_clicks();
    notes();
    go_to();
    layouts();
    printf("test_wcharmap: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
