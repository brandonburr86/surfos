/*
SurfOS Network Stack: ICMP
--------------------
File: icmp.c    Date: 10/3/26 (roadmap N2)
--------------------
Echo replies, and echo requests for ping with one outstanding request.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/timer.h>
#include <surfos/sync.h>
#include <net/net.h>
#include <net/ip.h>
#include <blibc_common.h>

static struct {
    bool waiting;
    u16 id, seq;
    ipaddr_t from;
    u8 ttl;
    u_long sent, rtt_ticks;
    event_t done;
} ping;

void icmp_input(struct netdev *dev, struct netbuf *nb, const struct ip_hdr *ip) {
    struct icmp_hdr *ic = (struct icmp_hdr *)netbuf_data(nb);
    if(nb->len < sizeof(*ic) || ip_checksum(ic, nb->len) != 0) { netbuf_free(nb); return; }
    if(ic->type == ICMP_ECHO && ip->dst == dev->ip) {      /* answer in place: same payload, swapped roles */
        ic->type = ICMP_ECHO_REPLY;
        ic->csum = 0;
        ic->csum = ip_checksum(ic, nb->len);
        ip_output(dev, ip->src, IPPROTO_ICMP, nb);
        return;
    }
    if(ic->type == ICMP_ECHO_REPLY && ping.waiting && ic->id == ping.id && ic->seq == ping.seq) {
        ping.waiting = false;
        ping.from = ip->src;
        ping.ttl = ip->ttl;
        ping.rtt_ticks = getticks() - ping.sent;
        event_set(&ping.done);
    }
    netbuf_free(nb);
}

int icmp_ping(ipaddr_t dst, u16 id, u16 seq, u_int payload, u_long timeout_ms, u8 *ttl_out) {
    struct netbuf *nb;
    struct icmp_hdr *ic;
    u8 *p;
    u_int i;
    int r;

    if(payload > 1400) payload = 1400;
    mutex_lock(&net_lock);
    nb = netbuf_alloc();
    if(!nb) { mutex_unlock(&net_lock); return -1; }
    ic = (struct icmp_hdr *)netbuf_put(nb, sizeof(*ic) + payload);
    ic->type = ICMP_ECHO;
    ic->code = 0;
    ic->id = htons(id);
    ic->seq = htons(seq);
    p = (u8 *)(ic + 1);
    for(i = 0; i < payload; i++) p[i] = (u8)('a' + i % 26);
    ic->csum = 0;
    ic->csum = ip_checksum(ic, sizeof(*ic) + payload);
    event_init(&ping.done);
    ping.waiting = true;
    ping.id = htons(id);
    ping.seq = htons(seq);
    ping.sent = getticks();
    r = ip_output(NULL, dst, IPPROTO_ICMP, nb);
    mutex_unlock(&net_lock);
    if(r != 0) { ping.waiting = false; return -1; }
    if(event_wait_timeout(&ping.done, timeout_ms) != 0) { ping.waiting = false; return -1; }
    if(ttl_out) *ttl_out = ping.ttl;
    return (int)(ping.rtt_ticks * (1000 / HZ));
}
