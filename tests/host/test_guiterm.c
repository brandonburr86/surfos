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
    char b[64];
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
    CHECK(v.count == GUI_VT_LINES && v.dropped == 4 + 50 + 3 - 3 && v.y == 2);   /* the oldest go */
    CHECK(v.dropped + (u32)v.count == 4u + GUI_VT_LINES + 50u);
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
    snprintf(b, sizeof(b), "%d", v.y);
    CHECK(b[0] == '2');
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
    char out[256];
    bool cut;
    u32 n;
    int a, b;
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
    vt_word(&v, 0, 2, &a, &b);
    CHECK(a == 0 && b == 5);
    vt_word(&v, 0, 6, &a, &b);
    CHECK(a == 6 && b == 7);
    vt_word(&v, 1, 9, &a, &b);
    CHECK(a == 0 && b == 10);
    feed("\r\n(a/b/c,d)");
    vt_word(&v, vt_screen(&v) + 3, 3, &a, &b);
    CHECK(a == 1 && b == 6);                        /* a path, without the brackets and the comma */
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
    int w, h;
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
    /* what was printed at the old size is read before the new one is set */
    fake_term_feed("old size", 8);
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    CHECK(!strncmp(row_of(&g.term.vt, 0), "old size", 8) && fake_term_cols == 117);
    /* the cursor stays on the screen */
    gui_act(&g, ACT_MAXIMIZE);
    tick();
    CHECK(g.term.vt.x < g.term.vt.cols && g.term.vt.y < g.term.vt.rows);
}

/* the scrollback: the wheel, the scroll bar, output while scrolled back, a key back to the end */
static void scrollback(void) {
    struct fb_surface screen;
    char b[64];
    int i, top;
    terminal(960, 540);
    for(i = 0; i < 600; i++) {
        snprintf(b, sizeof(b), "line %03d\n", i);
        fake_term_feed(b, (u32)strlen(b));
        if(fake_term_out_len > 6000) tick();
    }
    tick();
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
    for(i = 0; i < GUI_VT_LINES; i++) fake_term_feed("z\n", 2), i % 1000 == 999 ? tick() : (void)0;
    tick();
    tick();
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
    /* by the keyboard: the Edit menu (Ctrl+G, right twice, down to Copy) */
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
    for(i = 0; i < 40; i++) { fake_term_in_len = 0; tick(); }
    CHECK(g.term.pend_len == 0 || g.term.pend_len - g.term.pend_at == GUI_CLIP_MAX - 41 * sizeof(fake_term_in));
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
            fake_term_feed(b, (u32)strlen(b));
            if(k % 50 == 49) gui_tick(&g);
        }
        gui_tick(&g);
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
    windows(640, 400);
    windows(960, 540);
    windows(1280, 800);
    return fake_done("test_guiterm");
}
