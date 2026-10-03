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
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/timer.h>
#include <surfos/sync.h>
#include <mm/kalloc.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <fs/vfs.h>
#include <blibc_common.h>

#define TCP_SOCKETS 8
#define TCP_BUF 8192
#define TCP_MSS 1460
#define TCP_RTO (HZ)                    /* ticks */
#define TCP_MAX_RETRIES 8
#define TCP_TIME_WAIT (2 * HZ)
#define TCP_BACKLOG 4
#define ECONNREFUSED 111
#define ECONNRESET 104
#define ETIMEDOUT 110
#define ENOTCONN 107

enum { CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT };
static const char *const state_names[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                           "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };

#define SEQ_LT(a, b) ((i32)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((i32)((a) - (b)) <= 0)

struct tcp_socket {
    bool used, orphan;                  /* orphan: closed by the application, finishing on its own */
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

/**** sockets ****/

static struct tcp_socket *sock_alloc(void) {
    int i;
    for(i = 0; i < TCP_SOCKETS; i++) {
        struct tcp_socket *s = &socks[i];
        if(s->used) continue;
        memset(s, 0, sizeof(*s));
        s->txbuf = (u8 *)kalloc(TCP_BUF);
        s->rxbuf = (u8 *)kalloc(TCP_BUF);
        if(!s->txbuf || !s->rxbuf) { kfree(s->txbuf); kfree(s->rxbuf); return NULL; }
        s->used = true;
        s->state = CLOSED;
        event_init(&s->ev);
        return s;
    }
    return NULL;
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

static u16 ephemeral_port(void) {
    do { next_port++; if(next_port < 40000) next_port = 40000; } while(port_in_use(next_port));
    return next_port;
}

static void set_state(struct tcp_socket *s, int state) {
    s->state = state;
    event_set(&s->ev);
}

static void fail(struct tcp_socket *s, int err) {
    s->error = err;
    s->rto_at = 0;
    set_state(s, CLOSED);
    if(s->orphan) sock_free(s);
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

static void send_ack(struct tcp_socket *s) {
    send_segment(s->remote_ip, s->local_port, s->remote_port, s->snd_nxt, s->rcv_nxt, TCP_ACK, window(s), NULL, 0, false);
    s->tx_segs++;
}

static void arm_rto(struct tcp_socket *s) {
    if(!s->rto_at) { s->rto_at = getticks() + TCP_RTO; s->retries = 0; }
}

/* send whatever the window allows from snd_nxt on, then the FIN if it is due */
static void tcp_output(struct tcp_socket *s) {
    u32 in_flight = s->snd_nxt - s->snd_una;
    while(in_flight < s->tx_len) {
        u_int chunk = s->tx_len - in_flight, room = s->snd_wnd > in_flight ? s->snd_wnd - in_flight : 0;
        if(chunk > TCP_MSS) chunk = TCP_MSS;
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
}

/* the oldest unacknowledged segment again (or the SYN / FIN) */
static void retransmit(struct tcp_socket *s) {
    s->retrans++;
    if(++s->retries > TCP_MAX_RETRIES) { fail(s, -ETIMEDOUT); return; }
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
            u_int chunk = s->tx_len > TCP_MSS ? TCP_MSS : s->tx_len;
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
    event_set(&s->ev);
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
        if(!(flags & TCP_SYN) || (flags & (TCP_ACK | TCP_RST)) || s->backlog_n >= TCP_BACKLOG) { netbuf_free(nb); return; }
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
        c->state = SYN_RCVD;
        s->backlog[s->backlog_n++] = c;
        send_segment(c->remote_ip, c->local_port, c->remote_port, c->iss, c->rcv_nxt, TCP_SYN | TCP_ACK, window(c), NULL, 0, true);
        c->tx_segs++;
        arm_rto(c);
        netbuf_free(nb);
        return;
    }

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
            if(datalen || (flags & (TCP_SYN | TCP_FIN))) send_ack(s);        /* out of order or old: say where we are */
            netbuf_free(nb);
            return;
        }
    }

    if(s->state == SYN_RCVD) {
        if(ack != s->snd_nxt) { netbuf_free(nb); return; }
        s->snd_una = ack;
        s->rto_at = 0;
        set_state(s, ESTABLISHED);
        {   /* tell the listener there is a connection to accept */
            int i;
            for(i = 0; i < TCP_SOCKETS; i++) if(socks[i].used && socks[i].state == LISTEN && socks[i].local_port == s->local_port) event_set(&socks[i].ev);
        }
    }

    s->snd_wnd = ntohs(th->win);
    process_ack(s, ack);

    if(datalen && (s->state == ESTABLISHED || s->state == FIN_WAIT_1 || s->state == FIN_WAIT_2)) {
        u_int room = TCP_BUF - s->rx_len;
        if(datalen > room) datalen = room;
        memcpy(s->rxbuf + s->rx_len, data, datalen);
        s->rx_len += datalen;
        s->rcv_nxt += datalen;
        event_set(&s->ev);
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

void tcp_tick(void) {
    u_long now = getticks();
    int i;
    for(i = 0; i < TCP_SOCKETS; i++) {
        struct tcp_socket *s = &socks[i];
        if(!s->used) continue;
        if(s->state == TIME_WAIT && (long)(now - s->wait_until) >= 0) {
            set_state(s, CLOSED);
            if(s->orphan) sock_free(s);
            continue;
        }
        if(s->rto_at && (long)(now - s->rto_at) >= 0) retransmit(s);
    }
}

/**** the API ****/

/* wait until cond, with the lock released; returns false on timeout */
#define WAIT_FOR(s, cond, timeout_ms) ({                                   \
    bool ok_ = true;                                                        \
    u_long deadline_ = getticks() + (timeout_ms) / (1000 / HZ);            \
    while(!(cond)) {                                                        \
        long left_ = (long)(deadline_ - getticks());                        \
        if(left_ <= 0) { ok_ = false; break; }                              \
        event_clear(&(s)->ev);                                              \
        if(cond) break;                                                     \
        mutex_unlock(&net_lock);                                            \
        event_wait_timeout(&(s)->ev, (u_long)left_ * (1000 / HZ));          \
        mutex_lock(&net_lock);                                              \
    }                                                                       \
    ok_; })

struct tcp_socket *tcp_connect(ipaddr_t dst, u16 dport, u_long timeout_ms) {
    struct tcp_socket *s;
    bool ok;
    mutex_lock(&net_lock);
    s = sock_alloc();
    if(!s) { mutex_unlock(&net_lock); return NULL; }
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
    ok = WAIT_FOR(s, s->state != SYN_SENT, timeout_ms);
    if(!ok || s->state != ESTABLISHED) {
        int err = s->error ? s->error : -ETIMEDOUT;
        sock_free(s);
        mutex_unlock(&net_lock);
        (void)err;
        return NULL;
    }
    mutex_unlock(&net_lock);
    return s;
}

struct tcp_socket *tcp_listen(u16 port) {
    struct tcp_socket *s;
    mutex_lock(&net_lock);
    if(port_in_use(port)) { mutex_unlock(&net_lock); return NULL; }
    s = sock_alloc();
    if(s) { s->local_port = port; s->state = LISTEN; }
    mutex_unlock(&net_lock);
    return s;
}

struct tcp_socket *tcp_accept(struct tcp_socket *l, u_long timeout_ms) {
    struct tcp_socket *c = NULL;
    u_int i, j;
    mutex_lock(&net_lock);
    for(;;) {
        for(i = 0; i < l->backlog_n; i++) {
            if(l->backlog[i]->state == ESTABLISHED || l->backlog[i]->state == CLOSE_WAIT) {
                c = l->backlog[i];
                for(j = i + 1; j < l->backlog_n; j++) l->backlog[j - 1] = l->backlog[j];
                l->backlog_n--;
                break;
            }
            if(l->backlog[i]->state == CLOSED) {         /* died during the handshake */
                sock_free(l->backlog[i]);
                for(j = i + 1; j < l->backlog_n; j++) l->backlog[j - 1] = l->backlog[j];
                l->backlog_n--;
                i--;
            }
        }
        if(c || !WAIT_FOR(l, l->backlog_n && (l->backlog[0]->state == ESTABLISHED || l->backlog[0]->state == CLOSE_WAIT || l->backlog[0]->state == CLOSED), timeout_ms)) break;
    }
    mutex_unlock(&net_lock);
    return c;
}

int tcp_send(struct tcp_socket *s, const void *data, u_int len) {
    const u8 *p = (const u8 *)data;
    u_int done = 0;
    mutex_lock(&net_lock);
    while(done < len) {
        u_int room, chunk;
        if(s->state != ESTABLISHED && s->state != CLOSE_WAIT) { mutex_unlock(&net_lock); return s->error ? s->error : -ENOTCONN; }
        room = TCP_BUF - s->tx_len;
        if(!room) {
            if(!WAIT_FOR(s, s->tx_len < TCP_BUF || (s->state != ESTABLISHED && s->state != CLOSE_WAIT), 10000)) { mutex_unlock(&net_lock); return -ETIMEDOUT; }
            continue;
        }
        chunk = len - done < room ? len - done : room;
        memcpy(s->txbuf + s->tx_len, p + done, chunk);
        s->tx_len += chunk;
        done += chunk;
        tcp_output(s);
    }
    mutex_unlock(&net_lock);
    return (int)done;
}

int tcp_recv(struct tcp_socket *s, void *buf, u_int size, u_long timeout_ms) {
    int r;
    mutex_lock(&net_lock);
    if(!WAIT_FOR(s, s->rx_len || s->peer_fin || s->state == CLOSED, timeout_ms)) { mutex_unlock(&net_lock); return -1; }
    if(s->rx_len) {
        u_int n = s->rx_len < size ? s->rx_len : size;
        memcpy(buf, s->rxbuf, n);
        memmove(s->rxbuf, s->rxbuf + n, s->rx_len - n);
        s->rx_len -= n;
        if(s->state != CLOSED) send_ack(s);              /* the window opened */
        r = (int)n;
    } else r = s->state == CLOSED && s->error ? s->error : 0;
    mutex_unlock(&net_lock);
    return r;
}

int tcp_close(struct tcp_socket *s) {
    mutex_lock(&net_lock);
    switch(s->state) {
    case ESTABLISHED: s->fin_pending = true; set_state(s, FIN_WAIT_1); tcp_output(s); break;
    case CLOSE_WAIT:  s->fin_pending = true; set_state(s, LAST_ACK); tcp_output(s); break;
    case SYN_RCVD:    s->fin_pending = true; set_state(s, FIN_WAIT_1); tcp_output(s); break;
    case SYN_SENT: case LISTEN: case CLOSED: default:
        sock_free(s);
        mutex_unlock(&net_lock);
        return 0;
    }
    /* give the handshake a moment, then let the socket finish by itself */
    WAIT_FOR(s, s->state == CLOSED || s->state == TIME_WAIT, 2000);
    if(s->state == CLOSED) sock_free(s);
    else s->orphan = true;
    mutex_unlock(&net_lock);
    return 0;
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
