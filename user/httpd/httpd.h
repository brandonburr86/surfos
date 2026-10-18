/*
SurfOS web server: httpd
--------------------
File: httpd.h   Date: 10/17/26
--------------------
What the files of httpd share (the brief: docs/superpowers/plans/2026-10-17-httpd.md).

    main.c     httpd on [folder] [port], httpd serve [folder] [port], httpd off,
               httpd [status]: the command a user types
    server.c   the server: one poll() loop over the listening descriptor and each
               connection, answering requests from the files of one folder
    http.c     the HTTP protocol (http.h), with host tests in tests/host/test_httpd.c

httpd on starts the server as this same program with the internal word SERVER_WORD, the
port and the folder after it, and the listening descriptor as its descriptor 0.
*/

#ifndef _HTTPD_H
#define _HTTPD_H

#include <surf.h>
#include "http.h"

#define HTTPD_PATH   "/initrd/bin/httpd"
#define SERVER_WORD  "--server"
#define DEFAULT_ROOT "/initrd/www"              /* the sample site, from rootfs/www */
#define HTTP_PORT    80
#define MAX_CONNS    6                          /* a connection and the file it sends take 2 of 16 descriptors */
#define BACKLOG      6                          /* connections waiting for a place: a browser opens 6 at once. With
                                                   the places and the listener, 13 of the 15 sockets programs may have */

/* server.c: serves the folder root (absolute, normalised) on the listening descriptor lfd; it returns only
   on an error. verbose: each request is printed on descriptor 1 (httpd serve); otherwise the server detaches
   and says what it does in the kernel's log (httpd on) */
int serve(int lfd, int port, const char *root, int verbose);

#endif
