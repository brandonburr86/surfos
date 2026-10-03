/*
SurfOS Network Stack: DHCP client
--------------------
File: dhcp.c    Date: 10/3/26 (roadmap N2)
--------------------
DISCOVER, OFFER, REQUEST, ACK over a UDP socket on port 68; the lease is
applied to the interface. No renewal yet: the lease time is only reported.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/timer.h>
#include <net/net.h>
#include <net/eth.h>
#include <net/ip.h>
#include <net/dhcp.h>
#include <blibc_common.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6
#define OPT_SUBNET    1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_REQ_IP    50
#define OPT_LEASE     51
#define OPT_MSG_TYPE  53
#define OPT_SERVER_ID 54
#define OPT_PARAMS    55
#define OPT_END       255

struct dhcp_msg {
    u8 op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    ipaddr_t ciaddr, yiaddr, siaddr, giaddr;
    u8 chaddr[16], sname[64], file[128];
    u8 cookie[4];
    u8 options[312];
} __attribute__((packed));

static u_long lease_seconds;

u_long dhcp_lease_seconds(void) {
    return lease_seconds;
}

static u_int build(struct dhcp_msg *m, struct netdev *dev, u32 xid, u8 type, ipaddr_t req, ipaddr_t server) {
    u8 *o = m->options;
    memset(m, 0, sizeof(*m));
    m->op = 1;
    m->htype = 1;
    m->hlen = ETH_ALEN;
    m->xid = xid;
    m->flags = htons(0x8000);                      /* please broadcast the reply */
    memcpy(m->chaddr, dev->mac, ETH_ALEN);
    m->cookie[0] = 99; m->cookie[1] = 130; m->cookie[2] = 83; m->cookie[3] = 99;
    *o++ = OPT_MSG_TYPE; *o++ = 1; *o++ = type;
    if(req) { *o++ = OPT_REQ_IP; *o++ = 4; memcpy(o, &req, 4); o += 4; }
    if(server) { *o++ = OPT_SERVER_ID; *o++ = 4; memcpy(o, &server, 4); o += 4; }
    *o++ = OPT_PARAMS; *o++ = 4; *o++ = OPT_SUBNET; *o++ = OPT_ROUTER; *o++ = OPT_DNS; *o++ = OPT_LEASE;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, dev->mac, ETH_ALEN); o += ETH_ALEN;   /* client identifier */
    *o++ = OPT_END;
    return (u_int)(o - (u8 *)m) < 300 ? 300 : (u_int)(o - (u8 *)m);
}

/* the option, or NULL; *len gets its length */
static const u8 *option(const struct dhcp_msg *m, u_int size, u8 code, u_int *len) {
    const u8 *o = m->options, *end = (const u8 *)m + size;
    while(o + 2 <= end && *o != OPT_END) {
        if(*o == 0) { o++; continue; }
        if(o + 2 + o[1] > end) break;
        if(*o == code) { *len = o[1]; return o + 2; }
        o += 2 + o[1];
    }
    return NULL;
}

/* wait for a reply of the given type to our xid */
static int expect(struct udp_socket *s, u32 xid, u8 type, struct dhcp_msg *m, u_long deadline) {
    for(;;) {
        u_long now = getticks();
        const u8 *t;
        u_int n, l;
        int r;
        if(now >= deadline) return -1;
        r = udp_recvfrom(s, m, sizeof(*m), NULL, NULL, (deadline - now) * (1000 / HZ));
        if(r < 0) return -1;
        n = (u_int)r;
        if(n < 240 || m->op != 2 || m->xid != xid) continue;
        if(memcmp(m->cookie, "\x63\x82\x53\x63", 4)) continue;
        t = option(m, n, OPT_MSG_TYPE, &l);
        if(!t || l != 1) continue;
        if(*t == type) return (int)n;
        if(*t == DHCP_NAK) return -2;
    }
}

int dhcp_configure(struct netdev *dev, u_long timeout_ms) {
    struct udp_socket *s = udp_open(DHCP_CLIENT_PORT);
    struct dhcp_msg *m;
    u32 xid = (u32)getticks() * 2654435761UL ^ 0x5f0f1e2d;
    u_long deadline = getticks() + timeout_ms / (1000 / HZ);
    ipaddr_t offered = 0, server = 0, mask = 0, gw = 0, dns = 0;
    const u8 *o;
    u_int l, n;
    int attempt, r = -1;
    char a[16], b[16];

    if(!s) { kprintf("dhcp: port 68 is busy\n"); return -1; }
    m = (struct dhcp_msg *)kalloc(sizeof(*m));
    if(!m) { udp_close(s); return -1; }
    for(attempt = 0; attempt < 3 && getticks() < deadline; attempt++) {
        n = build(m, dev, xid, DHCP_DISCOVER, 0, 0);
        if(udp_sendto(s, IP_BROADCAST, DHCP_SERVER_PORT, m, n) != 0) break;
        {
            u_long step = getticks() + 2 * HZ;
            r = expect(s, xid, DHCP_OFFER, m, step < deadline ? step : deadline);
        }
        if(r <= 0) continue;
        offered = m->yiaddr;
        o = option(m, (u_int)r, OPT_SERVER_ID, &l);
        if(o && l == 4) memcpy(&server, o, 4);
        n = build(m, dev, xid, DHCP_REQUEST, offered, server);
        if(udp_sendto(s, IP_BROADCAST, DHCP_SERVER_PORT, m, n) != 0) break;
        {
            u_long step = getticks() + 2 * HZ;
            r = expect(s, xid, DHCP_ACK, m, step < deadline ? step : deadline);
        }
        if(r > 0) break;
    }
    if(r > 0) {
        if((o = option(m, (u_int)r, OPT_SUBNET, &l)) != NULL && l == 4) memcpy(&mask, o, 4);
        if((o = option(m, (u_int)r, OPT_ROUTER, &l)) != NULL && l >= 4) memcpy(&gw, o, 4);
        if((o = option(m, (u_int)r, OPT_DNS, &l)) != NULL && l >= 4) memcpy(&dns, o, 4);
        if((o = option(m, (u_int)r, OPT_LEASE, &l)) != NULL && l == 4) lease_seconds = ntohl(*(const u32 *)o);
        mutex_lock(&net_lock);
        dev->ip = m->yiaddr;
        dev->netmask = mask ? mask : IP4(255, 255, 255, 0);
        dev->gateway = gw;
        dev->dns = dns;
        arp_announce(dev);
        mutex_unlock(&net_lock);
        kprintf("dhcp: %s: lease %s for %lu s from %s\n", dev->name, ipfmt(dev->ip, a), lease_seconds, ipfmt(server, b));
        r = 0;
    } else {
        kprintf("dhcp: %s: no %s\n", dev->name, offered ? "acknowledgement" : "offer");
        r = -1;
    }
    kfree(m);
    udp_close(s);
    return r;
}
