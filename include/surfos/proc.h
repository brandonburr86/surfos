/*
SurfOS processes, as programs see them
----------------------
File: proc.h    Date: 10/6/26
----------------------
SYS_procinfo (int index, struct procinfo *pi): task number index (0, 1, ...) of the task
             list; -ENOENT after the last. For a ps program, and for telnetd to find its
             own running server.
SYS_detach   (): the caller leaves its parent: init becomes its parent and collects it
             when it ends, and it is no longer part of the job of the shell that started
             it (Ctrl+C there does not reach it). It keeps its descriptors and its
             terminal. 0, or -EINVAL for a task that is not a user process.
11/01/26 (top): struct procinfo gains each task's processor time and memory, appended; a
             program and the kernel come from one build, so nothing old reads the shorter one.
SYS_sysinfo  (struct sysinfo *si): what the whole machine has: the clock those times are on and
             the memory totals memstat prints. 0, or -EFAULT.

This header uses no SurfOS types: user programs reach it through user/include/surfos/proc.h.
*/

#ifndef _SURFOS_PROC_H
#define _SURFOS_PROC_H

struct procinfo {
    int pid, ppid;                      /* ppid -1: none (the idle task, init) */
    int state;                          /* PROC_READY ... PROC_DEAD */
    unsigned int flags;                 /* PROC_USER, PROC_DETACHED, PROC_SHELL */
    int tty;                            /* 0-3: the console F1-F4; PROC_TTY_PTY + n: pty n; -1: none */
    char name[16];                      /* NUL-terminated */
    unsigned int cpu_ticks;             /* 11/01/26: timer ticks that found it running (sysinfo's hz of them a second) */
    unsigned int switches;              /* how many times it was given the processor */
    unsigned int mem_pages;             /* the frames it holds (page_size bytes each): a program's pages, its tables
                                           and its kernel stack, as memstat counts them; 0 for a kernel thread */
};

#define PROC_READY    0
#define PROC_RUNNING  1
#define PROC_BLOCKED  2
#define PROC_SLEEPING 3
#define PROC_DEAD     4
#define PROC_STOPPED  5                 /* 10/25/26: job control (surfos/job.h): Ctrl+Z, or a read from the background */

#define PROC_USER     0x1               /* a program in ring 3 */
#define PROC_DETACHED 0x2               /* init collects it */
#define PROC_SHELL    0x4               /* init starts it again when it ends */

#define PROC_TTY_PTY  16

/* 11/01/26 (top): the same numbers as memstat's, for a program */
struct sysinfo {
    unsigned int hz;                    /* timer ticks a second */
    unsigned int ticks;                 /* ticks since boot, the clock cpu_ticks counts on */
    unsigned int page_size;             /* bytes in a frame */
    unsigned int total_pages;           /* frames SurfOS manages */
    unsigned int free_pages;            /* of them, in use by nobody */
    unsigned int program_pages;         /* held by programs (procinfo's mem_pages, added up) */
    unsigned int dying_pages;           /* those of programs that ended, being freed */
    unsigned int heap_used;             /* bytes of the kernel heap in use */
};

#endif
