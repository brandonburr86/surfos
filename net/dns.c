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
10/15/26 (Undertow review M1): the query ID and the source port are random (RFC 5452 section
9.2), no longer a counter from 0x3a7c and the next ephemeral port, so that an attacker off the
path has to guess about 2^30 values instead of a handful to forge an answer.
*/

#include <surfos/types.h>
#include <surfos/ktimer.h>
#include <surfos/task.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/dhcp.h>
#include <net/dns_wire.h>
#include <fs/vfs.h>
#include <crypto/crypto.h>
#include <blibc_common.h>

#define DNS_PORT 53
#define DNS_QUERY_MAX (12 + DNS_NAME_MAX + 2 + 4)   /* header, the name as labels, type and class */
#define DNS_RETRY_MS 1000           /* the first wait for an answer before the query goes out again */
#define DNS_RETRY_MAX_MS 4000       /* the waits double up to this */
#define DNS_PORT_FIRST 49152        /* the random source port: 49152-65535, as udp_open(0)'s */
#define DNS_PORT_TRIES 8            /* random ports tried before udp_open(0)'s next free one */

/* 10/15/26 (review M1): the ID and the port come from crypto_random(), the kernel's HMAC-DRBG
   (lib/crypto/random.c). With RDRAND it is seeded and refreshed from the hardware; without it
   (QEMU's default CPUs, a 486) only from the TSC and the tick count, which an attacker cannot
   read from the network but which is weaker than RDRAND. A lookup must work on any CPU, so this
   is the right trade-off here: crypto_random() never blocks or fails. The strict
   crypto_random_strong() (TLS, SYS_getentropy) stays RDRAND-only and is not used. */
static u16 dns_random16(void) {
    u8 r[2];
    crypto_random(r, sizeof(r));
    return (u16)(r[0] << 8 | r[1]);
}

/* a socket on a random port; when DNS_PORT_TRIES ports are all taken (eight sockets in all),
   the next free one */
static struct udp_socket *dns_socket(void) {
    struct udp_socket *s;
    int i;
    for(i = 0; i < DNS_PORT_TRIES; i++) {
        s = udp_open((u16)(DNS_PORT_FIRST + (dns_random16() & 0x3fff)));
        if(s) return s;
    }
    return udp_open(0);
}

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
    mutex_unlock(&net_lock);
    if(!server) return -ENETUNREACH;
    id = dns_random16();

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
    s = dns_socket();
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
