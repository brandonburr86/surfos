/*
SurfOS user program: wcode
--------------------
File: main.c    Date: 11/05/26
--------------------
The Code Editor (docs/superpowers/plans/2026-11-05-desktop-apps.md, track code; docs/GUI.md "Code
Editor"; man wcode): text and code in a window, a tab a file, coloured for C, assembly, Python,
shell, Makefiles, Markdown and JSON, with find and replace, find in files, auto-indent and
matching brackets. The window is window.c, the files the model's (app.c and what it uses); this is
the loop, the files' system calls (fileio.c, and the folders find in files reads), and what
happens when the desktop takes the window away.

wcode [file ...]    the files in tabs (8 at most); a name that is not there yet is a new file,
                    made when it is first saved

Status 0 when it ends by its own Quit or close button; 1 when it could not open its window, or
when the desktop's end took the window with changes unsaved (each such file's text is then put
beside it under a new name, and stderr says where; a file never saved has nowhere to go); 2 for
an option it does not know.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "window.h"
#include "fileio.h"

/**** find in files' calls: at most one folder and one file open at a time (grep.h) ****/

#define FOLDERS 2

static DIR *folders[FOLDERS];

static int fs_opendir(void *ctx, const char *path) {
    int i;
    (void)ctx;
    for(i = 0; i < FOLDERS && folders[i]; i++);
    if(i == FOLDERS || !(folders[i] = opendir(path))) return -1;
    return i;
}

static int fs_readdir(void *ctx, int h, char *name, unsigned size, int *dir, unsigned *bytes) {
    struct dirent *e;
    (void)ctx;
    if(h < 0 || h >= FOLDERS || !folders[h] || !(e = readdir(folders[h]))) return 0;
    snprintf(name, size, "%s", e->d_name);
    *dir = e->d_type == DT_DIR;
    *bytes = e->size;
    return 1;
}

static void fs_closedir(void *ctx, int h) {
    (void)ctx;
    if(h >= 0 && h < FOLDERS && folders[h]) closedir(folders[h]);
    if(h >= 0 && h < FOLDERS) folders[h] = NULL;
}

static int fs_open(void *ctx, const char *path) {
    (void)ctx;
    return open(path, O_RDONLY);
}

static int fs_read(void *ctx, int h, char *buf, unsigned n) {
    (void)ctx;
    return (int)read(h, buf, n);
}

static void fs_close(void *ctx, int h) {
    (void)ctx;
    close(h);
}

static const struct wc_fsops fs = { NULL, fs_opendir, fs_readdir, fs_closedir, fs_open, fs_read, fs_close };
static const struct wc_app_io io = { wc_file_read, wc_file_save };

/* the window has gone with changes unsaved: each file's text beside it, said on stderr */
static int rescue(struct wc_window *w) {
    char name[WC_PATH], where[WC_PATH];
    int i, lost = 0;
    for(i = 0; i < w->app.n; i++) {
        struct wc_doc *d = w->app.docs[i];
        if(wc_doc_safe(d)) continue;
        lost = 1;
        wc_doc_title(d, name, sizeof(name));
        if(wc_file_rescue(d, where, sizeof(where)) == 0)
            fprintf(stderr, "wcode: the window went with %s unsaved: its text is in %s\n", name, where);
        else if(!d->path[0]) fprintf(stderr, "wcode: the window went with %s, never saved: its text is lost\n", name);
        else fprintf(stderr, "wcode: the window went with %s unsaved, and %s\n", name, d->msg);
    }
    return lost;
}

int main(int argc, char **argv) {
    static struct wc_window w;
    struct gk_event ev;
    struct stat st;
    int i, r, quit = 0;
    for(i = 1; i < argc; i++)
        if(argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "usage: wcode [file ...]\n");
            return 2;
        }
    if(wc_window_open(&w, &io, &fs) < 0) {
        fprintf(stderr, "wcode: no window: %s\n", errno == ENODEV ? "this terminal is no desktop's" : strerror(errno));
        return 1;
    }
    if(!getcwd(w.app.cwd, sizeof(w.app.cwd))) w.app.cwd[0] = 0;  /* 11/09/26: a name not from / is taken from here */
    for(i = 1; i < argc; i++) {
        r = stat(argv[i], &st) < 0 && errno == ENOENT ? 0 : 1;
        wc_window_path(&w, argv[i], r);
    }
    for(;;) {
        r = gk_next(w.ui, &ev, w.idle ? 0 : -1);
        if(r < 0) break;                        /* the desktop let the window go, or a kill */
        if(!r) {
            wc_window_idle(&w);
            continue;
        }
        if(!wc_window_event(&w, &ev)) {
            quit = 1;
            break;
        }
    }
    r = quit ? 0 : rescue(&w);
    wc_window_close(&w);
    return r ? 1 : 0;
}
