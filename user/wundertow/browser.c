/*
SurfOS Undertow's window: the browser
----------------------
File: browser.c  Date: 10/25/26
----------------------
browser.h's window: its chrome drawn as the desktop draws its own apps (docs/GUI.md: a BAND toolbar of SHELL
buttons, a text field, a scroll bar, a status bar), the page through layout.c and paint.c.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "undertow.h"
#include "url.h"
#include "http.h"
#include "doc.h"
#include "font.h"
#include "layout.h"
#include "paint.h"
#include "browser.h"

/* the desktop's colours (docs/GUI.md) */
#define C_INK    0x153d49
#define C_TEAL   0x197d83
#define C_PAPER  0xf4f1e6
#define C_MUTED  0x6c888c
#define C_SHELL  0xdee3d8
#define C_BAND   0xe7e2d2
#define C_LINE   0xc9cfc4
#define C_PICK   0xbfe3dd
#define C_SHEET  0xfffdf5

#define BAR_Y    4                      /* the toolbar's buttons and field */
#define BAR_H    22
#define SCROLL_STEP 32                  /* an arrow's scroll */
#define WHEEL_ROW   16

static const char start_page[] =
    "<title>Undertow</title>"
    "<h1>Undertow</h1>"
    "<p>Type an address in the field above and press Enter: a web address (<code>example.com</code>, "
    "<code>https://...</code>) or a file on this computer (<code>/initrd/www/index.html</code>)."
    "<p>The sample web site that comes with SurfOS: <a href=\"/initrd/www/index.html\">/initrd/www/index.html</a>"
    "<h3>Keys</h3>"
    "<ul><li><b>Ctrl+L</b>: the address field"
    "<li><b>Page Up</b>, <b>Page Down</b>, <b>Space</b>, the <b>arrows</b>, <b>Home</b>, <b>End</b>: scroll"
    "<li><b>Backspace</b>: back"
    "<li><b>Ctrl+R</b>: load the page again</ul>"
    "<p>HTTPS is checked as Undertow checks it in the console: a certificate must be valid for the name, "
    "signed by a known authority, and in date.";

static const char *const labels[4] = { "\xe2\x86\x90 Back", "Forward \xe2\x86\x92", "Reload", "Go" };
static const char stop_label[] = "Stop";             /* in Reload's place while anything loads */

/**** geometry ****/

struct geom {
    int bx[4], bw[4];                   /* the buttons: Back, Forward, Reload, Go */
    int ax, aw;                         /* the address field */
    int cy, cw, ch;                     /* the page's area: from (0, cy), cw x ch */
    int sx;                             /* the scroll bar */
};

static void geom(const struct wb *b, struct geom *g) {
    int i, x = 7;
    for(i = 0; i < 4; i++) g->bw[i] = (int)wf_count(labels[i], strlen(labels[i])) * WF_W + 14;
    for(i = 0; i < 3; i++) {
        g->bx[i] = x;
        x += g->bw[i] + 4;
    }
    g->bx[3] = b->w - 7 - g->bw[3];
    g->ax = x;
    g->aw = g->bx[3] - 4 - x;
    if(g->aw < 40) g->aw = 40;
    g->cy = WB_TOOLBAR;
    g->cw = b->w - WB_SCROLLBAR;
    g->ch = b->h - WB_TOOLBAR - WB_STATUS;
    if(g->cw < 1) g->cw = 1;
    if(g->ch < 1) g->ch = 1;
    g->sx = b->w - WB_SCROLLBAR;
}

/**** pages ****/

struct wb_page *wb_current(struct wb *b) { return b->npages ? b->page[b->cur] : b->start; }

void wb_page_free(struct wb_page *p) {
    if(!p) return;
    wd_free(&p->doc);
    wl_free(&p->lay);
    free(p->texts);
    free(p);
}

static int file_url(const char *text, char *out, size_t cap) {     /* https://WB_FILE_HOST/x -> file:///x */
    static const char prefix[] = "https://" WB_FILE_HOST "/";
    if(strncmp(text, prefix, sizeof(prefix) - 1)) return 0;
    snprintf(out, cap, "file:///%s", text + sizeof(prefix) - 1);
    return 1;
}

struct wb_page *wb_page_new(const struct ut_response *r, int file, char *err, size_t cap) {
    struct wb_page *p = calloc(1, sizeof(*p));
    if(!p) {
        ut_error(err, cap, "out of memory showing the page");
        return NULL;
    }
    if(wd_build(r, &p->doc, err, cap)) {
        free(p);
        return NULL;
    }
    if(!(file && file_url(r->url.text, p->url, sizeof(p->url)))) snprintf(p->url, sizeof(p->url), "%s", r->url.text);
    if(r->url.fragment[0]) {
        size_t n = strlen(p->url);
        snprintf(p->fragment, sizeof(p->fragment), "%s", r->url.fragment);
        if(!strchr(p->url, '#')) snprintf(p->url + n, sizeof(p->url) - n, "#%s", r->url.fragment);
    }
    p->file = file;
    p->status = r->status;
    p->verified = r->verified;
    snprintf(p->tls, sizeof(p->tls), "%s", r->tls);
    snprintf(p->reason, sizeof(p->reason), "%s", r->reason);
    p->bytes = sizeof(*p) + p->doc.bytes;
    return p;
}

static int hexval(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int wb_file_path(const char *url, char *path, size_t cap) {
    size_t i, n = 0;
    if(strncmp(url, "file://", 7) || url[7] != '/') return 0;
    for(i = 7; url[i] && url[i] != '#' && url[i] != '?' && n + 1 < cap; i++) {
        int hi, lo;
        if(url[i] == '%' && (hi = hexval(url[i + 1])) >= 0 && (lo = hexval(url[i + 2])) >= 0) {
            path[n++] = (char)(hi << 4 | lo);
            i += 2;
        } else path[n++] = url[i];
    }
    path[n] = 0;
    return 1;
}

/**** the status bar and the address field ****/

static void status(struct wb *b, const char *fmt, const char *arg) {
    char raw[512];
    snprintf(raw, sizeof(raw), fmt, arg ? arg : "");
    ut_safe_text(b->status, sizeof(b->status), raw, strlen(raw), 1);
}

static void set_addr(struct wb *b, const char *s) {
    snprintf(b->addr, sizeof(b->addr), "%s", s);
    b->addr_len = strlen(b->addr);
    b->caret = b->addr_len;
    b->chosen = 0;
}

static void loaded_status(struct wb *b, const struct wb_page *p) {
    char s[128];
    if(p->status && p->status != 200) {
        snprintf(s, sizeof(s), "HTTP %d %s", p->status, p->reason);
        status(b, "%s", s);
    } else if(p->doc.notice[0]) status(b, "%s", p->doc.notice);
    else status(b, "%s", "Done");
}

/**** scrolling ****/

static void ensure_layout(struct wb *b, struct wb_page *p) {
    struct geom g;
    size_t off;
    geom(b, &g);
    if(p->lay_w == g.cw) return;
    wl_free(&p->lay);
    p->lay_w = 0;
    if(wl_layout(&p->doc, g.cw, &p->lay)) {
        status(b, "%s", "out of memory laying out the page");
        return;
    }
    p->lay_w = g.cw;
    if(p->fragment[0]) {
        if(!wd_anchor_offset(&p->doc, p->fragment, &off)) p->scroll = wl_offset_y(&p->lay, off) - 4;
        else status(b, "There is no place named \"%s\" on this page", p->fragment);
        p->fragment[0] = 0;
    }
}

static int max_scroll(struct wb *b, struct wb_page *p) {
    struct geom g;
    geom(b, &g);
    ensure_layout(b, p);
    return p->lay.height > g.ch ? p->lay.height - g.ch : 0;
}

static int scroll_to(struct wb *b, int y) {
    struct wb_page *p = wb_current(b);
    int m, old;
    if(!p) return 0;
    m = max_scroll(b, p);
    old = p->scroll;
    p->scroll = y < 0 ? 0 : y > m ? m : y;
    return p->scroll != old;
}

static int scroll_by(struct wb *b, int dy) {
    struct wb_page *p = wb_current(b);
    return p ? scroll_to(b, p->scroll + dy) : 0;
}

/* the thumb's place and size in the scroll bar's track (cy .. cy + ch) */
static void thumb(struct wb *b, int *ty, int *th) {
    struct wb_page *p = wb_current(b);
    struct geom g;
    int m;
    geom(b, &g);
    *ty = g.cy;
    *th = g.ch;
    if(!p) return;
    m = max_scroll(b, p);
    if(!m) return;
    *th = (int)((long)g.ch * g.ch / p->lay.height);
    if(*th < 16) *th = 16;
    if(*th > g.ch) *th = g.ch;
    *ty = g.cy + (int)((long)(g.ch - *th) * p->scroll / m);
}

/**** loading ****/

static void request(struct wb *b, const char *url, int reload) {
    snprintf(b->req.url, sizeof(b->req.url), "%s", url);
    b->req.reload = reload;
    b->req.kind = WB_LOAD_PAGE;
    b->req.page = 0;
    b->req.item = 0;
    b->req.budget = 0;
    if(b->running) b->cancel |= WB_CANCEL_PAGE;     /* a new load takes the place of the one on its way */
    b->running = 0;
    b->want = 1;
    b->loading = 1;
    set_addr(b, url);
    status(b, "Loading %s...", url);
}

/* May page p load url for itself? The address to load in out (cap bytes). Undertow's rules: HTTPS never to HTTP, and
   the rule of files: only a file page's addresses are files, and an http(s) page gets at none. NULL, or why not. */
static const char *may_load(const struct wb_page *p, const char *url, char *out, size_t cap) {
    static const char file_host[] = "https://" WB_FILE_HOST "/";
    if(p->file && file_url(url, out, cap)) return NULL;
    if(!strncmp(url, file_host, sizeof(file_host) - 1)) return "a web page's address never leads to a file";
    if(strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) return "not an http or https address";
    if(!strncmp(p->url, "https:", 6) && !strncmp(url, "http:", 5)) return "an https page's address over http";
    if((size_t)snprintf(out, cap, "%s", url) >= cap) return "too long an address";
    return NULL;
}

/* The next thing the page shown wants */
static int next_resource(struct wb *b, struct wb_request *out) {
    struct wb_page *p = wb_current(b);
    int i;
    if(!b->npages || p->halted || b->inflight >= WB_RESOURCES) return 0;
    for(i = 0; i < p->ntexts; i++) {
        struct wb_text *t = &p->texts[i];
        if(t->state != WP_WAIT) continue;
        memset(out, 0, sizeof(*out));
        memcpy(out->url, t->url, sizeof(out->url));
        out->kind = WB_LOAD_TEXT;
        out->page = p->serial;
        out->item = t->tag;
        t->state = WP_RUNNING;
        b->inflight++;
        return 1;
    }
    return 0;
}

/* The page shown has what it was loading stopped: it may ask again */
static void resources_stop(struct wb *b) {
    struct wb_page *p = wb_current(b);
    int i;
    if(!b->inflight) return;
    b->cancel |= WB_CANCEL_RESOURCES;
    b->inflight = 0;
    if(p) for(i = 0; i < p->ntexts; i++) if(p->texts[i].state == WP_RUNNING) p->texts[i].state = WP_WAIT;
}

int wb_fetch(struct wb *b, struct wb_page *p, const char *url, int tag) {
    struct wb_text *t;
    (void)b;
    if(p->ntexts >= WB_TEXTS_MAX) return -1;
    t = realloc(p->texts, ((size_t)p->ntexts + 1) * sizeof(*t));
    if(!t) return -1;
    p->texts = t;
    t += p->ntexts;
    if(may_load(p, url, t->url, sizeof(t->url))) return -1;
    t->tag = tag;
    t->state = WP_WAIT;
    p->ntexts++;
    return 0;
}

/* Stage 3.3's stylesheets are to be taken in here */
void wb_text(struct wb *b, unsigned int page, int tag, const char *body, size_t len, const char *type, const char *why) {
    struct wb_page *p = wb_current(b);
    int i;
    (void)body;
    (void)len;
    (void)type;
    (void)why;
    if(!p || p->serial != page) return;             /* a page left: its load was stopped */
    for(i = 0; i < p->ntexts; i++)
        if(p->texts[i].tag == tag && p->texts[i].state == WP_RUNNING) {
            p->texts[i].state = WP_DONE;
            if(b->inflight) b->inflight--;
            return;
        }
}

void wb_go(struct wb *b, const char *typed) {
    char t[WB_ADDR_MAX], err[UT_ERR_MAX];
    struct ut_url *u;
    size_t n;
    const char *s = typed;
    while(*s == ' ') s++;
    snprintf(t, sizeof(t), "%s", s);
    n = strlen(t);
    while(n && t[n - 1] == ' ') t[--n] = 0;
    b->focus = WB_FOCUS_PAGE;
    if(!n) {
        status(b, "%s", "Type an address: example.com, https://..., or a file: /initrd/www/index.html");
        return;
    }
    if(!strncmp(t, "file:", 5) || t[0] == '/') {          /* a file on this computer */
        char url[WB_ADDR_MAX];
        const char *path = t[0] == '/' ? t : t + 5;
        while(path[0] == '/' && path[1] == '/') path++;     /* file:///x, file://x, file:/x */
        if(!strncmp(t, "file://localhost/", 17)) path = t + 16;
        if(snprintf(url, sizeof(url), "file://%s%s", path[0] == '/' ? "" : "/", path) >= (int)sizeof(url))
            status(b, "%s", "That address is too long");
        else request(b, url, 0);
        return;
    }
    u = malloc(sizeof(*u));
    if(!u) {
        status(b, "%s", "out of memory");
        return;
    }
    if(ut_url_parse(t, u, err, sizeof(err))) status(b, "%s", err);
    else request(b, u->text, 0);
    free(u);
}

int wb_take(struct wb *b, struct wb_request *out) {
    if(!b->want) return next_resource(b, out);
    *out = b->req;
    b->want = 0;
    b->running = 1;
    return 1;
}

int wb_cancel(struct wb *b) {
    int c = b->cancel;
    b->cancel = 0;
    return c;
}

int wb_busy(const struct wb *b) { return b->loading || b->inflight; }

void wb_stop(struct wb *b) {
    struct wb_page *p = wb_current(b);
    if(!wb_busy(b)) return;
    if(b->running) b->cancel |= WB_CANCEL_PAGE;
    b->running = b->loading = b->want = 0;
    resources_stop(b);
    if(p && b->npages) p->halted = 1;
    status(b, "%s", "Stopped");
}

static size_t history_bytes(const struct wb *b) {
    size_t n = 0;
    int i;
    for(i = 0; i < b->npages; i++) n += b->page[i]->bytes;
    return n;
}

void wb_loaded(struct wb *b, struct wb_page *p, int reload) {
    int i;
    resources_stop(b);                              /* what the page before was loading */
    b->loading = 0;
    b->running = 0;
    b->hover = b->pressed = -1;
    p->serial = ++b->serial;
    if(reload && b->npages) {                       /* in place of the page, at its place */
        p->scroll = b->page[b->cur]->scroll;
        if(p->fragment[0] && p->scroll) p->fragment[0] = 0;
        wb_page_free(b->page[b->cur]);
        b->page[b->cur] = p;
    } else {
        for(i = b->cur + 1; i < b->npages; i++) wb_page_free(b->page[i]);   /* no forward after a new page */
        b->npages = b->npages ? b->cur + 1 : 0;
        if(b->npages == WB_HISTORY) {
            wb_page_free(b->page[0]);
            memmove(b->page, b->page + 1, (WB_HISTORY - 1) * sizeof(b->page[0]));
            b->npages--;
        }
        b->page[b->npages++] = p;
        b->cur = b->npages - 1;
        while(b->npages > 1 && history_bytes(b) > WB_HISTORY_BYTES) {   /* the oldest go, never this one */
            wb_page_free(b->page[0]);
            memmove(b->page, b->page + 1, (size_t)(b->npages - 1) * sizeof(b->page[0]));
            b->npages--;
            b->cur--;
        }
    }
    set_addr(b, p->url);
    loaded_status(b, p);
    b->focus = WB_FOCUS_PAGE;
}

void wb_failed(struct wb *b, const char *url, const char *why) {
    b->loading = 0;
    b->running = 0;
    set_addr(b, url);
    status(b, "%s", why);
}

/* the link n of the page shown: followed, jumped to on this page, or why not */
static void follow(struct wb *b, int n) {
    struct wb_page *p = wb_current(b);
    const struct ut_link *l;
    char url[WB_ADDR_MAX];
    const char *hash, *here;
    size_t k;
    if(!p || n < 0 || (size_t)n >= p->doc.link_count) return;
    l = &p->doc.links[n];
    if(!l->usable) {
        status(b, "Not followed: %s is not an http or https address", l->url);
        return;
    }
    if(!(p->file && file_url(l->url, url, sizeof(url)))) snprintf(url, sizeof(url), "%s", l->url);
    hash = strchr(url, '#');
    here = p->url;
    k = hash ? (size_t)(hash - url) : strlen(url);
    if(hash && !strncmp(url, here, k) && (here[k] == 0 || here[k] == '#')) {   /* a place on this page */
        size_t off;
        if(!wd_anchor_offset(&p->doc, hash + 1, &off)) {
            ensure_layout(b, p);
            scroll_to(b, wl_offset_y(&p->lay, off) - 4);
            snprintf(p->url, sizeof(p->url), "%s", url);
            set_addr(b, url);
            status(b, "%s", "Done");
        } else status(b, "There is no place named \"%s\" on this page", hash + 1);
        return;
    }
    request(b, url, 0);
}

static void back_forward(struct wb *b, int d) {
    if(!b->npages || b->cur + d < 0 || b->cur + d >= b->npages) return;
    resources_stop(b);
    b->cur += d;
    b->hover = b->pressed = -1;
    set_addr(b, b->page[b->cur]->url);
    loaded_status(b, b->page[b->cur]);
}

static void reload(struct wb *b) {
    if(b->npages && !b->loading) request(b, b->page[b->cur]->url, 1);
}

/**** events ****/

int wb_init(struct wb *b, int w, int h) {
    struct ut_response *r;
    char err[UT_ERR_MAX];
    memset(b, 0, sizeof(*b));
    b->w = w < WB_MIN_W ? WB_MIN_W : w;
    b->h = h < WB_MIN_H ? WB_MIN_H : h;
    b->hover = b->pressed = -1;
    b->focus = WB_FOCUS_ADDR;
    wp_theme_default(&b->theme);
    r = calloc(1, sizeof(*r));
    if(!r) return -1;
    ut_url_parse("https://" WB_FILE_HOST "/", &r->url, err, sizeof(err));
    r->body = (unsigned char *)(void *)(size_t)start_page;
    r->len = sizeof(start_page) - 1;
    r->status = 200;
    snprintf(r->content_type, sizeof(r->content_type), "text/html");
    b->start = wb_page_new(r, 1, err, sizeof(err));
    free(r);
    if(!b->start) return -1;
    b->start->url[0] = 0;                           /* the start page has no address */
    status(b, "%s", "Type an address and press Enter");
    return 0;
}

void wb_free(struct wb *b) {
    int i;
    for(i = 0; i < b->npages; i++) wb_page_free(b->page[i]);
    wb_page_free(b->start);
    memset(b, 0, sizeof(*b));
}

static int char_start(const char *s, size_t i) { return ((unsigned char)s[i] & 0xc0) != 0x80; }

static int addr_key(struct wb *b, unsigned int key) {
    struct wb_page *p;
    switch(key) {
    case 13: case 10:
        wb_go(b, b->addr);
        return 1;
    case 27:
        p = wb_current(b);
        set_addr(b, p ? p->url : "");
        b->focus = WB_FOCUS_PAGE;
        return 1;
    case 9:
        b->focus = WB_FOCUS_PAGE;
        return 1;
    case 1: case 12:                                /* Ctrl+A, Ctrl+L: all of it */
        b->chosen = 1;
        b->caret = b->addr_len;
        return 1;
    case 127: case 8:
        if(b->chosen) {
            set_addr(b, "");
            return 1;
        }
        if(!b->caret) return 0;
        {
            size_t k = b->caret - 1;
            while(k && !char_start(b->addr, k)) k--;
            memmove(b->addr + k, b->addr + b->caret, b->addr_len - b->caret + 1);
            b->addr_len -= b->caret - k;
            b->caret = k;
        }
        return 1;
    case WB_KEY_DELETE:
        if(b->chosen) {
            set_addr(b, "");
            return 1;
        }
        if(b->caret < b->addr_len) {
            size_t k = b->caret + 1;
            while(k < b->addr_len && !char_start(b->addr, k)) k++;
            memmove(b->addr + b->caret, b->addr + k, b->addr_len - k + 1);
            b->addr_len -= k - b->caret;
        }
        return 1;
    case WB_KEY_LEFT:
        b->chosen = 0;
        while(b->caret && !char_start(b->addr, --b->caret)) ;
        return 1;
    case WB_KEY_RIGHT:
        b->chosen = 0;
        if(b->caret < b->addr_len) for(b->caret++; b->caret < b->addr_len && !char_start(b->addr, b->caret); b->caret++) ;
        return 1;
    case WB_KEY_HOME:
        b->chosen = 0;
        b->caret = 0;
        return 1;
    case WB_KEY_END:
        b->chosen = 0;
        b->caret = b->addr_len;
        return 1;
    }
    if(key < 0x20 || key > 0x7e) return 0;          /* the keyboard types ASCII */
    if(b->chosen) set_addr(b, "");
    if(b->addr_len + 1 >= sizeof(b->addr)) return 0;
    memmove(b->addr + b->caret + 1, b->addr + b->caret, b->addr_len - b->caret + 1);
    b->addr[b->caret++] = (char)key;
    b->addr_len++;
    return 1;
}

int wb_key(struct wb *b, unsigned int key) {
    struct geom g;
    if(b->focus == WB_FOCUS_ADDR) return addr_key(b, key);
    geom(b, &g);
    switch(key) {
    case WB_KEY_UP: return scroll_by(b, -SCROLL_STEP);
    case WB_KEY_DOWN: return scroll_by(b, SCROLL_STEP);
    case WB_KEY_PGUP: return scroll_by(b, -(g.ch - SCROLL_STEP));
    case WB_KEY_PGDN: case ' ': return scroll_by(b, g.ch - SCROLL_STEP);
    case WB_KEY_HOME: return scroll_to(b, 0);
    case WB_KEY_END: return scroll_to(b, 1 << 30);
    case 127: case 8:
        back_forward(b, -1);
        return 1;
    case 12:                                        /* Ctrl+L */
        b->focus = WB_FOCUS_ADDR;
        b->chosen = 1;
        b->caret = b->addr_len;
        return 1;
    case 18:                                        /* Ctrl+R */
        reload(b);
        return 1;
    case 27:                                        /* Escape: stops what is loading */
        wb_stop(b);
        return 1;
    }
    return 0;
}

static int in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

static int enabled(struct wb *b, int n) {
    if(n == 0) return b->npages && b->cur > 0;
    if(n == 1) return b->npages && b->cur < b->npages - 1;
    if(n == 2) return wb_busy(b) || b->npages;
    return 1;
}

/* the byte of the address at x in the field (its text shown from cell 'first') */
static size_t addr_at(struct wb *b, int x, size_t first) {
    size_t i = 0, cells = 0;
    unsigned int cp;
    while(i < b->addr_len) {
        size_t k = ut_utf8_decode((const unsigned char *)b->addr + i, b->addr_len - i, &cp);
        if(cells >= first && (int)((cells - first) * WF_W + WF_W / 2) > x) break;
        cells += wf_count(b->addr + i, k);
        i += k;
    }
    return i;
}

static size_t addr_first(struct wb *b, int aw) {    /* the first cell the field shows: the caret stays in view */
    size_t caret_cells = wf_count(b->addr, b->caret), room = (size_t)((aw - 8) / WF_W);
    return caret_cells >= room ? caret_cells - room + 1 : 0;
}

int wb_pointer(struct wb *b, int x, int y, unsigned int buttons) {
    struct geom g;
    struct wb_page *p = wb_current(b);
    int press = (buttons & 1) && !(b->buttons & 1), release = !(buttons & 1) && (b->buttons & 1), i, redraw = 0;
    int link = -1, ty, th;
    geom(b, &g);
    b->buttons = buttons;
    if(b->drag) {                                   /* the thumb follows the pointer */
        if(release) {
            b->drag = 0;
            return 1;
        }
        thumb(b, &ty, &th);
        if(g.ch > th && p) {
            int m = max_scroll(b, p);
            return scroll_to(b, b->drag_scroll + (int)((long)(y - b->drag_y) * m / (g.ch - th)));
        }
        return 0;
    }
    if(p && in(x, y, 0, g.cy, g.cw, g.ch)) {
        ensure_layout(b, p);
        link = wl_link_at(&p->lay, x, y - g.cy + p->scroll);
    }
    if(link != b->hover) {                          /* the status bar names the link */
        b->hover = link;
        if(link >= 0) {
            char shown[WB_ADDR_MAX];
            const struct ut_link *l = &p->doc.links[link];
            if(!(p->file && file_url(l->url, shown, sizeof(shown)))) snprintf(shown, sizeof(shown), "%s", l->url);
            status(b, "%s", shown);
        } else if(!b->loading && p && b->npages) loaded_status(b, p);
        redraw = 1;
    }
    if(press) {
        b->pressed = -1;
        b->button = 0;
        for(i = 0; i < 4; i++)
            if(in(x, y, g.bx[i], BAR_Y, g.bw[i], BAR_H) && enabled(b, i)) {
                b->button = i + 1;
                return 1;
            }
        if(in(x, y, g.ax, BAR_Y, g.aw, BAR_H)) {     /* the address field: all of it, then the caret */
            if(b->focus != WB_FOCUS_ADDR) {
                b->focus = WB_FOCUS_ADDR;
                b->chosen = 1;
                b->caret = b->addr_len;
            } else {
                b->chosen = 0;
                b->caret = addr_at(b, x - g.ax - 4, addr_first(b, g.aw));
            }
            return 1;
        }
        if(in(x, y, g.sx, g.cy, WB_SCROLLBAR, g.ch) && p) {
            thumb(b, &ty, &th);
            if(y < ty) return scroll_by(b, -(g.ch - SCROLL_STEP));
            if(y >= ty + th) return scroll_by(b, g.ch - SCROLL_STEP);
            b->drag = 1;
            b->drag_y = y;
            b->drag_scroll = p->scroll;
            return 1;
        }
        if(in(x, y, 0, g.cy, g.cw, g.ch)) {
            b->focus = WB_FOCUS_PAGE;
            b->pressed = link;
            return 1;
        }
    }
    if(release) {
        int n = b->button;
        b->button = 0;
        if(n && in(x, y, g.bx[n - 1], BAR_Y, g.bw[n - 1], BAR_H) && enabled(b, n - 1)) {
            if(n == 1) back_forward(b, -1);
            else if(n == 2) back_forward(b, 1);
            else if(n == 3 && wb_busy(b)) wb_stop(b);
            else if(n == 3) reload(b);
            else wb_go(b, b->addr);
            return 1;
        }
        if(b->pressed >= 0 && b->pressed == link) {
            int l = b->pressed;
            b->pressed = -1;
            follow(b, l);
            return 1;
        }
        b->pressed = -1;
        return n != 0 || redraw;
    }
    return redraw;
}

int wb_wheel(struct wb *b, int rows) { return scroll_by(b, rows * WHEEL_ROW); }

int wb_resize(struct wb *b, int w, int h) {
    if(w < WB_MIN_W) w = WB_MIN_W;
    if(h < WB_MIN_H) h = WB_MIN_H;
    if(w == b->w && h == b->h) return 0;
    b->w = w;
    b->h = h;
    scroll_by(b, 0);                                /* laid out again at the new width, the place kept within it */
    return 1;
}

/**** painting ****/

void wb_paint(struct wb *b, unsigned int *pixels, int stride) {
    struct wp_target t;
    struct geom g;
    struct wb_page *p = wb_current(b);
    int i, ty, th;
    char right[48];
    geom(b, &g);
    t.pixels = pixels;
    t.stride = stride;
    t.w = b->w;
    t.h = b->h;
    t.x = 0;
    t.y = 0;
    t.cw = b->w;
    t.ch = b->h;
    /* the toolbar */
    wp_fill(&t, 0, 0, b->w, WB_TOOLBAR - 1, C_BAND);
    wp_fill(&t, 0, WB_TOOLBAR - 1, b->w, 1, C_LINE);
    for(i = 0; i < 4; i++) {
        int on = enabled(b, i), down = b->button == i + 1;
        const char *label = i == 2 && wb_busy(b) ? stop_label : labels[i];
        wp_fill(&t, g.bx[i], BAR_Y, g.bw[i], BAR_H, down ? C_PICK : on ? C_SHELL : C_BAND);
        if(on) wp_frame(&t, g.bx[i], BAR_Y, g.bw[i], BAR_H, C_LINE);
        wp_text(&t, g.bx[i] + 7, BAR_Y + 3, label, strlen(label), 1, 0, on ? C_INK : C_MUTED);
    }
    /* the address field */
    {
        struct wp_target f = t;
        size_t first = addr_first(b, g.aw), i0 = 0, cells = 0;
        int focused = b->focus == WB_FOCUS_ADDR;
        unsigned int cp;
        wp_fill(&t, g.ax, BAR_Y, g.aw, BAR_H, C_SHEET);
        wp_frame(&t, g.ax, BAR_Y, g.aw, BAR_H, focused ? C_TEAL : C_LINE);
        f.x = g.ax + 2;
        f.y = BAR_Y + 1;
        f.cw = g.aw - 4;
        f.ch = BAR_H - 2;
        while(i0 < b->addr_len && cells < first) {
            size_t k = ut_utf8_decode((const unsigned char *)b->addr + i0, b->addr_len - i0, &cp);
            cells += wf_count(b->addr + i0, k);
            i0 += k;
        }
        if(focused && b->chosen && b->addr_len)
            wp_fill(&f, g.ax + 4, BAR_Y + 3, (int)wf_count(b->addr + i0, b->addr_len - i0) * WF_W, WF_H, C_PICK);
        wp_text(&f, g.ax + 4, BAR_Y + 3, b->addr + i0, b->addr_len - i0, 1, 0, C_INK);
        if(focused && !b->chosen)
            wp_fill(&f, g.ax + 4 + (int)(wf_count(b->addr, b->caret) - first) * WF_W - 1, BAR_Y + 3, 2, WF_H, C_INK);
    }
    /* the page */
    if(p) {
        struct wp_target pg = t;
        ensure_layout(b, p);
        scroll_to(b, p->scroll);
        pg.x = 0;
        pg.y = g.cy;
        pg.cw = g.cw;
        pg.ch = g.ch;
        wp_paint(&p->lay, &p->doc, &b->theme, &pg, 0, p->scroll, b->hover);
    } else wp_fill(&t, 0, g.cy, g.cw, g.ch, C_SHEET);
    /* the scroll bar */
    wp_fill(&t, g.sx, g.cy, WB_SCROLLBAR, g.ch, C_BAND);
    wp_fill(&t, g.sx, g.cy, 1, g.ch, C_LINE);
    thumb(b, &ty, &th);
    if(p && max_scroll(b, p)) wp_fill(&t, g.sx + 2, ty + 1, WB_SCROLLBAR - 3, th - 2, b->drag ? C_TEAL : C_MUTED);
    /* the status bar */
    wp_fill(&t, 0, b->h - WB_STATUS, b->w, WB_STATUS, C_BAND);
    wp_fill(&t, 0, b->h - WB_STATUS, b->w, 1, C_LINE);
    right[0] = 0;
    if(p && b->npages && !p->file) snprintf(right, sizeof(right), "%s", p->verified ? "HTTPS verified " : "HTTP: not encrypted");
    if(p && b->npages && p->verified) snprintf(right + strlen(right), sizeof(right) - strlen(right), "%s", p->tls);
    if(p && b->npages && p->file) snprintf(right, sizeof(right), "%s", "File");
    {
        struct wp_target s = t;
        int rw = (int)wf_count(right, strlen(right)) * WF_W;
        s.x = 9;
        s.y = b->h - WB_STATUS + 1;
        s.cw = b->w - 9 - rw - 24 - 16;
        s.ch = WB_STATUS - 1;
        if(s.cw > 0) wp_text(&s, 9, b->h - WB_STATUS + 3, b->status, strlen(b->status), 1, 0, C_INK);
        wp_text(&t, b->w - 24 - rw, b->h - WB_STATUS + 3, right, strlen(right), 1, 0, C_MUTED);
    }
}
