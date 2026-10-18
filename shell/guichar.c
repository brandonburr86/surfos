/*
SurfOS Desktop: Character Map
--------------------
File: guichar.c Date: 10/17/26
--------------------
The console's 256 characters, to copy into Notepad or the Terminal.
Track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_charmap (struct gui_app, shell/guipriv.h). A placeholder until the track's work lands.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>

static const char *const help[] = {
    "#Character Map",
    "Being built.",
};

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_CHARMAP];
    gd_text(d, w->x + 16, w->y + TITLE_H + 16, "Character Map is being built.", MUTED);
}

const struct gui_app gui_app_charmap = {
    .title = "Character Map", .icon = ICON_CHARMAP, .shortcut = "", .w = 460, .h = 360,
    .resizable = false, .arrows = true, .keys = false,
    .render = render,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
};
