/*
SurfOS Interrupt System (riscv32)
----------------------
File: interrupt.h   Date: 10/15/26 (SurfCPU), the riscv32 face of include/surfos/interrupt.h
----------------------
The same handler chains as on x86 (several handlers may share a source), without the
IDT and the 8259s. An "irq" here is a source of the board's interrupt controller:
on the Arty, a bit of surf_intc (SURF_IRQ_UART 0, SURF_IRQ_PS2 1, ...); on QEMU's
virt machine, a PLIC source (the UART is 10). They all reach the CPU as the machine
external interrupt (mip.MEIP); the machine timer interrupt goes to timer_tick().
The PC's IRQ names (IRQ_KEYBOARD, IRQ_COM1, ...) are deliberately absent: a driver
written against them is a PC driver.
*/

#ifndef _SURFOS_INTERRUPT_H
#define _SURFOS_INTERRUPT_H

#include <surfos/types.h>
#include <surfos/trap.h>

#define NR_IRQS 32

void init_interrupt(void);

void _enable_irq(u_int irq);        /* the controller's enable bit for the source */
void _disable_irq(u_int irq);

/* IRQ handler chains: several handlers may share one source */
typedef int(*FuncIRQHandler)(u_int irq, void *parameter);

struct sIRQHandler {
    bool bSet;
    FuncIRQHandler fHandler;
    void *parameter;
    struct sIRQHandler *next;
};

void add_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter);
void del_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter);
void enable_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter);
void disable_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter);

/* the machine external interrupt: every pending source's chain (arch/riscv32/irq.c);
   the tick: a source that would not stop comes back */
void irq_external(void);
void irq_tick(void);

/* statistics and context */
extern u_long irq_count[NR_IRQS];
extern u_long spurious_irq_count;
extern u_long timer_irq_count;      /* machine timer interrupts (they have no source number) */
extern u_long irq_rests;            /* times a source was sent to the next tick */
extern volatile int in_irq;
#define in_interrupt() (in_irq > 0)

#endif
