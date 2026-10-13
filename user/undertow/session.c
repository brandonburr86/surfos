/*
SurfOS Undertow: the pages read, back and forward, the reading positions, find
----------------------
File: session.c  Date: 10/11/26
----------------------
session.h gives the rules. A load builds the new page apart (its block, the
response, the document) and only then changes the session, and the change
itself allocates nothing: dropping the pages after the current one, evicting
the oldest, putting the new one in. So whatever fails, and wherever memory
runs out, the session is as it was. A page's bytes are its own block and what
its document says it owns (ut_document.bytes, allocation by allocation).
*/

#include <stdlib.h>
#include <string.h>
#include "undertow.h"
#include "session.h"
#include "http.h"
#include "html.h"

#define NONE ((size_t)-1)

void ut_session_init(struct ut_session *s) {
    memset(s, 0, sizeof(*s));
}

static void page_free(struct ut_session *s, struct ut_page *p) {
    if(!p) return;
    if(s->match_page == p) s->match_page = NULL;
    ut_document_free(&p->doc);
    free(p);
}

void ut_session_free(struct ut_session *s) {
    unsigned int i;
    for(i = 0; i < s->count; i++) page_free(s, s->pages[i]);
    memset(s, 0, sizeof(*s));
}

struct ut_page *ut_session_current(const struct ut_session *s) {
    return s->count ? s->pages[s->current] : NULL;
}

/**** loading ****/

static void copy_field(char *dst, size_t size, const char *src) {
    size_t n = strnlen(src, size - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* url fetched and rendered into a new page, or NULL and why */
static struct ut_page *fetch_page(const struct ut_url *url, const struct ut_config *cfg, char *err, size_t cap) {
    struct ut_response *r = calloc(1, sizeof(*r));      /* 6 KB of address: not on the stack */
    struct ut_page *p = calloc(1, sizeof(*p));
    if(!r || !p) {
        free(r);
        free(p);
        ut_error(err, cap, "out of memory for a new page");
        return NULL;
    }
    if(ut_fetch(url, cfg, r, err, cap)) {
        free(r);
        free(p);
        return NULL;
    }
    if(ut_render(r, &p->doc, err, cap)) {
        ut_response_free(r);
        free(r);
        free(p);
        return NULL;
    }
    memcpy(&p->url, &r->url, sizeof(p->url));
    p->status = r->status;
    p->verified = r->verified;
    p->redirects = r->redirects;
    copy_field(p->tls, sizeof(p->tls), r->tls);
    copy_field(p->reason, sizeof(p->reason), r->reason);
    copy_field(p->content_type, sizeof(p->content_type), r->content_type);
    ut_response_free(r);
    free(r);
    p->bytes = sizeof(*p) + p->doc.bytes;
    return p;
}

/* the text offset of the part a fragment names: html.c's ut_anchor_offset() matches the fragment
   as it is written, then %XX-decoded (once: "#a%2541" is "a%41", never "aA"); 0 and *offset, or -1 */
static int find_part(const struct ut_page *p, const char *fragment, size_t *offset) {
    return ut_anchor_offset(&p->doc, fragment, offset);
}

/* the reading position at the part the page's address names: 1, or 0 and the top when there is none */
static int place(struct ut_page *p) {
    size_t off;
    p->position = 0;
    if(!p->url.fragment[0]) return 1;
    if(find_part(p, p->url.fragment, &off)) return 0;
    p->position = off <= p->doc.text_len ? off : p->doc.text_len;
    return 1;
}

/* page i goes (never the current one) */
static void evict(struct ut_session *s, unsigned int i) {
    s->bytes -= s->pages[i]->bytes;
    page_free(s, s->pages[i]);
    memmove(&s->pages[i], &s->pages[i + 1], (s->count - i - 1) * sizeof(s->pages[0]));
    s->count--;
    s->pages[s->count] = NULL;
    if(s->current > i) s->current--;
}

/* over the byte budget: the oldest pages go, never the current one */
static void keep_budget(struct ut_session *s) {
    while(s->bytes > UT_HISTORY_BYTES && s->count > 1) evict(s, s->current == 0 ? 1 : 0);
}

int ut_session_load(struct ut_session *s, const struct ut_url *url, const struct ut_config *cfg,
                    int *found, char *err, size_t cap) {
    struct ut_page *p = fetch_page(url, cfg, err, cap);
    int at;
    if(!p) return -1;
    at = place(p);
    while(s->count > s->current + 1) evict(s, s->count - 1);   /* forward: dropped now that a page loaded */
    if(s->count == UT_HISTORY_MAX) evict(s, 0);
    s->pages[s->count++] = p;
    s->current = s->count - 1;
    s->bytes += p->bytes;
    keep_budget(s);
    if(found) *found = at;
    return 0;
}

int ut_session_reload(struct ut_session *s, const struct ut_config *cfg, char *err, size_t cap) {
    struct ut_page *old = ut_session_current(s), *p;
    if(!old) return ut_error(err, cap, "no page is open, so there is nothing to reload");
    p = fetch_page(&old->url, cfg, err, cap);
    if(!p) return -1;
    p->position = old->position < p->doc.text_len ? old->position : 0;
    s->pages[s->current] = p;
    s->bytes = s->bytes - old->bytes + p->bytes;
    page_free(s, old);
    keep_budget(s);
    return 0;
}

int ut_session_move(struct ut_session *s, int direction) {
    if(!s->count) return -1;
    if(direction < 0 && s->current > 0) s->current--;
    else if(direction > 0 && s->current + 1 < s->count) s->current++;
    else return -1;
    return 0;
}

/**** links and parts of a page ****/

/* does ref start with a scheme (RFC 3986: a letter, then letters, digits, + - .) other than http and https? */
static int other_scheme(const char *ref) {
    static const char http[] = "https";
    size_t i = 0, k;
    if(!((ref[0] >= 'a' && ref[0] <= 'z') || (ref[0] >= 'A' && ref[0] <= 'Z'))) return 0;
    while((ref[i] >= 'a' && ref[i] <= 'z') || (ref[i] >= 'A' && ref[i] <= 'Z') || (ref[i] >= '0' && ref[i] <= '9') ||
          ref[i] == '+' || ref[i] == '-' || ref[i] == '.') i++;
    if(ref[i] != ':') return 0;
    if(i != 4 && i != 5) return 1;
    for(k = 0; k < i; k++) if((ref[k] | 0x20) != http[k]) return 1;
    return 0;
}

void ut_session_why(const struct ut_page *p, const char *ref, char *why, size_t cap) {
    struct ut_url *u;
    if(!ref) ref = "";
    if(other_scheme(ref)) {
        ut_error(why, cap, "Undertow opens only http: and https: addresses");
        return;
    }
    u = malloc(sizeof(*u));                     /* 6 KB: not on the stack */
    if(u && p && ut_url_resolve(&p->url, ref, u, why, cap)) {
        free(u);
        ut_error(why, cap, "%s", why);          /* url.c's message, made safe all the same */
        return;
    }
    free(u);
    ut_error(why, cap, "Undertow cannot open it");
}

int ut_session_link(const struct ut_session *s, unsigned long n, struct ut_url *out, char *err, size_t cap) {
    const struct ut_page *p = ut_session_current(s);
    const struct ut_link *l;
    unsigned long count;
    char ref[UT_FIELD_MAX], why[UT_ERR_MAX];
    size_t len;
    if(!p) return ut_error(err, cap, "no page is open: open an address first");
    count = (unsigned long)p->doc.link_count;
    if(!count) return ut_error(err, cap, "there is no link %lu: this page has no links", n);
    if(n < 1 || n > count)
        return ut_error(err, cap, "there is no link %lu: this page has %lu link%s, 1 to %lu (links lists them)", n,
                        count, count == 1 ? "" : "s", count);
    l = &p->doc.links[n - 1];
    if(!l->usable) {
        len = l->url ? strlen(l->url) : 0;
        ut_safe_text(ref, sizeof(ref), l->url, len > 200 ? 200 : len, 0);
        ut_session_why(p, l->url, why, sizeof(why));
        if(!strcmp(why, "Undertow cannot open it"))
            return ut_error(err, cap, "link %lu goes to %s%s, which Undertow cannot open", n, ref, len > 200 ? "..." : "");
        return ut_error(err, cap, "link %lu goes to %s%s: %s", n, ref, len > 200 ? "..." : "", why);
    }
    return ut_url_parse(l->url, out, err, cap);
}

int ut_session_same_page(const struct ut_session *s, const struct ut_url *url) {
    const struct ut_page *p = ut_session_current(s);
    size_t a, b;
    if(!p || !strchr(url->text, '#')) return 0;
    a = strcspn(url->text, "#");
    b = strcspn(p->url.text, "#");
    return a == b && !memcmp(url->text, p->url.text, a);
}

int ut_session_jump(struct ut_session *s, const struct ut_url *url, char *err, size_t cap) {
    struct ut_page *p = ut_session_current(s);
    size_t off;
    if(!p) return ut_error(err, cap, "no page is open: open an address first");
    if(find_part(p, url->fragment, &off))
        return ut_error(err, cap, "this page has no part called #%.200s", url->fragment);
    p->position = off <= p->doc.text_len ? off : p->doc.text_len;
    memcpy(&p->url, url, sizeof(p->url));
    return 0;
}

/**** find ****/

static unsigned char fold(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c;
}

/* the first match of needle (n > 0 bytes) that starts in [from, to) and ends in the text */
static size_t search(const char *text, size_t len, size_t from, size_t to, const char *needle, size_t n) {
    const unsigned char *t = (const unsigned char *)text, *w = (const unsigned char *)needle;
    unsigned char first = fold(w[0]);
    size_t i, k;
    if(n > len) return NONE;
    if(to > len - n + 1) to = len - n + 1;
    for(i = from; i < to; i++) {
        if(fold(t[i]) != first) continue;
        for(k = 1; k < n && fold(t[i + k]) == fold(w[k]); k++);
        if(k == n) return i;
    }
    return NONE;
}

int ut_session_find(struct ut_session *s, const char *text, int next, char *err, size_t cap) {
    struct ut_page *p = ut_session_current(s);
    size_t n, from, at, len;
    int wrapped = 0;
    if(!p) return ut_error(err, cap, "no page is open, so there is nothing to search");
    if(next) {
        if(!s->search[0]) return ut_error(err, cap, "there is nothing to find again yet: find WORDS searches first");
        from = s->match_page == p && p->position == s->match_position ? s->match_at + 1 : p->position;
    } else {
        if(!text || !text[0]) return ut_error(err, cap, "find what? Type find and the words to look for");
        n = strlen(text);
        if(n > UT_SEARCH_MAX)
            return ut_error(err, cap, "the text to find has %lu bytes: find takes at most %u", (unsigned long)n,
                            UT_SEARCH_MAX);
        memcpy(s->search, text, n + 1);
        from = p->position;
    }
    n = strlen(s->search);
    len = p->doc.text_len;
    if(from > len) from = len;
    at = search(p->doc.text, len, from, len, s->search, n);
    if(at == NONE) {
        at = search(p->doc.text, len, 0, from, s->search, n);
        wrapped = 1;
    }
    if(at == NONE) return ut_error(err, cap, "\"%.200s\" was not found on this page", s->search);
    p->position = at;
    s->match_page = p;
    s->match_at = at;
    s->match_position = at;
    return wrapped;
}
