/*
SurfOS Kernel Timers
--------------------
File: ktimer.c  Date: 10/3/26 (roadmap S3)
--------------------
A list sorted by expiry; the tick looks at the head only.
*/

#include <surfos/types.h>
#include <surfos/irq.h>
#include <surfos/timer.h>
#include <surfos/ktimer.h>
#include <blibc_common.h>

static struct ktimer *timers;
static u_int ntimers;

u_long ms_to_ticks(u_long ms) {
    u_long ticks = (ms * HZ + 999) / 1000;
    return ticks ? ticks : 1;
}

u_long uptime_ms(void) {
    return getticks() * (1000 / HZ);
}

void ktimer_init(struct ktimer *t, void (*fn)(void *), void *arg) {
    t->fn = fn;
    t->arg = arg;
    t->expires = t->period = 0;
    t->active = false;
    t->next = NULL;
}

static void insert_locked(struct ktimer *t) {
    struct ktimer **pp = &timers;
    while(*pp && (*pp)->expires <= t->expires) pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
    t->active = true;
    ntimers++;
}

static void remove_locked(struct ktimer *t) {
    struct ktimer **pp = &timers;
    while(*pp && *pp != t) pp = &(*pp)->next;
    if(*pp) {
        *pp = t->next;
        ntimers--;
    }
    t->active = false;
    t->next = NULL;
}

void ktimer_start(struct ktimer *t, u_long ms, bool periodic) {
    u_long flags = irq_save();
    if(t->active) remove_locked(t);
    t->period = periodic ? ms_to_ticks(ms) : 0;
    t->expires = getticks() + ms_to_ticks(ms);
    insert_locked(t);
    irq_restore(flags);
}

void ktimer_cancel(struct ktimer *t) {
    u_long flags = irq_save();
    if(t->active) remove_locked(t);
    irq_restore(flags);
}

bool ktimer_pending(struct ktimer *t) {
    return t->active;
}

u_int ktimers_count(void) {
    return ntimers;
}

/* interrupts are off: we are inside the timer interrupt */
void ktimers_tick(void) {
    u_long now = getticks();
    while(timers && timers->expires <= now) {
        struct ktimer *t = timers;
        timers = t->next;
        ntimers--;
        t->active = false;
        t->next = NULL;
        if(t->period) { /* re-arm before the callback so it may cancel itself */
            t->expires = now + t->period;
            insert_locked(t);
        }
        t->fn(t->arg);
    }
}
