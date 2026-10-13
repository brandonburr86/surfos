#ifndef _SURFOS_IRQ_H
#define _SURFOS_IRQ_H
#include <surfos/types.h>
static inline u_long irq_save(void) { return 0; }
static inline void irq_restore(u_long f) { (void)f; }
#endif
