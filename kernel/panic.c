/*
SurfOS Exception Handling
--------------------
File: panic.c   Date: Prior to 4/23/04, rebuilt 10/2026 (roadmap K1)
--------------------
(C)2004 Brandon Burr

The eighteen exN stubs and exDivZero()..exSIMDFP() became one handler on the trap
dispatch table. The policy is the 2004 one: an exception in a task kills that task
and the scheduler moves on; an exception in the idle task or inside an interrupt
handler is a kernel wipeout.

10/6/26: a kernel command run by the user shell keeps its output for a file while
the shell's descriptor 1 is redirected (SYS_kcmd, console_capture_begin()); a fault
report or a panic is never kept there, it goes to the screen.

10/25/26 (robustness, L16, L18): the double fault's report, from a task of its own: a
kernel stack overflow, or a stack that trap_dispatch() found nearly full.
*/

#include <surfos/system.h>
#include <surfos/types.h>
#include <surfos/panic.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>
#include <surfos/trap.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <surfos/apic.h>
#include <surfos/tty.h>
#include <sys/framebuffer.h>
#include <mm/paging.h>
#include <mm/memory.h>
#include <surfos/ksym.h>
#include <surfos/gdt.h>

#include <blibc_common.h>
#include <stdarg.h>

void dump_trapframe(struct trapframe *tf) {
    u_long cr2;
    asm volatile("movl %%cr2, %0" : "=r"(cr2));

    kprintf("\n");
    kprintf(" vector %i (%s)  error code 0x%x  eflags 0x%x\n", tf->vector, trap_name(tf->vector), tf->errcode, tf->eflags);
    kprintf(" EIP: 0x%x  CS: 0x%x  SS:ESP: 0x%x:0x%x\n", tf->eip, tf->cs,
            (tf->cs & 3) ? tf->ss : 0x10, (tf->cs & 3) ? tf->useresp : (u_long)&tf->useresp);
    kprintf(" EAX: 0x%x  EBX: 0x%x  ECX: 0x%x  EDX: 0x%x\n", tf->eax, tf->ebx, tf->ecx, tf->edx);
    kprintf(" ESI: 0x%x  EDI: 0x%x  EBP: 0x%x  CR2: 0x%x\n", tf->esi, tf->edi, tf->ebp, cr2);
    kprintf(" DS: 0x%x  ES: 0x%x  FS: 0x%x  GS: 0x%x\n", tf->ds, tf->es, tf->fs, tf->gs);
    if(curTask) kprintf(" task '%s' (pid %i), in_irq %i\n", curTask->name, curTask->pid, in_irq);
    kprintf("\n");
}

/* The fault policy, 2004 style: kill the task that did it and move on, unless it was
   the idle task, an interrupt handler, or there is no task yet: then nothing sane is
   left to run and the kernel stops with a dump. */
u_long *trap_fatal(struct trapframe *tf, const char *fmt, ...) {
    char why[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);

    if(!curTask || curTask->pid == 0 || in_interrupt()) { //a kernel exception.. beadd
        irq_disable();
        console_panic(); //whoever was drawing will not finish: the report must reach the screen, as plain text
        kprintf("\nKernel Wipeout: %s\n", why);
        dump_trapframe(tf);
        backtrace(tf->ebp, tf->eip);
        kprintf("\nSYSTEM HALTED\n");
        halt();
    }

    console_capture_end(); //10/6/26: not into a redirected kernel command's file (SYS_kcmd)
    tty_task_gone(curTask); //raw mode, colours and a hidden cursor it set end here, before the report
    printf("\nProcess (\'%s\':%i) killed by \"%s\" (eip 0x%x%s)\n", curTask->name, curTask->pid, why, tf->eip,
           (tf->cs & 3) ? ", user mode" : "");
    if(!(tf->cs & 3)) backtrace(tf->ebp, tf->eip);     /* a user stack holds no kernel frames */

    framebuffer_task_died(curTask); //it may have been drawing (a framebuffer console)
    kill_task_now(curTask); //it stays on its stack until the idle task reaps it (10/6/26: at once, also inside a system call)
    return schedule(tf); //switch to the next task
}

/* Exceptions 0-31 (the page fault has its own handler in mm/paging.c) */
static u_long *trap_exception(struct trapframe *tf) {
    return trap_fatal(tf, "%s (vector %i, error 0x%x)", trap_name(tf->vector), tf->vector, tf->errcode);
}

/* int n for an n nobody installed, or a stray interrupt: say so and carry on. With the
   local APIC on (profile=laptop) a stray one may be a device's message-signalled interrupt
   that the firmware left enabled: acknowledging it keeps the lower vectors flowing (an EOI
   with nothing in service does nothing). */
u_long *trap_unexpected(struct trapframe *tf) {
    kprintf("\nUnexpected trap: vector 0x%x (%s), error 0x%x, eip 0x%x, ignored\n",
            tf->vector, trap_name(tf->vector), tf->errcode, tf->eip);
    if(tf->vector >= 32) apic_eoi();   /* ring 3 cannot raise these with int n (DPL 0 gates fault) */
    return (u_long*)tf;
}

void init_exceptions() {
    int i;
    for(i = 0; i < 32; i++) trap_set_handler(i, trap_exception);
    trap_set_handler(T_PGFLT, page_fault_trap);
    kprintf("*Exceptions loaded\n");
}

void panic(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    u_long ebp;

    irq_disable();
    console_capture_end(); //10/6/26: nor in a redirected kernel command's file (SYS_kcmd)
    console_panic(); //a panic must be visible on a framebuffer console too, and not in an editor's colours
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    kprintf("\nKernel panic: %s\n", buf);
    if(curTask) kprintf("task '%s' (pid %i), in_irq %i\n", curTask->name, curTask->pid, in_irq);
    asm volatile("movl %%ebp, %0" : "=r"(ebp));
    backtrace(ebp, 0);
    kprintf("\nSYSTEM HALTED\n");
    halt();
}

struct trapframe *stack_low_frame;

/* 10/25/26 (robustness, L16, L18): the double fault's task (kernel/gdt.c, kernel/traps.asm), on a stack of its own,
   with interrupts off; what it interrupted is in the main TSS. A kernel stack that ran into its guard (kernel/task.c)
   faults again while the CPU pushes the page fault's frame there, which makes a double fault: said with the task's
   name, from the fault's address (CR2, in the guard) or the stack pointer. trap_dispatch() sends a stack that is
   nearly full here too (int $8: stack_low_frame), since its report would not fit on what is left. Nothing is
   recovered: an interrupt may have been in progress, and locks may be held. The report goes to the console shown
   and the serial line (in_irq: as an interrupt handler's kprintf() does), whichever console the task had. */
void double_fault_task(void) {
    const struct tss_entry *ts = tss_main();
    struct trapframe *tf = stack_low_frame;
    surf_task *t = NULL;
    bool guard = false;
    u_long cr2;

    asm volatile("movl %%cr2, %0" : "=r"(cr2));
    in_irq++;
    console_capture_end();
    console_panic();
    if(tf && curTask) {
        kprintf("\nKernel panic: the kernel stack of '%s' (pid %lu) is nearly full: a trap frame %lu bytes above "
                "its bottom\n", curTask->name, curTask->pid, (u_long)tf - task_stack_bottom(curTask));
        kprintf(" vector %lu (%s)  EIP: 0x%x  CS: 0x%x  EFLAGS: 0x%x  EBP: 0x%x  in_irq %i\n", tf->vector,
                trap_name(tf->vector), tf->eip, tf->cs, tf->eflags, tf->ebp, in_irq - 1);
        backtrace(tf->ebp, tf->eip);
    } else {
        t = task_stack_owner(cr2, &guard);
        if(!guard) t = task_stack_owner(ts->esp, &guard);
        if(guard) kprintf("\nkernel stack overflow in '%s' (pid %lu)\n", t ? t->name : "?", t ? t->pid : 0);
        else kprintf("\nKernel Wipeout: Double Fault in '%s' (pid %lu)\n", curTask ? curTask->name : "?",
                     curTask ? curTask->pid : 0);
        kprintf(" EIP: 0x%x  ESP: 0x%x  EBP: 0x%x  CR2: 0x%x  EFLAGS: 0x%x  CS: 0x%x\n", ts->eip, ts->esp, ts->ebp,
                cr2, ts->eflags, ts->cs);
        if(t) kprintf(" its stack: 0x%x-0x%x, the guard below it from 0x%x\n", task_stack_bottom(t), t->stack_top,
                      task_stack_bottom(t) - (KSTACK_SLOT - KSTACK_SIZE));
        backtrace(ts->ebp, ts->eip);
    }
    kprintf("\nSYSTEM HALTED\n");
    halt();
}

void panic_at(const char *file, int line, const char *what) {
    panic("%s:%i: %s", file, line, what);
}

void BUG() {
    panic("Woah.. this ain't supposdta happen.. a bug!");
}
