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

/* an A record through the configured server (dotted quads pass straight through); 0 or -1 */
int dns_resolve(const char *name, ipaddr_t *out, u_long timeout_ms);

#endif
