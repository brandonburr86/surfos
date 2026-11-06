/*
SurfOS chat client: wchat
--------------------
File: helper.c  Date: 11/05/26
--------------------
The connection's helper (link.h): wchat --helper HOST PORT tls|tcp, started by the window with its
descriptor 0 a pipe from the window and 1 a pipe to it. It makes the connection with Undertow's
transport (user/undertow/transport.c, as user/fetch/ uses it): the name looked up (undertow.conf's
host lines first, then DNS), TCP, and for tls a TLS 1.2 or 1.3 handshake in which the server's
certificate chain must lead to a root of /initrd/etc/ssl/cert.pem, name the host and be valid now.
Nothing turns those checks off. Then it relays: what the window writes goes to the server as it is,
and what the server sends goes to the window in LINK_DATA frames. It parses none of it: the window
does, and reads the helper's frames as hostile.

The transport waits inside its calls, and a read with nothing to read for 15 seconds fails, which a
quiet IRC connection cannot afford. So the helper reads only when poll() says the connection's
descriptor (ut_transport_fd()) is ready, and with room for a whole TLS record (16 KB), so that
nothing is left inside TLS where poll() cannot see it; a record that comes in pieces is waited for
within the transport's 15 seconds. Its deadline is 20 days (ut_now()'s differences hold 24): a
connection that lasts longer ends, and says so.

It ends when the window's pipe ends (the window closed it, or has gone): TLS's close_notify goes
when the connection is still good. A password passes through it inside the line that carries it,
and the buffer that held it is wiped once it has gone. It writes nowhere but the two pipes.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <surf.h>
#include "undertow.h"
#include "url.h"
#include "config.h"
#include "transport.h"
#include "link.h"

#define DEADLINE_MS 1728000000u         /* 20 days: the transport's deadline must be one */

static char rec[LINK_DATA_MAX];         /* from the server: a whole TLS record's plaintext */
static char out[LINK_DATA_MAX + 64];    /* a frame for the window */
static char lines[4096];                /* from the window: lines for the server */

static void wipe(void *p, size_t n) {
    volatile unsigned char *b = p;
    while(n--) *b++ = 0;
}

/* a frame to the window, all of it: 0, or -1 once the window has gone */
static int put(int kind, const void *data, int len) {
    int n = link_frame(out, sizeof(out), kind, data, len), o = 0;
    ssize_t k;
    if(n < 0) return -1;
    while(o < n) {
        k = write(1, out + o, (size_t)(n - o));
        if(k < 0 && errno == EINTR) continue;
        if(k <= 0) return -1;
        o += (int)k;
    }
    return 0;
}

/* the connection's end, said to the window: status 1 */
static int down(const char *why) {
    int n = (int)strlen(why);
    put(LINK_DOWN, why, n > LINK_TEXT_MAX ? LINK_TEXT_MAX : n);
    return 1;
}

/* the window's lines, to the server; 0, or 1 after a failure said */
static int relay(struct ut_transport *t, int n) {
    int o = 0, k;
    while(o < n) {
        k = ut_transport_write(t, lines + o, (size_t)(n - o));
        if(k <= 0) {
            wipe(lines, sizeof(lines));
            return down(ut_transport_error(t));
        }
        o += k;
    }
    wipe(lines, (size_t)n);                         /* a PASS or AUTHENTICATE line stays nowhere */
    return 0;
}

int helper_main(int argc, char **argv) {
    static struct ut_config cfg;
    static struct ut_url url;
    static char addr[300], err[UT_ERR_MAX];
    struct ut_transport *t = NULL;
    struct pollfd p[2];
    unsigned int deadline;
    int fd, n, status = 0;
    if(argc != 5 || (strcmp(argv[4], "tls") && strcmp(argv[4], "tcp")) || strlen(argv[2]) > 253 || !argv[3][0] ||
       strspn(argv[3], "0123456789") != strlen(argv[3]) || strlen(argv[3]) > 5) {
        fprintf(stderr, "wchat --helper is the Chat window's own\n");
        return 2;
    }
    broken_pipe_exit(0);                            /* the window gone: EPIPE, and an end */
    snprintf(addr, sizeof(addr), "%s://%s:%s/", strcmp(argv[4], "tls") ? "http" : "https", argv[2], argv[3]);
    if(ut_url_parse(addr, &url, err, sizeof(err)) < 0) return down(err);
    ut_config_init(&cfg);
    ut_config_load(&cfg, UT_CONF_PATH, err, sizeof(err));     /* a broken file: the defaults, as for Undertow */
    deadline = ut_now() + DEADLINE_MS;
    if(ut_transport_open(&t, &url, &cfg, deadline, err, sizeof(err)) < 0) {
        ut_transport_cleanup();
        return down(err);
    }
    fd = ut_transport_fd(t);
    if(fd < 0 || put(LINK_UP, ut_transport_tls(t), (int)strlen(ut_transport_tls(t)))) {
        ut_transport_close(t);
        ut_transport_cleanup();
        return 1;
    }
    for(;;) {
        p[0].fd = 0;
        p[0].events = POLLIN;
        p[1].fd = fd;
        p[1].events = POLLIN;
        p[0].revents = p[1].revents = 0;
        if(poll(p, 2, -1) < 0) {
            if(errno == EINTR) continue;
            status = down("waiting for the server failed");
            break;
        }
        if(p[1].revents) {
            n = ut_transport_read(t, rec, sizeof(rec));
            if(n == 0) {
                status = down("the server closed the connection");
                break;
            }
            if(n < 0) {
                status = down(ut_expired(deadline, ut_now()) ? "the connection reached its limit of 20 days: connect again"
                                                              : ut_transport_error(t));
                break;
            }
            if(put(LINK_DATA, rec, n)) break;       /* the window has gone */
        }
        if(p[0].revents) {
            n = (int)read(0, lines, sizeof(lines));
            if(n < 0 && errno == EINTR) continue;
            if(n <= 0) break;                       /* the window ended the connection */
            if((status = relay(t, n)) != 0) break;
        }
    }
    ut_transport_close(t);
    ut_transport_cleanup();
    wipe(lines, sizeof(lines));
    return status;
}
