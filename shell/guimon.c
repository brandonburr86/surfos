/*
SurfOS Desktop: System Monitor
--------------------
File: guimon.c Date: 10/17/26
--------------------
The tasks (and ending one), memory and the machine.
Track SYS of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_monitor (struct gui_app, shell/guipriv.h). A placeholder until the track's work lands.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>

static const char *const help[] = {
    "#System Monitor",
    "Being built.",
};

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_MONITOR];
    gd_text(d, w->x + 16, w->y + TITLE_H + 16, "System Monitor is being built.", MUTED);
}

const struct gui_app gui_app_monitor = {
    .title = "System Monitor", .icon = ICON_MONITOR, .shortcut = "", .w = 540, .h = 360,
    .resizable = true, .arrows = true, .keys = false,
    .render = render,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
};
