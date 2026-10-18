/*
SurfOS Desktop: Log
--------------------
File: guilog.c Date: 10/17/26
--------------------
The kernel log (dmesg), following what comes, with find.
Track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_log (struct gui_app, shell/guipriv.h). A placeholder until the track's work lands.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>

static const char *const help[] = {
    "#Log",
    "Being built.",
};

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_LOG];
    gd_text(d, w->x + 16, w->y + TITLE_H + 16, "Log is being built.", MUTED);
}

const struct gui_app gui_app_log = {
    .title = "Log", .icon = ICON_LOG, .shortcut = "", .w = 600, .h = 380,
    .resizable = true, .arrows = true, .keys = false,
    .render = render,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
};
