/*
SurfOS web server: httpd
--------------------
File: http.c    Date: 10/17/26
--------------------
The HTTP/1.1 protocol (http.h, RFC 9110 and 9112), as much of it as a server of static
files needs. No system calls, so the host tests (tests/host/test_httpd.c) run this file
as it is.

A request head is its request line and header fields, each line ended by CR LF or a lone
LF, then an empty line; empty lines before the request line are skipped (RFC 9112 2.2).
The request line is a method, the target and HTTP/1.x, one space between each. The
target is the origin form (/path?query) or the absolute form (http://host/path?query);
the query is ignored. The path is percent-decoded, then cut into its names: "." and
empty names are dropped, ".." is refused (400: browsers resolve it before they send, so
it comes only from a request that tries to leave the served folder), as are a NUL, other
controls and a backslash; a name starting with a dot is hidden (404).

Header fields: Host (HTTP/1.1 needs exactly one), Connection (close, keep-alive),
Content-Length and Transfer-Encoding (a body, which the server never reads: the
connection closes after the response) and If-None-Match. A field name with a blank
before its colon, a line folded onto the one before (obs-fold), a value with a control
in it and a Content-Length that is not a number are refused (400), as RFC 9112 allows.
HTTP/1.1 connections stay open after a response unless the client says close; HTTP/1.0
ones only when it says keep-alive. A major version other than 1 is 505.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include "http.h"

/* a character of a token (RFC 9110 5.6.2: method and field names) */
static int tchar(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("!#$%&'*+-.^_`|~", c));
}

int http_head_end(const char *buf, int len) {
    int i = 0, j;
    while(i < len && (buf[i] == '\r' || buf[i] == '\n')) i++;      /* empty lines before the request line */
    for(; i < len; i++) {
        if(buf[i] != '\n') continue;
        j = i + 1;
        if(j < len && buf[j] == '\r') j++;
        if(j < len && buf[j] == '\n') return j + 1;
    }
    return 0;
}

/* the line at *p (before end): its start and length without the CR LF or LF; *p moves past it */
static const char *line(const char **p, const char *end, int *n) {
    const char *s = *p, *e = s;
    while(e < end && *e != '\n') e++;
    *p = e < end ? e + 1 : end;
    if(e > s && e[-1] == '\r') e--;
    *n = (int)(e - s);
    return s;
}

/* does the token list s (n bytes, comma-separated) hold word? */
static int has_token(const char *s, int n, const char *word) {
    int i = 0, k, w = (int)strlen(word);
    while(i < n) {
        int e;
        while(i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) i++;
        k = i;
        while(k < n && s[k] != ',') k++;
        e = k;
        while(e > i && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
        if(e - i == w && !strncasecmp(s + i, word, (size_t)w)) return 1;
        i = k;
    }
    return 0;
}

static int hexval(int c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* the path of the target (n bytes, the query and fragment already cut off) decoded and normalised into
   req: 0, or 400, 404, 414 */
static int path_of(const char *s, int n, struct http_req *req) {
    char *out = req->path;
    int i, o = 1, seg = 1, hidden = 0, end;     /* out[seg..o): the name being decoded */
    out[0] = '/';
    for(i = 0; i <= n; i++) {
        int c = i < n ? (unsigned char)s[i] : '/';
        if(c == '%' && i < n) {
            int h = i + 2 < n ? hexval(s[i + 1]) : -1, l = i + 2 < n ? hexval(s[i + 2]) : -1;
            if(h < 0 || l < 0) return 400;
            c = h * 16 + l;
            i += 2;
            if(!c) return 400;                          /* %00 */
        }
        if(c < 0x20 || c == 0x7f || c == '\\') return 400;
        if(c != '/') {
            if(o >= HTTP_PATH_MAX - 1) return 414;
            out[o++] = (char)c;
            continue;
        }
        /* the end of a name: out[seg..o) */
        end = o;
        if(end == seg || (end - seg == 1 && out[seg] == '.')) o = seg;      /* empty, "." */
        else if(end - seg == 2 && out[seg] == '.' && out[seg + 1] == '.') return 400;
        else {
            if(out[seg] == '.') hidden = 1;
            if(o >= HTTP_PATH_MAX) return 414;          /* the slash after the last name goes again below */
            out[o++] = '/';
        }
        seg = o;
    }
    req->slash = n > 0 && (s[n - 1] == '/' || (n >= 3 && s[n - 3] == '%' && s[n - 2] == '2' &&
                                               (s[n - 1] == 'f' || s[n - 1] == 'F')));
    if(o > 1) o--;                                      /* the '/' after the last name */
    out[o] = 0;
    return hidden ? 404 : 0;
}

/* n bytes of s into out (size bytes) for the log, cut short, controls and non-ASCII as '?' */
static void keep_for_log(char *out, int size, const char *s, int n) {
    int i;
    if(n > size - 1) n = size - 1;
    for(i = 0; i < n; i++) out[i] = s[i] > 0x20 && s[i] < 0x7f ? s[i] : '?';
    out[n] = 0;
}

int http_parse(const char *buf, int len, struct http_req *req) {
    const char *p = buf, *end = buf + len, *l, *sp1, *sp2, *target;
    int n, i, tn, hosts = 0, close = 0, keepalive = 0, lengths = 0, tags = 0, lost_tag = 0;
    long clen = -1;

    memset(req, 0, sizeof(*req));
    req->path[0] = '/';
    while(p < end && (*p == '\r' || *p == '\n')) p++;
    l = line(&p, end, &n);
    /* the request line: method SP target SP HTTP/x.y */
    sp1 = memchr(l, ' ', (size_t)n);
    sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(l + n - sp1 - 1)) : NULL;
    if(!sp1 || !sp2 || sp1 == l || sp2 == sp1 + 1) return 400;
    keep_for_log(req->method_name, sizeof(req->method_name), l, (int)(sp1 - l));
    target = sp1 + 1;
    tn = (int)(sp2 - target);
    keep_for_log(req->target, sizeof(req->target), target, tn);
    for(i = 0; l + i < sp1; i++) if(!tchar((unsigned char)l[i])) return 400;
    if(sp1 - l == 3 && !strncmp(l, "GET", 3)) req->method = HTTP_GET;      /* before the version: a HEAD's error */
    else if(sp1 - l == 4 && !strncmp(l, "HEAD", 4)) req->method = HTTP_HEAD;  /* has no body either */
    else req->method = HTTP_OTHER;
    if(l + n - sp2 - 1 != 8 || strncmp(sp2 + 1, "HTTP/", 5) || !isdigit((unsigned char)sp2[6]) || sp2[7] != '.' ||
       !isdigit((unsigned char)sp2[8])) return 400;
    if(sp2[6] != '1') return 505;
    req->minor = sp2[8] - '0';
    req->keep = req->minor >= 1;

    /* the header fields */
    while(p < end) {
        const char *colon, *v, *ve;
        l = line(&p, end, &n);
        if(!n) break;                                   /* the empty line: the end of the head */
        if(l[0] == ' ' || l[0] == '\t') return 400;     /* obs-fold */
        colon = memchr(l, ':', (size_t)n);
        if(!colon || colon == l) return 400;
        for(v = l; v < colon; v++) if(!tchar((unsigned char)*v)) return 400;   /* a blank before the colon too */
        v = colon + 1;
        ve = l + n;
        while(v < ve && (*v == ' ' || *v == '\t')) v++;
        while(ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        for(i = 0; v + i < ve; i++) {
            unsigned char c = (unsigned char)v[i];
            if((c < 0x20 && c != '\t') || c == 0x7f) return 400;
        }
#define FIELD(name) (colon - l == (int)sizeof(name) - 1 && !strncasecmp(l, name, sizeof(name) - 1))
        if(FIELD("Host")) hosts++;
        else if(FIELD("Connection")) {
            if(has_token(v, (int)(ve - v), "close")) close = 1;
            if(has_token(v, (int)(ve - v), "keep-alive")) keepalive = 1;
        } else if(FIELD("Content-Length")) {
            long cl = 0;
            if(v == ve) return 400;
            for(i = 0; v + i < ve; i++) {
                if(!isdigit((unsigned char)v[i]) || cl > 100000000L) return 400;
                cl = cl * 10 + (v[i] - '0');
            }
            if(lengths++ && cl != clen) return 400;    /* two that differ */
            clen = cl;
            if(cl > 0) req->body = 1;
        } else if(FIELD("Transfer-Encoding")) req->body = 1;
        else if(FIELD("If-None-Match")) {
            if(tags++ || ve - v >= HTTP_TAG_MAX) lost_tag = 1;     /* a second field or a long list: none counts */
            else {
                memcpy(req->tag, v, (size_t)(ve - v));
                req->tag[ve - v] = 0;
            }
        }
#undef FIELD
    }
    if(lost_tag) req->tag[0] = 0;                       /* the whole file is sent: always a right answer */
    req->keep = !close && (req->minor >= 1 || keepalive) && !req->body;     /* close wins (RFC 9112 9.3) */
    if(req->minor >= 1 && hosts != 1) return 400;
    if(hosts > 1) return 400;
    if(req->method == HTTP_OTHER) return 405;

    /* the target: origin form, or absolute form with the scheme and authority dropped */
    if(tn >= 7 && !strncasecmp(target, "http://", 7)) {
        const char *s = memchr(target + 7, '/', (size_t)(tn - 7));
        if(!s) {
            target = "/";
            tn = 1;
        } else {
            tn -= (int)(s - target);
            target = s;
        }
    }
    if(target[0] != '/') return 400;
    i = 0;
    while(i < tn && target[i] != '?' && target[i] != '#') i++;
    return path_of(target, i, req);
}

const char *http_reason(int status) {
    switch(status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

int http_header(char *out, int size, const struct http_resp *r) {
    int n = snprintf(out, (size_t)size, "HTTP/1.1 %d %s\r\nServer: " HTTP_SERVER "\r\n", r->status, http_reason(r->status));
#define ADD(...) do { if(n >= 0 && n < size) n += snprintf(out + n, (size_t)(size - n), __VA_ARGS__); } while(0)
    if(r->type) ADD("Content-Type: %s\r\n", r->type);
    if(r->length >= 0) ADD("Content-Length: %ld\r\n", r->length);
    if(r->etag) ADD("ETag: %s\r\n", r->etag);
    if(r->location) ADD("Location: %s\r\n", r->location);
    if(r->status == 405) ADD("Allow: GET, HEAD\r\n");
    ADD("Cache-Control: no-cache\r\nConnection: %s\r\n\r\n", r->keep ? "keep-alive" : "close");
#undef ADD
    return n >= 0 && n < size ? n : -1;
}

static const struct { const char *ext, *type; } types[] = {
    { "html", "text/html; charset=utf-8" },     { "htm", "text/html; charset=utf-8" },
    { "css", "text/css; charset=utf-8" },       { "js", "text/javascript; charset=utf-8" },
    { "mjs", "text/javascript; charset=utf-8" }, { "json", "application/json" },
    { "txt", "text/plain; charset=utf-8" },     { "md", "text/plain; charset=utf-8" },
    { "csv", "text/csv; charset=utf-8" },       { "xml", "application/xml" },
    { "svg", "image/svg+xml" },                 { "png", "image/png" },
    { "jpg", "image/jpeg" },                    { "jpeg", "image/jpeg" },
    { "gif", "image/gif" },                     { "webp", "image/webp" },
    { "ico", "image/x-icon" },                  { "bmp", "image/bmp" },
    { "woff", "font/woff" },                    { "woff2", "font/woff2" },
    { "ttf", "font/ttf" },                      { "otf", "font/otf" },
    { "pdf", "application/pdf" },               { "wasm", "application/wasm" },
    { "webmanifest", "application/manifest+json" },
};

const char *http_type(const char *name) {
    const char *dot = strrchr(name, '.'), *slash = strrchr(name, '/');
    unsigned int i;
    if(dot && (!slash || dot > slash) && dot[1])
        for(i = 0; i < sizeof(types) / sizeof(types[0]); i++)
            if(!strcasecmp(dot + 1, types[i].ext)) return types[i].type;
    return "application/octet-stream";
}

void http_etag(char *out, int size, unsigned int filesize, unsigned int mtime) {
    snprintf(out, (size_t)size, "\"%x-%x\"", filesize, mtime);
}

int http_etag_match(const char *header, const char *etag) {
    const char *s = header;
    while(*s == ' ' || *s == '\t') s++;
    if(*s == '*' && !s[1]) return 1;
    return *etag && strstr(header, etag) != NULL;      /* our tags hold no quote: a match is the whole tag */
}

/* is c kept as it is in an encoded path? */
static int plain(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~' || c == '/';
}

int http_encode_path(char *out, int size, const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    const unsigned char *s = (const unsigned char *)path;
    for(; *s; s++) {
        if(plain(*s)) {
            if(o + 1 >= size) return -1;
            out[o++] = (char)*s;
        } else {
            if(o + 3 >= size) return -1;
            out[o++] = '%';
            out[o++] = hex[*s >> 4];
            out[o++] = hex[*s & 15];
        }
    }
    if(o >= size) return -1;
    out[o] = 0;
    return 0;
}

#define PAGE_STYLE "body{font:16px/1.5 system-ui,-apple-system,sans-serif;margin:2.5em auto;max-width:46em;" \
                   "padding:0 1em;background:#f4f1e6;color:#153d49}h1{color:#102e3b;font-size:1.5em}" \
                   "a{color:#197d83}footer{margin-top:2em;color:#6c888c;font-size:.85em}"

int http_error_page(char *out, int size, int status) {
    const char *why;
    int n;
    switch(status) {
    case 400: why = "The server could not read this request."; break;
    case 404: why = "There is nothing here with that name."; break;
    case 405: why = "This server only sends pages: it answers GET and HEAD."; break;
    case 408: why = "The request did not arrive in time."; break;
    case 414: why = "The address is too long for this server."; break;
    case 431: why = "The request's header is too large for this server."; break;
    case 505: why = "This server speaks HTTP/1.0 and HTTP/1.1."; break;
    default:  why = "Something went wrong on the server while reading the file."; break;
    }
    /* more than 512 bytes: Chrome puts its own page in the place of a shorter error page */
    n = snprintf(out, (size_t)size, "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                 "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                 "<title>%d %s</title><style>" PAGE_STYLE "</style></head>\n<body><h1>%d %s</h1><p>%s</p>"
                 "<p><a href=\"/\">The front page</a></p><footer>SurfOS httpd</footer></body></html>\n", status,
                 http_reason(status), status, http_reason(status), why);
    return n < 0 ? 0 : n < size ? n : size - 1;
}

static int entry_cmp(const void *a, const void *b) {
    const struct http_entry *x = a, *y = b;
    int r;
    if(x->dir != y->dir) return y->dir - x->dir;
    r = strcasecmp(x->name, y->name);
    return r ? r : strcmp(x->name, y->name);
}

void http_sort(struct http_entry *e, int n) {
    if(n > 1) qsort(e, (size_t)n, sizeof(*e), entry_cmp);
}

/* a page being made: grows with realloc(); bad once memory ran out */
struct page {
    char *p;
    int len, cap, bad;
};

static void put(struct page *b, const char *s, int n) {
    if(b->bad) return;
    if(b->len + n + 1 > b->cap) {
        int cap = b->cap ? b->cap : 2048;
        char *q;
        while(cap < b->len + n + 1) cap *= 2;
        if(!(q = realloc(b->p, (size_t)cap))) {
            b->bad = 1;
            return;
        }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
    b->p[b->len] = 0;
}

static void puts_(struct page *b, const char *s) {
    put(b, s, (int)strlen(s));
}

static void putf(struct page *b, const char *fmt, ...) {
    char tmp[128];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if(n > 0) put(b, tmp, n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1);
}

/* text with & < > " escaped for HTML */
static void put_html(struct page *b, const char *s) {
    for(; *s; s++) {
        switch(*s) {
        case '&': puts_(b, "&amp;"); break;
        case '<': puts_(b, "&lt;"); break;
        case '>': puts_(b, "&gt;"); break;
        case '"': puts_(b, "&quot;"); break;
        default:  put(b, s, 1);
        }
    }
}

static void put_url(struct page *b, const char *s) {
    char tmp[3 * HTTP_PATH_MAX + 1];
    if(http_encode_path(tmp, sizeof(tmp), s) == 0) puts_(b, tmp);
    else b->bad = 1;
}

static void put_size(struct page *b, unsigned int size) {
    if(size < 1024) putf(b, "%u B", size);
    else if(size < 1024 * 1024) putf(b, "%u.%u KB", size / 1024, size % 1024 * 10 / 1024);
    else putf(b, "%u.%u MB", size / (1024 * 1024), size / 1024 % 1024 * 10 / 1024);
}

int http_listing(const char *path, const struct http_entry *e, int n, int more, char **out) {
    struct page b = { NULL, 0, 0, 0 };
    int i, root = !strcmp(path, "/");
    puts_(&b, "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>Index of ");
    put_html(&b, path);
    puts_(&b, root ? "" : "/");
    puts_(&b, "</title><style>" PAGE_STYLE "table{border-collapse:collapse;width:100%}"
              "td{padding:.2em 1.2em .2em 0;white-space:nowrap}td.n{width:100%;white-space:normal}"
              ".s,.d{color:#6c888c;text-align:right}</style></head>\n<body><h1>\xe2\x89\x8b Index of ");
    put_html(&b, path);
    puts_(&b, root ? "" : "/");
    puts_(&b, "</h1>\n<table>\n");
    if(!root) puts_(&b, "<tr><td class=\"n\"><a href=\"../\">../</a></td><td></td><td></td></tr>\n");
    for(i = 0; i < n; i++) {
        puts_(&b, "<tr><td class=\"n\"><a href=\"");
        put_url(&b, e[i].name);
        puts_(&b, e[i].dir ? "/\">" : "\">");
        put_html(&b, e[i].name);
        puts_(&b, e[i].dir ? "/</a></td><td class=\"s\">-</td>" : "</a></td><td class=\"s\">");
        if(!e[i].dir) {
            put_size(&b, e[i].size);
            puts_(&b, "</td>");
        }
        puts_(&b, "<td class=\"d\">");
        if(e[i].mtime) {
            time_t t = (time_t)e[i].mtime;
            struct tm tm;
            if(gmtime_r(&t, &tm))
                putf(&b, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        }
        puts_(&b, "</td></tr>\n");
    }
    puts_(&b, "</table>\n");
    if(!n && !more) puts_(&b, "<p>This folder is empty.</p>\n");
    if(more) putf(&b, "<p>%d more entries are not listed.</p>\n", more);
    puts_(&b, "<footer>SurfOS httpd</footer></body></html>\n");
    if(b.bad) {
        free(b.p);
        return -1;
    }
    *out = b.p;
    return b.len;
}

int http_normalize(const char *path, char *out, int size) {
    int o = 0, i = 0, n;
    if(!path || path[0] != '/' || size < 2) return -1;
    while(path[i]) {
        while(path[i] == '/') i++;
        for(n = 0; path[i + n] && path[i + n] != '/'; n++);
        if(!n) break;
        if(n == 1 && path[i] == '.') ;                              /* "." */
        else if(n == 2 && path[i] == '.' && path[i + 1] == '.') {    /* "..": the last name and its slash go */
            while(o > 0 && out[o - 1] != '/') o--;
            if(o > 0) o--;
        } else {
            if(o + 1 + n >= size) return -1;
            out[o++] = '/';
            memcpy(out + o, path + i, (size_t)n);
            o += n;
        }
        i += n;
    }
    if(!o) out[o++] = '/';
    out[o] = 0;
    return 0;
}
