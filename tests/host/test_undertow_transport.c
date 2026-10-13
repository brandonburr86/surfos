/*
SurfOS Undertow: tests of the transport (TCP, TLS 1.2 and 1.3, the clocks, the deadlines)
----------------------
File: test_undertow_transport.c  Date: 10/11/26
----------------------
user/undertow/transport.c and tls_platform.c with the real Mbed TLS, built for
the host with the server side (tests/host/undertow/tls_host_config.h), against
peers in this process. Behind the SurfOS calls transport.c makes (tcp_connect,
net_resolve, poll, read, write, close, getentropy: tests/host/undertow/surf.h)
is a simulated network; uptime moves only when something waits, so every
deadline is exact; at the other end is Mbed TLS's own server or a scripted
plain peer. The certificates are tests/host/undertow/certs/ (make-certs.sh),
whose directory is the first argument. The TLS 1.3 server sends two session
tickets after each handshake, as OpenSSL's do, and the client must read and drop
them: Mbed TLS's server sends tickets only to a client that offers PSK, which
Undertow's does not, so the test writes them with Mbed TLS's own internal
handshake-message functions (ssl_misc.h), the test's only use of internals.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#define MBEDTLS_ALLOW_PRIVATE_ACCESS     /* the server's state, for its tickets */
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "psa/crypto.h"
#include "surf.h"
#include "alloc.h"
#include "undertow.h"
#include "config.h"
#include "url.h"
#include "transport.h"
#include "tls_platform.h"
#include "ssl_misc.h"                   /* third_party/mbedtls/library/: mbedtls_ssl_start_handshake_msg() */

#define NET_FD     7
#define TODAY      1791720000           /* 2026-10-11 12:00:00 UTC */
#define SEP28      1790553600           /* 2026-09-28 00:00:00: before the leaves (2026-10-01), after the floor */
#define Y2030JUN   1906502400           /* 2030-06-01 00:00:00: after the leaves (2030-01-01), before the CAs */
#define Y2004      1072915200           /* 2004-01-01: a clock whose battery ran down */
#define REQUEST    "GET / HTTP/1.1\r\nHost: fixture.test\r\n\r\n"
#define RESPONSE   "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"

static int checks, failures;
static const char *certdir = "tests/host/undertow/certs";
static char tmpdir[512];

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static int safe_line(const char *s) {
    for(; *s; s++) if((unsigned char)*s < 32 || (unsigned char)*s >= 127) return 0;
    return 1;
}

/**** the clocks ****/

static unsigned int sim_ms;
static time_t sim_rtc = TODAY;

unsigned int uptime(void) { return sim_ms; }
time_t ut_sim_time(time_t *t) { if(t) *t = sim_rtc; return sim_rtc; }
int ttyctl(int op, unsigned int arg) { (void)op; (void)arg; return -1; }

/**** randomness: a fixed sequence, failing on request (never for the test's own server) ****/

static int entropy_calls, client_entropy_calls, entropy_fail_after = -1, entropy_errno = ENOSYS, in_server,
           entropy_oversized;
static unsigned int rng = 2463534242u;

int ut_sim_getentropy(void *buf, size_t n) {
    unsigned char *p = buf;
    size_t i;
    entropy_calls++;
    if(!in_server) client_entropy_calls++;
    if(n > 256) { entropy_oversized++; errno = EIO; return -1; }
    if(!in_server && entropy_fail_after >= 0) {
        if(entropy_fail_after == 0) { errno = entropy_errno; return -1; }
        entropy_fail_after--;
    }
    for(i = 0; i < n; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        p[i] = (unsigned char)rng;
    }
    return 0;
}

/**** DNS and connecting ****/

static int dns_calls, dns_errno, connect_calls, connect_errno, closes, bad_calls;
static unsigned int dns_ip, dns_ms, connect_ip, connect_ms;
static unsigned short connect_port;
static char dns_name[300];

int net_resolve(const char *host, unsigned int *ip) {
    dns_calls++;
    snprintf(dns_name, sizeof(dns_name), "%s", host);
    sim_ms += dns_ms;
    if(dns_errno) { errno = dns_errno; return -1; }
    *ip = dns_ip;
    return 0;
}

/**** the peer: what the client's connection reaches ****/

enum { PEER_TLS, PEER_PLAIN, PEER_SILENT };
enum { AFTER_NOTHING, AFTER_CLOSE_NOTIFY, AFTER_CLOSE, AFTER_ALERT, AFTER_RESET };

/* what the next connection meets (set by a test before it connects) */
struct plan {
    int kind;
    int min_tls, max_tls;           /* the server's versions (0: Mbed TLS's defaults) */
    const char *cert, *chain, *key; /* in certdir: the leaf, an intermediate sent after it, the key */
    const uint16_t *groups;         /* the server's groups (NULL: the defaults) */
    const char *response;           /* sent once the request's blank line has arrived */
    size_t response_len;
    size_t record;                  /* TLS: in records of at most this many bytes (0: one write) */
    int after;                      /* what the peer does once the response is out */
    const char *garbage;            /* PEER_PLAIN: sends this when anything arrives, then closes */
    int close_at_once;              /* closes the connection when the first byte arrives */
    unsigned int trickle_ms;        /* the peer's bytes leave one at a time, this far apart */
    int trickle_after_handshake;    /* only what the server sends once its handshake is done */
    int stall;                      /* the peer's bytes never leave */
    int no_reading;                 /* the peer never reads what the client sends */
    size_t up_cap;                  /* the client's send buffer (0: 64 KB) */
    size_t write_max, read_max;     /* bytes per write and per read (0: no limit) */
};

static struct plan plan;

static struct {
    int open;
    unsigned char *up;              /* client to peer, not read by the peer yet */
    size_t up_len, up_cap;
    unsigned char *staged;          /* peer to client, not delivered yet */
    size_t staged_len, staged_off;
    unsigned int next_release;
    int releasing;                  /* next_release is set */
    unsigned char *down;            /* peer to client, delivered: what read() gives */
    size_t down_len, down_off;
    int peer_closed, reset;
    int trickling;
    char request[8192];
    size_t request_len;
    int responded, done;
    /* the TLS server */
    int tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    int tickets;                    /* NewSessionTicket messages the server wrote */
    int hs_done, hs_ret;
    char sni[300];
    char alpn[32];
    char version[16];
} net;

static long saved_fail;

/* the server's own work never fails on purpose: allocation and entropy failures are for the client */
static void server_begin(void) { saved_fail = ut_alloc_fail_after; ut_alloc_fail_after = -1; in_server++; }
static void server_end(void) { ut_alloc_fail_after = saved_fail; in_server--; }

static void append(unsigned char **buf, size_t *len, const void *p, size_t n) {
    *buf = realloc(*buf, *len + n + 1);
    memcpy(*buf + *len, p, n);
    *len += n;
}

static int srv_send(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    append(&net.staged, &net.staged_len, buf, len);
    return (int)len;
}

static int srv_recv(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    if(!net.up_len) return MBEDTLS_ERR_SSL_WANT_READ;
    if(len > net.up_len) len = net.up_len;
    memcpy(buf, net.up, len);
    memmove(net.up, net.up + len, net.up_len - len);
    net.up_len -= len;
    return (int)len;
}

static int srv_sni(void *ctx, mbedtls_ssl_context *ssl, const unsigned char *name, size_t len) {
    (void)ctx; (void)ssl;
    if(len >= sizeof(net.sni)) len = sizeof(net.sni) - 1;
    memcpy(net.sni, name, len);
    net.sni[len] = 0;
    return 0;
}

static unsigned char *load_file(const char *name, size_t *len) {
    char path[1024];
    FILE *f;
    unsigned char *buf = malloc(1 << 20);
    snprintf(path, sizeof(path), "%s/%s", certdir, name);
    f = fopen(path, "rb");
    if(!f) { printf("cannot open %s\n", path); exit(2); }
    *len = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    buf[*len] = 0;
    return buf;
}

static int add_cert(mbedtls_x509_crt *chain, const char *name) {
    size_t len;
    unsigned char *buf = load_file(name, &len);
    int ret = mbedtls_x509_crt_parse(chain, buf, len + 1);
    free(buf);
    return ret;
}

static const char *alpn_list[] = { "http/1.1", NULL };

/* two NewSessionTicket messages (RFC 8446 4.6.1: lifetime, age_add, nonce, ticket, extensions), sent as a TLS 1.3
   server sends them: at the end of its handshake, under the application traffic keys */
static int send_tickets(void) {
    static const unsigned char body[] = {
        0, 0, 0x1c, 0x20,  1, 2, 3, 4,  1, 7,  0, 16,  'n', 'o', 't', ' ', 'r', 'e', 's', 'u', 'm', 'e', 'd', '.', '.',
        '.', '.', '.',  0, 0
    };
    unsigned char *buf;
    size_t len;
    int i, ret;
    for(i = 0; i < 2; i++) {
        if((ret = mbedtls_ssl_start_handshake_msg(&net.ssl, MBEDTLS_SSL_HS_NEW_SESSION_TICKET, &buf, &len)) != 0)
            return ret;
        memcpy(buf, body, sizeof(body));
        if((ret = mbedtls_ssl_finish_handshake_msg(&net.ssl, len, sizeof(body))) != 0) return ret;
        net.tickets++;
    }
    return mbedtls_ssl_flush_output(&net.ssl);
}

/* the server's handshake, then, in TLS 1.3, its tickets: where a real server sends them, once the handshake is over
   (the application keys are in use) and before its state is freed */
static int server_handshake(void) {
    int ret;
    while(!mbedtls_ssl_is_handshake_over(&net.ssl))
        if((ret = mbedtls_ssl_handshake_step(&net.ssl)) != 0) return ret;
    if(net.ssl.tls_version == MBEDTLS_SSL_VERSION_TLS1_3 && !net.tickets) {
        if(!net.ssl.handshake || (ret = send_tickets()) != 0) {
            printf("the test's server could not send its tickets\n");
            return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        }
    }
    return 0;
}

static void server_free(void) {
    if(!net.tls) return;
    mbedtls_ssl_free(&net.ssl);
    mbedtls_ssl_config_free(&net.conf);
    mbedtls_x509_crt_free(&net.chain);
    mbedtls_pk_free(&net.key);
    mbedtls_ctr_drbg_free(&net.drbg);
    mbedtls_entropy_free(&net.entropy);
    net.tls = 0;
}

static void server_start(void) {
    size_t len;
    unsigned char *buf;
    int ret = 0;
    net.tls = 1;
    mbedtls_ssl_init(&net.ssl);
    mbedtls_ssl_config_init(&net.conf);
    mbedtls_x509_crt_init(&net.chain);
    mbedtls_pk_init(&net.key);
    mbedtls_entropy_init(&net.entropy);
    mbedtls_ctr_drbg_init(&net.drbg);
    ret |= mbedtls_ctr_drbg_seed(&net.drbg, mbedtls_entropy_func, &net.entropy, (const unsigned char *)"server", 6);
    ret |= add_cert(&net.chain, plan.cert);
    if(plan.chain) ret |= add_cert(&net.chain, plan.chain);
    buf = load_file(plan.key, &len);
    ret |= mbedtls_pk_parse_key(&net.key, buf, len + 1, NULL, 0, mbedtls_ctr_drbg_random, &net.drbg);
    free(buf);
    ret |= mbedtls_ssl_config_defaults(&net.conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&net.conf, mbedtls_ctr_drbg_random, &net.drbg);
    ret |= mbedtls_ssl_conf_own_cert(&net.conf, &net.chain, &net.key);
    if(plan.min_tls) mbedtls_ssl_conf_min_tls_version(&net.conf, plan.min_tls);
    if(plan.max_tls) mbedtls_ssl_conf_max_tls_version(&net.conf, plan.max_tls);
    if(plan.groups) mbedtls_ssl_conf_groups(&net.conf, plan.groups);
    ret |= mbedtls_ssl_conf_alpn_protocols(&net.conf, alpn_list);
    mbedtls_ssl_conf_sni(&net.conf, srv_sni, NULL);
    ret |= mbedtls_ssl_setup(&net.ssl, &net.conf);
    mbedtls_ssl_set_bio(&net.ssl, NULL, srv_send, srv_recv, NULL);
    if(ret) printf("the test's server did not start (%d)\n", ret);
}

/* the peer does what it can with what the client sent */
static void peer_step(void) {
    unsigned char buf[4096];
    int ret;
    if(!net.open || net.done) return;
    server_begin();
    if(plan.kind == PEER_SILENT) {
        if(!plan.no_reading) net.up_len = 0;
    } else if(plan.kind == PEER_PLAIN) {
        if(net.up_len && (plan.garbage || plan.close_at_once)) {
            if(plan.garbage) append(&net.staged, &net.staged_len, plan.garbage, strlen(plan.garbage));
            net.up_len = 0;
            net.peer_closed = 1;
            net.done = 1;
        } else if(net.up_len) {
            size_t n = net.up_len;
            if(n > sizeof(net.request) - 1 - net.request_len) n = sizeof(net.request) - 1 - net.request_len;
            memcpy(net.request + net.request_len, net.up, n);
            net.request_len += n;
            net.request[net.request_len] = 0;
            net.up_len = 0;
        }
        if(!net.responded && strstr(net.request, "\r\n\r\n")) {
            net.responded = 1;
            net.trickling = plan.trickle_ms != 0;
            append(&net.staged, &net.staged_len, plan.response, plan.response_len);
            if(plan.after == AFTER_CLOSE || plan.after == AFTER_CLOSE_NOTIFY) net.peer_closed = 1;
            if(plan.after == AFTER_RESET) net.reset = 1;
            net.done = 1;
        }
    } else {
        if(plan.close_at_once && net.up_len) {
            net.up_len = 0;
            net.peer_closed = 1;
            net.done = 1;
            server_end();
            return;
        }
        if(!net.hs_done) {
            ret = server_handshake();
            if(!ret) {
                const char *p = mbedtls_ssl_get_alpn_protocol(&net.ssl);
                net.hs_done = 1;
                snprintf(net.alpn, sizeof(net.alpn), "%s", p ? p : "");
                snprintf(net.version, sizeof(net.version), "%s", mbedtls_ssl_get_version(&net.ssl));
                if(plan.trickle_ms && plan.trickle_after_handshake) net.trickling = 1;
            } else if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                net.hs_ret = ret;
                net.peer_closed = 1;
                net.done = 1;
            }
        }
        while(net.hs_done && !net.responded) {
            ret = mbedtls_ssl_read(&net.ssl, buf, sizeof(buf));
            if(ret <= 0) break;
            if((size_t)ret > sizeof(net.request) - 1 - net.request_len) ret = (int)(sizeof(net.request) - 1 - net.request_len);
            memcpy(net.request + net.request_len, buf, (size_t)ret);
            net.request_len += (size_t)ret;
            net.request[net.request_len] = 0;
            if(strstr(net.request, "\r\n\r\n")) {
                size_t off = 0, n;
                net.responded = 1;
                while(off < plan.response_len) {
                    n = plan.record && plan.record < plan.response_len - off ? plan.record : plan.response_len - off;
                    ret = mbedtls_ssl_write(&net.ssl, (const unsigned char *)plan.response + off, n);
                    if(ret <= 0) { printf("the test's server could not write (%d)\n", ret); break; }
                    off += (size_t)ret;
                }
                if(plan.after == AFTER_CLOSE_NOTIFY) { mbedtls_ssl_close_notify(&net.ssl); net.peer_closed = 1; }
                if(plan.after == AFTER_CLOSE) net.peer_closed = 1;
                if(plan.after == AFTER_ALERT) {
                    mbedtls_ssl_send_alert_message(&net.ssl, MBEDTLS_SSL_ALERT_LEVEL_FATAL,
                                                   MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR);
                    net.peer_closed = 1;
                }
                if(plan.after == AFTER_RESET) net.reset = 1;
                net.done = 1;
            }
        }
    }
    server_end();
}

/* staged bytes become readable: all at once, one by one in time, or never */
static void release(void) {
    while(net.staged_off < net.staged_len && !plan.stall) {
        size_t n;
        if(plan.trickle_ms && (!plan.trickle_after_handshake || net.trickling)) {
            if(!net.releasing) { net.next_release = sim_ms + plan.trickle_ms; net.releasing = 1; }
            if(!ut_expired(net.next_release, sim_ms)) return;
            n = 1;
            net.next_release += plan.trickle_ms;
        } else {
            n = net.staged_len - net.staged_off;
        }
        append(&net.down, &net.down_len, net.staged + net.staged_off, n);
        net.staged_off += n;
    }
}

static int drained(void) {
    return net.staged_off == net.staged_len && net.down_off == net.down_len;
}

static short readiness(short events) {
    short r = 0;
    if(net.reset) return POLLERR | (events & (POLLIN | POLLOUT));
    if((events & POLLIN) && (net.down_off < net.down_len || (net.peer_closed && drained()))) r |= POLLIN;
    if((events & POLLOUT) && net.up_len < net.up_cap) r |= POLLOUT;
    if(net.peer_closed && drained()) r |= POLLHUP;
    return r;
}

/**** the calls transport.c makes ****/

static unsigned int polls, poll_waits;
static int poll_interrupt;

int tcp_connect(unsigned int ip, unsigned short port) {
    connect_calls++;
    connect_ip = ip;
    connect_port = port;
    sim_ms += connect_ms;
    if(connect_errno) { errno = connect_errno; return -1; }
    if(net.open) { bad_calls++; printf("  a second connection while one is open\n"); }
    free(net.up); free(net.staged); free(net.down);
    memset(&net, 0, sizeof(net));
    net.open = 1;
    net.up_cap = plan.up_cap ? plan.up_cap : 65536;
    net.up = malloc(net.up_cap);
    if(plan.kind == PEER_TLS) {
        server_begin();
        server_start();
        server_end();
    }
    return NET_FD;
}

int ut_sim_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    unsigned int until = sim_ms + (unsigned int)(timeout > 0 ? timeout : 0);
    polls++;
    if(timeout > 0) poll_waits++;
    if(nfds != 1 || fds[0].fd != NET_FD || !net.open) { bad_calls++; printf("  poll() on what is not the connection\n"); }
    if(timeout < 0) { bad_calls++; printf("  poll() without a time limit\n"); until = sim_ms + 3600000; }
    if(poll_interrupt) { poll_interrupt = 0; errno = EINTR; return -1; }
    for(;;) {
        peer_step();
        release();
        fds[0].revents = readiness(fds[0].events);
        if(fds[0].revents) return 1;
        if(!timeout) return 0;
        /* nothing yet: time passes, to the next byte of a trickle or to the end of the wait */
        if(net.releasing && net.staged_off < net.staged_len && !plan.stall &&
           ut_remaining(net.next_release, sim_ms) < ut_remaining(until, sim_ms)) {
            sim_ms = net.next_release;
            continue;
        }
        sim_ms = until;
        return 0;
    }
}

ssize_t ut_sim_read(int fd, void *buf, size_t n) {
    size_t avail;
    if(fd != NET_FD || !net.open) { bad_calls++; errno = EBADF; return -1; }
    peer_step();
    release();
    if(net.reset) { errno = ECONNRESET; return -1; }
    avail = net.down_len - net.down_off;
    if(avail) {
        if(n > avail) n = avail;
        if(plan.read_max && n > plan.read_max) n = plan.read_max;
        memcpy(buf, net.down + net.down_off, n);
        net.down_off += n;
        return (ssize_t)n;
    }
    if(net.peer_closed && drained()) return 0;
    bad_calls++;
    printf("  read() would wait: nothing to read and poll() did not say there was\n");
    errno = EAGAIN;
    return -1;
}

ssize_t ut_sim_write(int fd, const void *buf, size_t n) {
    size_t room;
    if(fd != NET_FD || !net.open) { bad_calls++; errno = EBADF; return -1; }
    if(net.reset) { errno = ECONNRESET; return -1; }
    room = net.up_cap - net.up_len;
    if(!room) {
        bad_calls++;
        printf("  write() would wait: no room and poll() did not say there was\n");
        errno = EAGAIN;
        return -1;
    }
    if(n > room) n = room;
    if(plan.write_max && n > plan.write_max) n = plan.write_max;
    memcpy(net.up + net.up_len, buf, n);
    net.up_len += n;
    return (ssize_t)n;
}

int ut_sim_close(int fd) {
    if(fd != NET_FD || !net.open) { bad_calls++; errno = EBADF; return -1; }
    closes++;
    server_begin();
    server_free();
    server_end();
    net.open = 0;
    return 0;
}

/**** helpers ****/

static struct ut_config cfg;
static char err[UT_ERR_MAX];

static void reset_world(void) {
    if(net.open) { printf("  a connection was left open\n"); bad_calls++; server_free(); }
    free(net.up); free(net.staged); free(net.down);
    memset(&net, 0, sizeof(net));
    memset(&plan, 0, sizeof(plan));
    dns_calls = connect_calls = closes = 0;
    dns_errno = connect_errno = 0;
    dns_ms = connect_ms = 0;
    dns_ip = 0x0b02000a;                        /* 10.0.2.11, network byte order */
    sim_rtc = TODAY;
    entropy_fail_after = -1;
    entropy_errno = ENOSYS;
    poll_interrupt = 0;
    err[0] = 0;
}

static void plan_tls(int min, int max, int rsa) {
    plan.kind = PEER_TLS;
    plan.min_tls = min;
    plan.max_tls = max;
    plan.cert = rsa ? "leaf-rsa.pem" : "leaf-ec.pem";
    plan.chain = rsa ? NULL : "int.pem";
    plan.key = rsa ? "leaf-rsa.key" : "leaf-ec.key";
    plan.response = RESPONSE;
    plan.response_len = strlen(RESPONSE);
    plan.after = AFTER_CLOSE_NOTIFY;
}

static void plan_plain(void) {
    plan.kind = PEER_PLAIN;
    plan.response = RESPONSE;
    plan.response_len = strlen(RESPONSE);
    plan.after = AFTER_CLOSE;
}

/* open address with the load's deadline this far away: what ut_transport_open returned */
static int open_in(const char *address, unsigned int ms, struct ut_transport **t) {
    struct ut_url *u = calloc(1, sizeof(*u));
    int r;
    if(ut_url_parse(address, u, err, sizeof(err))) {
        printf("  cannot parse %s: %s\n", address, err);
        free(u);
        return -2;
    }
    r = ut_transport_open(t, u, &cfg, sim_ms + ms, err, sizeof(err));
    free(u);
    return r;
}

static int open_at(const char *address, struct ut_transport **t) {
    return open_in(address, UT_LOAD_MS, t);
}

/* write all of req, then read until the end or an error, chunk bytes at a time; got holds what came */
static int exchange(struct ut_transport *t, const char *req, char *got, size_t cap, size_t chunk) {
    size_t off = 0, len = strlen(req), n = 0;
    int r;
    while(off < len) {
        r = ut_transport_write(t, req + off, len - off);
        if(r <= 0 || (size_t)r > len - off) return -1;
        off += (size_t)r;
    }
    for(;;) {
        size_t want = chunk < cap - 1 - n ? chunk : cap - 1 - n;
        if(!want) return -3;
        r = ut_transport_read(t, got + n, want);
        if(r <= 0) { got[n] = 0; return r; }
        if((size_t)r > want) return -4;
        n += (size_t)r;
    }
}

/* every allocation returned, every connection closed, nothing called that would have waited forever */
static void check_clean(const char *what) {
    char msg[200];
    ut_transport_cleanup();
    snprintf(msg, sizeof(msg), "%s: no memory kept (%ld blocks)", what, ut_alloc_live);
    check(ut_alloc_live == 0, msg);
    snprintf(msg, sizeof(msg), "%s: every connection closed, no call that waits forever", what);
    check(!net.open && bad_calls == 0, msg);
    bad_calls = 0;
}

/* a failed open: -1, a message that is one safe line, *out unchanged, the connection closed */
static void check_failed(int r, struct ut_transport *t, struct ut_transport *before, const char *want, const char *what) {
    char msg[400];
    snprintf(msg, sizeof(msg), "%.80s: refused (%.200s)", what, err);
    check(r == -1 && t == before, msg);
    snprintf(msg, sizeof(msg), "%.80s: the message says \"%.40s\" (%.160s)", what, want, err);
    check(strstr(err, want) != NULL && safe_line(err), msg);
    snprintf(msg, sizeof(msg), "%s: nothing left open", what);
    check(!net.open, msg);
}

/**** the tests ****/

static void plain_http(void) {
    struct ut_transport *t = NULL;
    char got[256];
    reset_world();
    plan_plain();
    check(open_at("http://fixture.test:8080/", &t) == 0 && t, "http: connects");
    check(dns_calls == 0 && connect_ip == 0x0202000a && connect_port == 8080, "the host table, no DNS, the port");
    check(!ut_transport_verified(t) && !strcmp(ut_transport_tls(t), "") && !strcmp(ut_transport_error(t), ""),
          "http: not verified, no TLS, no error");
    check(exchange(t, REQUEST, got, sizeof(got), 1000) == 0 && !strcmp(got, RESPONSE), "http: the request and the response");
    check(!strcmp(net.request, REQUEST), "http: the server got the request");
    check(ut_transport_read(t, got, 10) == 0, "http: the end stays the end");
    ut_transport_close(t);
    check(closes == 1, "http: closed");
    ut_transport_close(NULL);
    check_clean("http");

    reset_world();
    plan_plain();
    check(open_at("http://10.0.2.15/", &t) == 0 && dns_calls == 0 && connect_ip == 0x0f02000a && connect_port == 80,
          "an IPv4 literal needs no DNS");
    ut_transport_close(t);
    reset_world();
    plan_plain();
    check(open_at("http://www.example.test/", &t) == 0 && dns_calls == 1 && !strcmp(dns_name, "www.example.test") &&
          connect_ip == 0x0b02000a, "other names: DNS");
    ut_transport_close(t);
    reset_world();
    plan_plain();
    t = (struct ut_transport *)&cfg;
    check_failed(open_at("http://010.0.2.2/", &t), t, (struct ut_transport *)&cfg, "IPv4", "a number that is no IPv4 address");
    check(dns_calls == 0 && connect_calls == 0, "... is not looked up");
    check_clean("names");
}

static void dns_and_connect(void) {
    static const struct { int dns, connect; const char *want; } cases[] = {
        { ENOENT, 0, "not found" }, { ETIMEDOUT, 0, "did not answer" }, { ENETUNREACH, 0, "network" },
        { EINTR, 0, "nterrupted" }, { 0, ECONNREFUSED, "refused" }, { 0, ETIMEDOUT, "did not answer" },
        { 0, EHOSTUNREACH, "reach" }, { 0, ENETUNREACH, "network" }
    };
    struct ut_transport *t, *sentinel = (struct ut_transport *)&cfg;
    size_t i;
    char what[100];
    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset_world();
        plan_plain();
        dns_errno = cases[i].dns;
        connect_errno = cases[i].connect;
        t = sentinel;
        snprintf(what, sizeof(what), "failure %u (dns %d, connect %d)", (unsigned)i, cases[i].dns, cases[i].connect);
        check_failed(open_at("http://www.example.test/", &t), t, sentinel, cases[i].want, what);
    }
    check_clean("DNS and connect failures");

    /* a stage that needs more time than is left does not start */
    reset_world();
    plan_plain();
    t = sentinel;
    check_failed(open_in("http://www.example.test/", UT_DNS_MS - 1, &t), t, sentinel, "time", "DNS needs 5 seconds");
    check(dns_calls == 0, "... and was not asked");
    reset_world();
    plan_plain();
    dns_ms = 300;
    t = sentinel;
    check_failed(open_in("http://www.example.test/", UT_CONNECT_MS + 299, &t), t, sentinel, "time", "connect needs 10 seconds");
    check(dns_calls == 1 && connect_calls == 0, "... DNS ran, the connection did not start");
    reset_world();
    plan_plain();
    check(open_in("http://fixture.test/", UT_CONNECT_MS, &t) == 0, "exactly 10 seconds left is enough to connect");
    ut_transport_close(t);
    reset_world();
    plan_plain();
    t = sentinel;
    check_failed(open_in("http://fixture.test/", 0, &t), t, sentinel, "time", "a deadline already past");
    check(connect_calls == 0, "... nothing started");
    check_clean("stage times");
}

static void tls_handshakes(void) {
    struct ut_transport *t = NULL;
    char got[256];
    int r;
    static const struct { int min, max, rsa; const char *version; } cases[] = {
        { 0, 0, 0, "TLSv1.3" },
        { 0, MBEDTLS_SSL_VERSION_TLS1_2, 0, "TLSv1.2" },
        { 0, 0, 1, "TLSv1.3" },
        { 0, MBEDTLS_SSL_VERSION_TLS1_2, 1, "TLSv1.2" },
        { MBEDTLS_SSL_VERSION_TLS1_3, 0, 0, "TLSv1.3" }
    };
    size_t i;
    char what[100];
    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset_world();
        plan_tls(cases[i].min, cases[i].max, cases[i].rsa);
        snprintf(what, sizeof(what), "%s with %s: ", cases[i].version, cases[i].rsa ? "RSA" : "ECDSA and an intermediate");
        t = NULL;
        check(open_at("https://fixture.test/", &t) == 0 && t, what);
        if(!t) { printf("  %s\n", err); continue; }
        check(ut_transport_verified(t) && !strcmp(ut_transport_tls(t), cases[i].version), "... verified, the version");
        check(connect_port == 443 && connect_ip == 0x0202000a, "... port 443 of the host table's address");
        r = exchange(t, REQUEST, got, sizeof(got), 1000);
        if(r) printf("  %s\n", ut_transport_error(t));
        check(r == 0 && !strcmp(got, RESPONSE) && !strcmp(net.request, REQUEST),
              "... the request and the response, then close_notify is the end");
        /* (a TLS 1.3 server finishes its handshake when the client's first bytes after it arrive) */
        if(strcmp(net.sni, "fixture.test") || strcmp(net.alpn, "http/1.1") || strcmp(net.version, cases[i].version))
            printf("  the server saw SNI \"%s\", ALPN \"%s\", %s\n", net.sni, net.alpn, net.version);
        check(!strcmp(net.sni, "fixture.test") && !strcmp(net.alpn, "http/1.1") && !strcmp(net.version, cases[i].version),
              "... the server saw SNI fixture.test, ALPN http/1.1 and the version");
        check(net.tickets == (strcmp(cases[i].version, "TLSv1.3") ? 0 : 2),
              "... a TLS 1.3 server's two session tickets were read and dropped; TLS 1.2 asked for none");
        ut_transport_close(t);
    }
    check_clean("handshakes");

    reset_world();
    plan_tls(0, 0, 0);
    check(open_at("https://10.0.2.2/", &t) == 0 && ut_transport_verified(t), "an IP address is checked against the IP name");
    ut_transport_close(t);
    reset_world();
    plan_tls(0, 0, 0);
    check(open_at("https://www.fixture.test/", &t) == 0 && !strcmp(net.sni, "www.fixture.test"), "the second name");
    ut_transport_close(t);
    check_clean("names in certificates");
}

static void tls_refusals(void) {
    struct ut_transport *t, *sentinel = (struct ut_transport *)&cfg;
    static const uint16_t p521[] = { MBEDTLS_SSL_IANA_TLS_GROUP_SECP521R1, 0 };

    reset_world();
    plan_tls(0, 0, 0);
    t = sentinel;
    check_failed(open_at("https://wrong.test/", &t), t, sentinel, "not for wrong.test", "a certificate for another name");
    reset_world();
    plan_tls(0, 0, 0);
    plan.cert = "leaf-unknown.pem";
    plan.chain = NULL;
    plan.key = "leaf-unknown.key";
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "trusted issuer", "an unknown issuer");
    reset_world();
    plan_tls(0, 0, 0);
    plan.chain = NULL;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "trusted issuer", "a leaf without its intermediate");
    reset_world();
    plan_tls(0, MBEDTLS_SSL_VERSION_TLS1_2, 0);
    plan.cert = "leaf-unknown.pem";
    plan.chain = NULL;
    plan.key = "leaf-unknown.key";
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "trusted issuer", "an unknown issuer, TLS 1.2");
    reset_world();
    plan_tls(0, 0, 1);
    sim_rtc = Y2030JUN;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "expired on 2030-01-01", "an expired certificate");
    check(strstr(err, "clock") != NULL, "... the message mentions the clock");
    reset_world();
    plan_tls(0, 0, 0);
    sim_rtc = SEP28;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "not yet valid", "a certificate not valid yet");
    check(strstr(err, "2026-10-01 00:00:00 UTC") && strstr(err, "2026-09-28 00:00:00 UTC"), "... from when, and the clock");
    reset_world();
    plan_tls(0, 0, 0);
    plan.groups = p521;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "fixture.test", "no group in common");
    reset_world();
    plan_tls(0, MBEDTLS_SSL_VERSION_TLS1_2, 0);
    plan.groups = p521;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "fixture.test", "no curve in common, TLS 1.2");
    check_clean("refused certificates");

    /* a broken handshake: not TLS, or nothing */
    reset_world();
    plan.kind = PEER_PLAIN;
    plan.garbage = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "fixture.test", "an HTTP server on the HTTPS port");
    reset_world();
    plan.kind = PEER_PLAIN;
    plan.close_at_once = 1;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "closed", "a server that hangs up");
    reset_world();
    plan_tls(0, 0, 0);
    plan.close_at_once = 1;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "closed", "... a TLS one");
    check_clean("broken handshakes");
}

static void clocks(void) {
    struct ut_transport *t, *sentinel = (struct ut_transport *)&cfg;
    reset_world();
    plan_tls(0, 0, 0);
    sim_rtc = Y2004;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "clock YYYY-MM-DD HH:MM:SS", "a clock in 2004");
    check(strstr(err, "2004-01-01") != NULL && connect_calls == 0, "... says what the clock says; nothing connected");
    check(ut_transport_status(&cfg, err, sizeof(err)) == -1 && strstr(err, "clock"), "... the status says so too");
    reset_world();
    plan_plain();
    sim_rtc = Y2004;
    check(open_at("http://fixture.test/", &t) == 0, "... HTTP works all the same");
    ut_transport_close(t);

    /* the session's own time wins over the hardware clock */
    reset_world();
    plan_tls(0, 0, 0);
    sim_rtc = Y2004;
    check(ut_clock_set(&cfg.clock, "2026-10-11 02:00:00", err, sizeof(err)) == 0, "clock 2026-10-11 02:00:00");
    check(open_at("https://fixture.test/", &t) == 0 && ut_transport_verified(t), "... and HTTPS works with it");
    ut_transport_close(t);
    reset_world();
    plan_tls(0, 0, 0);
    check(ut_clock_set(&cfg.clock, "2026-09-28 00:00:00", err, sizeof(err)) == 0 &&
          open_at("https://fixture.test/", &t) == -1 && strstr(err, "not yet valid"), "... and it is the time certificates meet");
    memset(&cfg.clock, 0, sizeof(cfg.clock));

    /* an offset: the hardware clock keeps UTC-5 */
    reset_world();
    plan_tls(0, 0, 0);
    sim_rtc = 1790812800 - 5 * 3600 + 3600;     /* 2026-09-30 20:00 on a clock 5 hours behind: 2026-10-01 01:00 UTC */
    cfg.clock.rtc_offset_minutes = -300;
    check(open_at("https://fixture.test/", &t) == 0, "rtc_offset_minutes=-300 moves the clock into the leaf's dates");
    ut_transport_close(t);
    cfg.clock.rtc_offset_minutes = 0;
    reset_world();
    plan_tls(0, 0, 0);
    sim_rtc = 1790812800 - 5 * 3600 + 3600;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "not yet valid", "... without it, it is still September");
    check_clean("clocks");
}

static void entropy(void) {
    struct ut_transport *t, *sentinel = (struct ut_transport *)&cfg;
    int n, ok = 1, worked = 0;
    char what[100];
    ut_transport_cleanup();
    reset_world();
    plan_tls(0, 0, 0);
    entropy_fail_after = 0;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "RDRAND", "no RDRAND: no HTTPS");
    check(connect_calls == 0, "... before connecting");
    check(ut_transport_status(NULL, err, sizeof(err)) == -1 && strstr(err, "RDRAND"), "... the status says so too");
    reset_world();
    plan_plain();
    entropy_fail_after = 0;
    check(open_at("http://fixture.test/", &t) == 0, "... and HTTP works");
    ut_transport_close(t);
    reset_world();
    plan_tls(0, 0, 0);
    entropy_fail_after = 0;
    entropy_errno = EIO;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "random", "RDRAND failing");
    check_clean("entropy");
    /* a failure at any call: a clean refusal, never a weaker source */
    for(n = 0; n < 12; n++) {
        reset_world();
        plan_tls(0, 0, 0);
        entropy_fail_after = n;
        t = sentinel;
        if(open_at("https://fixture.test/", &t) == 0) {
            worked = 1;
            ut_transport_close(t);
        } else if(!strstr(err, "random") && !strstr(err, "RDRAND")) {
            snprintf(what, sizeof(what), "  entropy failing after %d calls: %.60s", n, err);
            printf("%s\n", what);
            ok = 0;
        }
        entropy_fail_after = -1;
        ut_transport_cleanup();
        if(ut_alloc_live) ok = 0;
    }
    check(ok && worked, "entropy failing at each call: refused cleanly until there is enough");
    check(!entropy_oversized, "getentropy() is never asked for more than 256 bytes");
    check_clean("entropy failures");
}

static void write_file(const char *path, const char *a, size_t alen, const char *b, size_t blen) {
    FILE *f = fopen(path, "wb");
    if(!f) { printf("cannot write %s\n", path); exit(2); }
    if(alen) fwrite(a, 1, alen, f);
    if(blen) fwrite(b, 1, blen, f);
    fclose(f);
}

static void roots(void) {
    struct ut_transport *t, *sentinel = (struct ut_transport *)&cfg;
    char path[1100];
    size_t len, otherlen;
    unsigned char *ca = load_file("ca.pem", &len), *other = load_file("other-ca.pem", &otherlen);
    const char *saved = ut_tls_roots_path;
    char *bad;

    snprintf(path, sizeof(path), "%s/undertow-roots-%d.pem", tmpdir, (int)getpid());

    ut_transport_cleanup();
    reset_world();
    plan_tls(0, 0, 0);
    ut_tls_roots_path = "/nonexistent/cert.pem";
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "/nonexistent/cert.pem", "no roots file");
    check(connect_calls == 0, "... nothing connected");
    check(ut_transport_status(&cfg, err, sizeof(err)) == -1 && strstr(err, "/nonexistent/cert.pem"),
          "... the status says so too");
    reset_world();
    plan_plain();
    check(open_at("http://fixture.test/", &t) == 0, "... HTTP works");
    ut_transport_close(t);

    write_file(path, "", 0, "", 0);
    ut_tls_roots_path = path;
    reset_world();
    plan_tls(0, 0, 0);
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "roots", "an empty roots file");
    write_file(path, "not a certificate\n", 18, "", 0);
    reset_world();
    plan_tls(0, 0, 0);
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "roots", "a roots file without certificates");

    /* one good root and one broken one: the good one is enough, and how many loaded is said */
    bad = malloc(otherlen + 1);
    memcpy(bad, other, otherlen);
    memcpy(bad + 100, "!!!!", 4);                       /* breaks the base64 of the second block */
    write_file(path, (char *)ca, len, bad, otherlen);
    reset_world();
    plan_tls(0, 0, 0);
    check(open_at("https://fixture.test/", &t) == 0 && ut_transport_verified(t), "a bundle with a broken root: the rest work");
    ut_transport_close(t);
    reset_world();
    plan_tls(0, 0, 0);
    plan.cert = "leaf-unknown.pem";
    plan.chain = NULL;
    plan.key = "leaf-unknown.key";
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "1 of 2", "... an untrusted issuer says how many roots loaded");
    free(bad);

    /* the welcome screen's question */
    reset_world();
    check(ut_transport_status(&cfg, err, sizeof(err)) == 0 && strstr(err, "HTTPS is ready") &&
          strstr(err, "1 trust roots (1 of the 2"), "the status says how many roots loaded and how many did not");
    check(connect_calls == 0, "... without connecting anywhere");

    /* the roots are read once per process */
    write_file(path, (char *)ca, len, "", 0);
    ut_transport_cleanup();
    reset_world();
    plan_tls(0, 0, 0);
    check(open_at("https://fixture.test/", &t) == 0, "roots from a file");
    ut_transport_close(t);
    remove(path);
    reset_world();
    plan_tls(0, 0, 0);
    check(open_at("https://fixture.test/", &t) == 0, "... read once: the file is gone and HTTPS still works");
    ut_transport_close(t);

    ut_tls_roots_path = saved;
    free(ca);
    free(other);
    check_clean("roots");
}

static void partial_io(void) {
    struct ut_transport *t;
    char got[256], big[20000];
    unsigned int waits;
    int r;

    /* partial writes and reads: 7 bytes at a time, a 64-byte send buffer */
    reset_world();
    plan_tls(0, 0, 0);
    plan.write_max = 7;
    plan.read_max = 5;
    plan.up_cap = 64;
    check(open_at("https://fixture.test/", &t) == 0, "TLS through a socket that takes 7 bytes at a time");
    check(exchange(t, REQUEST, got, sizeof(got), 1000) == 0 && !strcmp(got, RESPONSE), "... and the exchange");
    ut_transport_close(t);
    reset_world();
    plan_plain();
    plan.write_max = 7;
    check(open_at("http://fixture.test/", &t) == 0, "plain: partial writes");
    r = ut_transport_write(t, REQUEST, strlen(REQUEST));
    check(r == 7, "... a write gives how much went (7)");
    ut_transport_close(t);
    check_clean("partial I/O");

    /* bytes TLS holds come before any wait */
    reset_world();
    plan_tls(0, 0, 0);
    memset(big, 'x', sizeof(big));
    memcpy(big, "HTTP/1.1 200 OK\r\n\r\n", 19);
    plan.response = big;
    plan.response_len = sizeof(big);
    plan.record = 3000;
    check(open_at("https://fixture.test/", &t) == 0, "a response in records of 3000 bytes");
    r = ut_transport_write(t, REQUEST, strlen(REQUEST));
    check(r == (int)strlen(REQUEST), "... the request in one write");
    {
        size_t n = 0;
        char buf[100];
        int ok = 1;
        r = ut_transport_read(t, buf, 10);       /* the first record arrives */
        n += r > 0 ? (size_t)r : 0;
        waits = poll_waits;
        while(n < 3000) {
            r = ut_transport_read(t, buf, sizeof(buf));
            if(r <= 0) { ok = 0; break; }
            n += (size_t)r;
        }
        check(ok && n == 3000 && poll_waits == waits, "... the rest of a record is read without waiting");
        while((r = ut_transport_read(t, buf, sizeof(buf))) > 0) n += (size_t)r;
        check(r == 0 && n == sizeof(big), "... and all the records");
    }
    ut_transport_close(t);
    check_clean("buffered bytes");
}

static void endings(void) {
    struct ut_transport *t;
    char got[256];
    static const struct { int tls, after, want; const char *what; } cases[] = {
        { 1, AFTER_CLOSE_NOTIFY, 0, "TLS close_notify is the end" },
        { 1, AFTER_CLOSE, 0, "TLS: the connection closed without close_notify is the end too" },
        { 1, AFTER_ALERT, -1, "TLS: a fatal alert is an error" },
        { 1, AFTER_RESET, -1, "TLS: a reset is an error" },
        { 0, AFTER_CLOSE, 0, "TCP: the connection closed is the end" },
        { 0, AFTER_RESET, -1, "TCP: a reset is an error" }
    };
    size_t i;
    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int r;
        reset_world();
        if(cases[i].tls) plan_tls(0, 0, 0); else plan_plain();
        plan.after = cases[i].after;
        r = open_at(cases[i].tls ? "https://fixture.test/" : "http://fixture.test/", &t);
        if(r) { check(0, cases[i].what); printf("  %s\n", err); continue; }
        r = exchange(t, REQUEST, got, sizeof(got), 1000);
        check(r == cases[i].want && (r == 0 ? !strcmp(got, RESPONSE) && !ut_transport_error(t)[0]
                                             : ut_transport_error(t)[0] && safe_line(ut_transport_error(t))),
              cases[i].what);
        if(r) {
            check(ut_transport_read(t, got, 10) == -1 && ut_transport_write(t, "x", 1) == -1, "... and stays an error");
        }
        ut_transport_close(t);
    }
    check_clean("endings");
}

static void deadlines(void) {
    struct ut_transport *t, *sentinel = (struct ut_transport *)&cfg;
    char got[256];
    unsigned int start;
    int r;
    static char body[400];

    /* the handshake: 20 seconds, or the load's deadline when that comes first */
    reset_world();
    plan.kind = PEER_SILENT;
    start = sim_ms;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "20 seconds", "a server that never answers the handshake");
    check(sim_ms - start == UT_HANDSHAKE_MS, "... ends at exactly 20 seconds");
    reset_world();
    plan_tls(0, 0, 0);
    plan.trickle_ms = 900;
    start = sim_ms;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "20 seconds", "a handshake that trickles");
    check(sim_ms - start == UT_HANDSHAKE_MS, "... still ends at 20 seconds");
    reset_world();
    plan.kind = PEER_SILENT;
    connect_ms = 2000;
    start = sim_ms;
    t = sentinel;
    check_failed(open_in("https://fixture.test/", 12000, &t), t, sentinel, "60", "the load's deadline during the handshake");
    check(sim_ms - start == 12000, "... ends at the deadline");

    /* reading: 15 seconds without a byte */
    reset_world();
    plan_tls(0, 0, 0);
    plan.stall = 0;
    plan.response = "";
    plan.response_len = 0;
    plan.after = AFTER_NOTHING;
    check(open_at("https://fixture.test/", &t) == 0, "a server that answers nothing after the handshake");
    start = sim_ms;
    r = exchange(t, REQUEST, got, sizeof(got), 100);
    check(r == -1 && strstr(ut_transport_error(t), "15 seconds") && sim_ms - start == UT_IDLE_MS,
          "... ends after exactly 15 seconds of nothing");
    ut_transport_close(t);
    reset_world();
    plan_plain();
    plan.response_len = 0;
    plan.after = AFTER_NOTHING;
    check(open_at("http://fixture.test/", &t) == 0, "plain: a server that answers nothing");
    start = sim_ms;
    r = exchange(t, REQUEST, got, sizeof(got), 100);
    check(r == -1 && strstr(ut_transport_error(t), "15 seconds") && sim_ms - start == UT_IDLE_MS, "... 15 seconds");
    ut_transport_close(t);

    /* writing: 15 seconds without room */
    reset_world();
    plan.kind = PEER_SILENT;
    plan.no_reading = 1;
    plan.up_cap = 16;
    check(open_at("http://fixture.test/", &t) == 0, "a server that reads nothing");
    start = sim_ms;
    r = exchange(t, REQUEST, got, sizeof(got), 100);
    check(r == -1 && strstr(ut_transport_error(t), "15 seconds") && sim_ms - start == UT_IDLE_MS, "... writing ends after 15 seconds");
    ut_transport_close(t);

    /* a byte every 14.9 seconds keeps the idle timer back, never the deadline */
    memset(body, 'b', sizeof(body));
    memcpy(body, "HTTP/1.1 200 OK\r\n\r\n", 19);
    reset_world();
    plan_plain();
    plan.response = body;
    plan.response_len = sizeof(body);
    plan.trickle_ms = 14900;
    start = sim_ms;
    check(open_at("http://fixture.test/", &t) == 0, "plain: a trickle of a byte every 14.9 seconds");
    r = exchange(t, REQUEST, got, sizeof(got), 100);
    check(r == -1 && strstr(ut_transport_error(t), "60") && sim_ms - start == UT_LOAD_MS && strlen(got) == 4,
          "... ends at the 60-second deadline, after 4 bytes");
    ut_transport_close(t);
    reset_world();
    plan_tls(0, 0, 0);
    plan.response = body;
    plan.response_len = sizeof(body);
    plan.trickle_ms = 14900;
    plan.trickle_after_handshake = 1;
    start = sim_ms;
    check(open_at("https://fixture.test/", &t) == 0, "TLS: a response that trickles");
    r = exchange(t, REQUEST, got, sizeof(got), 100);
    check(r == -1 && strstr(ut_transport_error(t), "60") && sim_ms - start == UT_LOAD_MS, "... ends at the deadline");
    ut_transport_close(t);

    /* interrupted (Ctrl+C) */
    reset_world();
    plan.kind = PEER_SILENT;
    poll_interrupt = 1;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "nterrupted", "Ctrl+C during the handshake");
    check_clean("deadlines");
}

static void wrap(void) {
    struct ut_transport *t;
    char got[512];
    static char body[200];
    unsigned int start;
    int r;
    memset(body, 'w', sizeof(body));
    memcpy(body, "HTTP/1.1 200 OK\r\n\r\n", 19);

    /* uptime wraps in the middle of a response that comes a byte a second */
    reset_world();
    sim_ms = 0xffffffffu - 30000;
    plan_plain();
    plan.response = body;
    plan.response_len = 40;
    plan.trickle_ms = 1000;
    start = sim_ms;
    check(open_at("http://fixture.test/", &t) == 0, "uptime about to wrap: connected");
    r = exchange(t, REQUEST, got, sizeof(got), 100);
    check(r == 0 && strlen(got) == 40 && sim_ms < 20000 && sim_ms - start == 40000, "... the response, across the wrap");
    ut_transport_close(t);

    /* TLS records (the tickets, the response, close_notify) that arrive a byte every 100 ms across the wrap */
    reset_world();
    sim_ms = 0xffffffffu - 5000;
    plan_tls(0, 0, 0);
    plan.trickle_ms = 100;
    plan.trickle_after_handshake = 1;
    check(open_at("https://fixture.test/", &t) == 0, "TLS with uptime about to wrap");
    r = exchange(t, REQUEST, got, sizeof(got), 100);
    check(r == 0 && !strcmp(got, RESPONSE) && sim_ms > 5000 && sim_ms < 60000, "... the response, across the wrap");
    ut_transport_close(t);

    /* and a trickle past the deadline across the wrap still ends at the deadline */
    reset_world();
    sim_ms = 0xffffffffu - 30000;
    plan_plain();
    plan.response = body;
    plan.response_len = sizeof(body);
    plan.trickle_ms = 14000;
    start = sim_ms;
    check(open_at("http://fixture.test/", &t) == 0, "uptime about to wrap: a trickle");
    r = exchange(t, REQUEST, got, sizeof(got), 100);
    check(r == -1 && sim_ms - start == UT_LOAD_MS && strstr(ut_transport_error(t), "60"), "... ends at the deadline");
    ut_transport_close(t);
    sim_ms = 100000;
    check_clean("uptime wrap");
}

/* Allocations fail one after another: each time a clean failure (or a success), never a leak, a crash or a
   connection left open. Every run starts from nothing (the shared PSA, random generator and roots are made in
   it), so after it everything must be freed. A handshake makes thousands of allocations (Mbed TLS's big numbers
   grow one at a time): after the first 300, every 41st fails; UT_TRANSPORT_EXHAUSTIVE=1 in the environment makes
   it every one. refused: the connection is refused, so that only the setup runs.
   The build-time TLS 1.3 cleanup patch must free partial transforms too: no leak exceptions. */
static void allocation_failures(const char *what, const char *address, int tls, int max, int refused) {
    struct ut_transport *t, *sentinel = (struct ut_transport *)&cfg;
    struct ut_url *u = calloc(1, sizeof(*u));
    char got[256], msg[200];
    const char *all = getenv("UT_TRANSPORT_EXHAUSTIVE");
    int n, r, done = 0, bad = 0, injected = 0, step = 1, made = 0, last = -1;
    ut_url_parse(address, u, err, sizeof(err));
    for(n = 0; n < 200000 && !done; n += step) {
        step = (all && *all == '1') || n < 300 ? 1 : 41;
        ut_transport_cleanup();
        reset_world();
        if(tls) plan_tls(0, max, 0); else plan_plain();
        if(refused) connect_errno = ECONNREFUSED;
        t = sentinel;
        ut_alloc_fail_after = n;
        r = ut_transport_open(&t, u, &cfg, sim_ms + UT_LOAD_MS, err, sizeof(err));
        if(r == 0) {
            r = exchange(t, REQUEST, got, sizeof(got), 1000);
            if(r == 0 && strcmp(got, RESPONSE)) bad++;
            if(r && !ut_transport_error(t)[0]) bad++;
            ut_transport_close(t);
        } else if(t != sentinel || !err[0] || !safe_line(err)) {
            bad++;
        }
        done = ut_alloc_fail_after > 0;         /* fewer than n allocations: nothing failed */
        if(done) {
            made = n - (int)ut_alloc_fail_after;
            if(last == made) injected--;        /* exactly n allocations: the n-th never came */
        } else {
            injected++;
            last = n;
        }
        ut_alloc_fail_after = -1;
        if(net.open) bad++;
        ut_transport_cleanup();
        if(ut_alloc_live) {
            printf("  %s: failing allocation %d leaves %ld blocks (%s)\n", what, n, ut_alloc_live, err);
            bad++;
        }
    }
    free(u);
    snprintf(msg, sizeof(msg), "%s: allocation failure at %d of its %d allocations", what, injected, made);
    check(done && !bad && !bad_calls, msg);
    printf("  %s: %d allocations, %d of them made to fail", what, made, injected);
    printf("\n");
    bad_calls = 0;
    check_clean(what);
}

/* After a handshake that failed inside Mbed TLS, the shared state is made again before the next connection
   (transport.c: an allocation that fails in Mbed TLS's entropy code can leave it unusable); a refused
   certificate, the server's alert or its hanging up leave it as it is. */
static void fresh_state(void) {
    struct ut_transport *t, *sentinel = (struct ut_transport *)&cfg;
    int calls;
    ut_transport_cleanup();
    reset_world();
    plan_tls(0, 0, 0);
    check(ut_transport_status(&cfg, err, sizeof(err)) == 0 && !strcmp(err, "HTTPS is ready: strong random numbers, "
          "1 trust root"), "the status of a good setup");
    check(open_at("https://fixture.test/", &t) == 0, "the shared state is made");
    ut_transport_close(t);
    calls = client_entropy_calls;
    check(calls > 0, "... from the CPU's randomness");
    reset_world();
    plan_tls(0, 0, 0);
    t = sentinel;
    check_failed(open_at("https://wrong.test/", &t), t, sentinel, "another name", "a refused certificate");
    reset_world();
    plan_tls(0, 0, 0);
    plan.close_at_once = 1;
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "closed", "a server that hangs up");
    reset_world();
    plan_tls(0, 0, 0);
    check(open_at("https://fixture.test/", &t) == 0 && client_entropy_calls == calls,
          "... the next connection uses the same state");
    ut_transport_close(t);
    reset_world();
    plan.kind = PEER_PLAIN;
    plan.garbage = "HTTP/1.1 400 Bad Request\r\n\r\n";
    t = sentinel;
    check_failed(open_at("https://fixture.test/", &t), t, sentinel, "TLS", "a handshake broken inside Mbed TLS");
    reset_world();
    plan_tls(0, 0, 0);
    check(open_at("https://fixture.test/", &t) == 0 && client_entropy_calls > calls,
          "... the next connection makes PSA, the random generator and the roots again");
    ut_transport_close(t);
    check_clean("the shared state");
}

/* Repeated failures in one process must not accumulate state or prevent the next good handshake. */
static void repeated_failures(void) {
    struct ut_transport *t, *sentinel = (struct ut_transport *)&cfg;
    int i;
    for(i = 0; i < 20; i++) {
        ut_transport_cleanup();
        reset_world();
        plan_tls(0, 0, 0);
        t = sentinel;
        check_failed(open_at("https://wrong.test/", &t), t, sentinel, "another name", "repeated refusal");
        check_clean("repeated refused handshake frees every allocation");
    }
    reset_world();
    plan_tls(0, 0, 0);
    check(open_at("https://fixture.test/", &t) == 0, "good handshake after repeated failures");
    if(t != sentinel) ut_transport_close(t);
    check_clean("recovery handshake frees every allocation");
}

int main(int argc, char **argv) {
    const char *tmp = getenv("TMPDIR");
    static char ca[1024];
    if(argc > 1) certdir = argv[1];
    snprintf(tmpdir, sizeof(tmpdir), "%s", tmp && *tmp ? tmp : "/tmp");
    if(tmpdir[strlen(tmpdir) - 1] == '/') tmpdir[strlen(tmpdir) - 1] = 0;
    snprintf(ca, sizeof(ca), "%s/ca.pem", certdir);
    ut_tls_roots_path = ca;
    sim_ms = 100000;
    ut_config_init(&cfg);
    {
        static const char hosts[] = "host=fixture.test 10.0.2.2\nhost=www.fixture.test 10.0.2.2\nhost=wrong.test 10.0.2.2\n";
        check(ut_config_parse(&cfg, hosts, strlen(hosts), err, sizeof(err)) == 0, "the host table");
    }

    plain_http();
    dns_and_connect();
    tls_handshakes();
    tls_refusals();
    clocks();
    entropy();
    roots();
    partial_io();
    endings();
    deadlines();
    wrap();
    fresh_state();
    repeated_failures();
    allocation_failures("TLS setup (PSA, the random generator, the roots)", "https://fixture.test/", 1, 0, 1);
    allocation_failures("TLS 1.3", "https://fixture.test/", 1, 0, 0);
    allocation_failures("TLS 1.2", "https://fixture.test/", 1, MBEDTLS_SSL_VERSION_TLS1_2, 0);
    allocation_failures("plain HTTP", "http://fixture.test/", 0, 0, 0);

    ut_transport_cleanup();
    check(ut_alloc_live == 0, "everything freed at the end");
    printf("UNDERTOW TRANSPORT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
