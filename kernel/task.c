/*
SurfOS Multi-tasking Routine's
--------------------
File: task.c    Date: 6/10/04, rebuilt 10/2026 (roadmap S1)
--------------------
(C)2004 Brandon Burr

10/6/26 (surfsh): init starts the user shell on every console, and the ring-0 shell
when that cannot be (see init_main()).
10/6/26: a kill waits for the end of the call its task is in (task.h), and Ctrl+C
reaches the job in the foreground (task_interrupt_job()).
10/6/26: the tasks on a terminal, counted and killed, for the pseudo-terminals (kernel/pty.c).
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
#include <sys/framebuffer.h>
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

static void kill_escalate(void *arg);

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
    t->flags = (flags & ~TF_INCALL) | (ring == USER ? TF_USER : 0);
    t->in_call = (flags & TF_INCALL) ? TC_SYSCALL : 0;     /* 10/6/26: a kill waits for the call to end */
    t->con = con;
    t->tty = tty_for_console(con);
    strlcpy(t->cwd, (curTask && curTask->cwd[0]) ? curTask->cwd : "/", sizeof(t->cwd));
    t->stack_top = (u_long)t->stackmem + KSTACK_SIZE;
    t->esp = (u_long *)build_initial_frame(t->stackmem + KSTACK_SIZE, ring, (u_long)task_stublet);
    ktimer_init(&t->kill_timer, kill_escalate, t);
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
    /* 10/6/26: a process started on a pseudo-terminal that has hung up (kernel/pty.c) gets the kill
       that waits, as the tasks there did: it ends once it is loaded */
    if((flags & TF_INCALL) && t->tty && t->tty->hungup) {
        t->intr |= TI_KILL;
        ktimer_start(&t->kill_timer, KILL_WAIT_MS, false);
    }
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
    ktimer_cancel(&t->kill_timer);            /* 10/6/26: a kill that waited has come */
    if(t->wait_timer) {                       /* killed at once in wait_on_timeout(): its timer is on t's stack */
        ktimer_cancel(t->wait_timer);
        t->wait_timer = NULL;
    }
    if(t->list) task_list_remove(t->list, t); /* a run queue, the sleep list or a wait queue */
    t->exit_code = code;
    t->state = TS_DEAD;
    tty_task_gone(t);                         /* raw mode it set on a terminal ends with it */
    framebuffer_task_died(t);                 /* 10/6/26: killed from an interrupt while it drew, it gives the screen back */
    orphan_children(t);
    if(t->flags & TF_DETACHED) t->parent = init_task_ptr;
    task_list_append(&zombies, t);
    wake_up_all(&zombie_wq);
}

void task_exit(int code) {
    u_long flags;
    if(curTask == idle_task) panic("the idle task tried to exit");
    /* 10/6/26: its files close now, in its own context, not when someone waits for it: a
       background program's file is written out and its volume can be unmounted at once */
    fd_close_all(curTask);
    flags = irq_save();
    become_zombie(curTask, code);
    irq_restore(flags);
    yield();
    panic("task_exit: '%s' was scheduled again", curTask->name);
}

/* Mark a task dead. If it is the current task the caller must yield (or, in a trap
   handler, return schedule(tf)); the task keeps its stack until someone waits for it.
   10/6/26: at once; a fault (kernel/panic.c) and a kill that waited too long come here. */
void kill_task_now(surf_task *t) {
    u_long flags;
    if(!t) return;
    if(t == idle_task) panic("FATAL: Kernel Idle Task Killed");
    flags = irq_save();
    if(t->state != TS_DEAD) become_zombie(t, -1);
    irq_restore(flags);
}

/* 10/6/26: t goes on now if the wait it is in may be cut short; interrupts off */
static void wake_intr(surf_task *t) {
    if(!t->wait_intr || !t->list) return;
    if(t->state == TS_BLOCKED || t->state == TS_SLEEPING) {
        task_list_remove(t->list, t);
        task_make_ready(t);
    }
}

/* 10/6/26: the kill that waits (task.h). Outside a call (a program running its own code, a
   kernel thread) it comes at once, as it always did; inside one the task is marked, its
   wait cut short where that may be, and the call ends it (task_call_leave()). Also from
   an interrupt handler (Ctrl+C): a current task killed at once is switched away from
   before the handler returns (irq_dispatch(), sched_tick()). */
void kill_task(surf_task *t) {
    u_long flags;
    if(!t) return;
    if(t == idle_task) panic("FATAL: Kernel Idle Task Killed");
    flags = irq_save();
    if(t->state == TS_DEAD) {
        /* gone already */
    } else if(!t->in_call) {
        become_zombie(t, -1);
    } else if(!(t->intr & TI_KILL)) {
        t->intr |= TI_KILL;
        ktimer_start(&t->kill_timer, KILL_WAIT_MS, false);
        wake_intr(t);
    }
    irq_restore(flags);
}

/* 10/6/26: the kill waited KILL_WAIT_MS and the call has not ended: it is stuck where nothing
   cuts it short, or it does not look. Killed at once, as every kill was before (whatever it
   holds stays held); the kernel log says so. From the timer interrupt. */
static void kill_escalate(void *arg) {
    surf_task *t = (surf_task *)arg;
    char line[160];
    if(t->state == TS_DEAD) return;
    if(t == curTask && t->crit) {                   /* inside KCRIT_ENTER (drawing): a tick later */
        ktimer_start(&t->kill_timer, 10, false);
        return;
    }
    snprintf(line, sizeof(line), "kill: '%s' (pid %lu) did not end its %s within %u ms: killed at once%s\n",
             t->name, t->pid, (t->in_call & TC_COMMAND) ? "kernel command" : "system call", (u_int)KILL_WAIT_MS,
             t->mutexes ? ", holding a lock" : "");
    klog_write(line);
    become_zombie(t, -1);
}

/* 10/6/26: a call begins (TC_SYSCALL, TC_COMMAND) */
void task_call_enter(u_int what) {
    u_long flags = irq_save();
    curTask->in_call |= what;
    irq_restore(flags);
}

/* ... and ends. 1 when Ctrl+C cut a kernel command short. Once the task is in no call, a
   kill that waited for it comes: task_exit(-1), in the task's own context, which closes
   its files. */
int task_call_leave(u_int what) {
    u_long flags = irq_save();
    int cut = (what & TC_COMMAND) && (curTask->intr & TI_INT);
    curTask->in_call &= ~what;
    if(what & TC_COMMAND) curTask->intr &= ~TI_INT;
    if(!curTask->in_call) {
        if(curTask->intr & TI_KILL) {
            irq_restore(flags);
            task_exit(-1);
        }
        curTask->intr = 0;
    }
    irq_restore(flags);
    return cut;
}

/* is t in owner's job: owner itself, or a program it started in the foreground, or one such a
   program started (TF_FOREGROUND all the way up)? Interrupts off. */
static bool in_job(surf_task *t, surf_task *owner) {
    int depth;
    for(depth = 0; t && depth < 64; t = t->parent, depth++) {
        if(t == owner) return true;
        if(!(t->flags & TF_FOREGROUND)) return false;
    }
    return false;
}

/* 10/6/26: Ctrl+C on a terminal whose pager owner is owner, in line mode (tty.c), interrupts
   off. The job is the owner and the programs of its foreground. A kernel command one of
   them runs in its own task is cut short (TI_INT: its waits end, its status is 130); of the
   others, the innermost programs are killed, those with no program of the job of their
   own: a shell waiting for its program lives on, and the owner always does. 1 when
   anything was reached. */
int task_interrupt_job(surf_task *owner) {
    surf_task *job[32], *t;
    int n = 0, i, j, hit = 0;
    if(!owner || owner->state == TS_DEAD) return 0;
    for(t = all_tasks; t && n < 32; t = t->all_next)
        if(t->state != TS_DEAD && in_job(t, owner)) job[n++] = t;
    for(i = 0; i < n; i++) {
        t = job[i];
        if(t->in_call & TC_COMMAND) {
            t->intr |= TI_INT;
            wake_intr(t);
            hit = 1;
            continue;
        }
        if(t == owner) continue;
        for(j = 0; j < n && job[j]->parent != t; j++);
        if(j < n) continue;                         /* it waits for a program of its own */
        t->intr |= TI_INT;                          /* its line editor says ^C (tty.c) */
        kill_task(t);
        hit = 1;
    }
    return hit;
}

/* 10/6/26: the live tasks whose terminal is tty (a pseudo-terminal's session: kernel/pty.c) */
u_int task_count_tty(struct tty *tty) {
    surf_task *t;
    u_int n = 0;
    u_long flags = irq_save();
    for(t = all_tasks; t; t = t->all_next) if(t->tty == tty && t->state != TS_DEAD) n++;
    irq_restore(flags);
    return n;
}

/* 10/6/26: a pseudo-terminal hangs up (kernel/pty.c): every live task whose terminal it is gets
   the kill that waits for the end of its call; how many. The calling task, if it is one of them,
   only inside a call (it ends at the call's end): otherwise it is ending already. */
u_int task_kill_tty(struct tty *tty) {
    surf_task *t;
    u_int n = 0;
    u_long flags = irq_save();
    for(t = all_tasks; t; t = t->all_next) {
        if(t->tty != tty || t->state == TS_DEAD || t == idle_task) continue;
        if(t == curTask && !t->in_call) continue;
        kill_task(t);
        n++;
    }
    irq_restore(flags);
    return n;
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

/* the children of the calling task (10/6/26: the self test asks before it waits for any) */
u_int task_child_count(void) {
    surf_task *t;
    u_int n = 0;
    u_long flags = irq_save();
    for(t = all_tasks; t; t = t->all_next) if(t->parent == curTask && t != curTask) n++;
    irq_restore(flags);
    return n;
}

/* 10/6/26: WNOHANG in options answers 0 at once when children are left but none has ended; with
   WINTR a kill of the caller (TI_KILL) ends the wait, -1 */
static int wait_for(u_long pid, int *status, u_int *flags_out, surf_console **con_out, int options) {
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
        if(options & WNOHANG) {
            irq_restore(flags);
            return 0;
        }
        if(options & WINTR) {
            if(curTask->intr & TI_KILL) {
                irq_restore(flags);
                return -1;
            }
            wait_prepare_intr(&zombie_wq);              /* Ctrl+C wakes it too: it waits on */
        } else {
            wait_prepare(&zombie_wq);
        }
        irq_restore(flags);
        yield();
    }
}

int task_wait_ex(u_long pid, int *status, u_int *flags_out, surf_console **con_out) {
    return wait_for(pid, status, flags_out, con_out, 0);
}

int task_wait(u_long pid, int *status) {
    return wait_for(pid, status, NULL, NULL, 0);
}

int task_wait_opts(u_long pid, int *status, int options) {
    return wait_for(pid, status, NULL, NULL, options);
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
    curTask->wait_intr = 0;                  /* a driver's delay: never cut short */
    sleepers_insert(curTask);
    irq_restore(flags);
    yield();
}

/* 10/6/26: a sleep that a kill or Ctrl+C cuts short (sleep, ping's pause, SYS_sleep) */
int sleep_intr(u_long ms) {
    u_long flags;
    if(!curTask || curTask == idle_task || in_interrupt()) {
        mdelay(ms);
        return 0;
    }
    flags = irq_save();
    if(curTask->intr) {
        irq_restore(flags);
        return -EINTR;
    }
    curTask->wake_at = getticks() + ms_to_ticks(ms);
    curTask->state = TS_SLEEPING;
    curTask->wait_intr = 1;
    sleepers_insert(curTask);
    irq_restore(flags);
    yield();
    return curTask->intr ? -EINTR : 0;
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
    if(curTask->state != TS_RUNNING) return true; /* 10/6/26: killed in this tick (a kill that waited too long) */

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
      - as a recovery console (≋?>), with a line saying why: when surfsh cannot be started
        (no initrd, no such file, not a file) or loaded (not an executable: that does not get
        better by trying again), or when it ended abnormally three times within 30 seconds.
        Abnormal: killed (a fault, kill), or ended within 2 seconds of its start, whatever its
        status (it could not get going).
    A shell that cannot be started for want of memory leaves its console without one for a
    while: init tries again every second. Its restarts are said on the console and in the
    kernel log, except after a plain exit with status 0. A console keeps the ring-0 shell
    until the next boot; that shell is started again when it dies (crashgp), as it always was.
*/
#define SURFSH_PATH       "/initrd/bin/surfsh"
#define SHELL_FAILS       3             /* abnormal ends of surfsh... */
#define SHELL_WINDOW_MS   30000         /* ...within this long: the recovery console */
#define SHELL_QUICK_MS    2000          /* a surfsh that ends sooner did not get going */
#define SHELL_RETRY_MS    1000          /* a console left without a shell: the next try */

static struct console_shell {
    u_long pid;                         /* the shell there now, 0: none yet (init tries again) */
    bool kernel;                        /* the ring-0 shell: shell=kernel, or a recovery console */
    bool waiting;                       /* init said it tries again (once) */
    char why[96];                       /* a recovery console: init's line, shown before its first prompt */
    u_long started;                     /* uptime_ms() when surfsh last started */
    u_long ends[SHELL_FAILS - 1];       /* uptime_ms() of its last abnormal ends, oldest first */
    bool killed[SHELL_FAILS - 1];       /* and which of them were kills (the others: quick exits) */
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
    if(t) cshell[i].waiting = false;
    else if(!cshell[i].waiting) {                           /* init's loop tries again */
        init_say(i, "init: no memory for a shell here; trying again\n");
        cshell[i].waiting = true;
    }
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
    char *argv[] = { "surfsh", "-l" }, line[96];
    int pid = process_spawn(SURFSH_PATH, 2, argv, &conArray[i], TF_SHELL);
    if(pid == -ENOMEM || pid == -EIO) {                     /* it may work in a moment: init's loop tries again */
        cshell[i].pid = 0;
        if(!cshell[i].waiting) {
            snprintf(line, sizeof(line), "init: %s: %s; trying again\n", SURFSH_PATH, strerror(pid));
            init_say(i, line);
            cshell[i].waiting = true;
        }
        return;
    }
    if(pid < 0) {                                           /* missing, not a file: it cannot be started */
        snprintf(cshell[i].why, sizeof(cshell[i].why), "init: %s: %s; recovery console", SURFSH_PATH, strerror(pid));
        recovery_console(i);
        return;
    }
    cshell[i].pid = (u_long)pid;
    cshell[i].waiting = false;
    cshell[i].started = uptime_ms();
}

static void start_shell(int i) {
    if(cshell[i].kernel) start_kernel_shell(i);
    else start_surfsh(i);
}

/* the third abnormal end within the window: the recovery console, with what the ends were */
static void too_many_ends(int i, bool killed, int status) {
    struct console_shell *c = &cshell[i];
    u_int k, kills = killed;
    for(k = 0; k < c->nends; k++) kills += c->killed[k];
    if(kills == SHELL_FAILS)
        snprintf(c->why, sizeof(c->why), "init: surfsh was killed %i times in %i s; recovery console", SHELL_FAILS, SHELL_WINDOW_MS / 1000);
    else if(!kills)
        snprintf(c->why, sizeof(c->why), "init: surfsh exited at once %i times in %i s (status %i); recovery console",
                 SHELL_FAILS, SHELL_WINDOW_MS / 1000, status);
    else
        snprintf(c->why, sizeof(c->why), "init: surfsh failed %i times in %i s (%u kill%s, %u quick exit%s); recovery console",
                 SHELL_FAILS, SHELL_WINDOW_MS / 1000, kills, kills > 1 ? "s" : "", SHELL_FAILS - kills,
                 SHELL_FAILS - kills > 1 ? "s" : "");
    recovery_console(i);
}

/* the shell of console i ended with status (-1: killed); flags: the task's (TF_LOADED) */
static void shell_ended(int i, int status, u_int flags) {
    struct console_shell *c = &cshell[i];
    u_long now = uptime_ms();
    char line[96];
    bool killed = status == -1;
    if(c->kernel) {
        init_say(i, "init: restarted the shell\n");
        start_kernel_shell(i);
        return;
    }
    if(!killed && !(flags & TF_LOADED)) {                  /* not an executable: trying again will not help */
        snprintf(c->why, sizeof(c->why), "init: %s could not be loaded; recovery console", SURFSH_PATH);
        recovery_console(i);
        return;
    }
    if(!killed && now - c->started >= SHELL_QUICK_MS) {     /* it ran and was left: a new one */
        if(status) {
            snprintf(line, sizeof(line), "init: surfsh exited (status %i); restarted the shell\n", status);
            init_say(i, line);
        }
        start_surfsh(i);
        return;
    }
    if(c->nends == SHELL_FAILS - 1 && now - c->ends[0] <= SHELL_WINDOW_MS) {
        too_many_ends(i, killed, status);
        return;
    }
    if(c->nends == SHELL_FAILS - 1) {                       /* keep the last ones */
        memmove(c->ends, c->ends + 1, (SHELL_FAILS - 2) * sizeof(c->ends[0]));
        memmove(c->killed, c->killed + 1, (SHELL_FAILS - 2) * sizeof(c->killed[0]));
        c->nends--;
    }
    c->ends[c->nends] = now;
    c->killed[c->nends++] = killed;
    if(killed) snprintf(line, sizeof(line), "init: surfsh was killed; restarted the shell\n");
    else snprintf(line, sizeof(line), "init: surfsh exited at once (status %i); restarted the shell\n", status);
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
        int status, pid;
        u_int flags = 0;
        surf_console *con = NULL;
        bool missing = false;
        for(i = 0; i < NUM_CONSOLES; i++) if(!cshell[i].pid) missing = true;
        pid = wait_for(WAIT_ANY, &status, &flags, &con, missing ? WNOHANG : 0);
        if(pid <= 0) {
            /* 10/6/26: a console without a shell (no memory a moment ago): another try. No
               children at all should not happen, but never spin. */
            sleep_ms(missing ? SHELL_RETRY_MS : 100);
            for(i = 0; i < NUM_CONSOLES; i++) if(!cshell[i].pid) start_shell(i);
            continue;
        }
        if(!(flags & TF_SHELL) || !con) continue;           /* an orphan: collected, that is all */
        for(i = 0; i < NUM_CONSOLES; i++) {
            if(con == &conArray[i] && cshell[i].pid == (u_long)pid) shell_ended(i, status, flags);
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
