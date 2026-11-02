/*
SurfOS telnet client: telnet
--------------------
File: tnsession.h  Date: 11/01/26
--------------------
A telnet session over a connection that is made (session.c): keys to the server, what it sends
to the screen, the small prompt of Ctrl+]. main.c does the rest. The session makes the system
calls it needs through <surf.h>, so tests/host/test_telnetc.c runs it against a simulated
kernel, as test_telnetd runs the server's loop.
*/

#ifndef _TNSESSION_H
#define _TNSESSION_H

enum { TNS_QUIT, TNS_CLOSED, TNS_LOST, TNS_ENDED, TNS_SCREEN };

struct tn_session {
    int sock;                       /* the connection */
    int in, out;                    /* keys and the screen: 0 and 1 */
    int terminal;                   /* in is a terminal, in raw mode: Ctrl+] works, its window is told */
    int speak_first;                /* the connection is to port 23: this end makes its offers at once */
    const char *where;              /* the other end, for status */
};

/* The session, until it ends: TNS_QUIT, quit at the prompt; TNS_CLOSED, the server closed the
   connection; TNS_LOST, it failed, errno says why; TNS_ENDED, the input ended (a pipe's, or the
   terminal hung up) and the server said no more; TNS_SCREEN, the output cannot be written, errno
   says why. What it left on the terminal has been undone before it returns. */
int tn_session_run(const struct tn_session *s);

#endif
