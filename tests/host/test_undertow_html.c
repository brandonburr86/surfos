/*
SurfOS Undertow: tests of the HTML renderer
----------------------
File: test_undertow_html.c  Date: 10/11/26
----------------------
user/undertow/html.c (with url.c and util.c) on the host. Two kinds of test:

- The fixtures in tests/fixtures/undertow/. NAME.html (NAME.body when it is
  not HTML) is a response body;
  NAME.txt the text Undertow must make of it (with one final newline, which
  the text itself does not have); NAME.doc the response it came in (url:,
  type:, charset:) and the rest of the document: title, notice, truncated,
  every link (its number, "(unusable)" when it cannot be followed, its
  address, then its label), every fragment target (anchor NAME: LINE:COLUMN
  of the text, counted from 1, in characters) and every heading span
  (heading LINE: its text). Lines starting with # are comments.
  "test_undertow_html --dump FILE [URL [TYPE [CHARSET]]]" prints the same for
  any file: the text, a line "----", then the .doc lines.
- Pages made here: entities, charsets, hostile bytes (ESC, C1, bidirectional
  overrides, NUL, broken UTF-8) that must never reach the output, the link,
  anchor, field and text limits, every allocation failing in turn, random
  torture pages, and 1 MiB pages made to find anything slow or deep.

The allocator below stands in for malloc and its friends in the modules (the
Makefile renames them) and counts bytes, so a document's bytes field is
checked against what the document really holds.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "undertow.h"
#include "url.h"
#include "http.h"
#include "html.h"

#define PAGE_URL "https://example.test/dir/page.html"
#define LIT(s) s, sizeof(s) - 1     /* a literal and its length, NULs included */
#define NOTICE "\n\n[Undertow: the rest of this page is not shown: it is over 512 KB]"

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/**** the modules' allocator: counts blocks and bytes, fails on request ****/

struct hdr { size_t size, pad; };
static long live_blocks, alloc_calls, fail_after = -1;
static size_t live_bytes, peak_bytes;

static int refuse(void) {
    alloc_calls++;
    if(fail_after < 0) return 0;
    if(fail_after == 0) return 1;
    fail_after--;
    return 0;
}

static void *take(size_t n) {
    struct hdr *h = malloc(sizeof(*h) + n);
    if(!h) return NULL;
    h->size = n;
    live_blocks++;
    live_bytes += n;
    if(live_bytes > peak_bytes) peak_bytes = live_bytes;
    return h + 1;
}

void *ut_test_malloc(size_t n) {
    return refuse() ? NULL : take(n);
}

void *ut_test_calloc(size_t n, size_t size) {
    void *p;
    if(refuse() || (size && n > (size_t)-1 / size)) return NULL;
    p = take(n * size);
    if(p) memset(p, 0, n * size);
    return p;
}

void *ut_test_realloc(void *p, size_t n) {
    struct hdr *h, *q;
    if(!p) return ut_test_malloc(n);
    if(refuse()) return NULL;
    h = (struct hdr *)p - 1;
    q = realloc(h, sizeof(*q) + n);
    if(!q) return NULL;
    live_bytes = live_bytes - q->size + n;
    if(live_bytes > peak_bytes) peak_bytes = live_bytes;
    q->size = n;
    return q + 1;
}

void ut_test_free(void *p) {
    struct hdr *h;
    if(!p) return;
    h = (struct hdr *)p - 1;
    live_blocks--;
    live_bytes -= h->size;
    free(h);
}

char *ut_test_strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *p = ut_test_malloc(len + 1);
    if(!p) return NULL;
    memcpy(p, s, len);
    p[len] = 0;
    return p;
}

char *ut_test_strdup(const char *s) {
    return ut_test_strndup(s, strlen(s));
}

unsigned int uptime(void) { return 0; }        /* util.c's ut_now(); the renderer never waits */

/**** strings for reports ****/

struct sb { char *s; size_t len, cap; };

static void sb_add(struct sb *b, const char *s, size_t n) {
    if(b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
        if(!b->s) abort();
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void sb_str(struct sb *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_printf(struct sb *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if(n < 0) n = 0;
    if((size_t)n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    sb_add(b, tmp, (size_t)n);
}

/**** responses ****/

static struct ut_response *response(const char *url, const char *type, const char *charset,
                                    const void *body, size_t len) {
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

/**** what every document must be ****/

/* Characters that are invisible on a console, which Undertow drops too (the joiners U+200C and U+200D stay) */
static int invisible(unsigned int cp) {
    return cp == 0x61c || cp == 0xad || cp == 0x34f || cp == 0x180e || cp == 0x200b || (cp >= 0x2060 && cp <= 0x2064) ||
           (cp >= 0xfe00 && cp <= 0xfe0f);
}

/* Well-formed UTF-8 with no character that is unsafe on a terminal or invisible; newlines only if allowed */
static int safe_utf8(const char *s, size_t len, int newlines) {
    size_t i = 0, n;
    unsigned int cp;
    while(i < len) {
        n = ut_utf8_decode((const unsigned char *)s + i, len - i, &cp);
        if(n == 1 && (unsigned char)s[i] >= 0x80) return 0;
        if(!(cp == '\n' && newlines) && (ut_unsafe_char(cp) || invisible(cp))) return 0;
        i += n;
    }
    return 1;
}

/* No space or control byte: what url.c accepts in an address (non-ASCII is fine, http.c encodes it) */
static int no_space(const char *s) {
    for(; *s; s++) if((unsigned char)*s < 0x21 || *s == 0x7f) return 0;
    return 1;
}

/* The first rule d breaks, or NULL. held: the bytes the allocator says the render kept. */
static const char *bad_doc(const struct ut_document *d, size_t held) {
    static char why[200];
    const char *t = d->text;
    size_t i, prev_end = 0, link_spans = 0;
    if(!t || !d->title) return "no text or no title";
    if(strlen(t) != d->text_len) return "text_len is not the text's length (a NUL inside?)";
    if(!safe_utf8(t, d->text_len, 1)) return "the text is unsafe or not UTF-8";
    if(d->text_len && (t[0] == '\n' || t[d->text_len - 1] == '\n')) return "a blank line at an end of the text";
    if(strstr(t, "\n\n\n")) return "two blank lines in a row";
    if(strstr(t, " \n") || (d->text_len && t[d->text_len - 1] == ' ')) return "a trailing space";
    if(d->text_len > UT_TEXT_MAX) return "the text is over UT_TEXT_MAX";
    if(strlen(d->title) > UT_FIELD_MAX || !safe_utf8(d->title, strlen(d->title), 0)) return "the title is unsafe or too long";
    if(d->title[0] == ' ' || strstr(d->title, "  ") || (d->title[0] && d->title[strlen(d->title) - 1] == ' '))
        return "the title's whitespace is not collapsed";
    if(strlen(d->notice) >= sizeof(d->notice) || !safe_utf8(d->notice, strlen(d->notice), 0)) return "an unsafe notice";
    if(d->link_count > UT_LINKS_MAX) return "too many links";
    for(i = 0; i < d->link_count; i++) {
        const struct ut_link *l = &d->links[i];
        if(!l->label || !l->url) return "a link without label or address";
        if(strlen(l->label) > UT_FIELD_MAX || !safe_utf8(l->label, strlen(l->label), 0)) return "an unsafe or long label";
        if(!safe_utf8(l->url, strlen(l->url), 0) || strlen(l->url) > UT_URL_MAX) return "an unsafe or long link address";
        if(l->usable) {
            struct ut_url *u = malloc(sizeof(*u));
            char err[UT_ERR_MAX];
            int ok = u && !ut_url_parse(l->url, u, err, sizeof(err)) && !strcmp(u->text, l->url);
            free(u);
            if(!ok || !no_space(l->url)) {
                snprintf(why, sizeof(why), "usable link %u is not a parsed address without spaces", (unsigned)i + 1);
                return why;
            }
        }
    }
    if(d->anchor_count > UT_ANCHORS_MAX) return "too many anchors";
    for(i = 0; i < d->anchor_count; i++) {
        size_t j;
        if(!d->anchors[i].name || !d->anchors[i].name[0] || strlen(d->anchors[i].name) > UT_FIELD_MAX ||
           !safe_utf8(d->anchors[i].name, strlen(d->anchors[i].name), 0)) return "a bad anchor name";
        if(d->anchors[i].offset > d->text_len) return "an anchor past the text";
        if(i && d->anchors[i].offset < d->anchors[i - 1].offset) return "anchors out of order";
        for(j = 0; j < i; j++) if(!strcmp(d->anchors[j].name, d->anchors[i].name)) return "an anchor name twice";
    }
    if(d->span_count > UT_SPANS_MAX) return "too many spans";
    for(i = 0; i < d->span_count; i++) {
        const struct ut_span *s = &d->spans[i];
        if(!s->len || s->offset + s->len > d->text_len || s->offset < prev_end) return "a span out of order or bounds";
        if(memchr(t + s->offset, '\n', s->len)) return "a span across a line break";
        prev_end = s->offset + s->len;
        if(s->kind == UT_SPAN_LINK) {
            char marker[16];
            snprintf(marker, sizeof(marker), "[%u]", (unsigned)++link_spans);
            if(s->len != strlen(marker) || memcmp(t + s->offset, marker, s->len)) return "a link span that is not the next [N]";
        } else if(s->kind != UT_SPAN_HEADING) return "a span of no known kind";
        else if(t[s->offset] == ' ' || t[s->offset + s->len - 1] == ' ') return "a heading span with spaces at an end";
    }
    if(link_spans != d->link_count) return "a numbered link without its [N] in the text";
    if((d->truncated & UT_TRUNC_TEXT) && strcmp(t, &NOTICE[2]) &&       /* the notice, alone or after a blank line */
       (d->text_len < strlen(NOTICE) || strcmp(t + d->text_len - strlen(NOTICE), NOTICE)))
        return "a truncated text without its notice";
    if(d->bytes != held) {
        snprintf(why, sizeof(why), "bytes says %lu, the document holds %lu", (unsigned long)d->bytes, (unsigned long)held);
        return why;
    }
    return NULL;
}

/* Render r into *d (zeroed first) and check it. 0, or -1 when ut_render() failed. */
static int render(const struct ut_response *r, struct ut_document *d, const char *what) {
    char err[UT_ERR_MAX], msg[300];
    size_t before = live_bytes;
    const char *why;
    memset(d, 0, sizeof(*d));
    if(ut_render(r, d, err, sizeof(err))) {
        snprintf(msg, sizeof(msg), "%.150s: ut_render failed: %.120s", what, err);
        check(0, msg);
        return -1;
    }
    why = bad_doc(d, live_bytes - before);
    snprintf(msg, sizeof(msg), "%s: %s", what, why ? why : "a good document");
    check(!why, msg);
    return 0;
}

/**** the fixture format ****/

static void line_col(const struct ut_document *d, size_t off, unsigned *line, unsigned *col) {
    size_t i = 0;
    unsigned int cp;
    *line = 1;
    *col = 1;
    while(i < off && i < d->text_len) {
        if(d->text[i] == '\n') {
            ++*line;
            *col = 1;
            i++;
            continue;
        }
        i += ut_utf8_decode((const unsigned char *)d->text + i, d->text_len - i, &cp);
        ++*col;
    }
}

static void field(struct sb *b, const char *key, const char *value) {
    sb_str(b, key);
    sb_str(b, ":");
    if(value[0]) {
        sb_str(b, " ");
        sb_str(b, value);
    }
    sb_str(b, "\n");
}

/* Everything but the text, in the .doc format (malloc'ed) */
static char *dump(const struct ut_document *d, const struct ut_response *r) {
    struct sb b = { 0, 0, 0 };
    unsigned line, col;
    size_t i;
    field(&b, "url", r->url.text);
    field(&b, "type", r->content_type);
    field(&b, "charset", r->charset);
    field(&b, "title", d->title);
    field(&b, "notice", d->notice);
    sb_str(&b, "truncated:");
    if(d->truncated & UT_TRUNC_TEXT) sb_str(&b, " text");
    if(d->truncated & UT_TRUNC_LINKS) sb_str(&b, " links");
    if(d->truncated & UT_TRUNC_ANCHORS) sb_str(&b, " anchors");
    sb_str(&b, "\n");
    for(i = 0; i < d->link_count; i++) {
        sb_printf(&b, "link %u%s: ", (unsigned)i + 1, d->links[i].usable ? "" : " (unusable)");
        sb_str(&b, d->links[i].url);
        sb_str(&b, "\n");
        field(&b, "  label", d->links[i].label);
    }
    for(i = 0; i < d->anchor_count; i++) {
        line_col(d, d->anchors[i].offset, &line, &col);
        sb_str(&b, "anchor ");
        sb_str(&b, d->anchors[i].name);
        sb_printf(&b, ": %u:%u\n", line, col);
    }
    for(i = 0; i < d->span_count; i++) {
        if(d->spans[i].kind != UT_SPAN_HEADING) continue;
        line_col(d, d->spans[i].offset, &line, &col);
        sb_printf(&b, "heading %u: ", line);
        sb_add(&b, d->text + d->spans[i].offset, d->spans[i].len);
        sb_str(&b, "\n");
    }
    if(!b.s) sb_str(&b, "");
    return b.s;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *s = NULL;
    size_t cap = 0, n = 0, got;
    if(!f) return NULL;
    do {
        if(n + 4096 + 1 > cap) {
            cap = (n + 4096 + 1) * 2;
            s = realloc(s, cap);
            if(!s) exit(2);
        }
        got = fread(s + n, 1, 4096, f);
        n += got;
    } while(got);
    fclose(f);
    s[n] = 0;
    if(len) *len = n;
    return s;
}

/* The value of "key: value" in a .doc file, or "" */
static void doc_value(const char *doc, const char *key, char *out, size_t cap) {
    size_t k = strlen(key);
    const char *p = doc, *end;
    out[0] = 0;
    while(p && *p) {
        if(!strncmp(p, key, k) && p[k] == ':') {
            p += k + 1;
            while(*p == ' ') p++;
            end = strchr(p, '\n');
            if(!end) end = p + strlen(p);
            snprintf(out, cap, "%.*s", (int)(end - p), p);
            return;
        }
        p = strchr(p, '\n');
        if(p) p++;
    }
}

static char *without_comments(const char *s) {
    struct sb b = { 0, 0, 0 };
    const char *end;
    sb_str(&b, "");
    while(*s) {
        end = strchr(s, '\n');
        end = end ? end + 1 : s + strlen(s);
        if(*s != '#') sb_add(&b, s, (size_t)(end - s));
        s = end;
    }
    return b.s;
}

/* Report the first line where got and want differ */
static void same(const char *name, const char *what, const char *got, const char *want) {
    char msg[200];
    unsigned line = 1;
    const char *g = got, *w = want;
    snprintf(msg, sizeof(msg), "fixture %s: %s", name, what);
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
    "paragraphs", "headings", "lists", "pre", "tables", "images", "forms", "comments", "hidden",
    "attributes", "mismatched", "links", "anchors", "base", "entities", "utf8", "unknown-charset", "plain",
    "sniff", "xhtml", "article"
};

static void fixture(const char *dir, const char *name) {
    char path[512], url[UT_URL_MAX + 1], type[64], charset[32], msg[600];
    char *html, *txt, *doc, *want, *got;
    size_t len;
    struct ut_response *r;
    struct ut_document d;
    struct sb text = { 0, 0, 0 };
    snprintf(path, sizeof(path), "%s/%s.html", dir, name);
    html = read_file(path, &len);
    if(!html) {
        snprintf(path, sizeof(path), "%s/%s.body", dir, name);       /* a body that is not HTML */
        html = read_file(path, &len);
    }
    snprintf(path, sizeof(path), "%s/%s.txt", dir, name);
    txt = read_file(path, NULL);
    snprintf(path, sizeof(path), "%s/%s.doc", dir, name);
    doc = read_file(path, NULL);
    if(!html || !txt || !doc) {
        snprintf(msg, sizeof(msg), "fixture %s: its files are in %s", name, dir);
        check(0, msg);
        free(html); free(txt); free(doc);
        return;
    }
    doc_value(doc, "url", url, sizeof(url));
    doc_value(doc, "type", type, sizeof(type));
    doc_value(doc, "charset", charset, sizeof(charset));
    r = response(url[0] ? url : PAGE_URL, type, charset, html, len);
    snprintf(msg, sizeof(msg), "fixture %s", name);
    if(render(r, &d, msg) == 0) {
        sb_add(&text, d.text, d.text_len);
        if(d.text_len) sb_str(&text, "\n");
        if(!text.s) sb_str(&text, "");
        same(name, "text", text.s, txt);
        got = dump(&d, r);
        want = without_comments(doc);
        same(name, "title, notice, links, anchors and headings", got, want);
        free(got);
        free(want);
        free(text.s);
        ut_document_free(&d);
    }
    response_free(r);
    free(html);
    free(txt);
    free(doc);
}

static int dump_file(int argc, char **argv) {
    size_t len;
    char *html = read_file(argv[2], &len), *rest;
    struct ut_response *r;
    struct ut_document d;
    char err[UT_ERR_MAX];
    if(!html) {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }
    r = response(argc > 3 ? argv[3] : PAGE_URL, argc > 4 ? argv[4] : "text/html", argc > 5 ? argv[5] : "", html, len);
    if(ut_render(r, &d, err, sizeof(err))) {
        fprintf(stderr, "ut_render: %s\n", err);
        return 1;
    }
    rest = dump(&d, r);
    printf("%s%s----\n%s", d.text, d.text_len ? "\n" : "", rest);
    free(rest);
    ut_document_free(&d);
    response_free(r);
    free(html);
    return 0;
}

/**** pages made here ****/

static struct ut_document pg;       /* the last page() */
static int pg_ok;

static void page(const char *type, const char *charset, const char *body, size_t len, const char *what) {
    struct ut_response *r = response(PAGE_URL, type, charset, body, len);
    ut_document_free(&pg);
    pg_ok = render(r, &pg, what) == 0;
    response_free(r);
}

/* body as text/html in UTF-8: its text must be want */
static void text_is(const char *body, const char *want, const char *what) {
    page("text/html", "utf-8", body, strlen(body), what);
    if(!pg_ok) return;
    check(!strcmp(pg.text, want), what);
    if(strcmp(pg.text, want)) printf("  got:  \"%s\"\n  want: \"%s\"\n", pg.text, want);
}

static int link_is(size_t n, const char *url, int usable, const char *label) {
    const struct ut_link *l;
    if(!pg_ok || n < 1 || n > pg.link_count) return 0;
    l = &pg.links[n - 1];
    if(!strcmp(l->url, url) && l->usable == usable && (!label || !strcmp(l->label, label))) return 1;
    printf("  link %u: %s%s \"%s\"\n", (unsigned)n, l->url, l->usable ? "" : " (unusable)", l->label);
    return 0;
}

static void enc(char *out, unsigned int cp) {
    unsigned char b[4];
    size_t n = ut_utf8_encode(cp, b);
    memcpy(out, b, n);
    out[n] = 0;
}

static void entities(void) {
    static const char *const latin1[96] = {
        "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy", "ordf", "laquo",
        "not", "shy", "reg", "macr", "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot",
        "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest", "Agrave", "Aacute", "Acirc",
        "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute",
        "Icirc", "Iuml", "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times", "Oslash",
        "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc", "atilde",
        "auml", "aring", "aelig", "ccedil", "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc",
        "iuml", "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave",
        "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml"
    };
    static const struct { const char *name; unsigned int cp; int legacy; } more[] = {
        { "amp", 38, 1 }, { "lt", 60, 1 }, { "gt", 62, 1 }, { "quot", 34, 1 }, { "AMP", 38, 1 }, { "LT", 60, 1 },
        { "GT", 62, 1 }, { "QUOT", 34, 1 }, { "COPY", 169, 1 }, { "REG", 174, 1 }, { "apos", 39, 0 },
        { "trade", 0x2122, 0 }, { "hellip", 0x2026, 0 }, { "ndash", 0x2013, 0 }, { "mdash", 0x2014, 0 },
        { "lsquo", 0x2018, 0 }, { "rsquo", 0x2019, 0 }, { "ldquo", 0x201c, 0 }, { "rdquo", 0x201d, 0 },
        { "euro", 0x20ac, 0 }, { "bull", 0x2022, 0 }, { "larr", 0x2190, 0 }, { "rarr", 0x2192, 0 },
        { "uarr", 0x2191, 0 }, { "darr", 0x2193, 0 }, { "harr", 0x2194, 0 }, { "minus", 0x2212, 0 },
        { "le", 0x2264, 0 }, { "ge", 0x2265, 0 }, { "ne", 0x2260, 0 }, { "prime", 0x2032, 0 },
        { "Prime", 0x2033, 0 }, { "lsaquo", 0x2039, 0 }, { "rsaquo", 0x203a, 0 }, { "sbquo", 0x201a, 0 },
        { "bdquo", 0x201e, 0 }, { "dagger", 0x2020, 0 }, { "Dagger", 0x2021, 0 }, { "permil", 0x2030, 0 },
        { "thinsp", 0x2009, 0 }, { "ensp", 0x2002, 0 }, { "emsp", 0x2003, 0 }, { "zwj", 0x200d, 0 },
        { "zwnj", 0x200c, 0 }
    };
    char body[80], want[80], c[8];
    size_t i;
    int ok = 1;
    for(i = 0; i < 96; i++) {
        unsigned int cp = 0xa0 + (unsigned int)i;
        enc(c, cp);
        if(cp == 0xa0) strcpy(c, " ");          /* U+00A0 is a space */
        if(cp == 0xad) c[0] = 0;                /* &shy; is dropped */
        snprintf(body, sizeof(body), "x&%s;y x&%s-y", latin1[i], latin1[i]);
        snprintf(want, sizeof(want), "x%sy x%s-y", c, c);
        page("text/html", "utf-8", body, strlen(body), "a Latin-1 entity");
        if(!pg_ok || strcmp(pg.text, want)) {
            printf("  &%s;: \"%s\"\n", latin1[i], pg_ok ? pg.text : "");
            ok = 0;
        }
    }
    check(ok, "every Latin-1 entity (&nbsp; to &yuml;), with and without its semicolon");
    ok = 1;
    for(i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
        enc(c, more[i].cp);
        snprintf(body, sizeof(body), "x&%s;y x&%s-y", more[i].name, more[i].name);
        if(more[i].legacy) snprintf(want, sizeof(want), "x%sy x%s-y", c, c);
        else snprintf(want, sizeof(want), "x%sy x&%s-y", c, more[i].name);
        page("text/html", "utf-8", body, strlen(body), "a named entity");
        if(!pg_ok || strcmp(pg.text, want)) {
            printf("  &%s;: \"%s\"\n", more[i].name, pg_ok ? pg.text : "");
            ok = 0;
        }
    }
    check(ok, "the other named entities; only amp, lt, gt, quot (and AMP, LT, GT, QUOT, COPY, REG) without a semicolon");
    text_is("&notin; &notit; &ampx &copy2026 AT&T & &; &#; &#x; &#xg; &unknown; &Amp; &lrm;&rlm;.",
            "&notin; &notit; &x \xc2\xa9" "2026 AT&T & &; &#; &#x; &#xg; &unknown; &Amp; .",
            "unknown names stay as written; a legacy name needs no semicolon in text");
    text_is("&#65;&#x42;&#X43;&#0068&#x45 &#0;|&#xD800;|&#x110000;|&#99999999999999999999;|&#x10FFFF;|"
            "&#128;&#x9b;&#x85;|&#x81;&#27;&#x202e;&#127;&#1;|&#9;|&#160;|&#xad;|",
            "ABCDE \xef\xbf\xbd|\xef\xbf\xbd|\xef\xbf\xbd|\xef\xbf\xbd|\xf4\x8f\xbf\xbf|"
            "\xe2\x82\xac\xe2\x80\xba\xe2\x80\xa6|| | ||",
            "numeric references: 0, surrogates and big values are U+FFFD, 128-159 are windows-1252, controls go");
    page("text/html", "utf-8", LIT("<a href=\"/p?a=1&copy=2&amp;b=&lt;&notit=3&copy;x=4&copy\">x</a>"
                                   "<img alt=\"&copy2 &copy 2 &copy;2 &lt3 a&shy;b\">"), "entities in attributes");
    check(pg_ok && !strcmp(pg.text, "x[1][&copy2 \xc2\xa9 2 \xc2\xa9" "2 &lt3 ab]") &&
          link_is(1, "https://example.test/p?a=1&copy=2&b=<&notit=3\xc2\xa9x=4\xc2\xa9", 1, "x"),
          "in an attribute a legacy name before = or a letter or digit stays as written");
    if(pg_ok && strcmp(pg.text, "x[1][&copy2 \xc2\xa9 2 \xc2\xa9" "2 &lt3 ab]")) printf("  got \"%s\"\n", pg.text);
}

static void charsets(void) {
    static const char *const w1252[] = { "windows-1252", "iso-8859-1", "us-ascii", "latin1", "cp1252", "l1", "ascii",
                                         "iso8859-1", "x-cp1252", "ansi_x3.4-1968", "ISO-8859-1", " Latin1 " };
    size_t i;
    int ok = 1;
    page("text/html", "windows-1252", LIT("<p>\x93Quoted\x94 \x80 5 \x85 caf\xe9 \x81\x8d\x8f\x90\x9d end</p>"), "windows-1252");
    check(pg_ok && !strcmp(pg.text, "\xe2\x80\x9cQuoted\xe2\x80\x9d \xe2\x82\xac 5 \xe2\x80\xa6 caf\xc3\xa9 end"),
          "windows-1252: smart quotes, the euro, Latin-1; its five holes are C1 controls and go");
    for(i = 0; i < sizeof(w1252) / sizeof(w1252[0]); i++) {
        page("text/plain", w1252[i], LIT("\x93x\x94\xe9"), "a windows-1252 label");
        if(!pg_ok || strcmp(pg.text, "\xe2\x80\x9cx\xe2\x80\x9d\xc3\xa9")) {
            printf("  %s: \"%s\"\n", w1252[i], pg_ok ? pg.text : "");
            ok = 0;
        }
    }
    check(ok, "iso-8859-1, us-ascii, latin1 and the other labels of windows-1252 read as windows-1252");
    page("text/html", "utf-8", LIT("a\xff" "b\xc3"), "UTF-8 with bad bytes");
    check(pg_ok && !strcmp(pg.text, "a\xef\xbf\xbd" "b\xef\xbf\xbd"), "ill-formed UTF-8 becomes U+FFFD");
    page("text/html", "utf-8", LIT("caf\xc3<b>\xa9</b>!"), "a sequence split by a tag");
    check(pg_ok && !strcmp(pg.text, "caf\xef\xbf\xbd\xef\xbf\xbd!"), "a UTF-8 sequence split by a tag is two U+FFFD");
    page("text/html", "", LIT("caf\xc3\xa9"), "no charset, UTF-8");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9"), "no charset: valid UTF-8 is UTF-8");
    page("text/html", "", LIT("caf\xe9"), "no charset, not UTF-8");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9"), "no charset: anything else is windows-1252");
    page("text/html", "", LIT("caf\xc3\xa9 na\xc3"), "UTF-8 cut short");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9 na\xef\xbf\xbd"), "a body that ends inside a character is still UTF-8");
    page("text/html", "", LIT("<meta charset=\"windows-1252\"><p>caf\xc3\xa9"), "meta charset");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\x83\xc2\xa9"), "<meta charset> in the first 1024 bytes");
    page("text/html", "", LIT("<!-- <meta charset=utf-8> --><META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset='windows-1252'\">caf\xc3\xa9"),
         "meta http-equiv");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\x83\xc2\xa9"), "<meta http-equiv=Content-Type>, a meta in a comment does not count");
    {
        char body[1300];
        memset(body, ' ', 1100);
        strcpy(body + 1100, "<meta charset=\"windows-1252\">caf\xc3\xa9");
        page("text/html", "", body, strlen(body), "a late meta");
        check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9"), "a <meta charset> after 1024 bytes does not count");
    }
    page("text/html", "utf-8", LIT("<meta charset=\"windows-1252\">caf\xc3\xa9"), "header and meta");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9"), "the header's charset wins over <meta>");
    page("text/html", "shift_jis", LIT("abc\x82\xa0" "def"), "an unknown charset");
    check(pg_ok && !strcmp(pg.text, "abc??def") && strstr(pg.notice, "shift_jis") && strstr(pg.notice, "ASCII"),
          "an unknown charset: its ASCII, ? for the rest, and a notice naming it");
    page("text/html", "", LIT("<meta charset=\"Shift_JIS\">abc\x82\xa0"), "an unknown meta charset");
    check(pg_ok && !strcmp(pg.text, "abc??") && strstr(pg.notice, "Shift_JIS"), "an unknown <meta charset> is named too");
    page("text/html", "windows-1252", LIT("\xef\xbb\xbf" "caf\xc3\xa9"), "a byte order mark");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9"), "a UTF-8 byte order mark wins and is not shown");
    page("text/html", "", LIT("<meta charset=\"utf-16\">caf\xc3\xa9 \xe9"), "meta utf-16");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9 \xef\xbf\xbd"), "<meta charset=utf-16> means UTF-8, as in browsers");
    page("text/html", "", LIT("<meta charset=\"\x1b[2J\xc2\x9b" "evil\">x"), "a hostile charset name");
    check(pg_ok && strstr(pg.notice, "[2Jevil") && !strchr(pg.notice, 0x1b), "a charset name in the notice is safe text");
    page("text/html", "iso-8859-1", LIT("<title>Caf\xe9 Fran\xe7" "ais</title><p>Cr\xe8me br\xfbl\xe9" "e, \xe0 la carte, "
                                         "na\xefve, \xbfQu\xe9 tal?, \xa1Ol\xe9!, 25\xb0" "C, \xa9 2004.</p>"), "Latin-1");
    check(pg_ok && !strcmp(pg.title, "Caf\xc3\xa9 Fran\xc3\xa7" "ais") &&
          !strcmp(pg.text, "Cr\xc3\xa8me br\xc3\xbbl\xc3\xa9" "e, \xc3\xa0 la carte, na\xc3\xafve, \xc2\xbfQu\xc3\xa9 tal?, "
                           "\xc2\xa1Ol\xc3\xa9!, 25\xc2\xb0" "C, \xc2\xa9 2004."), "a Latin-1 page (accents, signs)");
    page("text/plain", "windows-1252", LIT("caf\xe9\x93"), "plain windows-1252");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9\xe2\x80\x9c"), "plain text in windows-1252");
}

static void plain(void) {
    page("text/plain", "", LIT("\n\n\na\tb\r\nc  \r\n\r\n\r\n\td\n<b>not markup</b> &amp;\r\rx\n\n"), "plain text");
    check(pg_ok && !strcmp(pg.text, "a       b\nc\n\n        d\n<b>not markup</b> &amp;\n\nx"),
          "plain text: tabs to 8-column stops, CR LF and CR, no trailing spaces, one blank line at most, none at the ends");
    if(pg_ok && strcmp(pg.text, "a       b\nc\n\n        d\n<b>not markup</b> &amp;\n\nx")) printf("  got \"%s\"\n", pg.text);
    page("text/css", "", LIT("a { }"), "other text");
    check(pg_ok && !strcmp(pg.text, "a { }"), "other text types are shown as they are");
    page("", "", LIT("  \n<!DOCTYPE html><title>T</title><p>Hi <b>there</b>"), "sniffed HTML");
    check(pg_ok && !strcmp(pg.text, "Hi there") && !strcmp(pg.title, "T"), "no Content-Type: a page that starts like HTML is HTML");
    page("", "", LIT("Some <b>text</b>"), "sniffed text");
    check(pg_ok && !strcmp(pg.text, "Some <b>text</b>"), "no Content-Type: anything else is plain text");
}

static void safety(void) {
    static const char hostile[] =
        "<title>T\x1b[2Ji\xc2\x9bt\xe2\x80\xaele\x07</title>"
        "<p>a\x1b[31mb\x9b" "c\xc2\x85" "d\xe2\x80\x8f" "e\xef\xbb\xbf" "f\0g&#27;h&#x9b;i&#x202E;j&#x85;k&#127;l&#1;m\xd8\x9cn</p>"
        "<a href=\"/x\x1b[2J\">L\x1b[1mabel</a>"
        "<a href=\"javascript:\x1b]0;evil\x07\">J</a>"
        "<img alt=\"A\x1b" "B\xe2\x81\xa6\">";
    page("text/html", "utf-8", hostile, sizeof(hostile) - 1, "a hostile page");
    check(pg_ok && !strcmp(pg.text, "a[31mb\xef\xbf\xbd" "c defgh\xe2\x80\xbaij\xe2\x80\xa6klmn\n\nL[1mabel[1]J[2][AB]"),
          "ESC, C1, bidirectional marks, BOM, NUL and entities for them never reach the text");
    if(pg_ok && strcmp(pg.text, "a[31mb\xef\xbf\xbd" "c defgh\xe2\x80\xbaij\xe2\x80\xa6klmn\n\nL[1mabel[1]J[2][AB]"))
        printf("  got \"%s\"\n", pg.text);
    check(pg_ok && !strcmp(pg.title, "T[2Jitle"), "nor the title");
    check(link_is(1, "/x[2J", 0, "L[1mabel") && link_is(2, "javascript:]0;evil", 0, "J"),
          "nor labels and link addresses (a control character inside an address makes it unusable)");
    page("text/html", "utf-8", LIT("<a href=\"/a\xe2\x80\xae" "b\xc2\x9b\">x</a>"), "bidirectional override in an address");
    check(link_is(1, "/ab", 0, "x"), "an address with an unsafe character is unusable, its text without it");
    {
        struct ut_response *r = response("https://example.test/p\xe2\x80\xae" "q", "text/html", "utf-8", LIT("<a href=#f>F</a>"));
        ut_document_free(&pg);
        pg_ok = render(r, &pg, "a page address with an override") == 0;
        response_free(r);
        check(link_is(1, "#f", 0, "F"), "a fragment of a page whose own address is unsafe is unusable");
    }
    page("text/html", "windows-1252", LIT("<title>\x9b\x1b</title>x\x81y\x9b"), "C1 in windows-1252");
    check(pg_ok && !strcmp(pg.text, "xy\xe2\x80\xba") && !strcmp(pg.title, "\xe2\x80\xba"), "windows-1252's holes are not C1 controls in the output");
}

static void links(void) {
    struct sb b = { 0, 0, 0 };
    unsigned i;
    for(i = 1; i <= 2048; i++) sb_printf(&b, "<a href=\"/p%u\">L%u</a> ", i, i);
    page("text/html", "utf-8", b.s, b.len, "2048 links");
    check(pg_ok && pg.link_count == 2048 && !(pg.truncated & UT_TRUNC_LINKS) && !pg.notice[0] &&
          link_is(2048, "https://example.test/p2048", 1, "L2048") &&
          strlen(pg.text) > 11 && !strcmp(pg.text + strlen(pg.text) - 11, "L2048[2048]"), "2048 links are all numbered");
    sb_str(&b, "<a href=\"/p2049\">L2049</a>");
    page("text/html", "utf-8", b.s, b.len, "2049 links");
    check(pg_ok && pg.link_count == 2048 && (pg.truncated & UT_TRUNC_LINKS) &&
          !strcmp(pg.notice, "only its first 2048 links have numbers") &&
          strlen(pg.text) > 17 && !strcmp(pg.text + strlen(pg.text) - 17, "L2048[2048] L2049"),
          "the 2049th link keeps its text but gets no number, and the notice says how many have one");
    free(b.s);

    page("text/html", "utf-8", LIT("<a href=\"#sec\">S</a> <a href=\"page\">P</a> <a href=\"//third.test/x\">T</a> "
                                   "<a href=\"?q=1\">Q</a><base href=\"javascript:x\"><base href=\"https://other.test/dir/\">"
                                   "<base href=\"https://fourth.test/\">"), "a late base");
    check(link_is(1, "https://example.test/dir/page.html#sec", 1, "S") && link_is(2, "https://other.test/dir/page", 1, "P") &&
          link_is(3, "https://third.test/x", 1, "T") && link_is(4, "https://other.test/dir/?q=1", 1, "Q"),
          "links resolve against the first <base> that resolves, wherever it is; a fragment against the page");
    page("text/html", "utf-8", LIT("<a href=\"mailto:me@example.test\">M</a><a href=\" javascript:alert(1) \">J</a>"
                                   "<a href=\"ftp://f.test/\">F</a><a href=\"data:text/html,<b>x</b>\">D</a><a href=\"tel:+1 555\">T</a>"),
         "other schemes");
    check(link_is(1, "mailto:me@example.test", 0, "M") && link_is(2, "javascript:alert(1)", 0, "J") &&
          link_is(3, "ftp://f.test/", 0, "F") && link_is(4, "data:text/html,<b>x</b>", 0, "D") && link_is(5, "tel:+1 555", 0, "T") &&
          !strcmp(pg.text, "M[1]J[2]F[3]D[4]T[5]"), "other schemes keep their number, unusable, with the reference as written");
    page("text/html", "utf-8", LIT("<a href=\"  /a b\tc\nd  \">1</a><a href=\"/caf\xc3\xa9?q=\xc3\xbc#\xc3\x9f\">2</a>"
                                   "<a href=\"\\x\\y?a\\b\">3</a><a href=\"/100%\">4</a><a href=\"/a%2Fb%zz\">5</a>"
                                   "<a href='/q?a=\"b\"<c>`'>6</a>"), "addresses as browsers clean them");
    check(link_is(1, "https://example.test/a%20bcd", 1, "1") && link_is(2, "https://example.test/caf\xc3\xa9?q=\xc3\xbc#\xc3\x9f", 1, "2") &&
          link_is(3, "\\x\\y?a\\b", 0, "3") && link_is(4, "/100%", 0, "4") && link_is(5, "/a%2Fb%zz", 0, "5") &&
          link_is(6, "https://example.test/q?a=\"b\"<c>`", 1, "6"),
          "hrefs as browsers clean them: whitespace trimmed, tabs and newlines dropped, spaces as %20; the rest is url.c's");
    text_is("<a href=\"\">empty</a> <a href=\"  \">blank</a> <a name=\"n\">named</a> <a>bare</a>", "empty blank named bare",
            "links without an address get no number");
    check(pg_ok && pg.link_count == 0, "and are not links");
    text_is("<a href=/x></a> <a href=/y><img src=a.png></a> <a href=/z><img alt=Logo></a>", "[1] [2] Logo[3]",
            "a link with no text is its [N] alone; an image's alt is a label, without brackets");
    check(link_is(1, "https://example.test/x", 1, "") && link_is(2, "https://example.test/y", 1, "") &&
          link_is(3, "https://example.test/z", 1, "Logo"), "their labels");
    text_is("<a href=/x><h2>Title</h2><p>Summary</p></a>after", "Title\n\nSummary[1]\n\nafter", "a link around blocks");
    check(link_is(1, "https://example.test/x", 1, "Title Summary"), "its label is its text on one line");
    text_is("<a href=/1>one <a href=/2>two</a></a> <a href=/3>Read more </a>next see <a href=/4> here</a>.",
            "one[1] two[2] Read more[3] next see here[4].", "a link starts where the last one ends; [N] right after the label");
    check(link_is(1, "https://example.test/1", 1, "one") && link_is(3, "https://example.test/3", 1, "Read more") &&
          link_is(4, "https://example.test/4", 1, "here"), "labels without the spaces around them");
    text_is("<a href=/x>Read <b>the <i>full</i> story</b></a>", "Read the full story[1]", "a label of nested elements");
    text_is("<table><tr><td><a href=/x>a</td><td>b</td></tr></table>", "a[1] | b", "a table cell ends its link");
    text_is("<button>Go </button>, <select><option>Small </option></select>; <a href=/y>link </a>.",
            "[button: Go], [select: Small]; link[1] .", "the space at the end of a button or an option stays inside it");
    b.s = NULL; b.len = b.cap = 0;
    sb_str(&b, "<a href=/x>");
    for(i = 0; i < 2000; i++) sb_str(&b, "\xc3\xa9");
    sb_str(&b, "</a>");
    page("text/html", "utf-8", b.s, b.len, "a long label");
    check(pg_ok && pg.link_count == 1 && strlen(pg.links[0].label) == UT_FIELD_MAX && pg.text_len == 4003,
          "a label is cut at UT_FIELD_MAX bytes, between characters; the text keeps everything");
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    sb_str(&b, "<a href=\"/");
    for(i = 0; i < 3000; i++) sb_str(&b, "a");
    sb_str(&b, "\">long</a>");
    page("text/html", "utf-8", b.s, b.len, "a long address");
    check(pg_ok && pg.link_count == 1 && !pg.links[0].usable && strlen(pg.links[0].url) <= UT_URL_MAX &&
          !strncmp(pg.links[0].url, "/aaaa", 5), "an address over 2048 bytes is unusable and cut");
    free(b.s);
}

static void anchors(void) {
    size_t off = 99;
    struct sb b = { 0, 0, 0 };
    unsigned i;
    page("text/html", "utf-8", LIT("<h1 id=top>Title</h1><p id=p1>Para <span id=s1>inner</span></p><a name=n1></a><p>After</p>"
                                   "<p id=p1>Dup</p><p id=\"caf\xc3\xa9\">X</p><p id=\"\">E</p><div id=end></div>"), "anchors");
    check(pg_ok && !strcmp(pg.text, "Title\n\nPara inner\n\nAfter\n\nDup\n\nX\n\nE") && pg.anchor_count == 6, "anchors: the page");
    check(!ut_anchor_offset(&pg, "top", &off) && off == 0, "an id on a heading");
    check(!ut_anchor_offset(&pg, "p1", &off) && off == 7, "the first of two equal names counts");
    check(!ut_anchor_offset(&pg, "s1", &off) && off == 12, "an id inside a line: where its text starts");
    check(!ut_anchor_offset(&pg, "n1", &off) && off == 19, "an empty <a name>: where the next text starts");
    check(!ut_anchor_offset(&pg, "caf\xc3\xa9", &off) && off == 31 && !ut_anchor_offset(&pg, "caf%C3%A9", &off) && off == 31,
          "a fragment as written or percent-encoded");
    check(!ut_anchor_offset(&pg, "end", &off) && off == pg.text_len, "an anchor after the last text: the end");
    off = 99;
    check(ut_anchor_offset(&pg, "missing", &off) == -1 && off == 99 && ut_anchor_offset(&pg, "P1", &off) == -1,
          "a name that is not there, names compared exactly");
    check(!ut_anchor_offset(&pg, "", &off) && off == 0 && !ut_anchor_offset(&pg, "Top", &off) && off == 0,
          "\"\" and \"top\" are the start");
    check(ut_anchor_offset(&pg, NULL, &off) == -1 && ut_anchor_offset(NULL, "top", &off) == -1, "nothing to look in or for");
    for(i = 0; i < 2049; i++) sb_printf(&b, "<p id=a%u>x</p>", i);
    page("text/html", "utf-8", b.s, b.len, "2049 anchors");
    check(pg_ok && pg.anchor_count == 2048 && (pg.truncated & UT_TRUNC_ANCHORS) && ut_anchor_offset(&pg, "a2048", &off) == -1 &&
          !ut_anchor_offset(&pg, "a2047", &off) && !ut_anchor_offset(&pg, "a0", &off) && !off,
          "2048 anchors are known, the 2049th is not");
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    sb_str(&b, "<p id=\"");
    for(i = 0; i < UT_FIELD_MAX + 1; i++) sb_str(&b, "a");
    sb_str(&b, "\">long</p><p id=\"e\x1b[2J\">esc</p><p id=ok>ok</p>");
    page("text/html", "utf-8", b.s, b.len, "odd anchor names");
    check(pg_ok && pg.anchor_count == 1 && !strcmp(pg.anchors[0].name, "ok"),
          "names over UT_FIELD_MAX bytes or with control characters are not kept");
    free(b.s);
}

/* What UT_LINK_BYTES_MAX counts: every link's label and address and every anchor's name, allocation by allocation */
static size_t link_strings(const struct ut_document *d) {
    size_t i, n = 0;
    for(i = 0; i < d->link_count; i++) n += strlen(d->links[i].label) + 1 + strlen(d->links[i].url) + 1;
    for(i = 0; i < d->anchor_count; i++) n += strlen(d->anchors[i].name) + 1;
    return n;
}

static void link_bytes(void) {
    struct sb b = { 0, 0, 0 };
    char long_part[1501], want[80], *after;
    size_t n, off;
    unsigned i;
    memset(long_part, 'a', 1500);
    long_part[1500] = 0;
    /* links whose addresses fill UT_LINK_BYTES_MAX long before the count limit */
    for(i = 0; i < 1000; i++) {
        sb_printf(&b, "<a href=\"/%u/", i);
        sb_str(&b, long_part);
        sb_printf(&b, "\">L%u</a> ", i);
    }
    sb_str(&b, "<a href=/s>short</a> <p id=late>x</p>");
    page("text/html", "utf-8", b.s, b.len, "links over UT_LINK_BYTES_MAX");
    n = pg_ok ? link_strings(&pg) : 0;
    check(pg_ok && pg.link_count > 600 && pg.link_count < 1000 && (pg.truncated & UT_TRUNC_LINKS) &&
          n <= UT_LINK_BYTES_MAX && n > UT_LINK_BYTES_MAX - 1600,
          "links stop getting numbers where their labels and addresses would pass UT_LINK_BYTES_MAX");
    snprintf(want, sizeof(want), "only its first %u links have numbers", pg_ok ? (unsigned)pg.link_count : 0);
    check(pg_ok && !strcmp(pg.notice, want), "the notice gives the number that applied");
    snprintf(want, sizeof(want), "L%u[%u] L%u L%u", pg_ok ? (unsigned)pg.link_count - 1 : 0, pg_ok ? (unsigned)pg.link_count : 0,
             pg_ok ? (unsigned)pg.link_count : 0, pg_ok ? (unsigned)pg.link_count + 1 : 0);
    after = pg_ok ? strstr(pg.text, want) : NULL;
    check(after && pg_ok && strstr(pg.text, "L999 short") && ut_anchor_offset(&pg, "late", &off) == -1 &&
          (pg.truncated & UT_TRUNC_ANCHORS), "the links after keep their text, and anchors after are not kept either");
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    /* anchors whose names fill it, before the count limit; links after them get no number */
    for(i = 0; i < 2000; i++) {
        sb_printf(&b, "<p id=\"n%u-", i);
        sb_add(&b, long_part, 690);
        sb_str(&b, "\">x</p>");
    }
    sb_str(&b, "<a href=/after>after</a>");
    page("text/html", "utf-8", b.s, b.len, "anchors over UT_LINK_BYTES_MAX");
    n = pg_ok ? link_strings(&pg) : 0;
    check(pg_ok && pg.anchor_count > 1400 && pg.anchor_count < 2000 && (pg.truncated & UT_TRUNC_ANCHORS) &&
          n <= UT_LINK_BYTES_MAX && n > UT_LINK_BYTES_MAX - 800 && !pg.link_count && (pg.truncated & UT_TRUNC_LINKS) &&
          strlen(pg.text) > 5 && !strcmp(pg.text + strlen(pg.text) - 5, "after"),
          "anchors stop being kept at UT_LINK_BYTES_MAX; a link after them keeps its text without a number");
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    /* a late <base> that would make the addresses of the links before it pass the cap is not used */
    for(i = 0; i < 1300; i++) sb_printf(&b, "<a href=\"x%u\">%u</a> ", i, i);
    sb_str(&b, "<base href=\"https://example.test/");
    sb_str(&b, long_part);
    sb_str(&b, "/\"><a href=y>y</a>");
    page("text/html", "utf-8", b.s, b.len, "a late base too big");
    n = pg_ok ? link_strings(&pg) : 0;
    check(pg_ok && pg.link_count == 1301 && n <= UT_LINK_BYTES_MAX && link_is(1, "https://example.test/dir/x0", 1, "0") &&
          link_is(1301, "https://example.test/dir/y", 1, "y"),
          "a late <base> whose addresses would pass UT_LINK_BYTES_MAX is not used, for any link");
    free(b.s);
}

static void limits(void) {
    struct sb b = { 0, 0, 0 };
    unsigned i;
    sb_str(&b, "<title>");
    for(i = 0; i < 3000; i++) sb_str(&b, "a");
    sb_str(&b, "</title><a href=/open>");
    while(b.len < 700 * 1024) sb_str(&b, "<p>Surf's up: words words words words words words words words words.</p>\n");
    sb_str(&b, "<a href=/late>late</a>");
    page("text/html", "utf-8", b.s, b.len, "the text limit");
    check(pg_ok && (pg.truncated & UT_TRUNC_TEXT) && pg.text_len <= UT_TEXT_MAX && pg.text_len > UT_TEXT_MAX - 200 &&
          !strcmp(pg.text + pg.text_len - strlen(NOTICE), NOTICE),
          "the text stops at UT_TEXT_MAX with a notice line");
    check(pg_ok && pg.link_count == 0, "a link cut by the limit, and links after it, are not kept");
    check(pg_ok && strlen(pg.title) == UT_FIELD_MAX, "a title is cut at UT_FIELD_MAX bytes");
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    for(i = 0; i < 300; i++) sb_printf(&b, "<a href=/l%u>l</a>", i);
    for(i = 0; i < 5000; i++) sb_printf(&b, "<h2>h%u</h2>", i);
    page("text/html", "utf-8", b.s, b.len, "5000 headings");
    check(pg_ok && pg.span_count <= UT_SPANS_MAX && pg.link_count == 300, "heading spans stop at UT_SPANS_MAX; every link keeps its span");
    free(b.s);
}

static void structure(void) {
    size_t off = 0;
    text_is("<xmp><b>&amp;</b>\n  kept</xmp>after", "<b>&amp;</b>\n  kept\n\nafter", "xmp: raw text, shown as it is");
    text_is("a<svg/>b<math/>c<svg><path/></svg>d", "abcd", "a self-closing svg or math hides nothing");
    text_is("<blockquote><ul><li>one<li>two</ul></blockquote><ol><li value=5>five<li>six<li value=x>seven</ol>",
            "  * one\n  * two\n\n5. five\n6. six\n7. seven", "a list in a quotation; li value");
    text_is("<ul><li>a<ul><li>b<blockquote>q</blockquote></ul></ul>", "* a\n  * b\n\n      q",
            "a quotation in a nested item is indented under its text");
    text_is("<p>x</p><hr><p>y</p><blockquote><hr></blockquote>",
            "x\n\n----------------------------------------\n\ny\n\n  ----------------------------------------",
            "hr: 40 dashes on a line, indented like the text around it");
    text_is("<table><tr><td><br><p>a</p></td><td id=c><br>b</td></tr></table>", "a | b",
            "line breaks at the start of a cell do not break its row");
    check(pg_ok && !ut_anchor_offset(&pg, "c", &off) && off == 4, "an anchor on a cell: where its text starts, after the \" | \"");
    text_is("<table><tr><td>a</td><td>&nbsp;</td><td>&nbsp;&nbsp;b&nbsp;&nbsp;c</td></tr><tr><td width=8>&nbsp;</td><td>d</td></tr></table>",
            "a | b  c\nd", "a cell of no-break spaces is empty (a spacer), and they collapse where a cell starts");
    text_is("<noframes><p>No frames</p></noframes><frameset><frame src=a.html></frameset>", "No frames",
            "noframes is shown; frames are not");
    text_is("<dl><dt>T<dd>D<dd>E<dt>U</dl>", "T\n  D\n  E\nU", "dt and dd close each other");
    text_is("<p>one<p>two<li>orphan item</li>three", "one\n\ntwo\n\n* orphan item\nthree", "an item outside a list");
    text_is("<h1>A<a href=/x>B</a>C</h1>", "AB[1]C", "a heading's spans leave the [N] out");
    check(pg_ok && pg.span_count == 3 && pg.spans[0].kind == UT_SPAN_HEADING && pg.spans[0].len == 2 &&
          pg.spans[1].kind == UT_SPAN_LINK && pg.spans[2].kind == UT_SPAN_HEADING && pg.spans[2].offset == 5,
          "spans in order, apart, the link's in the middle");
}

/* body (UTF-8, with "\x01" standing for a lone high surrogate) as UTF-16 with a byte order mark */
static void utf16(struct sb *b, const char *body, int big_endian, int bom) {
    const unsigned char *s = (const unsigned char *)body;
    size_t i = 0, n = strlen(body);
    unsigned int cp, units[2], k, u;
    unsigned char out[2];
    if(bom) cp = 0xfeff;
    while(bom || i < n) {
        if(!bom) {
            if(s[i] == 1) {
                cp = 0xd800;
                i++;
            } else i += ut_utf8_decode(s + i, n - i, &cp);
        }
        bom = 0;
        if(cp >= 0x10000) {
            units[0] = 0xd800 + ((cp - 0x10000) >> 10);
            units[1] = 0xdc00 + ((cp - 0x10000) & 0x3ff);
            k = 2;
        } else {
            units[0] = cp;
            k = 1;
        }
        for(u = 0; u < k; u++) {
            out[big_endian ? 0 : 1] = (unsigned char)(units[u] >> 8);
            out[big_endian ? 1 : 0] = (unsigned char)(units[u] & 0xff);
            sb_add(b, (const char *)out, 2);
        }
    }
}

static void review_round(void) {
    struct sb b = { 0, 0, 0 };
    char *body;
    size_t i, len;
    /* empty cells keep their place in a data table (one with a <th>), and vanish in a layout table */
    text_is("<table><tr><th>Day</th><th>High</th><th>Low</th></tr><tr><td>Wed</td><td></td><td>14:02</td></tr>"
            "<tr><td></td><td>07:00</td><td>13:00</td></tr><tr><td>Thu</td><td>08:10</td><td>&nbsp;</td></tr></table>",
            "Day | High | Low\nWed | - | 14:02\n- | 07:00 | 13:00\nThu | 08:10 | -",
            "a data table keeps every cell in its column: an empty or spacer cell is - (middle, start, end)");
    text_is("<table><tr><th></th><th>AM</th><th>PM</th></tr><tr><th>Sat</th><td>06:10</td><td><img src=x.png></td></tr></table>",
            "- | AM | PM\nSat | 06:10 | -", "an empty corner <th> is - too");
    text_is("<table><tr><td>a</td><td></td><td>&nbsp;</td><td>b</td></tr></table>", "a | b",
            "a table without <th> (for layout) still drops empty and spacer cells");
    text_is("<table><tr><td>x</td><td></td></tr><tr><th>H</th><td></td></tr></table>", "x\nH | -",
            "a table is a data table from its first <th> on");
    text_is("<table><tr><th>K</th><td><table><tr><td></td><td>in</td></tr></table></td><td></td></tr></table>", "K | in | -",
            "each table decides for itself");
    /* a self-closed raw text element in svg or math is whole, as in browsers */
    text_is("<p>Share this article:</p>\n<a href=\"/share\"><svg class=\"icon\" viewBox=\"0 0 24 24\"><title/><use href=\"#share\"/>"
            "</svg> Share</a>\n<h2>Next section</h2>\n<p>The rest of the article, with <a href=\"/more\">a link</a>.</p>\n",
            "Share this article:\n\nShare[1]\n\nNext section\n\nThe rest of the article, with a link[2].",
            "an svg icon with a self-closed <title/> hides nothing after it");
    text_is("<p>Before</p><svg viewBox='0 0 1 1'><style/><path d='M0 0'/></svg><p>After</p><math><script/></math>end",
            "Before\n\nAfter\n\nend", "<style/> in svg and <script/> in math are whole");
    /* list numbers within a billion, whatever the size of long */
    text_is("<ol start=3000000000><li>a<li>b</ol><ol start=-2147483649><li>c</ol><ol><li value=99999999999>d<li>e</ol>",
            "999999999. a\n1000000000. b\n\n-999999999. c\n\n999999999. d\n1000000000. e",
            "list numbers stop at a billion (a 32-bit long never overflows)");
    /* a cut page with only spaces before the cut */
    body = malloc(700 * 1024);
    memset(body, ' ', 700 * 1024);
    page("text/plain", "", body, 600 * 1024, "600 KiB of spaces");
    check(pg_ok && (pg.truncated & UT_TRUNC_TEXT) && !strcmp(pg.text, &NOTICE[2]),
          "a cut page with nothing but spaces before the cut is the notice alone");
    memcpy(body, "<pre>", 5);
    page("text/html", "utf-8", body, 700 * 1024, "a pre of 700 KiB of spaces");
    check(pg_ok && (pg.truncated & UT_TRUNC_TEXT) && !strcmp(pg.text, &NOTICE[2]), "and so is a <pre> of spaces");
    free(body);
    /* invisible characters */
    text_is("a&#x200B;b&#x2060;c&#x2064;d&#xFE0F;e&#xFE00;f&#x180E;g&#x34F;h&#x200C;i&#x200D;j\xe2\x80\x8bk\xef\xb8\x8f",
            "abcdefgh\xe2\x80\x8ci\xe2\x80\x8dj" "k", "invisible characters are dropped; the joiners ZWNJ and ZWJ stay");
    page("text/html", "utf-8", LIT("<title>T&#x200B;i</title><img alt=\"A&#x2060;B\"><a href=\"/x\">L&#xFE0F;M</a>"), "invisible in fields");
    check(pg_ok && !strcmp(pg.title, "Ti") && !strcmp(pg.text, "[AB]LM[1]") && link_is(1, "https://example.test/x", 1, "LM"),
          "nor in titles, alt text and labels");
    /* scripts that hide their markup in <!-- --> */
    text_is("<p>Top story</p>\n<script type=\"text/javascript\"><!--\n"
            "document.write('<script src=\"https://ads.example.test/ad.js\"></script>');\n//--></script>\n<p>Article text.</p>\n",
            "Top story\n\nArticle text.", "a script that writes a script inside <!-- --> is all script");
    text_is("<script><!-- a </script>b", "b", "in an escaped script its end tag still ends it");
    text_is("<script>a<!--b<script>c</script>d</script>e", "e", "double escaped: the inner end tag does not end the script");
    text_is("<script><!-- x --> </script>d<script><!--></script>f", "df", "a comment that ends inside a script");
    text_is("<style><!-- </style>x", "x", "style has no escaped states");
    page("text/html", "utf-8", LIT("a<script>x<"), "a script ending on <");
    check(pg_ok && !strcmp(pg.text, "a"), "a script that ends with the body on a lone <");
    page("text/html", "utf-8", LIT("b<script><!--x<"), "an escaped script ending on <");
    check(pg_ok && !strcmp(pg.text, "b"), "an escaped script that ends with the body on a lone <");
    text_is("<script><!--<scripts>--></script>g<script><!--<script>-></script>-->h</script>i", "gi",
            "<scripts> is not <script>; -> does not end the escape");
    /* the notice: the links part first, a long charset name cut */
    sb_str(&b, "<meta charset=\"an-extremely-long-and-made-up-charset-name-for-testing\"><form></form>");
    for(i = 0; i < 2049; i++) sb_str(&b, "<a href=/l>l</a>");
    page("text/html", "", b.s, b.len, "a full notice");
    check(pg_ok && !strcmp(pg.notice, "only its first 2048 links have numbers; the page says it is in an-extremely-long-and...; "
                                       "only its ASCII is shown; forms are not supported"),
          "the notice: links first, a charset name cut at 24 bytes, all three parts");
    if(pg_ok && !strstr(pg.notice, "an-extremely-long-and...")) printf("  notice \"%s\"\n", pg.notice);
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    /* UTF-16 */
    utf16(&b, "<title>Caf\xc3\xa9</title><p>Wave: \xf0\x9f\x8c\x8a, lone: \x01.</p><p>&amp; \xe4\xb8\xad</p>", 0, 1);
    page("text/html", "", b.s, b.len, "UTF-16LE");
    check(pg_ok && !strcmp(pg.title, "Caf\xc3\xa9") &&
          !strcmp(pg.text, "Wave: \xf0\x9f\x8c\x8a, lone: \xef\xbf\xbd.\n\n& \xe4\xb8\xad"),
          "UTF-16LE with its byte order mark: decoded, surrogate pairs joined, a lone surrogate U+FFFD");
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    utf16(&b, "<title>Caf\xc3\xa9</title><p>Wave: \xf0\x9f\x8c\x8a, lone: \x01.</p><p>&amp; \xe4\xb8\xad</p>", 1, 1);
    sb_add(&b, "\x00", 1);
    page("", "windows-1252", b.s, b.len, "UTF-16BE");
    check(pg_ok && !strcmp(pg.title, "Caf\xc3\xa9") &&
          !strcmp(pg.text, "Wave: \xf0\x9f\x8c\x8a, lone: \xef\xbf\xbd.\n\n& \xe4\xb8\xad\n\n\xef\xbf\xbd"),
          "UTF-16BE: its byte order mark wins over a charset and over no Content-Type; an odd last byte is U+FFFD");
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    utf16(&b, "<p>le \xc3\xa9</p>", 0, 0);
    page("text/html", "utf-16le", b.s, b.len, "utf-16le label");
    check(pg_ok && !strcmp(pg.text, "le \xc3\xa9"), "charset=utf-16le without a byte order mark");
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    utf16(&b, "<p>be \xc3\xa9</p>", 1, 0);
    page("text/html", "UTF-16BE", b.s, b.len, "utf-16be label");
    check(pg_ok && !strcmp(pg.text, "be \xc3\xa9"), "charset=UTF-16BE without a byte order mark");
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    /* paths no test reached before */
    page("text/html", "", LIT("<meta http-equiv=content-type content=\"text/html; charset=windows-1252\">caf\xc3\xa9"), "unquoted");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\x83\xc2\xa9"), "an unquoted charset in http-equiv's content");
    text_is("abc</", "abc</", "</ at the very end of the body is text");
    for(i = 0; i < 125; i++) sb_str(&b, "<div>");          /* the stack holds 128: the root and 127 more */
    sb_str(&b, "<table><tr><td>a</td><td>b</td></tr></table>x");
    page("text/html", "utf-8", b.s, b.len, "a cell that does not fit on the stack");
    check(pg_ok && !strcmp(pg.text, "ab\nx"), "a row on the stack's last place: cells past it are only their text");
    if(pg_ok && strcmp(pg.text, "ab\nx")) printf("  got \"%s\"\n", pg.text);
    b.len = 0;
    for(i = 0; i < 126; i++) sb_str(&b, "<div>");
    sb_str(&b, "<table><tr><td>a</td><td>b</td></tr></table>x");
    page("text/html", "utf-8", b.s, b.len, "a row that does not fit on the stack");
    check(pg_ok && !strcmp(pg.text, "a\nb\nx"), "a table on the stack's last place: each cell's row is wanted, none fits");
    if(pg_ok && strcmp(pg.text, "a\nb\nx")) printf("  got \"%s\"\n", pg.text);
    free(b.s);
    /* a link that shows no text: its aria-label, its title, or the first one inside it */
    text_is("<table><tr><td><a id=up_1 href=\"vote?id=1&amp;how=up\"><div class=\"votearrow\" title=\"upvote\"></div></a></td>"
            "<td><a href=\"item?id=1\">A story</a></td></tr></table>", "upvote[1] | A story[2]",
            "an empty link takes the title of an element inside it (a vote arrow)");
    check(link_is(1, "https://example.test/dir/vote?id=1&how=up", 1, "upvote"), "which is its label too");
    text_is("see <a href=/h aria-label=\" Home   page \" title=\"Go home\"><span title=\"inner\"></span></a>, "
            "<a href=/t title=\"Tide&#10;tables\"><img src=t.png></a>, <a href=/s><svg aria-label=\"Share\" title=\"Not this\"><path/></svg></a>, "
            "<a href=/i><b></b><span title=\"More\" aria-label=\"Info\"></span><i title=\"Later\"></i></a>.",
            "see Home page[1], Tide tables[2], Share[3], Info[4].",
            "the <a>'s aria-label, else its title, else the first inside: aria-label before title on one element");
    check(link_is(1, "https://example.test/h", 1, "Home page") && link_is(2, "https://example.test/t", 1, "Tide tables") &&
          link_is(3, "https://example.test/s", 1, "Share") && link_is(4, "https://example.test/i", 1, "Info"), "their labels");
    text_is("<a href=/e><div></div></a> <a href=/f aria-label=\"  \" title=\"\"><span title=\"&#x200B;\"></span></a> "
            "<a href=/x aria-label=\"Label\">Text</a> <a href=/y aria-label=\"A\x1b[2JB&#x202E;C\"></a>",
            "[1] [2] Text[3] A[2JBC[4]", "nothing at all: [N] alone; text wins over aria-label; the attribute is safe text");
    check(link_is(1, "https://example.test/e", 1, "") && link_is(3, "https://example.test/x", 1, "Text") &&
          link_is(4, "https://example.test/y", 1, "A[2JBC"), "and labels to match");
    body = malloc(3200);                            /* the tag and 1500 two-byte characters */
    strcpy(body, "<a href=/l title=\"");
    for(i = 0; i < 1500; i++) strcat(body, "\xc3\xa9");
    strcat(body, "\"></a>");
    page("text/html", "utf-8", body, strlen(body), "a long title");
    check(pg_ok && pg.link_count == 1 && strlen(pg.links[0].label) <= UT_FIELD_MAX && strlen(pg.links[0].label) > UT_FIELD_MAX - 4,
          "a title for a label is bounded by UT_FIELD_MAX");
    free(body);

    /* the rest of what a coverage run found unreached */
    page("text/html", "", LIT("<meta http-equiv=content-type content=\"text/html\">caf\xc3\xa9"), "a Content-Type without charset");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9"), "a <meta> Content-Type without a charset: the bytes decide");
    page("text/html", "", LIT("<meta http-equiv=content-type content=\"text/html; charset ; charset = windows-1252\">caf\xc3\xa9"),
         "charset = x");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\x83\xc2\xa9"), "spaces around a charset's = (after a charset with none)");
    page("text/html", "", LIT("<meta http-equiv=content-type content=\"text/html; charset='windows-1252\">caf\xc3\xa9"), "quote");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9"), "a charset whose quote never closes is none");
    page("text/html", "", LIT("<meta/charset=windows-1252>caf\xc3\xa9"), "meta/");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\x83\xc2\xa9"), "<meta/charset=...>");
    page("text/html", "", LIT("caf\xc3\xa9 \xe2\x82"), "a 3-byte character cut short");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\xa9 \xef\xbf\xbd\xef\xbf\xbd"), "a character cut after its second byte is still UTF-8");
    page("text/html", "", LIT("caf\xc3\xa9 \xe2" "A"), "a lead byte before ASCII");
    check(pg_ok && !strcmp(pg.text, "caf\xc3\x83\xc2\xa9 \xc3\xa2" "A"), "a lead byte followed by ASCII is not UTF-8");
    page("", "", LIT("\xef\xbb\xbf  <p>sniffed"), "a byte order mark before sniffing");
    check(pg_ok && !strcmp(pg.text, "sniffed"), "no Content-Type: sniffed after a UTF-8 byte order mark");
    text_is("<ol start=\" 7\"><li>s</ol><input type=image><input type=button><pre>\r\nx</pre>",
            "7. s\n\n[button: Submit][button]\n\nx", "start with spaces; image and button inputs without labels; CR LF after <pre>");
    b.s = NULL; b.len = b.cap = 0;
    for(i = 0; i < 48; i++) {                       /* the text full at each byte of two rows: " | ", "\n", "[N]", " " */
        size_t k, before = UT_TEXT_MAX - (sizeof(NOTICE) - 1) - 2 - i;     /* the pre's text, then its blank line */
        int ok;
        b.len = 0;
        sb_str(&b, "<pre>");
        for(k = 0; k < before; k++) sb_add(&b, "x", 1);
        sb_str(&b, "</pre><table>");
        for(k = 0; k < 4; k++) sb_str(&b, "<tr><td>ab</td><td>cd</td><td><a href=/l>e</a> f</td></tr><tr><td>g<br>h</td></tr>");
        sb_str(&b, "</table>");
        page("text/html", "utf-8", b.s, b.len, "a table at the text's end");
        ok = pg_ok && (pg.truncated & UT_TRUNC_TEXT) && pg.text_len <= UT_TEXT_MAX;
        if(!ok) printf("  cut %u bytes into the table: not cut as it should be\n", (unsigned)i);
        checks++;
        if(!ok) failures++;
    }
    free(b.s);
    (void)len;
}

static void misc(void) {
    struct ut_document d;
    char err[UT_ERR_MAX];
    size_t off;
    ut_document_free(NULL);
    memset(&d, 0, sizeof(d));
    ut_document_free(&d);
    check(ut_render(NULL, &d, err, sizeof(err)) == -1 && err[0], "no response is an error");
    check(ut_anchor_offset(&d, "x", &off) == -1, "an empty document has no anchors");
    page("text/html", "utf-8", "", 0, "an empty body");
    check(pg_ok && pg.text_len == 0 && !strcmp(pg.title, "") && !pg.link_count && !pg.anchor_count, "an empty body is an empty page");
    page("text/html", "utf-8", LIT("a\0b<title>t\0i</title><img alt=\"x\0y\">c"), "NUL bytes");
    check(pg_ok && !strcmp(pg.text, "ab[xy]c") && !strcmp(pg.title, "ti"), "NUL bytes are dropped everywhere");
    page("text/html", "utf-8", LIT("<title> Two\n\tlines &amp;\xc2\xa0more </title><title>Second</title>x"), "titles");
    check(pg_ok && !strcmp(pg.title, "Two lines & more"), "the first title, whitespace collapsed");
}

/**** every allocation failing in turn ****/

static void fail_each(const char *what, const char *type, const char *charset, const char *body, size_t len) {
    struct ut_response *r = response(PAGE_URL, type, charset, body, len);
    struct ut_document ref, d, untouched;
    char err[UT_ERR_MAX], msg[200], *want, *got;
    long n, total, blocks, failed = 0, kept = 0;
    size_t bytes;
    int ok = 1;
    if(render(r, &ref, what)) {
        response_free(r);
        return;
    }
    total = alloc_calls;
    if(!ut_render(r, &d, err, sizeof(err))) ut_document_free(&d);
    total = alloc_calls - total;                    /* the allocations one render asks for */
    want = dump(&ref, r);
    for(n = 0; n < total && ok; n++) {
        memset(&d, 0x5a, sizeof(d));
        memcpy(&untouched, &d, sizeof(d));
        blocks = live_blocks;
        bytes = live_bytes;
        err[0] = 0;
        fail_after = n;
        if(ut_render(r, &d, err, sizeof(err)) == 0) {
            const char *why;
            fail_after = -1;
            kept++;                                 /* a failure it could do without (an array not made smaller) */
            why = bad_doc(&d, live_bytes - bytes);
            got = dump(&d, r);
            if(why || strcmp(got, want) || d.text_len != ref.text_len || strcmp(d.text, ref.text)) {
                printf("  allocation %ld failing: %s\n", n, why ? why : "a different document");
                ok = 0;
            }
            free(got);
            ut_document_free(&d);
            continue;
        }
        fail_after = -1;
        failed++;
        if(memcmp(&d, &untouched, sizeof(d)) || !err[0] || live_blocks != blocks || live_bytes != bytes ||
           !strstr(err, "memory")) {
            printf("  allocation %ld failing: output %s, \"%s\", %ld blocks kept\n", n,
                   memcmp(&d, &untouched, sizeof(d)) ? "changed" : "unchanged", err, live_blocks - blocks);
            ok = 0;
        }
    }
    snprintf(msg, sizeof(msg), "%s: each of its %ld allocations failing: %ld times -1 with the output unchanged and "
             "nothing kept, %ld times the same document", what, total, failed, kept);
    check(ok && failed + kept == total, msg);
    printf("  %s\n", msg);
    free(want);
    ut_document_free(&ref);
    response_free(r);
}

static void alloc_failures(const char *dir) {
    char path[512], *article;
    size_t len;
    fail_each("a page with forms, lists and links", "text/html", "", LIT(
        "<title>Forms</title><form><input type=text name=q><input type=submit value=Go></form>"
        "<ul><li><a href=\"/a\">A</a><li><a href=\"b\">B</a><li><a href=\"mailto:x@y.test\">M</a></ul>"
        "<h1 id=h>Head</h1><table><tr><td>1<td>2</table><base href=\"https://other.test/\">"));
    fail_each("a page in an unknown charset", "text/html", "shift_jis", LIT("<p>abc\x82\xa0</p><form></form>"));
    fail_each("plain text", "text/plain", "", LIT("one\ttwo\nthree"));
    fail_each("a UTF-16 page", "text/html", "", LIT("\xff\xfe<\0p\0>\0x\0"));
    fail_each("links before a late base", "text/html", "", LIT("<a href=a>A</a><a href=#f>F</a><p id=x>x</p><a href=b>B</a>"
                                                               "<base href=\"https://other.test/d/\"><a href=c>C</a>"));
    fail_each("a data table", "text/html", "", LIT("<table><tr><th>a</th><td></td></tr><tr><td><a href=/x></a></td></tr></table>"));
    snprintf(path, sizeof(path), "%s/article.html", dir);
    article = read_file(path, &len);
    if(article) fail_each("the article fixture", "text/html", "utf-8", article, len);
    free(article);
}

/**** random pages ****/

static unsigned int rng = 2463534242u;

static unsigned int rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void torture(void) {
    static const char *const pieces[] = {
        "<p>", "</p>", "<div>", "</div>", "<ul>", "</ul>", "<ol start=-3>", "<ol start=99999999999>", "<li>", "</li>",
        "<li value=7>", "<dl>", "<dt>", "<dd>", "<blockquote>", "</blockquote>", "<a href=\"", "\">", "</a>", "<a href=",
        "<a name=", "<img alt=\"", "<img alt=", "<table>", "<tr>", "<td>", "<th>", "</td>", "</tr>", "</table>",
        "<caption>", "<pre>", "</pre>", "<listing>", "<xmp>", "</xmp>", "<h1>", "</h1>", "<h3>", "</h2>", "<br>",
        "</br>", "<hr>", "<script>", "</script>", "<style>", "</style>", "<title>", "</title>", "<textarea>",
        "</textarea>", "<svg>", "</svg>", "<math>", "<template>", "</template>", "<object>", "<iframe>", "<noembed>",
        "<noscript>", "<!--", "-->", "--!>", "<!", "<?", "</", "<![CDATA[", "]]>", "&", "&#", "&#x", ";", "&amp",
        "&nbsp;", "&#27;", "&#x9b;", "&#x202e;", "&#0;", "&#xd800;", "&#1114112;", "&shy;", "&copy", "&notin;",
        "&#128;", "&#x81;", "\"", "'", "=", ">", "<", " ", "\n", "\t", "\r", "\f", "\x1b[2J", "\xc2\x9b",
        "\xe2\x80\xae", "\xef\xbb\xbf", "\xd8\x9c", "\x9b", "\x85", "\xc3", "\xa9", "\xe2\x80", "\x00", "\x7f",
        "<base href=\"", "<meta charset=\"", "windows-1252", "utf-8", "shift_jis", "<select>", "<option>",
        "</select>", "<button>", "</button>", "<input type=submit value=\"", "<input>", "<form>", "javascript:",
        "#", "/", "http://", "https://x.test/", "\\", "%", "%zz", "<b>", "<frame src=", "id=", " id=x",
        "<span id=\"", "<center>", "<plaintext>", "<html>", "<body>", "</body>", "</html>", "<head>", "</head>",
        "<q>", "<font color=red>", "<h6 id=", "<wbr>", "&lrm;", "&#x2028;", "\xe2\x80\xa8", "\xc2\xa0", "\xc2\xad",
        "\xe2\x80\x8b", "&#x2060;", "\xef\xb8\x8f", "&#xFE0E;", "\xcd\x8f", "\xe1\xa0\x8e", "<th>", "</th>", "<svg><title/>",
        "<script><!--", "<script>", "--><", "\xff\xfe", "\xfe\xff", "\x00<\x00p\x00>"
    };
    static const char *const words[] = { "surf", "tide", "caf\xc3\xa9", "wave", "\xe4\xb8\xad", "board", "a", "AT&T" };
    static const char *const types[] = { "text/html", "text/html", "text/html", "text/plain", "", "application/xhtml+xml" };
    static const char *const sets[] = { "", "", "utf-8", "windows-1252", "iso-8859-1", "shift_jis", "utf-16", "utf-16be" };
    unsigned k, n, i, bad = 0;
    char msg[200];
    for(k = 0; k < 4000; k++) {
        struct sb b = { 0, 0, 0 };
        struct ut_response *r;
        struct ut_document d;
        char err[UT_ERR_MAX];
        const char *why = NULL;
        size_t before = live_bytes;
        int random_bytes = k % 8 == 7;
        n = rnd() % (random_bytes ? 4096 : 300);
        sb_str(&b, "");
        for(i = 0; i < n; i++) {
            unsigned c = random_bytes ? 9 : rnd() % 10;
            if(c < 6) sb_str(&b, pieces[rnd() % (sizeof(pieces) / sizeof(pieces[0]))]);
            else if(c < 8) sb_str(&b, words[rnd() % (sizeof(words) / sizeof(words[0]))]);
            else {
                char ch = (char)(rnd() & 0xff);
                sb_add(&b, &ch, 1);
            }
        }
        r = response(PAGE_URL, types[rnd() % 6], sets[rnd() % 8], b.s, b.len);
        if(ut_render(r, &d, err, sizeof(err))) why = "ut_render failed";
        else {
            why = bad_doc(&d, live_bytes - before);
            ut_document_free(&d);
        }
        if(why && bad++ < 5) {
            char path[64];
            FILE *f;
            snprintf(path, sizeof(path), "build/undertow-html-torture-%u.bin", k);
            printf("  random page %u (%lu bytes, %s, %s): %s (its bytes: %s)\n", k, (unsigned long)b.len,
                   r->content_type, r->charset, why, path);
            f = fopen(path, "wb");
            if(f) {
                fwrite(b.s, 1, b.len, f);
                fclose(f);
            }
        }
        response_free(r);
        free(b.s);
    }
    snprintf(msg, sizeof(msg), "4000 random pages (markup pieces, hostile bytes, random bytes): %u bad documents", bad);
    check(!bad, msg);
}

/**** pages made to be slow or deep ****/

#define MIB (1024u * 1024)

static char *repeat(const char *head, const char *unit, const char *tail, size_t total, size_t *len) {
    struct sb b = { 0, 0, 0 };
    size_t u = strlen(unit);
    sb_str(&b, head);
    while(b.len + u <= total) sb_add(&b, unit, u);
    sb_str(&b, tail);
    *len = b.len;
    return b.s;
}

/* Render body and report the time and the memory it took; it must be bounded */
static void timed(const char *what, const char *body, size_t len, double limit_ms, size_t *text_len) {
    struct ut_response *r = response(PAGE_URL, "text/html", "utf-8", body, len);
    struct ut_document d;
    char err[UT_ERR_MAX], msg[300];
    size_t base = live_bytes;
    const char *why;
    clock_t t0;
    double ms;
    peak_bytes = live_bytes;
    t0 = clock();
    if(ut_render(r, &d, err, sizeof(err))) {
        snprintf(msg, sizeof(msg), "%.150s: ut_render failed: %.120s", what, err);
        check(0, msg);
        response_free(r);
        return;
    }
    ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    why = bad_doc(&d, live_bytes - base);
    snprintf(msg, sizeof(msg), "%s (%lu KiB): %.1f ms, at most %lu KiB held, %lu bytes of text%s%s", what,
             (unsigned long)(len / 1024), ms, (unsigned long)((peak_bytes - base) / 1024), (unsigned long)d.text_len,
             why ? ": " : "", why ? why : "");
    check(!why && ms < limit_ms && peak_bytes - base < 3 * MIB, msg);
    printf("  %s\n", msg);
    if(text_len) *text_len = d.text_len;
    ut_document_free(&d);
    response_free(r);
}

static size_t n0;

static void bounded(void) {
    static const struct { const char *what, *head, *unit, *tail; } pages[] = {
        { "nested <div>", "", "<div>", "x" }, { "only <", "", "<", "" }, { "only <a", "", "<a", "" },
        { "one giant href", "<a href=\"", "x", "\">x</a>" }, { "one giant alt", "<img alt=\"", "y", "\">" },
        { "one giant unquoted value", "<p title=", "z", ">t" }, { "one giant id", "<p id=\"", "i", "\">t" },
        { "&# repeated", "", "&#", "" }, { "& repeated", "", "&", "" }, { "one long hex reference", "&#x", "0", "41;" },
        { "<!-- repeated", "", "<!--", "" }, { "comments", "", "<!-- c -->", "" },
        { "nested lists", "", "<ul><li>", "deep" }, { "nested tables", "", "<table><tr><td>", "cell" },
        { "</div> repeated", "", "</div>", "" }, { "<li> repeated", "", "<li>x", "" }, { "<h1> repeated", "", "<h1>x", "" },
        { "links", "", "<a href=/x>x", "" }, { "an unclosed script", "<script>", "x", "" },
        { "an unclosed title", "<title>", "t ", "" }, { "<select> repeated", "", "<select><option>o", "" },
        { "<button> repeated", "", "<button>b", "" }, { "newlines in pre", "<pre>", "\n", "x" },
        { "nested svg", "", "<svg>", "x" }, { "nested templates", "", "<template>", "x" },
        { "nested inline elements", "", "<b><i><span>", "x" }, { "nested blockquotes", "", "<blockquote>", "q" },
        { "tags with many attributes", "", "<p a=1 b='2' c=\"3\" d>", "" }, { "paragraphs of text", "", "<p>word word word word</p>", "" },
        { "an unterminated attribute", "<a href=\"", "<p>", "" }, { "end tags with attributes", "", "</p a=\"<b>\">", "" }
    };
    struct sb b = { 0, 0, 0 };
    size_t i, len, text_len = 0;
    char *body;
    unsigned n;
    for(i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        body = repeat(pages[i].head, pages[i].unit, pages[i].tail, MIB, &len);
        timed(pages[i].what, body, len, 5000, NULL);
        free(body);
    }
    for(n = 0; b.len < MIB; n++) sb_printf(&b, "<span id=a%u>x</span>", n);
    timed("distinct ids", b.s, b.len, 5000, NULL);
    free(b.s);
    b.s = NULL; b.len = b.cap = 0;
    for(n = 0; n < 2048; n++) sb_printf(&b, "<p id=\"s%u\"><a href=\"/w/p%u?a=1&amp;b=2#c\">Link %u</a></p>\n", n, n, n);
    timed("2048 links and 2048 ids", b.s, b.len, 5000, NULL);
    page("text/html", "utf-8", b.s, b.len, "2048 links and 2048 ids");
    check(pg_ok && pg.link_count == 2048 && pg.anchor_count == 2048 && !pg.truncated, "all 2048 links and 2048 ids are kept");
    b.len = 0;
    for(n = 0; n < 2048; n++) sb_printf(&b, "<a name=e%u></a>", n);
    sb_str(&b, "<pre>");
    while(b.len < MIB) sb_str(&b, " \n");
    sb_str(&b, "</pre>end");
    timed("2048 waiting anchors and lines of spaces", b.s, b.len, 5000, NULL);
    page("text/html", "utf-8", b.s, b.len, "anchors before blank lines");
    check(pg_ok && pg.anchor_count == 2048 && !strcmp(pg.text, "end") && !ut_anchor_offset(&pg, "e2047", &n0) && !n0,
          "anchors before lines of nothing but spaces point where the next text starts");
    free(b.s);
    body = repeat("", "<div>", "", 50000, &len);
    b.s = NULL; b.len = b.cap = 0;
    sb_add(&b, body, len);
    sb_str(&b, "text");
    free(body);
    body = repeat("", "</div>", "", 60000, &len);
    sb_add(&b, body, len);
    sb_str(&b, "after");
    free(body);
    page("text/html", "utf-8", b.s, b.len, "deep nesting");
    check(pg_ok && !strcmp(pg.text, "text\nafter"), "10000 nested <div> close again: the text after them is not lost");
    free(b.s);
    body = repeat("", "<ul><li>", "deep", 80000, &len);
    page("text/html", "utf-8", body, len, "deep lists");
    check(pg_ok && pg.text_len < 50 && pg.text_len > 6 && !strcmp(pg.text + pg.text_len - 6, "* deep"),
          "deeply nested lists: the indentation stops growing");
    free(body);

    /* a realistic page: navigation, headings, paragraphs with links, lists, tables, images */
    b.s = NULL; b.len = b.cap = 0;
    sb_str(&b, "<!DOCTYPE html><html><head><title>Tides</title><style>p{}</style><script>var x = '<p>';</script></head><body>"
               "<nav><ul><li><a href=\"/\">Home</a><li><a href=\"/news/\">News</a><li><a href=\"/tides/\">Tides</a></ul></nav>");
    for(n = 0; b.len < 256 * 1024; n++)
        sb_printf(&b, "<h2 id=\"s%u\">Section %u: the tides</h2>\n<p>The <a href=\"/glossary#tide\">tide</a> rises and falls twice "
                  "a day. <em>Spring tides</em> come with the new and the full moon, <strong>neap tides</strong> with the "
                  "quarters. See <a href=\"https://tides.example.test/table?id=%u&amp;fmt=txt\">the table</a> for times.</p>\n"
                  "<ul><li>High water: 06:12</li><li>Low water: 12:31 &ndash; mind the rocks</li><li>High water: 18:40</li></ul>\n"
                  "<table><tr><th>Day</th><th>High</th><th>Low</th></tr><tr><td>Mon</td><td>06:12</td><td>12:31</td></tr>"
                  "<tr><td>Tue</td><td>06:55</td><td>13:14</td></tr></table>\n<p>Caf&eacute; owners report &ldquo;busy "
                  "mornings&rdquo; when the surf is up. <img src=\"wave.png\" alt=\"A wave\"> <a href=\"#top\">Back to top</a></p>\n",
                  n, n, n);
    timed("a realistic article", b.s, b.len, 5000, &text_len);
    while(b.len < MIB) sb_str(&b, "<p>More of the same: the tide, the surf, the caf&eacute;, <a href=\"/more\">more</a>.</p>\n");
    sb_str(&b, "<footer><p>&copy; 2026 Tides</p></footer></body></html>");
    timed("a realistic article of 1 MiB", b.s, b.len, 5000, &text_len);
    free(b.s);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/fixtures/undertow";
    size_t i;
    if(argc > 2 && !strcmp(argv[1], "--dump")) return dump_file(argc, argv);
    setvbuf(stdout, NULL, _IOLBF, 0);               /* a test that hangs still shows how far it got */
    for(i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) fixture(dir, fixtures[i]);
    entities();
    charsets();
    plain();
    safety();
    links();
    anchors();
    limits();
    link_bytes();
    structure();
    review_round();
    misc();
    ut_document_free(&pg);
    alloc_failures(dir);
    torture();
    bounded();
    ut_document_free(&pg);
    check(live_blocks == 0 && live_bytes == 0, "nothing is left allocated");
    printf("UNDERTOW HTML TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
