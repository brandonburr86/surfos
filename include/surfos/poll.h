/*
SurfOS poll
----------------------
File: poll.h    Date: 10/6/26
----------------------
SYS_poll (struct pollfd *fds, int nfds, int timeout_ms): waits until one of the
descriptors is ready, or timeout_ms has passed (-1: no limit; 0: just look). It returns the
number of entries whose revents is not 0, 0 after the timeout, -EINTR when a kill or
Ctrl+C cuts it short, -EINVAL for nfds above POLL_MAX. As in POSIX: events says what the
caller wants (POLLIN, POLLOUT); revents gets what is true, and POLLERR, POLLHUP and
POLLNVAL (a descriptor that is not open) are reported whether asked for or not; an entry
whose fd is negative is skipped.

What is ready: a connection or a listening socket (surfos/sock.h); a pseudo-terminal's
master side (surfos/pty.h: POLLIN when the session printed something or has ended,
POLLHUP once it has ended and everything it printed was read, POLLOUT while a key fits in
its input queue); the console (POLLIN when a key is queued); anything else (a file,
/dev/null) is always ready for both.

In the kernel a vnode answers through vnode_ops->poll (fs/vfs.h), and whatever changes a
descriptor's readiness calls poll_wake(), which wakes every task in poll().

This header uses no SurfOS types: user programs reach it through <poll.h>.
*/

#ifndef _SURFOS_POLL_H
#define _SURFOS_POLL_H

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

#define POLL_MAX 64

#endif
