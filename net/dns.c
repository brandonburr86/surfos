/*
SurfOS Network Stack: DNS resolver
--------------------
File: dns.c     Date: 10/3/26 (roadmap N2)
--------------------
One A query to the configured server, with name compression on the way back.
10/9/26 (Wi-Fi, track S): the server is the default interface's, else that of the first
configured interface that has one (wlan0's when it is the only one with a lease).
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
    u8 pkt[NETBUF_SIZE];
    u_int n = 12;
    u16 id, port;
    u32 start = (u32)uptime_ms(), elapsed;
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

    memset(pkt, 0, 12);
    pkt[0] = id >> 8; pkt[1] = id;
    pkt[2] = 1; pkt[5] = 1;          /* RD, one question */
    while(*label) {
        const char *dot = strchr(label, '.');
        u_int len = dot ? (u_int)(dot - label) : strlen(label);
        pkt[n++] = len;
        memcpy(pkt + n, label, len); n += len;
        label += len;
        if(*label == '.') label++;
    }
    pkt[n++] = 0;
    pkt[n++] = 0; pkt[n++] = 1;      /* A/IN */
    pkt[n++] = 0; pkt[n++] = 1;
    if(!timeout_ms) return -ETIMEDOUT;
    s = udp_open(0);
    if(!s) return -ENFILE;
    udp_bind_dev(s, dev);
    if(udp_sendto(s, server, DNS_PORT, pkt, n) != 0) {
        r = task_interrupted() ? -EINTR : -ENETUNREACH;
        udp_close(s); return r;
    }
    for(;;) {
        if(task_interrupted()) { r = -EINTR; break; }
        elapsed = (u32)((u32)uptime_ms() - start);
        if(elapsed >= timeout_ms) { r = -ETIMEDOUT; break; }
        r = udp_recvfrom(s, pkt, sizeof(pkt), &source, &port, timeout_ms - elapsed);
        if(task_interrupted()) { r = -EINTR; break; }
        if(r < 0 || (u32)((u32)uptime_ms() - start) >= timeout_ms) { r = -ETIMEDOUT; break; }
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
