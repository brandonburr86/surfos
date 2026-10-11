/*
SurfOS Undertow: owned, bounded HTTP(S) URLs
----------------------
File: url.h  Date: 10/10/26
----------------------
*/
#ifndef UNDERTOW_URL_H
#define UNDERTOW_URL_H

#include <stddef.h>

#define UT_URL_MAX 2048

struct ut_url {
    char text[2049];
    char host[254];
    char target[2049];
    char fragment[2049];
    unsigned short port;
    int secure;
};

/* Keep these large objects on the heap. text is an absolute URL with a
 * lowercase scheme/host, decimal non-default port, and normalized path.
 * target includes the query but never the fragment. Percent escapes are
 * preserved, including encoded dots and slashes. No input is borrowed.
 * A bare address uses HTTPS. resolve uses RFC 3986 relative-reference rules.
 * Both return 0 on success, -1 on failure; out is unchanged on failure.
 * err may be NULL if cap is zero; messages are bounded and terminal-safe.
 * base must be a successfully parsed URL. out may alias base. */
int ut_url_parse(const char *input, struct ut_url *out, char *err, size_t cap);
int ut_url_resolve(const struct ut_url *base, const char *ref,
                   struct ut_url *out, char *err, size_t cap);

#endif
