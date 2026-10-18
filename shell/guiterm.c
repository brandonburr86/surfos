/*
SurfOS Desktop: Terminal
--------------------
File: guiterm.c Date: 10/17/26
--------------------
The SurfOS shell (surfsh) in a window: a session on a pseudo-terminal, through struct gui_sys's term_* calls.
Track TERM of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_terminal (struct gui_app, shell/guipriv.h). A placeholder until the track's work lands.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>

static const char *const help[] = {
    "#Terminal",
    "Being built.",
};

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_TERMINAL];
    gd_text(d, w->x + 16, w->y + TITLE_H + 16, "Terminal is being built.", MUTED);
}

const struct gui_app gui_app_terminal = {
    .title = "Terminal", .icon = ICON_TERMINAL, .shortcut = "Ctrl+T", .w = 640, .h = 400,
    .resizable = true, .arrows = false, .keys = true,
    .render = render,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
};
