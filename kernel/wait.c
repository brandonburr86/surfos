/*
SurfOS Wait Queues
--------------------
File: wait.c    Date: 10/3/26 (roadmap S2)
--------------------
*/

#include <surfos/types.h>
#include <surfos/irq.h>
#include <surfos/panic.h>
#include <surfos/interrupt.h>
#include <surfos/task.h>
#include <surfos/wait.h>
#include <surfos/ktimer.h>
#include <blibc_common.h>

void wq_init(wait_queue_t *q) {
    q->first = q->last = NULL;
    q->count = 0;
}

void wait_prepare(wait_queue_t *q) {
    BUG_ON(in_interrupt());
    BUG_ON(curTask->list != NULL);   /* a running task is on no list */
    curTask->state = TS_BLOCKED;
    curTask->wait_timed_out = false;
    task_list_append(q, curTask);
}

void wait_on(wait_queue_t *q) {
    u_long flags = irq_save();
    wait_prepare(q);
    irq_restore(flags);
    yield();
}

static void wait_timeout_fn(void *arg) {
    struct surf_task *t = (struct surf_task *)arg;
    if(t->state == TS_BLOCKED) {
        t->wait_timed_out = true;
        wake_up_task(t);
    }
}

int wait_on_timeout(wait_queue_t *q, u_long ms) {
    struct ktimer timer;
    u_long flags;

    ktimer_init(&timer, wait_timeout_fn, curTask);
    flags = irq_save();
    wait_prepare(q);
    ktimer_start(&timer, ms, false);
    irq_restore(flags);
    yield();
    ktimer_cancel(&timer);
    return curTask->wait_timed_out ? -1 : 0;
}

/* move a blocked task back to its run queue; interrupts must be disabled */
static void wake_locked(struct surf_task *t) {
    task_list_remove(t->list, t);
    t->state = TS_READY;
    task_make_ready(t);
}

void wake_up(wait_queue_t *q) {
    u_long flags = irq_save();
    if(q->first) wake_locked(q->first);
    irq_restore(flags);
}

void wake_up_all(wait_queue_t *q) {
    u_long flags = irq_save();
    while(q->first) wake_locked(q->first);
    irq_restore(flags);
}

void wake_up_task(struct surf_task *t) {
    u_long flags = irq_save();
    if(t->state == TS_BLOCKED && t->list) wake_locked(t);
    irq_restore(flags);
}

u_int wq_count(wait_queue_t *q) {
    return q->count;
}
