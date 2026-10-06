/*
SurfOS telnet server: telnetd
--------------------
File: server.c  Date: 10/6/26
--------------------
The server. telnetd on starts it with the listening descriptor as its descriptor 0 and
/dev/null as 1 and 2. It detach()es from the shell that started it (init is its parent
from then on, and Ctrl+C there does not reach it) and serves in one loop: poll() over
the listening descriptor and each session's connection and pseudo-terminal master.

A connection: up to MAX_SESSIONS at once, and one more is told so and closed. A session
gets a pseudo-terminal (openpty; 80x24 until the client's NAWS says otherwise), the
telnet opening (telnet.c) and surfsh -l on the pseudo-terminal (spawnpty), so it begins
with the banner and the prompt. Then bytes go both ways through the protocol: what the
client types to the master, where it arrives as typed keys, and what the session prints
to the connection. The session ends when surfsh does (the master reads 0: the
connection is closed, and the client sees the end) or when the client goes (the
connection reads 0 or is reset: the master is closed, which hangs the pseudo-terminal
up and ends surfsh and whatever it runs). The shells that end are collected here.

A write to a connection waits until every byte is queued (net/sock.c), with no limit, so
a client that reads slowly could hold up every session. What a session prints is read in
pieces of PIECE bytes (at most twice that once encoded), and only while the connection
has room (POLLOUT; poll() is asked about it once a piece found none): a slow client then
slows its own session, whose output waits in its pseudo-terminal, and the others go on.

telnetd off kills the server: the kill ends it at the end of its poll(), and its
descriptors close with it, so the port is free, every client gets its FIN and every
pseudo-terminal hangs up.

The log goes to the kernel's (dmesg) through /dev/kmsg, never to a screen: "telnetd:
listening on port 23", "telnetd: session 1 from 10.0.2.2:51234 (pty0)", "telnetd: session
1 ended (...)", and a connection refused.
*/

#include "telnetd.h"
#include "telnet.h"

#define SIZE_MAX_TN 1000                /* the largest window a client's NAWS may set */
#define PIECE       512                 /* the most of a session's output read and written at once */

struct session {
    int conn, master;                   /* -1: the slot is free */
    int pty, num;                       /* pty<pty>; the session's number for the log */
    int full;                           /* the connection had no room: the pty waits until it has */
    struct telnet tn;
};

static struct session sessions[MAX_SESSIONS];
static int kmsg = -1, sessions_ever;

/* a line for the kernel's log */
static void say(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    int n;
    if(kmsg < 0) return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if(n < 0) return;
    if(n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    write(kmsg, buf, (size_t)n);
}

static int write_all(int fd, const void *p, int n) {
    const char *s = p;
    while(n > 0) {
        ssize_t w = write(fd, s, (size_t)n);
        if(w <= 0) return -1;
        s += w;
        n -= (int)w;
    }
    return 0;
}

/* a connection that gets no session: why, then the end */
static void refuse(int conn, const char *why) {
    write_all(conn, "\r\ntelnetd: ", 11);
    write_all(conn, why, (int)strlen(why));
    write_all(conn, "\r\n", 2);
    close(conn);
}

static void end_session(struct session *s, const char *why) {
    close(s->conn);
    close(s->master);                   /* the last master descriptor: the pty hangs up */
    s->conn = s->master = -1;
    say("telnetd: session %d ended (%s)", s->num, why);
}

static void new_session(int lfd) {
    char *argv[] = { "surfsh", "-l", NULL }, ip[16], peer[24];
    unsigned char opening[TN_OPENING];
    struct session *s = NULL;
    struct netaddr from;
    int conn, master, pid, i, n;

    conn = tcp_accept(lfd, &from);
    if(conn < 0) {
        if(errno == EINTR || errno == EAGAIN) return;
        say("telnetd: accept: %s; the server ends", strerror(errno));
        _exit(1);                       /* the listening descriptor is not one (or broke) */
    }
    snprintf(peer, sizeof(peer), "%s:%u", ipstr(from.ip, ip), (unsigned int)from.port);
    for(i = 0; i < MAX_SESSIONS && !s; i++) if(sessions[i].conn < 0) s = &sessions[i];
    if(!s) {
        refuse(conn, "4 sessions are open already; try again later.");
        say("telnetd: %s refused: %d sessions are open", peer, MAX_SESSIONS);
        return;
    }
    master = openpty();
    if(master < 0) {
        refuse(conn, "no pseudo-terminal is free; try again later.");
        say("telnetd: %s refused: openpty: %s", peer, strerror(errno));
        return;
    }
    ptyctl(master, PTY_SETSIZE, (80u << 16) | 24u);
    telnet_init(&s->tn);
    n = telnet_opening(&s->tn, opening);
    if(write_all(conn, opening, n) < 0) {
        close(conn);
        close(master);
        return;
    }
    pid = spawnpty(SURFSH_PATH, argv, master);
    if(pid < 0) {
        say("telnetd: %s: spawnpty %s: %s", peer, SURFSH_PATH, strerror(errno));
        refuse(conn, "surfsh could not be started.");
        close(master);
        return;
    }
    s->conn = conn;
    s->master = master;
    s->full = 0;
    s->pty = ptyctl(master, PTY_INDEX, 0);
    s->num = ++sessions_ever;
    say("telnetd: session %d from %s (pty%d)", s->num, peer, s->pty);
}

/* what the client sent: keys for the session, answers for the client, perhaps a new size */
static void from_client(struct session *s) {
    unsigned char in[512], data[512], reply[1024];
    struct tn_buf d = { data, 0, sizeof(data) }, r = { reply, 0, sizeof(reply) };
    ssize_t n = read(s->conn, in, sizeof(in));
    if(n <= 0) {
        end_session(s, n < 0 ? strerror(errno) : "the client left");
        return;
    }
    telnet_recv(&s->tn, in, (int)n, &d, &r);
    if(r.len && write_all(s->conn, reply, r.len) < 0) {
        end_session(s, "the connection broke");
        return;
    }
    if(s->tn.resized) {
        unsigned int cols = s->tn.cols < SIZE_MAX_TN ? (unsigned int)s->tn.cols : SIZE_MAX_TN;
        unsigned int rows = s->tn.rows < SIZE_MAX_TN ? (unsigned int)s->tn.rows : SIZE_MAX_TN;
        s->tn.resized = 0;
        ptyctl(s->master, PTY_SETSIZE, cols << 16 | rows);
    }
    if(d.len && write_all(s->master, data, d.len) < 0) end_session(s, "the terminal broke");
}

/* what the session printed, for the client: a piece, when the connection has room for it */
static void from_pty(struct session *s) {
    unsigned char in[PIECE], out[2 * PIECE];
    struct pollfd room;
    ssize_t n;
    int k;
    room.fd = s->conn;
    room.events = POLLOUT;
    room.revents = 0;
    if(poll(&room, 1, 0) < 0 || !(room.revents & POLLOUT)) {
        if(room.revents & (POLLHUP | POLLERR | POLLNVAL)) end_session(s, "the client left");
        else s->full = 1;                       /* until poll() says the connection has room */
        return;
    }
    n = read(s->master, in, sizeof(in));
    if(n <= 0) {
        end_session(s, "the shell ended");
        return;
    }
    k = telnet_send(&s->tn, in, (int)n, out);
    if(write_all(s->conn, out, k) < 0) end_session(s, "the connection broke");
}

int serve(int lfd, int port) {
    struct pollfd fds[1 + 2 * MAX_SESSIONS];
    struct session *who[1 + 2 * MAX_SESSIONS];
    int i;
    for(i = 0; i < MAX_SESSIONS; i++) sessions[i].conn = sessions[i].master = -1;
    kmsg = open("/dev/kmsg", O_WRONLY);
    if(detach() < 0) say("telnetd: detach: %s", strerror(errno));
    say("telnetd: listening on port %d", port);
    for(;;) {
        int n = 1, r;
        fds[0].fd = lfd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        for(i = 0; i < MAX_SESSIONS; i++) {
            struct session *s = &sessions[i];
            if(s->conn < 0) continue;
            fds[n].fd = s->conn;
            fds[n].events = s->full ? POLLIN | POLLOUT : POLLIN;
            fds[n].revents = 0;
            who[n++] = s;
            if(s->full) continue;                   /* its output waits for room in the connection */
            fds[n].fd = s->master;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            who[n++] = s;
        }
        r = poll(fds, (nfds_t)n, -1);
        if(r < 0) {
            if(errno == EINTR) continue;        /* a kill ends this task when the call does */
            say("telnetd: poll: %s; the server ends", strerror(errno));
            return 1;
        }
        for(i = 1; i < n; i++) {
            struct session *s = who[i];
            if(!fds[i].revents || s->conn < 0) continue;    /* nothing, or the session ended just now */
            if(fds[i].fd != s->conn) from_pty(s);
            else {
                if(fds[i].revents & POLLOUT) s->full = 0;   /* room again: the pty is read next time */
                if(fds[i].revents & ~POLLOUT) from_client(s);
            }
        }
        if(fds[0].revents) new_session(lfd);
        while(waitpid(-1, NULL, WNOHANG) > 0);              /* the shells of sessions that ended */
    }
}
