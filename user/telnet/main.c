/*
SurfOS telnet client: telnet
--------------------
File: main.c      Date: 11/01/26
--------------------
telnet host [port]: a session on another computer's terminal server, in raw mode: the terminal
sends each key as it is typed and shows what comes back. Port 23 unless another is given; host is
an address or a name. On port 23 this end offers its options at once; on another port it says
nothing until the other end negotiates, so it also serves to talk to a mail or web server by
hand: a line typed then is edited here and goes out at Return (tnclient.h has the protocol).
Ctrl+] gives a prompt: quit leaves, status says how things stand, send ayt asks whether the other
end is there, an empty line goes back. Input from a pipe or a file goes out as lines (no raw mode
and no prompt), all of it, waiting while the other end is slow: when it ends, the session ends with
the other end's close or after 2 seconds without a byte. The terminal is put back however the
session ends: whatever the server left open is ended and the screen set right (session.c), raw mode
ends here, and a kill ends it in the kernel. Status 0 when quit or the other end ended it, 2 for a
usage mistake, a connection that could not be made or was lost, or input from a pipe that did not
all go: the other end closed first, or took nothing for a minute.
*/

#include <surf.h>
#include "tnsession.h"

#define TELNET_PORT 23

static int usage(void) {
    fprintf(stderr, "usage: telnet host [port]\n");
    return 2;
}

/* a port number as typed: digits only, 1-65535; 0 when it is none */
static int port_of(const char *word) {
    long v = 0;
    const char *p;
    if(!*word || strlen(word) > 5) return 0;
    for(p = word; *p; p++) {
        if(*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
    }
    return v >= 1 && v <= 65535 ? (int)v : 0;
}

int main(int argc, char **argv) {
    struct tn_session s;
    unsigned int ip;
    char a[16], where[40];
    int port = TELNET_PORT, why;
    if(argc < 2 || argc > 3 || argv[1][0] == '-') return usage();
    if(argc == 3 && !(port = port_of(argv[2]))) {
        fprintf(stderr, "telnet: %s is no port (1-65535)\n", argv[2]);
        return 2;
    }
    if(ipparse(argv[1], &ip) < 0 && net_resolve(argv[1], &ip) < 0) {
        fprintf(stderr, "telnet: %s: %s\n", argv[1], errno == ENOENT ? "no such host" :
                errno == EINVAL ? "not a host name" : strerror(errno));
        return 2;
    }
    ipstr(ip, a);
    if((s.sock = tcp_connect(ip, (unsigned short)port)) < 0) {
        fprintf(stderr, "telnet: %s port %d: %s\n", a, port, strerror(errno));
        return 2;
    }
    snprintf(where, sizeof(where), "%s port %d", a, port);
    s.in = 0;
    s.out = 1;
    s.terminal = isatty(0);
    s.out_terminal = isatty(1);
    s.speak_first = port == TELNET_PORT;
    s.where = where;
    fflush(stdout);
    if(s.terminal && ttyctl(TTY_RAW, 1) < 0) {
        fprintf(stderr, "telnet: the terminal will not go into raw mode: %s\n", strerror(errno));
        close(s.sock);
        return 2;
    }
    fprintf(stderr, "telnet: connected to %s%s\n", where, s.terminal ? "; Ctrl+] gives a prompt" : "");
    why = tn_session_run(&s);
    {
        int e = errno;
        if(s.terminal) ttyctl(TTY_RAW, 0);
        close(s.sock);
        errno = e;
    }
    switch(why) {
    case TNS_CLOSED:
        fprintf(stderr, "telnet: the other end closed the connection\n");
        return 0;
    case TNS_LOST:
        fprintf(stderr, "telnet: the connection was lost: %s\n", strerror(errno));
        return 2;
    case TNS_SCREEN:
        fprintf(stderr, "telnet: the output cannot be written: %s\n", strerror(errno));
        return 2;
    case TNS_STALLED:
        fprintf(stderr, "telnet: the other end took nothing for a minute: the input did not all go\n");
        return 2;
    case TNS_UNSENT:
        fprintf(stderr, "telnet: the other end closed before all the input went\n");
        return 2;
    }
    return 0;
}
