/*
SurfOS Network Stack: IPv4
--------------------
File: ip.c      Date: 10/3/26 (roadmap N2)
--------------------
No fragments, no options on output, one interface.
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

struct netdev *ip_route_dev(ipaddr_t dst) {
    return netdev_default();
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
    ipaddr_t hop = dst;
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
    /* next hop: on our subnet (or a broadcast) directly, otherwise the gateway */
    if(dst != IP_BROADCAST && dev->netmask && (dst & dev->netmask) != (dev->ip & dev->netmask) && dev->gateway) hop = dev->gateway;
    if(arp_resolve(dev, hop, mac, nb) != 0) return 0;      /* parked until the ARP reply arrives */
    return eth_output(dev, mac, ETH_P_IP, nb);
}
