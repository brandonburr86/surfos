/*
SurfOS Desktop: Clock
--------------------
File: guiclock.c Date: 10/17/26
--------------------
The time and date, a month's calendar, a stopwatch and a timer.
Track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_clock (struct gui_app, shell/guipriv.h). A placeholder until the track's work lands.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>

static const char *const help[] = {
    "#Clock",
    "Being built.",
};

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_CLOCK];
    gd_text(d, w->x + 16, w->y + TITLE_H + 16, "Clock is being built.", MUTED);
}

const struct gui_app gui_app_clock = {
    .title = "Clock", .icon = ICON_CLOCK, .shortcut = "", .w = 380, .h = 300,
    .resizable = false, .arrows = false, .keys = false,
    .render = render,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
};
