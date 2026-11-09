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
10/26/26 (stage C12): win_clip, the text a program's window put on the clipboard.
11/09/26 (the second review): the programs run and run_file start are TF_TOLD, and ended gives their ends
(kernel/task.c keeps them for this desktop), so that one that fails before it opens a window is said.
11/05/26 (the desktop applications, track desk): the menus, the clipboard, the choosers and the theme
(kernel/win.c's desktop side); run_file, a program of /initrd/bin with one argument, as Files opens a
file in the program the registry names for it; note, a line for the kernel log; and the desktop's
preferences, read from and written to the settings on the stick (kernel/netman.c, net/netcfg.c's
desktop.theme and pointer.speed). A preference is written as the settings command writes it, so
the desktop waits for the stick meanwhile, as it does for Files' Mount and Eject.
*/
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/process.h>
#include <surfos/spawn.h>
#include <surfos/klog.h>
#include <surfos/job.h>
#include <surfos/win.h>
#include <fs/vfs.h>
#include <net/netman.h>
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

int guisys_win_clip(void *ctx, int h, char *out, size_t n) {
    int r;
    (void)ctx;
    r = win_clip_take(h, out, (u32)n);
    return r > 0 ? r : 0;
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
    pid = process_spawn_files(path, argc, argv, curTask->con, TF_DETACHED | TF_TOLD, files);
    vfs_close(files[0]);                                /* the program holds its own */
    if(pid < 0) {
        snprintf(msg, n, "%s: %s", argv[0], strerror(pid));
        return pid;
    }
    snprintf(note, sizeof(note), "gui: started %s (pid %d)\n", argv[0], pid);
    klog_write(note);
    return pid;
}

/**** 11/05/26 (track desk) ****/

int guisys_win_paste(void *ctx, const char *text, u32 len) {
    (void)ctx;
    return win_paste_give(text, len);
}

int guisys_win_dialog(void *ctx, int h, struct win_dialog *out, u32 *no) {
    (void)ctx;
    return win_dialog_take(h, out, no);
}

int guisys_win_answer(void *ctx, int h, u32 no, const char *path) {
    (void)ctx;
    return win_dialog_answer(h, no, path);
}

int guisys_win_theme(void *ctx, int theme, const u32 *colors) {
    (void)ctx;
    return theme < 0 ? -EINVAL : win_theme_set((u32)theme, colors);
}

/* Files' Open: /initrd/bin/<program> with arg as its one argument (a path, spaces and all), started as the launcher
   starts a line (the desktop's console, /dev/null for its input, detached): its pid, or -errno with why in msg */
int guisys_run_file(void *ctx, const char *program, const char *arg, char *msg, size_t n) {
    char path[VFS_PATH_MAX], name[32], note[96], file[VFS_PATH_MAX], *argv[2];
    struct file *files[3] = { NULL, NULL, NULL };
    int pid, r;
    (void)ctx;
    if(!program[0] || strchr(program, '/') || strlen(program) >= sizeof(name) || strlen(arg) >= sizeof(file)) {
        strlcpy(msg, "That is no program to start.", n);
        return -EINVAL;
    }
    strlcpy(name, program, sizeof(name));
    strlcpy(file, arg, sizeof(file));
    snprintf(path, sizeof(path), "/initrd/bin/%s", name);
    argv[0] = name;
    argv[1] = file;
    r = vfs_open("/dev/null", O_RDONLY, &files[0]);
    if(r) {
        snprintf(msg, n, "/dev/null: %s", strerror(r));
        return r;
    }
    pid = process_spawn_files(path, file[0] ? 2 : 1, argv, curTask->con, TF_DETACHED | TF_TOLD, files);
    vfs_close(files[0]);
    if(pid < 0) {
        snprintf(msg, n, "%s: %s", name, strerror(pid));
        return pid;
    }
    snprintf(note, sizeof(note), "gui: started %s (pid %d) with a file\n", name, pid);
    klog_write(note);
    return pid;
}

/* a line for the kernel log, whole: a text too long is cut, never its newline (11/05/26, the review of stage 2) */
void guisys_note(void *ctx, const char *line) {
    char text[160];
    size_t n;
    (void)ctx;
    n = strlcpy(text, line, sizeof(text) - 1);
    if(n > sizeof(text) - 2) n = sizeof(text) - 2;
    text[n] = '\n';
    text[n + 1] = 0;
    klog_write(text);
}

int guisys_prefs(void *ctx, int *theme, int *speed) {
    struct netcfg_settings s;
    (void)ctx;
    netman_settings(&s);
    *theme = s.theme == NETCFG_THEME_DUSK ? 1 : 0;
    *speed = s.pointer >= 1 && s.pointer <= 3 ? s.pointer : 1;
    return 0;
}

int guisys_pref_set(void *ctx, int which, int value, char *msg, size_t n) {
    static const char *const speeds[] = { "slow", "normal", "fast" };
    (void)ctx;
    msg[0] = 0;
    if(which == 0) return netman_set(NETCFG_THEME, value ? "dusk" : "ocean", msg, n);
    if(which == 1 && value >= 1 && value <= 3) return netman_set(NETCFG_POINTER, speeds[value - 1], msg, n);
    strlcpy(msg, "no such preference", n);
    return -1;
}

/* 11/09/26 (the second review): the ends of the programs this desktop started (TF_TOLD), from kernel/task.c */
int guisys_ended(void *ctx, struct gui_end *out, int max) {
    struct task_end e[TASK_ENDS];
    int n, i;
    (void)ctx;
    n = task_ends_take(e, max < TASK_ENDS ? max : TASK_ENDS);
    for(i = 0; i < n; i++) {
        out[i].pid = (int)e[i].pid;
        out[i].status = e[i].code;
        out[i].killed = e[i].how != WX_EXITED;
        out[i].window = e[i].window;
        strlcpy(out[i].name, e[i].name, sizeof(out[i].name));
    }
    return n;
}
