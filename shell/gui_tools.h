/*
SurfOS Desktop: Log, Clock and Character Map, Privately
--------------------
File: gui_tools.h Date: 10/17/26
--------------------
What the files of track TOOLS (docs/superpowers/plans/2026-10-17-gui-suite.md) offer other files: the
Log's view and the Character Map's characters, for tests/host/test_guitools.c. Their state is in
include/sys/gui_tools.h; the window manager reaches them through struct gui_app. 10/26/26: the
calendar's arithmetic went with the Clock to user/wclock/clock.c, and the Character Map's characters
with it to user/wcharmap/charmap.c (stage C12).
*/
#ifndef _SHELL_GUI_TOOLS_H
#define _SHELL_GUI_TOOLS_H
#include "guipriv.h"

/* guilog.c: the rows the view shows, its first row, all of them, and the text of a row's line */
void log_view(const struct gui_state *g, int *first, int *rows, int *total);
int log_line_text(const struct gui_state *g, int row, char *out, u32 n);   /* the bytes, -1 below the text */
#endif
