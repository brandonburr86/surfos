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
10/6/26: the tasks on a terminal, counted and killed, for the pseudo-terminals (kernel/pty.c);
the task list as programs see it, and a process leaving its shell's job (kernel/proc.c). A
kernel thread started from a pseudo-terminal's session is on F1 (task_alloc()).
10/18/26 (the desktop's System Monitor): End task's kill waits for the end of the call however
long, as a hangup's does (kill_task_patient()), and the task list is copied in one go
(task_info_all()).
10/25/26 (robustness, L15): a user's kill ends processes and ring-0 shells only (task_user_may_kill(),
task_kill_user()): a process is marked TF_PROCESS from its creation.
10/25/26 (robustness, L10): a process has floating-point registers of its own, switched lazily
(kernel/fpu.c): an area made with the task and freed with it, the FPU no longer its own when it
dies, and CR0.TS set by schedule() for whoever does not own it.
10/25/26 (robustness, L7): a process's memory goes when it ends, not when it is collected: its
address space is destroyed by task_exit(), or, for a task killed on the spot, by the uvmreap
thread, which become_zombie() hands it to. schedule() compares with the directory loaded.
10/25/26 (robustness, L17): kernel stacks live in a window of their own, each over a guard left
unmapped (kstack_alloc()), so an overflow is a double fault that kernel/panic.c reports.
10/25/26 (robustness, L22, L23): the shutdown (kernel/shutdown.c) ends the processes and ring-0
shells (task_end_users()), and init starts no shell then; Ctrl+Alt+Del wakes init to start it.
10/17/26 (audit B39): the kill that waited is put off while its task is inside a file system or
the block cache (fs_hold, task.h): kill_escalate(), kill_task(), kill_hangup(). (B46, B47) Also
while it is in an umount or a device request. (B48) A kernel thread killed outside any call that
enters one before it dies gets that call's KILL_WAIT_MS. (B55) A hangup's kill of such a thread
keeps the hangup's rule in that call: no escalation, the call's end awaited.
10/19/26 (audit B75): reap() counts the waiter until it has freed the dead task, not only around its
closes. (B76) A kill put off while its task held something gives the task KILL_WAIT_MS from the
moment it has let go, to end its call or itself, rather than coming at the first look after it,
where its clean-up runs (kill_escalate()). (B79) That grace starts at the task's next path call
holding nothing too, and a task still holding something when it has run out gets EINTR from its
path calls (task_vfs_begin_slow()).
10/19/26 (audit B80): the grace is owed from the kill itself when the kill finds its task holding
something (kill_task(), kill_hangup()), and to a task that begins a VFS call after its kill, not
only once a look has seen it holding. (B81) Every VFS call begins in task_vfs_begin_slow() for a
killed task, a call on an open file too. (B82) The kernel log's line for a kill put off says what
comes: a grace, or, once that is over, the kill as soon as the task lets go. What the rule
guarantees, and what it does not, is said before kill_escalate().
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
#include <surfos/job.h>
#include <surfos/spawn.h>
#include <sys/framebuffer.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <mm/uvm.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <surfos/proc.h>
#include <surfos/fpu.h>
#include <surfos/shutdown.h>

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
static struct uvm *uvms_dead;            /* 10/25/26 (L7): address spaces of tasks killed on the spot, for uvmreap */
static u_long uvms_dead_pages;           /* the frames they hold until uvmreap frees them (memstat) */
static wait_queue_t uvmreap_wq;

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
    case TS_STOPPED: return "stopped";     /* 10/25/26: job control */
    }
    return "?";
}

/**** Creation ****/

#ifdef SURFOS_RISCV32
/* 10/15/26 (SurfCPU): riscv32 builds the frame its trap exit starts a task from (arch/riscv32/trap.c) */
#define build_initial_frame(stack_top, ring, eip) arch_task_frame(stack_top, ring, eip)
#else
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
#endif

/* Every task starts here: call its entry and exit when that returns, so a task function
   may simply return (the 2004 "task stublet"). */
static void task_stublet(void) {
    curTask->entry(curTask->arg);
    task_exit(0);
}

static void kill_escalate(void *arg);
static void kill_hangup(surf_task *t);
static void fill_info(surf_task *t, struct procinfo *pi);

/**** Kernel stacks (10/25/26, robustness, L17) ****/

/* The window (include/mm/memory.h) is cut into slots of KSTACK_SLOT. A stack is the top KSTACK_SIZE of its slot,
   mapped from frames when its task is made: a process's from the programs' class (PMM_USER), which leaves the
   kernel its reserve, a kernel thread's from the kernel's. The rest of the slot is never mapped, so a stack that
   overflows faults there; that fault cannot push its own frame, and the double fault's task gate reports it
   (kernel/panic.c). The 2004 stacks were heap blocks, with whatever lay below them to run into (M10). */
#define KSTACK_PAGES (KSTACK_SIZE / PAGE_SIZE)
#ifdef SURFOS_RISCV32
/* 10/15/26 (SurfCPU): machine mode has no paging, so no guard: a stack is a heap block */
static u_char *kstack_alloc(int cls) { (void)cls; return (u_char *)kalloc(KSTACK_SIZE); }
static void kstack_free(u_char *stack) { kfree(stack); }
#else
#define KSTACK_SLOTS ((KSTACK_WIN_END - KSTACK_WIN_START) / KSTACK_SLOT)
static u32 kstack_used[KSTACK_SLOTS / 32];
static u_long kstack_next;

static u_char *kstack_alloc(int cls) {
    u_long frame[KSTACK_PAGES], slot = 0, i, flags, base;
    u_int n;
    for(n = 0; n < KSTACK_PAGES; n++) {
        frame[n] = pmm_alloc_as(cls);
        if(!frame[n]) {
            while(n--) pmm_free(frame[n]);
            return NULL;
        }
    }
    flags = irq_save();
    for(i = 0; i < KSTACK_SLOTS; i++) {
        slot = (kstack_next + i) % KSTACK_SLOTS;
        if(!(kstack_used[slot / 32] & (1u << (slot % 32)))) break;
    }
    if(i == KSTACK_SLOTS) {                     /* 2048 tasks: memory runs out first */
        irq_restore(flags);
        for(n = 0; n < KSTACK_PAGES; n++) pmm_free(frame[n]);
        return NULL;
    }
    kstack_used[slot / 32] |= 1u << (slot % 32);
    kstack_next = slot + 1;
    irq_restore(flags);
    /* in the kernel's tables, which every address space shares */
    base = KSTACK_WIN_START + slot * KSTACK_SLOT + KSTACK_SLOT - KSTACK_SIZE;
    for(n = 0; n < KSTACK_PAGES; n++) vmm_map(base + n * PAGE_SIZE, frame[n], PTE_W);
    return (u_char *)base;
}

/* never the stack the caller runs on: reap() frees a zombie's, in its waiter */
static void kstack_free(u_char *stack) {
    u_long base = (u_long)stack, slot = (base - KSTACK_WIN_START) / KSTACK_SLOT, flags;
    u_int n;
    for(n = 0; n < KSTACK_PAGES; n++) {
        u_long frame = vmm_get_phys(base + n * PAGE_SIZE) & PTE_FRAME;
        vmm_unmap(base + n * PAGE_SIZE);
        pmm_free(frame);
    }
    flags = irq_save();
    kstack_used[slot / 32] &= ~(1u << (slot % 32));
    irq_restore(flags);
}

/* The task whose stack's slot holds addr (the guard below the stack included), and in *guard whether addr is in
   that guard. For the double fault's report: interrupts are off, and nothing is taken or changed. */
surf_task *task_stack_owner(u_long addr, bool *guard) {
    u_long slot, base;
    surf_task *t;
    u_int n = 0;
    *guard = false;
    if(addr < KSTACK_WIN_START || addr >= KSTACK_WIN_END) return NULL;
    slot = (addr - KSTACK_WIN_START) / KSTACK_SLOT;
    base = KSTACK_WIN_START + slot * KSTACK_SLOT + KSTACK_SLOT - KSTACK_SIZE;
    *guard = addr < base;
    if(curTask && (u_long)curTask->stackmem == base) return curTask;
    for(t = all_tasks; t && n < 4 * KSTACK_SLOTS; t = t->all_next, n++) if((u_long)t->stackmem == base) return t;
    return NULL;
}
#endif
#define FS_HOLD_RETRY_MS 20                     /* 10/17/26 (audit B39): a kill put off while the task is inside a file
                                                   system looks again this much later */
/* 10/17/26 (audit B39): is t inside a file system or the block cache, where a kill at once would leave locks, blocks
   and the cache's records behind: counted by fs/fat.c and fs/bcache.c (fs_hold), or holding the cache's lock.
   10/17/26 (B46, B47): also a step that must finish (fs_finish: umount) and a device request (driver/bdev.c: a driver's
   lock and transfer). 10/18/26 (audit B62): or holding a mutex (surf_task.mutexes, kernel/sync.c), a driver's lock in
   a thread's probe or poll outside any request among them: no task is killed at once holding one (before, a kill that
   had waited 2 s came at once and said "holding a lock"), and a lock handed over to a waiter that has not run yet (B63)
   counts as held by it. The console's lock, counted with them, does not count here: it is given back when its holder
   dies (console_task_gone()) */
static int holds(const surf_task *t) {
    if(t->fs_hold || bcache_holds(t)) return TASK_HOLDS_FS;
    return t->mutexes > (console_holds(t) ? 1u : 0u) ? TASK_HOLDS_LOCK : 0;
}
/* 10/18/26 (audit B73): or its call holds references to a mounted volume's vnodes (fs_refs, fs/vfs.c): killed at once
   in one of its waits there (a volume's lock, the cache, a device) it left them behind, and the volume could not be
   unmounted until restart. Not the fault policy's (below): that leaves no lock held */
static bool in_fs(const surf_task *t) { return holds(t) != 0 || t->fs_refs; }
int task_holds(const surf_task *t) { return t ? holds(t) : 0; }   /* 10/18/26 (audit B62) */
bool task_in_fs(const surf_task *t) { return t && holds(t) != 0; }    /* 10/18/26 (audit B56: the fault policy) */

static surf_task *task_alloc(const char *name, prio_level prio, u_int flags, surf_console *con, u_int ring) {
    surf_task *t = (surf_task *)kcalloc(1, sizeof(surf_task));
    u_long irqf;
    if(!t) return NULL;
    t->stackmem = kstack_alloc((flags & TF_PROCESS) ? PMM_USER : PMM_KERNEL);  /* 10/25/26 (L17) */
    if(!t->stackmem) {
        kfree(t);
        return NULL;
    }
    if((flags & TF_PROCESS) && fpu_task_new(t)) {         /* 10/25/26 (L10): its floating-point registers */
        kstack_free(t->stackmem);
        kfree(t);
        return NULL;
    }
    /* A kernel stack is mapped in full before it is used (kstack_alloc()): the CPU pushes a page fault's frame onto
       that very stack, so a fault there could not be taken. */
    memset(t->stackmem, 0, KSTACK_SIZE);
    strlcpy(t->name, name, TASK_NAME_LEN);
    t->prio = prio;
    t->flags = (flags & ~TF_INCALL) | (ring == USER ? TF_USER : 0);
    t->in_call = (flags & TF_INCALL) ? TC_SYSCALL : 0;     /* 10/6/26: a kill waits for the call to end */
    /* 10/6/26: a kernel thread (no TF_INCALL: not a process) is never part of a pseudo-terminal's
       session (kernel/pty.c): started from one, it prints on F1 and reads F1's keys, as a kernel
       thread of the boot does. So a session ends with its processes, and a hangup's kill, which
       waits for the end of a call, never meets a thread that is in none. */
    if(con && con->stream && !(flags & TF_INCALL)) con = &conArray[0];
    t->con = con;
    t->tty = tty_for_console(con);
    strlcpy(t->cwd, (curTask && curTask->cwd[0]) ? curTask->cwd : "/", sizeof(t->cwd));
    t->stack_top = (u_long)t->stackmem + KSTACK_SIZE;
    t->esp = (u_long *)build_initial_frame(t->stackmem + KSTACK_SIZE, ring, (u_long)task_stublet);
    ktimer_init(&t->kill_timer, kill_escalate, t);
    /* a detached task is init's from the start: nobody else may wait for it, and a shell running
       a background server must not see it as a child in task_wait(WAIT_ANY) */
    t->parent = ((flags & TF_DETACHED) && init_task_ptr) ? init_task_ptr : curTask;
    t->teller = (flags & TF_TOLD) && curTask ? curTask->pid : 0;  /* 11/09/26: its end is kept for its starter */
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
    /* 10/6/26: a process started on a pseudo-terminal that has hung up (kernel/pty.c) gets the
       hangup's kill, as the tasks there did: it ends once it is loaded */
    if((flags & TF_INCALL) && t->tty && t->tty->hungup) {
        u_long irqf = irq_save();
        kill_hangup(t);
        irq_restore(irqf);
    }
    task_start(t);
    return t;
}

/* 10/25/26: kthread_create_on() for a process, put in its job (surfos/job.h) before it can run: job
   SPAWN_JOB_NEW a new one it leads, SPAWN_JOB_NONE none, a job's id that one (made by the creator),
   0 the creator's, when the task is on the creator's terminal (none on another: a pty's session).
   Its pid, read before it can run (a detached task may be gone at once), or -ENOMEM. */
int kthread_create_job(const char *name, surf_console *con, kthread_fn fn, void *arg, prio_level prio,
                       u_int flags, int job) {
    surf_task *t = task_alloc(name, prio, flags | TF_KTHREAD, con ? con : &conArray[0], KERNEL);
    u_long pid;
    if(!t) return -ENOMEM;
    t->entry = fn;
    t->arg = arg;
    if(job == SPAWN_JOB_NEW) {
        t->job = t->pid;
        t->job_shell = curTask ? curTask->pid : 0;
    } else if(job > 0) {
        t->job = (u_long)job;
        t->job_shell = curTask ? curTask->pid : 0;
    } else if(job == 0 && curTask && curTask->tty == t->tty) {
        t->job = curTask->job;
        t->job_shell = curTask->job_shell;
    }
    if((flags & TF_INCALL) && t->tty && t->tty->hungup) {       /* as kthread_create_on() */
        u_long irqf = irq_save();
        kill_hangup(t);
        irq_restore(irqf);
    }
    pid = t->pid;
    task_start(t);
    return (int)pid;
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

/* 10/25/26: the shell t ends: the stopped tasks of the jobs it made are killed (nothing could let
   them go on), the others leave their jobs (surfos/job.h). Interrupts off. */
static void jobs_shell_gone(surf_task *t) {
    surf_task *u;
    for(u = all_tasks; u; u = u->all_next) {
        if(u == t || u->state == TS_DEAD || !u->job || u->job_shell != t->pid) continue;
        if(u->stopped || u->stop_req) kill_task(u);
        else u->job = u->job_shell = 0;
    }
}

/* interrupts off: hand t's children to init and tell waiters */
static void orphan_children(surf_task *t) {
    surf_task *c;
    for(c = all_tasks; c; c = c->all_next) {
        if(c->parent == t) c->parent = init_task_ptr;
    }
}

/* 11/09/26 (track desk): the ends of TF_TOLD tasks, for the tasks that started them (task_ends_take()); a slot with
   pid 0 is free, and seq says which is the oldest */
static struct task_end ends[TASK_ENDS];
static u_long ends_teller[TASK_ENDS], ends_seq[TASK_ENDS], ends_next;

/* interrupts off: t's end kept for its teller, in a free slot or the oldest one's */
static void keep_end(surf_task *t, int code) {
    u_int i, k = 0;
    for(i = 0; i < TASK_ENDS; i++) {
        if(!ends[i].pid) { k = i; break; }
        if(ends_seq[i] < ends_seq[k]) k = i;
    }
    ends[k].pid = t->pid;
    strlcpy(ends[k].name, t->name, sizeof(ends[k].name));
    ends[k].code = code;
    ends[k].how = t->end_how;
    ends[k].window = (t->flags & TF_WINDOW) != 0;
    ends_teller[k] = t->teller;
    ends_seq[k] = ++ends_next;
}

/* the ends kept for the calling task, at most max, oldest first; forgotten once given */
int task_ends_take(struct task_end *out, int max) {
    int n = 0;
    u_int i, k;
    u_long flags = irq_save();
    while(n < max) {
        for(i = 0, k = TASK_ENDS; i < TASK_ENDS; i++)
            if(ends[i].pid && ends_teller[i] == curTask->pid && (k == TASK_ENDS || ends_seq[i] < ends_seq[k])) k = i;
        if(k == TASK_ENDS) break;
        out[n++] = ends[k];
        ends[k].pid = 0;
    }
    irq_restore(flags);
    return n;
}

static void become_zombie(surf_task *t, int code) {
    ktimer_cancel(&t->kill_timer);            /* 10/6/26: a kill that waited has come */
    if(t->wait_timer) {                       /* killed at once in wait_on_timeout(): its timer is on t's stack */
        ktimer_cancel(t->wait_timer);
        t->wait_timer = NULL;
    }
    if(t->list) task_list_remove(t->list, t); /* a run queue, the sleep list or a wait queue */
    t->exit_code = code;
    /* 10/25/26 (SYS_waitx): every kill marks TI_KILL (a task that exits with -1 itself is not killed);
       one that Ctrl+C began (task_interrupt_job()) left TI_INT too */
    t->end_how = !(t->intr & TI_KILL) ? WX_EXITED : (t->intr & TI_INT) ? WX_INTERRUPTED : WX_KILLED;
    if((t->flags & TF_TOLD) && t->teller) keep_end(t, code);     /* 11/09/26 (track desk) */
    t->state = TS_DEAD;
    tty_task_gone(t);                         /* raw mode it set on a terminal ends with it */
    framebuffer_task_died(t);                 /* 10/6/26: killed from an interrupt while it drew, it gives the screen back */
    fpu_task_gone(t);                         /* 10/25/26 (L10): what the FPU holds is nobody's now */
    if(t->uvm) {                              /* 10/25/26 (L7): killed on the spot: uvmreap frees its memory now */
        t->uvm->reap_next = uvms_dead;
        uvms_dead = t->uvm;
        uvms_dead_pages += t->uvm->pages + t->uvm->table_pages;
        t->uvm = NULL;
        wake_up(&uvmreap_wq);
    }
    jobs_shell_gone(t);                       /* 10/25/26: the jobs it made */
    orphan_children(t);
    if(t->flags & TF_DETACHED) t->parent = init_task_ptr;
    task_list_append(&zombies, t);
    wake_up_all(&zombie_wq);
}

void task_exit(int code) {
    u_long flags;
    struct uvm *m;
    if(curTask == idle_task) panic("the idle task tried to exit");
    /* 10/6/26 (review of the network connections): the task ends by itself now. Closing its files
       (a socket's close takes net_lock, a file's may write) must not be cut by a kill at once (a
       second Ctrl+C, say) nor by the 2 s of a kill that waited: either would leave a lock held for
       good. So it is a call that every kill leaves alone (TC_EXIT), the kill that waited is
       answered, and its timer goes. */
    flags = irq_save();
    curTask->in_call |= TC_EXIT;
    ktimer_cancel(&curTask->kill_timer);
    irq_restore(flags);
    /* 10/6/26: its files close now, in its own context, not when someone waits for it: a
       background program's file is written out and its volume can be unmounted at once */
    fd_close_all(curTask);
    /* 10/25/26 (L7): and its memory goes now, whoever collects it and when: detached, and the kernel's directory
       loaded, with interrupts off (uvm_destroy() never frees the directory that is loaded) */
    flags = irq_save();
    m = curTask->uvm;
    curTask->uvm = NULL;
    if(m) uvm_switch(NULL);
    irq_restore(flags);
    if(m) uvm_destroy(m);
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
    t->intr |= TI_KILL;                         /* 10/25/26: for SYS_waitx (become_zombie()) */
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
    if(!t || (t->flags & TF_SYSTEM)) return;   /* 10/18/26 (audit B59): the kernel's own threads (task.h): never */
    if(t == idle_task) panic("FATAL: Kernel Idle Task Killed");
    flags = irq_save();
    if(t->state == TS_DEAD || (t->in_call & TC_EXIT)) {
        /* gone already, or ending by itself (task_exit()) */
    } else if(!t->in_call && !in_fs(t)) {
        t->intr |= TI_KILL;                     /* 10/25/26: for SYS_waitx */
        become_zombie(t, -1);
    } else if(!(t->intr & TI_KILL) || (t->intr & (TI_HANGUP | TI_PATIENT))) {
        if(!(t->intr & TI_KILL)) t->kill_at = getticks();   /* 10/18/26 (audit B69): when, for the log's one line */
        /* 10/6/26: after a hangup's kill too: this one's rule (10/18/26: and after End task's) */
        t->intr = (t->intr | TI_KILL) & ~(TI_HANGUP | TI_PATIENT);
        if(in_fs(t)) t->intr |= TI_HELD;            /* 10/19/26 (audit B80): it holds something: its grace is owed from
                                                       now (a thread that let go before the first look was killed at
                                                       once at it, and a call that let go just before its KILL_WAIT_MS
                                                       look at that look, both where their clean-up runs) */
        /* 10/17/26 (audit B39): a kernel thread outside any call inside a file system: as soon as it leaves it
           (B48: TI_NOCALL, so that a call it enters meanwhile gets the call's KILL_WAIT_MS) */
        if(!t->in_call) t->intr |= TI_NOCALL;
        else t->intr &= ~TI_NOCALL;             /* (B55: a hangup's mark of a task outside a call, which entered one since) */
        ktimer_start(&t->kill_timer, t->in_call ? KILL_WAIT_MS : FS_HOLD_RETRY_MS, false);
        wake_intr(t);
        if(t->state == TS_STOPPED) task_make_ready(t);  /* 10/25/26: stopped in its call: it ends it */
    }
    irq_restore(flags);
}

/* 10/6/26: the kill of a pseudo-terminal's hangup (task_kill_tty(), kthread_create_on()). In its own
   code a process ends at once, as with kill_task(); inside a call it is marked and its waits that may
   be cut short are, but no escalation follows: it ends when the call does, however long that takes,
   so whatever the call holds (a file system's lock, a claim) is let go on the way out. The kernel log
   says once, KILL_WAIT_MS later, that it is waited for (kill_escalate()). Interrupts off.
   10/18/26: the same rule for End task (kill_task_patient()), whose mark is TI_PATIENT instead of
   TI_HANGUP, so that the kernel log does not speak of a terminal */
static void kill_waiting(surf_task *t, u_int mark) {
    if(t->state == TS_DEAD ||                   /* who it spares, all here: gone already, */
       (t->in_call & TC_EXIT) ||                /* ending already (task_exit(): its files are closing), */
       (t->intr & TI_KILL))                     /* or killed already (that kill's rule stands) */
        return;
    t->kill_at = getticks();                    /* 10/18/26 (audit B69) */
    if(!t->in_call) {
        if(in_fs(t)) {                          /* 10/17/26 (audit B39): inside a file system: as soon as it leaves it */
            t->intr |= TI_KILL | TI_NOCALL | mark | TI_HELD;   /* (B48; 10/18/26, B55: a call it enters keeps the
                                                     hangup's rule, waited for to its end: kill_escalate(); 10/19/26,
                                                     B80: its grace owed, as kill_task() has it) */
            ktimer_start(&t->kill_timer, FS_HOLD_RETRY_MS, false);
            wake_intr(t);
            if(t->state == TS_STOPPED) task_make_ready(t);
        } else {
            t->intr |= TI_KILL;                 /* 10/25/26: for SYS_waitx */
            become_zombie(t, -1);
        }
        return;
    }
    t->intr |= TI_KILL | mark;
    ktimer_start(&t->kill_timer, KILL_WAIT_MS, false);
    wake_intr(t);
    if(t->state == TS_STOPPED) task_make_ready(t);      /* 10/25/26: stopped in its call: it ends it */
}

static void kill_hangup(surf_task *t) {
    kill_waiting(t, TI_HANGUP);
}

/* 10/18/26 (the desktop's End task, shell/guisys.c): kill_waiting()'s rule, which never leaves a zombie holding
   a lock: a task in its own code ends at once, one inside a call when the call ends, however long it takes.
   1 when it is inside a call (it ends when the call does), 0 when it ended at once or had ended */
int kill_task_patient(surf_task *t) {
    u_long flags;
    int waits;
    if(!t || t == idle_task) return 0;
    flags = irq_save();
    kill_waiting(t, TI_PATIENT);
    waits = t->state != TS_DEAD && t->in_call;
    irq_restore(flags);
    return waits;
}

/* 10/25/26 (L15): a user's kill of pid (SYS_kill; the kill command, which surfsh's kill of a pid runs): kill_task()'s
   rule for what task_user_may_kill() allows, -EPERM for the rest (killed on the spot, a kernel thread would keep its
   locks), -EINVAL when there is no such task. Found and killed with interrupts off: a zombie may be collected (and
   freed) by its parent at any time. Its name is copied into name (TASK_NAME_LEN bytes) when it is found. */
int task_kill_user(u_long pid, char *name) {
    surf_task *t;
    u_long flags = irq_save();
    int r = 0;
    t = task_find(pid);
    if(!t) r = -EINVAL;
    else {
        if(name) strlcpy(name, t->name, TASK_NAME_LEN);
        if(!task_user_may_kill(t)) r = -EPERM;
        else kill_task(t);
    }
    irq_restore(flags);
    return r;
}

/* 10/19/26 (audit B80-B82): the rule, what it guarantees and what it does not. A kill comes at the end of its task's
   call (task_call_leave(): task_exit(-1) in the task's own context, its files closed), or at once only (1) at the kill,
   to a task outside any call that holds nothing (2004's rule), (2) at a call's KILL_WAIT_MS look, to a call that holds
   nothing and is not marked TI_HELD (2004's rule: the moment is the kill's, not a letting go's), (3) at a look after the
   task's grace, to one that holds nothing then. A task holding something (in_fs()) is never killed at once (B39, B62,
   B73): it is looked at again every FS_HOLD_RETRY_MS. TI_HELD, a grace owed, is set where the kill finds the task holding
   something (kill_task(), kill_hangup(), B48's hand-over to a call), at each look that does, and at the start of each VFS
   call a killed task makes (task_vfs_begin_slow(): it is to take something). The grace (TI_GRACE, once) begins at the
   first moment after that the task is seen holding nothing, a look or the start of a VFS call, and lasts KILL_WAIT_MS:
   a call that ends meanwhile is killed at its end, a kernel thread that ends ends by itself. At its end a task holding
   something is marked TI_LATE: its VFS calls but a close say EINTR before they take anything, and it is killed at the
   first look that finds it holding nothing (3).
   So a task whose kill found it holding something, or that took something since, is never killed at once before
   KILL_WAIT_MS after the first moment it was seen holding nothing; one that ends in that time ends with its clean-up
   done, at its call's end or by itself; and one that ignores its kill and works on through VFS calls is killed within
   about KILL_WAIT_MS of its first letting go and the operation then under way. Not so: (a) the grace is once: a task
   holding something again at its end is killed at the first look after its next letting go, its clean-up then cut
   short; (b) a call not marked (holding nothing at its kill, no VFS call since, nothing held at its KILL_WAIT_MS look: a
   lock taken and let go outside any VFS call, a network call's, is not seen) is killed at once at that look wherever it
   is, as in 2004; (c) a task that never lets go (a lock or a file kept while it works) is not killed while it holds:
   its waits are cut short or bounded (B39, B73), and a command looks for its kill between its steps; (d) a hangup's kill
   in a call waits for the call's end however long (B55); (e) a fault is not a kill (kill_task_now(), B56) */

/* 10/6/26: the kill waited KILL_WAIT_MS and the call has not ended: it is stuck where nothing
   cuts it short, or it does not look. Killed at once, as every kill was before (whatever it
   holds stays held); the kernel log says so. From the timer interrupt. */
static void kill_escalate(void *arg) {
    surf_task *t = (surf_task *)arg;
    char line[200];
    if(t->state == TS_DEAD || (t->in_call & TC_EXIT)) return;
    if((t->intr & TI_NOCALL) && t->in_call) {       /* 10/17/26 (audit B48): killed outside any call (put off inside a file
                                                       system), it has entered a call since: that call's rule, KILL_WAIT_MS
                                                       from now (before, it was killed at once in it 20 ms later, said
                                                       to have overrun its 2 s, holding whatever the call held). 10/18/26
                                                       (B55): a hangup's (TI_HANGUP too) then says so once and waits for
                                                       the call's end, as kill_hangup() has it for a task in a call */
        t->intr &= ~(TI_NOCALL | TI_GRACE | TI_LATE);   /* (10/19/26, B76: the call's rule from its start; B80: a grace
                                                       still owed to it, TI_HELD kept, and owed if it holds something) */
        if(in_fs(t)) t->intr |= TI_HELD;
        ktimer_start(&t->kill_timer, KILL_WAIT_MS, false);
        return;
    }
    if((t->intr & TI_HANGUP) && !(t->intr & TI_NOCALL)) {   /* 10/6/26: a hangup's kill waits for the call, however long
                                                       (B55: outside any call it comes as soon as the task is out) */
        snprintf(line, sizeof(line), "kill: '%s' (pid %lu) is still in its %s %u ms after its terminal hung up: "
                 "waiting for its end%s\n", t->name, t->pid, (t->in_call & TC_COMMAND) ? "kernel command" : "system call",
                 (u_int)KILL_WAIT_MS, t->mutexes ? ", holding a lock" : "");
        klog_write(line);
        return;
    }
    if((t->intr & TI_PATIENT) && !(t->intr & TI_NOCALL)) {   /* 10/18/26: and End task's (kill_task_patient()) */
        snprintf(line, sizeof(line), "kill: '%s' (pid %lu) is still in its %s %u ms after it was asked to end: "
                 "waiting for its end%s\n", t->name, t->pid, (t->in_call & TC_COMMAND) ? "kernel command" : "system call",
                 (u_int)KILL_WAIT_MS, t->mutexes ? ", holding a lock" : "");
        klog_write(line);
        return;
    }
    if(t == curTask && t->crit) {                   /* inside KCRIT_ENTER (drawing): a tick later */
        ktimer_start(&t->kill_timer, 10, false);
        return;
    }
    if(in_fs(t)) {                                  /* 10/17/26 (audit B39): inside a file system or the block cache */
        u_long ms = (getticks() - t->kill_at) * (1000 / HZ);
        /* (its waits there are cut short: it leaves soon; at once it would leave the cache's records, marks, blocks and
           locks behind). Said once, KILL_WAIT_MS after the kill: for a task in a call this is its first look; 10/18/26
           (audit B69): a task killed outside any call is looked at every FS_HOLD_RETRY_MS from its kill, and was waited
           for without a word */
        t->intr |= TI_HELD;                         /* (10/19/26, B76: below, once it has let go) */
        if(t->intr & TI_GRACE) t->intr |= TI_LATE;  /* (10/19/26, B79: its grace is over: task_vfs_begin() refuses) */
        if(!(t->intr & TI_FSWAIT) && (t->in_call || ms >= KILL_WAIT_MS)) {
            const char *what = holds(t) == TASK_HOLDS_LOCK ? "holding a lock"
                             : holds(t) ? "inside a file system or a device request"
                             : "holding a volume's file or directory";   /* (10/18/26, B73: fs_refs) */
            t->intr |= TI_FSWAIT;
            if(t->intr & TI_GRACE)                  /* 10/19/26 (audit B82): this look is its grace's end (begun at a VFS
                                                       call, B79): a grace promised here was already spent */
                snprintf(line, sizeof(line), "kill: '%s' (pid %lu) is %s again %lu ms after its kill, its %u ms from "
                         "letting go over: killed as soon as it lets go\n", t->name, t->pid, what, ms, (u_int)KILL_WAIT_MS);
            else
                snprintf(line, sizeof(line), "kill: '%s' (pid %lu) is still %s %lu ms after its kill: once it lets go "
                         "it has %u ms to end\n", t->name, t->pid, what, ms, (u_int)KILL_WAIT_MS);
            klog_write(line);
        }
        ktimer_start(&t->kill_timer, FS_HOLD_RETRY_MS, false);
        return;
    }
    if((t->intr & (TI_HELD | TI_GRACE)) == TI_HELD) {
        /* 10/19/26 (audit B76): it held something at a look and has let go since. Killed at once here, at the first look
           after it let go, the kill came where its clean-up runs (a waiter's reap() freeing a dead task's memory, a
           spawn handing its files to the child, a command freeing its buffers), and that was left undone for good. It
           has KILL_WAIT_MS from now, once: a call comes to its end (task_call_leave(): the kill comes there, in its own
           context), a kernel thread to its own end. Only one still running then is killed at once (as soon as it holds
           nothing, if it holds something again then: no second grace) */
        t->intr |= TI_GRACE;
        ktimer_start(&t->kill_timer, KILL_WAIT_MS, false);
        return;
    }
    if(!t->in_call) {                               /* 10/17/26: a kernel thread killed inside a file system, out now */
        if(t->intr & TI_GRACE) {                    /* (10/19/26, B76: and its grace over) */
            snprintf(line, sizeof(line), "kill: '%s' (pid %lu) did not end within %u ms of letting go: killed at once\n",
                     t->name, t->pid, (u_int)KILL_WAIT_MS);
            klog_write(line);
        }
        become_zombie(t, -1);
        return;
    }
    snprintf(line, sizeof(line), "kill: '%s' (pid %lu) did not end its %s within %u ms%s: killed at once\n",
             t->name, t->pid, (t->in_call & TC_COMMAND) ? "kernel command" : "system call", (u_int)KILL_WAIT_MS,
             (t->intr & TI_GRACE) ? " of letting go" : "");
    klog_write(line);
    become_zombie(t, -1);
}

/**** 10/25/26: job control (surfos/job.h) ****/

/* t counts as stopped from now on: a parent waiting with WUNTRACED looks. Interrupts off. Also from
   wait_prepare_intr() (kernel/wait.c): a task that is to stop and waits again counts as stopped. */
void task_now_stopped(surf_task *t) {
    if(t->stopped) return;
    t->stopped = 1;
    t->stop_seen = 0;
    wake_up_all(&zombie_wq);
}

/* t is to stop (why: WX_STOP_*). In its own code it stops at once. In a sleep that a kill would cut
   short, or waiting for a key, it is woken and stops there (sleep_intr(), tty_getc(): the sleep goes
   on afterwards, and no key is taken meanwhile); in another wait that a kill would cut short it
   counts as stopped and the wait goes on; anywhere in a call it stops at the call's end
   (task_call_leave()), and counts as stopped as soon as it waits again (wait_prepare_intr()).
   Never a task that is ending, nor a shell that looks for Ctrl+C itself (TF_INTR: it runs commands
   of its own). Interrupts off. */
static void stop_task(surf_task *t, int why) {
    if(t->state == TS_DEAD || (t->in_call & TC_EXIT) || (t->flags & TF_INTR)) return;
    t->stop_req = 1;
    t->stop_why = (u_char)why;
    t->flags &= ~TF_FOREGROUND;                 /* out of the foreground: Ctrl+C no longer reaches it (fg) */
    if(!t->in_call) {
        if(t->state == TS_READY) {
            task_list_remove(t->list, t);
            t->state = TS_STOPPED;
        } else if(t == curTask && t->state == TS_RUNNING) {
            t->state = TS_STOPPED;              /* switched away from before the interrupt returns (irq_dispatch()) */
        }
        task_now_stopped(t);
    } else if(t->wait_intr && (t->state == TS_BLOCKED || t->state == TS_SLEEPING)) {
        task_now_stopped(t);
        if(t->state == TS_SLEEPING || tty_is_readers(t->list)) {
            task_list_remove(t->list, t);       /* it stops where it slept or waited for a key */
            task_make_ready(t);
        }
    }
}

/* t goes on: in the foreground of its terminal (Ctrl+C reaches it) or not. Interrupts off. */
static void go_on(surf_task *t, int foreground) {
    if(t->state == TS_DEAD) return;
    if(foreground) t->flags |= TF_FOREGROUND;
    else t->flags &= ~TF_FOREGROUND;
    t->stop_req = t->stopped = t->stop_seen = 0;
    if(t->state == TS_STOPPED) task_make_ready(t);
}

/* Ctrl+Z on tty: the tasks of its foreground job stop; how many. Interrupts off. */
int task_stop_job(struct tty *tty, u_long job) {
    surf_task *t;
    int n = 0;
    if(!job) return 0;
    for(t = all_tasks; t; t = t->all_next) {
        if(t->job != job || t->tty != tty || t->state == TS_DEAD) continue;
        stop_task(t, WX_STOP_KEY);
        n++;
    }
    return n;
}

/* The current task wants a key of tty (tty_getc()) while it is to stop (Ctrl+Z), or while its job
   is not the foreground one: it stops here, in its call, off the queue of readers, until its shell
   lets it go on, and reads once its job is in the foreground (in the background it stops again).
   0 when it may read, -EINTR when it was killed meanwhile. */
int task_stop_reader(struct tty *tty) {
    u_long flags = irq_save();
    int r;
    while(curTask->job && !(curTask->intr & TI_KILL) && !tty->hungup &&
          (curTask->stop_req || curTask->job != tty->fg_job)) {
        if(!curTask->stop_req) {                /* from the background */
            curTask->stop_req = 1;
            curTask->stop_why = WX_STOP_READ;
        }
        curTask->flags &= ~TF_FOREGROUND;
        task_now_stopped(curTask);
        curTask->state = TS_STOPPED;
        irq_restore(flags);
        yield();                                /* go_on() makes it ready, its marks taken off */
        flags = irq_save();
    }
    r = (curTask->intr & TI_KILL) ? -EINTR : 0;
    irq_restore(flags);
    return r;
}

/* 10/19/26 (audit B76, B79): task.h's task_vfs_begin(), when the current task's kill was put off. A task that keeps
   making VFS calls holds something nearly all the time, and the looks 20 ms apart rarely find it out: its grace starts
   here instead, at its next path call holding nothing (B76); and one still holding something when its grace has run out
   (TI_LATE) gets -EINTR from every path call from then on, so that it soon holds nothing and the next look kills it
   (B79: the review measured such a task not killed 10 s after its kill, 11 times of 42; and one that never lets go
   between its calls, holding a file, is not killed at all, as B73 has it). 10/19/26 (audit B80, B81): for every killed
   task, at the start of every VFS call (a call on an open file too: fs/vfs.c); one that begins a call is to take
   something, and is marked so (TI_HELD: a grace owed), here after the look above. Not for a hangup's kill in a call,
   which waits for the call's end (its timer is never started again: B55's line is said once), nor a task ending by
   itself (TC_EXIT: its files close); and a step that must finish (fs_finish: umount, eject's unmount) is never
   refused */
int task_vfs_begin_slow(void) {
    surf_task *t = curTask;
    u_long flags = irq_save();
    int r = 0;
    if(((t->intr & (TI_HANGUP | TI_PATIENT)) && !(t->intr & TI_NOCALL)) || (t->in_call & TC_EXIT)) {
        /* (the call's end awaited, or the task ending: nothing here) */
    } else if(t->intr & TI_LATE) {
        if(!t->fs_finish) r = -EINTR;
    } else {
        if((t->intr & (TI_HELD | TI_GRACE)) == TI_HELD && !in_fs(t)) {
            t->intr |= TI_GRACE;
            ktimer_start(&t->kill_timer, KILL_WAIT_MS, false);
        }
        t->intr |= TI_HELD;
    }
    irq_restore(flags);
    return r;
}

/* a task of job (one that ended and is not collected yet counts: a pipeline's first program may end
   before the next joins its job), made by shell? */
int task_job_made(u_long job, u_long shell) {
    surf_task *t;
    u_long flags = irq_save();
    int found = 0;
    for(t = all_tasks; t && !found; t = t->all_next) found = t->job == job && t->job_shell == shell;
    irq_restore(flags);
    return found;
}

/* SYS_job's JOB_FG and JOB_BG: the tasks of job (made by shell, on tty) go on; how many */
int task_job_go_on(struct tty *tty, u_long job, int foreground, u_long shell) {
    surf_task *t;
    u_long flags = irq_save();
    int n = 0;
    for(t = all_tasks; t; t = t->all_next) {
        if(t->job != job || t->job_shell != shell || t->tty != tty || t->state == TS_DEAD) continue;
        go_on(t, foreground);
        n++;
    }
    irq_restore(flags);
    return n;
}

/* SYS_job's JOB_KILL: the tasks of job (made by shell, on tty) killed, the stopped ones too; how many */
int task_job_kill(struct tty *tty, u_long job, u_long shell) {
    surf_task *t;
    u_long flags = irq_save();
    int n = 0;
    for(t = all_tasks; t; t = t->all_next) {
        if(t->job != job || t->job_shell != shell || t->tty != tty || t->state == TS_DEAD) continue;
        kill_task(t);
        n++;
    }
    irq_restore(flags);
    return n;
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
        while(curTask->stop_req) {              /* 10/25/26: stopped: here, out of its call, until it goes on */
            task_now_stopped(curTask);
            curTask->state = TS_STOPPED;        /* a kill now makes it a zombie where it is */
            irq_restore(flags);
            yield();
            flags = irq_save();
        }
    }
    irq_restore(flags);
    return cut;
}

/* 10/25/26: Ctrl+C for a shell that looks for it itself (TF_INTR): it counted it already; a wait of
   its own in a system call ends (a kernel command it runs is cut short as any is). Interrupts off. */
static void tell_intr(surf_task *t) {
    if(t->in_call & TC_SYSCALL) {
        t->intr |= TI_INT;
        wake_intr(t);
    }
}

/* 10/18/26 (audit B54): Ctrl+C reached the kernel command, which finished all the same (an umount, eject's unmount:
   steps that must finish, B46): it is not cut short, so task_call_leave() does not say it was (status 130 under
   "safe to remove"). A kill is no Ctrl+C: it stays */
void task_interrupt_absorbed(void) {
    u_long flags;
    if(!curTask) return;
    flags = irq_save();
    curTask->intr &= ~TI_INT;
    irq_restore(flags);
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
   anything was reached. 10/25/26: the tasks of the job count the Ctrl+C (TTY_INTR), so each
   shell among them (a script that runs a script) learns of it and leaves what it was
   running; but not those above a shell that looks for Ctrl+C itself (TF_INTR: a surfsh
   typed at a surfsh's prompt), which takes it for what runs below it. Such a shell is not
   killed where it would be: it is told, and a wait of its own in a system call (a read of
   the terminal) is cut short; so is the owner when nothing else was reached, and the key
   goes no further. */
int task_interrupt_job(surf_task *owner) {
    surf_task *job[32], *t, *u;
    u_char above[32];
    int n = 0, i, j, hit = 0, depth;
    if(!owner || owner->state == TS_DEAD) return 0;
    for(t = all_tasks; t && n < 32; t = t->all_next)
        if(t->state != TS_DEAD && in_job(t, owner)) job[n++] = t;
    memset(above, 0, sizeof(above));
    for(j = 0; j < n; j++) {                        /* the shells above one that is told itself */
        if(!(job[j]->flags & TF_INTR)) continue;
        for(u = job[j]->parent, depth = 0; u && u != job[j] && depth < 64; u = u->parent, depth++) {
            for(i = 0; i < n && job[i] != u; i++);
            if(i == n) break;
            above[i] = 1;
        }
    }
    for(i = 0; i < n; i++) {
        t = job[i];
        if(!above[i] && t->ctrl_c < 0xffff) t->ctrl_c++;
        if(t->in_call & TC_COMMAND) {
            t->intr |= TI_INT;
            wake_intr(t);
            hit = 1;
            continue;
        }
        if(t == owner) continue;
        for(j = 0; j < n && job[j]->parent != t; j++);
        if(j < n) continue;                         /* it waits for a program of its own */
        if(t->flags & TF_INTR) {                    /* 10/25/26: a shell that is told instead */
            tell_intr(t);
            hit = 1;
            continue;
        }
        t->intr |= TI_INT;                          /* its line editor says ^C (tty.c) */
        kill_task(t);
        hit = 1;
    }
    if(!hit && (owner->flags & TF_INTR)) {
        tell_intr(owner);
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
   the hangup's kill (kill_hangup(): it waits for the end of the call, however long); how many.
   They are all processes (a kernel thread is never on a pty: task_alloc()). The calling task, if
   it is one of them, only inside a call (it ends at the call's end): otherwise it is ending already. */
u_int task_kill_tty(struct tty *tty) {
    surf_task *t;
    u_int n = 0;
    u_long flags = irq_save();
    for(t = all_tasks; t; t = t->all_next) {
        if(t->tty != tty || t->state == TS_DEAD || t == idle_task) continue;
        if(t == curTask && !t->in_call) continue;
        kill_hangup(t);
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
    fs_ref();                       /* 10/18/26 (audit B73): t is the waiter's to free now: a kill of it waits until */
    fd_close_all(t);                /* in the waiter's context: closing may write a file system */
    kcmd_task_gone(t);              /* 10/6/26: a kernel command that ended its task (crashgp from the user shell) */
    if(t->uvm) uvm_destroy(t->uvm);
    fpu_task_free(t);               /* 10/25/26 (L10) */
    kstack_free(t->stackmem);       /* only now: kcmd_task_gone() reads that stack */
    kfree(t);
    fs_unref();                     /* t freed (10/19/26, audit B75: the closes only were counted, and the kill that
                                       waited for them came in the rest, leaving t's address space, stack and record
                                       allocated for good) */
}

/* 10/25/26 (robustness, L22): kill_task_patient() for every process and ring-0 shell but the caller (task.h). Under
   interrupts off: a killed task's record stays on the list until it is collected, so the walk goes on. */
u_int task_end_users(void) {
    surf_task *t;
    u_int n = 0;
    u_long flags = irq_save();
    for(t = all_tasks; t; t = t->all_next) {
        if(t == curTask || t->state == TS_DEAD || !task_user_may_kill(t)) continue;
        kill_task_patient(t);
        n++;
    }
    irq_restore(flags);
    return n;
}

u_int task_users_left(char *list, size_t len) {
    surf_task *t;
    u_int n = 0;
    size_t used = 0;
    u_long flags = irq_save();
    if(len) list[0] = 0;
    for(t = all_tasks; t; t = t->all_next) {
        if(t->state == TS_DEAD || !task_user_may_kill(t)) continue;
        n++;
        if(len && used < len)
            used += snprintf(list + used, len - used, "%s'%s' (pid %lu)", used ? ", " : "", t->name, t->pid);
    }
    irq_restore(flags);
    return n;
}

/* 10/25/26 (robustness, L23): Ctrl+Alt+Del (kernel/shutdown.c), interrupts off: init's wait for its children is cut
   short (TI_INT), and init starts the shutdown */
void task_wake_init(void) {
    if(!init_task_ptr || init_task_ptr->state == TS_DEAD) return;
    init_task_ptr->intr |= TI_INT;
    wake_intr(init_task_ptr);
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

/* 10/25/26: is z the child a wait for pid (or any of job, when job) is for? Interrupts off. */
static bool wait_matches(surf_task *z, u_long pid, u_long job) {
    if(z->parent != curTask) return false;
    if(job) return z->job == job;
    return pid == WAIT_ANY || z->pid == pid;
}

/* 10/6/26: WNOHANG in options answers 0 at once when children are left but none has ended; with
   WINTR a kill of the caller (TI_KILL) ends the wait, -1. 10/25/26: how_out, how it ended (WX_*);
   job: any child of that job; WUNTRACED: a child that stopped is said too, once a stop */
static int wait_for(u_long pid, u_long job, int *status, u_int *flags_out, surf_console **con_out, int options,
                    int *how_out, int *job_out) {
    for(;;) {
        u_long flags = irq_save();
        surf_task *z, *found = NULL;
        bool have_children = false;

        for(z = zombies.first; z; z = z->next) {
            if(wait_matches(z, pid, job)) { found = z; break; }
        }
        if(found) {
            u_long fpid = found->pid;
            task_list_remove(&zombies, found);
            irq_restore(flags);
            if(status) *status = found->exit_code;
            if(how_out) *how_out = found->end_how;
            if(job_out) *job_out = (int)found->job;
            if(flags_out) *flags_out = found->flags;
            if(con_out) *con_out = found->con;
            reap(found);
            return (int)fpid;
        }
        for(z = all_tasks; z; z = z->all_next) {
            if(!wait_matches(z, pid, job) || z->state == TS_DEAD) continue;
            have_children = true;
            if((options & WUNTRACED) && z->stopped && !z->stop_seen) {     /* 10/25/26: one that stopped */
                z->stop_seen = 1;
                irq_restore(flags);
                if(status) *status = z->stop_why;
                if(how_out) *how_out = WX_STOPPED;
                if(job_out) *job_out = (int)z->job;
                return (int)z->pid;
            }
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
            if((curTask->intr & TI_KILL) || ((options & WCTRLC) && (curTask->intr & TI_INT))) {
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
    return wait_for(pid, 0, status, flags_out, con_out, 0, NULL, NULL);
}

int task_wait(u_long pid, int *status) {
    return wait_for(pid, 0, status, NULL, NULL, 0, NULL, NULL);
}

int task_wait_opts(u_long pid, int *status, int options) {
    return wait_for(pid, 0, status, NULL, NULL, options, NULL, NULL);
}

int task_wait_how(u_long pid, u_long job, int *status, int *how, int *job_out, int options) {
    return wait_for(pid, job, status, NULL, NULL, options, how, job_out);
}

/**** Blocking ****/

void yield(void) {
#ifdef SURFOS_RISCV32
    asm volatile("ecall" : : : "memory");   /* 10/15/26 (SurfCPU): riscv32's int $0x40 (arch/riscv32/trap.c) */
#else
    asm volatile("int $0x40"); //  :)
#endif
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

/* 10/6/26: a sleep that a kill or Ctrl+C cuts short (sleep, ping's pause, SYS_sleep). 10/25/26: a task
   that is to stop (job control) stops here, holding no lock, and sleeps on to the same end afterwards */
int sleep_intr(u_long ms) {
    u_long flags, end;
    if(!curTask || curTask == idle_task || in_interrupt()) {
        mdelay(ms);
        return 0;
    }
    end = getticks() + ms_to_ticks(ms);
    for(;;) {
        flags = irq_save();
        if(curTask->intr) {
            irq_restore(flags);
            return -EINTR;
        }
        if(curTask->stop_req && !curTask->mutexes && !curTask->crit) {
            task_now_stopped(curTask);
            curTask->state = TS_STOPPED;        /* go_on() makes it ready; a kill too */
            irq_restore(flags);
            yield();
            continue;
        }
        if((long)(getticks() - end) >= 0) {
            irq_restore(flags);
            return 0;
        }
        curTask->wake_at = end;
        curTask->state = TS_SLEEPING;
        curTask->wait_intr = 1;
        sleepers_insert(curTask);
        irq_restore(flags);
        yield();
    }
}

/* 10/25/26 (L7): frees the address spaces of tasks killed on the spot (a fault, Ctrl+C in a program's own code, a
   kill that waited too long), which become_zombie() hands over: it may run in an interrupt, where nothing is freed.
   This is a kernel thread, so schedule() has the kernel's directory loaded when it runs: none of them is loaded. */
static void uvmreap_thread(void *arg) {
    struct uvm *m;
    u_long flags, held;
    for(;;) {
        flags = irq_save();
        while(!uvms_dead) {
            wait_prepare(&uvmreap_wq);
            irq_restore(flags);
            yield();
            flags = irq_save();
        }
        m = uvms_dead;
        uvms_dead = m->reap_next;
        held = m->pages + m->table_pages;
        uvms_dead_pages -= held;                /* 10/25/26 (the review of B1): uvm_freeing() counts them now */
        irq_restore(flags);
        uvm_destroy(m);
    }
}

void task_mem_held(u_long *programs, u_long *dying) {
    surf_task *t;
    u_long flags = irq_save(), n = 0;
    for(t = all_tasks; t; t = t->all_next) {
        if(t->uvm) n += t->uvm->pages + t->uvm->table_pages;
        if(t->flags & TF_PROCESS) n += KSTACK_PAGES;        /* 10/25/26 (L17): its kernel stack, of their class too */
    }
    *programs = n;
    *dying = uvms_dead_pages + uvm_freeing();   /* 10/25/26 (the review of B1): handed to uvmreap, or being freed */
    irq_restore(flags);
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
#ifndef SURFOS_RISCV32                      /* 10/15/26 (SurfCPU): machine mode has no TSS and no ring change */
        tss_set_kernel_stack(next->stack_top);
        fpu_switch(next);                                   /* 10/25/26 (L10): CR0.TS unless it owns the FPU */
#endif
        if(next->uvm != uvm_current()) uvm_switch(next->uvm);   /* another process: its page directory (10/25/26: against
                                                       the one loaded: a dying task's may have gone to uvmreap) */
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
#define SHELL_LOW_FRAMES  256           /* 10/25/26 (the review of B1): fewer left for programs, and surfsh's ends
                                           are memory's: they do not count toward the recovery console */

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
    if(pid == -EAGAIN) {                                    /* 10/25/26 (the review of B4): SurfOS is shutting down */
        cshell[i].pid = 0;
        return;
    }
    if(pid == -ENOMEM || pid == -EIO) {                     /* it may work in a moment: init's loop tries again */
        cshell[i].pid = 0;
        if(!cshell[i].waiting) {
            snprintf(line, sizeof(line), "init: %s: %s; trying again\n", SURFSH_PATH, strerror(pid));
            init_say(i, line);
            cshell[i].waiting = true;
        }
        return;
    }
    if(pid == -ENOEXEC) {                                   /* 10/25/26: refused at once now; the same words */
        snprintf(cshell[i].why, sizeof(cshell[i].why), "init: %s could not be loaded; recovery console", SURFSH_PATH);
        recovery_console(i);
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
    if(!killed && (flags & TF_LOADNOMEM)) {                /* 10/25/26 (the review of B1): it may load in a moment */
        c->pid = 0;                                         /* init's loop tries again, as for a spawn's ENOMEM */
        if(!c->waiting) {
            snprintf(line, sizeof(line), "init: %s: %s; trying again\n", SURFSH_PATH, strerror(-ENOMEM));
            init_say(i, line);
            c->waiting = true;
        }
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
    if(pmm_room(PMM_USER) < SHELL_LOW_FRAMES) {             /* 10/25/26 (the review of B1): memory's end, not the */
        c->pid = 0;                                         /* shell's: not counted; a new one a second later */
        if(!c->waiting) {
            init_say(i, "init: surfsh ended while memory is short; trying again\n");
            c->waiting = true;
        }
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
#ifdef SURFOS_RISCV32
    bool kernel = true;     /* 10/15/26 (SurfCPU): machine mode runs no programs: the ring-0 shell everywhere */
#else
    bool kernel = bootinfo_has_arg(&bootinfo, "shell=kernel");
#endif
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
        for(i = 0; i < NUM_CONSOLES; i++) if(!cshell[i].pid && !shutdown_started()) missing = true;
        /* 10/25/26 (L23): the wait may be cut short by Ctrl+Alt+Del (task_wake_init()) */
        pid = wait_for(WAIT_ANY, 0, &status, &flags, &con, (missing ? WNOHANG : 0) | WINTR | WCTRLC, NULL, NULL);
        if(curTask->intr & TI_INT) {
            u_long irqf = irq_save();
            curTask->intr &= ~TI_INT;
            irq_restore(irqf);
            if(shutdown_key_take()) shutdown_start(SHUT_REBOOT);
        }
        if(pid <= 0) {
            /* 10/6/26: a console without a shell (no memory a moment ago): another try. No
               children at all should not happen, but never spin. */
            sleep_ms(missing ? SHELL_RETRY_MS : 100);
            for(i = 0; i < NUM_CONSOLES && !shutdown_started(); i++) if(!cshell[i].pid) start_shell(i);
            continue;
        }
        if(!(flags & TF_SHELL) || !con) continue;           /* an orphan: collected, that is all */
        if(shutdown_started()) continue;                    /* 10/25/26 (L22): no new shell while SurfOS shuts down */
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
    init_fpu(); //10/25/26 (L10): programs' floating point, switched on first use (kernel/fpu.c)

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
    wq_init(&uvmreap_wq);                   /* 10/25/26 (L7) */
    if(!kthread_create("uvmreap", uvmreap_thread, NULL, PL_HIGH, TF_KTHREAD | TF_DETACHED))
        panic("init_task: no memory for uvmreap");
}

/**** ps ****/

/* 10/6/26: task number index of the list, as SYS_procinfo tells it (surfos/proc.h): 0, or -ENOENT
   after the last. A dead task (not collected yet) has no terminal any more. */
int task_info(int index, struct procinfo *pi) {
    surf_task *t;
    u_long flags;
    int i = 0;
    if(index < 0) return -EINVAL;
    flags = irq_save();
    for(t = all_tasks; t && i < index; t = t->all_next) i++;
    if(!t) {
        irq_restore(flags);
        return -ENOENT;
    }
    fill_info(t, pi);
    irq_restore(flags);
    return 0;
}

/* 10/18/26 (the desktop's System Monitor): the whole list at once, each task as task_info() tells it, with
   interrupts off throughout, so that a task collected meanwhile cannot shift the rest; the newest max when
   there are more. How many were copied */
int task_info_all(struct procinfo *out, int max) {
    surf_task *t;
    u_long flags;
    int n = 0, skip;
    if(max <= 0) return 0;
    flags = irq_save();
    skip = (int)ntasks > max ? (int)ntasks - max : 0;   /* the list is in the order the tasks were made */
    for(t = all_tasks; t && n < max; t = t->all_next) {
        if(skip > 0) { skip--; continue; }
        fill_info(t, &out[n++]);
    }
    irq_restore(flags);
    return n;
}

/* a task as SYS_procinfo tells it; interrupts off */
static void fill_info(surf_task *t, struct procinfo *pi) {
    memset(pi, 0, sizeof(*pi));
    pi->pid = (int)t->pid;
    pi->ppid = t->parent ? (int)t->parent->pid : -1;
    switch(t->state) {
    case TS_READY:    pi->state = PROC_READY; break;
    case TS_RUNNING:  pi->state = PROC_RUNNING; break;
    case TS_BLOCKED:  pi->state = PROC_BLOCKED; break;
    case TS_SLEEPING: pi->state = PROC_SLEEPING; break;
    case TS_STOPPED:  pi->state = PROC_STOPPED; break;
    default:          pi->state = PROC_DEAD; break;
    }
    if(t->stopped && t->state != TS_DEAD) pi->state = PROC_STOPPED;    /* 10/25/26: in a wait that goes on */
    pi->flags = ((t->flags & TF_USER) ? PROC_USER : 0) | ((t->flags & TF_DETACHED) ? PROC_DETACHED : 0) |
                ((t->flags & TF_SHELL) ? PROC_SHELL : 0);
    pi->tty = (t->tty && t->state != TS_DEAD) ? tty_number(t->tty) : -1;
    strlcpy(pi->name, t->name, sizeof(pi->name));
    pi->cpu_ticks = (unsigned int)t->cpu_ticks;         /* 11/01/26 (top) */
    pi->switches = (unsigned int)t->switches;
    /* as task_mem_held() counts them, so that the tasks add up to its total. A task on the list has its address space
       or none at all: both ways of ending clear t->uvm with interrupts off before anything is freed */
    pi->mem_pages = (unsigned int)((t->uvm ? t->uvm->pages + t->uvm->table_pages : 0) +
                                   ((t->flags & TF_PROCESS) ? KSTACK_PAGES : 0));
}

/* 10/6/26: SYS_detach (surfos/proc.h): a process leaves its parent and the job of the shell that
   started it. init becomes its parent (and collects it), it is no longer in the foreground (Ctrl+C
   there does not reach it, task_interrupt_job()) nor paged (the pager follows parents), and a pager
   it owns is let go (its shell takes paging back). A parent waiting for it looks again: it is not
   its child any more. Its descriptors and its terminal stay. -EINVAL for a task that is no process.
   10/26/26 (CODE-AUDIT T14): nor is it its pty session's shell any more (TF_SESSION): a task that
   detached may not set its terminal's foreground job (sys_job). */
int task_detach(surf_task *t) {
    u_long flags;
    if(!t || !(t->flags & TF_USER) || !init_task_ptr) return -EINVAL;
    flags = irq_save();
    t->parent = init_task_ptr;
    t->flags = (t->flags | TF_DETACHED) & ~(TF_FOREGROUND | TF_SESSION);
    t->job = t->job_shell = 0;                  /* 10/25/26: out of its job too */
    if(t->con && t->con->page_owner == t) console_page_start(t->con, NULL);
    irq_restore(flags);
    wake_up_all(&zombie_wq);
    return 0;
}

/* 10/31/26: the list copied with interrupts off and printed with them on: printing with them off, a line behind the
   console lock of a task that cannot run then waited in the console's short list, and the screen lost what passed it */
void print_tasks() {
    struct task_row {
        u_long pid, cpu_ticks, switches;
        long parent;
        const char *state, *prio;
        bool user;
        char name[TASK_NAME_LEN];
    } *rows;
    surf_task *t;
    u_long flags;
    u_int n = 0, i, room = ntasks + 8;
    printf("\nSurfOS Task List\n");
    printf("--------------------------\n");
    printf("  PID  STATE     PRIO    CPU(ticks)  SWITCHES  PARENT  NAME\n");
    rows = (struct task_row *)kalloc(room * sizeof(struct task_row));
    if(!rows) {
        printf("  no memory for the list\n");
        return;
    }
    flags = irq_save();
    for(t = all_tasks; t && n < room; t = t->all_next, n++) {
        rows[n].pid = t->pid;
        rows[n].cpu_ticks = t->cpu_ticks;
        rows[n].switches = t->switches;
        rows[n].parent = t->parent ? (long)t->parent->pid : -1L;
        rows[n].state = t->stopped && t->state != TS_DEAD ? "stopped" : task_state_name(t->state);
        rows[n].prio = plNumToName(t->prio);
        rows[n].user = (t->flags & TF_USER) != 0;
        strlcpy(rows[n].name, t->name, TASK_NAME_LEN);
    }
    irq_restore(flags);
    for(i = 0; i < n; i++)
        printf("%5lu  %-8s  %-6s  %10lu  %8lu  %6ld  %s%s\n", rows[i].pid, rows[i].state, rows[i].prio, rows[i].cpu_ticks,
               rows[i].switches, rows[i].parent, rows[i].name, rows[i].user ? " (ring 3)" : "");
    kfree(rows);
    printf("\n%u tasks\n", n);
}
