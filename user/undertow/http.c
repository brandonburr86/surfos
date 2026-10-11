/*
SurfOS Undertow: HTTP/1.1 GET with redirects
----------------------
File: http.c  Date: 10/11/26
----------------------
The contract is http.h's. One request per connection: the request is written
whole through partial writes; the response is read through a small buffer, a
line at a time for the status line, the header lines and a chunked body's own
lines, so nothing depends on how the bytes arrive. RFC 9112 is applied
strictly wherever a looser reading would have to guess: obsolete line folding,
a CR that does not end a line, whitespace before a colon, two different
lengths, Transfer-Encoding with Content-Length or in an HTTP/1.0 response, and
any transfer coding but chunked are refused. Bodies Undertow will not show
(not text, compressed, too big) and the bodies of redirects are never read.

UT_HEADERS_MAX counts, for one request, every status line and header line
with its line end and the empty lines that end the heads (interim responses'
and the final one's), and the trailer lines of a chunked body; a chunk's own
lines are bounded by CHUNK_LINE_MAX. The whole load, every redirect included,
has one deadline: every connection gets it, and every read and write checks
it as well.
*/

#include <stdlib.h>
#include <string.h>
#include "undertow.h"
#include "url.h"
#include "config.h"
#include "transport.h"
#include "http.h"

#define RBUF            4096            /* the read buffer */
#define CHUNK_LINE_MAX  4096            /* a chunk's size line, extensions and line end included */
#define BODY_START      16384           /* the first allocation for a body whose length is not known */
#define SNIFF_BYTES     1445            /* the bytes looked at for binary data, as in the WHATWG MIME sniffing */
#define SHOW_MAX        200             /* a server's value quoted in a message */
#define REQ_MAX         (3 * UT_URL_MAX + 512)  /* the target percent-encoded whole, the host, the fixed lines */

enum { L_STATUS, L_HEADER, L_TRAILER, L_CHUNK };       /* the kinds of line */
enum { F_NONE, F_LENGTH, F_CHUNKED, F_CLOSE };         /* how a body is framed */

struct head {                           /* what the head of one response says */
    int status, minor;                  /* HTTP/1.minor */
    char reason[64];                    /* safe text */
    int length_fields, lengths;         /* Content-Length lines, and the values in them */
    int length_bad, length_conflict;
    size_t length;                      /* the value; anything over UT_BODY_MAX counts as UT_BODY_MAX + 1 */
    int codings;                        /* Transfer-Encoding lines */
    int chunked, other_coding;          /* how many "chunked"; anything else */
    int empty_coding;                   /* a Transfer-Encoding line without any coding */
    char coding_text[96];               /* the codings as sent, for the message */
    int typed;                          /* a Content-Type that parses */
    int text;                           /* ... and is text */
    char type[128];                     /* its media type, lowercase */
    char charset[32];                   /* its charset, lowercase */
    int compressed;                     /* a Content-Encoding other than identity */
    char encoding_text[96];
    int locations, location_conflict, location_long;
    char location[UT_URL_MAX + 1];
};

struct body {
    char *p;
    size_t len, cap;                    /* cap > len once anything is allocated: room for the NUL */
};

struct fetch {                          /* one load, on the heap */
    struct ut_transport *t;
    const char *host;                   /* the current address's, for messages */
    unsigned int deadline;
    unsigned char rbuf[RBUF];
    size_t rpos, rend;
    int eof;
    size_t used;                        /* this request's head and trailer bytes */
    size_t line_len;
    char line[UT_HEADERS_MAX + 1];
    struct head h;
    struct ut_url cur, next;
    char seen[UT_REDIRECTS_MAX + 1][UT_URL_MAX + 1];   /* the addresses loaded, without fragments */
    char ref[UT_URL_MAX + 1];
    char req[REQ_MAX];
    char show[3 * UT_URL_MAX + 4];
    struct ut_response resp;
};

/**** characters ****/

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

static int ows(int c) {
    return c == ' ' || c == '\t';
}

/* RFC 9110 5.6.2 */
static int tchar(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("!#$%&'*+-.^_`|~", c));
}

static int hexval(int c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* src into dst (size bytes), cut if it must be */
static void copy(char *dst, size_t size, const char *src) {
    size_t n = strlen(src);
    if(n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* s (n bytes) is name, in any case */
static int named(const char *s, size_t n, const char *name) {
    size_t i;
    if(strlen(name) != n) return 0;
    for(i = 0; i < n; i++) if(lower((unsigned char)s[i]) != name[i]) return 0;
    return 1;
}

/* s (len bytes) printable for a message: bytes outside printable ASCII percent-encoded, as an
   address carries them; at most max bytes, ending in "..." when cut */
static const char *show(struct fetch *f, const char *s, size_t len, size_t max) {
    static const char hex[] = "0123456789ABCDEF";
    size_t i, n = 0;
    if(max > sizeof(f->show) - 4) max = sizeof(f->show) - 4;
    for(i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        int plain = c > 0x20 && c < 0x7f;
        if(n + (plain ? 1 : 3) > max) {
            memcpy(f->show + n, "...", 4);
            return f->show;
        }
        if(plain) f->show[n++] = (char)c;
        else {
            f->show[n++] = '%';
            f->show[n++] = hex[c >> 4];
            f->show[n++] = hex[c & 15];
        }
    }
    f->show[n] = 0;
    return f->show;
}

/* an address without its fragment, for messages and for the loop check */
static size_t unfragmented(const struct ut_url *u) {
    return strcspn(u->text, "#");
}

/**** messages ****/

static int late(struct fetch *f, char *err, size_t cap) {
    return ut_error(err, cap, "%s: the page did not load within %u seconds", f->host, UT_LOAD_MS / 1000);
}

static int broken(struct fetch *f, char *err, size_t cap) {
    const char *why = ut_transport_error(f->t);
    if(!why || !*why) return ut_error(err, cap, "the connection to %s failed", f->host);
    return ut_error(err, cap, "the connection to %s failed: %s", f->host, why);
}

static int too_big(struct fetch *f, char *err, size_t cap) {
    return ut_error(err, cap, "the page from %s is over %u MB, more than Undertow loads", f->host,
                    UT_BODY_MAX / (1024 * 1024));
}

static int no_memory(struct fetch *f, char *err, size_t cap) {
    return ut_error(err, cap, "out of memory for the page from %s", f->host);
}

/**** reading ****/

/* More bytes into the empty buffer: 0 (f->eof at the end of the stream), or -1 and a message.
   The load's deadline is checked here too, so a server that sends a byte now and then cannot
   keep a load going past it whatever the transport does. */
static int fill(struct fetch *f, char *err, size_t cap) {
    int n;
    f->rpos = f->rend = 0;
    if(f->eof) return 0;
    if(ut_expired(f->deadline, ut_now())) return late(f, err, cap);
    n = ut_transport_read(f->t, f->rbuf, RBUF);
    if(n < 0 || n > RBUF) return broken(f, err, cap);
    if(!n) f->eof = 1;
    f->rend = (size_t)n;
    return 0;
}

static int cut_short(struct fetch *f, int kind, size_t got, char *err, size_t cap) {
    if(kind == L_STATUS && !f->used && !got)
        return ut_error(err, cap, "%s closed the connection without answering", f->host);
    if(kind == L_STATUS || kind == L_HEADER)
        return ut_error(err, cap, "%s closed the connection before the end of the response headers", f->host);
    return ut_error(err, cap, "%s closed the connection before the end of the page", f->host);
}

/* One line into f->line: f->line_len bytes and a NUL, without the LF or a CR before it. The
   head's lines and the trailer lines come out of the request's UT_HEADERS_MAX (the empty line
   that ends a chunked body is the body's own); a chunk's lines are bounded on their own. */
static int read_line(struct fetch *f, int kind, char *err, size_t cap) {
    size_t limit, n = 0, k;
    unsigned char *lf;
    int empty;
    if(kind == L_CHUNK) limit = CHUNK_LINE_MAX;
    else if(kind == L_TRAILER) limit = UT_HEADERS_MAX - f->used + 2;    /* + the empty line that ends the body */
    else limit = UT_HEADERS_MAX - f->used;
    if(limit > UT_HEADERS_MAX) limit = UT_HEADERS_MAX;
    for(;;) {
        if(f->rpos == f->rend) {
            if(fill(f, err, cap)) return -1;
            if(f->eof) return cut_short(f, kind, n, err, cap);
        }
        lf = memchr(f->rbuf + f->rpos, '\n', f->rend - f->rpos);
        k = lf ? (size_t)(lf - (f->rbuf + f->rpos)) + 1 : f->rend - f->rpos;
        if(k > limit - n) {
            if(kind == L_CHUNK)
                return ut_error(err, cap, "%s sent a chunk size line over %u bytes", f->host, CHUNK_LINE_MAX);
            return ut_error(err, cap, "%s sent more than %u KB of headers", f->host, UT_HEADERS_MAX / 1024);
        }
        memcpy(f->line + n, f->rbuf + f->rpos, k);
        f->rpos += k;
        n += k;
        /* not HTTP at all: say so as soon as it shows, rather than wait for a line end */
        if(kind == L_STATUS && memcmp(f->line, "HTTP/", n < 5 ? n : 5))
            return ut_error(err, cap, "%s did not answer with an HTTP status line", f->host);
        if(lf) break;
    }
    empty = n == 1 || (n == 2 && f->line[0] == '\r');
    if(kind == L_STATUS || kind == L_HEADER) f->used += n;
    else if(kind == L_TRAILER && !empty) {
        f->used += n;
        if(f->used > UT_HEADERS_MAX)
            return ut_error(err, cap, "%s sent more than %u KB of headers", f->host, UT_HEADERS_MAX / 1024);
    }
    n--;
    if(n && f->line[n - 1] == '\r') n--;
    f->line[n] = 0;
    f->line_len = n;
    return 0;
}

/* The next n bytes of the stream into dst. */
static int take(struct fetch *f, char *dst, size_t n, char *err, size_t cap) {
    while(n) {
        size_t k;
        if(f->rpos == f->rend) {
            if(fill(f, err, cap)) return -1;
            if(f->eof) return cut_short(f, L_CHUNK, 1, err, cap);
        }
        k = f->rend - f->rpos;
        if(k > n) k = n;
        memcpy(dst, f->rbuf + f->rpos, k);
        f->rpos += k;
        dst += k;
        n -= k;
    }
    return 0;
}

/**** the head ****/

/* "HTTP/1.x NNN reason". The reason may hold anything but NUL and CR; it is kept as safe text. */
static int parse_status(struct fetch *f, char *err, size_t cap) {
    const char *s = f->line;
    size_t n = f->line_len;
    struct head *h = &f->h;
    if(n < 12 || memcmp(s, "HTTP/1.", 7) || s[7] < '0' || s[7] > '9' || s[8] != ' ' ||
       s[9] < '1' || s[9] > '5' || s[10] < '0' || s[10] > '9' || s[11] < '0' || s[11] > '9' ||
       (n > 12 && s[12] != ' ') || memchr(s, 0, n) || memchr(s, '\r', n))
        return ut_error(err, cap, "%s sent a malformed status line", f->host);
    memset(h, 0, sizeof(*h));
    h->minor = s[7] - '0';
    h->status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    if(n > 13) ut_safe_text(h->reason, sizeof(h->reason), s + 13, n - 13, 1);
    return 0;
}

/* Content-Length: digits, or a list of the same value (RFC 9110 8.6: "42, 42" is a duplicated
   field combined); two different values are a conflict, anything else is malformed. */
static void add_length(struct head *h, const char *v) {
    h->length_fields++;
    for(;;) {
        size_t value = 0;
        int digits = 0;
        while(ows(*v)) v++;
        for(; *v >= '0' && *v <= '9'; v++, digits++) {
            value = value * 10 + (size_t)(*v - '0');
            if(value > UT_BODY_MAX) value = UT_BODY_MAX + 1;    /* too big is too big, whatever its digits */
        }
        while(ows(*v)) v++;
        if(!digits || (*v && *v != ',')) {
            h->length_bad = 1;
            return;
        }
        if(h->lengths++ && value != h->length) h->length_conflict = 1;
        h->length = value;
        if(!*v++) return;
    }
}

/* text (bounded) gets ", " and v */
static void add_text(char *text, size_t size, const char *v) {
    size_t n = strlen(text);
    if(!*v) return;
    if(n && n + 2 < size) {
        memcpy(text + n, ", ", 3);
        n += 2;
    }
    while(*v && n + 1 < size) text[n++] = *v++;
    text[n] = 0;
}

/* Transfer-Encoding: only "chunked", once, without parameters, is read */
static void add_codings(struct head *h, const char *v) {
    int any = 0;
    add_text(h->coding_text, sizeof(h->coding_text), v);
    h->codings++;
    while(*v) {
        const char *name, *end;
        while(ows(*v) || *v == ',') v++;
        if(!*v) break;
        any = 1;
        name = v;
        while(tchar((unsigned char)*v)) v++;
        end = v;
        while(ows(*v)) v++;
        if(named(name, (size_t)(end - name), "chunked") && (!*v || *v == ',')) h->chunked++;
        else h->other_coding = 1;
        while(*v && *v != ',') v++;
    }
    if(!any) h->empty_coding = 1;
}

/* Content-Encoding: anything but identity is a coding Undertow does not undo */
static void add_encodings(struct head *h, const char *v) {
    add_text(h->encoding_text, sizeof(h->encoding_text), v);
    while(*v) {
        const char *name, *end;
        while(ows(*v) || *v == ',') v++;
        if(!*v) break;
        name = v;
        while(*v && *v != ',') v++;
        end = v;
        while(end > name && ows(end[-1])) end--;
        if(!named(name, (size_t)(end - name), "identity")) h->compressed = 1;
    }
}

/* A quoted string (RFC 9110 5.6.4) or a token at *p: its value in out (cut to size - 1 bytes),
   0, and *p past it; -1 when there is neither. ok is cleared when the value has a byte that is
   not printable ASCII. */
static int param_value(const char **p, char *out, size_t size, int *ok) {
    const char *s = *p;
    size_t n = 0;
    *ok = 1;
    if(*s == '"') {
        for(s++; *s != '"'; s++) {
            if(!*s) return -1;
            if(*s == '\\' && s[1]) s++;
            if((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) *ok = 0;
            if(n + 1 < size) out[n++] = (char)lower((unsigned char)*s);
        }
        s++;
    } else {
        if(!tchar((unsigned char)*s)) return -1;
        for(; tchar((unsigned char)*s); s++) if(n + 1 < size) out[n++] = (char)lower((unsigned char)*s);
    }
    out[n] = 0;
    *p = s;
    return 0;
}

/* Content-Type (RFC 9110 8.3): type "/" subtype, then parameters. A value that is not a media
   type is ignored, as if there were none; a later one that is replaces an earlier one. A
   parameter that does not parse ends the parameters but keeps the type. */
static void set_type(struct head *h, const char *v) {
    const char *p = v, *type, *sub;
    size_t tl, sl, i, n = 0;
    char charset[sizeof(h->charset)], value[sizeof(h->charset)];
    int have_charset = 0, ok;
    type = p;
    while(tchar((unsigned char)*p)) p++;
    tl = (size_t)(p - type);
    if(!tl || *p++ != '/') return;
    sub = p;
    while(tchar((unsigned char)*p)) p++;
    sl = (size_t)(p - sub);
    if(!sl) return;
    while(ows(*p)) p++;
    if(*p && *p != ';') return;
    charset[0] = 0;
    while(*p == ';') {
        const char *name;
        size_t nl;
        p++;
        while(ows(*p)) p++;
        name = p;
        while(tchar((unsigned char)*p)) p++;
        nl = (size_t)(p - name);
        if(!nl) {
            if(*p == ';' || !*p) continue;
            break;
        }
        if(*p++ != '=' || param_value(&p, value, sizeof(value), &ok)) break;
        if(!have_charset && named(name, nl, "charset")) {
            have_charset = 1;
            if(ok) copy(charset, sizeof(charset), value);
        }
        while(ows(*p)) p++;
    }
    h->typed = 1;
    h->text = named(type, tl, "text") || (named(type, tl, "application") && named(sub, sl, "xhtml+xml"));
    for(i = 0; i < tl + 1 + sl && n + 1 < sizeof(h->type); i++) h->type[n++] = (char)lower((unsigned char)type[i]);
    h->type[n] = 0;
    copy(h->charset, sizeof(h->charset), charset);
}

static void set_location(struct head *h, const char *v, size_t len) {
    if(h->locations++) {
        if(h->location_long || len > UT_URL_MAX || strcmp(h->location, v)) h->location_conflict = 1;
        return;
    }
    if(len > UT_URL_MAX) h->location_long = 1;
    else memcpy(h->location, v, len + 1);
}

/* "name: value" (a trailer line's is only checked: trailers never change the page) */
static int parse_header(struct fetch *f, int kind, char *err, size_t cap) {
    const char *what = kind == L_TRAILER ? "trailer" : "header";
    char *s = f->line, *colon, *v, *end;
    size_t n = f->line_len, i;
    if(ows(s[0]))
        return ut_error(err, cap, "%s sent a folded %s line (obsolete line folding), which Undertow refuses",
                        f->host, what);
    colon = memchr(s, ':', n);
    if(!colon || colon == s || memchr(s, 0, n) || memchr(s, '\r', n))
        return ut_error(err, cap, "%s sent a malformed %s line", f->host, what);
    for(i = 0; s + i < colon; i++)
        if(!tchar((unsigned char)s[i])) return ut_error(err, cap, "%s sent a malformed %s line", f->host, what);
    if(kind == L_TRAILER) return 0;
    v = colon + 1;
    while(ows(*v)) v++;
    end = s + n;
    while(end > v && ows(end[-1])) end--;
    *end = 0;
    n = (size_t)(colon - s);
    if(named(s, n, "content-length")) add_length(&f->h, v);
    else if(named(s, n, "transfer-encoding")) add_codings(&f->h, v);
    else if(named(s, n, "content-type")) set_type(&f->h, v);
    else if(named(s, n, "content-encoding")) add_encodings(&f->h, v);
    else if(named(s, n, "location")) set_location(&f->h, v, (size_t)(end - v));
    return 0;
}

/* Responses until a final one, whose head is then in f->h. */
static int read_head(struct fetch *f, char *err, size_t cap) {
    int interim = 0;
    f->used = 0;
    for(;;) {
        if(read_line(f, L_STATUS, err, cap) || parse_status(f, err, cap)) return -1;
        for(;;) {
            if(read_line(f, L_HEADER, err, cap)) return -1;
            if(!f->line_len) break;
            if(parse_header(f, L_HEADER, err, cap)) return -1;
        }
        if(f->h.status >= 200) return 0;
        if(f->h.status == 101)
            return ut_error(err, cap, "%s switched protocols (101), which Undertow does not do", f->host);
        if(++interim > UT_INTERIM_MAX)
            return ut_error(err, cap, "%s sent more than %d interim (1xx) responses", f->host, UT_INTERIM_MAX);
    }
}

/**** the request ****/

static int put(struct fetch *f, size_t *n, const char *s, size_t len) {
    if(len > REQ_MAX - *n) return -1;
    memcpy(f->req + *n, s, len);
    *n += len;
    return 0;
}

/* The request for f->cur, written whole. The target is never the fragment; bytes over 0x7e
   (url.c keeps UTF-8 as it is) go percent-encoded, as browsers send them. */
static int send_request(struct fetch *f, char *err, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    static const char tail[] = "User-Agent: " UT_USER_AGENT "\r\n"
                               "Accept: text/html, application/xhtml+xml, text/plain;q=0.9, */*;q=0.1\r\n"
                               "Accept-Encoding: identity\r\n"
                               "Connection: close\r\n"
                               "\r\n";
    const struct ut_url *u = &f->cur;
    const unsigned char *s;
    char port[8];
    size_t n = 0, done = 0, d = sizeof(port);
    unsigned int p = u->port;
    int bad = put(f, &n, "GET ", 4) || put(f, &n, "/", u->target[0] != '/');
    for(s = (const unsigned char *)u->target; *s && !bad; s++) {
        if(*s <= 0x20 || *s == 0x7f) bad = 1;
        else if(*s < 0x7f) bad = put(f, &n, (const char *)s, 1);
        else {
            char e[3];
            e[0] = '%';
            e[1] = hex[*s >> 4];
            e[2] = hex[*s & 15];
            bad = put(f, &n, e, 3);
        }
    }
    bad = bad || put(f, &n, " HTTP/1.1\r\nHost: ", 17) || put(f, &n, u->host, strlen(u->host));
    if(p != (u->secure ? 443u : 80u)) {
        do { port[--d] = (char)('0' + p % 10); p /= 10; } while(p);
        port[--d] = ':';
        bad = bad || put(f, &n, port + d, sizeof(port) - d);
    }
    bad = bad || put(f, &n, "\r\n", 2) || put(f, &n, tail, sizeof(tail) - 1);
    if(bad)
        return ut_error(err, cap, "the address %s cannot be requested", show(f, u->text, unfragmented(u), SHOW_MAX));
    while(done < n) {
        int w;
        if(ut_expired(f->deadline, ut_now())) return late(f, err, cap);
        w = ut_transport_write(f->t, f->req + done, n - done);
        if(w <= 0 || (size_t)w > n - done) {
            const char *why = ut_transport_error(f->t);
            return ut_error(err, cap, "could not send the request to %s%s%s", f->host, why && *why ? ": " : "",
                            why ? why : "");
        }
        done += (size_t)w;
    }
    return 0;
}

/**** the body ****/

/* room in b for need bytes and a NUL; the capacity doubles from BODY_START */
static int room(struct fetch *f, struct body *b, size_t need, char *err, size_t cap) {
    size_t want;
    char *p;
    if(need < b->cap) return 0;
    want = b->cap ? b->cap : BODY_START;
    while(want <= need) want *= 2;
    if(want > UT_BODY_MAX + 1) want = UT_BODY_MAX + 1;
    p = realloc(b->p, want);
    if(!p) return no_memory(f, err, cap);
    b->p = p;
    b->cap = want;
    return 0;
}

/* a chunk's size line: hexadecimal digits, then spaces or extensions (ignored) */
static int chunk_size(struct fetch *f, size_t *size, char *err, size_t cap) {
    const char *s = f->line;
    size_t v = 0;
    int digits = 0;
    if(!memchr(s, 0, f->line_len) && !memchr(s, '\r', f->line_len)) {
        for(; hexval((unsigned char)*s) >= 0; s++, digits++) {
            v = v * 16 + (size_t)hexval((unsigned char)*s);
            if(v > UT_BODY_MAX) v = UT_BODY_MAX + 1;
        }
        while(ows(*s)) s++;
        if(digits && (!*s || *s == ';')) {
            *size = v;
            return 0;
        }
    }
    return ut_error(err, cap, "%s sent a malformed chunk size", f->host);
}

static int chunks(struct fetch *f, struct body *b, char *err, size_t cap) {
    for(;;) {
        size_t size = 0;
        if(read_line(f, L_CHUNK, err, cap) || chunk_size(f, &size, err, cap)) return -1;
        if(!size) break;
        if(size > UT_BODY_MAX - b->len) return too_big(f, err, cap);
        if(room(f, b, b->len + size, err, cap) || take(f, b->p + b->len, size, err, cap)) return -1;
        b->len += size;
        if(read_line(f, L_CHUNK, err, cap)) return -1;
        if(f->line_len)
            return ut_error(err, cap, "%s sent a malformed chunked body (no line end after a chunk's data)", f->host);
    }
    for(;;) {
        if(read_line(f, L_TRAILER, err, cap)) return -1;
        if(!f->line_len) return 0;
        if(parse_header(f, L_TRAILER, err, cap)) return -1;
    }
}

static int until_close(struct fetch *f, struct body *b, char *err, size_t cap) {
    for(;;) {
        size_t k;
        if(f->rpos == f->rend) {
            if(fill(f, err, cap)) return -1;
            if(f->eof) return 0;
        }
        k = f->rend - f->rpos;
        if(k > UT_BODY_MAX - b->len) return too_big(f, err, cap);
        if(room(f, b, b->len + k, err, cap)) return -1;
        memcpy(b->p + b->len, f->rbuf + f->rpos, k);
        b->len += k;
        f->rpos += k;
    }
}

/* Text unless the first bytes have binary data bytes (the WHATWG's MIME sniffing: C0 controls but
   tab, line feed, form feed, carriage return and ESC) or a NUL comes anywhere. */
static int looks_like_text(const char *p, size_t n) {
    size_t i;
    if(memchr(p, 0, n)) return 0;
    for(i = 0; i < n && i < SNIFF_BYTES; i++) {
        unsigned char c = (unsigned char)p[i];
        if(c <= 0x08 || c == 0x0b || (c >= 0x0e && c <= 0x1a) || (c >= 0x1c && c <= 0x1f)) return 0;
    }
    return 1;
}

/* The final response (not a redirect): its body, if Undertow will show it, in f->resp. */
static int page(struct fetch *f, char *err, size_t cap) {
    struct head *h = &f->h;
    struct body b = { NULL, 0, 0 };
    const char *tls;
    int framing = F_CLOSE;
    if(h->status == 204 || h->status == 304) framing = F_NONE;     /* whatever the head says (RFC 9112 6.3) */
    else if(h->codings) {
        if(h->minor == 0)
            return ut_error(err, cap, "%s sent Transfer-Encoding in an HTTP/1.0 response: the length of the page "
                            "is ambiguous", f->host);
        if(h->length_fields)
            return ut_error(err, cap, "%s sent both Transfer-Encoding and Content-Length: the length of the page "
                            "is ambiguous", f->host);
        if(h->empty_coding) return ut_error(err, cap, "%s sent an empty Transfer-Encoding", f->host);
        if(h->chunked != 1 || h->other_coding)
            return ut_error(err, cap, "%s sent the page with Transfer-Encoding: %s, which Undertow does not read "
                            "(only chunked)", f->host, h->coding_text);
        framing = F_CHUNKED;
    } else if(h->length_fields) {
        if(h->length_bad) return ut_error(err, cap, "%s sent a malformed Content-Length", f->host);
        if(h->length_conflict) return ut_error(err, cap, "%s sent two different Content-Length values", f->host);
        if(h->length > UT_BODY_MAX) return too_big(f, err, cap);
        framing = F_LENGTH;
    }
    if(h->typed && !h->text) {
        if(h->status == 200)
            return ut_error(err, cap, "%s is not text: Undertow shows HTML and plain text", h->type);
        return ut_error(err, cap, "HTTP %d %s: %s is not text: Undertow shows HTML and plain text", h->status,
                        h->reason, h->type);
    }
    if(h->compressed)
        return ut_error(err, cap, "%s sent the page compressed (Content-Encoding: %s), which Undertow does not "
                        "read", f->host, h->encoding_text);
    if(framing == F_LENGTH) {
        b.p = malloc(h->length + 1);
        if(!b.p) return no_memory(f, err, cap);
        b.cap = h->length + 1;
        if(take(f, b.p, h->length, err, cap)) goto fail;
        b.len = h->length;
    } else if(framing == F_CHUNKED) {
        if(chunks(f, &b, err, cap)) goto fail;
    } else if(framing == F_CLOSE) {
        if(until_close(f, &b, err, cap)) goto fail;
    }
    if(!b.p) {
        b.p = malloc(1);
        if(!b.p) return no_memory(f, err, cap);
        b.cap = 1;
    }
    b.p[b.len] = 0;
    if(!h->typed && !looks_like_text(b.p, b.len)) {
        ut_error(err, cap, "the page from %s does not say what it is and does not look like text: Undertow shows "
                 "HTML and plain text", f->host);
        goto fail;
    }
    if(b.cap - b.len > 4096) {          /* give back what growing left over; keep it all if that fails */
        char *p = realloc(b.p, b.len + 1);
        if(p) b.p = p;
    }
    memset(&f->resp, 0, sizeof(f->resp));
    f->resp.body = (unsigned char *)b.p;
    f->resp.len = b.len;
    f->resp.status = h->status;
    f->resp.verified = ut_transport_verified(f->t) ? 1 : 0;
    tls = ut_transport_tls(f->t);
    if(tls) ut_safe_text(f->resp.tls, sizeof(f->resp.tls), tls, strlen(tls), 0);
    copy(f->resp.reason, sizeof(f->resp.reason), h->reason);
    if(h->typed) {
        copy(f->resp.content_type, sizeof(f->resp.content_type), h->type);
        copy(f->resp.charset, sizeof(f->resp.charset), h->charset);
    }
    return 0;
fail:
    free(b.p);
    return -1;
}

/**** one request, and the redirects ****/

/* One request to f->cur: 0 with the page in f->resp, 1 for a redirect (its head in f->h), or
   -1 with a message. The connection is closed whatever happens. */
static int exchange(struct fetch *f, const struct ut_config *cfg, int redirected, char *err, size_t cap) {
    int rc = -1;
    f->host = f->cur.host;
    f->t = NULL;
    f->rpos = f->rend = 0;
    f->eof = 0;
    if(ut_expired(f->deadline, ut_now())) return late(f, err, cap);
    if(ut_transport_open(&f->t, &f->cur, cfg, f->deadline, err, cap)) {
        f->t = NULL;
        if(redirected)
            ut_error(err, cap, "redirected to %s: %s", show(f, f->cur.text, unfragmented(&f->cur), SHOW_MAX), err);
        return -1;
    }
    if(!send_request(f, err, cap) && !read_head(f, err, cap)) {
        int s = f->h.status;
        if(s == 301 || s == 302 || s == 303 || s == 307 || s == 308) rc = 1;
        else rc = page(f, err, cap);
    }
    ut_transport_close(f->t);
    f->t = NULL;
    return rc;
}

/* The redirect in f->h from f->cur, after the checks: f->cur becomes its target. */
static int follow(struct fetch *f, int redirects, char *err, size_t cap) {
    struct head *h = &f->h;
    char uerr[128];
    size_t i, n = 0;
    if(!h->locations || (!h->location_long && !h->location[0]))
        return ut_error(err, cap, "%s answered %d %s without a Location to go to", f->host, h->status, h->reason);
    if(h->location_conflict) return ut_error(err, cap, "%s sent two different Locations in one redirect", f->host);
    if(h->location_long)
        return ut_error(err, cap, "cannot follow the redirect from %s: its Location is over %u bytes", f->host,
                        UT_URL_MAX);
    /* a space goes as %20, as browsers send it (url.c refuses one in an address typed in) */
    for(i = 0; h->location[i]; i++) {
        int space = h->location[i] == ' ';
        if(n + (space ? 3 : 1) > UT_URL_MAX)
            return ut_error(err, cap, "cannot follow the redirect from %s: its Location is over %u bytes", f->host,
                            UT_URL_MAX);
        if(space) {
            memcpy(f->ref + n, "%20", 3);
            n += 3;
        } else f->ref[n++] = h->location[i];
    }
    f->ref[n] = 0;
    if(ut_url_resolve(&f->cur, f->ref, &f->next, uerr, sizeof(uerr)))
        return ut_error(err, cap, "cannot follow the redirect from %s to %s: %s", f->host,
                        show(f, h->location, strlen(h->location), SHOW_MAX), uerr);
    /* without a fragment of its own, the target keeps the one the address had (RFC 9110 10.2.2);
       one that does not fit is dropped rather than the page */
    if(!strchr(h->location, '#') && f->cur.fragment[0]) {
        size_t tl = strlen(f->next.text), fl = strlen(f->cur.fragment);
        if(tl + 1 + fl <= UT_URL_MAX) {
            f->next.text[tl] = '#';
            memcpy(f->next.text + tl + 1, f->cur.fragment, fl + 1);
            memcpy(f->next.fragment, f->cur.fragment, fl + 1);
        }
    }
    if(f->cur.secure && !f->next.secure)
        return ut_error(err, cap, "%s redirected from HTTPS to unencrypted HTTP, which Undertow does not follow by "
                        "itself; to read it unencrypted, open %s", f->host,
                        show(f, f->next.text, strlen(f->next.text), sizeof(f->show)));
    n = unfragmented(&f->next);
    for(i = 0; i <= (size_t)redirects; i++)
        if(strlen(f->seen[i]) == n && !memcmp(f->seen[i], f->next.text, n))
            return ut_error(err, cap, "the page redirects in a loop (back to %s)", show(f, f->next.text, n, SHOW_MAX));
    if(redirects == UT_REDIRECTS_MAX)
        return ut_error(err, cap, "more than %d redirects (the next was to %s)", UT_REDIRECTS_MAX,
                        show(f, f->next.text, n, SHOW_MAX));
    memcpy(&f->cur, &f->next, sizeof(f->cur));
    memcpy(f->seen[redirects + 1], f->cur.text, n);
    f->seen[redirects + 1][n] = 0;
    return 0;
}

int ut_fetch(const struct ut_url *url, const struct ut_config *cfg, struct ut_response *out,
             char *err, size_t cap) {
    struct fetch *f;
    int redirects = 0, rc;
    size_t n;
    if(!url || !out) return ut_error(err, cap, "nothing to load");
    f = malloc(sizeof(*f));
    if(!f) return ut_error(err, cap, "out of memory for loading %s", url->host);
    f->deadline = ut_now() + UT_LOAD_MS;
    memcpy(&f->cur, url, sizeof(f->cur));
    n = unfragmented(&f->cur);
    memcpy(f->seen[0], f->cur.text, n);
    f->seen[0][n] = 0;
    for(;;) {
        rc = exchange(f, cfg, redirects > 0, err, cap);
        if(rc != 1) break;
        rc = follow(f, redirects, err, cap);
        if(rc) break;
        redirects++;
    }
    if(!rc) {
        memcpy(&f->resp.url, &f->cur, sizeof(f->cur));
        f->resp.redirects = redirects;
        memcpy(out, &f->resp, sizeof(*out));
    }
    free(f);
    return rc;
}

void ut_response_free(struct ut_response *r) {
    if(!r) return;
    free(r->body);
    memset(r, 0, sizeof(*r));
}
