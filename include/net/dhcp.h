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
/* 10/17/26: the same, with quiet saying a failure ("no offer") in the kernel log only: the network manager asking
   again and again for an address that does not come */
int dhcp_configure_quiet(struct netdev *dev, u_long timeout_ms, bool quiet);
u_long dhcp_lease_seconds(void);          /* of the last lease, 0 if none */

/* 10/17/26 (the network manager): keep the lease dev has (dev->lease_s): a DHCPREQUEST for its address, to the
   server that granted it (DHCP_RENEWING), to anyone (DHCP_REBINDING, once the renewals got no answer), or, when a
   link came back, asking for the address again (DHCP_REBOOTING). lease_at: the lease meant (dev->lease_at when the
   caller looked; 0: the one there is); nothing is done when dev has another by then, or an address set by hand.
   0: renewed (dev->lease_at, lease_s new), -1: no answer or nothing to renew, -2: refused (NAK): the address is
   given up (dhcp_drop()) */
#define DHCP_RENEWING  0
#define DHCP_REBINDING 1
#define DHCP_REBOOTING 2
int dhcp_renew(struct netdev *dev, int how, u_long timeout_ms, u_long lease_at);
/* the address dev's lease gave is given up (it expired): address, netmask, gateway, DNS and lease; only that lease's
   (lease_at, 0: whatever dev has) */
void dhcp_drop(struct netdev *dev, const char *why, u_long lease_at);
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
