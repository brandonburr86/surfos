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
};

#define PROC_READY    0
#define PROC_RUNNING  1
#define PROC_BLOCKED  2
#define PROC_SLEEPING 3
#define PROC_DEAD     4

#define PROC_USER     0x1               /* a program in ring 3 */
#define PROC_DETACHED 0x2               /* init collects it */
#define PROC_SHELL    0x4               /* init starts it again when it ends */

#define PROC_TTY_PTY  16

#endif
