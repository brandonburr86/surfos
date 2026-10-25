/*
SurfOS downloader: fetch
--------------------
File: fetchhttp.c   Date: 10/24/26
--------------------
What fetch reads of an HTTP response, with no system calls (fetch.h): the end of the head,
the head's status line and fields, the chunked transfer coding undone a piece at a time, the
name a download is saved under, and sizes as text. The rules for the head are Undertow's
(user/undertow/http.c), so that the browser and the downloader refuse the same responses.
Host-tested as it is (tests/host/test_fetch.c).
*/

#include <stdio.h>
#include <string.h>
#include "fetch.h"

/**** characters ****/

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int ows(int c) {
    return c == ' ' || c == '\t';
}

/* RFC 9110 5.6.2 */
static int tchar(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("!#$%&'*+-.^_`|~", c));
}

static int hex(int c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* is p (n bytes) name, in any case? */
static int named(const char *p, int n, const char *name) {
    int i;
    if((int)strlen(name) != n) return 0;
    for(i = 0; i < n; i++) if(lower((unsigned char)p[i]) != name[i]) return 0;
    return 1;
}

/* p (n bytes) into out (size bytes), what is not printable ASCII as '?' */
static void copy_safe(char *out, int size, const char *p, int n) {
    int i;
    if(n > size - 1) n = size - 1;
    for(i = 0; i < n; i++) out[i] = (unsigned char)p[i] < 0x20 || (unsigned char)p[i] > 0x7e ? '?' : p[i];
    out[n] = 0;
}

/**** the head ****/

int fx_head_end(const char *buf, int len) {
    int i;
    for(i = 0; i < len; i++) {
        if(buf[i] != '\n') continue;
        if(i + 1 < len && buf[i + 1] == '\n') return i + 2;                         /* LF LF */
        if(i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n') return i + 3;   /* LF CR LF */
    }
    return len >= FX_HEAD_MAX ? -1 : 0;
}

/* digits alone, as a number: 0 and *out; 1 for 2 GB or more (FX_FILE_MAX in *out); -1 for no number. A long has
   32 bits on SurfOS: nothing here goes past FX_FILE_MAX */
static int number(const char *p, int n, long *out) {
    unsigned long v = 0;
    int i, big = 0;
    if(n < 1) return -1;
    for(i = 0; i < n; i++) {
        unsigned long d = (unsigned long)(p[i] - '0');
        if(p[i] < '0' || p[i] > '9') return -1;
        if(!big && v > ((unsigned long)FX_FILE_MAX - d) / 10) big = 1;
        if(!big) v = v * 10 + d;
    }
    *out = big ? FX_FILE_MAX : (long)v;
    return big;
}

/* Content-Length: digits, or a list of the same value ("42, 42" is a field sent twice and combined,
   RFC 9110 8.6); -1 malformed, -2 two different values, -3 2 GB or more */
static int add_length(struct fx_head *h, int *seen, const char *v, const char *end) {
    for(;;) {
        const char *d;
        long l;
        int r;
        while(v < end && ows(*v)) v++;
        for(d = v; d < end && *d >= '0' && *d <= '9'; d++);
        if((r = number(v, (int)(d - v), &l)) < 0) return -1;
        if(r) return -3;
        for(v = d; v < end && ows(*v); v++);
        if(v < end && *v != ',') return -1;
        if((*seen)++ && l != h->length) return -2;
        h->length = l;
        if(v == end) return 0;
        v++;
    }
}

/* Transfer-Encoding: only chunked, once, without parameters; 0, or -1 */
static int add_codings(struct fx_head *h, const char *v, const char *end) {
    int any = 0;
    while(v < end) {
        const char *name;
        while(v < end && (ows(*v) || *v == ',')) v++;
        if(v == end) break;
        any = 1;
        for(name = v; v < end && tchar((unsigned char)*v); v++);
        if(!named(name, (int)(v - name), "chunked") || h->chunked) return -1;
        while(v < end && ows(*v)) v++;
        if(v < end && *v != ',') return -1;
        h->chunked = 1;
    }
    return any ? 0 : -1;
}

/* Content-Encoding: anything but identity is a coding fetch does not undo */
static void add_encodings(struct fx_head *h, const char *v, const char *end) {
    const char *p = v;
    while(p < end) {
        const char *name;
        while(p < end && (ows(*p) || *p == ',')) p++;
        if(p == end) break;
        for(name = p; p < end && *p != ','; p++);
        while(p > name && ows(p[-1])) p--;
        if(!named(name, (int)(p - name), "identity")) {
            h->encoded = 1;
            copy_safe(h->encoding, sizeof(h->encoding), v, (int)(end - v));
        }
        while(p < end && *p != ',') p++;
    }
}

/* Content-Range: "bytes a-b/c", "bytes a-b/" and a star (the total not known), or "bytes", a star and
   "/c" (a 416's); anything else is as if there were none */
static void content_range(struct fx_head *h, const char *v, const char *end) {
    const char *dash, *slash;
    long a = -1, b = -1, c = -1;
    if(end - v < 6 || strncmp(v, "bytes ", 6)) return;
    v += 6;
    if(!(slash = memchr(v, '/', (size_t)(end - v)))) return;
    if(!(end - slash == 2 && slash[1] == '*') && number(slash + 1, (int)(end - slash - 1), &c) != 0) return;
    if(!(slash - v == 1 && *v == '*')) {
        dash = memchr(v, '-', (size_t)(slash - v));
        if(!dash || number(v, (int)(dash - v), &a) != 0 || number(dash + 1, (int)(slash - dash - 1), &b) != 0 || b < a ||
           (c >= 0 && b >= c))
            return;
    } else if(c < 0) return;
    h->range_start = a;
    h->range_end = b;
    h->range_total = c;
}

int fx_parse_head(const char *buf, int len, struct fx_head *h, const char **err) {
    const char *p = buf, *end = buf + len, *eol, *colon, *v, *ve;
    int n, lengths = 0, codings = 0, i, r;
    memset(h, 0, sizeof(*h));
    h->length = h->range_start = h->range_end = h->range_total = -1;
    *err = "the response's status line is malformed";
    if(!(eol = memchr(p, '\n', (size_t)(end - p)))) return -1;
    n = (int)(eol - p);
    if(n && p[n - 1] == '\r') n--;
    if(n < 12 || strncmp(p, "HTTP/1.", 7) || p[7] < '0' || p[7] > '9' || p[8] != ' ' || p[9] < '1' || p[9] > '5' ||
       p[10] < '0' || p[10] > '9' || p[11] < '0' || p[11] > '9' || (n > 12 && p[12] != ' ') || memchr(p, 0, (size_t)n) ||
       memchr(p, '\r', (size_t)n))
        return -1;
    h->minor = p[7] - '0';
    h->status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
    if(n > 13) copy_safe(h->reason, sizeof(h->reason), p + 13, n - 13);
    for(p = eol + 1; p < end; p = eol + 1) {
        if(!(eol = memchr(p, '\n', (size_t)(end - p)))) break;
        n = (int)(eol - p);
        if(n && p[n - 1] == '\r') n--;
        if(!n) break;                                   /* the empty line: the end of the head */
        if(ows(*p)) {
            *err = "the response folds a header line (obsolete line folding)";
            return -1;
        }
        colon = memchr(p, ':', (size_t)n);
        *err = "the response has a malformed header line";
        if(!colon || colon == p || memchr(p, 0, (size_t)n) || memchr(p, '\r', (size_t)n)) return -1;
        for(i = 0; p + i < colon; i++) if(!tchar((unsigned char)p[i])) return -1;
        for(v = colon + 1; v < p + n && ows(*v); v++);
        for(ve = p + n; ve > v && ows(ve[-1]); ve--);
        n = (int)(colon - p);
        if(named(p, n, "content-length")) {
            if((r = add_length(h, &lengths, v, ve)) < 0) {
                *err = r == -1 ? "the response's Content-Length is malformed" :
                       r == -2 ? "the response has two different Content-Lengths" :
                                 "the response is 2 GB or more, more than a file here can hold";
                return -1;
            }
        } else if(named(p, n, "transfer-encoding")) {
            codings++;
            if(add_codings(h, v, ve) < 0) {
                *err = "the response uses a transfer coding other than chunked";
                return -1;
            }
        } else if(named(p, n, "content-encoding")) {
            add_encodings(h, v, ve);
        } else if(named(p, n, "content-range")) {
            content_range(h, v, ve);
        } else if(named(p, n, "location")) {
            if(ve - v >= FX_LOCATION) h->location_long = 1;
            else if(!h->locations || h->location_long) {
                if(!h->location_long) {
                    memcpy(h->location, v, (size_t)(ve - v));
                    h->location[ve - v] = 0;
                }
            } else if(strlen(h->location) != (size_t)(ve - v) || memcmp(h->location, v, (size_t)(ve - v))) {
                h->location_conflict = 1;
            }
            h->locations++;
        }
    }
    if(codings && lengths) {
        *err = "the response has both Content-Length and Transfer-Encoding";
        return -1;
    }
    if(codings && h->minor == 0) {
        *err = "the response is HTTP/1.0 and has Transfer-Encoding";
        return -1;
    }
    if(h->location_long) h->location[0] = 0;
    *err = NULL;
    return 0;
}

/**** the chunked coding ****/

enum { C_SIZE, C_EXT, C_SIZE_LF, C_DATA, C_DATA_CR, C_DATA_LF, C_TRAILER, C_DONE };

/* the size line has ended: the chunk's data, or the trailer after the last chunk */
static void size_done(struct fx_chunks *c) {
    c->state = c->left ? C_DATA : C_TRAILER;
    c->digits = c->line = 0;
}

int fx_chunks_feed(struct fx_chunks *c, const char *in, int n, char *out, int *got, int *done) {
    int i = 0, d;
    *got = 0;
    while(i < n && c->state != C_DONE) {
        unsigned char b = (unsigned char)in[i];
        if((c->state == C_SIZE || c->state == C_EXT || c->state == C_SIZE_LF) && ++c->line > FX_CHUNK_LINE) return -1;
        switch(c->state) {
        case C_SIZE:
            if((d = hex(b)) >= 0) {
                if(c->left > (unsigned long)FX_FILE_MAX >> 4) return -1;      /* 2 GB and more */
                c->left = (c->left << 4) | (unsigned long)d;
                c->digits++;
            } else if(!c->digits) return -1;
            else if(b == ';' || ows(b)) c->state = C_EXT;
            else if(b == '\r') c->state = C_SIZE_LF;
            else if(b == '\n') size_done(c);
            else return -1;
            i++;
            break;
        case C_EXT:                                     /* a chunk extension: nothing of it is used */
            if(b == '\r') c->state = C_SIZE_LF;
            else if(b == '\n') size_done(c);
            i++;
            break;
        case C_SIZE_LF:
            if(b != '\n') return -1;
            size_done(c);
            i++;
            break;
        case C_DATA: {
            int k = n - i;
            if((unsigned long)k > c->left) k = (int)c->left;
            memcpy(out + *got, in + i, (size_t)k);
            *got += k;
            i += k;
            if(!(c->left -= (unsigned long)k)) c->state = C_DATA_CR;
            break;
        }
        case C_DATA_CR:
            if(b == '\r') c->state = C_DATA_LF;
            else if(b == '\n') c->state = C_SIZE;
            else return -1;
            i++;
            break;
        case C_DATA_LF:
            if(b != '\n') return -1;
            c->state = C_SIZE;
            i++;
            break;
        case C_TRAILER:                                 /* trailer lines until an empty one */
            if(++c->trailer > FX_HEAD_MAX) return -1;
            if(b == '\n') {
                if(!c->line) c->state = C_DONE;
                c->line = 0;
            } else if(b != '\r') c->line++;
            i++;
            break;
        }
    }
    *done = c->state == C_DONE;
    return i;
}

/**** names and sizes ****/

static int unsafe(int c) {
    return c < 0x20 || c == 0x7f || (c && strchr("\\/:*?\"<>|", c) != NULL);
}

void fx_name_of(const char *path, char *out, int size) {
    char name[FX_NAME_MAX];
    const char *start = path, *end = path + strcspn(path, "?#"), *q;
    int n = 0, h1, h2;
    for(q = path; q < end; q++) if(*q == '/') start = q + 1;
    for(q = start; q < end && n < FX_NAME_MAX - 1; q++) {
        int c = (unsigned char)*q;
        if(c == '%' && end - q > 2 && (h1 = hex(q[1])) >= 0 && (h2 = hex(q[2])) >= 0) {
            c = h1 * 16 + h2;
            q += 2;
        }
        name[n++] = unsafe(c) ? '_' : (char)c;
    }
    name[n] = 0;
    if(!n || !strcmp(name, ".") || !strcmp(name, "..")) snprintf(out, (size_t)size, "index.html");
    else snprintf(out, (size_t)size, "%s", name);
}

void fx_size(char *out, int size, unsigned long b) {
    if(b < 1024UL) snprintf(out, (size_t)size, "%lu bytes", b);
    else if(b < 1024UL * 1024) snprintf(out, (size_t)size, "%lu.%lu KB", b >> 10, ((b & 1023) * 10) >> 10);
    else if(b < 1024UL * 1024 * 1024) snprintf(out, (size_t)size, "%lu.%lu MB", b >> 20, (((b >> 10) & 1023) * 10) >> 10);
    else snprintf(out, (size_t)size, "%lu.%lu GB", b >> 30, (((b >> 20) & 1023) * 10) >> 10);
}
