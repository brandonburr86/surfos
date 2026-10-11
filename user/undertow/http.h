/*
SurfOS Undertow: HTTP/1.1 GET with redirects
----------------------
File: http.h  Date: 10/11/26
----------------------
One request per connection (Connection: close), Host with the port when it is
not the default, the Undertow user agent, Accept-Encoding: identity. Responses
in HTTP/1.1 or 1.0 framed by Content-Length, chunked transfer coding, or the
end of the connection. Limits: UT_HEADERS_MAX for each response's header lines,
its interim responses and trailers included (every redirect hop has its own),
UT_INTERIM_MAX interim (1xx) responses, UT_BODY_MAX for a body,
UT_REDIRECTS_MAX redirects, UT_LOAD_MS for all of it. Anything ambiguous
(Content-Length twice with different values, or with Transfer-Encoding) is
refused rather than guessed.
*/

#ifndef UNDERTOW_HTTP_H
#define UNDERTOW_HTTP_H

#include <stddef.h>
#include "url.h"
#include "config.h"

struct ut_response {
    struct ut_url url;          /* where the body came from: the last address after redirects;
                                   a Location's own fragment wins, otherwise the fragment
                                   carries over from the address before (RFC 9110 10.2.2) */
    unsigned char *body;        /* malloc'ed: len bytes, then a NUL that len does not count */
    size_t len;
    int status;                 /* the final status code, 100..599 */
    int verified;               /* 1: the final hop was HTTPS with a verified certificate */
    int redirects;              /* how many were followed */
    char tls[16];               /* the final hop's TLS version ("TLSv1.3"), "" for http: */
    char reason[64];            /* the status line's reason phrase, safe text, may be "" */
    char content_type[64];      /* the media type, lowercase, no parameters ("text/html");
                                   "" when the server sent none */
    char charset[32];           /* its charset parameter, lowercase ("utf-8"), "" when none */
};

/* GET url within UT_LOAD_MS, following redirects (301, 302, 303, 307, 308). A final response
   of any status (200, 404, 500, ...) whose body is text is a success: 0 and *out, which the
   caller frees with ut_response_free(). Text is text/html, application/xhtml+xml, text/plain
   and the other text/ types; a response without a Content-Type is text when its bytes look
   like it (no NUL, mostly printable). Everything else is -1 with a message and *out
   unchanged: no connection, TLS refused (the transport's message), a framing error, a body
   over UT_BODY_MAX, a response that is not text ("image/png is not text: Undertow shows HTML
   and plain text"), a Content-Encoding other than identity, a redirect without a usable
   Location, too many redirects, a loop, and a redirect from https: to http:, whose message
   gives the address so that the user may open it on purpose. */
int ut_fetch(const struct ut_url *url, const struct ut_config *cfg, struct ut_response *out,
             char *err, size_t cap);

/* Free what a response owns and zero it; a zeroed response, or NULL, is fine. */
void ut_response_free(struct ut_response *r);

#endif
