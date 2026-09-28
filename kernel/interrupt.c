/*
SurfOS Core Interrupt System
--------------------
File: interrupt.c   Date: June 2004
--------------------
(c)2004 Brandon Burr and Martin McCormick

MM - June 20th update:
    + Specific EOI Ack instead of general (0x60 vs 0x20)
    + Full slave PIC support
    + Added one handler function for all IRQs (one calling stub)
    + Dynamic IRQ handling

MM - June 28th update:
    + Shared IRQs (multiple handlers per IRQ)
    + Fixed race conditions

MM - June 30th update:
    + Individual shared IRQ remove support added.
    + Added support for critical enable/disable interrupt blocks using turf descriptors
    + Added disable / enabling of handler
    + Critical IRQ blocks now have seperate functions (less to cache)

MM - July 2nd update:
     + Moved new interrupt system into SurfOS officialy
*/

#include <asm/io.h>
#include <surfos/panic.h>
#include <blibc_common.h>
#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>
#include <surfos/turf.h>

#include <mm/kalloc.h>

static u_int cached_irq_mask;         /* globally remembered irq mask (mask is write only) */
struct sIRQHandler IRQBaseHandlers[NR_IRQS+1]; /* these are the base handlers for each IRQ (linked list attaches to)*/
void *turfIRQ; /* see header file for more info */
/* The rules below are optimized for best performance.  You probably don't want to mess
    with them too much */
/* remember: 1 = disallow new turf treading, 0 = allow */
u_int turfIRQRules[TURF_IRQ_RULE_COUNT][TURF_IRQ_RULE_COUNT] =  {
       /* requested */  /* enab? */ /* disa? */ /* cenb? */ /* cdis? */ /* <- allowed during one of these ? */
        /* 0 - enab */   {0,          0,           1,           0},
        /* 1 - disa */   {0,          0,           0,           1},
        /* 2 - cenb */   {0,          1,           1,           0},
        /* 3 - cdis*/    {1,          0,           0,           1}
};


void init_interrupt() {
    int i;
    struct sIDTMaster idtIDTR;
    idtIDTR.limit = (u_short)(256 * 8)-1;     /* all 256 possible interrupts */
    idtIDTR.low_base = (u_short) IDT_LOWBASE;
    idtIDTR.high_base = (u_short) IDT_HIGHBASE;
    load_idtr(&idtIDTR);

    cached_irq_mask = 0xffff;

    outb(0x21, 0xff);       /* mask everything for master */
    outb(0xA1, 0xff);       /* mask everything for slave */

    outb(0x20, 0x11);       /* ICW1: expect ICW4*/
    outb(0x21, 0x20 + 0);   /* ICW2: IRQ 0-7 mapped to 0x20-0x27 */
    outb(0x21, 0x04);       /* ICW3: The master has a slave on IRQ 2 */
    outb(0x21, 0x01);       /* ICW4: 8086 mode */

    outb(0xA0, 0x11);       /* ICW1: expect ICW4*/
    outb(0xA1, 0x20 + 8);   /* ICW2: IRQ 0-7 mapped to 0x28-0x2f */
    outb(0xA1, 0x02);       /* ICW3: slave on master's IRQ 2 */
    outb(0xA1, 0x01);       /* ICW4: 8086 mode */

    irq_delay(100);        /* wait for initialize */

    outb(0x21,cached_21);   /* restore master mask */
    outb(0xA1,cached_A1);   /* restore slave mask */

    init_exceptions();

    /* initialize the irq turf */
    turfIRQ = turfCreate(TURF_IRQ_RULE_COUNT, (u_int*)turfIRQRules);

    _enable_irq(2); /* for slave 8259A */

    for(i=1;i<NR_IRQS;i++) {/* set asm entry points in IDT, and init handlers */
        /* set IRQ's interrupts to asm stubs which call handlers */
        /* (the assembly entry points are all consecutive and equally spaced) */
        _set_idt_int(0x20 + i, (u_int*)(&isrStartIRQ + (((&isrEndIRQ - &isrStartIRQ) / NR_IRQS) * (i-1))));

        IRQBaseHandlers[i].fHandler = 0;
        IRQBaseHandlers[i].bSet = 0;
        IRQBaseHandlers[i].parameter = 0;
    }

    kprintf("Interrupts Enabled");
    asm("sti");
}

void _set_idt_int(int num, u_int* address) {
    _set_idt_entry(num,(u_int)address,CS_SELECTOR,INT_GATE|IDT_PRESENT|INT_RING0|BITS_32);
}

void _set_idt_trap(int num, u_int* address) {
    _set_idt_entry(num,(u_int)address,CS_SELECTOR,TRAP_GATE|IDT_PRESENT|INT_RING0|BITS_32);
}

void _set_idt_entry(int num, u_int address, u_short selector, u_char opt) {

    idt_entry *idt_ptr  = (idt_entry*)IDTBASE + num;

    u_short loff ,uoff;
    loff = address & 0x0000FFFF;
    uoff = (address & 0xFFFF0000) >>16;

    idt_ptr->loffset = loff;
    idt_ptr->selector = selector;
    idt_ptr->unused = 0x00;
    idt_ptr->options = opt;
    idt_ptr->uoffset = uoff;
}

void load_idtr(struct sIDTMaster *IDTMaster) {
    asm volatile("lidt (%0) ": :"p" (IDTMaster));
}

void _disable_irq_enter_turf(unsigned int irq) {
    turfTread(turfIRQ, TURF_IRQ_CRIT_DISABLE);
    /* actually do a disable: */
    u_int mask = 1 << irq;
    cached_irq_mask |= mask;

    if (irq & 8) outb(0xA1, cached_A1);
    else outb(0x21, cached_21);

    return;
}

void _disable_irq_exit_turf(u_int irq) {
    turfLeave(turfIRQ, TURF_IRQ_CRIT_DISABLE);
    return; /* responsibility of caller to restore previous int state */

}
void _disable_irq(u_int irq) {
    turfTread(turfIRQ, TURF_IRQ_DISABLE); /* must always enter the normal disable turf */

   /* actually do a disable: */
   u_int mask = 1 << irq;
   cached_irq_mask |= mask;
   if (irq & 8) outb(0xA1, cached_A1);
   else outb(0x21, cached_21);

   turfLeave(turfIRQ, TURF_IRQ_DISABLE); /* done */

    return;
}

void _enable_irq_enter_turf(u_int irq) {
    turfTread(turfIRQ, TURF_IRQ_CRIT_ENABLE);

     /* actually do the enable: */
     u_int mask = ~(1 << irq);
     cached_irq_mask &= mask;
     if (irq & 8) outb(0xA1, cached_A1);
     else outb(0x21, cached_21);

    return;
}

void _enable_irq_exit_turf(u_int irq) {
    turfLeave(turfIRQ, TURF_IRQ_CRIT_ENABLE);
    return; /* responsibility of caller to restore previous int state */
}

/*
    Interrupts.... had a few problems w/ these.... but they work now!

The PIC has the ability to be re-programmed to use a different set of
interrupt values than the default ones. The default PIC values are;

PIC IRQ INT
0  0  8
0  1  9
0  2  A
0  3  B
0  4  C
0  5  D
0  6  E
0  7  F
1  8  70
1  9  71
1  A  72
1  B  73
1  C  74
1  D  75
1  E  76
1  F  77
*/

void _enable_irq(u_int irq) {
    turfTread(turfIRQ, TURF_IRQ_ENABLE); /* must always enter the normal enable turf */

    /* actually do the enable: */
    u_int mask = ~(1 << irq);
    cached_irq_mask &= mask;
    if (irq & 8) outb(0xA1, cached_A1);
    else outb(0x21, cached_21);
    turfLeave(turfIRQ, TURF_IRQ_ENABLE); /* done */

    return;
}

void irq_delay(u_long loops) { //Comment added 7/2/04: Brandon SMACKS Martin
    u_long i, j;
    for(i=0; i<loops; i++) {
        for (j=0; j<loops; j++) {
        asm("nop");
      }
    }
}

void add_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter) {
    struct sIRQHandler * pHandler = &IRQBaseHandlers[irq]; /* select base handler for requested irq */
    struct sIRQHandler * pNewHandler;

    if(!pHandler->fHandler)  {
        /* Head not used - so use it!  Unfortunately we can't access it atomically... */
        _disable_irq_enter_turf(irq);  /* don't want handler to run during fiddling */
        pHandler->bSet = true;
        pHandler->fHandler = fHandler;
        pHandler->parameter = parameter;
        pHandler->next = 0;
        _disable_irq_exit_turf(irq);
        _enable_irq(irq); /* re-enable */
        return;
    }

    do {
        if(pHandler->next) {
            pHandler = pHandler->next;
        }else{ /* no other handler, create new */
            pNewHandler = (struct sIRQHandler *)kalloc(sizeof(struct sIRQHandler));
            pNewHandler->bSet = true; /* do first, so no other thread will touch */
            pNewHandler->fHandler = fHandler;
            pNewHandler->parameter = parameter;
            pNewHandler->next = 0;
            pHandler->next = pNewHandler; /* this is an atomic set */
            break;
        }
    } while(pHandler);

    _enable_irq(irq); /* enable regardless if needed (uniform results) */

    return;
}

/* This function removes the first instance of the given handler with the same parameter and irq
    specified. (The first to be added).  It is impossible to make this truly atomic */
void del_irq_handler(u_int irq, FuncIRQHandler fHandler, void * parameter) {
    struct sIRQHandler * pHandler = &IRQBaseHandlers[irq]; /* select base handler for requested irq */
    struct sIRQHandler * pPrevHandler = 0;

    _disable_irq_enter_turf(irq);  /* don't want handler to run during fiddling */


    do {
       if(pHandler->fHandler == fHandler && pHandler->parameter == parameter) {
           pHandler->fHandler = 0;
           pHandler->parameter = 0;
           pHandler->bSet = false;

           if(pPrevHandler) {    /* if it is not the top entry */
               pPrevHandler->next = pHandler->next; /* take it out of link */
               pHandler->next = 0;
               kfree((void *)pHandler);/* finally, free it */
           } else {
            pHandler->next = 0; /* if top, just zero next */
            }
           break; /* our work is done */
       }

        pPrevHandler = pHandler;
        pHandler = pHandler->next;
    } while(pHandler);

    _disable_irq_exit_turf(irq);
    _enable_irq(irq); /* re-enable */

    return;
}

/* Enables (a handler) - this is an atomic with handling interrupts (no need to disable) */
void enable_irq_handler(u_int irq, FuncIRQHandler fHandler, void * parameter) {
    struct sIRQHandler * pHandler = &IRQBaseHandlers[irq]; /* select base handler for requested irq */

    do {
       if(pHandler->fHandler == fHandler && pHandler->parameter == parameter && !pHandler->bSet) {
           pHandler->bSet = true; /* enable */
           break; /* our work is done */
       }

        pHandler = pHandler->next;
    } while(pHandler);

    return;
}
/* Disables (a handler) - this is an atomic with handling interrupts (no need to disable) */
void disable_irq_handler(u_int irq, FuncIRQHandler fHandler, void * parameter) {
    struct sIRQHandler * pHandler = &IRQBaseHandlers[irq]; /* select base handler for requested irq */
    do {
       if(pHandler->fHandler == fHandler && pHandler->parameter == parameter && pHandler->bSet) {
           pHandler->bSet = false; /* disable */
           break; /* our work is done */
       }

        pHandler = pHandler->next;
    } while(pHandler);

    return;
}


/* The main IRQ Handler (IRQ 0 - 15) */
void handleIRQ(unsigned int num) {
    struct sIRQHandler * pHandler = &IRQBaseHandlers[num]; /* select base handler for requested irq */
    while(pHandler) {
         if(pHandler->bSet) pHandler->fHandler(num, (void *)pHandler->parameter);
         pHandler = pHandler->next;
    }
    /* ack the IRQ only once */
    if (num & 8) {/* PIC 2 */
        outb(0xA1, cached_A1);          /* OCW1 -  Just to get to OCW2 */
        outb(0xA0, 0x60 | (num & 7));   /* OCW2 - ACK Specific EOI (011<<5 == 0x60) */
        outb(0x20, 0x60 | 2);           /* ACK IRQ2 on Master */
    } else {
        outb(0x21, cached_21);          /* OCW1 -  Just to get to OCW2 */
        outb(0x20, 0x60 | num);         /* OCW2 - Ack master (specific) */
    }
    return;
}

void ack_irq(u_int num) {
    if (num & 8) { /* PIC 2 */
        outb(0xA1, cached_A1);          /* OCW1 -  Just to get to OCW2 */
        outb(0xA0, 0x60 | (num & 7));   /* OCW2 - ACK Specific EOI (011<<5 == 0x60) */
        outb(0x20, 0x60 | 2);           /* ACK IRQ2 on Master */
    } else {
        outb(0x21, cached_21);          /* OCW1 -  Just to get to OCW2 */
        outb(0x20, 0x60 | num);         /* 0CW2 - Ack master (specific) */
    }
    return;
}
