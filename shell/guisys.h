/*
SurfOS Desktop: the Kernel's struct gui_sys
--------------------
File: guisys.h  Date: 10/17/26
--------------------
The kernel's implementation of struct gui_sys (sys/gui_sys.h) is three files, one per track of
docs/superpowers/plans/2026-10-17-gui-suite.md, so that they never edit the same one:
shell/guisys.c (the tasks, memory, the machine, the log, and the table), shell/guisys_disk.c
(Disks, track SYS) and shell/guisys_term.c (Terminal, track TERM).
10/25/26: shell/guisys_win.c, programs' windows (docs/superpowers/plans/2026-10-25-app-windows.md).
*/
#ifndef _SHELL_GUISYS_H
#define _SHELL_GUISYS_H
#include <sys/gui_sys.h>

int guisys_disks(void *ctx, struct gui_disk *out, int max);             /* guisys_disk.c */
int guisys_term_open(void *ctx, int cols, int rows);                    /* guisys_term.c */
int guisys_term_read(void *ctx, int h, char *buf, u32 n);
int guisys_term_write(void *ctx, int h, const char *buf, u32 n);
int guisys_term_resize(void *ctx, int h, int cols, int rows);
void guisys_term_close(void *ctx, int h);
int guisys_win_accept(void *ctx);                                       /* guisys_win.c (10/25/26) */
int guisys_win_update(void *ctx, int h, struct win_state *st, struct win_view *view);
int guisys_win_event(void *ctx, int h, const struct win_event *e);
void guisys_win_close(void *ctx, int h);
int guisys_win_run(void *ctx, const char *line, char *msg, size_t n);
#endif
