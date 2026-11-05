/*
SurfOS Desktop: the System's Side of the Applications
--------------------
File: gui_sys.h Date: 10/17/26
--------------------
What the desktop's applications ask of the kernel (docs/superpowers/plans/2026-10-17-gui-suite.md):
the tasks, memory and the machine (System Monitor), the block devices (Disks), the kernel log
(Log), and a shell session on a pseudo-terminal (Terminal). Like struct gui_fs, the model calls
it from its handlers only (gui_key, gui_pointer, gui_tick), never while drawing, and it never
waits long: a call returns what is there now. Every call returns 0 (a list: how many entries)
or a negative errno. shell/guisys.c implements it on the kernel, tests/host/gui_fake.c in memory.
10/25/26 (programs' windows, docs/superpowers/plans/2026-10-25-app-windows.md): five calls at its
end for the windows of ordinary programs (shell/guiclient.c) and the launcher.
11/05/26 (the desktop applications, docs/superpowers/plans/2026-11-05-desktop-apps.md, track desk):
programs' menus, the clipboard they paste, their choosers and the theme (kernel/win.c's desktop
side), a program started with one argument (Files' Open), the kernel log's lines (the application
registry's refusals), and the desktop's preferences in the settings on the stick.
*/
#ifndef _SYS_GUI_SYS_H
#define _SYS_GUI_SYS_H
#include <surfos/types.h>
#include <surfos/win.h>                 /* 10/25/26: struct win_event, win_state, win_view */

/* the time of day and the date, as the CMOS clock keeps them (shell/gui.c sets gui_state.time) */
struct gui_time { u32 year, month, date, weekday, hour, minute, second; };   /* weekday: 1 Sunday .. 7 Saturday */

/**** System Monitor ****/

#define GUI_TASK_MAX 128                /* 10/18/26: 64 before; tasks() keeps the newest when there are more */
#define GUI_TASK_NAME 16
/* a task as surfos/proc.h describes it: state GUI_TASK_*; tty 0-3 the consoles F1-F4, 16+n pty n, -1 none */
#define GUI_TASK_READY 0
#define GUI_TASK_RUNNING 1
#define GUI_TASK_BLOCKED 2
#define GUI_TASK_SLEEPING 3
#define GUI_TASK_DEAD 4
#define GUI_TASK_USER 0x1               /* a program in ring 3 (else a kernel thread) */
#define GUI_TASK_DETACHED 0x2
#define GUI_TASK_SHELL 0x4
struct gui_task { int pid, ppid, state, tty; u32 flags; char name[GUI_TASK_NAME]; };

/* memory as memstat counts it. 10/17/26 (System Monitor): a heap's size is the most it may grow to (its room in the
   address space, its arena); used and free are of what it has grown to */
struct gui_memory {
    u32 ram_kb, high_kb;                /* physical RAM; above 4 GB (not used) */
    u32 free_pages, total_pages;        /* 4 KB frames */
    u32 heap_used, heap_free, heap_size;    /* the kernel heap, bytes */
    u32 dma_used, dma_free, dma_size;       /* the DMA heap */
};

/* the machine, for System Monitor's System page */
struct gui_machine {
    char cpu[64];                       /* the processor's brand string, or its vendor */
    char firmware[16];                  /* "UEFI", "BIOS" */
    char profile[16];                   /* "laptop", "standard" */
    char video[32];                     /* the framebuffer's mode, "1920x1080x32" */
    char build[80];                     /* /initrd/etc/build-id's first line, "" when there is none */
    u32 uptime_s, tasks, timers;
};

/**** Disks ****/

#define GUI_DISK_MAX 32
/* a block device as lsblk lists it, and, mounted, where and how full. 10/17/26 (Disks): lsblk's own words (size,
   type, label, status); readonly: it is mounted read-only, or would be; gone: unplugged while mounted (the mount is
   still there, for Eject) */
struct gui_disk {
    char name[16], disk[16];            /* usb1p1, and the disk it is on (usb1); a disk: its own name */
    char bus[8];                        /* "usb", "ata", "ram" */
    char type[32];                      /* "disk gpt", "part mbr FAT32 LBA" */
    char label[48];                     /* the model, or the partition's label */
    char status[40];                    /* lsblk's STATUS: "mounted", "boot drive: protected", ... */
    char mount[256];                    /* where it is mounted, "" */
    u32 size_kb, used_kb, free_kb;      /* used/free: a mounted file system's, 0 otherwise */
    bool part, removable, readonly, gone, mounted;
    char size[8];                       /* 10/17/26: lsblk's SIZE, "254M" */
    char fs[8], volume[12];             /* mounted: the file system ("fat32") and its volume label, "" */
    int statfs_err;                     /* 10/18/26: mounted, but its file system could not say how full: -errno */
};

struct gui_sys {
    void *ctx;
    /* the machine (System Monitor). 10/18/26: tasks copies the list in one go, the newest max when there are more;
       tasks and disks leave out as it was when they fail */
    int (*tasks)(void *ctx, struct gui_task *out, int max);
    int (*memory)(void *ctx, struct gui_memory *out);
    int (*machine)(void *ctx, struct gui_machine *out);
    /* end a task: 0, or -errno with the reason in msg (never the idle task, init, or the desktop's own). 10/18/26: nor
       the shells the desktop runs in; a task inside a call ends when the call does, however long (msg says so) */
    int (*kill)(void *ctx, int pid, char *msg, size_t n);
    /* the block devices (Disks); mounting and ejecting go through struct gui_fs's mount and eject */
    int (*disks)(void *ctx, struct gui_disk *out, int max);
    /* the kernel log (Log): its last len bytes as they are now (up to size), and how many bytes were ever written to
       it (so a reader sees what is new since it last looked) */
    int (*log)(void *ctx, char *buf, u32 size, u32 *len, u32 *written);
    /* a shell session on a pseudo-terminal (Terminal): open starts surfsh on a free one at that size and gives a
       handle >= 0 (-EAGAIN: none free); read returns what the session printed since, never waiting (0: nothing yet,
       -EPIPE: the session has ended and everything was read); write hands typed bytes to it (how many it took, never
       waiting); resize sets the terminal's size; close hangs up (its programs are killed) */
    int (*term_open)(void *ctx, int cols, int rows);
    int (*term_read)(void *ctx, int h, char *buf, u32 n);
    int (*term_write)(void *ctx, int h, const char *buf, u32 n);
    int (*term_resize)(void *ctx, int h, int cols, int rows);
    void (*term_close)(void *ctx, int h);
    /* 10/25/26: programs' windows (shell/guiclient.c; the kernel's side, shell/guisys_win.c on kernel/win.c). win_accept:
       the next window of this desktop that is ready to appear (its program presented, or 250 ms passed since it
       opened): a handle >= 0, -EAGAIN when none waits. win_update: the window's state and pixels as they are now (the
       pixels lent until the next update or win_close; 11/05/26: its menus too, in the view); -EPIPE once it has
       ended (its program closed it, or is gone):
       win_close it then. win_event: an event for its program (a full queue drops it, and counts it). win_close: this
       desktop is done with the window: its program's reads end and its presents fail. run (the launcher): the command
       line's program started on the desktop's console, its input /dev/null, detached (init collects it): its pid, or
       -errno with the reason in msg. 10/26/26: win_clip: the text a program's window put on the clipboard (WIN_CLIP,
       while it had the focus), the latest since the call before, taken: copied into out (at most n bytes) when it
       came from window h, the one with the desktop's focus now (-1: none), dropped otherwise; how many bytes, 0 for
       none */
    int (*win_accept)(void *ctx);
    int (*win_update)(void *ctx, int h, struct win_state *st, struct win_view *view);
    int (*win_event)(void *ctx, int h, const struct win_event *e);
    void (*win_close)(void *ctx, int h);
    int (*run)(void *ctx, const char *line, char *msg, size_t n);
    int (*win_clip)(void *ctx, int h, char *out, size_t n);
    /* 11/05/26 (track desk). win_paste: the desktop's clipboard (len bytes) given for WIN_PASTE: 0, -ENOMEM.
       win_dialog: the chooser window h asked for (WIN_DIALOG), taken into out, and its number into no: 0, -ENOENT
       when none waits. win_answer: the answer to request no of window h, an absolute path or NULL for cancelled
       (WIN_EV_DIALOG); -EINVAL for a request that is not the one waiting. win_theme: the desktop's theme and its
       WIN_COLORS colours, for its windows (WIN_THEME, WIN_EV_THEME). (A window's menus come with win_update.) */
    int (*win_paste)(void *ctx, const char *text, u32 len);
    int (*win_dialog)(void *ctx, int h, struct win_dialog *out, u32 *no);
    int (*win_answer)(void *ctx, int h, u32 no, const char *path);
    int (*win_theme)(void *ctx, int theme, const u32 *colors);
    /* run_file: the program of /initrd/bin named, with arg as its one argument (spaces and all), started as run
       starts a line: its pid, or -errno with the reason in msg. note: a line for the kernel log. prefs: the
       desktop's preferences in force (the settings on the stick, net/netcfg.c's desktop.theme and pointer.speed):
       the theme (0 ocean, 1 dusk) and the pointer's speed (1 slow, 2 normal, 3 fast); 0, or -errno. pref_set: one of
       them changed (which 0: the theme, 1: the speed), in force and on the stick: 0 saved, 1 in force but not saved
       (no settings stick, or it may not be written: msg says why), -1 refused (msg) */
    int (*run_file)(void *ctx, const char *program, const char *arg, char *msg, size_t n);
    void (*note)(void *ctx, const char *line);
    int (*prefs)(void *ctx, int *theme, int *speed);
    int (*pref_set)(void *ctx, int which, int value, char *msg, size_t n);
};

const struct gui_sys *gui_kernel_sys(void);     /* shell/guisys.c: the kernel's (the kernel only) */
#endif
