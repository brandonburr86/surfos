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
#include <surfos/task.h>

/*****Timer stuff****/

#define CPU_FREQ 1193180
#define HZ 100

#define TICKS_PER_SECOND (HZ)


#define TICKS_TO_SECOND(ticks) ((ticks)>=HZ?(ticks)/HZ:0)
#define SECOND_TO_TICKS(sec)   ((sec)*HZ)

//#define TICKS TO USEC(ticks) ((ticks))
//#define USEC_TO_TICKS(usec)  ((usec))

#define TICKS_TO_USEC(ticks) ((ticks)*(HZ/10))
#define USEC_TO_TICKS(usec)  ((usec)>=(HZ/10)?(usec)/(HZ/10):0)

#define REAL_TICKS_TO_USEC(a) ( ((a)*USEC_PER_SECOND)/TICKS_PER_USECOND )

/********************/
void init_timer();
u_long getticks();


inline u_long *timer_handler(u_long *esp);

#endif
