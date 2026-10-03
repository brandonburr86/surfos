/*
SurfOS Network Stack: UDP
--------------------
File: udp.c     Date: 10/3/26 (roadmap N2)
--------------------
Eight kernel sockets with a receive queue each; DHCP, DNS and the shell
use them.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/sync.h>
#include <net/net.h>
#include <net/ip.h>
#include <blibc_common.h>

#define UDP_SOCKETS 8
#define UDP_QUEUE_MAX 16

struct udp_socket {
    bool used;
    u16 port;
    struct netbuf *head, *tail;
    u_int queued;
    u_long rx, tx, dropped;
    event_t ready;
};

static struct udp_socket sockets[UDP_SOCKETS];
static u16 next_ephemeral = 49152;

static struct udp_socket *by_port(u16 port) {
    int i;
    for(i = 0; i < UDP_SOCKETS; i++) if(sockets[i].used && sockets[i].port == port) return &sockets[i];
    return NULL;
}

struct udp_socket *udp_open(u16 port) {
    struct udp_socket *s = NULL;
    int i;
    mutex_lock(&net_lock);
    if(port == 0) {
        do { port = next_ephemeral++; if(next_ephemeral == 0) next_ephemeral = 49152; } while(by_port(port));
    } else if(by_port(port)) {
        mutex_unlock(&net_lock);
        return NULL;
    }
    for(i = 0; i < UDP_SOCKETS; i++) if(!sockets[i].used) { s = &sockets[i]; break; }
    if(s) {
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->port = port;
        event_init(&s->ready);
    }
    mutex_unlock(&net_lock);
    return s;
}

void udp_close(struct udp_socket *s) {
    struct netbuf *nb;
    if(!s) return;
    mutex_lock(&net_lock);
    while((nb = s->head) != NULL) { s->head = nb->next; netbuf_free(nb); }
    s->used = false;
    mutex_unlock(&net_lock);
}

u16 udp_local_port(struct udp_socket *s) {
    return s->port;
}

int udp_sendto(struct udp_socket *s, ipaddr_t dst, u16 dport, const void *data, u_int len) {
    struct netbuf *nb;
    struct udp_hdr *uh;
    int r;
    if(len > NETBUF_SIZE - NETBUF_HEADROOM) return -1;
    mutex_lock(&net_lock);
    nb = netbuf_alloc();
    if(!nb) { mutex_unlock(&net_lock); return -1; }
    memcpy(netbuf_put(nb, len), data, len);
    uh = (struct udp_hdr *)netbuf_push(nb, sizeof(*uh));
    uh->sport = htons(s->port);
    uh->dport = htons(dport);
    uh->len = htons((u16)nb->len);
    uh->csum = 0;
    uh->csum = ip_checksum_pseudo(ip_route_src(dst), dst, IPPROTO_UDP, uh, nb->len);
    if(uh->csum == 0) uh->csum = 0xFFFF;
    r = ip_output(NULL, dst, IPPROTO_UDP, nb);
    if(r == 0) s->tx++;
    mutex_unlock(&net_lock);
    return r;
}

void udp_input(struct netdev *dev, struct netbuf *nb, const struct ip_hdr *ip) {
    struct udp_hdr *uh = (struct udp_hdr *)netbuf_data(nb);
    struct udp_socket *s;
    u_int len;
    if(nb->len < sizeof(*uh)) { netbuf_free(nb); return; }
    len = ntohs(uh->len);
    if(len < sizeof(*uh) || len > nb->len) { netbuf_free(nb); return; }
    nb->len = len;
    if(uh->csum && ip_checksum_pseudo(ip->src, ip->dst, IPPROTO_UDP, uh, len) != 0) { netbuf_free(nb); return; }
    s = by_port(ntohs(uh->dport));
    if(!s || s->queued >= UDP_QUEUE_MAX) { if(s) s->dropped++; netbuf_free(nb); return; }
    nb->aux_ip = ip->src;
    nb->aux_port = ntohs(uh->sport);
    netbuf_pull(nb, sizeof(*uh));
    nb->next = NULL;
    if(s->tail) s->tail->next = nb; else s->head = nb;
    s->tail = nb;
    s->queued++;
    s->rx++;
    event_set(&s->ready);
}

int udp_recvfrom(struct udp_socket *s, void *buf, u_int size, ipaddr_t *src, u16 *sport, u_long timeout_ms) {
    for(;;) {
        struct netbuf *nb;
        mutex_lock(&net_lock);
        nb = s->head;
        if(nb) {
            u_int n = nb->len < size ? nb->len : size;
            s->head = nb->next;
            if(!s->head) s->tail = NULL;
            s->queued--;
            mutex_unlock(&net_lock);
            memcpy(buf, netbuf_data(nb), n);
            if(src) *src = nb->aux_ip;
            if(sport) *sport = nb->aux_port;
            netbuf_free(nb);
            return (int)n;
        }
        event_clear(&s->ready);
        mutex_unlock(&net_lock);
        if(event_wait_timeout(&s->ready, timeout_ms) != 0) return -1;
    }
}

void udp_print(void) {
    int i, n = 0;
    for(i = 0; i < UDP_SOCKETS; i++) {
        if(!sockets[i].used) continue;
        printf("    udp  port %-5u  queued %u  rx %lu tx %lu dropped %lu\n", sockets[i].port, sockets[i].queued, sockets[i].rx, sockets[i].tx, sockets[i].dropped);
        n++;
    }
    if(!n) printf("    (no UDP sockets)\n");
}
