/*
SurfOS Undertow: tests of the HTTP client
----------------------
File: test_undertow_http.c  Date: 10/11/26
----------------------
user/undertow/http.c against scripted servers. This file is the transport
(transport.h's calls), so http.c is tested without TCP or TLS: every response
arrives whole, a byte at a time, split at every boundary (and every pair of
boundaries for some) and in random pieces; reads fail or end at every offset;
writes are partial or fail; the clock is the test's. Covered: Content-Length,
chunked bodies with extensions and trailers, close-delimited bodies, interim
responses, 204 and 304, early EOF, malformed and overflowing sizes, duplicate
and conflicting lengths, Transfer-Encoding ambiguity, the header and body
limits at and one past them, media types and sniffing, Content-Encoding, the
request's bytes, redirects (relative, five and six, loops, https to http,
fragments), the load's one deadline, allocation failure at every allocation,
and leaks (alloc.c counts what the modules hold).
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "surf.h"
#include "alloc.h"
#include "undertow.h"
#include "url.h"
#include "config.h"
#include "transport.h"
#include "http.h"

#define LIT(s) s, sizeof(s) - 1     /* a literal and its length, NULs included */
#define MAX_CONNS 12
#define MB (1024u * 1024)

static int checks, failures;
static long loads;                  /* ut_fetch() calls, for the summary */
static unsigned int sim_ms;

unsigned int uptime(void) { return sim_ms; }
time_t ut_sim_time(time_t *t) { if(t) *t = 1790812800; return 1790812800; }
int ttyctl(int op, unsigned int arg) { (void)op; (void)arg; return -1; }

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static char why[600];               /* the first thing that went wrong in a load ("" if nothing) */

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    va_list ap;
    if(why[0]) return;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
}

/* check() with the first thing that went wrong in the load, when something did */
static void check_noted(int ok, const char *what) {
    char line[1200];
    snprintf(line, sizeof(line), "%s%s%s", what, ok ? "" : ": ", why);
    check(ok, line);
}

static int safe_message(const char *s) {
    if(!*s) return 0;
    for(; *s; s++) if((unsigned char)*s < 32 || (unsigned char)*s > 126) return 0;
    return 1;
}

/* a growing buffer for responses too big for a literal (the host's own allocator) */
struct text { char *p; size_t len, cap; };

static void add(struct text *t, const char *s, size_t n) {
    if(t->len + n + 1 > t->cap) {
        t->cap = (t->len + n + 1) * 2;
        t->p = realloc(t->p, t->cap);
        if(!t->p) { printf("test: out of memory\n"); exit(2); }
    }
    memcpy(t->p + t->len, s, n);
    t->len += n;
    t->p[t->len] = 0;
}
static void adds(struct text *t, const char *s) { add(t, s, strlen(s)); }
static void addf(struct text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void addf(struct text *t, const char *fmt, ...) {
    char b[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    adds(t, b);
}
static void addn(struct text *t, char c, size_t n) {
    char b[256];
    memset(b, c, sizeof(b));
    while(n) {
        size_t k = n < sizeof(b) ? n : sizeof(b);
        add(t, b, k);
        n -= k;
    }
}
static void text_free(struct text *t) { free(t->p); memset(t, 0, sizeof(*t)); }

/**** the scripted servers: this test's transport ****/

struct server {
    const char *expect;             /* the address the client must connect to, without fragment (NULL: any) */
    const char *open_error;         /* the connection fails with this message */
    const char *data;               /* what the server sends before it closes the connection */
    size_t len;
    long fail_at;                   /* >= 0: a read at this offset fails */
    const char *fail_msg;
    int verified;
    const char *tls;
};

static struct conn {                /* what the client did with each connection */
    char address[UT_URL_MAX + 1];
    unsigned int deadline;
    char request[8192];
    size_t request_len;
    size_t pos;                     /* bytes the client has read */
    int eofs, closed;
} conns[MAX_CONNS];

struct ut_transport { int index; char error[128]; };

static struct server servers[MAX_CONNS];
static int server_count, opened, live_conns;
static const struct ut_config *cfg_seen;
static struct ut_config cfg;

enum { WHOLE, BYTES, CUTS, RANDOM };    /* how the bytes arrive */
static int mode, ncuts;
static size_t cuts[2];
static unsigned int seed = 1;
static size_t write_max;                /* > 0: a write takes at most this many bytes */
static long write_fail_at = -1;         /* >= 0: a write at this offset of the request fails */
static unsigned int read_ms;            /* simulated time each read takes */
static unsigned int write_ms;           /* ... and each write */
static int transport_deadline;          /* 1: reads fail at the deadline, as the real transport's do */

static unsigned int rnd(void) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) & 0x7fff;
}

static void reset(void) {
    memset(servers, 0, sizeof(servers));
    server_count = 0;
    mode = WHOLE;
    ncuts = 0;
    write_max = 0;
    write_fail_at = -1;
    read_ms = 0;
    write_ms = 0;
    transport_deadline = 0;
    opened = 0;
    live_conns = 0;
}

static struct server *serve(const char *expect, const char *data, size_t len) {
    struct server *s = &servers[server_count++];
    s->expect = expect;
    s->data = data;
    s->len = len;
    s->fail_at = -1;
    return s;
}

int ut_transport_open(struct ut_transport **out, const struct ut_url *url, const struct ut_config *c,
                      unsigned int deadline, char *err, size_t cap) {
    struct ut_transport *t;
    struct server *s;
    struct conn *k;
    const char *hash = strchr(url->text, '#');
    if(opened >= server_count) {
        note("a connection to %s that the test did not expect", url->text);
        return ut_error(err, cap, "test: no server for %s", url->text);
    }
    s = &servers[opened];
    k = &conns[opened];
    memset(k, 0, sizeof(*k));
    snprintf(k->address, sizeof(k->address), "%.*s", hash ? (int)(hash - url->text) : (int)strlen(url->text),
             url->text);
    k->deadline = deadline;
    cfg_seen = c;
    if(s->expect && strcmp(k->address, s->expect))
        note("connection %d went to %s, not %s", opened, k->address, s->expect);
    opened++;
    if(s->open_error) return ut_error(err, cap, "%s", s->open_error);
    t = malloc(sizeof(*t));
    t->index = opened - 1;
    t->error[0] = 0;
    live_conns++;
    *out = t;
    return 0;
}

int ut_transport_read(struct ut_transport *t, void *buf, size_t len) {
    struct conn *k = &conns[t->index];
    struct server *s = &servers[t->index];
    size_t avail, n;
    int i;
    sim_ms += read_ms;
    if(!len) { note("a read of 0 bytes"); return -1; }
    if(k->closed) note("a read after close");
    if(transport_deadline && ut_expired(k->deadline, sim_ms)) {
        snprintf(t->error, sizeof(t->error), "the server is too slow (test transport)");
        return -1;
    }
    if(s->fail_at >= 0 && k->pos >= (size_t)s->fail_at) {
        snprintf(t->error, sizeof(t->error), "%s", s->fail_msg ? s->fail_msg : "connection reset (test)");
        return -1;
    }
    avail = s->len - k->pos;
    if(s->fail_at >= 0 && (size_t)s->fail_at - k->pos < avail) avail = (size_t)s->fail_at - k->pos;
    if(!avail) {
        if(++k->eofs > 2) note("connection %d read again and again after its end", t->index);
        return 0;
    }
    n = len < avail ? len : avail;
    if(mode == BYTES) n = 1;
    else if(mode == RANDOM) {
        size_t r = 1 + rnd() % 9;
        if(r < n) n = r;
    } else if(mode == CUTS) {
        for(i = 0; i < ncuts; i++)
            if(cuts[i] > k->pos && cuts[i] - k->pos < n) n = cuts[i] - k->pos;
    }
    memcpy(buf, s->data + k->pos, n);
    k->pos += n;
    return (int)n;
}

int ut_transport_write(struct ut_transport *t, const void *buf, size_t len) {
    struct conn *k = &conns[t->index];
    size_t n = len;
    sim_ms += write_ms;
    if(!len) { note("a write of 0 bytes"); return -1; }
    if(k->closed) note("a write after close");
    if(write_fail_at >= 0 && k->request_len >= (size_t)write_fail_at) {
        snprintf(t->error, sizeof(t->error), "broken pipe (test)");
        return -1;
    }
    if(write_max && n > write_max) n = write_max;
    if(write_fail_at >= 0 && k->request_len + n > (size_t)write_fail_at) n = (size_t)write_fail_at - k->request_len;
    if(k->request_len + n > sizeof(k->request)) { note("a request over 8 KB"); return -1; }
    memcpy(k->request + k->request_len, buf, n);
    k->request_len += n;
    return (int)n;
}

void ut_transport_close(struct ut_transport *t) {
    if(!t) return;
    if(conns[t->index].closed) note("connection %d closed twice", t->index);
    conns[t->index].closed = 1;
    live_conns--;
    free(t);
}

const char *ut_transport_error(const struct ut_transport *t) { return t ? t->error : ""; }
int ut_transport_verified(const struct ut_transport *t) { return servers[t->index].verified; }
const char *ut_transport_tls(const struct ut_transport *t) {
    return servers[t->index].tls ? servers[t->index].tls : "";
}
void ut_transport_cleanup(void) { }

/**** one load ****/

#define REQUEST_TAIL "User-Agent: " UT_USER_AGENT "\r\n" \
                     "Accept: text/html, application/xhtml+xml, text/plain;q=0.9, */*;q=0.1\r\n" \
                     "Accept-Encoding: identity\r\n" \
                     "Connection: close\r\n" \
                     "\r\n"
#define PAGE_ADDRESS "http://fixture.test/page"
#define PAGE_REQUEST "GET /page HTTP/1.1\r\nHost: fixture.test\r\n" REQUEST_TAIL

/* Load address from the servers set up (fail_after: ut_alloc_fail_after during the load).
   The checks every load must pass go to why: a failure leaves the response as it was, with
   a safe message; no connection stays open; every connection got the load's one deadline,
   ut_now() + UT_LOAD_MS when the load started, and the settings. */
static int load(const char *address, struct ut_response *r, char *err, size_t cap, long fail_after) {
    struct ut_url *u = malloc(sizeof(*u));
    struct ut_response *before = malloc(sizeof(*before));
    unsigned int start = sim_ms;
    char uerr[128];
    int rc, i;
    why[0] = 0;
    opened = 0;
    live_conns = 0;
    cfg_seen = NULL;
    if(ut_url_parse(address, u, uerr, sizeof(uerr))) {
        printf("test: %s: %s\n", address, uerr);
        exit(2);
    }
    memset(r, 0xa5, sizeof(*r));
    memcpy(before, r, sizeof(*r));
    memset(err, 0, cap);
    ut_alloc_fail_after = fail_after;
    loads++;
    rc = ut_fetch(u, &cfg, r, err, cap);
    ut_alloc_fail_after = -1;
    if(rc != 0 && rc != -1) note("ut_fetch returned %d", rc);
    if(rc == -1 && memcmp(before, r, sizeof(*r))) note("the response changed although the load failed");
    if(rc == -1 && !safe_message(err)) note("the message is empty or unsafe: \"%s\"", err);
    if(live_conns) note("%d connections left open", live_conns);
    for(i = 0; i < opened; i++)
        if(conns[i].deadline != start + UT_LOAD_MS)
            note("connection %d had the deadline %u, not %u", i, conns[i].deadline, start + UT_LOAD_MS);
    if(opened && cfg_seen != &cfg) note("the settings were not passed to the transport");
    free(u);
    free(before);
    return rc;
}

struct expect {
    int ok;                         /* 1: a page */
    int status;
    const char *body;               /* blen bytes */
    size_t blen;
    const char *ctype, *charset, *reason;   /* NULL: not checked */
    const char *msg;                /* a failure: part of its message */
};

#define PAGE(status, body, ctype, charset, reason) { 1, status, body, sizeof(body) - 1, ctype, charset, reason, NULL }
#define FAILS(msg) { 0, 0, NULL, 0, NULL, NULL, NULL, msg }

static int zeroed(const void *p, size_t n) {
    const unsigned char *b = p;
    while(n--) if(*b++) return 0;
    return 1;
}

/* Compare a load's result with e (the response is freed). 1 when all is as expected. */
static int judge(int rc, struct ut_response *r, const char *err, const struct expect *e) {
    if(e->ok) {
        if(rc) note("it failed: %s", err);
        else {
            if(r->status != e->status) note("status %d, not %d", r->status, e->status);
            if(r->len != e->blen || memcmp(r->body, e->body, e->blen))
                note("the body is \"%.40s\" (%lu bytes), not \"%.40s\" (%lu bytes)", (const char *)r->body,
                     (unsigned long)r->len, e->body, (unsigned long)e->blen);
            else if(r->body[r->len]) note("the body has no NUL after it");
            if(e->ctype && strcmp(r->content_type, e->ctype))
                note("type \"%s\", not \"%s\"", r->content_type, e->ctype);
            if(e->charset && strcmp(r->charset, e->charset)) note("charset \"%s\", not \"%s\"", r->charset, e->charset);
            if(e->reason && strcmp(r->reason, e->reason)) note("reason \"%s\", not \"%s\"", r->reason, e->reason);
            ut_response_free(r);
            if(!zeroed(r, sizeof(*r))) note("ut_response_free() left something");
        }
    } else if(!rc) {
        note("it loaded (status %d, %lu bytes) instead of failing with \"%s\"", r->status, (unsigned long)r->len,
             e->msg);
        ut_response_free(r);
    } else if(!strstr(err, e->msg)) note("the message \"%s\" does not say \"%s\"", err, e->msg);
    if(ut_alloc_live) {
        note("%ld allocations are still held", ut_alloc_live);
        ut_alloc_live = 0;
    }
    return !why[0];
}

static int run(const char *address, const struct expect *e) {
    struct ut_response *r = malloc(sizeof(*r));
    char err[UT_ERR_MAX];
    int rc = load(address, r, err, sizeof(err), -1), ok;
    ok = judge(rc, r, err, e);
    free(r);
    return ok;
}

/* the single-server load of every_way(), with its request checked */
static int run1(const char *data, size_t len, const struct expect *e, int how, size_t c1, size_t c2, int cn,
                unsigned int s) {
    reset();
    serve("http://fixture.test/page", data, len);
    mode = how;
    ncuts = cn;
    cuts[0] = c1;
    cuts[1] = c2;
    seed = s;
    if(!run(PAGE_ADDRESS, e)) return 0;
    if(conns[0].request_len != sizeof(PAGE_REQUEST) - 1 ||
       memcmp(conns[0].request, PAGE_REQUEST, conns[0].request_len)) {
        note("the request was \"%.*s\"", (int)conns[0].request_len, conns[0].request);
        return 0;
    }
    return 1;
}

/* One response: whole, a byte at a time, split at every boundary, in random pieces. */
static void every_way(const char *name, const char *data, size_t len, const struct expect *e) {
    char what[900];
    size_t k;
    unsigned int s;
    int ok;
    ok = run1(data, len, e, WHOLE, 0, 0, 0, 1);
    snprintf(what, sizeof(what), "%s (whole)%s%s", name, ok ? "" : ": ", why);
    check(ok, what);
    ok = run1(data, len, e, BYTES, 0, 0, 0, 1);
    snprintf(what, sizeof(what), "%s (a byte at a time)%s%s", name, ok ? "" : ": ", why);
    check(ok, what);
    ok = 1;
    snprintf(what, sizeof(what), "%s (split at every boundary)", name);
    for(k = 1; k < len && ok; k++)
        if(!run1(data, len, e, CUTS, k, 0, 1, 1)) {
            ok = 0;
            snprintf(what, sizeof(what), "%s (split at %lu): %s", name, (unsigned long)k, why);
        }
    check(ok, what);
    ok = 1;
    snprintf(what, sizeof(what), "%s (in random pieces)", name);
    for(s = 1; s <= 24 && ok; s++)
        if(!run1(data, len, e, RANDOM, 0, 0, 0, s)) {
            ok = 0;
            snprintf(what, sizeof(what), "%s (random pieces, seed %u): %s", name, s, why);
        }
    check(ok, what);
}

/* ... and split at every pair of boundaries */
static void every_pair(const char *name, const char *data, size_t len, const struct expect *e) {
    char what[900];
    size_t a, b;
    int ok = 1;
    snprintf(what, sizeof(what), "%s (split at every pair of boundaries)", name);
    for(a = 1; a + 1 < len && ok; a++)
        for(b = a + 1; b < len && ok; b++)
            if(!run1(data, len, e, CUTS, a, b, 2, 1)) {
                ok = 0;
                snprintf(what, sizeof(what), "%s (split at %lu and %lu): %s", name, (unsigned long)a,
                         (unsigned long)b, why);
            }
    check(ok, what);
}

struct tcase {
    const char *name, *data;
    size_t len;
    struct expect e;
};

static void table(const struct tcase *t, size_t n) {
    size_t i;
    for(i = 0; i < n; i++) every_way(t[i].name, t[i].data, t[i].len, &t[i].e);
}

#define TABLE(t) table(t, sizeof(t) / sizeof(t[0]))
#define OK_H "HTTP/1.1 200 OK\r\n"
#define PLAIN "Content-Type: text/plain\r\n"

/**** bodies framed by Content-Length ****/

static const struct tcase lengths[] = {
    { "Content-Length", LIT(OK_H "Content-Type: text/html; charset=UTF-8\r\nContent-Length: 13\r\n\r\n<p>Hello</p>\n"),
      PAGE(200, "<p>Hello</p>\n", "text/html", "utf-8", "OK") },
    { "Content-Length: what follows the body is not read",
      LIT(OK_H "Content-Length: 5\r\n" PLAIN "\r\nHelloHTTP/1.1 200 OK\r\n"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
    { "Content-Length: 0", LIT(OK_H PLAIN "Content-Length: 0\r\n\r\n"), PAGE(200, "", "text/plain", "", "OK") },
    { "Content-Length with leading zeros", LIT(OK_H PLAIN "Content-Length: 0005\r\n\r\nHello"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
    { "Content-Length twice with the same value",
      LIT(OK_H "Content-Length: 5\r\n" PLAIN "Content-Length: 5\r\n\r\nHello"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
    { "Content-Length: the same value listed twice", LIT(OK_H PLAIN "Content-Length: 5 , 5\r\n\r\nHello"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
    { "Content-Length: 5 and 05 are the same value",
      LIT(OK_H PLAIN "Content-Length: 5\r\nContent-Length: 05\r\n\r\nHello"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
    { "Content-Length twice with two values", LIT(OK_H "Content-Length: 5\r\nContent-Length: 6\r\n" PLAIN "\r\nHello!"),
      FAILS("two different Content-Length") },
    { "Content-Length: two values in one line", LIT(OK_H PLAIN "Content-Length: 6, 5\r\n\r\nHello!"),
      FAILS("two different Content-Length") },
    { "Content-Length: empty", LIT(OK_H PLAIN "Content-Length: \r\n\r\nHello"), FAILS("Content-Length") },
    { "Content-Length: +5", LIT(OK_H PLAIN "Content-Length: +5\r\n\r\nHello"), FAILS("Content-Length") },
    { "Content-Length: -5", LIT(OK_H PLAIN "Content-Length: -5\r\n\r\nHello"), FAILS("Content-Length") },
    { "Content-Length: 5x", LIT(OK_H PLAIN "Content-Length: 5x\r\n\r\nHello"), FAILS("Content-Length") },
    { "Content-Length: 0x5", LIT(OK_H PLAIN "Content-Length: 0x5\r\n\r\nHello"), FAILS("Content-Length") },
    { "Content-Length: 5 5", LIT(OK_H PLAIN "Content-Length: 5 5\r\n\r\nHello"), FAILS("Content-Length") },
    { "Content-Length: 5.0", LIT(OK_H PLAIN "Content-Length: 5.0\r\n\r\nHello"), FAILS("Content-Length") },
    { "Content-Length: 5,", LIT(OK_H PLAIN "Content-Length: 5,\r\n\r\nHello"), FAILS("Content-Length") },
    { "Content-Length: a comma alone", LIT(OK_H PLAIN "Content-Length: ,\r\n\r\nHello"), FAILS("Content-Length") },
    { "Content-Length of 20 digits", LIT(OK_H PLAIN "Content-Length: 99999999999999999999\r\n\r\nHello"),
      FAILS("over 1 MB") },
    { "Content-Length of 2^32 + 5 (5 in 32 bits)", LIT(OK_H PLAIN "Content-Length: 4294967301\r\n\r\nHello"),
      FAILS("over 1 MB") },
    { "Content-Length of 2^64 + 5 (5 in 64 bits)", LIT(OK_H PLAIN "Content-Length: 18446744073709551621\r\n\r\nHello"),
      FAILS("over 1 MB") },
    { "Content-Length of 1 MiB + 1", LIT(OK_H PLAIN "Content-Length: 1048577\r\n\r\nHello"), FAILS("over 1 MB") },
    { "Content-Length: the end before the body is complete", LIT(OK_H PLAIN "Content-Length: 10\r\n\r\nHello"),
      FAILS("closed the connection") },
};

/**** chunked bodies ****/

#define CHUNKED OK_H PLAIN "Transfer-Encoding: chunked\r\n\r\n"

static const struct tcase chunked[] = {
    { "chunked, with extensions and trailers",
      LIT(OK_H "Content-Type: text/plain; charset=us-ascii\r\nTransfer-Encoding: chunked\r\n\r\n"
          "5;name=value\r\nHello\r\n"
          "7 ; quoted=\"a;b\\\"c\" ; flag\r\n, world\r\n"
          "1\r\n!\r\n"
          "0;last=1\r\n"
          "Expires: never\r\nX-Checksum: 42\r\n\r\n"),
      PAGE(200, "Hello, world!", "text/plain", "us-ascii", "OK") },
    { "chunked: hexadecimal sizes in both cases, leading zeros",
      LIT(CHUNKED "00a\r\n0123456789\r\n00B\r\nabcdefghijk\r\n000\r\n\r\n"),
      PAGE(200, "0123456789abcdefghijk", "text/plain", "", "OK") },
    { "chunked: the coding's name in any case",
      LIT(OK_H PLAIN "Transfer-Encoding:  CHUNKED \r\n\r\n5\r\nHello\r\n0\r\n\r\n"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
    { "chunked: lines ended by a line feed alone",
      LIT("HTTP/1.1 200 OK\nContent-Type: text/plain\nTransfer-Encoding: chunked\n\n5\nHello\n0\n\n"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
    { "chunked: an empty body", LIT(CHUNKED "0\r\n\r\n"), PAGE(200, "", "text/plain", "", "OK") },
    { "chunked: one-byte chunks", LIT(CHUNKED "1\r\na\r\n1\r\nb\r\n1\r\nc\r\n0\r\n\r\n"),
      PAGE(200, "abc", "text/plain", "", "OK") },
    { "chunked: a size with many leading zeros", LIT(CHUNKED "0000000000000000000000005\r\nHello\r\n0\r\n\r\n"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
    { "chunked: spaces after a size", LIT(CHUNKED "5 \t\r\nHello\r\n0 \r\n\r\n"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
    { "chunked: an empty size line", LIT(CHUNKED "\r\nHello\r\n0\r\n\r\n"), FAILS("chunk") },
    { "chunked: a size that is not hexadecimal", LIT(CHUNKED "g\r\nHello\r\n0\r\n\r\n"), FAILS("chunk") },
    { "chunked: a negative size", LIT(CHUNKED "-5\r\nHello\r\n0\r\n\r\n"), FAILS("chunk") },
    { "chunked: a size with 0x", LIT(CHUNKED "0x5\r\nHello\r\n0\r\n\r\n"), FAILS("chunk") },
    { "chunked: a size followed by junk", LIT(CHUNKED "5x\r\nHello\r\n0\r\n\r\n"), FAILS("chunk") },
    { "chunked: a space before a size", LIT(CHUNKED " 5\r\nHello\r\n0\r\n\r\n"), FAILS("chunk") },
    { "chunked: a size of ffffffffffffffff", LIT(CHUNKED "ffffffffffffffff\r\nHello\r\n0\r\n\r\n"),
      FAILS("over 1 MB") },
    { "chunked: a size of 2^64 + 5 (5 in 64 bits)", LIT(CHUNKED "10000000000000005\r\nHello\r\n0\r\n\r\n"),
      FAILS("over 1 MB") },
    { "chunked: a size of 2^32 + 5 (5 in 32 bits)", LIT(CHUNKED "100000005\r\nHello\r\n0\r\n\r\n"),
      FAILS("over 1 MB") },
    { "chunked: a chunk of 1 MiB + 1", LIT(CHUNKED "100001\r\nHello\r\n0\r\n\r\n"), FAILS("over 1 MB") },
    { "chunked: data longer than its size", LIT(CHUNKED "5\r\nHelloX\r\n0\r\n\r\n"), FAILS("chunk") },
    { "chunked: a CR alone after the data", LIT(CHUNKED "5\r\nHello\rX0\r\n\r\n"), FAILS("chunk") },
    { "chunked: the end in a size", LIT(CHUNKED "5"), FAILS("closed the connection") },
    { "chunked: the end in the data", LIT(CHUNKED "5\r\nHel"), FAILS("closed the connection") },
    { "chunked: the end after the data", LIT(CHUNKED "5\r\nHello"), FAILS("closed the connection") },
    { "chunked: the end before the last chunk", LIT(CHUNKED "5\r\nHello\r\n"), FAILS("closed the connection") },
    { "chunked: the end after the last chunk", LIT(CHUNKED "5\r\nHello\r\n0\r\n"), FAILS("closed the connection") },
    { "chunked: the end in the trailers", LIT(CHUNKED "5\r\nHello\r\n0\r\nX-A: 1\r\n"),
      FAILS("closed the connection") },
    { "chunked: a folded trailer", LIT(CHUNKED "5\r\nHello\r\n0\r\nX-A: 1\r\n 2\r\n\r\n"), FAILS("folded") },
    { "chunked: a trailer without a colon", LIT(CHUNKED "5\r\nHello\r\n0\r\nno colon\r\n\r\n"), FAILS("trailer") },
};

/**** Transfer-Encoding that is ambiguous or that Undertow does not read ****/

static const struct tcase codings[] = {
    { "Transfer-Encoding with Content-Length",
      LIT(OK_H PLAIN "Transfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n5\r\nHello\r\n0\r\n\r\n"),
      FAILS("ambiguous") },
    { "Content-Length with Transfer-Encoding",
      LIT(OK_H PLAIN "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n0\r\n\r\n"),
      FAILS("ambiguous") },
    { "Transfer-Encoding: gzip", LIT(OK_H PLAIN "Transfer-Encoding: gzip\r\n\r\nHello"), FAILS("gzip") },
    { "Transfer-Encoding: gzip, chunked",
      LIT(OK_H PLAIN "Transfer-Encoding: gzip, chunked\r\n\r\n5\r\nHello\r\n0\r\n\r\n"),
      FAILS("gzip, chunked") },
    { "Transfer-Encoding: chunked, gzip",
      LIT(OK_H PLAIN "Transfer-Encoding: chunked, gzip\r\n\r\n5\r\nHello\r\n0\r\n\r\n"),
      FAILS("chunked, gzip") },
    { "Transfer-Encoding: chunked, chunked",
      LIT(OK_H PLAIN "Transfer-Encoding: chunked, chunked\r\n\r\n5\r\nHello\r\n0\r\n\r\n"), FAILS("chunked, chunked") },
    { "Transfer-Encoding: chunked on two lines",
      LIT(OK_H PLAIN "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n0\r\n\r\n"),
      FAILS("chunked, chunked") },
    { "Transfer-Encoding: identity", LIT(OK_H PLAIN "Transfer-Encoding: identity\r\n\r\nHello"), FAILS("identity") },
    { "Transfer-Encoding: empty", LIT(OK_H PLAIN "Transfer-Encoding: \r\n\r\nHello"),
      FAILS("empty Transfer-Encoding") },
    { "Transfer-Encoding: chunked, and an empty line",
      LIT(OK_H PLAIN "Transfer-Encoding: chunked\r\nTransfer-Encoding:\r\n\r\n5\r\nHello\r\n0\r\n\r\n"),
      FAILS("empty Transfer-Encoding") },
    { "Transfer-Encoding: a list of commas", LIT(OK_H PLAIN "Transfer-Encoding: , ,\r\n\r\nHello"),
      FAILS("empty Transfer-Encoding") },
    { "Transfer-Encoding: chunked with a parameter",
      LIT(OK_H PLAIN "Transfer-Encoding: chunked;x=1\r\n\r\n5\r\nHello\r\n0\r\n\r\n"), FAILS("Transfer-Encoding") },
    { "Transfer-Encoding in an HTTP/1.0 response",
      LIT("HTTP/1.0 200 OK\r\n" PLAIN "Transfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n0\r\n\r\n"), FAILS("HTTP/1.0") },
    { "Content-Encoding: gzip", LIT(OK_H PLAIN "Content-Encoding: gzip\r\nContent-Length: 5\r\n\r\nHello"),
      FAILS("gzip") },
    { "Content-Encoding: br, gzip", LIT(OK_H PLAIN "Content-Encoding: br, gzip\r\nContent-Length: 5\r\n\r\nHello"),
      FAILS("br, gzip") },
    { "Content-Encoding: identity, gzip",
      LIT(OK_H PLAIN "Content-Encoding: identity, gzip\r\nContent-Length: 5\r\n\r\nHello"),
      FAILS("gzip") },
    { "Content-Encoding: identity", LIT(OK_H PLAIN "Content-Encoding: identity\r\nContent-Length: 5\r\n\r\nHello"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
    { "Content-Encoding: empty", LIT(OK_H PLAIN "Content-Encoding:\r\nContent-Length: 5\r\n\r\nHello"),
      PAGE(200, "Hello", "text/plain", "", "OK") },
};

/**** bodies that end with the connection, interim responses, responses without a body ****/

#define C100 "HTTP/1.1 100 Continue\r\n\r\n"

static const struct tcase others[] = {
    { "HTTP/1.0, the body ends with the connection", LIT("HTTP/1.0 200 OK\r\n" PLAIN "\r\nHello, 1.0"),
      PAGE(200, "Hello, 1.0", "text/plain", "", "OK") },
    { "HTTP/1.1, the body ends with the connection", LIT(OK_H PLAIN "\r\nUntil the end\n"),
      PAGE(200, "Until the end\n", "text/plain", "", "OK") },
    { "the connection ends at once after the headers", LIT(OK_H PLAIN "\r\n"), PAGE(200, "", "text/plain", "", "OK") },
    { "100 Continue and 103 Early Hints",
      LIT(C100 "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n"
          OK_H PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "OK") },
    { "8 interim responses", LIT(C100 C100 C100 C100 C100 C100 C100 C100 OK_H PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "OK") },
    { "9 interim responses", LIT(C100 C100 C100 C100 C100 C100 C100 C100 C100 OK_H PLAIN "Content-Length: 2\r\n\r\nok"),
      FAILS("interim") },
    { "101 Switching Protocols", LIT("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n"), FAILS("101") },
    { "an interim response, then the end", LIT(C100), FAILS("closed the connection") },
    { "an interim response's framing headers are ignored",
      LIT("HTTP/1.1 100 Continue\r\nContent-Length: 5\r\nTransfer-Encoding: gzip\r\n\r\n"
          OK_H PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "OK") },
    { "204 No Content", LIT("HTTP/1.1 204 No Content\r\n\r\n"), PAGE(204, "", "", "", "No Content") },
    { "204 with a Content-Length and bytes after it", LIT("HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\nHello"),
      PAGE(204, "", "", "", "No Content") },
    { "304 with framing headers",
      LIT("HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\nTransfer-Encoding: chunked\r\n"
          "Content-Type: text/html\r\n\r\n"), PAGE(304, "", "text/html", "", "Not Modified") },
    { "100 Continue, then 204", LIT(C100 "HTTP/1.1 204 No Content\r\n\r\n"), PAGE(204, "", "", "", "No Content") },
    { "204 with a Content-Encoding: there is no body to decode",
      LIT("HTTP/1.1 204 No Content\r\nContent-Encoding: gzip\r\n\r\n"), PAGE(204, "", "", "", "No Content") },
    { "204 with an image's type: there is no body to show",
      LIT("HTTP/1.1 204 No Content\r\nContent-Type: image/png\r\n\r\n"), PAGE(204, "", "image/png", "", "No Content") },
    { "304 with a Content-Encoding", LIT("HTTP/1.1 304 Not Modified\r\nContent-Encoding: gzip\r\n\r\n"),
      PAGE(304, "", "", "", "Not Modified") },
    { "304 with an image's type", LIT("HTTP/1.1 304 Not Modified\r\nContent-Type: image/png\r\n\r\n"),
      PAGE(304, "", "image/png", "", "Not Modified") },
};

/**** status lines and header lines ****/

static const struct tcase lines[] = {
    { "HTTP/1.0 with a Content-Length", LIT("HTTP/1.0 200 OK\r\n" PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "OK") },
    { "a status line without a reason", LIT("HTTP/1.1 200\r\n" PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "") },
    { "a status line with an empty reason", LIT("HTTP/1.1 200 \r\n" PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "") },
    { "HTTP/1.2 is read as HTTP/1.1", LIT("HTTP/1.2 200 OK\r\n" PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "OK") },
    { "a reason with escape sequences",
      LIT("HTTP/1.1 404 Not \x1b[31mFound\xc2\x9b" "31m\r\nContent-Type: text/html\r\nContent-Length: 2\r\n\r\nno"),
      PAGE(404, "no", "text/html", "", "Not [31mFound31m") },
    { "a reason in Latin-1", LIT("HTTP/1.1 200 Tr\xe8s bien\r\n" PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "Tr\xef\xbf\xbds bien") },
    { "a reason with a tab", LIT("HTTP/1.1 200 A\tB\r\n" PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "A B") },
    { "404 with HTML",
      LIT("HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: 17\r\n\r\n<h1>Not here</h1>"),
      PAGE(404, "<h1>Not here</h1>", "text/html", "", "Not Found") },
    { "500 with plain text", LIT("HTTP/1.1 500 Internal Server Error\r\n" PLAIN "\r\nbroken"),
      PAGE(500, "broken", "text/plain", "", "Internal Server Error") },
    { "nothing at all", LIT(""), FAILS("closed the connection") },
    { "the end in the status line", LIT("HTTP/1.1 20"), FAILS("closed the connection") },
    { "the end in the headers", LIT(OK_H "Content-Le"), FAILS("closed the connection") },
    { "the end before the empty line", LIT(OK_H "Content-Length: 2\r\n"), FAILS("closed the connection") },
    { "HTTP/2", LIT("HTTP/2 200 OK\r\n\r\n"), FAILS("status line") },
    { "HTTP/2.0", LIT("HTTP/2.0 200 OK\r\n\r\n"), FAILS("status line") },
    { "HTTP/1.x", LIT("HTTP/1.x 200 OK\r\n\r\n"), FAILS("status line") },
    { "HTTP/11.1", LIT("HTTP/11.1 200 OK\r\n\r\n"), FAILS("status line") },
    { "a two-digit status", LIT("HTTP/1.1 20 OK\r\n\r\n"), FAILS("status line") },
    { "a four-digit status", LIT("HTTP/1.1 2000 OK\r\n\r\n"), FAILS("status line") },
    { "status 099", LIT("HTTP/1.1 099 Low\r\n\r\n"), FAILS("status line") },
    { "status 600", LIT("HTTP/1.1 600 High\r\n\r\n"), FAILS("status line") },
    { "a status with a letter", LIT("HTTP/1.1 2O0 OK\r\n\r\n"), FAILS("status line") },
    { "lowercase http/1.1", LIT("http/1.1 200 OK\r\n\r\n"), FAILS("status line") },
    { "no space after the status", LIT("HTTP/1.1 200OK\r\n\r\n"), FAILS("status line") },
    { "two spaces before the status", LIT("HTTP/1.1  200 OK\r\n\r\n"), FAILS("status line") },
    { "a CR alone in the status line", LIT("HTTP/1.1 200 O\rK\r\n\r\n"), FAILS("status line") },
    { "a NUL in the status line", LIT("HTTP/1.1 200 O\0K\r\n\r\n"), FAILS("status line") },
    { "an answer that is not HTTP", LIT("ICY 200 OK\r\n\r\n"), FAILS("status line") },
    { "HTML without a status line", LIT("<html><body>hi</body></html>"), FAILS("status line") },
    { "obsolete line folding", LIT(OK_H "X-Long: a\r\n b\r\n" PLAIN "Content-Length: 2\r\n\r\nok"), FAILS("folded") },
    { "folding with a tab", LIT(OK_H "X-Long: a\r\n\tb\r\n" PLAIN "Content-Length: 2\r\n\r\nok"), FAILS("folded") },
    { "a space before the first header", LIT(OK_H " Content-Length: 2\r\n" PLAIN "\r\nok"), FAILS("folded") },
    { "a header line without a colon", LIT(OK_H "Content-Length 2\r\n" PLAIN "\r\nok"), FAILS("header line") },
    { "a space before the colon", LIT(OK_H "Content-Length : 2\r\n" PLAIN "\r\nok"), FAILS("header line") },
    { "an empty header name", LIT(OK_H ": x\r\n" PLAIN "Content-Length: 2\r\n\r\nok"), FAILS("header line") },
    { "a NUL in a header", LIT(OK_H "X-A: a\0b\r\n" PLAIN "Content-Length: 2\r\n\r\nok"), FAILS("header line") },
    { "a CR alone in a header", LIT(OK_H "X-A: a\rb\r\n" PLAIN "Content-Length: 2\r\n\r\nok"), FAILS("header line") },
    { "lines ended by a line feed alone", LIT("HTTP/1.1 200 OK\nContent-Type: text/plain\nContent-Length: 2\n\nok"),
      PAGE(200, "ok", "text/plain", "", "OK") },
    { "header names in any case", LIT(OK_H "CONTENT-LENGTH: 2\r\ncontent-type: TEXT/PLAIN\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "OK") },
    { "spaces and tabs around values", LIT(OK_H "Content-Length:\t 2 \t\r\nContent-Type:text/plain  \r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "OK") },
    { "controls and Latin-1 in a value that is ignored",
      LIT(OK_H "X-Junk: a\x01" "b\x7f\r\nX-Latin: caf\xe9\r\n" PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "OK") },
    { "a header name of every token character",
      LIT(OK_H "X!#$%&'*+-.^_`|~09az: v\r\n" PLAIN "Content-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "", "OK") },
};

/**** media types ****/

#define PNG "\x89PNG\r\n\x1a\n\0\0\0\rIHDR"

static const struct tcase types[] = {
    { "image/png is not text", LIT(OK_H "Content-Type: image/png\r\nContent-Length: 16\r\n\r\n" PNG),
      FAILS("image/png is not text") },
    { "application/json is not text", LIT(OK_H "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}"),
      FAILS("application/json is not text") },
    { "application/octet-stream is not text", LIT(OK_H "Content-Type: application/octet-stream\r\n\r\nabc"),
      FAILS("application/octet-stream is not text") },
    { "a 404 page that is an image",
      LIT("HTTP/1.1 404 Not Found\r\nContent-Type: image/gif\r\nContent-Length: 3\r\n\r\nGIF"),
      FAILS("404") },
    { "a quoted charset", LIT(OK_H "Content-Type: text/plain; charset=\"ISO-8859-1\"\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/plain", "iso-8859-1", "OK") },
    { "type and charset in capitals",
      LIT(OK_H "Content-Type: TEXT/HTML;Charset=Windows-1252\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/html", "windows-1252", "OK") },
    { "application/xhtml+xml", LIT(OK_H "Content-Type: application/xhtml+xml\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "application/xhtml+xml", "", "OK") },
    { "another text/ type", LIT(OK_H "Content-Type: text/css\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/css", "", "OK") },
    { "other parameters",
      LIT(OK_H "Content-Type: text/html; q=1; charset=utf-8; x=\"y;z\"\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/html", "utf-8", "OK") },
    { "a charset with a backslash escape",
      LIT(OK_H "Content-Type: text/html; charset=\"ut\\f-8\"\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/html", "utf-8", "OK") },
    { "the first charset counts",
      LIT(OK_H "Content-Type: text/html;charset=utf-8;charset=latin1\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/html", "utf-8", "OK") },
    { "a charset without a value", LIT(OK_H "Content-Type: text/html; charset\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/html", "", "OK") },
    { "an empty quoted charset", LIT(OK_H "Content-Type: text/html; charset=\"\"\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/html", "", "OK") },
    { "a malformed type is no type: text", LIT(OK_H "Content-Type: html\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "", "", "OK") },
    { "a malformed type is no type: binary", LIT(OK_H "Content-Type: text/\r\nContent-Length: 16\r\n\r\n" PNG),
      FAILS("does not look like text") },
    { "Content-Type twice: the last counts (text)",
      LIT(OK_H "Content-Type: image/png\r\nContent-Type: text/html\r\nContent-Length: 2\r\n\r\nok"),
      PAGE(200, "ok", "text/html", "", "OK") },
    { "Content-Type twice: the last counts (image)",
      LIT(OK_H "Content-Type: text/html\r\nContent-Type: image/png\r\nContent-Length: 2\r\n\r\nok"),
      FAILS("image/png is not text") },
    { "no Content-Type: plain text", LIT(OK_H "Content-Length: 31\r\n\r\nHello, plain text\n\twith a tab\r\n"),
      PAGE(200, "Hello, plain text\n\twith a tab\r\n", "", "", "OK") },
    { "no Content-Type: HTML", LIT(OK_H "\r\n<!DOCTYPE html><title>x</title>"),
      PAGE(200, "<!DOCTYPE html><title>x</title>", "", "", "OK") },
    { "no Content-Type: UTF-8 and escape sequences", LIT(OK_H "\r\ncaf\xc3\xa9 \x1b[1mbold\x1b[0m\x0c"),
      PAGE(200, "caf\xc3\xa9 \x1b[1mbold\x1b[0m\x0c", "", "", "OK") },
    { "no Content-Type: an empty body", LIT(OK_H "Content-Length: 0\r\n\r\n"), PAGE(200, "", "", "", "OK") },
    { "no Content-Type: a NUL", LIT(OK_H "\r\nabc\0def"), FAILS("does not look like text") },
    { "no Content-Type: a PNG", LIT(OK_H "\r\n" PNG), FAILS("does not look like text") },
    { "no Content-Type: control bytes", LIT(OK_H "\r\nab\x01\x02" "cd"), FAILS("does not look like text") },
};

/**** bytes that are never read ****/

/* The response's head is followed by bytes whose reading fails: a load that needs nothing after
   the head succeeds (or fails for its own reason), so it did not read them. */
static void not_read(void) {
    static const struct { const char *name, *head, *rest; struct expect e; } t[] = {
        { "204: its body is not read", "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n", "Hello",
          PAGE(204, "", "", "", "No Content") },
        { "304: its body is not read", "HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n",
          "5\r\nHello\r\n0\r\n\r\n",
          PAGE(304, "", "", "", "Not Modified") },
        { "an image is not downloaded", OK_H "Content-Type: image/png\r\nContent-Length: 1000000\r\n\r\n", PNG,
          FAILS("image/png is not text") },
        { "a page over the limit is not downloaded", OK_H PLAIN "Content-Length: 1048577\r\n\r\n", "aaaa",
          FAILS("over 1 MB") },
        { "a compressed page is not downloaded", OK_H PLAIN "Content-Encoding: gzip\r\nContent-Length: 5\r\n\r\n",
          "\x1f\x8b\x08",
          FAILS("gzip") },
        { "an ambiguous page is not downloaded", OK_H PLAIN "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
          "5\r\n",
          FAILS("ambiguous") },
        { "a chunk over the limit is not read", CHUNKED "100001\r\n", "aaaa", FAILS("over 1 MB") },
    };
    struct text b = { 0 };
    char what[900];
    size_t i, k;
    for(i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        int ok = 1, how;
        b.len = 0;
        adds(&b, t[i].head);
        adds(&b, t[i].rest);
        for(how = WHOLE; how <= BYTES && ok; how++) {
            reset();
            serve(NULL, b.p, b.len)->fail_at = (long)strlen(t[i].head);
            mode = how;
            ok = run(PAGE_ADDRESS, &t[i].e);
        }
        for(k = 1; k < strlen(t[i].head) && ok; k++) {
            reset();
            serve(NULL, b.p, b.len)->fail_at = (long)strlen(t[i].head);
            mode = CUTS;
            ncuts = 1;
            cuts[0] = k;
            ok = run(PAGE_ADDRESS, &t[i].e);
        }
        snprintf(what, sizeof(what), "%s%s%s", t[i].name, ok ? "" : ": ", why);
        check(ok, what);
    }
    text_free(&b);
}

/**** the end of the stream and read errors at every offset ****/

static void cut_short(void) {
    static const struct { const char *name, *data; size_t head; int to_end; } t[] = {
        { "Content-Length", OK_H PLAIN "Content-Length: 5\r\n\r\nHello", 0, 0 },
        { "chunked", CHUNKED "5;x=1\r\nHello\r\n6\r\n world\r\n0\r\nX-T: 1\r\n\r\n", 0, 0 },
        { "interim responses",
          C100 "HTTP/1.1 103 Early Hints\r\nLink: <x>\r\n\r\n" OK_H PLAIN "Content-Length: 2\r\n\r\nok", 0, 0 },
        { "the connection's end", OK_H PLAIN "\r\nUntil the end", sizeof(OK_H PLAIN "\r\n") - 1, 1 },
    };
    struct expect closed = FAILS("closed the connection"), reset_msg = FAILS("connection reset (test)");
    char what[900];
    size_t i, k;
    for(i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        size_t len = strlen(t[i].data);
        int ok = 1;
        for(k = 0; k < len && ok; k++) {
            reset();
            serve(NULL, t[i].data, k);
            if(t[i].to_end && k >= t[i].head) {
                struct expect e = { 1, 200, t[i].data + t[i].head, k - t[i].head, "text/plain", "", "OK", NULL };
                ok = run(PAGE_ADDRESS, &e);
            } else ok = run(PAGE_ADDRESS, &closed);
        }
        snprintf(what, sizeof(what), "%s: the end at every offset%s%s (at %lu)", t[i].name, ok ? "" : ": ", why,
                 (unsigned long)(k - 1));
        check(ok, what);
        ok = 1;
        for(k = 0; k < len && ok; k++) {
            reset();
            serve(NULL, t[i].data, len)->fail_at = (long)k;
            ok = run(PAGE_ADDRESS, &reset_msg);
        }
        snprintf(what, sizeof(what), "%s: a read error at every offset%s%s (at %lu)", t[i].name, ok ? "" : ": ", why,
                 (unsigned long)(k - 1));
        check(ok, what);
    }
    reset();
    serve(NULL, LIT(""))->fail_msg = "the server is not answering \x1b[2J (test)";
    servers[0].fail_at = 0;
    {
        struct ut_response *r = malloc(sizeof(*r));
        char err[UT_ERR_MAX];
        int rc = load(PAGE_ADDRESS, r, err, sizeof(err), -1);
        check(rc == -1 && !why[0] && strstr(err, "fixture.test") && strstr(err, "the server is not answering ?[2J"),
              "a read error names the host and the transport's message, safely");
        free(r);
    }
}

/**** the limits ****/

/* A response whose head (status line, header lines, empty line) is exactly n bytes */
static void head_of(struct text *b, size_t n, const char *before) {
    static const char top[] = OK_H "Content-Type: text/plain\r\nContent-Length: 2\r\nX-Pad: ";
    b->len = 0;
    adds(b, before);
    adds(b, top);
    addn(b, 'p', n - strlen(before) - strlen(top) - 4);
    adds(b, "\r\n\r\nok");
}

static void big_way(const char *name, const char *data, size_t len, const struct expect *e, const size_t *at,
                    size_t nat) {
    char what[900];
    size_t i;
    int ok;
    ok = run1(data, len, e, WHOLE, 0, 0, 0, 1);
    snprintf(what, sizeof(what), "%s (whole)%s%s", name, ok ? "" : ": ", why);
    check(ok, what);
    ok = run1(data, len, e, BYTES, 0, 0, 0, 1);
    snprintf(what, sizeof(what), "%s (a byte at a time)%s%s", name, ok ? "" : ": ", why);
    check(ok, what);
    ok = 1;
    snprintf(what, sizeof(what), "%s (split around the limit)", name);
    for(i = 0; i < nat && ok; i++)
        if(at[i] > 0 && at[i] < len && !run1(data, len, e, CUTS, at[i], 0, 1, 1)) {
            ok = 0;
            snprintf(what, sizeof(what), "%s (split at %lu): %s", name, (unsigned long)at[i], why);
        }
    check(ok, what);
}

static void header_limits(void) {
    struct text b = { 0 }, c = { 0 };
    struct expect ok2 = PAGE(200, "ok", "text/plain", "", "OK"), over = FAILS("32 KB");
    size_t at[] = { 1, 100, 32766, 32767, 32768, 32769, 32770 };
    head_of(&b, UT_HEADERS_MAX, "");
    big_way("headers of exactly 32 KiB", b.p, b.len, &ok2, at, 7);
    head_of(&b, UT_HEADERS_MAX + 1, "");
    big_way("headers of 32 KiB + 1", b.p, b.len, &over, at, 7);
    head_of(&b, UT_HEADERS_MAX, C100 "HTTP/1.1 103 Early Hints\r\nLink: <x>\r\n\r\n");
    big_way("interim responses and headers of exactly 32 KiB", b.p, b.len, &ok2, at, 7);
    head_of(&b, UT_HEADERS_MAX + 1, C100 "HTTP/1.1 103 Early Hints\r\nLink: <x>\r\n\r\n");
    big_way("interim responses and headers of 32 KiB + 1", b.p, b.len, &over, at, 7);

    /* a chunked body after a head of exactly 32 KiB: the body's own lines do not count */
    b.len = 0;
    adds(&b, OK_H PLAIN "Transfer-Encoding: chunked\r\nX-Pad: ");
    addn(&b, 'p', UT_HEADERS_MAX - b.len - 4);
    adds(&b, "\r\n\r\n2;ext=1\r\nok\r\n0\r\n\r\n");
    big_way("a head of exactly 32 KiB, then chunks", b.p, b.len, &ok2, at, 7);
    b.len -= 4;                         /* "0\n\n": the same with line feeds alone at the end */
    adds(&b, "\n\n");
    big_way("a head of exactly 32 KiB, then chunks ending in line feeds", b.p, b.len, &ok2, at, 7);

    /* the trailer lines count too (the chunked body's own lines do not) */
    {
        static const char top[] = CHUNKED;
        static const char body[] = "2;ext=1\r\nok\r\n0\r\n";
        size_t used = strlen(top), pad = UT_HEADERS_MAX - used - strlen("X-Trailer: \r\n");
        b.len = 0;
        adds(&b, top);
        adds(&b, body);
        adds(&b, "X-Trailer: ");
        addn(&b, 't', pad);
        adds(&b, "\r\n\r\n");
        big_way("headers and trailers of exactly 32 KiB", b.p, b.len, &ok2, at, 7);
        c.len = 0;
        adds(&c, top);
        adds(&c, body);
        adds(&c, "X-Trailer: ");
        addn(&c, 't', pad + 1);
        adds(&c, "\r\n\r\n");
        big_way("headers and trailers of 32 KiB + 1", c.p, c.len, &over, at, 7);
    }
    b.len = 0;
    adds(&b, OK_H "X-Big: ");
    addn(&b, 'x', 40000);
    adds(&b, "\r\n\r\nok");
    big_way("one header line of 40000 bytes", b.p, b.len, &over, at, 7);
    b.len = 0;
    adds(&b, "HTTP/1.1 200 ");
    addn(&b, 'x', 40000);
    adds(&b, "\r\n\r\nok");
    big_way("a status line of 40000 bytes", b.p, b.len, &over, at, 7);
    b.len = 0;
    adds(&b, OK_H PLAIN "Content-Length: 2\r\n");
    while(b.len < 40000) adds(&b, "X-Many: 1\r\n");
    adds(&b, "\r\nok");
    big_way("many header lines past 32 KiB", b.p, b.len, &over, at, 7);
    text_free(&b);
    text_free(&c);
}

static void body_limits(void) {
    struct text b = { 0 }, page = { 0 };
    struct expect e = { 1, 200, NULL, 0, "text/plain", "", "OK", NULL }, over = FAILS("over 1 MB");
    size_t head, at[8], i;
    for(i = 0; i < MB + 10; i++) {
        char ch = (char)('a' + i % 26);
        add(&page, &ch, 1);
    }
    e.body = page.p;
    e.blen = MB;

    /* Content-Length */
    addf(&b, OK_H PLAIN "Content-Length: %u\r\n\r\n", MB);
    head = b.len;
    add(&b, page.p, MB);
    at[0] = head - 1; at[1] = head; at[2] = head + 1; at[3] = head + 4096; at[4] = b.len - 1; at[5] = b.len - 2;
    big_way("Content-Length: 1 MiB", b.p, b.len, &e, at, 6);
    b.len = 0;
    addf(&b, OK_H PLAIN "Content-Length: %u\r\n\r\n", MB + 1);
    add(&b, page.p, MB + 1);
    big_way("Content-Length: 1 MiB + 1", b.p, b.len, &over, at, 6);

    /* chunked: 4096-byte chunks, then the last one short */
    b.len = 0;
    adds(&b, CHUNKED);
    for(i = 0; i < MB; i += 4096) {
        size_t n = MB - i < 4096 ? MB - i : 4096;
        addf(&b, "%lx;n=%lu\r\n", (unsigned long)n, (unsigned long)i);
        add(&b, page.p + i, n);
        adds(&b, "\r\n");
    }
    adds(&b, "0\r\n\r\n");
    at[4] = b.len - 3; at[5] = b.len - 6;
    big_way("chunked: 1 MiB in 256 chunks", b.p, b.len, &e, at, 6);
    b.len -= 5;
    adds(&b, "1\r\nX\r\n0\r\n\r\n");
    big_way("chunked: 1 MiB + 1", b.p, b.len, &over, at, 6);
    b.len = 0;
    adds(&b, CHUNKED "100000\r\n");
    add(&b, page.p, MB);
    adds(&b, "\r\n0\r\n\r\n");
    big_way("chunked: one chunk of 1 MiB", b.p, b.len, &e, at, 6);

    /* the end of the connection */
    b.len = 0;
    adds(&b, OK_H PLAIN "\r\n");
    head = b.len;
    add(&b, page.p, MB);
    at[0] = head - 1; at[1] = head; at[2] = head + 1; at[3] = head + 4096; at[4] = b.len - 1; at[5] = b.len - 2;
    big_way("the connection's end: 1 MiB", b.p, b.len, &e, at, 6);
    add(&b, page.p, 1);
    big_way("the connection's end: 1 MiB + 1", b.p, b.len, &over, at, 6);
    text_free(&b);
    text_free(&page);
}

/* long fields of the response: cut to their arrays, whole characters only */
static void long_fields(void) {
    struct text b = { 0 }, want = { 0 };
    struct ut_response *r = malloc(sizeof(*r));
    char err[UT_ERR_MAX];
    int rc;
    adds(&b, "HTTP/1.1 200 ");
    addn(&b, 'x', 100);
    adds(&b, "\r\nContent-Type: text/");
    addn(&b, 'y', 100);
    adds(&b, "; charset=");
    addn(&b, 'z', 100);
    adds(&b, "\r\nContent-Length: 2\r\n\r\nok");
    reset();
    serve(NULL, b.p, b.len);
    rc = load(PAGE_ADDRESS, r, err, sizeof(err), -1);
    check(rc == 0 && !why[0], "a long reason, type and charset");
    if(!rc) {
        addn(&want, 'x', sizeof(r->reason) - 1);
        check(!strcmp(r->reason, want.p), "the reason is cut to 63 bytes");
        check(strlen(r->content_type) == sizeof(r->content_type) - 1 && !strncmp(r->content_type, "text/yyy", 8),
              "the type is cut to 63 bytes");
        check(strlen(r->charset) == sizeof(r->charset) - 1 && r->charset[0] == 'z', "the charset is cut to 31 bytes");
        ut_response_free(r);
    }
    b.len = 0;
    adds(&b, "HTTP/1.1 200 ");
    addn(&b, 'x', 62);
    adds(&b, "\xc3\xa9\r\nContent-Length: 2\r\n\r\nok");
    reset();
    serve(NULL, b.p, b.len);
    rc = load(PAGE_ADDRESS, r, err, sizeof(err), -1);
    check(rc == 0 && strlen(r->reason) == 62, "a character that does not fit the reason is left out whole");
    if(!rc) ut_response_free(r);
    check(ut_alloc_live == 0, "no leaks after long fields");
    free(r);
    text_free(&b);
    text_free(&want);
}

/**** the request ****/

static void requests(void) {
    static const struct { const char *address, *request; } t[] = {
        { "http://fixture.test:8080/a/b?q=1#frag",
          "GET /a/b?q=1 HTTP/1.1\r\nHost: fixture.test:8080\r\n" REQUEST_TAIL },
        { "https://fixture.test/", "GET / HTTP/1.1\r\nHost: fixture.test\r\n" REQUEST_TAIL },
        { "https://fixture.test:443/x", "GET /x HTTP/1.1\r\nHost: fixture.test\r\n" REQUEST_TAIL },
        { "https://fixture.test:80/x", "GET /x HTTP/1.1\r\nHost: fixture.test:80\r\n" REQUEST_TAIL },
        { "http://fixture.test:80/x", "GET /x HTTP/1.1\r\nHost: fixture.test\r\n" REQUEST_TAIL },
        { "http://fixture.test:443/x", "GET /x HTTP/1.1\r\nHost: fixture.test:443\r\n" REQUEST_TAIL },
        { "http://10.0.2.2:65535/?x=1#", "GET /?x=1 HTTP/1.1\r\nHost: 10.0.2.2:65535\r\n" REQUEST_TAIL },
        { "fixture.test:1/#only-a-fragment", "GET / HTTP/1.1\r\nHost: fixture.test:1\r\n" REQUEST_TAIL },
        { "http://fixture.test/caf\xc3\xa9?q=\xe2\x82\xac",
          "GET /caf%C3%A9?q=%E2%82%AC HTTP/1.1\r\nHost: fixture.test\r\n" REQUEST_TAIL },
        { "http://fixture.test/a%20b/%2F?%23", "GET /a%20b/%2F?%23 HTTP/1.1\r\nHost: fixture.test\r\n" REQUEST_TAIL },
    };
    static const char page[] = OK_H PLAIN "Content-Length: 2\r\n\r\nok";
    struct expect e = PAGE(200, "ok", "text/plain", "", "OK"), broken = FAILS("broken pipe");
    struct text addr = { 0 }, want = { 0 };
    char what[900];
    size_t i, n, w;
    int ok;
    for(i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        static const size_t limits[] = { 0, 1, 7, 100 };
        for(w = 0; w < 4; w++) {
            reset();
            serve(NULL, LIT(page));
            write_max = limits[w];
            ok = run(t[i].address, &e);
            if(ok && (conns[0].request_len != strlen(t[i].request) ||
                      memcmp(conns[0].request, t[i].request, conns[0].request_len))) {
                note("the request was \"%.*s\"", (int)conns[0].request_len, conns[0].request);
                ok = 0;
            }
            snprintf(what, sizeof(what), "the request for %s (writes of at most %lu bytes)%s%s", t[i].address,
                     (unsigned long)limits[w], ok ? "" : ": ", why);
            check(ok, what);
        }
    }
    /* a write fails at every offset of the request */
    ok = 1;
    n = sizeof(PAGE_REQUEST) - 1;
    for(i = 0; i < n && ok; i++) {
        reset();
        serve(NULL, LIT(page));
        write_fail_at = (long)i;
        write_max = 3;
        ok = run(PAGE_ADDRESS, &broken);
    }
    snprintf(what, sizeof(what), "a write error at every offset of the request%s%s", ok ? "" : ": ", why);
    check(ok, what);
    /* the longest address */
    adds(&addr, "http://fixture.test/");
    addn(&addr, 'a', UT_URL_MAX - addr.len);
    adds(&want, "GET /");
    addn(&want, 'a', UT_URL_MAX - strlen("http://fixture.test/"));
    adds(&want, " HTTP/1.1\r\nHost: fixture.test\r\n" REQUEST_TAIL);
    reset();
    serve(NULL, LIT(page));
    ok = run(addr.p, &e) && conns[0].request_len == want.len && !memcmp(conns[0].request, want.p, want.len);
    check(ok, "the request for a 2048-byte address");
    text_free(&addr);
    text_free(&want);
}

/**** redirects ****/

static char bodies[MAX_CONNS][4096];

/* a redirect from server n (location NULL: none) */
static struct server *moved(const char *expect, int code, const char *location) {
    char *b = bodies[server_count];
    if(location) snprintf(b, sizeof(bodies[0]), "HTTP/1.1 %d Moved\r\nLocation: %s\r\nContent-Type: text/html\r\n"
                          "Content-Length: 9\r\n\r\n<a>x</a>\n", code, location);
    else snprintf(b, sizeof(bodies[0]), "HTTP/1.1 %d Moved\r\nContent-Type: text/html\r\nContent-Length: 9\r\n\r\n"
                  "<a>x</a>\n", code);
    return serve(expect, b, strlen(b));
}

static struct server *page_at(const char *expect) {
    return serve(expect, LIT(OK_H "Content-Type: text/html\r\nContent-Length: 6\r\n\r\n<p>ok\n"));
}

/* load start through the servers set up; 0 and the final address, redirects and status in r */
static int follow(const char *start, struct ut_response *r, char *err) {
    return load(start, r, err, UT_ERR_MAX, -1);
}

/* The ways the servers' bytes may arrive in a chain of connections, one per step: whole, a byte at
   a time, random pieces, then split at each offset (the same offset in every connection). 0 when
   there are no more; how says which. */
static int way(int step, char *how, size_t cap) {
    size_t longest = 0;
    int i;
    for(i = 0; i < server_count; i++) if(servers[i].len > longest) longest = servers[i].len;
    ncuts = 0;
    if(step == 0) { mode = WHOLE; snprintf(how, cap, "whole"); return 1; }
    if(step == 1) { mode = BYTES; snprintf(how, cap, "a byte at a time"); return 1; }
    if(step < 10) {
        mode = RANDOM;
        seed = (unsigned int)step;
        snprintf(how, cap, "random pieces, seed %d", step);
        return 1;
    }
    if((size_t)(step - 9) >= longest) return 0;
    mode = CUTS;
    ncuts = 1;
    cuts[0] = (size_t)(step - 9);
    snprintf(how, cap, "split at %d", step - 9);
    return 1;
}

/* the chain set up in servers ends at the page, every way its bytes may arrive */
static void expect_page(const char *name, const char *start, const char *final, int redirects, int hops) {
    struct ut_response *r = malloc(sizeof(*r));
    char err[UT_ERR_MAX], what[900], how[64];
    int step, ok = 1, rc;
    for(step = 0; ok && way(step, how, sizeof(how)); step++) {
        rc = follow(start, r, err);
        if(rc) note("it failed: %s", err);
        else {
            if(strcmp(r->url.text, final)) note("it ended at %s, not %s", r->url.text, final);
            if(r->redirects != redirects) note("%d redirects, not %d", r->redirects, redirects);
            if(r->status != 200 || r->len != 6 || memcmp(r->body, "<p>ok\n", 6))
                note("not the page (status %d)", r->status);
            ut_response_free(r);
        }
        if(opened != hops) note("%d connections, not %d", opened, hops);
        if(ut_alloc_live) { note("%ld allocations held", ut_alloc_live); ut_alloc_live = 0; }
        ok = !why[0];
    }
    if(ok) snprintf(what, sizeof(what), "%s", name);
    else snprintf(what, sizeof(what), "%s (%s): %s", name, how, why);
    check(ok, what);
    mode = WHOLE;
    free(r);
}

/* the chain set up in servers fails with a message saying msg1 (and msg2), every way */
static void expect_failure(const char *name, const char *start, const char *msg1, const char *msg2, int hops) {
    struct ut_response *r = malloc(sizeof(*r));
    char err[UT_ERR_MAX], what[900], how[64];
    int step, ok = 1, rc;
    for(step = 0; ok && way(step, how, sizeof(how)); step++) {
        rc = follow(start, r, err);
        if(!rc) {
            note("it loaded %s", r->url.text);
            ut_response_free(r);
        } else if(!strstr(err, msg1) || (msg2 && !strstr(err, msg2))) note("the message \"%s\"", err);
        if(opened != hops) note("%d connections, not %d", opened, hops);
        if(ut_alloc_live) { note("%ld allocations held", ut_alloc_live); ut_alloc_live = 0; }
        ok = !why[0];
    }
    if(ok) snprintf(what, sizeof(what), "%s", name);
    else snprintf(what, sizeof(what), "%s (%s): %s", name, how, why);
    check(ok, what);
    mode = WHOLE;
    free(r);
}

/* https to http: the message ends in the whole address, or says it could not show it whole and
   ends in "..." after a whole character of it (never inside an escape: escaped says that every
   byte after "http://fixture.test/" is one) */
static int cut_message(const char *err, size_t cap, const char *encoded, int escaped, const char *why_it) {
    size_t len = strlen(err), shown;
    const char *p = strstr(err, "http://fixture.test/");
    if(len > cap - 1 || len < 3 || strcmp(err + len - 3, "...") || !strstr(err, "too long to show in full") || !p) {
        note("%s: the message is \"%.80s...%s\" (%lu bytes)", why_it, err, len > 40 ? err + len - 40 : err,
             (unsigned long)len);
        return 0;
    }
    shown = (size_t)(err + len - 3 - p);
    if(shown < 20 || strncmp(p, encoded, shown) || (escaped && (shown - 20) % 3 != 0)) {
        note("%s: the address shown is not a whole-escape prefix (%lu bytes, ends \"%s\")", why_it,
             (unsigned long)shown, err + len - 10);
        return 0;
    }
    return 1;
}

static void downgrade_messages(void) {
    struct text host = { 0 }, start = { 0 }, loc = { 0 }, enc = { 0 };
    struct ut_response *r = malloc(sizeof(*r));
    char err[UT_ERR_MAX];
    size_t fixed, fit, i;
    int rc;
    addn(&host, 'a', 63); adds(&host, "."); addn(&host, 'b', 63); adds(&host, ".");
    addn(&host, 'c', 63); adds(&host, "."); addn(&host, 'd', 61);       /* 253 bytes */
    addf(&start, "https://%s/start", host.p);

    /* the sentence's own length, from an address that fits */
    reset();
    moved(NULL, 301, "http://fixture.test/s");
    rc = follow(start.p, r, err);
    fixed = strlen(err) - strlen("http://fixture.test/s");
    check_noted(rc == -1 && !why[0] && strstr(err, host.p) && !strcmp(err + fixed, "http://fixture.test/s"),
          "https to http from a 253-byte host: the message ends in the address");

    /* an address that fills the message to its last byte is whole; one byte more is cut, and says so */
    fit = UT_ERR_MAX - 1 - fixed;
    check_noted(fit > 20 && fit <= UT_URL_MAX, "the test's exact fit is an address url.c takes");
    adds(&loc, "http://fixture.test/");
    addn(&loc, 'x', fit - loc.len);
    reset();
    moved(NULL, 301, loc.p);
    rc = follow(start.p, r, err);
    check_noted(rc == -1 && !why[0] && strlen(err) == UT_ERR_MAX - 1 && !strcmp(err + fixed, loc.p),
          "an address that just fits the message is given whole");
    add(&loc, "x", 1);
    reset();
    moved(NULL, 301, loc.p);
    rc = follow(start.p, r, err);
    check_noted(rc == -1 && cut_message(err, UT_ERR_MAX, loc.p, 0, "one byte over") && !why[0],
          "an address one byte too long for the message: cut, and the message says so");

    /* a 253-byte host and a 2048-byte address */
    loc.len = 0;
    adds(&loc, "http://fixture.test/");
    addn(&loc, 'y', UT_URL_MAX - loc.len);
    reset();
    moved(NULL, 301, loc.p);
    rc = follow(start.p, r, err);
    check_noted(rc == -1 && cut_message(err, UT_ERR_MAX, loc.p, 0, "2048 bytes") && !why[0],
          "a 253-byte host and a 2048-byte address: cut, and the message says so");

    /* ... in a smaller error buffer: the room is what that buffer leaves */
    reset();
    moved(NULL, 301, loc.p);
    rc = load(start.p, r, err, 600, -1);
    check_noted(rc == -1 && cut_message(err, 600, loc.p, 0, "a 600-byte buffer") && strlen(err) <= 599 && !why[0],
          "the same in a 600-byte error buffer");

    /* 700 x U+00E9: 4220 bytes percent-encoded, cut between escapes */
    loc.len = 0;
    adds(&loc, "http://fixture.test/");
    adds(&enc, "http://fixture.test/");
    for(i = 0; i < 700; i++) {
        adds(&loc, "\xc3\xa9");
        adds(&enc, "%C3%A9");
    }
    reset();
    moved(NULL, 301, loc.p);
    rc = follow("https://fixture.test/start", r, err);
    check_noted(rc == -1 && cut_message(err, UT_ERR_MAX, enc.p, 1, "700 x e-acute") && !why[0],
          "an address of 700 x U+00E9 (4220 bytes encoded): cut between escapes, and the message says so");
    text_free(&host);
    text_free(&start);
    text_free(&loc);
    text_free(&enc);
    free(r);
}

/* Sniffing a body without a Content-Type: a NUL anywhere is binary; other binary bytes count in
   the first 1445 bytes only. */
static void sniffing(void) {
    struct text b = { 0 };
    struct expect binary = FAILS("does not look like text"), text = { 1, 200, NULL, 0, "", "", "OK", NULL };
    size_t head = strlen(OK_H "\r\n");
    adds(&b, OK_H "\r\n");
    addn(&b, 'a', 2000);
    add(&b, "\0b", 2);
    every_way("no Content-Type: a NUL after 2000 bytes", b.p, b.len, &binary);
    b.len = 0;
    adds(&b, OK_H "\r\n");
    addn(&b, 'a', 1444);
    add(&b, "\x01", 1);
    addn(&b, 'a', 10);
    every_way("no Content-Type: a control byte at offset 1444", b.p, b.len, &binary);
    b.len = 0;
    adds(&b, OK_H "\r\n");
    addn(&b, 'a', 1445);
    add(&b, "\x01", 1);
    addn(&b, 'a', 10);
    text.body = b.p + head;
    text.blen = b.len - head;
    every_way("no Content-Type: a control byte at offset 1445 is past the bytes looked at", b.p, b.len, &text);
    text_free(&b);
}

/* A chunk's size line: 4096 bytes with its line end are taken, 4097 are not. */
static void chunk_line_limits(void) {
    struct text b = { 0 };
    struct expect ok = PAGE(200, "Hello", "text/plain", "", "OK"), over = FAILS("chunk size line over 4096 bytes");
    adds(&b, CHUNKED "5;e=");
    addn(&b, 'x', 4096 - 4 - 2);
    adds(&b, "\r\nHello\r\n0\r\n\r\n");
    every_way("a chunk size line of exactly 4096 bytes", b.p, b.len, &ok);
    b.len = 0;
    adds(&b, CHUNKED "5;e=");
    addn(&b, 'x', 4096 - 4 - 1);
    adds(&b, "\nHello\r\n0\r\n\r\n");
    every_way("a chunk size line of exactly 4096 bytes, ended by a line feed alone", b.p, b.len, &ok);
    b.len = 0;
    adds(&b, CHUNKED "5;e=");
    addn(&b, 'x', 4096 - 4 - 2 + 1);
    adds(&b, "\r\nHello\r\n0\r\n\r\n");
    every_way("a chunk size line of 4097 bytes", b.p, b.len, &over);
    b.len = 0;
    adds(&b, CHUNKED "5\r\nHello\r\n0;e=");
    addn(&b, 'x', 4096 - 4 - 2 + 1);
    adds(&b, "\r\n\r\n");
    every_way("a last-chunk line of 4097 bytes", b.p, b.len, &over);
    text_free(&b);
}

static void redirects(void) {
    static const int codes[] = { 301, 302, 303, 307, 308 };
    static const char *const hops6[] = { "http://fixture.test/1", "http://fixture.test/2", "http://fixture.test/3",
                                         "http://fixture.test/4", "http://fixture.test/5", "http://fixture.test/6",
                                         "http://fixture.test/7" };
    struct ut_response *r = malloc(sizeof(*r));
    char err[UT_ERR_MAX], what[900];
    size_t i;
    int rc, ok;

    /* where a Location leads */
    reset();
    moved("http://fixture.test/dir/page?x", 302, "../other?y=1");
    page_at("http://fixture.test/other?y=1");
    expect_page("a relative redirect", "http://fixture.test/dir/page?x", "http://fixture.test/other?y=1", 1, 2);
    check(conns[1].request_len > 45 &&
          !memcmp(conns[1].request, "GET /other?y=1 HTTP/1.1\r\nHost: fixture.test\r\n", 45),
          "the request after a relative redirect");
    reset();
    moved("http://fixture.test/", 301, "http://www.fixture.test:8080/x");
    page_at("http://www.fixture.test:8080/x");
    expect_page("a redirect to another host and port", "http://fixture.test/", "http://www.fixture.test:8080/x", 1, 2);
    check(conns[1].request_len > 46 &&
          !memcmp(conns[1].request, "GET /x HTTP/1.1\r\nHost: www.fixture.test:8080\r\n", 46),
          "the request names the new host and port");
    reset();
    moved("https://fixture.test/", 301, "//www.fixture.test/y");
    page_at("https://www.fixture.test/y");
    expect_page("a scheme-relative redirect", "https://fixture.test/", "https://www.fixture.test/y", 1, 2);
    reset();
    moved("http://fixture.test/dir/page?x", 303, "?page=2");
    page_at("http://fixture.test/dir/page?page=2");
    expect_page("a query-only redirect", "http://fixture.test/dir/page?x", "http://fixture.test/dir/page?page=2", 1, 2);
    reset();
    moved("http://fixture.test/", 302, "/a b");
    page_at("http://fixture.test/a%20b");
    expect_page("a space in a Location is sent as %20", "http://fixture.test/", "http://fixture.test/a%20b", 1, 2);
    reset();
    moved("http://fixture.test/", 302, "/caf\xc3\xa9");
    page_at("http://fixture.test/caf\xc3\xa9");
    expect_page("a Location in UTF-8", "http://fixture.test/", "http://fixture.test/caf\xc3\xa9", 1, 2);
    check(conns[1].request_len > 30 && !memcmp(conns[1].request, "GET /caf%C3%A9 HTTP/1.1\r\n", 25),
          "a Location in UTF-8 is requested percent-encoded");
    for(i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        reset();
        moved("http://fixture.test/a", codes[i], "/b");
        page_at("http://fixture.test/b");
        snprintf(what, sizeof(what), "%d is followed", codes[i]);
        expect_page(what, "http://fixture.test/a", "http://fixture.test/b", 1, 2);
    }
    {
        static const int not_followed[] = { 300, 304, 305, 306 };
        for(i = 0; i < sizeof(not_followed) / sizeof(not_followed[0]); i++) {
            struct expect e = { 1, not_followed[i], not_followed[i] == 304 ? "" : "<a>x</a>\n",
                                not_followed[i] == 304 ? 0 : 9, "text/html", "", "Moved", NULL };
            reset();
            moved(NULL, not_followed[i], "/b");
            snprintf(what, sizeof(what), "%d is not followed: it is the page", not_followed[i]);
            ok = run("http://fixture.test/a", &e) && opened == 1;
            check(ok, what);
        }
    }

    /* the redirect response itself: every way its bytes may arrive, and its body is not read */
    {
#define REDIRECT_HEAD "HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Type: image/png\r\nContent-Length: 1000\r\n\r\n"
        static const char head[] = REDIRECT_HEAD, data[] = REDIRECT_HEAD PNG;
        size_t k;
        ok = 1;
        for(k = 0; k <= sizeof(head) && ok; k++) {
            reset();
            serve("http://fixture.test/page", LIT(data))->fail_at = (long)(sizeof(head) - 1);
            page_at("http://fixture.test/next");
            if(k == sizeof(head)) mode = BYTES;
            else if(k) { mode = CUTS; ncuts = 1; cuts[0] = k; }
            rc = follow(PAGE_ADDRESS, r, err);
            if(rc) note("it failed: %s", err);
            else ut_response_free(r);
            ok = !why[0] && opened == 2;
        }
        snprintf(what, sizeof(what), "a redirect's head split anywhere; its body is not read%s%s", ok ? "" : ": ", why);
        check(ok, what);
    }

    /* five hops, six hops */
    reset();
    for(i = 0; i < 5; i++) moved(hops6[i], 302, hops6[i + 1]);
    page_at(hops6[5]);
    expect_page("five redirects", hops6[0], hops6[5], 5, 6);
    reset();
    for(i = 0; i < 6; i++) moved(hops6[i], 302, hops6[i + 1]);
    page_at(hops6[6]);
    expect_failure("six redirects", hops6[0], "more than 5 redirects", hops6[6], 6);

    /* loops */
    reset();
    moved("http://fixture.test/a", 302, "/b");
    moved("http://fixture.test/b", 302, "/a");
    expect_failure("a loop of two", "http://fixture.test/a", "loop", "http://fixture.test/a", 2);
    reset();
    moved("http://fixture.test/a", 302, "/b");
    moved("http://fixture.test/b", 302, "/c");
    moved("http://fixture.test/c", 302, "/b");
    expect_failure("a loop back to the middle of the chain", "http://fixture.test/a", "loop",
                   "http://fixture.test/b", 3);
    reset();
    moved("http://fixture.test/a", 302, "/a");
    expect_failure("a redirect to itself", "http://fixture.test/a", "loop", "http://fixture.test/a", 1);
    reset();
    moved("http://fixture.test/a", 302, "/a#other");
    expect_failure("a redirect to itself with another fragment", "http://fixture.test/a#x", "loop",
                   "http://fixture.test/a", 1);
    reset();
    moved("http://fixture.test/a", 302, "http://FIXTURE.test:80/a");
    expect_failure("a redirect to itself, written another way", "http://fixture.test/a", "loop",
                   "http://fixture.test/a", 1);
    reset();
    moved("http://fixture.test/a", 302, "/a?again");
    page_at("http://fixture.test/a?again");
    expect_page("another query is another address", "http://fixture.test/a", "http://fixture.test/a?again", 1, 2);
    reset();
    moved("http://fixture.test/a", 302, "https://fixture.test/a");
    page_at("https://fixture.test/a");
    expect_page("http to https of the same address is no loop", "http://fixture.test/a", "https://fixture.test/a",
                1, 2);
    reset();
    for(i = 0; i < 5; i++) moved(hops6[i], 302, hops6[i + 1]);
    moved(hops6[5], 302, hops6[0]);
    expect_failure("a loop of six", hops6[0], "loop", hops6[0], 6);

    /* https to http */
    reset();
    moved("https://fixture.test/secure", 301, "http://fixture.test/plain?x=1#f");
    expect_failure("https to http stops and gives the address", "https://fixture.test/secure", "HTTPS",
                   "http://fixture.test/plain?x=1#f", 1);
    reset();
    moved("https://fixture.test/secure", 301, "http://fixture.test/caf\xc3\xa9");
    expect_failure("https to http names an address in UTF-8 percent-encoded", "https://fixture.test/secure",
                   "HTTPS", "http://fixture.test/caf%C3%A9", 1);
    reset();
    moved("http://fixture.test/", 301, "https://fixture.test/")->verified = 0;
    page_at("https://fixture.test/")->verified = 1;
    servers[1].tls = "TLSv1.3";
    rc = follow("http://fixture.test/", r, err);
    check(rc == 0 && !why[0] && r->verified == 1 && !strcmp(r->tls, "TLSv1.3") && r->redirects == 1,
          "http to https: the final hop's TLS counts");
    if(!rc) ut_response_free(r);
    reset();
    moved("https://fixture.test/", 301, "https://www.fixture.test/")->verified = 1;
    servers[0].tls = "TLSv1.3";
    page_at("https://www.fixture.test/")->verified = 1;
    servers[1].tls = "TLSv1.2";
    rc = follow("https://fixture.test/", r, err);
    check(rc == 0 && !why[0] && r->verified == 1 && !strcmp(r->tls, "TLSv1.2"),
          "https to https: the final hop's TLS version");
    if(!rc) ut_response_free(r);
    reset();
    page_at(NULL);
    rc = follow("http://fixture.test/", r, err);
    check(rc == 0 && !why[0] && r->verified == 0 && !strcmp(r->tls, "") && r->redirects == 0,
          "http: not verified, no TLS");
    if(!rc) ut_response_free(r);

    /* fragments */
    reset();
    moved("http://fixture.test/a", 302, "/b");
    page_at("http://fixture.test/b");
    expect_page("a Location without a fragment keeps the request's", "http://fixture.test/a#top",
                "http://fixture.test/b#top", 1, 2);
    check(conns[1].request_len > 17 && !memcmp(conns[1].request, "GET /b HTTP/1.1\r\n", 17),
          "the fragment is not sent");
    reset();
    moved("http://fixture.test/a", 302, "/b");
    moved("http://fixture.test/b", 302, "/c");
    page_at("http://fixture.test/c");
    expect_page("the fragment is kept through two redirects", "http://fixture.test/a#top",
                "http://fixture.test/c#top", 2, 3);
    reset();
    moved("http://fixture.test/a", 302, "/b#sec");
    page_at("http://fixture.test/b");
    expect_page("a Location's own fragment wins", "http://fixture.test/a#top", "http://fixture.test/b#sec", 1, 2);
    reset();
    moved("http://fixture.test/a", 302, "/b#mid");
    moved("http://fixture.test/b", 302, "/c");
    page_at("http://fixture.test/c");
    expect_page("the latest fragment is kept", "http://fixture.test/a#top", "http://fixture.test/c#mid", 2, 3);
    reset();
    moved("http://fixture.test/a", 302, "/b#");
    page_at("http://fixture.test/b");
    expect_page("an empty fragment in a Location is its own", "http://fixture.test/a#top", "http://fixture.test/b#",
                1, 2);
    reset();
    page_at(NULL);
    rc = follow("http://fixture.test/a#top", r, err);
    check(rc == 0 && !strcmp(r->url.text, "http://fixture.test/a#top") && !strcmp(r->url.fragment, "top"),
          "without redirects the address keeps its fragment");
    if(!rc) ut_response_free(r);

    /* Locations that cannot be followed */
    reset();
    moved("http://fixture.test/a", 302, NULL);
    expect_failure("a redirect without a Location", "http://fixture.test/a", "Location", "302", 1);
    reset();
    moved("http://fixture.test/a", 302, "");
    expect_failure("a redirect with an empty Location", "http://fixture.test/a", "Location", NULL, 1);
    {
        static const struct { const char *location, *msg; } bad[] = {
            { "ftp://fixture.test/file", "ftp://fixture.test/file" }, { "javascript:alert(1)", "javascript:alert(1)" },
            { "http://[::1]/", "IPv6" }, { "http://user:pass@fixture.test/", "credentials" },
            { "/a\x1b[2Jb", "/a%1B[2Jb" }, { "/a%zz", "escape" }
        };
        for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            reset();
            moved("http://fixture.test/a", 302, bad[i].location);
            snprintf(what, sizeof(what), "a Location Undertow cannot follow: %s", bad[i].msg);
            expect_failure(what, "http://fixture.test/a", "redirect", bad[i].msg, 1);
        }
    }
    {
        struct text big = { 0 };
        adds(&big, "/");
        addn(&big, 'l', UT_URL_MAX);
        reset();
        moved("http://fixture.test/a", 302, big.p);
        expect_failure("a Location over 2048 bytes", "http://fixture.test/a", "redirect", "2048", 1);
        text_free(&big);
    }
    reset();
    serve("http://fixture.test/a", LIT("HTTP/1.1 302 Found\r\nLocation: /b\r\nLocation: /c\r\n\r\n"));
    expect_failure("two different Locations", "http://fixture.test/a", "Location", NULL, 1);
    reset();
    serve("http://fixture.test/a", LIT("HTTP/1.1 302 Found\r\nLocation: /b\r\nLocation: /b\r\n\r\n"));
    page_at("http://fixture.test/b");
    expect_page("the same Location twice", "http://fixture.test/a", "http://fixture.test/b", 1, 2);
    reset();
    moved("http://fixture.test/a", 302, "http://www.fixture.test/b");
    serve("http://www.fixture.test/b", NULL, 0)->open_error = "www.fixture.test does not resolve (test)";
    expect_failure("a redirect to a host that cannot be reached", "http://fixture.test/a",
                   "www.fixture.test does not resolve", "http://www.fixture.test/b", 2);
    reset();
    serve(NULL, NULL, 0)->open_error = "fixture.test does not resolve (test)";
    expect_failure("the first connection fails", "http://fixture.test/a", "fixture.test does not resolve (test)",
                   NULL, 1);
    free(r);
}

/**** the load's deadline ****/

static void deadline_tests(void) {
    struct ut_response *r = malloc(sizeof(*r));
    char err[UT_ERR_MAX];
    struct text b = { 0 };
    int rc;

    /* the same deadline through every redirect, while time passes */
    sim_ms = 5000;
    reset();
    read_ms = 700;
    moved("http://fixture.test/a", 302, "/b");
    moved("http://fixture.test/b", 302, "/c");
    page_at("http://fixture.test/c");
    rc = follow("http://fixture.test/a", r, err);
    check(rc == 0 && !why[0] && opened == 3 && conns[0].deadline == 65000 && conns[2].deadline == 65000,
          "one deadline for every redirect");
    if(!rc) ut_response_free(r);

    /* ... across uptime's wrap */
    sim_ms = 0xffffffffu - 2000;
    reset();
    read_ms = 100;
    mode = BYTES;
    moved("http://fixture.test/a", 302, "/b");
    page_at("http://fixture.test/b");
    rc = follow("http://fixture.test/a", r, err);
    check(rc == 0 && !why[0] && conns[1].deadline == 0xffffffffu - 2000 + 60000u,
          "the deadline across uptime's wrap (redirects, a byte every 100 ms)");
    if(!rc) ut_response_free(r);

    /* a trickle: a byte every 10 seconds, the transport not stopping it */
    adds(&b, OK_H PLAIN "\r\n");
    addn(&b, 't', 1000);
    sim_ms = 1000;
    reset();
    serve(NULL, b.p, b.len);
    mode = BYTES;
    read_ms = 10000;
    rc = follow(PAGE_ADDRESS, r, err);
    check(rc == -1 && !why[0] && strstr(err, "60 seconds") && conns[0].pos <= 7,
          "a trickle ends at the load's deadline");
    sim_ms = 0xffffffffu - 25000;
    reset();
    serve(NULL, b.p, b.len);
    mode = BYTES;
    read_ms = 10000;
    rc = follow(PAGE_ADDRESS, r, err);
    check(rc == -1 && !why[0] && strstr(err, "60 seconds") && conns[0].pos <= 7, "a trickle across uptime's wrap");

    /* ... and when the transport stops it, its message is the one */
    sim_ms = 1000;
    reset();
    serve(NULL, b.p, b.len);
    mode = BYTES;
    read_ms = 20000;
    transport_deadline = 1;
    rc = follow(PAGE_ADDRESS, r, err);
    check(rc == -1 && !why[0] && strstr(err, "the server is too slow (test transport)") && strstr(err, "fixture.test"),
          "the transport's deadline message");

    /* the clock passes the deadline while the request is being written: the rest is not sent */
    sim_ms = 1000;
    reset();
    serve(NULL, LIT(OK_H PLAIN "Content-Length: 2\r\n\r\nok"));
    write_max = 10;
    write_ms = 25000;
    rc = follow(PAGE_ADDRESS, r, err);
    check(rc == -1 && !why[0] && strstr(err, "60 seconds") && conns[0].request_len == 30 && conns[0].pos == 0,
          "the deadline passes in the middle of the request: nothing more is written or read");

    /* a redirect that arrives after the deadline is not followed */
    sim_ms = 1000;
    reset();
    read_ms = 61000;
    moved("http://fixture.test/a", 302, "/b");
    page_at("http://fixture.test/b");
    rc = follow("http://fixture.test/a", r, err);
    check(rc == -1 && !why[0] && opened == 1 && strstr(err, "60 seconds"), "no new connection after the deadline");
    sim_ms = 0;
    free(r);
    text_free(&b);
}

/**** allocation failure ****/

static void allocation_failures(void) {
    struct text chunks = { 0 }, plain = { 0 }, want = { 0 };
    size_t i;
    int s;
    for(i = 0; i < 70000; i++) {
        char ch = (char)('A' + i % 26);
        add(&want, &ch, 1);
    }
    adds(&chunks, CHUNKED);
    for(i = 0; i < want.len; i += 1000) {
        size_t n = want.len - i < 1000 ? want.len - i : 1000;
        addf(&chunks, "%lx\r\n", (unsigned long)n);
        add(&chunks, want.p + i, n);
        adds(&chunks, "\r\n");
    }
    adds(&chunks, "0\r\nX-T: 1\r\n\r\n");
    adds(&plain, OK_H PLAIN "\r\n");
    add(&plain, want.p, want.len);
    for(s = 0; s < 4; s++) {
        static const char *const names[] = { "Content-Length", "chunked, growing", "the connection's end, growing",
                                             "two redirects, then chunked" };
        struct ut_response *r = malloc(sizeof(*r));
        char err[UT_ERR_MAX], what[900];
        long n;
        int rc = -1, ok = 1, failed = 0;
        for(n = 0; n < 200 && ok; n++) {
            reset();
            mode = n % 2 ? RANDOM : WHOLE;
            seed = (unsigned int)n + 1;
            if(s == 0) serve(NULL, LIT(OK_H PLAIN "Content-Length: 5\r\n\r\nHello"));
            else if(s == 1) serve(NULL, chunks.p, chunks.len);
            else if(s == 2) serve(NULL, plain.p, plain.len);
            else {
                moved("http://fixture.test/page", 302, "/b#frag");
                moved("http://fixture.test/b", 307, "http://www.fixture.test/c");
                serve("http://www.fixture.test/c", chunks.p, chunks.len);
            }
            rc = load(PAGE_ADDRESS, r, err, sizeof(err), n);
            if(rc) {
                failed++;
                if(!strstr(err, "memory")) note("the message \"%s\" does not mention memory", err);
            } else {
                size_t len = s ? want.len : 5;
                if(r->len != len || memcmp(r->body, s ? want.p : "Hello", len))
                    note("a wrong body after %ld allocations", n);
                ut_response_free(r);
            }
            if(ut_alloc_live) { note("%ld allocations held after %ld", ut_alloc_live, n); ut_alloc_live = 0; }
            ok = !why[0];
            if(!rc) break;
        }
        snprintf(what, sizeof(what), "allocation failure at every allocation: %s (%d failures, then a page)%s%s",
                 names[s], failed, ok ? "" : ": ", why);
        check(ok && rc == 0 && failed >= 2, what);
        free(r);
    }
    text_free(&chunks);
    text_free(&plain);
    text_free(&want);
}

/**** the rest of the interface ****/

static void interface(void) {
    struct ut_response *r = calloc(1, sizeof(*r));
    struct ut_url *u = malloc(sizeof(*u));
    char err[64], tiny[8];
    ut_response_free(NULL);
    ut_response_free(r);
    check(zeroed(r, sizeof(*r)), "ut_response_free of a zeroed response and of NULL");
    check(ut_url_parse(PAGE_ADDRESS, u, err, sizeof(err)) == 0, "an address");
    reset();
    check(ut_fetch(NULL, &cfg, r, err, sizeof(err)) == -1 && safe_message(err) && opened == 0, "no address");
    check(ut_fetch(u, &cfg, NULL, err, sizeof(err)) == -1 && safe_message(err) && opened == 0,
          "nowhere to put the page");
    reset();
    serve(NULL, LIT("nonsense"));
    check(ut_fetch(u, &cfg, r, tiny, sizeof(tiny)) == -1 && strlen(tiny) == sizeof(tiny) - 1, "a small error buffer");
    reset();
    serve(NULL, LIT("nonsense"));
    check(ut_fetch(u, &cfg, r, NULL, 0) == -1 && zeroed(r, sizeof(*r)), "no error buffer at all");
    check(ut_alloc_live == 0 && live_conns == 0, "nothing held after those");
    free(u);
    free(r);
}

int main(void) {
    TABLE(lengths);
    TABLE(chunked);
    TABLE(codings);
    TABLE(others);
    TABLE(lines);
    TABLE(types);
    every_pair(chunked[0].name, chunked[0].data, chunked[0].len, &chunked[0].e);
    every_pair(others[3].name, others[3].data, others[3].len, &others[3].e);
    not_read();
    cut_short();
    header_limits();
    body_limits();
    long_fields();
    requests();
    redirects();
    downgrade_messages();
    sniffing();
    chunk_line_limits();
    deadline_tests();
    allocation_failures();
    interface();
    printf("%ld loads\n", loads);
    printf("UNDERTOW HTTP TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
