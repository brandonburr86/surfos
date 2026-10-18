/*
SurfOS Synchronization Primitives
--------------------
File: sync.c    Date: 10/3/26 (roadmap S2)
--------------------
10/6/26: a task counts the mutexes it holds (surf_task.mutexes). The console's pager
(TTY_PAGE, kernel/console.c) makes a task wait for a key only when it holds none, so a
lock is never kept while somebody reads a screenful. An event's timed wait may be cut
short by a kill or Ctrl+C (wait_on_timeout()); a mutex, a semaphore and an untimed
event wait are never cut short: their callers cannot do without what they wait for.

10/18/26 (audit B63): a mutex is handed over in the order its waiters came. mutex_unlock()
gives it to the first waiter, which holds it from that moment (and counts it) and finds it
its own when it runs. Before, the waiter was only woken, and the task that let go could
take the lock again before the waiter ran: a task taking a lock again and again (a long
copy's requests, each through its driver's lock or the cache's bounce buffer) kept a
waiter out for as long as it went on. Now a waiter waits for those queued ahead of it,
each holding the lock once. A task holding a mutex, the one handed over too, is not killed
at once (kernel/task.c), so a lock handed over is never left with a dead task.

10/18/26 (audit B66): a waiter woken another way (wake_up_task(): none reaches a mutex's
waiter today) is out of the line when it runs; if its holder let go meanwhile, mutex_unlock()
found nobody in line and left the mutex free, and the waiter went back in line and slept on a
free mutex until somebody else took it and let go. Now a waiter that finds the mutex free
takes it, as a task arriving does: a free mutex has nobody in line (one is let go of free only
when nobody waits, and nobody joins the line while it is free), so nobody is passed over, and
the waiter, out of the line, can never be handed it again. It loses its place in the line if
somebody else takes the mutex first, never the mutex.
*/

#include <surfos/types.h>
#include <surfos/irq.h>
#include <surfos/panic.h>
#include <surfos/task.h>
#include <surfos/wait.h>
#include <surfos/sync.h>
#include <blibc_common.h>

/**** mutex ****/

void mutex_init(mutex_t *m, const char *name) {
    m->locked = 0;
    m->owner = NULL;
    m->name = name;
    wq_init(&m->waiters);
}

void mutex_lock(mutex_t *m) {
    u_long flags = irq_save();
    if(m->locked && m->owner == curTask) panic("mutex '%s': recursive lock by '%s'", m->name, curTask->name);
    while(m->owner != curTask) {                /* 10/18/26 (audit B63): in line; mutex_unlock() hands it over */
        if(!m->locked) {                        /* free: this task's (10/18/26, B66: also after a wake-up of another
                                                   kind, the holder gone meanwhile; out of the line once it runs) */
            m->locked = 1;
            m->owner = curTask;
            if(curTask) curTask->mutexes++;
            break;
        }
        wait_prepare(&m->waiters);
        irq_restore(flags);
        yield();
        flags = irq_save();
    }
    irq_restore(flags);
}

bool mutex_trylock(mutex_t *m) {
    u_long flags = irq_save();
    bool got = false;
    if(!m->locked) {
        m->locked = 1;
        m->owner = curTask;
        if(curTask) curTask->mutexes++;
        got = true;
    }
    irq_restore(flags);
    return got;
}

void mutex_unlock(mutex_t *m) {
    u_long flags = irq_save();
    struct surf_task *next = m->waiters.first;
    if(!m->locked || m->owner != curTask) panic("mutex '%s': unlock by '%s' who does not hold it", m->name, curTask ? curTask->name : "?");
    if(curTask && curTask->mutexes) curTask->mutexes--;
    if(next) {                                  /* 10/18/26 (audit B63): the first waiter holds it from now */
        m->owner = next;
        next->mutexes++;
        wake_up(&m->waiters);                   /* (that one) */
    } else {
        m->locked = 0;
        m->owner = NULL;
    }
    irq_restore(flags);
}

/**** semaphore ****/

void sem_init(semaphore_t *s, int count) {
    s->count = count;
    wq_init(&s->waiters);
}

void sem_wait(semaphore_t *s) {
    u_long flags = irq_save();
    while(s->count <= 0) {
        wait_prepare(&s->waiters);
        irq_restore(flags);
        yield();
        flags = irq_save();
    }
    s->count--;
    irq_restore(flags);
}

bool sem_trywait(semaphore_t *s) {
    u_long flags = irq_save();
    bool got = false;
    if(s->count > 0) {
        s->count--;
        got = true;
    }
    irq_restore(flags);
    return got;
}

void sem_post(semaphore_t *s) {
    u_long flags = irq_save();
    s->count++;
    wake_up(&s->waiters);
    irq_restore(flags);
}

/**** event ****/

void event_init(event_t *e) {
    e->set = false;
    wq_init(&e->waiters);
}

void event_wait(event_t *e) {
    u_long flags = irq_save();
    while(!e->set) {
        wait_prepare(&e->waiters);
        irq_restore(flags);
        yield();
        flags = irq_save();
    }
    irq_restore(flags);
}

int event_wait_timeout(event_t *e, u_long ms) {
    u_long flags = irq_save();
    int rc = 0;
    if(!e->set) {
        irq_restore(flags);
        rc = wait_on_timeout(&e->waiters, ms);
        flags = irq_save();
        if(rc == -EINTR && e->set) rc = 0;  /* 10/6/26: cut short, but it came */
        if(rc == 0 && !e->set) rc = -1; /* woken but not set: treat as no event */
    }
    irq_restore(flags);
    return rc;
}

void event_set(event_t *e) {
    u_long flags = irq_save();
    e->set = true;
    wake_up_all(&e->waiters);
    irq_restore(flags);
}

void event_clear(event_t *e) {
    e->set = false;
}
