/*
SurfOS poll
--------------------
File: poll.c    Date: 10/6/26
--------------------
SYS_poll (surfos/poll.h) and its one wait queue. A task in poll() looks at its descriptors
and, if none is ready, waits here; whatever may make a descriptor ready calls poll_wake(),
which wakes them all to look again. One queue for everything is simple and costs little
while few tasks poll (the telnet server, one at a time).

No wakeup is lost: poll_wake() counts its calls (poll_gen), and a task reads the count
before it looks at its descriptors and goes to sleep only if the count has not moved by
the time it is on the queue (interrupts off from that check to the enqueue). The
descriptors' poll operations are called with interrupts on, so they may take what locks
they need. The wait is one that a kill or Ctrl+C cuts short (-EINTR), and its timer is the
task's wait_timer while it waits, as in wait_on_timeout() (kernel/wait.c).
*/

#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/ktimer.h>
#include <surfos/timer.h>
#include <surfos/poll.h>
#include <fs/vfs.h>
#include <surfos/sysimpl.h>
#include <blibc_common.h>

static wait_queue_t poll_wq;
static volatile u_long poll_gen;        /* poll_wake() calls so far */

void poll_init(void) {
    wq_init(&poll_wq);
}

/* from task code and from interrupt handlers alike */
void poll_wake(void) {
    u_long flags = irq_save();
    poll_gen++;
    wake_up_all(&poll_wq);
    irq_restore(flags);
}

wait_queue_t *poll_queue(void) {
    return &poll_wq;
}

static void poll_timeout(void *arg) {
    struct surf_task *t = (struct surf_task *)arg;
    if(t->state == TS_BLOCKED) {
        t->wait_timed_out = true;
        wake_up_task(t);
    }
}

/* what descriptor fd is ready for, of what was asked (POLLERR, POLLHUP and POLLNVAL always) */
static short poll_one(const struct pollfd *p) {
    struct file *f;
    int r;
    if(p->fd < 0) return 0;
    f = fd_get(p->fd);
    if(!f) return POLLNVAL;
    r = f->v->ops->poll ? f->v->ops->poll(f->v, f) : (POLLIN | POLLOUT);    /* a file, /dev/null: always */
    return (short)(r & (p->events | POLLERR | POLLHUP | POLLNVAL));
}

int sys_poll(struct pollfd *fds, int nfds, int timeout_ms) {
    struct pollfd k[POLL_MAX];
    struct ktimer timer;
    u_long gen, flags, end = 0;
    long left;
    int i, n;

    if(nfds < 0 || nfds > POLL_MAX) return -EINVAL;
    if(nfds && !user_buf(fds, (u_long)nfds * sizeof(*fds), true)) return -EFAULT;
    memcpy(k, fds, (u_long)nfds * sizeof(*fds));
    if(timeout_ms > 0) end = getticks() + ms_to_ticks((u_long)timeout_ms);
    ktimer_init(&timer, poll_timeout, curTask);
    for(;;) {
        gen = poll_gen;                                 /* before looking: a wakeup from now on is seen below */
        asm volatile("" ::: "memory");
        for(i = 0, n = 0; i < nfds; i++) {
            k[i].revents = poll_one(&k[i]);
            if(k[i].revents) n++;
        }
        if(n || timeout_ms == 0) break;
        left = timeout_ms > 0 ? (long)(end - getticks()) : 1;
        if(left <= 0) break;                            /* the time is up: 0 */
        flags = irq_save();
        if(curTask->intr) {
            irq_restore(flags);
            return -EINTR;
        }
        if(poll_gen != gen) {                           /* something changed while we looked: again */
            irq_restore(flags);
            continue;
        }
        wait_prepare_intr(&poll_wq);
        if(timeout_ms > 0) {
            curTask->wait_timer = &timer;
            ktimer_start(&timer, (u_long)left * (1000 / HZ), false);
        }
        irq_restore(flags);
        yield();
        if(timeout_ms > 0) {
            flags = irq_save();
            ktimer_cancel(&timer);
            curTask->wait_timer = NULL;
            irq_restore(flags);
        }
        if(curTask->intr) return -EINTR;
    }
    memcpy(fds, k, (u_long)nfds * sizeof(*fds));
    return n;
}
