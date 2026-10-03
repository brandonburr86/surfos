/*
SurfOS Network Stack: common definitions
----------------------
File: net.h     Date: 10/3/26 (roadmap N1/N2)
----------------------
Byte order helpers, addresses, packet buffers and the device interface.
All protocol processing runs in the "net" kernel thread under net_lock;
drivers hand frames in with netdev_rx() from their interrupt handler.
*/

#ifndef _NET_NET_H
#define _NET_NET_H

#include <surfos/types.h>
#include <surfos/sync.h>

/* IPv4 addresses are kept in network byte order */
typedef u32 ipaddr_t;

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v) { return (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24); }
static inline u32 ntohl(u32 v) { return htonl(v); }
#define IP4(a, b, c, d) htonl(((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))
#define IP_BROADCAST 0xFFFFFFFF

const char *ipfmt(ipaddr_t ip, char *buf);           /* "10.0.2.15"; buf holds 16 bytes */
const char *macfmt(const u8 *mac, char *buf);        /* "52:54:00:12:34:56"; 18 bytes */
bool ip_parse(const char *s, ipaddr_t *ip);

/**** packet buffers ****/

#define NETBUF_SIZE     1600
#define NETBUF_HEADROOM 64          /* room for Ethernet + IP + TCP headers in front of a payload */

struct netbuf {
    struct netbuf *next;
    u_int off, len;                 /* the frame is data[off .. off+len) */
    ipaddr_t aux_ip;                /* UDP/TCP: where a queued datagram or segment came from */
    u16 aux_port;
    u8 data[NETBUF_SIZE];
};

struct netbuf *netbuf_alloc(void);  /* off = NETBUF_HEADROOM, len = 0 */
void netbuf_free(struct netbuf *nb);
static inline u8 *netbuf_data(struct netbuf *nb) { return nb->data + nb->off; }
void *netbuf_push(struct netbuf *nb, u_int n);      /* prepend n bytes (a header), returns them */
void *netbuf_pull(struct netbuf *nb, u_int n);      /* drop n bytes from the front, returns the old front */
void *netbuf_put(struct netbuf *nb, u_int n);       /* append n bytes, returns them */

/**** devices ****/

#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_MTU 1500
#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806

struct netdev;

struct netdev_ops {
    int (*send)(struct netdev *dev, struct netbuf *nb);   /* the frame in nb; 0 ok. nb stays the caller's */
    int (*ioctl)(struct netdev *dev, int cmd, void *arg); /* driver specific, may be NULL */
};

struct netdev {
    char name[8];                   /* eth0 */
    u8 mac[ETH_ALEN];
    u_int mtu;
    const struct netdev_ops *ops;
    void *priv;
    bool link_up;
    /* configuration (network byte order) */
    ipaddr_t ip, netmask, gateway, dns;
    /* counters */
    u_long rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped, tx_errors;
    struct netdev *next;
};

int netdev_register(struct netdev *dev);              /* names it ethN */
struct netdev *netdev_find(const char *name);
struct netdev *netdev_default(void);                   /* the first one */
void netdev_rx(struct netdev *dev, const void *frame, u_int len);  /* from an ISR: queue for the net thread */
int netdev_send(struct netdev *dev, struct netbuf *nb);           /* a complete Ethernet frame */
void netdev_print(void);

/**** the stack ****/

extern mutex_t net_lock;            /* every protocol structure below Ethernet is touched under it */
void init_net(void);                /* the net thread; called once the drivers have registered */
void net_tick(void);                /* every 100 ms from the net thread, under net_lock */

#endif
