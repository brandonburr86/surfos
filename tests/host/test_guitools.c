/* Host tests for Log, Clock and Character Map (track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md), against the
   in-memory file system and system of tests/host/gui_fake.c. Built and run by `make unittest` (unittest-gui).
   The Log reads a kernel log of its own here: a stream of everything "written", of which it gets the last 16 KB, as
   klog_copy() gives them, so that the log can wrap around. And kernel/klog.c itself, klog_copy() with an interrupt
   that logs while it copies. */
#include "gui_fake.h"
#include "../../shell/gui_tools.h"
#include <sys/cp437.h>
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
static const int ids[] = { GUI_LOG, GUI_CLOCK, GUI_CHARMAP };
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

static void title(int id, char *out, size_t n) {
    const struct gui_app *a = gui_app_of(id);
    if(a->title_of) a->title_of(&g, out, n);
    else strlcpy(out, a->title, n);
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

/**** Clock ****/

static void calendar_arithmetic(void) {
    static const struct { u32 y, m, d; int weekday; } dates[] = {
        { 1970, 1, 1, 4 }, { 2000, 1, 1, 6 }, { 2026, 10, 17, 6 }, { 2024, 2, 29, 4 }, { 2100, 3, 1, 1 },
        { 1900, 1, 1, 1 }, { 2026, 10, 1, 4 }, { 2027, 1, 1, 5 }, { 2000, 2, 29, 2 }, { 1, 1, 1, 1 },
        { 9999, 12, 31, 5 }, { 1582, 10, 15, 5 }, { 2038, 1, 19, 2 },
    };
    /* in 2026 the months begin on every day of the week */
    static const u32 firsts[7] = { 2, 6, 9, 4, 1, 5, 8 };      /* Feb Sunday, Jun Monday, ..., Aug Saturday */
    int i;
    CHECK(clock_leap(2000) && clock_leap(2024) && clock_leap(2400) && clock_leap(1600));
    CHECK(!clock_leap(1900) && !clock_leap(2026) && !clock_leap(2100) && !clock_leap(2023));
    CHECK(clock_days(2024, 2) == 29 && clock_days(2026, 2) == 28 && clock_days(1900, 2) == 28 && clock_days(2000, 2) == 29);
    CHECK(clock_days(2026, 4) == 30 && clock_days(2026, 12) == 31 && clock_days(2026, 1) == 31 && clock_days(2026, 0) == 0);
    for(i = 0; i < (int)(sizeof(dates) / sizeof(dates[0])); i++)
        CHECK(clock_weekday(dates[i].y, dates[i].m, dates[i].d) == dates[i].weekday);
    for(i = 0; i < 7; i++) CHECK(clock_weekday(2026, firsts[i], 1) == i && clock_cell(2026, firsts[i], 1) == i);
    CHECK(clock_yday(2026, 10, 17) == 290 && clock_yday(2024, 12, 31) == 366 && clock_yday(2026, 12, 31) == 365);
    CHECK(clock_yday(2026, 3, 1) == 60 && clock_yday(2024, 3, 1) == 61 && clock_yday(2026, 1, 1) == 1);
    CHECK(clock_cell(2026, 10, 17) == 20 && clock_cell(2026, 2, 28) == 27 && clock_cell(2026, 8, 31) == 36);
    /* a month that needs six weeks, and one that fits in four */
    CHECK(clock_cell(2026, 8, 31) / 7 == 5 && clock_cell(2026, 2, 28) / 7 == 3);
    /* day by day from 1970: the weekdays follow on, the days of the year add up */
    {
        u32 y, m, d;
        int wd = 4, ok = 1, yd;
        for(y = 1970; y < 2101; y++) {
            yd = 0;
            for(m = 1; m <= 12; m++) for(d = 1; d <= (u32)clock_days(y, m); d++) {
                if(clock_weekday(y, m, d) != wd || clock_yday(y, m, d) != ++yd) ok = 0;
                wd = (wd + 1) % 7;
            }
            if(yd != (clock_leap(y) ? 366 : 365)) ok = 0;
        }
        CHECK(ok);
    }
}

/* the calendar's cell of a day, on the screen */
static void day_cell(u32 year, u32 month, u32 day, int *x, int *y) {
    const struct gui_window *w = &g.win[GUI_CLOCK];
    int cell = clock_cell(year, month, day);
    *x = w->x + (w->w - 7 * 48) / 2 + cell % 7 * 48;
    *y = w->y + TITLE_H + BAR_H + 4 + 62 + cell / 7 * 24;
}

static void calendar(void) {
    const struct gui_window *w;
    struct gui_clock *c = &g.clockapp;
    int x, y;
    desktop(960, 540);
    gui_open(&g, GUI_CLOCK);
    w = &g.win[GUI_CLOCK];
    /* the clock's page: the time large and the date, the weekday worked out (not the CMOS clock's) */
    g.time.weekday = 3;
    draw();
    CHECK(shows(w->x + (w->w - 8 * 32) / 2, w->y + TITLE_H + BAR_H + 4 + 10, "14:05:30", INK, 4));
    CHECK(shows(w->x + (w->w - 8 * 16) / 2, w->y + TITLE_H + BAR_H + 4 + 88, "Saturday", TEAL, 2));
    CHECK(shows(w->x + (w->w - 16 * 16) / 2, w->y + TITLE_H + BAR_H + 4 + 124, "October 17, 2026", INK, 2));
    CHECK(shows(w->x + (w->w - 14 * 8) / 2, w->y + TITLE_H + BAR_H + 4 + 168, "Day 290 of 365", MUTED, 1));
    key('2');
    CHECK(c->page == GUI_CALENDAR_PAGE);
    draw();
    CHECK(shows(w->x + 70 + (w->w - 72 - 4 - 70 - 12 * 16) / 2, w->y + TITLE_H + BAR_H + 4 + 5, "October 2026", INK, 2));
    day_cell(2026, 10, 17, &x, &y);
    CHECK(fake_pixel(W, x + 8, y + 3) == TEAL && shows(x + 16, y + 4, "17", PAPER, 1));   /* today: marked */
    day_cell(2026, 10, 16, &x, &y);
    CHECK(fake_pixel(W, x + 8, y + 3) != TEAL && shows(x + 16, y + 4, "16", INK, 1));
    day_cell(2026, 10, 1, &x, &y);
    CHECK(shows(x + 20, y + 4, "1", INK, 1));            /* Thursday: the fifth column */
    CHECK(x == w->x + (w->w - 7 * 48) / 2 + 4 * 48);
    /* the next month and back, a year, Home */
    key(GUI_RIGHT);
    CHECK(c->year == 2026 && c->month == 11);
    draw();
    day_cell(2026, 11, 17, &x, &y);
    CHECK(fake_pixel(W, x + 8, y + 3) != TEAL);         /* not this month: no day marked */
    CHECK(shows(w->x + 70 + (w->w - 72 - 4 - 70 - 13 * 16) / 2, w->y + TITLE_H + BAR_H + 4 + 5, "November 2026", INK, 2));
    key(GUI_PGDN);
    key(GUI_PGDN);
    CHECK(c->year == 2027 && c->month == 1);
    key(GUI_LEFT);
    CHECK(c->year == 2026 && c->month == 12);
    key(GUI_UP);
    CHECK(c->year == 2025 && c->month == 12);
    key(GUI_DOWN);
    key(GUI_DOWN);
    CHECK(c->year == 2027 && c->month == 12);
    key(GUI_HOME);
    CHECK(c->year == 0);
    draw();
    day_cell(2026, 10, 17, &x, &y);
    CHECK(fake_pixel(W, x + 8, y + 3) == TEAL);
    /* the buttons: <, >, Today; a click on a day says which it is */
    click(w->x + 40 + 10, w->y + TITLE_H + BAR_H + 4 + 8 + 10);
    CHECK(c->month == 11);
    click(w->x + 10 + 10, w->y + TITLE_H + BAR_H + 4 + 8 + 10);
    click(w->x + 10 + 10, w->y + TITLE_H + BAR_H + 4 + 8 + 10);
    CHECK(c->month == 9 && c->year == 2026);
    click(w->x + w->w - 72 + 20, w->y + TITLE_H + BAR_H + 4 + 8 + 10);
    CHECK(c->year == 0);
    day_cell(2026, 10, 1, &x, &y);
    click(x + 20, y + 10);
    CHECK(!strcmp(c->status, "Thursday, October 1, 2026: day 274"));
    /* February 2024 (a leap year) and the limits */
    c->year = 2024; c->month = 2;
    draw();
    day_cell(2024, 2, 29, &x, &y);
    CHECK(shows(x + 16, y + 4, "29", INK, 1) && fake_guards(W, H));
    c->year = 9999; c->month = 12;
    key(GUI_RIGHT);
    CHECK(c->year == 9999 && c->month == 12);
    c->year = 1; c->month = 1;
    key(GUI_LEFT);
    key(GUI_UP);
    CHECK(c->year == 1 && c->month == 1);
    draw();
    CHECK(fake_guards(W, H));
    /* the date not known: the pages say so */
    memset(&g.time, 0, sizeof(g.time));
    key(GUI_HOME);
    key('1');
    draw();
    CHECK(shows(w->x + (w->w - 32 * 8) / 2, w->y + TITLE_H + BAR_H + 4 + 60, "The date and time are not known.", MUTED, 1));
    key('2');
    draw();
    CHECK(shows(w->x + 70 + (w->w - 72 - 4 - 70 - 12 * 16) / 2, w->y + TITLE_H + BAR_H + 4 + 5, "January 2000", INK, 2));
    /* Ctrl+C copies the month as cal prints it */
    g.time.year = 2026; g.time.month = 10; g.time.date = 17;
    key(3);
    CHECK(!g.quit && g.clip_len > 0);
    g.clip[g.clip_len] = 0;
    {
        const char *cal = "    October 2026\nSu Mo Tu We Th Fr Sa\n             1  2  3\n 4  5  6  7  8  9 10\n";
        CHECK(!strncmp(g.clip, cal, strlen(cal)) && !strcmp(g.clip + g.clip_len - 9, "\n25 26 27 28 29 30 31\n" + 13));
    }
}

static void stopwatch(void) {
    const struct gui_window *w;
    struct gui_clock *c = &g.clockapp;
    int y;
    desktop(960, 540);
    gui_open(&g, GUI_CLOCK);
    w = &g.win[GUI_CLOCK];
    y = w->y + TITLE_H + BAR_H + 4 + 8;
    key('3');
    draw();
    CHECK(shows(w->x + (w->w - 7 * 32) / 2, y, "00:00.0", INK, 4));
    g.now = 10000;
    key(' ');
    CHECK(c->watch_on);
    tick_ms(1234);
    CHECK(clock_watch(&g) == 1234);
    g.dirty = false;
    gui_tick(&g);
    CHECK(!g.dirty);                                    /* the same tenth: nothing to draw */
    tick_ms(70);
    CHECK(g.dirty);
    draw();
    CHECK(shows(w->x + (w->w - 7 * 32) / 2, y, "00:01.3", TEAL, 4));
    key('l');
    CHECK(c->laps == 1 && c->lap[0] == 1304);
    tick_ms(1000);
    key('L');
    CHECK(c->laps == 2 && c->lap[1] == 2304);
    draw();
    CHECK(shows(w->x + (w->w - 34 * 8) / 2, w->y + TITLE_H + BAR_H + 4 + 124, "Lap 2      00:02.3   +00:01.0", INK, 1));
    CHECK(shows(w->x + (w->w - 34 * 8) / 2, w->y + TITLE_H + BAR_H + 4 + 142, "Lap 1      00:01.3   +00:01.3", MUTED, 1));
    /* minimized, or another page: it keeps counting */
    gui_act(&g, ACT_MINIMIZE);
    CHECK(g.win[GUI_CLOCK].min);
    tick_ms(60000);
    CHECK(clock_watch(&g) == 62304);
    gui_open(&g, GUI_CLOCK);
    key('1');
    draw();
    CHECK(shows(w->x + 10, w->y + w->h - STATUS_H - 1 + 3, "Stopwatch: 01:02", INK, 1) &&
          !glyph_at(w->x + 10 + 16 * 8, w->y + w->h - STATUS_H - 1 + 3, '.', INK));
    gui_tick(&g);
    g.dirty = false;
    tick_ms(500);
    CHECK(!g.dirty);                                    /* its seconds, there */
    tick_ms(200);
    CHECK(g.dirty);
    key('3');
    key(' ');                                           /* stop */
    CHECK(!c->watch_on);
    tick_ms(5000);
    CHECK(clock_watch(&g) == 63004);
    /* an hour on, the hours show */
    key(' ');
    tick_ms(3600000);
    draw();
    CHECK(shows(w->x + (w->w - 9 * 32) / 2, y, "1:01:03.0", TEAL, 4));
    /* nine laps: the newest five shown, eight kept */
    for(c->laps = 0; c->laps < 9;) { tick_ms(100); key('l'); }
    CHECK(c->laps == 9 && c->lap[8 % GUI_LAPS] == clock_watch(&g));
    draw();
    CHECK(shows(w->x + (w->w - 34 * 8) / 2, w->y + TITLE_H + BAR_H + 4 + 124, "Lap 9 ", INK, 1));
    CHECK(fake_guards(W, H));
    /* busy while it runs: closing asks first, and Cancel keeps it */
    gui_close(&g, GUI_CLOCK);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && g.win[GUI_CLOCK].open && !strcmp(g.dialog.title, "Close the window?"));
    CHECK(!strncmp(g.dialog.text[0], "The stopwatch is running: 1:01:03.9", 35));
    key(27);
    CHECK(!g.dialog.kind && g.win[GUI_CLOCK].open && c->watch_on);
    key('r');
    CHECK(!c->watch_on && clock_watch(&g) == 0 && c->laps == 0);
    key(' ');
    key(23);                                            /* Ctrl+W: asks, Close ends it */
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM);
    key('\r');
    CHECK(!g.win[GUI_CLOCK].open && !c->watch_on && clock_watch(&g) == 0);
}

static bool taskbar_shows(const char *text) {
    int n, i;
    for(n = 1; n <= GUI_WINDOWS; n++) {                 /* wherever the button is: n buttons, the i-th */
        int bw = (W - 96) / n - 4;
        if(bw > 156) bw = 156;
        for(i = 0; i < n; i++)                          /* a window tucked away: its title muted */
            if(shows(6 + i * (bw + 4) + 24, H - TASK_H + 5, text, FOAM, 1) ||
               shows(6 + i * (bw + 4) + 24, H - TASK_H + 5, text, MUTED, 1)) return true;
    }
    return false;
}

static void timer(void) {
    const struct gui_window *w;
    struct gui_clock *c = &g.clockapp;
    char b[64];
    int y;
    desktop(960, 540);
    gui_open(&g, GUI_CLOCK);
    w = &g.win[GUI_CLOCK];
    y = w->y + TITLE_H + BAR_H + 4 + 8;
    key('4');
    draw();
    CHECK(c->timer_set == 300 && shows(w->x + (w->w - 5 * 32) / 2, y, "05:00", INK, 4));
    /* setting it: the keys and the buttons, within 0:00 and 99:59 */
    key(GUI_HOME);
    CHECK(c->timer_set == 0);
    key(GUI_DOWN);
    key(GUI_LEFT);
    CHECK(c->timer_set == 0);
    key(' ');
    CHECK(!c->timer_on && !strncmp(c->status, "Set a time first", 16));
    key(GUI_UP); key(GUI_PGUP); key(GUI_RIGHT); key(GUI_RIGHT);
    CHECK(c->timer_set == 72);
    key(GUI_PGDN);
    CHECK(c->timer_set == 62);
    click(w->x + (w->w - 4 * 76 - 24) / 2 + 84 + 10, w->y + TITLE_H + BAR_H + 4 + 100 + 10);    /* +1 min */
    click(w->x + (w->w - 4 * 76 - 24) / 2 + 3 * 84 + 10, w->y + TITLE_H + BAR_H + 4 + 100 + 10);   /* +10 s */
    CHECK(c->timer_set == 132);
    click(w->x + (w->w - 4 * 76 - 24) / 2 + 10, w->y + TITLE_H + BAR_H + 4 + 100 + 10);        /* -1 min */
    click(w->x + (w->w - 4 * 76 - 24) / 2 + 2 * 84 + 10, w->y + TITLE_H + BAR_H + 4 + 100 + 10);   /* -10 s */
    CHECK(c->timer_set == 62);
    for(y = 0; y < 120; y++) key(GUI_UP);
    CHECK(c->timer_set == GUI_TIMER_MAX);
    draw();
    CHECK(shows(w->x + (w->w - 5 * 32) / 2, w->y + TITLE_H + BAR_H + 4 + 8, "99:59", INK, 4));
    y = w->y + TITLE_H + BAR_H + 4 + 8;
    /* five seconds; then the window is tucked away and the alarm still comes */
    key(GUI_HOME);
    for(int i = 0; i < 5; i++) key(GUI_RIGHT);
    CHECK(c->timer_set == 5);
    key(' ');
    CHECK(c->timer_on);
    title(GUI_CLOCK, b, sizeof(b));
    CHECK(!strcmp(b, "Clock - 00:05"));
    key(GUI_UP);                                        /* not while it runs */
    CHECK(c->timer_set == 5 && !strncmp(c->status, "Reset the timer", 15));
    gui_act(&g, ACT_MINIMIZE);
    CHECK(g.win[GUI_CLOCK].min && g.focus != GUI_CLOCK);
    tick_ms(4000);
    title(GUI_CLOCK, b, sizeof(b));
    CHECK(!strcmp(b, "Clock - 00:01") && !c->alarm && !g.dialog.kind);
    tick_ms(999);
    CHECK(!c->alarm);
    tick_ms(1);
    CHECK(c->alarm && !c->timer_on && g.win[GUI_CLOCK].min);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && !strcmp(g.dialog.title, "Timer") && !strcmp(g.dialog.text[0], "Time's up!"));
    CHECK(!strcmp(g.dialog.text[1], "The 00:05 timer has finished."));
    title(GUI_CLOCK, b, sizeof(b));
    CHECK(!strcmp(b, "Time's up! - Clock"));
    draw();
    CHECK(taskbar_shows("Time's up!") && fake_guards(W, H));
    key('\r');                                          /* OK: the timer is ready again */
    CHECK(!g.dialog.kind && !c->alarm);
    title(GUI_CLOCK, b, sizeof(b));
    CHECK(!strcmp(b, "Clock") && clock_left(&g) == 5000);
    tick_ms(100);
    CHECK(!g.dialog.kind);
    /* pause and resume; and an alarm never takes the place of a question already asked */
    gui_open(&g, GUI_CLOCK);
    key(' ');
    tick_ms(2000);
    key(' ');
    CHECK(c->timer_paused && clock_left(&g) == 3000);
    draw();
    CHECK(shows(w->x + (w->w - 5 * 32) / 2, y, "00:03", INK, 4) &&
          shows(w->x + (w->w - 6 * 8) / 2, w->y + TITLE_H + BAR_H + 4 + 78, "Paused", MUTED, 1));
    tick_ms(10000);
    CHECK(clock_left(&g) == 3000 && !c->alarm);
    key(' ');
    CHECK(c->timer_on);
    dialog_message(&g, "Another", "A question asked first", "");
    tick_ms(3000);
    CHECK(c->alarm && !strcmp(g.dialog.title, "Another"));
    key('\r');
    CHECK(!g.dialog.kind);
    gui_tick(&g);
    CHECK(!strcmp(g.dialog.title, "Timer"));
    key(27);                                            /* Esc answers it too */
    CHECK(!c->alarm && !g.dialog.kind);
    /* Reset; busy while it runs: leaving the desktop asks */
    key(' ');
    tick_ms(1500);
    key('r');
    CHECK(!c->timer_on && clock_left(&g) == 5000);
    key(' ');
    gui_request_quit(&g);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && !strcmp(g.dialog.title, "Leave the desktop?") && !g.quit);
    CHECK(!strcmp(g.dialog.text[0], "The timer is running: 00:05 left."));
    key(27);
    /* the timer elsewhere: the status bar says so */
    key('1');
    draw();
    CHECK(shows(w->x + 10, w->y + w->h - STATUS_H - 1 + 3, "Timer: 00:05 left", INK, 1));
    /* closing ends it */
    gui_close(&g, GUI_CLOCK);
    key('\r');
    CHECK(!g.win[GUI_CLOCK].open && !c->timer_on);
    tick_ms(10000);
    CHECK(!c->alarm && !g.dialog.kind);
    gui_open(&g, GUI_CLOCK);
    CHECK(c->page == GUI_CLOCK_PAGE && c->timer_set == 5 && clock_left(&g) == 5000);
    /* Ctrl+C on the timer's page copies what it shows */
    key('4');
    key(3);
    CHECK(!g.quit && g.clip_len == 6 && !memcmp(g.clip, "00:05\n", 6));
}

/**** Character Map ****/

static void charmap_codes(void) {
    char u[CP437_UTF8_MAX], v[CP437_UTF8_MAX];
    u32 len;
    int c, n, all = 1;
    for(c = 1; c < 256; c++) {                          /* each pastes as itself: Notepad shows the same glyph */
        n = charmap_utf8(c, u);
        if(n != cp437_utf8((u8)c, v) || memcmp(u, v, n) || gd_char(u, n, &len) != c || len != (u32)n) all = 0;
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
}

static void charmap(void) {
    const struct gui_window *w, *nw;
    struct gui_charmap *m = &g.charmap;
    int x, y;
    desktop(960, 540);
    gui_open(&g, GUI_CHARMAP);
    w = &g.win[GUI_CHARMAP];
    draw();
    CHECK(m->selected == 0 && glyph_at(w->x + 26 + 7 * 17 + 5, w->y + 50 + 15 * 17 + 1, 0xF7, INK));   /* ≋ in its cell */
    CHECK(shows(w->x + 312, w->y + 148, "Code", MUTED, 1) && shows(w->x + 312 + 64, w->y + 148 + 36, "U+0000", INK, 1));
    CHECK(!gui_menu_on(&g, 3, ACT_COPY) && gui_menu_on(&g, 3, ACT_GOTO));
    /* the keys */
    key(GUI_RIGHT); key(GUI_RIGHT); key(GUI_DOWN);
    CHECK(m->selected == 18);
    key(GUI_END);
    CHECK(m->selected == 31);
    key(GUI_HOME);
    CHECK(m->selected == 16);
    key(GUI_PGDN);
    CHECK(m->selected == 0xF0);
    key(GUI_DOWN); key(GUI_PGUP);
    CHECK(m->selected == 0);
    key(GUI_LEFT); key(GUI_UP);
    CHECK(m->selected == 0);
    key('A');
    CHECK(m->selected == 'A');
    key(GUI_PGDN); key(GUI_END); key(GUI_RIGHT); key(GUI_DOWN);
    CHECK(m->selected == 255);
    /* a click chooses; the panel shows it large, with its code, code point and bytes */
    click(w->x + 26 + 7 * 17 + 8, w->y + 50 + 15 * 17 + 8);
    CHECK(m->selected == 0xF7 && !g.clip_len);
    draw();
    x = w->x + 312;
    y = w->y + 148;
    CHECK(shows(x + 64, y, "247", INK, 1) && shows(x + 64, y + 18, "0xF7", INK, 1));
    CHECK(shows(x + 64, y + 36, "U+224B", INK, 1) && shows(x + 64, y + 54, "E2 89 8B", INK, 1));
    CHECK(shows(x, y + 80, "One of SurfOS's", MUTED, 1));
    CHECK(glyph_at(w->x + 26 + 7 * 17 + 5, w->y + 50 + 15 * 17 + 1, 0xF7, PAPER));       /* chosen: lit */
    {
        const u8 *gl = font8x16[0xF7];                  /* large: one of its lit pixels, six times over */
        int row, col, ok = 0;
        for(row = 0; row < 16 && !ok; row++) for(col = 0; col < 8 && !ok; col++)
            if(gl[row] & (0x80 >> col))
                ok = fake_pixel(W, w->x + 312 + (138 - 66) / 2 + 9 + col * 6 + 3, w->y + 34 + 5 + row * 6 + 3) == INK;
        CHECK(ok);
    }
    /* Copy (Ctrl+C), then Notepad: Ctrl+V pastes ≋ as UTF-8, and Notepad shows its glyph */
    key(3);
    CHECK(!g.quit && g.clip_len == 3 && !memcmp(g.clip, "\xE2\x89\x8B", 3));
    CHECK(!strcmp(m->status, "Copied \xE2\x89\x8B (3 bytes of UTF-8): Ctrl+V pastes it"));
    draw();
    CHECK(glyph_at(w->x + 10 + 7 * 8, w->y + w->h - STATUS_H - 1 + 3, 0xF7, INK));      /* in the status bar */
    key(5);                                             /* Ctrl+E */
    CHECK(g.focus == GUI_NOTEPAD);
    key(22);                                            /* Ctrl+V */
    CHECK(g.note.len == 3 && !memcmp(g.note.text, "\xE2\x89\x8B", 3) && g.note.modified);
    draw();
    nw = &g.win[GUI_NOTEPAD];
    CHECK(glyph_at(nw->x + 44, nw->y + TITLE_H + BAR_H + 4, 0xF7, INK));
    CHECK(note_column(&g.note, g.note.cursor) == 1);   /* one character, one cell */
    gui_close(&g, GUI_NOTEPAD);                         /* closing without saving */
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && !strcmp(g.dialog.title, "Unsaved changes"));
    key(9); key('\r');                                  /* Don't save */
    CHECK(!g.win[GUI_NOTEPAD].open && !g.dialog.kind && g.focus == GUI_CHARMAP);
    /* Enter and a double-click copy too; the empty cell copies nothing */
    key(GUI_HOME); key(GUI_PGUP); key(GUI_RIGHT);
    key('\r');
    CHECK(g.clip_len == 3 && !memcmp(g.clip, "\xE2\x98\xBA", 3));                /* ☺ */
    click(w->x + 26 + 11 * 17 + 8, w->y + 50 + 13 * 17 + 8);
    click(w->x + 26 + 11 * 17 + 8, w->y + 50 + 13 * 17 + 8);
    CHECK(m->selected == 0xDB && g.clip_len == 3 && !memcmp(g.clip, "\xE2\x96\x88", 3));   /* █ */
    g.now += 1000;
    click(w->x + 26 + 5, w->y + 50 + 5);
    CHECK(m->selected == 0 && !gui_menu_on(&g, 3, ACT_COPY));
    key('\r');
    CHECK(g.clip_len == 3 && !strncmp(m->status, "0x00 is the console's empty cell", 32));
    key(' ');
    click(w->x + 312 + 10, w->y + w->h - STATUS_H - 1 - 34 + 10);   /* the Copy button, above the status bar */
    CHECK(g.clip_len == 1 && g.clip[0] == ' ' && !strcmp(m->status, "Copied a space: Ctrl+V pastes it"));
    /* Go to code (Ctrl+L): 0x41, 247, U+2588; what is not a code says so */
    key(12);
    CHECK(g.dialog.kind == GUI_DLG_INPUT && !strcmp(g.dialog.field.text, "0x20"));
    fake_type(&g, "0x41\r");
    CHECK(m->selected == 0x41);
    key(12); fake_type(&g, "247\r");
    CHECK(m->selected == 247);
    key(12); fake_type(&g, "U+2588\r");
    CHECK(m->selected == 0xDB);
    key(12); fake_type(&g, "u+00e9\r");
    CHECK(m->selected == 0x82);                         /* é */
    key(12); fake_type(&g, "300\r");
    CHECK(m->selected == 0x82 && !strcmp(m->status, "Not a code: 0-255, 0x00-0xFF, U+224B or a character"));
    key(12); fake_type(&g, "U+1234\r");
    CHECK(m->selected == 0x82 && !strcmp(m->status, "U+1234 is not one of the console's characters"));
    key(12); fake_type(&g, "Z\r");
    CHECK(m->selected == 'Z');
    key(12); key(27);
    CHECK(!g.dialog.kind && m->selected == 'Z');
    draw();
    CHECK(fake_guards(W, H));
    key(27);                                            /* Esc closes the window */
    CHECK(!g.win[GUI_CHARMAP].open);
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

/* the Clock: what the review found */
static void clock_review(void) {
    const struct gui_window *w;
    struct gui_clock *c = &g.clockapp;
    char t[64];
    int i, redraws;
    /* Copy with no date that can be used: the clipboard is left alone, and Copy is off */
    desktop(960, 540);
    memcpy(g.clip, "hello", 5);
    g.clip_len = 5;
    gui_open(&g, GUI_CLOCK);
    w = &g.win[GUI_CLOCK];
    g.time.month = 165;
    CHECK(!gui_menu_on(&g, 3, ACT_COPY));
    key(3);
    CHECK(!g.quit && g.clip_len == 5 && !memcmp(g.clip, "hello", 5));
    g.time.month = 0;
    key(3);
    CHECK(g.clip_len == 5 && !memcmp(g.clip, "hello", 5));
    g.time.month = 10;
    CHECK(gui_menu_on(&g, 3, ACT_COPY));
    key(3);
    g.clip[g.clip_len] = 0;
    CHECK(!strcmp(g.clip, "Saturday, October 17, 2026, 14:05:30\n") && !strcmp(c->status, "Copied"));
    /* the stopwatch's Copy: its time and the eight laps kept */
    key('3');
    key(' ');
    for(i = 0; i < 10; i++) { tick_ms(1000); key('l'); }
    key(' ');
    key(3);
    g.clip[g.clip_len] = 0;
    CHECK(!strncmp(g.clip, "00:10.0\nLap 3  00:03.0\n", 23) && !strcmp(g.clip + g.clip_len - 16, "Lap 10  00:10.0\n"));
    key('r');
    /* its tenths do not redraw the desktop while a window in front hides them: a maximized one, not a smaller one */
    key(' ');
    gui_open(&g, GUI_NOTEPAD);
    for(redraws = 0, i = 0; i < 100; i++) { g.dirty = false; tick_ms(10); if(g.dirty) redraws++; }
    CHECK(redraws >= 9);                                /* Notepad leaves some of the digits in sight */
    gui_act(&g, ACT_MAXIMIZE);
    tick_ms(10);
    for(redraws = 0, i = 0; i < 100; i++) { g.dirty = false; tick_ms(10); if(g.dirty) redraws++; }
    CHECK(redraws == 0);
    gui_act(&g, ACT_MAXIMIZE);
    gui_open(&g, GUI_CLOCK);
    for(redraws = 0, i = 0; i < 100; i++) { g.dirty = false; tick_ms(10); if(g.dirty) redraws++; }
    CHECK(redraws >= 9);
    key(' ');
    key('r');
    gui_close(&g, GUI_NOTEPAD);
    /* the alarm's message replaced by another dialog before it was answered: it comes back after that one */
    gui_open(&g, GUI_CLOCK);
    key('4');
    key(GUI_HOME);
    key(GUI_RIGHT);
    key(' ');
    tick_ms(1000);
    CHECK(c->alarm && !strcmp(g.dialog.title, "Timer"));
    dialog_message(&g, "Cannot show the picture", "x.bmp", "It could not be read to its end.");
    tick_ms(100);
    CHECK(!strcmp(g.dialog.title, "Cannot show the picture"));
    key('\r');
    tick_ms(100);
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && !strcmp(g.dialog.title, "Timer") && c->alarm);
    key('\r');
    CHECK(!g.dialog.kind && !c->alarm);
    /* closed with the alarm pending (its message waiting for another dialog): it goes with the window */
    key(' ');
    dialog_message(&g, "Another", "asked first", "");
    tick_ms(1000);
    CHECK(c->alarm && !strcmp(g.dialog.title, "Another"));
    gui_close(&g, GUI_CLOCK);
    CHECK(!g.win[GUI_CLOCK].open && !c->alarm);
    key('\r');
    tick_ms(100);
    CHECK(!g.dialog.kind);
    /* and with its message up when the desktop closes: answering it then changes nothing */
    gui_open(&g, GUI_CLOCK);
    key('4');
    key(' ');
    tick_ms(1000);
    CHECK(!strcmp(g.dialog.title, "Timer"));
    gui_finish(&g);
    CHECK(!c->alarm && !c->timer_on);
    key('\r');
    CHECK(!g.dialog.kind && !c->alarm);
    gui_close(&g, GUI_CLOCK);
    /* gui_state.now passing 2^32: the stopwatch and the timer count on */
    gui_open(&g, GUI_CLOCK);
    g.now = 0xFFFFFFFFu - 2000;
    key('3');
    key(' ');
    tick_ms(5000);
    CHECK(clock_watch(&g) == 5000);
    key(' ');
    key('r');
    key('4');
    key(GUI_HOME);
    key(GUI_PGUP);                                      /* ten seconds */
    g.now = 0xFFFFFFFFu - 3000;
    key(' ');
    tick_ms(2000);
    key(' ');                                           /* paused with 8 s left, across 2^32 */
    tick_ms(100000);
    key(' ');
    tick_ms(7000);
    CHECK(clock_left(&g) == 1000 && !c->alarm);
    gui_app_clock.title_of(&g, t, sizeof(t));
    CHECK(!strcmp(t, "Clock - 00:01"));
    tick_ms(1000);
    CHECK(c->alarm && !strcmp(g.dialog.title, "Timer"));
    key('\r');
    (void)w;
}

/* the Character Map: what the review found */
static void charmap_review(void) {
    const struct gui_window *w;
    struct gui_charmap *m = &g.charmap;
    int sy;
    desktop(960, 540);
    gui_open(&g, GUI_CHARMAP);
    w = &g.win[GUI_CHARMAP];
    sy = w->y + w->h - STATUS_H - 1;
    /* Go to code takes only what it says it takes */
    static const struct { const char *text; int code; const char *status; } cases[] = {
        { "4294967361", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },   /* 2^32 + 65 */
        { "U+100000041", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "U+41zz", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "65 66", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "0x", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "0x123", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "U+", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "1000", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { " ", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "AB", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "\xE2\x82\xAC", -2, "U+20AC is not one of the console's characters" },       /* € */
        { "\xF0\x9F\x98\x80", -2, "U+1F600 is not one of the console's characters" },  /* an emoji */
        { "U+FFFF", -2, "U+FFFF is not one of the console's characters" },
        { "007", 7, "" }, { " 0x41 ", 'A', "" }, { "u+2588", 0xDB, "" }, { "U+00e9", 0x82, "" },
        { "\xC3\xA9", 0x82, "" }, { "9", 9, "" }, { "255", 255, "" }, { "0xff", 255, "" }, { "~", '~', "" },
        { "U+0010FFFF", -1, "Not a code: 0-255, 0x00-0xFF, U+224B or a character" },
        { "U+10FFFF", -2, "U+10FFFF is not one of the console's characters" },
    };
    int i, ok = 1;
    for(i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        key('Z');
        key(12);
        CHECK(g.dialog.kind == GUI_DLG_INPUT);
        fake_type(&g, cases[i].text);
        key('\r');
        if(m->selected != (cases[i].code >= 0 ? cases[i].code : 'Z') || strcmp(m->status, cases[i].status)) {
            ok = 0;
            printf("Go to code \"%s\": 0x%02X, \"%s\"\n", cases[i].text, m->selected, m->status);
        }
    }
    CHECK(ok);
    key('Z');
    key(12);
    key(8);                                             /* empty */
    key('\r');
    CHECK(m->selected == 'Z' && !strcmp(m->status, "Not a code: 0-255, 0x00-0xFF, U+224B or a character"));
    draw();                                             /* the message whole in the status bar */
    CHECK(shows(w->x + 10, sy + 3, "Not a code: 0-255, 0x00-0xFF, U+224B or a character", INK, 1) &&
          !glyph_at(w->x + 10 + 51 * 8, sy + 3, 0xF9, INK));
    /* 0x10 is one of SurfOS's own characters, as 0xF7 and 0xF9 */
    key(GUI_HOME); key(GUI_PGUP); key(GUI_DOWN);
    CHECK(m->selected == 0x10);
    draw();
    CHECK(shows(w->x + 312, w->y + 148 + 80, "One of SurfOS's", MUTED, 1));
    key(GUI_UP); key(GUI_RIGHT);
    draw();
    CHECK(m->selected == 1 && shows(w->x + 312, w->y + 148 + 80, "^A", MUTED, 1) && shows(w->x + 312, w->y + 148 + 98, "copies the symbol", MUTED, 1));
}

/* every window at 640x400: what it shows above its status bar, nothing under it */
static void small_screen(void) {
    const struct gui_window *w;
    int sy, gy = 45, ch = 16, i, ty, first, rows, total;
    desktop(640, 400);
    /* the Character Map, 326 pixels high there: its cells 16 high, the last row and the Copy button whole */
    gui_open(&g, GUI_CHARMAP);
    w = &g.win[GUI_CHARMAP];
    CHECK(w->h == 400 - MENU_H - TASK_H - 20);
    draw();
    sy = w->y + w->h - STATUS_H - 1;
    CHECK(w->y + gy + 16 * ch < sy && fake_pixel(W, w->x + 26 + 8, w->y + gy + 16 * ch) == LINE);   /* the grid's end, above */
    CHECK(glyph_at(w->x + 26 + 7 * 17 + 5, w->y + gy + 15 * ch + 1, 0xF7, INK));        /* ≋ */
    CHECK(glyph_at(w->x + 26 + 14 * 17 + 5, w->y + gy + 15 * ch + 1, 0xFE, INK));       /* ■ */
    CHECK(fake_pixel(W, w->x + 26 + 15 * 17 + 8, w->y + gy + 15 * ch + 8) == 0xFFFDF5);  /* the 0xFF cell */
    CHECK(shows(w->x + 26 - 16, w->y + gy + 15 * ch + 1, "F", MUTED, 1) && shows(w->x + 26 + 7 * 17 + 5, w->y + gy - 16, "7", MUTED, 1));
    CHECK(shows(w->x + 312 + 32, sy - 34 + 5, "Copy", MUTED, 1) && sy - 34 + 26 <= sy);
    CHECK(shows(w->x + 312 + 64, w->y + 148 + 54, "none", INK, 1) && shows(w->x + 312, w->y + 148 + 98, "nothing to copy", MUTED, 1));
    click(w->x + 26 + 7 * 17 + 8, sy + 5);              /* the status bar chooses nothing */
    CHECK(g.charmap.selected == 0);
    click(w->x + 26 + 7 * 17 + 8, w->y + gy + 15 * ch + 8);
    CHECK(g.charmap.selected == 0xF7);
    click(w->x + 26 + 15 * 17 + 8, w->y + gy + 15 * ch + 14);
    CHECK(g.charmap.selected == 0xFF);
    click(w->x + 26 + 2 * 17 + 8, w->y + gy + 14 * ch + 15);    /* the last pixel row of row E */
    CHECK(g.charmap.selected == 0xE2);
    draw();
    CHECK(glyph_at(w->x + 26 + 2 * 17 + 5, w->y + gy + 14 * ch + 1, 0xE2, PAPER) && shows(w->x + 312 + 32, sy - 34 + 5, "Copy", PAPER, 1));
    click(w->x + 312 + 10, sy - 34 + 10);
    CHECK(g.clip_len == 2 && !memcmp(g.clip, "\xCE\x93", 2));    /* Γ */
    CHECK(fake_guards(W, H));
    gui_close(&g, GUI_CHARMAP);
    /* the Clock fits as it is: each page's last line above its status bar */
    gui_open(&g, GUI_CLOCK);
    w = &g.win[GUI_CLOCK];
    sy = w->y + w->h - STATUS_H - 1;
    CHECK(w->h == 300);
    g.clockapp.year = 2026;                             /* August 2026 takes six weeks */
    g.clockapp.month = 8;
    key('2');
    draw();
    CHECK(shows(w->x + (w->w - 7 * 48) / 2 + 1 * 48 + 16, w->y + TITLE_H + BAR_H + 4 + 62 + 5 * 24 + 4, "31", INK, 1));
    CHECK(w->y + TITLE_H + BAR_H + 4 + 62 + 6 * 24 <= sy);
    key('3');
    key(' ');
    for(i = 0; i < 6; i++) { tick_ms(1000); key('l'); }
    draw();
    CHECK(shows(w->x + (w->w - 34 * 8) / 2, w->y + TITLE_H + BAR_H + 4 + 124 + 4 * 18, "Lap 2 ", MUTED, 1));
    CHECK(w->y + TITLE_H + BAR_H + 4 + 124 + 5 * 18 <= sy);
    key(' ');
    key('4');
    draw();
    CHECK(shows(w->x + (w->w - 2 * 88 - 8) / 2 + 88 + 8 + (88 - 40) / 2, w->y + TITLE_H + BAR_H + 4 + 136 + 6, "Reset", MUTED, 1));
    CHECK(fake_guards(W, H));
    gui_close(&g, GUI_CLOCK);
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

/* each window opens, ticks, draws inside the screen (also maximized and hanging off the edge) and closes; the Clock's
   every page, the Log with its find bar, a selection and long lines */
static void windows(int width, int height) {
    int i, k, page;
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
        for(page = 0; page < (ids[i] == GUI_CLOCK ? 4 : 1); page++) {
            if(ids[i] == GUI_CLOCK) {
                key('1' + page);
                key(' ');                               /* the stopwatch and the timer run */
                g.now += 3723456;
            }
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
        }
        CHECK(g.win[ids[i]].x >= 0 && g.win[ids[i]].x + g.win[ids[i]].w <= width);
    }
    gui_open(&g, GUI_HELP);                             /* Help, with the suite's lines */
    gui_key(&g, GUI_END);
    draw();
    CHECK(fake_guards(width, height));
    gui_finish(&g);
    CHECK(!g.clockapp.watch_on && !g.clockapp.timer_on);
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
    calendar_arithmetic();
    calendar();
    stopwatch();
    timer();
    charmap_codes();
    charmap();
    help();
    klog_storms();
    log_review();
    clock_review();
    charmap_review();
    small_screen();
    klog_lengths();
    log_round3();
    windows(640, 400);
    windows(960, 540);
    windows(1280, 800);
    return fake_done("test_guitools");
}
