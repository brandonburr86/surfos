/*
SurfOS Undertow's window: tests of a page's style sheets from the network
----------------------
File: test_wundertow_sheets.c  Date: 11/01/26
----------------------
user/wundertow/sheets.c through browser.c, with the loads carried out here: a page shows at once and is built again
when its sheets have answered, in the page's order whichever answered first; what may not be loaded never is (an http
sheet on an https page, a file from a web page, url()), @import goes one level, what fails or is no sheet is left out,
Stop, leaving a page and coming back, and every allocation failing in turn.
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
#include "alloc.h"

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

/* go to url, the page's load carried out with this html, and nothing it asks for */
static void open_page(const char *url, const char *html) {
    struct wb_request q;
    int file = !strncmp(url, "file:", 5);
    wb_go(&b, url);
    while(wb_cancel(&b)) ;
    if(wb_take(&b, &q)) wb_loaded(&b, page_at(q.url, html, file), q.reload);
}

static struct wb_request q;

static int next(void) {
    while(wb_cancel(&b)) ;
    return wb_take(&b, &q);
}

/* the request waiting is for url, of a text */
static int asked(const char *url) {
    return next() && q.kind == WB_LOAD_TEXT && !strcmp(q.url, url);
}

static void answer(const char *css, const char *type) {
    wb_text(&b, q.page, q.item, css, css ? strlen(css) : 0, type, css ? NULL : "it failed");
}

static const struct wd_run *run(const char *s) {
    const struct wd_document *d = &wb_current(&b)->doc;
    size_t i, n = strlen(s);
    for(i = 0; i < d->run_count; i++) {
        const struct wd_run *r = &d->runs[i];
        const char *p;
        if(r->kind != WR_TEXT) continue;
        for(p = d->text + r->off; p + n <= d->text + r->off + r->len; p++) if(!memcmp(p, s, n)) return r;
    }
    return NULL;
}

static unsigned int color_of(const char *s) {
    const struct wd_run *r = run(s);
    return r ? r->color : 0xbad;
}

static unsigned int screen[W * H];

int main(void) {
    struct wb_page *p;
    long i;
    static char many[4000], tall[20000];
    size_t n;
    int k;

    check(!wb_init(&b, W, H), "wb_init");

    /* a page, its sheet, and the page again */
    open_page("https://a.test/d/one.html", "<link rel=stylesheet href=a.css><p>x");
    p = wb_current(&b);
    check(p && color_of("x") == WD_NONE && !strcmp(b.status, "Done"), "a page shows at once, without its sheet");
    wb_paint(&b, screen, W);
    check(asked("https://a.test/d/a.css"), "its sheet is asked for, at the address it resolves to");
    check(wb_busy(&b), "and the window says it is still loading");
    answer("p{color:#ff0000}", "text/css");
    check(color_of("x") == 0xff0000 && p->lay_w == 0 && !p->doc.sheets && !p->style, "with the answer the page is built again");
    check(!wb_busy(&b), "and nothing more is loading");
    wb_paint(&b, screen, W);
    check(p->lay_w > 0, "and laid out again");

    /* the page's order, whichever answers first */
    open_page("https://a.test/d/two.html", "<link rel=stylesheet href=a.css><link rel=stylesheet href=b.css><style>p{font-weight:bold}</style><p>x");
    check(asked("https://a.test/d/a.css"), "two sheets: the first");
    {
        struct wb_request first = q;
        check(asked("https://a.test/d/b.css"), "and the second, at once");
        answer("p{color:#0000b0}", "text/css");
        check(color_of("x") == WD_NONE, "one answered: the page waits for the other");
        q = first;
        answer("p{color:#0000a0}", "text/css");
    }
    check(color_of("x") == 0x0000b0, "the page's order, not the order they came in");
    check(run("x")->style & WS_BOLD, "with what the page has of its own");

    /* failures */
    open_page("https://a.test/d/three.html", "<link rel=stylesheet href=a.css><link rel=stylesheet href=b.css><p>x");
    check(asked("https://a.test/d/a.css"), "a sheet that fails");
    answer(NULL, "");
    check(asked("https://a.test/d/b.css"), "and the next");
    answer("p{color:#0000b0}", "text/css");
    check(color_of("x") == 0x0000b0, "is left out, the others stand");
    open_page("https://a.test/d/four.html", "<link rel=stylesheet href=a.css><p>x");
    p = wb_current(&b);
    check(asked("https://a.test/d/a.css"), "the only sheet");
    answer(NULL, "");
    check(p->lay_w >= 0 && !p->style && p->doc.sheets, "fails: the page stays as it is, not built again");
    open_page("https://a.test/d/five.html", "<link rel=stylesheet href=a.css><p>x");
    check(asked("https://a.test/d/a.css"), "an error page for a sheet");
    answer("<html>p{color:#ff0000}</html>", "text/html");
    check(color_of("x") == WD_NONE && !wb_busy(&b), "is no sheet");
    open_page("https://a.test/d/six.html", "<link rel=stylesheet href=a.css><p>x");
    check(asked("https://a.test/d/a.css"), "text/plain from a web page");
    answer("p{color:#ff0000}", "text/plain");
    check(color_of("x") == WD_NONE, "is no sheet either");
    open_page("https://a.test/d/seven.html", "<link rel=stylesheet href=a.css><p>x");
    check(asked("https://a.test/d/a.css"), "no type at all");
    answer("p{color:#ff0000}", "");
    check(color_of("x") == 0xff0000, "is taken as one");
    open_page("https://a.test/d/eight.html", "<link rel=stylesheet href=a.css><p>x");
    check(asked("https://a.test/d/a.css"), "a malformed sheet");
    answer("p{color:}}}{{{ ((( [[[ \"", "text/css");
    check(color_of("x") == WD_NONE && !wb_current(&b)->style, "changes nothing");

    /* what may not be loaded never is */
    open_page("https://a.test/d/nine.html", "<link rel=stylesheet href=http://a.test/a.css><p>x");
    check(!next() && !wb_busy(&b) && !wb_current(&b)->style, "an https page: no sheet over http");
    open_page("https://a.test/d/ten.html", "<link rel=stylesheet href=https://" WB_FILE_HOST "/etc/secret.css><p>x");
    check(!next() && !wb_busy(&b), "an https page never gets at a file, by the host that means one");
    open_page("http://a.test/d/ten.html", "<link rel=stylesheet href=file:///etc/secret.css><link rel=stylesheet href=/etc/secret.css><p>x");
    check(asked("http://a.test/etc/secret.css") && !next(), "an http page: a path is the server's, a file: address is nothing");
    answer("p{color:#ff0000}", "text/css");
    open_page("https://a.test/d/url.html", "<style>p{background:url(https://b.test/x.png)} h1{background-image:url(y.png)}</style>"
              "<style>@import url(z.css);</style><p>x");
    check(!next() && !wb_busy(&b), "url() loads nothing, in a value or an @import");
    open_page("https://a.test/d/redir.html", "<link rel=stylesheet href=a.css><p>x");
    check(asked("https://a.test/d/a.css"), "a sheet's own url()");
    answer("@import url(z.css); p{background:url(q.png);color:#ff0000}", "text/css");
    check(!next() && color_of("x") == 0xff0000, "loads nothing either");

    /* a sheet never gets at a file, on a file page neither */
    open_page("file:///web/page.html", "<link rel=stylesheet href=http://evil.test/a.css><p>x");
    check(asked("http://evil.test/a.css"), "a file page's sheet from the web");
    answer("@import \"https://" WB_FILE_HOST "/home/secret\"; p{color:#ff0000}", "text/css");
    check(!next() && color_of("x") == 0xff0000, "imports no file");
    open_page("file:///web/page.html", "<link rel=stylesheet href=style.css><p>x");
    check(asked("file:///web/style.css"), "a file page's own sheet");
    answer("@import \"other.css\"; p{color:#ff0000}", "text/plain");
    check(!next() && color_of("x") == 0xff0000, "imports no file either: only the page's <link> reaches one");
    open_page("file:///web/page.html", "<style>@import \"other.css\"; p{color:#ff0000}</style><p>x");
    check(!next() && !wb_busy(&b) && color_of("x") == 0xff0000, "and neither does a <style>'s @import");

    /* a file page's sheets are files */
    open_page("file:///web/page.html", "<link rel=stylesheet href=style.css><p>x");
    check(asked("file:///web/style.css"), "a file page: a file beside it");
    answer("p{color:#ff0000}", "text/plain");
    check(color_of("x") == 0xff0000, "by its place: a file has no type");
    open_page("file:///web/page.html", "<link rel=stylesheet href=https://a.test/a.css><p>x");
    check(asked("https://a.test/a.css"), "a file page: a sheet of the web");
    answer("p{color:#ff0000}", "text/css");

    /* @import, one level */
    open_page("https://a.test/d/imp.html", "<link rel=stylesheet href=a.css><p>x");
    check(asked("https://a.test/d/a.css"), "a sheet with an @import");
    answer("@import \"sub/b.css\"; p{font-weight:bold}", "text/css");
    check(wb_current(&b)->style != NULL, "the page waits for what it imports");
    check(asked("https://a.test/d/sub/b.css"), "against the sheet's address");
    answer("@import \"c.css\"; p{color:#0000b0}", "text/css");
    check(!next() && !wb_busy(&b), "an imported sheet's @import is not followed");
    check(color_of("x") == 0x0000b0 && (run("x")->style & WS_BOLD), "both count");
    open_page("https://a.test/d/imp2.html", "<style>@import \"a.css\"; p{font-weight:bold}</style><p>x");
    check(asked("https://a.test/d/a.css"), "@import in a <style>");
    answer("@import \"c.css\"; p{color:#0000a0;font-weight:normal}", "text/css");
    check(!next() && color_of("x") == 0x0000a0 && (run("x")->style & WS_BOLD), "one level, and it comes before the sheet that has it");
    open_page("https://a.test/d/imp3.html", "<link rel=stylesheet href=a.css><p>x");
    check(asked("https://a.test/d/a.css"), "an https sheet");
    answer("@import \"http://a.test/b.css\"; p{color:#ff0000}", "text/css");
    check(!next() && color_of("x") == 0xff0000, "imports nothing over http");
    open_page("https://a.test/d/imp4.html", "<link rel=stylesheet href=a.css><p>x");
    check(asked("https://a.test/d/a.css"), "an @import that fails");
    answer("@import \"b.css\"; p{color:#ff0000}", "text/css");
    check(asked("https://a.test/d/b.css"), "is asked for");
    answer(NULL, "");
    check(color_of("x") == 0xff0000, "and the sheet that has it stands");

    /* no more than three at a time, none more than may be */
    n = 0;
    for(k = 0; k < 20; k++) n += (size_t)snprintf(many + n, sizeof(many) - n, "<link rel=stylesheet href=%d.css>", k);
    snprintf(many + n, sizeof(many) - n, "<p>x");
    open_page("https://a.test/d/many.html", many);
    {
        struct wb_request first[WB_RESOURCES];
        int got = 0, total = 0;
        while(next()) first[got++ % WB_RESOURCES] = q;
        check(got == WB_RESOURCES, "three at a time");
        while(got) {
            q = first[--got];
            answer("p{color:#ff0000}", "text/css");
            total++;
            if(!got) while(next()) first[got++] = q;
        }
        check(total == WD_SHEETS_MAX && color_of("x") == 0xff0000 && !wb_current(&b)->style, "16 in all, and the page");
    }

    /* Stop, leaving, coming back */
    open_page("https://a.test/d/stop.html", "<link rel=stylesheet href=a.css><p>x");
    check(asked("https://a.test/d/a.css"), "a sheet on its way");
    wb_stop(&b);
    check((wb_cancel(&b) & WB_CANCEL_RESOURCES) && !wb_busy(&b) && !next(), "Stop ends it, and asks no more");
    check(color_of("x") == WD_NONE && !wb_current(&b)->style, "the page stands, and holds nothing more for it");
    answer("p{color:#ff0000}", "text/css");
    check(color_of("x") == WD_NONE, "an answer that comes late counts for nothing");
    open_page("https://a.test/d/stop2.html", "<link rel=stylesheet href=a.css><link rel=stylesheet href=b.css><p>x");
    check(asked("https://a.test/d/a.css"), "two sheets on their way");
    answer("p{color:#ff0000}", "text/css");
    check(asked("https://a.test/d/b.css") && color_of("x") == WD_NONE, "one answered, the other not");
    wb_stop(&b);
    check(color_of("x") == 0xff0000 && !wb_current(&b)->style, "Stop: the page takes what came");
    open_page("https://a.test/d/leave.html", "<link rel=stylesheet href=a.css><p>x");
    check(asked("https://a.test/d/a.css"), "another");
    open_page("https://a.test/d/next.html", "<p>y");
    answer("p{color:#ff0000}", "text/css");
    check(!wb_current(&b)->style && color_of("y") == WD_NONE, "a page left: its answer goes nowhere");
    wb_key(&b, 127);
    check(asked("https://a.test/d/a.css"), "back at it: its sheet is asked for again");
    answer("p{color:#ff0000}", "text/css");
    check(color_of("x") == 0xff0000, "and counts");

    /* a place on the page */
    n = (size_t)snprintf(tall, sizeof(tall), "<link rel=stylesheet href=a.css><p class=gone>one");
    for(k = 0; k < 100; k++) n += (size_t)snprintf(tall + n, sizeof(tall) - n, "<p>line");
    snprintf(tall + n, sizeof(tall) - n, "<p id=end>The end");
    open_page("https://a.test/d/tall.html#end", tall);
    p = wb_current(&b);
    wb_paint(&b, screen, W);
    {
        int before = p->scroll, top;
        size_t off;
        check(before > 0 && asked("https://a.test/d/a.css"), "a fragment target on a page with a sheet");
        answer(".gone{display:none} p{margin:0}", "text/css");
        wb_paint(&b, screen, W);
        check(!wd_anchor_offset(&p->doc, "end", &off), "its target is still there");
        top = wl_offset_y(&p->lay, off) - 4;
        if(top > p->lay.height - (H - WB_TOOLBAR - WB_STATUS)) top = p->lay.height - (H - WB_TOOLBAR - WB_STATUS);
        check(p->scroll == top && top < before, "the page goes to where the target is now");
    }
    open_page("https://a.test/d/tall.html#end", tall);
    p = wb_current(&b);
    wb_paint(&b, screen, W);
    check(asked("https://a.test/d/a.css"), "a reader who scrolled while the sheet came");
    p->scroll = 50;
    answer(".gone{display:none} p{margin:0}", "text/css");
    wb_paint(&b, screen, W);
    check(p->scroll == 50, "keeps the place, and is not sent back to the target");
    open_page("https://a.test/d/tall.html#end", tall);
    check(asked("https://a.test/d/a.css"), "a sheet that answers before the page was ever laid out");
    answer("p{margin:0}", "text/css");
    p = wb_current(&b);
    wb_paint(&b, screen, W);
    check(p->scroll > 0, "the target still counts");
    p->scroll = 120;
    wb_key(&b, 18);
    {
        struct wb_request one;
        check(wb_take(&b, &one) && one.reload, "reloaded at a place");
        wb_loaded(&b, page_at(one.url, tall, 0), one.reload);
    }
    check(asked("https://a.test/d/a.css"), "its sheet again");
    answer("p{margin:0}", "text/css");
    p = wb_current(&b);
    wb_paint(&b, screen, W);
    check(p->scroll == 120, "keeps that place");
    open_page("https://a.test/d/keep.html", "<link rel=stylesheet href=a.css><p>x<p>y<p>z<p>w<p>v");
    p = wb_current(&b);
    check(asked("https://a.test/d/a.css"), "a page read a little way");
    p->scroll = 20;
    answer("p{color:#ff0000}", "text/css");
    check(p->scroll == 20, "keeps its place");

    /* the budget of the history */
    open_page("https://a.test/d/bytes.html", "<link rel=stylesheet href=a.css><p>x");
    p = wb_current(&b);
    {
        size_t held = p->bytes;
        check(asked("https://a.test/d/a.css"), "bytes held while a sheet is on its way");
        answer("p{color:#ff0000}", "text/css");
        check(p->bytes < held && p->bytes == sizeof(*p) + p->doc.bytes, "are given back");
    }

    /* every allocation refused in turn: the page stands, and nothing is left */
    wb_free(&b);
    {
        long before = ut_alloc_live;
        int done = 0;
        for(i = 0; i < 200 && !done; i++) {
            check(!wb_init(&b, W, H), "wb_init");
            open_page("https://a.test/d/alloc.html", "<link rel=stylesheet href=a.css><link rel=stylesheet href=b.css><p>x");
            ut_alloc_fail_after = i;
            p = wb_current(&b);
            if(next()) {
                struct wb_request one = q;
                if(next()) {
                    answer("@import \"c.css\"; p{color:#0000b0}", "text/css");
                    q = one;
                }
                answer("@import \"d.css\"; p{color:#0000a0}", "text/css");
                while(next()) answer("p{font-weight:bold}", "text/css");
            }
            done = ut_alloc_fail_after > 0;
            ut_alloc_fail_after = -1;
            check(run("x") != NULL, "the page stands");
            wb_paint(&b, screen, W);
            wb_free(&b);
        }
        check(done && i > 3, "every allocation refused in turn");
        check(ut_alloc_live == before, "and every one given back");
    }

    if(failures) {
        printf("WUNDERTOW SHEETS TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW SHEETS TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
