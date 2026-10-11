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

/* -- session.c, view.c, main.c (track S) -- */

#endif
