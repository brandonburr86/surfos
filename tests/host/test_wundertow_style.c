/*
SurfOS Undertow's window: tests of a page's CSS on its boxes and runs
----------------------
File: test_wundertow_style.c  Date: 11/01/26
----------------------
user/wundertow/docstyle.c through doc.c and layout.c on the host, on pages made here: which rule wins (specificity,
order, !important, style attributes, HTML's own attributes below them all), what is inherited, selectors against the
open elements, what display does to boxes (none with an end tag or without, block on what has no box, inline on what
has one), margins, paddings and backgrounds where layout.c puts them, sizes, the page's root, where sheets come from.
A malformed sheet changes nothing on a page, a hostile one is stopped, and every allocation may fail.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "undertow.h"
#include "url.h"
#include "http.h"
#include "doc.h"
#include "docstyle.h"
#include "layout.h"
#include "alloc.h"

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

static struct wd_document d;
static struct wl_layout l;
static int have, laid;

static int build(const char *html, size_t len, struct wd_document *out, const char *charset) {
    struct ut_response r;
    char err[UT_ERR_MAX];
    int rc;
    memset(&r, 0, sizeof(r));
    if(ut_url_parse(PAGE_URL, &r.url, err, sizeof(err))) return -1;
    r.body = malloc(len + 1);
    if(!r.body) return -1;
    memcpy(r.body, html, len);
    r.body[len] = 0;
    r.len = len;
    r.status = 200;
    snprintf(r.content_type, sizeof(r.content_type), "text/html");
    snprintf(r.charset, sizeof(r.charset), "%s", charset);
    rc = wd_build(&r, out, err, sizeof(err));
    free(r.body);
    return rc;
}

static void page(const char *html) {
    if(laid) wl_free(&l);
    if(have) wd_free(&d);
    laid = 0;
    have = !build(html, strlen(html), &d, "");
    if(!have) printf("no page from: %s\n", html);
}

static void laid_out(const char *html, int width) {
    page(html);
    laid = have && !wl_layout(&d, width, &l);
}

/* the run whose text holds s */
static const struct wd_run *run(const char *s) {
    size_t i, n = strlen(s);
    for(i = 0; have && i < d.run_count; i++) {
        const struct wd_run *r = &d.runs[i];
        const char *p;
        if(r->kind != WR_TEXT) continue;
        for(p = d.text + r->off; p + n <= d.text + r->off + r->len; p++) if(!memcmp(p, s, n)) return r;
    }
    return NULL;
}

/* the box whose runs hold s */
static const struct wd_block *box(const char *s) {
    const struct wd_run *r = run(s);
    size_t i;
    if(!r) return NULL;
    for(i = 0; i < d.block_count; i++) {
        const struct wd_block *x = &d.blocks[i];
        if((size_t)(r - d.runs) >= x->run && (size_t)(r - d.runs) < x->run + x->nruns) return x;
    }
    return NULL;
}

/* the text item holding s */
static const struct wl_item *item(const char *s) {
    size_t i, n = strlen(s);
    for(i = 0; laid && i < l.count; i++) {
        const struct wl_item *it = &l.items[i];
        const char *p;
        if(it->kind != WI_TEXT) continue;
        for(p = d.text + it->off; p + n <= d.text + it->off + it->len; p++) if(!memcmp(p, s, n)) return it;
    }
    return NULL;
}

static const struct wl_item *fill(unsigned int color) {
    size_t i;
    for(i = 0; laid && i < l.count; i++) if(l.items[i].kind == WI_FILL && l.items[i].color == color) return &l.items[i];
    return NULL;
}

static unsigned int color_of(const char *s) {
    const struct wd_run *r = run(s);
    return r ? r->color : 0xbad;
}

static int style_of(const char *s) {
    const struct wd_run *r = run(s);
    return r ? r->style : -1;
}

static void cascade(void) {
    page("<style>p{color:red}</style><p>x");
    check(color_of("x") == 0xff0000, "a <style> rule");
    page("<p style=\"color:blue\">x");
    check(color_of("x") == 0x0000ff, "a style attribute");
    page("<p>x");
    check(color_of("x") == WD_NONE && !run("x")->px && run("x")->bg == WD_NONE, "no CSS: nothing on a run");

    page("<style>#a{color:red} .b{color:blue} p{color:lime}</style><p id=a class=b>x");
    check(color_of("x") == 0xff0000, "an id beats a class beats a type");
    page("<style>.b{color:blue} p{color:lime}</style><p class=b>x");
    check(color_of("x") == 0x0000ff, "a class beats a later type");
    page("<style>p.b{color:blue} .b{color:lime}</style><p class=b>x");
    check(color_of("x") == 0x0000ff, "two simple selectors beat a later one");
    page("<style>p{color:blue} p{color:lime}</style><p>x");
    check(color_of("x") == 0x00ff00, "the later of two equals");
    page("<style>p{color:blue;color:lime}</style><p>x");
    check(color_of("x") == 0x00ff00, "the later in a rule");
    page("<style>p{color:lime !important} #a{color:red}</style><p id=a>x");
    check(color_of("x") == 0x00ff00, "!important beats specificity");
    page("<style>#a{color:red}</style><p id=a style=color:blue>x");
    check(color_of("x") == 0x0000ff, "a style attribute beats an id");
    page("<style>p{color:lime !important}</style><p style=color:blue>x");
    check(color_of("x") == 0x00ff00, "!important in a sheet beats a style attribute");
    page("<style>p{color:lime !important}</style><p style=\"color:blue !important\">x");
    check(color_of("x") == 0x0000ff, "both !important: the attribute");
    page("<style>p{color:lime !important} p{color:red}</style><p>x");
    check(color_of("x") == 0x00ff00, "!important beats a later rule");
    page("<style>p{color:red; color:notacolor}</style><p>x");
    check(color_of("x") == 0xff0000, "what is not a colour counts for nothing");

    /* inheritance */
    page("<style>div{color:red}</style><div><p>x<b>y</b></div>");
    check(color_of("x") == 0xff0000 && color_of("y") == 0xff0000, "inherited through boxes and inline elements");
    page("<style>div{color:red} p{color:blue}</style><div><p>x</p>y</div>");
    check(color_of("x") == 0x0000ff && color_of("y") == 0xff0000, "its own beats what it inherits, and it ends with it");
    page("<style>body{font-weight:bold}</style><p>x");
    check(style_of("x") & WS_BOLD, "from the body");
    page("<div style=color:red><span style=color:lime>x</span>y</div>");
    check(color_of("x") == 0x00ff00 && color_of("y") == 0xff0000, "a span's end gives the colour back");

    /* selectors against what is open */
    page("<style>div p{color:red}</style><div><section><p>x</section></div><p>y");
    check(color_of("x") == 0xff0000 && color_of("y") == WD_NONE, "descendant");
    page("<style>div>p{color:red}</style><div><p>x</p><section><p>y</section></div>");
    check(color_of("x") == 0xff0000 && color_of("y") == WD_NONE, "child");
    page("<style>section>div span{color:red}</style><section><div><article><div><span>x</span></div></article></div></section>");
    check(color_of("x") == 0xff0000, "a run that does not hold on the nearest element holds further up");
    page("<style>nav>div>p span{color:red}</style><nav><section><div><p><span>x</span></div></section></nav>");
    check(color_of("x") == WD_NONE, "and one that nowhere holds");
    page("<style>h1,h2 , h3{color:red}</style><h2>x</h2>");
    check(color_of("x") == 0xff0000, "grouping");
    page("<style>.a.b{color:red}</style><p class=\"b  c\na\">x<p class=a>y");
    check(color_of("x") == 0xff0000 && color_of("y") == WD_NONE, "classes, however they are spaced");
    page("<style>.A{color:red}</style><p class=a>x<p class=A>y");
    check(color_of("x") == WD_NONE && color_of("y") == 0xff0000, "a class's case counts");
    page("<style>#A{color:red}</style><p id=a>x<p id=A>y");
    check(color_of("x") == WD_NONE && color_of("y") == 0xff0000, "an id's too");
    page("<style>P{color:red}</style><p>x");
    check(color_of("x") == 0xff0000, "a type's does not");
    page("<STYLE>p{color:red}</STYLE><P>x");
    check(color_of("x") == 0xff0000, "an element in capitals");
    page("<style>*{color:red}</style><p>x");
    check(color_of("x") == 0xff0000, "'*'");
    page("<style>foo-bar{color:red}</style><foo-bar>x</foo-bar>y");
    check(color_of("x") == 0xff0000 && color_of("y") == WD_NONE, "an element HTML does not know");
    page("<style>a-very-long-element-name{color:red}</style><a-very-long-element-name>x</a-very-long-element-name>");
    check(color_of("x") == 0xff0000, "with a long name");
    page("<style>.caf\xc3\xa9{color:red}</style><p class=\"caf\xc3\xa9\">x");
    check(color_of("x") == 0xff0000, "a class in UTF-8");
    page("<style>.w-1\\/2{color:red}</style><p class=\"w-1/2\">x");
    check(color_of("x") == 0xff0000, "a class with an escape");
    page("<style>tbody td{color:red}</style><table><tbody><tr><td>x</table>");
    check(color_of("x") == 0xff0000, "a table's section");
    page("<style>thead td{color:red}</style><table><thead><tr><td>x<tbody><tr><td>y</table>");
    check(color_of("x") == 0xff0000 && color_of("y") == WD_NONE, "one section ends the one before it");
    page("<style>li li{color:red}</style><ul><li>x<li>y<ul><li>z</ul></ul>");
    check(color_of("x") == WD_NONE && color_of("y") == WD_NONE && color_of("z") == 0xff0000, "an item an item ends is no parent");
    page("<style>p span{color:red}</style><p>a<div><span>x</span></div>");
    check(color_of("x") == WD_NONE, "a block that ended a <p> does not count as in it");
    page("<style>a:link{color:red}</style><a href=x>y</a><a name=z>w</a>");
    check(color_of("y") == 0xff0000 && color_of("w") == WD_NONE, ":link: a link, not an anchor");
    page("<style>a:visited{color:red}</style><a href=x>y</a>");
    check(color_of("y") == WD_NONE, ":visited: no link here counts as visited");
    page("<style>a:link{color:blue} a:visited{color:red}</style><a href=x>y</a>");
    check(color_of("y") == 0x0000ff, "a page's two link colours: the unvisited one");
    page("<style>a{color:blue} a:hover{color:red}</style><a href=x>y</a>");
    check(color_of("y") == 0x0000ff, ":hover: nothing is under a pointer here");
}

static void html_below(void) {
    page("<font color=red style=color:blue>x</font>");
    check(color_of("x") == 0x0000ff, "<font color> is below the page's CSS on it");
    page("<font color=red><span style=color:blue>x</span>y</font>");
    check(color_of("x") == 0x0000ff && color_of("y") == 0xff0000, "CSS inside <font>");
    page("<span style=color:blue><font color=red>x</font>y</span>");
    check(color_of("x") == 0xff0000 && color_of("y") == 0x0000ff, "<font> inside CSS: the nearest says");
    page("<span style=color:blue><font size=2>x</font></span>");
    check(color_of("x") == 0x0000ff, "a <font> without a colour says nothing of it");
    page("<table bgcolor=#112233 style=background:#445566><tr bgcolor=#112233 style=background:#445577>"
         "<td bgcolor=#112233 style=background:#445588>x</table>");
    {
        const struct wd_block *c = box("x");
        check(c && c->bg == 0x445588 && d.blocks[c->parent].bg == 0x445577 && d.blocks[d.blocks[c->parent].parent].bg == 0x445566,
              "bgcolor is below the page's CSS");
    }
    page("<td bgcolor=#112233>x");
    page("<table><tr><td bgcolor=#112233 style=background:transparent>x</table>");
    check(box("x") && box("x")->bg == WD_NONE, "background: transparent takes bgcolor away");
    page("<p align=right style=text-align:center>x");
    check(box("x") && box("x")->align == WA_CENTER, "align is below the page's CSS");
    page("<center style=text-align:left>x</center>");
    check(box("x") && box("x")->align == WA_LEFT, "<center> too");
    page("<table align=center style=text-align:right><tr><td>x</table>");
    check(box("x") && box("x")->align == WA_RIGHT && d.blocks[d.blocks[box("x")->parent].parent].align == WA_CENTER,
          "a table's align stays its place, text-align goes to its cells");
    page("<table><tr><th>x</table>");
    check(box("x") && box("x")->align == WA_CENTER && (style_of("x") & WS_BOLD), "a th without CSS");
    page("<style>th{text-align:left;font-weight:normal}</style><table><tr><th>x</table>");
    check(box("x") && box("x")->align == WA_LEFT && !(style_of("x") & WS_BOLD), "a th with");
    page("<style>th{text-align:inherit}</style><table><tr><th>x</table>");
    check(box("x") && box("x")->align == WA_INHERIT, "a th's text-align: inherit");

    /* a link's colour is the link's own */
    page("<div style=color:red><a href=x>y</a></div>");
    check(color_of("y") == WD_NONE, "a link does not take the colour around it");
    page("<style>a{color:inherit}</style><div style=color:red><a href=x>y</a></div>");
    check(color_of("y") == 0xff0000, "color: inherit, when it asks");
    page("<style>a{color:inherit}</style><a href=x>y</a>");
    check(color_of("y") == WD_TEXT, "with no colour around it: the page's text colour");
    page("<style>a{color:inherit}</style><font color=red><a href=x>y</a></font>");
    check(color_of("y") == 0xff0000, "a <font>'s, around it");
    page("<style>a{color:initial}</style><div style=color:red><a href=x>y</a></div>");
    check(color_of("y") == WD_TEXT, "color: initial");
    page("<style>a{color:revert}</style><a href=x>y</a>");
    check(color_of("y") == WD_NONE, "color: revert: the link's own");
    page("<a href=x style=color:red>y<span>z</span></a>");
    check(color_of("y") == 0xff0000 && color_of("z") == 0xff0000 && run("z")->link == 0, "a link's own CSS, into what it holds");

    /* weight, style, lines */
    page("<style>b{font-weight:normal}</style><b>x</b>");
    check(!(style_of("x") & WS_BOLD), "<b> made normal");
    page("<style>h1{font-weight:normal}</style><h1>x</h1>");
    check(!(style_of("x") & WS_BOLD), "a heading made normal");
    page("<h1 style=font-weight:400><b>x</b>y</h1>");
    check((style_of("x") & WS_BOLD) && !(style_of("y") & WS_BOLD), "<b> inside: the nearest says");
    page("<span style=font-weight:bold>x<b style=font-weight:normal>y</b></span>");
    check((style_of("x") & WS_BOLD) && !(style_of("y") & WS_BOLD), "CSS on <b> inside CSS");
    page("<b>x<span style=font-weight:normal>y<i>z</i></span></b>");
    check(!(style_of("y") & WS_BOLD) && !(style_of("z") & WS_BOLD) && (style_of("z") & WS_ITALIC), "normal inside <b>, inherited by what says nothing");
    page("<style>h1{font-weight:inherit}</style><h1>x</h1><b><h2 style=font-weight:inherit>y</h2></b>");
    check(!(style_of("x") & WS_BOLD) && (style_of("y") & WS_BOLD), "font-weight: inherit");
    page("<style>em{font-style:normal}</style><em>x</em>");
    check(!(style_of("x") & WS_ITALIC), "<em> made upright");
    page("<span style=font-style:italic>x</span>");
    check(style_of("x") & WS_ITALIC, "font-style");
    page("<style>address{font-style:normal}</style><address>x</address>");
    check(!(style_of("x") & WS_ITALIC), "<address> made upright");
    page("<a href=x style=text-decoration:none>y</a>");
    check(style_of("y") & WS_NOLINE, "a link without its underline");
    page("<style>a{text-decoration:none}</style><a href=x>y<b>z</b></a>");
    check((style_of("y") & WS_NOLINE) && (style_of("z") & WS_NOLINE), "what it holds too");
    page("<a href=x>y</a>");
    check(!(style_of("y") & (WS_NOLINE | WS_UNDER)), "a link as it was");
    page("<div style=text-decoration:none><a href=x>y</a></div>");
    check(!(style_of("y") & WS_NOLINE), "none around a link says nothing of it");
    page("<a href=x style=text-decoration:none><span style=text-decoration:underline>y</span></a>");
    check((style_of("y") & WS_UNDER), "a line inside such a link");
    page("<u style=text-decoration:none>x</u>");
    check(!(style_of("x") & WS_UNDER), "<u> without its line");
    page("<s style=text-decoration:underline>x</s>");
    check((style_of("x") & WS_UNDER) && !(style_of("x") & WS_STRIKE), "<s> with another");
    page("<span style=\"text-decoration:underline line-through\">x<b>y</b></span>z");
    check((style_of("x") & WS_UNDER) && (style_of("x") & WS_STRIKE) && (style_of("y") & WS_UNDER) && !(style_of("z") & WS_UNDER),
          "lines go on what is inside, and end");
    page("<u>x<span style=text-decoration:none>y</span></u>");
    check(style_of("y") & WS_UNDER, "none does not take away a line from around it");
}

static void display(void) {
    page("<style>.h{display:none}</style><p>a<div class=h>b<p>c</div><p>d");
    check(run("a") && !run("b") && !run("c") && run("d"), "display: none: nothing of it");
    page("<span style=display:none>x</span>y");
    check(!run("x") && run("y"), "on an inline element");
    page("<a href=u style=display:none>x</a>y");
    check(!run("x") && run("y") && !run(" "), "on a link");
    page("<b style=display:none>x</b>y");
    check(!run("x") && run("y") && !(style_of("y") & WS_BOLD), "<b>'s count goes on and ends");
    page("<ul><li style=display:none>a<li>b</ul>");
    check(!run("a") && run("b"), "an item that closes by the next item's start");
    page("<p style=display:none>a<p>b");
    check(!run("a") && run("b"), "a <p> that closes by the next's");
    page("<dl><dd style=display:none>a<dd>b</dl>");
    check(!run("a") && run("b"), "a <dd>");
    page("<table><tr><td style=display:none>a<td>b<tr style=display:none><td>c<tr><td>d</table>");
    check(!run("a") && run("b") && !run("c") && run("d"), "a cell and a row");
    page("<table style=display:none><tr><td>a</table>b");
    check(!run("a") && run("b"), "a table");
    page("<ol><li>a<li style=display:none>b<li>c</ol>");
    check(run("a") && box("c") && box("c")->number == 2, "an item not shown is not counted");
    page("<img src=a.png alt=x style=display:none>y");
    check(!d.image_count && run("y"), "a void element");
    page("<br style=display:none>y<input value=z style=display:none>");
    check(d.run_count == 1 && run("y"), "more of them");
    page("<button style=display:none>x</button>y");
    check(d.run_count == 1 && run("y"), "a button");
    page("<select style=display:none><option>x</select>y");
    check(d.run_count == 1 && run("y"), "a select");
    page("<textarea style=display:none>x</textarea>y");
    check(d.run_count == 1 && run("y"), "a text area");
    page("<div style=display:none><hr><img src=a alt=b><table><tr><td>x</table><ul><li>y</ul></div>z");
    check(d.run_count == 1 && d.block_count <= 2, "nothing inside makes a box");
    page("<div style=display:none><a href=x name=n>y</a></div><a href=#n>z</a>");
    check(!run("y") && d.link_count == 2 && d.anchor_count == 1, "links and anchors inside still count, as Undertow's text has them");
    page("<body style=display:none>x");
    check(!d.run_count, "the page's body: nothing");
    page("<style>body{display:none}</style>x");
    check(!d.run_count, "and with no <body> tag");
    page("<style>html{display:none}</style><body>x");
    check(!d.run_count, "the page's root");
    page("<style>body{display:none}</style><noscript><style>body{display:block}</style></noscript>x");
    check(run("x") != NULL, "a body hidden until a script runs, and shown by <noscript>'s sheet");
    page("<style>.a{display:none}</style><body class=b>x");
    check(run("x") != NULL, "a class the body does not have");
    page("<div hidden style=display:block>x</div>");
    check(run("x") != NULL, "the hidden attribute is below the page's CSS");
    page("<div hidden>x</div><p class=y hidden>z</p>");
    check(!run("x") && !run("z"), "as it was without it");
    page("<style>.s{display:block}</style><div hidden class=s>x</div>");
    check(run("x") != NULL, "and from a sheet");

    /* block on what has no box */
    page("x<span style=display:block>y</span>z");
    check(box("x") != box("y") && box("y") != box("z") && box("y") && box("y")->kind == WB_BLOCK, "block on an inline element: a box");
    page("<a href=u style=display:block>y</a>z");
    check(box("y") != box("z") && run("y")->link == 0 && run("z")->link == -1, "on a link");
    page("<b style=display:block>y</b>z");
    check(box("y") != box("z") && (style_of("y") & WS_BOLD) && !(style_of("z") & WS_BOLD), "on <b>");
    page("<label style=display:block><abbr>x</abbr>y</label>z");
    check(box("x") == box("y") && box("y") != box("z"), "on an element HTML does not know, with another inside");
    page("<img src=a alt=x style=display:block>y");
    check(d.run_count == 2 && d.blocks[1].nruns == 1 && d.runs[d.blocks[1].run].kind == WR_IMAGE, "on a void element");
    page("<span style=\"display:block;background:#123456;margin:5px;text-align:center\">y</span>");
    check(box("y") && box("y")->bg == 0x123456 && box("y")->margin[0] == 5 && box("y")->align == WA_CENTER && run("y")->bg == WD_NONE,
          "a block's background is its box's, not its text's");
    page("<ul style=display:block><li style=display:block>x</ul>");
    check(box("x") && box("x")->marker == WMK_NONE, "an item made a block has no marker");

    /* inline on what has one */
    page("<div style=display:inline>x </div><div style=display:inline>y</div>");
    check(box("x") == box("y") && d.text_len == 3 && !memcmp(d.text, "x y", 3), "inline on a block: no box, its space kept");
    page("<ul><li style=display:inline>x</li> <li style=display:inline>y</li></ul>");
    check(box("x") == box("y") && box("x")->marker == WMK_NONE, "on items: no markers");
    page("<p style=display:inline>x<p style=display:inline>y");
    check(box("x") == box("y"), "a <p> still ends the one before it");
    page("<h1 style=display:inline>x</h1>y");
    check(box("x") == box("y") && (style_of("x") & WS_BOLD) && !(style_of("y") & WS_BOLD) && run("x")->size == WZ_H1, "a heading keeps its look");
    page("<table style=display:inline><tr style=display:inline><td style=display:inline>x</table>");
    check(box("x") && box("x")->kind == WB_CELL, "a table keeps its parts");
    page("<div style=display:block>x</div>");
    check(box("x") && box("x")->kind == WB_BLOCK, "block on a block");
    page("<div style=display:table-cell>x</div>");
    check(box("x") && box("x")->kind == WB_BLOCK, "what it has no use for leaves it as it is");
    page("<style>.a{display:none} .a.b{display:table-cell}</style><div class=\"a b\">x</div>");
    check(run("x") != NULL, "and undoes an earlier none");
}

static void boxes(void) {
    const struct wl_item *a, *b, *f;

    laid_out("<p style=margin:0>x<p style=margin:0>y", 640);
    a = item("x");
    b = item("y");
    check(a && b && a->y == 8 && b->y == 8 + 16, "margins of 0: no space between");
    laid_out("<p style=\"margin-top:40px\">x", 640);
    check(item("x") && item("x")->y == 8 + 40, "a margin above");
    laid_out("<p style=\"margin-bottom:30px\">x<p>y", 640);
    check(item("y") && item("y")->y == item("x")->y + 16 + 30, "two margins meet: the larger");
    laid_out("<p style=\"margin-left:20px;margin-right:400px\">aaaa bbbb", 640);
    check(item("aaaa") && item("aaaa")->x == 8 + 20, "a left margin");
    laid_out("<p style=\"margin-left:50%\">x", 640);
    check(item("x") && item("x")->x == 8 + 312, "percent of the width");
    laid_out("<p style=\"margin-left:-20px\">x", 640);
    check(item("x") && item("x")->x == 8, "a negative margin stays at 0");
    laid_out("<p style=\"margin-left:2em\">x", 640);
    check(item("x") && item("x")->x == 8 + 32, "em: the element's font");
    laid_out("<p style=\"font-size:32px;margin-left:1em\">x", 640);
    check(item("x") && item("x")->x == 8 + 32, "em of a font it sets itself");
    laid_out("<p style=\"margin-left:1rem\">x", 640);
    check(item("x") && item("x")->x == 8 + 16, "rem: the page's base");
    laid_out("<p style=\"margin:0 auto\">x", 640);
    check(item("x") && item("x")->x == 8, "auto: no width to centre by");
    laid_out("<blockquote style=margin:0>x</blockquote>", 640);
    check(item("x") && item("x")->x == 8, "a quote without its indent");
    laid_out("<dl><dd style=margin-left:0>x</dl>", 640);
    check(item("x") && item("x")->x == 8, "a <dd> without its");
    laid_out("<ul style=padding:0><li>x</ul>", 640);
    check(item("x") && item("x")->x == 8, "a list without its padding");
    laid_out("<ul style=padding-left:10px><li>x</ul>", 640);
    check(item("x") && item("x")->x == 8 + 10, "a list with its own");
    laid_out("<ul><li>x</ul>", 640);
    check(item("x") && item("x")->x == 8 + 40, "a list as it was");
    laid_out("<div style=\"padding:10px 20px 30px 40px\">x</div>y", 640);
    a = item("x");
    b = item("y");
    check(a && b && a->x == 8 + 40 && a->y == 8 + 10 && b->y == a->y + 16 + 30, "padding");
    laid_out("<div style=\"padding:0 0 0 50%\">x</div>", 640);
    check(item("x") && item("x")->x == 8 + 312, "padding in percent");
    laid_out("<hr style=margin:30px>x", 640);
    check(item("x") && item("x")->y == 8 + 30 + 2 + 30, "a rule's margins");
    laid_out("<table style=\"margin-left:30px;margin-top:0\"><tr><td style=padding:7px>x</table>", 640);
    a = item("x");
    check(a && a->x == 8 + 30 + 2 + 1 + 7 && a->y == 8 + 2 + 1 + 7, "a table's margin, a cell's padding");
    laid_out("<table><tr><td style=padding:7px>abcd</table>", 640);
    {
        const struct wl_item *t = item("abcd");
        check(t != NULL, "a cell's padding counts in its column");
    }

    /* backgrounds */
    laid_out("<div style=background:#123456>x</div>", 640);
    f = fill(0x123456);
    a = item("x");
    check(f && a && f->x == 8 && f->w == 624 && f->y == a->y && f->h == 16 && f < a, "a box's background, under its text");
    laid_out("<div style=\"background:#123456;padding:4px\">x</div>", 640);
    f = fill(0x123456);
    a = item("x");
    check(f && a && f->y == a->y - 4 && f->h == 16 + 8 && f->x == a->x - 4, "with padding");
    laid_out("<div style=background:#123456><p>x</p></div>", 640);
    f = fill(0x123456);
    a = item("x");
    check(f && a && f->y == a->y && f->h == 16, "a child's margins meet outside it");
    laid_out("<div style=background:#123456><p>x<p>y</div>", 640);
    f = fill(0x123456);
    check(f && item("y") && f->h == item("y")->y + 16 - item("x")->y, "around all it holds");
    laid_out("<div style=background:#123456></div>x", 640);
    f = fill(0x123456);
    check(f && !f->h, "an empty box has no height");
    laid_out("<li style=background:#123456>x", 640);
    check(fill(0x123456) != NULL, "a list item");
    laid_out("a <span style=background:#123456>bc</span> d", 640);
    f = fill(0x123456);
    a = item("bc");
    check(f && a && f->x == a->x && f->w == a->w && f->y == a->y && f->h == a->h, "an inline element's, behind its text");
    laid_out("<p style=\"width:88px\"><span style=background:#123456>aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa aaaaaaa</span>", 200);
    {
        size_t i;
        int fills = 0, lines = 0, same = 1;
        for(i = 0; i < l.count; i++) {
            const struct wl_item *it = &l.items[i];
            if(it->kind == WI_FILL) fills++;
            if(it->kind == WI_TEXT) {
                lines++;
                if(i == 0 || l.items[i - 1].kind != WI_FILL || l.items[i - 1].w != it->w) same = 0;
            }
        }
        check(fills == lines && lines > 1 && same, "on every line it is on, as wide as the text");
    }

    /* alignment */
    laid_out("<div style=text-align:center>abc</div>", 640);
    a = item("abc");
    check(a && a->x == 8 + (624 - a->w) / 2, "centred");
    laid_out("<div style=text-align:right><p>abc</div>", 640);
    a = item("abc");
    check(a && a->x + a->w == 8 + 624, "right, inherited");
    laid_out("<div style=text-align:right><p style=text-align:left>abc</div>", 640);
    check(item("abc") && item("abc")->x == 8, "its own over what it inherits");
    laid_out("<style>body{text-align:center}</style><p>abc", 640);
    a = item("abc");
    check(a && a->x == 8 + (624 - a->w) / 2, "from the body");
    laid_out("<div style=text-align:center><table><tr><td style=width:300px>abc</table></div>", 640);
    check(item("abc") != NULL, "into a table");
    laid_out("<span style=text-align:center>abc</span>", 640);
    check(item("abc") && item("abc")->x == 8, "not on an inline element");

    /* the page's root */
    laid_out("<style>body{margin:0}</style><p style=margin:0>x", 640);
    check(item("x") && item("x")->x == 0 && item("x")->y == 0, "body's margin");
    laid_out("<body style=\"margin:20px 30px 40px 50px\"><p style=margin:0>x", 640);
    check(item("x") && item("x")->x == 50 && item("x")->y == 20 && l.height == 20 + 16 + 40, "all four, on the tag");
    laid_out("<style>body{padding:10px}</style><p style=margin:0>x", 640);
    check(item("x") && item("x")->x == 8 + 10 && item("x")->y == 8 + 10, "its padding adds to them");
    laid_out("<style>body{margin:0 9999px}</style><p>x", 640);
    check(item("x") != NULL && item("x")->x >= 0, "margins wider than the page");
    laid_out("<style>body{margin-left:50%;padding-left:50%}</style><p style=margin-left:50%>x", 640);
    check(item("x") && item("x")->x + item("x")->w <= l.width, "margins that leave no room");
    page("<style>body{background:#112233;color:#445566}</style>x");
    check(d.bg == 0x112233 && d.fg == 0x445566 && color_of("x") == WD_NONE, "body's colours are the page's");
    page("<style>html{background:#112233;color:#445566}</style>x");
    check(d.bg == 0x112233 && d.fg == 0x445566, "html's too");
    page("<style>html{background:#112233} body{background:#445566}</style>x");
    check(d.bg == 0x445566, "body's over html's");
    page("<body bgcolor=#112233 text=#223344 style=\"background:#445566;color:#556677\">x");
    check(d.bg == 0x445566 && d.fg == 0x556677, "over <body bgcolor text>");
    page("<body bgcolor=#112233 text=#223344>x");
    check(d.bg == 0x112233 && d.fg == 0x223344, "<body bgcolor text> without CSS");
    page("<style>body.a{color:#112233}</style><body class=a>x");
    check(d.fg == 0x112233, "body's class, and the rule before the tag");
    page("<style>.a p{color:#112233}</style><body class=a><p>x");
    check(color_of("x") == 0x112233, "what is inside the body");
    page("<style>.a p{color:#112233}</style><html class=a><p>x");
    check(color_of("x") == 0x112233, "html's class");
    page("<style>body p{color:#112233}</style><p>x");
    check(color_of("x") == 0x112233, "a body with no tag is there");
    page("<style>html>body>p{color:#112233}</style><head><title>t</title><meta charset=utf-8></head><body><p>x");
    check(color_of("x") == 0x112233, "a head that was there leaves nothing open");
    page("<head><title>t</title><body class=a><style>.a{color:#112233}</style><p>x");
    check(d.fg == 0x112233, "a body without an end to its head");
}

static void sizes(void) {
    page("<p style=font-size:20px>x");
    check(run("x") && run("x")->px == 20, "px");
    page("<div style=font-size:20px><p>x</div>");
    check(run("x") && run("x")->px == 20, "inherited");
    page("<div style=font-size:20px><p style=font-size:2em>x</div>");
    check(run("x") && run("x")->px == 40, "em of the parent's");
    page("<div style=font-size:20px><p style=font-size:150%>x</div>");
    check(run("x") && run("x")->px == 30, "percent of the parent's");
    page("<div style=font-size:20px><p style=font-size:1.5rem>x</div>");
    check(run("x") && run("x")->px == 24, "rem of the page's");
    page("<p style=font-size:x-large>x");
    check(run("x") && run("x")->px == 24, "a keyword");
    page("<div style=font-size:32px><p style=font-size:smaller>x</div>");
    check(run("x") && run("x")->px == 27, "smaller");
    page("<h1 style=font-size:20px>x</h1>");
    check(run("x") && run("x")->px == 20 && run("x")->size == WZ_H1, "on a heading");
    page("<h1 style=font-size:50%>x</h1>");
    check(run("x") && run("x")->px == 8, "on a heading: of the parent's, not its own");
    page("<div style=font-size:12px><h1>x</h1></div>");
    check(run("x") && run("x")->px == 24, "a heading in a size CSS gave: twice that");
    page("<div style=font-size:12px><small>x</small></div>");
    check(run("x") && run("x")->px == 10, "<small> in it");
    page("<h1>x</h1><small>y</small>");
    check(run("x") && !run("x")->px && !run("y")->px, "without CSS, by HTML's word");
    page("<h2 style=font-size:inherit>x</h2>");
    check(run("x") && run("x")->px == 16, "font-size: inherit");
    page("<p style=font-size:0>x");
    check(run("x") && run("x")->px == 1, "0 is the smallest there is");
    page("<p style=font-size:9999px>x<p style=font-size:9999em>y<p style=font-size:999999%>z");
    check(run("x")->px == DS_PX_MAX && run("y")->px == DS_PX_MAX && run("z")->px == DS_PX_MAX, "the largest");
    page("<body style=font-size:9999em><div style=font-size:9999em><div style=font-size:9999em><p style=font-size:9999%>x");
    check(run("x") && run("x")->px == DS_PX_MAX, "nested: nothing overflows");
}

static void sheets(void) {
    page("<p>x<style>p{color:red}</style>");
    check(color_of("x") == 0xff0000, "a <style> after what it styles");
    page("<style>p{color:red}</style><style>p{color:blue}</style><p>x");
    check(color_of("x") == 0x0000ff, "sheets in their order");
    page("<style media=print>p{color:red}</style><p>x");
    check(color_of("x") == WD_NONE, "media=print");
    page("<style media=\"screen, print\">p{color:red}</style><p>x");
    check(color_of("x") == 0xff0000, "media for a screen");
    page("<style type=text/less>p{color:red}</style><p>x");
    check(color_of("x") == WD_NONE, "another type");
    page("<style type=\"TEXT/CSS\">p{color:red}</style><p>x");
    check(color_of("x") == 0xff0000, "text/css in capitals");
    page("<style type=\"\">p{color:red}</style><p>x");
    check(color_of("x") == 0xff0000, "an empty type");
    page("<!-- <style>p{color:red}</style> --><p>x");
    check(color_of("x") == WD_NONE, "in a comment: none");
    page("<script>var s = '<style>p{color:red}</style>';</script><p>x");
    check(color_of("x") == WD_NONE, "in a script: none");
    page("<textarea><style>p{color:red}</style></textarea><p>x");
    check(color_of("x") == WD_NONE, "in a text area: none");
    page("<title><style>p{color:red}</style></title><p>x");
    check(color_of("x") == WD_NONE, "in a title: none");
    page("<template><style>p{color:red}</style></template><p>x");
    check(color_of("x") == WD_NONE, "in a template: none");
    page("<div hidden><style>p{color:red}</style></div><p>x");
    check(color_of("x") == 0xff0000, "in a hidden element: as in a browser");
    page("<noscript><style>p{color:red}</style></noscript><p>x");
    check(color_of("x") == 0xff0000, "in <noscript>: no script runs here");
    page("<style><!-- p{color:red} --></style><p>x");
    check(color_of("x") == 0xff0000, "the old hiding of a sheet in a comment");
    page("<style>p{color:red}");
    check(!d.run_count, "a <style> left open: its text is not shown");
    page("<style>p:after{content:'&lt;/style&gt;'}</style><p>x");
    check(run("x") != NULL, "character references are not read in a sheet");
    {
        static const char latin[] = "<style>.caf\xe9{color:red}</style><p class=\"caf\xe9\">x";
        struct wd_document e;
        check(!build(latin, sizeof(latin) - 1, &e, "windows-1252"), "a page in windows-1252");
        check(e.run_count == 1 && e.runs[0].color == 0xff0000, "its sheet is read in its charset");
        wd_free(&e);
    }
    page("x");
    check(!d.notice[0], "no notice");
}

/* the same page with and without a sheet that says nothing */
static void same_tree(const char *body, const char *sheet, const char *what) {
    static char a[70000];
    char msg[300], *x, *y;
    struct wd_document p, q;
    size_t n = (size_t)snprintf(a, sizeof(a), "<style>%s</style>%s", sheet, body);
    int ok = !build(body, strlen(body), &p, "") && !build(a, n, &q, "");
    snprintf(msg, sizeof(msg), "%s: nothing changes", what);
    x = ok ? wd_dump(&p) : NULL;
    y = ok ? wd_dump(&q) : NULL;
    check(x && y && !strcmp(x, y), msg);
    free(x);
    free(y);
    if(ok) {
        wd_free(&p);
        wd_free(&q);
    }
}

static const char body[] =
    "<title>A page</title><h1 id=top>Heading</h1><p>Text with <b>bold</b>, <a href=a.html>a link</a>."
    "<ul><li>one<li>two</ul><table border=1><tr><td bgcolor=#ff0000>cell</table><pre>code\n  here</pre>"
    "<form><input value=x><button>go</button></form><img src=a.png alt=pic>";

static void malformed(void) {
    static const char *const bad[] = {
        "}", "{", "(", "[", ")", "]", "\"", "'", "/*", "@", "@media", "@media {", "p", "p {", "p { color", "p { color:",
        "p { color: }", "p { nonsense: red }", "p { color: nonsense }", "p, { color: red }", ", p { color: red }",
        "p > { color: red }", "{{{{{{{{{{", "}}}}}}}}}}", "((((((((((", "@media print { p { color: red } }",
        "@font-face { src: url(x) }", "\\", "<!--", "-->", "p:unknown { color: red }", "#1 { color: red }",
        "p { color: rgb(1,2 }", "p { margin: 1 2 3 4 5 6 }", "\xff\xfe", "@import 'x'", "a::b { color: red }",
    };
    size_t i;
    for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) same_tree(body, bad[i], bad[i]);
}

static void hostile(void) {
    char *s = malloc(4000000);
    size_t n, i, k;
    struct wd_document e;
    if(!s) exit(2);

    /* more open elements than the frames: nothing runs off the end */
    n = 0;
    for(i = 0; i < 2000; i++) n += (size_t)sprintf(s + n, "<span class=a style=color:red>");
    n += (size_t)sprintf(s + n, "x");
    for(i = 0; i < 2000; i++) n += (size_t)sprintf(s + n, "</span>");
    n += (size_t)sprintf(s + n, "y");
    check(!build(s, n, &e, ""), "2000 open elements");
    check(e.run_count >= 1, "and their text");
    wd_free(&e);
    n = 0;
    for(i = 0; i < 2000; i++) n += (size_t)sprintf(s + n, "<div class=a style=display:inline>");
    n += (size_t)sprintf(s + n, "x");
    check(!build(s, n, &e, ""), "2000 open blocks made inline");
    wd_free(&e);
    n = 0;
    for(i = 0; i < 2000; i++) n += (size_t)sprintf(s + n, "<span style=display:block>");
    n += (size_t)sprintf(s + n, "x");
    for(i = 0; i < 2000; i++) n += (size_t)sprintf(s + n, "</span>");
    check(!build(s, n, &e, ""), "2000 inline elements made blocks");
    wd_free(&e);
    n = 0;
    for(i = 0; i < 300; i++) n += (size_t)sprintf(s + n, "<div style=display:none>");
    n += (size_t)sprintf(s + n, "x");
    for(i = 0; i < 300; i++) n += (size_t)sprintf(s + n, "</div>");
    n += (size_t)sprintf(s + n, "y");
    check(!build(s, n, &e, ""), "300 hidden blocks one in another");
    check(e.run_count == 1, "only what comes after them");
    wd_free(&e);

    /* a sheet that makes every element try every selector: the matching stops, the page stands */
    n = (size_t)sprintf(s, "<style>");
    for(i = 0; i < 16000; i++) n += (size_t)sprintf(s + n, "div div div div div div div div{color:red}");
    n += (size_t)sprintf(s + n, "</style>");
    for(i = 0; i < 100; i++) n += (size_t)sprintf(s + n, "<div>");
    for(i = 0; i < 20000; i++) n += (size_t)sprintf(s + n, "<div></div>");
    n += (size_t)sprintf(s + n, "x");
    check(!build(s, n, &e, ""), "a sheet that takes all the matching a page gets");
    check(strstr(e.notice, "style is left out") != NULL && e.run_count == 1, "the page stands, and says so");
    wd_free(&e);

    /* a sheet past a limit: the rules before it stay, and a notice */
    n = (size_t)sprintf(s, "<style>p{color:red}");
    for(i = 0; i < 20000; i++) n += (size_t)sprintf(s + n, "h1{color:red}");
    n += (size_t)sprintf(s + n, "</style><p>x");
    check(!build(s, n, &e, ""), "more rules than may be");
    check(e.run_count == 1 && e.runs[0].color == 0xff0000 && strstr(e.notice, "style is left out") != NULL, "what came before stays, and it says so");
    wd_free(&e);

    /* attributes past what is read */
    n = (size_t)sprintf(s, "<p style=\"color:red;margin:");
    for(i = 0; i < 5000; i++) s[n++] = '0';
    n += (size_t)sprintf(s + n, "\">x");
    check(!build(s, n, &e, "") && e.runs[0].color == WD_NONE, "a style attribute over 4 KB goes whole");
    wd_free(&e);
    n = (size_t)sprintf(s, "<style>.c15{color:red}.c16{color:blue}</style><p class=\"");
    for(k = 0; k < 32; k++) n += (size_t)sprintf(s + n, "c%u ", (unsigned int)k);
    n += (size_t)sprintf(s + n, "\">x");
    check(!build(s, n, &e, "") && e.runs[0].color == 0xff0000, "16 classes of an element count");
    wd_free(&e);
    n = (size_t)sprintf(s, "<p style=\"");
    for(k = 0; k < 200; k++) n += (size_t)sprintf(s + n, "color:#%06x;", (unsigned int)k);
    n += (size_t)sprintf(s + n, "\">x");
    check(!build(s, n, &e, "") && e.runs[0].color == 63, "64 declarations of a style attribute count");
    wd_free(&e);
    free(s);
}

static void allocations(void) {
    static const char html[] =
        "<style>p{color:red} .a{margin:1px} @media screen{b{font-weight:normal}}</style><html class=a><body class=b>"
        "<p style=color:blue>x<span class=a>y</span><div style=display:none>z</div><table><tbody><tr><td>c</table>";
    long i, before = ut_alloc_live;
    int ok = 0;
    for(i = 0; i < 400 && !ok; i++) {
        struct wd_document e;
        ut_alloc_fail_after = i;
        ok = !build(html, sizeof(html) - 1, &e, "");
        ut_alloc_fail_after = -1;
        if(ok) {
            check(e.run_count >= 2 && e.runs[0].color == 0x0000ff, "with memory enough the page is whole");
            wd_free(&e);
        }
    }
    check(ok && i > 3, "every allocation refused in turn");
    check(ut_alloc_live == before, "and every one given back");
}

int main(void) {
    cascade();
    html_below();
    display();
    boxes();
    sizes();
    sheets();
    malformed();
    hostile();
    allocations();
    if(laid) wl_free(&l);
    if(have) wd_free(&d);
    if(failures) {
        printf("WUNDERTOW STYLE TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW STYLE TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
