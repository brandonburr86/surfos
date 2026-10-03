/*
SurfOS Network Stack: packet buffers, addresses, device registry, the net thread
--------------------
File: netbuf.c  Date: 10/3/26 (roadmap N1)
--------------------
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/timer.h>
#include <mm/kalloc.h>
#include <net/net.h>
#include <net/eth.h>
#include <net/tcp.h>
#include <blibc_common.h>

/**** addresses ****/

const char *ipfmt(ipaddr_t ip, char *buf) {
    const u8 *p = (const u8 *)&ip;
    snprintf(buf, 16, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return buf;
}

const char *macfmt(const u8 *mac, char *buf) {
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

bool ip_parse(const char *s, ipaddr_t *ip) {
    u8 b[4];
    int i;
    for(i = 0; i < 4; i++) {
        char *end;
        u_long v = strtoul(s, &end, 10);
        if(end == s || v > 255) return false;
        if((i < 3 && *end != '.') || (i == 3 && *end != 0)) return false;
        b[i] = (u8)v;
        s = end + 1;
    }
    memcpy(ip, b, 4);
    return true;
}

/**** buffers ****/

struct netbuf *netbuf_alloc(void) {
    struct netbuf *nb = (struct netbuf *)kalloc(sizeof(struct netbuf));
    if(!nb) return NULL;
    nb->next = NULL;
    nb->off = NETBUF_HEADROOM;
    nb->len = 0;
    return nb;
}

void netbuf_free(struct netbuf *nb) {
    kfree(nb);
}

void *netbuf_push(struct netbuf *nb, u_int n) {
    if(n > nb->off) return NULL;
    nb->off -= n;
    nb->len += n;
    return nb->data + nb->off;
}

void *netbuf_pull(struct netbuf *nb, u_int n) {
    u8 *old = nb->data + nb->off;
    if(n > nb->len) return NULL;
    nb->off += n;
    nb->len -= n;
    return old;
}

void *netbuf_put(struct netbuf *nb, u_int n) {
    u8 *p = nb->data + nb->off + nb->len;
    if(nb->off + nb->len + n > NETBUF_SIZE) return NULL;
    nb->len += n;
    return p;
}

/**** devices ****/

static struct netdev *netdevs;
static u_int ndevs;

int netdev_register(struct netdev *dev) {
    struct netdev **pp;
    snprintf(dev->name, sizeof(dev->name), "eth%u", ndevs++);
    if(!dev->mtu) dev->mtu = ETH_MTU;
    dev->next = NULL;
    for(pp = &netdevs; *pp; pp = &(*pp)->next);
    *pp = dev;
    return 0;
}

struct netdev *netdev_find(const char *name) {
    struct netdev *d;
    for(d = netdevs; d; d = d->next) if(!strcmp(d->name, name)) return d;
    return NULL;
}

struct netdev *netdev_default(void) {
    return netdevs;
}

int netdev_send(struct netdev *dev, struct netbuf *nb) {
    int r;
    if(!dev || !dev->ops || !dev->ops->send) return -1;
    r = dev->ops->send(dev, nb);
    if(r == 0) { dev->tx_packets++; dev->tx_bytes += nb->len; }
    else dev->tx_errors++;
    return r;
}

void netdev_print(void) {
    struct netdev *d;
    char a[16], b[16], c[16], m[18], dn[16];
    if(!netdevs) { printf("    (no network devices)\n"); return; }
    for(d = netdevs; d; d = d->next) {
        printf("    %s: %s, link %s, mtu %u\n", d->name, macfmt(d->mac, m), d->link_up ? "up" : "down", d->mtu);
        printf("        inet %s  netmask %s  gateway %s  dns %s\n", ipfmt(d->ip, a), ipfmt(d->netmask, b), ipfmt(d->gateway, c), ipfmt(d->dns, dn));
        printf("        RX packets %lu bytes %lu dropped %lu   TX packets %lu bytes %lu errors %lu\n",
               d->rx_packets, d->rx_bytes, d->rx_dropped, d->tx_packets, d->tx_bytes, d->tx_errors);
    }
}

/**** receive queue and the net thread ****/

#define RXQ_SIZE 64
static struct { struct netdev *dev; struct netbuf *nb; } rxq[RXQ_SIZE];
static volatile u_int rxq_head, rxq_tail;
static event_t net_event;              /* set by netdev_rx(); the thread clears it before it looks */
mutex_t net_lock = MUTEX_INIT("net");

void netdev_rx(struct netdev *dev, const void *frame, u_int len) {
    struct netbuf *nb;
    u_long flags;
    u_int next;
    if(len > NETBUF_SIZE || len < ETH_HLEN) { dev->rx_dropped++; return; }
    nb = netbuf_alloc();
    if(!nb) { dev->rx_dropped++; return; }
    nb->off = 0;
    memcpy(nb->data, frame, len);
    nb->len = len;
    flags = irq_save();
    next = (rxq_head + 1) % RXQ_SIZE;
    if(next == rxq_tail) { irq_restore(flags); netbuf_free(nb); dev->rx_dropped++; return; }
    rxq[rxq_head].dev = dev;
    rxq[rxq_head].nb = nb;
    rxq_head = next;
    irq_restore(flags);
    dev->rx_packets++;
    dev->rx_bytes += len;
    event_set(&net_event);
}

static void net_thread(void *arg) {
    u_long last_tick = getticks();
    for(;;) {
        event_clear(&net_event);
        if(rxq_tail == rxq_head) event_wait_timeout(&net_event, 100);   /* 100 ms heartbeat for the timers */
        while(rxq_tail != rxq_head) {
            struct netdev *dev = rxq[rxq_tail].dev;
            struct netbuf *nb = rxq[rxq_tail].nb;
            rxq_tail = (rxq_tail + 1) % RXQ_SIZE;
            mutex_lock(&net_lock);
            eth_input(dev, nb);                              /* consumes nb */
            mutex_unlock(&net_lock);
        }
        if(getticks() - last_tick >= HZ / 10) {
            last_tick = getticks();
            mutex_lock(&net_lock);
            net_tick();
            mutex_unlock(&net_lock);
        }
    }
}

void init_net(void) {
    event_init(&net_event);
    kthread_create("net", net_thread, NULL, PL_HIGH, TF_KTHREAD | TF_DETACHED);
}

/* the 100 ms heartbeat: retransmissions, ARP retries and ageing */
void net_tick(void) {
    arp_tick();
    tcp_tick();
}
