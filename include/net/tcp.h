/*
SurfOS Network Stack: TCP
----------------------
File: tcp.h     Date: 10/3/26 (roadmap N2)
----------------------
10/6/26 (the telnet server): tcp_open() and tcp_listen_on() answer -errno: why a
connection or a listening socket could not be had.
*/

#ifndef _NET_TCP_H
#define _NET_TCP_H

#include <net/net.h>
#include <net/ip.h>

struct tcp_hdr {
    u16 sport, dport;
    u32 seq, ack;
    u8 off, flags;
    u16 win, csum, urg;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

struct tcp_socket;

void tcp_input(struct netdev *dev, struct netbuf *nb, const struct ip_hdr *ip);
void tcp_tick(void);
void tcp_print(void);
void tcp_unreachable(ipaddr_t hop);        /* 10/6/26: ARP gave up on hop (net/arp.c), under net_lock */

/* the kernel socket API: every call may block; a negative return is -errno */
struct tcp_socket *tcp_connect(ipaddr_t dst, u16 dport, u_long timeout_ms);
struct tcp_socket *tcp_listen(u16 port);                       /* a listening socket */
struct tcp_socket *tcp_accept(struct tcp_socket *l, u_long timeout_ms);  /* the next connection, or NULL */
int tcp_send(struct tcp_socket *s, const void *data, u_int len);        /* bytes queued (all or error) */
int tcp_recv(struct tcp_socket *s, void *buf, u_int size, u_long timeout_ms); /* bytes; 0 = peer closed; -1 timeout */
int tcp_close(struct tcp_socket *s);                           /* FIN, then release */
ipaddr_t tcp_peer(struct tcp_socket *s, u16 *port);

/* 10/6/26: the same with the reason they fail, -errno (surfos/sock.h lists them) */
int tcp_open(ipaddr_t dst, u16 dport, u_long timeout_ms, struct tcp_socket **out);
int tcp_listen_on(u16 port, struct tcp_socket **out);

#endif
