/*
SurfOS poll: the wakeup
--------------------
File: poll.c    Date: 10/6/26
--------------------
The one wait queue of poll() (surfos/poll.h). A task in poll() looks at its descriptors
and, if none is ready, waits here; whatever may make a descriptor ready calls poll_wake(),
which wakes them all to look again. One queue for everything is simple and costs little
while few tasks poll (the telnet server, one at a time). SYS_poll (sys_poll) is not
there yet: it answers -ENOSYS.
*/

#include <surfos/types.h>
#include <surfos/wait.h>
#include <fs/vfs.h>
#include <surfos/sysimpl.h>

static wait_queue_t poll_wq;

void poll_init(void) {
    wq_init(&poll_wq);
}

/* from task code and from interrupt handlers alike */
void poll_wake(void) {
    wake_up_all(&poll_wq);
}

wait_queue_t *poll_queue(void) {
    return &poll_wq;
}

int sys_poll(struct pollfd *fds, int nfds, int timeout_ms) {
    (void)fds; (void)nfds; (void)timeout_ms;
    return -ENOSYS;
}
