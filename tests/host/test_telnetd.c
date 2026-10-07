/* Host-side test of telnetd's server loop (from the review of 10/6/26): user/telnetd/server.c,
   included unchanged, against a simulated kernel (its API through tests/host/telnetd/surf.h).
   The kernel: sockets with an 8 KB send buffer whose write() takes what fits; pty masters with
   an 8 KB output ring and a 255-key input queue; shells that print, exit, or die some time after
   a hangup; a clock that moves only in poll(), by a little each time, or by the whole timeout when
   nothing comes; masters whose write takes the keys that fit in a 255-key input queue (in line
   mode a Ctrl+C needs no room and drops what was typed ahead) and says EAGAIN when none fits.
   Random clients connect, send IAC-heavy input, stall for a while or for good, drain, half-close
   or reset; shells read their keys or not; accept(), openpty() and spawnpty() fail now and then.
   Checks: no read or write that would block, and none on a closed or wrong descriptor; POLLOUT
   asked exactly while output waits; the bytes each client gets are, in order, the opening, the
   answers and the encoded session output (a shadow protocol state says what they should be),
   answers dropped only whole and session output never; the keys reaching each master are the
   decoded client data as the server's key buffer passes them on (a Ctrl+C drops those before
   it, and what overflows the buffer is dropped), written only on POLLOUT unless a Ctrl+C leads,
   and POLLOUT asked on a master exactly while keys wait; NAWS sizes in range; at most
   MAX_SESSIONS sessions, refusals only when full with no session stalled, without a pty or when
   the shell could not start; the server never exits; a master that read 0 is closed before
   anything else; a session is ended for a stall only when its client has taken nothing for
   STALL_MS while output waited, a new connection came with every session taken, and no other
   session was stalled longer; while a shell waits to be collected poll() never waits longer
   than TICK_MS; and at the end every descriptor, pty, session and shell is gone. Run by
   `make unittest`. */
#include "server.c"
#include <setjmp.h>

#define RUNS      50          /* runs of each kind (which failures are injected) with each set of seeds */
#define POLLS     2000        /* the server's polls in a run before its clients finish */
#define SEED_SETS 3           /* sets of seeds */

#define NFD     16
#define TXCAP   8192
#define RXCAP   8192
#define PTYOUT  8192
#define TTYQ    255
#define MAXC    400

enum { F_FREE, F_LISTEN, F_CONN, F_MASTER, F_KMSG, F_NUL };
static struct { int type, obj; } fdt[NFD];

struct buf { unsigned char *p; int len, cap; };
static void bput(struct buf *b, const unsigned char *p, int n) {
    if(n <= 0) return;
    if(b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->p = realloc(b->p, (size_t)b->cap);
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

struct sconn {
    int used, pending, fd, srv_closed, left, fin, rst, stalled, bound;   /* stalled: 1 for a while, 2 for good */
    unsigned char rx[RXCAP];
    int rx_len, tx_len;
    struct buf got, expq;
    unsigned char kpend[KEYS_SIZE];     /* the keys the server should hold for the master, by its rules */
    int kpend_len;
    long written;
    int pty;
    struct telnet shadow;
    int last_read;                      /* 1: conn read, 2: master read */
    int ended_by_server;
    int ptyctl_expect;                  /* a resize is due */
    int corrupt;
    int opening_pending, opening_len;
    unsigned char opening[TN_OPENING];
    int waiting;                        /* the server holds output for this client */
    int answers;                        /* the answers to the last read: whole, or dropped whole */
    unsigned int wait_since, last_progress;   /* when that began; when the client last took bytes */
};
struct spty {
    int used, master_fd, shell, shell_pid, ended, conn;   /* shell: 0 none, 1 alive, 2 dying, 3 zombie, 4 reaped */
    unsigned char out[PTYOUT];
    int out_len, tty_q, reads_keys, eof_read, linemode, pollout;   /* pollout: poll() said POLLOUT this round */
    struct buf keys;
};

static struct sconn C[MAXC];
static int nconn;
static struct spty P[PTY_MAX];
static int next_pid = 100, last_accepted = -1, closing_fd = -1;
static jmp_buf done;
static long iter, max_iter, quiet, looks, accept_iter = -1;
static int accept_full, evicted;                /* the last accept: every session was taken; one was ended for it */
static int quiesce, server_exited;
static unsigned int sim_now, time_step;        /* time_step: the most a poll() takes when something is there at once */
static unsigned long long rs = 1;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 11); }

/* tallies over all runs */
static long bugs, reply_drop_total, sessions_made, refusals, stall_ends, timeouts_taken, short_writes,
            keys_dropped, keys_eagain, keys_taken, cut_answers, rests;
static int inject_enomem, inject_spawnfail, inject_ptyfull;
static char lastlog[256];

static long run_bugs;
#define BUG(...) do { bugs++; run_bugs++; if(bugs < 40) { printf("BUG (iter %ld, t %u): ", iter, sim_now); printf(__VA_ARGS__); printf("\n"); } } while(0)

unsigned int sim_uptime(void) { return sim_now; }

static int fd_alloc(int type, int obj) {
    int i;
    for(i = 0; i < NFD; i++) if(fdt[i].type == F_FREE) { fdt[i].type = type; fdt[i].obj = obj; return i; }
    return -1;
}

static struct session *srv_session(int fd) {
    int i;
    for(i = 0; i < MAX_SESSIONS; i++) if(sessions[i].conn == fd && fd >= 0) return &sessions[i];
    return NULL;
}

static unsigned int later(unsigned int a, unsigned int b) { return (int)(a - b) > 0 ? a : b; }

/* the server's session buffers against what the shadow says should be there; a master that
   read 0 must be closed before the server does anything else */
static void checkpoint(void) {
    int i;
    for(i = 0; i < PTY_MAX; i++)
        if(P[i].used && P[i].eof_read && P[i].master_fd >= 0 && P[i].master_fd != closing_fd) {
            BUG("pty %d: its master read 0 and is still open", i);
            P[i].eof_read = 0;
        }
    for(i = 0; i < nconn; i++) {
        struct sconn *c = &C[i];
        struct session *s;
        long have, want;
        if(!c->bound || c->ended_by_server || c->srv_closed) continue;
        s = srv_session(c->fd);
        if(c->opening_pending) {                        /* queued by new_session after spawnpty and PTY_INDEX */
            if(!s || !s->out_len) continue;
            bput(&c->expq, c->opening, c->opening_len);
            c->opening_pending = 0;
        }
        if(!s) { BUG("conn %d (fd %d) bound but no server session", i, c->fd); c->ended_by_server = 1; continue; }
        if(s->out_len < 0 || s->out_len > OUT_SIZE) BUG("out_len %d", s->out_len);
        if(s->out_len && !c->waiting) c->wait_since = sim_now;
        c->waiting = s->out_len > 0;
        have = c->written + s->out_len;
        want = c->expq.len;
        if(have < want) {
            long d = want - have;
            if(c->last_read == 2) BUG("conn %d: %ld bytes of session output dropped", i, d);
            else if(d != c->answers) BUG("conn %d: answers cut: %ld of %d bytes dropped", i, d, c->answers);
            else reply_drop_total += d;
            c->expq.len -= (int)d;
        } else if(have > want) {
            BUG("conn %d: more queued than expected (%ld > %ld)", i, have, want);
        }
        c->answers = 0;
        if(s->out_len && memcmp(s->out, c->expq.p + c->written, (size_t)s->out_len)) {
            if(!c->corrupt) BUG("conn %d: out buffer content differs from the expected stream", i);
            c->corrupt = 1;
        }
    }
}

/* ---------------- the world ---------------- */
static void gen_client_bytes(struct sconn *c) {
    unsigned char b[1400];
    int n = 0, want = 1 + (int)(rnd() % (rnd() % 4 ? 40 : 1400)), mode = (int)(rnd() % 12);
    if(mode == 11) want = 1400;                       /* a flood of keys (a paste, or a stuck key) */
    while(n < want - 12) {
        int k = (int)(rnd() % 16);
        if(mode == 0) k = 10;                         /* AYT flood */
        if(mode == 1) k = 11;                         /* DO/WILL flood */
        if(mode == 11) k = (int)(rnd() % 4);          /* letters only */
        switch(k) {
        case 0: case 1: case 2: case 3: b[n++] = (unsigned char)('a' + rnd() % 26); break;
        case 4: b[n++] = '\r'; b[n++] = rnd() % 2 ? '\n' : 0; break;
        case 5: b[n++] = '\r'; break;
        case 6: b[n++] = 255; b[n++] = 255; break;
        case 7: b[n++] = (unsigned char)(rnd() & 255); break;
        case 8: b[n++] = 255; b[n++] = (unsigned char)(240 + rnd() % 16); break;
        case 9: {                                     /* NAWS, sometimes odd */
            int cols = (int)(rnd() % 3 ? rnd() % 300 : rnd() & 0xffff), rows = (int)(rnd() % 3 ? rnd() % 100 : rnd() & 0xffff), j;
            unsigned char sz[4] = { (unsigned char)(cols >> 8), (unsigned char)cols, (unsigned char)(rows >> 8), (unsigned char)rows };
            b[n++] = 255; b[n++] = 250; b[n++] = 31;
            for(j = 0; j < 4 - (int)(rnd() % 8 == 0); j++) { b[n++] = sz[j]; if(sz[j] == 255) b[n++] = 255; }
            if(rnd() % 10) { b[n++] = 255; b[n++] = 240; }
            break;
        }
        case 10: b[n++] = 255; b[n++] = 246; break;
        case 11: b[n++] = 255; b[n++] = (unsigned char)(251 + rnd() % 4); b[n++] = (unsigned char)(rnd() % 3 ? rnd() % 40 : rnd() & 255); break;
        case 12: b[n++] = 255; b[n++] = 244; break;
        case 13: b[n++] = 0xc3; b[n++] = 0xa9; break;
        case 14: b[n++] = 0x7f; break;
        default: b[n++] = 255; b[n++] = 250; b[n++] = (unsigned char)(rnd() & 255); break;   /* an SB left open */
        }
    }
    if(n > RXCAP - c->rx_len) n = RXCAP - c->rx_len;
    memcpy(c->rx + c->rx_len, b, (size_t)n);
    c->rx_len += n;
}

static void pty_maybe_free(struct spty *p) {
    if(p->used && p->master_fd < 0 && p->shell != 1 && p->shell != 2) p->used = 0;
}

/* may the client read? not while it stalls, unless its connection is over */
static int reads(const struct sconn *c) {
    return !c->stalled || (c->stalled == 2 && c->srv_closed);
}

/* one random event; 0 when nothing could happen */
static int world_step(void) {
    int tries;
    for(tries = 0; tries < 50; tries++) {
        int e = (int)(rnd() % 12);
        int i = nconn ? (int)(rnd() % (unsigned)nconn) : 0, j = (int)(rnd() % PTY_MAX);
        struct sconn *c = &C[i];
        struct spty *p = &P[j];
        switch(e) {
        case 0:                                         /* a client connects */
            if(quiesce || nconn >= MAXC) break;
            { int pend = 0, k; for(k = 0; k < nconn; k++) pend += C[k].pending; if(pend >= 4) break; }
            memset(&C[nconn], 0, sizeof(C[nconn]));
            C[nconn].used = 1; C[nconn].pending = 1; C[nconn].fd = -1; C[nconn].pty = -1;
            C[nconn].stalled = rnd() % 16 == 0 ? 2 : rnd() % 8 == 0;
            nconn++;
            return 1;
        case 1: case 2:                                 /* a client sends */
            if(quiesce || !nconn || !c->used || c->left || c->rx_len > RXCAP - 1500) break;
            gen_client_bytes(c);
            return 1;
        case 3: case 4: {                               /* a client reads */
            int k;
            if(!nconn || !c->used || !reads(c) || c->rst || !c->tx_len) break;
            k = 1 + (int)(rnd() % (unsigned)c->tx_len);
            c->tx_len -= k;                             /* the bytes were appended to got at write time */
            return 1;
        }
        case 5:                                         /* stall for a while, or go on */
            if(!nconn || !c->used || quiesce || c->stalled == 2) break;
            c->stalled = !c->stalled;
            return 1;
        case 6:                                         /* the client goes */
            if(!nconn || !c->used || c->left || (quiesce ? 0 : rnd() % 4)) break;
            if(quiesce && c->tx_len && c->stalled != 2) break;  /* in the end, clients read everything first; one */
            c->left = 1;                                         /* stalled for good just goes, with a reset */
            if((rnd() % 3 == 0 && !quiesce) || (quiesce && c->stalled == 2)) c->rst = 1; else c->fin = 1;
            return 1;
        case 7: case 8: {                               /* a shell prints */
            unsigned char b[2048];
            int n, k;
            if(!p->used || p->shell != 1 || p->out_len >= PTYOUT) break;
            n = 1 + (int)(rnd() % (rnd() % 3 ? 200 : 2048));
            if(n > PTYOUT - p->out_len) n = PTYOUT - p->out_len;
            for(k = 0; k < n; k++) {
                unsigned r = rnd() % 20;
                b[k] = r == 0 ? '\n' : r == 1 ? '\r' : r == 2 ? 255 : r == 3 ? 0xe2 : (unsigned char)('A' + rnd() % 26);
            }
            memcpy(p->out + p->out_len, b, (size_t)n);
            p->out_len += n;
            return 1;
        }
        case 9:                                         /* a shell exits by itself */
            if(!p->used || p->shell != 1 || rnd() % 6) break;
            p->shell = 3;
            p->ended = 1;
            return 1;
        case 10:                                        /* a killed shell dies */
            if(!p->used || p->shell != 2) break;
            p->shell = 3;
            p->ended = 1;
            pty_maybe_free(p);
            return 1;
        case 11:                                        /* a shell reads its keys, or not */
            if(!p->used || p->shell != 1) break;
            p->reads_keys = rnd() % 4 != 0;
            if(p->reads_keys) p->tty_q = (int)(rnd() % (unsigned)(p->tty_q + 1));     /* takes some of its keys */
            if(rnd() % 8 == 0) p->linemode = !p->linemode;
            return 1;
        }
    }
    return 0;
}

static short readiness(int fd) {
    struct sconn *c;
    struct spty *p;
    short r = 0;
    if(fd < 0) return 0;
    if(fd >= NFD || fdt[fd].type == F_FREE) return POLLNVAL;
    switch(fdt[fd].type) {
    case F_LISTEN: { int k; for(k = 0; k < nconn; k++) if(C[k].pending) return POLLIN; return 0; }
    case F_CONN:
        c = &C[fdt[fd].obj];
        if(c->rx_len || c->fin || c->rst) r |= POLLIN;
        if(!c->rst && c->tx_len < TXCAP) r |= POLLOUT;
        if(c->fin || c->rst) r |= POLLHUP;
        if(c->rst) r |= POLLERR;
        return r;
    case F_MASTER:
        p = &P[fdt[fd].obj];
        r = p->tty_q < TTYQ ? POLLOUT : 0;
        if(p->out_len || p->ended) r |= POLLIN;
        if(p->ended && !p->out_len) r |= POLLHUP;
        return r;
    default: return POLLIN | POLLOUT;
    }
}

/* how long the most stalled client has taken nothing while output waited for it (0: none waits) */
static unsigned int longest_stall(void) {
    unsigned int most = 0;
    int k;
    for(k = 0; k < nconn; k++) {
        struct sconn *c = &C[k];
        struct session *s = c->bound && !c->srv_closed ? srv_session(c->fd) : NULL;
        if(s && s->out_len && sim_now - later(c->wait_since, c->last_progress) > most)
            most = sim_now - later(c->wait_since, c->last_progress);
    }
    return most;
}

static int no_events_left(void) {
    int k;
    for(k = 0; k < nconn; k++) {
        if(C[k].used && !C[k].left) return 0;
        if(C[k].used && C[k].tx_len && !C[k].rst) return 0;
    }
    for(k = 0; k < PTY_MAX; k++) if(P[k].used && (P[k].shell == 1 || P[k].shell == 2)) return 0;
    return 1;
}

static int zombies(void) {
    int k, z = 0;
    for(k = 0; k < PTY_MAX; k++) if(P[k].shell == 3) z++;
    return z;
}

/* the checks that hold whenever the server is about to wait */
static void before_wait(struct pollfd *fds, nfds_t n, int timeout) {
    unsigned i;
    int k;
    if(n > 1 + 2 * MAX_SESSIONS) BUG("poll set of %u", n);
    for(i = 0; i < n; i++) {                            /* open descriptors; POLLOUT exactly while output waits */
        if(fds[i].fd < 0 || fds[i].fd >= NFD || fdt[fds[i].fd].type == F_FREE) BUG("poll on closed fd %d", fds[i].fd);
        else if(fdt[fds[i].fd].type == F_CONN) {
            struct session *s = srv_session(fds[i].fd);
            if(s && !!(fds[i].events & POLLOUT) != !!s->out_len) BUG("POLLOUT asked %d, out_len %d", fds[i].events, s->out_len);
        } else if(fdt[fds[i].fd].type == F_MASTER) {
            int j;
            for(j = 0; j < MAX_SESSIONS; j++)
                if(sessions[j].conn >= 0 && sessions[j].master == fds[i].fd && !!(fds[i].events & POLLOUT) != !!sessions[j].keys_len)
                    BUG("master: POLLOUT asked %d, keys_len %d", fds[i].events, sessions[j].keys_len);
        }
    }
    for(k = 0; k < MAX_SESSIONS; k++) {                 /* keys that wait: their master is asked about POLLOUT */
        int found = 0;
        if(sessions[k].conn < 0 || sessions[k].master < 0 || !sessions[k].keys_len) continue;
        for(i = 0; i < n; i++) if(fds[i].fd == sessions[k].master && (fds[i].events & POLLOUT)) found = 1;
        if(!found) BUG("session %d: %d keys wait and its master is not asked about POLLOUT", k, sessions[k].keys_len);
    }
    if(zombies() && (timeout < 0 || timeout > TICK_MS)) BUG("a shell waits to be collected and poll() waits %d ms", timeout);
}

int sim_poll(struct pollfd *fds, nfds_t n, int timeout) {
    unsigned i;
    int tries, k;
    checkpoint();
    if(timeout == 0) {                                  /* a look, as refuse() takes: no time passes */
        int ready = 0;
        if(++looks > 1000) { BUG("the server spins: poll() without a wait, again and again"); longjmp(done, 5); }
        for(i = 0; i < n; i++) {
            fds[i].revents = readiness(fds[i].fd) & (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
            if(fds[i].revents) ready++;
        }
        return ready;
    }
    iter++;
    looks = 0;
    for(k = 0; k < PTY_MAX; k++) P[k].pollout = 0;     /* a POLLOUT stands for the round it was said in: a write that
                                                           finds the queue full after a Ctrl+C took the room is told
                                                           EAGAIN, which costs nothing */
    before_wait(fds, n, timeout);
    if(run_bugs > 20) longjmp(done, 5);                 /* this run has said enough */
    if((iter > max_iter || nconn >= MAXC) && !quiesce) quiesce = 1;     /* the end: no more clients come */
    if(quiesce) for(k = 0; k < nconn; k++) if(C[k].stalled == 1) C[k].stalled = 0;
    sim_now += rnd() % time_step;                       /* time passes */
    for(k = (int)(rnd() % 4); k > 0; k--) world_step();
    for(tries = 0;; tries++) {
        int ready = 0, stepped;
        for(i = 0; i < n; i++) {
            fds[i].revents = readiness(fds[i].fd) & (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
            if(fds[i].revents) ready++;
        }
        if(ready) {                                     /* quiet: waits in a row that ran out */
            quiet = 0;
            for(i = 0; i < n; i++)
                if((fds[i].revents & POLLOUT) && fdt[fds[i].fd].type == F_MASTER) P[fdt[fds[i].fd].obj].pollout = 1;
            return ready;
        }
        if(timeout > 0 && tries > 0 && rnd() % 16 == 0) {     /* the wait runs out */
            sim_now += (unsigned)timeout;
            timeouts_taken++;
            return 0;
        }
        stepped = world_step();
        if(!stepped && quiesce && no_events_left()) {
            if(timeout < 0) longjmp(done, 1);
            sim_now += (unsigned)timeout;               /* the server has something to look at later */
            timeouts_taken++;
            if(++quiet > 20000) { BUG("the server keeps waking with nothing to do"); longjmp(done, 4); }
            return 0;
        }
        if(!stepped && timeout > 0 && tries > 200) {    /* nothing can happen now: the wait runs out */
            sim_now += (unsigned)timeout;
            timeouts_taken++;
            if(++quiet > 20000) { BUG("the server keeps waking with nothing happening"); longjmp(done, 4); }
            return 0;
        }
        if(tries > 100000) longjmp(done, 3);
    }
}

ssize_t sim_read(int fd, void *buf, size_t n) {
    checkpoint();
    if(fd < 0 || fd >= NFD || fdt[fd].type == F_FREE) { BUG("read on closed fd %d", fd); errno = EBADF; return -1; }
    if(fdt[fd].type == F_CONN) {
        struct sconn *c = &C[fdt[fd].obj];
        struct session *s = srv_session(fd);
        int k;
        if(c->rx_len) {
            k = c->rx_len < (int)n ? c->rx_len : (int)n;
            memcpy(buf, c->rx, (size_t)k);
            memmove(c->rx, c->rx + k, (size_t)(c->rx_len - k));
            c->rx_len -= k;
            if(c->bound && s) {                         /* the shadow: answers and keys */
                static unsigned char d[2048], r[65536];
                struct tn_buf D = { d, 0, sizeof(d) }, R = { r, 0, OUT_KEEP };
                telnet_recv(&c->shadow, buf, k, &D, &R);
                bput(&c->expq, r, R.len);
                c->answers = R.len;
                if(s->master >= 0 && D.len) {           /* the server's key buffer, by its rules */
                    int j, room, m = D.len;
                    const unsigned char *kp = d;
                    for(j = m - 1; j >= 0 && kp[j] != 0x03; j--);
                    if(j >= 0) { c->kpend_len = 0; kp += j; m -= j; }
                    room = KEYS_SIZE - c->kpend_len;
                    if(m > room) { keys_dropped += m - room; m = room; }
                    memcpy(c->kpend + c->kpend_len, kp, (size_t)m);
                    c->kpend_len += m;
                }
                if(c->shadow.resized && s->master >= 0) { c->ptyctl_expect = 1; c->shadow.resized = 0; }
                c->last_read = 1;
            }
            return k;
        }
        if(c->rst) { errno = ECONNRESET; return -1; }
        if(c->fin) return 0;
        BUG("read on conn fd %d would block", fd);
        errno = EAGAIN;
        return -1;
    }
    if(fdt[fd].type == F_MASTER) {
        struct spty *p = &P[fdt[fd].obj];
        struct sconn *c = p->conn >= 0 ? &C[p->conn] : NULL;
        int k;
        if(p->out_len) {
            k = p->out_len < (int)n ? p->out_len : (int)n;
            if(k > 1024) k = 1024;
            memcpy(buf, p->out, (size_t)k);
            memmove(p->out, p->out + k, (size_t)(p->out_len - k));
            p->out_len -= k;
            if(c) {
                static unsigned char o[4096];
                int m = telnet_send(&c->shadow, buf, k, o);
                bput(&c->expq, o, m);
                c->last_read = 2;
            }
            return k;
        }
        if(p->ended) { p->eof_read = 1; return 0; }
        BUG("read on master fd %d would block", fd);
        errno = EAGAIN;
        return -1;
    }
    BUG("read on fd %d of type %d", fd, fdt[fd].type);
    errno = EINVAL;
    return -1;
}

ssize_t sim_write(int fd, const void *buf, size_t n) {
    checkpoint();
    if(fd < 0 || fd >= NFD || fdt[fd].type == F_FREE) { BUG("write on closed fd %d", fd); errno = EBADF; return -1; }
    if(fdt[fd].type == F_KMSG) {
        size_t k = n < sizeof(lastlog) - 1 ? n : sizeof(lastlog) - 1;
        memcpy(lastlog, buf, k);
        lastlog[k] = 0;
        return (ssize_t)n;
    }
    if(fdt[fd].type == F_CONN) {
        struct sconn *c = &C[fdt[fd].obj];
        int room = TXCAP - c->tx_len, k;
        if(c->rst) { errno = ECONNRESET; return -1; }
        if(!n) return 0;
        if(!room) { BUG("write on conn fd %d would block (no room)", fd); errno = EAGAIN; return -1; }
        k = (int)n < room ? (int)n : room;
        if(k < (int)n) short_writes++;
        if(c->bound) {
            if(c->written + k > c->expq.len || memcmp(buf, c->expq.p + c->written, (size_t)k)) {
                if(!c->corrupt) BUG("conn %ld: bytes written differ from the expected stream at %ld", (long)(c - C), c->written);
                c->corrupt = 1;
            }
            c->last_progress = sim_now;
        }
        bput(&c->got, buf, k);
        c->tx_len += k;
        c->written += k;
        return k;
    }
    if(fdt[fd].type == F_MASTER) {                     /* never waits: the keys that fit, or EAGAIN */
        struct spty *p = &P[fdt[fd].obj];
        struct sconn *c = p->conn >= 0 ? &C[p->conn] : NULL;
        const unsigned char *b = buf;
        int taken = 0;
        if(!n) return 0;
        if(!p->pollout && b[0] != 0x03) BUG("pty %ld: keys written without POLLOUT", (long)(p - P));
        if(c && ((int)n != c->kpend_len || memcmp(buf, c->kpend, n))) {
            BUG("pty %ld: the keys written (%d) are not the ones the server should hold (%d)", (long)(p - P), (int)n, c->kpend_len);
            c = NULL;                                   /* no comparison from here on */
            C[p->conn].kpend_len = 0;
        }
        while(taken < (int)n) {
            if(p->linemode && b[taken] == 0x03) p->tty_q = 0;      /* needs no room; drops the type-ahead */
            else if(p->tty_q < TTYQ) p->tty_q++;
            else break;
            taken++;
        }
        if(!taken) { keys_eagain++; errno = EAGAIN; return -1; }
        bput(&p->keys, buf, taken);
        keys_taken += taken;
        if(c) {
            memmove(c->kpend, c->kpend + taken, (size_t)(c->kpend_len - taken));
            c->kpend_len -= taken;
        }
        return taken;
    }
    if(fdt[fd].type == F_NUL) return (ssize_t)n;
    BUG("write on fd %d of type %d", fd, fdt[fd].type);
    errno = EINVAL;
    return -1;
}

int sim_close(int fd) {
    closing_fd = fd;
    checkpoint();
    closing_fd = -1;
    if(fd < 0 || fd >= NFD || fdt[fd].type == F_FREE) { BUG("close of closed fd %d", fd); errno = EBADF; return -1; }
    if(fdt[fd].type == F_CONN) {
        struct sconn *c = &C[fdt[fd].obj];
        struct session *s = srv_session(fd);
        if(s && c->bound) {
            if(s->out_len && !c->left && !c->rst) {     /* output dropped although the client is there: a stall */
                unsigned int stall = sim_now - later(c->wait_since, c->last_progress);
                if(stall < STALL_MS || accept_iter != iter || !accept_full || evicted || stall < longest_stall())
                    BUG("conn %ld: %d bytes of output dropped after %u ms without progress (accepted now %d, full %d)",
                        (long)(c - C), s->out_len, stall, accept_iter == iter, accept_full);
                else {
                    stall_ends++;
                    evicted = 1;
                }
            }
            c->ended_by_server = 1;
        } else if(!c->bound && fdt[fd].obj == last_accepted && accept_iter == iter && accept_full && !evicted &&
                  longest_stall() >= STALL_MS) {
            BUG("conn %d refused while a session had taken nothing for %u ms", fdt[fd].obj, longest_stall());
        }
        c->srv_closed = 1;
        c->fd = -1;
    } else if(fdt[fd].type == F_MASTER) {
        struct spty *p = &P[fdt[fd].obj];
        p->master_fd = -1;
        p->eof_read = 0;
        if(p->shell == 1) p->shell = 2;                 /* the hangup: killed, dies later */
        pty_maybe_free(p);
    }
    fdt[fd].type = F_FREE;
    return 0;
}

int sim_open(const char *path, int flags) {
    (void)flags;
    checkpoint();
    if(!strcmp(path, "/dev/kmsg")) return fd_alloc(F_KMSG, 0);
    errno = ENOENT;
    return -1;
}

void sim_exit(int code) {
    (void)code;
    server_exited = 1;
    longjmp(done, 9);
}

int sim_waitpid(int pid, int *status, int options) {
    int k, live = 0;
    checkpoint();
    if(pid != -1 || !(options & WNOHANG)) BUG("waitpid(%d, %d)", pid, options);
    for(k = 0; k < PTY_MAX; k++) {
        if(P[k].shell == 3) { P[k].shell = 4; if(status) *status = 0; pty_maybe_free(&P[k]); return P[k].shell_pid; }
        if(P[k].shell == 1 || P[k].shell == 2) live++;
    }
    if(live) return 0;
    errno = ECHILD;
    return -1;
}

int sim_detach(void) { return 0; }

int sim_tcp_accept(int fd, struct netaddr *peer) {
    int k, nfd;
    checkpoint();
    if(fd < 0 || fd >= NFD || fdt[fd].type != F_LISTEN) { BUG("accept on fd %d", fd); errno = EINVAL; return -1; }
    for(k = 0; k < nconn; k++) if(C[k].pending) break;
    if(k == nconn) { BUG("accept would block"); errno = EAGAIN; return -1; }
    if(inject_enomem && rnd() % 50 == 0) {              /* the connection is lost with it */
        C[k].pending = 0;
        C[k].srv_closed = 1;
        rests++;
        errno = rnd() % 2 ? ENOMEM : EMFILE;
        return -1;
    }
    nfd = fd_alloc(F_CONN, k);
    if(nfd < 0) { BUG("accept: no descriptor left"); errno = EMFILE; return -1; }
    {                                                   /* every session taken? (then a stalled one may make room) */
        int j, busy = 0;
        for(j = 0; j < MAX_SESSIONS; j++) if(sessions[j].conn >= 0) busy++;
        accept_full = busy >= MAX_SESSIONS;
        accept_iter = iter;
        evicted = 0;
    }
    C[k].pending = 0;
    C[k].fd = nfd;
    if(peer) { peer->ip = 0x0202000a; peer->port = (unsigned short)(40000 + k); peer->zero = 0; }
    last_accepted = k;
    return nfd;
}

int sim_openpty(void) {
    int k, nfd;
    checkpoint();
    for(k = 0; k < PTY_MAX; k++) if(!P[k].used) break;
    if(k == PTY_MAX || (inject_ptyfull && rnd() % 5 == 0)) { errno = EAGAIN; return -1; }
    nfd = fd_alloc(F_MASTER, k);
    if(nfd < 0) { BUG("openpty: no descriptor left"); errno = EMFILE; return -1; }
    memset(&P[k], 0, sizeof(P[k]));
    P[k].used = 1;
    P[k].master_fd = nfd;
    P[k].conn = -1;
    P[k].reads_keys = 1;
    return nfd;
}

int sim_spawnpty(const char *path, char *const argv[], int master) {
    struct spty *p;
    struct sconn *c;
    unsigned char op[TN_OPENING];
    int n, k, active = 0;
    (void)path; (void)argv;
    checkpoint();
    if(master < 0 || master >= NFD || fdt[master].type != F_MASTER) { BUG("spawnpty on fd %d", master); errno = EINVAL; return -1; }
    if(inject_spawnfail && rnd() % 10 == 0) { errno = ENOMEM; return -1; }
    p = &P[fdt[master].obj];
    c = &C[last_accepted];
    for(k = 0; k < MAX_SESSIONS; k++) if(sessions[k].conn >= 0) active++;
    if(active >= MAX_SESSIONS) BUG("a fifth session is being started");
    p->shell = 1;
    p->shell_pid = next_pid++;
    p->conn = last_accepted;
    c->pty = (int)(p - P);
    c->bound = 1;
    telnet_init(&c->shadow);
    n = telnet_opening(&c->shadow, op);
    memcpy(c->opening, op, (size_t)n);
    c->opening_len = n;
    c->opening_pending = 1;
    c->last_read = 0;
    c->last_progress = c->wait_since = sim_now;
    sessions_made++;
    return p->shell_pid;
}

int sim_ptyctl(int master, int op, unsigned int arg) {
    struct spty *p;
    checkpoint();
    if(master < 0 || master >= NFD || fdt[master].type != F_MASTER) { BUG("ptyctl on fd %d", master); errno = EBADF; return -1; }
    p = &P[fdt[master].obj];
    if(op == PTY_INDEX) return (int)(p - P);
    if(op == PTY_SETSIZE) {
        unsigned cols = arg >> 16, rows = arg & 0xffff;
        if(cols < 8 || cols > 1024 || rows < 2 || rows > 1024) { BUG("PTY_SETSIZE %ux%u out of range", cols, rows); errno = EINVAL; return -1; }
        if(p->conn >= 0) {
            struct sconn *c = &C[p->conn];
            int wc = c->shadow.cols < 8 ? 8 : c->shadow.cols > 1024 ? 1024 : c->shadow.cols;
            int wr = c->shadow.rows < 2 ? 2 : c->shadow.rows > 1024 ? 1024 : c->shadow.rows;
            if(c->ptyctl_expect && ((int)cols != wc || (int)rows != wr)) BUG("PTY_SETSIZE %ux%u, the client said %dx%d", cols, rows, c->shadow.cols, c->shadow.rows);
            c->ptyctl_expect = 0;
        }
        return 0;
    }
    errno = EINVAL;
    return -1;
}

char *sim_ipstr(unsigned int ip, char buf[16]) {
    snprintf(buf, 16, "%u.%u.%u.%u", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);
    return buf;
}

/* the client's bytes as a telnet client parses them: every IAC is IAC IAC or a whole command */
static int cut_sequences(const struct buf *b) {
    int q, st = 0, bad = 0;
    for(q = 0; q < b->len; q++) {
        unsigned char x = b->p[q];
        if(st == 0) { if(x == 255) st = 1; }
        else if(st == 1) {
            if(x == 255) st = 0;
            else if(x >= 251) st = 2;                   /* the option follows */
            else if(x >= 240) st = 0;
            else { bad++; st = 0; }                     /* IAC then a data byte: a cut sequence */
        } else st = 0;
    }
    return bad;
}

static void run(unsigned long long seed, long iters) {
    int k, r;
    rs = seed * 2654435761ULL + 1;
    memset(fdt, 0, sizeof(fdt));
    for(k = 0; k < nconn; k++) { free(C[k].got.p); free(C[k].expq.p); }
    memset(C, 0, sizeof(C));
    for(k = 0; k < PTY_MAX; k++) free(P[k].keys.p);
    memset(P, 0, sizeof(P));
    memset(sessions, 0, sizeof(sessions));                  /* the server's statics, as a new process has them */
    kmsg = -1;
    sessions_ever = shells = resting = 0;
    rest_end = 0;
    nconn = 0; iter = 0; quiet = 0; looks = 0; run_bugs = 0; accept_iter = -1; accept_full = evicted = 0; max_iter = iters; quiesce = 0; server_exited = 0; last_accepted = -1;
    sim_now = (unsigned int)(rnd() % 100000);
    time_step = seed % 3 ? 20 : 400;                    /* some runs live long enough for clients to stall for good */
    fdt[0].type = F_LISTEN; fdt[1].type = F_NUL; fdt[2].type = F_NUL;
    r = setjmp(done);
    if(!r) { serve(0, 23); BUG("serve returned"); return; }
    if(server_exited) { BUG("seed %llu: the server exited: %s", seed, lastlog); return; }
    if(r == 3 || r == 4) BUG("seed %llu: the run did not settle (%d)", seed, r);
    if(r == 5) return;
    /* the end: every client left and read what it could */
    for(k = 0; k < MAX_SESSIONS; k++) if(sessions[k].conn >= 0 || sessions[k].master >= 0) BUG("seed %llu: session %d still open (r=%d)", seed, k, r);
    for(k = 4; k < NFD; k++) if(fdt[k].type != F_FREE) BUG("seed %llu: fd %d (type %d) left open", seed, k, fdt[k].type);
    for(k = 0; k < nconn; k++) {
        struct sconn *c = &C[k];
        if(c->pending) BUG("seed %llu: conn %d never accepted", seed, k);
        else if(!c->srv_closed) BUG("seed %llu: conn %d never closed", seed, k);
        if(!c->bound && c->srv_closed && !c->pending) refusals++;
        if(c->bound && !c->corrupt && c->got.len != c->written) BUG("got != written");
        if(c->bound && (r = cut_sequences(&c->got))) { cut_answers += r; BUG("seed %llu: conn %d got %d cut sequences", seed, k, r); }
    }
    for(k = 0; k < PTY_MAX; k++) {
        if(P[k].shell == 3) BUG("seed %llu: pty %d's shell is a zombie at the end", seed, k);
        if(P[k].used) BUG("seed %llu: pty %d still in use (shell %d)", seed, k, P[k].shell);
    }
    if(shells) BUG("seed %llu: %d shells counted at the end", seed, shells);
}

/* test_telnetd [runs [polls [seed sets [first set]]]]: each kind of run (the failures injected:
   none, accept, spawnpty, openpty, all three) RUNS times with each set of seeds */
int main(int argc, char **argv) {
    static const char *const modes[] = { "", "m", "s", "p", "msp" };
    long runs = argc > 1 ? atol(argv[1]) : RUNS, iters = argc > 2 ? atol(argv[2]) : POLLS;
    long sets = argc > 3 ? atol(argv[3]) : SEED_SETS, first = argc > 4 ? atol(argv[4]) : 0, i, k, total = 0;
    unsigned m;
    for(k = first; k < first + sets; k++)
        for(m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
            inject_enomem = !!strchr(modes[m], 'm');
            inject_spawnfail = !!strchr(modes[m], 's');
            inject_ptyfull = !!strchr(modes[m], 'p');
            for(i = 0; i < runs; i++) run((unsigned long long)(k * 1000003 + m * 100000 + i + 1), iters);
            total += runs;
        }
    printf("telnetd server: %ld runs of %ld polls (failures of accept, spawnpty, openpty in some): %ld sessions, %ld refusals, "
           "%ld ended for a stall to make room, %ld rests of the listener, %ld short writes, %ld waits that ran out, %ld answer bytes "
           "dropped whole, %ld keys taken by masters, %ld dropped (the buffer full), %ld key writes that found no room\n",
           total, iters, sessions_made, refusals, stall_ends, rests, short_writes, timeouts_taken, reply_drop_total, keys_taken,
           keys_dropped, keys_eagain);
    if(bugs) {
        printf("TELNETD SERVER UNIT TESTS FAILED: %ld bugs\n", bugs);
        return 1;
    }
    printf("TELNETD SERVER UNIT TESTS PASSED: %ld runs, 0 bugs\n", total);
    return 0;
}
