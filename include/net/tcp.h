/*
SurfOS Network Stack: TCP
----------------------
File: tcp.h     Date: 10/3/26 (roadmap N2)
----------------------
10/6/26 (the telnet server): calls that answer -errno and may wait without a time
limit, for the connections programs have (net/sock.c, surfos/sock.h). A wait that a
kill or Ctrl+C cuts short gives up: -EINTR (surfos/task.h).
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
void tcp_unreachable(struct netdev *dev, ipaddr_t hop);   /* 10/6/26: ARP gave up on hop (net/arp.c), under net_lock;
                                                             10/9/26: on the interface dev */

/* the kernel socket API: every call may block; a negative return is -errno */
struct tcp_socket *tcp_connect(ipaddr_t dst, u16 dport, u_long timeout_ms);
struct tcp_socket *tcp_listen(u16 port);                       /* a listening socket */
struct tcp_socket *tcp_accept(struct tcp_socket *l, u_long timeout_ms);  /* the next connection, or NULL */
int tcp_send(struct tcp_socket *s, const void *data, u_int len);        /* bytes queued (all or error) */
int tcp_recv(struct tcp_socket *s, void *buf, u_int size, u_long timeout_ms); /* bytes; 0 = peer closed; -1 timeout */
int tcp_close(struct tcp_socket *s);                           /* FIN, then release */
ipaddr_t tcp_peer(struct tcp_socket *s, u16 *port);

/* 10/6/26: the same with the reason they fail, -errno (surfos/sock.h lists them); TCP_NO_LIMIT
   waits until it happens or the wait is cut short (-EINTR) */
#define TCP_NO_LIMIT ((u_long)-1)
int tcp_open(ipaddr_t dst, u16 dport, u_long timeout_ms, struct tcp_socket **out);
int tcp_listen_on(u16 port, struct tcp_socket **out);
int tcp_accept_next(struct tcp_socket *l, u_long timeout_ms, struct tcp_socket **out);
int tcp_read(struct tcp_socket *s, void *buf, u_int len);     /* read(): bytes, 0 at the end; waits */
int tcp_write(struct tcp_socket *s, const void *data, u_int len); /* write(): what fits now (waits while nothing does) */
void tcp_release(struct tcp_socket *s);    /* the last descriptor closed: FIN, or stop listening; never waits */
int tcp_poll(struct tcp_socket *s);        /* POLLIN POLLOUT POLLHUP POLLERR (surfos/poll.h); takes net_lock */
bool tcp_listening(const struct tcp_socket *s);
void tcp_set_nodelay(struct tcp_socket *s);     /* 10/6/26: small writes go at once (no Nagle): a program's connection */

#endif
