/*
SurfOS Network Stack: bounded DNS packet decoding
--------------------
File: dns_wire.h Date: 10/10/26 (Undertow)
--------------------
*/
#ifndef _NET_DNS_WIRE_H
#define _NET_DNS_WIRE_H
#include <net/net.h>
#define DNS_NAME_MAX 253
/* 0: matching A answer, 1: unrelated packet, negative errno: applicable failure.
   The output is assigned only on success. No allocation, I/O or shared state. */
int dns_parse_a(const u8 *packet, u_int len, u16 id, const char *name, ipaddr_t *out);
/* Hostname syntax accepted by both the query encoder and syscall. */
int dns_name_valid(const char *name);
#endif
