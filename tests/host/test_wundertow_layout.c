/*
SurfOS Undertow's window: tests of the layout and the painting
----------------------
File: test_wundertow_layout.c  Date: 10/25/26
----------------------
user/wundertow/layout.c and paint.c (with doc.c, font.c, html_scan.c, url.c, util.c, and the console's font
and code page 437 from driver/) on the host:

- the fixtures in tests/fixtures/wundertow/: NAME.layout, what wl_dump() must make of NAME.html at 640 pixels;
- pages made here: lines broken at spaces and inside a word too wide, margins that meet, indentation and
  markers, centring, headings at twice the size, pre kept and broken at the page's edge, tables' columns, links
  under the pointer, fragment targets' places; and the painting: the background, text in its colours and
  styles, underlined links, clipping to the viewport, scrolling;
- every page of tests/fixtures/{wundertow,undertow}/ at widths from 64 to 1300 pixels: no item off the page's
  left, every item within the page's size, the painting inside its viewport.

"test_wundertow_layout --render FILE OUT.ppm [WIDTH [URL]]" paints a whole page as an image (P6).
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "undertow.h"
#include "url.h"
#include "http.h"
#include "doc.h"
#include "font.h"
#include "layout.h"
#include "paint.h"

#define PAGE_URL "https://example.test/dir/page.html"

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(ok) return;
    failures++;
    printf("FAIL %s\n", what);
}

unsigned int uptime(void) { return 0; }
time_t ut_sim_time(time_t *t) { if(t) *t = 0; return 0; }

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *s;
    long n;
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s = malloc((size_t)n + 1);
    if(!s || fread(s, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(s);
        return NULL;
    }
    s[n] = 0;
    fclose(f);
    if(len) *len = (size_t)n;
    return s;
}

static int document(const char *url, const char *type, const char *body, size_t len, struct wd_document *d) {
    struct ut_response r;
    char err[UT_ERR_MAX];
    int rc;
    memset(&r, 0, sizeof(r));
    if(ut_url_parse(url, &r.url, err, sizeof(err))) return -1;
    r.body = (unsigned char *)malloc(len + 1);
    if(!r.body) return -1;
    memcpy(r.body, body, len);
    r.body[len] = 0;
    r.len = len;
    r.status = 200;
    snprintf(r.content_type, sizeof(r.content_type), "%s", type);
    rc = wd_build(&r, d, err, sizeof(err));
    free(r.body);
    return rc;
}

/**** an image to paint into ****/

struct image { unsigned int *px; int w, h; };

static void image_new(struct image *im, int w, int h, unsigned int fill) {
    int i;
    im->w = w;
    im->h = h;
    im->px = malloc((size_t)w * h * sizeof(unsigned int));
    if(!im->px) exit(2);
    for(i = 0; i < w * h; i++) im->px[i] = fill;
}

static unsigned int pixel(const struct image *im, int x, int y) { return im->px[y * im->w + x]; }

static struct wp_target target(struct image *im, int x, int y, int w, int h) {
    struct wp_target t;
    t.pixels = im->px;
    t.stride = im->w;
    t.w = im->w;
    t.h = im->h;
    t.x = x;
    t.y = y;
    t.cw = w;
    t.ch = h;
    return t;
}

static int write_ppm(const char *path, const struct image *im) {
    FILE *f = fopen(path, "wb");
    int i;
    if(!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    for(i = 0; i < im->w * im->h; i++) {
        unsigned char rgb[3] = { (unsigned char)(im->px[i] >> 16), (unsigned char)(im->px[i] >> 8), (unsigned char)im->px[i] };
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f);
}

static int render(int argc, char **argv) {
    size_t len;
    char *body = read_file(argv[2], &len);
    int width = argc > 4 ? atoi(argv[4]) : 640, h;
    struct wd_document d;
    struct wl_layout l;
    struct wp_theme th;
    struct image im;
    struct wp_target t;
    if(!body || document(argc > 5 ? argv[5] : PAGE_URL, "text/html", body, len, &d)) {
        fprintf(stderr, "cannot read or build %s\n", argv[2]);
        return 2;
    }
    if(wl_layout(&d, width, &l)) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    h = l.height > 6000 ? 6000 : l.height;
    image_new(&im, l.width, h, 0);
    t = target(&im, 0, 0, im.w, im.h);
    wp_theme_default(&th);
    wp_paint(&l, &d, &th, &t, 0, 0, -1);
    if(write_ppm(argv[3], &im)) {
        fprintf(stderr, "cannot write %s\n", argv[3]);
        return 2;
    }
    printf("%s: %dx%d, %u items\n", argv[3], im.w, im.h, (unsigned int)l.count);
    free(im.px);
    wl_free(&l);
    wd_free(&d);
    free(body);
    return 0;
}

/**** fixtures ****/

static const char *const fixtures[] = {
    "basics", "styles", "lists", "tables", "links", "pre", "forms", "colors", "hidden"
};

static void meta_url(const char *dir, const char *name, char *url, size_t cap) {
    char path[600], *meta, *u;
    snprintf(path, sizeof(path), "%s/%s.meta", dir, name);
    snprintf(url, cap, "%s", PAGE_URL);
    meta = read_file(path, NULL);
    if(meta && (u = strstr(meta, "url: ")) != NULL) snprintf(url, cap, "%.*s", (int)strcspn(u + 5, "\n"), u + 5);
    free(meta);
}

static void fixture(const char *dir, const char *name) {
    char path[600], url[UT_URL_MAX + 1], msg[1200], *html, *want, *got;
    size_t len;
    struct wd_document d;
    struct wl_layout l;
    snprintf(path, sizeof(path), "%s/%s.html", dir, name);
    html = read_file(path, &len);
    snprintf(path, sizeof(path), "%s/%s.layout", dir, name);
    want = read_file(path, NULL);
    snprintf(msg, sizeof(msg), "fixture %s: its .html and .layout in %s", name, dir);
    check(html && want, msg);
    meta_url(dir, name, url, sizeof(url));
    if(html && want && !document(url, "text/html", html, len, &d)) {
        if(!wl_layout(&d, 640, &l)) {
            got = wl_dump(&l, &d);
            snprintf(msg, sizeof(msg), "fixture %s: its layout at 640 pixels", name);
            check(got && !strcmp(got, want), msg);
            if(got && strcmp(got, want)) {
                const char *g = got, *w = want;
                while(*g && *g == *w) { g++; w++; }
                while(g > got && g[-1] != '\n') g--;
                while(w > want && w[-1] != '\n') w--;
                printf("  got:  %.*s\n  want: %.*s\n", (int)strcspn(g, "\n"), g, (int)strcspn(w, "\n"), w);
            }
            free(got);
            wl_free(&l);
        }
        wd_free(&d);
    }
    free(html);
    free(want);
}

static int dump_layout(int argc, char **argv) {
    size_t len;
    char *html = read_file(argv[2], &len), *s;
    struct wd_document d;
    struct wl_layout l;
    if(!html || document(argc > 4 ? argv[4] : PAGE_URL, "text/html", html, len, &d)) return 2;
    if(wl_layout(&d, argc > 3 ? atoi(argv[3]) : 640, &l)) return 1;
    s = wl_dump(&l, &d);
    fputs(s ? s : "(no memory)\n", stdout);
    free(s);
    wl_free(&l);
    wd_free(&d);
    free(html);
    return 0;
}

/**** pages made here ****/

static struct wd_document pd;
static struct wl_layout pl;
static int pl_ok;

static void page(const char *html, int width) {
    wl_free(&pl);
    wd_free(&pd);
    pl_ok = !document(PAGE_URL, "text/html", html, strlen(html), &pd) && !wl_layout(&pd, width, &pl);
}

/* the n-th item of kind (from 0), NULL when there is none */
static const struct wl_item *nth(int kind, int n) {
    size_t i;
    for(i = 0; i < pl.count; i++) if(pl.items[i].kind == kind && !n--) return &pl.items[i];
    return NULL;
}

/* the text item holding s */
static const struct wl_item *text_item(const char *s) {
    size_t i, n = strlen(s);
    for(i = 0; i < pl.count; i++) {
        const struct wl_item *it = &pl.items[i];
        const char *p;
        if(it->kind != WI_TEXT) continue;
        for(p = pd.text + it->off; p + n <= pd.text + it->off + it->len; p++) if(!memcmp(p, s, n)) return it;
    }
    return NULL;
}

/* the first fill of color, NULL when there is none */
static const struct wl_item *fill_of(unsigned int color) {
    size_t i;
    for(i = 0; i < pl.count; i++) if(pl.items[i].kind == WI_FILL && pl.items[i].color == color) return &pl.items[i];
    return NULL;
}

/* the frames whose top is at y: how many, each n high */
static int frames_at(int y, int h) {
    size_t i;
    int n = 0;
    for(i = 0; i < pl.count; i++) if(pl.items[i].kind == WI_FRAME && pl.items[i].y == y) n += pl.items[i].h == h ? 1 : -1000;
    return n;
}

/* every item within the page, none to its left or above it */
static int inside_page(const struct wl_layout *l) {
    size_t i;
    for(i = 0; i < l->count; i++) {
        const struct wl_item *it = &l->items[i];
        if(it->x < 0 || it->y < 0 || it->w < 0 || it->h < 0 || it->x + it->w > l->width || it->y + it->h > l->height)
            return 0;
    }
    return 1;
}

#define GUARD 0xdeadbe

/* paint into the middle of a larger image whose edges are GUARD: 1 when they stayed so */
static int paints_inside(const struct wl_layout *l, const struct wd_document *d, int w, int h, int sx, int sy) {
    struct image im;
    struct wp_target t;
    struct wp_theme th;
    int x, y, ok = 1;
    image_new(&im, w + 20, h + 20, GUARD);
    t = target(&im, 10, 10, w, h);
    wp_theme_default(&th);
    wp_paint(l, d, &th, &t, sx, sy, 0);
    for(y = 0; y < im.h && ok; y++)
        for(x = 0; x < im.w; x++)
            if((x < 10 || y < 10 || x >= 10 + w || y >= 10 + h) && pixel(&im, x, y) != GUARD) {
                ok = 0;
                break;
            }
    free(im.px);
    return ok;
}

static void made_pages(void) {
    const struct wl_item *a, *b, *c;
    struct image im;
    struct wp_target t;
    struct wp_theme th;
    char msg[200];
    int x, y, found;
    size_t off;

    /* "aaa bbb" fits, "ccc" goes to the next line */
    page("<p>aaa bbb ccc", 8 + 72 + 8);                         /* 9 cells */
    a = text_item("aaa");
    c = text_item("ccc");
    check(pl_ok && a && c && a->len == 7 && a->w == 56 && c->x == a->x && c->y == a->y + 16, "lines: broken at a space");

    page("<p>abcdefghijklmnopqrstuvwxyz", 8 + 80 + 8);           /* a word of 26 in lines of 10 */
    found = 0;
    for(off = 0; off < pl.count; off++) if(pl.items[off].kind == WI_TEXT && pl.items[off].w > 80) found = 1;
    check(pl_ok && nth(WI_TEXT, 2) && nth(WI_TEXT, 2)->len == 6 && !found, "lines: a word too wide, broken at the edge");

    page("<p>one</p><p>two</p><h1>three</h1>", 640);
    a = text_item("one");
    b = text_item("two");
    c = text_item("three");
    check(pl_ok && a && b && c && a->y == 8 + 16 && b->y == a->y + 16 + 16 && c->y == b->y + 16 + 16, "margins: the larger of two");
    check(pl_ok && c && c->scale == WF_S32 && c->h == 32 && c->w == 5 * 16 && (c->style & WS_BOLD), "h1: the 16x32 font, bold");

    page("<center>abc</center><p align=right>xyz", 640);
    a = text_item("abc");
    b = text_item("xyz");
    check(pl_ok && a && a->x == 8 + (624 - 24) / 2, "centred");
    check(pl_ok && b && b->x == 8 + 624 - 24, "right-aligned");

    page("<ul><li>item</ul><ol start=9><li>nine<li>ten</ol>", 640);
    a = text_item("item");
    b = nth(WI_MARK, 0);
    c = nth(WI_MARK, 2);
    check(pl_ok && a && b && a->x == 8 + 40 && b->x + b->w == a->x - 8 && b->y == a->y, "a list's marker: in its indentation, on its line");
    check(pl_ok && c && c->len == 3 && !memcmp(pl.marks + c->off, "10.", 3) && c->x + c->w == 48 - 8, "numbers: right-aligned");

    page("<pre>0123456789 0123456789</pre>", 8 + 80 + 8);
    a = nth(WI_TEXT, 0);
    b = nth(WI_TEXT, 1);
    check(pl_ok && a && b && a->len == 10 && b->len == 10 && b->y == a->y + 16, "pre: kept, broken at the page's edge");

    page("<table border=1><tr><td>a<td>bbbbbbbbbb</table>", 640);
    a = text_item("a");
    b = text_item("bbbbbbbbbb");
    c = nth(WI_FRAME, 0);
    check(pl_ok && a && b && c && b->x > a->x + 8 && c->w == 8 + 2 * 2, "tables: columns as wide as their content");

    /* 10/25/26 (the review): a cell over two columns, taller than the row before it, whose cells' fills and frames
       keep that row's height (the columns a span covers were left without a fill or a frame of their own) */
    page("<table border=1><tr><td bgcolor=#ff0000>a<td bgcolor=#0000ff>b"
         "<tr><td colspan=2 bgcolor=#00ff00>c<br>d<br>e</table>", 640);
    a = fill_of(0xff0000);
    b = fill_of(0x0000ff);
    c = fill_of(0x00ff00);
    check(pl_ok && a && b && c && a->h > 16 && b->h == a->h && b->y == a->y && c->y == a->y + a->h + 2 &&
          c->h == a->h + 2 * 16 && c->w == b->x + b->w - a->x && frames_at(a->y, a->h) == 2 && frames_at(c->y, c->h) == 1,
          "tables: a cell over two columns; every row's fills and frames as high as their row");

    page("<p>x <a href=y>link</a> z", 640);
    a = text_item("link");
    check(pl_ok && a && wl_link_at(&pl, a->x + 4, a->y + 8) == 0 && wl_link_at(&pl, a->x - 4, a->y + 8) == -1 &&
          wl_link_at(&pl, a->x + 4, a->y + 40) == -1, "links: found under the pointer");

    page("<p>one<p>two<p id=target>three", 640);
    check(pl_ok && !wd_anchor_offset(&pd, "target", &off) && wl_offset_y(&pl, off) == text_item("three")->y,
          "a fragment target: its line's place");

    /* painting */
    page("<body bgcolor=#ffffff text=#000000 link=#0000ff><p>Hi <a href=x>there</a>", 200);
    image_new(&im, 200, pl.height, 0);
    t = target(&im, 0, 0, 200, pl.height);
    wp_theme_default(&th);
    wp_paint(&pl, &pd, &th, &t, 0, 0, -1);
    a = text_item("Hi");
    b = text_item("there");
    found = 0;
    for(y = a->y; y < a->y + 16; y++) for(x = a->x; x < a->x + 16; x++) found += pixel(&im, x, y) == 0x000000;
    check(found > 10 && pixel(&im, 2, 2) == 0xffffff, "painting: the page's background and its text colour");
    found = 0;
    for(x = b->x; x < b->x + b->w; x++) found += pixel(&im, x, b->y + WF_UNDER) == 0x0000ff;
    check(found == b->w, "painting: a link underlined in its colour");
    free(im.px);

    page("<p>a<p>b<p>c<p>d<p>e<p>f<p>g<p>h", 300);
    check(pl_ok && paints_inside(&pl, &pd, 120, 50, 0, 40) && paints_inside(&pl, &pd, 300, 2000, -30, -30),
          "painting: only inside the viewport");
    image_new(&im, 300, 40, 0);
    t = target(&im, 0, 0, 300, 40);
    a = text_item("e");
    wp_paint(&pl, &pd, &th, &t, 0, a->y, -1);
    found = 0;
    for(y = 0; y < 16; y++) for(x = 8; x < 16; x++) found += pixel(&im, x, y) == th.fg;
    snprintf(msg, sizeof(msg), "painting: scrolled, the line at the top (%d lit)", found);
    check(found > 5, msg);
    free(im.px);

    /* real sizes */
    page("<h1>a</h1><h2>b</h2><h3>c</h3><h4>d</h4><h5>e</h5><h6>f</h6><big>g</big><small>h</small>", 640);
    a = text_item("b");
    b = text_item("c");
    check(pl_ok && a && b && a->scale == WF_S24 && a->h == 24 && a->w == 12 && b->scale == WF_S16 && b->h == 16 &&
          text_item("f")->scale == WF_S16 && text_item("g")->scale == WF_S16, "h2 in 12x24, the others in 8x16");
    page("<span style=font-size:14px>a</span><span style=font-size:20px>b</span><span style=font-size:21px>c</span>"
         "<span style=font-size:28px>d</span><span style=font-size:29px>e</span><span style=font-size:200px>f</span>", 640);
    check(pl_ok && text_item("a")->scale == WF_S16 && text_item("b")->scale == WF_S16 && text_item("c")->scale == WF_S24 &&
          text_item("d")->scale == WF_S24 && text_item("e")->scale == WF_S32 && text_item("f")->scale == WF_S32,
          "font-size: the nearest size there is");
    page("<p>a <span style=font-size:32px>b</span> c", 640);
    a = text_item("a");
    b = text_item("b");
    check(pl_ok && a && b && a->y + wf_ascent(WF_S16) == b->y + wf_ascent(WF_S32), "sizes on a line share a baseline");
    page("x<sup>a</sup><h1>y<sub>b</sub></h1>", 640);
    a = text_item("a");
    b = text_item("x");
    check(pl_ok && a && b && a->y == b->y - 4 && text_item("y")->y + 8 == text_item("b")->y, "sub and sup by a quarter of the size");
    wd_base = 24;
    page("<p>a<p>b<ul><li>c</ul><h1>d</h1><p><small>e</small>", 640);
    a = text_item("a");
    b = text_item("b");
    c = nth(WI_MARK, 0);
    check(pl_ok && a && b && c && a->scale == WF_S24 && a->h == 24 && b->y == a->y + 24 + 24 && c->h == 24 && c->scale == WF_S24 &&
          text_item("d")->scale == WF_S32 && text_item("e")->scale == WF_S16, "a base of 24: text, margins, markers and sizes from it");
    wd_base = 32;
    page("<p>a<p>b", 640);
    a = text_item("a");
    b = text_item("b");
    check(pl_ok && a && b && a->scale == WF_S32 && b->y == a->y + 32 + 32, "a base of 32");
    wd_base = 16;

    /* every font, every style, painted within its viewport */
    page("<h1><b>a</b><i>b</i><u>c</u><s>d</s></h1><h2><b>e</b><i>f</i></h2><p><b><i>g</i></b>", 300);
    check(pl_ok && paints_inside(&pl, &pd, 300, 300, 0, 0) && paints_inside(&pl, &pd, 40, 40, 4, 4) &&
          paints_inside(&pl, &pd, 100, 20, -3, 9), "sizes and styles: only inside the viewport");
    page("<h1>A</h1>", 300);
    image_new(&im, 300, pl.height, 0);
    t = target(&im, 0, 0, 300, pl.height);
    wp_paint(&pl, &pd, &th, &t, 0, 0, -1);
    a = text_item("A");
    found = 0;
    for(y = 0; y < 32; y++)
        for(x = 0; x < 16; x++) {
            unsigned int bits = wf_row(WF_S32, 'A', y);
            bits |= bits >> 1;
            bits |= bits >> 1;
            if((pixel(&im, a->x + x, a->y + y) == th.fg) != !!(bits & (0x8000u >> x))) found++;
        }
    check(!found, "a heading's glyph is the 16x32 font's, bold by a smear of two");
    free(im.px);

    /* deep and narrow */
    {
        char *deep = malloc(200 * 12 + 16);
        int k;
        if(!deep) exit(2);
        deep[0] = 0;
        for(k = 0; k < 200; k++) strcat(deep, "<blockquote>");
        strcat(deep, "deep");
        page(deep, 300);
        check(pl_ok && text_item("de") && inside_page(&pl), "deep quotes: laid out within the page");
        free(deep);
    }
    wl_free(&pl);
    wd_free(&pd);
}

/* every page of both browsers' fixtures at widths from 64 to 1300 */
static void every_page(const char *top) {
    static const char *const undertow[] = {
        "paragraphs", "headings", "lists", "pre", "tables", "images", "forms", "comments", "hidden",
        "attributes", "mismatched", "links", "anchors", "base", "entities", "utf8", "unknown-charset", "sniff",
        "xhtml", "article"
    };
    static const int widths[] = { 64, 100, 200, 640, 1300 };
    char path[600], msg[300];
    size_t i, k, len;
    for(i = 0; i < sizeof(undertow) / sizeof(undertow[0]) + sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        int own = i >= sizeof(undertow) / sizeof(undertow[0]);
        const char *name = own ? fixtures[i - sizeof(undertow) / sizeof(undertow[0])] : undertow[i];
        char *html;
        struct wd_document d;
        snprintf(path, sizeof(path), "%s/%s/%s.html", top, own ? "wundertow" : "undertow", name);
        html = read_file(path, &len);
        if(!html) continue;
        if(document(PAGE_URL, "text/html", html, len, &d)) {
            free(html);
            continue;
        }
        for(k = 0; k < sizeof(widths) / sizeof(widths[0]); k++) {
            struct wl_layout l;
            int ok = !wl_layout(&d, widths[k], &l);
            snprintf(msg, sizeof(msg), "%s at %d pixels: laid out within the page, painted within the viewport", name, widths[k]);
            check(ok && inside_page(&l) && paints_inside(&l, &d, widths[k], 300, 0, 0), msg);
            if(ok) wl_free(&l);
        }
        wd_free(&d);
        free(html);
    }
}

int main(int argc, char **argv) {
    const char *top = argc > 1 && argv[1][0] != '-' ? argv[1] : "tests/fixtures";
    char dir[512];
    size_t i;
    if(argc > 3 && !strcmp(argv[1], "--render")) return render(argc, argv);
    if(argc > 2 && !strcmp(argv[1], "--dump")) return dump_layout(argc, argv);
    snprintf(dir, sizeof(dir), "%s/wundertow", top);
    for(i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) fixture(dir, fixtures[i]);
    made_pages();
    every_page(top);
    if(failures) {
        printf("WUNDERTOW LAYOUT TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW LAYOUT TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
