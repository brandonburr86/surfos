/*
SurfOS Network Stack: Ethernet and ARP
----------------------
File: eth.h     Date: 10/3/26 (roadmap N2)
----------------------
*/

#ifndef _NET_ETH_H
#define _NET_ETH_H

#include <net/net.h>

struct eth_hdr {
    u8 dst[ETH_ALEN], src[ETH_ALEN];
    u16 type;                        /* network order */
} __attribute__((packed));

extern const u8 eth_broadcast[ETH_ALEN];

void eth_input(struct netdev *dev, struct netbuf *nb);                         /* consumes nb */
int eth_output(struct netdev *dev, const u8 *dst, u16 type, struct netbuf *nb); /* prepends the header, sends, frees nb */

/* ARP */
#define ARP_TABLE_SIZE 16
struct arp_entry {
    ipaddr_t ip;
    u8 mac[ETH_ALEN];
    bool valid, resolved;
    u_long updated;                  /* tick */
    struct netbuf *pending;          /* one frame waiting for the reply */
    u_int retries;
};

void arp_input(struct netdev *dev, struct netbuf *nb);
/* look ip up. 0: mac filled in, the caller still owns park. 1: unknown; a request went out and park
   (if given) now belongs to the table, to be sent when the reply arrives. */
int arp_resolve(struct netdev *dev, ipaddr_t ip, u8 *mac, struct netbuf *park);
void arp_announce(struct netdev *dev);                                         /* gratuitous ARP for our address */
void arp_tick(void);
void arp_print(void);
const struct arp_entry *arp_table(void);

#endif
