/*
SurfOS Network Stack: DNS resolver
--------------------
File: dns.c     Date: 10/3/26 (roadmap N2)
--------------------
One A query to the configured server, with name compression on the way back.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/timer.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/dhcp.h>
#include <blibc_common.h>

#define DNS_PORT 53

struct dns_hdr {
    u16 id, flags, qd, an, ns, ar;
} __attribute__((packed));

/* skip a (possibly compressed) name; returns the next offset or 0 */
static u_int skip_name(const u8 *p, u_int len, u_int off) {
    while(off < len) {
        u8 l = p[off];
        if(l == 0) return off + 1;
        if((l & 0xC0) == 0xC0) return off + 2;
        off += 1 + l;
    }
    return 0;
}

int dns_resolve(const char *name, ipaddr_t *out, u_long timeout_ms) {
    struct netdev *dev = netdev_default();
    struct udp_socket *s;
    u8 pkt[512];
    struct dns_hdr *h = (struct dns_hdr *)pkt;
    u_int n = sizeof(*h), i, off, qd, an;
    const char *label = name;
    u16 id = (u16)getticks() ^ 0x3a7c;
    int r;

    if(ip_parse(name, out)) return 0;
    if(!dev || !dev->dns) return -1;
    memset(h, 0, sizeof(*h));
    h->id = htons(id);
    h->flags = htons(0x0100);                    /* recursion desired */
    h->qd = htons(1);
    while(*label) {                              /* www.example.com -> 3www7example3com0 */
        const char *dot = strchr(label, '.');
        u_int l = dot ? (u_int)(dot - label) : strlen(label);
        if(!l || l > 63 || n + l + 2 > sizeof(pkt) - 4) return -1;
        pkt[n++] = (u8)l;
        memcpy(pkt + n, label, l);
        n += l;
        label += l;
        if(*label == '.') label++;
    }
    pkt[n++] = 0;
    pkt[n++] = 0; pkt[n++] = 1;                  /* type A */
    pkt[n++] = 0; pkt[n++] = 1;                  /* class IN */

    s = udp_open(0);
    if(!s) return -1;
    if(udp_sendto(s, dev->dns, DNS_PORT, pkt, n) != 0) { udp_close(s); return -1; }
    for(;;) {
        r = udp_recvfrom(s, pkt, sizeof(pkt), NULL, NULL, timeout_ms);
        if(r < (int)sizeof(*h)) { udp_close(s); return -1; }
        if(ntohs(h->id) == id) break;
    }
    udp_close(s);
    if((ntohs(h->flags) & 0x000F) != 0) return -1;   /* RCODE: NXDOMAIN and friends */
    qd = ntohs(h->qd);
    an = ntohs(h->an);
    off = sizeof(*h);
    for(i = 0; i < qd; i++) { off = skip_name(pkt, r, off); if(!off) return -1; off += 4; }
    for(i = 0; i < an; i++) {
        u16 type, rdlen;
        off = skip_name(pkt, r, off);
        if(!off || off + 10 > (u_int)r) return -1;
        type = (u16)((pkt[off] << 8) | pkt[off + 1]);
        rdlen = (u16)((pkt[off + 8] << 8) | pkt[off + 9]);
        off += 10;
        if(off + rdlen > (u_int)r) return -1;
        if(type == 1 && rdlen == 4) { memcpy(out, pkt + off, 4); return 0; }
        off += rdlen;
    }
    return -1;
}
