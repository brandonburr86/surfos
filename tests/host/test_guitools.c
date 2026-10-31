/* Host tests for Log, Clock and Character Map (track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md), against the
   in-memory file system and system of tests/host/gui_fake.c. Built and run by `make unittest` (unittest-gui).
   10/26/26 (stage C12 of docs/superpowers/plans/2026-10-25-app-windows.md): the Clock is a program now (user/wclock/):
   its tests went with it to tests/host/test_wclock.c and tools/gui_apps_tools.py; its Help lines stay here. So is
   the Character Map (user/wcharmap/), whose tests went to tests/host/test_wcharmap.c.
   The Log reads a kernel log of its own here: a stream of everything "written", of which it gets the last 16 KB, as
   klog_copy() gives them, so that the log can wrap around. And kernel/klog.c itself, klog_copy() with an interrupt
   that logs while it copies. */
#include "gui_fake.h"
#include "../../shell/gui_tools.h"
#include <stdlib.h>

/**** kernel/klog.c, on the host: its kernel headers' guards taken, and the little it uses of them here; irq_save()
      lets an "interrupt" log a storm of text at a chosen call (between two of klog_copy()'s chunks) ****/
#define _SURFOS_IRQ_H
#define _SURFOS_CONSOLE_H
#define _SURFOS_TASK_H
#define _SURFOS_INTERRUPT_H
typedef struct surf_console surf_console;
static struct { surf_console *con; } *curTask;
static surf_console *conActive;
#define KERN_TXT_COLOR 0
static char dumped[2 * 16384];                  /* what klog_dump() printed */
static u32 dumped_len;
static void kputch(surf_console *con, int color, char c) {
    (void)con; (void)color;
    if(dumped_len < sizeof(dumped)) dumped[dumped_len++] = c;
}
static bool in_interrupt(void) { return false; }
void klog_write(const char *s);
static int saves, storm_at;
static const char *storm;
static u_long irq_save(void) {
    if(storm && ++saves == storm_at) {
        const char *s = storm;
        storm = NULL;
        klog_write(s);
    }
    return 0;
}
static void irq_restore(u_long flags) { (void)flags; }
#include "../../kernel/klog.c"

static struct gui_state g;
static struct fb_surface screen;
static int W, H;                                /* the screen's size */
static const int ids[] = { GUI_LOG };                 /* 10/26/26: the Clock and the Character Map are programs */
#define IDS (int)(sizeof(ids) / sizeof(ids[0]))

/**** the screen ****/

static void draw(void) { gui_render(&g, &screen); }

/* the screen shows text at x, y in colour c: every lit pixel of its glyphs (scale: each a square of that many) */
static bool shows(int x, int y, const char *s, u32 c, int scale) {
    int i, row, col;
    for(i = 0; s[i]; i++)
        for(row = 0; row < 16; row++)
            for(col = 0; col < 8; col++)
                if((font8x16[(u8)s[i]][row] & (0x80 >> col)) &&
                   fake_pixel(W, x + (i * 8 + col) * scale, y + row * scale) != c) return false;
    return true;
}

static bool glyph_at(int x, int y, u8 ch, u32 c) {
    char s[2] = { (char)ch, 0 };
    return shows(x, y, s, c, 1);
}

static void desktop(int width, int height) {
    W = width;
    H = height;
    fake_desktop(&g, width, height);
    fake_screen(&screen, width, height);
}

static void key(int k) { gui_key(&g, k); }

static void click(int x, int y) { fake_click(&g, x, y); }

static void button(int x, int y, bool held) {
    struct pointer_event e;
    memset(&e, 0, sizeof(e));
    e.x = x; e.y = y; e.buttons = held ? POINTER_LEFT : 0;
    gui_pointer(&g, &e);
}

static void wheel_at(int x, int y, int dz) {
    struct pointer_event e;
    memset(&e, 0, sizeof(e));
    e.x = x; e.y = y; e.wheel = dz;
    gui_pointer(&g, &e);
}

/**** a kernel log that wraps around ****/

static char stream[256 * 1024];                /* everything ever written */
static u32 stream_len;
static int reads;
static struct gui_sys sys;                      /* fake_sys, with this log */

static u32 written_offset;                      /* added to `written`: as if the kernel had written that much before */

static int ring_log(void *ctx, char *buf, u32 size, u32 *len, u32 *written) {
    u32 n = stream_len < GUI_LOG_MAX ? stream_len : GUI_LOG_MAX;
    (void)ctx;
    if(n > size) n = size;
    memcpy(buf, stream + stream_len - n, n);
    *len = n;
    *written = stream_len + written_offset;
    reads++;
    return 0;
}

static void say(const char *s) {
    u32 n = strlen(s);
    memcpy(stream + stream_len, s, n);
    stream_len += n;
}

static void lines(int from, int to) {           /* "line 0001 ...": every tenth one with "marker" */
    char b[96];
    int i;
    for(i = from; i <= to; i++) {
        snprintf(b, sizeof(b), "line %04d %s the quick brown fox\n", i, i % 10 ? "and" : "marker");
        say(b);
    }
}

/* the line of the first "marker" from index at of the stream (cut at at), in out: its index, 0 for none */
static u32 first_marker_line(u32 at, char *out, u32 n) {
    u32 i, a, b;
    for(i = at; i + 6 <= stream_len && memcmp(stream + i, "marker", 6); i++) ;
    if(i + 6 > stream_len) return 0;
    for(a = i; a > at && stream[a - 1] != '\n'; a--) ;
    for(b = i; b < stream_len && stream[b] != '\n'; b++) ;
    if(b < stream_len) b++;
    if(b - a >= n) return 0;
    memcpy(out, stream + a, b - a);
    out[b - a] = 0;
    return i;
}

static void log_desktop(void) {
    desktop(960, 540);
    stream_len = 0;
    written_offset = 0;
    reads = 0;
    sys = fake_sys;
    sys.log = ring_log;
    g.sys = &sys;
}

/* the view's line at row (its text up to the line end) begins with s */
static bool row_is(int row, const char *s) {
    char b[400];
    return log_line_text(&g, row, b, sizeof(b)) >= 0 && !strncmp(b, s, strlen(s));
}

static void first_rows(int *first, int *rows, int *total) { log_view(&g, first, rows, total); }

static void tick_ms(u32 ms) { g.now += ms; gui_tick(&g); }

/**** klog_copy() ****/

static char kwritten[1024 * 1024];             /* everything klog_write() was given, here */
static u32 kwritten_len;

static void klog_say(const char *s) {
    u32 n = strlen(s);
    memcpy(kwritten + kwritten_len, s, n);
    kwritten_len += n;
    klog_write(s);
}

static void klog_copying(void) {
    static char buf[KLOG_SIZE + 64], text[1100], big[9001];
    u_long n, w = 1;
    int i;
    n = klog_copy(buf, sizeof(buf), &w);
    CHECK(n == 0 && w == 0);
    klog_say("hello\n");
    n = klog_copy(buf, sizeof(buf), &w);
    CHECK(n == 6 && w == 6 && !memcmp(buf, "hello\n", 6));
    n = klog_copy(buf, 3, NULL);                        /* a smaller buffer: the end */
    CHECK(n == 3 && !memcmp(buf, "lo\n", 3));
    for(i = 0; i < 40; i++) {                           /* 40 KB: the ring wraps around */
        memset(text, 'a' + i % 26, 1023);
        text[1023] = '\n';
        text[1024] = 0;
        klog_say(text);
    }
    n = klog_copy(buf, sizeof(buf), &w);
    CHECK(n == KLOG_SIZE && w == kwritten_len && !memcmp(buf, kwritten + kwritten_len - KLOG_SIZE, KLOG_SIZE));
    CHECK(klog_length() == KLOG_SIZE);
    /* 9000 bytes logged between its second and third chunk: the oldest bytes wanted are gone, so the copy is
       shorter, and still the end as it was when it started */
    memset(big, 'z', 9000);
    big[9000] = 0;
    saves = 0;
    storm_at = 3;
    storm = big;
    n = klog_copy(buf, sizeof(buf), &w);
    CHECK(!storm && w == kwritten_len);
    CHECK(n == 7 * 1024 && !memcmp(buf, kwritten + kwritten_len - n, n));
    kwritten_len += 9000;
    memset(kwritten + kwritten_len - 9000, 'z', 9000);
    n = klog_copy(buf, sizeof(buf), &w);                /* and the next copy has the storm at its end */
    CHECK(n == KLOG_SIZE && w == kwritten_len && !memcmp(buf, kwritten + kwritten_len - KLOG_SIZE, KLOG_SIZE));
    /* a storm before the very first chunk: whatever is copied is still the end */
    saves = 0;
    storm_at = 2;
    storm = big;
    n = klog_copy(buf, sizeof(buf), &w);
    CHECK(!storm && n == 7 * 1024 && w == kwritten_len && !memcmp(buf, kwritten + kwritten_len - n, n));
    kwritten_len += 9000;
    memset(kwritten + kwritten_len - 9000, 'z', 9000);
}

/**** Log ****/

static void log_reading(void) {
    const struct gui_window *w;
    struct gui_logview *v = &g.logview;
    int first, rows, total, old;
    char b[400];
    log_desktop();
    lines(1, 10);
    gui_open(&g, GUI_LOG);
    w = &g.win[GUI_LOG];
    CHECK(reads == 1 && v->len == stream_len && v->written == stream_len && v->follow);
    first_rows(&first, &rows, &total);
    CHECK(first == 0 && total == 10 && rows > 10);
    draw();
    CHECK(shows(w->x + 8, w->y + TITLE_H + BAR_H + 4, "line 0001 and", INK, 1));
    CHECK(shows(w->x + 8, w->y + TITLE_H + BAR_H + 4 + 9 * 16, "line 0010 marker", INK, 1));
    /* what is new, about twice a second */
    lines(11, 13);
    tick_ms(100);
    CHECK(reads == 1);
    tick_ms(400);
    CHECK(reads == 2 && v->len == stream_len);
    first_rows(&first, &rows, &total);
    CHECK(total == 13 && row_is(12, "line 0013"));
    tick_ms(500);
    CHECK(reads == 3);                                  /* nothing new: read, but nothing changes */
    /* Follow: the end stays in view */
    lines(14, 120);
    tick_ms(500);
    first_rows(&first, &rows, &total);
    CHECK(total == 120 && first == total - rows && row_is(rows - 1, "line 0120"));
    draw();
    CHECK(shows(w->x + 8, w->y + TITLE_H + BAR_H + 4 + (rows - 1) * 16, "line 0120 marker", INK, 1));
    CHECK(shows(w->x + w->w - 24 - 9 * 8, w->y + w->h - STATUS_H - 1 + 3, "Follow on", TEAL, 1));
    /* scrolling up stops it: new lines do not move the view */
    key(GUI_UP);
    CHECK(!v->follow);
    first_rows(&first, &rows, &total);
    CHECK(first == total - rows - 1);
    log_line_text(&g, 0, b, sizeof(b));
    old = first;
    lines(121, 130);
    tick_ms(500);
    first_rows(&first, &rows, &total);
    CHECK(total == 130 && first == old && row_is(0, b));
    draw();
    CHECK(shows(w->x + w->w - 24 - 15 * 8, w->y + w->h - STATUS_H - 1 + 3, "Follow off: End", MUTED, 1));
    /* End starts it again */
    key(GUI_END);
    first_rows(&first, &rows, &total);
    CHECK(v->follow && first == total - rows);
    lines(131, 135);
    tick_ms(500);
    first_rows(&first, &rows, &total);
    CHECK(first == total - rows && row_is(rows - 1, "line 0135"));
    /* Home, Page Down, Page Up, Down to the end (Follow again), the wheel */
    key(GUI_HOME);
    first_rows(&first, &rows, &total);
    CHECK(first == 0 && !v->follow && row_is(0, "line 0001"));
    key(GUI_PGDN);
    first_rows(&first, &rows, &total);
    CHECK(first == rows - 1);
    key(GUI_PGUP);
    first_rows(&first, &rows, &total);
    CHECK(first == 0);
    for(old = 0; old < total - rows - 1; old++) key(GUI_DOWN);
    first_rows(&first, &rows, &total);
    CHECK(first == total - rows - 1 && !v->follow);
    key(GUI_DOWN);
    first_rows(&first, &rows, &total);
    CHECK(first == total - rows && v->follow);           /* moved down to the end: Follow is on again */
    wheel_at(w->x + 100, w->y + 100, 1);                /* the wheel away from you: up, three rows */
    first_rows(&first, &rows, &total);
    CHECK(first == total - rows - 3 && !v->follow);
    wheel_at(w->x + 100, w->y + 100, -1);
    first_rows(&first, &rows, &total);
    CHECK(first == total - rows && v->follow);
    /* the toolbar's Follow: off keeps the view, on goes to the end */
    click(w->x + 8 + 10, w->y + TITLE_H + 4 + 10);
    CHECK(!v->follow);
    key(GUI_HOME);
    click(w->x + 8 + 10, w->y + TITLE_H + 4 + 10);
    first_rows(&first, &rows, &total);
    CHECK(v->follow && first == total - rows);
    /* the scroll bar: a click above the thumb pages up, and the thumb drags */
    click(w->x + w->w - 2 - SCROLL_W + 4, w->y + TITLE_H + BAR_H + 4 + 4);
    first_rows(&first, &rows, &total);
    CHECK(first == total - 2 * rows && !v->follow);
    {
        int bar = w->h - STATUS_H - 1 - (TITLE_H + BAR_H + 4) + 2, top, size, by = w->y + TITLE_H + BAR_H + 2;
        gd_thumb(bar, first, rows, total, &top, &size);
        button(w->x + w->w - 2 - SCROLL_W + 4, by + top + size / 2, true);     /* the thumb */
        button(w->x + w->w - 2 - SCROLL_W + 4, by - 40, true);                  /* dragged past the top */
        button(w->x + w->w - 2 - SCROLL_W + 4, by - 40, false);
        first_rows(&first, &rows, &total);
        CHECK(first == 0 && !v->follow);
        button(w->x + w->w - 2 - SCROLL_W + 4, by + size / 2, true);           /* and down past the end: Follow */
        button(w->x + w->w - 2 - SCROLL_W + 4, by + bar + 40, true);
        button(w->x + w->w - 2 - SCROLL_W + 4, by + bar + 40, false);
        first_rows(&first, &rows, &total);
        CHECK(first == total - rows && v->follow);
    }
    gui_act(&g, ACT_MINIMIZE);                          /* tucked away: not read; read at once when it is back */
    CHECK(g.win[GUI_LOG].min);
    old = reads;
    lines(136, 140);
    tick_ms(3000);
    CHECK(reads == old);
    gui_open(&g, GUI_LOG);
    tick_ms(10);
    first_rows(&first, &rows, &total);
    CHECK(reads == old + 1 && total == 140 && row_is(rows - 1, "line 0140"));
    gui_close(&g, GUI_LOG);
    CHECK(!g.win[GUI_LOG].open);
    old = reads;                                        /* not read while it is closed */
    tick_ms(5000);
    CHECK(reads == old);
}

/* more than 16 KB written: the log wraps around, and nothing points at text that has gone */
static void log_wrap(void) {
    const struct gui_window *w;
    struct gui_logview *v = &g.logview;
    int first, rows, total, i;
    u32 at;
    char b[400], line[64];
    log_desktop();
    lines(1, 100);
    gui_open(&g, GUI_LOG);
    w = &g.win[GUI_LOG];
    /* a line selected near the top, another near the end, and a match */
    key(GUI_HOME);
    click(w->x + 50, w->y + TITLE_H + BAR_H + 4 + 2 * 16 + 4);       /* line 0003 */
    CHECK(v->any && !v->follow);
    key(3);                                             /* Ctrl+C copies it (and does not leave the desktop) */
    CHECK(!g.quit && g.clip_len == 34 && !memcmp(g.clip, "line 0003 and the quick brown fox\n", 34));
    lines(101, 700);                                    /* about 20 KB more: lines 1 to 220 or so drop off */
    CHECK(stream_len > GUI_LOG_MAX + 4000);
    tick_ms(500);
    CHECK(v->len == GUI_LOG_MAX && v->written == stream_len);
    CHECK(!v->any);                                     /* its line has gone: nothing is selected */
    key(GUI_HOME);
    first_rows(&first, &rows, &total);
    at = stream_len - GUI_LOG_MAX;                      /* the copy's first line is what is left of a line */
    for(i = 0; stream[at + i] != '\n'; i++) b[i] = stream[at + i];
    b[i] = 0;
    CHECK(first == 0 && row_is(0, b) && row_is(1, "line 0"));
    draw();
    CHECK(fake_guards(W, H));
    /* a selection that survives stays on its text */
    key(GUI_END);
    first_rows(&first, &rows, &total);
    click(w->x + 50, w->y + TITLE_H + BAR_H + 4 + (rows - 1) * 16 + 4);   /* line 0700 */
    key(3);
    CHECK(g.clip_len == 37 && !memcmp(g.clip, "line 0700 marker", 16));
    lines(701, 720);
    tick_ms(500);
    key(3);
    CHECK(g.clip_len == 37 && !memcmp(g.clip, "line 0700 marker", 16));
    first_rows(&first, &rows, &total);
    CHECK(!v->follow && row_is(rows - 1, "line 0700"));  /* selecting stopped Follow: the view stayed */
    /* find across the wrap: the next marker after the selection, then one that drops off */
    key(6);                                             /* Ctrl+F */
    CHECK(v->finding);
    fake_type(&g, "MARKER");
    key('\r');                                          /* the selected line's own, first */
    CHECK(v->match_len == 6 && !memcmp(v->text + (v->match - (v->written - v->len)), "marker", 6));
    key(3);
    CHECK(!memcmp(g.clip, "line 0700 marker", 16));
    key('\r');
    CHECK(!v->status[0]);
    key(3);
    CHECK(!memcmp(g.clip, "line 0710 marker", 16));
    key('\r');
    key(3);
    CHECK(!memcmp(g.clip, "line 0720 marker", 16));
    key('\r');                                          /* round to the top: the first marker left in the copy */
    CHECK(!strcmp(v->status, "Found it again from the top"));
    key(3);
    at = first_marker_line(stream_len - GUI_LOG_MAX, line, sizeof(line));
    CHECK(at && g.clip_len == strlen(line) && !memcmp(g.clip, line, g.clip_len));
    lines(721, 1300);                                   /* that match goes too */
    tick_ms(500);
    CHECK(v->match_len == 0 && !v->any);
    key('\r');                                          /* from the view's top: the next marker there */
    CHECK(v->match_len == 6);
    key(3);
    CHECK(strstr(g.clip, "marker") != NULL && !memcmp(g.clip, "line ", 5) && atoi(g.clip + 5) > 700);
    key(27);                                            /* Esc closes the bar, then the window */
    CHECK(!v->finding && g.win[GUI_LOG].open);
    key(27);
    CHECK(!g.win[GUI_LOG].open);
    /* not found */
    log_desktop();
    lines(1, 20);
    gui_open(&g, GUI_LOG);
    gui_act(&g, ACT_FIND);
    fake_type(&g, "zebra");
    key('\r');
    CHECK(v->match_len == 0 && !strcmp(v->status, "Not found: zebra"));
    CHECK(gui_menu_on(&g, 3, ACT_FIND_NEXT) && !gui_menu_on(&g, 3, ACT_PASTE) && !gui_menu_on(&g, 3, ACT_UNDO));
}

static void log_selection(void) {
    const struct gui_window *w;
    struct gui_logview *v = &g.logview;
    int first, rows, total, ty;
    char want[200];
    log_desktop();
    lines(1, 50);
    gui_open(&g, GUI_LOG);
    w = &g.win[GUI_LOG];
    ty = w->y + TITLE_H + BAR_H + 4;
    key(GUI_HOME);
    CHECK(!gui_menu_on(&g, 3, ACT_COPY));               /* nothing selected yet */
    /* drag over three lines (upwards), then copy them by the Edit menu */
    button(w->x + 60, ty + 6 * 16 + 3, true);
    button(w->x + 70, ty + 5 * 16 + 3, true);
    button(w->x + 80, ty + 4 * 16 + 3, true);
    button(w->x + 80, ty + 4 * 16 + 3, false);
    CHECK(gui_menu_on(&g, 3, ACT_COPY));
    gui_act(&g, ACT_COPY);
    strlcpy(want, "line 0005 and the quick brown fox\nline 0006 and the quick brown fox\n"
                  "line 0007 and the quick brown fox\n", sizeof(want));
    CHECK(g.clip_len == strlen(want) && !memcmp(g.clip, want, g.clip_len));
    CHECK(!strcmp(v->status, "Copied 3 lines (102 bytes)"));
    draw();
    CHECK(fake_pixel(W, w->x + 300, ty + 5 * 16 + 1) == PICK);    /* the selected rows */
    CHECK(fake_pixel(W, w->x + 300, ty + 8 * 16 + 1) == 0xFFFDF5);
    /* dragging below the view scrolls it */
    first_rows(&first, &rows, &total);
    button(w->x + 60, ty + 3, true);
    button(w->x + 60, ty + rows * 16 + 30, true);
    button(w->x + 60, ty + rows * 16 + 30, true);
    button(w->x + 60, ty + rows * 16 + 30, false);
    first_rows(&first, &rows, &total);
    CHECK(first == 2 && !v->follow);
    gui_act(&g, ACT_COPY);
    CHECK(!memcmp(g.clip, "line 0001", 9) && !memcmp(g.clip + g.clip_len - 37, "line 0020 marker", 16));   /* 1 to 20 */
    /* Select all (Ctrl+A), and a click below the text selects nothing */
    key(1);
    key(3);
    CHECK(g.clip_len == stream_len && !memcmp(g.clip, stream, stream_len));
    log_desktop();
    lines(1, 3);
    gui_open(&g, GUI_LOG);
    click(w->x + 60, ty + 10 * 16);
    CHECK(!v->any && v->follow);
    click(w->x + 60, ty + 1 * 16 + 2);
    CHECK(v->any && !v->follow);
    /* Help and the toolbar's Copy */
    click(w->x + 8 + 62 + 4 + 46 + 4 + 10, w->y + TITLE_H + 4 + 10);
    CHECK(g.clip_len == 34 && !memcmp(g.clip, "line 0002", 9));
}

/* long lines wrap, their later rows two cells in; UTF-8, tabs and the console's colour sequences */
static void log_drawing(void) {
    const struct gui_window *w;
    int first, rows, total, ty, i;
    char b[400];
    log_desktop();
    for(i = 0; i < 100; i++) b[i] = (char)('a' + i % 26);
    b[100] = 0;
    say("short\n");
    say(b);
    say("\n\xE2\x89\x8B SurfOS\tx \x1b[1;31mred\x1b[0m\r\n");
    say("no newline yet");
    gui_open(&g, GUI_LOG);
    w = &g.win[GUI_LOG];
    ty = w->y + TITLE_H + BAR_H + 4;
    first_rows(&first, &rows, &total);
    CHECK(total == 1 + 2 + 1 + 1);                      /* 100 cells in 71 columns: two rows */
    draw();
    CHECK(shows(w->x + 8, ty, "short", INK, 1));
    CHECK(shows(w->x + 8, ty + 16, "abcdefghij", INK, 1));
    CHECK(shows(w->x + 8 + 2 * 8, ty + 32, "tuvwxyz", INK, 1));   /* 71 letters, then the rest two cells in */
    CHECK(glyph_at(w->x + 8, ty + 48, 0xF7, INK));      /* ≋, as the console draws it */
    CHECK(shows(w->x + 8 + 2 * 8, ty + 48, "SurfOS", INK, 1));
    CHECK(shows(w->x + 8 + 16 * 8, ty + 48, "x red", INK, 1));   /* the tab to column 16; no colour codes */
    CHECK(shows(w->x + 8, ty + 64, "no newline yet", INK, 1));
    say(" - now it has one\n");
    tick_ms(500);
    first_rows(&first, &rows, &total);
    CHECK(total == 5 && row_is(4, "no newline yet - now"));
    /* narrower: more rows; the window maximized: fewer */
    gui_act(&g, ACT_MAXIMIZE);
    first_rows(&first, &rows, &total);
    CHECK(total == 4);
    gui_act(&g, ACT_MAXIMIZE);
    /* no system: the window says so */
    desktop(640, 400);
    g.sys = NULL;
    gui_open(&g, GUI_LOG);
    w = &g.win[GUI_LOG];
    draw();
    CHECK(shows(w->x + 8, w->y + TITLE_H + BAR_H + 4, "No kernel log", MUTED, 1) && fake_guards(W, H));
    tick_ms(1000);
    key(GUI_END);
    key(GUI_HOME);
    gui_act(&g, ACT_SELECT_ALL);
    gui_act(&g, ACT_COPY);
    click(w->x + 50, w->y + 100);
    draw();
    CHECK(fake_guards(W, H) && !g.logview.any);
}

/**** what the review found (10/18/26) ****/

/* klog_copy(): storms of every size before each chunk, buffers that are no multiple of 1024, a storm that leaves
   nothing to copy (copied again from the new head), and the head passing 2^32 */
static void klog_storms(void) {
    static char buf[KLOG_SIZE + 64], text[1100], big[20001];
    static const u32 sizes[] = { 1, 7, 1000, 1023, 1025, 5000, 16383, 16384 };
    static const u32 storms[] = { 100, 9000, 15000, 16000, 20000 };
    u_long n, w;
    u32 i, j, k, before, fill;
    int ok = 1, cut = 0, empty = 0, line = 0;
    bool fired;
    for(i = 0; i < 8; i++) for(j = 0; j < 5; j++) for(k = 2; k <= 6; k++) {
        for(fill = 0; fill < 2; fill++, line++) {      /* lines of the log meanwhile */
            snprintf(text, sizeof(text), "a line of the kernel log, number %d\n", line);
            klog_say(text);
        }
        memset(big, 'A' + (i + j + k) % 26, storms[j]);
        big[storms[j]] = 0;
        before = kwritten_len;
        saves = 0;
        storm_at = (int)k;
        storm = big;
        n = klog_copy(buf, sizes[i], &w);
        fired = !storm;
        storm = NULL;
        if(fired) {                                     /* (what the "interrupt" wrote, in the reference too) */
            memcpy(kwritten + kwritten_len, big, storms[j]);
            kwritten_len += storms[j];
        }
        if(!(w == before || (fired && w == before + storms[j])) || !n || n > sizes[i] || n > KLOG_SIZE ||
           memcmp(buf, kwritten + w - n, n)) ok = 0;
        if(n < sizes[i] && n < KLOG_SIZE) cut++;
        if(!n) empty++;
    }
    CHECK(ok && !empty && cut > 0);
    /* 16000 bytes before the first chunk: nothing of the end it read is left, so it reads the head again */
    memset(big, 'R', 16000);
    big[16000] = 0;
    before = kwritten_len;
    saves = 0;
    storm_at = 2;
    storm = big;
    n = klog_copy(buf, sizeof(buf), &w);
    CHECK(!storm && n == KLOG_SIZE && w == before + 16000);
    memcpy(kwritten + kwritten_len, big, 16000);
    kwritten_len += 16000;
    CHECK(!memcmp(buf, kwritten + kwritten_len - KLOG_SIZE, KLOG_SIZE));
    /* the head passing 2^32: the ring is still full (the same place in the ring, 32 KB before 2^32) */
    klog_head = (0u - 2 * KLOG_SIZE) + klog_head % KLOG_SIZE;
    for(i = 0; i < 40; i++) {
        memset(text, 'a' + i % 26, 1023);
        text[1023] = '\n';
        text[1024] = 0;
        klog_say(text);
    }
    n = klog_copy(buf, sizeof(buf), &w);
    CHECK(klog_head < 2 * KLOG_SIZE && n == KLOG_SIZE && w == klog_head && klog_length() == KLOG_SIZE);
    CHECK(!memcmp(buf, kwritten + kwritten_len - KLOG_SIZE, KLOG_SIZE));
    n = klog_copy(buf, 5000, &w);
    CHECK(n == 5000 && !memcmp(buf, kwritten + kwritten_len - 5000, 5000));
}

/* the Log: what the review found */
static void log_review(void) {
    const struct gui_window *w;
    struct gui_logview *v = &g.logview;
    int first, rows, total, ty, i, old;
    char want[64];
    u32 at;
    /* a click in the strip below the last whole row shown selects nothing */
    log_desktop();
    lines(1, 100);
    gui_open(&g, GUI_LOG);
    w = &g.win[GUI_LOG];
    ty = w->y + TITLE_H + BAR_H + 4;
    key(GUI_HOME);
    first_rows(&first, &rows, &total);
    CHECK(ty + rows * 16 + 3 < w->y + w->h - STATUS_H - 1);
    click(w->x + 50, ty + rows * 16 + 3);
    key(3);
    CHECK(!v->any && g.clip_len == 0);
    /* Select all stops Follow: the view stays where it is as lines come */
    key(GUI_END);
    CHECK(v->follow);
    key(1);
    CHECK(v->any && !v->follow);
    lines(101, 105);
    tick_ms(500);
    first_rows(&first, &rows, &total);
    CHECK(first == total - rows - 5);
    /* at the end with Follow off, scrolling down (the view does not move) starts it again */
    key(GUI_END);
    click(w->x + 8 + 10, w->y + TITLE_H + 4 + 10);      /* the toolbar's Follow: off, the view stays at the end */
    CHECK(!v->follow);
    key(GUI_DOWN);
    CHECK(v->follow);
    click(w->x + 8 + 10, w->y + TITLE_H + 4 + 10);
    CHECK(!v->follow);
    wheel_at(w->x + 100, w->y + 100, -1);
    CHECK(v->follow);
    click(w->x + 8 + 10, w->y + TITLE_H + 4 + 10);
    key(GUI_PGDN);
    CHECK(v->follow);
    /* dragging past the top edge: a row a movement, and on while it is held there still (the tick), then no more */
    log_desktop();
    lines(1, 100);
    gui_open(&g, GUI_LOG);
    for(i = 0; i < 10; i++) key(GUI_UP);
    first_rows(&old, &rows, &total);
    CHECK(old == total - rows - 10);
    button(w->x + 60, ty + 5 * 16 + 3, true);           /* line old + 6 */
    button(w->x + 60, ty - 20, true);                   /* above the text: a row */
    first_rows(&first, &rows, &total);
    CHECK(first == old - 1);
    for(i = 0; i < 10; i++) tick_ms(50);                /* held still there: a row every 50 ms */
    first_rows(&first, &rows, &total);
    CHECK(first == old - 11);
    button(w->x + 60, ty - 20, false);
    tick_ms(500);
    first_rows(&first, &rows, &total);
    CHECK(first == old - 11);
    key(3);
    snprintf(want, sizeof(want), "line %04d", old - 11 + 1);
    CHECK(!memcmp(g.clip, want, 9) && g.clip_len == 16 * 34 + 37);   /* lines 62 to 78, line 70 a marker */
    /* a selection whose first lines drop off keeps the lines it still has whole */
    log_desktop();
    lines(1, 480);                                      /* 80 bytes too many: the copy starts inside line 3 */
    gui_open(&g, GUI_LOG);
    key(GUI_HOME);
    button(w->x + 60, ty + 2 * 16 + 3, true);           /* lines 5 to 13 */
    button(w->x + 60, ty + 10 * 16 + 3, true);
    button(w->x + 60, ty + 10 * 16 + 3, false);
    key(3);
    CHECK(!memcmp(g.clip, "line 0005", 9) && g.clip_len == 8 * 34 + 37);
    lines(481, 486);                                    /* 204 bytes more: lines 1 to 8 go, line 9 is cut */
    tick_ms(500);
    key(3);
    CHECK(v->any && !memcmp(g.clip, "line 0010 marker", 16) && g.clip_len == 37 + 3 * 34);
    lines(487, 520);                                    /* and when none of them is left whole, it goes */
    tick_ms(500);
    CHECK(!v->any);
    /* the mark on a wrapped row; Find next with the bar closed */
    log_desktop();
    say("first\n");
    for(i = 0; i < 100; i++) say("x");
    say("NEEDLE");
    for(i = 0; i < 24; i++) say("y");
    say("\nneedle two\n");
    gui_open(&g, GUI_LOG);
    gui_act(&g, ACT_FIND);
    fake_type(&g, "needle");
    key('\r');
    draw();
    /* column 100 of the second line: 71 cells in its first row, then 29 in its second, two cells in */
    CHECK(fake_pixel(W, w->x + 8 + (2 + 29) * 8, ty + 2 * 16) == 0xE8D08A && glyph_at(w->x + 8 + (2 + 29) * 8, ty + 2 * 16, 'N', INK));
    key(27);
    CHECK(!v->finding && gui_menu_on(&g, 3, ACT_FIND_NEXT));
    gui_act(&g, ACT_FIND_NEXT);
    at = v->match - (v->written - v->len);
    CHECK(v->match_len == 6 && !memcmp(v->text + at, "needle two", 10));
    gui_act(&g, ACT_FIND_NEXT);
    CHECK(!strcmp(v->status, "Found it again from the top") && !memcmp(v->text + (v->match - (v->written - v->len)), "NEEDLE", 6));
    /* what is drawn: a copy that starts inside a UTF-8 sequence, escapes cut short, ESC alone, control bytes */
    log_desktop();
    say("\x89\x8B start\n");                             /* bytes that are not UTF-8: the 437 characters they are */
    say("A\x1b[31\n");                                 /* an escape cut short at the line end: nothing after A */
    say("B\x1b[\n");
    say("C\x1b\n");
    say("D\x1bXY\n");                                  /* ESC, not ESC [: no cell, then XY */
    say("E\x1b[1;31mred\x1b[0m!\n");
    say("G\xE2\x89\n");                                /* a sequence cut short: two 437 characters */
    say("J\x01K\x7fL\n");
    gui_open(&g, GUI_LOG);
    draw();
    CHECK(glyph_at(w->x + 8, ty, 0x89, INK) && glyph_at(w->x + 16, ty, 0x8B, INK) && shows(w->x + 8 + 3 * 8, ty, "start", INK, 1));
    CHECK(shows(w->x + 8, ty + 16, "A", INK, 1) && !glyph_at(w->x + 16, ty + 16, '3', INK) && fake_pixel(W, w->x + 16 + 3, ty + 16 + 8) == 0xFFFDF5);
    CHECK(shows(w->x + 8, ty + 32, "B", INK, 1) && shows(w->x + 8, ty + 48, "C", INK, 1) && shows(w->x + 8, ty + 64, "DXY", INK, 1));
    CHECK(shows(w->x + 8, ty + 80, "Ered!", INK, 1) && shows(w->x + 8, ty + 96, "G\xE2\x89", INK, 1) && shows(w->x + 8, ty + 112, "JKL", INK, 1));
    click(w->x + 50, ty + 3);                           /* the cut first line copies as it is */
    key(3);
    CHECK(g.clip_len == 9 && !memcmp(g.clip, "\x89\x8B start\n", 9));
    /* `written` passing 2^32: places are counted modulo 2^32, the selection and the match stay on their text */
    log_desktop();
    written_offset = 0xFFFFFFFFu - 20000;
    lines(1, 400);
    gui_open(&g, GUI_LOG);
    key(GUI_END);
    first_rows(&first, &rows, &total);
    click(w->x + 50, ty + (rows - 3) * 16 + 4);         /* line 398 */
    gui_act(&g, ACT_FIND);
    fake_type(&g, "marker");
    key('\r');                                          /* from it: line 400's */
    key(27);
    key(3);
    CHECK(!memcmp(g.clip, "line 0400 marker", 16));
    memcpy(want, g.clip, 37);
    first_rows(&old, &rows, &total);
    lines(401, 700);                                    /* `written` passes 2^32 */
    tick_ms(500);
    CHECK(v->written == stream_len + written_offset && v->written < 30000 && v->len == GUI_LOG_MAX);
    key(3);
    CHECK(v->any && !memcmp(g.clip, want, 37));
    CHECK(v->match_len == 6 && !memcmp(v->text + (v->match - (v->written - v->len)), "marker", 6));
    first_rows(&first, &rows, &total);
    CHECK(!v->follow && row_is(0, "line 0") && first > 0 && first < total - rows);
    lines(701, 705);                                    /* and what comes after it is new: read, and drawn */
    g.dirty = false;
    tick_ms(500);
    CHECK(g.dirty && !memcmp(v->text + v->len - 34, "line 0705", 9));
    key(GUI_END);
    CHECK(row_is(rows - 1, "line 0705"));
}

/* every window at 640x400: what it shows above its status bar, nothing under it */
static void small_screen(void) {
    const struct gui_window *w;
    int sy, ty, first, rows, total;
    /* the Log, 326 pixels high: its last row above the status bar, and with the find bar its buttons */
    log_desktop();
    desktop(640, 400);
    g.sys = &sys;
    lines(1, 100);
    gui_open(&g, GUI_LOG);
    w = &g.win[GUI_LOG];
    sy = w->y + w->h - STATUS_H - 1;
    ty = w->y + TITLE_H + BAR_H + 4;
    draw();
    first_rows(&first, &rows, &total);
    CHECK(rows == 15 && ty + rows * 16 <= sy && shows(w->x + 8, ty + (rows - 1) * 16, "line 0100 marker", INK, 1));
    gui_act(&g, ACT_FIND);
    draw();
    first_rows(&first, &rows, &total);
    CHECK(ty + rows * 16 <= sy - 28 && shows(w->x + 8, ty + (rows - 1) * 16, "line 0100 marker", INK, 1));
    CHECK(shows(w->x + w->w - 100 + 14, sy - 28 + 3 + 3, "Next", PAPER, 1) && shows(w->x + w->w - 34 + 8, sy - 28 + 3 + 3, "x", INK, 1));
    CHECK(fake_guards(W, H));
}

/**** what the second review found (10/18/26) ****/

/* klog_length() and klog_dump(): the head and the ring's fullness read together, a log never longer than the ring;
   from empty, across the first fill (with something logged inside the call), and past 2^32 */
static void klog_lengths(void) {
    static char text[1100], big[1001];
    u32 i;
    klog_head = 0;                                      /* a fresh log */
    klog_full = false;
    kwritten_len = 0;
    dumped_len = 0;
    klog_dump();
    CHECK(klog_length() == 0 && dumped_len == 0);
    for(i = 0; i < 16; i++) {                           /* 16000 bytes: not full yet */
        memset(text, 'a' + i % 26, 999);
        text[999] = '\n';
        text[1000] = 0;
        klog_say(text);
    }
    dumped_len = 0;
    klog_dump();
    CHECK(klog_length() == 16000 && dumped_len == 16000 && !memcmp(dumped, kwritten, 16000));
    memset(big, 'S', 1000);                             /* 1000 bytes logged inside the call fill the ring */
    big[1000] = 0;
    saves = 0;
    storm_at = 1;
    storm = big;
    CHECK(klog_length() == KLOG_SIZE && !storm && klog_full);
    memcpy(kwritten + kwritten_len, big, 1000);
    kwritten_len += 1000;
    dumped_len = 0;
    klog_dump();
    CHECK(dumped_len == KLOG_SIZE && !memcmp(dumped, kwritten + kwritten_len - KLOG_SIZE, KLOG_SIZE));
    saves = 0;                                          /* and inside the dump */
    storm_at = 1;
    storm = big;
    dumped_len = 0;
    klog_dump();
    memcpy(kwritten + kwritten_len, big, 1000);
    kwritten_len += 1000;
    CHECK(!storm && dumped_len == KLOG_SIZE && !memcmp(dumped, kwritten + kwritten_len - KLOG_SIZE, KLOG_SIZE));
    klog_head = (0u - KLOG_SIZE) + klog_head % KLOG_SIZE;   /* past 2^32 (the same place in the ring) */
    for(i = 0; i < 450; i++) klog_say("a line written as the head passes 2^32\n");   /* 17550 bytes: past it */
    dumped_len = 0;
    klog_dump();
    CHECK(klog_head < KLOG_SIZE && klog_length() == KLOG_SIZE && dumped_len == KLOG_SIZE &&
          !memcmp(dumped, kwritten + kwritten_len - KLOG_SIZE, KLOG_SIZE));
}

/* the Log: what the second review found */
static void log_round3(void) {
    const struct gui_window *w;
    struct gui_logview *v = &g.logview;
    int first, rows, total, ty, i, redraws, before, lines_n;
    u32 d;
    char b[64];
    /* a drag the desktop ended without the Log's release (shell/gui.c, the screen back from another console): the tick
       scrolls no more, and does not redraw */
    log_desktop();
    lines(1, 200);
    gui_open(&g, GUI_LOG);
    w = &g.win[GUI_LOG];
    ty = w->y + TITLE_H + BAR_H + 4;
    for(i = 0; i < 20; i++) key(GUI_UP);
    first_rows(&before, &rows, &total);
    button(w->x + 60, ty + 5 * 16 + 3, true);
    button(w->x + 60, ty - 20, true);                   /* past the top edge: a row */
    g.drag = g.resize = g.scroll = g.app_drag = -1;
    for(redraws = 0, i = 0; i < 40; i++) { g.dirty = false; tick_ms(50); if(g.dirty) redraws++; }
    first_rows(&first, &rows, &total);
    CHECK(!v->drag && first == before - 1 && redraws == 0);
    button(w->x + 60, ty - 20, false);
    /* tucked away from the keyboard with the button held past the edge: the same */
    button(w->x + 60, ty + 5 * 16 + 3, true);
    button(w->x + 60, ty - 20, true);
    first_rows(&before, &rows, &total);
    gui_act(&g, ACT_MINIMIZE);
    CHECK(g.win[GUI_LOG].min);
    for(redraws = 0, i = 0; i < 40; i++) { g.dirty = false; tick_ms(50); if(g.dirty) redraws++; }
    gui_open(&g, GUI_LOG);
    first_rows(&first, &rows, &total);
    CHECK(!v->drag && first == before && redraws == 0);
    button(w->x + 60, ty - 20, false);
    /* held past the edge with nothing left to scroll: no redraws either */
    key(GUI_HOME);
    button(w->x + 60, ty + 3 * 16 + 3, true);
    button(w->x + 60, ty - 20, true);
    for(redraws = 0, i = 0; i < 40; i++) { g.dirty = false; tick_ms(50); if(g.dirty) redraws++; }
    first_rows(&first, &rows, &total);
    CHECK(v->drag && first == 0 && redraws == 0);
    button(w->x + 60, ty - 20, false);
    CHECK(!v->drag);
    /* a wrap exactly at a line's start: the selection keeps that whole line */
    log_desktop();
    lines(1, 500);                                      /* the copy starts inside line 23 */
    gui_open(&g, GUI_LOG);
    key(GUI_HOME);
    for(d = 0; d < v->len && v->text[d] != '\n'; d++) ;
    d++;                                                /* the partial first line's bytes: 22 */
    button(w->x + 60, ty + 3, true);                    /* rows 0 to 4: the partial line, lines 24 to 27 */
    button(w->x + 60, ty + 4 * 16 + 3, true);
    button(w->x + 60, ty + 4 * 16 + 3, false);
    memset(b, '-', d - 1);
    b[d - 1] = '\n';
    b[d] = 0;
    say(b);                                             /* exactly that many more: the copy starts at line 24's start */
    tick_ms(500);
    CHECK(d == 22 && !memcmp(v->text, "line 0024", 9));
    key(3);
    CHECK(!memcmp(g.clip, "line 0024", 9) && g.clip_len == 4 * 34);
    lines(501, 501);                                    /* 34 more: line 24 goes, 25 to 27 stay */
    tick_ms(500);
    key(3);
    CHECK(v->any && !memcmp(g.clip, "line 0025", 9) && g.clip_len == 3 * 34);
    /* the status bar's note once `written` has passed 2^32 */
    log_desktop();
    written_offset = 0xFFFFFFFFu - 20000;
    lines(1, 700);                                      /* written is about 4000 past 2^32, the copy full */
    gui_open(&g, GUI_LOG);
    draw();
    for(lines_n = 0, i = 0; i < (int)v->len; i++) if(v->text[i] == '\n') lines_n++;
    snprintf(b, sizeof(b), "%d lines, the newest 16 KB", lines_n);
    CHECK(v->written < v->len && shows(w->x + 10, w->y + w->h - STATUS_H - 1 + 3, b, INK, 1));
}

/**** every window, inside the screen ****/

/* each window opens, ticks, draws inside the screen (also maximized and hanging off the edge) and closes; the Log with
   its find bar, a selection and long lines */
static void windows(int width, int height) {
    int i, k;
    desktop(width, height);
    stream_len = 0;
    sys = fake_sys;
    sys.log = ring_log;
    g.sys = &sys;
    for(i = 0; i < 400; i++) say(i % 7 ? "a line of the kernel log, long enough to wrap at the edge of a narrow window"
                                         " of the desktop, with \xE2\x89\x8B\n" : "short\n");
    for(i = 0; i < IDS; i++) {
        gui_open(&g, ids[i]);
        CHECK(g.win[ids[i]].open && g.focus == ids[i]);
        CHECK(g.win[ids[i]].w <= width && g.win[ids[i]].y + g.win[ids[i]].h <= height - TASK_H);
        for(k = 0; k < 3; k++) { g.now += 1000; gui_tick(&g); }
        if(ids[i] == GUI_LOG) { key(1); gui_act(&g, ACT_FIND); fake_type(&g, "wrap"); key('\r'); }
        draw();
        CHECK(fake_guards(width, height));
        gui_act(&g, ACT_MAXIMIZE);
        draw();
        CHECK(fake_guards(width, height));
        g.win[ids[i]].x = width - 10;
        g.win[ids[i]].y = height - 10;
        draw();
        CHECK(fake_guards(width, height));
        gui_act(&g, ACT_MAXIMIZE);
        gui_place(&g, ids[i]);
        CHECK(g.win[ids[i]].x >= 0 && g.win[ids[i]].x + g.win[ids[i]].w <= width);
    }
    gui_open(&g, GUI_HELP);                             /* Help, with the suite's lines */
    gui_key(&g, GUI_END);
    draw();
    CHECK(fake_guards(width, height));
    gui_finish(&g);
    for(i = 0; i < IDS; i++) {
        gui_open(&g, ids[i]);
        gui_act(&g, ACT_CLOSE);
        CHECK(!g.win[ids[i]].open);
    }
}

/* each application's Help lines fit Help's window */
static void help(void) {
    const struct gui_app *apps[] = { &gui_app_log, &gui_app_clock, &gui_app_charmap };
    int i, k, ok = 1;
    for(i = 0; i < 3; i++) {
        CHECK(apps[i]->help_lines > 1 && apps[i]->help[0][0] == '#');
        for(k = 0; k < apps[i]->help_lines; k++) if(gd_cells(apps[i]->help[k]) > 58) ok = 0;
    }
    CHECK(ok);
}

int main(void) {
    klog_copying();
    log_reading();
    log_wrap();
    log_selection();
    log_drawing();
    help();
    klog_storms();
    log_review();
    small_screen();
    klog_lengths();
    log_round3();
    windows(640, 400);
    windows(960, 540);
    windows(1280, 800);
    return fake_done("test_guitools");
}
