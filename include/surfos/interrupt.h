/*
SurfOS Interrupt System
----------------------
File: interrupt.h   Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr + Martin McCormick

*/

#ifndef _SURFOS_INTERRUPT_H
#define _SURFOS_INTERRUPT_H

#include <surfos/types.h>

#define CS_SELECTOR 0x08
#define IDTBASE 0x6800
#define IDT_LOWBASE 0x6800
#define IDT_HIGHBASE 0

#define INT_GATE 0x06
#define TRAP_GATE 0x07

#define BITS_16 0x00
#define BITS_32 0x08

#define IDT_ABSENT 0x0
#define IDT_PRESENT 0x80

#define INT_RING0 0x00
#define INT_RING1 0x20
#define INT_RING2 0x40
#define INT_RING3 0x60

#define __byte(x,y)     (((unsigned char *)&(y))[x])
#define cached_21   (__byte(0,cached_irq_mask))
#define cached_A1   (__byte(1,cached_irq_mask))

#define NR_IRQS 15

typedef struct idt_entry {
    u_short loffset;
    u_short selector;
    u_char unused;
    u_char options;
    u_short uoffset;
} idt_entry;


struct sIDTMaster {
    u_short limit;
    u_short low_base;
    u_short high_base;
};

void load_idt_entry(int num,u_int offset,u_short selector,u_char options);
void idt_init();
void load_idtr(struct sIDTMaster*);
void irq_delay(unsigned long loops);
void handleIRQ(unsigned int num);

/**** Port Addresses ****/
#define MASTER_PORT_A 0x20
#define MASTER_PORT_B 0x21
#define SLAVE_PORT_A 0xA0
#define SLAVE_PORT_B 0xA1
/***********************/

/***** IRQ Numbers *****/
enum irqnums
{IRQ_TIMER, IRQ_KEYBOARD, IRQ_SLAVE, IRQ_TTY1,
IRQ_TTY2, IRQ_XT_WINCHESTER, IRQ_FLOPPY, IRQ_PRINTER,
IRQ_RTC, IRQ_FPU_EXCEPTION, IRQ_AT_WINCHESTER};
/***********************/

void _enable_irq(u_int);
void _disable_irq(u_int);
void _enable_irq_enter_turf(u_int irq);
void _enable_irq_exit_turf(u_int irq);
void _disable_irq_enter_turf(u_int irq);
void _disable_irq_exit_turf(u_int irq);
void init_interrupt();

/* IDT functions */
void _set_idt_int(int num, u_int* address);
void _set_idt_trap(int num, u_int* address);
void _set_idt_entry(int num, u_int address, u_short selector, u_char opt);
/****************/

/* IRQ Handlers */
typedef int(*FuncIRQHandler) /* function definition */
    ( /* parameters */
      u_int irq,
      void * parameter
    );

struct sIRQHandler {
    bool bSet;
    FuncIRQHandler fHandler;
    void * parameter;
    struct sIRQHandler * next;
};

/* Turf stuff -
    Often in drivers or other code in a multithreaded/proccess environment, a block of code
    is executed between _irq_disable() and _irq_enable() calls.  This block of code may make
    changes to global variables and such that must be done without interruption from interrupts
    It is possible, however, that another thread/proccess enables interrupts while code is being
    executed within this block.  Obviously this is very bad.
    To stop this, access to enabling and disabling interrupts must be limited using
    Turf Descriptors.

    for example:
    {
    _disable_irq_enter_turf(0);

    ... CODE THAT ASSUMES IRQ 0 IS DISABLED ...

    _disable_irq_exit_turf(0); //enabling is now allowed (but is not enabled)

    }

    This will cause other code that may normally enable interrupt zero to stall until
    the critical block is done.

    _enable_irq(0); //This must wait for critical to end.

    _disable_irq(0); //Additional disabling however, is allowed.
*/
/* side note: the opposite can also be done (a critical block that requires interrupts ENABLED */


#define TURF_IRQ_RULE_COUNT 4
/* There are 4 types of access */
/*
    0 = Enable (normal enabling interrupts)
    1 = Disable (normal enabling interrupts)
    2 = Critical Enable (enabling in a block that cannot be changed)
    3 = Critical Disable (disabling in a block that cannot be changed)
*/
#define TURF_IRQ_ENABLE 0
#define TURF_IRQ_DISABLE 1
#define TURF_IRQ_CRIT_ENABLE 2
#define TURF_IRQ_CRIT_DISABLE 3

#define IRQ_NORMAL          0
#define IRQ_CRITICAL        1
#define IRQ_OUT_CRITICAL    2

void add_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter);
void removeHandler(u_int irq, FuncIRQHandler fHandler, void *parameter);
void enable_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter);
void disable_irq_handler(u_int irq, FuncIRQHandler fHandler, void *parameter);

void isrStartIRQ(void);
void isrEndIRQ(void);

void isrIRQ1(void);
void isrIRQ2(void);
void isrIRQ3(void);
void isrIRQ4(void);
void isrIRQ5(void);
void isrIRQ6(void);
void isrIRQ7(void);
void isrIRQ8(void);
void isrIRQ9(void);
void isrIRQ10(void);
void isrIRQ11(void);
void isrIRQ12(void);
void isrIRQ13(void);
void isrIRQ14(void);
void isrIRQ15(void);
#endif
