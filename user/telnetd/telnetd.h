/*
SurfOS telnet server: telnetd
--------------------
File: telnetd.h Date: 10/6/26
--------------------
What the files of telnetd share (the brief: docs/superpowers/plans/2026-10-06-telnetd.md).

    main.c     telnetd on [port], telnetd off, telnetd [status]: the command a user types
    server.c   the server: detached, one poll() loop over the listening descriptor and each
               session's connection and pseudo-terminal
    telnet.c   the telnet protocol (telnet.h), with host tests in tests/host/test_telnet.c

telnetd on starts the server as this same program with the internal word SERVER_WORD,
the port after it, and the listening descriptor as its descriptor 0.
*/

#ifndef _TELNETD_H
#define _TELNETD_H

#include <surf.h>

#define TELNETD_PATH  "/initrd/bin/telnetd"
#define SURFSH_PATH   "/initrd/bin/surfsh"
#define SERVER_WORD   "--server"
#define TELNET_PORT   23
#define MAX_SESSIONS  4

/* server.c: the server's loop on the listening descriptor lfd; it returns only on an error */
int serve(int lfd, int port);

#endif
