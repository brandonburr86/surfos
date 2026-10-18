/*
SurfOS riscv32 Traps
--------------------
File: trap.c    Date: 10/15/26 (SurfCPU)
--------------------
trap_dispatch() is what arch/riscv32/traps.S calls for every trap, with the frame it
built on the interrupted stack; it returns the frame to resume. The policy is the
x86 kernel's (kernel/panic.c, kernel/interrupt.c):

  * the machine timer interrupt is the tick: timer_tick(), which may switch tasks;
  * the machine external interrupt runs the handler chains of every pending source of
    the board's interrupt controller (arch/riscv32/irq.c);
  * ECALL is yield(): mepc steps past it and the scheduler picks the next task, as
    int $0x40 does on x86;
  * EBREAK is reported and the task goes on after it (x86's crashint: an unexpected
    vector, reported and ignored);
  * any other exception kills the task that caused it, with a report and a backtrace;
    in the idle task, inside an interrupt handler, before there are tasks, or inside
    trap handling itself, nothing sane is left to run: a kernel wipeout. 10/18/26 (audit
    B56): so is one while the task is inside a file system, the block cache or a device
    request (task_in_fs(): kernel/panic.c says why); every task runs in machine mode here.

Machine mode has no memory protection: a wild store is not caught unless it lands on
an unmapped address or a read-only region. What is caught cheaply is kept: every
kernel stack has a canary word at its bottom, checked at every trap, and the
interrupted sp must lie inside the current task's stack.
*/

#include <surfos/types.h>
#include <surfos/trap.h>
#include <surfos/interrupt.h>
#include <surfos/irq.h>
#include <surfos/panic.h>
#include <surfos/task.h>
#include <surfos/timer.h>
#include <surfos/console.h>
#include <surfos/tty.h>
#include <surfos/ksym.h>
#include <surfos/system.h>
#include <sys/framebuffer.h>
#include <riscv/csr.h>
#include <riscv/arch.h>
#include <blibc_common.h>
#include <stdarg.h>

static trap_handler_t handlers[NR_VECTORS];
volatile u_long trap_depth;
u_long trap_counts[NR_VECTORS];

static const char *const exc_names[16] = {
    "instruction address misaligned", "instruction access fault", "illegal instruction", "breakpoint",
    "load address misaligned", "load access fault", "store address misaligned", "store access fault",
    "environment call from U-mode", "environment call from S-mode", "reserved (10)", "environment call from M-mode",
    "instruction page fault", "load page fault", "reserved (14)", "store page fault"
};

const char *trap_name(u_long mcause) {
    if(mcause & MCAUSE_INTR) {
        switch(MCAUSE_CODE(mcause)) {
        case IRQ_M_SOFT: return "machine software interrupt";
        case IRQ_M_TIMER: return "machine timer interrupt";
        case IRQ_M_EXT: return "machine external interrupt";
        }
        return "unknown interrupt";
    }
    return mcause < 16 ? exc_names[mcause] : "unknown exception";
}

void trap_set_handler(u_int vector, trap_handler_t handler) {
    if(vector < NR_VECTORS) handlers[vector] = handler;
}

/* The first frame of a new kernel task (kernel/task.c): traps.S's exit pops it and MRET
   starts pc with interrupts on (MPIE), in machine mode (MPP), sp at the top of its stack,
   gp the kernel's, ra and s0 zero so a backtrace ends there. The stack's bottom word
   becomes its canary. There is no user mode to start a task in. */
struct trapframe *arch_task_frame(u_char *stack_top, u_int ring, u_long pc) {
    struct trapframe *tf;
    if(ring != KERNEL) panic("arch_task_frame: no user mode on riscv32 (machine mode only)");
    if((u_long)stack_top & 15) panic("arch_task_frame: stack top %p is not 16-byte aligned", stack_top);
    tf = (struct trapframe *)(stack_top - sizeof(struct trapframe));
    memset(tf, 0, sizeof(*tf));
    tf->sp = (u_long)stack_top;
    tf->gp = rv_read_gp();
    tf->mepc = pc;
    tf->mstatus = MSTATUS_MPP | MSTATUS_MPIE;
    *(volatile u32 *)(stack_top - KSTACK_SIZE) = STACK_CANARY;
    return tf;
}

/* the current task's stack: its canary intact, and the interrupted sp inside it */
static void check_stack(struct trapframe *tf) {
    surf_task *t = curTask;
    u_long bottom;
    if(!t || !t->stack_top) return;
    bottom = t->stack_top - KSTACK_SIZE;
    if(*(volatile u32 *)bottom != STACK_CANARY) {
        *(volatile u32 *)bottom = STACK_CANARY;         /* once: the report itself runs on this stack */
        panic("kernel stack overflow in task '%s' (pid %lu): the canary at 0x%08lx is gone", t->name, t->pid, bottom);
    }
    if(tf->sp <= bottom || tf->sp > t->stack_top)
        panic("sp 0x%08lx is outside the stack of task '%s' (0x%08lx-0x%08lx)", tf->sp, t->name, bottom, t->stack_top);
}

/**** reports ****/

static const char *const reg_names[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

void dump_trapframe(struct trapframe *tf) {
    int i;
    kprintf("\n");
    kprintf(" mcause 0x%08lx (%s)  mepc 0x%08lx  mtval 0x%08lx  mstatus 0x%08lx\n",
            tf->mcause, trap_name(tf->mcause), tf->mepc, tf->mtval, tf->mstatus);
    for(i = 1; i < 32; i++) {
        kprintf(" %4s 0x%08lx%s", reg_names[i], TF_REG(tf, i), (i % 4 == 3) ? "\n" : " ");
    }
    kprintf("\n");
    if(curTask) kprintf(" task '%s' (pid %lu), in_irq %i\n", curTask->name, curTask->pid, in_irq);
    kprintf("\n");
}

/* The fault policy, 2004 style: kill the task that did it and move on, unless it was the
   idle task, an interrupt handler, or there is no task yet: then nothing sane is left to
   run and the kernel stops with a dump. */
u_long *trap_fatal(struct trapframe *tf, const char *fmt, ...) {
    char why[160];
    va_list ap;
    bool inside;

    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);

    inside = curTask && task_in_fs(curTask);     /* 10/18/26 (audit B56): kernel/panic.c says why */
    if(!curTask || curTask->pid == 0 || in_interrupt() || inside) {
        irq_disable();
        console_panic();
        kprintf("\nKernel Wipeout: %s\n", why);
        if(inside)
            kprintf("in '%s' (pid %lu) inside a file system, the block cache or a device request: what it held there (a\n"
                    "volume's lock, the cache's, a driver's, blocks under a device's I/O) cannot be given back\n",
                    curTask->name, curTask->pid);
        dump_trapframe(tf);
        backtrace(tf->s0, tf->mepc);
        kprintf("\nSYSTEM HALTED\n");
        halt();
    }

    console_capture_end();
    tty_task_gone(curTask);
    printf("\nProcess (\'%s\':%lu) killed by \"%s\" (pc 0x%lx)\n", curTask->name, curTask->pid, why, tf->mepc);
    backtrace(tf->s0, tf->mepc);
    framebuffer_task_died(curTask);
    kill_task_now(curTask);
    return schedule(tf);
}

static u_long *trap_exception(struct trapframe *tf) {
    return trap_fatal(tf, "%s (mcause %lu, mtval 0x%lx)", trap_name(tf->mcause), tf->mcause, tf->mtval);
}

/* EBREAK: said and stepped over (a debugger would take it; there is none) */
static u_long *trap_breakpoint(struct trapframe *tf) {
    kprintf("\nUnexpected trap: breakpoint at pc 0x%lx (task '%s'), ignored\n", tf->mepc, curTask ? curTask->name : "-");
    tf->mepc += 4;
    return (u_long *)tf;
}

/* a fault while a trap was being handled: interrupts are off there, so it is a bug in the
   kernel's own trap path. Said once with what can be said, then the hart stops. */
static void nested_fault(struct trapframe *tf) {
    if(trap_depth > 2) for(;;) rv_wfi();                /* the report faulted too */
    console_panic();
    kprintf("\nKernel Wipeout: %s inside trap handling (pc 0x%lx, mtval 0x%lx)\n",
            trap_name(tf->mcause), tf->mepc, tf->mtval);
    dump_trapframe(tf);
    backtrace(tf->s0, tf->mepc);
    kprintf("\nSYSTEM HALTED\n");
    halt();
}

/**** dispatch ****/

static u_long *interrupt_dispatch(struct trapframe *tf) {
    switch(MCAUSE_CODE(tf->mcause)) {
    case IRQ_M_TIMER:
        timer_irq_count++;
        return timer_tick(tf);              /* outside in_irq, as IRQ 0 on x86 */
    case IRQ_M_EXT:
        irq_external();
        if(curTask && curTask->state == TS_DEAD) return schedule(tf);  /* Ctrl+C killed what it interrupted */
        return (u_long *)tf;
    default:
        spurious_irq_count++;
        arch_clear_soft_irq();
        return (u_long *)tf;
    }
}

u_long *trap_dispatch(struct trapframe *tf) {
    u_long cause = tf->mcause, *ret;
    trap_depth++;
    if(trap_depth > 1) nested_fault(tf);
    check_stack(tf);
    if(cause & MCAUSE_INTR) {
        ret = interrupt_dispatch(tf);
    } else {
        if(cause < NR_VECTORS) trap_counts[cause]++;
        if(cause == CAUSE_ECALL_M) tf->mepc += 4;       /* resume after the ecall */
        if(cause < NR_VECTORS && handlers[cause]) ret = handlers[cause](tf);
        else ret = trap_exception(tf);
    }
    trap_depth--;
    return ret;
}

/* Every cause without a handler is fatal (trap_exception()). ECALL is yield(): kernel/task.c
   installs that handler (T_YIELD) in init_task(); an ecall before there is a scheduler is a bug. */
void trap_init(void) {
    handlers[CAUSE_BREAKPOINT] = trap_breakpoint;
    csr_write(mtvec, (u_long)trap_entry);
    kprintf("*mtvec 0x%08lx (direct mode), %u exception causes handled\n", csr_read(mtvec), (u_int)NR_VECTORS);
}
