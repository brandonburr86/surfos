/*
SurfOS Desktop: Log, Clock and Character Map
--------------------
File: gui_tools.h Date: 10/17/26
--------------------
Their state in gui_state (shell/guilog.c, shell/guiclock.c, shell/guichar.c). Included by
sys/gui.h; owned by track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md.
10/26/26 (stage C12 of docs/superpowers/plans/2026-10-25-app-windows.md): the Clock is a program
(user/wclock/), with no state here.
*/
#ifndef _SYS_GUI_TOOLS_H
#define _SYS_GUI_TOOLS_H

/* Log: a copy of the kernel log's end. A place in it is counted as `written` is, from the first byte the kernel
   ever wrote (text[i] is byte written - len + i), so the view, the selection and a match stay on their text when
   the log wraps around and old lines drop off the front. */
#define GUI_LOG_MAX 16384               /* the kernel log's size (surfos/klog.h) */
struct gui_logview {
    u32 read_ms, len, written;          /* when it was read, its bytes, how many the kernel ever wrote */
    int error;                          /* the last read's error (a negative errno), 0 */
    bool follow;                        /* the view stays at the end as lines come */
    bool finding;                       /* the find bar is open and has the keys */
    bool any;                           /* lines are selected: from anchor's line to cursor's (either way) */
    u32 top;                            /* the first line shown (where it starts) ... */
    int top_row;                        /* ... and its first row shown (a long line wraps) */
    u32 anchor, cursor;
    u32 match, match_len;               /* the last match found (match_len 0: none) */
    int drag, thumb_at;                 /* what the held button does (selecting lines, the scroll bar's thumb) */
    int drag_y;                         /* 10/18/26: where it was last (window-relative), and when it scrolled last */
    u32 drag_ms;
    struct gui_field find;
    char text[GUI_LOG_MAX];
    char status[GUI_STATUS];
};

/* 10/26/26 (stage C12): the Clock is a program now, user/wclock/; its state went with it */

/* Character Map: the 256 characters of code page 437 that the console draws */
struct gui_charmap {
    int selected;                       /* the code page 437 character, 0-255 */
    u32 click_ms;                       /* the last click, for a double-click: when, and on which */
    int click_code;
    char status[GUI_STATUS];
};
#endif
