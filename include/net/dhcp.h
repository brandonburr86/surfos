/*
SurfOS Network Stack: DHCP client and DNS resolver
----------------------
File: dhcp.h    Date: 10/3/26 (roadmap N2)
----------------------
*/

#ifndef _NET_DHCP_H
#define _NET_DHCP_H

#include <net/net.h>

/* obtain and apply a lease (address, netmask, gateway, DNS); 0 or -1 */
int dhcp_configure(struct netdev *dev, u_long timeout_ms);
u_long dhcp_lease_seconds(void);          /* of the last lease, 0 if none */

/* 10/17/26 (the network manager): keep the lease dev has (dev->lease_s): a DHCPREQUEST for its address, to the
   server that granted it (renewing), or to anyone (broadcast: rebinding, once the renewals got no answer). 0: renewed
   (dev->lease_at, lease_s new), -1: no answer, -2: refused (NAK: the address must go; dhcp_drop()) */
int dhcp_renew(struct netdev *dev, bool broadcast, u_long timeout_ms);
/* the address dev's lease gave is given up (it expired, or was refused): address, netmask, gateway, DNS and lease */
void dhcp_drop(struct netdev *dev, const char *why);
/* the name requests carry (option 12), so the router can list SurfOS by it: letters, digits and '-', 1-32
   characters; NULL or "" sends none */
void dhcp_set_hostname(const char *name);

/* Bounded A lookup: 0 or negative errno; output changes only on success. */
int dns_lookup(const char *name, ipaddr_t *out, u_long timeout_ms);

/* an A record through the configured server (dotted quads pass straight through); 0 or -1 */
int dns_resolve(const char *name, ipaddr_t *out, u_long timeout_ms);
/* 10/9/26: the server dns_resolve() asks: the default interface's, else the first configured one's
   (a link up first); 0 when none is known */
ipaddr_t dns_server(void);

#endif
