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
to the connection. The session ends when surfsh does (the master reads 0: what is left
for the client is sent, then the connection is closed) or when the client goes (the
connection reads 0 or an error, ECONNRESET, or ETIMEDOUT when a vanished client's
keepalive runs out, or poll() says POLLHUP or POLLERR with nothing left to read: the
master is closed, which hangs the pseudo-terminal up and ends surfsh and whatever it
runs). The shells that end are collected here.

Nothing here waits for one client. A write to a connection takes what fits, so each
session keeps what its connection has not taken yet (the session's output, the answers to
the client's negotiation, the opening) in an output buffer, and poll() is asked about
POLLOUT only while that holds something; the connection is written only when POLLOUT
says it takes bytes. The pseudo-terminal is read, in pieces of PIECE bytes (at most twice
that once encoded), only while the buffer has room for a piece: a client that reads
slowly, or not at all, holds back its own session, whose programs then wait to print,
and the other sessions and new connections go on.

telnetd off kills the server: the kill ends it at the end of its poll(), and its
descriptors close with it, so the port is free, every client gets its FIN and every
pseudo-terminal hangs up.

The log goes to the kernel's (dmesg) through /dev/kmsg, never to a screen: "telnetd:
listening on port 23", "telnetd: session 1 from 10.0.2.2:51234 (pty0)", "telnetd: session
1 ended (...)", and a connection refused.
*/

#include "telnetd.h"
#include "telnet.h"

#define COLS_MIN 8                      /* the window sizes PTY_SETSIZE takes; a client's NAWS is kept in them */
#define COLS_MAX 1024
#define ROWS_MIN 2
#define ROWS_MAX 1024
#define PIECE    512                    /* the most of a session's output read at once */
#define OUT_SIZE 4096                   /* what waits for the client */
#define OUT_KEEP 256                    /* the last of it, kept for answers to the client's negotiation */

struct session {
    int conn, master;                   /* -1: the slot is free */
    int pty, num;                       /* pty<pty>; the session's number for the log */
    int ended;                          /* the shell ended: the connection closes once out is sent */
    int out_len;
    unsigned char out[OUT_SIZE];        /* for the client, not taken by the connection yet */
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

/* bytes for the client, behind what waits already; what does not fit is dropped */
static void queue(struct session *s, const unsigned char *p, int n) {
    if(n > OUT_SIZE - s->out_len) n = OUT_SIZE - s->out_len;
    memcpy(s->out + s->out_len, p, (size_t)n);
    s->out_len += n;
}

/* is there room for another piece of the session's output, with OUT_KEEP left? */
static int room(const struct session *s) {
    return OUT_SIZE - s->out_len >= 2 * PIECE + OUT_KEEP;
}

/* a connection that gets no session: why, if the connection takes it now (a new one has room
   for it all; what it does not take is dropped), then the end: the FIN goes out behind it */
static void refuse(int conn, const char *why) {
    struct pollfd p;
    char msg[96];
    int n = snprintf(msg, sizeof(msg), "\r\ntelnetd: %s\r\n", why);
    p.fd = conn;
    p.events = POLLOUT;
    p.revents = 0;
    if(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT)) write(conn, msg, (size_t)n);
    close(conn);
}

static void end_session(struct session *s, const char *why) {
    close(s->conn);
    close(s->master);                   /* the last master descriptor: the pty hangs up */
    s->conn = s->master = -1;
    s->out_len = 0;
    say("telnetd: session %d ended (%s)", s->num, why);
}

static void new_session(int lfd) {
    char *argv[] = { "surfsh", "-l", NULL }, ip[16], peer[24];
    unsigned char opening[TN_OPENING];
    struct session *s = NULL;
    struct netaddr from;
    int conn, master, pid, i;

    conn = tcp_accept(lfd, &from);
    if(conn < 0) {
        if(errno == EINTR || errno == EAGAIN || errno == ECONNRESET) return;
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
    pid = spawnpty(SURFSH_PATH, argv, master);
    if(pid < 0) {
        say("telnetd: %s: spawnpty %s: %s", peer, SURFSH_PATH, strerror(errno));
        refuse(conn, "surfsh could not be started.");
        close(master);
        return;
    }
    s->conn = conn;
    s->master = master;
    s->ended = 0;
    s->out_len = 0;
    s->pty = ptyctl(master, PTY_INDEX, 0);
    s->num = ++sessions_ever;
    telnet_init(&s->tn);
    queue(s, opening, telnet_opening(&s->tn, opening));    /* sent at the first POLLOUT, before the banner */
    say("telnetd: session %d from %s (pty%d)", s->num, peer, s->pty);
}

/* what the client sent (POLLIN: the read does not wait): keys for the session, answers for the
   client, perhaps a new size; or the end of the connection, the client's or an error's */
static void from_client(struct session *s) {
    unsigned char in[512], data[512], reply[OUT_KEEP];
    struct tn_buf d = { data, 0, sizeof(data) }, r = { reply, 0, sizeof(reply) };
    ssize_t n = read(s->conn, in, sizeof(in));
    if(n <= 0) {
        end_session(s, n < 0 ? strerror(errno) : "the client left");
        return;
    }
    telnet_recv(&s->tn, in, (int)n, &d, &r);
    queue(s, reply, r.len);
    if(s->ended) return;                /* the shell is gone: nothing to type to */
    if(s->tn.resized) {
        int cols = s->tn.cols < COLS_MIN ? COLS_MIN : s->tn.cols > COLS_MAX ? COLS_MAX : s->tn.cols;
        int rows = s->tn.rows < ROWS_MIN ? ROWS_MIN : s->tn.rows > ROWS_MAX ? ROWS_MAX : s->tn.rows;
        s->tn.resized = 0;
        ptyctl(s->master, PTY_SETSIZE, (unsigned int)cols << 16 | (unsigned int)rows);
    }
    if(d.len) write(s->master, data, (size_t)d.len);   /* typed keys: the pty waits briefly, then drops */
}

/* what the session printed, a piece of it, for the client */
static void from_pty(struct session *s) {
    unsigned char in[PIECE], out[2 * PIECE];
    ssize_t n = read(s->master, in, sizeof(in));
    if(n <= 0) {                        /* the shell ended: what is left goes first */
        s->ended = 1;
        if(!s->out_len) end_session(s, "the shell ended");
        return;
    }
    queue(s, out, telnet_send(&s->tn, in, (int)n, out));
}

/* the connection takes bytes (POLLOUT): as many as it will */
static void to_client(struct session *s) {
    ssize_t n = write(s->conn, s->out, (size_t)s->out_len);
    if(n < 0) {
        if(errno != EINTR && errno != EAGAIN) end_session(s, strerror(errno));
        return;
    }
    memmove(s->out, s->out + n, (size_t)(s->out_len - n));
    s->out_len -= (int)n;
    if(s->ended && !s->out_len) end_session(s, "the shell ended");
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
            fds[n].events = s->out_len ? POLLIN | POLLOUT : POLLIN;
            fds[n].revents = 0;
            who[n++] = s;
            if(s->ended || !room(s)) continue;      /* its output waits in the pty for room here */
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
            short ev = fds[i].revents;
            if(!ev || s->conn < 0) continue;    /* nothing, or the session ended just now */
            if(fds[i].fd == s->master) {
                if(!s->ended) from_pty(s);
                continue;
            }
            if((ev & POLLOUT) && s->out_len) to_client(s);
            if(s->conn < 0) continue;
            if(ev & POLLIN) from_client(s);     /* what is left, then 0 or the error that ended it */
            else if(ev & (POLLERR | POLLHUP | POLLNVAL))
                end_session(s, ev & POLLHUP ? "the client left" : "the connection broke");
        }
        if(fds[0].revents) new_session(lfd);
        while(waitpid(-1, NULL, WNOHANG) > 0);              /* the shells of sessions that ended */
    }
}
