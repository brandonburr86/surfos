/*
SurfOS network connections for programs
----------------------
File: sock.h    Date: 10/6/26
----------------------
TCP for user programs (milestone M11, the telnet server). A connection or a listening
socket is a file descriptor: read() and write() move data, close() ends it (a FIN; a
listening socket stops listening), dup2() and spawnfd() pass it on. Addresses are IPv4 in
network byte order, ports in host order. The calls (surfos/syscall.h):

SYS_listen  (u16 port): a descriptor listening on port, on every interface. -EADDRINUSE
            when a socket has the port already, -EMFILE without a free descriptor,
            -ENFILE without a free socket.
SYS_accept  (int fd, struct netaddr *peer): the next connection on a listening
            descriptor. It waits; a kill or Ctrl+C cuts the wait short (-EINTR). peer,
            when not NULL, gets the other end's address.
SYS_connect (u32 ip, u16 port): a connection to ip:port, within 10 seconds:
            -ECONNREFUSED, -ETIMEDOUT, -EHOSTUNREACH, -ENETUNREACH (no interface).
SYS_netinfo (struct netinfo *ni): the default network interface (the first): name, MAC
            address, link, address, netmask, gateway, DNS server (0 when not set);
            -ENODEV without an interface.

read() on a connection waits for data and returns what has arrived (at most len bytes),
0 once the other end has closed and everything was read, -ECONNRESET after a reset.
write() queues as many bytes as fit in the send buffer (8 KB) and returns how many: it
waits only while none fits (-EINTR when cut short), so after poll() says POLLOUT it does
not wait, and a caller with more to send writes the rest later (libsurf's stdio does).
-ECONNRESET or -ETIMEDOUT when that ended the connection, -EPIPE once it is closed both
ways, -ENOTCONN on a listening socket. A reader that stopped reading shuts its window: it
is probed, and the bytes go on when it reads again.
poll() (surfos/poll.h) says POLLIN when read() would not wait (data, the end, or on a
listening socket a connection to accept), POLLOUT when write() would queue a byte at once,
POLLHUP when the other end has closed.

This header uses no SurfOS types: user programs reach it through user/include/surfos/sock.h.
*/

#ifndef _SURFOS_SOCK_H
#define _SURFOS_SOCK_H

struct netaddr {
    unsigned int ip;                    /* network byte order */
    unsigned short port;                /* host order */
    unsigned short zero;
};

struct netinfo {
    char name[8];                       /* "eth0" */
    unsigned char mac[6];
    unsigned char link;                 /* 1: the link is up */
    unsigned char configured;           /* 1: it has an address (dhcp, or ifconfig by hand) */
    unsigned int ip, netmask, gateway, dns;   /* network byte order; 0 when not set */
};

#endif
