/*
SurfOS Synchronization Primitives
--------------------
File: sync.c    Date: 10/3/26 (roadmap S2)
--------------------
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
    while(m->locked) {
        wait_prepare(&m->waiters);
        irq_restore(flags);
        yield();
        flags = irq_save();
    }
    m->locked = 1;
    m->owner = curTask;
    irq_restore(flags);
}

bool mutex_trylock(mutex_t *m) {
    u_long flags = irq_save();
    bool got = false;
    if(!m->locked) {
        m->locked = 1;
        m->owner = curTask;
        got = true;
    }
    irq_restore(flags);
    return got;
}

void mutex_unlock(mutex_t *m) {
    u_long flags = irq_save();
    if(!m->locked || m->owner != curTask) panic("mutex '%s': unlock by '%s' who does not hold it", m->name, curTask ? curTask->name : "?");
    m->locked = 0;
    m->owner = NULL;
    wake_up(&m->waiters);
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
