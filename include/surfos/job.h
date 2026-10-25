/*
SurfOS jobs: how a child ended, job control
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
  status   the exit status for WX_EXITED, -1 for a kill; for WX_STOPPED why it stopped:
           WX_STOP_KEY (Ctrl+Z) or WX_STOP_READ (it read its terminal from the background)
  job      the child's job (below), 0 for none

pid -1: any child; pid < -1: any child of the job -pid. options WNOHANG, and WUNTRACED: a child
that stopped is said too (how WX_STOPPED), once for each time it stops. The child's pid; 0 with
WNOHANG while none has ended; -ECHILD when there is no such child, -EINTR when the caller was
killed meanwhile, or a Ctrl+C reached it and nothing else of its job (TTY_INTR 1: a shell
waiting for a job in the background), -EFAULT, -EINVAL (options).

10/25/26, job control. A job is the tasks of one command a shell runs (a pipeline's processes
and what they start); its id is the pid of its first task. SYS_spawnx's job starts one
(SPAWN_JOB_NEW), joins one (its id) or keeps the caller's (0: on the caller's terminal; a
pty's session is in none, and so is what a kernel command starts, run waiting for its end).
Each terminal has a foreground job, or none: the one that Ctrl+Z stops (in line mode; at
--More-- it is a key) and the only one whose tasks may read the terminal. A task of another
job that reads it stops instead (WX_STOP_READ) until its shell lets it go on in the
foreground. A stopped task waits where it was: in its own code, at the end of a system call,
or in a wait that a kill would cut short, which goes on afterwards (nothing is cut short,
nothing is lost, and it stops at the end of that call); waiting for a key, or in a sleep, it
is woken and waits there, off the queue of the terminal's readers (the keys are its shell's
meanwhile), and then reads or sleeps on. A shell that looks for Ctrl+C itself (TTY_INTR 1) is never stopped. When the shell
that made a job ends, the job's stopped tasks are killed (nothing could let them go on) and
the others leave it. SYS_detach leaves the job too.

SYS_job (int op, int arg), for the caller's terminal and the jobs the caller made:
  JOB_FG    arg: a job, or 0: the terminal's foreground job; the job's stopped tasks go on, in
            the foreground (Ctrl+C reaches them too). The previous foreground job.
  JOB_BG    arg: a job: its stopped tasks go on, in the background. How many tasks it has.
  JOB_KILL  arg: a job: every task of it killed, a stopped one too. How many.
  JOB_ID    the caller's job (0: none).
-EINVAL for an op or a job that is not the caller's, -EACCES for JOB_FG from a task of a job in
the background (but the shell that made the foreground job may give the terminal back to its
own), -ENOTTY without a terminal.

This header uses no SurfOS types: user programs reach it through user/include/surfos/job.h.
*/

#ifndef _SURFOS_JOB_H
#define _SURFOS_JOB_H

#define WX_EXITED       0
#define WX_KILLED       1
#define WX_INTERRUPTED  2
#define WX_STOPPED      3           /* 10/25/26: with WUNTRACED */

#define WX_STOP_KEY     0           /* WX_STOPPED's status: Ctrl+Z */
#define WX_STOP_READ    1           /* ... it read its terminal from the background */

#define WUNTRACED       4           /* SYS_waitx's options, with WNOHANG (1) */

#define JOB_FG          1           /* SYS_job's ops */
#define JOB_BG          2
#define JOB_KILL        3
#define JOB_ID          4

struct waitx {
    int pid;
    int how;                        /* WX_* */
    int status;                     /* WX_EXITED: the exit status; WX_STOPPED: WX_STOP_*; -1 a kill */
    int job;                        /* the child's job, 0 for none */
};

#endif
