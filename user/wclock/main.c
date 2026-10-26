/*
SurfOS user program: wclock
--------------------
File: main.c    Date: 10/26/26
--------------------
The desktop's Clock as a program with a window of its own (libgui, gui.h; the model is clock.c):
the time and the date, a month's calendar, a stopwatch with laps and a timer, chosen in the
toolbar or with the keys 1 to 4, drawn as the desktop drew its own Clock, at the same places. The
desktop's Apps menu and its Clock tile start it (shell/guicore.c's launcher stub), and bring its
window back when it runs already. It reads the PC's clock (time()) and counts on the milliseconds
since boot (uptime()), so the stopwatch and the timer go on while the window is tucked away. At
zero the timer asks the desktop to show the window (gw_alert(): in front, without the focus);
Ctrl+C puts what the page shows on the desktop's clipboard (gw_clip(), while the window has the
focus). Esc, Ctrl+W or the close button end it, asking first while the stopwatch or the timer
runs. A fault in it costs only its window. Stage C12 of
docs/superpowers/plans/2026-10-25-app-windows.md: the second application out of the kernel, after
the Calculator (wcalc).
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <surf.h>
#include <gui.h>
#include "clock.h"

/* the window's coordinates, as the desktop's Clock drew in them, onto the content's: a pixel of border at the left
   and the title bar above */
#define X0 (-1)
#define Y0 (-CLOCK_TITLE_H)
#define W CLOCK_WIN_W
#define BIG 4                           /* the large digits' scale */
#define LAPS_SHOWN 5                    /* the stopwatch's laps below its buttons */
#define COPY_MAX 512

static void read_time(struct clock_time *t) {
    time_t now = time(NULL);
    struct tm tm;
    memset(t, 0, sizeof(*t));
    if(now <= 0 || !localtime_r(&now, &tm)) return;     /* not known: year 0 */
    t->year = (unsigned)tm.tm_year + 1900;
    t->month = (unsigned)tm.tm_mon + 1;
    t->date = (unsigned)tm.tm_mday;
    t->hour = (unsigned)tm.tm_hour;
    t->minute = (unsigned)tm.tm_min;
    t->second = (unsigned)tm.tm_sec;
}

/* text at scale, centred in the window */
static void centred(struct gw *win, int y, const char *s, unsigned color, int scale) {
    gw_text_scaled(win, X0 + (W - (int)strlen(s) * 8 * scale) / 2, Y0 + y, s, color, scale);
}

/* at most cells characters, the last a dot when it goes on (the desktop's gd_name()) */
static void name(struct gw *win, int x, int y, const char *s, int cells, unsigned color) {
    char b[CLOCK_STATUS + 4];
    int n = (int)strlen(s);
    if(n <= cells) { gw_text(win, x, y, s, color); return; }
    snprintf(b, sizeof(b), "%.*s\xe2\x88\x99", cells - 1, s);
    gw_text(win, x, y, b, color);
}

static void clock_page(struct gw *win, const struct wclock *c) {
    const struct clock_time *t = &c->time;
    char b[48];
    if(!clock_known(t)) {
        centred(win, CLOCK_TOP + 60, "The date and time are not known.", GW_MUTED, 1);
        return;
    }
    snprintf(b, sizeof(b), "%02u:%02u:%02u", t->hour % 24, t->minute % 60, t->second % 60);
    centred(win, CLOCK_TOP + 10, b, GW_INK, BIG);
    centred(win, CLOCK_TOP + 88, clock_weekdays[clock_weekday(t->year, t->month, t->date)], GW_TEAL, 2);
    snprintf(b, sizeof(b), "%s %u, %u", clock_months[t->month - 1], t->date, t->year);
    centred(win, CLOCK_TOP + 124, b, GW_INK, 2);
    snprintf(b, sizeof(b), "Day %d of %d", clock_yday(t->year, t->month, t->date), clock_leap(t->year) ? 366 : 365);
    centred(win, CLOCK_TOP + 168, b, GW_MUTED, 1);
}

static void calendar_page(struct gw *win, const struct wclock *c) {
    static const char *const names[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    char b[32];
    unsigned year, month;
    int gx = X0 + (W - 7 * CLOCK_CAL_W) / 2, i, cell, days, cx, cy, today = 0, tx;
    clock_shown_month(c, &year, &month);
    if(clock_known(&c->time) && c->time.year == year && c->time.month == month) today = (int)c->time.date;
    snprintf(b, sizeof(b), "%s %u", clock_months[month - 1], year);
    tx = 70 + (W - 72 - 4 - 70 - (int)strlen(b) * 16) / 2;
    gw_text_scaled(win, X0 + tx, Y0 + CLOCK_TOP + 5, b, GW_INK, 2);
    for(i = 0; i < 7; i++) gw_text(win, gx + i * CLOCK_CAL_W + (CLOCK_CAL_W - 16) / 2, Y0 + CLOCK_CAL_NAMES, names[i], GW_MUTED);
    gw_rect(win, gx, Y0 + CLOCK_CAL_NAMES + 19, 7 * CLOCK_CAL_W, 1, GW_LINE);
    days = clock_days(year, month);
    for(i = 1; i <= days; i++) {
        cell = clock_cell(year, month, (unsigned)i);
        cx = gx + cell % 7 * CLOCK_CAL_W;
        cy = Y0 + CLOCK_CAL_TOP + cell / 7 * CLOCK_CAL_H;
        snprintf(b, sizeof(b), "%d", i);
        if(i == today) gw_rect(win, cx + 6, cy + 2, CLOCK_CAL_W - 12, CLOCK_CAL_H - 4, GW_TEAL);
        gw_text(win, cx + (CLOCK_CAL_W - (int)strlen(b) * 8) / 2, cy + 4, b, i == today ? GW_PAPER : GW_INK);
    }
}

static void stopwatch_page(struct gw *win, const struct wclock *c) {
    char b[48], at[24], split[24];
    int i, row, scale;
    clock_watch_text(clock_watch(c), b, sizeof(b));
    scale = (int)strlen(b) * 8 * BIG <= W - 16 ? BIG : 3;
    centred(win, CLOCK_TOP + 8 + (BIG - scale) * 8, b, c->watch_on ? GW_TEAL : GW_INK, scale);
    /* the newest laps first, with the time since the lap before (five shown of the eight kept: that one is kept too) */
    for(i = c->laps - 1, row = 0; i >= 0 && row < LAPS_SHOWN; i--, row++) {
        unsigned total = c->lap[i % CLOCK_LAPS];
        clock_watch_text(total, at, sizeof(at));
        clock_watch_text(total - (i ? c->lap[(i - 1) % CLOCK_LAPS] : 0), split, sizeof(split));
        snprintf(b, sizeof(b), "Lap %-3d %10s   +%s", i + 1, at, split);
        gw_text(win, X0 + (W - 34 * 8) / 2, Y0 + CLOCK_TOP + 124 + row * 18, b, row ? GW_MUTED : GW_INK);
    }
}

static void timer_page(struct gw *win, const struct wclock *c) {
    char b[16];
    clock_timer_text(c->timer_on || c->timer_paused || c->alarm ? clock_left_seconds(c) : c->timer_set, b, sizeof(b));
    centred(win, CLOCK_TOP + 8, b, c->alarm ? GW_CORAL : c->timer_on ? GW_TEAL : GW_INK, BIG);
    centred(win, CLOCK_TOP + 78, c->alarm ? "Time's up!" : c->timer_on ? "Running" : c->timer_paused ? "Paused" :
            "Set the time, then Start", c->alarm ? GW_CORAL : GW_MUTED, 1);
}

static void draw(struct gw *win, const struct wclock *c) {
    struct clock_button b[8];
    char line[CLOCK_STATUS + 32], why[64];
    int i, n, at = 8, sy = CLOCK_WIN_H - CLOCK_STATUS_H - 1, width, lit, usable;
    const char *label;
    gw_fill(win, GW_PAPER);
    gw_rect(win, X0 + 1, Y0 + CLOCK_TITLE_H, W - 2, CLOCK_BAR_H + 2, GW_BAND);
    for(i = 0; i < 4; at += width + 4, i++) {           /* the toolbar, the page shown its tab lit */
        width = (int)strlen(clock_tool_label(i)) * 8 + 14;
        gw_button(win, X0 + at, Y0 + CLOCK_TITLE_H + 4, width, 22, clock_tool_label(i), i == c->page ? GW_TEAL : GW_SHELL,
                  i == c->page ? GW_PAPER : GW_INK);
    }
    gw_rect(win, X0 + 1, Y0 + CLOCK_TITLE_H + CLOCK_BAR_H + 2, W - 2, 1, GW_LINE);
    switch(c->page) {
    case CLOCK_PAGE: clock_page(win, c); break;
    case CALENDAR_PAGE: calendar_page(win, c); break;
    case STOPWATCH_PAGE: stopwatch_page(win, c); break;
    case TIMER_PAGE: timer_page(win, c); break;
    }
    if(c->asking) {                                     /* the closing question, over the status bar */
        gw_rect(win, X0 + 1, Y0 + CLOCK_ASK_Y, W - 2, CLOCK_WIN_H - 1 - CLOCK_ASK_Y, GW_BAND);
        gw_rect(win, X0 + 1, Y0 + CLOCK_ASK_Y, W - 2, 1, GW_LINE);
        if(!clock_busy(c, why, sizeof(why))) why[0] = 0;
        name(win, X0 + 10, Y0 + CLOCK_ASK_Y + 4, why, (W - 20) / 8, GW_INK);
    }
    n = clock_buttons(c, b);
    for(i = 0; i < n; i++) {
        label = clock_button_label(c, b[i].act, &lit, &usable);
        gw_button(win, X0 + b[i].x, Y0 + b[i].y, b[i].w, b[i].h, label, lit ? GW_TEAL : usable ? GW_SHELL : GW_BAND,
                  lit ? GW_PAPER : usable ? GW_INK : GW_MUTED);
    }
    if(c->asking) return;
    gw_rect(win, X0 + 1, Y0 + sy, W - 2, CLOCK_STATUS_H, GW_BAND);
    gw_rect(win, X0 + 1, Y0 + sy, W - 2, 1, GW_LINE);
    clock_status(c, line, sizeof(line));
    name(win, X0 + 10, Y0 + sy + 3, line, (W - 20) / 8, GW_INK);
}

/* what the window shows that changes with the time alone: drawn again when it does */
static unsigned shown_now(const struct wclock *c) {
    unsigned v = (unsigned)c->page;
    if(c->page == CLOCK_PAGE) v = v * 61 + c->time.second + c->time.minute * 60 + c->time.hour * 3600;
    if(c->watch_on) v = v * 7 + (c->page == STOPWATCH_PAGE ? clock_watch(c) / 100 : clock_watch(c) / 1000);
    if(c->timer_on) v = v * 13 + clock_left_seconds(c);
    return v;
}

int main(void) {
    struct wclock c;
    struct win_event ev;
    struct gw *win;
    char title[64], titled[64] = "", text[COPY_MAX];
    unsigned buttons = 0, was = 0;
    int r, k, n, redraw = 1;
    clock_init(&c);
    c.now = uptime();
    read_time(&c.time);
    win = gw_open("Clock", CLOCK_W, CLOCK_H, 0, GW_PAPER);
    if(!win) {
        fprintf(stderr, "wclock: no window: %s\n", strerror(errno));
        return 1;
    }
    for(;;) {
        c.now = uptime();
        read_time(&c.time);
        if(clock_tick(&c)) {                            /* at zero: in front, without the focus */
            gw_alert(win);
            redraw = 1;
        }
        clock_title(&c, title, sizeof(title));
        if(strcmp(title, titled)) {                     /* the taskbar counts down too */
            gw_title(win, title);
            snprintf(titled, sizeof(titled), "%s", title);
        }
        if(redraw || shown_now(&c) != was) {
            draw(win, &c);
            if(gw_present_all(win) < 0 && errno == EPIPE) break;
            was = shown_now(&c);
            redraw = 0;
        }
        r = gw_next(win, &ev, c.watch_on && c.page == STOPWATCH_PAGE ? 50 :
                    c.watch_on || c.timer_on || c.page == CLOCK_PAGE ? 200 : 1000);
        if(r < 0) break;                                /* the desktop let it go, or a kill */
        if(r == 0) continue;
        c.now = uptime();
        k = 0;
        if(ev.type == WIN_EV_CLOSE) k = clock_close(&c);
        else if(ev.type == WIN_EV_KEY) k = clock_key(&c, (int)ev.value);
        else if(ev.type == WIN_EV_POINTER) {            /* a press of the left button, in the window's coordinates */
            if((ev.value & WIN_BUTTON_LEFT) && !(buttons & WIN_BUTTON_LEFT))
                k = clock_press(&c, ev.x - X0, ev.y - Y0);
            buttons = ev.value;
        }
        if(k == CLOCK_CLOSE) break;
        if(k == CLOCK_COPY) {
            n = clock_copy(&c, text, sizeof(text));
            if(n > 0) snprintf(c.status, sizeof(c.status), "%s", gw_clip(win, text, (unsigned)n) == 0 ? "Copied" :
                               errno == EACCES ? "Not copied: the window must have the focus" : "Not copied");
        }
        if(k) redraw = 1;
    }
    gw_close(win);
    return 0;
}
