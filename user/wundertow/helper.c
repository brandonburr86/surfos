/*
SurfOS Undertow's window: the helper
----------------------
File: helper.c  Date: 11/1/26
----------------------
wundertow --helper KIND, the helper's side of loader.h: one load, through Undertow's HTTP and TLS or from a file, one
answer on descriptor 1, and the end. It is alone with what a server sends: if that kills it, the window hears only
that its helper stopped. A file is read as a page is, only when the address is a file:///: the window starts a
helper for one only when the page's rules allow it (browser.c).

--layout and --render, which have no window to keep answering, load their one page by the same call.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "undertow.h"
#include "url.h"
#include "config.h"
#include "http.h"
#include "image.h"
#include "browser.h"
#include "loader.h"

static int ends_with(const char *s, const char *end) {
    size_t n = strlen(s), k = strlen(end);
    size_t i;
    if(n < k) return 0;
    for(i = 0; i < k; i++) if((s[n - k + i] | 0x20) != end[i]) return 0;
    return 1;
}

/* A file's response: read whole (at most max bytes), at https://WB_FILE_HOST/path, HTML by its name */
static int load_file(const char *path, size_t max, struct ut_response *r, char *err, size_t cap) {
    char url[UT_URL_MAX + 64];
    size_t n = 0, i, o;
    ssize_t k;
    int fd;
    memset(r, 0, sizeof(*r));
    o = (size_t)snprintf(url, sizeof(url), "https://" WB_FILE_HOST);
    for(i = 0; path[i] && o + 4 < sizeof(url); i++) {      /* the path as an address: escaped */
        unsigned char c = (unsigned char)path[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("/-._~", c)) url[o++] = (char)c;
        else o += (size_t)snprintf(url + o, sizeof(url) - o, "%%%02X", c);
    }
    url[o] = 0;
    if(ut_url_parse(url, &r->url, err, cap)) return -1;
    fd = open(path, O_RDONLY);
    if(fd < 0) return ut_error(err, cap, "%s: %s", path, strerror(errno));
    r->body = malloc(max + 1);
    if(!r->body) {
        close(fd);
        return ut_error(err, cap, "out of memory reading %s", path);
    }
    while(n < max && (k = read(fd, r->body + n, max - n)) > 0) n += (size_t)k;
    if(n == max && read(fd, &k, 1) > 0) {
        close(fd);
        free(r->body);
        r->body = NULL;
        return ut_error(err, cap, "%s is over %u MB: Undertow shows pages of 1 MB at most", path, (unsigned int)(max >> 20));
    }
    close(fd);
    r->body[n] = 0;
    r->len = n;
    r->status = 200;
    snprintf(r->content_type, sizeof(r->content_type), "%s",
             ends_with(path, ".html") || ends_with(path, ".htm") ? "text/html" :
             ends_with(path, ".xhtml") ? "application/xhtml+xml" : "text/plain");
    return 0;
}

/* url's response: a file, or from the network; any = what is not text is read too (at most UT_ANY_MAX bytes) */
static int fetch(const char *url, const struct ut_config *cfg, int any, struct ut_response *r, int *file, char *err, size_t cap) {
    char path[UT_URL_MAX + 1];
    struct ut_url *u;
    int rc;
    *file = wb_file_path(url, path, sizeof(path));
    if(*file) return load_file(path, any ? UT_ANY_MAX : UT_BODY_MAX, r, err, cap);
    u = malloc(sizeof(*u));
    if(!u) return ut_error(err, cap, "out of memory");
    rc = ut_url_parse(url, u, err, cap);
    if(!rc) rc = any ? ut_fetch_any(u, cfg, UT_ANY_MAX, r, err, cap) : ut_fetch(u, cfg, r, err, cap);
    free(u);
    return rc;
}

int ld_fetch_page(const char *url, const struct ut_config *cfg, struct ut_response *r, int *file, char *err, size_t cap) {
    return fetch(url, cfg, 0, r, file, err, cap);
}

void ld_fetch_free(struct ut_response *r, int file) {
    if(file) free(r->body);
    else ut_response_free(r);
}

/**** the answer ****/

static void put(const void *p, size_t n) {
    const char *s = p;
    while(n) {
        ssize_t k = write(1, s, n);
        if(k <= 0) {
            if(k < 0 && errno == EINTR) continue;
            exit(1);                    /* nobody to answer: the window has gone, or stopped wanting this */
        }
        s += k;
        n -= (size_t)k;
    }
}

static int answer_failed(const char *why, int w, int h) {
    struct ld_head a;
    memset(&a, 0, sizeof(a));
    a.magic = LD_MAGIC;
    a.what = LD_FAILED;
    a.w = (unsigned int)w;
    a.h = (unsigned int)h;
    a.len = (unsigned int)strlen(why);
    put(&a, sizeof(a));
    put(why, a.len);
    return 1;
}

static void head_of(struct ld_head *a, const struct ut_response *r, unsigned int what) {
    memset(a, 0, sizeof(*a));
    a->magic = LD_MAGIC;
    a->what = what;
    a->status = r->status;
    a->verified = r->verified;
    a->redirects = r->redirects;
    a->url_len = (unsigned int)strlen(r->url.text);
    snprintf(a->tls, sizeof(a->tls), "%s", r->tls);
    snprintf(a->reason, sizeof(a->reason), "%s", r->reason);
    snprintf(a->type, sizeof(a->type), "%s", r->content_type);
    snprintf(a->charset, sizeof(a->charset), "%s", r->charset);
}

static int answer_body(const struct ut_response *r) {
    struct ld_head a;
    head_of(&a, r, LD_BODY);
    a.len = (unsigned int)r->len;
    put(&a, sizeof(a));
    put(r->url.text, a.url_len);
    put(r->body, r->len);
    return 0;
}

static int answer_picture(const struct ut_response *r) {
    struct ld_head a;
    struct wimg m;
    char err[UT_ERR_MAX];
    int w, h;
    if(r->status < 200 || r->status > 299) {
        snprintf(err, sizeof(err), "HTTP %d %s", r->status, r->reason);
        return answer_failed(err, 0, 0);
    }
    if(wimg_decode(r->body, r->len, &m, err, sizeof(err))) return answer_failed(err, 0, 0);
    w = m.w;
    h = m.h;
    wimg_shrink(&m, WIMG_KEEP_SIDE);
    head_of(&a, r, LD_PIXELS);
    a.w = (unsigned int)w;
    a.h = (unsigned int)h;
    a.pw = (unsigned int)m.w;
    a.ph = (unsigned int)m.h;
    a.len = (unsigned int)((size_t)m.w * m.h * sizeof(*m.px));
    put(&a, sizeof(a));
    put(r->url.text, a.url_len);
    put(m.px, a.len);
    wimg_free(&m);
    return 0;
}

int ld_helper(const char *kind, const struct ut_config *cfg) {
    static struct ut_response r;
    char err[UT_ERR_MAX];
    const char *url = getenv(LD_URL_ENV);
    int image = !strcmp(kind, "image"), file = 0, rc;
    if(!url) return answer_failed("the helper was given no address", 0, 0);
    err[0] = 0;
    if(fetch(url, cfg, image, &r, &file, err, sizeof(err))) return answer_failed(err[0] ? err : "the load failed", 0, 0);
    rc = image ? answer_picture(&r) : answer_body(&r);
    ld_fetch_free(&r, file);
    return rc;
}
