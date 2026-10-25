/*
SurfOS Floating Point for Programs
--------------------
File: fpu.c     Date: 10/25/26 (robustness, track B: L10-L13)
--------------------
Every process has floating-point registers of its own: the x87 stack with its control and
status words, and with SSE the XMM registers and MXCSR. Until now nothing saved them, so two
programs using floating point at once corrupted each other's numbers.

They are switched lazily. The FPU holds one task's registers (the owner). schedule() sets
CR0.TS whenever it switches to any other task (fpu_switch(), which keeps a shadow of the bit so
that CR0 is written only when it changes), and that task's first floating-point or SSE
instruction then raises the device-not-available trap (#NM, vector 7). The trap clears TS,
saves the owner's registers in the owner's area (fxsave, or fnsave on a CPU without FXSR), loads
the current task's (fxrstor, frstor) and returns to the instruction. A program that never uses
floating point never pays for it; one that uses it while the others do not pays once.

A process's area (FPU_AREA bytes, kalloc'd 16-aligned when the process is created, freed when
it is collected) starts as every program's registers start: the x87 control word 0x037F and
MXCSR 0x1F80 (every exception masked, rounding to nearest), the stack empty, every register
zero, so no program sees another's values (fninit alone would leave the old ones in the
registers, where fxsave shows them). Kernel threads have no area.

The kernel never uses the FPU. It is built with -mno-80387 and -mno-sse (Makefile), so a float in
kernel code fails to link (the kernel has no libgcc); interrupt handlers therefore never touch a
program's registers, and a #NM from ring 0 is a kernel bug: a panic. Nothing here allocates or
waits: the trap runs with interrupts off (an interrupt gate), as schedule() does.

10/25/26 (the review of B2): when no task owns the FPU, the trap clears the exceptions left
pending in it before loading the current task's registers (fnclex), so that a program that
died of an x87 exception cannot make frstor fault in ring 0 for every program after it.
*/

#include <surfos/types.h>
#include <surfos/fpu.h>
#include <surfos/task.h>
#include <surfos/trap.h>
#include <surfos/cpu.h>
#include <surfos/panic.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <blibc_common.h>

static surf_task *owner;        /* whose registers the FPU holds now; NULL: nobody's */
static u_long saves;            /* registers saved for another task's (cpuinfo; the self test's test_fpu) */
static bool ts_on;              /* CR0.TS as fpu_switch() and the trap last left it (clear at boot: init_cpu()) */
static u8 fresh[FPU_AREA] __attribute__((aligned(16)));    /* a program's registers at its start */

static void fpu_save(void *area) {
    if(cpu.fxsr) asm volatile("fxsave (%0)" : : "r"(area) : "memory");
    else asm volatile("fnsave (%0)" : : "r"(area) : "memory");     /* and fninit, which the restore undoes */
}

static void fpu_restore(void *area) {
    if(cpu.fxsr) asm volatile("fxrstor (%0)" : : "r"(area) : "memory");
    else asm volatile("frstor (%0)" : : "r"(area) : "memory");
}

/* #NM: the current task's first floating-point instruction since it was switched to */
static u_long *fpu_trap(struct trapframe *tf) {
    surf_task *t = curTask;
    if(!(tf->cs & 3)) panic("FPU used by the kernel (eip 0x%lx, task '%s')", tf->eip, t ? t->name : "none");
    if(!t || !t->fpu) return trap_fatal(tf, "floating point in a task that has no registers for it");
    asm volatile("clts" : : : "memory");
    ts_on = false;
    if(owner != t) {
        if(owner) {
            fpu_save(owner->fpu);
            saves++;
        } else {
            /* the registers of a program that ended are there, with an exception pending if it raised one (the #MF
               that killed it). frstor checks for one first, as every x87 instruction but the FN* ones does, and would
               raise it here, in ring 0, for each program to come; fnsave clears it on the other branch. Not at the
               end (fpu_task_gone()): CR0.TS may be set there */
            asm volatile("fnclex");
        }
        fpu_restore(t->fpu);
        owner = t;
    }
    return (u_long *)tf;
}

void init_fpu(void) {
    memset(fresh, 0, sizeof(fresh));
    fresh[0] = 0x7F;                            /* the control word, 0x037F, first in both layouts */
    fresh[1] = 0x03;
    if(cpu.fxsr) {
        if(cpu.sse) {                           /* MXCSR at 24: 0x1F80 */
            fresh[24] = 0x80;
            fresh[25] = 0x1F;
        }                                       /* the abridged tag word at 4: 0, every register empty */
    } else {
        fresh[8] = fresh[9] = 0xFF;             /* fnsave's tag word at 8: 0xFFFF, every register empty */
    }
    trap_set_handler(T_DEVICE, fpu_trap);
}

int fpu_task_new(surf_task *t) {
    t->fpu = kalloc(FPU_AREA);
    if(!t->fpu) return -ENOMEM;
    memcpy(t->fpu, fresh, FPU_AREA);
    return 0;
}

void fpu_task_gone(surf_task *t) {
    if(owner == t) owner = NULL;                /* what the FPU holds is nobody's now: never saved */
}

void fpu_task_free(surf_task *t) {
    kfree(t->fpu);
    t->fpu = NULL;
}

u_long fpu_saves(void) {
    return saves;
}

void fpu_switch(surf_task *next) {
    bool ts = next != owner;
    u_long cr0;
    if(ts == ts_on) return;
    if(ts) {
        asm volatile("movl %%cr0, %0" : "=r"(cr0));
        asm volatile("movl %0, %%cr0" : : "r"(cr0 | CR0_TS) : "memory");
    } else {
        asm volatile("clts" : : : "memory");
    }
    ts_on = ts;
}
