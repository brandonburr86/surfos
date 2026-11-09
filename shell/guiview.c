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
11/05/26 (the review of track img): one Image Viewer from Files. A file opened while its wview runs
replaces it: the one before is told to close (it has nothing to save), or ended when it has no
window yet (11/09/26: or when it has not gone 5 s later, or when the new one needs its window's
place), and the new one stands where it stood; Open with's entry still starts others, in the
cascade. A wview started whose window has not come in VIEWER_LATE_MS is said on the screen (tick(),
which the desktop runs for an application that is a program), and so is a desktop whose programs
have every window they may have, before anything is started.
*/
#include "gui_media.h"
#include <string.h>
#include <stdio.h>

#define VIEWER_LATE_MS 5000                     /* a wview started whose window has not come by then is said */

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

static void viewer_end(struct gui_state *g, int pid) {
    char msg[80];
    if(pid > 0 && g->sys && g->sys->kill) g->sys->kill(g->sys->ctx, pid, msg, sizeof(msg));
}

/* the Image Viewer's wview before a new one, gone. One with a window is told to close (CLOSE: wview ends at once) and
   remembered (asked_pid): when its window is still there at the next replace, or VIEWER_LATE_MS later (tick()), it is
   ended, so that a busy or hung one is not left behind. One told so before by its own button that is still there, one
   started just now whose window has not come, and, when programs have every window they may have (full), one with a
   window, whose place the new one needs, are ended at once */
static void viewer_replace(struct gui_state *g, bool full) {
    struct gui_viewer *v = &g->viewer;
    int k = GUI_VIEWER - GUI_FIRST_APP, pid = g->app_pid[k], id = client_of_pid(g, pid);
    struct gui_client *c = id >= 0 ? client_of(g, id) : NULL;
    if(v->asked_pid > 0 && client_of_pid(g, v->asked_pid) >= 0) viewer_end(g, v->asked_pid);
    v->asked_pid = 0;
    if(pid <= 0) return;
    if(c && !c->closing && !full) {
        client_close(g, id);
        v->asked_pid = pid;
        v->asked_at = g->now;
    } else if(c || (g->app_starting[k] && g->now - g->app_at[k] < VIEWER_LATE_MS)) viewer_end(g, pid);
    g->app_pid[k] = 0;
    g->app_starting[k] = false;
}

/* the program started on the file (one argument, spaces and all), its window to take the focus as it appears and
   to stand where this window stood: its pid is the Image Viewer's, as for the tile (guiclient.c's accept() places it
   by gui_app_slot_of()), so the tile brings it back. What is no file (a folder, a device, a name not there) is said
   here, as the desktop's own viewer said it, and starts nothing; a start refused leaves the Image Viewer's window
   as it was. 11/05/26 (the review): the Image Viewer's wview before it goes (viewer_replace()), and with every
   program's window the desktop has in use but that one, nothing starts: its window could not open. 11/09/26: with
   that one among them, it is ended rather than asked, so that the new one has its place */
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
    if(client_count(g) - (client_of_pid(g, g->app_pid[k]) >= 0) >= GUI_CLIENTS) {
        dialog_message(g, "Not opened", "Programs have every window they may have.", "Close one, then open the picture again.");
        return;
    }
    viewer_replace(g, client_count(g) >= GUI_CLIENTS);
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

/* a wview told to close for another whose window is still there VIEWER_LATE_MS later: ended. A wview started (from
   Files, Apps or the tile) whose window has not come in VIEWER_LATE_MS: said once, when no other dialog is up (its
   program could not open a window, or ended first: the Log may say why) */
static void tick(struct gui_state *g) {
    struct gui_viewer *v = &g->viewer;
    int k = GUI_VIEWER - GUI_FIRST_APP;
    if(v->asked_pid > 0 && g->now - v->asked_at >= VIEWER_LATE_MS) {
        if(client_of_pid(g, v->asked_pid) >= 0) viewer_end(g, v->asked_pid);
        v->asked_pid = 0;
    }
    if(!g->app_starting[k] || g->now - g->app_at[k] < VIEWER_LATE_MS || g->dialog.kind) return;
    g->app_starting[k] = false;
    if(client_of_pid(g, g->app_pid[k]) >= 0) return;
    dialog_message(g, "Not shown", "The Image Viewer did not open its window.", "Close a program's window, or see the Log.");
}

static void answer(struct gui_state *g, int a, int button, const char *text, const char *target) {
    if(a == VIEW_SKETCH_SAVE || a == VIEW_SKETCH_REPLACE || a == VIEW_SKETCH_OPEN)     /* Sketch's (guibmp.c) */
        sketch_answer(g, a, button, text, target);
}

const struct gui_app gui_app_viewer = {
    .title = "Image Viewer", .icon = ICON_VIEWER, .shortcut = "", .w = 560, .h = 400,
    .tick = tick, .answer = answer,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .program = "/initrd/bin/wview",
};
