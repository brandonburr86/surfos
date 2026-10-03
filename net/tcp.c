/*
SurfOS Network Stack: TCP (placeholder until the real one lands)
--------------------
File: tcp.c     Date: 10/3/26 (roadmap N2)
--------------------
*/

#include <surfos/types.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <blibc_common.h>

void tcp_input(struct netdev *dev, struct netbuf *nb, const struct ip_hdr *ip) {
    netbuf_free(nb);
}

void tcp_tick(void) {
}

void tcp_print(void) {
}
