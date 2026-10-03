/*
SurfOS Interrupt System
----------------------
File: interrupt.h   Date: Prior to 4/23/04, rebuilt 10/2026 (roadmap K1)
----------------------
(C)2004 Brandon Burr + Martin McCormick
*/

#ifndef _SURFOS_INTERRUPT_H
#define _SURFOS_INTERRUPT_H

#include <surfos/types.h>
#include <surfos/trap.h>

#define NR_IRQS 16

/* IDT gate option bits (byte 5 of a gate descriptor) */
#define INT_GATE    0x06
#define TRAP_GATE   0x07
#define BITS_32     0x08
#define IDT_PRESENT 0x80
#define INT_RING0   0x00
#define INT_RING3   0x60

typedef struct idt_entry {
    u_short loffset;
    u_short selector;
    u_char  unused;
    u_char  options;
    u_short uoffset;
} __attribute__((packed)) idt_entry;

/**** 8259 PIC ports ****/
#define MASTER_PORT_A 0x20
#define MASTER_PORT_B 0x21
#define SLAVE_PORT_A  0xA0
#define SLAVE_PORT_B  0xA1

/***** IRQ Numbers (PC/AT wiring) *****/
enum irqnums {
    IRQ_TIMER = 0, IRQ_KEYBOARD, IRQ_SLAVE, IRQ_COM2, IRQ_COM1, IRQ_LPT2, IRQ_FLOPPY, IRQ_PRINTER,
    IRQ_RTC, IRQ_ACPI, IRQ_10, IRQ_11, IRQ_MOUSE, IRQ_FPU, IRQ_ATA0, IRQ_ATA1
};

void init_interrupt(void);
void _set_idt_entry(int num, u_long address, u_short selector, u_char opt);

void _enable_irq(u_int irq);
void _disable_irq(u_int irq);

/* IRQ handler chains: several handlers may share one IRQ */
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

/* statistics and context */
extern u_long irq_count[NR_IRQS];
extern u_long spurious_irq_count;
extern volatile int in_irq;
#define in_interrupt() (in_irq > 0)

#endif
