/*
SurfOS Interrupt Flag Helpers
----------------------
File: irq.h     Date: 10/3/26 (roadmap K1)
----------------------
Short critical sections against interrupt handlers on this single CPU:

    u_long flags = irq_save();
    ... touch data an ISR also touches ...
    irq_restore(flags);

KCRIT_ENTER/KCRIT_LEAVE (task.h) only stop the scheduler; these stop the ISRs too.
*/

#ifndef _SURFOS_IRQ_H
#define _SURFOS_IRQ_H

#include <surfos/types.h>

static inline u_long irq_save(void) {
    u_long flags;
    asm volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(u_long flags) {
    asm volatile("pushl %0\n\tpopfl" : : "r"(flags) : "memory", "cc");
}

static inline void irq_enable(void) { asm volatile("sti" : : : "memory"); }
static inline void irq_disable(void) { asm volatile("cli" : : : "memory"); }

static inline bool irqs_enabled(void) {
    u_long flags;
    asm volatile("pushfl\n\tpopl %0" : "=r"(flags));
    return (flags & 0x200) ? true : false;
}

#endif
