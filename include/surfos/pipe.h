/*
SurfOS pipes
----------------------
File: pipe.h    Date: 10/24/26
----------------------
SYS_pipe (int fds[2]): a pipe, a buffer of PIPE_SIZE bytes in the kernel with two ends:
fds[0] reads, in order, what fds[1] writes. -EMFILE without two free descriptors, -ENFILE
when PIPE_MAX pipes are open already, -ENOMEM, -EFAULT. The ends are descriptors like any
other: dup2() and spawnfd() pass them on (a pipeline's programs get them as 0 and 1), and
an end is closed when its last descriptor is, in whichever process (also as a process
ends). The pipe is gone once both ends are.

read() on fds[0] waits while the pipe is empty and a descriptor of fds[1] is open anywhere,
and returns what is there, at most len bytes; 0 once it is empty and every descriptor of
fds[1] is closed (the end of the input).
write() on fds[1] writes all len bytes, waiting while the pipe is full. A write of up to
PIPE_BUF bytes goes in whole, never mixed with another writer's bytes; a longer one may be,
in pieces, when several programs write at once. When every descriptor of fds[0] is closed,
nobody will read: a write answers -EPIPE (in the middle of a write, the number of bytes
written by then), and libsurf ends a program that gets EPIPE from a pipe with status 141,
as SIGPIPE does elsewhere, unless it asked for the error (broken_pipe_exit(0) in surf.h).
Every wait of a pipe is one that a kill, Ctrl+C or a hangup cuts short: read answers
-EINTR, write the bytes written by then or -EINTR.
poll() (surfos/poll.h): fds[0] is POLLIN when there is something to read or the end has
come, POLLHUP once every descriptor of fds[1] is closed; fds[1] is POLLOUT when PIPE_BUF
bytes fit (a write of up to that many does not wait), POLLOUT and POLLERR once nobody
reads. lseek() answers -ESPIPE. fstat() says file system "pipefs" and size 0; isatty() is 0.

This header uses no SurfOS types: user programs reach it through user/include/surfos/pipe.h.
*/

#ifndef _SURFOS_PIPE_H
#define _SURFOS_PIPE_H

#define PIPE_SIZE   8192            /* bytes a pipe holds */
#define PIPE_BUF    4096            /* a write of up to this many goes in whole */
#define PIPE_MAX    32              /* pipes open at once, in all */

#endif
