/*
SurfOS Desktop: Disks
--------------------
File: guidisks.c Date: 10/17/26
--------------------
The block devices as lsblk lists them, how full the mounted ones are, Mount and Eject.
Track SYS of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_disks (struct gui_app, shell/guipriv.h). A placeholder until the track's work lands.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>

static const char *const help[] = {
    "#Disks",
    "Being built.",
};

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_DISKS];
    gd_text(d, w->x + 16, w->y + TITLE_H + 16, "Disks is being built.", MUTED);
}

const struct gui_app gui_app_disks = {
    .title = "Disks", .icon = ICON_DISKS, .shortcut = "", .w = 580, .h = 320,
    .resizable = true, .arrows = true, .keys = false,
    .render = render,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
};
