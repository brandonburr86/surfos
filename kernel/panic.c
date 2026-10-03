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
*/

#include <surfos/system.h>
#include <surfos/types.h>
#include <surfos/panic.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>
#include <surfos/trap.h>
#include <surfos/irq.h>
#include <surfos/task.h>
#include <mm/paging.h>
#include <surfos/ksym.h>

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
        kprintf("\nKernel Wipeout: %s\n", why);
        dump_trapframe(tf);
        backtrace(tf->ebp, tf->eip);
        kprintf("\nSYSTEM HALTED\n");
        halt();
    }

    printf("\nProcess (\'%s\':%i) killed by \"%s\" (eip 0x%x%s)\n", curTask->name, curTask->pid, why, tf->eip,
           (tf->cs & 3) ? ", user mode" : "");
    if(!(tf->cs & 3)) backtrace(tf->ebp, tf->eip);     /* a user stack holds no kernel frames */

    kill_task(curTask); //it stays on its stack until the idle task reaps it
    return schedule(tf); //switch to the next task
}

/* Exceptions 0-31 (the page fault has its own handler in mm/paging.c) */
static u_long *trap_exception(struct trapframe *tf) {
    return trap_fatal(tf, "%s (vector %i, error 0x%x)", trap_name(tf->vector), tf->vector, tf->errcode);
}

/* int n for an n nobody installed, or a stray interrupt: say so and carry on */
u_long *trap_unexpected(struct trapframe *tf) {
    kprintf("\nUnexpected trap: vector 0x%x (%s), error 0x%x, eip 0x%x, ignored\n",
            tf->vector, trap_name(tf->vector), tf->errcode, tf->eip);
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

void panic_at(const char *file, int line, const char *what) {
    panic("%s:%i: %s", file, line, what);
}

void BUG() {
    panic("Woah.. this ain't supposdta happen.. a bug!");
}
