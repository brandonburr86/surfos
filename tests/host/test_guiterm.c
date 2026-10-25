/* Host tests for the Terminal (track TERM of docs/superpowers/plans/2026-10-17-gui-suite.md), against the
   in-memory file system and system of tests/host/gui_fake.c. Built and run by `make unittest` (unittest-gui).
   10/17/26: the terminal emulator on its own (shell/guivt.c: sequences, the wrap, scrolling, UTF-8, sequences it
   does not know, a new size), then the Terminal on the desktop against the fake session: keys to bytes, the size,
   the scrollback and the selection, copy and paste with a full input queue, the session's end and a new one, a
   session that cannot start, busy and the close question, and every window inside guard pixels at three sizes. */
#include "gui_fake.h"
#include "../../shell/guivt.h"

static struct gui_state g;
static struct gui_vt v;
static const int ids[] = { GUI_TERMINAL };
#define IDS (int)(sizeof(ids) / sizeof(ids[0]))
#define TERM (&g.win[GUI_TERMINAL])
#define TX (TERM->x + 8)                         /* where the text's first cell is on the screen */
#define TY (TERM->y + TITLE_H + 2)

/**** helpers ****/

static void feed(const char *s) { vt_feed(&v, s, (u32)strlen(s)); }

/* the screen's row y of vt as text, blanks at its end left out (cells as the bytes they are) */
static const char *row_of(const struct gui_vt *vt, int y) {
    static char out[GUI_VT_COLS + 1];
    const struct gui_vt_cell *c = vt_line(vt, vt_screen(vt) + y);
    int n = vt->cols;
    while(n > 0 && (c[n - 1].ch == ' ' || !c[n - 1].ch)) n--;
    for(y = 0; y < n; y++) out[y] = c[y].ch ? (char)c[y].ch : ' ';
    out[n] = 0;
    return out;
}

/* line i of those kept as text */
static const char *line_of(const struct gui_vt *vt, int i) {
    return row_of(vt, i - vt_screen(vt));
}

static const struct gui_vt_cell *cell(const struct gui_vt *vt, int x, int y) {
    return &vt_line(vt, vt_screen(vt) + y)[x];
}

static void tick(void) { g.now += 20; gui_tick(&g); }

static void session_says(const char *s) {
    fake_term_feed(s, (u32)strlen(s));
    tick();
}

/* frames until all the session printed is drawn (a frame draws a budget of it) */
static void drain(void) {
    int k;
    for(k = 0; k < 2000 && (fake_term_out_len || g.term.carry_at < g.term.carry_len); k++) tick();
}

static bool typed(const char *want, u32 n) {
    return fake_term_in_len == n && !memcmp(fake_term_in, want, n);
}

static bool clip_is(const char *want) {
    return g.clip_len == strlen(want) && !memcmp(g.clip, want, g.clip_len);
}

static void pointer(int x, int y, unsigned buttons, int wheel) {
    struct pointer_event e;
    memset(&e, 0, sizeof(e));
    e.x = x; e.y = y; e.buttons = buttons; e.wheel = wheel;
    gui_pointer(&g, &e);
}

/* a drag with the left button from one place to another, the button let go there */
static void drag(int x0, int y0, int x1, int y1) {
    pointer(x0, y0, POINTER_LEFT, 0);
    pointer(x1, y1, POINTER_LEFT, 0);
    pointer(x1, y1, 0, 0);
}

/* a desktop of width x height with the Terminal open (Ctrl+T) and its session started */
static void terminal(int width, int height) {
    fake_desktop(&g, width, height);
    gui_key(&g, 20);
    tick();
}

/**** the emulator on its own ****/

/* shell/main.c's ansi command, as it prints, at cols x rows; tools/qemu-usb.py's ANSI_ROWS read back */
static void ansi(int cols, int rows) {
    char b[64];
    int i;
    vt_reset(&v, cols, rows);
    feed("some text before\r\n");
    for(i = 0; i < rows; i++) feed("\r\n");          /* into the scrollback, which stays */
    feed("\033[2J\033[HANSI-TEST top left");
    feed("\033[3;10H\033[7mINVERSE\033[0m normal");
    feed("\033[4;10H\033[1;33;44mYELLOW-ON-BLUE\033[m");
    feed("\033[5;10HKEEP-ERASED\033[5;14H\033[K");
    feed("\033[8;10HA\033[BB\033[2AC\033[3DD\033[5CE");
    feed("\033[11;10H\033[5n\033[?7h\033(B\033[ q\033[38;5;196mUNKNOWN-IGNORED\033[0m");
    snprintf(b, sizeof(b), "\033[%d;%dHZ", rows, cols);
    feed(b);
    snprintf(b, sizeof(b), "\033[13;1H\033[?25l\033[?25hANSI-TEST done, %dx%d\n", cols, rows);
    feed(b);
    CHECK(!strcmp(row_of(&v, 0), "ANSI-TEST top left"));
    CHECK(!strcmp(row_of(&v, 1), ""));
    CHECK(!strcmp(row_of(&v, 2), "         INVERSE normal"));
    CHECK(!strcmp(row_of(&v, 3), "         YELLOW-ON-BLUE"));
    CHECK(!strcmp(row_of(&v, 4), "         KEEP"));
    CHECK(!strcmp(row_of(&v, 6), "         D C   E"));
    CHECK(!strcmp(row_of(&v, 7), "         A"));
    CHECK(!strcmp(row_of(&v, 8), "          B"));
    CHECK(!strcmp(row_of(&v, 10), "         UNKNOWN-IGNORED"));
    snprintf(b, sizeof(b), "ANSI-TEST done, %dx%d", cols, rows);
    CHECK(!strcmp(row_of(&v, 12), b));
    CHECK(cell(&v, cols - 1, rows - 1)->ch == 'Z' && strlen(row_of(&v, rows - 1)) == (size_t)cols);   /* the corner: no scroll */
    CHECK(v.count == rows + 2 && !strcmp(line_of(&v, 0), "some text before"));
    CHECK(cell(&v, 9, 2)->attr == 0x70 && cell(&v, 16, 2)->attr == 0x07);      /* inverse, then plain */
    CHECK(cell(&v, 9, 3)->attr == 0x1E);                                       /* bright yellow on blue */
    CHECK(cell(&v, 9, 10)->attr == 0x07);                                      /* 38;5;196 passed over */
    CHECK(v.y == 13 && v.x == 0 && !v.cursor_off);
}

static void emulator(void) {
    int i;
    /* text, the VT100 wrap, line ends */
    vt_reset(&v, 10, 3);
    CHECK(v.count == 3 && v.x == 0 && v.y == 0 && !strcmp(row_of(&v, 0), ""));
    feed("0123456789");
    CHECK(v.x == 9 && v.y == 0 && v.wrap && !vt_wrapped(&v, 0));
    feed("A");
    CHECK(vt_wrapped(&v, 0) && !strcmp(row_of(&v, 1), "A") && v.x == 1 && v.y == 1);
    vt_reset(&v, 10, 3);
    feed("0123456789\nX");                          /* a full row, then LF: one row, not two */
    CHECK(!strcmp(row_of(&v, 1), "X") && !vt_wrapped(&v, 0));
    vt_reset(&v, 10, 3);
    feed("0123456789\r\nX");
    CHECK(!strcmp(row_of(&v, 1), "X"));
    feed("\rY\nZ");                                 /* CR to the first column; LF to the next row's start */
    CHECK(!strcmp(row_of(&v, 1), "Y") && !strcmp(row_of(&v, 2), "Z") && v.x == 1);
    /* scrolling: the top line goes into the scrollback */
    vt_reset(&v, 10, 3);
    feed("a\nb\nc\nd");
    CHECK(v.count == 4 && !strcmp(line_of(&v, 0), "a") && !strcmp(row_of(&v, 0), "b") && !strcmp(row_of(&v, 2), "d"));
    for(i = 0; i < GUI_VT_LINES + 50; i++) feed("x\n");
    CHECK(v.count == GUI_VT_LINES && v.y == 2);    /* 4 + 1,074 lines made, the oldest 54 gone */
    CHECK(v.dropped == 54 && !strcmp(line_of(&v, 0), "x") && !strcmp(row_of(&v, 1), "x"));
    /* Backspace, Tab */
    vt_reset(&v, 20, 3);
    feed("ab\b\bX");
    CHECK(!strcmp(row_of(&v, 0), "Xb") && v.x == 1);
    feed("\r\tY\t\tZ");
    CHECK(cell(&v, 8, 0)->ch == 'Y' && cell(&v, 19, 0)->ch == 'Z' && v.wrap);   /* Tab stops at the last column */
    feed("\tW");                                    /* and does not move at a pending wrap */
    CHECK(cell(&v, 0, 1)->ch == 'W' && v.y == 1 && v.x == 1);
    feed("V\b \b");                                /* as the stream console erases */
    CHECK(cell(&v, 1, 1)->ch == ' ' && v.x == 1 && !v.wrap);
    feed("\b\b\b");
    CHECK(v.x == 0 && v.y == 1);                    /* not past the first column */
    /* the stream console's erases: BS at a pending wrap, at a row's start */
    vt_reset(&v, 10, 3);
    feed("0123456789\r\033[999C\033[K");
    CHECK(!strcmp(row_of(&v, 0), "012345678") && v.x == 9 && !v.wrap);
    feed("\nab\033[A\033[999C\033[K");
    CHECK(!strcmp(row_of(&v, 0), "012345678") && v.y == 0 && v.x == 9);
    /* moves: clamped, a count of 0 is 1; G */
    vt_reset(&v, 20, 5);
    feed("\033[3;5HP\033[99;99HQ\033[HR\033[0BS\033[2CT\033[9DU\033[7GV");
    CHECK(cell(&v, 4, 2)->ch == 'P' && cell(&v, 19, 4)->ch == 'Q' && cell(&v, 0, 0)->ch == 'R');
    CHECK(cell(&v, 1, 1)->ch == 'S' && cell(&v, 4, 1)->ch == 'T' && cell(&v, 0, 1)->ch == 'U' && cell(&v, 6, 1)->ch == 'V');
    feed("\033[5;1f\033[AW");
    CHECK(cell(&v, 0, 3)->ch == 'W');
    /* erase in display and line, in the colours in force */
    vt_reset(&v, 10, 4);
    feed("aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc\r\ndddddddddd");
    feed("\033[2;5H\033[44m\033[K");
    CHECK(!strcmp(row_of(&v, 1), "bbbb") && cell(&v, 4, 1)->attr == 0x17 && cell(&v, 3, 1)->attr == 0x07);
    feed("\033[0m\033[3;3H\033[1K");
    CHECK(!strcmp(row_of(&v, 2), "   ccccccc"));
    feed("\033[3;3H\033[2K");
    CHECK(!strcmp(row_of(&v, 2), ""));
    feed("\033[1;5H\033[1J");
    CHECK(!strcmp(row_of(&v, 0), "     aaaaa") && !strcmp(row_of(&v, 3), "dddddddddd"));
    feed("\033[4;3H\033[J");
    CHECK(!strcmp(row_of(&v, 3), "dd") && !strcmp(row_of(&v, 0), "     aaaaa"));
    feed("\033[2J");
    for(i = 0; i < 4; i++) CHECK(!strcmp(row_of(&v, i), ""));
    CHECK(v.count == 4);                            /* a clear screen keeps the scrollback as it was */
    /* colours: SGR as the console makes them, and what other terminals send */
    vt_reset(&v, 40, 3);
    feed("\033[31mA\033[1mB\033[7mC\033[27mD\033[22mE\033[42mF\033[39mG\033[49mH\033[0;93;104mI\033[0mJ\033[mK");
    CHECK(cell(&v, 0, 0)->attr == 0x04 && cell(&v, 1, 0)->attr == 0x0C && cell(&v, 2, 0)->attr == 0x40);
    CHECK(cell(&v, 3, 0)->attr == 0x0C && cell(&v, 4, 0)->attr == 0x04 && cell(&v, 5, 0)->attr == 0x24);
    CHECK(cell(&v, 6, 0)->attr == 0x27 && cell(&v, 7, 0)->attr == 0x07 && cell(&v, 8, 0)->attr == 0x1E);
    CHECK(cell(&v, 9, 0)->attr == 0x07 && cell(&v, 10, 0)->attr == 0x07);
    feed("\033[38;2;10;20;30;1mL\033[0m\033[48;5;33mM");
    CHECK(cell(&v, 11, 0)->attr == 0x0F && cell(&v, 12, 0)->attr == 0x07);   /* 1 after the colour's numbers */
    feed("\033[0m\033[1;2;3;4;5;6;7;8;9;10;31mN\033[0m");                     /* more numbers than kept */
    CHECK(cell(&v, 13, 0)->ch == 'N');
    feed("\033[?25l");
    CHECK(v.cursor_off);
    feed("\033[?25h");
    CHECK(!v.cursor_off);
    /* UTF-8 as the console decodes it */
    vt_reset(&v, 40, 3);
    feed("caf\xC3\xA9 \xE2\x89\x8B \xE2\x96\xB8 \xE2\x82\xAC \xC3""A \x80!");
    CHECK(cell(&v, 3, 0)->ch == 0x82 && cell(&v, 5, 0)->ch == 0xF7 && cell(&v, 7, 0)->ch == 0x10);
    CHECK(cell(&v, 9, 0)->ch == '?' && cell(&v, 11, 0)->ch == 0xC3 && cell(&v, 12, 0)->ch == 'A');
    CHECK(cell(&v, 14, 0)->ch == 0x80 && cell(&v, 15, 0)->ch == '!' && v.x == 16);
    feed("\r\n\xE2\x94");                           /* half a character, the rest in the next piece */
    CHECK(v.x == 0);
    feed("\x80");
    CHECK(cell(&v, 0, 1)->ch == 0xC4 && v.x == 1);
    /* sequences it does not know are dropped whole; control characters are not drawn */
    vt_reset(&v, 40, 6);
    feed("\033]0;a window title\007A\033P1;2|data\033\\B\0337C\033cD\033(BE\033[?1049hF\033[5nG");
    CHECK(!strcmp(row_of(&v, 0), "ABCDEFG"));
    feed("\r\n\x01\x02\x07\x7F\x0E\x0FH\033[12\x7F;3HI");
    CHECK(!strcmp(row_of(&v, 1), "H") && cell(&v, 2, 5)->ch == 'I');   /* DEL inside a sequence is dropped */
    feed("\033[2;1H\033[3\nJ");                     /* a control character ends a sequence and is one */
    CHECK(!strcmp(row_of(&v, 2), "J"));
    feed("\033\xC3\xA9");                           /* ESC, then a UTF-8 character: the character */
    CHECK(cell(&v, 1, 2)->ch == 0x82);
    for(i = 0; i < 300; i++) feed("\033]");         /* strings never end the screen: at most 256 bytes */
    feed("\007K");
    CHECK(cell(&v, 2, 2)->ch == 'K');
    feed("\033]");
    for(i = 0; i < 260; i++) feed("x");
    feed("L");
    CHECK(!strcmp(row_of(&v, 2), "J\x82KxxxxL"));    /* dropped up to 256 bytes, then text again */
    /* the pager's question in a stream: inverse, taken away with CR ESC[K */
    vt_reset(&v, 80, 5);
    feed("\033[7m--More-- (Space: next page, Enter: next line, q: skip the rest)\033[27m");
    CHECK(cell(&v, 0, 0)->attr == 0x70 && !strncmp(row_of(&v, 0), "--More--", 8));
    feed("\r\033[Kpagetest line 042\n");
    CHECK(!strcmp(row_of(&v, 0), "pagetest line 042") && cell(&v, 0, 0)->attr == 0x07);
    ansi(80, 24);
    ansi(77, 21);
    ansi(117, 26);
    /* a new start: plain colours, the cursor shown, at a row's start */
    vt_reset(&v, 20, 4);
    feed("\033[7m\033[?25lab\033]0;half");
    vt_fresh(&v);
    CHECK(v.x == 0 && v.y == 1 && !v.cursor_off && !v.inverse && !v.esc);
    feed("c");
    CHECK(cell(&v, 0, 1)->ch == 'c' && cell(&v, 0, 1)->attr == 0x07);
    vt_fresh(&v);
    vt_fresh(&v);
    CHECK(v.y == 2);                                /* once at a row's start, it stays */
}

/* a new size, without reflow */
static void sizes(void) {
    int i;
    char b[32];
    vt_reset(&v, 20, 10);
    feed("one\ntwo\nthree");
    vt_resize(&v, 20, 5);                           /* the rows below the cursor go first */
    CHECK(v.rows == 5 && v.count == 5 && v.y == 2 && v.x == 5 && !strcmp(row_of(&v, 2), "three"));
    vt_resize(&v, 20, 10);                          /* nothing in the scrollback: blank rows below */
    CHECK(v.rows == 10 && v.count == 10 && v.y == 2 && !strcmp(row_of(&v, 0), "one"));
    vt_reset(&v, 20, 10);
    for(i = 0; i < 12; i++) { snprintf(b, sizeof(b), "L%d\n", i); feed(b); }
    CHECK(v.y == 9 && !strcmp(row_of(&v, 8), "L11") && v.count == 13);
    vt_resize(&v, 20, 4);                           /* the cursor at the bottom: the top rows go into the scrollback */
    CHECK(v.rows == 4 && v.y == 3 && !strcmp(row_of(&v, 2), "L11") && !strcmp(row_of(&v, 0), "L9") && v.count == 13);
    vt_resize(&v, 20, 10);                          /* and come back */
    CHECK(v.rows == 10 && v.y == 9 && !strcmp(row_of(&v, 0), "L3") && !strcmp(row_of(&v, 8), "L11") && v.count == 13);
    vt_resize(&v, 20, 16);                          /* three lines back, then blank rows */
    CHECK(v.rows == 16 && v.count == 16 && v.y == 12 && !strcmp(row_of(&v, 0), "L0") && !strcmp(row_of(&v, 11), "L11"));
    /* columns: cut, the cursor kept on the screen */
    vt_reset(&v, 20, 3);
    feed("0123456789ABCDEFGHIJ");
    CHECK(v.wrap && v.x == 19);
    vt_resize(&v, 8, 3);
    CHECK(v.cols == 8 && v.x == 7 && !v.wrap && !strcmp(row_of(&v, 0), "01234567"));
    vt_resize(&v, 20, 3);
    CHECK(!strcmp(row_of(&v, 0), "01234567"));      /* what was cut stays cut */
    feed("X");
    CHECK(cell(&v, 7, 0)->ch == 'X');
    vt_resize(&v, 1000, 1000);                      /* never beyond the cells there are */
    CHECK(v.cols == GUI_VT_COLS && v.rows == GUI_VT_ROWS);
    vt_resize(&v, 0, 0);
    CHECK(v.cols == 1 && v.rows == 1 && v.x == 0 && v.y == 0);
    feed("abc");
    CHECK(cell(&v, 0, 0)->ch == 'c');
}

/* copied text: blanks at the ends left out, a wrap joined again, UTF-8 */
static void text(void) {
    char out[256], want[200];
    bool cut;
    u32 n;
    int a, b, c, d, i;
    vt_reset(&v, 10, 4);
    feed("hello  \r\n0123456789abc\r\ncaf\xC3\xA9 \xE2\x89\x8B");
    n = vt_text(&v, 0, 0, 3, 10, out, sizeof(out), &cut);
    CHECK(!cut && n == strlen("hello\n0123456789abc\ncaf\xC3\xA9 \xE2\x89\x8B") &&
          !memcmp(out, "hello\n0123456789abc\ncaf\xC3\xA9 \xE2\x89\x8B", n));
    n = vt_text(&v, 1, 5, 2, 2, out, sizeof(out), &cut);
    CHECK(n == 7 && !memcmp(out, "56789ab", 7));
    n = vt_text(&v, 0, 1, 1, 0, out, sizeof(out), &cut);
    CHECK(n == 5 && !memcmp(out, "ello\n", 5));
    n = vt_text(&v, 0, 0, 3, 10, out, 8, &cut);
    CHECK(cut && n == 8 && !memcmp(out, "hello\n01", 8));
    n = vt_text(&v, -5, 3, 99, 0, out, sizeof(out), &cut);    /* outside the lines kept: what is there */
    CHECK(n > 20 && out[0] == 'h');
    vt_word(&v, 0, 2, &a, &b, &c, &d);
    CHECK(a == 0 && b == 0 && c == 0 && d == 5);
    vt_word(&v, 0, 6, &a, &b, &c, &d);
    CHECK(a == 0 && b == 6 && c == 0 && d == 7);    /* a blank: that cell */
    vt_word(&v, 1, 9, &a, &b, &c, &d);
    CHECK(a == 1 && b == 0 && c == 2 && d == 3);    /* 10/18/26: on across the wrap */
    vt_word(&v, 2, 1, &a, &b, &c, &d);
    CHECK(a == 1 && b == 0 && c == 2 && d == 3);    /* and back across it */
    feed("\r\n(a/b/c,d)");
    vt_word(&v, vt_screen(&v) + 3, 3, &a, &b, &c, &d);
    CHECK(a == vt_screen(&v) + 3 && b == 1 && c == a && d == 6);  /* a path, without the brackets and the comma */
    vt_word(&v, -1, 0, &a, &b, &c, &d);
    CHECK(a == c && b == d);                        /* outside the lines: nothing */
    vt_word(&v, 0, GUI_VT_COLS, &a, &b, &c, &d);
    CHECK(a == c && b == d);
    /* 10/18/26 (the review): a line that wrapped at another width is joined where it wrapped */
    vt_reset(&v, 77, 5);
    for(i = 0; i < 100; i++) want[i] = 'a';
    want[100] = 0;
    feed(want);
    feed("\r\nnext\r\n");
    vt_resize(&v, 117, 5);                          /* wider: no blanks come in where it wrapped */
    n = vt_text(&v, 0, 0, v.count - 1, v.cols, out, sizeof(out), &cut);
    CHECK(n == 106 && !memcmp(out, want, 100) && !memcmp(out + 100, "\nnext\n", 6));
    vt_resize(&v, 50, 5);                           /* narrower than it wrapped: cut, and no more joined */
    n = vt_text(&v, 0, 0, v.count - 1, v.cols, out, sizeof(out), &cut);
    CHECK(n == 50 + 1 + 23 + 1 + 4 + 1 && !memcmp(out, want, 50) && out[50] == '\n' && !memcmp(out + 51, want, 23));
    vt_reset(&v, 117, 4);                           /* the other way: printed at 117, then 77 */
    for(i = 0; i < 117; i++) want[i] = 'b';
    memcpy(want + 117, "ccc", 4);
    feed(want);
    feed("\r\n");
    vt_resize(&v, 77, 4);                           /* on the screen: cut, the rest is gone, not fused */
    n = vt_text(&v, 0, 0, v.count - 1, v.cols, out, sizeof(out), &cut);
    CHECK(n == 77 + 1 + 3 + 1 && !memcmp(out, want, 77) && !memcmp(out + 77, "\nccc\n", 5));
    vt_reset(&v, 117, 3);                           /* in the scrollback: whole, and joined, all of it */
    feed(want);
    feed("\r\n1\r\n2\r\n3\r\n4");
    vt_resize(&v, 77, 3);
    CHECK(v.count == 6 && !strcmp(row_of(&v, 0), "2") && vt_wrapped(&v, 0) == 117);
    n = vt_text(&v, 0, 0, 1, 77, out, sizeof(out), &cut);
    CHECK(n == 120 && !memcmp(out, want, 120));
    vt_word(&v, 1, 1, &a, &b, &c, &d);              /* the word goes back across that wrap into the b's */
    CHECK(a == 0 && b == 0 && c == 1 && d == 3);
    /* the rows below the cursor dropped by a smaller screen take their wrap mark with them */
    vt_reset(&v, 10, 5);
    feed("0123456789AB\033[A");
    CHECK(vt_wrapped(&v, 0) == 10 && v.y == 0);
    vt_resize(&v, 10, 1);
    CHECK(!vt_wrapped(&v, 0));
    feed("\r\nXYZ");
    n = vt_text(&v, 0, 0, v.count - 1, v.cols, out, sizeof(out), &cut);
    CHECK(n == 14 && !memcmp(out, "0123456789\nXYZ", 14));
    /* only blanks: nothing, and out as it was */
    vt_reset(&v, 10, 4);
    feed("x\r\n\r\n   \r\n");
    memcpy(out, "kept", 4);
    CHECK(vt_text(&v, 1, 0, 3, 10, out, sizeof(out), &cut) == 0 && !memcmp(out, "kept", 4) && !cut);
    n = vt_text(&v, 0, 0, 3, 10, out, sizeof(out), &cut);
    CHECK(n == 2 && !memcmp(out, "x\n", 2));        /* blank lines at the end: one line end */
    n = vt_text(&v, 0, 0, 0, 0, out, sizeof(out), &cut);
    CHECK(n == 0);
}

/**** the Terminal on the desktop ****/

static void starting(void) {
    terminal(960, 540);
    CHECK(g.win[GUI_TERMINAL].open && g.focus == GUI_TERMINAL);
    CHECK(fake_term_running && fake_term_open_count == 1 && g.term.handle == 3 && !g.term.ended);
    CHECK(g.term.vt.cols == 77 && g.term.vt.rows == 21);         /* 640x400: 77x21 cells */
    CHECK(fake_term_cols == 77 && fake_term_rows == 21);
    session_says("\r\n\033[1;31mBrandon Burr's SurfOS\n\033[0m\033[0;1;34mSURF \xE2\x89\x8B@> \033[0;36m");
    CHECK(!strcmp(row_of(&g.term.vt, 1), "Brandon Burr's SurfOS") && cell(&g.term.vt, 0, 1)->attr == 0x0C);
    CHECK(cell(&g.term.vt, 5, 2)->ch == 0xF7 && cell(&g.term.vt, 0, 2)->attr == 0x09 && g.term.vt.x == 9);
    CHECK(g.dirty);
    g.dirty = false;
    tick();
    CHECK(!g.dirty);                                /* nothing new: nothing to draw again */
    CHECK(!strcmp(gui_app_terminal.title, "Terminal") && gui_app_terminal.keys && gui_app_terminal.help_lines > 5);
    /* Ctrl+T in front of another window opens it, its own Ctrl+T is the shell's */
    gui_open(&g, GUI_CALC);
    gui_key(&g, 20);
    CHECK(g.focus == GUI_TERMINAL && fake_term_open_count == 1 && fake_term_in_len == 0);
}

/* every key but Ctrl+G is the session's, as a terminal sends it */
static void keys(void) {
    static const int k[] = { 'a', '\r', GUI_UP, GUI_DOWN, GUI_LEFT, GUI_RIGHT, GUI_HOME, GUI_END, GUI_PGUP, GUI_PGDN,
                             GUI_DELETE, GUI_INSERT, 127, 8, 9, 27, 1, 17, 23, 20, 2, 15, 14, 5, 11, 4, 16, 19, 26, 0xC3, 3 };
    static const char want[] = "a\r\033[A\033[B\033[D\033[C\033[H\033[F\033[5~\033[6~\033[3~\033[2~\x7F\x08\t\033\x01\x11\x17\x14"
                               "\x02\x0F\x0E\x05\x0B\x04\x10\x13\x1A\xC3\x03";
    int i;
    terminal(960, 540);
    for(i = 0; i < (int)(sizeof(k) / sizeof(k[0])); i++) gui_key(&g, k[i]);
    CHECK(typed(want, sizeof(want) - 1));
    CHECK(!g.quit && g.win[GUI_TERMINAL].open && g.focus == GUI_TERMINAL && !g.dialog.kind);   /* Ctrl+Q, W, ... its own */
    CHECK(!g.win[GUI_NOTEPAD].open && !g.win[GUI_FILES].open && !g.win[GUI_CALC].open);
    fake_term_in_len = 0;
    gui_key(&g, 7);                                 /* Ctrl+G: the menus, not the shell's */
    CHECK(g.menu == 1 && fake_term_in_len == 0);
    gui_key(&g, GUI_DOWN);
    gui_key(&g, 27);
    CHECK(!g.menu && fake_term_in_len == 0);
    gui_key(&g, 0);                                 /* Ctrl+Space too */
    CHECK(g.menu == 1);
    gui_key(&g, 27);
    gui_key(&g, 'x');
    CHECK(typed("x", 1));
    CHECK(gui_wants_arrows(&g));
}

/* the window's size is the terminal's, set once per change, and after the corner is let go */
static void resizing(void) {
    int w, h, i;
    terminal(960, 540);
    gui_act(&g, ACT_MAXIMIZE);
    CHECK(TERM->max);
    tick();
    CHECK(g.term.vt.cols == (960 - 2 - SCROLL_W - 8 - 2) / 8 && g.term.vt.rows == (540 - MENU_H - TASK_H - STATUS_H - 1 - TITLE_H - 4) / 16);
    CHECK(fake_term_cols == g.term.vt.cols && fake_term_rows == g.term.vt.rows && g.term.vt.cols == 117 && g.term.vt.rows == 26);
    fake_term_cols = fake_term_rows = 0;            /* once per change, not every frame */
    tick();
    tick();
    CHECK(fake_term_cols == 0 && fake_term_rows == 0);
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    CHECK(!TERM->max && fake_term_cols == 77 && fake_term_rows == 21);
    /* the corner: the size goes to the session when it is let go */
    w = TERM->w;
    h = TERM->h;
    pointer(TERM->x + w - 4, TERM->y + h - 4, POINTER_LEFT, 0);
    CHECK(g.resize == GUI_TERMINAL);
    pointer(TERM->x + w - 4 - 80, TERM->y + h - 4 - 64, POINTER_LEFT, 0);
    tick();
    CHECK(TERM->w == w - 80 && TERM->h == h - 64 && fake_term_cols == 77 && g.term.vt.cols == 77);
    pointer(TERM->x + w - 4 - 160, TERM->y + h - 4 - 96, POINTER_LEFT, 0);
    tick();
    CHECK(fake_term_cols == 77);
    pointer(TERM->x + w - 4 - 160, TERM->y + h - 4 - 96, 0, 0);
    tick();
    CHECK(fake_term_cols == 57 && fake_term_rows == 15 && g.term.vt.cols == 57 && g.term.vt.rows == 15);
    /* what was printed at the old size is read, and wraps, at that size before the new one is set */
    for(i = 0; i < 60; i++) fake_term_feed("x", 1);
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    CHECK(fake_term_cols == 117 && g.term.vt.cols == 117);
    CHECK(strlen(row_of(&g.term.vt, 0)) == 57 && !strcmp(row_of(&g.term.vt, 1), "xxx"));
    /* the cursor stays on the screen */
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    CHECK(g.term.vt.x < g.term.vt.cols && g.term.vt.y < g.term.vt.rows);
    /* 10/18/26: the user guide's sizes on a desktop of 640x400 (a 1280x800 screen): 71x16, 77x18 maximized */
    terminal(640, 400);
    CHECK(g.term.vt.cols == 71 && g.term.vt.rows == 16 && fake_term_cols == 71 && fake_term_rows == 16);
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    CHECK(g.term.vt.cols == 77 && g.term.vt.rows == 18 && fake_term_cols == 77 && fake_term_rows == 18);
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    CHECK(g.term.vt.cols == 71 && g.term.vt.rows == 16 && fake_term_cols == 71 && fake_term_rows == 16);
}

/* the scrollback: the wheel, the scroll bar, output while scrolled back, a key back to the end */
static void scrollback(void) {
    struct fb_surface screen;
    char b[64];
    int i, top;
    terminal(960, 540);
    for(i = 0; i < 600; i++) {
        snprintf(b, sizeof(b), "line %03d\n", i);
        if(fake_term_out_len + strlen(b) > sizeof(fake_term_out)) drain();
        fake_term_feed(b, (u32)strlen(b));
    }
    drain();
    CHECK(g.term.vt.count == 601 && g.term.follow && !strcmp(row_of(&g.term.vt, 0), "line 580"));
    pointer(TX + 100, TY + 100, 0, 1);              /* the wheel away from the user: up three lines */
    CHECK(!g.term.follow && !strcmp(line_of(&g.term.vt, 577), "line 577"));
    fake_screen(&screen, 960, 540);
    gui_render(&g, &screen);
    CHECK(fake_guards(960, 540));
    session_says("new 1\nnew 2\n");                 /* output keeps the view where it is, and says so */
    CHECK(!g.term.follow && g.term.news == 2 && g.term.view == 577);
    gui_render(&g, &screen);
    for(i = 0; i < 300; i++) pointer(TX + 100, TY + 100, 0, 1);
    CHECK(g.term.view == 0 && !g.term.follow);
    pointer(TX + 100, TY + 100, 0, -1);
    CHECK(g.term.view == 3);
    gui_key(&g, 'q');                               /* a key: back to the end */
    CHECK(g.term.follow && g.term.news == 0 && typed("q", 1));
    /* the scroll bar: a page up above the thumb, down below it; the thumb dragged */
    top = vt_screen(&g.term.vt);
    pointer(TERM->x + TERM->w - 8, TY + 4, POINTER_LEFT, 0);
    pointer(TERM->x + TERM->w - 8, TY + 4, 0, 0);
    CHECK(!g.term.follow && g.term.view == (u32)(top - 21));
    pointer(TERM->x + TERM->w - 8, TERM->y + TERM->h - STATUS_H - 4, POINTER_LEFT, 0);
    pointer(TERM->x + TERM->w - 8, TERM->y + TERM->h - STATUS_H - 4, 0, 0);
    CHECK(g.term.follow);
    pointer(TERM->x + TERM->w - 8, TERM->y + TERM->h - STATUS_H - 10, POINTER_LEFT, 0);   /* the thumb, at the bottom */
    CHECK(g.term.thumb);
    pointer(TERM->x + TERM->w - 8, TY - 200, POINTER_LEFT, 0);
    CHECK(g.term.view == 0 && !g.term.follow);
    pointer(TERM->x + TERM->w - 8, TY + 2000, POINTER_LEFT, 0);
    CHECK(g.term.follow);
    pointer(TERM->x + TERM->w - 8, TY + 2000, 0, 0);
    CHECK(!g.term.thumb);
    /* lines that leave the ring while the view shows them: the oldest kept */
    pointer(TX + 100, TY + 100, 0, 3);
    for(i = 0; i < GUI_VT_LINES; i++) {
        fake_term_feed("z\n", 2);
        if(i % 1000 == 999) drain();
    }
    drain();
    CHECK(!g.term.follow && g.term.view < g.term.vt.dropped);
    gui_render(&g, &screen);
    CHECK(fake_guards(960, 540));
}

/* selecting with the mouse, and copying */
static void selecting(void) {
    char want[128];
    int i;
    terminal(960, 540);
    session_says("hello world\nsecond line\n");
    drag(TX, TY + 8, TX + 5 * 8, TY + 16 + 8);
    CHECK(g.term.selected && !g.term.selecting);
    CHECK(gui_menu_on(&g, 3, ACT_COPY) && gui_menu_on(&g, 3, ACT_SELECT_ALL) && !gui_menu_on(&g, 3, ACT_UNDO));
    CHECK(!gui_menu_on(&g, 3, ACT_CUT) && !gui_menu_on(&g, 3, ACT_FIND) && !gui_menu_on(&g, 3, ACT_GOTO));
    gui_act(&g, ACT_COPY);
    CHECK(clip_is("hello world\nsecon"));
    CHECK(fake_term_in_len == 0);                   /* the mouse sends nothing to the session */
    pointer(TX + 8 * 8, TY + 4, POINTER_LEFT, 0);   /* a double-click on a word */
    pointer(TX + 8 * 8, TY + 4, 0, 0);
    g.now += 100;
    pointer(TX + 8 * 8, TY + 4, POINTER_LEFT, 0);
    pointer(TX + 8 * 8, TY + 4, 0, 0);
    gui_act(&g, ACT_COPY);
    CHECK(clip_is("world"));
    g.now += 1000;
    fake_click(&g, TX + 30, TY + 8);                /* a click: no selection, and Copy takes the screen */
    CHECK(!g.term.selected);
    gui_act(&g, ACT_COPY);
    CHECK(clip_is("hello world\nsecond line\n"));
    /* a wrap is joined again; backwards works too */
    for(i = 0; i < 100; i++) want[i] = 'x';
    want[100] = 0;
    session_says(want);
    session_says("\n");
    drag(TX + 77 * 8, TY + 3 * 16 + 8, TX, TY + 2 * 16 + 8);
    gui_act(&g, ACT_COPY);
    CHECK(clip_is(want));
    /* Select all: the scrollback too */
    for(i = 0; i < 40; i++) session_says("more\n");
    gui_act(&g, ACT_SELECT_ALL);
    gui_act(&g, ACT_COPY);
    CHECK(g.clip_len > 100 && !memcmp(g.clip, "hello world\nsecond line\nxxxx", 28));
    CHECK(!memcmp(g.clip + g.clip_len - 5, "more\n", 5));
    /* by the keyboard: the Edit menu (Ctrl+G, right twice; Copy is its first item that is on) */
    gui_key(&g, 7);
    gui_key(&g, GUI_RIGHT);
    gui_key(&g, GUI_RIGHT);
    CHECK(g.menu == 3);
    g.clip_len = 0;
    gui_key(&g, '\r');                              /* Copy is the first item that is on */
    CHECK(g.clip_len > 100 && fake_term_in_len == 0);
    /* a drag past the top edge scrolls the view, also while the pointer rests there */
    g.now += 1000;
    pointer(TX + 40, TY + 5 * 16, POINTER_LEFT, 0);
    pointer(TX + 40, TY - 40, POINTER_LEFT, 0);
    CHECK(!g.term.follow);
    i = (int)g.term.view;
    tick(); tick(); tick(); tick(); tick(); tick();
    CHECK((int)g.term.view < i && g.term.selecting);
    pointer(TX + 40, TY - 40, 0, 0);
    CHECK(!g.term.selecting && g.term.selected);
    i = (int)g.term.view;
    tick(); tick(); tick();
    CHECK((int)g.term.view == i);                   /* let go: it stays */
    gui_act(&g, ACT_COPY);
    CHECK(g.clip_len > 0);
    /* past the bottom edge: down again, to the end */
    pointer(TX + 40, TY + 10, POINTER_LEFT, 0);
    for(i = 0; i < 80; i++) { pointer(TX + 40, TY + 2000, POINTER_LEFT, 0); tick(); }
    CHECK(g.term.follow);
    pointer(TX + 40, TY + 2000, 0, 0);
    /* a console switch drops the drag without a release: it ends at the next frame */
    pointer(TX + 40, TY + 10, POINTER_LEFT, 0);
    g.app_drag = -1;
    tick();
    CHECK(!g.term.selecting);
}

/* paste goes through the input queue as it takes it; Ctrl+C drops what waits */
static void pasting(void) {
    static const char text[] = "echo one\necho two\r\nlast";
    static const char sent[] = "echo one\recho two\rlast";
    struct fb_surface screen;
    u32 i;
    terminal(960, 540);
    CHECK(!gui_menu_on(&g, 3, ACT_PASTE));          /* an empty clipboard */
    memcpy(g.clip, text, sizeof(text) - 1);
    g.clip_len = sizeof(text) - 1;
    CHECK(gui_menu_on(&g, 3, ACT_PASTE));
    fake_term_in_len = sizeof(fake_term_in) - 5;    /* the input queue takes five bytes now */
    gui_act(&g, ACT_PASTE);
    CHECK(fake_term_in_len == sizeof(fake_term_in) && !memcmp(fake_term_in + sizeof(fake_term_in) - 5, "echo ", 5));
    CHECK(g.term.pend_len - g.term.pend_at == sizeof(sent) - 1 - 5);
    tick();                                         /* still full: it waits */
    CHECK(g.term.pend_len - g.term.pend_at == sizeof(sent) - 1 - 5);
    fake_term_in_len = 0;                           /* the session reads */
    tick();
    CHECK(typed(sent + 5, sizeof(sent) - 1 - 5) && g.term.pend_len == 0);
    /* typed keys go behind what waits */
    fake_term_in_len = sizeof(fake_term_in) - 2;
    gui_act(&g, ACT_PASTE);
    gui_key(&g, 'Z');
    fake_term_in_len = 0;
    tick();
    CHECK(fake_term_in_len == sizeof(sent) - 1 - 2 + 1 && fake_term_in[fake_term_in_len - 1] == 'Z');
    /* Ctrl+C: what waits is dropped, and it goes first */
    fake_term_in_len = sizeof(fake_term_in);
    gui_act(&g, ACT_PASTE);
    CHECK(g.term.pend_len == sizeof(sent) - 1);
    gui_key(&g, 3);
    CHECK(g.term.pend_len - g.term.pend_at == 1 && g.term.pend[g.term.pend_at] == 3);
    fake_term_in_len = 0;
    tick();
    CHECK(typed("\x03", 1) && g.term.pend_len == 0);
    /* a paste of the whole clipboard waits whole; its progress shows */
    for(i = 0; i < GUI_CLIP_MAX; i++) g.clip[i] = (char)('a' + i % 26);
    g.clip_len = GUI_CLIP_MAX;
    fake_term_in_len = sizeof(fake_term_in);
    gui_act(&g, ACT_PASTE);
    CHECK(g.term.pend_len == GUI_CLIP_MAX && !g.term.status[0]);
    fake_screen(&screen, 960, 540);
    gui_render(&g, &screen);
    CHECK(fake_guards(960, 540));
    for(i = 0; i < 40 && g.term.pend_len; i++) { fake_term_in_len = 0; tick(); }   /* the queue takes 8 KB a frame */
    CHECK(g.term.pend_len == 0 && i == GUI_CLIP_MAX / sizeof(fake_term_in));
    CHECK(fake_term_in_len == sizeof(fake_term_in) &&
          !memcmp(fake_term_in, g.clip + GUI_CLIP_MAX - sizeof(fake_term_in), sizeof(fake_term_in)));
    gui_act(&g, ACT_PASTE);                         /* more than there is room for behind it: the rest is dropped */
    gui_act(&g, ACT_PASTE);
    CHECK(g.term.pend_len - g.term.pend_at <= GUI_TERM_PEND && g.term.status[0]);
}

/* the session ends (exit): the window says so; Enter starts another */
static void ending(void) {
    struct fb_surface screen;
    char why[80];
    int i;
    terminal(960, 540);
    session_says("SURF \xE2\x89\x8B@> exit\n");
    CHECK(gui_app_terminal.busy(&g, why, sizeof(why)) && strstr(why, "surfsh"));
    fake_term_ended = true;
    tick();
    CHECK(g.term.handle == -1 && g.term.ended && !fake_term_running);
    CHECK(!gui_app_terminal.busy(&g, why, sizeof(why)));
    CHECK(strstr(g.term.status, "ended") != NULL);
    for(i = 0; i < g.term.vt.rows && !strstr(row_of(&g.term.vt, i), "The session has ended"); i++) ;
    CHECK(i < g.term.vt.rows && cell(&g.term.vt, 1, i)->attr == 0x0B);
    CHECK(!gui_menu_on(&g, 3, ACT_PASTE));
    fake_screen(&screen, 960, 540);
    gui_render(&g, &screen);
    CHECK(fake_guards(960, 540));
    gui_key(&g, 'x');                               /* no session: nothing is sent */
    CHECK(fake_term_in_len == 0 && fake_term_open_count == 1);
    gui_key(&g, '\r');
    CHECK(fake_term_open_count == 2 && fake_term_running && g.term.handle == 3 && !g.term.ended);
    CHECK(g.term.vt.x == 0 && g.term.vt.y == i + 1);   /* below the line that said so */
    session_says("again");
    CHECK(!strcmp(row_of(&g.term.vt, i + 1), "again"));
    /* the New session button */
    fake_term_ended = true;
    tick();
    CHECK(g.term.handle == -1);
    fake_click(&g, TERM->x + TERM->w - 24 - 5 * 8 - 8 - 104 + 20, TERM->y + TERM->h - STATUS_H + 8);
    CHECK(fake_term_open_count == 3 && g.term.handle == 3);
    /* with no session, Esc closes the window */
    fake_term_ended = true;
    tick();
    gui_key(&g, 27);
    CHECK(!g.win[GUI_TERMINAL].open);
    /* a session that cannot start says why, and Enter tries again */
    fake_term_ended = false;
    fake_term_running = true;                       /* the fake's one session is taken */
    gui_key(&g, 20);
    CHECK(g.win[GUI_TERMINAL].open && g.term.handle == -1 && g.term.error == -EAGAIN && fake_term_open_count == 3);
    CHECK(strstr(row_of(&g.term.vt, 0), "No session") && strstr(g.term.status, "pseudo-terminal"));
    gui_render(&g, &screen);
    CHECK(fake_guards(960, 540));
    fake_term_running = false;
    gui_key(&g, '\r');
    CHECK(g.term.handle == 3 && fake_term_open_count == 4 && g.term.vt.y == 1);
}

/* busy: closing the window and leaving the desktop ask first */
static void closing(void) {
    terminal(960, 540);
    gui_close(&g, GUI_TERMINAL);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && g.dialog.act == gui_app_act(GUI_TERMINAL, GUI_APP_CLOSE));
    CHECK(g.win[GUI_TERMINAL].open && fake_term_running);
    gui_key(&g, 27);                                /* Cancel: it goes on */
    CHECK(!g.dialog.kind && g.win[GUI_TERMINAL].open && fake_term_running && g.term.handle == 3);
    gui_key(&g, 'k');
    CHECK(typed("k", 1));
    fake_click(&g, TERM->x + TERM->w - 16, TERM->y + 14);    /* the title bar's x */
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && !strcmp(g.dialog.label[0], "Close"));
    gui_key(&g, '\r');                              /* Close: it ends */
    CHECK(!g.dialog.kind && !g.win[GUI_TERMINAL].open && !fake_term_running && g.term.handle == -1);
    /* leaving the desktop */
    terminal(960, 540);
    gui_act(&g, ACT_QUIT);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && g.dialog.act == ACT_DLG_LEAVE && !g.quit);
    gui_key(&g, 27);
    CHECK(!g.quit && fake_term_running);
    gui_act(&g, ACT_QUIT);
    gui_key(&g, '\r');
    CHECK(g.quit && fake_term_running);             /* the shell's gui_finish() hangs up */
    gui_finish(&g);
    CHECK(!fake_term_running && g.term.handle == -1);
    /* ended, it asks nothing */
    terminal(960, 540);
    fake_term_ended = true;
    tick();
    gui_close(&g, GUI_TERMINAL);
    CHECK(!g.dialog.kind && !g.win[GUI_TERMINAL].open);
    fake_term_ended = false;
    /* minimized, it is read all the same */
    terminal(960, 540);
    gui_act(&g, ACT_MINIMIZE);
    CHECK(TERM->min && g.focus != GUI_TERMINAL);
    session_says("while hidden");
    CHECK(!strcmp(row_of(&g.term.vt, 0), "while hidden"));
}

/* a desktop without a system (test_gui's): the window says what is missing */
static void no_system(void) {
    struct fb_surface screen;
    fake_desktop(&g, 640, 400);
    g.sys = NULL;
    gui_key(&g, 20);
    CHECK(g.win[GUI_TERMINAL].open && g.term.handle == -1 && fake_term_open_count == 0);
    gui_key(&g, 'a');
    gui_key(&g, '\r');
    tick();
    CHECK(fake_term_in_len == 0 && fake_term_open_count == 0);
    CHECK(!gui_menu_on(&g, 3, ACT_PASTE));
    fake_screen(&screen, 640, 400);
    gui_render(&g, &screen);
    CHECK(fake_guards(640, 400));
    gui_close(&g, GUI_TERMINAL);
    CHECK(!g.win[GUI_TERMINAL].open && !g.dialog.kind);
}

/* what the window draws: the colours, the cursor, the selection */
static void drawing(void) {
    struct fb_surface screen;
    int i, lit = 0;
    terminal(960, 540);
    session_says("\033[1;33;44mY\033[0m ");
    fake_screen(&screen, 960, 540);
    gui_render(&g, &screen);
    CHECK(fake_pixel(960, TX + 7, TY + 15) == 0x3C6EB4);               /* the blue background */
    for(i = 0; i < 8 * 16; i++) if(fake_pixel(960, TX + i % 8, TY + i / 8) == 0xF0D27A) lit++;
    CHECK(lit > 10);                                                    /* bright yellow */
    CHECK(fake_pixel(960, TX + 3 * 8 + 4, TY + 8) == NAVY);             /* the background */
    CHECK(fake_pixel(960, TX + 2 * 8 + 4, TY + 8) == SHELL);            /* the cursor: a block, after the blank */
    gui_open(&g, GUI_CALC);                                             /* not in front: an outline */
    gui_open(&g, GUI_TERMINAL);
    gui_act(&g, ACT_MINIMIZE);
    gui_act(&g, ACT_WINDOW + GUI_TERMINAL);
    gui_open(&g, GUI_CALC);
    g.win[GUI_CALC].x = 900;
    gui_render(&g, &screen);
    CHECK(fake_pixel(960, TX + 2 * 8 + 4, TY + 8) == NAVY && fake_pixel(960, TX + 2 * 8, TY + 8) == SHELL);
    gui_open(&g, GUI_TERMINAL);
    session_says("\033[?25l");
    gui_render(&g, &screen);
    CHECK(fake_pixel(960, TX + 2 * 8, TY + 8) == NAVY);                 /* hidden */
    drag(TX, TY + 8, TX + 8, TY + 8);
    gui_render(&g, &screen);
    CHECK(fake_pixel(960, TX + 7, TY + 15) == PICK);                    /* selected */
}

/* each window opens, ticks, draws inside the screen (also maximized and hanging off the edge) and closes */
static void windows(int width, int height) {
    struct fb_surface screen;
    char b[64];
    int i, k;
    fake_desktop(&g, width, height);
    fake_screen(&screen, width, height);
    for(i = 0; i < IDS; i++) {
        gui_open(&g, ids[i]);
        CHECK(g.win[ids[i]].open && g.focus == ids[i]);
        for(k = 0; k < 3; k++) { g.now += 1000; gui_tick(&g); }
        for(k = 0; k < 200; k++) {                  /* something on the screen, and in the scrollback */
            snprintf(b, sizeof(b), "\033[3%dmline %d \xE2\x89\x8B a long line that wraps around\033[0m\n", k % 8, k);
            if(fake_term_out_len + strlen(b) > sizeof(fake_term_out)) drain();
            fake_term_feed(b, (u32)strlen(b));
        }
        drain();
        drag(TX + 8, TY + 8, TX + 200, TY + 64);
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
        gui_act(&g, ACT_MAXIMIZE);
        gui_tick(&g);
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
        CHECK(g.win[ids[i]].w == width && g.win[ids[i]].x + g.win[ids[i]].w <= width);
        pointer(width / 2, height / 2, 0, 5);
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
        gui_act(&g, ACT_MAXIMIZE);
        g.win[ids[i]].x = width - 10;
        g.win[ids[i]].y = height - 10;
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
        gui_tick(&g);
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
        fake_term_ended = true;                     /* ended: the button in the status bar */
        gui_tick(&g);
        g.win[ids[i]].x = 0;
        g.win[ids[i]].y = MENU_H + 6;
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
        fake_term_ended = false;
    }
    gui_finish(&g);
    for(i = 0; i < IDS; i++) {
        gui_open(&g, ids[i]);
        gui_act(&g, ACT_CLOSE);
        CHECK(!g.win[ids[i]].open);
    }
}

/**** 10/18/26: what the review found ****/

/* a line copied after the width changed: joined where it wrapped, cut where the screen cut it */
static void widths(void) {
    char text[200];
    int i;
    terminal(960, 540);
    for(i = 0; i < 100; i++) text[i] = 'a';
    text[100] = 0;
    session_says(text);
    session_says("\r\nnext\r\n");
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    CHECK(g.term.vt.cols == 117);
    gui_act(&g, ACT_SELECT_ALL);
    gui_act(&g, ACT_COPY);
    CHECK(g.clip_len == 106 && !memcmp(g.clip, text, 100) && !memcmp(g.clip + 100, "\nnext\n", 6));
    gui_act(&g, ACT_MAXIMIZE);                      /* back to 77 columns: the screen's copy */
    tick();
    fake_click(&g, TX + 30, TY + 5 * 16 + 8);
    CHECK(!g.term.selected);
    gui_act(&g, ACT_COPY);
    CHECK(g.clip_len == 106 && !memcmp(g.clip, text, 100) && !memcmp(g.clip + 100, "\nnext\n", 6));
    gui_finish(&g);
    terminal(960, 540);                             /* printed at 117 columns, then 77: cut, not fused */
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    for(i = 0; i < 117; i++) text[i] = 'b';
    memcpy(text + 117, "ccc\r\n", 6);
    session_says(text);
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    gui_act(&g, ACT_SELECT_ALL);
    gui_act(&g, ACT_COPY);
    CHECK(g.clip_len == 82 && !memcmp(g.clip, text, 77) && !memcmp(g.clip + 77, "\nccc\n", 5));
    gui_finish(&g);
}

/* a key goes whole or not at all; a paste is cut where the room ends, never inside a UTF-8 character */
static void whole_keys(void) {
    u32 n;
    terminal(960, 540);
    fake_term_in_len = sizeof(fake_term_in);        /* the input queue is full */
    for(n = 0; n < GUI_CLIP_MAX; n++) g.clip[n] = 'p';
    g.clip_len = GUI_CLIP_MAX;
    gui_act(&g, ACT_PASTE);
    g.clip_len = GUI_TERM_PEND - GUI_CLIP_MAX - 1;  /* room for one byte more */
    gui_act(&g, ACT_PASTE);
    CHECK(g.term.pend_len == GUI_TERM_PEND - 1);
    g.term.status[0] = 0;
    gui_key(&g, GUI_UP);                            /* three bytes: none of them */
    CHECK(g.term.pend_len == GUI_TERM_PEND - 1 && g.term.pend[GUI_TERM_PEND - 2] == 'p' &&
          strstr(g.term.status, "dropped") != NULL);
    gui_key(&g, 'k');                               /* one byte: it goes */
    CHECK(g.term.pend_len == GUI_TERM_PEND && g.term.pend[GUI_TERM_PEND - 1] == 'k');
    fake_term_in_len = 0;
    for(n = 0; n < 40 && g.term.pend_len; n++) { fake_term_in_len = 0; tick(); }
    CHECK(g.term.pend_len == 0);
    fake_term_in_len = sizeof(fake_term_in);
    g.clip_len = GUI_CLIP_MAX;
    gui_act(&g, ACT_PASTE);
    g.clip_len = GUI_TERM_PEND - GUI_CLIP_MAX - 3;  /* room for three bytes more */
    gui_act(&g, ACT_PASTE);
    memcpy(g.clip, "\xC3\xA9\xE2\x82\xAC", 5);      /* two bytes and three: the second does not fit */
    g.clip_len = 5;
    gui_act(&g, ACT_PASTE);
    CHECK(g.term.pend_len == GUI_TERM_PEND - 1 && !memcmp(g.term.pend + GUI_TERM_PEND - 3, "\xC3\xA9", 2));
    CHECK(strstr(g.term.status, "all but 3 bytes") != NULL);
    gui_finish(&g);
}

/* nothing but blanks selected: Copy keeps the clipboard as it was */
static void blank_copies(void) {
    terminal(960, 540);
    session_says("word\r\n");
    strlcpy(g.clip, "precious", sizeof(g.clip));
    g.clip_len = 8;
    fake_click(&g, TX + 30 * 8 + 4, TY + 8);
    g.now += 100;
    fake_click(&g, TX + 30 * 8 + 4, TY + 8);        /* a double-click on a blank: that cell */
    CHECK(g.term.selected);
    gui_act(&g, ACT_COPY);
    CHECK(clip_is("precious") && strstr(g.term.status, "Nothing copied") != NULL);
    gui_finish(&g);
    terminal(960, 540);                             /* a blank screen */
    strlcpy(g.clip, "precious", sizeof(g.clip));
    g.clip_len = 8;
    gui_act(&g, ACT_COPY);
    CHECK(clip_is("precious") && strstr(g.term.status, "screen is blank") != NULL);
    gui_finish(&g);
}

/* a word the wrap broke: a double-click on either half takes all of it */
static void broken_words(void) {
    char text[100];
    int i;
    terminal(960, 540);
    for(i = 0; i < 74; i++) text[i] = '.';
    memcpy(text + 74, " /initrd/bin/surfsh", 20);
    session_says(text);
    session_says("\r\n");
    fake_click(&g, TX + 76 * 8 + 4, TY + 8);
    g.now += 100;
    fake_click(&g, TX + 76 * 8 + 4, TY + 8);
    gui_act(&g, ACT_COPY);
    CHECK(clip_is("/initrd/bin/surfsh"));
    g.now += 1000;
    fake_click(&g, TX + 4 * 8 + 4, TY + 16 + 8);
    g.now += 100;
    fake_click(&g, TX + 4 * 8 + 4, TY + 16 + 8);
    g.clip_len = 0;
    gui_act(&g, ACT_COPY);
    CHECK(clip_is("/initrd/bin/surfsh"));
    gui_finish(&g);
}

/* a frame's work is bounded, in bytes and in cells written; the rest, and a new size, wait for later frames */
static void budget(void) {
    static char flood[8192];
    u32 i, w;
    int frames;
    terminal(960, 540);
    for(i = 0; i + 4 <= sizeof(flood); i += 4) memcpy(flood + i, "\033[2J", 4);
    fake_term_feed(flood, sizeof(flood));           /* 2,048 erases of the whole screen: 3.3 million cells */
    w = g.term.vt.written;
    tick();
    CHECK(g.term.vt.written - w >= 32768 && g.term.vt.written - w < 32768 + 77 * 21);   /* and one erase's own */
    CHECK(g.term.carry_at < g.term.carry_len || fake_term_out_len);
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    CHECK(g.term.vt.cols == 77 && fake_term_cols == 77);   /* the size waits until it is drawn (250 ms at most) */
    for(frames = 0; frames < 1000 && (fake_term_out_len || g.term.carry_at < g.term.carry_len); frames++) {
        w = g.term.vt.written;
        tick();
        CHECK(g.term.vt.written - w < 32768 + 117 * 26);   /* an erase of the screen at either size beyond it */
    }
    CHECK(frames > 50 && frames < 1000);
    tick();
    CHECK(g.term.vt.cols == 117 && fake_term_cols == 117);
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    CHECK(g.term.vt.cols == 77 && fake_term_cols == 77);
    for(i = 0; i < sizeof(flood); i++) flood[i] = 'x';
    fake_term_feed(flood, sizeof(flood));           /* READ_MAX of text: all of it in a frame, which reaches */
    gui_act(&g, ACT_MAXIMIZE);                      /* the limit, so the new size waits a frame more */
    tick();
    CHECK(!fake_term_out_len && g.term.carry_at == g.term.carry_len && g.term.vt.cols == 77 && fake_term_cols == 77);
    tick();
    CHECK(g.term.vt.cols == 117 && fake_term_cols == 117);
    gui_finish(&g);
}

/* a read that fails otherwise than at the session's end: the session is lost, and the window says so */
static void lost(void) {
    int i;
    terminal(960, 540);
    g.term.handle = 4;                              /* a handle the system does not know */
    tick();
    CHECK(g.term.handle == -1 && g.term.ended && strstr(g.term.status, "ended") != NULL);
    for(i = 0; i < g.term.vt.rows && !strstr(row_of(&g.term.vt, i), "The session was lost"); i++) ;
    CHECK(i < g.term.vt.rows);
    gui_finish(&g);
}

/* a selection whose lines have left the scrollback: Copy says so, and keeps the clipboard */
static void gone_selection(void) {
    int i;
    terminal(960, 540);
    session_says("old line\r\n");
    drag(TX, TY + 8, TX + 3 * 8, TY + 8);
    CHECK(g.term.selected);
    for(i = 0; i < GUI_VT_LINES + 10; i++) {
        fake_term_feed("x\n", 2);
        if(i % 500 == 499) drain();
    }
    drain();
    CHECK(g.term.end_line < g.term.vt.dropped);
    strlcpy(g.clip, "kept", sizeof(g.clip));
    g.clip_len = 4;
    gui_act(&g, ACT_COPY);
    CHECK(clip_is("kept") && strstr(g.term.status, "left the scrollback") != NULL);
    gui_finish(&g);
}

/**** 10/18/26 (the second review): its cases 7 to 10 ****/

/* output that never pauses (a program in the background: 8 KB before every frame) and the window maximized: the
   new size waits 250 ms at most, then goes the first frame after (case 9); text, a new line every two bytes (the
   cells' budget, not the bytes', ends a frame), and erases. Of the text, what was read is drawn first. */
static void flood_resize(void) {
    static const char *const floods[] = { "line of output\n", "y\n", "\033[2J" };
    u32 n, w, most = 0;
    int f, frame, at;
    bool drawn = false;
    for(f = 0; f < 3; f++) {
        terminal(960, 540);
        n = (u32)strlen(floods[f]);
        for(frame = 0, at = -1; frame < 60; frame++) {
            while(fake_term_out_len + n <= sizeof(fake_term_out)) fake_term_feed(floods[f], n);
            if(frame == 5) gui_act(&g, ACT_MAXIMIZE);
            w = g.term.vt.written;
            tick();
            if(g.term.vt.written - w > most) most = g.term.vt.written - w;
            if(at < 0 && fake_term_cols == 117) {
                at = frame;
                if(!f) drawn = g.term.carry_at == g.term.carry_len;
            }
        }
        /* frame 4 is the last without it waiting; frame 17 the first 260 ms after it (frame 16: 240) */
        CHECK(at == 17 && fake_term_rows == 26 && g.term.vt.cols == 117 && g.term.vt.rows == 26 && TERM->max);
        fake_term_out_len = 0;                      /* it pauses: everything is drawn, at the new size */
        drain();
        CHECK(!fake_term_out_len && g.term.carry_at == g.term.carry_len && g.term.vt.cols == 117);
        gui_finish(&g);
    }
    CHECK(drawn && most < 32768 + 117 * 26);        /* and a frame's work stays within its budget */
}

/* a row that wrapped at 77, the window wider (the row is whole: its mark stays), then written on past column 77
   without an erase: its mark goes, so Copy takes the row as it shows, not joined to the next (case 8); a character
   written before the wrap leaves the mark */
static void past_wrap(void) {
    char out[256];
    bool cut;
    u32 n;
    int i;
    vt_reset(&v, 77, 5);
    for(i = 0; i < 80; i++) feed("x");
    vt_resize(&v, 117, 5);
    CHECK(vt_wrapped(&v, vt_screen(&v)) == 77);
    feed("\033[1;10HQ");
    CHECK(vt_wrapped(&v, vt_screen(&v)) == 77);
    feed("\033[1;78HYYYY\033[2;1H\033[K\033[3;1Hnext");
    CHECK(vt_wrapped(&v, vt_screen(&v)) == 0);
    n = vt_text(&v, 0, 0, v.count - 1, v.cols, out, sizeof(out), &cut);
    CHECK(n == 77 + 4 + 2 + 4 + 1 && out[9] == 'Q' && out[76] == 'x' && !memcmp(out + 77, "YYYY\n\nnext\n", 11));
}

/* a paste goes into the queue in chunks that end between characters: an e-acute across two chunks, the room ending
   at the first, waits whole or not at all (case 7); and once a byte did not fit, nothing after it goes */
static void paste_chars(void) {
    u32 n;
    terminal(960, 540);
    fake_term_in_len = sizeof(fake_term_in);        /* the input queue is full: everything waits */
    for(n = 0; n < GUI_CLIP_MAX; n++) g.clip[n] = 'q';
    g.clip_len = GUI_CLIP_MAX;
    gui_act(&g, ACT_PASTE);
    g.clip_len = GUI_TERM_PEND - GUI_CLIP_MAX - 256;
    gui_act(&g, ACT_PASTE);
    CHECK(g.term.pend_len == GUI_TERM_PEND - 256);  /* room for 256 bytes more */
    for(n = 0; n < 255; n++) g.clip[n] = 'r';
    memcpy(g.clip + 255, "\xC3\xA9s", 3);           /* its first byte would be the first chunk's last */
    g.clip_len = 258;
    gui_act(&g, ACT_PASTE);
    CHECK(g.term.pend_len == GUI_TERM_PEND - 1 && g.term.pend[GUI_TERM_PEND - 2] == 'r');
    CHECK(strstr(g.term.status, "all but 3 bytes") != NULL);
    memcpy(g.clip, "\xC3\xA9", 2);                  /* one byte of room: e-acute does not fit, the a's would */
    for(n = 2; n < 300; n++) g.clip[n] = 'a';
    g.clip_len = 300;
    gui_act(&g, ACT_PASTE);
    CHECK(g.term.pend_len == GUI_TERM_PEND - 1 && strstr(g.term.status, "all but 300 bytes") != NULL);
    gui_finish(&g);
}

/* Copy with nothing selected, scrolled back over lines printed when the window was wider: as they show, cut at 77
   columns, and one that wrapped at 117 not joined (case 10); a selection still takes them whole */
static void shown_copy(void) {
    char text[160], want[400];
    u32 n = 0;
    int i;
    terminal(960, 540);
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    memset(text, 'w', 116);
    memcpy(text + 116, "Z\r\n", 4);
    session_says(text);                             /* 117 characters: the row is full, no wrap */
    memset(text, 'v', 150);
    memcpy(text + 150, "\r\n", 3);
    session_says(text);                             /* 150: it wraps at 117 */
    for(i = 0; i < 40; i++) session_says("filler\r\n");
    gui_act(&g, ACT_MAXIMIZE);                      /* restored: 77 columns, those lines in the scrollback */
    tick();
    for(i = 0; i < 300; i++) pointer(TX + 100, TY + 100, 0, 1);
    fake_click(&g, TX + 30, TY + 8);
    CHECK(!g.term.selected && !g.term.follow && g.term.view == 0 && !g.term.vt.dropped);   /* the top */
    gui_act(&g, ACT_COPY);
    memset(want, 'w', 77);
    want[77] = '\n';
    memset(want + 78, 'v', 77);
    want[155] = '\n';
    memset(want + 156, 'v', 33);
    want[189] = '\n';
    for(n = 190, i = 0; i < 21 - 3; i++, n += 7) memcpy(want + n, "filler\n", 7);
    n--;                                            /* the last row has text: no line end after it */
    CHECK(g.clip_len == n && !memcmp(g.clip, want, n));
    gui_act(&g, ACT_SELECT_ALL);
    gui_act(&g, ACT_COPY);
    memset(want, 'w', 116);                         /* the selection: 117 characters, and 150 joined */
    want[116] = 'Z';
    want[117] = '\n';
    memset(want + 118, 'v', 150);
    want[268] = '\n';
    CHECK(g.clip_len > 269 && !memcmp(g.clip, want, 269) && !memcmp(g.clip + 269, "filler\n", 7));
    gui_finish(&g);
}

/**** 10/18/26: the emulator and the Terminal against random input; their invariants hold ****/

static u32 seed = 12345;
static u32 rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

/* the screen's invariants (with cells: also nothing beyond the width, and no wrap mark wider than the screen) */
static bool sane(const struct gui_vt *vt, bool cells) {
    int x, y;
    if(vt->cols < 1 || vt->cols > GUI_VT_COLS || vt->rows < 1 || vt->rows > GUI_VT_ROWS) return false;
    if(vt->x < 0 || vt->x >= vt->cols || vt->y < 0 || vt->y >= vt->rows) return false;
    if(vt->count < vt->rows || vt->count > GUI_VT_LINES || vt->first >= GUI_VT_LINES) return false;
    if(vt->nparam < 0 || vt->nparam > GUI_VT_PARAMS || vt->utf.have > 3) return false;
    for(y = 0; cells && y < vt->rows; y++) {
        const struct gui_vt_cell *c = vt_line(vt, vt_screen(vt) + y);
        if(vt_wrapped(vt, vt_screen(vt) + y) > vt->cols) return false;
        for(x = vt->cols; x < GUI_VT_COLS; x++) if(c[x].ch != ' ' || c[x].attr != VT_PLAIN) return false;
    }
    return true;
}

static u8 hostile(void) {
    static const char pool[] = "\033\033\033[[[;;:0123456789?HfABCDGJKmlh\n\r\b\t\a\x7f abcXYZ";
    u32 k = rnd() % 10;
    if(k < 7) return (u8)pool[rnd() % (sizeof(pool) - 1)];
    if(k < 9) return (u8)(0x80 + rnd() % 128);
    return (u8)rnd();
}

static void fuzz_vt(void) {
    static const int colset[] = { -5, 0, 1, 2, 8, 77, 117, 157, 160, 161, 1000 };
    static const int rowset[] = { -5, 0, 1, 2, 21, 26, 43, 64, 65, 1000 };
    char out[256 + 16];
    long it, bad = 0;
    int i, k, l0, c0, l1, c1;
    u32 size, got;
    bool cut;
    vt_reset(&v, 80, 24);
    for(it = 0; it < 150000 && !bad; it++) {
        u32 op = rnd() % 1000;
        if(op < 930) {
            u8 b[32];
            k = 1 + (int)(rnd() % 32);
            for(i = 0; i < k; i++) b[i] = hostile();
            vt_feed(&v, (const char *)b, (u32)k);
        } else if(op < 960) {
            vt_resize(&v, rnd() & 1 ? colset[rnd() % 11] : (int)(rnd() % 200) - 10,
                      rnd() & 1 ? rowset[rnd() % 10] : (int)(rnd() % 80) - 5);
        } else if(op < 965) {
            vt_fresh(&v);
        } else if(op < 990) {                       /* copied text into a buffer of some size: never beyond it */
            l0 = (int)(rnd() % (u32)(v.count + 20)) - 10;
            l1 = (int)(rnd() % (u32)(v.count + 20)) - 10;
            c0 = (int)(rnd() % (u32)(v.cols + 10)) - 5;
            c1 = (int)(rnd() % (u32)(v.cols + 10)) - 5;
            size = rnd() % 256;
            memset(out, 0x5A, sizeof(out));
            got = vt_text(&v, l0, c0, l1, c1, out, size, &cut);
            if(got > size) bad++;
            for(i = (int)size; i < (int)sizeof(out); i++) if((u8)out[i] != 0x5A) bad++;
        } else if(op < 999) {
            vt_word(&v, (int)(rnd() % (u32)(v.count + 20)) - 10, (int)(rnd() % (u32)(v.cols + 10)) - 5, &l0, &c0, &l1, &c1);
            if(l0 > l1 || (l0 == l1 && c0 > c1) || c0 < 0 || c1 > GUI_VT_COLS) bad++;
        } else {
            vt_reset(&v, colset[rnd() % 11], rowset[rnd() % 10]);
        }
        if(!sane(&v, !(it & 63))) bad++;
    }
    CHECK(!bad && it == 150000);
}

static void fuzz_model(void) {
    static const int sizes[][2] = { { 640, 400 }, { 960, 540 }, { 1280, 800 } };
    static const int keyset[] = { 'a', ' ', '\r', 3, 4, 7, 0, 9, 8, 127, 27, 23, 17, 20, 22, GUI_UP, GUI_PGDN,
                                  GUI_DELETE, 0xC3, 255, 256, -1 };
    static const int acts[] = { ACT_COPY, ACT_PASTE, ACT_SELECT_ALL, ACT_MAXIMIZE, ACT_MINIMIZE, ACT_CLOSE,
                                ACT_WINDOW + GUI_TERMINAL, ACT_QUIT, ACT_NEXT_WINDOW, ACT_UNDO, ACT_FIND };
    struct fb_surface screen;
    struct pointer_event e;
    char b[300];
    long step, bad = 0;
    int r, i, n, width, height;
    for(r = 0; r < 3; r++) {
        width = sizes[r][0];
        height = sizes[r][1];
        fake_desktop(&g, width, height);
        fake_screen(&screen, width, height);
        gui_key(&g, 20);
        memset(&e, 0, sizeof(e));
        for(step = 0; step < 4000; step++) {
            const struct gui_window *w = &g.win[GUI_TERMINAL];
            u32 k = rnd() % 100;
            if(k < 25) {
                n = (int)(rnd() % sizeof(b));
                for(i = 0; i < n; i++) b[i] = (char)hostile();
                fake_term_feed(b, (u32)n);
            } else if(k < 45) {
                g.now += rnd() % 60;
                gui_tick(&g);
            } else if(k < 60) {
                gui_key(&g, keyset[rnd() % (sizeof(keyset) / sizeof(keyset[0]))]);
            } else if(k < 82) {                     /* the pointer, mostly on the Terminal and its edges */
                e.x = rnd() % 3 ? w->x + (int)(rnd() % (u32)(w->w + 40)) - 20 : (int)(rnd() % (u32)width);
                e.y = rnd() % 3 ? w->y + (int)(rnd() % (u32)(w->h + 40)) - 20 : (int)(rnd() % (u32)height);
                if(rnd() % 3 == 0) e.buttons ^= POINTER_LEFT;
                e.wheel = rnd() % 8 ? 0 : (int)(rnd() % 7) - 3;
                gui_pointer(&g, &e);
            } else if(k < 87) {
                i = acts[rnd() % (sizeof(acts) / sizeof(acts[0]))];
                if(i != ACT_MAXIMIZE || g.focus >= 0) gui_act(&g, i);
            } else if(k < 90) {
                n = (int)(rnd() % 300);
                for(i = 0; i < n; i++) g.clip[i] = rnd() % 5 ? (char)('a' + rnd() % 26) : (char)"\r\n\t\x03\033"[rnd() % 5];
                g.clip_len = (u32)n;
            } else if(k < 94) {
                fake_term_in_len = rnd() & 1 ? sizeof(fake_term_in) - rnd() % 4 : 0;
            } else if(k < 95) {
                fake_term_ended = true;
            } else if(k < 99) {
                gui_render(&g, &screen);
                if(!fake_guards(width, height)) bad++;
            } else gui_key(&g, rnd() & 1 ? '\r' : 27);
            if(fake_term_ended && !fake_term_out_len && rnd() % 4 == 0) fake_term_ended = false;
            if(g.term.pend_at > g.term.pend_len || g.term.pend_len > GUI_TERM_PEND || g.clip_len > GUI_CLIP_MAX) bad++;
            if(g.term.carry_at > g.term.carry_len || g.term.carry_len > sizeof(g.term.carry)) bad++;
            if(!sane(&g.term.vt, false)) bad++;
            if(g.term.handle >= 0 && !fake_term_running) bad++;
            if(!g.win[GUI_TERMINAL].open && fake_term_running) bad++;
            if(g.quit) {                            /* the desktop leaves: gui_finish(), as shell/gui.c does */
                gui_finish(&g);
                if(fake_term_running) bad++;
                fake_desktop(&g, width, height);
                gui_key(&g, 20);
            }
        }
        gui_finish(&g);
        if(fake_term_running) bad++;
    }
    CHECK(!bad);
}

/**** 10/18/26: the kernel's side, shell/guisys_term.c, against stand-ins for the kernel's tasks, descriptors and
   pseudo-terminals: kernel/pty.c's calls and fs/vfs.c's descriptors as small tables ****/

#define _SURFOS_TASK_H                  /* the stand-ins below take the place of these headers */
#define _SURFOS_IRQ_H
#define _SURFOS_KLOG_H
#define _FS_VFS_H
#ifndef EMFILE
#define EMFILE 24
#endif
#define TS_DEAD 4
#define WNOHANG 1
#define KT 32
#define KF 16                           /* NR_OPEN */
#include <surfos/pty.h>                 /* PTY_MAX, and the kernel's pty calls the stand-ins below give */
typedef struct surf_task {
    u_long pid;
    struct surf_task *parent;
    int state;                          /* 0, or TS_DEAD: it has ended, not collected yet */
    bool gone, late;                    /* collected; inside a call, it ends at the call's end (not at the hangup) */
    int pty;                            /* the pty it is on, -1 none */
    struct file *files[KF];             /* its descriptors */
} surf_task;
struct file { int pty; };
surf_task *curTask;
static surf_task kt[KT], k_init;
static int k_ntasks, k_spawn_err, k_hangups, k_dangling, k_spawn_bare;
static u_long k_pid = 100;
static struct file k_master[PTY_MAX];
static bool k_used[PTY_MAX];
static char k_out[PTY_MAX][16], k_log[160], k_argv[64];
static int k_out_len[PTY_MAX], k_cols[PTY_MAX], k_rows[PTY_MAX];
static u_long irq_save(void) { return 0; }
static void irq_restore(u_long flags) { (void)flags; }
void klog_write(const char *s);
surf_task *task_find(u_long pid);
int task_wait_opts(u_long pid, int *status, int options);
int task_detach(surf_task *t);
int fd_install(struct file *f);
struct file *fd_get(int fd);
int fd_close(int fd);
static bool k_adopted[PTY_MAX];                 /* 10/25/26: the ptys whose programs may open windows (kernel/win.c) */
int win_adopt(int pty) { k_adopted[pty] = true; return 0; }
void win_drop(int pty) { k_adopted[pty] = false; }
#include "../../shell/guisys_term.c"

void klog_write(const char *s) { strlcpy(k_log, s, sizeof(k_log)); }

surf_task *task_find(u_long pid) {
    int i;
    for(i = 0; i < k_ntasks; i++) if(!kt[i].gone && kt[i].pid == pid) return &kt[i];
    return NULL;
}

int task_wait_opts(u_long pid, int *status, int options) {
    surf_task *t = task_find(pid);
    (void)options;
    if(!t || t->parent != curTask) return -1;
    if(t->state != TS_DEAD) return 0;
    t->gone = true;
    *status = -1;
    return (int)pid;
}

int task_detach(surf_task *t) { t->parent = &k_init; return 0; }

static surf_task *k_task(int pty) {
    surf_task *t = &kt[k_ntasks++];
    memset(t, 0, sizeof(*t));
    t->pid = k_pid++;
    t->pty = pty;
    return t;
}

/* the master's last close (kernel/pty.c's master_release()): what is on the pty ends, or at its call's end */
static void k_hangup(struct file *f) {
    int i, left = 0;
    k_hangups++;
    for(i = 0; i < k_ntasks; i++) {
        if(kt[i].gone || kt[i].pty != f->pty || kt[i].state == TS_DEAD) continue;
        if(kt[i].late) left++;
        else kt[i].state = TS_DEAD;
    }
    if(!left) k_used[f->pty] = false;
}

/* 10/18/26 (the second review): a master closed while a session's place still points at it */
static void k_held(struct file *f) {
    int i;
    for(i = 0; i < SESSIONS; i++) if(sessions[i].master == f) k_dangling++;
}

int fd_install(struct file *f) {
    int i;
    for(i = 0; i < KF; i++) if(!curTask->files[i]) { curTask->files[i] = f; return i; }
    return -EMFILE;
}

struct file *fd_get(int fd) { return fd >= 0 && fd < KF ? curTask->files[fd] : NULL; }

int fd_close(int fd) {
    struct file *f = fd_get(fd);
    if(!f) return -1;
    k_held(f);
    curTask->files[fd] = NULL;
    k_hangup(f);
    return 0;
}

/* a task collected by its parent: reap()'s fd_close_all() */
static void k_reaped(surf_task *t) {
    int i;
    for(i = 0; i < KF; i++) if(t->files[i]) { k_hangup(t->files[i]); t->files[i] = NULL; }
    t->gone = true;
}

int pty_open(struct file **master) {
    int i;
    for(i = 0; i < PTY_MAX; i++) {
        if(k_used[i]) continue;
        k_used[i] = true;
        k_master[i].pty = i;
        k_out_len[i] = 0;
        *master = &k_master[i];
        return 0;
    }
    return -EAGAIN;
}

int pty_spawn(struct file *master, const char *path, int argc, char *const argv[]) {
    surf_task *t;
    int i;
    for(i = 0; i < KF && curTask->files[i] != master; i++) ;
    if(i == KF) k_spawn_bare++;                     /* 10/18/26: the master is a descriptor before the shell starts */
    if(k_spawn_err) return k_spawn_err;
    snprintf(k_argv, sizeof(k_argv), "%s %d %s %s", path, argc, argv[0], argv[1]);
    t = k_task(master->pty);
    t->parent = curTask;
    return (int)t->pid;
}

int pty_read(struct file *master, char *buf, u32 len) {
    int i, n = k_out_len[master->pty];
    if(n) {
        if(n > (int)len) n = (int)len;
        memcpy(buf, k_out[master->pty], (u32)n);
        memmove(k_out[master->pty], k_out[master->pty] + n, (u32)(k_out_len[master->pty] - n));
        k_out_len[master->pty] -= n;
        return n;
    }
    for(i = 0; i < k_ntasks; i++) if(!kt[i].gone && kt[i].pty == master->pty && kt[i].state != TS_DEAD) return 0;
    return -EPIPE;                                  /* nobody is on the pty: the session has ended */
}

int pty_write(struct file *master, const char *buf, u32 len) { (void)master; (void)buf; return (int)len; }
int pty_resize(struct file *master, u32 cols, u32 rows) { k_cols[master->pty] = (int)cols; k_rows[master->pty] = (int)rows; return 0; }
int pty_number(struct file *master) { return master->pty; }
void pty_close(struct file *master) { k_held(master); k_hangup(master); }

static void kernel_side(void) {
    static struct file spare;
    surf_task *a, *b, *c, *shell, *job;
    struct file *f;
    char buf[16];
    int h, i, k, fd, hs[PTY_MAX];
    k_init.pid = 1;
    a = k_task(-1);                                 /* two desktops' tasks */
    b = k_task(-1);
    curTask = a;
    h = guisys_term_open(NULL, 77, 21);
    shell = task_find((u_long)sessions[0].pid);
    CHECK(h == 0 && sessions[0].fd == 0 && a->files[0] == &k_master[0] && k_cols[0] == 77 && k_rows[0] == 21);
    CHECK(!strcmp(k_argv, "/initrd/bin/surfsh 2 surfsh -l") && strstr(k_log, "surfsh (pid") && strstr(k_log, "on pty0"));
    CHECK(shell && shell->parent == a && shell->pty == 0);
    CHECK(k_adopted[0]);                            /* 10/25/26: its programs may open windows on this desktop */
    memcpy(k_out[0], "hi", 2);
    k_out_len[0] = 2;
    CHECK(guisys_term_read(NULL, h, buf, sizeof(buf)) == 2 && !memcmp(buf, "hi", 2));
    CHECK(guisys_term_read(NULL, h, buf, sizeof(buf)) == 0);
    CHECK(guisys_term_write(NULL, h, "ls\r", 3) == 3 && guisys_term_resize(NULL, h, 117, 26) == 0 && k_cols[0] == 117);
    curTask = b;                                    /* another desktop's task has none of it */
    CHECK(guisys_term_read(NULL, h, buf, sizeof(buf)) == -EINVAL && guisys_term_write(NULL, h, "x", 1) == -EINVAL);
    CHECK(guisys_term_resize(NULL, h, 80, 24) == -EINVAL && guisys_term_read(NULL, -1, buf, 1) == -EINVAL);
    guisys_term_close(NULL, h);
    CHECK(sessions[0].used && !k_hangups);
    curTask = a;
    /* the shell exits, a program it left on the pty goes on: what is left, then the end */
    job = k_task(0);
    job->parent = shell;
    shell->state = TS_DEAD;
    memcpy(k_out[0], "bye", 3);
    k_out_len[0] = 3;
    CHECK(guisys_term_read(NULL, h, buf, sizeof(buf)) == 3 && shell->gone && !sessions[0].pid);
    CHECK(guisys_term_read(NULL, h, buf, sizeof(buf)) == -EPIPE);
    guisys_term_close(NULL, h);                     /* the hangup ends that program, the descriptor goes */
    CHECK(k_hangups == 1 && !a->files[0] && job->state == TS_DEAD && !sessions[0].used && !k_used[0]);
    CHECK(!k_adopted[0]);                           /* and no more windows from that pty */
    CHECK(strstr(k_log, "the session on pty0 ended") && !strstr(k_log, "init"));
    /* a session whose shell ends by itself, no program left: the end without the shell's help too */
    h = guisys_term_open(NULL, 77, 21);
    shell = task_find((u_long)sessions[h].pid);
    shell->state = TS_DEAD;
    CHECK(guisys_term_read(NULL, h, buf, sizeof(buf)) == -EPIPE && shell->gone);
    guisys_term_close(NULL, h);
    CHECK(k_hangups == 2 && !sessions[h].used);
    /* a shell inside a call when the session closes: init collects it */
    h = guisys_term_open(NULL, 77, 21);
    shell = task_find((u_long)sessions[h].pid);
    shell->late = true;
    guisys_term_close(NULL, h);
    CHECK(shell->parent == &k_init && shell->state != TS_DEAD && strstr(k_log, "init collects its shell"));
    CHECK(k_hangups == 3 && !sessions[h].used && !a->files[0]);
    shell->state = TS_DEAD;                         /* it ends; init collects it */
    shell->gone = true;
    k_used[shell->pty] = false;
    /* a shell that cannot start: the pty and the place are free again */
    k_spawn_err = -ENOENT;
    CHECK(guisys_term_open(NULL, 77, 21) == -ENOENT && k_hangups == 4 && !a->files[0]);
    k_spawn_err = 0;
    for(i = k = 0; i < PTY_MAX; i++) k += sessions[i].used || k_used[i];
    CHECK(!k);
    /* every pty in use: the next waits; every master a descriptor of the desktop's task */
    for(i = 0; i < PTY_MAX; i++) hs[i] = guisys_term_open(NULL, 77, 21);
    for(i = k = 0; i < PTY_MAX; i++) k += hs[i] == i && sessions[i].used && sessions[i].fd == i && a->files[i] == &k_master[i];
    CHECK(k == PTY_MAX && guisys_term_open(NULL, 77, 21) == -EAGAIN);
    /* the desktop's task dies without closing them (killed at once, no gui_finish()): collected, its descriptors close
       (eight hangups); the next open, by another task, takes the places back and closes nothing */
    a->state = TS_DEAD;
    k_reaped(a);
    CHECK(k_hangups == 12);
    curTask = b;
    h = guisys_term_open(NULL, 77, 21);
    CHECK(h >= 0 && k_hangups == 12 && sessions[h].owner == b->pid);
    for(i = k = 0; i < PTY_MAX; i++) k += sessions[i].used;
    CHECK(k == 1);
    guisys_term_close(NULL, h);
    CHECK(k_hangups == 13);
    /* 10/18/26 (the second review): a task killed at once inside an open or a close leaves its places claimed, the
       master not in them (not there yet, or taken out to close) but in its descriptors: the next open takes the
       places back and closes nothing; the task's end closes the master, once */
    c = k_task(-1);
    curTask = c;
    sessions[1] = (struct session){ .used = true, .master = NULL, .fd = -1, .owner = c->pid };    /* before pty_open */
    f = NULL;
    CHECK(pty_open(&f) == 0 && f);
    fd = f ? fd_install(f) : -1;
    CHECK(fd == 0);
    sessions[2] = (struct session){ .used = true, .master = NULL, .fd = fd, .owner = c->pid, .pty = f->pty };
    c->state = TS_DEAD;
    curTask = b;
    h = guisys_term_open(NULL, 77, 21);
    CHECK(h == 0 && !sessions[1].used && !sessions[2].used && k_hangups == 13 && k_used[f->pty]);
    k_reaped(c);
    CHECK(k_hangups == 14 && !k_used[f->pty]);
    guisys_term_close(NULL, h);
    CHECK(k_hangups == 15);
    /* no free descriptor: no session (-EMFILE, as sys_openpty() answers), and the pty is free again */
    for(i = 0; i < KF; i++) b->files[i] = &spare;
    CHECK(guisys_term_open(NULL, 77, 21) == -EMFILE && k_hangups == 16);
    for(i = k = 0; i < PTY_MAX; i++) k += sessions[i].used || k_used[i];
    CHECK(!k);
    for(i = 0; i < KF; i++) b->files[i] = NULL;
    /* a master was never closed while a place pointed at it, and always a descriptor when its shell started */
    CHECK(!k_dangling && !k_spawn_bare);
}

int main(void) {
    emulator();
    sizes();
    text();
    starting();
    keys();
    resizing();
    scrollback();
    selecting();
    pasting();
    ending();
    closing();
    no_system();
    drawing();
    widths();
    whole_keys();
    blank_copies();
    broken_words();
    budget();
    lost();
    gone_selection();
    flood_resize();
    past_wrap();
    paste_chars();
    shown_copy();
    fuzz_vt();
    fuzz_model();
    kernel_side();
    windows(640, 400);
    windows(960, 540);
    windows(1280, 800);
    return fake_done("test_guiterm");
}
