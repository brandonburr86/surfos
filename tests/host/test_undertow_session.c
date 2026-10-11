/*
SurfOS Undertow: tests of the session: history, positions, links, find
----------------------
File: test_undertow_session.c  Date: 10/11/26
----------------------
user/undertow/session.c with fake pages: this file's own ut_fetch() and
ut_render() (and the rest of what session.c uses from http.c and html.c), so
neither the network nor the HTML renderer takes part. The fakes allocate
through the counting allocator (tests/host/undertow/alloc.c) like the real
modules, so allocation failure is tested through every step of a load.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "surf.h"
#include "alloc.h"
#include "undertow.h"
#include "session.h"

static int checks, failures;

unsigned int uptime(void) { return 1000; }
time_t ut_sim_time(time_t *t) { if(t) *t = 0; return 0; }
int ttyctl(int op, unsigned int arg) { (void)op; (void)arg; return -1; }

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/**** the fake web ****/

struct fake {
    const char *url;                /* the address without its fragment */
    const char *title, *text;
    const char *links[5];           /* their addresses; "!" first: a reference Undertow cannot open */
    const char *anchors[3];         /* names, each the start of the same word in the text */
};

static const struct fake site[] = {
    { "https://a.test/", "Page A",
      "Welcome to page A.\nIt has a First part and a Second part.\n\nSecond: the second part.",
      { "https://b.test/", "https://a.test/#Second", "!mailto:someone@example.com", "https://a.test/#Nowhere", NULL },
      { "First", "Second", NULL } },
    { "https://b.test/", "Page B", "Page B is short.", { NULL }, { NULL } },
    { "https://c.test/", "Page C", "Page C.", { NULL }, { NULL } },
    { "https://find.test/", "Find",
      "Alpha foo beta.\nFOO in capitals, Foo in title case.\ncaf\xc3\xa9 and CAF\xc3\x89 differ beyond ASCII.\nlast foo",
      { NULL }, { NULL } },
};

static long fetches, renders;
static size_t claim;                /* every document rendered says it owns this many bytes more */
static const char *changed_text;    /* when set, the next documents of reload.test have this text */

static const struct fake *fake_for(const char *url) {
    size_t i;
    for(i = 0; i < sizeof(site) / sizeof(site[0]); i++) if(!strcmp(site[i].url, url)) return &site[i];
    return NULL;
}

/* the address without its fragment */
static void bare(const char *text, char *out, size_t cap) {
    size_t n = strcspn(text, "#");
    if(n >= cap) n = cap - 1;
    memcpy(out, text, n);
    out[n] = 0;
}

int ut_fetch(const struct ut_url *url, const struct ut_config *cfg, struct ut_response *out, char *err, size_t cap) {
    char key[UT_URL_MAX + 1];
    char *body;
    (void)cfg;
    fetches++;
    if(!strncmp(url->host, "down", 4)) return ut_error(err, cap, "%s does not answer (fake)", url->host);
    bare(url->text, key, sizeof(key));
    body = ut_test_strdup(key);
    if(!body) return ut_error(err, cap, "out of memory (fake fetch)");
    memset(out, 0, sizeof(*out));
    memcpy(&out->url, url, sizeof(*url));
    out->body = (unsigned char *)body;
    out->len = strlen(body);
    out->status = strstr(key, "/missing") ? 404 : 200;
    out->verified = url->secure;
    strcpy(out->tls, url->secure ? "TLSv1.3" : "");
    strcpy(out->reason, out->status == 200 ? "OK" : "Not Found");
    strcpy(out->content_type, "text/html");
    return 0;
}

void ut_response_free(struct ut_response *r) {
    if(!r) return;
    ut_test_free(r->body);
    memset(r, 0, sizeof(*r));
}

void ut_document_free(struct ut_document *d) {
    size_t i;
    if(!d) return;
    for(i = 0; i < d->link_count; i++) {
        ut_test_free(d->links[i].label);
        ut_test_free(d->links[i].url);
    }
    for(i = 0; i < d->anchor_count; i++) ut_test_free(d->anchors[i].name);
    ut_test_free(d->text);
    ut_test_free(d->title);
    ut_test_free(d->links);
    ut_test_free(d->anchors);
    ut_test_free(d->spans);
    memset(d, 0, sizeof(*d));
}

int ut_render(const struct ut_response *r, struct ut_document *out, char *err, size_t cap) {
    const struct fake *f = fake_for((const char *)r->body);
    struct ut_document d;
    char generated[UT_URL_MAX + 64];
    const char *text = generated;
    size_t i, n;
    renders++;
    memset(&d, 0, sizeof(d));
    snprintf(generated, sizeof(generated), "The page at %s", (const char *)r->body);
    if(f) text = f->text;
    if(changed_text && strstr((const char *)r->body, "reload.test")) text = changed_text;
    d.text = ut_test_strdup(text);
    d.title = ut_test_strdup(f ? f->title : "");
    if(!d.text || !d.title) goto oom;
    d.text_len = strlen(d.text);
    d.bytes = d.text_len + 1 + strlen(d.title) + 1 + claim;
    for(n = 0; f && n < 5 && f->links[n]; n++);
    if(n) {
        d.links = ut_test_calloc(n, sizeof(*d.links));
        if(!d.links) goto oom;
        d.bytes += n * sizeof(*d.links);
        for(i = 0; i < n; i++) {
            const char *u = f->links[i];
            d.link_count = i + 1;
            d.links[i].usable = u[0] != '!';
            d.links[i].url = ut_test_strdup(u + (u[0] == '!'));
            d.links[i].label = ut_test_strdup("a link");
            if(!d.links[i].url || !d.links[i].label) goto oom;
            d.bytes += strlen(d.links[i].url) + 1 + 7;
        }
    }
    for(n = 0; f && n < 3 && f->anchors[n]; n++);
    if(n) {
        d.anchors = ut_test_calloc(n, sizeof(*d.anchors));
        if(!d.anchors) goto oom;
        d.bytes += n * sizeof(*d.anchors);
        for(i = 0; i < n; i++) {
            d.anchor_count = i + 1;
            d.anchors[i].name = ut_test_strdup(f->anchors[i]);
            if(!d.anchors[i].name) goto oom;
            d.anchors[i].offset = (size_t)(strstr(d.text, f->anchors[i]) - d.text);
            d.bytes += strlen(f->anchors[i]) + 1;
        }
    }
    memcpy(out, &d, sizeof(d));
    return 0;
oom:
    ut_document_free(&d);
    return ut_error(err, cap, "out of memory (fake render)");
}

int ut_anchor_offset(const struct ut_document *d, const char *fragment, size_t *offset) {
    size_t i;
    if(!*fragment || !strcmp(fragment, "top")) {
        *offset = 0;
        return 0;
    }
    for(i = 0; i < d->anchor_count; i++)
        if(!strcmp(d->anchors[i].name, fragment)) {
            *offset = d->anchors[i].offset;
            return 0;
        }
    return -1;
}

/**** helpers ****/

static struct ut_url *url_of(const char *text) {
    static struct ut_url *u;
    char err[UT_ERR_MAX];
    if(!u) u = malloc(sizeof(*u));
    if(ut_url_parse(text, u, err, sizeof(err))) {
        printf("bad test address %s: %s\n", text, err);
        exit(2);
    }
    return u;
}

static struct ut_config cfg;

static int load(struct ut_session *s, const char *address, int *found) {
    char err[UT_ERR_MAX];
    return ut_session_load(s, url_of(address), &cfg, found, err, sizeof(err));
}

/* the current page's address, "" without one */
static const char *here(const struct ut_session *s) {
    const struct ut_page *p = ut_session_current(s);
    return p ? p->url.text : "";
}

static size_t counted(const struct ut_session *s) {
    size_t i, total = 0;
    for(i = 0; i < s->count; i++) total += s->pages[i]->bytes;
    return total;
}

static int safe_line(const char *s) {
    for(; *s; s++) if((unsigned char)*s < 32 || (unsigned char)*s >= 127) return 0;
    return 1;
}

/**** navigation ****/

static void navigation(void) {
    struct ut_session *s = malloc(sizeof(*s));
    char err[UT_ERR_MAX];
    long before;
    int found = -1;
    ut_session_init(s);
    check(ut_session_current(s) == NULL && s->count == 0, "a new session has no page");
    check(ut_session_move(s, -1) == -1 && ut_session_move(s, 1) == -1, "no back or forward without pages");
    check(ut_session_reload(s, &cfg, err, sizeof(err)) == -1 && err[0] && safe_line(err), "no reload without a page");

    check(load(s, "https://a.test/", &found) == 0 && found == 1 && !strcmp(here(s), "https://a.test/"), "open A");
    check(load(s, "https://b.test/", &found) == 0 && !strcmp(here(s), "https://b.test/") && s->count == 2, "open B");
    ut_session_current(s)->position = 7;
    before = fetches;
    check(ut_session_move(s, -1) == 0 && !strcmp(here(s), "https://a.test/"), "back to A");
    ut_session_current(s)->position = 3;
    check(ut_session_move(s, -1) == -1 && !strcmp(here(s), "https://a.test/"), "nothing before A");
    check(load(s, "https://down.test/", &found) == -1 && !strcmp(here(s), "https://a.test/") && s->count == 2 &&
          ut_session_current(s)->position == 3, "a failed open C: still A, at its position");
    check(ut_session_move(s, 1) == 0 && !strcmp(here(s), "https://b.test/") && ut_session_current(s)->position == 7,
          "forward to B survived the failure, its position kept");
    check(ut_session_move(s, 1) == -1, "nothing after B");
    check(ut_session_move(s, -1) == 0 && ut_session_current(s)->position == 3, "back to A again, at its position");
    check(fetches == before + 1, "back and forward never fetch (only the failed open did)");
    check(load(s, "https://c.test/", &found) == 0 && s->count == 2 && !strcmp(s->pages[0]->url.text, "https://a.test/") &&
          !strcmp(here(s), "https://c.test/") && ut_session_move(s, 1) == -1, "a successful open C drops B");
    check(s->bytes == counted(s), "the session's bytes are its pages'");
    check(ut_session_move(s, -1) == 0 && !strcmp(here(s), "https://a.test/") && ut_session_move(s, 1) == 0, "A, then C again");
    ut_session_free(s);
    check(s->count == 0 && ut_alloc_live == 0, "free gives back every page");
    free(s);
}

/**** reload ****/

static void reload(void) {
    struct ut_session *s = malloc(sizeof(*s));
    char err[UT_ERR_MAX];
    struct ut_page *old;
    int found;
    ut_session_init(s);
    changed_text = "A text of some length, version one, long enough to keep a position of forty.";
    check(load(s, "https://reload.test/x", &found) == 0, "open a page to reload");
    check(load(s, "https://b.test/", &found) == 0 && ut_session_move(s, -1) == 0, "another after it, and back");
    ut_session_current(s)->position = 40;
    old = ut_session_current(s);
    changed_text = "Version two is longer still, so the reading position at byte forty stays where it was.";
    check(ut_session_reload(s, &cfg, err, sizeof(err)) == 0 && ut_session_current(s)->position == 40 &&
          strstr(ut_session_current(s)->doc.text, "Version two") && s->count == 2 && s->current == 0,
          "reload replaces the entry, keeps the position");
    check(ut_session_move(s, 1) == 0 && !strcmp(here(s), "https://b.test/") && ut_session_move(s, -1) == 0,
          "reload keeps the pages after it");
    changed_text = "Short now.";
    check(ut_session_reload(s, &cfg, err, sizeof(err)) == 0 && ut_session_current(s)->position == 0,
          "reload of a shorter text: the top");
    ut_session_current(s)->position = 5;
    strcpy(ut_session_current(s)->url.host, "down.test");      /* the next load of it fails */
    check(ut_session_reload(s, &cfg, err, sizeof(err)) == -1 && ut_session_current(s)->position == 5 &&
          !strcmp(ut_session_current(s)->doc.text, "Short now.") && err[0] && safe_line(err),
          "a failed reload keeps the text and the position");
    check(s->bytes == counted(s), "the bytes follow reloads");
    (void)old;
    changed_text = NULL;
    ut_session_free(s);
    check(ut_alloc_live == 0, "reloads leak nothing");
    free(s);
}

/**** limits ****/

static void limits(void) {
    struct ut_session *s = malloc(sizeof(*s));
    char address[64];
    int i, found, ok = 1;
    ut_session_init(s);
    for(i = 1; i <= 17; i++) {
        snprintf(address, sizeof(address), "https://p%d.test/", i);
        if(load(s, address, &found)) ok = 0;
    }
    check(ok && s->count == 16 && !strcmp(s->pages[0]->url.text, "https://p2.test/") && !strcmp(here(s), "https://p17.test/"),
          "the 17th page evicts the oldest");
    for(i = 0; i < 15; i++) if(ut_session_move(s, -1)) ok = 0;
    check(ok && !strcmp(here(s), "https://p2.test/") && ut_session_move(s, -1) == -1, "back 15 times reaches the oldest kept");
    ut_session_free(s);

    /* the byte budget: three pages of 3 MB cannot all stay */
    claim = 3u * 1024 * 1024;
    check(load(s, "https://m1.test/", &found) == 0 && load(s, "https://m2.test/", &found) == 0 && s->count == 2,
          "two pages of 3 MB");
    check(load(s, "https://m3.test/", &found) == 0 && s->count == 2 && !strcmp(s->pages[0]->url.text, "https://m2.test/") &&
          s->bytes <= UT_HISTORY_BYTES && s->bytes == counted(s), "a third: over 8 MB, the oldest goes");
    /* back to m2, then a page over the budget alone: everything else goes, never the current page */
    check(ut_session_move(s, -1) == 0, "back to m2");
    claim = 9u * 1024 * 1024;
    check(load(s, "https://huge.test/", &found) == 0 && s->count == 1 && !strcmp(here(s), "https://huge.test/") &&
          s->bytes == counted(s), "a page over the budget alone stays: it is the current one");
    claim = 3u * 1024 * 1024;
    check(load(s, "https://m4.test/", &found) == 0 && s->count == 1 && !strcmp(here(s), "https://m4.test/"),
          "the next page evicts it");
    /* a reload that grows: the oldest that is not the current page goes */
    claim = 1u * 1024 * 1024;
    ut_session_free(s);
    check(load(s, "https://reload.test/1", &found) == 0 && load(s, "https://m5.test/", &found) == 0 &&
          load(s, "https://m6.test/", &found) == 0 && ut_session_move(s, -1) == 0 && ut_session_move(s, -1) == 0,
          "three pages, back at the first");
    claim = 6u * 1024 * 1024;                  /* 6 + 1 + 1 MB and the pages' own bytes: over 8 MB */
    {
        char err[UT_ERR_MAX];
        check(ut_session_reload(s, &cfg, err, sizeof(err)) == 0 && s->current == 0 &&
              !strcmp(here(s), "https://reload.test/1") && s->bytes <= UT_HISTORY_BYTES && s->count == 2 &&
              !strcmp(s->pages[1]->url.text, "https://m6.test/"),
              "a reload over the budget: the oldest page that is not the current one goes (m5)");
    }
    claim = 0;
    ut_session_free(s);
    check(ut_alloc_live == 0, "the limits leak nothing");
    free(s);
}

/**** links and fragments ****/

static void links(void) {
    struct ut_session *s = malloc(sizeof(*s));
    struct ut_url *u = malloc(sizeof(*u));
    char err[UT_ERR_MAX];
    long before;
    int found;
    ut_session_init(s);
    check(ut_session_link(s, 1, u, err, sizeof(err)) == -1 && err[0], "no page, no links");
    check(load(s, "https://a.test/", &found) == 0, "open A");
    check(ut_session_link(s, 1, u, err, sizeof(err)) == 0 && !strcmp(u->text, "https://b.test/") &&
          !ut_session_same_page(s, u), "link 1: another page");
    check(ut_session_link(s, 0, u, err, sizeof(err)) == -1 && strstr(err, "4") && safe_line(err), "no link 0");
    check(ut_session_link(s, 5, u, err, sizeof(err)) == -1 && strstr(err, "5") && strstr(err, "4") && safe_line(err),
          "link 5 of 4: says how many there are");
    check(ut_session_link(s, 4294967295UL, u, err, sizeof(err)) == -1, "the largest number");
    check(ut_session_link(s, 3, u, err, sizeof(err)) == -1 && strstr(err, "mailto:someone@example.com") && safe_line(err),
          "an unusable link says what it is");
    check(ut_session_link(s, 2, u, err, sizeof(err)) == 0 && ut_session_same_page(s, u), "link 2: a part of this page");
    before = fetches;
    check(ut_session_jump(s, u, err, sizeof(err)) == 0 && ut_session_current(s)->position ==
          (size_t)(strstr(ut_session_current(s)->doc.text, "Second") - ut_session_current(s)->doc.text) &&
          !strcmp(here(s), "https://a.test/#Second") && fetches == before && s->count == 1,
          "a jump moves to the part, without fetching or a new entry");
    ut_session_current(s)->position = 4;
    check(ut_session_link(s, 4, u, err, sizeof(err)) == 0 && ut_session_same_page(s, u) &&
          ut_session_jump(s, u, err, sizeof(err)) == -1 && strstr(err, "Nowhere") && safe_line(err) &&
          ut_session_current(s)->position == 4 && !strcmp(here(s), "https://a.test/#Second"),
          "a missing part: why, and nothing moves");
    check(!ut_session_same_page(s, url_of("https://a.test/")), "the page's own address without a fragment loads it");
    check(!ut_session_same_page(s, url_of("https://a.test/other#Second")), "another path is another page");
    check(!ut_session_same_page(s, url_of("http://a.test/#Second")), "another scheme is another page");
    check(load(s, "https://a.test/#First", &found) == 0 && found == 1 &&
          ut_session_current(s)->position == (size_t)(strstr(ut_session_current(s)->doc.text, "First") -
                                                      ut_session_current(s)->doc.text), "a load with a fragment: at its part");
    check(load(s, "https://a.test/#Missing", &found) == 0 && found == 0 && ut_session_current(s)->position == 0,
          "a load with a missing fragment: the top, and found says so");
    check(load(s, "https://b.test/#", &found) == 0 && found == 1 && ut_session_current(s)->position == 0, "an empty fragment");
    ut_session_free(s);
    check(ut_alloc_live == 0, "links leak nothing");
    free(u);
    free(s);
}

/**** find ****/

static void find(void) {
    struct ut_session *s = malloc(sizeof(*s));
    const char *text;
    char err[UT_ERR_MAX], *big;
    int found;
    ut_session_init(s);
    check(ut_session_find(s, "foo", 0, err, sizeof(err)) == -1 && err[0], "no page: nothing to find in");
    check(load(s, "https://find.test/", &found) == 0, "open the page to search");
    text = ut_session_current(s)->doc.text;
    check(ut_session_find(s, NULL, 1, err, sizeof(err)) == -1 && strstr(err, "find"), "findnext before any find says so");
    check(ut_session_find(s, "foo", 0, err, sizeof(err)) == 0 && ut_session_current(s)->position == (size_t)(strstr(text, "foo") - text),
          "find: the first match");
    check(ut_session_find(s, NULL, 1, err, sizeof(err)) == 0 && ut_session_current(s)->position == (size_t)(strstr(text, "FOO") - text),
          "findnext: capitals match");
    check(ut_session_find(s, NULL, 1, err, sizeof(err)) == 0 && ut_session_current(s)->position == (size_t)(strstr(text, "Foo") - text),
          "findnext: title case");
    check(ut_session_find(s, NULL, 1, err, sizeof(err)) == 0 &&
          ut_session_current(s)->position == (size_t)(strstr(text, "last foo") - text) + 5, "findnext: the last");
    check(ut_session_find(s, NULL, 1, err, sizeof(err)) == 1 && ut_session_current(s)->position == (size_t)(strstr(text, "foo") - text),
          "findnext past the last: round to the top once");
    check(ut_session_find(s, "FoO", 0, err, sizeof(err)) == 0 && ut_session_current(s)->position == (size_t)(strstr(text, "foo") - text),
          "find from the reading position includes the match there");
    ut_session_current(s)->position = (size_t)(strstr(text, "caf") - text);
    check(ut_session_find(s, NULL, 1, err, sizeof(err)) == 0 && ut_session_current(s)->position == (size_t)(strstr(text, "last foo") - text) + 5,
          "findnext after the reader moved: from the new position");
    ut_session_current(s)->position = 0;
    check(ut_session_find(s, "caf\xc3\xa9", 0, err, sizeof(err)) == 0 && ut_session_find(s, NULL, 1, err, sizeof(err)) == 1 &&
          ut_session_current(s)->position == (size_t)(strstr(text, "caf\xc3\xa9") - text),
          "beyond ASCII the bytes must be the same: CAF\xc3\x89 is not caf\xc3\xa9");
    ut_session_current(s)->position = 3;
    check(ut_session_find(s, "nowhere", 0, err, sizeof(err)) == -1 && strstr(err, "nowhere") && safe_line(err) &&
          ut_session_current(s)->position == 3, "no match: why, and the position stays");
    check(ut_session_find(s, "", 0, err, sizeof(err)) == -1 && ut_session_current(s)->position == 3, "nothing to find");
    big = malloc(UT_SEARCH_MAX + 2);
    memset(big, 'a', UT_SEARCH_MAX + 1);
    big[UT_SEARCH_MAX + 1] = 0;
    check(ut_session_find(s, big, 0, err, sizeof(err)) == -1 && strstr(err, "1024") && !strcmp(s->search, "nowhere"),
          "text over 1024 bytes is refused, the last search kept");
    big[UT_SEARCH_MAX] = 0;
    check(ut_session_find(s, big, 0, err, sizeof(err)) == -1 && strlen(s->search) == UT_SEARCH_MAX, "1024 bytes are fine");
    free(big);
    check(ut_session_find(s, "\033[2J", 0, err, sizeof(err)) == -1 && safe_line(err), "a message about unsafe text is safe");
    ut_session_free(s);
    check(ut_alloc_live == 0, "find leaks nothing");
    free(s);
}

/**** out of memory, at every allocation ****/

static void out_of_memory(void) {
    struct ut_session *s = malloc(sizeof(*s));
    struct ut_url *u = malloc(sizeof(*u));
    char err[UT_ERR_MAX];
    long n;
    int found, rc, ok = 1, tries = 0;
    ut_session_init(s);
    if(load(s, "https://a.test/", &found) || load(s, "https://b.test/", &found) || ut_session_move(s, -1)) ok = 0;
    ut_session_current(s)->position = 9;
    memcpy(u, url_of("https://c.test/"), sizeof(*u));     /* parsed before allocations fail */
    for(n = 0; ; n++) {
        long live = ut_alloc_live;
        ut_alloc_fail_after = n;
        rc = ut_session_load(s, u, &cfg, &found, err, sizeof(err));
        ut_alloc_fail_after = -1;
        tries++;
        if(rc == 0) break;
        if(s->count != 2 || s->current != 0 || strcmp(here(s), "https://a.test/") || ut_session_current(s)->position != 9 ||
           ut_alloc_live != live || s->bytes != counted(s)) {
            printf("  open, failing after %ld allocations: the session changed or leaked\n", n);
            ok = 0;
        }
    }
    check(ok && tries > 3 && s->count == 2 && !strcmp(here(s), "https://c.test/"), "open: out of memory at every step changes nothing");
    ok = 1;
    ut_session_current(s)->position = 2;
    for(n = 0; ; n++) {
        long live = ut_alloc_live;
        ut_alloc_fail_after = n;
        rc = ut_session_reload(s, &cfg, err, sizeof(err));
        ut_alloc_fail_after = -1;
        if(rc == 0) break;
        if(s->count != 2 || ut_session_current(s)->position != 2 || ut_alloc_live != live || !err[0]) ok = 0;
    }
    check(ok, "reload: out of memory at every step keeps the page and its position");
    ok = ut_session_move(s, -1) == 0 && !strcmp(here(s), "https://a.test/");
    for(n = 0; ; n++) {
        ut_alloc_fail_after = n;
        rc = ut_session_link(s, 1, u, err, sizeof(err));
        ut_alloc_fail_after = -1;
        if(rc == 0) break;
        if(!err[0] || n > 10) { ok = 0; break; }
    }
    check(ok && n > 0 && rc == 0 && !strcmp(u->text, "https://b.test/"), "a link's address: out of memory first, then it");
    ut_session_free(s);
    check(ut_alloc_live == 0, "out of memory leaks nothing");
    free(u);
    free(s);
}

int main(void) {
    ut_config_init(&cfg);
    navigation();
    reload();
    limits();
    links();
    find();
    out_of_memory();
    printf("UNDERTOW SESSION TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
