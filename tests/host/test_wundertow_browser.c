/*
SurfOS Undertow's window: tests of the browser
----------------------
File: test_wundertow_browser.c  Date: 10/25/26
----------------------
user/wundertow/browser.c on the host, with the rest of the window's modules: the window's events as a person
gives them, and the loads it asks for carried out here with pages made in the test. An address typed, a link
followed (on the page, to a place on the same page, to a file from a file page, one that is not http), back
and forward, the history's limit, scrolling by keys, the wheel and the scroll bar, a new size, the address
field's editing, a load that fails, and the chrome painted.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "undertow.h"
#include "url.h"
#include "http.h"
#include "doc.h"
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

    wb_free(&b);
    if(failures) {
        printf("WUNDERTOW BROWSER TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW BROWSER TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
