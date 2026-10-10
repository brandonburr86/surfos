/*
SurfOS Network Stack: IPv4
--------------------
File: ip.c      Date: 10/3/26 (roadmap N2)
--------------------
No fragments, no options on output, one interface.
10/6/26: ip_next_hop() says where a datagram goes first (TCP asks it which
connections an ARP failure concerns).
10/9/26 (Wi-Fi, track S): two interfaces, eth0 and wlan0, perhaps on one network
(10.0.0.0/24 on the laptop). ip_route() chooses by the source address when the caller
has one (a connection keeps the interface its address belongs to), then by the subnet
that holds the destination, then the first configured interface with a gateway; when
several would do, one with its link up, then Ethernet (ethN) before Wi-Fi, then the
order they registered. The laptop registers wlan0 at boot and its USB adapter later as
eth0, and traffic with no source address still takes eth0, as dhcp, netinfo and
dns_server() do (the review of track S, M1).
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <net/net.h>
#include <net/eth.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <blibc_common.h>

static u16 ip_id;

u16 ip_checksum(const void *data, u_int len) {
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    while(len > 1) { sum += (u32)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if(len) sum += (u32)(p[0] << 8);
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((u16)~sum);
}

u16 ip_checksum_pseudo(ipaddr_t src, ipaddr_t dst, u8 proto, const void *data, u_int len) {
    const u8 *p = (const u8 *)data, *s = (const u8 *)&src, *d = (const u8 *)&dst;
    u32 sum = 0;
    sum += (s[0] << 8) | s[1]; sum += (s[2] << 8) | s[3];
    sum += (d[0] << 8) | d[1]; sum += (d[2] << 8) | d[3];
    sum += proto;
    sum += len;
    while(len > 1) { sum += (u32)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if(len) sum += (u32)(p[0] << 8);
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((u16)~sum);
}

/* 10/9/26: of the configured interfaces whose subnet holds dst (subnet), or that have a gateway, the
   one the routes prefer: its link up before one without, then Ethernet (ethN) before Wi-Fi (wlanN),
   then the first to register; NULL when none would do */
static struct netdev *best(ipaddr_t dst, bool subnet) {
    struct netdev *d, *pick = NULL;
    int score, top = -1;
    for(d = netdev_first(); d; d = d->next) {
        if(!d->ip) continue;
        if(subnet ? !d->netmask || (dst & d->netmask) != (d->ip & d->netmask) : !d->gateway) continue;
        score = (d->link_up ? 2 : 0) + (strncmp(d->name, "eth", 3) ? 0 : 1);
        if(score > top) {
            top = score;
            pick = d;
        }
    }
    return pick;
}

/* 10/9/26: the interface for a datagram from src (0: none yet) to dst:
   1. the interface that has the address src: what a TCP connection made to eth0's address
      sends goes out through eth0 whatever wlan0 does (the integrator's telnet session to the
      laptop), and one made to wlan0's address through wlan0;
   2. the interface whose subnet holds dst (its broadcast too);
   3. the configured interface with a gateway;
   4. the default interface: one with no address yet (DHCP's broadcasts name theirs: udp_bind_dev()).
   In 2 and 3, of several: one with its link up, then Ethernet before Wi-Fi, then the first to register
   (best()): on the laptop, wlan0 registers at boot and the USB adapter later as eth0 */
struct netdev *ip_route(ipaddr_t src, ipaddr_t dst) {
    struct netdev *d;
    if(src) for(d = netdev_first(); d; d = d->next) if(d->ip == src) return d;
    d = best(dst, true);
    if(!d) d = best(dst, false);
    return d ? d : netdev_default();
}

struct netdev *ip_route_dev(ipaddr_t dst) {
    return ip_route(0, dst);
}

/* where a datagram to dst goes first: on our subnet (or a broadcast) directly, otherwise the gateway */
ipaddr_t ip_next_hop(struct netdev *dev, ipaddr_t dst) {
    if(dst != IP_BROADCAST && dev->netmask && (dst & dev->netmask) != (dev->ip & dev->netmask) && dev->gateway) return dev->gateway;
    return dst;
}

ipaddr_t ip_route_src(ipaddr_t dst) {
    struct netdev *dev = ip_route_dev(dst);
    return dev ? dev->ip : 0;
}

static bool for_us(struct netdev *dev, ipaddr_t dst) {
    if(dst == IP_BROADCAST) return true;
    if(!dev->ip) return true;                             /* unconfigured: DHCP may answer to the offered address */
    if(dst == dev->ip) return true;
    if(dev->netmask && (dst | dev->netmask) == IP_BROADCAST && (dst & dev->netmask) == (dev->ip & dev->netmask)) return true;
    return false;
}

void ip_input(struct netdev *dev, struct netbuf *nb) {
    struct ip_hdr *ip = (struct ip_hdr *)netbuf_data(nb);
    u_int hlen, total;
    if(nb->len < IP_HLEN || (ip->ver_ihl >> 4) != 4) { netbuf_free(nb); return; }
    hlen = (ip->ver_ihl & 15) * 4;
    total = ntohs(ip->len);
    if(hlen < IP_HLEN || total < hlen || total > nb->len) { netbuf_free(nb); return; }
    if(ip_checksum(ip, hlen) != 0) { netbuf_free(nb); return; }
    if(ntohs(ip->frag) & 0x3FFF) { netbuf_free(nb); return; }   /* a fragment: not supported */
    if(!for_us(dev, ip->dst)) { netbuf_free(nb); return; }
    nb->len = total;                                       /* drop Ethernet padding */
    netbuf_pull(nb, hlen);
    switch(ip->proto) {
    case IPPROTO_ICMP: icmp_input(dev, nb, ip); break;
    case IPPROTO_UDP:  udp_input(dev, nb, ip); break;
    case IPPROTO_TCP:  tcp_input(dev, nb, ip); break;
    default: netbuf_free(nb); break;
    }
}

int ip_output(struct netdev *dev, ipaddr_t dst, u8 proto, struct netbuf *nb) {
    struct ip_hdr *ip;
    ipaddr_t hop;
    u8 mac[ETH_ALEN];
    if(!dev) dev = ip_route_dev(dst);
    if(!dev) { netbuf_free(nb); return -1; }
    if(nb->len + IP_HLEN > dev->mtu) { netbuf_free(nb); return -1; }
    ip = (struct ip_hdr *)netbuf_push(nb, IP_HLEN);
    if(!ip) { netbuf_free(nb); return -1; }
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->len = htons((u16)nb->len);
    ip->id = htons(ip_id++);
    ip->frag = htons(0x4000);                              /* don't fragment */
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip->src = dev->ip;
    ip->dst = dst;
    ip->csum = ip_checksum(ip, IP_HLEN);
    hop = ip_next_hop(dev, dst);
    if(arp_resolve(dev, hop, mac, nb) != 0) return 0;      /* parked until the ARP reply arrives */
    return eth_output(dev, mac, ETH_P_IP, nb);
}
