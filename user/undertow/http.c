/*
SurfOS Undertow: HTTP/1.1 GET with redirects
----------------------
File: http.c  Date: 10/11/26
----------------------
A stand-in that fails every fetch, until track H's HTTP client lands
(docs/superpowers/plans/2026-10-11-undertow-tracks.md). The interface is http.h's.
*/

#include <stdlib.h>
#include <string.h>
#include "undertow.h"
#include "http.h"

int ut_fetch(const struct ut_url *url, const struct ut_config *cfg, struct ut_response *out,
             char *err, size_t cap) {
    (void)url; (void)cfg; (void)out;
    return ut_error(err, cap, "this build of Undertow has no HTTP client");
}

void ut_response_free(struct ut_response *r) {
    if(!r) return;
    free(r->body);
    memset(r, 0, sizeof(*r));
}
