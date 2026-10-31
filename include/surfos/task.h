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
#define TF_SYSTEM   0x80 /* 10/18/26 (audit B59): one of the kernel's own threads (a driver's, the network's): no kill
                            reaches it (kill_task()). Killed at once outside any call, it left its driver's lock held
                            (USB storage's in the hub thread's probe and poll, the SD card's in the card watch), and
                            every later request to that device waited for good, an umount's too */

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
    afterwards brings back the KILL_WAIT_MS rule.
    10/17/26 (audit B39): nor while the task is inside a file system or the block cache (fs_hold,
    counted by fs/fat.c in its calls and by fs/bcache.c around device requests and copies; and
    while it holds the cache's lock, bcache_holds()): killed at once there it left the cache's
    records of copies under way on its dead stack, blocks marked as copied, written or read for
    good, the blocks it held and the locks. Its long waits
    there are cut short like the others, so the call unwinds; when the kill that waited comes and
    the task is still inside (a device that takes its time), it comes as soon as the task leaves,
    and the kernel log says once that it waits. A kernel thread outside any call that is killed
    inside one is marked the same way and killed as soon as it leaves (10/17/26, audit B48: unless
    it has entered a call meanwhile, which then gets KILL_WAIT_MS as any call does: TI_NOCALL).
    10/17/26 (audit B46, B47): a device request counts as inside (driver/bdev.c: a driver's lock or
    transfer is never left half done), and so does a file system step that must finish once begun
    (fs_finish: umount, eject's unmount, fs/vfs.c), whose waits are not cut short either: cut short,
    an umount gave up changes the card would have taken and left a working card unclean.
    10/18/26 (audit B73): nor while its call holds references to a mounted volume's vnodes (fs_refs,
    fs/vfs.c: a path's walk, a file it opened and has not handed to its descriptor table): killed at
    once in a wait there (for the volume's lock) it left them behind, and the volume could not be
    unmounted until restart. A wait to read a terminal, a pseudo-terminal or a socket holds none.
    10/19/26 (audit B76): a kill put off while the task held something does not come at once the
    moment it has let go, which is where its own clean-up runs (a waiter's reap() freeing a dead
    task, a spawn handing its files over, a command freeing its buffers): from that moment the task
    has KILL_WAIT_MS to end its call (the kill comes at its end, task_call_leave()) or, a kernel
    thread outside any call, to end by itself; only one still running then is killed at once (as
    soon as it holds nothing, if it holds something again by then). (B79) The grace starts at the
    task's next VFS path call holding nothing, if no look found it out before (task_vfs_begin());
    and one still holding something when its grace has run out gets EINTR from every new path
    call from then on, so that it soon holds nothing and is killed.
    10/19/26 (audit B80, B81): the grace is owed to every task its kill finds holding something,
    at the kill itself (it was owed only to one a look had seen holding: a thread that let go
    before the first look, 20 ms after its kill, was killed at once there, and a call that let go
    just before its KILL_WAIT_MS look at that look), and to a task that begins a VFS call after
    its kill (it is to take something); and every VFS call begins so, a call on an open file too
    (read, write, lseek, readdir, ftruncate, fsync, close), statfs and sync: a task working on a
    file of its descriptor table reached no path call, and through a slow device it was found
    holding something at every look. A close is never refused: it lets go. What the rule
    guarantees, and what it does not: kernel/task.c, before kill_escalate().
*/
#define TC_SYSCALL  0x1  /* in_call: a system call (kernel/syscall.c), or a program being loaded */
#define TC_COMMAND  0x2  /* in_call: a kernel command (the ring-0 shell's line, SYS_kcmd's command) */
#define TC_EXIT     0x4  /* in_call: 10/6/26: the task is ending (task_exit()): a kill has nothing left to do */
#define TI_KILL     0x1  /* intr: killed; it ends at the end of its call */
#define TI_INT      0x2  /* intr: Ctrl+C reached it */
#define TI_HANGUP   0x4  /* 10/6/26, intr: with TI_KILL, its terminal hung up: no escalation (above) */
#define TI_FSWAIT   0x8  /* 10/17/26, intr: the kill that waited found it inside a file system (said once) */
#define TI_NOCALL   0x10 /* 10/17/26 (audit B48), intr: killed outside any call, put off inside a file system */
#define TI_HELD     0x20 /* 10/19/26 (audit B76), intr: the kill that waited found it holding something (in_fs()):
                            10/19/26 (B80): the kill itself, a look, or a VFS call it began since: a grace is owed */
#define TI_GRACE    0x40 /* 10/19/26 (audit B76), intr: ... and later found it let go: KILL_WAIT_MS to end (once) */
#define TI_LATE     0x80 /* 10/19/26 (audit B79), intr: ... and found it holding something again at the grace's end:
                            every new VFS call but a close says EINTR (task_vfs_begin(); B81: a call on an open file
                            too), so that it is soon out and killed */
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
    struct ktimer *wait_timer;  /* 10/6/26: wait_on_timeout()'s timer, on its stack: cancelled when it dies
                                   (10/17/26: fs/bcache.c's timed waits too) */
    u_int fs_hold;              /* 10/17/26 (audit B39): inside a file system or the block cache (above): the kill
                                   that waited comes once it is 0 (B47: or a device request) */
    u_int fs_finish;            /* 10/17/26 (audit B46): inside a file system step that must finish (above): its
                                   waits are not cut short */
    struct utf8_dec utf;        /* 10/6/26: a UTF-8 character it has half printed (kernel/console.c): its own,
                                   so other writers' bytes never mix with it, and it goes with the task */
    u_long kill_at;             /* 10/18/26 (audit B69): the tick its kill came (TI_KILL set): the kernel log says once,
                                   KILL_WAIT_MS after it, that a kill is put off (kill_escalate()) */
    u_int fs_refs;              /* 10/18/26 (audit B73): references to a mounted volume's vnodes its call holds, the
                                   files of one it opened and has not handed to its descriptor table and those it is
                                   closing (fs/vfs.c), and reap()'s of a dead task's: a kill that waited comes once it
                                   is 0 too, as for fs_hold (kernel/task.c). Not a fault's wipeout (B56): what a task
                                   killed there leaves keeps a volume mounted, and no lock held */
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
void kill_task(surf_task *t);      /* 10/6/26: inside a call the kill waits for its end (above), otherwise at once
                                      (10/17/26: once it is out of a file system) */
void kill_task_now(surf_task *t);  /* at once, wherever it is (a fault); the caller reschedules if it killed itself */
int task_kill_user(u_long pid, char *name);  /* 10/31/26 (review 14): SYS_kill's and the kill command's: 0, -EINVAL (no such
                                                task), -EACCES (idle, init, TF_SYSTEM); name: TASK_NAME_LEN bytes or NULL */
void task_call_enter(u_int what);  /* 10/6/26: a call (TC_*) begins in the current task */
int task_call_leave(u_int what);   /* it ends: a kill that waited comes now (no return); 1 when Ctrl+C cut it short */
int task_interrupt_job(surf_task *owner); /* 10/6/26: Ctrl+C for owner's job (tty.c), interrupts off: 1 if it reached any */
#define task_interrupted() (curTask && curTask->intr)   /* 10/6/26: is the current task's call to end? */
void task_interrupt_absorbed(void); /* 10/18/26 (audit B54): Ctrl+C reached a kernel command that finished all the same
                                       (umount, eject): its status is its own, not 130 (task_call_leave()) */
/* 10/17/26 (audit B39, B46): may the current task's waits in a file system be cut short: its call is to end (a kill,
   Ctrl+C in a kernel command), and it is neither ending by itself (TC_EXIT: its files close and are written out) nor
   in a step that must finish (fs_finish) */
#define fs_cut_short() (curTask && curTask->intr && !(curTask->in_call & TC_EXIT) && !curTask->fs_finish)
/* 10/17/26 (audit B39, B46, B47): the current task inside a file system, the block cache or a device request (a kill
   that waited comes once it is out: kernel/task.c), and inside a step that must finish (its waits not cut short too) */
static inline void fs_enter(void) { if(curTask) curTask->fs_hold++; }
static inline void fs_leave(void) { if(curTask && curTask->fs_hold) curTask->fs_hold--; }
static inline void fs_finish_enter(void) { if(curTask) { curTask->fs_hold++; curTask->fs_finish++; } }
static inline void fs_finish_leave(void) {
    if(!curTask) return;
    if(curTask->fs_finish) curTask->fs_finish--;
    if(curTask->fs_hold) curTask->fs_hold--;
}
/* 10/18/26 (audit B73): a reference to a mounted volume's vnode (or file) the current task's call holds, and its end */
static inline void fs_ref(void) { if(curTask) curTask->fs_refs++; }
static inline void fs_unref(void) { if(curTask && curTask->fs_refs) curTask->fs_refs--; }
/* 10/19/26 (audit B76, B79): a VFS path call begins in the current task (fs/vfs.c's lookup_abs()). A kill of it that was
   put off and finds it holding nothing here gives it its KILL_WAIT_MS from here (B76), as a look finding it out does; and
   a task still holding something when that ran out gets -EINTR, nothing taken (B79). 0 to go on. 10/19/26 (audit B80,
   B81): every VFS call begins here (a call on an open file, statfs and sync too; a close takes no refusal), and a killed
   task that begins one is marked as one that holds something (TI_HELD): it is to take something */
int task_vfs_begin_slow(void);
static inline int task_vfs_begin(void) {
    return curTask && (curTask->intr & TI_KILL) ? task_vfs_begin_slow() : 0;
}
/* 10/18/26 (audit B56): is t inside a file system, the block cache or a device request (counted, or holding the cache's
   lock or its bounce buffer's): a fault there stops the system (kernel/panic.c, arch/riscv32/trap.c), since what t
   holds there (a volume's lock, the cache's, a driver's, blocks under a device's I/O) cannot be given back. (B62) Or
   holding a mutex: no kill at once comes then either (kernel/task.c) */
bool task_in_fs(const surf_task *t);
/* 10/18/26 (audit B62): which of the two: TASK_HOLDS_FS, TASK_HOLDS_LOCK (a mutex, kernel/sync.c, counted in
   surf_task.mutexes with the console's lock, which does not count here: it is given back when its holder dies,
   console_task_gone()), or 0 */
#define TASK_HOLDS_FS   1
#define TASK_HOLDS_LOCK 2
int task_holds(const surf_task *t);
surf_task *task_find(u_long pid);
u_int task_count(void);
u_int task_count_tty(struct tty *tty);   /* 10/6/26: the live tasks whose terminal it is (kernel/pty.c) */
u_int task_kill_tty(struct tty *tty);    /* 10/6/26: kill_task() for each of them (a hangup); how many */
struct procinfo;
int task_info(int index, struct procinfo *pi);   /* 10/6/26: SYS_procinfo's task number index (surfos/proc.h) */
int task_detach(surf_task *t);           /* 10/6/26: SYS_detach: init's child, out of its shell's job */

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
