/*
SurfOS Desktop: Log, Clock and Character Map, Privately
--------------------
File: gui_tools.h Date: 10/17/26
--------------------
What the files of track TOOLS (docs/superpowers/plans/2026-10-17-gui-suite.md) offer other files: the
calendar's arithmetic, the Log's view and the Character Map's characters, for tests/host/test_guitools.c.
Their state is in include/sys/gui_tools.h; the window manager reaches them through struct gui_app.
*/
#ifndef _SHELL_GUI_TOOLS_H
#define _SHELL_GUI_TOOLS_H
#include "guipriv.h"

/* guiclock.c: the Gregorian calendar (months 1-12, days from 1) */
bool clock_leap(u32 year);
int clock_days(u32 year, u32 month);                    /* the days in a month */
int clock_weekday(u32 year, u32 month, u32 day);        /* 0 Sunday .. 6 Saturday */
int clock_yday(u32 year, u32 month, u32 day);           /* the day of the year, 1-366 */
/* the calendar's grid: the cell (0-41, Sunday first) of a day of the month shown */
int clock_cell(u32 year, u32 month, u32 day);
/* what counts: the stopwatch's milliseconds, the timer's milliseconds left */
u32 clock_watch(const struct gui_state *g);
u32 clock_left(const struct gui_state *g);

/* guilog.c: the rows the view shows, its first row, all of them, and the text of a row's line */
void log_view(const struct gui_state *g, int *first, int *rows, int *total);
int log_line_text(const struct gui_state *g, int row, char *out, u32 n);   /* the bytes, -1 below the text */

/* guichar.c: what Copy puts on the clipboard for a character (its UTF-8; 0 bytes for the empty cell 0x00) */
int charmap_utf8(int code, char *out);
#endif
