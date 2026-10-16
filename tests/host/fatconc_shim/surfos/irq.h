/* tests/host/test_fatconc.c (10/16/26, round fs-fix-2: the review of B5-B11's conc_shim/surfos/irq.h, as it was).
   review harness (conc): "interrupts off" is one recursive host mutex, so an irq_save() region is
   atomic against every other thread's, as it is against preemption on the kernel's one CPU */
#ifndef _SURFOS_IRQ_H
#define _SURFOS_IRQ_H
#include <surfos/types.h>
u_long conc_irq_save(void);
void conc_irq_restore(u_long flags);
static inline u_long irq_save(void) { return conc_irq_save(); }
static inline void irq_restore(u_long flags) { conc_irq_restore(flags); }
static inline void irq_enable(void) {}
static inline void irq_disable(void) {}
static inline bool irqs_enabled(void) { return true; }
#endif
