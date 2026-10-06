/*
SurfOS user library: <poll.h>
----------------------
File: poll.h    Date: 10/6/26
----------------------
poll() over the kernel's SYS_poll (surfos/poll.h has what is ready when).
*/

#ifndef _POLL_H
#define _POLL_H

#include <surfos/poll.h>

typedef unsigned int nfds_t;

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);    /* entries ready, 0 timeout, -1 errno */

#endif
