/*
SurfOS Multitasking Header
----------------------
File: task.h    Date: 6/10/04, rebuilt 10/2026 (roadmap S1)
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_TASK_H
#define _SURFOS_TASK_H

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/trap.h>
#include <surfos/ktimer.h>

#define DEF_EFLAGS_0 0x202   /* IF, plus bit 1 which is always set */
#define DEF_EFLAGS_3 0x3202  /* the same with IOPL 3 */
#define KSTACK_SIZE 0x4000   /* 16 KB kernel stack per task (10/25/26: over a guard, kernel/task.c; so is the boot stack's size) */
#define KSTACK_LOW  0x400    /* 10/25/26 (L18): a trap frame this close to its stack's bottom halts the machine */
#define TASK_NAME_LEN 32
#define TASK_CWD_LEN 256
#define NR_OPEN 16
struct file;
struct uvm;

#define KERNEL 0
#define USER 3

typedef void (*task_stub)(void);
typedef void (*kthread_fn)(void *arg);

/* task flags */
#define TF_SHELL    0x1  /* init starts a new shell on its console when this one dies */
#define TF_DETACHED 0x2  /* nobody waits for it: init reaps it when it exits */
#define TF_KTHREAD  0x4
#define TF_USER     0x8  /* runs in ring 3 (roadmap P1) */
#define TF_LOADED   0x10 /* 10/6/26: a process whose program was loaded and started (process.c) */
#define TF_FOREGROUND 0x20 /* 10/6/26: a process started by a task its console's pager covers (TTY_PAGE):
                              a program of the job in the foreground, which Ctrl+C reaches (tty.c) */
#define TF_INCALL   0x40 /* 10/6/26, for kthread_create_on() only: the task starts inside a call (TC_SYSCALL
                            below), as a process does until its program is loaded (process.c) */
#define TF_INTR     0x80 /* 10/25/26: a shell that looks for Ctrl+C itself while it owns the pager (tty.h, TTY_INTR) */
#define TF_PROCESS  0x100 /* 10/25/26 (L15): a process, from its creation (process_spawn_ex()): a process carries
                             TF_KTHREAD too, and TF_USER only once its loading starts */
#define TF_LOADNOMEM 0x200 /* 10/25/26 (the review of B1): a process whose loading failed for want of memory, which init
                              tries again for a console's shell instead of giving the console up (task.c) */
#define TF_SESSION  0x400 /* 10/26/26 (CODE-AUDIT T14): the program a pseudo-terminal's session began with (kernel/pty.c),
                             from its creation until a detach(): a shell of that terminal, as init's TF_SHELL is of a
                             console (sys_job) */

/*
    10/6/26: a kill that waits for the end of a call. A task inside a system call or a kernel
    command (in_call) holds what the call holds: files, memory, a pager, a lock. kill_task()
    then only marks it (TI_KILL) and cuts short the wait it is in, if that wait may be cut
    short (wait_prepare_intr(), sleep_intr(): the terminal, the pager, sleeps, the network,
    the wait for a child); the call gives up along its own error paths, lets go of what it
    holds, and the task ends where the call does (task_call_leave(): task_exit(-1)). A call
    that has not ended KILL_WAIT_MS later is killed at once, as every kill was before, and
    the kernel log says so. Ctrl+C on a terminal (tty.c) marks a kernel command run by the
    pager's owner TI_INT instead: its waits are cut short the same way, its task lives on,
    and its status is KCMD_INTERRUPTED (130, surfos/kcmd.h). 10/6/26: the kill of a pseudo-
    terminal's hangup (kernel/pty.c) is marked TI_HANGUP too: it waits for the end of the call
    however long that takes, never killing at once what is inside one (a client going away
    must not leave a lock held); the kernel log says once that it waits. An explicit kill_task()
    afterwards brings back the KILL_WAIT_MS rule. 10/18/26: so is the kill of the desktop's End
    task (kill_task_patient(), TI_PATIENT), whose waiting the kernel log says without speaking of
    a terminal.
*/
#define TC_SYSCALL  0x1  /* in_call: a system call (kernel/syscall.c), or a program being loaded */
#define TC_COMMAND  0x2  /* in_call: a kernel command (the ring-0 shell's line, SYS_kcmd's command) */
#define TC_EXIT     0x4  /* in_call: 10/6/26: the task is ending (task_exit()): a kill has nothing left to do */
#define TI_KILL     0x1  /* intr: killed; it ends at the end of its call */
#define TI_INT      0x2  /* intr: Ctrl+C reached it */
#define TI_HANGUP   0x4  /* 10/6/26, intr: with TI_KILL, its terminal hung up: no escalation (above) */
#define TI_PATIENT  0x8  /* 10/18/26, intr: with TI_KILL, End task's kill (kill_task_patient()): no escalation */
#define KILL_WAIT_MS 2000

typedef enum {
    TS_READY = 0,   /* on a run queue */
    TS_RUNNING,     /* curTask */
    TS_BLOCKED,     /* on a wait queue */
    TS_SLEEPING,    /* on the sleep list until wake_at */
    TS_DEAD,        /* on the zombie list until a parent (or init) collects it */
    TS_STOPPED      /* 10/25/26: stopped (job control, surfos/job.h): on no list until it goes on */
} task_state;

/*
    The SurfOS scheduling classes, unchanged since 2004: FIFO tasks run until they
    block or finish; HIGH, NORMAL and LOW get slices of 4, 2 and 1 ticks and are
    picked in the order F H N L, F H N H, so HIGH sees about twice the turns of LOW.
*/
typedef enum {
    PL_FIFO = 0,
    PL_HIGH = 1,
    PL_NORMAL = 2,
    PL_LOW = 3
} prio_level;
#define NUM_PRIO 4

struct surf_task;
struct tty;

/* a task is on at most one of these: a run queue, the sleep list, a wait queue or the zombies */
struct task_list {
    struct surf_task *first, *last;
    u_int count;
};

typedef struct surf_task {
    char name[TASK_NAME_LEN];
    u_long pid;
    prio_level prio;
    task_state state;
    u_int flags;
    surf_console *con;
    struct tty *tty;            /* where getch()/gets() read from */
    char cwd[TASK_CWD_LEN];     /* current directory, absolute and normalized ("/" when empty) */
    struct file *files[NR_OPEN]; /* open file descriptors (fs/vfs.c) */
    struct uvm *uvm;            /* user address space (mm/uvm.c), NULL for kernel threads */
    void *fpu;                  /* 10/25/26 (L10): a process's floating-point registers while the FPU holds another
                                   task's (kernel/fpu.c): FPU_AREA bytes, 16-aligned; NULL for a kernel thread */

    u_long *esp;                /* saved trap frame while not running */
    u_char *stackmem;           /* kernel stack (NULL for the idle task: it uses the boot stack) */
    u_long stack_top;

    struct surf_task *parent;
    int exit_code;
    u_long wake_at;             /* TS_SLEEPING: tick count to wake at */
    bool wait_timed_out;        /* set by wait_on_timeout() */

    struct task_list *list;     /* the list this task is on, NULL while running */
    struct surf_task *prev, *next;
    struct surf_task *all_next; /* the list of every task, for ps and task_find() */

    u_int crit;                 /* KCRIT_ENTER depth: the tick does not preempt while > 0 */
    u_int quantum;              /* ticks left in this slice */
    u_long cpu_ticks, switches;

    kthread_fn entry;
    void *arg;

    struct kcapture *kcap;      /* 10/6/26: what this task prints to its console is kept here instead
                                   (a kernel command redirected to a file, kernel/console.c) */
    u_int mutexes;              /* 10/6/26: mutexes it holds (kernel/sync.c); the pager waits for a key only at 0 */
    u_char in_call;             /* 10/6/26: TC_*: inside a call, where a kill waits for its end */
    u_char intr;                /* 10/6/26: TI_*: why its waits are cut short (task_interrupted()) */
    u_char wait_intr;           /* 10/6/26: the wait it is in may be cut short (wait_prepare_intr(), sleep_intr()) */
    struct ktimer kill_timer;   /* 10/6/26: the kill that waited comes at once when this fires */
    struct ktimer *wait_timer;  /* 10/6/26: wait_on_timeout()'s timer, on its stack: cancelled when it dies */
    struct utf8_dec utf;        /* 10/6/26: a UTF-8 character it has half printed (kernel/console.c): its own,
                                   so other writers' bytes never mix with it, and it goes with the task */
    u_char end_how;             /* 10/25/26: how it ended, for SYS_waitx: WX_* (surfos/job.h) */
    u_short ctrl_c;             /* 10/25/26: the Ctrl+Cs that reached it since it last asked (TTY_INTR) */
    u_long job;                 /* 10/25/26: its job (surfos/job.h), 0 for none; what it starts is in it too */
    u_long job_shell;           /* the pid of the shell that made the job */
    u_char stop_req;            /* 10/25/26: it is to stop (Ctrl+Z, a read from the background) */
    u_char stopped;             /* it counts as stopped (SYS_waitx, ps): parked, or in a wait that goes on */
    u_char stop_seen;           /* its parent was told of this stop (WUNTRACED) */
    u_char stop_why;            /* WX_STOP_KEY, WX_STOP_READ */
} surf_task;

extern surf_task *curTask;
extern surf_task *idle_task;
extern surf_task *init_task_ptr;

void init_task(void);

/* kernel critical sections: the scheduler will not preempt the task while it holds one */
#define KCRIT_ENTER kcritical_enter();
#define KCRIT_LEAVE kcritical_leave();
void kcritical_enter(void);
void kcritical_leave(void);

/* lists (interrupts must be off) */
void task_list_append(struct task_list *l, surf_task *t);
void task_list_remove(struct task_list *l, surf_task *t);
void task_make_ready(surf_task *t);

/* creation and lifetime */
surf_task *kthread_create(const char *name, kthread_fn fn, void *arg, prio_level prio, u_int flags);
surf_task *kthread_create_on(const char *name, surf_console *con, kthread_fn fn, void *arg, prio_level prio, u_int flags);
surf_task *new_task(char *name, surf_console *con, u_int ring, prio_level prio, u_long *eip); /* 2004 API: a detached task */
void task_exit(int code) __attribute__((noreturn));
#define WAIT_ANY ((u_long)-1)
#define WNOHANG  1              /* 10/6/26, task_wait_opts() and SYS_wait: 0 at once when no child has ended */
#define WINTR    2              /* 10/6/26, task_wait_opts(): a kill of the caller ends the wait (-1) */
#define WCTRLC   8              /* 10/25/26, with WINTR: so does a Ctrl+C that reached the caller itself (TTY_INTR) */
/* 10/25/26: WUNTRACED (4, surfos/job.h): a child that stopped too, how WX_STOPPED */
int task_wait(u_long pid, int *status);                            /* returns the pid, or -1 with nothing to wait for */
int task_wait_opts(u_long pid, int *status, int options);   /* WNOHANG: 0 while children are left running */
u_int task_child_count(void);
int task_wait_ex(u_long pid, int *status, u_int *flags, surf_console **con);
int task_wait_how(u_long pid, u_long job, int *status, int *how, int *job_out, int options);  /* 10/25/26: how: WX_*; job: any child of it */
/* 10/25/26: job control (surfos/job.h); interrupts off for the first */
struct tty;
int task_stop_job(struct tty *tty, u_long job);         /* Ctrl+Z: the job's tasks stop; how many */
int task_stop_reader(struct tty *tty);                  /* a read from the background stops first: 0, -EINTR */
int task_job_go_on(struct tty *tty, u_long job, int foreground, u_long shell);   /* how many tasks */
int task_job_kill(struct tty *tty, u_long job, u_long shell);                    /* how many */
int task_job_made(u_long job, u_long shell);            /* a task of that job (collected or not), made by that shell? */
void task_now_stopped(surf_task *t);                    /* it counts as stopped (WUNTRACED); interrupts off */
int kthread_create_job(const char *name, surf_console *con, kthread_fn fn, void *arg, prio_level prio,
                       u_int flags, int job);           /* kthread_create_on() in a job (SPAWN_JOB_* or one): its pid */
void kill_task(surf_task *t);      /* 10/6/26: inside a call the kill waits for its end (above), otherwise at once */
void kill_task_now(surf_task *t);  /* at once, wherever it is (a fault); the caller reschedules if it killed itself */
int kill_task_patient(surf_task *t); /* 10/18/26: as a hangup's kill: inside a call it waits for its end, however long
                                        (above); 1 when it does, 0 when the task ended at once or had ended */
/* 10/25/26 (L15): may a user's kill (SYS_kill, the kill command, End task) end t? A process, or a ring-0 shell (TF_SHELL:
   init starts it again, and its commands are calls that a kill waits for). Never the idle task, init or a kernel
   thread: in no call, a kernel thread is killed on the spot, holding whatever locks it holds. A thread that users need
   to stop has a command of its own for it. */
static inline bool task_user_may_kill(const surf_task *t) {
    return t && t != idle_task && t != init_task_ptr && (t->flags & (TF_PROCESS | TF_SHELL)) != 0;
}
int task_kill_user(u_long pid, char *name);  /* 10/25/26 (L15): SYS_kill's and the kill command's: 0, -EINVAL (no such task)
                                                or -EPERM (above); name: its name (TASK_NAME_LEN) when found, or NULL */
void task_call_enter(u_int what);  /* 10/6/26: a call (TC_*) begins in the current task */
int task_call_leave(u_int what);   /* it ends: a kill that waited comes now (no return); 1 when Ctrl+C cut it short */
int task_interrupt_job(surf_task *owner); /* 10/6/26: Ctrl+C for owner's job (tty.c), interrupts off: 1 if it reached any */
#define task_interrupted() (curTask && curTask->intr)   /* 10/6/26: is the current task's call to end? */
surf_task *task_find(u_long pid);
u_int task_count(void);
u_int task_count_tty(struct tty *tty);   /* 10/6/26: the live tasks whose terminal it is (kernel/pty.c) */
u_int task_kill_tty(struct tty *tty);    /* 10/6/26: kill_task() for each of them (a hangup); how many */
struct procinfo;
int task_info(int index, struct procinfo *pi);   /* 10/6/26: SYS_procinfo's task number index (surfos/proc.h) */
int task_info_all(struct procinfo *out, int max); /* 10/18/26: the list in one go, the newest max; how many */
int task_detach(surf_task *t);           /* 10/6/26: SYS_detach: init's child, out of its shell's job */
void task_mem_held(u_long *programs, u_long *dying);   /* 10/25/26 (L9): frames programs hold, and those of ended ones
                                                          not freed yet: uvmreap's, and any being freed (memstat) */
/* 10/25/26 (L17): the task whose kernel stack's slot holds addr, and whether addr is in the guard below that stack
   (the double fault's report) */
surf_task *task_stack_owner(u_long addr, bool *guard);
/* 10/25/26 (L22, L23): shutdown (kernel/shutdown.c): the kill that waits for the end of the call (kill_task_patient()) for
   every process and ring-0 shell but the caller, how many; those still alive, how many, and the first of them in list as
   'name' (pid n), up to len bytes; init woken (Ctrl+Alt+Del, from the keyboard's interrupt) */
u_int task_end_users(void);
u_int task_users_left(char *list, size_t len);
void task_wake_init(void);
#define task_stack_bottom(t) ((t)->stack_top - KSTACK_SIZE)     /* every task's, the idle task's boot stack too */

void yield(void);
void sleep_ms(u_long ms);
int sleep_intr(u_long ms);       /* 10/6/26: sleep_ms() that a kill or Ctrl+C cuts short: 0, or -EINTR then */

/* scheduler entry points, trap context only */
u_long *schedule(struct trapframe *tf);
bool sched_tick(void);           /* from the timer: wake sleepers, account the slice; true = reschedule */

void print_tasks(void);
char *plNumToName(prio_level prio);
const char *task_state_name(task_state s);
/* the ring-0 shell task body (shell/main.c). 10/6/26: arg is NULL when it was asked for (shell=kernel),
   otherwise init's one-line reason for a recovery console (kernel/task.c) */
void shell(void *arg);
/* 10/6/26: its commands for the user shell (SYS_kcmd, SYS_kcmdinfo: surfos/kcmd.h) */
struct kcmd_info;
int shell_run_command(int argc, char **argv);             /* in the calling task: the status, or -ENOENT */
int shell_command_info(int index, struct kcmd_info *out); /* entry index of the table; -ENOENT after the last */

#endif
