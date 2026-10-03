/*
SurfOS Network Stack: IPv4, ICMP, UDP
----------------------
File: ip.h      Date: 10/3/26 (roadmap N2)
----------------------
*/

#ifndef _NET_IP_H
#define _NET_IP_H

#include <net/net.h>

struct ip_hdr {
    u8 ver_ihl, tos;
    u16 len, id, frag;
    u8 ttl, proto;
    u16 csum;
    ipaddr_t src, dst;
} __attribute__((packed));

#define IP_HLEN 20
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

u16 ip_checksum(const void *data, u_int len);                  /* ones' complement sum, folded */
u16 ip_checksum_pseudo(ipaddr_t src, ipaddr_t dst, u8 proto, const void *data, u_int len); /* TCP/UDP */
void ip_input(struct netdev *dev, struct netbuf *nb);         /* after the Ethernet header */
/* send nb (payload only) to dst: adds the IP header, resolves the next hop, hands to Ethernet; frees nb */
int ip_output(struct netdev *dev, ipaddr_t dst, u8 proto, struct netbuf *nb);
ipaddr_t ip_route_src(ipaddr_t dst);                          /* our address to use for dst */
struct netdev *ip_route_dev(ipaddr_t dst);

/* ICMP */
struct icmp_hdr {
    u8 type, code;
    u16 csum;
    u16 id, seq;
} __attribute__((packed));
#define ICMP_ECHO_REPLY   0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO         8

void icmp_input(struct netdev *dev, struct netbuf *nb, const struct ip_hdr *ip);
/* send one echo request and wait up to timeout_ms for the reply; returns the round trip in ms, or -1 */
int icmp_ping(ipaddr_t dst, u16 id, u16 seq, u_int payload, u_long timeout_ms, u8 *ttl_out);

/* UDP */
struct udp_hdr {
    u16 sport, dport, len, csum;
} __attribute__((packed));

struct udp_socket;
struct udp_socket *udp_open(u16 local_port);                  /* 0: any free port above 1024 */
void udp_close(struct udp_socket *s);
int udp_sendto(struct udp_socket *s, ipaddr_t dst, u16 dport, const void *data, u_int len);
/* a datagram, or -1 after timeout_ms; src/sport tell where it came from */
int udp_recvfrom(struct udp_socket *s, void *buf, u_int size, ipaddr_t *src, u16 *sport, u_long timeout_ms);
u16 udp_local_port(struct udp_socket *s);
void udp_input(struct netdev *dev, struct netbuf *nb, const struct ip_hdr *ip);
void udp_print(void);

#endif
