/*
SurfOS Desktop: System Monitor and Disks Inside
--------------------
File: gui_sys.h Date: 10/17/26
--------------------
What System Monitor (guimon.c) and Disks (guidisks.c) share, in guimeter.c: sizes for people,
the bar of what is used, and a list's selection and view moved by the keys. Private to track
SYS of docs/superpowers/plans/2026-10-17-gui-suite.md (shell/guipriv.h is the desktop's).
*/
#ifndef _SHELL_GUI_SYS_H
#define _SHELL_GUI_SYS_H
#include "guipriv.h"

#define SYS_SHEET 0xFFFDF5              /* a list's background, as Files' */

void sys_kb(u32 kb, char *out, size_t n);           /* "512 KB", "9.5 MB", "62 MB", "118 GB", "1.8 TB" */
u32 sys_part(u32 part, u32 total, u32 scale);       /* part of total in scale's units (rounded down), for any sizes */
/* a bar of used out of total (any unit), w x h at x, y: TEAL what is used, CORAL from 90% */
void sys_bar(const struct gui_draw *d, int x, int y, int w, int h, u32 used, u32 total);
/* a list of count rows, rows of them shown: the row a key selects (Up, Down, Page Up and Down, Home, End), or -2 for
   a key that moves nothing; and the first row shown so that the selected one is in view */
int sys_move(int selected, int count, int rows, int key);
int sys_view(int selected, int top, int count, int rows);
/* 10/18/26: as sys_move(), but when nothing is selected because the item followed is not in this reading, from the
   row it was last on (hint): Down goes to that row (what came after it), Up to the one before */
int sys_move_from(int selected, int hint, int count, int rows, int key);
#endif
