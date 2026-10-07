/*
SurfOS network connections for programs
----------------------
File: sock.h    Date: 10/6/26
----------------------
TCP for user programs (milestone M11, the telnet server). A connection or a listening
socket is a file descriptor: read() and write() move data, close() ends it (a FIN; a
listening socket stops listening), dup2() and spawnfd() pass it on. Addresses are IPv4 in
network byte order, ports in host order. The calls (surfos/syscall.h):

SYS_listen  (u32 port): a descriptor listening on port (1-65535), on every interface.
            -EINVAL for port 0 or above 65535, -EADDRINUSE when another socket listens on
            the port (connections a server accepted there before do not count), -ENFILE
            without a free socket, -EMFILE without a free descriptor, -ENOMEM (no memory
            for the descriptor's file).
SYS_accept  (int fd, struct netaddr *peer): the next connection on a listening
            descriptor. It waits as long as it takes; a kill or Ctrl+C cuts the wait short
            (-EINTR). After poll() says POLLIN on the listener it does not wait. peer, when
            not NULL, gets the other end's address. -EBADF, -EINVAL when fd is no listening
            socket, -EFAULT for peer (checked before anything waits), -EMFILE, -ENOMEM. A
            connection reset after its handshake but before accept() is handed out all the
            same: read() then says -ECONNRESET.
SYS_connect (u32 ip, u32 port): a connection to ip:port, within 10 seconds: -ECONNREFUSED,
            -ETIMEDOUT, -EHOSTUNREACH (the next hop does not answer ARP), -ENETUNREACH (no
            interface, no address on it, no link, or ip off its network with no gateway),
            -EINTR, -EINVAL (ip 0, port 0 or above 65535), -ENFILE, -ENOMEM, -EMFILE. A
            connection the other end answered is handed out even when it has sent its data
            and closed, or reset it, at once: read() says so.
SYS_netinfo (struct netinfo *ni): the default network interface (the first): name, MAC
            address, link, address, netmask, gateway, DNS server (0 when not set);
            -ENODEV without an interface, -EFAULT.

read() on a connection waits for data as long as it takes (-EINTR when a kill or Ctrl+C
cuts the wait short) and returns what has arrived, at most len bytes; 0 once the other end
has closed and everything was read; -ECONNRESET after a reset; -ETIMEDOUT when the other
end stopped answering (data it never acknowledged, or keepalive: a connection silent for
two minutes is probed every 10 seconds and given up after 5 probes unanswered); -ENOTCONN
on a listening socket.
write() queues as many bytes as fit in the send buffer (8 KB) and returns how many: it
waits only while none fits (-EINTR when cut short), so after poll() says POLLOUT it does
not wait, and a caller with more to send writes the rest later (libsurf's stdio does).
-ECONNRESET or -ETIMEDOUT when that ended the connection (nothing else ends it while the
program has it: there is no shutdown()), -ENOTCONN on a listening socket. A reader that
stopped reading shuts its window: it is probed, and the bytes go on when it reads again.
What is written goes out at once, however small (no Nagle delay); only a window too small
for a sensible segment holds a small one back, a second at most (silly window avoidance).
close() never waits: a connection's FIN goes out behind the bytes still queued and the
socket finishes on its own; when the other end stops answering (its retries run out, or
30 seconds pass without progress) it is given up with a reset. A listening socket resets
the connections it had not accepted.
poll() (surfos/poll.h) says POLLIN when read() would not wait (data, the end, a reset or a
timeout; on a listening socket a connection to accept), POLLOUT when write() would queue a
byte at once, POLLHUP when the other end has closed or the connection is gone, POLLERR
when it ended with an error (a reset, a timeout).
There are 16 sockets, a listening one included; programs and the connections that come in
leave the last free one to the kernel's own (a command like httpget).

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
