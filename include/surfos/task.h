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

#define DEF_EFLAGS_0 0x202   /* IF, plus bit 1 which is always set */
#define DEF_EFLAGS_3 0x3202  /* the same with IOPL 3 */
#define KSTACK_SIZE 0x4000   /* 16 KB kernel stack per task */
#define TASK_NAME_LEN 32

#define KERNEL 0
#define USER 3

typedef void (*task_stub)(void);
typedef void (*kthread_fn)(void *arg);

/* task flags */
#define TF_SHELL    0x1  /* init starts a new shell on its console when this one dies */
#define TF_DETACHED 0x2  /* nobody waits for it: init reaps it when it exits */
#define TF_KTHREAD  0x4
#define TF_USER     0x8  /* runs in ring 3 (roadmap P1) */

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
int task_wait(u_long pid, int *status);                            /* returns the pid, or -1 with nothing to wait for */
int task_wait_ex(u_long pid, int *status, u_int *flags, surf_console **con);
void kill_task(surf_task *t);                                      /* mark dead; the caller yields if it killed itself */
surf_task *task_find(u_long pid);
u_int task_count(void);

void yield(void);
void sleep_ms(u_long ms);

/* scheduler entry points, trap context only */
u_long *schedule(struct trapframe *tf);
bool sched_tick(void);           /* from the timer: wake sleepers, account the slice; true = reschedule */

void print_tasks(void);
char *plNumToName(prio_level prio);
const char *task_state_name(task_state s);
void shell(void);                /* the ring-0 shell task body (shell/main.c) */

#endif
