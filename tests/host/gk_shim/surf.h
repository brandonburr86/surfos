/*
SurfOS host tests: what libgui and the toolkit use of <surf.h>
--------------------
File: surf.h    Date: 11/05/26
--------------------
For the host tests of programs' windows (the desktop applications, track kit): libgui
(user/libsurf/gui.c) and the toolkit (gk.h, user/libsurf/gk*.c) are built for the host against its
own headers, and this stands in for SurfOS's surf.h, with only the window calls and the clock,
which the fake desktop provides (tests/host/gk_fake.c). It comes first on the include path
(-Itests/host/gk_shim), and user/include after the host's (-idirafter) for gui.h and surfos/win.h.
*/
#ifndef _SURF_H
#define _SURF_H

#include <stddef.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <surfos/win.h>

int winopen(const struct win_open *o);
int winctl(int fd, int op, void *arg);
unsigned int uptime(void);
int msleep(unsigned int ms);            /* 11/05/26: the fake's clock moves on, nothing waits */

#endif
