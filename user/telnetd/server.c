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
to the connection. The session ends when surfsh does (the master reads 0: the master is
closed at once, which frees the pseudo-terminal, what is left for the client is sent,
then the connection is closed), or when the client goes (the connection reads 0 or an
error: ECONNRESET, or ETIMEDOUT when a vanished client's keepalive runs out; the master
is closed, which hangs the pseudo-terminal up and ends surfsh and whatever it runs).

Nothing here waits for one client. A write to a connection takes what fits, so each
session keeps what its connection has not taken yet (the session's output, the answers to
the client's negotiation, the opening) in an output buffer, and poll() is asked about
POLLOUT only while that holds something; the connection is written only when POLLOUT
says it takes bytes. The pseudo-terminal is read, in pieces of PIECE bytes (at most twice
that and one more once encoded), only while the buffer has room for a piece and OUT_KEEP
for answers: a client that reads slowly holds back its own session, whose programs then
wait to print, and the other sessions and new connections go on. A client that takes
nothing for STALL_MS while output waits for it is gone or stuck: its session is ended, so
that clients which stop reading cannot keep every session for good. The answers to one
read from the client go into the buffer whole or not at all (a client that sends without
reading loses answers, never a part of one).

The other way: the client is always read, so that its Ctrl+C always arrives, and what it
types waits in a buffer of KEYS_SIZE keys (what does not fit is dropped, as the keyboard
drops keys) until the master takes it. A write on the master never waits: it takes the
keys that fit in the terminal's input queue, and poll() is asked about the master's
POLLOUT only while keys wait. A Ctrl+C (typed, or IAC IP) drops the keys that waited
before it and is written at once: in line mode it always fits (and drops what was typed
ahead), and when it does not (raw mode, a full queue) it waits first in line. So a client
that types faster than its session reads holds back only its own keys.

The shells are collected here (waitpid) after every poll(): while a session's shell has
ended or is ending, poll() waits REAP_MS at most, so that none stays a zombie long. When
accept() fails for want of something (ENOMEM, EMFILE), the listening descriptor rests for
PAUSE_MS before it is polled again, so a lasting failure cannot keep the loop spinning.

telnetd off kills the server: the kill ends it at the end of its poll(), and its
descriptors close with it, so the port is free, every client gets its FIN and every
pseudo-terminal hangs up.

The log goes to the kernel's (dmesg) through /dev/kmsg, never to a screen: "telnetd:
listening on port 23", "telnetd: session 1 from 10.0.2.2:51234 (pty0, surfsh pid 13)",
"telnetd: session 1 ended (...)", and a connection refused.
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
#define STALL_MS 60000                  /* output waiting this long with nothing taken: the session ends */
#define REAP_MS  200                    /* poll()'s longest wait while a shell is to be collected soon */
#define TICK_MS  1000                   /* ... while sessions run (a shell that ended while its output waits) */
#define PAUSE_MS 1000                   /* the listening descriptor rests this long after accept() failed */
#define KEYS_SIZE 1024                  /* typed keys the master has not taken yet */

struct session {
    int conn, master;                   /* conn -1: the slot is free; master -1 once the shell has ended */
    int pty, num, pid;                  /* pty<pty>; the session's number for the log; its shell */
    int ended;                          /* the shell ended: the connection closes once out is sent */
    unsigned int since;                 /* uptime() when the client last took output, or output began to wait */
    int out_len;
    unsigned char out[OUT_SIZE];        /* for the client, not taken by the connection yet */
    int keys_len;
    unsigned char keys[KEYS_SIZE];      /* typed, not taken by the master yet */
    struct telnet tn;
};

static struct session sessions[MAX_SESSIONS];
static int kmsg = -1, sessions_ever;
static int shells;                      /* shells started and not collected yet */
static int resting;                     /* the listening descriptor is left out of poll() until rest_end */
static unsigned int rest_end;

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

/* bytes for the client, behind what waits already: all of them, or none when they do not fit
   (only answers can meet a full buffer: the session's output is read only when there is room) */
static void queue(struct session *s, const unsigned char *p, int n) {
    if(n <= 0 || n > OUT_SIZE - s->out_len) return;
    if(!s->out_len) s->since = uptime();            /* output begins to wait: the stall clock starts */
    memcpy(s->out + s->out_len, p, (size_t)n);
    s->out_len += n;
}

/* is there room for another piece of the session's output, with OUT_KEEP left? */
static int room(const struct session *s) {
    return OUT_SIZE - s->out_len >= 2 * PIECE + 1 + OUT_KEEP;
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
    if(s->master >= 0) close(s->master);    /* the last master descriptor: the pty hangs up */
    s->conn = s->master = -1;
    s->out_len = s->keys_len = 0;
    say("telnetd: session %d ended (%s)", s->num, why);
}

static void new_session(int lfd) {
    char *argv[] = { "surfsh", "-l", NULL }, ip[16], peer[24], why[64];
    unsigned char opening[TN_OPENING];
    struct session *s = NULL;
    struct netaddr from;
    int conn, master, pid, i;

    conn = tcp_accept(lfd, &from);
    if(conn < 0) {
        if(errno == EINTR || errno == EAGAIN) return;
        if(errno == EBADF || errno == EINVAL) {         /* descriptor 0 is no listening socket after all */
            say("telnetd: accept: %s; the server ends", strerror(errno));
            _exit(1);
        }
        say("telnetd: accept: %s; new connections wait a moment", strerror(errno));   /* ENOMEM, EMFILE */
        resting = 1;
        rest_end = uptime() + PAUSE_MS;
        return;
    }
    snprintf(peer, sizeof(peer), "%s:%u", ipstr(from.ip, ip), (unsigned int)from.port);
    for(i = 0; i < MAX_SESSIONS && !s; i++) if(sessions[i].conn < 0) s = &sessions[i];
    if(!s) {
        snprintf(why, sizeof(why), "%d sessions are open already; try again later.", MAX_SESSIONS);
        refuse(conn, why);
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
    shells++;
    s->conn = conn;
    s->master = master;
    s->pid = pid;
    s->ended = 0;
    s->out_len = s->keys_len = 0;
    s->pty = ptyctl(master, PTY_INDEX, 0);
    s->num = ++sessions_ever;
    telnet_init(&s->tn);
    queue(s, opening, telnet_opening(&s->tn, opening));    /* sent at the first POLLOUT, before the banner */
    say("telnetd: session %d from %s (pty%d, surfsh pid %d)", s->num, peer, s->pty, pid);
}

/* the keys that wait, to the master: as many as it takes (it never waits; -EAGAIN when none fits) */
static void to_pty(struct session *s) {
    ssize_t n = write(s->master, s->keys, (size_t)s->keys_len);
    if(n <= 0) {
        if(n < 0 && errno != EAGAIN && errno != EINTR) s->keys_len = 0;    /* the pty is gone: nobody reads them */
        return;
    }
    memmove(s->keys, s->keys + n, (size_t)(s->keys_len - n));
    s->keys_len -= (int)n;
}

/* typed keys, behind those that wait; a Ctrl+C drops those before it and goes to the master at once */
static void type_keys(struct session *s, const unsigned char *p, int n) {
    int i, room;
    for(i = n - 1; i >= 0 && p[i] != 0x03; i--);
    if(i >= 0) {
        s->keys_len = 0;
        p += i;
        n -= i;
    }
    room = KEYS_SIZE - s->keys_len;
    if(n > room) n = room;                  /* beyond the buffer: dropped, as a keyboard drops keys */
    memcpy(s->keys + s->keys_len, p, (size_t)n);
    s->keys_len += n;
    if(i >= 0) to_pty(s);
}

/* what the client sent (the read does not wait: poll() said so): keys for the session, answers
   for the client, perhaps a new size; or the end of the connection, the client's or an error's */
static void from_client(struct session *s) {
    unsigned char in[512], data[512], reply[OUT_KEEP];
    struct tn_buf d = { data, 0, sizeof(data) }, r = { reply, 0, sizeof(reply) };
    ssize_t n = read(s->conn, in, sizeof(in));
    if(n <= 0) {
        end_session(s, n < 0 ? strerror(errno) : "the client left");
        return;
    }
    telnet_recv(&s->tn, in, (int)n, &d, &r);
    queue(s, reply, r.len);             /* the answers whole, or none when the client does not read */
    if(s->master < 0) return;           /* the shell has ended: nothing to type to */
    if(s->tn.resized) {
        int cols = s->tn.cols < COLS_MIN ? COLS_MIN : s->tn.cols > COLS_MAX ? COLS_MAX : s->tn.cols;
        int rows = s->tn.rows < ROWS_MIN ? ROWS_MIN : s->tn.rows > ROWS_MAX ? ROWS_MAX : s->tn.rows;
        s->tn.resized = 0;
        ptyctl(s->master, PTY_SETSIZE, (unsigned int)cols << 16 | (unsigned int)rows);
    }
    if(d.len) type_keys(s, data, d.len);
}

/* what the session printed, a piece of it, for the client; or the end of the shell */
static void from_pty(struct session *s) {
    unsigned char in[PIECE], out[2 * PIECE + 1];
    ssize_t n = read(s->master, in, sizeof(in));
    if(n <= 0) {                        /* the shell ended: its pty is freed now, what is left is sent */
        close(s->master);
        s->master = -1;
        s->ended = 1;
        s->keys_len = 0;
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
    if(n > 0) s->since = uptime();      /* the client takes output: the stall clock starts again */
    memmove(s->out, s->out + n, (size_t)(s->out_len - n));
    s->out_len -= (int)n;
    if(s->ended && !s->out_len) end_session(s, "the shell ended");
}

/* how long poll() may wait: until a session's stall limit, the end of the listening
   descriptor's rest, or a look for shells to collect; -1: until something happens */
static int wait_ms(unsigned int now) {
    int i, ms = -1, running = 0;
    for(i = 0; i < MAX_SESSIONS; i++) {
        const struct session *s = &sessions[i];
        if(s->conn < 0) continue;
        if(s->master >= 0) running++;
        if(s->out_len) {
            unsigned int waited = now - s->since;
            int left = waited >= STALL_MS ? 0 : (int)(STALL_MS - waited);
            if(ms < 0 || left < ms) ms = left;
        }
    }
    if(shells > running && (ms < 0 || ms > REAP_MS)) ms = REAP_MS;      /* a shell ended or is ending */
    else if(shells && (ms < 0 || ms > TICK_MS)) ms = TICK_MS;
    if(resting) {
        int left = (int)(rest_end - now) > 0 ? (int)(rest_end - now) : 0;
        if(ms < 0 || left < ms) ms = left;
    }
    return ms;
}

int serve(int lfd, int port) {
    struct pollfd fds[1 + 2 * MAX_SESSIONS];
    struct session *who[1 + 2 * MAX_SESSIONS];  /* whose descriptor each is (NULL: the listener) */
    char master[1 + 2 * MAX_SESSIONS];          /* 1: a session's master, 0: its connection */
    int i;
    for(i = 0; i < MAX_SESSIONS; i++) sessions[i].conn = sessions[i].master = -1;
    kmsg = open("/dev/kmsg", O_WRONLY);
    if(detach() < 0) say("telnetd: detach: %s", strerror(errno));
    say("telnetd: listening on port %d", port);
    for(;;) {
        unsigned int now = uptime();
        int n = 0, r, l = -1;
        if(resting && (int)(now - rest_end) >= 0) resting = 0;
        if(!resting) {
            fds[n].fd = lfd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            who[n] = NULL;
            l = n++;
        }
        for(i = 0; i < MAX_SESSIONS; i++) {
            struct session *s = &sessions[i];
            if(s->conn < 0) continue;
            fds[n].fd = s->conn;
            fds[n].events = s->out_len ? POLLIN | POLLOUT : POLLIN;
            fds[n].revents = 0;
            master[n] = 0;
            who[n++] = s;
            if(s->master < 0 || (!room(s) && !s->keys_len)) continue;
            fds[n].fd = s->master;
            fds[n].events = (room(s) ? POLLIN : 0) | (s->keys_len ? POLLOUT : 0);   /* output while there is room */
            fds[n].revents = 0;                                                    /* for it; keys while they wait */
            master[n] = 1;
            who[n++] = s;
        }
        r = poll(fds, (nfds_t)n, wait_ms(now));
        if(r < 0) {
            if(errno == EINTR) continue;        /* a kill ends this task when the call does */
            say("telnetd: poll: %s; the server ends", strerror(errno));
            return 1;
        }
        for(i = 0; i < n; i++) {
            struct session *s = who[i];
            short ev = fds[i].revents;
            if(!ev || !s || s->conn < 0) continue;      /* nothing, the listener, or a session ended just now */
            if(master[i]) {
                if((ev & POLLOUT) && s->master >= 0 && s->keys_len) to_pty(s);
                if((ev & (POLLIN | POLLHUP | POLLERR)) && s->master >= 0) from_pty(s);
                else if(ev & POLLNVAL) end_session(s, "the pseudo-terminal broke");
                continue;
            }
            if((ev & POLLOUT) && s->out_len) to_client(s);
            if(s->conn < 0) continue;
            if(ev & (POLLIN | POLLHUP | POLLERR)) from_client(s);   /* the read says which: data, the end, an error */
            else if(ev & POLLNVAL) end_session(s, "the connection broke");
        }
        now = uptime();
        for(i = 0; i < MAX_SESSIONS; i++) {
            struct session *s = &sessions[i];
            if(s->conn >= 0 && s->out_len && now - s->since >= STALL_MS)
                end_session(s, "the client took nothing for 60 s");
        }
        if(l >= 0 && fds[l].revents) new_session(lfd);
        for(;;) {                                   /* the shells of sessions that ended */
            int pid = waitpid(-1, NULL, WNOHANG);
            if(pid > 0) {
                shells--;
                continue;
            }
            if(pid < 0 && errno == ECHILD) shells = 0;
            break;
        }
    }
}
