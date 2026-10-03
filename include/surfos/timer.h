/*
SurfOS Timer/Scheduler
----------------------
File: timer.h   Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_TIMER_H
#define _SURFOS_TIMER_H

#include <surfos/types.h>
#include <surfos/trap.h>

/*****Timer stuff****/

#define CPU_FREQ 1193180
#define HZ 100

#define TICKS_PER_SECOND (HZ)


#define TICKS_TO_SECOND(ticks) ((ticks)>=HZ?(ticks)/HZ:0)
#define SECOND_TO_TICKS(sec)   ((sec)*HZ)

/* the names say usec but the unit is milliseconds (1 tick = 10 ms) */
#define TICKS_TO_USEC(ticks) ((ticks)*(HZ/10))
#define USEC_TO_TICKS(usec)  ((usec)>=(HZ/10)?(usec)/(HZ/10):0)

/********************/
void init_delay();  /* calibrate udelay()/mdelay(); needs only the PIT */
void init_timer();
u_long getticks();

u_long *timer_tick(struct trapframe *tf); /* IRQ 0: called by the interrupt dispatcher */

/* calibrated busy waits for drivers; tasks should sleep_ms() instead */
void udelay(u_long us);
void mdelay(u_long ms);
u_long delay_loops_per_ms(void);

#endif
