/* Host-side test shim (tests/host/test_telnetc.c, 11/01/26): the SurfOS user API that the telnet client uses, mapped
   to the test's simulated kernel; the host's C library does the rest, so these files build with clang's sanitizers
   on the Mac. */
#ifndef SIM_TELNETC_SURF_H
#define SIM_TELNETC_SURF_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include "sim.h"

#define TTY_RAW     1
#define TTY_GETSIZE 2

#undef fprintf
#undef fflush
#define read        sim_read
#define write       sim_write
#define close       sim_close
#define poll(p, n, t) sim_poll(p, n, t)
#define ttyctl      sim_ttyctl
#define isatty      sim_isatty
#define uptime      sim_uptime
#define tcp_connect sim_tcp_connect
#define net_resolve sim_net_resolve
#define ipparse     sim_ipparse
#define ipstr       sim_ipstr
#define fprintf     sim_fprintf
#define fflush      sim_fflush
#endif
