/*
SurfOS Desktop: Image Viewer
--------------------
File: guiview.c Date: 10/17/26
--------------------
BMP pictures (guibmp.c reads them), fitted to the window or zoomed. A picture is opened with the
chooser (the toolbar's Open, or O) or by another file (viewer_show, shell/gui_media.h: Files).
Track MEDIA of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_viewer (struct gui_app, shell/guipriv.h). Its answer hook also answers Sketch's
dialogs (Save as BMP and Open BMP, guibmp.c).
11/05/26 (track img of docs/superpowers/plans/2026-11-05-desktop-apps.md): the Image Viewer is a
program now, user/wview/ (/initrd/bin/wview), out of the kernel as the Clock and the Character Map
are: BMP, PNG, GIF and JPEG through libsurf's decoders, the same views, keys, toolbar and status bar
in a window of its own that stands where this window stood, and the folder's other pictures, the
file's details and Copy path. What stays here is its entry in the suite's table: the tile, the Apps
menu and Next window's list start the program, or bring its window back (guicore.c's launcher);
viewer_show() starts it on a file, for Files, as the Image Viewer's own (its window at the Image
Viewer's place, the tile bringing it back); Help keeps its lines; and the answer hook still answers
Sketch's dialogs, whose files stay the desktop's (guibmp.c). Files' rule for what no registry entry
names (shell/guifiles.c) takes a picture of any of the four kinds here now, not a .bmp alone.
*/
#include "gui_media.h"
#include <string.h>

static const char *const help[] = {
    "#Image Viewer",
    "BMP, PNG, GIF and JPEG pictures. Open (or O) chooses",
    "one; N and P (or the toolbar's arrows) show the next",
    "and previous picture of its folder. F (or 0) fits it",
    "to the window (the default), 1 shows its actual size,",
    "+ and - zoom in and out (1/64 to 16 times); a",
    "double-click switches between Fit and the actual size.",
    "Drag a zoomed picture, or move it with the arrows,",
    "Page Up and Down, Home, End and the wheel. I shows the",
    "file's details; Ctrl+C copies its path. The Image",
    "Viewer is the program wview, in a window of its own.",
    "Sketch: Ctrl+S, or File > Save as BMP\xF9, saves the",
    "drawing as a 24-bit BMP; File > Open BMP\xF9 reads one",
    "of up to 8 MB in, fitted, in Sketch's six colours.",
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* a name it opens: a picture by its ending, in any case */
bool viewer_takes(const char *name) {
    static const char *const ends[] = { ".bmp", ".png", ".gif", ".jpg", ".jpeg" };
    u32 n = strlen(name), e, i, k;
    for(i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
        e = strlen(ends[i]);
        if(n <= e) continue;
        for(k = 0; k < e && lower((u8)name[n - e + k]) == ends[i][k]; k++) ;
        if(k == e) return true;
    }
    return false;
}

/* the program started on the file (one argument, spaces and all), its window to take the focus as it appears and
   to stand where this window stood: its pid is the Image Viewer's, as for the tile (guiclient.c's accept() places it
   by gui_app_slot_of()), so the tile brings it back. What is no file (a folder, a device, a name not there) is said
   here, as the desktop's own viewer said it, and starts nothing; a start refused leaves the Image Viewer's window
   as it was */
void viewer_show(struct gui_state *g, const char *path) {
    int k = GUI_VIEWER - GUI_FIRST_APP, pid, r;
    struct gui_entry e;
    bool device = false, readonly = false;
    char msg[80];
    g->menu = 0;
    g->dirty = true;
    if(!g->fs || !g->fs->stat) {
        dialog_message(g, "Cannot show the picture", gui_basename(path), "There are no files to read here.");
        return;
    }
    r = g->fs->stat(g->fs->ctx, path, &e, &device, &readonly);
    if(!r && e.type == GUI_DIR) r = -EISDIR;
    if(r || device) {
        dialog_message(g, "Cannot show the picture", gui_basename(path), r ? gui_error(g, r) : "It is a device, not a file.");
        return;
    }
    if(!g->sys || !g->sys->run_file) {
        dialog_message(g, "Not opened", "The Image Viewer could not be started.", gui_basename(path));
        return;
    }
    msg[0] = 0;
    pid = g->sys->run_file(g->sys->ctx, "wview", path, msg, sizeof(msg));
    if(pid < 0) {
        dialog_message(g, "Not opened", msg[0] ? msg : "The Image Viewer could not be started.", gui_basename(path));
        return;
    }
    if(pid > 0) client_launched(g, pid);
    g->app_pid[k] = pid > 0 ? pid : 0;
    g->app_at[k] = g->now;
    g->app_starting[k] = pid > 0;
}

static void answer(struct gui_state *g, int a, int button, const char *text, const char *target) {
    if(a == VIEW_SKETCH_SAVE || a == VIEW_SKETCH_REPLACE || a == VIEW_SKETCH_OPEN)     /* Sketch's (guibmp.c) */
        sketch_answer(g, a, button, text, target);
}

const struct gui_app gui_app_viewer = {
    .title = "Image Viewer", .icon = ICON_VIEWER, .shortcut = "", .w = 560, .h = 400,
    .answer = answer,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .program = "/initrd/bin/wview",
};
