/*
SurfOS Undertow's window: the program
----------------------
File: main.c  Date: 10/25/26
----------------------
wundertow, the web browser in a window (docs/UNDERTOW.md):

    wundertow [ADDRESS]                       a window on the desktop of the terminal it was started from
    wundertow --layout ADDRESS [WIDTH]        the page laid out at WIDTH pixels (640), as text (the tests')
    wundertow --render ADDRESS FILE [WIDTH]   the page painted at WIDTH pixels, as a BMP picture in FILE
    wundertow --load ADDRESS [WIDTH]          as --layout, loaded as the window loads: through a helper

ADDRESS is a web address (https:// when it names no scheme) or a file on this computer (/path or
file:///path). The web is Undertow's own: url.c, http.c and transport.c, with every HTTPS check, its limits,
its settings and clock in /initrd/etc/undertow.conf. 11/1/26 (stage 2.3): the window loads in helpers (loader.h),
wundertow --helper KIND, which nobody types, and goes on answering meanwhile; --layout and --render, with no window to
keep answering, load their page themselves.
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
#include "loader.h"
#include <surf.h>
#include <surfos/win.h>

static struct ut_config cfg;            /* 4 KB: off the stack */
static struct loader L;

static int usage(void) {
    fprintf(stderr, "usage: wundertow [ADDRESS]\n"
                    "       wundertow --layout ADDRESS [WIDTH]\n"
                    "       wundertow --render ADDRESS FILE.bmp [WIDTH]\n"
                    "       wundertow --load ADDRESS [WIDTH]\n"
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

/* The page at url (http, https or file): 0 and *out, or -1 with why */
static int load(const char *url, struct wb_page **out, char *err, size_t cap) {
    static struct ut_response r;
    int file = 0;
    if(ld_fetch_page(url, &cfg, &r, &file, err, cap)) return -1;
    *out = wb_page_new(&r, file, err, cap);
    ld_fetch_free(&r, file);
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

/* The page laid out at width, as text */
static int print_layout(struct wb_page *p, int width) {
    char *s, shown[WB_ADDR_MAX];
    wl_free(&p->lay);
    p->lay_w = 0;
    if(wl_layout(&p->doc, width, &p->lay)) {
        fprintf(stderr, "wundertow: out of memory laying out the page\n");
        return 1;
    }
    ut_safe_text(shown, sizeof(shown), p->url, strlen(p->url), 1);      /* it came from a server, or a helper */
    printf("address: %s\nstatus: %d%s%s\ntitle: %s\n", shown, p->status, p->verified ? " verified " : " ",
           p->verified ? p->tls : "", p->doc.title);
    if(p->doc.notice[0]) printf("notice: %s\n", p->doc.notice);
    s = wl_dump(&p->lay, &p->doc);
    fputs(s ? s : "(out of memory)\n", stdout);
    free(s);
    return 0;
}

static int layout_mode(const char *typed, int width) {
    static struct wb b;
    struct wb_page *p;
    char err[UT_ERR_MAX];
    int rc;
    err[0] = 0;
    if(wb_init(&b, 640, 400) || load_typed(typed, &b, &p, err, sizeof(err))) {
        fprintf(stderr, "wundertow: %s\n", err[0] ? err : b.status);
        return 1;
    }
    rc = print_layout(p, width);
    wb_page_free(p);
    wb_free(&b);
    return rc;
}

/* What became of the page's pictures, and each one's colour at its middle, for the tests */
static void print_pictures(const struct wb_page *p) {
    static const char *const states[] = { "waits", "loading", "loaded", "failed", "refused" };
    size_t i;
    for(i = 0; i < p->doc.image_count; i++) {
        const struct wd_picture *m = &p->doc.pics[i];
        printf("picture %u %s %dx%d", (unsigned int)i, states[m->state], m->w, m->h);
        if(m->px) printf(" kept %dx%d middle %08x", m->pw, m->ph, m->px[(size_t)(m->ph / 2) * m->pw + m->pw / 2]);
        printf("\n");
    }
}

/* --load: as the window loads, with no window: the tests see the helpers and the rules of what a page loads */
static int load_mode(const char *typed, int width) {
    static struct wb b;
    int win, rc;
    if(wb_init(&b, 640, 400)) {
        fprintf(stderr, "wundertow: out of memory\n");
        return 1;
    }
    wb_go(&b, typed);
    if(!b.want) {
        fprintf(stderr, "wundertow: %s\n", b.status);
        return 1;
    }
    for(;;) {
        ld_start_loads(&L, &b);
        if(!ld_busy(&L)) break;
        ld_wait(&L, &b, -1, &win);
    }
    if(!b.npages) {
        fprintf(stderr, "wundertow: %s\n", b.status);
        return 1;
    }
    rc = print_layout(wb_current(&b), width);
    print_pictures(wb_current(&b));
    while(ld_reap(&L)) msleep(20);
    wb_free(&b);
    return rc;
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
    {
        char shown[WB_ADDR_MAX];
        ut_safe_text(shown, sizeof(shown), p->url, strlen(p->url), 1);
        printf("wundertow: %s: %dx%d pixels of %s (\"%s\")\n", file, t.w, h, shown, p->doc.title);
    }
    free(px);
    wb_page_free(p);
    wb_free(&b);
    return 0;
}

/**** the window ****/

/* The stack the window will want, while there is memory: a helper may take what is left later, and a program whose stack
   cannot grow is ended, where a failed malloc is only a notice */
static void stack_ready(void) {
    volatile char room[64 * 1024];
    size_t i;
    for(i = 0; i < sizeof(room); i += 4096) room[i] = 0;
}

static int window_mode(const char *typed) {
    static struct wb b;
    struct win_open o;
    struct win_info info;
    struct win_present pr;
    struct win_event ev[16];
    unsigned int *px;
    char title[WIN_TITLE_MAX], shown[WIN_TITLE_MAX] = "";
    int fd, w, h, n, i, ready, redraw = 1;
    memset(&o, 0, sizeof(o));
    o.version = WIN_VERSION;
    stack_ready();
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
        if(ld_start_loads(&L, &b)) {
            redraw = 1;
            continue;
        }
        redraw |= ld_wait(&L, &b, fd, &ready);
        if(!ready) continue;
        n = (int)read(fd, ev, sizeof(ev));
        if(n == 0) break;                           /* the desktop let the window go */
        if(n < 0) {
            if(errno == EINTR) continue;
            break;
        }
        for(i = 0; i < n / (int)sizeof(ev[0]); i++) {
            switch(ev[i].type) {
            case WIN_EV_KEY:
                redraw |= wb_key(&b, ev[i].value == '\r' && (ev[i].mods & WIN_MOD_SHIFT) ? WB_KEY_SHIFT_ENTER : ev[i].value);
                break;
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
                goto closed;
            }
        }
    }
closed:
    ld_stop_all(&L);
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
    ld_init(&L);
    if(argc > 0 && strchr(argv[0], '/')) ld_self = argv[0];     /* a build run from elsewhere is its own helper */
    if(argc == 3 && !strcmp(argv[1], "--helper")) rc = ld_helper(argv[2], &cfg);
    else if(argc >= 3 && argc <= 4 && !strcmp(argv[1], "--load"))
        rc = (width = width_arg(argc > 3 ? argv[3] : NULL)) < 0 ? usage() : load_mode(argv[2], width);
    else if(argc >= 3 && argc <= 4 && !strcmp(argv[1], "--layout"))
        rc = (width = width_arg(argc > 3 ? argv[3] : NULL)) < 0 ? usage() : layout_mode(argv[2], width);
    else if(argc >= 4 && argc <= 5 && !strcmp(argv[1], "--render"))
        rc = (width = width_arg(argc > 4 ? argv[4] : NULL)) < 0 ? usage() : render_mode(argv[2], argv[3], width);
    else if(argc > 2 || (argc == 2 && argv[1][0] == '-')) rc = usage();
    else rc = window_mode(argc == 2 ? argv[1] : NULL);
    ut_transport_cleanup();
    return rc;
}
