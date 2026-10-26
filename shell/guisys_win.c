/*
SurfOS Desktop: the Kernel's Side of Programs' Windows
--------------------
File: guisys_win.c Date: 10/25/26
--------------------
struct gui_sys's win_* calls (sys/gui_sys.h) on the windows of kernel/win.c, for the desktop's
model (shell/guiclient.c); milestone M12, stage C4 of
docs/superpowers/plans/2026-10-25-app-windows.md. The desktop registers itself in shell/gui.c
(win_register(): a descriptor of its task) and its Terminal adopts its pty in guisys_term.c;
each call here is then the calling task's, whose desktop is the one it registered. Nothing here
waits: win_update() lends the window's pixels until the next update or close, so drawing reads
them without a call.
10/25/26 (stage C6): run, the launcher (Apps > Run program...).
*/
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/process.h>
#include <surfos/spawn.h>
#include <surfos/klog.h>
#include <surfos/win.h>
#include <fs/vfs.h>
#include "guisys.h"

#define RUN_LINE 128                    /* a command line the launcher takes */

int guisys_win_accept(void *ctx) {
    (void)ctx;
    return win_accept();
}

int guisys_win_update(void *ctx, int h, struct win_state *st, struct win_view *view) {
    (void)ctx;
    return win_update(h, st, view);
}

int guisys_win_event(void *ctx, int h, const struct win_event *e) {
    (void)ctx;
    return win_event(h, e);
}

void guisys_win_close(void *ctx, int h) {
    (void)ctx;
    win_close(h);
}

/* The launcher: the line's first word is a program (of /initrd/bin when it names no folder), the others its
   arguments. It starts on the desktop's console with /dev/null for its input, so it never takes the desktop's keys,
   and detached (init collects it): it outlives the Terminal, and its windows go with the desktop. Its pid, or -errno
   with why in msg. */
int guisys_win_run(void *ctx, const char *line, char *msg, size_t n) {
    char buf[RUN_LINE], path[VFS_PATH_MAX], note[96], *argv[SPAWN_MAX_ARGS], *p;
    struct file *files[3] = { NULL, NULL, NULL };
    int argc = 0, pid, r;
    (void)ctx;
    strlcpy(buf, line, sizeof(buf));
    for(p = buf; *p && argc < SPAWN_MAX_ARGS;) {
        while(*p == ' ') p++;
        if(!*p) break;
        argv[argc++] = p;
        while(*p && *p != ' ') p++;
        if(*p) *p++ = 0;
    }
    if(!argc) {
        strlcpy(msg, "No program was named.", n);
        return -EINVAL;
    }
    if(strchr(argv[0], '/')) strlcpy(path, argv[0], sizeof(path));
    else snprintf(path, sizeof(path), "/initrd/bin/%s", argv[0]);
    r = vfs_open("/dev/null", O_RDONLY, &files[0]);
    if(r) {
        snprintf(msg, n, "/dev/null: %s", strerror(r));
        return r;
    }
    pid = process_spawn_files(path, argc, argv, curTask->con, TF_DETACHED, files);
    vfs_close(files[0]);                                /* the program holds its own */
    if(pid < 0) {
        snprintf(msg, n, "%s: %s", argv[0], strerror(pid));
        return pid;
    }
    snprintf(note, sizeof(note), "gui: started %s (pid %d)\n", argv[0], pid);
    klog_write(note);
    return pid;
}
