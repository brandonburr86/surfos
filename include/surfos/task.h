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
#define KSTACK_SIZE 0x4000   /* 16 KB kernel stack per task */
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
    and its status is KCMD_INTERRUPTED (130, surfos/kcmd.h).
*/
#define TC_SYSCALL  0x1  /* in_call: a system call (kernel/syscall.c), or a program being loaded */
#define TC_COMMAND  0x2  /* in_call: a kernel command (the ring-0 shell's line, SYS_kcmd's command) */
#define TI_KILL     0x1  /* intr: killed; it ends at the end of its call */
#define TI_INT      0x2  /* intr: Ctrl+C reached it */
#define KILL_WAIT_MS 2000

typedef enum {
    TS_READY = 0,   /* on a run queue */
    TS_RUNNING,     /* curTask */
    TS_BLOCKED,     /* on a wait queue */
    TS_SLEEPING,    /* on the sleep list until wake_at */
    TS_DEAD         /* on the zombie list until a parent (or init) collects it */
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
int task_wait(u_long pid, int *status);                            /* returns the pid, or -1 with nothing to wait for */
int task_wait_opts(u_long pid, int *status, int options);   /* WNOHANG: 0 while children are left running */
u_int task_child_count(void);
int task_wait_ex(u_long pid, int *status, u_int *flags, surf_console **con);
void kill_task(surf_task *t);      /* 10/6/26: inside a call the kill waits for its end (above), otherwise at once */
void kill_task_now(surf_task *t);  /* at once, wherever it is (a fault); the caller reschedules if it killed itself */
void task_call_enter(u_int what);  /* 10/6/26: a call (TC_*) begins in the current task */
int task_call_leave(u_int what);   /* it ends: a kill that waited comes now (no return); 1 when Ctrl+C cut it short */
int task_interrupt_job(surf_task *owner); /* 10/6/26: Ctrl+C for owner's job (tty.c), interrupts off: 1 if it reached any */
#define task_interrupted() (curTask && curTask->intr)   /* 10/6/26: is the current task's call to end? */
surf_task *task_find(u_long pid);
u_int task_count(void);

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
