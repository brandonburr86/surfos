/*
SurfOS user program: wclock
--------------------
File: clock.c   Date: 10/26/26
--------------------
The Clock's model: four pages, chosen in the toolbar or with the keys 1 to 4: the time and the
date large, from the PC's clock; a month's calendar, its weeks from Sunday, today marked; a
stopwatch with laps; and a timer. The weekday is worked out from the date, since firmware does
not always keep the CMOS clock's own. The stopwatch and the timer count on the milliseconds since
boot (uptime()), not on frames, so they keep running while the window is tucked away or behind
another. At zero the timer's title says "Time's up!" and main.c asks the desktop to show the
window (WIN_ALERT: in front, without the focus); a key or a click in the window answers it, and
the timer is ready again. Closing the window ends both; while one of them runs, closing it asks
first, in the window (Cancel first). Ctrl+C copies what the page shows.

The desktop's Clock (shell/guiclock.c, track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md)
moved here, out of the kernel, in stage C12 of docs/superpowers/plans/2026-10-25-app-windows.md:
the same pages, keys and buttons, at the same places (window coordinates, as the desktop's were).
The alarm, which was a message of the desktop's in front of everything, is the window shown again
in front of the others without the focus, so that no key typed elsewhere reaches it. Nothing here
makes a system call: main.c hands in the time, and tests/host/test_wclock.c runs it on the host.
*/
#include <stdio.h>
#include <string.h>
#include "clock.h"

#define TOP CLOCK_TOP
#define CAL_HEAD CLOCK_CAL_HEAD
#define CAL_TOP CLOCK_CAL_TOP
#define CAL_W CLOCK_CAL_W
#define CAL_H CLOCK_CAL_H
#define BTN_W 88                        /* the stopwatch's and the timer's buttons */
#define BTN_H 28
#define WIN_W CLOCK_WIN_W
#define ASK_Y CLOCK_ASK_Y

const char *const clock_months[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                       "September", "October", "November", "December" };
const char *const clock_weekdays[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                                        "Saturday" };
static const char *const tools[] = { "Clock", "Calendar", "Stopwatch", "Timer" };

/* the pages' buttons, and the closing question's */
enum { B_PREV = 1, B_NEXT, B_TODAY, B_WATCH, B_LAP, B_WATCH_RESET, B_MIN_LESS, B_MIN_MORE, B_SEC_LESS, B_SEC_MORE,
       B_TIMER, B_TIMER_RESET, B_ASK_CLOSE, B_ASK_CANCEL };
static const char *const setting[] = { "-1 min", "+1 min", "-10 s", "+10 s" };

/**** the calendar's arithmetic ****/

int clock_leap(unsigned year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int clock_days(unsigned year, unsigned month) {
    static const unsigned char days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if(month < 1 || month > 12) return 0;
    return month == 2 && clock_leap(year) ? 29 : days[month - 1];
}

/* Sakamoto's method, on the Gregorian calendar */
int clock_weekday(unsigned year, unsigned month, unsigned day) {
    static const unsigned char t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if(month < 1 || month > 12) return 0;
    if(month < 3) year--;
    return (int)((year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7);
}

int clock_yday(unsigned year, unsigned month, unsigned day) {
    unsigned m;
    int n = (int)day;
    for(m = 1; m < month && m <= 12; m++) n += clock_days(year, m);
    return n;
}

int clock_cell(unsigned year, unsigned month, unsigned day) {
    return clock_weekday(year, month, 1) + (int)day - 1;
}

/* the PC's clock gave a date that can be used */
int clock_known(const struct clock_time *t) {
    return t->year >= 1 && t->year <= 9999 && t->month >= 1 && t->month <= 12 && t->date >= 1 &&
           (int)t->date <= clock_days(t->year, t->month);
}

/* the month the calendar shows: the one chosen, or today's (January 2000 when the date is not known) */
void clock_shown_month(const struct wclock *c, unsigned *year, unsigned *month) {
    if(c->year) { *year = c->year; *month = c->month; }
    else if(clock_known(&c->time)) { *year = c->time.year; *month = c->time.month; }
    else { *year = 2000; *month = 1; }
}

/**** what counts ****/

void clock_init(struct wclock *c) {
    memset(c, 0, sizeof(*c));
    c->timer_set = 5 * 60;
    c->timer_left = c->timer_set * 1000;
}

unsigned clock_watch(const struct wclock *c) {
    return c->watch_ms + (c->watch_on ? c->now - c->watch_at : 0);
}

unsigned clock_left(const struct wclock *c) {
    int d;
    if(!c->timer_on) return c->timer_left;
    d = (int)(c->timer_end - c->now);
    return d > 0 ? (unsigned)d : 0;
}

unsigned clock_left_seconds(const struct wclock *c) {
    return (clock_left(c) + 999) / 1000;
}

void clock_watch_text(unsigned ms, char *out, int n) {
    unsigned t = ms / 100, s = t / 10, m = s / 60;
    if(m >= 60) snprintf(out, (size_t)n, "%u:%02u:%02u.%u", m / 60, m % 60, s % 60, t % 10);
    else snprintf(out, (size_t)n, "%02u:%02u.%u", m, s % 60, t % 10);
}

void clock_timer_text(unsigned s, char *out, int n) {
    snprintf(out, (size_t)n, "%02u:%02u", s / 60, s % 60);
}

static void status(struct wclock *c, const char *msg) {
    snprintf(c->status, sizeof(c->status), "%s", msg);
}

/**** what the buttons and keys do ****/

static void choose_page(struct wclock *c, int page) {
    c->page = page;
    c->status[0] = 0;
}

static void month_step(struct wclock *c, int months_on) {
    unsigned year, month;
    int m;
    clock_shown_month(c, &year, &month);
    m = (int)year * 12 + (int)month - 1 + months_on;
    if(m < 12) m = 12;                                  /* the years 1 to 9999 */
    if(m > 9999 * 12 + 11) m = 9999 * 12 + 11;
    c->year = (unsigned)(m / 12);
    c->month = (unsigned)(m % 12 + 1);
    c->status[0] = 0;
}

static void watch_toggle(struct wclock *c) {
    if(c->watch_on) {
        c->watch_ms += c->now - c->watch_at;
        c->watch_on = 0;
    } else {
        c->watch_at = c->now;
        c->watch_on = 1;
    }
    c->status[0] = 0;
}

static void watch_lap(struct wclock *c) {
    if(!c->watch_on) { status(c, "Laps are taken while it runs"); return; }
    c->lap[c->laps % CLOCK_LAPS] = clock_watch(c);
    c->laps++;
    c->status[0] = 0;
}

static void watch_reset(struct wclock *c) {
    c->watch_on = 0;
    c->watch_ms = 0;
    c->laps = 0;
    c->status[0] = 0;
}

/* the timer is set (not running, not paused): its time can change */
static int timer_idle(const struct wclock *c) { return !c->timer_on && !c->timer_paused; }

static void timer_add(struct wclock *c, int seconds) {
    int s = (int)c->timer_set + seconds;
    if(!timer_idle(c)) { status(c, "Reset the timer to set another time"); return; }
    c->timer_set = (unsigned)(s < 0 ? 0 : s > CLOCK_TIMER_MAX ? CLOCK_TIMER_MAX : s);
    c->timer_left = c->timer_set * 1000;
    c->alarm = 0;
    c->status[0] = 0;
}

/* Start, Pause and Resume */
static void timer_toggle(struct wclock *c) {
    c->status[0] = 0;
    if(c->timer_on) {
        c->timer_left = clock_left(c);
        c->timer_on = 0;
        c->timer_paused = 1;
        return;
    }
    if(!c->timer_paused) c->timer_left = c->timer_set * 1000;
    if(!c->timer_left) { status(c, "Set a time first: the arrow keys, or the buttons"); return; }
    c->alarm = 0;
    c->timer_end = c->now + c->timer_left;
    c->timer_on = 1;
    c->timer_paused = 0;
}

static void timer_reset(struct wclock *c) {
    c->timer_on = c->timer_paused = c->alarm = 0;
    c->timer_left = c->timer_set * 1000;
    c->status[0] = 0;
}

/* the alarm answered: the timer is ready again (the desktop's Clock's message's OK) */
static void answer(struct wclock *c) {
    c->alarm = 0;
    c->timer_left = c->timer_set * 1000;
}

static int act(struct wclock *c, int act) {
    switch(act) {
    case B_PREV: month_step(c, -1); break;
    case B_NEXT: month_step(c, 1); break;
    case B_TODAY: c->year = 0; c->status[0] = 0; break;
    case B_WATCH: watch_toggle(c); break;
    case B_LAP: watch_lap(c); break;
    case B_WATCH_RESET: watch_reset(c); break;
    case B_MIN_LESS: timer_add(c, -60); break;
    case B_MIN_MORE: timer_add(c, 60); break;
    case B_SEC_LESS: timer_add(c, -10); break;
    case B_SEC_MORE: timer_add(c, 10); break;
    case B_TIMER: timer_toggle(c); break;
    case B_TIMER_RESET: timer_reset(c); break;
    case B_ASK_CLOSE: c->asking = 0; return CLOCK_CLOSE;
    case B_ASK_CANCEL: c->asking = 0; break;
    }
    return CLOCK_DRAW;
}

static int tool_width(int page) { return (int)strlen(tools[page]) * 8 + 14; }

const char *clock_tool_label(int page) { return page >= 0 && page < 4 ? tools[page] : ""; }

/* the toolbar: buttons from 8 pixels in, 4 apart, 4 below the title bar and 22 high (the desktop's tool_hit()) */
int clock_tool_at(int x, int y) {
    int i, at = 8;
    if(y < CLOCK_TITLE_H + 4 || y >= CLOCK_TITLE_H + 4 + 22) return -1;
    for(i = 0; i < 4; at += tool_width(i) + 4, i++) if(x >= at && x < at + tool_width(i)) return i;
    return -1;
}

int clock_buttons(const struct wclock *c, struct clock_button *b) {
    int w = WIN_W, n = 0, x, i;
    if(c->asking) {                                     /* the closing question: Close, then Cancel (lit) */
        b[n++] = (struct clock_button){ w - 10 - 2 * 72 - 8, ASK_Y + 22, 72, 22, B_ASK_CLOSE };
        b[n++] = (struct clock_button){ w - 10 - 72, ASK_Y + 22, 72, 22, B_ASK_CANCEL };
        return n;
    }
    switch(c->page) {
    case CALENDAR_PAGE:
        b[n++] = (struct clock_button){ 10, CAL_HEAD, 26, 26, B_PREV };
        b[n++] = (struct clock_button){ 40, CAL_HEAD, 26, 26, B_NEXT };
        b[n++] = (struct clock_button){ w - 72, CAL_HEAD, 60, 26, B_TODAY };
        break;
    case STOPWATCH_PAGE:
        x = (w - 3 * BTN_W - 16) / 2;
        for(i = 0; i < 3; i++) b[n++] = (struct clock_button){ x + i * (BTN_W + 8), TOP + 84, BTN_W, BTN_H, B_WATCH + i };
        break;
    case TIMER_PAGE:
        x = (w - 4 * 76 - 24) / 2;
        for(i = 0; i < 4; i++) b[n++] = (struct clock_button){ x + i * 84, TOP + 100, 76, 26, B_MIN_LESS + i };
        x = (w - 2 * BTN_W - 8) / 2;
        for(i = 0; i < 2; i++) b[n++] = (struct clock_button){ x + i * (BTN_W + 8), TOP + 136, BTN_W, BTN_H, B_TIMER + i };
        break;
    }
    return n;
}

const char *clock_button_label(const struct wclock *c, int act, int *lit, int *usable) {
    *lit = 0;
    *usable = 1;
    switch(act) {
    case B_PREV: return "<";
    case B_NEXT: return ">";
    case B_TODAY: return "Today";
    case B_WATCH: *lit = 1; return c->watch_on ? "Stop" : "Start";
    case B_LAP: *usable = c->watch_on; return "Lap";
    case B_WATCH_RESET: *usable = c->watch_on || c->watch_ms || c->laps; return "Reset";
    case B_TIMER: *lit = 1; return c->timer_on ? "Pause" : c->timer_paused ? "Resume" : "Start";
    case B_TIMER_RESET: *usable = !timer_idle(c) || c->alarm; return "Reset";
    case B_ASK_CLOSE: return "Close";
    case B_ASK_CANCEL: *lit = 1; return "Cancel";
    default: *usable = timer_idle(c); return act >= B_MIN_LESS && act <= B_SEC_MORE ? setting[act - B_MIN_LESS] : "";
    }
}

/* the text Copy takes: what the page shows. The longest: the stopwatch with its eight laps, a month as cal prints it */
int clock_copy(struct wclock *c, char *out, int size) {
    const struct clock_time *t = &c->time;
    char line[48];
    unsigned year, month;
    int n = 0, i, cell, days;
#define PUT(s) do { snprintf(out + n, (size_t)(size - n), "%s", (s)); n += (int)strlen(out + n); } while(0)
    if(size < 1) return 0;
    out[0] = 0;
    switch(c->page) {
    case CLOCK_PAGE:
        if(!clock_known(t)) { status(c, "The date is not known: nothing to copy"); return 0; }
        snprintf(line, sizeof(line), "%s, %s %u, %u, %02u:%02u:%02u\n", clock_weekdays[clock_weekday(t->year, t->month,
                 t->date)], clock_months[t->month - 1], t->date, t->year, t->hour % 24, t->minute % 60, t->second % 60);
        PUT(line);
        break;
    case CALENDAR_PAGE:                                 /* as cal prints it: the title centred over the weeks */
        clock_shown_month(c, &year, &month);
        snprintf(line, sizeof(line), "%s %u", clock_months[month - 1], year);
        for(i = 0; i < (20 - (int)strlen(line)) / 2; i++) PUT(" ");
        PUT(line);
        PUT("\nSu Mo Tu We Th Fr Sa\n");
        days = clock_days(year, month);
        for(cell = 0; cell < clock_cell(year, month, 1); cell++) PUT("   ");
        for(i = 1; i <= days; i++, cell++) {
            snprintf(line, sizeof(line), "%2d%s", i, cell % 7 == 6 || i == days ? "\n" : " ");
            PUT(line);
        }
        break;
    case STOPWATCH_PAGE:                                /* its time, and the laps kept (the page shows five) */
        clock_watch_text(clock_watch(c), line, sizeof(line));
        PUT(line);
        PUT("\n");
        for(i = c->laps > CLOCK_LAPS ? c->laps - CLOCK_LAPS : 0; i < c->laps; i++) {
            char at[24];
            clock_watch_text(c->lap[i % CLOCK_LAPS], at, sizeof(at));
            snprintf(line, sizeof(line), "Lap %d  %s\n", i + 1, at);
            PUT(line);
        }
        break;
    case TIMER_PAGE:
        clock_timer_text(clock_left_seconds(c), line, sizeof(line));
        PUT(line);
        PUT("\n");
        break;
    }
#undef PUT
    return n;
}

int clock_busy(const struct wclock *c, char *why, int n) {
    char t[16];
    if(c->timer_on) {
        clock_timer_text(clock_left_seconds(c), t, sizeof(t));
        snprintf(why, (size_t)n, "The timer is running: %s left.", t);
        return 1;
    }
    if(c->watch_on) {
        clock_watch_text(clock_watch(c), t, sizeof(t));
        snprintf(why, (size_t)n, "The stopwatch is running: %s.", t);
        return 1;
    }
    return 0;
}

int clock_close(struct wclock *c) {
    char why[64];
    if(!clock_busy(c, why, sizeof(why))) return CLOCK_CLOSE;
    c->asking = 1;
    return CLOCK_DRAW;
}

void clock_title(const struct wclock *c, char *out, int n) {
    char t[16];
    if(c->alarm) snprintf(out, (size_t)n, "Time's up! - Clock");
    else if(c->timer_on) {                              /* the taskbar counts down too */
        clock_timer_text(clock_left_seconds(c), t, sizeof(t));
        snprintf(out, (size_t)n, "Clock - %s", t);
    } else snprintf(out, (size_t)n, "Clock");
}

void clock_status(const struct wclock *c, char *out, int n) {
    char t[24];
    if(c->status[0]) snprintf(out, (size_t)n, "%s", c->status);
    else if(c->page != TIMER_PAGE && (c->timer_on || c->timer_paused || c->alarm)) {   /* the timer, elsewhere */
        clock_timer_text(clock_left_seconds(c), t, sizeof(t));
        if(c->alarm) snprintf(out, (size_t)n, "Timer: time's up!");
        else snprintf(out, (size_t)n, c->timer_on ? "Timer: %s left" : "Timer: %s, paused", t);
    } else if(c->page != STOPWATCH_PAGE && c->watch_on) {
        clock_watch_text(clock_watch(c), t, sizeof(t));
        t[strlen(t) - 2] = 0;                           /* whole seconds here */
        snprintf(out, (size_t)n, "Stopwatch: %s", t);
    } else snprintf(out, (size_t)n, "%s", c->page == CLOCK_PAGE ? "The keys 1 to 4 choose a page" :
                    c->page == CALENDAR_PAGE ? "Arrows: month and year; Home: today" :
                    c->page == STOPWATCH_PAGE ? "Space: start or stop  L: lap  R: reset" :
                    "Arrows set it  Space: start  R: reset");
}

/* every pass: the timer reaching zero */
int clock_tick(struct wclock *c) {
    if(c->timer_on && (int)(c->now - c->timer_end) >= 0) {
        c->timer_on = c->timer_paused = 0;
        c->timer_left = 0;
        c->alarm = 1;
        return 1;
    }
    return 0;
}

int clock_key(struct wclock *c, int key) {
    if(c->alarm) { answer(c); return CLOCK_DRAW; }       /* any key answers it, and does nothing else */
    if(c->asking) {                                     /* the closing question: Cancel first */
        if(key == '\r' || key == '\n' || key == ' ' || key == 27) c->asking = 0;
        return CLOCK_DRAW;
    }
    if(key == 3) return CLOCK_COPY;                     /* Ctrl+C */
    if(key == 27 || key == 23) return clock_close(c);   /* Esc, Ctrl+W */
    if(key >= '1' && key <= '4') { choose_page(c, key - '1'); return CLOCK_DRAW; }
    switch(c->page) {
    case CALENDAR_PAGE:
        switch(key) {
        case CLOCK_LEFT: case CLOCK_PGUP: month_step(c, -1); return CLOCK_DRAW;
        case CLOCK_RIGHT: case CLOCK_PGDN: month_step(c, 1); return CLOCK_DRAW;
        case CLOCK_UP: month_step(c, -12); return CLOCK_DRAW;
        case CLOCK_DOWN: month_step(c, 12); return CLOCK_DRAW;
        case CLOCK_HOME: return act(c, B_TODAY);
        }
        break;
    case STOPWATCH_PAGE:
        switch(key) {
        case ' ': case '\r': case '\n': watch_toggle(c); return CLOCK_DRAW;
        case 'l': case 'L': watch_lap(c); return CLOCK_DRAW;
        case 'r': case 'R': watch_reset(c); return CLOCK_DRAW;
        }
        break;
    case TIMER_PAGE:
        switch(key) {
        case ' ': case '\r': case '\n': timer_toggle(c); return CLOCK_DRAW;
        case 'r': case 'R': timer_reset(c); return CLOCK_DRAW;
        case CLOCK_UP: timer_add(c, 60); return CLOCK_DRAW;
        case CLOCK_DOWN: timer_add(c, -60); return CLOCK_DRAW;
        case CLOCK_RIGHT: timer_add(c, 1); return CLOCK_DRAW;
        case CLOCK_LEFT: timer_add(c, -1); return CLOCK_DRAW;
        case CLOCK_PGUP: timer_add(c, 10); return CLOCK_DRAW;
        case CLOCK_PGDN: timer_add(c, -10); return CLOCK_DRAW;
        case CLOCK_HOME: timer_add(c, -CLOCK_TIMER_MAX); return CLOCK_DRAW;
        }
        break;
    }
    return 0;
}

int clock_press(struct wclock *c, int x, int y) {
    struct clock_button b[8];
    unsigned year, month;
    int i, n, cell, day;
    char line[CLOCK_STATUS];
    if(c->alarm) { answer(c); return CLOCK_DRAW; }       /* a click answers it, and does nothing else */
    n = clock_buttons(c, b);
    for(i = 0; i < n; i++)
        if(x >= b[i].x && x < b[i].x + b[i].w && y >= b[i].y && y < b[i].y + b[i].h) return act(c, b[i].act);
    if(c->asking) return 0;
    i = clock_tool_at(x, y);
    if(i >= 0) { choose_page(c, i); return CLOCK_DRAW; }
    if(c->page != CALENDAR_PAGE) return 0;
    x -= (WIN_W - 7 * CAL_W) / 2;                       /* a day: its date in the status bar */
    if(x < 0 || x >= 7 * CAL_W || y < CAL_TOP || y >= CAL_TOP + 6 * CAL_H) return 0;
    clock_shown_month(c, &year, &month);
    cell = (y - CAL_TOP) / CAL_H * 7 + x / CAL_W;
    day = cell - clock_cell(year, month, 1) + 1;
    if(day < 1 || day > clock_days(year, month)) return 0;
    snprintf(line, sizeof(line), "%s, %s %d, %u: day %d", clock_weekdays[cell % 7], clock_months[month - 1], day, year,
             clock_yday(year, month, (unsigned)day));
    status(c, line);
    return CLOCK_DRAW;
}
