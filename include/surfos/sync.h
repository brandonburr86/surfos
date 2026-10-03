/*
SurfOS Synchronization Primitives
----------------------
File: sync.h    Date: 10/3/26 (roadmap S2)
----------------------
Built on wait queues. One CPU, so the short critical sections inside are
irq_save() regions. A mutex has an owner and may only be released by it; a
semaphore and an event may be signalled from an interrupt handler.
*/

#ifndef _SURFOS_SYNC_H
#define _SURFOS_SYNC_H

#include <surfos/types.h>
#include <surfos/wait.h>

typedef struct {
    volatile int locked;
    struct surf_task *owner;
    wait_queue_t waiters;
    const char *name;
} mutex_t;

#define MUTEX_INIT(n) { 0, NULL, WAIT_QUEUE_INIT, n }

void mutex_init(mutex_t *m, const char *name);
void mutex_lock(mutex_t *m);
bool mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);

typedef struct {
    volatile int count;
    wait_queue_t waiters;
} semaphore_t;

void sem_init(semaphore_t *s, int count);
void sem_wait(semaphore_t *s);
bool sem_trywait(semaphore_t *s);
void sem_post(semaphore_t *s);           /* ISR safe */

typedef struct {
    volatile bool set;
    wait_queue_t waiters;
} event_t;

void event_init(event_t *e);
void event_wait(event_t *e);             /* returns once set; does not clear */
int event_wait_timeout(event_t *e, u_long ms); /* 0 set, -1 timeout */
void event_set(event_t *e);              /* ISR safe: wakes everyone */
void event_clear(event_t *e);

#endif
