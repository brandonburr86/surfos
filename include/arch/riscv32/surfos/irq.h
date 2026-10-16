/*
SurfOS Interrupt Flag Helpers (riscv32)
----------------------
File: irq.h     Date: 10/15/26 (SurfCPU), the riscv32 face of include/surfos/irq.h
----------------------
The same interface as the x86 header, on mstatus.MIE instead of EFLAGS.IF. The
riscv32 build puts include/arch/riscv32 ahead of include, so this file replaces the
x86 one there and nowhere else.

    u_long flags = irq_save();
    ... touch data an interrupt handler also touches ...
    irq_restore(flags);

One hart and no atomics (rv32im has no A extension): a critical section is
interrupts off, nothing more. irq_save() is a single csrrc, so reading the old
state and disabling cannot be split by an interrupt.
*/

#ifndef _SURFOS_IRQ_H
#define _SURFOS_IRQ_H

#include <surfos/types.h>
#include <riscv/csr.h>

static inline u_long irq_save(void) {
    return csr_read_clear(mstatus, MSTATUS_MIE) & MSTATUS_MIE;
}

/* MIE exactly as it was, on or off (as popfl restores IF on x86) */
static inline void irq_restore(u_long flags) {
    if(flags & MSTATUS_MIE) csr_set(mstatus, MSTATUS_MIE);
    else csr_clear(mstatus, MSTATUS_MIE);
}

static inline void irq_enable(void) { csr_set(mstatus, MSTATUS_MIE); }
static inline void irq_disable(void) { csr_clear(mstatus, MSTATUS_MIE); }

static inline bool irqs_enabled(void) {
    return (csr_read(mstatus) & MSTATUS_MIE) ? true : false;
}

#endif
