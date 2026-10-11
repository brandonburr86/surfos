/*
SurfOS Undertow: one connection, plain TCP or TLS
----------------------
File: transport.h  Date: 10/11/26
----------------------
http:// is a TCP connection; https:// adds TLS 1.2 or 1.3 (Mbed TLS) with the
server's certificate chain checked against the roots in UT_CA_PATH, its name
against the URL's host (also sent as SNI), its dates against ut_clock_now(), and
ALPN http/1.1. There is no way to skip any of these checks. Names resolve
through the settings' host table first, then DNS (SYS_resolve); IPv4 literals
need neither. Every wait is bounded: the stages by UT_DNS_MS, UT_CONNECT_MS and
UT_HANDSHAKE_MS, each read or write by UT_IDLE_MS without progress, and all of
it by the caller's deadline. A stage that would need more time than is left
before the deadline does not start.
*/

#ifndef UNDERTOW_TRANSPORT_H
#define UNDERTOW_TRANSPORT_H

#include <stddef.h>
#include "url.h"
#include "config.h"

struct ut_transport;                    /* opaque, on the heap */

/* Connect to url's host and port (https: and the handshake). deadline is an absolute
   ut_now() value. 0 and *out, or -1 with a message (*out unchanged): the name does not
   resolve, the connection is refused or times out, no strong randomness for TLS
   (getentropy fails: a CPU without RDRAND), no usable roots, a certificate that is not
   trusted, for another name, expired or not yet valid, a clock that is not plausible
   (ut_clock_plausible()), a failed handshake, or the deadline. */
int ut_transport_open(struct ut_transport **out, const struct ut_url *url, const struct ut_config *cfg,
                      unsigned int deadline, char *err, size_t cap);

/* Up to len bytes: how many (> 0); 0 at the end of the stream (the server closed the
   connection or, over TLS, sent close_notify); -1 on an error, a fatal TLS alert,
   UT_IDLE_MS without a byte, or the deadline (ut_transport_error() says which). Bytes
   that TLS already holds are returned before anything waits. */
int ut_transport_read(struct ut_transport *t, void *buf, size_t len);

/* Some of buf: 1..len bytes (how many), or -1 as for read. The caller writes the rest. */
int ut_transport_write(struct ut_transport *t, const void *buf, size_t len);

void ut_transport_close(struct ut_transport *t);                 /* NULL is fine */
const char *ut_transport_error(const struct ut_transport *t);    /* the last error, safe text, "" if none */
int ut_transport_verified(const struct ut_transport *t);         /* 1: TLS with a verified certificate */
const char *ut_transport_tls(const struct ut_transport *t);      /* "TLSv1.3", "TLSv1.2"; "" for http: */

/* Free what is kept between connections (the parsed roots, the random generator): at exit,
   and in tests that count allocations. */
void ut_transport_cleanup(void);

/* 10/11/26 (track T, additive): can HTTPS work in this process? For the welcome screen. 0 and a line
   in msg ("HTTPS is ready: strong random numbers, 121 trust roots"; how many could not be read when
   some could not), or -1 and why not, as ut_transport_open() would say it (the clock, no RDRAND, no
   usable roots). It reads the roots and seeds the random generator if nothing did yet. */
int ut_transport_status(const struct ut_config *cfg, char *msg, size_t cap);

#endif
