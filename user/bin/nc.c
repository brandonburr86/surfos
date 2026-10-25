/*
SurfOS user program: nc
--------------------
File: nc.c      Date: 10/24/26
--------------------
nc, the netcat of SurfOS: a TCP connection made by hand, to see what a server says, to talk to
it, or to send a file to another computer or take one from it.

    nc [-v] [-c] [-w seconds] <host> <port>   a connection to host (an address or a name) and port
    nc [-v] [-c] [-w seconds] -l <port>       wait for one connection to port (from anyone), then the same
    nc [-v] -z <host> <port>...               whether each port is open: a connection made and closed at
                                              once; a port may be a range, first-last (1000 ports at most)

What is typed, a line at a time (or a file given with <), goes out; what comes back is printed
(or saved with >), byte for byte. When the input ends (the end of the file given with <), nc
says so to the other end with shutdown(), the half-close: the other end reads the end of what
was sent while its answer still comes back. The other end's FIN says it has sent all it will;
nc ends when both ways have ended, or at that FIN when the input is typed (the console has no
end of its own). The Mac's nc does the same, so either one sends a file to the other:

    the receiving side:  nc -l 9000 > file         (or on the Mac: nc -l 9000 > file)
    the sending side:    nc 10.0.0.4 9000 < file   (or on the Mac: nc 10.0.0.4 9000 < file)

The console reads a line when its Enter comes, so what arrives while a line is being typed is
printed after that Enter; the console has no key for the end of the input: Ctrl+C ends nc.
nc -l says where it listens and who connected (on stderr, so a > file gets only what came);
-v says more there: both ends of the connection and why it ended. -c sends CR LF for each line
typed (for a mail or web server talked to by hand), -w ends a connection silent that long, and
nc -l when no connection comes within it. Exit status: 0 when the connection ended both ways
(for -z: every port was open), 1 on an error or a port that is not open, 2 for a usage error.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <surf.h>

#define BUF_SIZE  8192
#define SCAN_MAX  1000                  /* ports in one -z */
#define REST_MS   20                    /* a half-closed connection that takes nothing now is polled after this */

static int verbose, crlf, wait_s;

static int usage(void) {
    fprintf(stderr, "usage: nc [-vc] [-w seconds] host port\n"
                    "       nc [-vc] [-w seconds] -l port\n"
                    "       nc [-v] -z host port|first-last ...\n");
    return 2;
}

/* a line on stderr: always (say_always) or with -v (say) */
static void vsay(const char *fmt, va_list ap) {
    fflush(stdout);
    fputs("nc: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static void say_always(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say_always(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    if(!verbose) return;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
}

/* an error, said: status 1 */
static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
    return 1;
}

/* a port number as typed: digits only, 1-65535; 0 when it is none */
static int port_of(const char *word) {
    long v = 0;
    const char *p;
    if(!*word || strlen(word) > 5) return 0;
    for(p = word; *p; p++) {
        if(*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
    }
    return v >= 1 && v <= 65535 ? (int)v : 0;
}

/* host as an address: a dotted quad, or a name for DNS. 0, or -1 with errno */
static int resolve(const char *host, unsigned int *ip) {
    if(ipparse(host, ip) == 0) return 0;
    return net_resolve(host, ip);
}

static int write_all(int fd, const char *p, int n) {
    while(n > 0) {
        ssize_t w = write(fd, p, (size_t)n);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0) return -1;
        p += w;
        n -= (int)w;
    }
    return 0;
}

/* both ends of the connection, with -v */
static void say_ends(int sock, const char *how) {
    struct netaddr here, there;
    char a[16], b[16];
    if(!verbose || tcp_addrs(sock, &here, &there) < 0) return;
    say("%s %s port %u (this end: %s port %u)", how, ipstr(there.ip, a), there.port, ipstr(here.ip, b), here.port);
}

/* what is typed (or read from a file) to sock, and what sock sends to stdout: each way ends by itself (the input's
   end, sent as the half-close; the other end's FIN, its half-close or its close), and the session when both have.
   Typed input has no end of its own: there the other end's FIN ends the session */
static int session(int sock) {
    static char in[2 * BUF_SIZE], piece[BUF_SIZE], got[BUF_SIZE];
    int in_len = 0, in_off = 0, input = 1, net = 1, shut = 0, rest = 0, typed = isatty(0);
    unsigned int last = uptime();
    for(;;) {
        struct pollfd p[2];
        int n = 0, si = -1, ki = -1, timeout = -1, r;
        if(!input && !net && in_off == in_len) {
            say("both ways have ended");
            return 0;
        }
        /* -w: the time since the last byte in or out, whatever poll() says meanwhile (10/25/26, review: a
           connection that rests below, the other end's FIN in and nothing taken from us, made no poll() time
           out, and once the time was up the rests took none) */
        if(wait_s && (timeout = wait_s * 1000 - (int)(uptime() - last)) <= 0) {
            say_always("nothing came or went for %d s: the connection is closed", wait_s);
            return 1;
        }
        if(input && in_off == in_len) {             /* more is read when what was read has gone */
            p[n].fd = 0;
            p[n].events = POLLIN;
            p[n].revents = 0;
            si = n++;
        }
        /* the connection: read while the other end sends, written while input waits. Once its FIN came, poll() says
           POLLHUP at once every time: so it is left out while nothing waits to go, and rests REST_MS when what waits
           does not fit now */
        if(!rest && (net || in_off < in_len)) {
            p[n].fd = sock;
            p[n].events = (net ? POLLIN : 0) | (in_off < in_len ? POLLOUT : 0);
            p[n].revents = 0;
            ki = n++;
        }
        if(rest && (timeout < 0 || timeout > REST_MS)) timeout = REST_MS;
        r = poll(p, (nfds_t)n, timeout);
        if(r < 0) {
            if(errno == EINTR) continue;    /* a kill ends this task when the call does */
            return fail("poll: %s", strerror(errno));
        }
        rest = 0;                           /* r 0: -w's time is up, or the rest is over (the next pass says) */
        if(si >= 0 && p[si].revents) {
            ssize_t m = read(0, piece, crlf ? BUF_SIZE / 2 : BUF_SIZE);
            if(m < 0) {
                if(errno != EINTR) return fail("the input: %s", strerror(errno));
            } else if(m == 0) {
                input = 0;
            } else {
                int i;
                in_off = in_len = 0;
                for(i = 0; i < m; i++) {            /* -c: CR LF for each line's end */
                    if(crlf && piece[i] == '\n' && (i == 0 || piece[i - 1] != '\r')) in[in_len++] = '\r';
                    in[in_len++] = piece[i];
                }
                last = uptime();
            }
        }
        if(!input && !shut && in_off == in_len) {   /* all of it is out: the FIN goes behind it */
            if(shutdown(sock, SHUT_WR) < 0 && errno != ENOTCONN) return fail("shutdown: %s", strerror(errno));
            shut = 1;
            say("the input has ended: the other end is told so");
        }
        if(ki < 0) continue;
        if((p[ki].revents & POLLOUT) && in_off < in_len) {
            ssize_t w = write(sock, in + in_off, (size_t)(in_len - in_off));
            if(w < 0 && errno != EINTR) return fail("the connection: %s", strerror(errno));
            if(w > 0) {
                in_off += (int)w;
                last = uptime();
            }
        } else if(!net && (p[ki].revents & POLLHUP) && !(p[ki].revents & (POLLERR | POLLNVAL)))
            rest = 1;                           /* its FIN came, and what waits does not fit yet */
        if(net && (p[ki].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t m = read(sock, got, sizeof(got));
            if(m < 0) {
                if(errno == EINTR) continue;
                return fail("the connection: %s", strerror(errno));
            }
            if(m == 0) {
                net = 0;
                say("the other end has sent all it will");
                if(typed) return 0;             /* the conversation is over: typed input has no end to wait for */
                continue;
            }
            if(write_all(1, got, (int)m) < 0) return fail("the output: %s", strerror(errno));
            last = uptime();
        } else if(!net && (p[ki].revents & (POLLERR | POLLNVAL))) {
            char b;
            return fail("the connection: %s", read(sock, &b, 1) < 0 ? strerror(errno) : "it failed");
        }
    }
}

static int cmd_connect(const char *host, int port) {
    unsigned int ip;
    char a[16];
    int sock;
    if(resolve(host, &ip) < 0) return fail("%s: %s", host, errno == ENOENT ? "no such host" : errno == EINVAL ? "not a host name" : strerror(errno));
    if((sock = tcp_connect(ip, (unsigned short)port)) < 0)
        return fail("%s port %d: %s", ipstr(ip, a), port, strerror(errno));
    say_ends(sock, "connected to");
    return session(sock);
}

static int cmd_listen(int port) {
    struct netaddr peer;
    struct netinfo ni;
    struct pollfd p;
    char a[16];
    int l, c, r;
    if((l = tcp_listen((unsigned short)port)) < 0) {
        if(errno == EADDRINUSE) return fail("port %d is in use", port);
        return fail("listen on port %d: %s", port, strerror(errno));
    }
    if(netinfo(&ni) == 0 && ni.ip) say_always("listening on %s port %d", ipstr(ni.ip, a), port);
    else say_always("listening on port %d (no address yet: run dhcp)", port);
    p.fd = l;
    p.events = POLLIN;
    p.revents = 0;
    while((r = poll(&p, 1, wait_s ? wait_s * 1000 : -1)) < 0 && errno == EINTR);
    if(r == 0) return fail("no connection within %d s", wait_s);
    if(r < 0 || (c = tcp_accept(l, &peer)) < 0) return fail("accept: %s", strerror(errno));
    close(l);                           /* one connection: nobody else gets in */
    say_always("connection from %s port %u", ipstr(peer.ip, a), peer.port);
    return session(c);
}

/* the ports of a -z word: one, or first-last; 0, or -1 when it is neither */
static int ports_of(const char *word, int *first, int *last) {
    char a[8];
    const char *dash = strchr(word, '-');
    size_t n;
    if(!dash) return (*first = *last = port_of(word)) ? 0 : -1;
    if((n = (size_t)(dash - word)) >= sizeof(a)) return -1;
    memcpy(a, word, n);
    a[n] = 0;
    *first = port_of(a);
    *last = port_of(dash + 1);
    return *first && *last && *first <= *last ? 0 : -1;
}

static int cmd_scan(const char *host, int n, char **words) {
    unsigned int ip;
    char a[16];
    int i, port, first, last, count = 0, bad = 0, fd;
    for(i = 0; i < n; i++) {
        if(ports_of(words[i], &first, &last) < 0) {
            fprintf(stderr, "nc: %s is no port (1-65535) or range (first-last)\n", words[i]);
            return 2;
        }
        count += last - first + 1;
    }
    if(count > SCAN_MAX) {
        fprintf(stderr, "nc: %d ports: at most %d at once\n", count, SCAN_MAX);
        return 2;
    }
    if(resolve(host, &ip) < 0) return fail("%s: %s", host, errno == ENOENT ? "no such host" : errno == EINVAL ? "not a host name" : strerror(errno));
    ipstr(ip, a);
    for(i = 0; i < n; i++) {
        ports_of(words[i], &first, &last);
        for(port = first; port <= last; port++) {
            if((fd = tcp_connect(ip, (unsigned short)port)) >= 0) {
                printf("%s port %d: open\n", a, port);
                close(fd);
            } else {
                printf("%s port %d: %s\n", a, port, errno == ECONNREFUSED ? "closed" : strerror(errno));
                bad++;
            }
            fflush(stdout);
        }
    }
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    int i = 1, listen = 0, scan = 0, port;
    while(i < argc && argv[i][0] == '-' && argv[i][1]) {
        const char *o;
        for(o = argv[i] + 1; *o; o++) {
            if(*o == 'v') verbose = 1;
            else if(*o == 'c') crlf = 1;
            else if(*o == 'l') listen = 1;
            else if(*o == 'z') scan = 1;
            else if(*o == 'w' && !o[1] && i + 1 < argc) {
                char *end;
                long s = strtol(argv[++i], &end, 10);
                if(*end || s < 1 || s > 86400) {
                    fprintf(stderr, "nc: -w %s: a time in seconds, 1-86400\n", argv[i]);
                    return 2;
                }
                wait_s = (int)s;
                break;
            } else {
                fprintf(stderr, "nc: no option -%c\n", *o);
                return usage();
            }
        }
        i++;
    }
    if(listen && scan) return usage();
    if(scan) return argc - i >= 2 ? cmd_scan(argv[i], argc - i - 1, argv + i + 1) : usage();
    if(listen) {
        if(argc - i != 1) return usage();
        if(!(port = port_of(argv[i]))) {
            fprintf(stderr, "nc: %s is no port (1-65535)\n", argv[i]);
            return 2;
        }
        return cmd_listen(port);
    }
    if(argc - i != 2) return usage();
    if(!(port = port_of(argv[i + 1]))) {
        fprintf(stderr, "nc: %s is no port (1-65535)\n", argv[i + 1]);
        return 2;
    }
    return cmd_connect(argv[i], port);
}
