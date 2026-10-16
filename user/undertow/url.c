/*
SurfOS Undertow: HTTP(S) parsing and RFC 3986 reference resolution
----------------------
File: url.c  Date: 10/10/26
----------------------
*/
#include <stdlib.h>
#include <string.h>
#include "url.h"

/* A relative path may briefly contain both the base and reference before
 * removing dot segments. Never put these buffers on the user's small stack. */
struct work {
    struct ut_url url;
    char path[UT_URL_MAX * 2 + 2];
};

static int error(char *err, size_t cap, const char *message) {
    size_t n = strlen(message);
    if(err && cap) {
        if(n >= cap) n = cap - 1;
        memcpy(err, message, n);
        err[n] = 0;
    }
    return -1;
}

static int alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int hex(unsigned char c) {
    return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

static int input_size(const char *s, size_t *len, char *err, size_t cap) {
    size_t i;
    if(!s) return error(err, cap, "missing URL");
    for(i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if(i == UT_URL_MAX) return error(err, cap, "URL exceeds 2048 bytes");
        if(c < 32 || c == 127) return error(err, cap, "URL contains a control byte");
        if(c == ' ') return error(err, cap, "URL contains a space");
        if(c == '\\') return error(err, cap, "URL contains a backslash");
    }
    *len = i;
    for(i = 0; i < *len; i++) {
        if(s[i] == '%') {
            if(i + 2 >= *len || !hex(s[i+1]) || !hex(s[i+2]))
                return error(err, cap, "invalid URL percent escape");
            i += 2;
        }
    }
    return 0;
}

/* A colon in the first segment denotes a scheme in a relative reference. */
static const char *first_colon(const char *s) {
    while(*s && *s != '/' && *s != '?' && *s != '#') {
        if(*s == ':') return s;
        s++;
    }
    return NULL;
}
static int scheme_is(const char *s, size_t n, const char *want) {
    size_t i;
    if(strlen(want) != n) return 0;
    for(i = 0; i < n; i++) if(lower(s[i]) != want[i]) return 0;
    return 1;
}

static int authority(const char *s, size_t n, struct ut_url *u, char *err, size_t cap) {
    size_t i, hostlen = n, label = 0;
    const char *port = NULL;
    unsigned int value = 0;
    for(i = 0; i < n; i++) {
        if(s[i] == '@') return error(err, cap, "URL credentials are unsupported");
        if(s[i] == '[' || s[i] == ']') return error(err, cap, "IPv6 is unsupported");
        if(s[i] == ':') {
            if(port) return error(err, cap, "IPv6 is unsupported");
            port = s + i + 1;
            hostlen = i;
        }
    }
    if(!hostlen) return error(err, cap, "URL host is empty");
    if(hostlen >= sizeof(u->host)) return error(err, cap, "URL host exceeds 253 bytes");
    for(i = 0; i < hostlen; i++) {
        unsigned char c = (unsigned char)s[i];
        if(c >= 128) return error(err, cap, "URL host must be ASCII (use punycode)");
        if(c == '.') {
            if(!label || s[i-1] == '-') return error(err, cap, "invalid URL host label");
            label = 0;
        } else {
            if((!alpha(c) && !digit(c) && c != '-') || (!label && c == '-'))
                return error(err, cap, "invalid URL host character");
            if(++label > 63) return error(err, cap, "URL host label exceeds 63 bytes");
        }
        u->host[i] = lower(c);
    }
    if(s[hostlen-1] == '-') return error(err, cap, "invalid URL host label");
    u->host[hostlen] = 0;
    u->port = u->secure ? 443 : 80;
    if(port) {
        if(port == s + n) return error(err, cap, "URL port is empty");
        while(port < s + n) {
            if(!digit(*port)) return error(err, cap, "URL port must be decimal");
            value = value * 10 + (*port++ - '0');
            if(value > 65535) return error(err, cap, "URL port exceeds 65535");
        }
        if(!value) return error(err, cap, "URL port must be at least 1");
        u->port = (unsigned short)value;
    }
    return 0;
}

static void pop_segment(char *s, size_t *len) {
    while(*len && s[*len-1] != '/') --*len;
    if(*len) --*len;
}

/* RFC 3986 5.2.4, in place: output never advances beyond unread input.
 * Match literal dots only. Queries and fragments never enter this function. */
static size_t normalize(char *s, size_t n) {
    size_t i = 0, out = 0, end;
    while(i < n) {
        if(n-i >= 3 && !memcmp(s+i, "../", 3)) i += 3;
        else if(n-i >= 2 && !memcmp(s+i, "./", 2)) i += 2;
        else if(n-i >= 3 && !memcmp(s+i, "/./", 3)) i += 2;
        else if(n-i == 2 && !memcmp(s+i, "/.", 2)) { i += 2; s[out++] = '/'; }
        else if(n-i >= 4 && !memcmp(s+i, "/../", 4)) { i += 3; pop_segment(s, &out); }
        else if(n-i == 3 && !memcmp(s+i, "/..", 3)) {
            i += 3; pop_segment(s, &out); s[out++] = '/';
        } else if((n-i == 1 && s[i] == '.') || (n-i == 2 && !memcmp(s+i, "..", 2))) i = n;
        else {
            end = i + (s[i] == '/');
            while(end < n && s[end] != '/') end++;
            memmove(s+out, s+i, end-i);
            out += end-i;
            i = end;
        }
    }
    if(!out) s[out++] = '/';
    s[out] = 0;
    return out;
}

static int append(char *dst, size_t *n, const char *s, size_t len, char *err, size_t cap) {
    if(len > UT_URL_MAX - *n) return error(err, cap, "URL exceeds 2048 bytes");
    memcpy(dst + *n, s, len);
    *n += len;
    dst[*n] = 0;
    return 0;
}

static int finish(struct work *w, const char *query, size_t qlen, const char *fragment,
                  char *err, size_t cap) {
    struct ut_url *u = &w->url;
    size_t n = 0, len = normalize(w->path, strlen(w->path));
    char digits[6];
    size_t d = sizeof(digits);
    unsigned int port = u->port;
    if(append(u->target, &n, w->path, len, err, cap) ||
       append(u->target, &n, query, qlen, err, cap)) return -1;
    u->fragment[0] = 0;
    if(fragment) memcpy(u->fragment, fragment, strlen(fragment) + 1);
    n = 0;
    if(append(u->text, &n, u->secure ? "https://" : "http://", u->secure ? 8 : 7, err, cap) ||
       append(u->text, &n, u->host, strlen(u->host), err, cap)) return -1;
    if(port != (u->secure ? 443u : 80u)) {
        do { digits[--d] = '0' + port % 10; port /= 10; } while(port);
        if(append(u->text, &n, ":", 1, err, cap) ||
           append(u->text, &n, digits+d, sizeof(digits)-d, err, cap)) return -1;
    }
    if(append(u->text, &n, u->target, strlen(u->target), err, cap)) return -1;
    if(fragment && (append(u->text, &n, "#", 1, err, cap) ||
                    append(u->text, &n, fragment, strlen(fragment), err, cap))) return -1;
    return 0;
}

/* input has already been bounded and checked for unsafe bytes. */
static int parse(const char *input, struct work *w, char *err, size_t cap) {
    const char *s = input, *end, *colon = first_colon(input), *query, *fragment;
    size_t pathlen;
    int explicit_scheme = 0;
    w->url.secure = 1;
    if(!*s) return error(err, cap, "URL is empty");
    if(colon) {
        size_t n = colon-input;
        if(scheme_is(input, n, "https") || scheme_is(input, n, "http")) {
            if(colon[1] != '/' || colon[2] != '/') return error(err, cap, "URL scheme requires //");
            w->url.secure = n == 5;
            s = colon+3;
            explicit_scheme = 1;
        } else {
            /* Give bare unsupported authorities the same specific errors. */
            for(end = input; *end && *end != '/' && *end != '?' && *end != '#'; end++) {
                if(*end == '@') return error(err, cap, "URL credentials are unsupported");
                if(*end == '[' || *end == ']' || (*end == ':' && end != colon))
                    return error(err, cap, "IPv6 is unsupported");
            }
            /* A decimal suffix is a bare host:port, not a scheme. */
            end = colon+1;
            while(digit(*end)) end++;
            if(end == colon+1 || (*end && *end != '/' && *end != '?' && *end != '#'))
                return error(err, cap, "unsupported URL scheme");
        }
    }
    if(!explicit_scheme && s[0] == '/' && s[1] == '/') s += 2;
    end = s;
    while(*end && *end != '/' && *end != '?' && *end != '#') end++;
    if(authority(s, end-s, &w->url, err, cap)) return -1;
    fragment = strchr(end, '#');
    query = strchr(end, '?');
    if(query && fragment && query > fragment) query = NULL;
    pathlen = (query ? query : (fragment ? fragment : end+strlen(end))) - end;
    memcpy(w->path, end, pathlen);
    w->path[pathlen] = 0;
    return finish(w, query ? query : "", query ? (size_t)((fragment ? fragment : query+strlen(query))-query) : 0,
                  fragment ? fragment+1 : NULL, err, cap);
}

int ut_url_parse(const char *input, struct ut_url *out, char *err, size_t cap) {
    struct work *w;
    size_t len;
    int rc;
    if(!out) return error(err, cap, "missing URL output");
    if(input_size(input, &len, err, cap)) return -1;
    w = malloc(sizeof(*w));
    if(!w) return error(err, cap, "out of memory for URL");
    memset(w, 0, sizeof(*w));
    rc = parse(input, w, err, cap);
    if(!rc) { memcpy(out, &w->url, sizeof(*out)); if(err && cap) err[0] = 0; }
    free(w);
    return rc;
}

int ut_url_resolve(const struct ut_url *base, const char *ref,
                   struct ut_url *out, char *err, size_t cap) {
    struct work *w;
    const char *query, *fragment, *end, *basequery, *p;
    size_t len, pathlen, prefix = 0, qlen = 0;
    int rc;
    if(!base || !out) return error(err, cap, "missing base URL or output");
    if(input_size(ref, &len, err, cap)) return -1;
    w = malloc(sizeof(*w));
    if(!w) return error(err, cap, "out of memory for URL");
    memset(w, 0, sizeof(*w));
    if(first_colon(ref)) {
        const char *colon = first_colon(ref);
        if(!scheme_is(ref, colon-ref, "http") && !scheme_is(ref, colon-ref, "https"))
            rc = error(err, cap, "unsupported URL scheme");
        else rc = parse(ref, w, err, cap);
    } else if(ref[0] == '/' && ref[1] == '/') {
        /* Network paths inherit the scheme, including the default port. */
        w->url.secure = base->secure;
        end = ref+2;
        while(*end && *end != '/' && *end != '?' && *end != '#') end++;
        rc = authority(ref+2, end-(ref+2), &w->url, err, cap);
        if(!rc) {
            fragment = strchr(end, '#');
            query = strchr(end, '?');
            if(query && fragment && query > fragment) query = NULL;
            pathlen = (query ? query : (fragment ? fragment : ref+len))-end;
            memcpy(w->path, end, pathlen); w->path[pathlen] = 0;
            rc = finish(w, query ? query : "", query ? (size_t)((fragment ? fragment : ref+len)-query) : 0,
                        fragment ? fragment+1 : NULL, err, cap);
        }
    } else {
        memcpy(&w->url, base, sizeof(*base));
        fragment = strchr(ref, '#');
        query = strchr(ref, '?');
        if(query && fragment && query > fragment) query = NULL;
        pathlen = (query ? query : (fragment ? fragment : ref+len))-ref;
        basequery = strchr(base->target, '?');
        if(!pathlen) {
            prefix = basequery ? (size_t)(basequery-base->target) : strlen(base->target);
            if(!query) query = basequery;
        } else if(ref[0] != '/') {
            p = base->target;
            end = basequery ? basequery : p+strlen(p);
            while(p < end) { if(*p == '/') prefix = p-base->target+1; p++; }
        }
        memcpy(w->path, base->target, prefix);
        memcpy(w->path+prefix, ref, pathlen);
        w->path[prefix+pathlen] = 0;
        if(query) qlen = query == basequery ? strlen(query) : (size_t)((fragment ? fragment : ref+len)-query);
        rc = finish(w, query ? query : "", qlen, fragment ? fragment+1 : NULL, err, cap);
    }
    if(!rc) { memcpy(out, &w->url, sizeof(*out)); if(err && cap) err[0] = 0; }
    free(w);
    return rc;
}
