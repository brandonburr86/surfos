/*
SurfOS Desktop: Log, Clock and Character Map
--------------------
File: gui_tools.h Date: 10/17/26
--------------------
Their state in gui_state (shell/guilog.c, shell/guiclock.c, shell/guichar.c). Included by
sys/gui.h; owned by track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md.
*/
#ifndef _SYS_GUI_TOOLS_H
#define _SYS_GUI_TOOLS_H

#define GUI_LOG_MAX 16384               /* the kernel log's size (surfos/klog.h) */
struct gui_logview {
    u32 read_ms, len, written;          /* when it was read, its bytes, how many the kernel ever wrote */
    int top;
    bool follow;                        /* the view stays at the end as lines come */
    char text[GUI_LOG_MAX];
    char status[GUI_STATUS];
};

struct gui_clock {
    int page;                           /* the clock, the month, the stopwatch, the timer */
    u32 month, year;                    /* the month shown */
    u32 watch_ms, watch_at;             /* the stopwatch: what it counted, and since when it runs (0: stopped) */
    bool watch_on;
};

struct gui_charmap {
    int selected;                       /* the code page 437 character, 0-255 */
};
#endif
