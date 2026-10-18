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
*/
#ifndef _SYS_GUI_SYS_H
#define _SYS_GUI_SYS_H
#include <surfos/types.h>

/* the time of day and the date, as the CMOS clock keeps them (shell/gui.c sets gui_state.time) */
struct gui_time { u32 year, month, date, weekday, hour, minute, second; };   /* weekday: 1 Sunday .. 7 Saturday */

/**** System Monitor ****/

#define GUI_TASK_MAX 64
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

/* memory as memstat counts it */
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
/* a block device as lsblk lists it, and, mounted, where and how full */
struct gui_disk {
    char name[16], disk[16];            /* usb1p1, and the disk it is on (usb1); a disk: its own name */
    char bus[8];                        /* "usb", "ata", "ram" */
    char type[32];                      /* "disk gpt", "part FAT32 LBA" */
    char label[48];                     /* the model, or the partition's label */
    char status[40];                    /* lsblk's STATUS: "mounted", "boot drive: protected", ... */
    char mount[256];                    /* where it is mounted, "" */
    u32 size_kb, used_kb, free_kb;      /* used/free: a mounted file system's, 0 otherwise */
    bool part, removable, readonly, gone, mounted;
};

struct gui_sys {
    void *ctx;
    /* the machine (System Monitor) */
    int (*tasks)(void *ctx, struct gui_task *out, int max);
    int (*memory)(void *ctx, struct gui_memory *out);
    int (*machine)(void *ctx, struct gui_machine *out);
    /* end a task: 0, or -errno with the reason in msg (never the idle task, init, or the desktop's own) */
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
};

const struct gui_sys *gui_kernel_sys(void);     /* shell/guisys.c: the kernel's (the kernel only) */
#endif
