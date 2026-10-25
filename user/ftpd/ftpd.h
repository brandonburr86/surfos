/*
SurfOS file transfer server: ftpd
--------------------
File: ftpd.h    Date: 10/24/26
--------------------
What the files of ftpd share (the brief: docs/superpowers/plans/2026-10-24-nettools.md).

    main.c     ftpd on [-r] [-a address] [folder] [port] [first-last], ftpd serve ...,
               ftpd off, ftpd [status]: the command a user types
    server.c   the server: one poll() loop over the listening descriptor and each
               session's connections, serving the files of one folder over FTP
    ftp.c      the FTP protocol (ftp.h), with host tests in tests/host/test_ftp.c

ftpd on starts the server as this same program with the internal word SERVER_WORD, the
port, the folder, "rw" or "ro", the passive range and the passive address (-a, when one was
given) after it, and the listening descriptor as its descriptor 0.
*/

#ifndef _FTPD_H
#define _FTPD_H

#include <surf.h>
#include "ftp.h"

#define FTPD_PATH    "/initrd/bin/ftpd"
#define SERVER_WORD  "--server"
#define DEFAULT_ROOT "/"                        /* every mounted volume, at its own path */
#define PASV_FIRST   50000                      /* the passive range, unless one is given */
#define PASV_LAST    50999
#define MAX_SESSIONS 6                          /* a session holds 1 of the 16 descriptors, 3 while it moves a
                                                   file (its data connection and the file), 2 while it waits for
                                                   its passive connection (the listener: 10/25/26, the file is
                                                   opened once the connection is there, and accept() found none
                                                   left after a 150). 0 (the listener), 1, 2 and the kernel log's
                                                   leave 12: three sessions move files at once, and the next is
                                                   told to try again (425) instead of its 150 */
#define BACKLOG      4                          /* connections waiting for a place */

struct ftpd_conf {
    const char *root;                           /* the folder served: absolute, normalised */
    int port;                                   /* the control connection's */
    int readonly;                               /* ftpd on -r: nothing is written */
    int pasv_first, pasv_last;                  /* where passive data connections listen */
    unsigned int pasv_addr;                     /* the address PASV gives (-a, network order); 0: the one the client
                                                   reached, which is right but behind a router (or QEMU's user net) */
    int verbose;                                /* ftpd serve: each command printed on descriptor 1 */
};

/* server.c: serves conf->root on the listening descriptor lfd; it returns only on an error. Verbose:
   each command is printed on descriptor 1 (ftpd serve); otherwise the server detaches and says what it
   does in the kernel's log (ftpd on) */
int serve(int lfd, const struct ftpd_conf *conf);

#endif
