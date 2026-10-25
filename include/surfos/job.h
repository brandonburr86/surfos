/*
SurfOS jobs: how a child ended
----------------------
File: job.h     Date: 10/25/26
----------------------
SYS_waitx (int pid, struct waitx *w, int options) waits for a child as SYS_wait does (pid -1:
any child of the caller; options WNOHANG: 0 at once while children are left but none has
ended) and says in w how it ended:

  pid      the child's
  how      WX_EXITED: it ended by itself, and status is its exit status; WX_KILLED: it was
           killed (kill, a fault, its terminal's hangup); WX_INTERRUPTED: Ctrl+C killed it, as
           a program of the job in the foreground of its terminal (tty.h, TTY_PAGE): a shell
           that waited for it leaves the rest of its line, loop or script
  status   the exit status for WX_EXITED, -1 otherwise (what SYS_wait gives for a kill)
  job      0 (job control comes later)

The child's pid; 0 with WNOHANG while none has ended; -ECHILD when there is no such child,
-EINTR when the caller was killed meanwhile, or a Ctrl+C reached it and nothing else of its
job (TTY_INTR 1: a shell waiting for a job in the background), -EFAULT, -EINVAL (options).

This header uses no SurfOS types: user programs reach it through user/include/surfos/job.h.
*/

#ifndef _SURFOS_JOB_H
#define _SURFOS_JOB_H

#define WX_EXITED       0
#define WX_KILLED       1
#define WX_INTERRUPTED  2

struct waitx {
    int pid;
    int how;                        /* WX_* */
    int status;                     /* WX_EXITED: the exit status; -1 otherwise */
    int job;                        /* 0 */
};

#endif
