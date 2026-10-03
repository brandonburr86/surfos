/*
SurfOS Network Stack: Ethernet
--------------------
File: eth.c     Date: 10/3/26 (roadmap N2)
--------------------
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <net/net.h>
#include <net/eth.h>
#include <net/ip.h>
#include <blibc_common.h>

const u8 eth_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void eth_input(struct netdev *dev, struct netbuf *nb) {
    struct eth_hdr *eh;
    u16 type;
    if(nb->len < ETH_HLEN) { netbuf_free(nb); return; }
    eh = (struct eth_hdr *)netbuf_data(nb);
    type = ntohs(eh->type);
    netbuf_pull(nb, ETH_HLEN);
    switch(type) {
    case ETH_P_ARP: arp_input(dev, nb); break;
    case ETH_P_IP:  ip_input(dev, nb); break;
    default: netbuf_free(nb); break;
    }
}

int eth_output(struct netdev *dev, const u8 *dst, u16 type, struct netbuf *nb) {
    struct eth_hdr *eh = (struct eth_hdr *)netbuf_push(nb, ETH_HLEN);
    int r;
    if(!eh) { netbuf_free(nb); return -1; }
    memcpy(eh->dst, dst, ETH_ALEN);
    memcpy(eh->src, dev->mac, ETH_ALEN);
    eh->type = htons(type);
    while(nb->len < 60) *(u8 *)netbuf_put(nb, 1) = 0;    /* minimum frame size */
    r = netdev_send(dev, nb);
    netbuf_free(nb);
    return r;
}
