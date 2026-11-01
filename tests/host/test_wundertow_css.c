/*
SurfOS Undertow's window: tests of the style sheet parser
----------------------
File: test_wundertow_css.c  Date: 11/01/26
----------------------
user/wundertow/css.c on the host: selectors and what the standard says of malformed ones, declarations and their
values, shorthands, comments and escapes, at-rules, the limits, a style attribute, media queries, @import, and sheets
that are not sheets: cut at every byte, broken in every way a sheet can be, every allocation failing in turn. A sheet
that is hostile must leave what came before it, and take no time or memory it should not.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "undertow.h"
#include "css.h"
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

static struct css *c;                   /* 16 KB: off the stack */

static void sheet(const char *s) {
    css_free(c);
    css_add(c, s, strlen(s));
}

static unsigned int T(const char *s) { return css_type(s, strlen(s)); }
static unsigned int N(const char *s) { return css_name(s, strlen(s)); }

/* the number of the selector with these parts (kind, hash, ..., in the order kept), -1 when there is none */
static int find(int n, const unsigned int *want) {
    size_t i;
    int k;
    for(i = 0; i < c->nsels; i++) {
        const struct css_selector *s = &c->sels[i];
        if((int)s->nparts != n) continue;
        for(k = 0; k < n; k++)
            if(c->parts[s->first + k].kind != want[2 * k] ||
               (want[2 * k] < CSS_LINK && c->parts[s->first + k].hash != want[2 * k + 1])) break;
        if(k == n) return (int)i;
    }
    return -1;
}

/* the sheet's declaration of prop in its last rule, NULL when there is none */
static const struct css_decl *decl(int prop) {
    size_t i;
    const struct css_selector *s;
    if(!c->nsels) return NULL;
    s = &c->sels[c->nsels - 1];
    for(i = s->decl + s->ndecl; i > s->decl; i--) if(c->decls[i - 1].prop == prop) return &c->decls[i - 1];
    return NULL;
}

static int is(int prop, int unit, int value) {
    const struct css_decl *d = decl(prop);
    return d && d->unit == unit && d->value == value;
}

static int color_is(const char *value, unsigned int rgb) {
    char s[200];
    snprintf(s, sizeof(s), "p { color: %s }", value);
    sheet(s);
    return is(CP_COLOR, CU_RGB, (int)rgb);
}

static int no_color(const char *value) {
    char s[200];
    snprintf(s, sizeof(s), "p { color: %s }", value);
    sheet(s);
    return !c->nsels;
}

/* the page's rule "p { color: red }" stays after this text, and the text adds nothing it should not */
static void leaves(const char *bad, int selectors, const char *what) {
    char s[600], msg[700];
    snprintf(s, sizeof(s), "%s p{color:red}", bad);
    sheet(s);
    snprintf(msg, sizeof(msg), "%s: %s (%u selectors)", what, bad, (unsigned int)c->nsels);
    check((int)c->nsels == selectors, msg);
    snprintf(msg, sizeof(msg), "%s: the rule after it stays: %s", what, bad);
    check(c->nsels && is(CP_COLOR, CU_RGB, 0xff0000), msg);
}

/* the sheet comes to nothing at all */
static void nothing(const char *bad) {
    char msg[300];
    sheet(bad);
    snprintf(msg, sizeof(msg), "nothing comes of: %s", bad);
    check(!c->nsels && !c->ndecls && !c->rules, msg);
}

static void selectors(void) {
    unsigned int w[12];

    sheet("h1 { color: red }");
    w[0] = CSS_TYPE;
    w[1] = T("h1");
    check(c->nsels == 1 && c->rules == 1 && find(1, w) == 0 && c->sels[0].spec == 1, "a type");
    check(c->sels[0].key == T("h1") && c->head[T("h1") % CSS_BUCKETS] == 0, "a type: filed by it");
    check(is(CP_COLOR, CU_RGB, 0xff0000), "its declaration");

    sheet("H1.Foo#Bar { color: red }");
    w[0] = CSS_TYPE;
    w[1] = T("h1");
    w[2] = CSS_CLASS;
    w[3] = N("Foo");
    w[4] = CSS_ID;
    w[5] = N("Bar");
    check(find(3, w) == 0 && c->sels[0].spec == (1u << 20 | 1u << 10 | 1), "a compound: case kept but for the type; specificity");
    check(c->sels[0].key == N("Bar"), "a compound: filed by its id");

    sheet(".a.b { color: red }");
    check(c->sels[0].key == N("b") && c->sels[0].spec == 2u << 10, "two classes");

    sheet("a, b ,\n c{color:red}");
    check(c->nsels == 3 && c->sels[0].decl == c->sels[2].decl && c->sels[1].ndecl == 1 && c->rules == 1, "grouping");

    sheet("div p { color: red }");
    w[0] = CSS_TYPE;
    w[1] = T("p");
    w[2] = CSS_DESCENDANT;
    w[3] = 0;
    w[4] = CSS_TYPE;
    w[5] = T("div");
    check(find(3, w) == 0 && c->sels[0].key == T("p") && c->sels[0].spec == 2, "descendant: the subject first");

    sheet("ul>li { color: red }");
    w[0] = CSS_TYPE;
    w[1] = T("li");
    w[2] = CSS_CHILD;
    w[3] = 0;
    w[4] = CSS_TYPE;
    w[5] = T("ul");
    check(find(3, w) == 0, "child without spaces");
    sheet("ul  >\n\tli{color:red}");
    check(find(3, w) == 0, "child with spaces");

    sheet("a:link { color: red }");
    w[0] = CSS_TYPE;
    w[1] = T("a");
    w[2] = CSS_LINK;
    w[3] = 0;
    check(find(2, w) == 0 && c->sels[0].spec == (1u << 10 | 1), ":link counts as a class");
    sheet("a:visited { color: red }");
    check(!c->nsels, ":visited: every link counts as :link, so it matches nothing");
    sheet("a:visited, a:link { color: red }");
    check(c->nsels == 1 && find(2, w) == 0, ":link and :visited together");

    sheet("* { color: red }");
    check(c->nsels == 1 && c->sels[0].nparts == 0 && c->any == 0 && !c->sels[0].spec, "'*'");
    sheet("div > * { color: red }");
    check(c->nsels == 1 && c->sels[0].nparts == 2 && c->parts[c->sels[0].first].kind == CSS_CHILD, "'*' as a subject");
    sheet("* p { color: red }");
    w[0] = CSS_TYPE;
    w[1] = T("p");
    w[2] = CSS_DESCENDANT;
    w[3] = 0;
    check(find(2, w) == 0, "'*' as an ancestor");
    sheet(":link { color: red }");
    check(c->nsels == 1 && c->any == 0, ":link alone is filed with '*'");

    sheet(".w-1\\/2, #\\31 23, .\\66oo { color: red }");
    w[0] = CSS_CLASS;
    w[1] = N("w-1/2");
    check(find(1, w) == 0, "an escaped character in a class");
    w[0] = CSS_ID;
    w[1] = N("123");
    check(find(1, w) == 1, "an escape in hex in an id");
    w[0] = CSS_CLASS;
    w[1] = N("foo");
    check(find(1, w) == 2, "an escaped letter is the letter");

    /* well-formed, but of what is not here: it matches nothing and takes nothing with it */
    leaves("a:hover { color: blue }", 1, "alone: it goes");
    sheet("h1, a:hover, li + li, li ~ li, [hidden], a[href], a::before, a:not(.x), p:nth-child(2n+1) { color: red }");
    check(c->nsels == 1 && c->rules == 1 && is(CP_COLOR, CU_RGB, 0xff0000), "those that match nothing leave their list");
    leaves("a::-webkit-scrollbar { color: blue }", 1, "-webkit- pseudo-elements are well-formed");
    leaves("a:before, a:after, a:first-line { color: blue }", 1, "the old pseudo-elements with one colon");

    /* malformed: the rule goes, the next stays */
    leaves("h2,, h3 { color: blue }", 1, "an empty selector");
    leaves("h2, { color: blue }", 1, "a list's end");
    leaves(", h2 { color: blue }", 1, "a list's start");
    leaves("{ color: blue }", 1, "no selector");
    leaves("> h2 { color: blue }", 1, "a combinator first");
    leaves("h2 > { color: blue }", 1, "a combinator last");
    leaves("h2 > > h3 { color: blue }", 1, "two combinators");
    leaves("h2 + { color: blue }", 1, "a combinator last");
    leaves(". { color: blue }", 1, "no class name");
    leaves(".5a { color: blue }", 1, "a class that is a number");
    leaves("#123 { color: blue }", 1, "an id that is no name");
    leaves("a..b { color: blue }", 1, "two dots");
    leaves("h2:unknown, h3 { color: blue }", 1, "an unknown pseudo-class takes its list");
    leaves("h2::unknown, h3 { color: blue }", 1, "an unknown pseudo-element");
    leaves("h2::hover, h3 { color: blue }", 1, "a pseudo-class as an element");
    leaves("h2:selection { color: blue }", 1, "a pseudo-element as a class");
    leaves("h2: { color: blue }", 1, "a colon alone");
    leaves("h2:: { color: blue }", 1, "two colons alone");
    leaves("[] { color: blue }", 1, "an empty attribute");
    leaves("[=x], h3 { color: blue }", 1, "an attribute without a name");
    leaves("h2, td[colspan=2] { color: blue }", 1, "an attribute's number without quotes");
    leaves("h2, [a=] { color: blue }", 1, "an attribute without a value");
    leaves("h2, [a b] { color: blue }", 1, "two names");
    leaves("h2, [a=b x] { color: blue }", 1, "a flag that is none");
    leaves("h2, [a=b { color: blue }] { color: blue }", 1, "a block in one");
    leaves("h2, :hover() { color: blue }", 1, "a pseudo-class written as a function");
    leaves("h2, :not { color: blue }", 1, "a function written as a pseudo-class");
    leaves("h2, :nth-child { color: blue }", 1, "and another");
    leaves("h2, :left { color: blue }", 1, "@page's are not a selector's");
    leaves("h2, :-webkit-foo { color: blue }", 1, "only pseudo-elements get -webkit-'s pass");
    leaves("h2, ::before.x { color: blue }", 1, "a class after a pseudo-element");
    leaves("h2, ::before p { color: blue }", 1, "a compound after one");
    leaves("h2, ::before::after { color: blue }", 1, "two of them");
    sheet("h2, ::before:hover, [a], [ a ], [a=b], [a~=\"b\"], [a|=b i], [a^=b], [a$=b], [a*=b S] { color: red }");
    check(c->nsels == 1 && is(CP_COLOR, CU_RGB, 0xff0000), "those are well-formed");
    leaves("(h2) { color: blue }", 1, "brackets");
    leaves("h2(x) { color: blue }", 1, "a function");
    leaves("h2 ; h3 { color: blue }", 1, "a semicolon");
    leaves("\"h2\" { color: blue }", 1, "a string");
    leaves("12 { color: blue }", 1, "a number");
    leaves("h2 * { color: blue } h3* { color: blue }", 2, "'*' after a name");
    leaves("*h2 { color: blue }", 1, "a name after '*'");
    leaves("h2|h3 { color: blue }", 1, "a namespace");
    leaves("} h2 { color: blue }", 1, "a stray brace");
    leaves("] h2 { color: blue }", 1, "a stray bracket");
    leaves("[{}] h2 { color: blue }", 1, "a block in brackets is not the rule's");
    leaves("(;{}) h2 { color: blue }", 1, "a block in parentheses is not the rule's");
}

static void values(void) {
    /* declarations: what it cannot read goes alone */
    sheet("p { color: blue; foo: bar; display: none }");
    check(c->ndecls == 2 && is(CP_COLOR, CU_RGB, 0x0000ff) && is(CP_DISPLAY, CU_NONE, CSS_D_NONE), "an unknown property");
    sheet("p { color: blue; color: notacolor }");
    check(c->ndecls == 1 && is(CP_COLOR, CU_RGB, 0x0000ff), "a bad value");
    sheet("p { color blue; display: none }");
    check(c->ndecls == 1 && is(CP_DISPLAY, CU_NONE, CSS_D_NONE), "no colon");
    sheet("p { : blue; display: none }");
    check(c->ndecls == 1 && is(CP_DISPLAY, CU_NONE, CSS_D_NONE), "no name");
    sheet("p { color: ; display: none }");
    check(c->ndecls == 1 && is(CP_DISPLAY, CU_NONE, CSS_D_NONE), "no value");
    sheet("p { 12: blue; display: none }");
    check(c->ndecls == 1 && is(CP_DISPLAY, CU_NONE, CSS_D_NONE), "a number for a name");
    sheet("p { COLOR: BLUE; ; ;; DISPLAY:NONE; }");
    check(c->ndecls == 2 && is(CP_COLOR, CU_RGB, 0x0000ff), "any case, empty declarations");
    sheet("p { color: red; a { color: blue } display: none }");
    check(c->ndecls == 2 && is(CP_DISPLAY, CU_NONE, CSS_D_NONE), "a nested rule ends with its block, the next stays");
    sheet("p { color: red; .a { color: blue } display: none }");
    check(c->ndecls == 2 && is(CP_DISPLAY, CU_NONE, CSS_D_NONE), "one of a class too");
    sheet("p { color: red; @media screen { color: blue } display: none }");
    check(c->ndecls == 2 && is(CP_DISPLAY, CU_NONE, CSS_D_NONE), "a nested at-rule too");
    sheet("p { color: red; --x: { a: b; c: d }; display: none }");
    check(c->ndecls == 2 && is(CP_DISPLAY, CU_NONE, CSS_D_NONE), "a custom property's block is read past");
    sheet("p { color: red; @media print { x: y } ; display: none }");
    check(c->ndecls == 2, "an at-rule among declarations");
    sheet("p { --x: { a: b } display: none; color: red }");
    check(c->ndecls == 1 && is(CP_COLOR, CU_RGB, 0xff0000), "a custom property's block does not end it");
    sheet("p { foo: bar }");
    check(!c->nsels && !c->rules, "a rule with nothing it knows is not kept");

    /* !important */
    sheet("p { color: red !important; margin-top: 1px ! important; display: none !IMPORTANT; font-style: italic }");
    check(decl(CP_COLOR)->important && decl(CP_MARGIN_TOP)->important && decl(CP_DISPLAY)->important &&
          !decl(CP_STYLE)->important, "!important");
    sheet("p { color: red ! foo; font-style: italic !; margin-top: 1px !important!important }");
    check(!decl(CP_COLOR) && !decl(CP_STYLE) && !decl(CP_MARGIN_TOP), "what is not !important");

    /* colours */
    check(color_is("red", 0xff0000) && color_is("REBECCAPURPLE", 0x663399) && color_is("lightgoldenrodyellow", 0xfafad2), "names");
    check(color_is("aliceblue", 0xf0f8ff) && color_is("yellowgreen", 0x9acd32), "names: the first and the last");
    check(color_is("#abc", 0xaabbcc) && color_is("#AbCdEf", 0xabcdef), "#rgb, #rrggbb");
    check(color_is("#abcf", 0xaabbcc) && color_is("#abcdefff", 0xabcdef), "#rgba, #rrggbbaa, opaque");
    check(color_is("#00000080", 0x7f7f7f), "#rrggbbaa, half: over white");
    sheet("p { color: inherit }");
    check(is(CP_COLOR, CU_INHERIT, 0), "inherit");
    check(color_is("rgb(1,2,3)", 0x010203) && color_is("rgb(1 2 3)", 0x010203) && color_is("RGB( 1 , 2 , 3 )", 0x010203), "rgb()");
    check(color_is("rgb(100%,0%,50%)", 0xff0080) && color_is("rgb(300,-20,0)", 0xff0000), "rgb(): percent, saturated");
    check(color_is("rgba(0,0,0,1)", 0x000000) && color_is("rgb(0 0 0 / 100%)", 0x000000), "rgba(), opaque");
    check(color_is("rgba(0,0,0,.5)", 0x7f7f7f), "rgba(): half");
    check(color_is("hsl(0,100%,50%)", 0xff0000) && color_is("hsl(120, 100%, 50%)", 0x00ff00) &&
          color_is("hsl(240deg 100% 50%)", 0x0000ff) && color_is("hsl(0,0%,100%)", 0xffffff) &&
          color_is("hsl(-120,100%,50%)", 0x0000ff) && color_is("hsla(60,100%,50%,1)", 0xffff00), "hsl()");
    sheet("p { color: inherit }");
    check(is(CP_COLOR, CU_INHERIT, 0), "inherit");
    sheet("p { color: currentColor }");
    check(is(CP_COLOR, CU_INHERIT, 0), "currentcolor");
    check(no_color("transparent") && no_color("rgba(0,0,0,0)"), "text no one sees is not kept");
    check(no_color("#12") && no_color("#12345") && no_color("#1234567") && no_color("#123456789") && no_color("#ggg"), "hex that is none");
    check(no_color("ffffff") && no_color("123") && no_color("rgb(1,2)") && no_color("rgb(1,2,3,4,5)") && no_color("rgb(a,b,c)"), "other colours that are none");
    check(no_color("rgb(1px,2px,3px)") && no_color("red blue") && no_color("#fff #000") && no_color("var(--x)") && no_color("calc(1 + 2)"), "and more");
    sheet("p { background-color: transparent }");
    check(is(CP_BACKGROUND, CU_TRANSPARENT, 0), "a transparent background");
    sheet("p { background-color: inherit }");
    check(is(CP_BACKGROUND, CU_REVERT, 0), "a background's inherit is read as revert: a parent's is not to hand");

    /* background */
    sheet("p { background: #fff url(x.png) no-repeat center / cover }");
    check(is(CP_BACKGROUND, CU_RGB, 0xffffff), "background: its colour among the rest");
    sheet("p { background: url(\"x.png\") red }");
    check(is(CP_BACKGROUND, CU_RGB, 0xff0000), "background: a colour after an image");
    sheet("p { background: url(x.png) }");
    check(is(CP_BACKGROUND, CU_TRANSPARENT, 0), "background: no colour says so");
    sheet("p { background: none }");
    check(is(CP_BACKGROUND, CU_TRANSPARENT, 0), "background: none");
    sheet("p { background: #123456; background: linear-gradient(red, blue) }");
    check(is(CP_BACKGROUND, CU_RGB, 0x123456) && c->ndecls == 1, "background: a gradient leaves the colour before it");
    sheet("p { background: #123456; background: -webkit-repeating-linear-gradient(red, blue) }");
    check(c->ndecls == 1, "a gradient by a long name too");
    sheet("p { background: linear-gradient(red, blue) #123456 }");
    check(is(CP_BACKGROUND, CU_RGB, 0x123456), "a gradient and a colour");
    sheet("p { background: #123456; background: repeating-linear-gradient(red, blue) }");
    check(is(CP_BACKGROUND, CU_RGB, 0x123456) && c->ndecls == 1, "one whose name is cut in the token");
    sheet("p { background: #fff repeating-linear-gradient(red, blue) }");
    check(is(CP_BACKGROUND, CU_RGB, 0xffffff), "that and a colour");
    sheet("p { background: url(x) no-repeat calc(100% - 10px) 0 #fff }");
    check(is(CP_BACKGROUND, CU_RGB, 0xffffff), "background: calc() in an image's place");
    sheet("p { background: #fff image-set('a.png' 1x) }");
    check(is(CP_BACKGROUND, CU_RGB, 0xffffff), "background: image-set()");
    sheet("p { background: #123456; background: var(--x) }");
    check(is(CP_BACKGROUND, CU_RGB, 0x123456) && c->ndecls == 1, "background: var() leaves the colour before it");
    sheet("p { background: #123456; background: rgb(1,2) }");
    check(is(CP_BACKGROUND, CU_RGB, 0x123456) && c->ndecls == 1, "background: a function it cannot read too");
    sheet("p { background: rgb(1,2,3) }");
    check(is(CP_BACKGROUND, CU_RGB, 0x010203), "background: rgb()");
    sheet("p { background: nonsense }");
    check(!c->nsels, "background: what it is not");

    /* display */
    sheet("p { display: inline-block }");
    check(is(CP_DISPLAY, CU_NONE, CSS_D_INLINE), "display: inline-block");
    sheet("p { display: flex }");
    check(is(CP_DISPLAY, CU_NONE, CSS_D_BLOCK), "display: flex is a block's");
    sheet("p { display: table-cell }");
    check(is(CP_DISPLAY, CU_NONE, CSS_D_ASIS), "display: what it has no use for still undoes an earlier none");
    sheet("p { display: nonsense }");
    check(!c->nsels, "display: what is none");
    sheet("p { display: block none }");
    check(!c->nsels, "display: two words");

    /* font-weight, font-style */
    sheet("p { font-weight: bold }");
    check(is(CP_WEIGHT, CU_NONE, 1), "bold");
    sheet("p { font-weight: 700 }");
    check(is(CP_WEIGHT, CU_NONE, 1), "700");
    sheet("p { font-weight: 400 }");
    check(is(CP_WEIGHT, CU_NONE, 0), "400");
    sheet("p { font-weight: lighter }");
    check(is(CP_WEIGHT, CU_NONE, 0), "lighter");
    sheet("p { font-weight: 0 }");
    check(!c->nsels, "weight 0 is none");
    sheet("p { font-weight: 1001 }");
    check(!c->nsels, "weight 1001 is none");
    sheet("p { font-style: oblique 10deg }");
    check(is(CP_STYLE, CU_NONE, 1), "oblique with an angle");
    sheet("p { font-style: normal 10deg }");
    check(!c->nsels, "normal with an angle");

    /* text-decoration */
    sheet("a { text-decoration: none }");
    check(is(CP_DECORATION, CU_NONE, 0), "none");
    sheet("a { text-decoration: underline line-through }");
    check(is(CP_DECORATION, CU_NONE, CSS_LINE_UNDER | CSS_LINE_THROUGH), "two lines");
    sheet("a { text-decoration: underline dotted red 2px }");
    check(is(CP_DECORATION, CU_NONE, CSS_LINE_UNDER), "the shorthand's rest");
    sheet("a { text-decoration-line: line-through }");
    check(is(CP_DECORATION, CU_NONE, CSS_LINE_THROUGH), "text-decoration-line");
    sheet("a { text-decoration: none underline }");
    check(!c->nsels, "none and a line");
    sheet("a { text-decoration: nonsense }");
    check(!c->nsels, "nonsense");

    /* text-align */
    sheet("p { text-align: center }");
    check(is(CP_ALIGN, CU_NONE, CSS_A_CENTER), "center");
    sheet("p { text-align: end }");
    check(is(CP_ALIGN, CU_NONE, CSS_A_RIGHT), "end");
    sheet("p { text-align: justify }");
    check(is(CP_ALIGN, CU_NONE, CSS_A_LEFT), "justify: lines are not stretched here");
    sheet("p { text-align: middle }");
    check(!c->nsels, "HTML's word is none of CSS's");

    /* lengths */
    sheet("p { margin-top: 10px }");
    check(is(CP_MARGIN_TOP, CU_PX, 10 * 256), "px");
    sheet("p { margin-top: 12pt }");
    check(is(CP_MARGIN_TOP, CU_PX, 16 * 256), "pt");
    sheet("p { margin-top: 1in }");
    check(is(CP_MARGIN_TOP, CU_PX, 96 * 256), "in");
    sheet("p { margin-top: 1.5em }");
    check(is(CP_MARGIN_TOP, CU_EM, 384), "em");
    sheet("p { margin-top: .5rem }");
    check(is(CP_MARGIN_TOP, CU_REM, 128), "rem");
    sheet("p { margin-top: 2ex }");
    check(is(CP_MARGIN_TOP, CU_EM, 256), "ex");
    sheet("p { margin-top: 25% }");
    check(is(CP_MARGIN_TOP, CU_PCT, 25 * 256), "percent");
    sheet("p { margin-top: 0 }");
    check(is(CP_MARGIN_TOP, CU_PX, 0), "0 without a unit");
    sheet("p { margin-top: 0.0 }");
    check(is(CP_MARGIN_TOP, CU_PX, 0), "0.0 without a unit");
    sheet("p { margin-top: -4px }");
    check(is(CP_MARGIN_TOP, CU_PX, -4 * 256), "negative margins are read");
    sheet("p { margin-top: +4px }");
    check(is(CP_MARGIN_TOP, CU_PX, 4 * 256), "a plus sign");
    sheet("p { margin-top: auto }");
    check(is(CP_MARGIN_TOP, CU_AUTO, 0), "auto");
    sheet("p { padding-top: auto }");
    check(!c->nsels, "padding has no auto");
    sheet("p { padding-top: -1px }");
    check(!c->nsels, "padding is never negative");
    sheet("p { margin-top: 10 }");
    check(!c->nsels, "a number is no length");
    sheet("p { margin-top: 10furlongs }");
    check(!c->nsels, "a unit that is none");
    sheet("p { margin-top: calc(1px + 2px) }");
    check(!c->nsels, "calc() is not known");

    /* numbers, saturated, and their exponents */
    sheet("p { margin-top: 1e2px }");
    check(is(CP_MARGIN_TOP, CU_PX, 100 * 256), "an exponent");
    sheet("p { margin-top: 1500e-2px }");
    check(is(CP_MARGIN_TOP, CU_PX, 15 * 256), "a negative exponent");
    sheet("p { margin-top: 1e999999px }");
    check(is(CP_MARGIN_TOP, CU_PX, 1 << 20), "a huge exponent saturates");
    sheet("p { margin-top: 99999999999999999999999999px }");
    check(is(CP_MARGIN_TOP, CU_PX, 1 << 20), "a huge number saturates");
    sheet("p { margin-top: -99999999999999999999999999in }");
    check(is(CP_MARGIN_TOP, CU_PX, -(1 << 20) * 96), "a huge length in another unit saturates first");
    sheet("p { margin-top: 1e-999999px }");
    check(is(CP_MARGIN_TOP, CU_PX, 0), "a tiny number is 0");
    sheet("p { margin-top: 1.123456789012345678901234567890px }");
    check(is(CP_MARGIN_TOP, CU_PX, 256 + 31), "only four decimals count");

    /* shorthands */
    sheet("p { margin: 1px }");
    check(is(CP_MARGIN_TOP, CU_PX, 256) && is(CP_MARGIN_RIGHT, CU_PX, 256) && is(CP_MARGIN_BOTTOM, CU_PX, 256) &&
          is(CP_MARGIN_LEFT, CU_PX, 256), "margin: one value");
    sheet("p { margin: 1px 2px }");
    check(is(CP_MARGIN_TOP, CU_PX, 256) && is(CP_MARGIN_RIGHT, CU_PX, 512) && is(CP_MARGIN_BOTTOM, CU_PX, 256) &&
          is(CP_MARGIN_LEFT, CU_PX, 512), "margin: two");
    sheet("p { padding: 1px 2px 3px }");
    check(is(CP_PADDING_TOP, CU_PX, 256) && is(CP_PADDING_RIGHT, CU_PX, 512) && is(CP_PADDING_BOTTOM, CU_PX, 768) &&
          is(CP_PADDING_LEFT, CU_PX, 512), "padding: three");
    sheet("p { padding: 1px 2px 3px 4px }");
    check(is(CP_PADDING_TOP, CU_PX, 256) && is(CP_PADDING_RIGHT, CU_PX, 512) && is(CP_PADDING_BOTTOM, CU_PX, 768) &&
          is(CP_PADDING_LEFT, CU_PX, 1024), "padding: four");
    sheet("p { margin: 0 auto }");
    check(is(CP_MARGIN_TOP, CU_PX, 0) && is(CP_MARGIN_LEFT, CU_AUTO, 0), "margin: 0 auto");
    sheet("p { margin: 1px 2px 3px 4px 5px }");
    check(!c->nsels, "margin: five");
    sheet("p { padding: 1px auto }");
    check(!c->nsels, "padding: one bad value takes it all");

    /* font-size, font */
    sheet("p { font-size: 20px }");
    check(is(CP_SIZE, CU_PX, 20 * 256), "font-size in px");
    sheet("p { font-size: 150% }");
    check(is(CP_SIZE, CU_PCT, 150 * 256), "font-size in percent");
    sheet("p { font-size: x-large }");
    check(is(CP_SIZE, CU_BASE, 384), "font-size: x-large");
    sheet("p { font-size: larger }");
    check(is(CP_SIZE, CU_EM, 256 * 6 / 5), "font-size: larger");
    sheet("p { font-size: -1px }");
    check(!c->nsels, "a size is never negative");
    sheet("p { font: italic bold 12px/1.5 Georgia, \"Times New Roman\", serif }");
    check(is(CP_STYLE, CU_NONE, 1) && is(CP_WEIGHT, CU_NONE, 1) && is(CP_SIZE, CU_PX, 12 * 256), "font: all of it");
    sheet("p { font: 700 2em sans-serif }");
    check(is(CP_STYLE, CU_NONE, 0) && is(CP_WEIGHT, CU_NONE, 1) && is(CP_SIZE, CU_EM, 512), "font: a weight by number");
    sheet("p { font: x-small serif }");
    check(is(CP_STYLE, CU_NONE, 0) && is(CP_WEIGHT, CU_NONE, 0) && is(CP_SIZE, CU_BASE, 192), "font: what it does not name is set back");
    sheet("p { font: 12px }");
    check(!c->nsels, "font: no family");
    sheet("p { font: Georgia }");
    check(!c->nsels, "font: no size");
    sheet("p { font: inherit }");
    check(is(CP_STYLE, CU_INHERIT, 0) && is(CP_WEIGHT, CU_INHERIT, 0) && is(CP_SIZE, CU_INHERIT, 0), "font: inherit");
    sheet("p { font: bold }");
    check(!c->nsels, "font: a weight alone");

    /* the keywords every property has */
    sheet("p { font-weight: inherit; font-style: unset; font-size: inherit; color: initial; text-align: inherit }");
    check(is(CP_WEIGHT, CU_INHERIT, 0) && is(CP_STYLE, CU_INHERIT, 0) && is(CP_SIZE, CU_INHERIT, 0) &&
          is(CP_COLOR, CU_INITIAL, 0) && is(CP_ALIGN, CU_INHERIT, 0), "inherit, unset, initial");
    sheet("p { background-color: initial; display: initial; margin: initial; padding-top: unset }");
    check(is(CP_BACKGROUND, CU_TRANSPARENT, 0) && is(CP_DISPLAY, CU_NONE, CSS_D_INLINE) && is(CP_MARGIN_LEFT, CU_PX, 0) &&
          is(CP_PADDING_TOP, CU_PX, 0), "initial and unset of what is not inherited");
    sheet("p { font-weight: initial; font-size: initial; text-align: initial; text-decoration: initial }");
    check(is(CP_WEIGHT, CU_NONE, 0) && is(CP_SIZE, CU_BASE, 256) && is(CP_ALIGN, CU_NONE, CSS_A_LEFT) &&
          is(CP_DECORATION, CU_NONE, 0), "initial of what is");
    sheet("a { text-decoration: inherit }");
    check(is(CP_DECORATION, CU_INHERIT, 0), "text-decoration: inherit, not inherited but asked for");
    sheet("a { text-decoration: unset }");
    check(is(CP_DECORATION, CU_NONE, 0), "text-decoration: unset");
    sheet("p { font: revert; margin: revert; background: inherit; display: inherit !important }");
    check(is(CP_WEIGHT, CU_REVERT, 0) && is(CP_MARGIN_TOP, CU_REVERT, 0) && is(CP_BACKGROUND, CU_REVERT, 0) &&
          is(CP_DISPLAY, CU_REVERT, 0) && decl(CP_DISPLAY)->important, "revert, and inherit of what is not inherited");
    sheet("p { color: red inherit }");
    check(!c->nsels, "they stand alone");

    /* the end of a long value */
    {
        char s[400];
        size_t n = (size_t)snprintf(s, sizeof(s), "p { font: 12px a");
        int i;
        for(i = 0; i < 20; i++) n += (size_t)snprintf(s + n, sizeof(s) - n, ",b");
        snprintf(s + n, sizeof(s) - n, " !important }");
        sheet(s);
        check(is(CP_SIZE, CU_PX, 12 * 256) && decl(CP_SIZE)->important, "!important past the tokens looked at");
        n = (size_t)snprintf(s, sizeof(s), "p { background: url(a)");
        for(i = 0; i < 20; i++) n += (size_t)snprintf(s + n, sizeof(s) - n, ",url(b)");
        snprintf(s + n, sizeof(s) - n, " red }");
        sheet(s);
        check(is(CP_BACKGROUND, CU_RGB, 0xff0000), "a colour past them");
    }
}

static void syntax(void) {
    /* comments and CDO, CDC */
    sheet("/* a */ p /* b */ { /* c */ color /* d */ : /* e */ red /* f */ ; /* g */ } /* h");
    check(is(CP_COLOR, CU_RGB, 0xff0000), "comments anywhere, one left open at the end");
    sheet("p { color: /* } */ red }");
    check(is(CP_COLOR, CU_RGB, 0xff0000), "a brace in a comment");
    sheet("<!-- p { color: red } -->");
    check(is(CP_COLOR, CU_RGB, 0xff0000) && c->rules == 1, "<!-- and --> around a sheet");
    sheet("p { color: red } /*/ h2 { color: blue } /*/ h3 { color: lime }");
    check(c->nsels == 2 && is(CP_COLOR, CU_RGB, 0x00ff00), "/*/ opens a comment and does not close it");

    /* the end of the text closes what is open */
    sheet("p { color: red");
    check(c->rules == 1 && is(CP_COLOR, CU_RGB, 0xff0000), "a sheet's end closes a block");
    sheet("p { background: rgb(255,0,0");
    check(is(CP_BACKGROUND, CU_RGB, 0xff0000), "and a function");
    sheet("p { color: red; font-style: italic");
    check(c->ndecls == 2, "and a declaration without its ';'");
    nothing("p");
    nothing("p, h1");
    nothing("p {");
    nothing("p { color");
    nothing("p { color:");
    nothing("@media");
    nothing("@media screen");
    nothing("@");
    nothing("");
    nothing(" \t\r\n\f ");
    nothing("/*");
    nothing("\\");
    nothing("<!--");
    nothing("-->");

    /* strings and bad ones */
    sheet("p { font: 12px \"a;}b\"; color: red }");
    check(is(CP_COLOR, CU_RGB, 0xff0000) && is(CP_SIZE, CU_PX, 12 * 256), "a string keeps its ; and }");
    sheet("p { font: 12px \"a\\\"b\"; color: red }");
    check(is(CP_COLOR, CU_RGB, 0xff0000), "an escaped quote");
    sheet("p { font: 12px 'a\\\nb'; color: red }");
    check(is(CP_COLOR, CU_RGB, 0xff0000), "a line break escaped in a string");
    sheet("p { font: 12px \"a\n; color: red }");
    check(c->ndecls == 1 && is(CP_COLOR, CU_RGB, 0xff0000), "a string its line ends: that declaration goes alone");

    /* url() is one token */
    sheet("p { background: url(a;b}c) red; color: lime }");
    check(is(CP_BACKGROUND, CU_RGB, 0xff0000) && is(CP_COLOR, CU_RGB, 0x00ff00), "url(): its ; and } are its own");
    sheet("p { background: url(  a\\)b  ) red }");
    check(is(CP_BACKGROUND, CU_RGB, 0xff0000), "url(): an escaped bracket");
    sheet("p { background: URL(a) red }");
    check(is(CP_BACKGROUND, CU_RGB, 0xff0000), "URL(): any case");
    sheet("p { background: url(a b) red; color: lime }");
    check(is(CP_COLOR, CU_RGB, 0x00ff00) && !decl(CP_BACKGROUND), "a broken url() is still one token, and takes its declaration");
    sheet("p { background: url(a'b) red; color: lime }");
    check(is(CP_COLOR, CU_RGB, 0x00ff00) && !decl(CP_BACKGROUND), "a quote in one");
    sheet("p { background: url(a\\\nb) red; color: lime }");
    check(is(CP_COLOR, CU_RGB, 0x00ff00) && !decl(CP_BACKGROUND), "a line break after a backslash in one");
    {
        static const char zero[] = "p { background: url(a\0b) red }";
        css_free(c);
        css_add(c, zero, sizeof(zero) - 1);
        check(is(CP_BACKGROUND, CU_RGB, 0xff0000), "a NUL in one is U+FFFD");
    }
    sheet("p { background: url( a  ) red }");
    check(is(CP_BACKGROUND, CU_RGB, 0xff0000), "spaces around its address are fine");
    sheet("p { background: url(");
    check(is(CP_BACKGROUND, CU_TRANSPARENT, 0), "url() at the sheet's end");

    /* brackets */
    sheet("p { foo: ( ; ) ; color: red }");
    check(is(CP_COLOR, CU_RGB, 0xff0000) && c->ndecls == 1, "a ; in brackets is the value's");
    sheet("p { foo: [ } ] ; color: red }");
    check(is(CP_COLOR, CU_RGB, 0xff0000), "a } in brackets is the value's");
    sheet("p { color: red ) ; font-style: italic }");
    check(c->ndecls == 1 && is(CP_STYLE, CU_NONE, 1), "a stray bracket takes its declaration");
    sheet("p { foo: ( } h2 { color: red } ) ; color: blue }");
    check(c->nsels == 1 && is(CP_COLOR, CU_RGB, 0x0000ff), "an open bracket reaches past the }");

    /* at-rules */
    leaves("@media print { h2 { color: blue } }", 1, "@media print");
    leaves("@font-face { font-family: x; src: url(a.woff) }", 1, "@font-face");
    leaves("@keyframes k { from { color: blue } to { color: lime } }", 1, "@keyframes");
    leaves("@charset \"utf-8\";", 1, "@charset");
    leaves("@import url(x.css);", 1, "@import url()");
    leaves("@import \"x.css\";", 1, "@import");
    leaves("@namespace svg url(http://www.w3.org/2000/svg);", 1, "@namespace");
    leaves("@supports (display: grid) { h2 { color: blue } }", 1, "@supports");
    leaves("@foo bar { baz } ", 1, "an unknown at-rule with a block");
    leaves("@foo bar;", 1, "an unknown at-rule without");
    leaves("@foo (;{}) ;", 1, "a block in brackets is not its block");
    sheet("@media screen { h2 { color: blue } } p { color: red }");
    check(c->nsels == 2 && c->rules == 2, "@media screen");
    sheet("@media all { h2 { color: blue } }");
    check(c->nsels == 1, "@media all");
    sheet("@MEDIA SCREEN { h2 { color: blue } }");
    check(c->nsels == 1, "any case");
    sheet("@media { h2 { color: blue } }");
    check(c->nsels == 1, "@media with no query");
    sheet("@media print, screen { h2 { color: blue } }");
    check(c->nsels == 1, "@media: a list");
    sheet("@media only screen { h2 { color: blue } }");
    check(c->nsels == 1, "only");
    sheet("@media not print { h2 { color: blue } }");
    check(c->nsels == 1, "not print");
    sheet("@media not screen { h2 { color: blue } }");
    check(!c->nsels, "not screen");
    sheet("@media (max-width: 600px) { h2 { color: blue } }");
    check(!c->nsels, "a feature does not match");
    sheet("@media screen and (min-width: 600px) { h2 { color: blue } }");
    check(!c->nsels, "and with a type");
    sheet("@media screen { @media print { h2 { color: blue } } h3 { color: red } }");
    check(c->nsels == 1, "@media in @media");
    sheet("@media screen { @media screen { @media screen { @media screen { @media screen { h2 { color: blue } } h3 { color: red } } } } }");
    check(c->nsels == 1, "@media too deep is skipped, what is around it stays");
    sheet("@media screen { h2 { color: blue }");
    check(c->nsels == 1, "@media left open");
    sheet("@media screen { h2 { color: blue } h3 } p { color: red }");
    check(c->nsels == 2 && is(CP_COLOR, CU_RGB, 0xff0000), "@media's } ends a rule without a block");
    sheet("@media screen { @foo } p { color: red }");
    check(c->nsels == 1 && c->rules == 1, "@media's } ends an at-rule");
    sheet("@media [;p{color:red}]{} h2 { color: blue }");
    check(c->nsels == 1 && is(CP_COLOR, CU_RGB, 0x0000ff), "a block in brackets in a media query");

    /* a byte order mark */
    sheet("\xef\xbb\xbfp { color: red }");
    check(c->nsels == 1 && c->sels[0].key == T("p"), "a byte order mark is no name");
    sheet("\xef\xbb\xbf@charset \"utf-8\"; p { color: red }");
    check(c->rules == 1 && c->sels[0].key == T("p"), "before @charset too");

    /* escapes at every edge */
    sheet("p { color: re\\64 }");
    check(is(CP_COLOR, CU_RGB, 0xff0000), "an escape in a word");
    sheet("\\");
    check(!c->nsels, "a backslash last");
    {
        static const char zero[] = "p { color\0: red; margin-top: 4px\0; background: #abc\0def; display: none }";
        css_free(c);
        css_add(c, zero, sizeof(zero) - 1);
        check(c->ndecls == 1 && is(CP_DISPLAY, CU_NONE, CSS_D_NONE), "a NUL is no word's end");
    }
    sheet(".a\\\n{ color: red }");
    check(!c->nsels, "a line break after a backslash is no escape");
    sheet(".\\0 { color: red } .\\110000 { color: red } .\\d800 { color: red }");
    check(c->nsels == 3, "escapes of what is no character");
    check(c->sels[0].key == N("\xef\xbf\xbd") && c->sels[1].key == N("\xef\xbf\xbd") && c->sels[2].key == N("\xef\xbf\xbd"), "are U+FFFD");
    sheet(".\\1234567 { color: red }");
    check(c->nsels == 1, "an escape takes six digits");
    sheet(".\\41  b { color: red }");
    check(c->nsels == 1 && c->sels[0].nparts == 3, "one space ends an escape, the next is a combinator");
    sheet(".\\41\r\nb { color: red }");
    check(c->nsels == 1 && c->sels[0].key == N("Ab"), "CR LF ends an escape as one");
    /* bytes */
    sheet(".caf\xc3\xa9 { color: red }");
    check(c->sels[0].key == N("caf\xc3\xa9"), "UTF-8 in a name");
    sheet(".\xff\xfe { color: red }");
    check(c->nsels == 1, "bytes that are no UTF-8 in a name");
    {
        static const char zero[] = "p\0{ color: red } h2 { color: blue }";
        css_free(c);
        css_add(c, zero, sizeof(zero) - 1);
        check(c->rules == 2, "a NUL in a name");
    }
}

static char *text;                      /* big sheets */

static void limits(void) {
    size_t i, n;
    char msg[120];

    /* a sheet too big is left out whole */
    n = CSS_BYTES_MAX + 1;
    memset(text, ' ', n);
    memcpy(text, "p{color:red}", 12);
    css_free(c);
    css_add(c, text, n);
    check(!c->nsels && c->truncated, "a sheet over 1 MB is left out whole");
    css_free(c);
    css_add(c, text, n - 1);
    check(c->nsels == 1 && !c->truncated, "a sheet of 1 MB is read");

    /* rules */
    n = 0;
    for(i = 0; i < CSS_RULES_MAX + 10; i++) n += (size_t)sprintf(text + n, "p{color:red}");
    css_free(c);
    css_add(c, text, n);
    check(c->rules == CSS_RULES_MAX && c->truncated, "rules: past the limit the sheet stops, what came before stays");
    css_add(c, "h2{color:red}", 13);
    check(c->rules == CSS_RULES_MAX, "and no sheet after it adds any");

    /* selectors of a page */
    n = 0;
    for(i = 0; i < CSS_SELECTORS_MAX / CSS_RULE_SELECTORS + 2; i++) {
        size_t k;
        for(k = 0; k < CSS_RULE_SELECTORS; k++) n += (size_t)sprintf(text + n, "%sp", k ? "," : "");
        n += (size_t)sprintf(text + n, "{color:red}");
    }
    css_free(c);
    css_add(c, text, n);
    check(c->nsels <= CSS_SELECTORS_MAX && c->truncated && c->nsels >= CSS_SELECTORS_MAX - CSS_RULE_SELECTORS, "selectors of a page");

    /* selectors of a rule: a longer list loses its rule, the next stays */
    n = 0;
    for(i = 0; i < CSS_RULE_SELECTORS + 1; i++) n += (size_t)sprintf(text + n, "%sh2", i ? "," : "");
    n += (size_t)sprintf(text + n, "{color:red}p{color:blue}");
    css_free(c);
    css_add(c, text, n);
    check(c->nsels == 1 && c->rules == 1 && !c->truncated && is(CP_COLOR, CU_RGB, 0x0000ff), "a rule of too many selectors goes alone");
    n = 0;
    for(i = 0; i < CSS_RULE_SELECTORS; i++) n += (size_t)sprintf(text + n, "%sh2", i ? "," : "");
    n += (size_t)sprintf(text + n, "{color:red}");
    css_free(c);
    css_add(c, text, n);
    check(c->nsels == CSS_RULE_SELECTORS, "a rule of as many as may be stays");

    /* parts of a selector: a longer one matches nothing */
    n = (size_t)sprintf(text, "h1,");
    for(i = 0; i < CSS_SEL_PARTS + 1; i++) n += (size_t)sprintf(text + n, ".a");
    n += (size_t)sprintf(text + n, "{color:red}");
    css_free(c);
    css_add(c, text, n);
    check(c->nsels == 1 && c->rules == 1, "a selector of too many parts matches nothing, its list stays");

    /* the parts of a page */
    n = 0;
    for(i = 0; i < CSS_PARTS_MAX / CSS_SEL_PARTS + 2; i++) {
        size_t k;
        for(k = 0; k < CSS_SEL_PARTS; k++) n += (size_t)sprintf(text + n, ".a");
        n += (size_t)sprintf(text + n, "{color:red}");
    }
    css_free(c);
    css_add(c, text, n);
    check(c->nparts <= CSS_PARTS_MAX && c->truncated && c->nparts > CSS_PARTS_MAX - CSS_SEL_PARTS, "parts of a page");

    /* declarations */
    n = (size_t)sprintf(text, "p{");
    for(i = 0; i < CSS_DECLS_MAX / 4 + 10; i++) n += (size_t)sprintf(text + n, "margin:0;");
    n += (size_t)sprintf(text + n, "}");
    css_free(c);
    css_add(c, text, n);
    check(c->ndecls <= CSS_DECLS_MAX && c->truncated, "declarations of a page");

    /* depth: every kind of bracket, in every place */
    {
        static const char *const open[] = { "(", "[", "{", "f(" };
        static const char *const before[] = { "", "p{", "p{a:", "@media screen{", "@x ", "p{}p", "[", ":not(" };
        size_t b, k;
        for(b = 0; b < sizeof(before) / sizeof(before[0]); b++)
            for(k = 0; k < sizeof(open) / sizeof(open[0]); k++) {
                n = (size_t)sprintf(text, "h1{color:red}%s", before[b]);
                for(i = 0; i < 300000 && n < CSS_BYTES_MAX - 8; i++) n += (size_t)sprintf(text + n, "%s", open[k]);
                css_free(c);
                css_add(c, text, n);
                snprintf(msg, sizeof(msg), "deep brackets: \"%s\" then %s: what came before stays", before[b], open[k]);
                check(c->rules >= 1, msg);
            }
    }
    /* as deep as may be is read, and the rule after it */
    n = (size_t)sprintf(text, "p{a:");
    for(i = 0; i < CSS_DEPTH_MAX; i++) n += (size_t)sprintf(text + n, "(");
    for(i = 0; i < CSS_DEPTH_MAX; i++) n += (size_t)sprintf(text + n, ")");
    n += (size_t)sprintf(text + n, ";color:red}");
    css_free(c);
    css_add(c, text, n);
    check(is(CP_COLOR, CU_RGB, 0xff0000) && !c->truncated, "brackets as deep as may be");
    n = (size_t)sprintf(text, "h1{color:red}p{a:");
    for(i = 0; i < CSS_DEPTH_MAX + 1; i++) n += (size_t)sprintf(text + n, "(");
    n += (size_t)sprintf(text + n, ";color:red}");
    css_free(c);
    css_add(c, text, n);
    check(c->rules == 1 && c->truncated, "one deeper stops the sheet, what came before stays");

    /* long names and long values */
    n = (size_t)sprintf(text, ".");
    for(i = 0; i < 100000; i++) text[n++] = 'a';
    n += (size_t)sprintf(text + n, "{color:red}");
    css_free(c);
    css_add(c, text, n);
    check(c->nsels == 1 && is(CP_COLOR, CU_RGB, 0xff0000), "a very long class");
    n = (size_t)sprintf(text, "p{");
    for(i = 0; i < 100000; i++) text[n++] = 'a';
    n += (size_t)sprintf(text + n, ":b;color:red}");
    css_free(c);
    css_add(c, text, n);
    check(c->ndecls == 1, "a very long property");
    n = (size_t)sprintf(text, "p{margin:");
    for(i = 0; i < 100000; i++) n += (size_t)sprintf(text + n, "1px ");
    n += (size_t)sprintf(text + n, ";color:red}");
    css_free(c);
    css_add(c, text, n);
    check(c->ndecls == 1, "a very long value is too long for margin");
    n = (size_t)sprintf(text, "p{font:12px a");
    for(i = 0; i < 100000; i++) n += (size_t)sprintf(text + n, ",b");
    n += (size_t)sprintf(text + n, ";color:red}");
    css_free(c);
    css_add(c, text, n);
    check(c->ndecls == 4, "a very long font list: only its start is looked at");
    n = (size_t)sprintf(text, "p{background:url(");
    for(i = 0; i < 500000; i++) text[n++] = 'a';
    n += (size_t)sprintf(text + n, ") red}");
    css_free(c);
    css_add(c, text, n);
    check(is(CP_BACKGROUND, CU_RGB, 0xff0000), "a very long url()");
    n = (size_t)sprintf(text, "p{font:12px \"");
    for(i = 0; i < 500000; i++) text[n++] = 'a';
    n += (size_t)sprintf(text + n, "\";color:red}");
    css_free(c);
    css_add(c, text, n);
    check(is(CP_COLOR, CU_RGB, 0xff0000), "a very long string");
}

static void attributes(void) {
    struct css_decl d[8];
    size_t n;
    n = css_style("color:red", 9, d, 8);
    check(n == 1 && d[0].prop == CP_COLOR && d[0].value == 0xff0000 && !d[0].important, "a style attribute");
    n = css_style(";;color:red ; ; margin-top:1px!important;", 41, d, 8);
    check(n == 2 && d[1].prop == CP_MARGIN_TOP && d[1].important, "empty declarations, !important");
    n = css_style("color:red; foo:bar; nonsense; color:nope; margin-top:1px", 56, d, 8);
    check(n == 2 && d[1].prop == CP_MARGIN_TOP, "what it does not know is skipped");
    n = css_style("} color:red; margin-top:1px", 27, d, 8);
    check(n == 1 && d[0].prop == CP_MARGIN_TOP, "a stray } takes its declaration");
    n = css_style("color:red } ; margin-top:1px", 28, d, 8);
    check(n == 1 && d[0].prop == CP_MARGIN_TOP, "a } in a value too");
    n = css_style("h1 { color:red }", 16, d, 8);
    check(n == 0, "a rule is no attribute");
    n = css_style("margin:1px;padding:2px", 22, d, 3);
    check(n == 3, "no more than there is room for");
    n = css_style("", 0, d, 8);
    check(n == 0, "an empty attribute");
    n = css_style("color", 5, d, 8);
    check(n == 0, "a word");
    n = css_style("color:", 6, d, 8);
    check(n == 0, "a word and a colon");
    n = css_style("color:rgb(255,0,0", 17, d, 8);
    check(n == 1 && d[0].value == 0xff0000, "the attribute's end closes a function");
}

static void media(void) {
    static const struct { const char *s; int yes; } cases[] = {
        { "", 1 }, { "screen", 1 }, { "SCREEN", 1 }, { "all", 1 }, { "print", 0 }, { "speech", 0 }, { "tv", 0 },
        { "screen, print", 1 }, { "print, screen", 1 }, { "print, tv", 0 }, { "only screen", 1 }, { "not print", 1 },
        { "not screen", 0 }, { "not all", 0 }, { "screen and (min-width: 1px)", 0 }, { "(min-width: 1px)", 0 },
        { "(min-width: 1px), screen", 1 }, { "not screen and (color)", 0 }, { "screen,", 1 }, { ",", 0 }, { "12", 0 },
        { "print ,, screen", 1 }, { "[", 0 }, { "screen (", 0 }, { "not", 0 }, { "only", 0 }, { "print screen", 0 },
        { "tv all", 0 }, { "screen and", 0 }, { "and screen", 0 }, { "screen or print", 0 }, { "[,screen", 0 },
        { "not print and", 0 }, { "not not print", 0 }, { "screen and print", 0 }, { "(color) (color)", 0 },
        { "screen and(color)", 0 }, { "only print", 0 }, { "not (color)", 0 }, { "not or", 0 }, { "not layer", 0 }
    };
    size_t i;
    char msg[100];
    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(msg, sizeof(msg), "media \"%s\"", cases[i].s);
        check(css_media(cases[i].s, strlen(cases[i].s)) == cases[i].yes, msg);
    }
}

static void imports(void) {
    static char urls[CSS_IMPORTS_MAX][CSS_URL_MAX + 1];
    static const char a[] = "@charset \"utf-8\"; /* c */ @import \"a.css\"; @import url(b.css); @import url(\"c.css\");\n"
                            "@import \"d.css\" print; @import 'e.css' screen; @layer base; @import \"f\\.css\";"
                            "h1 { color: red } @import \"g.css\";";
    size_t n = css_imports(a, strlen(a), urls, CSS_IMPORTS_MAX);
    char *big;
    check(n == 3 && !strcmp(urls[0], "a.css") && !strcmp(urls[1], "e.css") && !strcmp(urls[2], "f.css"),
          "@import: strings only, media kept to the screen, before the rules only");
    n = css_imports("@import \"a\"", 11, urls, CSS_IMPORTS_MAX);
    check(n == 1, "@import ended by the sheet's end");
    n = css_imports("\xef\xbb\xbf@import \"a\";", 15, urls, CSS_IMPORTS_MAX);
    check(n == 1, "after a byte order mark");
    n = css_imports("@import \"a\0b\";", 14, urls, CSS_IMPORTS_MAX);
    check(n == 1 && !strcmp(urls[0], "a\xef\xbf\xbd" "b"), "a NUL in an address is U+FFFD: nothing is cut there");
    n = css_imports("@import \"a\" {} @import \"b\";", 26, urls, CSS_IMPORTS_MAX);
    check(n == 0, "@import with a block is none");
    n = css_imports("@import 12; @import \"b\";", 24, urls, CSS_IMPORTS_MAX);
    check(n == 1 && !strcmp(urls[0], "b"), "@import of a number");
    n = css_imports("@import \"\";", 11, urls, CSS_IMPORTS_MAX);
    check(n == 0, "@import of nothing");
    n = css_imports("@layer a {} @import \"b\";", 24, urls, CSS_IMPORTS_MAX);
    check(n == 0, "a @layer with a block is a rule");
    n = css_imports("@font-face {} @import \"b\";", 26, urls, CSS_IMPORTS_MAX);
    check(n == 0, "any other at-rule ends them");
    big = malloc(CSS_URL_MAX + 64);
    if(!big) exit(2);
    n = (size_t)sprintf(big, "@import \"");
    memset(big + n, 'a', CSS_URL_MAX + 1);
    n += CSS_URL_MAX + 1;
    n += (size_t)sprintf(big + n, "\"; @import \"b\";");
    n = css_imports(big, n, urls, CSS_IMPORTS_MAX);
    check(n == 1 && !strcmp(urls[0], "b"), "@import of an address too long is left out");
    free(big);
    for(n = 0; n < 40; n++) memcpy(text + n * 12, "@import \"a\";", 12);
    n = css_imports(text, 40 * 12, urls, CSS_IMPORTS_MAX);
    check(n == CSS_IMPORTS_MAX, "@import: no more than there is room for");
    n = css_imports("@import \"a\"; @import \"b\";", 25, urls, 1);
    check(n == 1, "no more than the caller's room");
}

/* Pages' sheets broken in every way: cut at every byte, each byte changed, a slice of bytes doubled or dropped. What
   counts is that nothing runs off anything (the sanitizers' copy says so too) and that nothing is left allocated. */
static const char sample[] =
    "@charset \"utf-8\"; @import url(a.css); /* comment */\n"
    "html, body { margin: 0; padding: 0 } a:link, a:visited { color: #0645ad; text-decoration: none }\n"
    "#main > .content p.first, ul li a { font: italic bold 12px/1.5 \"Georgia\", serif; background: url(x.png) #fff }\n"
    "h1::before { content: \"}\" } .w-1\\/2, .\\31 23 { margin: 0 auto 1em 10%; padding: 1px 2px !important }\n"
    "@media screen and (max-width: 600px) { .x { display: none } } @media print { p { color: rgb(0 0 0 / 50%) } }\n"
    "@media screen { .y { color: hsl(120, 100%, 50%); background-color: rgba(1, 2, 3, .5) } }\n"
    "p { color: red; foo: (a;b); } <!-- --> [x=\"y\"] { color: blue } li + li { font-size: larger }\n";

static void mangled(void) {
    size_t n = sizeof(sample) - 1, i, k;
    char msg[100];
    static const unsigned char bytes[] = { 0, '\\', '"', '\'', '(', ')', '{', '}', '[', ']', ';', ':', ',', '/', '*', '<',
                                           '!', '-', '@', '#', '.', '\n', 0x80, 0xff };
    struct css_decl d[8];
    for(i = 0; i <= n; i++) {
        css_free(c);
        css_add(c, sample, i);
        css_style(sample, i, d, 8);
        css_media(sample, i);
    }
    snprintf(msg, sizeof(msg), "the sample comes to %u selectors", (unsigned int)c->nsels);
    css_free(c);
    css_add(c, sample, n);
    check(c->nsels >= 9 && !c->truncated, msg);
    for(i = 0; i < n; i++)
        for(k = 0; k < sizeof(bytes); k++) {
            char s[sizeof(sample)];
            memcpy(s, sample, n);
            s[i] = (char)bytes[k];
            css_free(c);
            css_add(c, s, n);
            css_style(s, n, d, 8);
            css_media(s, n);
        }
    for(i = 0; i + 40 < n; i += 3) {
        memcpy(text, sample, i + 40);               /* a slice twice */
        memcpy(text + i + 40, sample + i, n - i);
        css_free(c);
        css_add(c, text, n + 40);
        css_free(c);
        memcpy(text, sample, i);                    /* a slice gone */
        memcpy(text + i, sample + i + 40, n - i - 40);
        css_add(c, text, n - 40);
    }
    css_free(c);
    {                                               /* random bytes from a fixed seed, and tokens at random */
        static const char *const tokens[] = {
            "p", ".a", "#b", " ", ">", ",", "{", "}", "(", ")", "[", "]", ";", ":", "::", "!important", "color", "red",
            "margin", "1px", "@media", "@import", "\"s\"", "url(", "/*", "*/", "\\", "<!--", "-->", "rgb(", "%", "1e9"
        };
        unsigned int seed = 12345;
        size_t round;
        for(round = 0; round < 3000; round++) {
            size_t m = 0;
            for(i = 0; i < 400; i++) {
                seed = seed * 1103515245u + 12345u;
                if(round & 1) text[m++] = (char)(seed >> 16);
                else {
                    const char *t = tokens[(seed >> 16) % (sizeof(tokens) / sizeof(tokens[0]))];
                    size_t l = strlen(t);
                    memcpy(text + m, t, l);
                    m += l;
                }
            }
            css_free(c);
            css_add(c, text, m);
            css_style(text, m, d, 8);
            css_media(text, m);
        }
    }
    css_free(c);
}

static void allocations(void) {
    long i, before = ut_alloc_live;
    int done = 0;
    for(i = 0; i < 200 && !done; i++) {
        int rc;
        css_init(c);
        ut_alloc_fail_after = i;
        rc = css_add(c, sample, sizeof(sample) - 1);
        ut_alloc_fail_after = -1;
        check(rc == (c->failed ? -1 : 0), "every allocation refused in turn: it says so");
        done = !c->failed;
        css_free(c);
    }
    check(done && i > 2, "the sample's allocations were all refused in turn");
    check(ut_alloc_live == before, "and every allocation was given back");
}

int main(void) {
    c = malloc(sizeof(*c));
    text = malloc(4 * CSS_BYTES_MAX);
    if(!c || !text) return 2;
    css_init(c);
    selectors();
    values();
    syntax();
    limits();
    attributes();
    media();
    imports();
    mangled();
    allocations();
    css_free(c);
    free(c);
    free(text);
    if(failures) {
        printf("WUNDERTOW CSS TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW CSS TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
