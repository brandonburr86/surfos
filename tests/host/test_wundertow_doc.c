/*
SurfOS Undertow's window: tests of a page's boxes
----------------------
File: test_wundertow_doc.c  Date: 10/25/26
----------------------
user/wundertow/doc.c (with html_scan.c, url.c and util.c) on the host:

- the fixtures in tests/fixtures/wundertow/: NAME.html (NAME.body when it is
  not HTML; NAME.meta with url:, type: and charset: lines when the page needs
  them) and NAME.tree, what wd_dump() must make of it;
- every page of Undertow's own fixtures (tests/fixtures/undertow/), each of
  which must make a sound tree: its links within the tree, every box with
  boxes or runs but not both, the runs and their text within their arrays,
  each run in one box, nothing unsafe in any string;
- pages made here: whitespace, styles, lists, tables, links and the base,
  anchors, hidden content, pre, fields, colours, the limits, and every
  allocation failing in turn, with no leak.

"test_wundertow_doc --dump FILE [URL [TYPE [CHARSET]]]" prints the tree of any
file, as a fixture's NAME.tree has it.
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
#include "alloc.h"

#define PAGE_URL "https://example.test/dir/page.html"

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(ok) return;
    failures++;
    printf("FAIL %s\n", what);
}

unsigned int uptime(void) { return 0; }        /* util.c's ut_now(); building never waits */
time_t ut_sim_time(time_t *t) { if(t) *t = 0; return 0; }

/**** responses and documents ****/

static struct ut_response *response(const char *url, const char *type, const char *charset, const void *body, size_t len) {
    struct ut_response *r = calloc(1, sizeof(*r));
    char err[UT_ERR_MAX];
    if(!r || ut_url_parse(url, &r->url, err, sizeof(err))) {
        printf("test setup: bad URL %s\n", url);
        exit(2);
    }
    r->body = malloc(len + 1);
    if(!r->body) exit(2);
    if(len) memcpy(r->body, body, len);
    r->body[len] = 0;
    r->len = len;
    r->status = 200;
    snprintf(r->content_type, sizeof(r->content_type), "%s", type);
    snprintf(r->charset, sizeof(r->charset), "%s", charset);
    return r;
}

static void response_free(struct ut_response *r) {
    if(!r) return;
    free(r->body);
    free(r);
}

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

/* "key: value" from a .meta or .doc file's lines, "" when it has none */
static void meta_value(const char *meta, const char *key, char *out, size_t cap) {
    size_t k = strlen(key);
    const char *p = meta;
    out[0] = 0;
    while(p && *p) {
        if(!strncmp(p, key, k) && p[k] == ':') {
            const char *v = p + k + 1, *e;
            while(*v == ' ') v++;
            e = strchr(v, '\n');
            if(!e) e = v + strlen(v);
            snprintf(out, cap, "%.*s", (int)(e - v), v);
            return;
        }
        p = strchr(p, '\n');
        if(p) p++;
    }
}

/**** what every document must be ****/

static int safe_utf8(const char *s, size_t len) {
    size_t i = 0, n;
    unsigned int cp;
    while(i < len) {
        n = ut_utf8_decode((const unsigned char *)s + i, len - i, &cp);
        if(n == 1 && (unsigned char)s[i] >= 0x80) return 0;
        if(ut_unsafe_char(cp) || cp == '\n' || cp == '\t') return 0;
        i += n;
    }
    return 1;
}

static char sound_why[200];

static int unsound(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sound_why, sizeof(sound_why), fmt, ap);
    va_end(ap);
    return 0;
}

/* The tree and every string are as doc.h says */
static int sound(const struct wd_document *d) {
    unsigned char *owner;
    size_t i, r;
    int c, count;
    if(!d->block_count || d->blocks[0].parent != -1) return unsound("no root");
    if(!d->text || d->text[d->text_len] || !d->title) return unsound("no text or title");
    if(!safe_utf8(d->text, d->text_len)) return unsound("unsafe text");
    if(!safe_utf8(d->title, strlen(d->title)) || !safe_utf8(d->notice, strlen(d->notice))) return unsound("unsafe title");
    owner = calloc(d->run_count + 1, 1);
    if(!owner) exit(2);
    for(i = 0; i < d->block_count; i++) {
        const struct wd_block *x = &d->blocks[i];
        if(i && (x->parent < 0 || (size_t)x->parent >= d->block_count)) return free(owner), unsound("box %u: parent", (unsigned)i);
        if(x->first >= 0 && x->nruns) return free(owner), unsound("box %u: boxes and runs", (unsigned)i);
        if(x->run + x->nruns > d->run_count) return free(owner), unsound("box %u: runs past the end", (unsigned)i);
        for(r = x->run; r < x->run + x->nruns; r++) {
            if(owner[r]) return free(owner), unsound("run %u in two boxes", (unsigned)r);
            owner[r] = 1;
        }
        count = 0;
        for(c = x->first; c >= 0; c = d->blocks[c].next) {
            if((size_t)c >= d->block_count || d->blocks[c].parent != (int)i) return free(owner), unsound("box %u: child", (unsigned)i);
            if(++count > (int)d->block_count) return free(owner), unsound("box %u: a loop", (unsigned)i);
            if(d->blocks[c].next < 0 && c != x->last) return free(owner), unsound("box %u: last", (unsigned)i);
        }
        if(x->kind == WB_CELL && d->blocks[x->parent].kind != WB_ROW) return free(owner), unsound("a cell outside a row");
        if(x->kind == WB_ROW && d->blocks[x->parent].kind != WB_TABLE) return free(owner), unsound("a row outside a table");
    }
    for(r = 0; r < d->run_count; r++) {
        const struct wd_run *u = &d->runs[r];
        if(!owner[r]) return free(owner), unsound("run %u in no box", (unsigned)r);
        if(u->off + u->len > d->text_len) return free(owner), unsound("run %u: text past the end", (unsigned)r);
        if(u->link >= (int)d->link_count || u->link < -1) return free(owner), unsound("run %u: link", (unsigned)r);
        if(u->image >= (int)d->image_count || u->image < -1) return free(owner), unsound("run %u: image", (unsigned)r);
        if(u->kind == WR_BR && u->len) return free(owner), unsound("run %u: a break with text", (unsigned)r);
    }
    free(owner);
    for(i = 0; i < d->link_count; i++) {
        const struct ut_link *l = &d->links[i];
        if(!l->url || !l->label || !safe_utf8(l->url, strlen(l->url)) || !safe_utf8(l->label, strlen(l->label)))
            return unsound("link %u", (unsigned)i);
        if(l->usable && strncmp(l->url, "http://", 7) && strncmp(l->url, "https://", 8)) return unsound("link %u usable", (unsigned)i);
    }
    for(i = 0; i < d->anchor_count; i++)
        if(d->anchors[i].offset > d->text_len || !safe_utf8(d->anchors[i].name, strlen(d->anchors[i].name)))
            return unsound("anchor %u", (unsigned)i);
    for(i = 0; i < d->image_count; i++)
        if(d->images[i] && strncmp(d->images[i], "http://", 7) && strncmp(d->images[i], "https://", 8))
            return unsound("image %u", (unsigned)i);
    return 1;
}

static int build(const struct ut_response *r, struct wd_document *d, const char *what) {
    char err[UT_ERR_MAX], msg[UT_ERR_MAX + 400];
    int rc = wd_build(r, d, err, sizeof(err));
    snprintf(msg, sizeof(msg), "%s: wd_build (%s)", what, rc ? err : "ok");
    check(rc == 0, msg);
    if(rc) return -1;
    snprintf(msg, sizeof(msg), "%s: a sound tree (%s)", what, sound(d) ? "yes" : sound_why);
    check(sound(d), msg);
    return 0;
}

/**** fixtures ****/

static void same(const char *name, const char *got, const char *want) {
    const char *g = got, *w = want;
    unsigned int line = 1;
    char msg[300];
    snprintf(msg, sizeof(msg), "fixture %s: its tree", name);
    if(!strcmp(got, want)) {
        check(1, msg);
        return;
    }
    while(*g && *g == *w) {
        if(*g == '\n') line++;
        g++;
        w++;
    }
    while(g > got && g[-1] != '\n') g--;
    while(w > want && w[-1] != '\n') w--;
    check(0, msg);
    printf("  line %u\n  got:  %.*s\n  want: %.*s\n", line, (int)strcspn(g, "\n"), g, (int)strcspn(w, "\n"), w);
}

static const char *const fixtures[] = {
    "basics", "styles", "lists", "tables", "links", "pre", "forms", "colors", "hidden"
};

static struct ut_response *fixture_response(const char *dir, const char *name, int undertow) {
    char path[600], url[UT_URL_MAX + 1], type[64], charset[32];
    char *body, *meta;
    size_t len;
    struct ut_response *r;
    int html = 1;
    snprintf(path, sizeof(path), "%s/%s.html", dir, name);
    body = read_file(path, &len);
    if(!body) {
        snprintf(path, sizeof(path), "%s/%s.body", dir, name);
        body = read_file(path, &len);
        html = 0;
    }
    if(!body) return NULL;
    snprintf(path, sizeof(path), "%s/%s.%s", dir, name, undertow ? "doc" : "meta");
    meta = read_file(path, NULL);
    meta_value(meta ? meta : "", "url", url, sizeof(url));
    meta_value(meta ? meta : "", "type", type, sizeof(type));
    meta_value(meta ? meta : "", "charset", charset, sizeof(charset));
    if(!meta && html) snprintf(type, sizeof(type), "text/html");
    r = response(url[0] ? url : PAGE_URL, type, charset, body, len);
    free(body);
    free(meta);
    return r;
}

static void fixture(const char *dir, const char *name) {
    char path[600], msg[1200], *want, *got;
    struct ut_response *r = fixture_response(dir, name, 0);
    struct wd_document d;
    snprintf(path, sizeof(path), "%s/%s.tree", dir, name);
    want = read_file(path, NULL);
    snprintf(msg, sizeof(msg), "fixture %s: its files in %s", name, dir);
    check(r && want, msg);
    if(r && want && build(r, &d, name) == 0) {
        got = wd_dump(&d);
        check(got != NULL, "wd_dump");
        if(got) same(name, got, want);
        free(got);
        wd_free(&d);
    }
    response_free(r);
    free(want);
}

static const char *const undertow_fixtures[] = {
    "paragraphs", "headings", "lists", "pre", "tables", "images", "forms", "comments", "hidden",
    "attributes", "mismatched", "links", "anchors", "base", "entities", "utf8", "unknown-charset", "plain",
    "sniff", "xhtml", "article"
};

static void undertow_fixture(const char *dir, const char *name) {
    struct ut_response *r = fixture_response(dir, name, 1);
    struct wd_document d;
    char msg[300];
    snprintf(msg, sizeof(msg), "Undertow's fixture %s", name);
    check(r != NULL, msg);
    if(r && build(r, &d, msg) == 0) wd_free(&d);
    response_free(r);
}

static int dump_file(int argc, char **argv) {
    size_t len;
    char *body = read_file(argv[2], &len), *tree, err[UT_ERR_MAX];
    struct ut_response *r;
    struct wd_document d;
    if(!body) {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }
    r = response(argc > 3 ? argv[3] : PAGE_URL, argc > 4 ? argv[4] : "text/html", argc > 5 ? argv[5] : "", body, len);
    if(wd_build(r, &d, err, sizeof(err))) {
        fprintf(stderr, "wd_build: %s\n", err);
        return 1;
    }
    if(!sound(&d)) fprintf(stderr, "unsound: %s\n", sound_why);
    tree = wd_dump(&d);
    fputs(tree ? tree : "(no memory)\n", stdout);
    free(tree);
    wd_free(&d);
    response_free(r);
    free(body);
    return 0;
}

/**** pages made here ****/

static struct wd_document pg;
static int pg_ok;
static char *pg_tree;

static void page_as(const char *type, const char *charset, const char *body, size_t len, const char *what) {
    struct ut_response *r = response(PAGE_URL, type, charset, body, len);
    wd_free(&pg);
    free(pg_tree);
    pg_tree = NULL;
    pg_ok = build(r, &pg, what) == 0;
    if(pg_ok) pg_tree = wd_dump(&pg);
    response_free(r);
}

static void page(const char *html, const char *what) { page_as("text/html", "", html, strlen(html), what); }

/* the page's tree has these lines, in this order (each one a whole line, without its indentation) */
static void has(const char *what, ...) {
    va_list ap;
    const char *line, *at;
    char msg[400];
    va_start(ap, what);
    at = pg_tree ? pg_tree : "";
    while((line = va_arg(ap, const char *)) != NULL) {
        const char *p = at;
        size_t n = strlen(line);
        int found = 0;
        while((p = strstr(p, line)) != NULL) {
            const char *s = p;
            while(s > pg_tree && s[-1] == ' ') s--;
            if((s == pg_tree || s[-1] == '\n') && (p[n] == '\n' || !p[n])) {
                found = 1;
                at = p + n;
                break;
            }
            p += n;
        }
        snprintf(msg, sizeof(msg), "%s: has \"%s\"", what, line);
        check(found, msg);
        if(!found) {
            printf("  the tree:\n%s", pg_tree ? pg_tree : "(none)\n");
            break;
        }
    }
    va_end(ap);
}

static void lacks(const char *what, const char *s) {
    char msg[400];
    snprintf(msg, sizeof(msg), "%s: lacks \"%s\"", what, s);
    check(pg_tree && !strstr(pg_tree, s), msg);
}

static void made_pages(void) {
    char *big;
    size_t i, n;

    page("<p>  a   b\n\t c  </p>", "whitespace");
    has("whitespace", "block para", "text \"a b c\"", NULL);

    page("<p>a <b>b <i>c</i></b> d</p>", "styles");
    has("styles", "text \"a \"", "text b \"b \"", "text b i \"c\"", "text \" d\"", NULL);

    page("<p>x<sub>2</sub> <code>int</code> <u>u</u><s>s</s><nobr>no wrap</nobr><mark>m</mark>"
         "<big>big</big><small>small</small> <kbd>k</kbd><del>d</del><ins>i</ins><cite>c</cite></p>", "more styles");
    has("more styles", "text sub \"2\"", "text mono \"int\"", "text u \"u\"", "text s \"s\"", "text nowrap \"no wrap\"",
        "text mark \"m\"", "text big \"big\"", "text small \"small\"", "text mono \"k\"", "text s \"d\"", "text u \"i\"",
        "text i \"c\"", NULL);

    page("<h1>One<h2>Two</h2><h3 align=center>Three</h3>", "headings");
    has("headings", "block h1 para", "text b h1 \"One\"", "block h2 para", "text b h2 \"Two\"",
        "block h3 para center", "text b h3 \"Three\"", NULL);

    page("<ul><li>one<li>two<ul><li>x<ul><li>y</ul></ul></ul><ol start=3><li>a<li value=10>b<li>c</ol>"
         "<ol type=i><li>r</ol><li>lone", "lists");
    has("lists", "block para list", "item disc", "text \"one\"", "item disc", "text \"two\"", "block list",
        "item circle", "text \"x\"", "block list", "item square", "text \"y\"", "block para list", "item decimal 3",
        "item decimal 10", "item decimal 11", "item lower-roman 1", "item indent disc", "text \"lone\"", NULL);

    page("<table border=1 cellpadding=4 width=80%><caption>Cap</caption><tr bgcolor=#ff0000><th>h<td colspan=2 width=50>b"
         "<tr><td>c</table><table><td>implied</td></table>", "tables");
    has("tables", "table border=1 padding=4 width=80%", "block caption center", "text \"Cap\"", "row bg=ff0000",
        "cell header center", "text b \"h\"", "cell colspan=2 width=50", "text \"b\"", "row", "cell", "text \"c\"",
        "table border=0 padding=1", "row", "cell", "text \"implied\"", NULL);

    page("<table><tr><td><b>bold<td>plain</table>", "a cell gives back the style around its table");
    has("a cell gives back the style around its table", "text b \"bold\"", "text \"plain\"", NULL);

    page("<li>text<div>block</div>more</li>", "anonymous boxes");
    has("anonymous boxes", "item indent disc", "block anon", "text \"text\"", "block", "text \"block\"",
        "block anon", "text \"more\"", NULL);

    page("<base href=\"https://other.test/base/\"><a href=a.html>A</a> <a href=#f>F</a> <a href=mailto:x@y.test>M</a>"
         " <a href=\" javascript:x() \">J</a><a href=\"https://three.test/\"><a href=b.html>nested</a>", "links");
    has("links", "text \"A\" link=0", "text \"F\" link=1", "text \"M\" link=2", "text \"J\" link=3",
        "text \"nested\" link=5", "link 0 \"https://other.test/base/a.html\" \"A\"",
        "link 1 \"https://example.test/dir/page.html#f\" \"F\"", "link 2 unusable \"mailto:x@y.test\" \"M\"",
        "link 3 unusable \"javascript:x()\" \"J\"", "link 4 \"https://three.test/\" \"\"",
        "link 5 \"https://other.test/base/b.html\" \"nested\"", NULL);

    page("<a href=late.html>L</a><base href=\"https://late.test/\">", "a late base counts for the links before it");
    has("a late base counts for the links before it", "link 0 \"https://late.test/late.html\" \"L\"", NULL);

    page("<a href=x title=\"The title\"><img src=i.png width=20 height=10></a>", "a link that shows no text");
    has("a link that shows no text", "image 20x10 src=https://example.test/dir/i.png \"\" link=0",
        "link 0 \"https://example.test/dir/x\" \"The title\"", NULL);

    page("<p id=top>x</p><a name=n></a><p id=two>y<span id=two>z</span>", "anchors");
    has("anchors", "anchor top 0", "anchor n 1", "anchor two 1", NULL);
    {
        size_t off = 99;
        check(pg_ok && !wd_anchor_offset(&pg, "two", &off) && off == 1, "anchors: two");
        check(pg_ok && !wd_anchor_offset(&pg, "", &off) && off == 0, "anchors: the top");
        check(pg_ok && wd_anchor_offset(&pg, "missing", &off) == -1, "anchors: one the page has not");
        /* 10/25/26 (the review): as Undertow (ut_anchor_offset): escapes decoded, never past a name's end for %00 */
        check(pg_ok && !wd_anchor_offset(&pg, "%74wo", &off) && off == 1, "anchors: escapes decoded");
        check(pg_ok && wd_anchor_offset(&pg, "n%00", &off) == -1 && wd_anchor_offset(&pg, "n%00x", &off) == -1,
              "anchors: %00 matches no name's end");
    }
    page("<p>one</p><p>two</p><p id=top>three", "an anchor named top");
    {
        size_t off = 99;
        check(pg_ok && !wd_anchor_offset(&pg, "top", &off) && off == 6, "anchors: the page's own top before the top");
        check(pg_ok && !wd_anchor_offset(&pg, "TOP", &off) && off == 0, "anchors: TOP, no anchor's name: the top");
    }

    page("<p>a<script>no()</script><style>p{}</style><template>no</template><svg><text>no</text></svg>"
         "<div hidden>no<div>no</div>no</div>b<object>no</object><noscript>yes</noscript>"
         "<math><mi>no</mi><p>out</p></math>", "hidden content");
    has("hidden content", "text \"abyes\"", "text \"out\"", NULL);
    lacks("hidden content", "no");

    page("<pre>\nline 1\n\tx\ty\n<b>bold</b></pre>", "pre");
    has("pre", "block para pre", "text mono pre \"line 1\"", "br", "text mono pre \"        x       y\"", "br",
        "text b mono pre \"bold\"", NULL);

    page("<p><img alt=\" An  image \" src=\"pic.png\"> <img src=decor.png> <img width=5 height=6>"
         "<input value=v> <input placeholder=\"Type here\"> <input type=checkbox checked> <input type=radio>"
         " <input type=submit> <input type=hidden value=secret> <input type=password value=abc> <input type=file>"
         " <button>Press <b>me</b></button> <select><option>First<option>Second</select>"
         " <textarea>Some\ntext</textarea>", "fields and images");
    has("fields and images", "image src=https://example.test/dir/pic.png \"An image\"", "image 5x6 \"\"",
        "text field \"v\"", "text field checked \"Type here\"", "checkbox field checked \"\"", "radio field \"\"",
        "button field \"Submit\"", "password field \"***\"", "file field \"Choose a file\"",
        "button field \"Press me\"", "select field \"First\"", "area field \"Some text\"", NULL);
    lacks("fields and images", "secret");

    page("<body bgcolor=white text=\"#123\" link=navy><font color=red>r<font>still red<font color=blue>b</font>r</font>"
         "</font>plain", "colours");
    has("colours", "colors bg=ffffff fg=112233 link=000080", "text color=ff0000 \"rstill red\"",
        "text color=0000ff \"b\"", "text color=ff0000 \"r\"", "text \"plain\"", NULL);
    check(wd_color("#AbCdEf") == 0xabcdef && wd_color("abcdef") == 0xabcdef && wd_color(" Teal ") == 0x008080 &&
          wd_color("#abc") == 0xaabbcc && wd_color("nonsense") == WD_NONE && wd_color("#12") == WD_NONE, "wd_color");

    page_as("text/plain", "", "plain\ttext\n<b>not bold</b>", 26, "plain text");
    has("plain text", "block pre", "text mono pre \"plain   text\"", "br", "text mono pre \"<b>not bold</b>\"", NULL);

    page_as("text/html", "", "\xff\xfe<\0p\0>\0\xe9\0", 10, "UTF-16");
    has("UTF-16", "text \"\xc3\xa9\"", NULL);

    page_as("text/html", "shift_jis", "<p>a\x82\xa0z", 7, "a charset Undertow cannot read");
    has("a charset Undertow cannot read", "text \"a??z\"",
        "notice the page says it is in shift_jis; only its ASCII is shown", NULL);

    page("<p>\x1b[31mred\xe2\x80\xaeoverride\xc2\x9b" "c1&#0;&#x202e;<b>\x01</b>", "unsafe characters never reach the text");
    has("unsafe characters never reach the text", "text \"[31mredoverridec1\xef\xbf\xbd\"", NULL);

    /* the limits */
    n = 600 * 1024;
    big = malloc(n + 16);
    if(!big) exit(2);
    memcpy(big, "<p>", 3);
    for(i = 3; i < n; i++) big[i] = (char)('a' + i % 26);
    big[n] = 0;
    page(big, "a page past 512 KB of text");
    check(pg_ok && pg.text_len <= UT_TEXT_MAX && (pg.truncated & UT_TRUNC_TEXT) && strstr(pg.notice, "512 KB"),
          "a page past 512 KB of text: cut there, with a notice");
    free(big);

    n = 40000;
    big = malloc(n * 8 + 16);
    if(!big) exit(2);
    for(i = 0; i < n; i++) memcpy(big + i * 8, "<p>x</p>", 8);
    big[n * 8] = 0;
    page(big, "more boxes than WD_BLOCKS_MAX");
    check(pg_ok && pg.block_count <= WD_BLOCKS_MAX && (pg.truncated & WD_TRUNC_BOXES), "boxes: stopped at the limit");
    free(big);

    n = 70000;
    big = malloc(n * 10 + 16);
    if(!big) exit(2);
    for(i = 0; i < n; i++) memcpy(big + i * 9, "<b>x</b>y", 9);
    big[n * 9] = 0;
    page(big, "more runs than WD_RUNS_MAX");
    check(pg_ok && pg.run_count <= WD_RUNS_MAX && (pg.truncated & WD_TRUNC_BOXES), "runs: stopped at the limit");
    free(big);

    n = 3000;
    big = malloc(n * 16 + 16);
    if(!big) exit(2);
    for(i = 0; i < n; i++) memcpy(big + i * 15, "<a href=x>l</a>", 15);
    big[n * 15] = 0;
    page(big, "more links than UT_LINKS_MAX");
    check(pg_ok && pg.link_count == UT_LINKS_MAX && (pg.truncated & UT_TRUNC_LINKS), "links: 2048, then text only");
    free(big);

    n = 300;
    big = malloc(n * 12 + 16);
    if(!big) exit(2);
    for(i = 0; i < n; i++) memcpy(big + i * 11, "<div><span>", 11);
    big[n * 11] = 0;
    page(big, "elements deeper than the stack");
    check(pg_ok, "deep elements: only counted");

    wd_free(&pg);
    free(pg_tree);
    pg_tree = NULL;
}

/* every allocation failing in turn: a failure or a document, and nothing left allocated after it */
static void allocation_failures(void) {
    static const char page_text[] =
        "<html><head><title>T</title><base href=\"https://b.test/\"></head><body bgcolor=red><h1 id=h>Head</h1>"
        "<p>a <b>b</b> <a href=x title=t>link</a><img alt=i src=s.png><ul><li>one<li>two</ul>"
        "<table border=1><tr><td>c<td>d</table><pre>p\tq\n</pre><select><option>o</select><p id=x>end";
    struct ut_response *r = response(PAGE_URL, "text/html", "", page_text, sizeof(page_text) - 1);
    struct wd_document d;
    char err[UT_ERR_MAX], msg[120];
    long k, base, failed = 0, made = 0;
    for(k = 0; k < 2000; k++) {
        base = ut_alloc_live;
        ut_alloc_fail_after = k;
        if(wd_build(r, &d, err, sizeof(err)) == 0) {
            ut_alloc_fail_after = -1;
            made++;
            wd_free(&d);
            if(made == 1) {
                snprintf(msg, sizeof(msg), "allocation failures: the first success after %ld, nothing left", k);
                check(ut_alloc_live == base, msg);
                break;
            }
        } else {
            ut_alloc_fail_after = -1;
            failed++;
            if(ut_alloc_live != base) {
                snprintf(msg, sizeof(msg), "allocation %ld failing leaves %ld blocks", k, ut_alloc_live - base);
                check(0, msg);
                break;
            }
        }
    }
    ut_alloc_fail_after = -1;
    check(failed > 10 && made == 1, "allocation failures: each one fails cleanly until there is memory enough");
    response_free(r);
}

int main(int argc, char **argv) {
    const char *top = argc > 1 ? argv[1] : "tests/fixtures";
    char dir[512], udir[512];
    size_t i;
    if(argc > 2 && !strcmp(argv[1], "--dump")) return dump_file(argc, argv);
    snprintf(dir, sizeof(dir), "%s/wundertow", top);
    for(i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) fixture(dir, fixtures[i]);
    snprintf(udir, sizeof(udir), "%s/undertow", top);
    for(i = 0; i < sizeof(undertow_fixtures) / sizeof(undertow_fixtures[0]); i++) undertow_fixture(udir, undertow_fixtures[i]);
    made_pages();
    allocation_failures();
    if(failures) {
        printf("WUNDERTOW DOC TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW DOC TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
