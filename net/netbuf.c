/*
SurfOS Network Stack: packet buffers, addresses, device registry, the net thread
--------------------
File: netbuf.c  Date: 10/3/26 (roadmap N1)
--------------------
10/9/26 (Wi-Fi, track S): a name the driver set (wlan0, from the station layer) is kept; the
others are eth0, eth1, ... counted apart, so Wi-Fi found at boot leaves eth0 to the first
Ethernet interface. The default interface (commands that name none) is the first Ethernet one.
10/9/26 (the review of track S, L1): the receive queue holds 128 frames, of which one device
may hold 64 (the whole queue before): wlan0 handing over an A-MSDU's MSDUs one after the
other cannot crowd out eth0's frames (a telnet session). The station layer's thread waits for
room (netdev_rx_wait(), a second at most) instead of losing the frame. 10/10/26: only it: a
driver's netdev_rx() never waits, as before (a frame with no room is dropped), since drivers
call it from an interrupt handler or, as usbnet's completions, with interrupts off in the
middle of the xHCI event ring, where waiting would let the timer's poll of that ring in.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/timer.h>
#include <surfos/interrupt.h>
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
static u_int neth;                      /* 10/9/26: ethN names given (not a driver's own, such as wlan0) */

/* 10/10/26 (review nit): a name another device has gets the lowest free number after its letters
   instead (wlan0 taken: wlan1), so no two devices share one */
static void unique_name(struct netdev *dev) {
    size_t n = strlen(dev->name);
    u_int i;
    while(n && dev->name[n - 1] >= '0' && dev->name[n - 1] <= '9') n--;
    if(n > sizeof(dev->name) - 4) n = sizeof(dev->name) - 4;   /* room for three digits */
    for(i = 0; i < 1000 && netdev_find(dev->name); i++) snprintf(dev->name + n, sizeof(dev->name) - n, "%u", i);
}

/* ethN names are given here alone, in the order the devices come: a driver's own name is kept
   (wlan0) unless it begins with "eth", so Ethernet is ethN whatever registers it */
int netdev_register(struct netdev *dev) {
    struct netdev **pp;
    if(!dev->name[0] || !strncmp(dev->name, "eth", 3)) snprintf(dev->name, sizeof(dev->name), "eth%u", neth++);
    unique_name(dev);
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

/* 10/9/26: the first Ethernet interface, else the first: on the laptop the Wi-Fi card registers
   wlan0 at boot and the USB adapter eth0 later, and `dhcp` or a program's netinfo still mean eth0 */
struct netdev *netdev_default(void) {
    struct netdev *d;
    for(d = netdevs; d; d = d->next) if(!strncmp(d->name, "eth", 3)) return d;
    return netdevs;
}

struct netdev *netdev_first(void) {
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

#define RXQ_SIZE 128                   /* 10/9/26: 64 before */
#define RXQ_SHARE 64                   /* 10/9/26: the most one device may hold */
#define RXQ_WAIT_MS 1000               /* 10/9/26: the longest a task waits for room */
static struct { struct netdev *dev; struct netbuf *nb; } rxq[RXQ_SIZE];
static volatile u_int rxq_head, rxq_tail;
static event_t net_event;              /* set by netdev_rx(); the thread clears it before it looks */
static event_t rxq_room;               /* 10/9/26: set by the net thread as it takes frames */
static surf_task *net_task;            /* 10/9/26: the net thread, which never waits for room it makes itself */
mutex_t net_lock = MUTEX_INIT("net");

static void rx_queue(struct netdev *dev, const void *frame, u_int len, bool may_wait) {
    struct netbuf *nb;
    u_long flags;
    u_int next, waited = 0;
    if(len > NETBUF_SIZE || len < ETH_HLEN) { dev->rx_dropped++; return; }
    nb = netbuf_alloc();
    if(!nb) { dev->rx_dropped++; return; }
    nb->off = 0;
    memcpy(nb->data, frame, len);
    nb->len = len;
    for(;;) {
        flags = irq_save();
        next = (rxq_head + 1) % RXQ_SIZE;
        if(next != rxq_tail && dev->rx_inq < RXQ_SHARE) break;   /* room, and this device under its share */
        irq_restore(flags);
        /* 10/9/26: a caller that may wait does, for the net thread to take some, a second at most; the frame
           goes otherwise. 10/10/26: never with interrupts off, in a handler, or in the net thread itself */
        if(!may_wait || in_interrupt() || !irqs_enabled() || !curTask || curTask == net_task || waited >= RXQ_WAIT_MS) {
            netbuf_free(nb);
            dev->rx_dropped++;
            return;
        }
        event_clear(&rxq_room);
        event_set(&net_event);
        event_wait_timeout(&rxq_room, 10);
        waited += 10;
    }
    rxq[rxq_head].dev = dev;
    rxq[rxq_head].nb = nb;
    rxq_head = next;
    dev->rx_inq++;
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
            u_long flags = irq_save();
            rxq_tail = (rxq_tail + 1) % RXQ_SIZE;
            dev->rx_inq--;
            irq_restore(flags);
            event_set(&rxq_room);                            /* 10/9/26: a task may be waiting for it */
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
    event_init(&rxq_room);
    net_task = kthread_create("net", net_thread, NULL, PL_HIGH, TF_KTHREAD | TF_DETACHED);
}

/* 10/10/26: a driver's frames: never waiting (an interrupt handler, a completion with interrupts off) */
void netdev_rx(struct netdev *dev, const void *frame, u_int len) {
    rx_queue(dev, frame, len, false);
}

/* 10/10/26: a thread's, with interrupts on and no lock the net thread needs (the station layer's): it
   waits for room, a second at most, rather than lose the frame */
void netdev_rx_wait(struct netdev *dev, const void *frame, u_int len) {
    rx_queue(dev, frame, len, true);
}

/* the 100 ms heartbeat: retransmissions, ARP retries and ageing */
void net_tick(void) {
    arp_tick();
    tcp_tick();
}
