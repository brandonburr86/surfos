/* Host tests for watch's text (user/bin/watchtext.h), built as it is with the host's C library (under clang's
   AddressSanitizer and UBSan by hand on the Mac, never in `make unittest`, whose image hangs on a sanitized binary:
   docs/superpowers/plans/2026-11-01-cli-suite-sys.md): lines, wrapping, what moves the cursor; characters, whole, cut between two
   reads at every place, damaged and truncated; escape sequences and control characters, which never reach the terminal,
   in pieces too; more text than the screen holds; the command's words and the title row; and random bytes, on small and
   huge screens, in reads of every size, which must leave the same screen as one read and only text fit to show.
   No real tool to compare with: Linux's watch draws with curses, and its screen cannot be read back here.
   Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "watchtext.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

static struct wscreen w;
static char row[TV_MAX_COLS * 4 + 1];

static const char *line(int y) {
    size_t n = ws_text(&w, y, w.cols, row);
    row[n] = 0;
    return row;
}

/* a screen of that size with this text on it, in one read */
static void show(int rows, int cols, const char *s) {
    CHECK(ws_size(&w, rows, cols) == 0);
    w.utf8 = 1;
    ws_feed(&w, s, strlen(s));
    ws_finish(&w);
}

static void lines(void) {
    show(4, 10, "one\ntwo\n");
    CHECK(!strcmp(line(0), "one") && !strcmp(line(1), "two") && !strcmp(line(2), "") && !strcmp(line(3), ""));
    show(4, 10, "one\r\ntwo\r\n");                  /* CR LF */
    CHECK(!strcmp(line(0), "one") && !strcmp(line(1), "two") && !strcmp(line(2), ""));
    show(4, 5, "abcdefghijkl\nz");                  /* a long line goes on in the next row */
    CHECK(!strcmp(line(0), "abcde") && !strcmp(line(1), "fghij") && !strcmp(line(2), "kl") && !strcmp(line(3), "z"));
    show(4, 5, "abcde\nfghij\nk");                  /* a line of exactly the width leaves no row empty */
    CHECK(!strcmp(line(0), "abcde") && !strcmp(line(1), "fghij") && !strcmp(line(2), "k"));
    show(2, 5, "a\nb\nc\nd\n");                     /* past the last row it goes nowhere */
    CHECK(!strcmp(line(0), "a") && !strcmp(line(1), "b"));
    show(2, 3, "abcdefghijklmnop");
    CHECK(!strcmp(line(0), "abc") && !strcmp(line(1), "def"));
    show(3, 20, "a\tb\n\tc\n1234567\t8");           /* TAB: the next multiple of 8, blanks under it */
    CHECK(!strcmp(line(0), "a       b") && !strcmp(line(1), "        c") && !strcmp(line(2), "1234567 8"));
    show(2, 10, "12345678\tx");                     /* a TAB past the end of the row */
    CHECK(!strcmp(line(0), "12345678") && !strcmp(line(1), "x"));
    show(2, 6, "1234567\t8");
    CHECK(!strcmp(line(0), "123456") && !strcmp(line(1), "7"));
    show(2, 10, "abc\rX\n");                        /* CR goes back and writes over */
    CHECK(!strcmp(line(0), "Xbc"));
    show(2, 10, "abc\b\bX");
    CHECK(!strcmp(line(0), "aXc"));
    show(2, 10, "\bq");                             /* at the start of the row BS stays */
    CHECK(!strcmp(line(0), "q"));
    show(2, 5, "abcde\bX");                         /* BS with the row just filled */
    CHECK(!strcmp(line(0), "abcdX"));
    show(2, 10, "");
    CHECK(!strcmp(line(0), "") && !strcmp(line(1), ""));
    show(2, 10, "abc");
    ws_clear(&w);
    CHECK(!strcmp(line(0), "") && !w.row && !w.col);
    ws_feed(&w, "d", 1);
    CHECK(!strcmp(line(0), "d"));
    show(2, 10, "abcdefgh");                        /* less than the row's width, for the bottom row of the screen */
    { size_t n = ws_text(&w, 0, 5, row); row[n] = 0; CHECK(!strcmp(row, "abcde")); }
}

static void characters(void) {
    static const struct { const char *in, *want; } t[] = {
        { "caf\xc3\xa9", "caf\xc3\xa9" }, { "\xe2\x89\x8b>", "\xe2\x89\x8b>" }, { "\xf0\x9f\x8c\x8a!", "\xf0\x9f\x8c\x8a!" },
        { "a\xff" "b", "a?b" }, { "\x80\x80", "??" }, { "\xc0\x80", "??" }, { "\xe0\x80\x80z", "???z" },
        { "\xed\xa0\x80z", "???z" }, { "\xf4\x90\x80\x80z", "????z" }, { "\xc2\x80z", "??z" },
        { "\xe2\x89z", "??z" }, { "\xf0\x9f\x8cz", "???z" }, { "\xc3\xc3\xa9", "?\xc3\xa9" }, { "\xe2\xe2\x89\x8b", "?\xe2\x89\x8b" },
        { "\xc3", "?" }, { "\xe2\x89", "??" }, { "\xf0\x9f\x8c", "???" },       /* the text ends inside a character */
    };
    unsigned int i;
    for(i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        size_t n = strlen(t[i].in), cut;
        show(2, 20, t[i].in);
        CHECK(!strcmp(line(0), t[i].want));
        for(cut = 1; cut < n; cut++) {              /* the same, whatever read it was cut by */
            CHECK(ws_size(&w, 2, 20) == 0);
            w.utf8 = 1;
            ws_feed(&w, t[i].in, cut);
            ws_feed(&w, t[i].in + cut, n - cut);
            ws_finish(&w);
            CHECK(!strcmp(line(0), t[i].want));
        }
    }
    show(2, 3, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");     /* a cell a character, and the row wraps between characters */
    CHECK(!strcmp(line(0), "\xc3\xa9\xc3\xa9\xc3\xa9") && !strcmp(line(1), "\xc3\xa9"));
    CHECK(ws_size(&w, 2, 20) == 0);                 /* the terminal has no UTF-8 */
    w.utf8 = 0;
    ws_feed(&w, "caf\xc3\xa9 \xe2\x89\x8b", 9);
    ws_finish(&w);
    CHECK(!strcmp(line(0), "caf? ?"));
    show(2, 20, "\xe2\x89\n");                      /* a line's end cuts a character short */
    CHECK(!strcmp(line(0), "??") && !w.npend);
    show(2, 20, "\xe2\x1b[1mz");                    /* and so does an escape sequence, which still ends */
    CHECK(!strcmp(line(0), "?z"));
}

static void sequences(void) {
    static const struct { const char *in, *want; } t[] = {
        { "\x1b[31mred\x1b[0m", "red" }, { "\x1b[1;33;44mX", "X" }, { "a\x1b[2Jb", "ab" }, { "a\x1b[Hb", "ab" },
        { "a\x1b[?1049hb", "ab" }, { "a\x1b[?25lb", "ab" }, { "a\x1b[38;5;196mb", "ab" }, { "a\x1b[K", "a" },
        { "a\x1b" "7b", "ab" }, { "a\x1b" "8b", "ab" }, { "a\x1b" "cb", "ab" }, { "a\x1b(Bb", "ab" }, { "a\x1b)0b", "ab" },
        { "a\x1b]0;title\x07" "b", "ab" }, { "a\x1b]0;title\x1b\\b", "ab" }, { "a\x1bPdata\x1b\\b", "ab" },
        { "a\x1b_x\x1b\\b", "ab" }, { "a\x1b^x\x1b\\b", "ab" }, { "a\x1bXx\x1b\\b", "ab" },
        { "\x01\x02\x03\x04\x05\x06\x07z\x0b\x0c\x0e\x0f\x10\x11\x7f", "z" },
        { "a\x1b[", "a" }, { "a\x1b", "a" }, { "a\x1b]", "a" },              /* the text ends inside one */
        { "a\x1b[1\x03" "b", "ab" }, { "a\x1b\x03" "b", "ab" },              /* a control character ends one */
        { "a\x1b[1\x1b[2mb", "ab" }, { "a\x1b\x1b[2mb", "ab" },              /* and so does the Escape of the next */
    };
    unsigned int i;
    for(i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        size_t n = strlen(t[i].in), cut;
        show(2, 20, t[i].in);
        CHECK(!strcmp(line(0), t[i].want));
        for(cut = 1; cut < n; cut++) {
            CHECK(ws_size(&w, 2, 20) == 0);
            w.utf8 = 1;
            ws_feed(&w, t[i].in, cut);
            ws_feed(&w, t[i].in + cut, n - cut);
            ws_finish(&w);
            CHECK(!strcmp(line(0), t[i].want));
        }
    }
    show(3, 20, "a\x1b]0;never ended\nb\n");        /* a title that never ends does not swallow the text */
    CHECK(!strcmp(line(0), "a") && !strcmp(line(1), "b"));
    show(2, 20, "a\x1b[\nb");                       /* nor a sequence */
    CHECK(!strcmp(line(0), "a") && !strcmp(line(1), "b"));
    { char nul[] = { 'a', 0, 'b' }; CHECK(ws_size(&w, 2, 20) == 0); ws_feed(&w, nul, 3); CHECK(!strcmp(line(0), "ab")); }
}

static void words(void) {
    char *one[] = { "date" }, *three[] = { "ls", "-l", "/hda1" }, *empty[] = { "" };
    char out[WATCH_TEXT_MAX + 1], big[WATCH_TEXT_MAX + 2];
    char *two[2];
    CHECK(ws_join(1, one, out, sizeof(out)) == 0 && !strcmp(out, "date"));
    CHECK(ws_join(3, three, out, sizeof(out)) == 0 && !strcmp(out, "ls -l /hda1"));
    CHECK(ws_join(1, empty, out, sizeof(out)) == 0 && !strcmp(out, ""));
    memset(big, 'x', sizeof(big));
    big[WATCH_TEXT_MAX] = 0;
    one[0] = big;
    CHECK(ws_join(1, one, out, sizeof(out)) == 0 && strlen(out) == WATCH_TEXT_MAX);     /* exactly what a program may take */
    big[WATCH_TEXT_MAX] = 'x';
    big[WATCH_TEXT_MAX + 1] = 0;
    CHECK(ws_join(1, one, out, sizeof(out)) < 0);
    big[WATCH_TEXT_MAX - 1] = 0;
    two[0] = big;
    two[1] = "y";
    CHECK(ws_join(2, two, out, sizeof(out)) < 0);       /* the blank between them counts */
    big[WATCH_TEXT_MAX - 2] = 0;
    CHECK(ws_join(2, two, out, sizeof(out)) == 0 && strlen(out) == WATCH_TEXT_MAX);
}

static void title(void) {
    char out[TV_MAX_COLS * 4 + 256], want[128];
    size_t n;
    const char *when = "2026-11-01 14:03:22";
    n = ws_title(2000, "date", when, 80, 1, out, sizeof(out));
    snprintf(want, sizeof(want), "Every 2s: date%*s%s", 80 - 14 - 19, "", when);
    CHECK(!strcmp(out, want) && n == 80);
    n = ws_title(500, "ps", when, 40, 1, out, sizeof(out));
    CHECK(!strncmp(out, "Every 0.5s: ps", 14) && n == 40 && !strcmp(out + 21, when));
    n = ws_title(2000, "a command too long for this narrow screen by far", when, 40, 1, out, sizeof(out));
    CHECK(!strncmp(out, "Every 2s: a command  ", 21) && !strcmp(out + 21, when) && n == 40);
    n = ws_title(2000, "date", when, 30, 1, out, sizeof(out));
    CHECK(!strcmp(out, "Every 2s: date") && n == 14);      /* no room for both: the command */
    n = ws_title(2000, "a long enough command for this", when, 20, 1, out, sizeof(out));
    CHECK(!strcmp(out, "Every 2s: a long eno") && n == 20);
    n = ws_title(2000, "e\x1b[2J\xc3\xa9", when, 80, 1, out, sizeof(out));
    CHECK(!strncmp(out, "Every 2s: e?[2J\xc3\xa9 ", 18) && !strchr(out, 0x1b));
    CHECK(tv_chars(out, n) == 80);                         /* a two-byte character is one cell of the row */
    n = ws_title(2000, "\xc3\xa9", when, 80, 0, out, sizeof(out));
    CHECK(!strncmp(out, "Every 2s: ? ", 12) && tv_chars(out, n) == 80);
}

/* text fit to show: valid UTF-8, no control character, no more than width characters */
static int fit(const char *s, size_t n, int width, int utf8) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    int chars = 0;
    while(i < n) {
        int len = tv_utf8(p + i, n - i);
        if(len < 1 || (len == 1 && (p[i] < 0x20 || p[i] == 0x7f))) return 0;
        if(len > 1 && !utf8) return 0;
        i += (size_t)len;
        chars++;
    }
    return chars <= width;
}

static unsigned int rnd_state = 54321;
static unsigned int rnd(void) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return rnd_state >> 8;
}

/* bytes that find the corners: sequences, starts of characters and plain text */
static void random_bytes(unsigned char *b, size_t n) {
    static const unsigned char spice[] = { 0x1b, '[', ']', 0x07, '\\', '\n', '\r', '\t', '\b', 0, 0xc3, 0xa9, 0xe2, 0x89, 0x8b, 0xf0,
                                           0x9f, 0x80, 0xc2, 0xff, ';', '0', 'm', '?', 'P', '(' };
    size_t i;
    for(i = 0; i < n; i++) {
        unsigned int r = rnd();
        if(r & 0x100) b[i] = spice[(r >> 16) % sizeof(spice)];
        else if(r & 0x200) b[i] = (unsigned char)(0x20 + (r >> 16) % 0x5f);
        else b[i] = (unsigned char)(r >> 16);
    }
}

static void random_screens(void) {
    static const struct { int rows, cols; } sizes[] = { { 1, 1 }, { 1, 16 }, { 3, 16 }, { 2, 3 }, { 23, 80 }, { 31, 120 }, { 254, 512 } };
    static unsigned char buf[20000];
    int round;
    for(round = 0; round < 700; round++) {
        struct wscreen a, b;
        int rows = sizes[(unsigned)round % 7].rows, cols = sizes[(unsigned)round % 7].cols, utf8 = round & 1, y;
        size_t n = rnd() % (round % 7 == 6 ? sizeof(buf) : 3000), i = 0;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        CHECK(ws_size(&a, rows, cols) == 0 && ws_size(&b, rows, cols) == 0);
        a.utf8 = b.utf8 = utf8;
        random_bytes(buf, n);
        ws_feed(&a, buf, n);                        /* one read */
        while(i < n) {                              /* reads of any size */
            size_t k = 1 + rnd() % 40;
            if(k > n - i) k = n - i;
            ws_feed(&b, buf + i, k);
            i += k;
        }
        CHECK(a.row == b.row && a.col == b.col && a.esc == b.esc && a.npend == b.npend);
        CHECK(!memcmp(a.cells, b.cells, (size_t)rows * (size_t)cols * sizeof(a.cells[0])));
        ws_finish(&a);
        CHECK(!a.npend && !a.esc && a.col >= 0 && a.col <= cols && a.row >= 0);
        for(y = 0; y < rows; y++) {
            size_t len = ws_text(&a, y, cols, row);
            CHECK(len <= (size_t)cols * 4 && fit(row, len, cols, utf8));
        }
        free(a.cells);
        free(b.cells);
    }
}

/* the review of 11/01/26: every line end counted a row, past the last one too, so 2^31 of them, which a pipe carries in
   minutes, took the row round to below 0 and the next character outside the cells. It stops at the end of the screen. */
static void row_bounded(void) {
    int i;
    show(3, 5, "");
    for(i = 0; i < 100; i++) ws_feed(&w, "\n", 1);
    CHECK(w.row == 3);
    show(3, 5, "");
    for(i = 0; i < 100; i++) ws_feed(&w, "abcdefgh", 8);        /* and so does a line that wraps */
    CHECK(w.row == 3);
    show(3, 5, "a\nb\nc\n\n\nd\ne");                          /* with what fits still there */
    CHECK(!strcmp(line(0), "a") && !strcmp(line(1), "b") && !strcmp(line(2), "c") && w.row == 3);
}

static void oversized(void) {
    size_t i, n = 1 << 24;
    char *big = malloc(1 << 16);
    CHECK(big != NULL);
    memset(big, 'x', 1 << 16);
    show(2, 16, "");
    for(i = 0; i < n; i += 1 << 16) ws_feed(&w, big, 1 << 16);  /* 16 MB without a newline */
    CHECK(!strcmp(line(0), "xxxxxxxxxxxxxxxx") && !strcmp(line(1), "xxxxxxxxxxxxxxxx"));
    CHECK(w.row == 2 && w.col <= 16);
    memset(big, '\n', 1 << 16);
    for(i = 0; i < n; i += 1 << 16) ws_feed(&w, big, 1 << 16);
    CHECK(!strcmp(line(1), "xxxxxxxxxxxxxxxx") && w.row == 2);
    free(big);
}

int main(void) {
    lines();
    characters();
    sequences();
    words();
    title();
    random_screens();
    row_bounded();
    oversized();
    free(w.cells);
    free(tv_out);
    printf("test_watch: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
