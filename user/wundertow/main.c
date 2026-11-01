/*
SurfOS Undertow's window: the program
----------------------
File: main.c  Date: 10/25/26
----------------------
wundertow, the web browser in a window (docs/UNDERTOW.md):

    wundertow [ADDRESS]                       a window on the desktop of the terminal it was started from
    wundertow --layout ADDRESS [WIDTH]        the page laid out at WIDTH pixels (640), as text (the tests')
    wundertow --render ADDRESS FILE [WIDTH]   the page painted at WIDTH pixels, as a BMP picture in FILE

ADDRESS is a web address (https:// when it names no scheme) or a file on this computer (/path or
file:///path). The web is Undertow's own: url.c, http.c and transport.c, with every HTTPS check, its limits,
its settings and clock in /initrd/etc/undertow.conf. Loading waits for the page (at most Undertow's minute);
the window says so first.
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
#include "transport.h"
#include "doc.h"
#include "layout.h"
#include "paint.h"
#include "browser.h"
#include <surf.h>
#include <surfos/win.h>

static struct ut_config cfg;            /* 4 KB: off the stack */

static int usage(void) {
    fprintf(stderr, "usage: wundertow [ADDRESS]\n"
                    "       wundertow --layout ADDRESS [WIDTH]\n"
                    "       wundertow --render ADDRESS FILE.bmp [WIDTH]\n"
                    "ADDRESS: a web address (example.com, https://...) or a file (/initrd/www/index.html)\n"
                    "WIDTH: 64 to %d pixels (640)\n", WL_WIDTH_MAX);
    return 2;
}

/* WIDTH on the command line (NULL: none, 640): a whole number from 64 to WL_WIDTH_MAX, or -1 */
static int width_arg(const char *s) {
    char *end;
    long v;
    if(!s) return 640;
    v = strtol(s, &end, 10);
    return end != s && !*end && v >= 64 && v <= WL_WIDTH_MAX ? (int)v : -1;
}

/**** loading ****/

static int ends_with(const char *s, const char *end) {
    size_t n = strlen(s), k = strlen(end);
    size_t i;
    if(n < k) return 0;
    for(i = 0; i < k; i++) if((s[n - k + i] | 0x20) != end[i]) return 0;
    return 1;
}

/* A file's response: read whole (at most UT_BODY_MAX bytes), at https://WB_FILE_HOST/path, HTML by its name */
static int load_file(const char *path, struct ut_response *r, char *err, size_t cap) {
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
    r->body = malloc(UT_BODY_MAX + 1);
    if(!r->body) {
        close(fd);
        return ut_error(err, cap, "out of memory reading %s", path);
    }
    while(n < UT_BODY_MAX && (k = read(fd, r->body + n, UT_BODY_MAX - n)) > 0) n += (size_t)k;
    if(n == UT_BODY_MAX && read(fd, &k, 1) > 0) {
        close(fd);
        free(r->body);
        r->body = NULL;
        return ut_error(err, cap, "%s is over 1 MB: Undertow shows pages of 1 MB at most", path);
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

/* The page at url (http, https or file): 0 and *out, or -1 with why */
static int load(const char *url, struct wb_page **out, char *err, size_t cap) {
    struct ut_response r;
    struct ut_url *u;
    char path[UT_URL_MAX + 1];
    int file = wb_file_path(url, path, sizeof(path)), rc;
    memset(&r, 0, sizeof(r));
    if(file) rc = load_file(path, &r, err, cap);
    else {
        u = malloc(sizeof(*u));
        if(!u) return ut_error(err, cap, "out of memory");
        rc = ut_url_parse(url, u, err, cap);
        if(!rc) rc = ut_fetch(u, &cfg, &r, err, cap);
        free(u);
    }
    if(rc) return -1;
    *out = wb_page_new(&r, file, err, cap);
    if(file) free(r.body);
    else ut_response_free(&r);
    return *out ? 0 : -1;
}

/* An address as typed on the command line, as the window's address field takes it */
static int load_typed(const char *typed, struct wb *b, struct wb_page **out, char *err, size_t cap) {
    struct wb_request q;
    wb_go(b, typed);
    if(!wb_take(b, &q)) return ut_error(err, cap, "%s", b->status);
    return load(q.url, out, err, cap);
}

/**** --layout and --render ****/

static int layout_mode(const char *typed, int width) {
    static struct wb b;
    struct wb_page *p;
    char err[UT_ERR_MAX], *s;
    err[0] = 0;
    if(wb_init(&b, 640, 400) || load_typed(typed, &b, &p, err, sizeof(err))) {
        fprintf(stderr, "wundertow: %s\n", err[0] ? err : b.status);
        return 1;
    }
    if(wl_layout(&p->doc, width, &p->lay)) {
        fprintf(stderr, "wundertow: out of memory laying out the page\n");
        return 1;
    }
    printf("address: %s\nstatus: %d%s%s\ntitle: %s\n", p->url, p->status, p->verified ? " verified " : " ",
           p->verified ? p->tls : "", p->doc.title);
    if(p->doc.notice[0]) printf("notice: %s\n", p->doc.notice);
    s = wl_dump(&p->lay, &p->doc);
    fputs(s ? s : "(out of memory)\n", stdout);
    free(s);
    wb_page_free(p);
    wb_free(&b);
    return 0;
}

static void put32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* A 24-bit BMP of w x h pixels (the Image Viewer opens it) */
static int write_bmp(const char *path, const unsigned int *px, int w, int h) {
    unsigned char head[54], *row;
    int fd, y, x, rowlen = (w * 3 + 3) & ~3;
    memset(head, 0, sizeof(head));
    head[0] = 'B';
    head[1] = 'M';
    put32(head + 2, (unsigned int)(54 + rowlen * h));
    put32(head + 10, 54);
    put32(head + 14, 40);
    put32(head + 18, (unsigned int)w);
    put32(head + 22, (unsigned int)h);
    head[26] = 1;
    head[28] = 24;
    put32(head + 34, (unsigned int)(rowlen * h));
    put32(head + 38, 2835);
    put32(head + 42, 2835);
    row = calloc(1, (size_t)rowlen);
    if(!row) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0 || write(fd, head, sizeof(head)) != (ssize_t)sizeof(head)) {
        if(fd >= 0) close(fd);
        free(row);
        return -1;
    }
    for(y = h - 1; y >= 0; y--) {                   /* bottom row first */
        for(x = 0; x < w; x++) {
            unsigned int c = px[(size_t)y * w + x];
            row[x * 3] = (unsigned char)c;
            row[x * 3 + 1] = (unsigned char)(c >> 8);
            row[x * 3 + 2] = (unsigned char)(c >> 16);
        }
        if(write(fd, row, (size_t)rowlen) != rowlen) {
            close(fd);
            free(row);
            return -1;
        }
    }
    free(row);
    return close(fd);
}

static int render_mode(const char *typed, const char *file, int width) {
    static struct wb b;
    struct wb_page *p;
    struct wp_target t;
    unsigned int *px;
    char err[UT_ERR_MAX];
    int h;
    err[0] = 0;
    if(wb_init(&b, 640, 400) || load_typed(typed, &b, &p, err, sizeof(err))) {
        fprintf(stderr, "wundertow: %s\n", err[0] ? err : b.status);
        return 1;
    }
    if(wl_layout(&p->doc, width, &p->lay)) {
        fprintf(stderr, "wundertow: out of memory laying out the page\n");
        return 1;
    }
    h = p->lay.height > 8000 ? 8000 : p->lay.height;
    px = malloc((size_t)p->lay.width * h * sizeof(*px));
    if(!px) {
        fprintf(stderr, "wundertow: out of memory painting the page\n");
        return 1;
    }
    t.pixels = px;
    t.stride = p->lay.width;
    t.w = p->lay.width;
    t.h = h;
    t.x = t.y = 0;
    t.cw = t.w;
    t.ch = h;
    wp_paint(&p->lay, &p->doc, &b.theme, &t, 0, 0, -1);
    if(write_bmp(file, px, t.w, h)) {
        fprintf(stderr, "wundertow: %s: %s\n", file, strerror(errno));
        return 1;
    }
    printf("wundertow: %s: %dx%d pixels of %s (\"%s\")\n", file, t.w, h, p->url, p->doc.title);
    free(px);
    wb_page_free(p);
    wb_free(&b);
    return 0;
}

/**** the window ****/

static int window_mode(const char *typed) {
    static struct wb b;
    struct win_open o;
    struct win_info info;
    struct win_present pr;
    struct win_event ev[16];
    struct wb_request q;
    unsigned int *px;
    char title[WIN_TITLE_MAX], shown[WIN_TITLE_MAX] = "";
    int fd, w, h, n, i, redraw = 1;
    memset(&o, 0, sizeof(o));
    o.version = WIN_VERSION;
    o.w = 760;
    o.h = 460;
    o.min_w = WB_MIN_W;
    o.min_h = WB_MIN_H;
    o.flags = WIN_RESIZABLE;
    o.background = 0xfffdf5;
    snprintf(o.title, sizeof(o.title), "Undertow");
    fd = winopen(&o);
    if(fd < 0) {
        fprintf(stderr, "wundertow: no window: %s (wundertow --layout and --render work anywhere)\n",
                errno == ENODEV ? "this terminal is no desktop's" : strerror(errno));
        return 1;
    }
    memset(&info, 0, sizeof(info));
    info.version = WIN_VERSION;
    winctl(fd, WIN_INFO, &info);
    w = info.w;
    h = info.h;
    px = malloc((size_t)w * h * sizeof(*px));
    if(!px || wb_init(&b, w, h)) {
        fprintf(stderr, "wundertow: out of memory\n");
        return 1;
    }
    if(typed) wb_go(&b, typed);
    for(;;) {
        if(redraw) {
            struct wb_page *p = wb_current(&b);
            wb_paint(&b, px, w);
            pr.pixels = px;
            pr.stride = (unsigned int)w;
            pr.x = pr.y = 0;
            pr.w = (unsigned short)w;
            pr.h = (unsigned short)h;
            if(winctl(fd, WIN_PRESENT, &pr) < 0 && errno == EPIPE) break;
            if(b.npages && p->doc.title[0]) snprintf(title, sizeof(title), "%s - Undertow", p->doc.title);
            else snprintf(title, sizeof(title), "Undertow");
            if(strcmp(title, shown)) {
                winctl(fd, WIN_TITLE, title);
                snprintf(shown, sizeof(shown), "%s", title);
            }
            redraw = 0;
        }
        if(wb_take(&b, &q)) {                       /* the window says Loading... first */
            struct wb_page *p;
            char err[UT_ERR_MAX];
            if(load(q.url, &p, err, sizeof(err))) wb_failed(&b, q.url, err);
            else wb_loaded(&b, p, q.reload);
            redraw = 1;
            continue;
        }
        n = (int)read(fd, ev, sizeof(ev));
        if(n == 0) break;                           /* the desktop let the window go */
        if(n < 0) {
            if(errno == EINTR) continue;
            break;
        }
        for(i = 0; i < n / (int)sizeof(ev[0]); i++) {
            switch(ev[i].type) {
            case WIN_EV_KEY: redraw |= wb_key(&b, ev[i].value); break;
            case WIN_EV_POINTER: redraw |= wb_pointer(&b, ev[i].x, ev[i].y, ev[i].value); break;
            case WIN_EV_WHEEL: redraw |= wb_wheel(&b, (int)ev[i].value); break;
            case WIN_EV_RESIZE:
                if(ev[i].x > 0 && ev[i].y > 0) {
                    unsigned int *np = malloc((size_t)ev[i].x * ev[i].y * sizeof(*np));
                    if(np && !winctl(fd, WIN_SETSIZE, (void *)(unsigned long)((unsigned int)ev[i].x << 16 | (unsigned int)ev[i].y))) {
                        free(px);
                        px = np;
                        w = ev[i].x;
                        h = ev[i].y;
                        wb_resize(&b, w, h);
                        redraw = 1;
                    } else free(np);
                }
                break;
            case WIN_EV_CLOSE:
                close(fd);
                free(px);
                wb_free(&b);
                ut_transport_cleanup();
                return 0;
            }
        }
    }
    close(fd);
    free(px);
    wb_free(&b);
    return 0;
}

int main(int argc, char **argv) {
    char err[UT_ERR_MAX];
    int rc, width;
    ut_config_init(&cfg);
    if(ut_config_load(&cfg, UT_CONF_PATH, err, sizeof(err)) < 0)
        fprintf(stderr, "wundertow: %s: %s (the defaults are used)\n", UT_CONF_PATH, err);
    if(cfg.font) wd_base = cfg.font;
    if(argc >= 3 && argc <= 4 && !strcmp(argv[1], "--layout"))
        rc = (width = width_arg(argc > 3 ? argv[3] : NULL)) < 0 ? usage() : layout_mode(argv[2], width);
    else if(argc >= 4 && argc <= 5 && !strcmp(argv[1], "--render"))
        rc = (width = width_arg(argc > 4 ? argv[4] : NULL)) < 0 ? usage() : render_mode(argv[2], argv[3], width);
    else if(argc > 2 || (argc == 2 && argv[1][0] == '-')) rc = usage();
    else rc = window_mode(argc == 2 ? argv[1] : NULL);
    ut_transport_cleanup();
    return rc;
}
