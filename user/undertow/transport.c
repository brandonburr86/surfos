/*
SurfOS Undertow: one connection, plain TCP or TLS
----------------------
File: transport.c  Date: 10/11/26
----------------------
transport.h is the contract. A connection is: the address (the settings' host
table, then an IPv4 literal, then DNS), tcp_connect(), and for https: a TLS 1.2
or 1.3 handshake with Mbed TLS 3.6.7 (tls_config.h) in which the server's
certificate chain must end at one of the roots in UT_CA_PATH, name the URL's
host (also sent as SNI) and be valid at ut_clock_now(); ALPN offers http/1.1.

Nothing here ever waits without a limit. Every wait is poll() with what is left
of the stage's time: DNS and connect are blocking calls with their own bounds
(5 and 10 seconds), so they start only when that much of the caller's deadline
remains; the handshake has 20 seconds; a read or a write ends after 15 seconds
without a byte moving either way, and everything ends at the caller's deadline,
however the bytes trickle. Mbed TLS reads and writes through net_recv() and
net_send(), which never wait either: poll() with no time first, WANT_READ or
WANT_WRITE when the socket is not ready, and the loops here do the waiting. Its
own computations (key exchange, signatures) cannot be interrupted, so the time
is checked again after each step.

The limits hold inside Mbed TLS's calls as well. One mbedtls_ssl_handshake(),
mbedtls_ssl_read() or mbedtls_ssl_write() goes on for as long as the socket is
ready: it skips one warning alert after another, and sends a record to its end
however few bytes each write takes. So net_recv() and net_send() check the time
before each read or write (the stage's limit) and after one that moved bytes
(the handshake's 20 seconds or the deadline: bytes that came too late are never
used). Once it has run out they return MBEDTLS_ERR_SSL_TIMEOUT, every time, and
the library's call ends with it; the loops here turn that into the same message
as their own timeout, and the connection is over.

What every connection shares, per process: PSA Crypto, a CTR_DRBG seeded from
the CPU (tls_platform.c: getentropy(), which fails on a CPU without RDRAND and
never falls back to anything weaker) and the parsed roots, made at the first
https: address and kept until ut_transport_cleanup(). A handshake that fails
inside Mbed TLS (not a refused certificate, a timeout, the server's alert or
its closing the connection) has them made anew before the next connection, once
no TLS connection is open: Mbed TLS 3.6.7 can leave an entropy context unusable
after an allocation fails inside it (its next reseed would dereference NULL,
third_party/README.md), and such a failure often reaches us only as
MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE.

10/25/26 (fetch): the end of a TLS stream remembers whether it was the server's close_notify
or the TCP connection's end alone (ut_transport_eof_clean()); reads return 0 for both, as
before.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pem.h"
#include "psa/crypto.h"
#include <surf.h>
#include "undertow.h"
#include "config.h"
#include "url.h"
#include "transport.h"
#include "tls_platform.h"

#define ROOTS_FILE_MAX (1024L * 1024)       /* a bundle bigger than this is not one */

struct ut_transport {
    int fd;                                 /* the connection, -1 once closed */
    int secure;                             /* https: every byte goes through ssl */
    int ssl_ready;                          /* ssl and conf are initialized (and are freed) */
    int handshake_done, verified;
    int eof;                                /* the end of the stream was returned */
    int notified;                           /* 10/25/26: TLS: that end was the server's close_notify */
    int failed;                             /* an error ended the connection: read and write give -1 */
    int suspect;                            /* the handshake failed inside Mbed TLS: the shared state is made again */
    unsigned int deadline;                  /* the caller's, absolute ut_now() */
    unsigned int progress;                  /* ut_now() when the last byte moved */
    unsigned int hs_until;                  /* the handshake's end, absolute ut_now() */
    int late;                               /* net_recv() or net_send() found the time run out */
    int closing;                            /* ut_transport_close()'s close_notify: one try, no limit */
    int net_errno;                          /* errno of the last failed poll, read or write */
    char host[254];
    unsigned short port;
    char tls[16];                           /* "TLSv1.3" */
    char error[UT_ERR_MAX];
    /* what certificate verification saw (verify_cb) */
    int have_leaf;
    uint32_t leaf_flags;
    mbedtls_x509_time leaf_from, leaf_to;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
};

/* the process's TLS state, made at the first https: address */
static struct {
    int psa;                                /* psa_crypto_init() succeeded */
    int drbg;                               /* entropy and drbg are seeded */
    int roots;                              /* how many roots loaded (0: none yet) */
    int roots_failed;                       /* how many of the bundle's certificates could not be read */
    int open;                               /* TLS connections open now */
    int suspect;                            /* a TLS failure since the state was made: make it again */
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr;
    mbedtls_x509_crt chain;
} tls;

/* what a server's certificates may use: SHA-2 signatures by RSA (2048 bits or more, PKCS#1 v1.5 or
   PSS) or ECDSA on P-256, P-384 and P-521; never SHA-1 (Mbed TLS reads it only for the old roots) */
static const mbedtls_x509_crt_profile cert_profile = {
    MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA256) | MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA384) |
        MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA512),
    MBEDTLS_X509_ID_FLAG(MBEDTLS_PK_RSA) | MBEDTLS_X509_ID_FLAG(MBEDTLS_PK_RSASSA_PSS) |
        MBEDTLS_X509_ID_FLAG(MBEDTLS_PK_ECDSA) | MBEDTLS_X509_ID_FLAG(MBEDTLS_PK_ECKEY),
    MBEDTLS_X509_ID_FLAG(MBEDTLS_ECP_DP_SECP256R1) | MBEDTLS_X509_ID_FLAG(MBEDTLS_ECP_DP_SECP384R1) |
        MBEDTLS_X509_ID_FLAG(MBEDTLS_ECP_DP_SECP521R1),
    2048
};

/* the handshake's signatures, the same algorithms */
static const uint16_t sig_algs[] = {
    MBEDTLS_TLS1_3_SIG_ECDSA_SECP256R1_SHA256, MBEDTLS_TLS1_3_SIG_ECDSA_SECP384R1_SHA384,
    MBEDTLS_TLS1_3_SIG_ECDSA_SECP521R1_SHA512, MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA256,
    MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA384, MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA512,
    MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA256, MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA384,
    MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA512, MBEDTLS_TLS1_3_SIG_NONE
};

/* key exchange: X25519, P-256, P-384 (P-521 is for certificates only) */
static const uint16_t groups[] = {
    MBEDTLS_SSL_IANA_TLS_GROUP_X25519, MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1, 0
};

static const char *alpn[] = { "http/1.1", NULL };

/**** small things ****/

/* the earlier of two ut_now() times, both in the future of now (or now) */
static unsigned int earlier(unsigned int a, unsigned int b, unsigned int now) {
    return ut_remaining(a, now) <= ut_remaining(b, now) ? a : b;
}

static void ip_text(unsigned int ip, char out[16]) {
    const unsigned char *b = (const unsigned char *)&ip;
    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* "a.b.c.d", four numbers 0-255 without leading zeros: 1 and *ip (network byte order). -1 for
   something that only looks like an address (all digits and dots, the last label a number: 010.0.0.1,
   1.2.3, 4294967295), which other programs read in other ways; 0 for a name. */
static int ipv4_literal(const char *s, unsigned int *ip) {
    unsigned char b[4];
    const char *last = strrchr(s, '.');
    const char *p = s;
    int i;
    last = last ? last + 1 : s;
    if(!*last || strspn(last, "0123456789") != strlen(last)) return 0;      /* a name ends with a letter */
    for(i = 0; i < 4; i++) {
        int v = 0, n = 0;
        while(*p >= '0' && *p <= '9' && n < 4) { v = v * 10 + (*p++ - '0'); n++; }
        if(!n || n > 3 || v > 255 || (n > 1 && p[-n] == '0')) return -1;
        b[i] = (unsigned char)v;
        if(i < 3 && *p++ != '.') return -1;
    }
    if(*p) return -1;
    memcpy(ip, b, 4);
    return 1;
}

/* An Mbed TLS error that is really "out of memory", in either half of a combined code */
static int out_of_memory(int ret) {
    static const int codes[] = {
        MBEDTLS_ERR_SSL_ALLOC_FAILED, MBEDTLS_ERR_X509_ALLOC_FAILED, MBEDTLS_ERR_PK_ALLOC_FAILED,
        MBEDTLS_ERR_ECP_ALLOC_FAILED, MBEDTLS_ERR_CIPHER_ALLOC_FAILED, MBEDTLS_ERR_MD_ALLOC_FAILED,
        MBEDTLS_ERR_PEM_ALLOC_FAILED, MBEDTLS_ERR_MPI_ALLOC_FAILED, MBEDTLS_ERR_ASN1_ALLOC_FAILED
    };
    int high = -((-ret) & 0xff80), low = -((-ret) & 0x007f);
    size_t i;
    if(ret == PSA_ERROR_INSUFFICIENT_MEMORY) return 1;
    for(i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
        if(ret == codes[i] || high == codes[i] || low == codes[i]) return 1;
    return 0;
}

/* why there is no strong randomness, from tls_platform.c's last getentropy() failure */
static int no_randomness(char *err, size_t cap) {
    if(ut_tls_entropy_errno() == ENOSYS)
        return ut_error(err, cap, "HTTPS needs strong random numbers and this CPU has no RDRAND instruction; "
                        "http:// addresses still work (in QEMU, use -cpu max)");
    if(ut_tls_entropy_errno() == EIO)
        return ut_error(err, cap, "HTTPS needs strong random numbers and the CPU's random number generator "
                        "(RDRAND) failed; http:// addresses still work");
    return ut_error(err, cap, "HTTPS needs strong random numbers and none could be had; http:// addresses still work");
}

/**** the state every connection shares ****/

static int load_roots(char *err, size_t cap) {
    const char *path = ut_tls_roots_path;
    unsigned char *buf;
    mbedtls_x509_crt *c;
    FILE *f;
    long size;
    size_t len;
    int ret, failed;
    f = fopen(path, "rb");
    if(!f) return ut_error(err, cap, "HTTPS cannot check certificates: %s cannot be read (%s)", path, strerror(errno));
    if(fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return ut_error(err, cap, "HTTPS cannot check certificates: %s cannot be read", path);
    }
    if(size > ROOTS_FILE_MAX) {
        fclose(f);
        return ut_error(err, cap, "HTTPS cannot check certificates: %s is over %ld bytes", path, ROOTS_FILE_MAX);
    }
    buf = malloc((size_t)size + 1);
    if(!buf) {
        fclose(f);
        return ut_error(err, cap, "out of memory for the trust roots");
    }
    len = fread(buf, 1, (size_t)size, f);
    failed = ferror(f) || len != (size_t)size;
    fclose(f);
    if(failed) {
        free(buf);
        return ut_error(err, cap, "HTTPS cannot check certificates: %s cannot be read", path);
    }
    buf[len] = 0;
    mbedtls_x509_crt_init(&tls.chain);
    ret = mbedtls_x509_crt_parse(&tls.chain, buf, len + 1);        /* PEM: the length counts the NUL */
    free(buf);
    for(tls.roots = 0, c = &tls.chain; c && c->raw.p; c = c->next) tls.roots++;
    if(ret < 0 || !tls.roots) {
        mbedtls_x509_crt_free(&tls.chain);
        tls.roots = 0;
        if(out_of_memory(ret)) return ut_error(err, cap, "out of memory for the trust roots");
        return ut_error(err, cap, "HTTPS cannot check certificates: %s has no trust roots it can read", path);
    }
    tls.roots_failed = ret;
    return 0;
}

static int tls_setup(char *err, size_t cap) {
    psa_status_t status;
    int ret;
    if(!tls.psa) {
        status = psa_crypto_init();
        if(status == PSA_ERROR_INSUFFICIENT_MEMORY) return ut_error(err, cap, "out of memory for TLS");
        if(status != PSA_SUCCESS) return no_randomness(err, cap);
        tls.psa = 1;
    }
    if(!tls.drbg) {
        mbedtls_entropy_init(&tls.entropy);
        mbedtls_ctr_drbg_init(&tls.ctr);
        ret = mbedtls_ctr_drbg_seed(&tls.ctr, mbedtls_entropy_func, &tls.entropy,
                                    (const unsigned char *)"SurfOS Undertow", 15);
        if(ret) {
            mbedtls_ctr_drbg_free(&tls.ctr);
            mbedtls_entropy_free(&tls.entropy);
            if(out_of_memory(ret)) return ut_error(err, cap, "out of memory for TLS");
            return no_randomness(err, cap);
        }
        tls.drbg = 1;
    }
    if(!tls.roots && load_roots(err, cap)) return -1;
    return 0;
}

void ut_transport_cleanup(void) {
    int open = tls.open;
    if(tls.roots) mbedtls_x509_crt_free(&tls.chain);
    if(tls.drbg) {
        mbedtls_ctr_drbg_free(&tls.ctr);
        mbedtls_entropy_free(&tls.entropy);
    }
    if(tls.psa) mbedtls_psa_crypto_free();
    memset(&tls, 0, sizeof(tls));
    tls.open = open;
}

/**** the socket under Mbed TLS: never waits ****/

/* is fd ready for events now? 1, 0, or -1 with errno */
static int ready(int fd, short events) {
    struct pollfd p;
    int n;
    p.fd = fd;
    p.events = events;
    p.revents = 0;
    n = poll(&p, 1, 0);
    if(n < 0) return -1;
    return n > 0 && p.revents != 0;
}

/* a failed poll, read or write under Mbed TLS: its code, errno kept for the message (EINTR: interrupted) */
static int net_error(struct ut_transport *t, int fallback) {
    t->net_errno = errno;
    return errno == ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET : fallback;
}

/* the same in the plain reads and writes: the message, and the connection is over */
static int io_failed(struct ut_transport *t, const char *doing) {
    t->net_errno = errno;
    if(t->net_errno == EINTR) ut_error(t->error, sizeof(t->error), "interrupted");
    else if(t->net_errno == ECONNRESET) ut_error(t->error, sizeof(t->error), "%s reset the connection", t->host);
    else ut_error(t->error, sizeof(t->error), "%s %s failed: %s", doing, t->host, strerror(t->net_errno));
    t->failed = 1;
    return -1;
}

/* Has the time for what Mbed TLS is doing run out? Before bytes move: the handshake's limit, or for reading
   and writing the deadline or 15 seconds since the last byte; once some have moved, the handshake's limit or
   the deadline. Once it has run out it stays so (late), whatever the library does after the first
   MBEDTLS_ERR_SSL_TIMEOUT. */
static int out_of_time(struct ut_transport *t, int moved) {
    unsigned int now = ut_now(), until;
    if(t->late) return 1;
    if(t->closing) return 0;
    if(!t->handshake_done) until = t->hs_until;
    else if(moved) until = t->deadline;
    else until = earlier(t->deadline, t->progress + UT_IDLE_MS, now);
    if(!ut_expired(until, now)) return 0;
    t->late = 1;
    return 1;
}

static int net_recv(void *ctx, unsigned char *buf, size_t len) {
    struct ut_transport *t = ctx;
    ssize_t n;
    int r;
    if(out_of_time(t, 0)) return MBEDTLS_ERR_SSL_TIMEOUT;
    r = ready(t->fd, POLLIN);
    if(r < 0) return net_error(t, MBEDTLS_ERR_NET_RECV_FAILED);
    if(!r) return MBEDTLS_ERR_SSL_WANT_READ;
    n = read(t->fd, buf, len > INT_MAX ? INT_MAX : len);
    if(n > 0) {
        t->progress = ut_now();
        if(out_of_time(t, 1)) return MBEDTLS_ERR_SSL_TIMEOUT;      /* too late: the bytes are never used */
        return (int)n;
    }
    if(n == 0) return 0;                    /* the other end closed: Mbed TLS calls it MBEDTLS_ERR_SSL_CONN_EOF */
    return net_error(t, MBEDTLS_ERR_NET_RECV_FAILED);
}

static int net_send(void *ctx, const unsigned char *buf, size_t len) {
    struct ut_transport *t = ctx;
    ssize_t n;
    int r;
    if(out_of_time(t, 0)) return MBEDTLS_ERR_SSL_TIMEOUT;
    r = ready(t->fd, POLLOUT);
    if(r < 0) return net_error(t, MBEDTLS_ERR_NET_SEND_FAILED);
    if(!r) return MBEDTLS_ERR_SSL_WANT_WRITE;
    n = write(t->fd, buf, len > INT_MAX ? INT_MAX : len);
    if(n > 0) {
        t->progress = ut_now();
        if(out_of_time(t, 1)) return MBEDTLS_ERR_SSL_TIMEOUT;      /* too late: the write does not count */
        return (int)n;
    }
    if(n == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return net_error(t, MBEDTLS_ERR_NET_SEND_FAILED);
}

/**** messages ****/

/* stage: what was going on when the time ran out */
enum { DURING_HANDSHAKE, DURING_READ, DURING_WRITE };

static int timed_out(struct ut_transport *t, int stage, char *err, size_t cap) {
    static const char *what[] = { "during the TLS handshake with", "while reading from", "while writing to" };
    if(ut_expired(t->deadline, ut_now()))
        return ut_error(err, cap, "the %u-second limit for loading a page ran out %s %s",
                        UT_LOAD_MS / 1000, what[stage], t->host);
    if(stage == DURING_HANDSHAKE)
        return ut_error(err, cap, "the TLS handshake with %s took more than %u seconds", t->host, UT_HANDSHAKE_MS / 1000);
    if(stage == DURING_READ)
        return ut_error(err, cap, "%s sent nothing for %u seconds", t->host, UT_IDLE_MS / 1000);
    return ut_error(err, cap, "%s accepted nothing for %u seconds", t->host, UT_IDLE_MS / 1000);
}

/* a certificate's date (Mbed TLS checked its fields when it parsed it) */
static void date_text(const mbedtls_x509_time *d, char out[32]) {
    snprintf(out, 32, "%04d-%02d-%02d %02d:%02d:%02d", d->year, d->mon, d->day, d->hour, d->min, d->sec);
}

/* why the server's certificate was refused, from Mbed TLS's flags (and the leaf, verify_cb) */
static int certificate_error(struct ut_transport *t, const struct ut_config *cfg, char *err, size_t cap) {
    uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
    const char *whose = "the certificate of", *fix = "";
    char date[32], clock[UT_CLOCK_TEXT] = "", also[96];
    int leaf_dates = t->have_leaf && (t->leaf_flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE));

    if(flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) {
        ut_clock_format(ut_clock_now(&cfg->clock), clock);
        fix = "; if that clock is wrong, set it with clock YYYY-MM-DD HH:MM:SS (UTC)";
        if(!leaf_dates) whose = "a certificate in the chain of";
    }
    if(flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
        also[0] = 0;
        if(tls.roots_failed)
            snprintf(also, sizeof(also), " (only %d of %d roots in %s could be read)",
                     tls.roots, tls.roots + tls.roots_failed, ut_tls_roots_path);
        return ut_error(err, cap, "HTTPS refused: the certificate of %s is not from a trusted issuer%s", t->host, also);
    }
    if(flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
        return ut_error(err, cap, "HTTPS refused: the certificate %s sent is not for %s but for another name",
                        t->host, t->host);
    if(flags & MBEDTLS_X509_BADCERT_EXPIRED) {
        if(leaf_dates) {
            date_text(&t->leaf_to, date);
            return ut_error(err, cap, "HTTPS refused: the certificate of %s expired on %s UTC (the clock says %s%s)",
                            t->host, date, clock, fix);
        }
        return ut_error(err, cap, "HTTPS refused: %s %s has expired (the clock says %s%s)", whose, t->host, clock, fix);
    }
    if(flags & MBEDTLS_X509_BADCERT_FUTURE) {
        if(leaf_dates) {
            date_text(&t->leaf_from, date);
            return ut_error(err, cap, "HTTPS refused: the certificate of %s is not yet valid: it starts on %s UTC "
                            "(the clock says %s%s)", t->host, date, clock, fix);
        }
        return ut_error(err, cap, "HTTPS refused: %s %s is not yet valid (the clock says %s%s)", whose, t->host, clock, fix);
    }
    if(flags & (MBEDTLS_X509_BADCERT_BAD_MD | MBEDTLS_X509_BADCERT_BAD_PK | MBEDTLS_X509_BADCERT_BAD_KEY))
        return ut_error(err, cap, "HTTPS refused: the certificate of %s uses a weak or unknown algorithm or key", t->host);
    if(flags & (MBEDTLS_X509_BADCERT_KEY_USAGE | MBEDTLS_X509_BADCERT_EXT_KEY_USAGE | MBEDTLS_X509_BADCERT_NS_CERT_TYPE))
        return ut_error(err, cap, "HTTPS refused: the certificate of %s is not one for a web server", t->host);
    return ut_error(err, cap, "HTTPS refused: the certificate of %s could not be verified (0x%x)", t->host, (unsigned)flags);
}

/* a TLS error (not WANT_READ or WANT_WRITE) as a message */
static int tls_error(struct ut_transport *t, int ret, const struct ut_config *cfg, char *err, size_t cap) {
    t->failed = 1;
    if(!t->handshake_done && ret != MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && ret != MBEDTLS_ERR_SSL_CONN_EOF &&
       ret != MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE && ret != MBEDTLS_ERR_NET_CONN_RESET &&
       ret != MBEDTLS_ERR_NET_RECV_FAILED && ret != MBEDTLS_ERR_NET_SEND_FAILED)
        t->suspect = 1;
    if(ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && cfg) return certificate_error(t, cfg, err, cap);
    if((ret == MBEDTLS_ERR_NET_RECV_FAILED || ret == MBEDTLS_ERR_NET_SEND_FAILED) && t->net_errno == EINTR)
        return ut_error(err, cap, "interrupted");
    if(out_of_memory(ret)) return ut_error(err, cap, "out of memory for TLS");
    switch(ret) {
    case MBEDTLS_ERR_SSL_CONN_EOF:
        return ut_error(err, cap, "%s closed the connection during the TLS handshake", t->host);
    case MBEDTLS_ERR_NET_CONN_RESET:
        return ut_error(err, cap, "%s reset the connection", t->host);
    case MBEDTLS_ERR_NET_RECV_FAILED:
    case MBEDTLS_ERR_NET_SEND_FAILED:
        return ut_error(err, cap, "the connection to %s failed (%s)", t->host, strerror(t->net_errno));
    case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE:
        return t->handshake_done ? ut_error(err, cap, "%s ended the connection with a TLS alert", t->host)
                                 : ut_error(err, cap, "%s refused the TLS handshake (it sent an alert)", t->host);
    case MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE:
    case MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION:
        return ut_error(err, cap, "%s and Undertow have no TLS version, cipher or key exchange in common "
                        "(Undertow speaks TLS 1.2 and 1.3)", t->host);
    case MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED:
    case MBEDTLS_ERR_ENTROPY_SOURCE_FAILED:
    case MBEDTLS_ERR_ENTROPY_NO_STRONG_SOURCE:
        return no_randomness(err, cap);
    case MBEDTLS_ERR_SSL_INVALID_RECORD:
    case MBEDTLS_ERR_SSL_INVALID_MAC:
    case MBEDTLS_ERR_SSL_DECODE_ERROR:
    case MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE:
    case MBEDTLS_ERR_SSL_UNEXPECTED_RECORD:
    case MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER:
    case MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION:
    case MBEDTLS_ERR_SSL_BAD_CERTIFICATE:
        if(t->handshake_done) return ut_error(err, cap, "%s broke the TLS protocol", t->host);
        return ut_error(err, cap, "%s does not answer in TLS (a broken handshake: is it an https server?)", t->host);
    }
    return ut_error(err, cap, "TLS with %s failed (Mbed TLS error -0x%04x)", t->host, (unsigned)-ret);
}

/**** opening ****/

static void transport_free(struct ut_transport *t) {
    if(t->suspect) tls.suspect = 1;
    if(t->ssl_ready) {
        mbedtls_ssl_free(&t->ssl);
        mbedtls_ssl_config_free(&t->conf);
    }
    if(t->fd >= 0) close(t->fd);
    free(t);
}

static int resolve(struct ut_transport *t, const struct ut_config *cfg, unsigned int *ip, char *err, size_t cap) {
    int literal;
    if(!ut_config_host(cfg, t->host, ip)) return 0;
    literal = ipv4_literal(t->host, ip);
    if(literal > 0) return 0;
    if(literal < 0)
        return ut_error(err, cap, "%s is not an IPv4 address Undertow reads: write four numbers from 0 to 255 "
                        "without leading zeros", t->host);
    if(ut_remaining(t->deadline, ut_now()) < UT_DNS_MS)
        return ut_error(err, cap, "not enough time is left to look up %s (that may take %u seconds)",
                        t->host, UT_DNS_MS / 1000);
    if(!net_resolve(t->host, ip)) return 0;
    switch(errno) {
    case ENOENT:      return ut_error(err, cap, "%s was not found (DNS)", t->host);
    case ETIMEDOUT:   return ut_error(err, cap, "the name server did not answer for %s within %u seconds",
                                      t->host, UT_DNS_MS / 1000);
    case ENETUNREACH: return ut_error(err, cap, "no network to look up %s with (is it connected? try dhcp)", t->host);
    case EINTR:       return ut_error(err, cap, "interrupted");
    case EINVAL:      return ut_error(err, cap, "%s is not a name DNS can look up", t->host);
    }
    return ut_error(err, cap, "cannot look up %s: %s", t->host, strerror(errno));
}

static int dial(struct ut_transport *t, unsigned int ip, char *err, size_t cap) {
    char addr[16];
    int fd;
    ip_text(ip, addr);
    if(ut_remaining(t->deadline, ut_now()) < UT_CONNECT_MS)
        return ut_error(err, cap, "not enough time is left to connect to %s (that may take %u seconds)",
                        t->host, UT_CONNECT_MS / 1000);
    fd = tcp_connect(ip, t->port);
    if(fd >= 0) {
        t->fd = fd;
        return 0;
    }
    switch(errno) {
    case ECONNREFUSED: return ut_error(err, cap, "%s (%s) refused the connection to port %u", t->host, addr, t->port);
    case ETIMEDOUT:    return ut_error(err, cap, "%s (%s) did not answer on port %u within %u seconds",
                                       t->host, addr, t->port, UT_CONNECT_MS / 1000);
    case EHOSTUNREACH: return ut_error(err, cap, "%s (%s) cannot be reached", t->host, addr);
    case ENETUNREACH:  return ut_error(err, cap, "no network route to %s (%s): is the network up?", t->host, addr);
    case EINTR:        return ut_error(err, cap, "interrupted");
    case ENFILE:
    case EMFILE:       return ut_error(err, cap, "too many connections are open to connect to %s", t->host);
    }
    return ut_error(err, cap, "cannot connect to %s (%s): %s", t->host, addr, strerror(errno));
}

/* wait until the socket may be ready for events or until is reached: 0, or -1 with a message (the
   caller checks the time before calling again) */
static int wait_for(struct ut_transport *t, short events, unsigned int until, char *err, size_t cap) {
    struct pollfd p;
    unsigned int left = ut_remaining(until, ut_now());
    p.fd = t->fd;
    p.events = events;
    p.revents = 0;
    if(poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left) >= 0) return 0;
    t->net_errno = errno;
    if(errno == EINTR) return ut_error(err, cap, "interrupted");
    return ut_error(err, cap, "waiting for %s failed: %s", t->host, strerror(errno));
}

/* MBEDTLS_SSL_VERIFY_REQUIRED fails the handshake on any flag; this keeps what the messages need */
static int verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    struct ut_transport *t = ctx;
    if(depth == 0) {
        t->have_leaf = 1;
        t->leaf_flags = *flags;
        t->leaf_from = crt->valid_from;
        t->leaf_to = crt->valid_to;
    }
    return 0;
}

static int handshake(struct ut_transport *t, const struct ut_config *cfg, char *err, size_t cap) {
    unsigned int until;
    const char *version;
    int ret;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    t->ssl_ready = 1;
    ret = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if(ret) return tls_error(t, ret, NULL, err, cap);       /* (out of memory, the only way it fails) */
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, &tls.chain, NULL);
    mbedtls_ssl_conf_cert_profile(&t->conf, &cert_profile);
    mbedtls_ssl_conf_sig_algs(&t->conf, sig_algs);
    mbedtls_ssl_conf_groups(&t->conf, groups);
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &tls.ctr);
    mbedtls_ssl_conf_verify(&t->conf, verify_cb, t);
    if((ret = mbedtls_ssl_conf_alpn_protocols(&t->conf, alpn)) != 0 ||
       (ret = mbedtls_ssl_setup(&t->ssl, &t->conf)) != 0 ||
       (ret = mbedtls_ssl_set_hostname(&t->ssl, t->host)) != 0)       /* the name checked and sent as SNI */
        return tls_error(t, ret, NULL, err, cap);
    mbedtls_ssl_set_bio(&t->ssl, t, net_send, net_recv, NULL);

    until = t->hs_until = earlier(t->deadline, ut_now() + UT_HANDSHAKE_MS, ut_now());
    ut_tls_set_clock(&cfg->clock);
    for(;;) {
        if(ut_expired(until, ut_now())) {
            t->failed = 1;
            ret = timed_out(t, DURING_HANDSHAKE, err, cap);
            break;
        }
        ret = mbedtls_ssl_handshake(&t->ssl);
        /* the time ran out inside the call (net_recv(), net_send()), or the call ended after it */
        if(t->late || (!ret && ut_expired(until, ut_now()))) {
            t->failed = 1;
            ret = timed_out(t, DURING_HANDSHAKE, err, cap);
            break;
        }
        if(!ret) break;
        if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ret = tls_error(t, ret, cfg, err, cap);
            break;
        }
        if(wait_for(t, ret == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, until, err, cap)) {
            t->failed = 1;
            ret = -1;
            break;
        }
    }
    ut_tls_set_clock(NULL);
    if(ret) return -1;
    t->handshake_done = 1;
    t->verified = mbedtls_ssl_get_verify_result(&t->ssl) == 0;
    if(!t->verified)                                                   /* VERIFY_REQUIRED: never reached */
        return ut_error(err, cap, "HTTPS refused: the certificate of %s was not verified", t->host);
    version = mbedtls_ssl_get_version(&t->ssl);
    snprintf(t->tls, sizeof(t->tls), "%s", version ? version : "TLS");
    return 0;
}

/* what HTTPS needs before anything goes out: a believable clock, strong randomness and the roots */
static int https_ready(const struct ut_config *cfg, char *err, size_t cap) {
    time_t now = ut_clock_now(&cfg->clock);
    if(tls.suspect && !tls.open) ut_transport_cleanup();
    if(!ut_clock_plausible(now)) {
        char when[UT_CLOCK_TEXT];
        ut_clock_format(now, when);
        return ut_error(err, cap, "HTTPS cannot check certificates: the clock says %s, which cannot be right. "
                        "Set the time (UTC) with clock YYYY-MM-DD HH:MM:SS, or correct rtc_offset_minutes in %s",
                        when, UT_CONF_PATH);
    }
    return tls_setup(err, cap);
}

int ut_transport_status(const struct ut_config *cfg, char *msg, size_t cap) {
    static const struct ut_config defaults;
    if(https_ready(cfg ? cfg : &defaults, msg, cap)) return -1;
    if(tls.roots_failed)
        ut_error(msg, cap, "HTTPS is ready: strong random numbers, %d trust roots (%d of the %d in %s could not be read)",
                 tls.roots, tls.roots_failed, tls.roots + tls.roots_failed, ut_tls_roots_path);
    else
        ut_error(msg, cap, "HTTPS is ready: strong random numbers, %d trust root%s", tls.roots, tls.roots == 1 ? "" : "s");
    return 0;
}

int ut_transport_open(struct ut_transport **out, const struct ut_url *url, const struct ut_config *cfg,
                      unsigned int deadline, char *err, size_t cap) {
    static const struct ut_config defaults;
    struct ut_transport *t;
    unsigned int ip;
    if(!out || !url) return ut_error(err, cap, "no address to connect to");
    if(!cfg) cfg = &defaults;
    if(ut_expired(deadline, ut_now()))
        return ut_error(err, cap, "the time for loading %s ran out before connecting", url->host);
    if(url->secure && https_ready(cfg, err, cap)) return -1;
    t = calloc(1, sizeof(*t));
    if(!t) return ut_error(err, cap, "out of memory for a connection");
    t->fd = -1;
    t->secure = url->secure;
    t->deadline = deadline;
    t->port = url->port;
    snprintf(t->host, sizeof(t->host), "%s", url->host);
    if(resolve(t, cfg, &ip, err, cap) || dial(t, ip, err, cap) || (t->secure && handshake(t, cfg, err, cap))) {
        transport_free(t);
        return -1;
    }
    if(t->secure) tls.open++;
    *out = t;
    return 0;
}

/**** reading and writing ****/

static int fail(struct ut_transport *t) {
    t->failed = 1;
    return -1;
}

/* the time ran out while reading or writing: the message, and the connection is over */
static int too_late(struct ut_transport *t, int stage) {
    timed_out(t, stage, t->error, sizeof(t->error));
    return fail(t);
}

int ut_transport_read(struct ut_transport *t, void *buf, size_t len) {
    int n;
    if(!t) return -1;
    if(t->failed) return -1;
    if(t->eof) return 0;
    if(!len) {
        ut_error(t->error, sizeof(t->error), "a read of no bytes");
        return -1;
    }
    if(len > INT_MAX) len = INT_MAX;
    t->progress = ut_now();
    for(;;) {
        unsigned int now = ut_now(), until = earlier(t->deadline, t->progress + UT_IDLE_MS, now);
        short event = POLLIN;
        if(ut_expired(until, now)) return too_late(t, DURING_READ);    /* nothing is returned once it is over */
        if(t->secure) {
            /* what TLS holds already comes first: mbedtls_ssl_read() returns it without reading */
            n = mbedtls_ssl_read(&t->ssl, buf, len);
            if(t->late) return too_late(t, DURING_READ);               /* it ran out inside the call */
            if(n > 0) {
                if(ut_expired(t->deadline, ut_now())) return too_late(t, DURING_READ);
                return n;
            }
            if(n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF) {
                t->eof = 1;
                t->notified = n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;     /* 0, CONN_EOF: the TCP end alone */
                return 0;
            }
            if(n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
            if(n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
                tls_error(t, n, NULL, t->error, sizeof(t->error));
                return fail(t);
            }
            if(n == MBEDTLS_ERR_SSL_WANT_WRITE) event = POLLOUT;
            now = ut_now();                 /* a record may have taken time to process */
            until = earlier(t->deadline, t->progress + UT_IDLE_MS, now);
        } else {
            int r = ready(t->fd, POLLIN);
            if(r < 0) return io_failed(t, "reading from");
            if(r) {
                ssize_t got = read(t->fd, buf, len);
                if(got > 0) {
                    if(ut_expired(t->deadline, ut_now())) return too_late(t, DURING_READ);
                    return (int)got;
                }
                if(got < 0) return io_failed(t, "reading from");
                t->eof = 1;
                return 0;
            }
        }
        if(ut_expired(until, now)) return too_late(t, DURING_READ);
        if(wait_for(t, event, until, t->error, sizeof(t->error))) return fail(t);
    }
}

int ut_transport_write(struct ut_transport *t, const void *buf, size_t len) {
    int n;
    if(!t) return -1;
    if(t->failed) return -1;
    if(!len) return 0;
    if(len > INT_MAX) len = INT_MAX;
    t->progress = ut_now();
    for(;;) {
        unsigned int now = ut_now(), until = earlier(t->deadline, t->progress + UT_IDLE_MS, now);
        short event = POLLOUT;
        if(ut_expired(until, now)) return too_late(t, DURING_WRITE);
        if(t->secure) {
            /* after WANT_WRITE Mbed TLS wants the same buffer again: this loop gives it */
            n = mbedtls_ssl_write(&t->ssl, buf, len);
            if(t->late) return too_late(t, DURING_WRITE);              /* it ran out inside the call */
            if(n > 0) {
                if(ut_expired(t->deadline, ut_now())) return too_late(t, DURING_WRITE);
                return n;
            }
            if(n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
                tls_error(t, n, NULL, t->error, sizeof(t->error));
                return fail(t);
            }
            if(n == MBEDTLS_ERR_SSL_WANT_READ) event = POLLIN;
        } else {
            int r = ready(t->fd, POLLOUT);
            if(r < 0) return io_failed(t, "writing to");
            if(r) {
                ssize_t put = write(t->fd, buf, len);
                if(put > 0) {
                    if(ut_expired(t->deadline, ut_now())) return too_late(t, DURING_WRITE);
                    return (int)put;
                }
                return io_failed(t, "writing to");
            }
        }
        now = ut_now();
        until = earlier(t->deadline, t->progress + UT_IDLE_MS, now);
        if(ut_expired(until, now)) return too_late(t, DURING_WRITE);
        if(wait_for(t, event, until, t->error, sizeof(t->error))) return fail(t);
    }
}

void ut_transport_close(struct ut_transport *t) {
    if(!t) return;
    /* a polite close_notify when the connection is still good; one try, it never waits */
    t->closing = 1;
    if(t->handshake_done && !t->failed && !t->eof) (void)mbedtls_ssl_close_notify(&t->ssl);
    if(t->secure && tls.open > 0) tls.open--;
    transport_free(t);
}

const char *ut_transport_error(const struct ut_transport *t) {
    return t ? t->error : "";
}

int ut_transport_verified(const struct ut_transport *t) {
    return t && t->verified;
}

const char *ut_transport_tls(const struct ut_transport *t) {
    return t ? t->tls : "";
}

int ut_transport_eof_clean(const struct ut_transport *t) {
    return t && t->eof && (!t->secure || t->notified);
}

int ut_transport_fd(const struct ut_transport *t) {
    return t ? t->fd : -1;
}

int ut_transport_read_now(struct ut_transport *t, void *buf, size_t len) {
    int n;
    if(!t) return -1;
    if(t->failed) return -1;
    if(t->eof) return 0;
    if(!len) {
        ut_error(t->error, sizeof(t->error), "a read of no bytes");
        return -1;
    }
    if(len > INT_MAX) len = INT_MAX;
    if(ut_expired(t->deadline, ut_now())) return too_late(t, DURING_READ);
    t->progress = ut_now();                 /* as a read that starts: the quiet before it is no stall */
    if(t->secure) {
        for(;;) {
            n = mbedtls_ssl_read(&t->ssl, buf, len);
            if(t->late) return too_late(t, DURING_READ);
            if(n > 0) return n;
            if(n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF) {
                t->eof = 1;
                t->notified = n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
                return 0;
            }
            if(n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
            if(n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) return UT_TRANSPORT_AGAIN;
            tls_error(t, n, NULL, t->error, sizeof(t->error));
            return fail(t);
        }
    } else {
        int r = ready(t->fd, POLLIN);
        ssize_t got;
        if(r < 0) return io_failed(t, "reading from");
        if(!r) return UT_TRANSPORT_AGAIN;
        got = read(t->fd, buf, len);
        if(got > 0) return (int)got;
        if(got < 0) return io_failed(t, "reading from");
        t->eof = 1;
        return 0;
    }
}
