/*
SurfOS user program: nettest
--------------------
File: nettest.c   Date: 10/6/26
--------------------
Network connections for programs (surfos/sock.h) from user space.

nettest echo <port> [n]: listens on port, takes n connections one after another (1
when not given; 0 only listens and stops), sends back whatever each one sends until
it closes, then says how many bytes that was. Exit 0, or 1 when a call fails.

nettest send <port> <bytes>: listens on port, takes one connection, sends it <bytes>
bytes (the pattern "nettest 0000000\n", 16 bytes each), closes it and says so; the
other end reads them at its own pace.

nettest get <ip> <port> <path>: an HTTP/1.0 GET of path from ip:port, the response
printed as it comes, then the number of bytes. Exit 0 when something came back.

nettest info: the default network interface (netinfo). Exit 1 without one.

nettest connect <ip> <port>: a connection, closed at once, or why there is none.
nettest refuse <ip> <port>: a connection to a closed port: exit 0 when it is refused
(ECONNREFUSED), 1 otherwise.

For the tests with hand-made segments (tools/qemu-tcp.py), each a line "nettest: ..." at
the end saying how it went:
nettest reader <port>: one connection, read until it ends.
nettest writer <port> <bytes> [chunk]: one connection gets <bytes> bytes of the pattern in
writes of chunk bytes (4096), each written again from where a partial one stopped; how
many writes there were, and how many partial.
nettest sleepsrv <port> <ms>: listens, sleeps ms, then accepts one connection and reads it.
nettest recv <ip> <port>: a connection to ip:port, read until it ends (what came is shown).
nettest churn <ip> <port> <n>: n connections to ip:port one after another, each closed at
once; how many could not be made, and why.
nettest pollsrv <port> <n>: a server driven by poll() (as telnetd is): it echoes what its
connections send and stops after n connections have ended.
nettest burst <port> <bursts> <ms> <size>...: one connection gets <bursts> bursts, each a
write() of every size in turn (the pattern), ms apart; then it is read until it ends, or,
with ms below 0, closed at once after the writes. When each burst began (the uptime).

nettest limits: the calls' errors: listen on port 0 (EINVAL) and on a port in use
(EADDRINUSE); accept on no descriptor (EBADF), on the console or a file (EINVAL: no
listening socket), into memory that is not the caller's (EFAULT, before anything
waits); read and write on a listening socket (ENOTCONN); connect to
port 0 (EINVAL); netinfo into the kernel's memory (EFAULT); listening sockets until
the descriptors run out (EMFILE, or ENFILE when the sockets run out first), then a
child that listens until the sockets run out (ENFILE); everything closed, every port
free again. One line per check, then NETTEST PASS (exit 0) or NETTEST FAIL.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <surf.h>

#define KERNEL_ADDR ((void *)0x100000)          /* the kernel's text: never the caller's memory */
#define LIMIT_PORT  5300                        /* limits: ports from here on */

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;
    if(ok) { printf("  ok    %s\n", what); return; }
    failures++;
    printf("  FAIL  %s (errno %d: %s)\n", what, errno, strerror(errno));
}

/* 10/6/26: write() on a connection queues what fits: the rest goes in later writes */
static int write_all(int fd, const char *p, long n, long *writes, long *partial) {
    while(n > 0) {
        long w = (long)write(fd, p, (size_t)n);
        if(w <= 0) return -1;
        if(writes) (*writes)++;
        if(partial && w < n) (*partial)++;
        p += w;
        n -= w;
    }
    return 0;
}

/* how a read loop ended: "EOF", or the error */
static const char *ending(long r) {
    return r < 0 ? strerror(errno) : "EOF";
}

static int ip_arg(const char *s, unsigned int *ip) {
    if(ipparse(s, ip) == 0) return 0;
    fprintf(stderr, "nettest: %s: not an IPv4 address\n", s);
    return -1;
}

/**** echo ****/

static int echo(int port, int n) {
    char buf[1024], a[16];
    int l = tcp_listen((unsigned short)port), i;
    if(l < 0) { printf("nettest: listen on port %d: %s\n", port, strerror(errno)); return 1; }
    printf("nettest: listening on port %d\n", port);
    fflush(stdout);
    for(i = 0; i < n; i++) {
        struct netaddr peer;
        long total = 0;
        int c = tcp_accept(l, &peer), r;
        if(c < 0) { printf("nettest: accept: %s\n", strerror(errno)); close(l); return 1; }
        printf("nettest: connection from %s port %u\n", ipstr(peer.ip, a), peer.port);
        fflush(stdout);
        while((r = (int)read(c, buf, sizeof(buf))) > 0) {
            if(write_all(c, buf, r, NULL, NULL)) { printf("nettest: write: %s\n", strerror(errno)); break; }
            total += r;
        }
        if(r < 0) printf("nettest: read: %s\n", strerror(errno));
        close(c);
        printf("nettest: %ld bytes echoed\n", total);
        fflush(stdout);
    }
    close(l);
    return 0;
}

/* byte at of the pattern: line at / 16 is "nettest " and the line's number in 7 digits, then a newline */
static char pattern_byte(long at) {
    long line = at / 16, d;
    int col = (int)(at % 16);
    if(col < 8) return "nettest "[col];
    if(col == 15) return '\n';
    for(d = 1; col < 14; col++) d *= 10;
    return (char)('0' + (line / d) % 10);
}

/* one connection gets count bytes of a pattern the other end can check, then the FIN */
static int send_bytes(int port, long count) {
    char buf[1024], a[16];
    struct netaddr peer;
    long done = 0;
    int l = tcp_listen((unsigned short)port), c, i;
    if(l < 0) { printf("nettest: listen on port %d: %s\n", port, strerror(errno)); return 1; }
    printf("nettest: listening on port %d\n", port);
    fflush(stdout);
    c = tcp_accept(l, &peer);
    close(l);
    if(c < 0) { printf("nettest: accept: %s\n", strerror(errno)); return 1; }
    printf("nettest: connection from %s port %u\n", ipstr(peer.ip, a), peer.port);
    fflush(stdout);
    while(done < count) {
        int n = count - done < (long)sizeof(buf) ? (int)(count - done) : (int)sizeof(buf);
        for(i = 0; i < n; i++) buf[i] = pattern_byte(done + i);
        if(write_all(c, buf, n, NULL, NULL)) { printf("nettest: write after %ld bytes: %s\n", done, strerror(errno)); close(c); return 1; }
        done += n;
    }
    close(c);
    printf("nettest: %ld bytes sent\n", done);
    return 0;
}

/**** for the tests with hand-made segments ****/

/* one connection on port, the listener closed again: its descriptor, or -1 */
static int accept_one(int port, int ms) {
    struct netaddr peer;
    char a[16];
    int l = tcp_listen((unsigned short)port), c;
    if(l < 0) { printf("nettest: listen on port %d: %s\n", port, strerror(errno)); return -1; }
    printf("nettest: listening on port %d\n", port);
    fflush(stdout);
    if(ms) {
        msleep((unsigned int)ms);
        printf("nettest: accepting now\n");
        fflush(stdout);
    }
    c = tcp_accept(l, &peer);
    close(l);
    if(c < 0) { printf("nettest: accept: %s\n", strerror(errno)); return -1; }
    printf("nettest: connection from %s port %u\n", ipstr(peer.ip, a), peer.port);
    fflush(stdout);
    return c;
}

/* read c until it ends, the data shown when show; closed afterwards */
static int read_to_end(int c, int show) {
    char buf[1025];
    long total = 0, r;
    while((r = (long)read(c, buf, sizeof(buf) - 1)) > 0) {
        buf[r] = 0;
        if(show) fputs(buf, stdout);
        total += r;
    }
    printf("%snettest: read %ld bytes, then %s\n", show && total ? "\n" : "", total, ending(r));
    close(c);
    return r < 0;
}

static int reader(int port, int ms) {
    int c = accept_one(port, ms);
    return c < 0 ? 1 : read_to_end(c, 0);
}

static int writer(int port, long count, int chunk) {
    static char buf[16384];
    long done = 0, writes = 0, partial = 0;
    int c = accept_one(port, 0), i, n;
    if(c < 0) return 1;
    if(chunk < 1 || chunk > (int)sizeof(buf)) chunk = 4096;
    while(done < count) {
        n = count - done < chunk ? (int)(count - done) : chunk;
        for(i = 0; i < n; i++) buf[i] = pattern_byte(done + i);
        if(write_all(c, buf, n, &writes, &partial)) {
            printf("nettest: write after %ld bytes: %s\n", done, strerror(errno));
            close(c);
            return 1;
        }
        done += n;
    }
    close(c);
    printf("nettest: wrote %ld bytes in %ld writes, %ld partial\n", done, writes, partial);
    return 0;
}

static int recv_from(unsigned int ip, int port) {
    char a[16];
    int c = tcp_connect(ip, (unsigned short)port);
    if(c < 0) { printf("nettest: connect to %s port %d: %s\n", ipstr(ip, a), port, strerror(errno)); return 1; }
    printf("nettest: connected to %s port %d\n", ipstr(ip, a), port);
    fflush(stdout);
    return read_to_end(c, 1);
}

static int churn(unsigned int ip, int port, int n) {
    char a[16];
    int i, failed = 0, first_errno = 0;
    for(i = 0; i < n; i++) {
        int c = tcp_connect(ip, (unsigned short)port);
        if(c < 0) {
            if(!failed++) first_errno = errno;
            continue;
        }
        close(c);
    }
    printf("nettest: %d connections to %s port %d, %d failed%s%s\n", n, ipstr(ip, a), port, failed,
           failed ? ": " : "", failed ? strerror(first_errno) : "");
    return failed != 0;
}

/* a poll()-driven echo server, telnetd's shape: the listener and up to 4 connections. A connection's
   echo that did not fit (write() queues what fits) waits in its own buffer, and the server reads
   no more from that connection until the buffer is out (POLLOUT): a client that stops reading
   holds up nobody else. It stops after ends connections have ended. */
static int pollsrv(int port, int ends) {
    struct pollfd p[5];
    static char out[4][512];
    int conns[4] = { -1, -1, -1, -1 }, pend[4] = { 0, 0, 0, 0 }, l = tcp_listen((unsigned short)port), i, n, ended = 0;
    char a[16];
    if(l < 0) { printf("nettest: listen on port %d: %s\n", port, strerror(errno)); return 1; }
    printf("nettest: polling on port %d\n", port);
    fflush(stdout);
    while(ended < ends) {
        p[0].fd = l;
        p[0].events = POLLIN;
        for(i = 0; i < 4; i++) {
            p[i + 1].fd = conns[i];
            p[i + 1].events = pend[i] ? POLLOUT : POLLIN;
        }
        n = poll(p, 5, -1);
        if(n < 0) { printf("nettest: poll: %s\n", strerror(errno)); close(l); return 1; }
        if(p[0].revents & POLLIN) {
            struct netaddr peer;
            int c = tcp_accept(l, &peer);
            if(c < 0) printf("nettest: accept: %s\n", strerror(errno));
            else {
                for(i = 0; i < 4 && conns[i] >= 0; i++);
                if(i == 4) { write(c, "busy\r\n", 6); close(c); }
                else conns[i] = c;
                printf("nettest: connection from %s port %u%s\n", ipstr(peer.ip, a), peer.port, i == 4 ? ": busy" : "");
            }
            fflush(stdout);
        }
        for(i = 0; i < 4; i++) {
            long r;
            if(conns[i] < 0 || !p[i + 1].revents) continue;
            if(pend[i]) {                                   /* POLLOUT: more of the echo */
                r = (long)write(conns[i], out[i], (size_t)pend[i]);
                if(r > 0) {
                    memmove(out[i], out[i] + r, (size_t)(pend[i] - r));
                    pend[i] -= (int)r;
                    continue;
                }
            } else {
                r = (long)read(conns[i], out[i], sizeof(out[i]));
                if(r > 0) {
                    long w = (long)write(conns[i], out[i], (size_t)r);
                    if(w > 0) {
                        memmove(out[i], out[i] + w, (size_t)(r - w));
                        pend[i] = (int)(r - w);
                        continue;
                    }
                    r = w;
                }
            }
            printf("nettest: a connection ended: %s\n", ending(r));
            fflush(stdout);
            close(conns[i]);
            conns[i] = -1;
            pend[i] = 0;
            ended++;
        }
    }
    for(i = 0; i < 4; i++) if(conns[i] >= 0) close(conns[i]);
    close(l);
    printf("nettest: %d connections ended\n", ended);
    return 0;
}

/* 10/6/26 (second review): bursts of small writes (latency, a lost small segment) */
static int burst(int port, int bursts, int ms, int nsizes, char **sizes) {
    static char buf[16384];
    static unsigned int at[64];
    long done = 0, w;
    int c = accept_one(port, 0), b, j, i, n;
    if(c < 0) return 1;
    for(b = 0; b < bursts; b++) {
        if(b < 64) at[b] = uptime();
        for(j = 0; j < nsizes; j++) {
            n = atoi(sizes[j]);
            if(n < 1 || n > (int)sizeof(buf)) n = 1;
            for(i = 0; i < n; i++) buf[i] = pattern_byte(done + i);
            w = (long)write(c, buf, (size_t)n);
            if(w != n) printf("nettest: burst write: %ld of %d: %s\n", w, n, strerror(errno));
            if(w > 0) done += w;
        }
        if(ms > 0) msleep((unsigned int)ms);
    }
    printf("nettest: burst wrote %ld bytes; bursts at", done);
    for(b = 0; b < bursts && b < 64; b++) printf(" %u", at[b]);
    printf(" ms\n");
    fflush(stdout);
    if(ms >= 0) return read_to_end(c, 0);
    close(c);
    printf("nettest: burst closed\n");
    return 0;
}

/**** a client ****/

static int get(unsigned int ip, int port, const char *path) {
    char req[512], buf[513], a[16];
    long total = 0;
    int c = tcp_connect(ip, (unsigned short)port), n, r = 0;
    if(c < 0) { printf("nettest: connect to %s port %d: %s\n", ipstr(ip, a), port, strerror(errno)); return 1; }
    n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: nettest\r\nConnection: close\r\n\r\n",
                 path, ipstr(ip, a));
    if(n < 0 || n >= (int)sizeof(req)) { printf("nettest: the path is too long\n"); close(c); return 1; }
    if(write_all(c, req, n, NULL, NULL)) { printf("nettest: write: %s\n", strerror(errno)); close(c); return 1; }
    while((r = (int)read(c, buf, sizeof(buf) - 1)) > 0) {
        buf[r] = 0;
        fputs(buf, stdout);
        total += r;
    }
    if(r < 0) printf("\nnettest: read: %s\n", strerror(errno));
    close(c);
    printf("\nnettest: %ld bytes received\n", total);
    return r < 0 || !total;
}

static int connect_to(unsigned int ip, int port, int want_refused) {
    char a[16];
    int c = tcp_connect(ip, (unsigned short)port);
    if(c >= 0) {
        printf("nettest: connected to %s port %d\n", ipstr(ip, a), port);
        close(c);
        return want_refused;
    }
    printf("nettest: connect to %s port %d: %s\n", ipstr(ip, a), port, strerror(errno));
    return want_refused ? errno != ECONNREFUSED : 1;
}

static int info(void) {
    struct netinfo ni;
    char a[16], b[16], c[16], d[16];
    if(netinfo(&ni) < 0) { printf("nettest: netinfo: %s\n", strerror(errno)); return 1; }
    printf("%s: %02x:%02x:%02x:%02x:%02x:%02x, link %s\n", ni.name, ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4],
           ni.mac[5], ni.link ? "up" : "down");
    printf("    inet %s  netmask %s  gateway %s  dns %s%s\n", ipstr(ni.ip, a), ipstr(ni.netmask, b), ipstr(ni.gateway, c),
           ipstr(ni.dns, d), ni.configured ? "" : "   (no address yet: dhcp)");
    return 0;
}

/**** limits ****/

/* listening sockets on ports from first on, until a listen fails: how many, and why it failed (errno) */
static int listen_all(int first, int *fds, int max, int *why) {
    int n = 0;
    *why = 0;
    while(n < max) {
        int fd = tcp_listen((unsigned short)(first + n));
        if(fd < 0) { *why = errno; break; }
        fds[n++] = fd;
    }
    return n;
}

/* nettest hold <port> [ms]: the child of limits; listens until the sockets run out, keeps them ms
   (the tests: the kernel's own connections still get one). Exit 0 for ENFILE */
static int hold(int first, int ms) {
    int fds[32], why, n = listen_all(first, fds, 32, &why), i;
    printf("nettest: a child process listened on %d more ports, then: %s\n", n, strerror(why));
    fflush(stdout);
    if(ms) msleep((unsigned int)ms);
    for(i = 0; i < n; i++) close(fds[i]);
    return why == ENFILE ? 0 : 1;
}

static int limits(void) {
    char *child[] = { "nettest", "hold", "5400", NULL };
    char what[120], buf[8];
    struct netinfo *kernel_ni = (struct netinfo *)KERNEL_ADDR;
    int l, fd, fds[32], n, why, i, pid, status = -99;

    errno = 0;
    check(tcp_listen(0) < 0 && errno == EINVAL, "listen on port 0: EINVAL");
    l = tcp_listen(LIMIT_PORT);
    check(l >= 0, "listen on a free port");
    errno = 0;
    check(tcp_listen(LIMIT_PORT) < 0 && errno == EADDRINUSE, "listen on a port in use: EADDRINUSE");
    errno = 0;
    check(tcp_accept(99, NULL) < 0 && errno == EBADF, "accept on a descriptor that is not open: EBADF");
    errno = 0;
    check(tcp_accept(0, NULL) < 0 && errno == EINVAL, "accept on the console: EINVAL");
    fd = open("/initrd/etc/version", O_RDONLY);
    errno = 0;
    check(fd >= 0 && tcp_accept(fd, NULL) < 0 && errno == EINVAL, "accept on a file: EINVAL");
    if(fd >= 0) close(fd);
    errno = 0;
    check(tcp_accept(l, (struct netaddr *)KERNEL_ADDR) < 0 && errno == EFAULT, "accept into the kernel's memory: EFAULT at once");
    errno = 0;
    check(read(l, buf, sizeof(buf)) < 0 && errno == ENOTCONN, "read on a listening socket: ENOTCONN");
    errno = 0;
    check(write(l, "x", 1) < 0 && errno == ENOTCONN, "write on a listening socket: ENOTCONN");
    errno = 0;
    check(tcp_connect(0x0202000a, 0) < 0 && errno == EINVAL, "connect to port 0: EINVAL");
    errno = 0;
    check(netinfo(kernel_ni) < 0 && errno == EFAULT, "netinfo into the kernel's memory: EFAULT");
    close(l);
    l = tcp_listen(LIMIT_PORT);
    check(l >= 0, "close frees the port: listen on it again");
    if(l >= 0) close(l);

    n = listen_all(LIMIT_PORT, fds, 32, &why);
    snprintf(what, sizeof(what), "listening on %d ports, until there is no descriptor or socket left: %s", n, strerror(why));
    errno = why;
    check(n > 0 && (why == EMFILE || why == ENFILE), what);
    fflush(stdout);
    pid = spawn("/initrd/bin/nettest", child);
    check(pid > 0 && waitpid(pid, &status, 0) == pid && status == 0, "then a child process listens until there is no socket left: ENFILE");
    for(i = 0; i < n; i++) close(fds[i]);
    n = listen_all(LIMIT_PORT, fds, 3, &why);
    check(n == 3, "every socket closed: their ports are free again");
    for(i = 0; i < n; i++) close(fds[i]);

    printf("%d checks, %d failed\n", checks, failures);
    puts(failures ? "NETTEST FAIL" : "NETTEST PASS");
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    unsigned int ip;
    if((argc == 3 || argc == 4) && !strcmp(argv[1], "echo")) return echo(atoi(argv[2]), argc == 4 ? atoi(argv[3]) : 1);
    if(argc == 4 && !strcmp(argv[1], "send")) return send_bytes(atoi(argv[2]), atol(argv[3]));
    if(argc == 5 && !strcmp(argv[1], "get")) return ip_arg(argv[2], &ip) ? 2 : get(ip, atoi(argv[3]), argv[4]);
    if(argc == 2 && !strcmp(argv[1], "info")) return info();
    if(argc == 4 && !strcmp(argv[1], "connect")) return ip_arg(argv[2], &ip) ? 2 : connect_to(ip, atoi(argv[3]), 0);
    if(argc == 4 && !strcmp(argv[1], "refuse")) return ip_arg(argv[2], &ip) ? 2 : connect_to(ip, atoi(argv[3]), 1);
    if(argc == 2 && !strcmp(argv[1], "limits")) return limits();
    if((argc == 3 || argc == 4) && !strcmp(argv[1], "hold")) return hold(atoi(argv[2]), argc == 4 ? atoi(argv[3]) : 0);
    if(argc == 3 && !strcmp(argv[1], "reader")) return reader(atoi(argv[2]), 0);
    if((argc == 4 || argc == 5) && !strcmp(argv[1], "writer")) return writer(atoi(argv[2]), atol(argv[3]), argc == 5 ? atoi(argv[4]) : 4096);
    if(argc == 4 && !strcmp(argv[1], "sleepsrv")) return reader(atoi(argv[2]), atoi(argv[3]));
    if(argc == 4 && !strcmp(argv[1], "recv")) return ip_arg(argv[2], &ip) ? 2 : recv_from(ip, atoi(argv[3]));
    if(argc == 5 && !strcmp(argv[1], "churn")) return ip_arg(argv[2], &ip) ? 2 : churn(ip, atoi(argv[3]), atoi(argv[4]));
    if(argc == 4 && !strcmp(argv[1], "pollsrv")) return pollsrv(atoi(argv[2]), atoi(argv[3]));
    if(argc >= 6 && !strcmp(argv[1], "burst")) return burst(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), argc - 5, argv + 5);
    fprintf(stderr, "usage: nettest echo <port> [n]\n"
                    "       nettest send <port> <bytes>\n"
                    "       nettest get <ip> <port> <path>\n"
                    "       nettest info\n"
                    "       nettest connect <ip> <port>\n"
                    "       nettest refuse <ip> <port>\n"
                    "       nettest limits\n"
                    "       nettest reader|writer|sleepsrv|recv|churn|pollsrv|burst ...   (tools/qemu-tcp.py)\n");
    return 2;
}
