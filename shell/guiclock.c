/*
SurfOS Desktop: Clock
--------------------
File: guiclock.c Date: 10/17/26
--------------------
Four pages, chosen in the toolbar or with the keys 1 to 4: the time and the date large, from the PC's
clock (gui_state.time, which shell/gui.c reads from the CMOS clock each second, as for the taskbar's
clock); a month's calendar, its weeks from Sunday, today marked; a stopwatch with laps; and a timer.
The weekday is worked out from the date, since firmware does not always keep the CMOS clock's own.
The stopwatch and the timer count on gui_state.now (milliseconds since boot), not on frames, so they
keep running while the window is minimized or behind another. gui_tick() watches the timer: at zero
the window's title, and so its taskbar button, says "Time's up!" and a message comes up in front of
everything (after a dialog that is open has been answered). Closing the window ends both; while one
of them runs, closing it or leaving the desktop asks first.
Track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_clock (struct gui_app, shell/guipriv.h).
*/
#include "gui_tools.h"
#include <string.h>
#include <stdio.h>

#define TOP (TITLE_H + BAR_H + 4)       /* the page, below the toolbar */
#define BIG 4                           /* the large digits' scale */
#define CAL_HEAD (TOP + 8)              /* the calendar: the month's row (<, >, its name, Today) */
#define CAL_NAMES (TOP + 44)            /* the weekdays' names */
#define CAL_TOP (TOP + 62)              /* the first week */
#define CAL_W 48                        /* a day's cell */
#define CAL_H 24
#define BTN_W 88                        /* the stopwatch's and the timer's buttons */
#define BTN_H 28
#define LAPS_SHOWN 5                    /* the stopwatch's laps below its buttons */
#define ANS_ALARM 1                     /* the alarm's message was answered (gui_app_act) */

static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                      "September", "October", "November", "December" };
static const char *const weekdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static const struct gui_tool tools[] = {
    { "Clock", GUI_CLOCK_PAGE }, { "Calendar", GUI_CALENDAR_PAGE }, { "Stopwatch", GUI_STOPWATCH_PAGE },
    { "Timer", GUI_TIMER_PAGE },
};
#define TOOLS (int)(sizeof(tools) / sizeof(tools[0]))

/* the pages' buttons */
enum { B_PREV = 1, B_NEXT, B_TODAY, B_WATCH, B_LAP, B_WATCH_RESET, B_MIN_LESS, B_MIN_MORE, B_SEC_LESS, B_SEC_MORE,
       B_TIMER, B_TIMER_RESET };
static const char *const setting[] = { "-1 min", "+1 min", "-10 s", "+10 s" };
struct button { int x, y, w, h, act; };

static const char *const help[] = {
    "#Clock",
    "The keys 1 to 4 choose the page: the time and the date,",
    "the calendar, the stopwatch, the timer.",
    "Calendar: Left and Right (or Page Up and Page Down) a",
    "month, Up and Down a year, Home this month again.",
    "Stopwatch: Space starts and stops it, L takes a lap,",
    "R resets it. Timer: Up and Down set the minutes, Left",
    "and Right the seconds (Page Up, Page Down: ten), Home",
    "0:00; Space starts or pauses it, R resets it. At zero",
    "the title says so and a message comes up, even when the",
    "window is tucked away. Both keep running while the",
    "window is minimized; closing it ends them. Ctrl+C copies",
    "what the page shows.",
};

/**** the calendar's arithmetic ****/

bool clock_leap(u32 year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int clock_days(u32 year, u32 month) {
    static const u8 days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if(month < 1 || month > 12) return 0;
    return month == 2 && clock_leap(year) ? 29 : days[month - 1];
}

/* Sakamoto's method, on the Gregorian calendar */
int clock_weekday(u32 year, u32 month, u32 day) {
    static const u8 t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if(month < 1 || month > 12) return 0;
    if(month < 3) year--;
    return (int)((year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7);
}

int clock_yday(u32 year, u32 month, u32 day) {
    u32 m;
    int n = (int)day;
    for(m = 1; m < month && m <= 12; m++) n += clock_days(year, m);
    return n;
}

int clock_cell(u32 year, u32 month, u32 day) {
    return clock_weekday(year, month, 1) + (int)day - 1;
}

/* the PC's clock gave a date that can be used */
static bool known(const struct gui_state *g) {
    const struct gui_time *t = &g->time;
    return t->year >= 1 && t->year <= 9999 && t->month >= 1 && t->month <= 12 && t->date >= 1 &&
           (int)t->date <= clock_days(t->year, t->month);
}

/* the month the calendar shows: the one chosen, or today's (January 2000 when the date is not known) */
static void shown_month(const struct gui_state *g, u32 *year, u32 *month) {
    const struct gui_clock *c = &g->clockapp;
    if(c->year) { *year = c->year; *month = c->month; }
    else if(known(g)) { *year = g->time.year; *month = g->time.month; }
    else { *year = 2000; *month = 1; }
}

/**** what counts ****/

u32 clock_watch(const struct gui_state *g) {
    const struct gui_clock *c = &g->clockapp;
    return c->watch_ms + (c->watch_on ? g->now - c->watch_at : 0);
}

u32 clock_left(const struct gui_state *g) {
    const struct gui_clock *c = &g->clockapp;
    i32 d;
    if(!c->timer_on) return c->timer_left;
    d = (i32)(c->timer_end - g->now);
    return d > 0 ? (u32)d : 0;
}

/* the stopwatch's time: 01:02.3, from an hour on 1:02:03.4 */
static void watch_text(u32 ms, char *out, size_t n) {
    u32 t = ms / 100, s = t / 10, m = s / 60;
    if(m >= 60) snprintf(out, n, "%u:%02u:%02u.%u", m / 60, m % 60, s % 60, t % 10);
    else snprintf(out, n, "%02u:%02u.%u", m, s % 60, t % 10);
}

/* the timer's: the whole seconds left, rounded up (05:00 until a second has gone) */
static u32 left_seconds(const struct gui_state *g) { return (clock_left(g) + 999) / 1000; }
static void timer_text(u32 s, char *out, size_t n) { snprintf(out, n, "%02u:%02u", s / 60, s % 60); }

static void status(struct gui_state *g, const char *msg) {
    strlcpy(g->clockapp.status, msg, sizeof(g->clockapp.status));
    g->dirty = true;
}

/**** what the buttons and keys do ****/

static void choose_page(struct gui_state *g, int page) {
    g->clockapp.page = page;
    g->clockapp.status[0] = 0;
    g->dirty = true;
}

static void month_step(struct gui_state *g, int months_on) {
    struct gui_clock *c = &g->clockapp;
    u32 year, month;
    int m;
    shown_month(g, &year, &month);
    m = (int)year * 12 + (int)month - 1 + months_on;
    if(m < 12) m = 12;                                  /* the years 1 to 9999 */
    if(m > 9999 * 12 + 11) m = 9999 * 12 + 11;
    c->year = (u32)(m / 12);
    c->month = (u32)(m % 12 + 1);
    c->status[0] = 0;
    g->dirty = true;
}

static void watch_toggle(struct gui_state *g) {
    struct gui_clock *c = &g->clockapp;
    if(c->watch_on) {
        c->watch_ms += g->now - c->watch_at;
        c->watch_on = false;
    } else {
        c->watch_at = g->now;
        c->watch_on = true;
    }
    c->status[0] = 0;
    g->dirty = true;
}

static void watch_lap(struct gui_state *g) {
    struct gui_clock *c = &g->clockapp;
    if(!c->watch_on) { status(g, "Laps are taken while it runs"); return; }
    c->lap[c->laps % GUI_LAPS] = clock_watch(g);
    c->laps++;
    c->status[0] = 0;
    g->dirty = true;
}

static void watch_reset(struct gui_state *g) {
    struct gui_clock *c = &g->clockapp;
    c->watch_on = false;
    c->watch_ms = 0;
    c->laps = 0;
    c->status[0] = 0;
    g->dirty = true;
}

/* the timer is set (not running, not paused): its time can change */
static bool timer_idle(const struct gui_clock *c) { return !c->timer_on && !c->timer_paused; }

static void timer_add(struct gui_state *g, int seconds) {
    struct gui_clock *c = &g->clockapp;
    int s = (int)c->timer_set + seconds;
    if(!timer_idle(c)) { status(g, "Reset the timer to set another time"); return; }
    c->timer_set = (u32)gui_clamp(s, 0, GUI_TIMER_MAX);
    c->timer_left = c->timer_set * 1000;
    c->alarm = false;
    c->status[0] = 0;
    g->dirty = true;
}

/* Start, Pause and Resume */
static void timer_toggle(struct gui_state *g) {
    struct gui_clock *c = &g->clockapp;
    c->status[0] = 0;
    g->dirty = true;
    if(c->timer_on) {
        c->timer_left = clock_left(g);
        c->timer_on = false;
        c->timer_paused = true;
        return;
    }
    if(!c->timer_paused) c->timer_left = c->timer_set * 1000;
    if(!c->timer_left) { status(g, "Set a time first: the arrow keys, or the buttons"); return; }
    c->alarm = false;
    c->timer_end = g->now + c->timer_left;
    c->timer_on = true;
    c->timer_paused = false;
}

static void timer_reset(struct gui_state *g) {
    struct gui_clock *c = &g->clockapp;
    c->timer_on = c->timer_paused = c->alarm = false;
    c->timer_left = c->timer_set * 1000;
    c->status[0] = 0;
    g->dirty = true;
}

static void act(struct gui_state *g, int act) {
    switch(act) {
    case B_PREV: month_step(g, -1); break;
    case B_NEXT: month_step(g, 1); break;
    case B_TODAY: g->clockapp.year = 0; g->clockapp.status[0] = 0; g->dirty = true; break;
    case B_WATCH: watch_toggle(g); break;
    case B_LAP: watch_lap(g); break;
    case B_WATCH_RESET: watch_reset(g); break;
    case B_MIN_LESS: timer_add(g, -60); break;
    case B_MIN_MORE: timer_add(g, 60); break;
    case B_SEC_LESS: timer_add(g, -10); break;
    case B_SEC_MORE: timer_add(g, 10); break;
    case B_TIMER: timer_toggle(g); break;
    case B_TIMER_RESET: timer_reset(g); break;
    }
}

/* the page's buttons, window-relative: how many */
static int page_buttons(const struct gui_state *g, struct button *b) {
    int w = g->win[GUI_CLOCK].w, n = 0, x, i;
    switch(g->clockapp.page) {
    case GUI_CALENDAR_PAGE:
        b[n++] = (struct button){ 10, CAL_HEAD, 26, 26, B_PREV };
        b[n++] = (struct button){ 40, CAL_HEAD, 26, 26, B_NEXT };
        b[n++] = (struct button){ w - 72, CAL_HEAD, 60, 26, B_TODAY };
        break;
    case GUI_STOPWATCH_PAGE:
        x = (w - 3 * BTN_W - 16) / 2;
        for(i = 0; i < 3; i++) b[n++] = (struct button){ x + i * (BTN_W + 8), TOP + 84, BTN_W, BTN_H, B_WATCH + i };
        break;
    case GUI_TIMER_PAGE:
        x = (w - 4 * 76 - 24) / 2;
        for(i = 0; i < 4; i++) b[n++] = (struct button){ x + i * 84, TOP + 100, 76, 26, B_MIN_LESS + i };
        x = (w - 2 * BTN_W - 8) / 2;
        for(i = 0; i < 2; i++) b[n++] = (struct button){ x + i * (BTN_W + 8), TOP + 136, BTN_W, BTN_H, B_TIMER + i };
        break;
    }
    return n;
}

/* the text that Copy puts on the clipboard: what the page shows */
static void copy(struct gui_state *g) {
    const struct gui_clock *c = &g->clockapp;
    const struct gui_time *t = &g->time;
    char *out = g->clip, line[48];
    u32 n = 0, year, month;
    int i, cell, days;
#define PUT(s) do { strlcpy(out + n, (s), GUI_CLIP_MAX - n); n += strlen(out + n); } while(0)
    out[0] = 0;
    switch(c->page) {
    case GUI_CLOCK_PAGE:
        if(!known(g)) { status(g, "The date is not known: nothing to copy"); return; }
        snprintf(line, sizeof(line), "%s, %s %u, %u, %02u:%02u:%02u\n", weekdays[clock_weekday(t->year, t->month, t->date)],
                 months[t->month - 1], t->date, t->year, t->hour % 24, t->minute % 60, t->second % 60);
        PUT(line);
        break;
    case GUI_CALENDAR_PAGE:                             /* as cal prints it */
        shown_month(g, &year, &month);
        snprintf(line, sizeof(line), "%s %u\nSu Mo Tu We Th Fr Sa\n", months[month - 1], year);
        PUT(line);
        days = clock_days(year, month);
        for(cell = 0; cell < clock_cell(year, month, 1); cell++) PUT("   ");
        for(i = 1; i <= days; i++, cell++) {
            snprintf(line, sizeof(line), "%2d%s", i, cell % 7 == 6 || i == days ? "\n" : " ");
            PUT(line);
        }
        break;
    case GUI_STOPWATCH_PAGE:
        watch_text(clock_watch(g), line, sizeof(line));
        PUT(line);
        PUT("\n");
        for(i = c->laps > GUI_LAPS ? c->laps - GUI_LAPS : 0; i < c->laps; i++) {
            char at[24];
            watch_text(c->lap[i % GUI_LAPS], at, sizeof(at));
            snprintf(line, sizeof(line), "Lap %d  %s\n", i + 1, at);
            PUT(line);
        }
        break;
    case GUI_TIMER_PAGE:
        timer_text(left_seconds(g), line, sizeof(line));
        PUT(line);
        PUT("\n");
        break;
    }
#undef PUT
    g->clip_len = n;
    status(g, "Copied");
}

static bool can_edit(const struct gui_state *g, int act) {
    (void)g;
    return act == ACT_COPY;
}

static void edit(struct gui_state *g, int act) {
    if(act == ACT_COPY) copy(g);
}

/**** the window ****/

static void opened(struct gui_state *g) {
    struct gui_clock *c = &g->clockapp;
    c->year = 0;                                        /* the calendar: this month */
    if(!c->timer_set) c->timer_set = 5 * 60;
    c->timer_left = c->timer_set * 1000;
    c->status[0] = 0;
}

/* the window closes (or the desktop does): the stopwatch and the timer end */
static void closed(struct gui_state *g) {
    struct gui_clock *c = &g->clockapp;
    c->watch_on = false;
    c->watch_ms = 0;
    c->laps = 0;
    c->timer_on = c->timer_paused = c->alarm = c->alarm_asked = false;
    c->timer_left = c->timer_set * 1000;
}

static bool busy(const struct gui_state *g, char *why, size_t n) {
    const struct gui_clock *c = &g->clockapp;
    char t[16];
    if(c->timer_on) {
        timer_text(left_seconds(g), t, sizeof(t));
        snprintf(why, n, "The timer is running: %s left.", t);
        return true;
    }
    if(c->watch_on) {
        watch_text(clock_watch(g), t, sizeof(t));
        snprintf(why, n, "The stopwatch is running: %s.", t);
        return true;
    }
    return false;
}

static void title_of(const struct gui_state *g, char *out, size_t n) {
    const struct gui_clock *c = &g->clockapp;
    char t[16];
    if(c->alarm) strlcpy(out, "Time's up! - Clock", n);
    else if(c->timer_on) {                              /* the taskbar counts down too */
        timer_text(left_seconds(g), t, sizeof(t));
        snprintf(out, n, "Clock - %s", t);
    } else strlcpy(out, "Clock", n);
}

/* every frame, minimized too: the timer reaching zero, and what counts drawn again when it shows a new value */
static void tick(struct gui_state *g) {
    struct gui_clock *c = &g->clockapp;
    char line[48], t[16];
    u32 v;
    if(c->timer_on && (i32)(g->now - c->timer_end) >= 0) {
        c->timer_on = c->timer_paused = false;
        c->timer_left = 0;
        c->alarm = true;
        c->alarm_asked = false;
        g->dirty = true;                                /* the title, in the taskbar too */
    }
    if(c->alarm && !c->alarm_asked && !g->dialog.kind) {   /* never over a question that waits for its answer */
        c->alarm_asked = true;
        timer_text(c->timer_set, t, sizeof(t));
        snprintf(line, sizeof(line), "The %s timer has finished.", t);
        dialog_confirm(g, gui_app_act(GUI_CLOCK, ANS_ALARM), "Timer", "Time's up!", line, "OK", NULL, NULL);
    }
    v = c->timer_on ? left_seconds(g) : 0;
    if(v != c->shown_left) { c->shown_left = v; g->dirty = true; }
    v = !c->watch_on || g->win[GUI_CLOCK].min ? 0 :     /* its page shows tenths, the others' status bar seconds */
        c->page == GUI_STOPWATCH_PAGE ? clock_watch(g) / 100 : clock_watch(g) / 1000;
    if(v != c->shown_watch) { c->shown_watch = v; g->dirty = true; }
}

static void answer(struct gui_state *g, int act, int button, const char *text, const char *target) {
    struct gui_clock *c = &g->clockapp;
    (void)button; (void)text; (void)target;
    if(act != ANS_ALARM) return;
    c->alarm = c->alarm_asked = false;                  /* OK or Esc: the timer is ready again */
    c->timer_left = c->timer_set * 1000;
    g->dirty = true;
}

static bool key(struct gui_state *g, int key) {
    struct gui_clock *c = &g->clockapp;
    if(key >= '1' && key <= '4') { choose_page(g, key - '1'); return true; }
    switch(c->page) {
    case GUI_CALENDAR_PAGE:
        switch(key) {
        case GUI_LEFT: case GUI_PGUP: month_step(g, -1); return true;
        case GUI_RIGHT: case GUI_PGDN: month_step(g, 1); return true;
        case GUI_UP: month_step(g, -12); return true;
        case GUI_DOWN: month_step(g, 12); return true;
        case GUI_HOME: act(g, B_TODAY); return true;
        }
        break;
    case GUI_STOPWATCH_PAGE:
        switch(key) {
        case ' ': case '\r': case '\n': watch_toggle(g); return true;
        case 'l': case 'L': watch_lap(g); return true;
        case 'r': case 'R': watch_reset(g); return true;
        }
        break;
    case GUI_TIMER_PAGE:
        switch(key) {
        case ' ': case '\r': case '\n': timer_toggle(g); return true;
        case 'r': case 'R': timer_reset(g); return true;
        case GUI_UP: timer_add(g, 60); return true;
        case GUI_DOWN: timer_add(g, -60); return true;
        case GUI_RIGHT: timer_add(g, 1); return true;
        case GUI_LEFT: timer_add(g, -1); return true;
        case GUI_PGUP: timer_add(g, 10); return true;
        case GUI_PGDN: timer_add(g, -10); return true;
        case GUI_HOME: timer_add(g, -GUI_TIMER_MAX); return true;
        }
        break;
    }
    return false;
}

static void press(struct gui_state *g, int x, int y) {
    struct gui_clock *c = &g->clockapp;
    struct button b[8];
    u32 year, month;
    int i, n, cell, day;
    char line[GUI_STATUS];
    i = tool_hit(tools, TOOLS, x, y);
    if(i >= 0) { choose_page(g, tools[i].act); return; }
    n = page_buttons(g, b);
    for(i = 0; i < n; i++) if(gui_inside(x, y, b[i].x, b[i].y, b[i].w, b[i].h)) { act(g, b[i].act); return; }
    if(c->page != GUI_CALENDAR_PAGE) return;
    x -= (g->win[GUI_CLOCK].w - 7 * CAL_W) / 2;         /* a day: its date in the status bar */
    if(x < 0 || x >= 7 * CAL_W || y < CAL_TOP || y >= CAL_TOP + 6 * CAL_H) return;
    shown_month(g, &year, &month);
    cell = (y - CAL_TOP) / CAL_H * 7 + x / CAL_W;
    day = cell - clock_cell(year, month, 1) + 1;
    if(day < 1 || day > clock_days(year, month)) return;
    snprintf(line, sizeof(line), "%s, %s %d, %u: day %d", weekdays[cell % 7], months[month - 1], day, year,
             clock_yday(year, month, (u32)day));
    status(g, line);
}

/**** drawing ****/

/* text at scale, centred in the window */
static void centred(const struct gui_draw *d, int x0, int w, int y, const char *s, u32 c, int scale) {
    gd_text_scaled(d, x0 + (w - (int)strlen(s) * 8 * scale) / 2, y, s, c, scale);
}

static void clock_page(const struct gui_state *g, const struct gui_draw *d, int x0, int y0, int w) {
    const struct gui_time *t = &g->time;
    char b[48];
    if(!known(g)) {
        centred(d, x0, w, y0 + TOP + 60, "The date and time are not known.", MUTED, 1);
        return;
    }
    snprintf(b, sizeof(b), "%02u:%02u:%02u", t->hour % 24, t->minute % 60, t->second % 60);
    centred(d, x0, w, y0 + TOP + 10, b, INK, BIG);
    centred(d, x0, w, y0 + TOP + 88, weekdays[clock_weekday(t->year, t->month, t->date)], TEAL, 2);
    snprintf(b, sizeof(b), "%s %u, %u", months[t->month - 1], t->date, t->year);
    centred(d, x0, w, y0 + TOP + 124, b, INK, 2);
    snprintf(b, sizeof(b), "Day %d of %d", clock_yday(t->year, t->month, t->date), clock_leap(t->year) ? 366 : 365);
    centred(d, x0, w, y0 + TOP + 168, b, MUTED, 1);
}

static void calendar_page(const struct gui_state *g, const struct gui_draw *d, int x0, int y0, int w) {
    static const char *const names[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    char b[32];
    u32 year, month;
    int gx = x0 + (w - 7 * CAL_W) / 2, i, cell, days, cx, cy, today = 0, tx;
    shown_month(g, &year, &month);
    if(known(g) && g->time.year == year && g->time.month == month) today = (int)g->time.date;
    snprintf(b, sizeof(b), "%s %u", months[month - 1], year);
    tx = 70 + (w - 72 - 4 - 70 - (int)strlen(b) * 16) / 2;
    gd_text_scaled(d, x0 + tx, y0 + TOP + 5, b, INK, 2);
    for(i = 0; i < 7; i++) gd_text(d, gx + i * CAL_W + (CAL_W - 16) / 2, y0 + CAL_NAMES, names[i], MUTED);
    gd_rect(d, gx, y0 + CAL_NAMES + 19, 7 * CAL_W, 1, LINE);
    days = clock_days(year, month);
    for(i = 1; i <= days; i++) {
        cell = clock_cell(year, month, (u32)i);
        cx = gx + cell % 7 * CAL_W;
        cy = y0 + CAL_TOP + cell / 7 * CAL_H;
        snprintf(b, sizeof(b), "%d", i);
        if(i == today) gd_rect(d, cx + 6, cy + 2, CAL_W - 12, CAL_H - 4, TEAL);
        gd_text(d, cx + (CAL_W - (int)strlen(b) * 8) / 2, cy + 4, b, i == today ? PAPER : INK);
    }
}

static void stopwatch_page(const struct gui_state *g, const struct gui_draw *d, int x0, int y0, int w) {
    const struct gui_clock *c = &g->clockapp;
    char b[48], at[24], split[24];
    int i, row, scale;
    watch_text(clock_watch(g), b, sizeof(b));
    scale = (int)strlen(b) * 8 * BIG <= w - 16 ? BIG : 3;
    centred(d, x0, w, y0 + TOP + 8 + (BIG - scale) * 8, b, c->watch_on ? TEAL : INK, scale);
    /* the newest laps first, with the time since the lap before (five shown of the eight kept: that one is kept too) */
    for(i = c->laps - 1, row = 0; i >= 0 && row < LAPS_SHOWN; i--, row++) {
        u32 total = c->lap[i % GUI_LAPS];
        watch_text(total, at, sizeof(at));
        watch_text(total - (i ? c->lap[(i - 1) % GUI_LAPS] : 0), split, sizeof(split));
        snprintf(b, sizeof(b), "Lap %-3d %10s   +%s", i + 1, at, split);
        gd_text(d, x0 + (w - 34 * 8) / 2, y0 + TOP + 124 + row * 18, b, row ? MUTED : INK);
    }
}

static void timer_page(const struct gui_state *g, const struct gui_draw *d, int x0, int y0, int w) {
    const struct gui_clock *c = &g->clockapp;
    char b[16];
    timer_text(c->timer_on || c->timer_paused || c->alarm ? left_seconds(g) : c->timer_set, b, sizeof(b));
    centred(d, x0, w, y0 + TOP + 8, b, c->alarm ? CORAL : c->timer_on ? TEAL : INK, BIG);
    centred(d, x0, w, y0 + TOP + 78, c->alarm ? "Time's up!" : c->timer_on ? "Running" : c->timer_paused ? "Paused" :
            "Set the time, then Start", c->alarm ? CORAL : MUTED, 1);
}

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_CLOCK];
    const struct gui_clock *c = &g->clockapp;
    struct button b[8];
    bool on[TOOLS], idle = timer_idle(c);
    char line[GUI_STATUS + 32], t[24];
    const char *label;
    int x0 = w->x, y0 = w->y, i, n, at = 8, sy = w->h - STATUS_H - 1;
    for(i = 0; i < TOOLS; i++) on[i] = true;
    gd_rect(d, x0 + 1, y0 + TITLE_H, w->w - 2, BAR_H + 2, BAND);
    tool_render(d, x0, y0, tools, TOOLS, on);
    for(i = 0; i < TOOLS; at += (int)strlen(tools[i].label) * 8 + 14 + 4, i++)   /* the page shown: its tab lit */
        if(tools[i].act == c->page)
            gd_button(d, x0 + at, y0 + TITLE_H + 4, (int)strlen(tools[i].label) * 8 + 14, 22, tools[i].label, TEAL, PAPER);
    gd_rect(d, x0 + 1, y0 + TITLE_H + BAR_H + 2, w->w - 2, 1, LINE);
    switch(c->page) {
    case GUI_CLOCK_PAGE: clock_page(g, d, x0, y0, w->w); break;
    case GUI_CALENDAR_PAGE: calendar_page(g, d, x0, y0, w->w); break;
    case GUI_STOPWATCH_PAGE: stopwatch_page(g, d, x0, y0, w->w); break;
    case GUI_TIMER_PAGE: timer_page(g, d, x0, y0, w->w); break;
    }
    n = page_buttons(g, b);
    for(i = 0; i < n; i++) {
        bool lit = false, usable = true;
        switch(b[i].act) {
        case B_PREV: label = "<"; break;
        case B_NEXT: label = ">"; break;
        case B_TODAY: label = "Today"; break;
        case B_WATCH: label = c->watch_on ? "Stop" : "Start"; lit = true; break;
        case B_LAP: label = "Lap"; usable = c->watch_on; break;
        case B_WATCH_RESET: label = "Reset"; usable = c->watch_on || c->watch_ms || c->laps; break;
        case B_TIMER: label = c->timer_on ? "Pause" : c->timer_paused ? "Resume" : "Start"; lit = true; break;
        case B_TIMER_RESET: label = "Reset"; usable = !idle || c->alarm; break;
        default: label = setting[b[i].act - B_MIN_LESS]; usable = idle; break;
        }
        gd_button(d, x0 + b[i].x, y0 + b[i].y, b[i].w, b[i].h, label, lit ? TEAL : usable ? SHELL : BAND,
                  lit ? PAPER : usable ? INK : MUTED);
    }
    gd_rect(d, x0 + 1, y0 + sy, w->w - 2, STATUS_H, BAND);
    gd_rect(d, x0 + 1, y0 + sy, w->w - 2, 1, LINE);
    if(c->status[0]) strlcpy(line, c->status, sizeof(line));
    else if(c->page != GUI_TIMER_PAGE && (c->timer_on || c->timer_paused || c->alarm)) {   /* the timer, elsewhere */
        timer_text(left_seconds(g), t, sizeof(t));
        snprintf(line, sizeof(line), c->alarm ? "Timer: time's up!" : c->timer_on ? "Timer: %s left" : "Timer: %s, paused", t);
    } else if(c->page != GUI_STOPWATCH_PAGE && c->watch_on) {
        watch_text(clock_watch(g), t, sizeof(t));
        t[strlen(t) - 2] = 0;                           /* whole seconds here */
        snprintf(line, sizeof(line), "Stopwatch: %s", t);
    } else strlcpy(line, c->page == GUI_CLOCK_PAGE ? "The keys 1 to 4 choose a page" :
                         c->page == GUI_CALENDAR_PAGE ? "Arrows: month and year; Home: today" :
                         c->page == GUI_STOPWATCH_PAGE ? "Space: start or stop  L: lap  R: reset" :
                         "Arrows set it  Space: start  R: reset", sizeof(line));
    gd_name(d, x0 + 10, y0 + sy + 3, line, (w->w - 20) / 8, INK);
}

const struct gui_app gui_app_clock = {
    .title = "Clock", .icon = ICON_CLOCK, .shortcut = "", .w = 380, .h = 300,
    .resizable = false, .arrows = true, .keys = false,
    .opened = opened, .closed = closed, .render = render, .key = key, .press = press, .tick = tick,
    .title_of = title_of, .busy = busy, .answer = answer,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .can_edit = can_edit, .edit = edit,
};
