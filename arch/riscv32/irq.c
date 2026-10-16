/*
SurfOS riscv32 Interrupts
--------------------
File: irq.c     Date: 10/15/26 (SurfCPU)
--------------------
The interrupt system of include/surfos/interrupt.h on RISC-V machine mode. The handler
chains are the x86 kernel's (kernel/interrupt.c, by Brandon Burr and Martin McCormick):
several handlers may share a source, each is called in turn. What changes is beneath
them: no IDT, no 8259s and no EOI; every device interrupt arrives as the one machine
external interrupt, and the board's controller says which sources are pending
(board_irq_claim(): the PLIC's claim on QEMU virt, surf_intc's NEXT on the Arty) and
is told when one is done (the PLIC's complete; nothing on surf_intc, whose sources are
levels that drop when the device is served).

A source with no handler that still interrupts is switched off at the controller, so
a level that nobody clears cannot hold the CPU in an interrupt storm.
*/

#include <surfos/types.h>
#include <surfos/interrupt.h>
#include <surfos/irq.h>
#include <surfos/console.h>
#include <surfos/panic.h>
#include <mm/kalloc.h>
#include <riscv/csr.h>
#include <riscv/board.h>
#include <riscv/arch.h>
#include <blibc_common.h>

u_long irq_count[NR_IRQS];
u_long spurious_irq_count;
u_long timer_irq_count;
volatile int in_irq;

static struct sIRQHandler IRQBaseHandlers[NR_IRQS];

void _enable_irq(u_int irq) {
    if(irq < NR_IRQS) board_irq_enable(irq, true);
}

void _disable_irq(u_int irq) {
    if(irq < NR_IRQS) board_irq_enable(irq, false);
}

/* the machine software interrupt is not used: a stray one is cleared */
void arch_clear_soft_irq(void) {
    csr_clear(mie, MIE_MSIE);
}

/* The machine external interrupt (trap.c): every pending source, a bounded number of times. A
   level source still pending after 32 rounds has a handler that does not clear it: it is
   switched off, or it would hold the CPU here for good (the external interrupt outranks the
   timer's), and the kernel log says which. */
void irq_external(void) {
    int irq = -1, n;
    struct sIRQHandler *h;
    in_irq++;
    for(n = 0; n < 32; n++) {
        irq = board_irq_claim();
        if(irq < 0) break;
        if(irq >= NR_IRQS) {
            spurious_irq_count++;
            board_irq_complete(irq);
            continue;
        }
        irq_count[irq]++;
        if(!IRQBaseHandlers[irq].fHandler) {        /* nobody listens: off, before it storms */
            spurious_irq_count++;
            board_irq_enable(irq, false);
        }
        for(h = &IRQBaseHandlers[irq]; h; h = h->next) {
            if(h->bSet && h->fHandler) h->fHandler(irq, h->parameter);
        }
        board_irq_complete(irq);
    }
    if(n == 0) spurious_irq_count++;
    if(n == 32 && (irq = board_irq_claim()) >= 0) {
        board_irq_enable((u_int)irq, false);
        board_irq_complete((u_int)irq);
        kprintf("\n+IRQ: source %d stays pending although its handler ran 32 times: switched off\n", irq);
    }
    in_irq--;
}

void init_interrupt(void) {
    char buf[96];
    kprintf("\nInterrupt Initialization\n");
    memset(IRQBaseHandlers, 0, sizeof(IRQBaseHandlers));
    memset(irq_count, 0, sizeof(irq_count));
    spurious_irq_count = 0;
    timer_irq_count = 0;
    in_irq = 0;
    trap_init();
    board_irq_init();
    kprintf("*%s\n", board_irq_describe(buf, sizeof(buf)));
    csr_write(mie, MIE_MEIE);           /* the timer's MTIE comes with init_timer() */
    kprintf("*Interrupts Enabled\n");
    irq_enable();
}

/**** shared handler chains (as kernel/interrupt.c) ****/

void add_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter) {
    struct sIRQHandler *pHandler, *pNewHandler;
    u_long flags;

    if(irq >= NR_IRQS) return;
    pHandler = &IRQBaseHandlers[irq];
    flags = irq_save();
    if(!pHandler->fHandler) {
        pHandler->bSet = true;
        pHandler->fHandler = fHandler;
        pHandler->parameter = parameter;
        pHandler->next = 0;
    } else {
        while(pHandler->next) pHandler = pHandler->next;
        pNewHandler = (struct sIRQHandler *)kalloc(sizeof(struct sIRQHandler));
        if(!pNewHandler) {
            irq_restore(flags);
            return;
        }
        pNewHandler->bSet = true;
        pNewHandler->fHandler = fHandler;
        pNewHandler->parameter = parameter;
        pNewHandler->next = 0;
        pHandler->next = pNewHandler;
    }
    irq_restore(flags);
    _enable_irq(irq);
}

void del_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter) {
    struct sIRQHandler *pHandler, *pPrevHandler = 0;
    u_long flags;

    if(irq >= NR_IRQS) return;
    pHandler = &IRQBaseHandlers[irq];
    flags = irq_save();
    do {
        if(pHandler->fHandler == fHandler && pHandler->parameter == parameter) {
            if(pPrevHandler) {
                pPrevHandler->next = pHandler->next;
                kfree((void *)pHandler);
            } else if(pHandler->next) {
                struct sIRQHandler *pNext = pHandler->next;
                *pHandler = *pNext;
                kfree((void *)pNext);
            } else {
                pHandler->fHandler = 0;
                pHandler->parameter = 0;
                pHandler->bSet = false;
            }
            break;
        }
        pPrevHandler = pHandler;
        pHandler = pHandler->next;
    } while(pHandler);
    if(!IRQBaseHandlers[irq].fHandler) _disable_irq(irq);
    irq_restore(flags);
}

void enable_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter) {
    struct sIRQHandler *pHandler;
    if(irq >= NR_IRQS) return;
    for(pHandler = &IRQBaseHandlers[irq]; pHandler; pHandler = pHandler->next) {
        if(pHandler->fHandler == fHandler && pHandler->parameter == parameter && !pHandler->bSet) {
            pHandler->bSet = true;
            break;
        }
    }
}

void disable_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter) {
    struct sIRQHandler *pHandler;
    if(irq >= NR_IRQS) return;
    for(pHandler = &IRQBaseHandlers[irq]; pHandler; pHandler = pHandler->next) {
        if(pHandler->fHandler == fHandler && pHandler->parameter == parameter && pHandler->bSet) {
            pHandler->bSet = false;
            break;
        }
    }
}
