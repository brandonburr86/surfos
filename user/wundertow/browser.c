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
#include "find.h"
#include "font.h"
#include "form.h"
#include "image.h"
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
#define C_FOUND  0xffd34d                       /* what find found: a highlight over the text */
#define C_MISS   0xf3c9c0                       /* find's field, when the word is not there */
#define LIST_ROWS 10                            /* a select's list shows this many options at most */
#define LIST_ROW  (WF_H + 2)

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
    "<li><b>Ctrl+R</b>: load the page again"
    "<li><b>Ctrl+F</b>: find a word on the page; <b>Enter</b>, the next place; <b>Shift+Enter</b>, the one before"
    "<li><b>Tab</b>, <b>Shift+Tab</b>: from link to link; <b>Enter</b> follows one</ul>"
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
    return (size_t)snprintf(out, cap, "file:///%s", text + sizeof(prefix) - 1) < cap;
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

/**** where this window has been ****/

/* FNV-1a in 64 bits: a place is its hash, so that no address stays in memory longer than its page does */
static unsigned long long place_hash(const char *s) {
    unsigned long long h = 14695981039346656037ull;
    for(; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h ? h : 1;                               /* 0: an empty slot */
}

static void visit(struct wb *b, const char *url) {
    unsigned long long h = place_hash(url);
    unsigned int i = (unsigned int)h % WB_VISITED;
    while(b->seen[i]) {
        if(b->seen[i] == h) return;
        i = (i + 1) % WB_VISITED;
    }
    if(b->seen_count >= WB_VISITED / 2) {           /* full: it starts again, as such a list may */
        memset(b->seen, 0, sizeof(b->seen));
        b->seen_count = 0;
        i = (unsigned int)h % WB_VISITED;
    }
    b->seen[i] = h;
    b->seen_count++;
}

static int visited(const struct wb *b, const char *url) {
    unsigned long long h = place_hash(url);
    unsigned int i = (unsigned int)h % WB_VISITED;
    while(b->seen[i]) {
        if(b->seen[i] == h) return 1;
        i = (i + 1) % WB_VISITED;
    }
    return 0;
}

/* The address a link is known by: as the address field would show it */
static void link_address(const struct wb_page *p, const struct ut_link *l, char *out, size_t cap) {
    if(!(p->file && file_url(l->url, out, cap))) snprintf(out, cap, "%s", l->url);
}

/* Which of the page's links lead to where the window has been */
static void mark_visited(struct wb *b, struct wb_page *p) {
    char url[WB_ADDR_MAX];
    size_t i;
    if(!p->doc.link_count) return;
    if(!p->doc.visited) {
        p->doc.visited = calloc(p->doc.link_count, 1);
        if(!p->doc.visited) return;                 /* no colours of their own: nothing else depends on them */
        p->bytes += p->doc.link_count;
    }
    for(i = 0; i < p->doc.link_count; i++) {
        link_address(p, &p->doc.links[i], url, sizeof(url));
        p->doc.visited[i] = visited(b, url) ? 1 : 0;
    }
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
    b->edited = 0;
}

/* A load ended while someone may be typing in the field: their keys are not taken from them */
static void addr_after_load(struct wb *b, const char *s) {
    if(!b->edited) set_addr(b, s);
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
    size_t off, top = 0;
    int into = 0, place;
    geom(b, &g);
    if(p->lay_w == g.cw) return;
    place = p->scroll > 0 && wl_place(&p->lay, p->scroll, &top, &into);     /* what is being read stays where it is */
    wl_free(&p->lay);
    p->lay_w = 0;
    if(wl_layout(&p->doc, g.cw, &p->lay)) {
        status(b, "%s", "out of memory laying out the page");
        return;
    }
    p->lay_w = g.cw;
    if(place) p->scroll = wl_place_y(&p->lay, top, into);
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
    if(!p) return 0;
    ensure_layout(b, p);                            /* a new width moves the place, before anything counts from it */
    return scroll_to(b, p->scroll + dy);
}

/* A row of the page, h high, into the view: as little as it takes, with some of what is around it */
static void show_rows(struct wb *b, struct wb_page *p, int y, int h) {
    struct geom g;
    geom(b, &g);
    if(y < p->scroll) scroll_to(b, y - g.ch / 4);
    else if(y + h > p->scroll + g.ch) scroll_to(b, y + h - g.ch + g.ch / 4);
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

static void page_shown(struct wb *b, struct wb_page *p);

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
    size_t n;
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
    for(n = 0; n < p->doc.image_count; n++) {
        struct wd_picture *m = &p->doc.pics[n];
        if(m->state != WP_WAIT) continue;
        if(p->pic_bytes >= WB_PAGE_IMAGES) {        /* its frame stays */
            m->state = WP_FAILED;
            continue;
        }
        memset(out, 0, sizeof(*out));
        if(may_load(p, p->doc.images[n], out->url, sizeof(out->url))) {
            m->state = WP_REFUSED;
            continue;
        }
        out->kind = WB_LOAD_IMAGE;
        out->page = p->serial;
        out->item = (int)n;
        out->budget = WB_PAGE_IMAGES - p->pic_bytes;
        m->state = WP_RUNNING;
        b->inflight++;
        return 1;
    }
    return 0;
}

/* The page shown has what it was loading stopped: it may ask again */
static void resources_stop(struct wb *b) {
    struct wb_page *p = wb_current(b);
    size_t n;
    int i;
    if(!b->inflight) return;
    b->cancel |= WB_CANCEL_RESOURCES;
    b->inflight = 0;
    if(!p) return;
    for(i = 0; i < p->ntexts; i++) if(p->texts[i].state == WP_RUNNING) p->texts[i].state = WP_WAIT;
    for(n = 0; n < p->doc.image_count; n++) if(p->doc.pics[n].state == WP_RUNNING) p->doc.pics[n].state = WP_WAIT;
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

static void trim_history(struct wb *b);

static struct wd_picture *picture_of(struct wb *b, unsigned int page, int item) {
    struct wb_page *p = wb_current(b);
    if(!b->npages || p->serial != page || item < 0 || (size_t)item >= p->doc.image_count) return NULL;
    return p->doc.pics[item].state == WP_RUNNING ? &p->doc.pics[item] : NULL;
}

/* A picture whose own size is known now: when the page has to change for that, it is laid out again */
static void picture_size(struct wb_page *p, struct wd_picture *m, int w, int h) {
    if(m->w == w && m->h == h) return;
    m->w = w;
    m->h = h;
    p->lay_w = 0;
}

void wb_image(struct wb *b, unsigned int page, int item, int w, int h, int pw, int ph, unsigned int *px) {
    struct wd_picture *m = picture_of(b, page, item);
    struct wb_page *p = wb_current(b);
    size_t bytes = (size_t)pw * ph * sizeof(*px);
    if(!m || bytes > WB_PAGE_IMAGES - p->pic_bytes) {
        free(px);
        if(m) wb_image_failed(b, page, item, w, h);
        return;
    }
    m->px = px;
    m->pw = pw;
    m->ph = ph;
    m->state = WP_DONE;
    p->pic_bytes += bytes;
    p->bytes += bytes;
    picture_size(p, m, w, h);
    b->inflight--;
    trim_history(b);
}

void wb_image_failed(struct wb *b, unsigned int page, int item, int w, int h) {
    struct wd_picture *m = picture_of(b, page, item);
    if(!m) return;
    m->state = WP_FAILED;
    if(w > 0 && h > 0) picture_size(wb_current(b), m, w, h);
    b->inflight--;
}

/* Pixels made to the size pictures in view are shown at, where that is smaller than what is kept: an average of what
   each stands for looks better than the nearest, which is what paint.c does else, and is too slow to do each paint */
static void prepare_pictures(struct wb_page *p, int top, int bottom) {
    size_t i;
    for(i = 0; i < p->lay.count; i++) {
        const struct wl_item *it = &p->lay.items[i];
        struct wd_picture *m;
        size_t bytes;
        unsigned int *s;
        if(it->kind != WI_IMAGE || it->image < 0 || it->y >= bottom || it->y + it->h <= top) continue;
        m = &p->doc.pics[it->image];
        if(!m->px || it->w <= 0 || it->h <= 0 || (it->w >= m->pw && it->h >= m->ph)) continue;
        if(m->scaled && m->sw == it->w && m->sh == it->h) continue;
        if(m->scaled) {
            bytes = (size_t)m->sw * m->sh * sizeof(*m->scaled);
            free(m->scaled);
            m->scaled = NULL;
            p->pic_bytes -= bytes;
            p->bytes -= bytes;
        }
        bytes = (size_t)it->w * it->h * sizeof(*s);
        if(bytes > WB_PAGE_IMAGES - p->pic_bytes) continue;         /* paint.c's nearest will do */
        s = malloc(bytes);
        if(!s) continue;
        wimg_scale(m->px, m->pw, m->ph, s, it->w, it->h);
        m->scaled = s;
        m->sw = it->w;
        m->sh = it->h;
        p->pic_bytes += bytes;
        p->bytes += bytes;
    }
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

/* Over WB_HISTORY_BYTES: the oldest pages go, then those ahead of the one shown, never that one */
static void trim_history(struct wb *b) {
    while(b->npages > 1 && history_bytes(b) > WB_HISTORY_BYTES) {
        if(b->cur > 0) {
            wb_page_free(b->page[0]);
            memmove(b->page, b->page + 1, (size_t)(b->npages - 1) * sizeof(b->page[0]));
            b->cur--;
        } else wb_page_free(b->page[b->npages - 1]);
        b->npages--;
    }
}

void wb_loaded(struct wb *b, struct wb_page *p, int reload) {
    int i;
    resources_stop(b);                              /* what the page before was loading */
    if(b->npages) fm_forget(&b->page[b->cur]->doc); /* the history keeps no password */
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
        trim_history(b);
    }
    visit(b, p->url);
    if(b->req.kind == WB_LOAD_PAGE) visit(b, b->req.url);   /* where it was asked for, before any redirect */
    page_shown(b, p);
    addr_after_load(b, p->url);
    loaded_status(b, p);
}

void wb_failed(struct wb *b, const char *url, const char *why) {
    b->loading = 0;
    b->running = 0;
    addr_after_load(b, url);
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
            visit(b, url);
            mark_visited(b, p);
            status(b, "%s", "Done");
        } else status(b, "There is no place named \"%s\" on this page", hash + 1);
        return;
    }
    request(b, url, 0);
}

static void back_forward(struct wb *b, int d) {
    if(!b->npages || b->cur + d < 0 || b->cur + d >= b->npages) return;
    if(b->running) b->cancel |= WB_CANCEL_PAGE;     /* a page on its way goes: as in any browser, and a reload's */
    b->running = b->loading = b->want = 0;          /* place is the page it was asked on */
    resources_stop(b);
    fm_forget(&b->page[b->cur]->doc);
    b->cur += d;
    b->hover = b->pressed = -1;
    page_shown(b, b->page[b->cur]);
    set_addr(b, b->page[b->cur]->url);
    loaded_status(b, b->page[b->cur]);
}

/* A page comes into view: nothing of find or of the keyboard's focus was about it */
static void page_shown(struct wb *b, struct wb_page *p) {
    b->tab_link = -1;
    b->field = -1;
    b->press_field = -1;
    b->list = -1;
    b->find_open = 0;
    b->find_hit = 0;
    if(b->focus == WB_FOCUS_FIND || b->focus == WB_FOCUS_FIELD) b->focus = WB_FOCUS_PAGE;
    mark_visited(b, p);
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
    b->tab_link = b->field = b->press_field = b->list = -1;
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
            b->edited = 1;
            return 1;
        }
        if(!b->caret) return 0;
        b->edited = 1;
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
            b->edited = 1;
            return 1;
        }
        if(b->caret < b->addr_len) {
            size_t k = b->caret + 1;
            b->edited = 1;
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
    b->edited = 1;
    if(b->addr_len + 1 >= sizeof(b->addr)) return 0;
    memmove(b->addr + b->caret + 1, b->addr + b->caret, b->addr_len - b->caret + 1);
    b->addr[b->caret++] = (char)key;
    b->addr_len++;
    return 1;
}

static int in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

/**** find, and links by keyboard ****/

/* Where the text in view starts */
static size_t top_offset(struct wb *b) {
    struct wb_page *p = wb_current(b);
    size_t off = 0;
    int into;
    if(p && p->scroll > 0 && !wl_place(&p->lay, p->scroll, &off, &into)) off = 0;
    return off;
}

/* The word from from on (back: before it), round the page: its place in view, and what the field says */
static void find_go(struct wb *b, size_t from, int back) {
    struct wb_page *p = wb_current(b);
    b->find_hit = 0;
    b->find_index = b->find_total = 0;
    if(!b->find_len || !p) return;
    if(!fi_search(&p->doc, b->find, b->find_len, from, back, &b->find_at) &&
       !fi_search(&p->doc, b->find, b->find_len, back ? (size_t)-1 : 0, back, &b->find_at)) return;
    b->find_hit = 1;
    b->find_total = fi_count(&p->doc, b->find, b->find_len, b->find_at, &b->find_index);
    ensure_layout(b, p);
    {
        size_t i;
        for(i = 0; i < p->lay.count; i++) {
            const struct wl_item *it = &p->lay.items[i];
            if(it->kind == WI_TEXT && it->off + it->len > b->find_at) {
                show_rows(b, p, it->y, it->h);
                break;
            }
        }
    }
}

static int find_key(struct wb *b, unsigned int key) {
    switch(key) {
    case 13: case 10:
        find_go(b, b->find_hit ? b->find_at + 1 : 0, 0);
        return 1;
    case WB_KEY_SHIFT_ENTER:
        find_go(b, b->find_hit ? b->find_at : (size_t)-1, 1);
        return 1;
    case 27:
        b->find_open = 0;
        b->find_hit = 0;
        b->focus = WB_FOCUS_PAGE;
        return 1;
    case 127: case 8:
        if(!b->find_len) return 0;
        b->find_len--;                              /* (what is typed is ASCII) */
        break;
    default:
        if(key < 0x20 || key > 0x7e || b->find_len + 1 >= sizeof(b->find)) return 0;
        b->find[b->find_len++] = (char)key;
    }
    b->find[b->find_len] = 0;
    find_go(b, b->find_hit ? b->find_at : top_offset(b), 0);    /* as it is typed: from where it is */
    return 1;
}

/* What Tab stops at: a link, or a control that can be used */
static int field_item(const struct wb_page *p, const struct wl_item *it) {
    return it->kind == WI_FIELD && it->control >= 0 && !p->doc.controls[it->control].disabled;
}

static int has_focus(const struct wb *b, const struct wb_page *p, const struct wl_item *it) {
    if(b->tab_link >= 0) return it->link == b->tab_link;
    return b->field >= 0 && field_item(p, it) && it->control == b->field;
}

/* A control gets the keyboard's keys */
static void focus_field(struct wb *b, int control) {
    b->field = control;
    b->tab_link = -1;
    b->focus = WB_FOCUS_FIELD;
}

/* Tab: the next stop in the page's order from the one that has the focus, or from what is in view */
static void tab_move(struct wb *b, int back) {
    struct wb_page *p = wb_current(b);
    struct geom g;
    size_t n, i, start = 0;
    long k;
    int found = 0;
    if(!p) return;
    geom(b, &g);
    ensure_layout(b, p);
    n = p->lay.count;
    if(!n) return;
    if(b->tab_link >= 0 || b->field >= 0) {         /* past all of its items, or before them */
        for(i = 0; i < n; i++)
            if(has_focus(b, p, &p->lay.items[i])) {
                start = i;
                found = 1;
                if(back) break;
            }
    } else {
        for(i = 0; i < n; i++) {
            const struct wl_item *it = &p->lay.items[i];
            if(it->y + it->h > p->scroll) break;
        }
        start = i;
    }
    for(k = 0; k < (long)n; k++) {
        size_t at;
        const struct wl_item *it;
        if(back) at = (start + 2 * n - (size_t)k - 1) % n;
        else at = (start + (size_t)k + (found ? 1 : 0)) % n;
        it = &p->lay.items[at];
        if(it->link < 0 && !field_item(p, it)) continue;
        show_rows(b, p, it->y, it->h);
        b->list = -1;
        if(field_item(p, it)) {
            focus_field(b, it->control);
            if(!b->loading && b->npages) loaded_status(b, p);
            return;
        }
        b->tab_link = it->link;
        b->field = -1;
        b->focus = WB_FOCUS_PAGE;
        {
            char shown[WB_ADDR_MAX];
            link_address(p, &p->doc.links[it->link], shown, sizeof(shown));
            status(b, "%s", shown);
        }
        return;
    }
}

/* A person submits: control is what did it (-1: Enter with no button). The form goes as a GET, or the status bar says why
   not. Only a key or a click comes here. */
static void submit(struct wb *b, int form, int control, int x, int y) {
    struct wb_page *p = wb_current(b);
    char *url = malloc(WB_ADDR_MAX), *to = malloc(WB_ADDR_MAX);
    struct ut_url *u = malloc(sizeof(*u));
    char err[UT_ERR_MAX];
    const char *why;
    if(!url || !to || !u) why = "out of memory: nothing was sent";
    else why = fm_submit(&p->doc, form, control, x, y, url, WB_ADDR_MAX);
    if(!why && strlen(url) > UT_URL_MAX) why = "This form's fields do not fit in an address: nothing was sent";
    if(!why && may_load(p, url, to, WB_ADDR_MAX)) why = !strncmp(url, "http:", 5) ?
        "This form would go unencrypted from an encrypted page: nothing was sent" :
        "This form's address is not one this browser can use: nothing was sent";
    if(!why && strncmp(to, "file:", 5) && ut_url_parse(to, u, err, sizeof(err)))       /* wb_go()'s rule for an address */
        why = "This form's address is not one this browser can use: nothing was sent";
    if(why) status(b, "%s", why);
    else {
        b->field = -1;
        b->focus = WB_FOCUS_PAGE;
        request(b, strncmp(to, "file:", 5) ? u->text : to, 0);
    }
    free(url);
    free(to);
    free(u);
}

/* Enter in a field: its form's first button, or by itself where the standard lets it */
static void implicit_submit(struct wb *b, const struct wd_control *c) {
    struct wb_page *p = wb_current(b);
    int button;
    if(c->form < 0) return;
    button = fm_default_button(&p->doc, c->form);
    if(button != -2) submit(b, c->form, button, 0, 0);
}

/* A press and a release on a control, or Space or Enter on one */
static void activate(struct wb *b, int control, int x, int y) {
    struct wb_page *p = wb_current(b);
    struct wd_control *c = &p->doc.controls[control];
    if(c->disabled) return;
    switch(c->kind) {
    case WF_CHECKBOX: case WF_RADIO:
        fm_click(&p->doc, control);
        break;
    case WF_BUTTON:
        if(c->role == WC_RESET) {
            if(c->form >= 0) fm_reset(&p->doc, c->form);
        } else if(c->role == WC_SUBMIT) submit(b, c->form, control, x, y);
        break;
    case WF_SELECT:
        if(c->noptions) {
            int at = fm_chosen(&p->doc, control);
            b->list = control;
            b->list_at = at < 0 ? 0 : at;
            b->list_top = b->list_at >= LIST_ROWS ? b->list_at - LIST_ROWS + 1 : 0;
        }
        break;
    case WF_FILE:
        status(b, "%s", "Choosing a file is not something this browser does");
        break;
    }
}

/* The keys of an open select's list */
static int list_key(struct wb *b, unsigned int key) {
    struct wb_page *p = wb_current(b);
    struct wd_control *c = &p->doc.controls[b->list];
    int n = (int)c->noptions;
    switch(key) {
    case WB_KEY_UP:
        if(b->list_at > 0) b->list_at--;
        break;
    case WB_KEY_DOWN:
        if(b->list_at < n - 1) b->list_at++;
        break;
    case ' ':
        if(c->multiple) {
            fm_pick(&p->doc, b->list, (unsigned int)b->list_at);
            return 1;
        }
        /* fall through */
    case 13: case 10:
        if(!c->multiple) fm_pick(&p->doc, b->list, (unsigned int)b->list_at);
        b->list = -1;
        return 1;
    case 27: case 9:
        b->list = -1;
        return 1;
    default:
        return 0;
    }
    if(b->list_at < b->list_top) b->list_top = b->list_at;
    if(b->list_at >= b->list_top + LIST_ROWS) b->list_top = b->list_at - LIST_ROWS + 1;
    return 1;
}

/* The keys of the control with the focus */
static int field_key(struct wb *b, unsigned int key) {
    struct wb_page *p = wb_current(b);
    struct wd_control *c = &p->doc.controls[b->field];
    int typing = c->kind == WF_TEXT || c->kind == WF_PASSWORD || c->kind == WF_AREA;
    if(b->list >= 0) return list_key(b, key);
    switch(key) {
    case 9:
        tab_move(b, 0);
        return 1;
    case WB_KEY_BACKTAB:
        tab_move(b, 1);
        return 1;
    case 27:
        b->field = -1;
        b->focus = WB_FOCUS_PAGE;
        return 1;
    case 6: case 12: case 18:                       /* Ctrl+F, Ctrl+L, Ctrl+R: the window's */
        b->focus = WB_FOCUS_PAGE;
        b->field = -1;
        return wb_key(b, key);
    case 13: case 10:
        if(c->kind == WF_AREA) break;
        if(c->kind == WF_BUTTON) activate(b, b->field, 0, 0);
        else implicit_submit(b, c);
        return 1;
    case ' ':
        if(typing) break;
        activate(b, b->field, 0, 0);
        return 1;
    case WB_KEY_UP: case WB_KEY_DOWN:
        if(c->kind == WF_SELECT && !c->multiple && c->noptions) {
            int at = fm_chosen(&p->doc, b->field) + (key == WB_KEY_DOWN ? 1 : -1);
            while(at >= 0 && at < (int)c->noptions && p->doc.options[c->first_option + (unsigned int)at].disabled)
                at += key == WB_KEY_DOWN ? 1 : -1;
            if(at >= 0 && at < (int)c->noptions) fm_pick(&p->doc, b->field, (unsigned int)at);
            return 1;
        }
        break;
    }
    if(!typing || c->disabled) return 0;
    if(!fm_key(c, key)) return 0;
    {
        long i = wl_control_item(&p->lay, b->field);
        if(i >= 0) show_rows(b, p, p->lay.items[i].y, p->lay.items[i].h);
    }
    return 1;
}

/* Where a select's open list is on the window; 0 when its field is not laid out */
static int list_place(struct wb *b, struct wb_page *p, int *x, int *y, int *w, int *rows) {
    struct geom g;
    const struct wd_control *c = &p->doc.controls[b->list];
    long i = wl_control_item(&p->lay, b->list);
    const struct wl_item *it;
    unsigned int k;
    int most = 0;
    if(i < 0) return 0;
    geom(b, &g);
    it = &p->lay.items[i];
    for(k = 0; k < c->noptions; k++) {
        const char *s = p->doc.options[c->first_option + k].label;
        int cells = (int)wf_count(s, strlen(s));
        if(cells > most) most = cells;
    }
    *w = most * WF_W + 16;
    if(*w < it->w) *w = it->w;
    if(*w > g.cw - 2) *w = g.cw - 2;
    *rows = c->noptions > LIST_ROWS ? LIST_ROWS : (int)c->noptions;
    *x = it->x + *w > g.cw ? g.cw - *w : it->x;
    *y = g.cy + it->y - p->scroll + it->h;
    if(*y + *rows * LIST_ROW + 2 > g.cy + g.ch) *y = g.cy + it->y - p->scroll - *rows * LIST_ROW - 2;
    if(*y < g.cy) *y = g.cy;
    return 1;
}

/* The pointer goes down on the open list: 1, an option taken; else it closes */
static void list_press(struct wb *b, struct wb_page *p, int x, int y) {
    int lx, ly, lw, rows;
    if(list_place(b, p, &lx, &ly, &lw, &rows) && in(x, y, lx, ly, lw, rows * LIST_ROW + 2)) {
        int at = b->list_top + (y - ly - 1) / LIST_ROW;
        const struct wd_control *c = &p->doc.controls[b->list];
        if(at >= 0 && at < (int)c->noptions) {
            fm_pick(&p->doc, b->list, (unsigned int)at);
            b->list_at = at;
            if(c->multiple) return;
        }
    }
    b->list = -1;
}

/* The pointer goes down on a control: the keys are its own, and in a text its caret goes there */
static void field_press(struct wb *b, struct wb_page *p, int control, int px, int py) {
    struct wd_control *c = &p->doc.controls[control];
    long i = wl_control_item(&p->lay, control);
    focus_field(b, control);
    if(i < 0) return;
    if(c->kind == WF_TEXT || c->kind == WF_PASSWORD)
        c->caret = fm_byte_at(c, (px - p->lay.items[i].x - 4 + WF_W / 2) / WF_W);
    else if(c->kind == WF_AREA)
        c->caret = fm_at(c, c->top + (py - p->lay.items[i].y - 4) / WF_H, (px - p->lay.items[i].x - 4 + WF_W / 2) / WF_W);
}

int wb_key(struct wb *b, unsigned int key) {
    struct geom g;
    if(b->focus == WB_FOCUS_ADDR) return addr_key(b, key);
    if(b->focus == WB_FOCUS_FIND) return find_key(b, key);
    if(b->focus == WB_FOCUS_FIELD && b->field >= 0) return field_key(b, key);
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
    case 27:                                        /* Escape: closes find, or stops what is loading */
        if(b->find_open) {
            b->find_open = 0;
            b->find_hit = 0;
            return 1;
        }
        wb_stop(b);
        return 1;
    case 6:                                         /* Ctrl+F */
        b->find_open = 1;
        b->focus = WB_FOCUS_FIND;
        find_go(b, b->find_hit ? b->find_at : top_offset(b), 0);
        return 1;
    case 9:
        tab_move(b, 0);
        return 1;
    case WB_KEY_BACKTAB:
        tab_move(b, 1);
        return 1;
    case 13: case 10:
        if(b->tab_link < 0) return 0;
        {
            int l = b->tab_link;
            follow(b, l);
        }
        return 1;
    }
    return 0;
}


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
            link_address(p, &p->doc.links[link], shown, sizeof(shown));
            status(b, "%s", shown);
        } else if(!b->loading && p && b->npages) loaded_status(b, p);
        redraw = 1;
    }
    if(press) {
        b->pressed = -1;
        b->button = 0;
        if(b->list >= 0 && p) {
            list_press(b, p, x, y);
            return 1;
        }
        for(i = 0; i < 4; i++)
            if(in(x, y, g.bx[i], BAR_Y, g.bw[i], BAR_H) && enabled(b, i)) {
                b->button = i + 1;
                b->stop_held = wb_busy(b);
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
            int control = p ? wl_control_at(&p->lay, x, y - g.cy + p->scroll) : -1;
            if(control >= 0 && !p->doc.controls[control].disabled) {
                b->press_field = control;
                field_press(b, p, control, x, y - g.cy + p->scroll);
                return 1;
            }
            b->focus = WB_FOCUS_PAGE;
            b->pressed = link;
            b->tab_link = -1;
            b->field = -1;
            return 1;
        }
    }
    if(release) {
        int n = b->button;
        b->button = 0;
        if(n && in(x, y, g.bx[n - 1], BAR_Y, g.bw[n - 1], BAR_H) && enabled(b, n - 1)) {
            if(n == 1) back_forward(b, -1);
            else if(n == 2) back_forward(b, 1);
            else if(n == 3 && b->stop_held) wb_stop(b);    /* what it said when pressed, if the load has ended since */
            else if(n == 3) reload(b);
            else wb_go(b, b->addr);
            return 1;
        }
        if(b->press_field >= 0) {
            int control = b->press_field;
            b->press_field = -1;
            if(p && in(x, y, 0, g.cy, g.cw, g.ch) && wl_control_at(&p->lay, x, y - g.cy + p->scroll) == control) {
                long at = wl_control_item(&p->lay, control);
                activate(b, control, x - p->lay.items[at].x, y - g.cy + p->scroll - p->lay.items[at].y);
            }
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

int wb_wheel(struct wb *b, int rows) {
    if(b->list >= 0) {
        const struct wd_control *c = &wb_current(b)->doc.controls[b->list];
        int most = (int)c->noptions - LIST_ROWS, top = b->list_top + rows;
        b->list_top = top > most ? (most > 0 ? most : 0) : top < 0 ? 0 : top;
        return 1;
    }
    return scroll_by(b, rows * WHEEL_ROW);
}

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

/* The word found, over what is painted */
static void mark_found(struct wb *b, struct wb_page *p, const struct wp_target *pg) {
    size_t at = b->find_at, end = at + b->find_len, i;
    for(i = 0; i < p->lay.count; i++) {
        const struct wl_item *it = &p->lay.items[i];
        size_t from, to;
        int x0, x1, y;
        if(it->kind != WI_TEXT || it->off >= end || it->off + it->len <= at) continue;
        from = at > it->off ? at : it->off;
        to = end < it->off + it->len ? end : it->off + it->len;
        x0 = it->x + wf_width(p->doc.text + it->off, from - it->off, it->scale, it->style);
        x1 = it->x + wf_width(p->doc.text + it->off, to - it->off, it->scale, it->style);
        y = pg->y + it->y - p->scroll;
        wp_tint(pg, x0, y, x1 - x0, it->h, C_FOUND);
    }
}

/* A frame round each piece of the link with the keyboard's focus */
static void mark_focus(struct wb *b, struct wb_page *p, const struct wp_target *pg) {
    size_t i;
    for(i = 0; i < p->lay.count; i++) {
        const struct wl_item *it = &p->lay.items[i];
        if(it->link == b->tab_link) wp_frame(pg, it->x - 1, pg->y + it->y - p->scroll - 1, it->w + 2, it->h + 2, C_INK);
    }
}

/* The field with the keyboard's focus: a frame, and in a text its caret, with its text kept in view */
static void mark_field(struct wb *b, struct wb_page *p, const struct wp_target *pg) {
    struct wd_control *c = &p->doc.controls[b->field];
    long i = wl_control_item(&p->lay, b->field);
    const struct wl_item *it;
    int x, y;
    if(i < 0) return;
    it = &p->lay.items[i];
    x = it->x;
    y = pg->y + it->y - p->scroll;
    wp_frame(pg, x - 1, y - 1, it->w + 2, it->h + 2, C_TEAL);
    if(b->focus != WB_FOCUS_FIELD) return;
    if(c->kind == WF_AREA) {
        int lines = (it->h - 8) / WF_H, line, col;
        fm_where(c, c->caret, &line, &col);
        if(line < c->top) c->top = line;
        if(line >= c->top + lines) c->top = line - lines + 1;
        wp_fill(pg, x + 4 + col * WF_W - 1, y + 4 + (line - c->top) * WF_H, 2, WF_H, C_INK);
    } else if(c->kind == WF_TEXT || c->kind == WF_PASSWORD) {
        size_t k, cells = 0;
        fm_keep_in_view(c, (it->w - 8) / WF_W);
        for(k = c->first; k < c->caret; k++) if(((unsigned char)c->value[k] & 0xc0) != 0x80) cells++;
        wp_fill(pg, x + 4 + (int)cells * WF_W - 1, y + (it->h - WF_H) / 2, 2, WF_H, C_INK);
    }
}

/* A select's open list, over the page */
static void paint_list(struct wb *b, struct wb_page *p, const struct wp_target *t) {
    const struct wd_control *c = &p->doc.controls[b->list];
    int x, y, w, rows, r;
    if(!list_place(b, p, &x, &y, &w, &rows)) return;
    wp_fill(t, x, y, w, rows * LIST_ROW + 2, C_SHEET);
    wp_frame(t, x, y, w, rows * LIST_ROW + 2, C_INK);
    for(r = 0; r < rows && (unsigned int)(b->list_top + r) < c->noptions; r++) {
        const struct wd_option *o = &p->doc.options[c->first_option + (unsigned int)(b->list_top + r)];
        struct wp_target f = *t;
        int ry = y + 1 + r * LIST_ROW;
        if(b->list_top + r == b->list_at) wp_fill(t, x + 1, ry, w - 2, LIST_ROW, C_PICK);
        if(c->multiple && o->on) wp_fill(t, x + 3, ry + 5, 6, 6, C_TEAL);
        f.x = x + 1;
        f.y = ry;
        f.cw = w - 2;
        f.ch = LIST_ROW;
        wp_text(&f, x + 12, ry + 1, o->label, strlen(o->label), 1, 0, o->disabled ? C_MUTED : C_INK);
    }
}

/* find's field, in the status bar */
static void paint_find(struct wb *b, const struct wp_target *t) {
    static const char label[] = "Find:";
    char count[32];
    int y = b->h - WB_STATUS, fx = 9 + (int)sizeof(label) * WF_W, fw = b->w - fx - 120, cx;
    wp_fill(t, 0, y, b->w, WB_STATUS, C_BAND);
    wp_fill(t, 0, y, b->w, 1, C_LINE);
    wp_text(t, 9, y + 3, label, sizeof(label) - 1, 1, 0, C_INK);
    if(fw < 40) fw = 40;
    wp_fill(t, fx, y + 2, fw, WB_STATUS - 3, b->find_len && !b->find_hit ? C_MISS : C_SHEET);
    wp_frame(t, fx, y + 2, fw, WB_STATUS - 3, b->focus == WB_FOCUS_FIND ? C_TEAL : C_LINE);
    {
        struct wp_target f = *t;
        f.x = fx + 2;
        f.cw = fw - 4;
        f.y = y + 3;
        f.ch = WB_STATUS - 5;
        cx = wp_text(&f, fx + 4, y + 3, b->find, b->find_len, 1, 0, C_INK);
        if(b->focus == WB_FOCUS_FIND) wp_fill(&f, cx, y + 3, 2, WF_H, C_INK);
    }
    if(b->find_hit) snprintf(count, sizeof(count), "%d of %d", b->find_index, b->find_total);
    else snprintf(count, sizeof(count), "%s", b->find_len ? "Not found" : "");
    wp_text(t, b->w - 12 - (int)strlen(count) * WF_W, y + 3, count, strlen(count), 1, 0, C_MUTED);
}

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
        prepare_pictures(p, p->scroll, p->scroll + g.ch);
        pg.x = 0;
        pg.y = g.cy;
        pg.cw = g.cw;
        pg.ch = g.ch;
        wp_paint(&p->lay, &p->doc, &b->theme, &pg, 0, p->scroll, b->hover);
        if(b->find_open && b->find_hit) mark_found(b, p, &pg);
        if(b->tab_link >= 0) mark_focus(b, p, &pg);
        if(b->field >= 0) mark_field(b, p, &pg);
        if(b->list >= 0) paint_list(b, p, &pg);
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
    if(b->find_open) paint_find(b, &t);
    else {
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
