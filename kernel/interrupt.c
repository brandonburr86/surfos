/*
SurfOS Core Interrupt System
--------------------
File: interrupt.c   Date: June 2004, rebuilt 10/2026
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

MM - July 2nd update:
     + Moved new interrupt system into SurfOS officialy

10/2026 (ai-dev, roadmap K1):
    + One entry stub per vector (kernel/traps.asm) building a struct trapframe
    + Dispatch table: handlers return the frame to resume, so the scheduler is just
      another handler and IRQs run on the current task's stack
    + IDT lives in kernel memory and every vector is populated
    + Spurious IRQ 7/15 detection, EOI only for real IRQs, IRQ 15 installed
    + Masking under irq_save() instead of the turf descriptors
*/

#include <asm/io.h>
#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>
#include <surfos/trap.h>
#include <surfos/irq.h>
#include <surfos/panic.h>
#include <surfos/gdt.h>
#include <surfos/timer.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

static idt_entry idt[NR_VECTORS] __attribute__((aligned(8)));
static trap_handler_t trap_handlers[NR_VECTORS];

static u_int cached_irq_mask = 0xffff;   /* the PIC masks are write only, remember them */
#define cached_21 ((u_char)(cached_irq_mask & 0xff))
#define cached_A1 ((u_char)(cached_irq_mask >> 8))

struct sIRQHandler IRQBaseHandlers[NR_IRQS]; /* the base handlers for each IRQ (a linked list attaches to) */
u_long irq_count[NR_IRQS];
u_long spurious_irq_count;
volatile int in_irq;

struct idt_ptr {
    u_short limit;
    u_long base;
} __attribute__((packed));

static const char *trap_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound Range Exceeded",
    "Invalid Opcode", "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 Floating Point Error", "Alignment Check", "Machine Check",
    "SIMD Floating Point Error", "Virtualization Exception", "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved"
};

const char *trap_name(u_int vector) {
    if(vector < 32) return trap_names[vector];
    if(vector >= T_IRQ0 && vector < T_IRQ0 + NR_IRQS) return "IRQ";
    if(vector == T_YIELD) return "yield";
    if(vector == T_SYSCALL) return "syscall";
    return "unassigned vector";
}

/**** IDT ****/

void _set_idt_entry(int num, u_long address, u_short selector, u_char opt) {
    idt[num].loffset  = address & 0xFFFF;
    idt[num].selector = selector;
    idt[num].unused   = 0;
    idt[num].options  = opt;
    idt[num].uoffset  = (address >> 16) & 0xFFFF;
}

static void load_idtr(void) {
    struct idt_ptr idtr;
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (u_long)idt;
    asm volatile("lidt %0" : : "m"(idtr));
}

void trap_set_handler(u_int vector, trap_handler_t handler) {
    if(vector < NR_VECTORS) trap_handlers[vector] = handler;
}

/**** 8259 PICs ****/

static void pic_eoi(u_int irq) {
    if(irq >= 8) outb(SLAVE_PORT_A, 0x60 | (irq & 7)); /* OCW2: specific EOI on the slave */
    outb(MASTER_PORT_A, 0x60 | (irq >= 8 ? IRQ_SLAVE : irq)); /* and on the master */
}

/* IRQ 7 and 15 fire spuriously when a request vanishes before the CPU acknowledges it;
   a real one has its in-service bit set. */
static bool pic_spurious(u_int irq) {
    u_short port = irq < 8 ? MASTER_PORT_A : SLAVE_PORT_A;
    outb(port, 0x0B); /* OCW3: next read returns the ISR */
    return (inb(port) & 0x80) ? false : true;
}

void _enable_irq(u_int irq) {
    u_long flags = irq_save();
    cached_irq_mask &= ~(1 << irq);
    if(irq & 8) outb(SLAVE_PORT_B, cached_A1);
    else outb(MASTER_PORT_B, cached_21);
    irq_restore(flags);
}

void _disable_irq(u_int irq) {
    u_long flags = irq_save();
    cached_irq_mask |= (1 << irq);
    if(irq & 8) outb(SLAVE_PORT_B, cached_A1);
    else outb(MASTER_PORT_B, cached_21);
    irq_restore(flags);
}

/**** Dispatch ****/

static u_long *irq_dispatch(struct trapframe *tf) {
    u_int irq = tf->vector - T_IRQ0;
    struct sIRQHandler *pHandler;

    if((irq == 7 || irq == 15) && pic_spurious(irq)) {
        spurious_irq_count++;
        if(irq == 15) outb(MASTER_PORT_A, 0x60 | IRQ_SLAVE); /* the cascade itself was real */
        return (u_long*)tf;
    }

    irq_count[irq]++;
    in_irq++;
    for(pHandler = &IRQBaseHandlers[irq]; pHandler; pHandler = pHandler->next) {
        if(pHandler->bSet && pHandler->fHandler) pHandler->fHandler(irq, pHandler->parameter);
    }
    in_irq--;
    pic_eoi(irq);

    if(irq == IRQ_TIMER) return timer_tick(tf); /* the scheduler may hand back another task */
    return (u_long*)tf;
}

u_long *trap_dispatch(struct trapframe *tf) {
    u_int vector = tf->vector;
    if(vector < NR_VECTORS && trap_handlers[vector]) return trap_handlers[vector](tf);
    if(vector >= T_IRQ0 && vector < T_IRQ0 + NR_IRQS) return irq_dispatch(tf);
    return trap_unexpected(tf);
}

/**** Initialization ****/

void init_interrupt() {
    int i;
    kprintf("\nInterrupt Initialization\n");

    memset(idt, 0, sizeof(idt));
    memset(trap_handlers, 0, sizeof(trap_handlers));
    memset(IRQBaseHandlers, 0, sizeof(IRQBaseHandlers));
    memset(irq_count, 0, sizeof(irq_count));
    spurious_irq_count = 0;
    in_irq = 0;

    for(i = 0; i < NR_VECTORS; i++) {
        _set_idt_entry(i, isr_stub_table[i], KERNEL_CS, INT_GATE | IDT_PRESENT | INT_RING0 | BITS_32);
    }
    /* user mode will enter through int 0x80, so that gate must be reachable from ring 3 */
    _set_idt_entry(T_SYSCALL, isr_stub_table[T_SYSCALL], KERNEL_CS, INT_GATE | IDT_PRESENT | INT_RING3 | BITS_32);
    load_idtr();
    kprintf("*IDT: %i vectors\n", NR_VECTORS);

    cached_irq_mask = 0xffff;
    outb(MASTER_PORT_B, 0xff);      /* mask everything for master */
    outb(SLAVE_PORT_B, 0xff);       /* mask everything for slave */

    outb(MASTER_PORT_A, 0x11);      /* ICW1: expect ICW4 */
    io_wait();
    outb(MASTER_PORT_B, T_IRQ0);    /* ICW2: IRQ 0-7 mapped to 0x20-0x27 */
    io_wait();
    outb(MASTER_PORT_B, 0x04);      /* ICW3: the master has a slave on IRQ 2 */
    io_wait();
    outb(MASTER_PORT_B, 0x01);      /* ICW4: 8086 mode */
    io_wait();

    outb(SLAVE_PORT_A, 0x11);       /* ICW1: expect ICW4 */
    io_wait();
    outb(SLAVE_PORT_B, T_IRQ0 + 8); /* ICW2: IRQ 8-15 mapped to 0x28-0x2f */
    io_wait();
    outb(SLAVE_PORT_B, 0x02);       /* ICW3: slave on master's IRQ 2 */
    io_wait();
    outb(SLAVE_PORT_B, 0x01);       /* ICW4: 8086 mode */
    io_wait();

    outb(MASTER_PORT_B, cached_21); /* restore master mask */
    outb(SLAVE_PORT_B, cached_A1);  /* restore slave mask */
    kprintf("*PICs remapped to 0x20/0x28\n");

    init_exceptions();              /* panic.c: vectors 0-31 and the page fault */

    _enable_irq(IRQ_SLAVE);         /* for slave 8259A */

    kprintf("*Interrupts Enabled\n");
    irq_enable();
}

/**** Shared handler chains ****/

void add_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter) {
    struct sIRQHandler *pHandler = &IRQBaseHandlers[irq]; /* select base handler for requested irq */
    struct sIRQHandler *pNewHandler;
    u_long flags;

    if(irq >= NR_IRQS) return;

    flags = irq_save();
    if(!pHandler->fHandler) {
        /* head not used - so use it! */
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

    _enable_irq(irq); /* enable regardless if needed (uniform results) */
}

/* This function removes the first instance of the given handler with the same parameter and irq
    specified. (The first to be added). */
void del_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter) {
    struct sIRQHandler *pHandler = &IRQBaseHandlers[irq];
    struct sIRQHandler *pPrevHandler = 0;
    u_long flags;

    if(irq >= NR_IRQS) return;

    flags = irq_save();
    do {
        if(pHandler->fHandler == fHandler && pHandler->parameter == parameter) {
            if(pPrevHandler) { /* if it is not the top entry */
                pPrevHandler->next = pHandler->next; /* take it out of link */
                kfree((void *)pHandler);
            } else if(pHandler->next) { /* top entry with followers: pull the next one up */
                struct sIRQHandler *pNext = pHandler->next;
                *pHandler = *pNext;
                kfree((void *)pNext);
            } else { /* top and alone */
                pHandler->fHandler = 0;
                pHandler->parameter = 0;
                pHandler->bSet = false;
            }
            break; /* our work is done */
        }
        pPrevHandler = pHandler;
        pHandler = pHandler->next;
    } while(pHandler);

    if(!IRQBaseHandlers[irq].fHandler) _disable_irq(irq); /* nobody is listening any more */
    irq_restore(flags);
}

/* Enables (a handler) - this is an atomic with handling interrupts (no need to disable) */
void enable_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter) {
    struct sIRQHandler *pHandler = &IRQBaseHandlers[irq];
    do {
        if(pHandler->fHandler == fHandler && pHandler->parameter == parameter && !pHandler->bSet) {
            pHandler->bSet = true; /* enable */
            break;
        }
        pHandler = pHandler->next;
    } while(pHandler);
}

/* Disables (a handler) - this is an atomic with handling interrupts (no need to disable) */
void disable_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter) {
    struct sIRQHandler *pHandler = &IRQBaseHandlers[irq];
    do {
        if(pHandler->fHandler == fHandler && pHandler->parameter == parameter && pHandler->bSet) {
            pHandler->bSet = false; /* disable */
            break;
        }
        pHandler = pHandler->next;
    } while(pHandler);
}
