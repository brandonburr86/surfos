/*
SurfOS Undertow: one connection, plain TCP or TLS
----------------------
File: transport.c  Date: 10/11/26
----------------------
A stand-in that refuses every connection, until track T's transport and Mbed TLS
land (docs/superpowers/plans/2026-10-11-undertow-tracks.md). The interface is
transport.h's.
*/

#include <stddef.h>
#include "undertow.h"
#include "transport.h"

int ut_transport_open(struct ut_transport **out, const struct ut_url *url, const struct ut_config *cfg,
                      unsigned int deadline, char *err, size_t cap) {
    (void)out; (void)url; (void)cfg; (void)deadline;
    return ut_error(err, cap, "this build of Undertow has no network transport");
}

int ut_transport_read(struct ut_transport *t, void *buf, size_t len) {
    (void)t; (void)buf; (void)len;
    return -1;
}

int ut_transport_write(struct ut_transport *t, const void *buf, size_t len) {
    (void)t; (void)buf; (void)len;
    return -1;
}

void ut_transport_close(struct ut_transport *t) {
    (void)t;
}

const char *ut_transport_error(const struct ut_transport *t) {
    (void)t;
    return "no network transport";
}

int ut_transport_verified(const struct ut_transport *t) {
    (void)t;
    return 0;
}

const char *ut_transport_tls(const struct ut_transport *t) {
    (void)t;
    return "";
}

void ut_transport_cleanup(void) {
}
