/* Host tests for the Clock as a program (user/wclock/clock.c, its model; stage C12 of
   docs/superpowers/plans/2026-10-25-app-windows.md), built as it is with the host's C library. They are the
   desktop's Clock's tests (tests/host/test_guitools.c, until the Clock left the kernel) on the model: the calendar's
   arithmetic; the pages by keys and the toolbar; the calendar's months, its buttons and a day's status; the date not
   known; the stopwatch, its laps, its status on another page and past an hour; the timer's settings, its title, its
   alarm (answered by a key or a click, the timer ready again), pause and resume, Reset; closing asking first while
   one runs (Cancel first, Close ends it); what Copy takes on each page; the milliseconds since boot passing 2^32.
   The drawing is checked on the screen (tools/gui_apps_tools.py). Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include "clock.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

static struct wclock c;

#define TOP CLOCK_TOP                   /* the window's coordinates, as the desktop's Clock's */
#define W CLOCK_WIN_W

static int key(int k) { return clock_key(&c, k); }
static int click(int x, int y) { return clock_press(&c, x, y); }
static int tick_ms(unsigned ms) { c.now += ms; return clock_tick(&c); }

/* a clock at 10 s since boot, the PC's clock at 2026-10-17 14:05:30, a Saturday */
static void start(void) {
    clock_init(&c);
    c.now = 10000;
    c.time.year = 2026;
    c.time.month = 10;
    c.time.date = 17;
    c.time.hour = 14;
    c.time.minute = 5;
    c.time.second = 30;
}

static void title_is(const char *want) {
    char t[64];
    clock_title(&c, t, sizeof(t));
    CHECK(!strcmp(t, want));
}

static void status_is(const char *want) {
    char t[128];
    clock_status(&c, t, sizeof(t));
    CHECK(!strcmp(t, want));
}

static void calendar_arithmetic(void) {
    static const struct { unsigned y, m, d; int weekday; } dates[] = {
        { 1970, 1, 1, 4 }, { 2000, 1, 1, 6 }, { 2026, 10, 17, 6 }, { 2024, 2, 29, 4 }, { 2100, 3, 1, 1 },
        { 1900, 1, 1, 1 }, { 2026, 10, 1, 4 }, { 2027, 1, 1, 5 }, { 2000, 2, 29, 2 }, { 1, 1, 1, 1 },
        { 9999, 12, 31, 5 }, { 1582, 10, 15, 5 }, { 2038, 1, 19, 2 },
    };
    /* in 2026 the months begin on every day of the week */
    static const unsigned firsts[7] = { 2, 6, 9, 4, 1, 5, 8 };     /* Feb Sunday, Jun Monday, ..., Aug Saturday */
    unsigned y, m, d;
    int i, wd = 4, ok = 1, yd;
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
    CHECK(clock_cell(2026, 8, 31) / 7 == 5 && clock_cell(2026, 2, 28) / 7 == 3);  /* six weeks; four */
    for(y = 1970; y < 2101; y++) {                      /* day by day: the weekdays follow on, the days add up */
        yd = 0;
        for(m = 1; m <= 12; m++) for(d = 1; d <= (unsigned)clock_days(y, m); d++) {
            if(clock_weekday(y, m, d) != wd || clock_yday(y, m, d) != ++yd) ok = 0;
            wd = (wd + 1) % 7;
        }
        if(yd != (clock_leap(y) ? 366 : 365)) ok = 0;
    }
    CHECK(ok);
}

/* the calendar's cell of a day, in the window */
static void day_cell(unsigned year, unsigned month, unsigned day, int *x, int *y) {
    int cell = clock_cell(year, month, day);
    *x = (W - 7 * 48) / 2 + cell % 7 * 48;
    *y = TOP + 62 + cell / 7 * 24;
}

static void calendar(void) {
    char b[512];
    unsigned year, month;
    int x, y, n;
    start();
    CHECK(c.page == CLOCK_PAGE && clock_known(&c.time));
    status_is("The keys 1 to 4 choose a page");
    CHECK(key('2') == CLOCK_DRAW && c.page == CALENDAR_PAGE);
    status_is("Arrows: month and year; Home: today");
    clock_shown_month(&c, &year, &month);
    CHECK(year == 2026 && month == 10);
    CHECK(key(CLOCK_RIGHT) && c.year == 2026 && c.month == 11);
    key(CLOCK_PGDN);
    key(CLOCK_PGDN);
    CHECK(c.year == 2027 && c.month == 1);
    key(CLOCK_LEFT);
    CHECK(c.year == 2026 && c.month == 12);
    key(CLOCK_UP);
    CHECK(c.year == 2025 && c.month == 12);
    key(CLOCK_DOWN);
    key(CLOCK_DOWN);
    CHECK(c.year == 2027 && c.month == 12);
    key(CLOCK_HOME);
    CHECK(c.year == 0);
    /* the buttons: <, >, Today; a click on a day says which it is */
    click(40 + 10, TOP + 8 + 10);
    CHECK(c.month == 11);
    click(10 + 10, TOP + 8 + 10);
    click(10 + 10, TOP + 8 + 10);
    CHECK(c.month == 9 && c.year == 2026);
    click(W - 72 + 20, TOP + 8 + 10);
    CHECK(c.year == 0);
    day_cell(2026, 10, 1, &x, &y);
    CHECK(x == (W - 7 * 48) / 2 + 4 * 48);              /* Thursday: the fifth column */
    CHECK(click(x + 20, y + 10) == CLOCK_DRAW && !strcmp(c.status, "Thursday, October 1, 2026: day 274"));
    day_cell(2026, 10, 31, &x, &y);
    click(x + 20, y + 10 + 24);                         /* under the last day: no day */
    CHECK(!strcmp(c.status, "Thursday, October 1, 2026: day 274"));
    /* the limits */
    c.year = 9999; c.month = 12;
    key(CLOCK_RIGHT);
    CHECK(c.year == 9999 && c.month == 12);
    c.year = 1; c.month = 1;
    key(CLOCK_LEFT);
    key(CLOCK_UP);
    CHECK(c.year == 1 && c.month == 1);
    /* the date not known: January 2000 on the calendar, nothing to copy on the clock's page */
    memset(&c.time, 0, sizeof(c.time));
    key(CLOCK_HOME);
    clock_shown_month(&c, &year, &month);
    CHECK(!clock_known(&c.time) && year == 2000 && month == 1);
    key('1');
    CHECK(key(3) == CLOCK_COPY && clock_copy(&c, b, sizeof(b)) == 0 &&
          !strcmp(c.status, "The date is not known: nothing to copy"));
    c.time.month = 165;
    CHECK(!clock_known(&c.time) && clock_copy(&c, b, sizeof(b)) == 0);
    /* Ctrl+C copies the month as cal prints it, and the clock's page its date and time */
    start();
    key('2');
    n = clock_copy(&c, b, sizeof(b));
    CHECK(n > 0 && n == (int)strlen(b));
    {
        const char *cal = "    October 2026\nSu Mo Tu We Th Fr Sa\n             1  2  3\n 4  5  6  7  8  9 10\n";
        CHECK(!strncmp(b, cal, strlen(cal)) && !strcmp(b + n - 22, "\n25 26 27 28 29 30 31\n"));
    }
    key('1');
    CHECK(clock_copy(&c, b, sizeof(b)) > 0 && !strcmp(b, "Saturday, October 17, 2026, 14:05:30\n"));
    CHECK(clock_copy(&c, b, 8) == 7 && !strcmp(b, "Saturda"));   /* as much as fits */
    /* the toolbar */
    CHECK(clock_tool_at(8 + 2, 28 + 4 + 5) == 0 && clock_tool_at(8 + 2, 28 + 3) == -1);
    x = 8 + (5 * 8 + 14) + 4 + (8 * 8 + 14) + 4 + 3;   /* Clock, Calendar, then Stopwatch */
    CHECK(clock_tool_at(x, 28 + 10) == 2 && click(x, 28 + 10) == CLOCK_DRAW && c.page == STOPWATCH_PAGE);
}

static void stopwatch(void) {
    char why[64];
    struct clock_button b[8];
    int n, i;
    start();
    key('3');
    CHECK(!c.watch_on && clock_watch(&c) == 0);
    key(' ');
    CHECK(c.watch_on);
    tick_ms(1234);
    CHECK(clock_watch(&c) == 1234);
    key('l');
    CHECK(c.laps == 1 && c.lap[0] == 1234);
    tick_ms(1000);
    key('L');
    CHECK(c.laps == 2 && c.lap[1] == 2234);
    /* another page: it keeps counting, the status bar says its seconds */
    tick_ms(60070);
    key('1');
    status_is("Stopwatch: 01:02");
    key('3');
    key(' ');                                           /* stop */
    CHECK(!c.watch_on);
    tick_ms(5000);
    CHECK(clock_watch(&c) == 62304);
    key('l');
    CHECK(c.laps == 2 && !strcmp(c.status, "Laps are taken while it runs"));
    /* an hour on, the hours show */
    key(' ');
    tick_ms(3600000);
    clock_watch_text(clock_watch(&c), why, sizeof(why));
    CHECK(!strcmp(why, "1:01:02.3"));
    /* nine laps: eight kept */
    for(c.laps = 0; c.laps < 9;) { tick_ms(100); key('l'); }
    CHECK(c.laps == 9 && c.lap[8 % CLOCK_LAPS] == clock_watch(&c));
    /* its buttons: Start or Stop, Lap, Reset */
    n = clock_buttons(&c, b);
    CHECK(n == 3 && click(b[0].x + 5, b[0].y + 5) == CLOCK_DRAW && !c.watch_on);
    CHECK(click(b[2].x + 5, b[2].y + 5) && clock_watch(&c) == 0 && c.laps == 0);
    /* busy while it runs: closing asks first, Cancel first, and Close ends it */
    key(' ');
    tick_ms(3000);
    CHECK(clock_busy(&c, why, sizeof(why)) && !strcmp(why, "The stopwatch is running: 00:03.0."));
    CHECK(clock_close(&c) == CLOCK_DRAW && c.asking);
    n = clock_buttons(&c, b);
    CHECK(n == 2);
    key(27);                                            /* Esc: Cancel */
    CHECK(!c.asking && c.watch_on);
    CHECK(key(23) == CLOCK_DRAW && c.asking);           /* Ctrl+W asks too */
    key('\r');
    CHECK(!c.asking);
    CHECK(key(27) == CLOCK_DRAW && c.asking);           /* Esc asks */
    CHECK(click(b[1].x + 5, b[1].y + 5) == CLOCK_DRAW && !c.asking);   /* Cancel */
    clock_close(&c);
    CHECK(click(b[0].x + 5, b[0].y + 5) == CLOCK_CLOSE);                /* Close */
    key('r');
    CHECK(!c.watch_on && clock_close(&c) == CLOCK_CLOSE);  /* nothing runs: it closes at once */
    /* Copy: its time and the eight laps kept */
    start();
    key('3');
    key(' ');
    for(i = 0; i < 10; i++) { tick_ms(1000); key('l'); }
    key(' ');
    {
        char t[512];
        n = clock_copy(&c, t, sizeof(t));
        CHECK(n > 0 && !strncmp(t, "00:10.0\nLap 3  00:03.0\n", 23) && !strcmp(t + n - 16, "Lap 10  00:10.0\n"));
    }
}

static void timer(void) {
    struct clock_button b[8];
    char t[64];
    int n, i;
    start();
    key('4');
    CHECK(c.timer_set == 300 && clock_left(&c) == 300000);
    status_is("Arrows set it  Space: start  R: reset");
    /* setting it: the keys and the buttons, within 0:00 and 99:59 */
    key(CLOCK_HOME);
    CHECK(c.timer_set == 0);
    key(CLOCK_DOWN);
    key(CLOCK_LEFT);
    CHECK(c.timer_set == 0);
    key(' ');
    CHECK(!c.timer_on && !strncmp(c.status, "Set a time first", 16));
    key(CLOCK_UP); key(CLOCK_PGUP); key(CLOCK_RIGHT); key(CLOCK_RIGHT);
    CHECK(c.timer_set == 72);
    key(CLOCK_PGDN);
    CHECK(c.timer_set == 62);
    click((W - 4 * 76 - 24) / 2 + 84 + 10, TOP + 100 + 10);       /* +1 min */
    click((W - 4 * 76 - 24) / 2 + 3 * 84 + 10, TOP + 100 + 10);   /* +10 s */
    CHECK(c.timer_set == 132);
    click((W - 4 * 76 - 24) / 2 + 10, TOP + 100 + 10);            /* -1 min */
    click((W - 4 * 76 - 24) / 2 + 2 * 84 + 10, TOP + 100 + 10);   /* -10 s */
    CHECK(c.timer_set == 62);
    for(i = 0; i < 120; i++) key(CLOCK_UP);
    CHECK(c.timer_set == CLOCK_TIMER_MAX);
    /* five seconds: the title counts down, and at zero it is up */
    key(CLOCK_HOME);
    for(i = 0; i < 5; i++) key(CLOCK_RIGHT);
    CHECK(c.timer_set == 5);
    key(' ');
    CHECK(c.timer_on);
    title_is("Clock - 00:05");
    key(CLOCK_UP);                                      /* not while it runs */
    CHECK(c.timer_set == 5 && !strncmp(c.status, "Reset the timer", 15));
    CHECK(!tick_ms(4000));
    title_is("Clock - 00:01");
    CHECK(!tick_ms(999) && !c.alarm);
    CHECK(tick_ms(1) == 1 && c.alarm && !c.timer_on);  /* main.c asks the desktop to show the window */
    CHECK(!tick_ms(100));                               /* once */
    title_is("Time's up! - Clock");
    key('1');                                           /* a key answers it, and does nothing else */
    CHECK(!c.alarm && c.page == TIMER_PAGE && clock_left(&c) == 5000);
    title_is("Clock");
    /* a click answers it too */
    key(' ');
    tick_ms(5000);
    CHECK(c.alarm);
    CHECK(click(10, TOP + 10) == CLOCK_DRAW && !c.alarm && clock_left(&c) == 5000);
    /* pause and resume */
    key(' ');
    tick_ms(2000);
    key(' ');
    CHECK(c.timer_paused && clock_left(&c) == 3000);
    tick_ms(10000);
    CHECK(clock_left(&c) == 3000 && !c.alarm);
    n = clock_buttons(&c, b);
    CHECK(n == 6 && !strcmp(clock_button_label(&c, b[4].act, &i, &i), "Resume"));
    key(' ');
    CHECK(c.timer_on);
    /* Reset */
    tick_ms(1500);
    key('r');
    CHECK(!c.timer_on && clock_left(&c) == 5000);
    /* the timer elsewhere: the status bar says so; busy: closing asks */
    key(' ');
    key('1');
    status_is("Timer: 00:05 left");
    CHECK(clock_busy(&c, t, sizeof(t)) && !strcmp(t, "The timer is running: 00:05 left."));
    CHECK(clock_close(&c) == CLOCK_DRAW && c.asking);
    key(27);
    key('4');
    key(' ');
    status_is("Arrows set it  Space: start  R: reset");
    key('1');
    status_is("Timer: 00:05, paused");
    key('4');
    key('r');
    /* Ctrl+C on the timer's page copies what it shows */
    CHECK(key(3) == CLOCK_COPY && clock_copy(&c, t, sizeof(t)) == 6 && !memcmp(t, "00:05\n", 6));
}

/* what the review of the desktop's Clock found, on the model */
static void review(void) {
    char t[64];
    /* the milliseconds since boot passing 2^32: the stopwatch and the timer count on */
    start();
    c.now = 0xFFFFFFFFu - 2000;
    key('3');
    key(' ');
    tick_ms(5000);
    CHECK(clock_watch(&c) == 5000);
    key(' ');
    key('r');
    key('4');
    key(CLOCK_HOME);
    key(CLOCK_PGUP);                                    /* ten seconds */
    c.now = 0xFFFFFFFFu - 3000;
    key(' ');
    tick_ms(2000);
    key(' ');                                           /* paused with 8 s left, across 2^32 */
    tick_ms(100000);
    key(' ');
    tick_ms(7000);
    CHECK(clock_left(&c) == 1000 && !c.alarm);
    clock_title(&c, t, sizeof(t));
    CHECK(!strcmp(t, "Clock - 00:01"));
    CHECK(tick_ms(1000) && c.alarm);
}

int main(void) {
    calendar_arithmetic();
    calendar();
    stopwatch();
    timer();
    review();
    printf("test_wclock: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
