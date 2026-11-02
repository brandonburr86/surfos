/*
SurfOS Undertow's window: tests of the browser
----------------------
File: test_wundertow_browser.c  Date: 10/25/26
----------------------
user/wundertow/browser.c on the host, with the rest of the window's modules: the window's events as a person
gives them, and the loads it asks for carried out here with pages made in the test. An address typed, a link
followed (on the page, to a place on the same page, to a file from a file page, one that is not http), back
and forward, the history's limit, scrolling by keys, the wheel and the scroll bar, a new size, the address
field's editing, a load that fails, and the chrome painted. 11/1/26: Stop, a load that takes another's place, what a
page loads for itself and the rules of that; pictures; find, links by keyboard, visited links.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "undertow.h"
#include "alloc.h"
#include "url.h"
#include "http.h"
#include "doc.h"
#include "font.h"
#include "layout.h"
#include "paint.h"
#include "browser.h"

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(ok) return;
    failures++;
    printf("FAIL %s\n", what);
}

unsigned int uptime(void) { return 0; }
time_t ut_sim_time(time_t *t) { if(t) *t = 0; return 0; }

#define W 640
#define H 400

static struct wb b;

/* the page a request gets: its body as given, at url (for file: the base the program gives a file page) */
static struct wb_page *page_at(const char *url, const char *html, int file) {
    struct ut_response r;
    char err[UT_ERR_MAX], path[UT_URL_MAX + 1];
    struct wb_page *p;
    memset(&r, 0, sizeof(r));
    if(file) {
        wb_file_path(url, path, sizeof(path));
        snprintf(err, sizeof(err), "https://" WB_FILE_HOST "%s", path);
        ut_url_parse(err, &r.url, err, sizeof(err));
    } else ut_url_parse(url, &r.url, err, sizeof(err));
    r.body = (unsigned char *)strdup(html);
    r.len = strlen(html);
    r.status = 200;
    r.verified = !strncmp(url, "https:", 6);
    snprintf(r.tls, sizeof(r.tls), "%s", r.verified ? "TLSv1.3" : "");
    snprintf(r.content_type, sizeof(r.content_type), "text/html");
    p = wb_page_new(&r, file, err, sizeof(err));
    free(r.body);
    return p;
}

/* carry out the request that waits with this page: 1 when there was one */
static int serve(const char *html) {
    struct wb_request q;
    if(!wb_take(&b, &q)) return 0;
    wb_loaded(&b, page_at(q.url, html, !strncmp(q.url, "file:", 5)), q.reload);
    return 1;
}

static void type(const char *s) { while(*s) wb_key(&b, (unsigned char)*s++); }

/* the middle of link n's first item on the screen (the page's area starts at WB_TOOLBAR) */
static int link_spot(int n, int *x, int *y) {
    struct wb_page *p = wb_current(&b);
    size_t i;
    unsigned int px[1];
    (void)px;
    for(i = 0; p && i < p->lay.count; i++)
        if(p->lay.items[i].link == n) {
            *x = p->lay.items[i].x + p->lay.items[i].w / 2;
            *y = p->lay.items[i].y + p->lay.items[i].h / 2 + WB_TOOLBAR - p->scroll;
            return 1;
        }
    return 0;
}

static void click(int x, int y) {
    wb_pointer(&b, x, y, 0);
    wb_pointer(&b, x, y, 1);
    wb_pointer(&b, x, y, 0);
}

static unsigned int screen[W * H];

static void paint(void) { wb_paint(&b, screen, W); }

static int has_request(const char *url) {
    struct wb_request q;
    int ok = b.want && !strcmp(b.req.url, url);
    if(ok) wb_take(&b, &q);
    return ok;
}

static unsigned int busy_bar[W * 31];
static int has_color(unsigned int c, int y0, int y1);

/* Pixels as a helper's answer brings them: the page owns them from then on */
static unsigned int *pixels_of(int w, int h, unsigned int c) {
    unsigned int *px = ut_test_malloc((size_t)w * h * sizeof(*px));
    int i;
    for(i = 0; i < w * h; i++) px[i] = c;
    return px;
}

/* Answer the picture the program was told to load */
static void answer(const struct wb_request *q, int w, int h, unsigned int c) {
    wb_image(&b, q->page, q->item, w, h, w, h, pixels_of(w, h, c));
}

/* Stage 2.4 */
static void pictures(void) {
    struct wb_request q, r;
    struct wb_page *p, *old;
    struct wd_picture *m;
    static char tall[20000];
    int i, s0, top;
    long before;

    wb_cancel(&b);
    /* a few at a time, in the order of the page, after the page */
    wb_go(&b, "https://pics.test/p");
    serve("<title>P</title><img src=1.png alt=one><img src=2.png><img src=3.png><img src=4.png>"
          "<img src=http://plain.test/5.png alt=five><img src=/6.png>"
          "<img src=https://" WB_FILE_HOST "/etc/7.png alt=seven><img src=mailto:a@b alt=eight>");
    p = wb_current(&b);
    check(p->doc.image_count == 8, "the images of a page");
    for(i = 0; i < WB_RESOURCES; i++) {
        char url[64];
        snprintf(url, sizeof(url), "https://pics.test/%d.png", i + 1);
        check(wb_take(&b, &q) && q.kind == WB_LOAD_IMAGE && q.item == i && q.page == p->serial && !strcmp(q.url, url) && q.budget == WB_PAGE_IMAGES, "the next picture");
    }
    check(!wb_take(&b, &q) && wb_busy(&b), "a few at a time: Stop in the toolbar meanwhile");
    wb_image_failed(&b, p->serial, 1, 0, 0);
    check(wb_take(&b, &q) && q.item == 3 && p->doc.pics[1].state == WP_FAILED, "one that failed: the next");
    answer(&q, 2, 2, 0x123456);
    check(p->doc.pics[3].state == WP_DONE && p->doc.pics[3].px && p->doc.pics[3].w == 2, "one that came");
    /* http from https is passed over: not loaded, as Undertow refuses HTTPS to HTTP; a file likewise */
    check(wb_take(&b, &q) && q.item == 5 && !strcmp(q.url, "https://pics.test/6.png"), "an http picture of an https page is not asked for, nor a file");
    wb_image_failed(&b, p->serial, 0, 0, 0);
    wb_image_failed(&b, p->serial, 2, 0, 0);
    answer(&q, 1, 1, 0);
    check(!wb_busy(&b) && !wb_take(&b, &q), "all answered");
    check(p->doc.pics[4].state == WP_REFUSED && p->doc.pics[6].state == WP_REFUSED && p->doc.pics[7].state == WP_REFUSED, "they are refused");
    check(p->bytes >= p->doc.bytes + 8 + 4 && p->pic_bytes == 16 + 4, "its pixels are counted in the page's bytes");
    paint();

    /* a second answer for one that has its own: dropped, and nothing changes */
    wb_go(&b, "https://pics.test/twice");
    serve("<img src=a.png>");
    p = wb_current(&b);
    wb_take(&b, &q);
    answer(&q, 2, 2, 0x111111);
    before = ut_alloc_live;
    wb_image(&b, q.page, q.item, 3, 3, 3, 3, pixels_of(3, 3, 0x222222));
    check(ut_alloc_live == before && p->doc.pics[0].px[0] == 0x111111 && p->doc.pics[0].w == 2 && p->pic_bytes == 16, "pixels again: the first stay");
    wb_image_failed(&b, q.page, q.item, 9, 9);
    check(p->doc.pics[0].state == WP_DONE && p->doc.pics[0].w == 2 && !b.inflight, "a failure after them: nothing");

    /* the page left: an answer late for it is nobody's, and its pixels go */
    wb_go(&b, "https://pics.test/q");
    serve("<img src=a.png alt=A><img src=b.png alt=B>");
    p = wb_current(&b);
    wb_take(&b, &q);
    wb_take(&b, &r);
    before = ut_alloc_live;
    wb_image(&b, p->serial + 5, q.item, 2, 2, 2, 2, pixels_of(2, 2, 1));
    check(ut_alloc_live == before && wb_busy(&b), "an answer for another page: dropped");
    wb_image(&b, p->serial, 99, 2, 2, 2, 2, pixels_of(2, 2, 1));
    wb_image(&b, p->serial, -1, 2, 2, 2, 2, pixels_of(2, 2, 1));
    check(ut_alloc_live == before && wb_busy(&b), "for a picture it does not have: dropped");
    wb_image_failed(&b, p->serial, 99, 0, 0);
    check(b.inflight == 2, "a failure of one it does not have");
    /* Back: what it was loading stops, and starts again on the way back */
    wb_key(&b, 127);
    check((wb_cancel(&b) & WB_CANCEL_RESOURCES) && !wb_busy(&b) && p->doc.pics[0].state == WP_WAIT, "left: its pictures wait");
    wb_image(&b, p->serial, 0, 2, 2, 2, 2, pixels_of(2, 2, 1));
    check(!p->doc.pics[0].px, "an answer after that is dropped");
    check(!wb_take(&b, &q), "and the page shown wants nothing");
    wb_pointer(&b, 7 + 82 + 4 + 40, 15, 0);
    wb_pointer(&b, 7 + 82 + 4 + 40, 15, 1);
    wb_pointer(&b, 7 + 82 + 4 + 40, 15, 0);
    check(wb_current(&b) == p && wb_take(&b, &q) && q.item == 0 && wb_take(&b, &r) && r.item == 1, "back to it: it asks again");
    /* Stop: the frames stay */
    wb_stop(&b);
    check((wb_cancel(&b) & WB_CANCEL_RESOURCES) && !wb_busy(&b) && !wb_take(&b, &q) && !p->doc.pics[0].px, "Stop: no more of them");

    /* the page is laid out again, and what is being read stays */
    {
        char *e = tall;
        e += snprintf(e, 64, "<img src=top.png alt=top><p>Under it");
        for(i = 0; i < 100; i++) e += snprintf(e, 32, "<p>Line %d", i);
    }
    wb_go(&b, "https://pics.test/tall");
    serve(tall);
    p = wb_current(&b);
    paint();
    wb_key(&b, 0x107);
    wb_key(&b, 0x107);
    s0 = p->scroll;
    check(s0 > 300, "well down the page");
    wb_take(&b, &q);
    answer(&q, 100, 300, 0x445566);
    paint();
    check(p->scroll == s0 + 300 - 24, "a picture above grew by 276 pixels: the view with it");
    wb_key(&b, 0x104);
    check(p->scroll == 0, "the top");
    wb_go(&b, "https://pics.test/tall");
    serve(tall);
    p = wb_current(&b);
    paint();
    wb_take(&b, &q);
    answer(&q, 100, 300, 0x445566);
    paint();
    check(p->scroll == 0, "read from the top: the page grows below");
    top = p->lay.height;
    check(top > 300, "with its picture's room");
    wb_resize(&b, 400, H);
    paint();
    check(p->lay_w == 400 - WB_SCROLLBAR, "a new width with a picture");
    wb_resize(&b, W, H);
    paint();
    /* a picture larger than it is kept: its own size counts, its pixels are fewer */
    wb_go(&b, "https://pics.test/large");
    serve("<img src=l.png>");
    p = wb_current(&b);
    wb_take(&b, &q);
    wb_image(&b, q.page, q.item, 400, 200, 2, 1, pixels_of(2, 1, 0x777777));
    paint();
    check(p->doc.pics[0].w == 400 && p->lay.height >= 200, "laid out at its own size");
    /* shown smaller than it is kept: pixels made to its size, once, and counted; a new size, again */
    wb_go(&b, "https://pics.test/small");
    serve("<img src=l.png width=10 height=10>");
    p = wb_current(&b);
    wb_take(&b, &q);
    answer(&q, 40, 40, 0x808080);
    paint();
    m = &p->doc.pics[0];
    check(m->scaled && m->sw == 10 && m->sh == 10 && m->scaled[0] == 0x808080, "made to the size it is shown at");
    check(p->pic_bytes == (40 * 40 + 10 * 10) * 4, "and counted");
    paint();
    check(p->pic_bytes == (40 * 40 + 10 * 10) * 4, "once");
    {
        const unsigned int *made = m->scaled;
        long calls = ut_alloc_calls;
        paint();
        check(m->scaled == made && ut_alloc_calls == calls, "and painting again makes nothing");
    }
    /* out of view it is not made */
    {
        char *e = tall;
        e += snprintf(e, 64, "<p>Above");
        for(i = 0; i < 60; i++) e += snprintf(e, 32, "<p>Line %d", i);
        e += snprintf(e, 64, "<p><img src=l.png width=10 height=10>");
    }
    wb_go(&b, "https://pics.test/below");
    serve(tall);
    p = wb_current(&b);
    wb_take(&b, &q);
    answer(&q, 40, 40, 0x808080);
    paint();
    m = &p->doc.pics[0];
    check(!m->scaled && p->pic_bytes == 40 * 40 * 4, "below the view: not yet");
    wb_key(&b, 0x105);
    paint();
    check(m->scaled != NULL, "in view: made");
    /* a page's limit met exactly: nothing more is asked for */
    wb_go(&b, "https://pics.test/exact");
    serve("<img src=1.png alt=a><img src=2.png alt=b>");
    p = wb_current(&b);
    wb_take(&b, &q);
    wb_image(&b, q.page, q.item, 2048, 4096, 2048, 4096, pixels_of(2048, 4096, 1));
    check(p->pic_bytes == WB_PAGE_IMAGES, "as many pixels as a page may have");
    check(!wb_take(&b, &q) && p->doc.pics[1].state == WP_FAILED && !wb_busy(&b), "the next is not asked for: its frame stays");
    /* the place kept, where what is at the top of the view is a field */
    {
        char *e = tall;
        e += snprintf(e, 64, "<img src=top.png alt=top>");
        for(i = 0; i < 60; i++) e += snprintf(e, 40, "<p><input name=a value=f%d>", i);
    }
    wb_go(&b, "https://pics.test/fields");
    serve(tall);
    p = wb_current(&b);
    paint();
    wb_key(&b, 0x107);
    wb_key(&b, 0x107);
    s0 = p->scroll;
    check(s0 > 300, "well down, among the fields");
    wb_take(&b, &q);
    answer(&q, 100, 300, 0x445566);
    paint();
    check(p->scroll > s0 + 200, "a picture above grew: the view with it");
    /* the history: with pages behind and ahead, the oldest goes first */
    wb_go(&b, "https://pics.test/h1");
    serve("<img src=1.png>");
    old = wb_current(&b);
    wb_take(&b, &q);
    answer(&q, 1000, 1000, 1);
    wb_go(&b, "https://pics.test/h2");
    serve("<img src=2.png>");
    p = wb_current(&b);
    wb_go(&b, "https://pics.test/h3");
    serve("<p>Ahead");
    wb_key(&b, 127);
    wb_cancel(&b);
    check(wb_current(&b) == p && b.cur >= 1 && b.cur + 1 < b.npages && b.page[b.cur - 1] == old, "a page behind, one ahead");
    wb_take(&b, &q);
    {
        int cur = b.cur;
        struct wb_page *ahead = b.page[b.cur + 1];
        answer(&q, 1250, 1000, 7);
        check(b.cur < cur && wb_current(&b) == p && b.page[0] != old, "over the history's bytes: what is behind goes first");
        check(b.page[b.cur + 1] == ahead, "and the page ahead stays while one is behind");
    }
    /* a narrower window, a smaller box: the pixels of the size before go, and their bytes */
    wb_go(&b, "https://pics.test/wide");
    serve("<img src=w.png>");
    p = wb_current(&b);
    wb_take(&b, &q);
    answer(&q, 600, 60, 0x808080);
    paint();
    m = &p->doc.pics[0];
    check(!m->scaled && p->pic_bytes == 600 * 60 * 4, "shown at its size: nothing made");
    wb_resize(&b, 500, H);
    paint();
    i = m->sw;
    check(m->scaled && i < 500 && p->pic_bytes == (size_t)(600 * 60 + m->sw * m->sh) * 4, "smaller: made");
    wb_resize(&b, 400, H);
    paint();
    check(m->scaled && m->sw < i && p->pic_bytes == (size_t)(600 * 60 + m->sw * m->sh) * 4, "smaller again: those go, and their bytes");
    check(p->bytes == p->doc.bytes + sizeof(*p) + p->pic_bytes, "and they go from the page's own count");
    wb_resize(&b, W, H);
    paint();
    check(m->scaled && p->pic_bytes == (size_t)(600 * 60 + m->sw * m->sh) * 4, "(its own size again: what was made stays)");
    /* no room left for such pixels: paint.c's nearest does */
    wb_go(&b, "https://pics.test/nobudget");
    serve("<img src=1.png width=600 height=1000><img src=2.png width=600 height=1000>");
    p = wb_current(&b);
    wb_take(&b, &q);
    wb_take(&b, &r);
    wb_image(&b, q.page, q.item, 2000, 2000, 2000, 2000, pixels_of(2000, 2000, 0x515151));
    wb_image(&b, r.page, r.item, 2000, 2000, 2000, 2000, pixels_of(2000, 2000, 0x515151));
    paint();
    check(p->pic_bytes == 32000000u && !p->doc.pics[0].scaled, "a page's pictures have its room: none is made");
    check(has_color(0x515151, WB_TOOLBAR, H - WB_STATUS), "and the picture shows");
    /* a shape from anywhere: a box stays within a page's sizes */
    wb_go(&b, "https://pics.test/shape");
    serve("<img src=s.png width=4000>");
    p = wb_current(&b);
    wb_take(&b, &q);
    wb_image(&b, q.page, q.item, 1, 4096, 1, 1, pixels_of(1, 1, 3));
    paint();
    check(p->lay.count == 1 && p->lay.items[0].h <= WD_SIZE_MAX, "a tall picture shown wide: its height is cut at the limit of one");
    /* failing with a size: the frame takes it */
    wb_go(&b, "https://pics.test/big");
    serve("<img src=l.png alt=l><p>After");
    p = wb_current(&b);
    wb_take(&b, &q);
    paint();
    wb_image_failed(&b, q.page, q.item, 300, 100);
    paint();
    check(p->doc.pics[0].w == 300 && !p->doc.pics[0].px && p->doc.pics[0].state == WP_FAILED && p->lay.height > 100, "too much for the page: its frame has its size");

    /* a page's limit: past it the frames stay, and nothing more is asked for */
    wb_go(&b, "https://pics.test/many");
    serve("<img src=1.png alt=a><img src=2.png alt=b><img src=3.png alt=c><img src=4.png alt=d>");
    p = wb_current(&b);
    wb_take(&b, &q);
    wb_take(&b, &r);
    wb_image(&b, q.page, q.item, 2000, 2000, 2000, 2000, pixels_of(2000, 2000, 1));
    wb_image(&b, r.page, r.item, 2000, 2000, 2000, 2000, pixels_of(2000, 2000, 2));
    check(p->pic_bytes == WB_PAGE_IMAGES - (WB_PAGE_IMAGES - 2000 * 2000 * 8), "two of 16 MB");
    check(b.npages == 1 && wb_current(&b) == p, "over the history's bytes: the older pages have gone, never this one");
    check(wb_take(&b, &q) && q.budget == WB_PAGE_IMAGES - 32000000u, "what is left of 32 MB goes with the next");
    wb_image(&b, q.page, q.item, 2000, 2000, 2000, 2000, pixels_of(2000, 2000, 3));
    check(p->doc.pics[2].state == WP_FAILED && !p->doc.pics[2].px && p->doc.pics[2].w == 2000, "more than that: no pixels, its frame");
    check(wb_take(&b, &q) && q.budget == WB_PAGE_IMAGES - 32000000u, "the small one may still come");
    wb_image(&b, q.page, q.item, 1, 1, 1, 1, pixels_of(1, 1, 3));
    paint();
    /* the history: pictures count; forward pages go too, never the one shown */
    wb_go(&b, "https://pics.test/one");
    serve("<img src=1.png>");
    old = wb_current(&b);
    wb_take(&b, &q);
    wb_go(&b, "https://pics.test/two");
    serve("<p>two");
    check(b.npages >= 2, "a page after it");
    wb_key(&b, 127);
    check(wb_current(&b) == old, "back");
    wb_cancel(&b);
    wb_take(&b, &q);
    while(b.cur > 0) {                              /* to the oldest it has */
        wb_key(&b, 127);
        wb_cancel(&b);
    }
    p = wb_current(&b);
    if(b.cur == 0 && p->doc.image_count && p->doc.pics[0].state != WP_DONE) {}
    wb_go(&b, "https://pics.test/only");
    serve("<img src=1.png><p>x");
    wb_key(&b, 127);
    wb_cancel(&b);
    wb_key(&b, 127);
    wb_cancel(&b);
    /* one more big picture for the first page: the page after goes, the first stays */
    wb_go(&b, "https://pics.test/first");
    serve("<img src=f.png>");
    wb_take(&b, &q);
    {
        struct wb_page *first = wb_current(&b);
        wb_go(&b, "https://pics.test/second");
        serve("<p>second");
        wb_key(&b, 127);
        wb_cancel(&b);
        while(b.cur > 0 && wb_current(&b) != first) wb_key(&b, 127), wb_cancel(&b);
        check(wb_current(&b) == first, "the first page");
        wb_take(&b, &q);
        if(b.cur == 0) {
            int n = b.npages;
            answer(&q, 2000, 1000, 9);
            check(b.npages == 1 && n == 2 && wb_current(&b) == first, "over the history's bytes with only a page ahead: that page goes");
        }
    }
    paint();
}

/* Stage 2.3: what the program is told to start and stop */
static void loads(void) {
    struct wb_request q;
    struct wb_page *p;
    unsigned int serial;
    int i;

    /* a load, Stop in the toolbar, and the page stays */
    wb_cancel(&b);                                  /* what the tests above left to say */
    wb_go(&b, "https://example.com/start");
    serve("<title>S</title><p>Stay");
    p = wb_current(&b);
    check(p->serial && !wb_busy(&b) && !wb_cancel(&b), "a page: nothing loading, nothing to stop");
    wb_go(&b, "https://never.test/");
    check(wb_busy(&b) && b.loading, "busy while a page loads");
    check(wb_take(&b, &q) && q.kind == WB_LOAD_PAGE && !strcmp(q.url, "https://never.test/"), "the program is told to load it");
    paint();
    click(7 + 82 + 4 + 106 + 4 + 8, 4 + 11);       /* Reload's place: after Back (82) and Forward (106) */
    check(!wb_busy(&b) && !strcmp(b.status, "Stopped") && (wb_cancel(&b) & WB_CANCEL_PAGE), "Stop in the toolbar: it ends the helper");
    check(!wb_cancel(&b), "said once");
    check(wb_current(&b) == p && !wb_take(&b, &q), "the page stays, and nothing starts");
    wb_go(&b, "https://never.test/");
    wb_take(&b, &q);
    wb_key(&b, 27);
    check(!wb_busy(&b) && (wb_cancel(&b) & WB_CANCEL_PAGE), "Escape in the page stops it too");
    p->halted = 0;
    snprintf(b.status, sizeof(b.status), "As it was");
    wb_key(&b, 27);
    check(!wb_cancel(&b) && !p->halted && !strcmp(b.status, "As it was"), "Escape with nothing loading stops nothing");

    /* Stop before the program took the load: nothing starts */
    wb_go(&b, "https://never.test/");
    wb_stop(&b);
    check(!wb_take(&b, &q) && !wb_cancel(&b), "Stopped before it started");
    p->halted = 0;

    /* the third button is what it said when it was pressed, and says what will happen */
    wb_go(&b, "https://slow.test/");
    wb_take(&b, &q);
    paint();
    memcpy(busy_bar, screen, sizeof(busy_bar));
    wb_pointer(&b, 7 + 82 + 4 + 106 + 4 + 8, 15, 0);
    wb_pointer(&b, 7 + 82 + 4 + 106 + 4 + 8, 15, 1);
    wb_failed(&b, "https://slow.test/", "over by itself");
    wb_pointer(&b, 7 + 82 + 4 + 106 + 4 + 8, 15, 0);
    check(!b.want && !wb_busy(&b), "pressed as Stop, let go after the load ended: no reload");
    paint();
    check(memcmp(busy_bar, screen, sizeof(busy_bar)) != 0, "the label is Stop while it loads, Reload after");
    wb_cancel(&b);

    /* Back and Forward end a page load, whichever it is, and history stays as it was */
    wb_go(&b, "https://example.com/before");
    serve("<title>B</title><p>Before");
    wb_go(&b, "https://example.com/after");
    serve("<title>A</title><p>After");
    wb_cancel(&b);
    {
        struct wb_page *before = b.page[b.cur - 1], *after = wb_current(&b);
        int n = b.npages;
        wb_key(&b, 18);
        wb_take(&b, &q);
        wb_key(&b, 127);
        check(wb_current(&b) == before && !wb_busy(&b) && (wb_cancel(&b) & WB_CANCEL_PAGE) && !wb_take(&b, &q), "Back during a reload: it ends");
        check(b.npages == n && b.page[b.cur + 1] == after, "and the page after is still there");
        wb_go(&b, "https://example.com/third");
        wb_take(&b, &q);
        wb_key(&b, 127);
        check(!wb_busy(&b) && (wb_cancel(&b) & WB_CANCEL_PAGE), "Back during a new page's load: it ends");
        wb_key(&b, 12);
        wb_key(&b, 27);
        wb_go(&b, "https://example.com/untaken");
        check(b.want, "a load the program has not taken");
        wb_pointer(&b, 7 + 82 + 4 + 40, 15, 0);
        wb_pointer(&b, 7 + 82 + 4 + 40, 15, 1);
        wb_pointer(&b, 7 + 82 + 4 + 40, 15, 0);
        check(!b.want && !wb_busy(&b) && !wb_cancel(&b), "Forward: it does not start");
    }

    /* a load that ends does not take the field from someone typing in it */
    wb_go(&b, "https://example.com/slow");
    wb_take(&b, &q);
    wb_key(&b, 12);
    wb_key(&b, 'h');
    wb_key(&b, 'i');
    wb_loaded(&b, page_at(q.url, "<p>Slow", 0), 0);
    check(!strcmp(b.addr, "hi") && b.focus == WB_FOCUS_ADDR && b.caret == 2, "the page came, and the keys typed stay");
    wb_key(&b, '!');
    check(!strcmp(b.addr, "hi!"), "and go on");
    wb_key(&b, 27);
    check(!strcmp(b.addr, "https://example.com/slow") && b.focus == WB_FOCUS_PAGE, "Escape: the page's address");
    wb_go(&b, "https://nowhere.test/");
    wb_take(&b, &q);
    wb_key(&b, 12);
    wb_key(&b, 'x');
    wb_failed(&b, q.url, "no such host");
    check(!strcmp(b.addr, "x") && b.focus == WB_FOCUS_ADDR && !strcmp(b.status, "no such host"), "the same when it fails: its reason in the status bar");
    wb_key(&b, 27);
    wb_go(&b, "https://example.com/again");
    wb_take(&b, &q);
    wb_key(&b, 12);                                 /* in the field, nothing typed */
    wb_loaded(&b, page_at(q.url, "<p>Again", 0), 0);
    check(!strcmp(b.addr, "https://example.com/again") && b.focus == WB_FOCUS_ADDR, "nothing typed: the new address, the focus where it was");
    wb_key(&b, 27);
    wb_cancel(&b);
    p = wb_current(&b);
    wb_failed(&b, "https://z.test/", "gone");
    wb_go(&b, "https://z.test/");
    wb_take(&b, &q);
    wb_failed(&b, q.url, "gone");
    wb_go(&b, "https://y.test/");
    check(!wb_cancel(&b), "a load that failed leaves nothing to stop");
    wb_take(&b, &q);
    wb_failed(&b, q.url, "gone");

    /* a load takes another's place */
    wb_go(&b, "https://one.test/");
    wb_take(&b, &q);
    wb_go(&b, "https://two.test/");
    check((wb_cancel(&b) & WB_CANCEL_PAGE) && wb_take(&b, &q) && !strcmp(q.url, "https://two.test/"), "a new load: the old one's helper goes, the new one starts");
    wb_go(&b, "https://three.test/");
    wb_go(&b, "https://four.test/");                /* one not started yet needs nothing stopped */
    check((wb_cancel(&b) & WB_CANCEL_PAGE) && wb_take(&b, &q) && !strcmp(q.url, "https://four.test/"), "only the last");
    wb_go(&b, "https://five.test/");
    check((wb_cancel(&b) & WB_CANCEL_PAGE) && wb_take(&b, &q), "and the one that started goes in its turn");

    /* a helper that dies, or failed to start, is a failed load */
    wb_go(&b, "https://dies.test/");
    wb_take(&b, &q);
    wb_failed(&b, q.url, "the load ended before its end: its helper stopped");
    check(!wb_busy(&b) && wb_current(&b) == p && strstr(b.status, "helper stopped"), "the window goes on, with the page it had");

    /* what a page loads for itself: under the rules of Undertow and of files */
    wb_go(&b, "https://example.com/rules");
    serve("<p>Rules");
    wb_cancel(&b);
    p = wb_current(&b);
    check(!wb_fetch(&b, p, "https://cdn.test/a.css", 1), "an https page: https");
    check(wb_fetch(&b, p, "http://cdn.test/a.css", 2), "an https page: never over http");
    check(wb_fetch(&b, p, "https://" WB_FILE_HOST "/etc/x.css", 3), "an http(s) page: never a file");
    check(wb_fetch(&b, p, "file:///etc/x.css", 4), "nor by a file: address");
    check(wb_fetch(&b, p, "ftp://cdn.test/x.css", 5), "nor another scheme");
    serial = p->serial;
    check(wb_take(&b, &q) && q.kind == WB_LOAD_TEXT && q.item == 1 && q.page == serial && !strcmp(q.url, "https://cdn.test/a.css") && wb_busy(&b),
          "the program is told to load it, for this page");
    check(!wb_take(&b, &q), "once");
    wb_text(&b, serial + 99, 1, "a{}", 3, "text/css", NULL);
    check(wb_busy(&b), "an answer for another page is nobody's");
    wb_text(&b, serial, 77, "a{}", 3, "text/css", NULL);
    check(wb_busy(&b), "an answer of another tag is nobody's");
    wb_text(&b, serial, 1, "a{}", 3, "text/css", NULL);
    check(!wb_busy(&b) && !wb_take(&b, &q), "the answer: done");
    wb_text(&b, serial, 1, "a{}", 3, "text/css", NULL);
    check(!wb_busy(&b) && !b.inflight, "a second answer changes nothing");
    check(!wb_fetch(&b, p, "https://cdn.test/b.css", 2) && wb_take(&b, &q), "another");
    wb_text(&b, serial, 1, "a{}", 3, "text/css", NULL);
    check(b.inflight == 1, "an answer again to the one that ended does not count for it");
    wb_text(&b, serial, 2, "a{}", 3, "text/css", NULL);

    /* three at a time, and a page that is left takes its loads with it */
    for(i = 10; i < 15; i++) {
        char url[64];
        snprintf(url, sizeof(url), "https://cdn.test/%d.css", i);
        check(!wb_fetch(&b, p, url, i), "another");
    }
    for(i = 0; i < WB_RESOURCES; i++) check(wb_take(&b, &q) && q.item == 10 + i, "as many as there are helpers");
    check(!wb_take(&b, &q), "the rest wait");
    wb_text(&b, serial, 10, NULL, 0, "", "gone");
    check(wb_take(&b, &q) && q.item == 13, "one ended, the next starts");
    wb_go(&b, "https://next.test/");
    check(wb_take(&b, &q) && q.kind == WB_LOAD_PAGE, "a page before what is waiting");
    wb_loaded(&b, page_at(q.url, "<p>next", 0), 0);
    check(wb_current(&b) != p && (wb_cancel(&b) & WB_CANCEL_RESOURCES) && !wb_busy(&b), "a new page: what the old one was loading stops");
    wb_key(&b, 127);
    check(wb_current(&b) == p, "back");
    check(wb_take(&b, &q) && (q.item == 11 || q.item == 12 || q.item == 13), "and what it had not got starts again");
    check(wb_busy(&b) && b.inflight == 1, "one on its way");
    wb_pointer(&b, 7 + 82 + 4 + 40, 15, 0);        /* Forward: leaving it with a load running */
    wb_pointer(&b, 7 + 82 + 4 + 40, 15, 1);
    wb_pointer(&b, 7 + 82 + 4 + 40, 15, 0);
    check(wb_current(&b) != p && (wb_cancel(&b) & WB_CANCEL_RESOURCES) && !b.inflight && !wb_busy(&b), "left: its load stops");
    wb_key(&b, 127);
    check(wb_current(&b) == p && wb_take(&b, &q), "and back again it starts again");
    wb_key(&b, 27);
    check((wb_cancel(&b) & WB_CANCEL_RESOURCES) && !strcmp(b.status, "Stopped") && !wb_take(&b, &q), "Stop: it wants nothing more");
    wb_key(&b, 18);
    check(wb_take(&b, &q) && q.reload && q.kind == WB_LOAD_PAGE, "until it is loaded again");
    wb_failed(&b, q.url, "no");

    /* the limit of a list */
    for(i = 0; i < WB_TEXTS_MAX; i++) wb_fetch(&b, p, "https://cdn.test/more.css", 100 + i);
    check(wb_fetch(&b, p, "https://cdn.test/more.css", 999) == -1, "a list has its end");

    /* a file page: its file addresses are files */
    wb_go(&b, "/stick/page.html");
    wb_take(&b, &q);
    wb_loaded(&b, page_at(q.url, "<p>file", 1), 0);
    p = wb_current(&b);
    check(p->file && !wb_fetch(&b, p, "https://" WB_FILE_HOST "/stick/a.css", 7), "a file page: a file");
    check(wb_take(&b, &q) && !strcmp(q.url, "file:///stick/a.css"), "as a file");
    check(!wb_fetch(&b, p, "http://cdn.test/a.css", 8), "and the web");
    wb_stop(&b);
    wb_cancel(&b);
}

#define FOUND ((0xff * 0xff / 255) << 16 | (0xfd * 0xd3 / 255) << 8 | 0xf5 * 0x4d / 255)  /* find's mark over the page */

/* Is there a pixel of this colour in the rows of the page's area? */
static int has_color(unsigned int c, int y0, int y1) {
    int i;
    for(i = y0 * W; i < y1 * W; i++) if(screen[i] == c) return 1;
    return 0;
}

/* Stage 2.5 */
static void reading(void) {
    static char page[20000];
    struct wb_request q;
    struct wb_page *p;
    size_t at;
    char *e = page;
    int i, s0;

    wb_cancel(&b);
    e += snprintf(e, 200, "<title>R</title><p>One word here<p>Second <b>wo</b>rd there, and a WoRd too<p><a href=a.html>a link</a>"
                          "<img src=z.png alt=picture-word>");
    for(i = 0; i < 80; i++) e += snprintf(e, 32, "<p>filler %d", i);
    snprintf(e, 64, "<p>A last word at the bottom");
    wb_go(&b, "https://read.test/");
    serve(page);
    while(wb_take(&b, &q)) wb_image_failed(&b, q.page, q.item, 0, 0);
    p = wb_current(&b);
    paint();

    /* find: as it is typed, from where it is */
    wb_key(&b, 6);
    check(b.find_open && b.focus == WB_FOCUS_FIND && !b.find_hit, "Ctrl+F: the find field");
    paint();
    type("wor");
    check(b.find_hit && b.find_total == 4 && b.find_index == 1 && !strncmp(p->doc.text + b.find_at, "word", 4), "found as it is typed");
    type("d");
    check(b.find_hit && b.find_total == 4 && b.find_index == 1, "the word: four places, and alt text is none of them");
    at = b.find_at;
    paint();
    check(has_color(FOUND, WB_TOOLBAR, H - WB_STATUS), "the word shows, over the page's background");
    wb_key(&b, 13);
    check(b.find_index == 2 && b.find_at > at, "Enter: the next place, over two styles");
    check(!strncmp(p->doc.text + b.find_at, "word", 4), "that place");
    paint();
    wb_key(&b, 13);
    check(b.find_index == 3 && !strncmp(p->doc.text + b.find_at, "WoRd", 4), "either case");
    check(p->scroll == 0, "in view already: no scrolling");
    wb_key(&b, 13);
    check(b.find_index == 4 && p->scroll > 500, "out of view: it comes into view");
    s0 = p->scroll;
    paint();
    check(has_color(FOUND, WB_TOOLBAR, H - WB_STATUS), "and shows there");
    wb_key(&b, 13);
    check(b.find_index == 1 && p->scroll < s0, "round to the top");
    wb_key(&b, WB_KEY_SHIFT_ENTER);
    check(b.find_index == 4, "Shift+Enter: back round to the last");
    wb_key(&b, WB_KEY_SHIFT_ENTER);
    check(b.find_index == 3, "Shift+Enter again");
    wb_key(&b, 127);
    check(b.find_len == 3 && b.find_hit, "Backspace: a shorter word");
    type("q");
    check(!b.find_hit && !b.find_total, "a word that is not there");
    paint();
    check(has_color(0xf3c9c0, H - WB_STATUS, H), "the field says so");
    for(i = 0; i < 4; i++) wb_key(&b, 127);
    check(!b.find_len && !b.find_hit, "nothing to find");
    for(i = 0; i < 400; i++) wb_key(&b, 'a');
    check(b.find_len == WB_FIND_MAX - 1, "a word has its end");
    for(i = 0; i < 400; i++) wb_key(&b, 127);
    type("hereSecond");
    check(!b.find_hit, "a word does not go from a box into the next");
    while(b.find_len) wb_key(&b, 127);
    type("Second word");
    check(b.find_hit && b.find_total == 1, "a space is a space, over a style");
    wb_key(&b, 27);
    check(!b.find_open && b.focus == WB_FOCUS_PAGE && !b.find_hit, "Escape: closed, the page has the keys");
    paint();
    check(!has_color(FOUND, WB_TOOLBAR, H - WB_STATUS), "and the mark goes");
    wb_key(&b, 6);
    check(b.find_open && b.find_hit && b.find_len, "Ctrl+F again: the word as it was");
    wb_pointer(&b, 300, 200, 0);
    wb_pointer(&b, 300, 200, 1);
    wb_pointer(&b, 300, 200, 0);
    check(b.focus == WB_FOCUS_PAGE && b.find_open, "a click on the page: the keys are the page's, the field stays");
    wb_key(&b, 27);
    check(!b.find_open, "and Escape there closes it");
    wb_key(&b, 6);
    while(b.find_len) wb_key(&b, 127);
    wb_key(&b, 27);
    /* a new page ends it */
    wb_key(&b, 6);
    wb_go(&b, "https://read.test/2");
    serve("<p>Other");
    check(!b.find_open && b.focus == WB_FOCUS_PAGE, "a new page: find has gone");
    wb_key(&b, 127);
    wb_cancel(&b);
    check(wb_current(&b) == p, "back");

    /* links by keyboard: from what is in view, in the page's order, round again */
    wb_go(&b, "https://read.test/links");
    e = page;
    e += snprintf(e, 200, "<title>L</title><p>Text <a href=a.html>first</a> and <a href=b.html>second link, long enough to go over the "
                          "end of its line and on to the next line of the page, and still go on</a>.");
    for(i = 0; i < 80; i++) e += snprintf(e, 32, "<p>filler %d", i);
    snprintf(e, 64, "<p><a href=c.html>third</a>");
    serve(page);
    p = wb_current(&b);
    paint();
    wb_key(&b, 9);
    check(b.tab_link == 0 && strstr(b.status, "a.html") != NULL, "Tab: the first link, its address in the status bar");
    check(p->scroll == 0, "in view");
    paint();
    {
        const struct wl_item *it = NULL;
        size_t k;
        for(k = 0; k < p->lay.count; k++) if(p->lay.items[k].link == 0) {
            it = &p->lay.items[k];
            break;
        }
        check(it && screen[(WB_TOOLBAR + it->y - 1) * W + it->x + 2] == 0x153d49, "drawn focused");
    }
    wb_key(&b, 9);
    check(b.tab_link == 1, "the next: a link of two lines is one stop");
    paint();
    wb_key(&b, 9);
    check(b.tab_link == 2 && p->scroll > 500, "the last: scrolled into view");
    paint();
    wb_key(&b, 9);
    check(b.tab_link == 0 && p->scroll == 0, "and round");
    wb_key(&b, WB_KEY_BACKTAB);
    check(b.tab_link == 2 && p->scroll > 500, "Shift+Tab: back round");
    wb_key(&b, WB_KEY_BACKTAB);
    check(b.tab_link == 1 && p->scroll < 100, "Shift+Tab again: the one before");
    wb_key(&b, WB_KEY_BACKTAB);
    check(b.tab_link == 0, "and the first");
    wb_pointer(&b, 500, H - WB_STATUS - 3, 0);
    wb_pointer(&b, 500, H - WB_STATUS - 3, 1);
    wb_pointer(&b, 500, H - WB_STATUS - 3, 0);
    check(b.tab_link == -1, "a click on the page: nothing has the focus");
    wb_key(&b, 0x105);
    paint();
    wb_key(&b, 9);
    check(b.tab_link == 2, "nothing has it: Tab starts from what is in view");
    wb_key(&b, 0x104);
    wb_key(&b, 9);
    /* Enter follows it */
    wb_key(&b, 13);
    check(has_request("https://read.test/a.html"), "Enter follows the link with the focus");
    wb_stop(&b);
    wb_cancel(&b);
    p->halted = 0;

    /* visited links, for as long as the window runs */
    wb_go(&b, "https://read.test/visits");
    serve("<body vlink=#123456 link=#654321><p><a href=one.html>one</a> <a href=two.html>two</a>");
    p = wb_current(&b);
    check(p->doc.visited && !p->doc.visited[0] && !p->doc.visited[1], "nothing visited yet");
    paint();
    check(has_color(0x654321, WB_TOOLBAR, H - WB_STATUS) && !has_color(0x123456, WB_TOOLBAR, H - WB_STATUS), "in the link colour");
    wb_key(&b, 9);
    wb_key(&b, 13);
    serve("<p>One");
    wb_key(&b, 127);
    wb_cancel(&b);
    check(wb_current(&b) == p && p->doc.visited[0] && !p->doc.visited[1], "where it went is visited");
    paint();
    check(has_color(0x123456, WB_TOOLBAR, H - WB_STATUS) && has_color(0x654321, WB_TOOLBAR, H - WB_STATUS), "in the visited colour, the other not");
    /* a redirect: both ends count; the page's own: the theme's colour */
    wb_go(&b, "https://read.test/other");
    serve("<p><a href=one.html>one</a> <a href=#x>here</a><p id=x>x");
    p = wb_current(&b);
    check(p->doc.visited[0], "the same address on another page");
    paint();
    check(has_color(b.theme.vlink, WB_TOOLBAR, H - WB_STATUS), "the theme's colour for one");
    wb_key(&b, 9);
    wb_key(&b, 9);
    check(b.tab_link == 1, "the second link");
    wb_key(&b, 13);
    check(!b.want && p->doc.visited[1], "a place on the same page, by Enter: visited, with no load");
    /* a new width: what is being read stays what is at the top */
    {
        static char text[12000];
        char *e = text;
        size_t off, again;
        int into, into2;
        for(i = 0; i < 60; i++) e += snprintf(e, 180, "<p>Paragraph %d has words enough on it to take more than a line when the window is narrower than it is now, or so it is hoped here.", i);
        wb_go(&b, "https://read.test/long");
        serve(text);
        p = wb_current(&b);
        paint();
        wb_key(&b, 0x107);
        wb_key(&b, 0x107);
        wb_key(&b, 0x107);
        check(wl_place(&p->lay, p->scroll, &off, &into), "a place, well down");
        wb_resize(&b, 400, H);
        check(wl_place(&p->lay, p->scroll, &again, &into2) && again == off, "a window narrower: the same place at the top, before any paint");
        wb_resize(&b, W, H);
        check(wl_place(&p->lay, p->scroll, &again, &into2) && again == off, "wider again");
        paint();
    }
    /* where a redirect started counts, as where it ended does */
    wb_go(&b, "https://read.test/from");
    wb_take(&b, &q);
    wb_loaded(&b, page_at("https://read.test/to", "<p><a href=from>from</a> <a href=to>to</a> <a href=none>none</a>", 0), 0);
    p = wb_current(&b);
    check(p->doc.visited[0] && p->doc.visited[1] && !p->doc.visited[2], "both ends of a redirect");
    /* a load's end leaves all of the address field chosen, for the key that replaces it */
    wb_go(&b, "https://read.test/chosen");
    wb_take(&b, &q);
    wb_key(&b, 12);
    check(b.focus == WB_FOCUS_ADDR && b.chosen, "Ctrl+L while it loads");
    wb_loaded(&b, page_at(q.url, "<p>Here", 0), 0);
    check(b.chosen, "it ends");
    wb_key(&b, 'x');
    check(!strcmp(b.addr, "x"), "the key replaces it all");
    wb_key(&b, 27);
    /* Stop: the address field says what is shown */
    p = wb_current(&b);
    wb_go(&b, "https://never.test/");
    wb_take(&b, &q);
    check(!strcmp(b.addr, "https://never.test/"), "a load on its way");
    wb_stop(&b);
    wb_cancel(&b);
    check(!strcmp(b.addr, p->url), "stopped: the page's address again");
    /* a jump on the page while another page loads does not say Done */
    wb_go(&b, "https://read.test/jump");
    serve("<p><a href=#x>here</a><p id=x>x");
    wb_go(&b, "https://read.test/slowly");
    wb_take(&b, &q);
    wb_key(&b, 9);
    wb_key(&b, 13);
    check(!strstr(b.status, "Done"), "a jump while a page loads: it is not done");
    wb_stop(&b);
    wb_cancel(&b);
    /* Tab to a field while a page loads: the status bar goes on saying so */
    wb_go(&b, "https://read.test/field");
    serve("<form><input name=a></form>");
    wb_go(&b, "https://read.test/slow");
    wb_take(&b, &q);
    check(strstr(b.status, "Loading") != NULL, "a load");
    wb_key(&b, 9);
    check(b.field == 0 && strstr(b.status, "Loading") != NULL, "Tab to a field: still loading");
    wb_key(&b, 27);
    wb_stop(&b);
    wb_cancel(&b);
    /* a file page's links are known by their file: addresses */
    wb_go(&b, "/stick/a.html");
    wb_take(&b, &q);
    wb_loaded(&b, page_at(q.url, "<p><a href=a.html>me</a> <a href=b.html>it</a>", 1), 0);
    p = wb_current(&b);
    check(p->doc.visited[0] && !p->doc.visited[1], "a file page's link to itself is visited");
    /* many places: the table starts again, and goes on working */
    for(i = 0; i < WB_VISITED; i++) {
        char url[64];
        snprintf(url, sizeof(url), "https://many.test/%d", i);
        wb_go(&b, url);
        serve("<p>a page");
    }
    check(b.seen_count > 0 && b.seen_count <= WB_VISITED / 2, "so many places: a bounded table");
    wb_cancel(&b);
}

/**** measured as painted (11/02/26, the integration of milestones 2 and 3) ****/

static unsigned int tinted(unsigned int c) {    /* paint.c's wp_tint() of find's colour over c */
    return ((c >> 16 & 255) * 0xff / 255) << 16 | ((c >> 8 & 255) * 0xd3 / 255) << 8 | (c & 255) * 0x4d / 255;
}

/* Is there a pixel of the page's text in column x between rows y0 and y1 of a screen, its background being bg? */
static int ink_in(const unsigned int *px, int x, int y0, int y1, unsigned int bg) {
    int y;
    for(y = y0; y < y1; y++) if(px[y * W + x] != bg) return 1;
    return 0;
}

/* find's mark, Tab's frame and an image's frame where what they are about is painted: proportional text of any size
   and style by wf_width(), as layout.c and paint.c have it, code in its cells, an image's alt text in cells */
static void measured(void) {
    static unsigned int plain[W * H];
    struct wb_request q;
    struct wb_page *p;
    const unsigned int bg = 0xfffdf5;
    size_t i;
    int k, frames = 0, framed = 0;

    wb_cancel(&b);
    wb_go(&b, "https://measure.test/");
    serve("<title>M</title><h1>Big wave</h1><p>a <b>bold wave</b> and <code>mono wave</code> and plain wave here"
          "<p><a href=x.html>a link that goes on and on, over the end of its line and on to the next one, long enough to "
          "wrap at least once on a page this wide</a><p><img src=n.png alt=\"an alt text\">");
    while(wb_take(&b, &q)) wb_image_failed(&b, q.page, q.item, 0, 0);
    p = wb_current(&b);
    paint();
    memcpy(plain, screen, sizeof(plain));
    wb_key(&b, 6);
    type("wave");
    for(k = 0; k < 4; k++) {
        const struct wl_item *it = NULL;
        int x0, x1, y0, y1, x, y, outside = 0, inside = 0;
        char what[160];
        for(i = 0; i < p->lay.count && !it; i++)
            if(p->lay.items[i].kind == WI_TEXT && p->lay.items[i].off <= b.find_at && b.find_at + 4 <= p->lay.items[i].off + p->lay.items[i].len)
                it = &p->lay.items[i];
        if(!b.find_hit || !it) {
            check(0, "find's word, in one item");
            break;
        }
        x0 = it->x + wf_width(p->doc.text + it->off, b.find_at - it->off, it->scale, it->style);
        x1 = x0 + wf_width(p->doc.text + b.find_at, 4, it->scale, it->style);
        y0 = WB_TOOLBAR + it->y - p->scroll;
        y1 = y0 + it->h;
        paint();
        for(y = y0; y < y1; y++)
            for(x = 0; x < W - WB_SCROLLBAR; x++) {
                unsigned int was = plain[y * W + x], now = screen[y * W + x];
                if(x >= x0 && x < x1) inside += now == tinted(was);
                else outside += now != was;
            }
        snprintf(what, sizeof(what), "find's mark on \"%.*s\" (size %d%s): the word's width and height, nothing else",
                 (int)it->len, p->doc.text + it->off, wf_cell_h(it->scale), it->style & WS_MONO ? ", code" : it->style & WS_BOLD ? ", bold" : "");
        check(!outside && inside == (x1 - x0) * (y1 - y0), what);
        if(it->style & WS_MONO) check(x1 - x0 == 4 * wf_cell_w(it->scale), "code: four cells");
        else check(ink_in(plain, x0, y0, y1, bg) && !ink_in(plain, x0 - 1, y0, y1, bg) && !ink_in(plain, x1, y0, y1, bg),
                   "from the first column of its ink to the end of its last letter's advance");
        wb_key(&b, 13);
    }
    wb_key(&b, 27);

    /* Tab's frame round each line of a link, its text inside it */
    wb_key(&b, 9);
    check(b.tab_link == 0, "Tab: the link");
    paint();
    for(i = 0; i < p->lay.count; i++) {
        const struct wl_item *it = &p->lay.items[i];
        int y0 = WB_TOOLBAR + it->y - p->scroll;
        if(it->link != 0 || it->kind != WI_TEXT) continue;
        frames++;
        framed += screen[(y0 - 1) * W + it->x - 1] == 0x153d49 && screen[(y0 + it->h) * W + it->x + it->w] == 0x153d49 &&
                  ink_in(plain, it->x, y0, y0 + it->h, bg) && !ink_in(plain, it->x + it->w, y0, y0 + it->h, bg) &&
                  it->w == wf_width(p->doc.text + it->off, it->len, it->scale, it->style);
    }
    check(frames >= 2 && framed == frames, "a link over two lines: a frame round each, its ink inside, as wide as its text");
    wb_key(&b, WB_KEY_BACKTAB);

    /* an image's alt text in its frame is in cells, as layout.c sizes the frame */
    for(i = 0; i < p->lay.count; i++)
        if(p->lay.items[i].kind == WI_IMAGE)
            check(p->lay.items[i].w == (int)wf_count("an alt text", 11) * WF_W + 8 && p->lay.items[i].h == WF_H + 8,
                  "an image's frame: its alt text's cells and the frame's padding");
    wb_cancel(&b);
}

int main(void) {
    char tall[20000], msg[200];
    int x, y, i, s0;
    struct wb_page *p;

    check(!wb_init(&b, W, H), "wb_init");
    paint();
    check(b.focus == WB_FOCUS_ADDR && !b.npages && wb_current(&b) == b.start, "the start page, the address field waiting");
    check(screen[1 * W + 1] == 0xe7e2d2 && screen[(H - 2) * W + 1] == 0xe7e2d2, "the toolbar and the status bar painted");

    type("example.com\r");
    check(has_request("https://example.com/") && b.loading && b.focus == WB_FOCUS_PAGE, "an address typed: https by default");
    b.want = 1;
    snprintf(b.req.url, sizeof(b.req.url), "https://example.com/");
    serve("<title>One</title><p>Go <a href=two.html>two</a> or <a href=#end>down</a> or <a href=mailto:a@b.test>mail</a>"
          "<p id=end>The end.");
    check(b.npages == 1 && !strcmp(b.addr, "https://example.com/") && !strcmp(b.status, "Done") && !b.loading,
          "a page loaded: the address, Done");
    paint();

    check(link_spot(0, &x, &y), "link 0 is on the screen");
    wb_pointer(&b, x, y, 0);
    check(b.hover == 0 && !strcmp(b.status, "https://example.com/two.html"), "a link under the pointer: its address in the status bar");
    click(x, y);
    check(has_request("https://example.com/two.html"), "a link followed");
    b.loading = 0;

    check(link_spot(1, &x, &y), "link 1 is on the screen");
    click(x, y);
    check(!b.want && !strcmp(b.addr, "https://example.com/#end"), "a place on the same page: no load, its address");

    check(link_spot(2, &x, &y), "link 2 is on the screen");
    click(x, y);
    check(!b.want && strstr(b.status, "not an http or https address") != NULL, "a mailto: link is not followed, and says why");

    check(link_spot(0, &x, &y), "link 0 again");
    wb_pointer(&b, x, y, 1);
    wb_pointer(&b, x + 300, y + 100, 1);
    wb_pointer(&b, x + 300, y + 100, 0);
    check(!b.want, "a press on a link let go elsewhere follows nothing");

    /* a second page, back and forward */
    wb_go(&b, "https://example.com/two.html");
    serve("<title>Two</title><p>Page two");
    check(b.npages == 2 && b.cur == 1, "two pages in the history");
    wb_key(&b, 127);
    check(b.cur == 0 && !strcmp(b.addr, "https://example.com/#end"), "Backspace: back, with its address");
    paint();
    click(7 + 82 + 4 + 40, 4 + 11);                 /* Forward: after Back (8 cells + 14) */
    check(b.cur == 1, "the Forward button");
    wb_key(&b, 18);
    check(has_request("https://example.com/two.html") && b.req.reload, "Ctrl+R: the page again, in its place");

    /* scrolling */
    strcpy(tall, "<title>Tall</title>");
    for(i = 0; i < 300; i++) strcat(tall, "<p>line");
    wb_go(&b, "https://example.com/tall");
    serve(tall);
    p = wb_current(&b);
    paint();
    wb_key(&b, 0x107);
    check(p->scroll > 200, "Page Down");
    s0 = p->scroll;
    wb_key(&b, 0x101);
    check(p->scroll == s0 + 32, "the arrow down: 32 pixels");
    wb_wheel(&b, -3);
    check(p->scroll == s0 + 32 - 48, "the wheel: 16 pixels a row");
    wb_key(&b, 0x105);
    check(p->scroll == p->lay.height - (H - WB_TOOLBAR - WB_STATUS), "End: the bottom");
    wb_key(&b, 0x104);
    check(p->scroll == 0, "Home: the top");
    wb_pointer(&b, W - 6, WB_TOOLBAR + 5, 1);      /* the thumb, at the top */
    wb_pointer(&b, W - 6, WB_TOOLBAR + 105, 1);
    wb_pointer(&b, W - 6, WB_TOOLBAR + 105, 0);
    snprintf(msg, sizeof(msg), "the scroll bar's thumb dragged (scroll %d)", p->scroll);
    check(p->scroll > 100 && !b.drag, msg);
    click(W - 6, H - WB_STATUS - 3);
    check(p->scroll > 400, "a click below the thumb: a page down");
    s0 = p->scroll;
    wb_resize(&b, 400, 300);
    paint();
    check(b.w == 400 && p->lay_w == 400 - WB_SCROLLBAR && p->scroll <= p->lay.height, "a new size: laid out again");
    wb_resize(&b, W, H);

    /* the address field */
    wb_key(&b, 12);
    check(b.focus == WB_FOCUS_ADDR && b.chosen, "Ctrl+L: the address, all of it chosen");
    type("abc");
    check(!strcmp(b.addr, "abc") && b.caret == 3, "typing replaces what is chosen");
    wb_key(&b, 0x102);
    wb_key(&b, 127);
    check(!strcmp(b.addr, "ac") && b.caret == 1, "Left, Backspace");
    wb_key(&b, 0x108);
    check(!strcmp(b.addr, "a"), "Delete");
    wb_key(&b, 27);
    check(!strcmp(b.addr, "https://example.com/tall") && b.focus == WB_FOCUS_PAGE, "Escape: the page's address again");

    /* files */
    wb_key(&b, 12);
    type("/initrd/www/index.html\r");
    check(has_request("file:///initrd/www/index.html"), "a path: a file");
    {
        char path[256];
        check(wb_file_path("file:///initrd/my%20site/a.html#x", path, sizeof(path)) && !strcmp(path, "/initrd/my site/a.html") &&
              !wb_file_path("https://a.test/", path, sizeof(path)), "wb_file_path");
    }
    b.want = 1;
    snprintf(b.req.url, sizeof(b.req.url), "file:///initrd/www/index.html");
    serve("<p><a href=other.html>other</a> <a href=https://a.test/>web</a>");
    check(!strcmp(b.addr, "file:///initrd/www/index.html"), "a file page: its address");
    paint();
    link_spot(0, &x, &y);
    click(x, y);
    check(has_request("file:///initrd/www/other.html"), "a file page's link to a file: followed to the file");
    b.loading = 0;
    link_spot(1, &x, &y);
    click(x, y);
    check(has_request("https://a.test/"), "a file page's link to the web");
    b.loading = 0;
    wb_go(&b, "https://evil.test/");
    serve("<p><a href=\"https://" WB_FILE_HOST "/etc/secret\">bait</a>");
    paint();
    link_spot(0, &x, &y);
    click(x, y);
    check(has_request("https://" WB_FILE_HOST "/etc/secret"), "an http page's link never opens a file");
    b.loading = 0;

    /* a load that fails */
    wb_go(&b, "https://nowhere.test/");
    wb_take(&b, &b.req);
    wb_failed(&b, "https://nowhere.test/", "nowhere.test: no such host");
    check(!b.loading && !strcmp(b.status, "nowhere.test: no such host") && !strcmp(b.addr, "https://nowhere.test/"),
          "a failed load: why, and the address kept");
    wb_go(&b, "not a valid address");
    check(!b.want, "an address that is none: no load");

    /* the history's limit */
    for(i = 0; i < 20; i++) {
        snprintf(msg, sizeof(msg), "https://example.com/%d", i);
        wb_go(&b, msg);
        serve("<p>a page");
    }
    check(b.npages == WB_HISTORY && b.cur == WB_HISTORY - 1 && !strcmp(b.addr, "https://example.com/19"),
          "the history: 16 pages, the oldest gone");
    paint();

    loads();
    pictures();
    reading();
    measured();

    wb_free(&b);
    check(ut_alloc_live == 0, "nothing held at the end");
    if(failures) {
        printf("WUNDERTOW BROWSER TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW BROWSER TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
