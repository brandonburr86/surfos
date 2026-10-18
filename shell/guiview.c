/*
SurfOS Desktop: Image Viewer
--------------------
File: guiview.c Date: 10/17/26
--------------------
BMP pictures, fitted to the window or zoomed.
Track MEDIA of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_viewer (struct gui_app, shell/guipriv.h). A placeholder until the track's work lands.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>

static const char *const help[] = {
    "#Image Viewer",
    "Being built.",
};

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_VIEWER];
    gd_text(d, w->x + 16, w->y + TITLE_H + 16, "Image Viewer is being built.", MUTED);
}

const struct gui_app gui_app_viewer = {
    .title = "Image Viewer", .icon = ICON_VIEWER, .shortcut = "", .w = 560, .h = 400,
    .resizable = true, .arrows = true, .keys = false,
    .render = render,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
};
