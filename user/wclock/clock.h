/*
SurfOS user program: wclock
--------------------
File: clock.h   Date: 10/26/26
--------------------
The Clock's model (clock.c): its four pages, the calendar's arithmetic, the stopwatch and the
timer, what Copy takes, the title, the status bar and what a key or a press does, with the time
and the milliseconds since boot handed in by main.c, so that tests/host/test_wclock.c runs it on
the host as it is. The desktop's Clock (shell/guiclock.c, stage C12 of
docs/superpowers/plans/2026-10-25-app-windows.md) moved out of the kernel: the same pages, keys,
buttons and drawing, in a window of its own.
*/
#ifndef _WCLOCK_CLOCK_H
#define _WCLOCK_CLOCK_H

#define CLOCK_PAGE 0                    /* the pages, keys 1 to 4 */
#define CALENDAR_PAGE 1
#define STOPWATCH_PAGE 2
#define TIMER_PAGE 3
#define CLOCK_LAPS 8                    /* the stopwatch's last laps, kept */
#define CLOCK_TIMER_MAX (100 * 60 - 1)  /* the timer's longest: 99:59 */
#define CLOCK_STATUS 96

/* the window, as the desktop's Clock's was: 380 x 300 with its frame, this its content */
#define CLOCK_W 378
#define CLOCK_H 271

/* the keys that are no bytes (surfos/win.h's WIN_KEY_*) */
#define CLOCK_UP 0x100
#define CLOCK_DOWN 0x101
#define CLOCK_LEFT 0x102
#define CLOCK_RIGHT 0x103
#define CLOCK_HOME 0x104
#define CLOCK_PGUP 0x106
#define CLOCK_PGDN 0x107

/* the PC's clock as localtime() gives it; year 0: not known */
struct clock_time { unsigned year, month, date, hour, minute, second; };

struct wclock {
    int page;
    unsigned month, year;               /* the month the calendar shows (year 0: not chosen, today's) */
    unsigned now;                       /* milliseconds since boot, from main.c before each call */
    struct clock_time time;             /* the PC's clock, from main.c */
    unsigned watch_ms, watch_at;        /* the stopwatch: what it counted before it last started; since when */
    int watch_on;
    int laps;                           /* laps taken; the last CLOCK_LAPS of them in lap[] (the total at each) */
    unsigned lap[CLOCK_LAPS];
    unsigned timer_set;                 /* the timer: the seconds set */
    unsigned timer_left;                /* milliseconds left when it last paused (or was set) */
    unsigned timer_end;                 /* when it reaches zero, while it runs */
    int timer_on, timer_paused;
    int alarm;                          /* it reached zero: "Time's up!" until a key or a click answers it */
    int asking;                         /* closing asked about: the stopwatch or the timer runs */
    char status[CLOCK_STATUS];
};

/* what a key or a press asks main.c to do besides drawing again */
#define CLOCK_DRAW 1                    /* draw again */
#define CLOCK_COPY 2                    /* Copy: clock_copy()'s text to the clipboard */
#define CLOCK_CLOSE 3                   /* the window closes */

/* the calendar's arithmetic (the Gregorian calendar) */
int clock_leap(unsigned year);
int clock_days(unsigned year, unsigned month);
int clock_weekday(unsigned year, unsigned month, unsigned day);    /* 0 Sunday */
int clock_yday(unsigned year, unsigned month, unsigned day);       /* 1 January 1st */
int clock_cell(unsigned year, unsigned month, unsigned day);       /* in the month's grid, weeks from Sunday */
int clock_known(const struct clock_time *t);

void clock_init(struct wclock *c);
unsigned clock_watch(const struct wclock *c);   /* the stopwatch's milliseconds */
unsigned clock_left(const struct wclock *c);    /* the timer's milliseconds left */
unsigned clock_left_seconds(const struct wclock *c);    /* the whole seconds left, rounded up */
void clock_shown_month(const struct wclock *c, unsigned *year, unsigned *month);
void clock_watch_text(unsigned ms, char *out, int n);   /* 01:02.3, from an hour on 1:02:03.4 */
void clock_timer_text(unsigned s, char *out, int n);    /* 05:00 */

/* every pass: 1 when the timer has just reached zero (main.c then asks to be seen) */
int clock_tick(struct wclock *c);
int clock_key(struct wclock *c, int key);       /* 0, or CLOCK_DRAW, CLOCK_COPY, CLOCK_CLOSE */
int clock_press(struct wclock *c, int x, int y);    /* the left button, in the window's coordinates */
int clock_close(struct wclock *c);              /* the close button: CLOCK_CLOSE, or CLOCK_DRAW to ask first */
int clock_copy(struct wclock *c, char *out, int n); /* the page's text: its length, 0 for none (status says) */
void clock_title(const struct wclock *c, char *out, int n);
void clock_status(const struct wclock *c, char *out, int n);   /* the status bar */
int clock_busy(const struct wclock *c, char *why, int n);      /* the stopwatch or the timer runs */

/* the buttons of the page, and the closing question's, in the window's coordinates (the desktop's Clock's: its
   title bar's height above the content, a pixel of border at the left), and where the pages draw */
#define CLOCK_TITLE_H 28
#define CLOCK_WIN_W (CLOCK_W + 2)       /* the window with its frame, as the desktop's Clock's */
#define CLOCK_WIN_H (CLOCK_H + CLOCK_TITLE_H + 1)
#define CLOCK_BAR_H 28                  /* the toolbar */
#define CLOCK_STATUS_H 22               /* the status bar */
#define CLOCK_TOP (CLOCK_TITLE_H + CLOCK_BAR_H + 4)   /* the page, below the toolbar */
#define CLOCK_CAL_HEAD (CLOCK_TOP + 8)  /* the calendar: the month's row (<, >, its name, Today) */
#define CLOCK_CAL_NAMES (CLOCK_TOP + 44)    /* the weekdays' names */
#define CLOCK_CAL_TOP (CLOCK_TOP + 62)  /* the first week */
#define CLOCK_CAL_W 48                  /* a day's cell */
#define CLOCK_CAL_H 24
#define CLOCK_ASK_Y (CLOCK_WIN_H - 1 - 48)  /* the closing question's band */
struct clock_button { int x, y, w, h, act; };
int clock_buttons(const struct wclock *c, struct clock_button *b);
int clock_tool_at(int x, int y);                /* the toolbar's page at x, y, -1 */
const char *clock_tool_label(int page);
const char *clock_button_label(const struct wclock *c, int act, int *lit, int *usable);
extern const char *const clock_months[12];
extern const char *const clock_weekdays[7];

#endif
