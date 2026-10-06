/*
SurfOS Network Stack: TCP
--------------------
File: tcp.c     Date: 10/3/26 (roadmap N2)
--------------------
A small but complete TCP: active and passive open, in-order receive with a
fixed window, retransmission from the unacknowledged send buffer on a
one-second timer, FIN handshake both ways, RST handling, TIME_WAIT. There is
no congestion control, no out-of-order queueing, no window scaling and no
urgent data; a lost segment costs a second. Eight sockets, each with 8 KB
of send and receive buffer. Everything runs under net_lock; the API waits
on the socket's event with the lock released.

10/6/26: a wait gives up like a timeout when the caller's call is cut short (a
kill waits for its end, or Ctrl+C: surfos/task.h).

10/6/26 (the telnet server): connections for programs (net/sock.c, surfos/sock.h).
Sixteen sockets; a listening one has no buffers, and the last free one is kept
for the kernel's own connections. The calls programs need answer -errno and may
wait without a time limit (a kill or Ctrl+C still ends the wait): tcp_open() keeps
why a connection failed (refused, timed out, no route, no network), tcp_read()
blocks like read(), tcp_write() waits only while nothing fits and queues what
does, tcp_poll() says what is ready, and tcp_release() closes without waiting: the
socket sends its FIN and finishes on its own (an orphan, reset when it makes no
progress for 30 seconds). Whatever changes what a socket is ready for also wakes
poll() (poll_wake()). A listening socket conflicts only with another listening one
on its port, so a server can listen again while its old connections finish.

10/6/26 (review of the above): nothing waits forever on the other end. A
connection that dies during its handshake leaves its listener's backlog at once,
and a SYN finding the backlog full takes the place of a half-open one (a SYN scan
or flood cannot make a listener deaf). A shut window is probed (the persist
timer, with backoff, never giving up while the other end answers), and an ACK
that only says the window is still shut proves the other end alive. A connection
silent for two minutes is probed (keepalive: every 10 seconds, ETIMEDOUT after 5
unanswered; tcpka=<idle>,<interval> in seconds on the kernel command line for the
tests). An unacceptable segment always gets an ACK (a keepalive or a window probe
from the other end is answered); a FIN behind data that did not fit is not taken;
a window update goes out only when the window opened enough to matter; segments
are cut to the other end's MSS.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/timer.h>
#include <surfos/sync.h>
#include <surfos/task.h>
#include <mm/kalloc.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <fs/vfs.h>
#include <surfos/poll.h>
#include <surfos/multiboot.h>
#include <blibc_common.h>

#define TCP_SOCKETS 16                  /* 10/6/26: 8 before the telnet server */
#define TCP_RESERVED 1                  /* 10/6/26: free sockets only the kernel's own connections may take */
#define TCP_BUF 8192
#define TCP_MSS 1460
#define TCP_MIN_MSS 64                  /* 10/6/26: the smallest MSS of the other end's taken */
#define TCP_RTO (HZ)                    /* ticks */
#define TCP_MAX_RETRIES 8
#define TCP_SYNACK_RETRIES 4            /* 10/6/26: a half-open connection gives up after about 10 s */
#define TCP_TIME_WAIT (2 * HZ)
#define TCP_BACKLOG 4
#define TCP_ORPHAN_LIFE (30 * HZ)       /* 10/6/26: an orphan that made no progress this long is reset */
#define TCP_PERSIST_MAX (60 * HZ)       /* 10/6/26: the longest pause between two probes of a shut window */
#define TCP_KEEPIDLE (120 * HZ)         /* 10/6/26: keepalive: silent this long, the other end is asked */
#define TCP_KEEPINTVL (10 * HZ)         /*          again after this, */
#define TCP_KEEPCNT 5                   /*          and given up after this many unanswered */

enum { CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT };
static const char *const state_names[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                           "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };

#define SEQ_LT(a, b) ((i32)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((i32)((a) - (b)) <= 0)

struct tcp_socket {
    bool used, orphan;                  /* orphan: closed by the application, finishing on its own */
    bool synced;                        /* 10/6/26: it reached ESTABLISHED (accept() hands it out then) */
    u_long orphan_at;                   /* 10/6/26: when an orphan last made progress */
    struct tcp_socket *listener;        /* 10/6/26: whose backlog it waits in, until accept() takes it */
    u16 mss;                            /* 10/6/26: the most data a segment carries: the other end's MSS, at most ours */
    u_long last_rx;                     /* 10/6/26: when the other end last sent anything (keepalive) */
    u_int ka_probes;                    /* 10/6/26: keepalive probes unanswered */
    u_long persist_at;                  /* 10/6/26: the next probe of a shut window, 0: none due */
    u_int persist_n;                    /* 10/6/26: probes since the window shut (the pause doubles) */
    int state;
    ipaddr_t remote_ip;
    u16 local_port, remote_port;
    /* send side */
    u32 iss, snd_una, snd_nxt, snd_wnd;
    u8 *txbuf;                          /* data from snd_una on, not yet acknowledged */
    u_int tx_len;
    bool fin_pending, fin_sent;         /* FIN queued behind the data; FIN counted in snd_nxt */
    /* receive side */
    u32 irs, rcv_nxt;
    u8 *rxbuf;
    u_int rx_len;
    bool peer_fin;
    /* timers */
    u_long rto_at, wait_until;
    u_int retries;
    /* listening */
    struct tcp_socket *backlog[TCP_BACKLOG];
    u_int backlog_n;
    event_t ev;
    int error;
    u_long rx_segs, tx_segs, retrans;
};

static struct tcp_socket socks[TCP_SOCKETS];
static u16 next_port = 40000;
static u_long ka_idle = TCP_KEEPIDLE, ka_intvl = TCP_KEEPINTVL;   /* 10/6/26: tcpka= on the command line */

/**** sockets ****/

static void sock_free(struct tcp_socket *s);

/* 10/6/26: a listening socket moves no data: no buffers. The last TCP_RESERVED free sockets are
   for the kernel's own connections (reserved): programs and the connections that come in cannot
   take them all. An orphan in TIME_WAIT counts as free, the oldest giving its place up when none
   is (a program that connects and closes in a loop does not run out). *err says why there is
   none: -ENFILE (all sockets in use) or -ENOMEM. */
static struct tcp_socket *sock_new(bool buffers, bool reserved, int *err) {
    struct tcp_socket *s = NULL, *tw = NULL;
    int i, n = 0;
    for(i = 0; i < TCP_SOCKETS; i++) {
        struct tcp_socket *x = &socks[i];
        if(!x->used) {
            if(!s) s = x;
            n++;
        } else if(x->orphan && x->state == TIME_WAIT) {   /* 10/6/26: one done but for its 2 s: as good as free */
            n++;
            if(!tw || (long)(x->wait_until - tw->wait_until) < 0) tw = x;
        }
    }
    if((!s && !tw) || (!reserved && n <= TCP_RESERVED)) {
        if(err) *err = -ENFILE;
        return NULL;
    }
    if(!s) {                                             /* the oldest of them gives its place up */
        sock_free(tw);
        s = tw;
    }
    memset(s, 0, sizeof(*s));
    if(buffers) {
        s->txbuf = (u8 *)kalloc(TCP_BUF);
        s->rxbuf = (u8 *)kalloc(TCP_BUF);
        if(!s->txbuf || !s->rxbuf) {
            kfree(s->txbuf);
            kfree(s->rxbuf);
            s->txbuf = s->rxbuf = NULL;
            if(err) *err = -ENOMEM;
            return NULL;
        }
    }
    s->used = true;
    s->state = CLOSED;
    s->mss = TCP_MSS;
    s->last_rx = getticks();
    event_init(&s->ev);
    return s;
}

/* a connection that came in (a SYN to a listener) */
static struct tcp_socket *sock_alloc(void) {
    return sock_new(true, false, NULL);
}

static void sock_free(struct tcp_socket *s) {
    u_int i;
    for(i = 0; i < s->backlog_n; i++) if(s->backlog[i]) sock_free(s->backlog[i]);
    kfree(s->txbuf);
    kfree(s->rxbuf);
    s->used = false;
}

static bool port_in_use(u16 port) {
    int i;
    for(i = 0; i < TCP_SOCKETS; i++) if(socks[i].used && socks[i].local_port == port) return true;
    return false;
}

/* 10/6/26: a listener's port: only another listener takes it (connections keep their own) */
static bool port_listened(u16 port) {
    int i;
    for(i = 0; i < TCP_SOCKETS; i++) if(socks[i].used && socks[i].state == LISTEN && socks[i].local_port == port) return true;
    return false;
}

/* 10/6/26: what waits for s goes on: its own call (the event) and poll() */
static void notify(struct tcp_socket *s) {
    event_set(&s->ev);
    poll_wake();
}

static u16 ephemeral_port(void) {
    do { next_port++; if(next_port < 40000) next_port = 40000; } while(port_in_use(next_port));
    return next_port;
}

static void set_state(struct tcp_socket *s, int state) {
    s->state = state;
    if(state == ESTABLISHED) s->synced = true;
    if(s->orphan) s->orphan_at = getticks();             /* 10/6/26: progress */
    notify(s);
}

/* 10/6/26: c leaves its listener's backlog (accepted, or dead) */
static void backlog_remove(struct tcp_socket *c) {
    struct tcp_socket *l = c->listener;
    u_int i, j;
    c->listener = NULL;
    if(!l) return;
    for(i = 0; i < l->backlog_n; i++) {
        if(l->backlog[i] != c) continue;
        for(j = i + 1; j < l->backlog_n; j++) l->backlog[j - 1] = l->backlog[j];
        l->backlog_n--;
        return;
    }
}

/* The connection is over, err says why. 10/6/26: one that died during its handshake, still in a
   listener's backlog, goes at once (nobody accepts it, and the listener needs the room), and so
   does an orphan; the caller does not touch s after that. */
static void fail(struct tcp_socket *s, int err) {
    s->error = err;
    s->rto_at = 0;
    s->persist_at = 0;
    set_state(s, CLOSED);
    if(s->listener && !s->synced) {
        backlog_remove(s);
        sock_free(s);
    } else if(s->orphan) sock_free(s);
}

/**** segments ****/

static int send_segment(ipaddr_t dst, u16 sport, u16 dport, u32 seq, u32 ack, u8 flags, u16 win, const void *data, u_int len, bool mss) {
    struct netbuf *nb = netbuf_alloc();
    struct tcp_hdr *th;
    u_int hlen = sizeof(*th) + (mss ? 4 : 0);
    if(!nb) return -1;
    if(len) memcpy(netbuf_put(nb, len), data, len);
    th = (struct tcp_hdr *)netbuf_push(nb, hlen);
    th->sport = htons(sport);
    th->dport = htons(dport);
    th->seq = htonl(seq);
    th->ack = htonl(ack);
    th->off = (u8)((hlen / 4) << 4);
    th->flags = flags;
    th->win = htons(win);
    th->csum = 0;
    th->urg = 0;
    if(mss) {
        u8 *o = (u8 *)(th + 1);
        o[0] = 2; o[1] = 4; o[2] = TCP_MSS >> 8; o[3] = TCP_MSS & 0xFF;
    }
    th->csum = ip_checksum_pseudo(ip_route_src(dst), dst, IPPROTO_TCP, th, nb->len);
    return ip_output(NULL, dst, IPPROTO_TCP, nb);
}

static u16 window(struct tcp_socket *s) {
    return (u16)(TCP_BUF - s->rx_len);
}

/* 10/6/26: the MSS option of a SYN or SYN-ACK, at least TCP_MIN_MSS and at most ours; ours without one */
static u16 peer_mss(const struct tcp_hdr *th, u_int hlen) {
    const u8 *o = (const u8 *)(th + 1), *end = (const u8 *)th + hlen;
    while(o < end && *o != 0) {                          /* 0: the end of the options */
        if(*o == 1) { o++; continue; }                   /* no-op */
        if(o + 1 >= end || o[1] < 2 || o + o[1] > end) break;
        if(o[0] == 2 && o[1] == 4) {
            u16 m = (u16)((o[2] << 8) | o[3]);
            return m < TCP_MIN_MSS ? TCP_MIN_MSS : m > TCP_MSS ? TCP_MSS : m;
        }
        o += o[1];
    }
    return TCP_MSS;
}

/* 10/6/26: an ACK the other end has to answer (an old sequence number, RFC 793), and its answer says
   its window: the probe of a shut window and keepalive's */
static void send_probe(struct tcp_socket *s) {
    send_segment(s->remote_ip, s->local_port, s->remote_port, s->snd_una - 1, s->rcv_nxt, TCP_ACK, window(s), NULL, 0, false);
    s->tx_segs++;
}

static void send_ack(struct tcp_socket *s) {
    send_segment(s->remote_ip, s->local_port, s->remote_port, s->snd_nxt, s->rcv_nxt, TCP_ACK, window(s), NULL, 0, false);
    s->tx_segs++;
}

static void arm_rto(struct tcp_socket *s) {
    if(!s->rto_at) { s->rto_at = getticks() + TCP_RTO; s->retries = 0; }
}

/* send whatever the window allows from snd_nxt on, then the FIN if it is due. 10/6/26: with data
   waiting, nothing in flight and the window shut, the persist timer probes it (tcp_tick) */
static void tcp_output(struct tcp_socket *s) {
    u32 in_flight = s->snd_nxt - s->snd_una;
    while(in_flight < s->tx_len) {
        u_int chunk = s->tx_len - in_flight, room = s->snd_wnd > in_flight ? s->snd_wnd - in_flight : 0;
        if(chunk > s->mss) chunk = s->mss;
        if(chunk > room) chunk = room;
        if(!chunk) break;
        if(send_segment(s->remote_ip, s->local_port, s->remote_port, s->snd_nxt, s->rcv_nxt, TCP_ACK | TCP_PSH, window(s),
                        s->txbuf + in_flight, chunk, false) != 0) break;
        s->tx_segs++;
        s->snd_nxt += chunk;
        in_flight += chunk;
        arm_rto(s);
    }
    if(s->fin_pending && !s->fin_sent && in_flight == s->tx_len) {
        send_segment(s->remote_ip, s->local_port, s->remote_port, s->snd_nxt, s->rcv_nxt, TCP_ACK | TCP_FIN, window(s), NULL, 0, false);
        s->tx_segs++;
        s->fin_sent = true;
        s->snd_nxt++;
        arm_rto(s);
    }
    if(s->tx_len > in_flight && !s->snd_wnd && !in_flight) {
        if(!s->persist_at) {
            s->persist_n = 0;
            s->persist_at = getticks() + TCP_RTO;
        }
    } else s->persist_at = 0;
}

/* 10/6/26: the persist timer: the next probe of the shut window, after twice the pause (a minute at most) */
static void probe_window(struct tcp_socket *s) {
    u_long pause = (u_long)TCP_RTO << (s->persist_n < 6 ? s->persist_n + 1 : 6);
    send_probe(s);
    s->persist_n++;
    s->persist_at = getticks() + (pause < TCP_PERSIST_MAX ? pause : TCP_PERSIST_MAX);
}

/* the oldest unacknowledged segment again (or the SYN / FIN) */
static void retransmit(struct tcp_socket *s) {
    s->retrans++;
    if(++s->retries > (s->state == SYN_RCVD ? TCP_SYNACK_RETRIES : TCP_MAX_RETRIES)) { fail(s, -ETIMEDOUT); return; }
    s->rto_at = getticks() + TCP_RTO * (s->retries < 4 ? s->retries : 4);
    switch(s->state) {
    case SYN_SENT:
        send_segment(s->remote_ip, s->local_port, s->remote_port, s->iss, 0, TCP_SYN, window(s), NULL, 0, true);
        break;
    case SYN_RCVD:
        send_segment(s->remote_ip, s->local_port, s->remote_port, s->iss, s->rcv_nxt, TCP_SYN | TCP_ACK, window(s), NULL, 0, true);
        break;
    default:
        if(s->tx_len) {
            u_int chunk = s->tx_len > s->mss ? s->mss : s->tx_len;
            send_segment(s->remote_ip, s->local_port, s->remote_port, s->snd_una, s->rcv_nxt, TCP_ACK | TCP_PSH, window(s), s->txbuf, chunk, false);
        } else if(s->fin_sent) {
            send_segment(s->remote_ip, s->local_port, s->remote_port, s->snd_nxt - 1, s->rcv_nxt, TCP_ACK | TCP_FIN, window(s), NULL, 0, false);
        } else s->rto_at = 0;
        break;
    }
}

/* ack numbers that acknowledge our data: drop it from the send buffer */
static void process_ack(struct tcp_socket *s, u32 ack) {
    u32 acked;
    if(SEQ_LEQ(ack, s->snd_una) || SEQ_LT(s->snd_nxt, ack)) return;
    acked = ack - s->snd_una;
    if(s->fin_sent && ack == s->snd_nxt) {               /* the FIN itself counts for one */
        if(acked > s->tx_len) acked = s->tx_len;
    }
    if(acked > s->tx_len) acked = s->tx_len;
    if(acked) {
        memmove(s->txbuf, s->txbuf + acked, s->tx_len - acked);
        s->tx_len -= acked;
    }
    s->snd_una = ack;
    s->rto_at = 0;
    s->retries = 0;
    if(s->snd_una != s->snd_nxt) arm_rto(s);
    if(s->orphan) s->orphan_at = getticks();             /* 10/6/26: progress */
    notify(s);                                           /* room in the send buffer */
}

static struct tcp_socket *lookup(ipaddr_t rip, u16 rport, u16 lport) {
    struct tcp_socket *listener = NULL;
    int i;
    for(i = 0; i < TCP_SOCKETS; i++) {
        struct tcp_socket *s = &socks[i];
        if(!s->used || s->local_port != lport) continue;
        if(s->state == LISTEN) { listener = s; continue; }
        if(s->remote_ip == rip && s->remote_port == rport) return s;
    }
    return listener;
}

static void send_rst_for(const struct ip_hdr *ip, const struct tcp_hdr *th, u_int datalen) {
    if(th->flags & TCP_RST) return;
    if(th->flags & TCP_ACK) send_segment(ip->src, ntohs(th->dport), ntohs(th->sport), ntohl(th->ack), 0, TCP_RST, 0, NULL, 0, false);
    else send_segment(ip->src, ntohs(th->dport), ntohs(th->sport), 0, ntohl(th->seq) + datalen + ((th->flags & TCP_SYN) ? 1 : 0),
                      TCP_RST | TCP_ACK, 0, NULL, 0, false);
}

void tcp_input(struct netdev *dev, struct netbuf *nb, const struct ip_hdr *ip) {
    struct tcp_hdr *th = (struct tcp_hdr *)netbuf_data(nb);
    struct tcp_socket *s;
    u_int hlen, datalen;
    u32 seq, ack;
    u8 flags, *data;

    if(nb->len < sizeof(*th)) { netbuf_free(nb); return; }
    hlen = (th->off >> 4) * 4;
    if(hlen < sizeof(*th) || hlen > nb->len) { netbuf_free(nb); return; }
    if(ip_checksum_pseudo(ip->src, ip->dst, IPPROTO_TCP, th, nb->len) != 0) { netbuf_free(nb); return; }
    seq = ntohl(th->seq);
    ack = ntohl(th->ack);
    flags = th->flags;
    data = (u8 *)th + hlen;
    datalen = nb->len - hlen;

    s = lookup(ip->src, ntohs(th->sport), ntohs(th->dport));
    if(!s) { send_rst_for(ip, th, datalen); netbuf_free(nb); return; }
    s->rx_segs++;

    if(s->state == LISTEN) {
        struct tcp_socket *c;
        u_int i;
        if(!(flags & TCP_SYN) || (flags & (TCP_ACK | TCP_RST))) { netbuf_free(nb); return; }
        /* 10/6/26: a full backlog makes room by dropping the oldest half-open connection: a real client
           finishes its handshake in a round trip, so only a scan or a flood loses its place */
        for(i = 0; s->backlog_n >= TCP_BACKLOG && i < s->backlog_n; i++) {
            if(s->backlog[i]->synced) continue;
            c = s->backlog[i];
            backlog_remove(c);
            sock_free(c);
        }
        if(s->backlog_n >= TCP_BACKLOG) { netbuf_free(nb); return; }
        c = sock_alloc();
        if(!c) { netbuf_free(nb); return; }
        c->local_port = s->local_port;
        c->remote_ip = ip->src;
        c->remote_port = ntohs(th->sport);
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->iss = (u32)getticks() * 2654435761UL ^ (u32)seq;
        c->snd_una = c->iss;
        c->snd_nxt = c->iss + 1;
        c->snd_wnd = ntohs(th->win);
        c->mss = peer_mss(th, hlen);
        c->state = SYN_RCVD;
        c->listener = s;
        s->backlog[s->backlog_n] = c;
        s->backlog_n++;
        send_segment(c->remote_ip, c->local_port, c->remote_port, c->iss, c->rcv_nxt, TCP_SYN | TCP_ACK, window(c), NULL, 0, true);
        c->tx_segs++;
        arm_rto(c);
        netbuf_free(nb);
        return;
    }

    s->last_rx = getticks();                             /* 10/6/26: the other end is there (keepalive) */
    s->ka_probes = 0;

    if(flags & TCP_RST) {
        if(s->state == SYN_SENT ? ack == s->snd_nxt : (SEQ_LEQ(s->rcv_nxt, seq) && SEQ_LT(seq, s->rcv_nxt + TCP_BUF)))
            fail(s, s->state == SYN_SENT ? -ECONNREFUSED : -ECONNRESET);
        netbuf_free(nb);
        return;
    }

    if(s->state == SYN_SENT) {
        if((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == s->snd_nxt) {
            s->irs = seq;
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_wnd = ntohs(th->win);
            s->mss = peer_mss(th, hlen);
            s->rto_at = 0;
            set_state(s, ESTABLISHED);
            send_ack(s);
        }
        netbuf_free(nb);
        return;
    }

    /* from here on the connection is synchronized: the segment must be in the window */
    if(!(flags & TCP_ACK)) { netbuf_free(nb); return; }
    if(seq != s->rcv_nxt) {
        if(SEQ_LT(seq, s->rcv_nxt) && SEQ_LT(s->rcv_nxt, seq + datalen)) {   /* partly old: trim */
            u32 skip = s->rcv_nxt - seq;
            data += skip;
            datalen -= skip;
            seq = s->rcv_nxt;
        } else {
            send_ack(s);        /* out of order or old: say where we are (10/6/26: also to a bare ACK, a probe) */
            netbuf_free(nb);
            return;
        }
    }

    if(s->state == SYN_RCVD) {
        if(ack != s->snd_nxt) { netbuf_free(nb); return; }
        s->snd_una = ack;
        s->rto_at = 0;
        set_state(s, ESTABLISHED);
        if(s->listener) notify(s->listener);             /* a connection to accept */
    }

    s->snd_wnd = ntohs(th->win);
    if(!s->snd_wnd && ack == s->snd_una && s->snd_una != s->snd_nxt) s->retries = 0;   /* 10/6/26: alive, window shut */
    process_ack(s, ack);

    if(datalen && s->orphan) {
        s->rcv_nxt += datalen;                           /* 10/6/26: nobody reads it any more: taken and dropped */
    } else if(datalen && (s->state == ESTABLISHED || s->state == FIN_WAIT_1 || s->state == FIN_WAIT_2)) {
        u_int room = TCP_BUF - s->rx_len;
        if(datalen > room) {                             /* 10/6/26: the rest comes again, the FIN with it */
            datalen = room;
            flags &= (u8)~TCP_FIN;
        }
        memcpy(s->rxbuf + s->rx_len, data, datalen);
        s->rx_len += datalen;
        s->rcv_nxt += datalen;
        notify(s);
    }
    if((flags & TCP_FIN) && seq + datalen == s->rcv_nxt) {
        s->rcv_nxt++;
        s->peer_fin = true;
        switch(s->state) {
        case ESTABLISHED: set_state(s, CLOSE_WAIT); break;
        case FIN_WAIT_1: set_state(s, s->fin_sent && s->snd_una == s->snd_nxt ? TIME_WAIT : CLOSING); break;
        case FIN_WAIT_2: set_state(s, TIME_WAIT); break;
        default: break;
        }
        if(s->state == TIME_WAIT) s->wait_until = getticks() + TCP_TIME_WAIT;
        datalen = 1;                                     /* so an ACK goes out below */
    }

    /* our FIN acknowledged? */
    if(s->fin_sent && s->snd_una == s->snd_nxt) {
        if(s->state == FIN_WAIT_1) set_state(s, FIN_WAIT_2);
        else if(s->state == CLOSING) { set_state(s, TIME_WAIT); s->wait_until = getticks() + TCP_TIME_WAIT; }
        else if(s->state == LAST_ACK) { set_state(s, CLOSED); if(s->orphan) { sock_free(s); netbuf_free(nb); return; } }
    }
    if(datalen) send_ack(s);
    if(s->used && s->state != CLOSED) tcp_output(s);     /* the window may have opened */
    netbuf_free(nb);
}

/* 10/6/26: a connection given up at once: a reset to the other end, and the socket goes */
static void abort_conn(struct tcp_socket *s) {
    if(s->state != CLOSED && s->state != LISTEN && s->state != SYN_SENT && s->state != TIME_WAIT)
        send_segment(s->remote_ip, s->local_port, s->remote_port, s->snd_nxt, s->rcv_nxt, TCP_RST | TCP_ACK, 0, NULL, 0, false);
    sock_free(s);
}

/* 10/6/26: tcpka=<idle>[,<interval>] on the kernel command line, in seconds: keepalive sooner (the tests) */
static void keepalive_setting(void) {
    const char *p = strstr(bootinfo.cmdline, "tcpka=");
    char *end;
    u_long idle, intvl;
    if(!p) return;
    idle = strtoul(p + 6, &end, 10);
    if(idle) ka_idle = idle * HZ;
    if(*end == ',' && (intvl = strtoul(end + 1, NULL, 10)) != 0) ka_intvl = intvl * HZ;
}

/* 10/6/26: keepalive: a connection the other end has said nothing on for ka_idle gets a probe, and
   another every ka_intvl; TCP_KEEPCNT unanswered, the other end is gone: ETIMEDOUT */
static void keepalive(struct tcp_socket *s, u_long now) {
    if(now - s->last_rx < ka_idle + s->ka_probes * ka_intvl) return;
    if(s->ka_probes >= TCP_KEEPCNT) {
        fail(s, -ETIMEDOUT);
        return;
    }
    send_probe(s);
    s->ka_probes++;
}

void tcp_tick(void) {
    static bool setting_read;
    u_long now = getticks();
    int i;
    if(!setting_read) {
        setting_read = true;
        keepalive_setting();
    }
    for(i = 0; i < TCP_SOCKETS; i++) {
        struct tcp_socket *s = &socks[i];
        if(!s->used) continue;
        if(s->state == TIME_WAIT && (long)(now - s->wait_until) >= 0) {
            set_state(s, CLOSED);
            if(s->orphan) sock_free(s);
            continue;
        }
        if(s->orphan && now - s->orphan_at >= TCP_ORPHAN_LIFE) {   /* 10/6/26: the other end stopped finishing it */
            abort_conn(s);
            continue;
        }
        if(s->rto_at && (long)(now - s->rto_at) >= 0) retransmit(s);
        if(!s->used) continue;                           /* it failed and went */
        if(s->persist_at && (long)(now - s->persist_at) >= 0) probe_window(s);
        if((s->state == ESTABLISHED || s->state == CLOSE_WAIT) && !s->rto_at) keepalive(s, now);
    }
}

/* 10/6/26: the next hop to hop does not answer ARP (net/arp.c): a connection being opened through
   it fails (EHOSTUNREACH) instead of retrying until it times out. Under net_lock. */
void tcp_unreachable(ipaddr_t hop) {
    struct netdev *dev = netdev_default();
    int i;
    for(i = 0; i < TCP_SOCKETS; i++) {
        struct tcp_socket *s = &socks[i];
        if(s->used && s->state == SYN_SENT && dev && ip_next_hop(dev, s->remote_ip) == hop) fail(s, -EHOSTUNREACH);
    }
}

/**** the API ****/

/* wait until cond, with the lock released; returns false on timeout (10/6/26: or when cut short).
   10/6/26: TCP_NO_LIMIT waits until cond or until the wait is cut short, a second at a time */
#define WAIT_FOR(s, cond, timeout_ms) ({                                   \
    bool ok_ = true, forever_ = (timeout_ms) == TCP_NO_LIMIT;              \
    u_long deadline_ = forever_ ? 0 : getticks() + (timeout_ms) / (1000 / HZ); \
    while(!(cond)) {                                                        \
        long left_ = forever_ ? HZ : (long)(deadline_ - getticks());       \
        if(left_ <= 0 || task_interrupted()) { ok_ = false; break; }        \
        event_clear(&(s)->ev);                                              \
        if(cond) break;                                                     \
        mutex_unlock(&net_lock);                                            \
        event_wait_timeout(&(s)->ev, (u_long)left_ * (1000 / HZ));          \
        mutex_lock(&net_lock);                                              \
    }                                                                       \
    ok_; })

/* 10/6/26: why a connection to dst cannot even be tried: no interface, no address on it, no
   link, or dst off the interface's network with no gateway to send it to; 0 when it can */
static int no_route(ipaddr_t dst) {
    struct netdev *dev = netdev_default();
    if(!dev || !dev->ip || !dev->link_up) return -ENETUNREACH;
    if(dev->netmask && (dst & dev->netmask) != (dev->ip & dev->netmask) && !dev->gateway) return -ENETUNREACH;
    return 0;
}

/* 10/6/26: the active open, with the reason it failed: -ENETUNREACH, -ENFILE, -ENOMEM,
   -ECONNREFUSED (a reset answered), -EHOSTUNREACH (the next hop never answered ARP),
   -ETIMEDOUT (no answer within timeout_ms, or the SYN's retries ran out), -EINTR (cut short).
   A connection that was made is handed out even when the other end has sent its data and closed,
   or reset it, before this task ran again: read() says so. reserved: the kernel's own. */
static int open_conn(ipaddr_t dst, u16 dport, u_long timeout_ms, bool reserved, struct tcp_socket **out) {
    struct tcp_socket *s;
    int err = 0;
    *out = NULL;
    if(!dst || !dport) return -EINVAL;
    mutex_lock(&net_lock);
    err = no_route(dst);
    s = err ? NULL : sock_new(true, reserved, &err);
    if(!s) { mutex_unlock(&net_lock); return err; }
    s->local_port = ephemeral_port();
    s->remote_ip = dst;
    s->remote_port = dport;
    s->iss = (u32)getticks() * 2654435761UL ^ 0x7e57c0de;
    s->snd_una = s->iss;
    s->snd_nxt = s->iss + 1;
    s->snd_wnd = TCP_MSS;
    s->state = SYN_SENT;
    send_segment(dst, s->local_port, dport, s->iss, 0, TCP_SYN, window(s), NULL, 0, true);
    s->tx_segs++;
    arm_rto(s);
    WAIT_FOR(s, s->state != SYN_SENT, timeout_ms);
    if(!s->synced) {
        err = s->state == SYN_SENT ? (task_interrupted() ? -EINTR : -ETIMEDOUT) : (s->error ? s->error : -ECONNREFUSED);
        sock_free(s);
        mutex_unlock(&net_lock);
        return err;
    }
    mutex_unlock(&net_lock);
    *out = s;
    return 0;
}

int tcp_open(ipaddr_t dst, u16 dport, u_long timeout_ms, struct tcp_socket **out) {
    return open_conn(dst, dport, timeout_ms, false, out);
}

struct tcp_socket *tcp_connect(ipaddr_t dst, u16 dport, u_long timeout_ms) {
    struct tcp_socket *s;
    return open_conn(dst, dport, timeout_ms, true, &s) ? NULL : s;
}

/* 10/6/26: -EINVAL for port 0, -EADDRINUSE when another socket listens there, -ENFILE */
static int listen_port(u16 port, bool reserved, struct tcp_socket **out) {
    struct tcp_socket *s = NULL;
    int err = 0;
    *out = NULL;
    if(!port) return -EINVAL;
    mutex_lock(&net_lock);
    if(port_listened(port)) err = -EADDRINUSE;
    else s = sock_new(false, reserved, &err);
    if(s) { s->local_port = port; s->state = LISTEN; }
    mutex_unlock(&net_lock);
    *out = s;
    return s ? 0 : err;
}

int tcp_listen_on(u16 port, struct tcp_socket **out) {
    return listen_port(port, false, out);
}

struct tcp_socket *tcp_listen(u16 port) {
    struct tcp_socket *s;
    return listen_port(port, true, &s) ? NULL : s;
}

/* the first connection in l's backlog that finished its handshake (it may have been closed or reset
   since: read() says so), taken out of the backlog. 10/6/26: those that die during the handshake
   leave it at once (fail()). Under net_lock. */
static struct tcp_socket *backlog_take(struct tcp_socket *l) {
    u_int i;
    for(i = 0; i < l->backlog_n; i++) {
        struct tcp_socket *c = l->backlog[i];
        if(!c->synced) continue;
        backlog_remove(c);
        return c;
    }
    return NULL;
}

/* a connection to hand out (accept() does not wait; poll() says POLLIN) */
static bool backlog_ready(const struct tcp_socket *l) {
    u_int i;
    for(i = 0; i < l->backlog_n; i++) if(l->backlog[i]->synced) return true;
    return false;
}

/* 10/6/26: the next connection, within timeout_ms (TCP_NO_LIMIT: no limit); -ETIMEDOUT, -EINTR,
   -EINVAL when l does not listen */
int tcp_accept_next(struct tcp_socket *l, u_long timeout_ms, struct tcp_socket **out) {
    struct tcp_socket *c = NULL;
    int err = 0;
    mutex_lock(&net_lock);
    if(l->state != LISTEN) err = -EINVAL;
    while(!err && !(c = backlog_take(l))) {
        if(!WAIT_FOR(l, backlog_ready(l), timeout_ms)) err = task_interrupted() ? -EINTR : -ETIMEDOUT;
    }
    mutex_unlock(&net_lock);
    *out = c;
    return c ? 0 : err;
}

struct tcp_socket *tcp_accept(struct tcp_socket *l, u_long timeout_ms) {
    struct tcp_socket *c;
    return tcp_accept_next(l, timeout_ms, &c) ? NULL : c;
}

/* a connection data may still be sent on */
static bool can_send(const struct tcp_socket *s) {
    return s->state == ESTABLISHED || s->state == CLOSE_WAIT;
}

/* why nothing more can be sent: the error that ended the connection (a reset, a timeout), or the
   end of it (-EPIPE: the connection was closed both ways) */
static int send_error(const struct tcp_socket *s) {
    return s->error ? s->error : -EPIPE;
}

/* queue up to len bytes (as many as there is room for, at least one); under net_lock */
static u_int queue_some(struct tcp_socket *s, const u8 *p, u_int len) {
    u_int room = TCP_BUF - s->tx_len, chunk = len < room ? len : room;
    memcpy(s->txbuf + s->tx_len, p, chunk);
    s->tx_len += chunk;
    tcp_output(s);
    return chunk;
}

int tcp_send(struct tcp_socket *s, const void *data, u_int len) {
    const u8 *p = (const u8 *)data;
    u_int done = 0;
    mutex_lock(&net_lock);
    while(done < len) {
        if(!can_send(s)) { mutex_unlock(&net_lock); return s->error ? s->error : -ENOTCONN; }
        if(s->tx_len == TCP_BUF) {
            if(!WAIT_FOR(s, s->tx_len < TCP_BUF || !can_send(s), 10000)) { mutex_unlock(&net_lock); return -ETIMEDOUT; }
            continue;
        }
        done += queue_some(s, p + done, len - done);
    }
    mutex_unlock(&net_lock);
    return (int)done;
}

/* 10/6/26: write() on a connection: as many bytes as fit in the send buffer now, at least one: it
   waits (as long as it takes, a kill or Ctrl+C aside: -EINTR) only while none fits, so after poll()
   says POLLOUT it never waits; the caller writes the rest later. -ECONNRESET or -ETIMEDOUT when
   that ended the connection, -EPIPE once it is closed both ways, -ENOTCONN on a listening socket. */
int tcp_write(struct tcp_socket *s, const void *data, u_int len) {
    int r = 0;
    mutex_lock(&net_lock);
    if(s->state == LISTEN) r = -ENOTCONN;
    else if(!WAIT_FOR(s, s->tx_len < TCP_BUF || !can_send(s), TCP_NO_LIMIT)) r = -EINTR;
    else if(!can_send(s)) r = send_error(s);
    else r = (int)queue_some(s, (const u8 *)data, len);
    mutex_unlock(&net_lock);
    return r;
}

/* the received bytes, up to size, into buf; under net_lock, with something there. 10/6/26: the
   window opened: the other end hears of it when that matters (from less than a segment to one or
   more, or by two segments); smaller openings it finds by itself, probing a shut window */
static int take_rx(struct tcp_socket *s, void *buf, u_int size) {
    u_int n = s->rx_len < size ? s->rx_len : size, before = TCP_BUF - s->rx_len, after;
    memcpy(buf, s->rxbuf, n);
    memmove(s->rxbuf, s->rxbuf + n, s->rx_len - n);
    s->rx_len -= n;
    after = TCP_BUF - s->rx_len;
    if(s->state != CLOSED && ((before < s->mss && after >= s->mss) || after - before >= 2u * s->mss)) send_ack(s);
    return (int)n;
}

int tcp_recv(struct tcp_socket *s, void *buf, u_int size, u_long timeout_ms) {
    int r;
    mutex_lock(&net_lock);
    if(!WAIT_FOR(s, s->rx_len || s->peer_fin || s->state == CLOSED, timeout_ms)) { mutex_unlock(&net_lock); return -1; }
    if(s->rx_len) r = take_rx(s, buf, size);
    else r = s->state == CLOSED && s->error ? s->error : 0;
    mutex_unlock(&net_lock);
    return r;
}

/* 10/6/26: read() on a connection: what has arrived, up to len, waiting for something as long as it
   takes; 0 once the other end has closed and all was read; -ECONNRESET (or -ETIMEDOUT) when that
   ended it; -ENOTCONN on a listening socket; -EINTR cut short */
int tcp_read(struct tcp_socket *s, void *buf, u_int len) {
    int r;
    mutex_lock(&net_lock);
    if(s->state == LISTEN) r = -ENOTCONN;
    else if(!WAIT_FOR(s, s->rx_len || s->peer_fin || s->state == CLOSED, TCP_NO_LIMIT)) r = -EINTR;
    else if(s->rx_len) r = take_rx(s, buf, len);
    else r = s->state == CLOSED && s->error ? s->error : 0;
    mutex_unlock(&net_lock);
    return r;
}

/* the FIN goes out behind the data; 0, or 1 when the socket is gone already. Under net_lock. */
static int start_close(struct tcp_socket *s) {
    switch(s->state) {
    case ESTABLISHED: s->fin_pending = true; set_state(s, FIN_WAIT_1); tcp_output(s); return 0;
    case CLOSE_WAIT:  s->fin_pending = true; set_state(s, LAST_ACK); tcp_output(s); return 0;
    case SYN_RCVD:    s->fin_pending = true; set_state(s, FIN_WAIT_1); tcp_output(s); return 0;
    case FIN_WAIT_1: case FIN_WAIT_2: case CLOSING: case LAST_ACK: case TIME_WAIT: return 0;
    default:
        sock_free(s);
        return 1;
    }
}

static void make_orphan(struct tcp_socket *s) {
    s->orphan = true;
    s->orphan_at = getticks();
}

int tcp_close(struct tcp_socket *s) {
    mutex_lock(&net_lock);
    if(start_close(s)) { mutex_unlock(&net_lock); return 0; }
    /* give the handshake a moment, then let the socket finish by itself */
    WAIT_FOR(s, s->state == CLOSED || s->state == TIME_WAIT, 2000);
    if(s->state == CLOSED) sock_free(s);
    else make_orphan(s);
    mutex_unlock(&net_lock);
    return 0;
}

/* 10/6/26: the last descriptor of s is closed (net/sock.c). It never waits. A connection sends its
   FIN behind the data still queued and finishes on its own; a listening socket stops listening,
   and the connections waiting in its backlog, never accepted, are reset. */
void tcp_release(struct tcp_socket *s) {
    mutex_lock(&net_lock);
    if(s->state == LISTEN) {
        u_int i;
        for(i = 0; i < s->backlog_n; i++) abort_conn(s->backlog[i]);
        s->backlog_n = 0;
        sock_free(s);
    } else if(!start_close(s)) make_orphan(s);
    mutex_unlock(&net_lock);
}

/* 10/6/26: what poll() reports for s (surfos/poll.h). poll() asks with interrupts on and no lock
   held; net_lock keeps the backlog and the buffers still while it looks. Every change wakes poll(),
   which looks again. */
int tcp_poll(struct tcp_socket *s) {
    int ev = 0;
    mutex_lock(&net_lock);
    if(s->state == LISTEN) {
        if(backlog_ready(s)) ev = POLLIN;
    } else {
        if(s->rx_len || s->peer_fin || s->state == CLOSED) ev |= POLLIN;
        if(can_send(s) && s->tx_len < TCP_BUF) ev |= POLLOUT;
        if(s->peer_fin || s->state == CLOSED) ev |= POLLHUP;
        if(s->error) ev |= POLLERR;
    }
    mutex_unlock(&net_lock);
    return ev;
}

bool tcp_listening(const struct tcp_socket *s) {
    return s->state == LISTEN;
}

ipaddr_t tcp_peer(struct tcp_socket *s, u16 *port) {
    if(port) *port = s->remote_port;
    return s->remote_ip;
}

void tcp_print(void) {
    char a[16];
    int i, n = 0;
    for(i = 0; i < TCP_SOCKETS; i++) {
        struct tcp_socket *s = &socks[i];
        if(!s->used) continue;
        printf("    tcp  %-11s local %-5u remote %s:%u  rx %lu tx %lu retrans %lu  buf in %u out %u%s\n", state_names[s->state],
               s->local_port, ipfmt(s->remote_ip, a), s->remote_port, s->rx_segs, s->tx_segs, s->retrans, s->rx_len, s->tx_len,
               s->orphan ? " (orphan)" : "");
        n++;
    }
    if(!n) printf("    (no TCP sockets)\n");
}
