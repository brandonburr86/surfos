/*
SurfOS Undertow: host-side stand-in for <surf.h>
----------------------
File: surf.h  Date: 10/11/26
----------------------
Undertow's modules include <surf.h> for SurfOS's own calls; the host tests
(tests/host/test_undertow_*.c) put this directory first, and each test defines
the calls that the modules it links use. Standard C comes from the host's
library, built -m32 like SurfOS's programs.
*/

#ifndef UT_HOST_SURF_H
#define UT_HOST_SURF_H

#include <stddef.h>
#include <time.h>

/* the clocks: the test decides what time it is */
unsigned int uptime(void);              /* milliseconds since boot */
time_t ut_sim_time(time_t *t);          /* the hardware clock, as SurfOS's time() reads it */
#define time(t) ut_sim_time(t)

/* the terminal (surf.h, surfos/tty.h) */
#define TTY_RAW       1
#define TTY_GETSIZE   2
#define TTY_PAGE      3
#define TTY_CAPS      4
#define TTY_SKIP      5
#define TTY_CAP_UTF8  1
#define TTY_CAP_COLOR 2
int ttyctl(int op, unsigned int arg);

/* -- transport.c (track T): the network calls -- */
/* SurfOS's own (user/include/surf.h); test_undertow_transport.c simulates them, with the
   network behind them. Compiled with UT_HOST_NET (only transport.c and tls_platform.c are,
   for that test), the standard calls they make on a connection and getentropy() go to the
   simulation too. The headers that declare the real ones come first, so the renaming below
   reaches only the code that includes this file. */
#include <poll.h>
#include <unistd.h>
#include <errno.h>
int tcp_connect(unsigned int ip, unsigned short port);
int net_resolve(const char *host, unsigned int *ip);
#ifdef UT_HOST_NET
int ut_sim_poll(struct pollfd *fds, nfds_t nfds, int timeout);
ssize_t ut_sim_read(int fd, void *buf, size_t n);
ssize_t ut_sim_write(int fd, const void *buf, size_t n);
int ut_sim_close(int fd);
int ut_sim_getentropy(void *buf, size_t n);
#define poll(f, n, t)       ut_sim_poll(f, n, t)
#define read(f, b, n)       ut_sim_read(f, b, n)
#define write(f, b, n)      ut_sim_write(f, b, n)
#define close(f)            ut_sim_close(f)
#define getentropy(b, n)    ut_sim_getentropy(b, n)
#endif

/* -- session.c, view.c, main.c (track S) -- */

#endif
