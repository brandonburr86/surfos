/*
SurfOS Kernel Timers
----------------------
File: ktimer.h  Date: 10/3/26 (roadmap S3)
----------------------
Callbacks run from the timer interrupt (interrupts off): keep them short, wake a
task if there is real work. A struct ktimer must stay valid until it fires or
is cancelled.
*/

#ifndef _SURFOS_KTIMER_H
#define _SURFOS_KTIMER_H

#include <surfos/types.h>

struct ktimer {
    u_long expires;        /* tick count */
    u_long period;         /* ticks; 0 for one-shot */
    void (*fn)(void *arg);
    void *arg;
    bool active;
    struct ktimer *next;
};

void ktimer_init(struct ktimer *t, void (*fn)(void *), void *arg);
void ktimer_start(struct ktimer *t, u_long ms, bool periodic);
void ktimer_cancel(struct ktimer *t);
bool ktimer_pending(struct ktimer *t);

void ktimers_tick(void);        /* run from timer_tick() */
u_int ktimers_count(void);

u_long uptime_ms(void);
u_long ms_to_ticks(u_long ms);  /* rounds up, at least 1 */

#endif
