/*
SurfOS Desktop: the Kernel's Side of Programs' Windows
--------------------
File: guisys_win.c Date: 10/25/26
--------------------
struct gui_sys's win_* calls (sys/gui_sys.h) on the windows of kernel/win.c, for the desktop's
model (shell/guiclient.c); milestone M12, stage C4 of
docs/superpowers/plans/2026-10-25-app-windows.md. The desktop registers itself in shell/gui.c
(win_register(): a descriptor of its task) and its Terminal adopts its pty in guisys_term.c;
each call here is then the calling task's, whose desktop is the one it registered. Nothing here
waits: win_update() lends the window's pixels until the next update or close, so drawing reads
them without a call.
*/
#include <surfos/types.h>
#include <surfos/win.h>
#include "guisys.h"

int guisys_win_accept(void *ctx) {
    (void)ctx;
    return win_accept();
}

int guisys_win_update(void *ctx, int h, struct win_state *st, struct win_view *view) {
    (void)ctx;
    return win_update(h, st, view);
}

int guisys_win_event(void *ctx, int h, const struct win_event *e) {
    (void)ctx;
    return win_event(h, e);
}

void guisys_win_close(void *ctx, int h) {
    (void)ctx;
    win_close(h);
}
