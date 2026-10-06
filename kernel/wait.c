/*
SurfOS Wait Queues
--------------------
File: wait.c    Date: 10/3/26 (roadmap S2)
--------------------
10/6/26: waits that a kill or Ctrl+C cuts short (wait.h, task.h).
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
    curTask->wait_intr = 0;
    task_list_append(q, curTask);
}

/* 10/6/26: kill_task() and Ctrl+C wake a task queued this way (task.c) */
void wait_prepare_intr(wait_queue_t *q) {
    wait_prepare(q);
    curTask->wait_intr = 1;
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

/* 10/6/26: one that may be cut short (a kill, Ctrl+C), and its timer is the task's wait_timer while it
   waits: a task killed at once in the middle of it takes the timer off the list as it dies (task.c),
   since the timer lives on the stack that goes with the task */
int wait_on_timeout(wait_queue_t *q, u_long ms) {
    struct ktimer timer;
    u_long flags;

    ktimer_init(&timer, wait_timeout_fn, curTask);
    flags = irq_save();
    if(curTask->intr) {
        irq_restore(flags);
        return -EINTR;
    }
    wait_prepare_intr(q);
    curTask->wait_timer = &timer;
    ktimer_start(&timer, ms, false);
    irq_restore(flags);
    yield();
    flags = irq_save();
    ktimer_cancel(&timer);
    curTask->wait_timer = NULL;
    irq_restore(flags);
    if(curTask->wait_timed_out) return -1;
    return curTask->intr ? -EINTR : 0;
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
