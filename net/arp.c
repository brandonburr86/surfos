/*
SurfOS Network Stack: ARP
--------------------
File: arp.c     Date: 10/3/26 (roadmap N2)
--------------------
A 16-entry table. An unresolved entry parks one frame and re-sends the
request from arp_tick() a few times before giving up; entries age out
after ten minutes.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/timer.h>
#include <net/net.h>
#include <net/eth.h>
#include <blibc_common.h>

struct arp_pkt {
    u16 htype, ptype;
    u8 hlen, plen;
    u16 op;
    u8 sha[ETH_ALEN];
    ipaddr_t spa;
    u8 tha[ETH_ALEN];
    ipaddr_t tpa;
} __attribute__((packed));

#define ARP_REQUEST 1
#define ARP_REPLY   2
#define ARP_MAX_AGE (600 * HZ)
#define ARP_RETRIES 3

static struct arp_entry table[ARP_TABLE_SIZE];

const struct arp_entry *arp_table(void) {
    return table;
}

static struct arp_entry *find(ipaddr_t ip) {
    int i;
    for(i = 0; i < ARP_TABLE_SIZE; i++) if(table[i].valid && table[i].ip == ip) return &table[i];
    return NULL;
}

static struct arp_entry *alloc(ipaddr_t ip) {
    struct arp_entry *e = find(ip), *oldest = &table[0];
    int i;
    if(e) return e;
    for(i = 0; i < ARP_TABLE_SIZE; i++) {
        if(!table[i].valid) { oldest = &table[i]; break; }
        if(table[i].updated < oldest->updated) oldest = &table[i];
    }
    if(oldest->pending) netbuf_free(oldest->pending);
    memset(oldest, 0, sizeof(*oldest));
    oldest->ip = ip;
    oldest->valid = true;
    oldest->updated = getticks();
    return oldest;
}

static int send_arp(struct netdev *dev, u16 op, const u8 *dst_mac, const u8 *tha, ipaddr_t tpa) {
    struct netbuf *nb = netbuf_alloc();
    struct arp_pkt *a;
    if(!nb) return -1;
    a = (struct arp_pkt *)netbuf_put(nb, sizeof(*a));
    a->htype = htons(1);
    a->ptype = htons(ETH_P_IP);
    a->hlen = ETH_ALEN;
    a->plen = 4;
    a->op = htons(op);
    memcpy(a->sha, dev->mac, ETH_ALEN);
    a->spa = dev->ip;
    memcpy(a->tha, tha, ETH_ALEN);
    a->tpa = tpa;
    return eth_output(dev, dst_mac, ETH_P_ARP, nb);
}

static void request(struct netdev *dev, ipaddr_t ip) {
    static const u8 zero[ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };
    send_arp(dev, ARP_REQUEST, eth_broadcast, zero, ip);
}

void arp_announce(struct netdev *dev) {
    if(dev->ip) send_arp(dev, ARP_REQUEST, eth_broadcast, eth_broadcast, dev->ip);
}

static void learn(struct netdev *dev, ipaddr_t ip, const u8 *mac) {
    struct arp_entry *e = alloc(ip);
    memcpy(e->mac, mac, ETH_ALEN);
    e->resolved = true;
    e->updated = getticks();
    if(e->pending) {                                     /* the frame that was waiting for this */
        struct netbuf *nb = e->pending;
        e->pending = NULL;
        eth_output(dev, mac, ETH_P_IP, nb);
    }
}

void arp_input(struct netdev *dev, struct netbuf *nb) {
    struct arp_pkt *a = (struct arp_pkt *)netbuf_data(nb);
    if(nb->len < sizeof(*a) || ntohs(a->htype) != 1 || ntohs(a->ptype) != ETH_P_IP || a->hlen != ETH_ALEN || a->plen != 4) {
        netbuf_free(nb);
        return;
    }
    if(a->spa && (find(a->spa) || a->tpa == dev->ip)) learn(dev, a->spa, a->sha);   /* refresh, or learn who asks us */
    if(ntohs(a->op) == ARP_REQUEST && dev->ip && a->tpa == dev->ip) {
        send_arp(dev, ARP_REPLY, a->sha, a->sha, a->spa);
    }
    netbuf_free(nb);
}

int arp_resolve(struct netdev *dev, ipaddr_t ip, u8 *mac, struct netbuf *park) {
    struct arp_entry *e;
    /* 0: the caller keeps park and sends it; 1: park now belongs to the table */
    if(ip == IP_BROADCAST || (dev->netmask && (ip | dev->netmask) == IP_BROADCAST)) {
        memcpy(mac, eth_broadcast, ETH_ALEN);
        return 0;
    }
    e = find(ip);
    if(e && e->resolved) {
        memcpy(mac, e->mac, ETH_ALEN);
        return 0;
    }
    if(!e) {
        e = alloc(ip);
        e->retries = 0;
        request(dev, ip);
    }
    if(park) {
        if(e->pending) netbuf_free(e->pending);
        e->pending = park;
    }
    return 1;
}

void arp_tick(void) {
    u_long now = getticks();
    struct netdev *dev = netdev_default();
    int i;
    for(i = 0; i < ARP_TABLE_SIZE; i++) {
        struct arp_entry *e = &table[i];
        if(!e->valid) continue;
        if(e->resolved) {
            if(now - e->updated > ARP_MAX_AGE) e->valid = false;
        } else if(now - e->updated >= HZ) {                /* no reply within a second: ask again */
            if(++e->retries > ARP_RETRIES) {
                if(e->pending) netbuf_free(e->pending);
                e->valid = false;
            } else if(dev) {
                e->updated = now;
                request(dev, e->ip);
            }
        }
    }
}

void arp_print(void) {
    char a[16], m[18];
    int i, n = 0;
    printf("    ADDRESS          HWADDRESS          AGE\n");
    for(i = 0; i < ARP_TABLE_SIZE; i++) {
        if(!table[i].valid) continue;
        printf("    %-16s %-18s %lus%s\n", ipfmt(table[i].ip, a), table[i].resolved ? macfmt(table[i].mac, m) : "(incomplete)",
               (getticks() - table[i].updated) / HZ, table[i].pending ? " (frame waiting)" : "");
        n++;
    }
    if(!n) printf("    (empty)\n");
}
