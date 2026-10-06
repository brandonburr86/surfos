/*
SurfOS Multi-tasking Routine's
--------------------
File: task.c    Date: 6/10/04, rebuilt 10/2026 (roadmap S1)
--------------------
(C)2004 Brandon Burr

10/6/26 (surfsh): init starts the user shell on every console, and the ring-0 shell
when that cannot be (see init_main()).
*/

#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <mm/kalloc.h>
#include <surfos/interrupt.h>
#include <surfos/system.h>
#include <surfos/gdt.h>
#include <surfos/trap.h>
#include <surfos/irq.h>
#include <surfos/panic.h>
#include <surfos/timer.h>
#include <surfos/ktimer.h>
#include <surfos/wait.h>
#include <surfos/multiboot.h>
#include <surfos/tty.h>
#include <surfos/process.h>
#include <surfos/klog.h>
#include <fs/vfs.h>
#include <mm/uvm.h>

#include <blibc_common.h>

/*
    The SurfOS tasking model is one of elegance and stuff. It is based on a software task
    switching model with four run levels (see task.h). A task is always on exactly one
    list, or on none while it is the running task:

      run queues (TS_READY)   sleepers (TS_SLEEPING)   a wait queue (TS_BLOCKED)   zombies (TS_DEAD)

    The scheduler only ever runs in trap context (the timer tick, or int 0x40 from
    yield()); it saves the trap frame address in the outgoing task and returns the
    incoming task's frame, and trap_common resumes it. Nothing touches the stack of a
    task that may still be running on it: a dead task is freed by whoever waits for it,
    and init waits for everything nobody else does.
*/

extern surf_console conArray[];
extern u_char stack[]; /* the boot stack in boot.S, which becomes the idle task's */

surf_task *curTask, *idle_task, *init_task_ptr;

static struct task_list runq[NUM_PRIO];
static struct task_list sleepers;        /* sorted by wake_at */
static struct task_list zombies;
static wait_queue_t zombie_wq;           /* parents waiting in task_wait() */
static surf_task *all_tasks, *all_tasks_tail; /* in pid order */
static u_long next_pid;
static u_int ntasks;

static const u_int quantum_for[NUM_PRIO] = { 0, 4, 2, 1 };   /* FIFO: no slice */
static const prio_level pick_order[8] = { PL_FIFO, PL_HIGH, PL_NORMAL, PL_LOW, PL_FIFO, PL_HIGH, PL_NORMAL, PL_HIGH };
static u_int pick_pos;
static prio_level best_woken = NUM_PRIO; /* class of the best task made ready since the last switch */

/**** Lists ****/

void task_list_append(struct task_list *l, surf_task *t) {
    t->prev = l->last;
    t->next = NULL;
    if(l->last) l->last->next = t;
    else l->first = t;
    l->last = t;
    l->count++;
    t->list = l;
}

void task_list_remove(struct task_list *l, surf_task *t) {
    if(t->prev) t->prev->next = t->next;
    else l->first = t->next;
    if(t->next) t->next->prev = t->prev;
    else l->last = t->prev;
    l->count--;
    t->prev = t->next = NULL;
    t->list = NULL;
}

static void sleepers_insert(surf_task *t) {
    surf_task *p = sleepers.first;
    while(p && p->wake_at <= t->wake_at) p = p->next;
    if(!p) {
        task_list_append(&sleepers, t);
        return;
    }
    t->next = p;
    t->prev = p->prev;
    if(p->prev) p->prev->next = t;
    else sleepers.first = t;
    p->prev = t;
    sleepers.count++;
    t->list = &sleepers;
}

/* onto its run queue; interrupts off */
void task_make_ready(surf_task *t) {
    t->state = TS_READY;
    task_list_append(&runq[t->prio], t);
    if(t->prio < best_woken) best_woken = t->prio;
}

/**** Critical sections ****/

void kcritical_enter(void) {
    if(curTask) curTask->crit++;
}

void kcritical_leave(void) {
    if(curTask && curTask->crit) curTask->crit--;
}

/**** Priority names ****/

char *plNumToName(prio_level prio) {
    switch(prio) {
    case PL_FIFO: return "FIFO";
    case PL_HIGH: return "HIGH";
    case PL_NORMAL: return "NORMAL";
    case PL_LOW: return "LOW";
    }
    return "?";
}

const char *task_state_name(task_state s) {
    switch(s) {
    case TS_READY: return "ready";
    case TS_RUNNING: return "running";
    case TS_BLOCKED: return "blocked";
    case TS_SLEEPING: return "sleeping";
    case TS_DEAD: return "zombie";
    }
    return "?";
}

/**** Creation ****/

/* Build the first trap frame of a task at the top of its stack, so that "returning"
   from a trap into it starts task_stublet() with the right segments and flags. The frame
   has no useresp/ss for a ring-0 task because iret does not pop them within ring 0. */
static struct trapframe *build_initial_frame(u_char *stack_top, u_int ring, u_long eip) {
    u_int size = (ring == USER) ? sizeof(struct trapframe) : TRAPFRAME_KERNEL_SIZE;
    struct trapframe *tf = (struct trapframe*)(stack_top - size);
    memset(tf, 0, size);
    tf->eip = eip;
    if(ring == USER) {
        tf->cs = USER_CS;
        tf->ds = tf->es = tf->fs = tf->gs = tf->ss = USER_DS;
        tf->eflags = DEF_EFLAGS_3;
    } else {
        tf->cs = KERNEL_CS;
        tf->ds = tf->es = tf->fs = tf->gs = KERNEL_DS;
        tf->eflags = DEF_EFLAGS_0;
    }
    return tf;
}

/* Every task starts here: call its entry and exit when that returns, so a task function
   may simply return (the 2004 "task stublet"). */
static void task_stublet(void) {
    curTask->entry(curTask->arg);
    task_exit(0);
}

static surf_task *task_alloc(const char *name, prio_level prio, u_int flags, surf_console *con, u_int ring) {
    surf_task *t = (surf_task *)kcalloc(1, sizeof(surf_task));
    u_long irqf;
    if(!t) return NULL;
    t->stackmem = (u_char *)kalloc(KSTACK_SIZE);
    if(!t->stackmem) {
        kfree(t);
        return NULL;
    }
    /* Touch every page now: a kernel stack must never be demand paged, because the CPU
       pushes the page fault frame onto that very stack and faults again (double fault). */
    memset(t->stackmem, 0, KSTACK_SIZE);
    strlcpy(t->name, name, TASK_NAME_LEN);
    t->prio = prio;
    t->flags = flags | (ring == USER ? TF_USER : 0);
    t->con = con;
    t->tty = tty_for_console(con);
    strlcpy(t->cwd, (curTask && curTask->cwd[0]) ? curTask->cwd : "/", sizeof(t->cwd));
    t->stack_top = (u_long)t->stackmem + KSTACK_SIZE;
    t->esp = (u_long *)build_initial_frame(t->stackmem + KSTACK_SIZE, ring, (u_long)task_stublet);
    /* a detached task is init's from the start: nobody else may wait for it, and a shell running
       a background server must not see it as a child in task_wait(WAIT_ANY) */
    t->parent = ((flags & TF_DETACHED) && init_task_ptr) ? init_task_ptr : curTask;
    t->state = TS_READY;

    irqf = irq_save();
    t->pid = next_pid++;
    t->all_next = NULL;
    if(all_tasks_tail) all_tasks_tail->all_next = t;
    else all_tasks = t;
    all_tasks_tail = t;
    ntasks++;
    irq_restore(irqf);
    return t;
}

/* hand a fully set up task to the scheduler */
static void task_start(surf_task *t) {
    u_long irqf = irq_save();
    task_make_ready(t);
    irq_restore(irqf);
}

/* A kernel thread on a given console: it must not run before con and tty are set,
   which is why task_alloc() does not queue it. */
surf_task *kthread_create_on(const char *name, surf_console *con, kthread_fn fn, void *arg, prio_level prio, u_int flags) {
    surf_task *t = task_alloc(name, prio, flags | TF_KTHREAD, con ? con : &conArray[0], KERNEL);
    if(!t) return NULL;
    t->entry = fn;
    t->arg = arg;
    task_start(t);
    return t;
}

/* A kernel thread that prints where its creator prints */
surf_task *kthread_create(const char *name, kthread_fn fn, void *arg, prio_level prio, u_int flags) {
    surf_console *con = (curTask && curTask->con) ? curTask->con : (conActive ? conActive : &conArray[0]);
    return kthread_create_on(name, con, fn, arg, prio, flags);
}

/* The 2004 interface: a detached task whose body takes no argument */
surf_task *new_task(char *name, surf_console *con, u_int ring, prio_level prio, u_long *eip) {
    surf_task *t = task_alloc(name, prio, TF_DETACHED, con, ring);
    if(!t) return NULL;
    t->entry = (kthread_fn)eip;
    t->arg = NULL;
    task_start(t);
    return t;
}

surf_task *task_find(u_long pid) {
    surf_task *t;
    for(t = all_tasks; t; t = t->all_next) if(t->pid == pid) return t;
    return NULL;
}

u_int task_count(void) {
    return ntasks;
}

/**** Death ****/

/* interrupts off: hand t's children to init and tell waiters */
static void orphan_children(surf_task *t) {
    surf_task *c;
    for(c = all_tasks; c; c = c->all_next) {
        if(c->parent == t) c->parent = init_task_ptr;
    }
}

static void become_zombie(surf_task *t, int code) {
    if(t->list) task_list_remove(t->list, t); /* a run queue, the sleep list or a wait queue */
    t->exit_code = code;
    t->state = TS_DEAD;
    tty_task_gone(t);                         /* raw mode it set on a terminal ends with it */
    orphan_children(t);
    if(t->flags & TF_DETACHED) t->parent = init_task_ptr;
    task_list_append(&zombies, t);
    wake_up_all(&zombie_wq);
}

void task_exit(int code) {
    u_long flags = irq_save();
    if(curTask == idle_task) panic("the idle task tried to exit");
    become_zombie(curTask, code);
    irq_restore(flags);
    yield();
    panic("task_exit: '%s' was scheduled again", curTask->name);
}

/* Mark a task dead. If it is the current task the caller must yield (or, in a trap
   handler, return schedule(tf)); the task keeps its stack until someone waits for it. */
void kill_task(surf_task *t) {
    u_long flags;
    if(!t) return;
    if(t == idle_task) panic("FATAL: Kernel Idle Task Killed");
    flags = irq_save();
    if(t->state != TS_DEAD) become_zombie(t, -1);
    irq_restore(flags);
}

static void reap(surf_task *t) {
    surf_task **pp;
    u_long flags = irq_save();
    for(pp = &all_tasks; *pp; pp = &(*pp)->all_next) {
        if(*pp == t) { *pp = t->all_next; break; }
    }
    if(all_tasks_tail == t) { /* find the new tail */
        surf_task *x = all_tasks;
        all_tasks_tail = NULL;
        for(; x; x = x->all_next) all_tasks_tail = x;
    }
    ntasks--;
    irq_restore(flags);
    fd_close_all(t);                /* in the waiter's context: closing may write a file system */
    kcmd_task_gone(t);              /* 10/6/26: a kernel command that ended its task (crashgp from the user shell) */
    if(t->uvm) uvm_destroy(t->uvm);
    kfree(t->stackmem);             /* only now: kcmd_task_gone() reads that stack */
    kfree(t);
}

int task_wait_ex(u_long pid, int *status, u_int *flags_out, surf_console **con_out) {
    for(;;) {
        u_long flags = irq_save();
        surf_task *z, *found = NULL;
        bool have_children = false;

        for(z = zombies.first; z; z = z->next) {
            if(z->parent == curTask && (pid == WAIT_ANY || z->pid == pid)) { found = z; break; }
        }
        if(found) {
            u_long fpid = found->pid;
            task_list_remove(&zombies, found);
            irq_restore(flags);
            if(status) *status = found->exit_code;
            if(flags_out) *flags_out = found->flags;
            if(con_out) *con_out = found->con;
            reap(found);
            return (int)fpid;
        }
        for(z = all_tasks; z; z = z->all_next) {
            if(z->parent == curTask && (pid == WAIT_ANY || z->pid == pid)) { have_children = true; break; }
        }
        if(!have_children) {
            irq_restore(flags);
            return -1;
        }
        wait_prepare(&zombie_wq);
        irq_restore(flags);
        yield();
    }
}

int task_wait(u_long pid, int *status) {
    return task_wait_ex(pid, status, NULL, NULL);
}

/**** Blocking ****/

void yield(void) {
    asm volatile("int $0x40"); //  :)
}

void sleep_ms(u_long ms) {
    u_long flags;
    if(!curTask || curTask == idle_task || in_interrupt()) { /* nothing to switch to: spin */
        mdelay(ms);
        return;
    }
    flags = irq_save();
    curTask->wake_at = getticks() + ms_to_ticks(ms);
    curTask->state = TS_SLEEPING;
    sleepers_insert(curTask);
    irq_restore(flags);
    yield();
}

/**** The scheduler ****/

static surf_task *pick_next(void) {
    int i;
    for(i = 0; i < 8; i++) {
        prio_level p = pick_order[pick_pos];
        pick_pos = (pick_pos + 1) & 7;
        if(runq[p].first) {
            surf_task *t = runq[p].first;
            task_list_remove(&runq[p], t);
            return t;
        }
    }
    return idle_task;
}

/* Ahh... sexy. The infamous schedule(). This inputs the current task's trap frame and
   outputs the next task's. Trap context only. */
u_long *schedule(struct trapframe *tf) {
    surf_task *cur = curTask, *next;

    cur->esp = (u_long*)tf; //save the current ESP
    if(cur->state == TS_RUNNING) { /* preempted or yielded: back in line (idle is never queued) */
        cur->state = TS_READY;
        if(cur != idle_task) task_list_append(&runq[cur->prio], cur);
    }
    /* otherwise it already moved itself to a wait queue, the sleep list or the zombies */

    best_woken = NUM_PRIO;
    next = pick_next();
    next->state = TS_RUNNING;
    next->quantum = quantum_for[next->prio];
    if(next != cur) {
        next->switches++;
        curTask = next;
        tss_set_kernel_stack(next->stack_top);
        if(next->uvm != cur->uvm) uvm_switch(next->uvm);   /* another process: its page directory */
    }
    return next->esp;
}

/* Every tick, interrupts off. */
bool sched_tick(void) {
    u_long now = getticks();

    curTask->cpu_ticks++;
    while(sleepers.first && sleepers.first->wake_at <= now) {
        surf_task *t = sleepers.first;
        task_list_remove(&sleepers, t);
        task_make_ready(t);
    }

    if(curTask == idle_task) return best_woken < NUM_PRIO || runq[0].first || runq[1].first || runq[2].first || runq[3].first;
    if(curTask->crit) return false;             /* inside KCRIT_ENTER */
    if(curTask->prio == PL_FIFO) return false;  /* FIFO tasks are never preempted */
    if(curTask->quantum) curTask->quantum--;
    if(!curTask->quantum) return true;
    return best_woken < curTask->prio;          /* something better became runnable */
}

/* yield() arrives here through int 0x40: just reschedule */
static u_long *yield_trap(struct trapframe *tf) {
    return schedule(tf);
}

/**** init: the first task, parent of everything that has no parent ****/

/*
    10/6/26 (surfsh): every console (F1..F4) gets the user shell, /initrd/bin/surfsh -l, a
    process flagged TF_SHELL, started again whenever it ends. The kernel's ring-0 shell (Shell N)
    takes a console instead
      - on purpose: shell=kernel on the kernel command line, every console (its prompt ≋#>);
      - as a recovery console (≋?>), with a line saying why: when surfsh cannot be started at
        all (no initrd, no such file), or when it ended abnormally three times within 30
        seconds. Abnormal: killed (a fault, kill), not loadable (exit status 127), or gone
        within 2 seconds of its start, whatever its status (it could not get going).
    A console keeps the ring-0 shell until the next boot; that shell is started again when
    it dies (crashgp), as it always was.
*/
#define SURFSH_PATH       "/initrd/bin/surfsh"
#define SURFSH_LOAD_FAILED 127          /* process.c: the exit status of a program that could not be loaded */
#define SHELL_FAILS       3             /* abnormal ends of surfsh... */
#define SHELL_WINDOW_MS   30000         /* ...within this long: the recovery console */
#define SHELL_QUICK_MS    2000          /* a surfsh that ends sooner did not get going */

static struct console_shell {
    u_long pid;                         /* the shell there now */
    bool kernel;                        /* the ring-0 shell: shell=kernel, or a recovery console */
    char why[96];                       /* a recovery console: init's line, shown before its first prompt */
    u_long started;                     /* uptime_ms() when surfsh last started */
    u_long ends[SHELL_FAILS - 1];       /* uptime_ms() of its last abnormal ends, oldest first */
    u_int nends;
} cshell[NUM_CONSOLES];

/* a line on console i, and in the kernel log with the console's key */
static void init_say(int i, const char *line) {
    char logged[16];
    snprintf(logged, sizeof(logged), "F%i: ", i + 1);
    klog_write(logged);
    klog_write(line);
    kcprintf(&conArray[i], "%s", line);
}

static void start_kernel_shell(int i) {
    char name[TASK_NAME_LEN];
    surf_task *t;
    snprintf(name, sizeof(name), "Shell %i", i);
    t = kthread_create_on(name, &conArray[i], shell, cshell[i].why[0] ? cshell[i].why : NULL, PL_HIGH, TF_SHELL);
    cshell[i].pid = t ? t->pid : 0;
    if(!t) kprintf("init: no memory for a shell on F%i\n", i + 1);
}

/* the ring-0 shell from now on, with init's reason: the shell shows it, the kernel log keeps it */
static void recovery_console(int i) {
    char logged[16];
    cshell[i].kernel = true;
    snprintf(logged, sizeof(logged), "F%i: ", i + 1);
    klog_write(logged);
    klog_write(cshell[i].why);
    klog_write("\n");
    start_kernel_shell(i);
}

static void start_surfsh(int i) {
    char *argv[] = { "surfsh", "-l" };
    int pid = process_spawn(SURFSH_PATH, 2, argv, &conArray[i], TF_SHELL);
    if(pid < 0) {
        snprintf(cshell[i].why, sizeof(cshell[i].why), "init: %s: %s; recovery console", SURFSH_PATH, strerror(pid));
        recovery_console(i);
        return;
    }
    cshell[i].pid = (u_long)pid;
    cshell[i].started = uptime_ms();
}

static void start_shell(int i) {
    if(cshell[i].kernel) start_kernel_shell(i);
    else start_surfsh(i);
}

/* the shell of console i ended with status (-1: killed) */
static void shell_ended(int i, int status) {
    struct console_shell *c = &cshell[i];
    u_long now = uptime_ms();
    char how[40], line[80];
    if(c->kernel) {
        init_say(i, "init: restarted the shell\n");
        start_kernel_shell(i);
        return;
    }
    if(status == -1) strlcpy(how, "was killed", sizeof(how));
    else if(status == SURFSH_LOAD_FAILED) strlcpy(how, "could not be loaded", sizeof(how));
    else if(now - c->started < SHELL_QUICK_MS) snprintf(how, sizeof(how), "ended at once, status %i", status);
    else {                                                  /* it ran and was left: just a new one */
        start_surfsh(i);
        return;
    }
    if(c->nends == SHELL_FAILS - 1 && now - c->ends[0] <= SHELL_WINDOW_MS) {
        snprintf(c->why, sizeof(c->why), "init: surfsh failed %i times in %i s (%s); recovery console",
                 SHELL_FAILS, SHELL_WINDOW_MS / 1000, how);
        recovery_console(i);
        return;
    }
    if(c->nends == SHELL_FAILS - 1) {                       /* keep the last ones */
        memmove(c->ends, c->ends + 1, (SHELL_FAILS - 2) * sizeof(c->ends[0]));
        c->nends--;
    }
    c->ends[c->nends++] = now;
    snprintf(line, sizeof(line), "init: surfsh %s; restarted the shell\n", how);
    init_say(i, line);
    start_surfsh(i);
}

static void init_main(void *arg) {
    bool kernel = bootinfo_has_arg(&bootinfo, "shell=kernel");
    int i;
    for(i = 0; i < NUM_CONSOLES; i++) {                      /* F1..F4 */
        cshell[i].kernel = kernel;
        start_shell(i);
    }
    for(;;) {
        int status;
        u_int flags = 0;
        surf_console *con = NULL;
        int pid = task_wait_ex(WAIT_ANY, &status, &flags, &con);
        if(pid < 0) { /* no children at all: should not happen, but never spin */
            sleep_ms(100);
            continue;
        }
        if(!(flags & TF_SHELL) || !con) continue;           /* an orphan: collected, that is all */
        for(i = 0; i < NUM_CONSOLES; i++) {
            if(con == &conArray[i] && cshell[i].pid == (u_long)pid) shell_ended(i, status);
        }
    }
}

void init_task() {
    int i;
    for(i = 0; i < NUM_PRIO; i++) runq[i].first = runq[i].last = NULL, runq[i].count = 0;
    sleepers.first = sleepers.last = NULL; sleepers.count = 0;
    zombies.first = zombies.last = NULL; zombies.count = 0;
    wq_init(&zombie_wq);
    all_tasks = all_tasks_tail = NULL;
    next_pid = 0;
    ntasks = 0;

    trap_set_handler(T_YIELD, yield_trap); //interrupt for yield()

    /* The code running now (kmain) becomes the idle task: its registers are first saved
       by the first timer interrupt, so curTask MUST start out as the idle task. */
    idle_task = (surf_task *)kcalloc(1, sizeof(surf_task));
    if(!idle_task) panic("init_task: no memory for the idle task");
    strlcpy(idle_task->name, "Kernel Idle Task", TASK_NAME_LEN);
    idle_task->pid = next_pid++;
    idle_task->prio = PL_LOW;
    idle_task->state = TS_RUNNING;
    idle_task->con = &conArray[0];
    idle_task->tty = tty_for_console(&conArray[0]);
    idle_task->stack_top = (u_long)stack + 0x4000;
    idle_task->all_next = NULL;
    all_tasks = all_tasks_tail = idle_task;
    ntasks = 1;
    curTask = idle_task;

    init_task_ptr = kthread_create("init", init_main, NULL, PL_HIGH, TF_KTHREAD);
    if(!init_task_ptr) panic("init_task: no memory for init");
    init_task_ptr->parent = NULL;
}

/**** ps ****/

void print_tasks() {
    surf_task *t;
    u_long flags;
    printf("\nSurfOS Task List\n");
    printf("--------------------------\n");
    printf("  PID  STATE     PRIO    CPU(ticks)  SWITCHES  PARENT  NAME\n");
    flags = irq_save();
    for(t = all_tasks; t; t = t->all_next) {
        printf("%5lu  %-8s  %-6s  %10lu  %8lu  %6ld  %s%s\n", t->pid, task_state_name(t->state), plNumToName(t->prio),
               t->cpu_ticks, t->switches, t->parent ? (long)t->parent->pid : -1L, t->name,
               (t->flags & TF_USER) ? " (ring 3)" : "");
    }
    irq_restore(flags);
    printf("\n%u tasks\n", ntasks);
}
