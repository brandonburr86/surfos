/* tests/host/test_pager: the interrupt flag of the host test's one CPU (tests/host/pager_bridge.c);
   cli and pushf/popf would fault in a host process */
#ifndef _SURFOS_IRQ_H
#define _SURFOS_IRQ_H
#include <surfos/types.h>
extern int sim_if;
/* 10/26/26 (the review of the gap fix): a tick, armed by the test for one task's n-th irq_save() that finds
   interrupts on (pager_bridge.c's sim_preempt_irq()): that task yields there, before interrupts go off. Only
   test_pager has the bridge (SIM_PREEMPT, its PAGER_FLAGS); the other tests that use this shim do not */
#ifdef SIM_PREEMPT
extern int sim_preempt_armed;
void sim_preempt_check(void);
#endif
static inline u_long irq_save(void) {
    u_long f;
#ifdef SIM_PREEMPT
    if(sim_preempt_armed && sim_if) sim_preempt_check();
#endif
    f = sim_if ? 0x200 : 0;
    sim_if = 0;
    return f;
}
static inline void irq_restore(u_long flags) { sim_if = (flags & 0x200) != 0; }
static inline void irq_enable(void) { sim_if = 1; }
static inline void irq_disable(void) { sim_if = 0; }
static inline bool irqs_enabled(void) { return sim_if ? true : false; }
#endif
