/*
SurfOS Wait Queues
----------------------
File: wait.h    Date: 10/3/26 (roadmap S2)
----------------------
A blocked task sits on exactly one wait queue, linked through its prev/next
pointers (it is on no run queue while it waits). wake_up() may be called from an
interrupt handler; wait_on() may not.

    while(!condition) wait_on(&q);        in a task
    condition = true; wake_up(&q);        in the ISR or another task
*/

#ifndef _SURFOS_WAIT_H
#define _SURFOS_WAIT_H

#include <surfos/types.h>
#include <surfos/task.h>

typedef struct task_list wait_queue_t;

#define WAIT_QUEUE_INIT { NULL, NULL, 0 }

void wq_init(wait_queue_t *q);

/* Put the current task on q as BLOCKED. Interrupts must be disabled by the caller
   (irq_save), which keeps the condition check and the enqueue atomic; the caller then
   restores interrupts and calls yield(). wait_on() does all of that. */
void wait_prepare(wait_queue_t *q);
void wait_on(wait_queue_t *q);

/* 0 when woken, -1 when ms passed first */
int wait_on_timeout(wait_queue_t *q, u_long ms);

void wake_up(wait_queue_t *q);       /* the first waiter */
void wake_up_all(wait_queue_t *q);
void wake_up_task(struct surf_task *t); /* a specific waiter (used by timeouts and kill) */
u_int wq_count(wait_queue_t *q);

#endif
