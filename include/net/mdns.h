/*
SurfOS Network Stack: multicast DNS, a name on the local network
--------------------
File: mdns.h     Date: 10/25/26 (the network tools)
--------------------
The responder (net/mdns.c) answers for <name>.local (RFC 6762) on 224.0.0.251 port 5353, so
another computer on the network reaches SurfOS by name: ftp://surfos.local/, telnet
surfos.local, http://surfos.local/. `mdns on [name]` starts it (surfos by default), `mdns
off` stops it, `mdns` says what it answers for and where; it never starts by itself. It
probes for the name first and announces it on every interface with an address, again
when an address comes or changes; a conflict (another host answering for the name) makes
it take name-2, name-3 and so on, and say so, and another host probing for the name at the
same moment is a tie that the later records win. A, for IPv4, is the only record (SurfOS
has no IPv6), and an NSEC record says so to whoever asks for another type.

mdns_wire.c is the messages, pure (host-tested); mdns.c the thread, its socket and the
interfaces.
*/

#ifndef _NET_MDNS_H
#define _NET_MDNS_H

#include <net/net.h>

#define MDNS_PORT     5353
#define MDNS_NAME_MAX 63                /* the name, a label of its own: .local follows */
#define MDNS_FULL_MAX 253               /* a name in a message */
#define MDNS_MSG_MAX  512               /* what a message of ours takes at most */
#define MDNS_TTL      120               /* our records' TTL, seconds (a one-shot querier's answer: 10) */
#define MDNS_GROUP    0xFB0000E0u       /* 224.0.0.251, in network order */

/* What the message p (len bytes, from port sport) asks of name.local: the response to it in out (cap bytes), its
   length; 0 when nothing in it asks for our name (or it is a response, or it knows our records already, or has an
   opcode or a response code other than 0), -1 for a broken message. A and ANY get our A record, with an NSEC
   record saying it is the only one; another type (AAAA) gets the NSEC record alone. *unicast: answer the querier
   alone (it asked so, QU, or it is a one-shot querier, a port other than 5353: its ID and questions back, and no
   NSEC). addr is in network order */
int mdns_respond(const u8 *p, u_int len, u16 sport, const char *name, ipaddr_t addr, u8 *out, u_int cap,
                 int *unicast);

/* an announcement of name.local -> addr with the TTL ttl (120; 0 says goodbye), and a probe for it: their lengths
   in out (0: no room) */
int mdns_announce(const char *name, ipaddr_t addr, u32 ttl, u8 *out, u_int cap);
int mdns_probe(const char *name, ipaddr_t addr, u8 *out, u_int cap);

/* 1 when the response p claims name.local for something other than addr (another host has the name) */
int mdns_conflict(const u8 *p, u_int len, const char *name, ipaddr_t addr);

/* Whether the query p is another host's probe for name.local (our name in its authority section), and who wins the
   tie between its records and ours, an A record for addr (RFC 6762 section 8.2: by class, type, then data, the
   later wins): 1 its (we wait a second and probe again), -1 ours, or the same records; 0 when it is no probe for
   the name, or it is broken */
int mdns_tiebreak(const u8 *p, u_int len, const char *name, ipaddr_t addr);

/* a name we may answer for: 1 to 63 letters, digits and hyphens, not starting or ending with one */
int mdns_name_valid(const char *name);

/* the responder (mdns.c): 0, or -EBUSY (running), -EINVAL (no such name), -EADDRINUSE (port 5353 taken, or no
   UDP socket free), -ENOMEM. mdns_stop(): 0 when it has stopped, -EBUSY when it has not yet (it does soon) */
int mdns_start(const char *name);
int mdns_stop(void);
int mdns_running(char *name, u_int size);      /* 1 and the name answered for now, or 0 */
void mdns_status(char *out, u_int size);       /* the interfaces it answers on, as a line */

#endif
