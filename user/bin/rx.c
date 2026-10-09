/*
SurfOS user program: rx
--------------------
File: rx.c      Date: 10/9/26
--------------------
rx <port> <path>: receive one file over the network, for the development loop
(tools/devloop.py on the Mac pushes builds to the data stick with it; docs/USB-BOOT.md
section 13).

It listens on port, takes one connection within 120 seconds (the listener closes as soon
as it has one, so nobody else gets in), and writes everything that comes to <path>.part
until the other end closes. Then the file is written out (fsync), closed and renamed to
<path>, replacing a file of that name, and rx says

    rx: <n> bytes, crc32 <8 hex digits> -> <path>

with the CRC-32 of what it received (the one zlib and Python compute), so the sender can
check it. Before that: "rx: listening on port <port> for <path>", then the address the
connection came from. A connection that stays silent for 120 seconds is given up.

Exit status 0; 1 on any error, said with its reason (the partial file <path>.part is
removed); 2 for a usage error. Nothing else: no shell, nothing is run, nothing is sent back.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <surf.h>

#define WAIT_MS   120000                /* for the connection, and for each piece of data after it */
#define BUF_SIZE  32768                 /* written to the file in pieces this large */
#define MAX_BYTES 0x7fffffffL           /* a program's file offsets stop at 2 GB */

static unsigned int crc_table[256];
static char buf[BUF_SIZE];

/* the CRC-32 of zlib and PNG: polynomial 0xEDB88320 (reflected), all ones in and out */
static void crc_init(void) {
    unsigned int c, n, k;
    for(n = 0; n < 256; n++) {
        for(c = n, k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static unsigned int crc_update(unsigned int crc, const unsigned char *p, long n) {
    crc = ~crc;
    while(n-- > 0) crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* a port number as typed: digits only, 1-65535; 0 when it is none */
static int port_of(const char *word) {
    long v = 0;
    const char *p;
    if(!*word || strlen(word) > 5) return 0;
    for(p = word; *p; p++) {
        if(!isdigit((unsigned char)*p)) return 0;
        v = v * 10 + (*p - '0');
    }
    return v >= 1 && v <= 65535 ? (int)v : 0;
}

static char part[PATH_MAX];             /* <path>.part */
static int file = -1, conn = -1, listener = -1;

/* an error: said, then everything let go of and the partial file removed */
static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...) {
    va_list ap;
    int e = errno;
    fflush(stdout);
    fputs("rx: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if(conn >= 0) close(conn);
    if(listener >= 0) close(listener);
    if(file >= 0) {
        close(file);
        unlink(part);
    }
    errno = e;
    return 1;
}

static int write_all(int fd, const char *p, long n) {
    while(n > 0) {
        long w = (long)write(fd, p, (size_t)n);
        if(w <= 0) return -1;
        p += w;
        n -= w;
    }
    return 0;
}

/* wait until fd has something to read, at most WAIT_MS: 1, 0 after the time, -1 with errno */
static int readable(int fd) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, WAIT_MS);
}

int main(int argc, char **argv) {
    struct netaddr peer;
    struct stat st;
    const char *path;
    char ip[16];
    unsigned int crc = 0;
    long total = 0, fill = 0, r;
    int port;

    if(argc != 3) {
        fprintf(stderr, "usage: rx <port> <path>   (one file from the network into path)\n");
        return 2;
    }
    if(!(port = port_of(argv[1]))) {
        fprintf(stderr, "rx: %s is no port (1-65535)\n", argv[1]);
        return 2;
    }
    path = argv[2];
    if(!*path || strlen(path) + 6 > sizeof(part)) {
        fprintf(stderr, "rx: the path is %s\n", *path ? "too long" : "empty");
        return 2;
    }
    if(stat(path, &st) == 0 && S_ISDIR(st.type)) {
        errno = EISDIR;
        return fail("%s: %s", path, strerror(errno));
    }
    snprintf(part, sizeof(part), "%s.part", path);
    crc_init();

    /* the file first: a path that cannot be written is said before anything is sent */
    if((file = open(part, O_WRONLY | O_CREAT | O_TRUNC)) < 0) return fail("%s: %s", part, strerror(errno));
    if((listener = tcp_listen((unsigned short)port)) < 0) {
        if(errno == EADDRINUSE) return fail("port %d is in use", port);
        return fail("listen on port %d: %s", port, strerror(errno));
    }
    printf("rx: listening on port %d for %s\n", port, path);
    fflush(stdout);

    r = readable(listener);
    if(r == 0) return fail("no connection within %d s", WAIT_MS / 1000);
    if(r < 0) return fail("waiting for a connection: %s", strerror(errno));
    if((conn = tcp_accept(listener, &peer)) < 0) return fail("accept: %s", strerror(errno));
    close(listener);                    /* one connection: nobody else gets in */
    listener = -1;
    printf("rx: connection from %s port %u\n", ipstr(peer.ip, ip), peer.port);
    fflush(stdout);

    for(;;) {
        r = readable(conn);
        if(r == 0) return fail("nothing came for %d s (after %ld bytes)", WAIT_MS / 1000, total);
        if(r < 0) return fail("waiting for data: %s", strerror(errno));
        r = (long)read(conn, buf + fill, (size_t)(BUF_SIZE - fill));
        if(r < 0) return fail("the connection, after %ld bytes: %s", total, strerror(errno));
        if(r == 0) break;               /* the other end closed: everything is here */
        if(total > MAX_BYTES - r) return fail("more than 2 GB: refused");
        crc = crc_update(crc, (const unsigned char *)buf + fill, r);
        total += r;
        fill += r;
        if(fill == BUF_SIZE) {
            if(write_all(file, buf, fill) < 0) return fail("%s: %s", part, strerror(errno));
            fill = 0;
        }
    }
    if(fill && write_all(file, buf, fill) < 0) return fail("%s: %s", part, strerror(errno));
    if(fsync(file) < 0) return fail("%s: %s", part, strerror(errno));
    r = close(file);
    file = -1;
    if(r < 0) {
        r = errno;
        unlink(part);
        errno = (int)r;
        return fail("%s: %s", part, strerror(errno));
    }
    if(rename(part, path) < 0) {
        r = errno;
        unlink(part);
        errno = (int)r;
        return fail("%s -> %s: %s", part, path, strerror(errno));
    }
    close(conn);
    printf("rx: %ld bytes, crc32 %08x -> %s\n", total, crc, path);
    return 0;
}
