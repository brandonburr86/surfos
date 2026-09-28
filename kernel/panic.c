/*
SurfOS Exception Handling
--------------------
File: panic.c   Date: Prior to 4/23/04
--------------------
(C)2004 Brandon Burr
*/


#include <surfos/system.h>
#include <surfos/types.h>
#include <surfos/panic.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>
#include <mm/memory.h>
#include <surfos/task.h>
#include <surfos/timer.h>

#include <blibc_common.h>

/********* Exception C handlers ******/
void exDivZero(surf_regs *curReg) {
    panic(curTask,"Divide by zero");
}

void exDebug(surf_regs *curReg) {
    panic(curTask,"Debug Exception");
}

void exNMI(surf_regs *curReg) {
    panic(curTask,"NMI Error");
}

void exBreakpoint(surf_regs *curRegs) {
    panic(curTask,"Breakpoint");
}


void exOverflow(surf_regs *curReg) {
    panic(curTask,"Stack Overflow");
}

void exBoundRange(surf_regs *curReg) {
    panic(curTask,"Bound Range Exception");
}

void exInvalOpcode(surf_regs *curRegs) {
    panic(curTask,"Invalid Opcode");
}

void exDevNotAvailable(surf_regs *curReg) {
    panic(curTask,"Device Not Available");
}

void exDoubleFault(surf_regs *curReg) {
    panic(curTask,"Double Fault");
}

void exCoprocSeg(surf_regs *curReg) {
    panic(curTask,"Coprocessor Segment Error");
}

void exInvalTSS(surf_regs *curReg) {
    panic(curTask,"Invalid TSS");
}

void exSegNotPresent(surf_regs *curReg) {
    panic(curTask,"Segment Not Present");
}

void exStackFault(surf_regs *curReg) {
    panic(curTask,"Stack Fault");
}

void exGPF(surf_regs *curReg) {
    panic(curTask,"General Protection Fault");
}

void exFPError(surf_regs *curReg) {
    panic(curTask,"Floating Point Error");
}

void exAlignCheck(surf_regs *curReg) {
    panic(curTask,"Alignment Check");
}

void exMachineCheck(surf_regs *curReg) {
    panic(curTask,"Machine Check");
}

void exSIMDFP(surf_regs *curReg) {
    panic(curTask,"SIMD Floating Point Error");
}

#define ISRPTR(func) ((u_int*)(func))
void init_exceptions() {
    _set_idt_int(0,ISRPTR(ex0));
    _set_idt_int(1,ISRPTR(ex1));
    _set_idt_int(2,ISRPTR(ex2));
    _set_idt_int(3,ISRPTR(ex3));
    _set_idt_int(4,ISRPTR(ex4));
    _set_idt_int(5,ISRPTR(ex5));
    _set_idt_int(6,ISRPTR(ex6));
    _set_idt_int(7,ISRPTR(ex7));
    _set_idt_int(8,ISRPTR(ex8));
    _set_idt_int(9,ISRPTR(ex9));
    _set_idt_int(10,ISRPTR(ex10));
    _set_idt_int(11,ISRPTR(ex11));
    _set_idt_int(12,ISRPTR(ex12));
    _set_idt_int(13,ISRPTR(ex13));
    _set_idt_int(14,ISRPTR(ex14));
    _set_idt_int(15,ISRPTR(ex15));
    _set_idt_int(16,ISRPTR(ex16));
    _set_idt_int(17,ISRPTR(ex17));
    _set_idt_int(18,ISRPTR(ex18));
  kprintf("Exceptions loaded\n");
}

void panic(surf_task *task, char *msg) {
    asm("cli");

    if(!task) {
        kprintf("Kernel panic: panic() issued on null task\n");
        halt();
        return;
    }

    if(task->pid == 0) { //a kernel exception.. beadd
        kprintf("\nKernel Wipeout: %s\n",msg);
        dump_regs((surf_regs*)task->esp);
        halt();
    }

    printf("\nProcess (\'%s\':%i) killed by \"%s\"\n",task->name,task->pid,msg);

    kill_task(task);

    asm("sti");
    asm("nop");

    yield(); //switch to the next task
}

void BUG() {
    kprintf("Woah.. this ain't supposdta happen.. a bug!\n");
    kprintf("\nSYSTEM HALTED\n");
    asm("cli");
    asm("hlt");
}

extern surf_task *curTask;

void dump_regs(surf_regs *stk) {
    kprintf("\n\nStack dump for process: \"%s (%i)\"\n",curTask->name,curTask->swapCount);
    kprintf(" CS:  0x%x  DS:  0x%x  ES:  0x%x  GS:  0x%x  FS:  0x%x\n",stk->cs,stk->ds,stk->es,stk->gs,stk->fs);
    kprintf(" EAX: 0x%x  EBX: 0x%x  ECX: 0x%x  EDX: 0x%x\n",stk->eax,stk->ebx,stk->ecx,stk->edx);
    kprintf(" EIP: 0x%x  ESP: 0x%x  EBP: 0x%x  EDI: 0x%x  ESI: 0x%x\n",stk->eip,stk->esp,stk->ebp,stk->edi,stk->esi);
    kprintf("\n");
}
