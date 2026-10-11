/*
SurfOS Network Stack: DNS resolver
--------------------
File: dns.c     Date: 10/3/26 (roadmap N2)
--------------------
One A query to the configured server, with name compression on the way back.
10/9/26 (Wi-Fi, track S): the server is the default interface's, else that of the first
configured interface that has one (wlan0's when it is the only one with a lease).
10/11/26 (Undertow): the query goes out again after 1 s and 3 s (then every 4 s) while no
answer has come, all within the caller's one budget.
*/

#include <surfos/types.h>
#include <surfos/ktimer.h>
#include <surfos/task.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/dhcp.h>
#include <net/dns_wire.h>
#include <fs/vfs.h>
#include <blibc_common.h>

#define DNS_PORT 53
#define DNS_QUERY_MAX (12 + DNS_NAME_MAX + 2 + 4)   /* header, the name as labels, type and class */
#define DNS_RETRY_MS 1000           /* the first wait for an answer before the query goes out again */
#define DNS_RETRY_MAX_MS 4000       /* the waits double up to this */
/* Protected with net_lock; same-tick callers receive distinct query IDs. */
static u16 next_id = 0x3a7c;

ipaddr_t dns_server(void) {
    struct netdev *d = netdev_default(), *down = NULL;
    if(d && d->ip && d->dns && d->link_up) return d->dns;
    for(d = netdev_first(); d; d = d->next) {
        if(!d->ip || !d->dns) continue;
        if(d->link_up) return d->dns;
        if(!down) down = d;
    }
    return down ? down->dns : 0;
}

/* One absolute budget, including send time and every discarded datagram.
   Unsigned elapsed arithmetic remains valid across the 32-bit uptime wrap. */
int dns_lookup(const char *name, ipaddr_t *out, u_long timeout_ms) {
    ipaddr_t server, source, address;
    struct netdev *dev;
    struct udp_socket *s;
    u8 pkt[NETBUF_SIZE], query[DNS_QUERY_MAX];
    u_int n = 12;
    u16 id, port;
    u32 start = (u32)uptime_ms(), elapsed, resend = 0, gap = DNS_RETRY_MS;
    const char *label = name;
    int r;

    if(!out || !dns_name_valid(name) || timeout_ms > 0x7fffffffUL) return -EINVAL;
    if(ip_parse(name, &address)) { *out = address; return 0; }
    if(task_interrupted()) return -EINTR;
    mutex_lock(&net_lock);
    server = dns_server();
    dev = server ? ip_route(0, server) : NULL;
    if(!dev || !dev->ip || !dev->link_up) server = 0;
    else if(dev->netmask && (server & dev->netmask) != (dev->ip & dev->netmask) && !dev->gateway) server = 0;
    id = next_id++;
    mutex_unlock(&net_lock);
    if(!server) return -ENETUNREACH;

    memset(query, 0, 12);
    query[0] = id >> 8; query[1] = id;
    query[2] = 1; query[5] = 1;      /* RD, one question */
    while(*label) {
        const char *dot = strchr(label, '.');
        u_int len = dot ? (u_int)(dot - label) : strlen(label);
        query[n++] = len;
        memcpy(query + n, label, len); n += len;
        label += len;
        if(*label == '.') label++;
    }
    query[n++] = 0;
    query[n++] = 0; query[n++] = 1;  /* A/IN */
    query[n++] = 0; query[n++] = 1;
    if(!timeout_ms) return -ETIMEDOUT;
    s = udp_open(0);
    if(!s) return -ENFILE;
    udp_bind_dev(s, dev);
    for(;;) {
        if(task_interrupted()) { r = -EINTR; break; }
        elapsed = (u32)((u32)uptime_ms() - start);
        if(elapsed >= timeout_ms) { r = -ETIMEDOUT; break; }
        if(elapsed >= resend) {
            /* 10/11/26 (Undertow): the question or its answer may be lost on the way (a UDP datagram,
               Wi-Fi): ask again after 1 s, then 2 s, then every 4 s while the budget lasts, with the
               same ID, so that an answer to any of the copies counts (RFC 1123 6.1.3.3) */
            if(udp_sendto(s, server, DNS_PORT, query, n) != 0) {
                r = task_interrupted() ? -EINTR : -ENETUNREACH;
                break;
            }
            resend = elapsed + gap;
            if(gap < DNS_RETRY_MAX_MS) gap *= 2;
            continue;
        }
        r = udp_recvfrom(s, pkt, sizeof(pkt), &source, &port, (resend < timeout_ms ? resend : timeout_ms) - elapsed);
        if(task_interrupted()) { r = -EINTR; break; }
        if(r < 0) continue;              /* nothing came in time: ask again, or give up, above */
        if((u32)((u32)uptime_ms() - start) >= timeout_ms) { r = -ETIMEDOUT; break; }
        if(source != server || port != DNS_PORT) continue;
        r = dns_parse_a(pkt, (u_int)r, id, name, &address);
        if(r == 1) continue;
        if(r == 0) *out = address;
        break;
    }
    udp_close(s);
    return r;
}

/* Keep the original kernel command API's 0/-1 convention. */
int dns_resolve(const char *name, ipaddr_t *out, u_long timeout_ms) {
    return dns_lookup(name, out, timeout_ms) == 0 ? 0 : -1;
}
