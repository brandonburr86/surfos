/* tests/host/test_pager: the interrupt flag of the host test's one CPU (tests/host/pager_bridge.c);
   cli and pushf/popf would fault in a host process */
#ifndef _SURFOS_IRQ_H
#define _SURFOS_IRQ_H
#include <surfos/types.h>
extern int sim_if;
static inline u_long irq_save(void) { u_long f = sim_if ? 0x200 : 0; sim_if = 0; return f; }
static inline void irq_restore(u_long flags) { sim_if = (flags & 0x200) != 0; }
static inline void irq_enable(void) { sim_if = 1; }
static inline void irq_disable(void) { sim_if = 0; }
static inline bool irqs_enabled(void) { return sim_if ? true : false; }
#endif
