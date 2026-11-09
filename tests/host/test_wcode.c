/* Host tests of the Code Editor's model (user/wcode/, 11/05/26, the desktop applications, track code): every file of
   it but main.c, built as it is with the host's C library and libsurf's regular expressions (tests/host/wcode/regex.h).

   - UTF-8: the lengths RFC 3629 allows, decoding and encoding.
   - The text: random insertions and deletions (line feeds, CR LF, lone CRs, tabs, UTF-8 and bytes that are none)
     against a plain array, with the index's marks checked after each (sorted, at line starts, the right numbers),
     and every lookup against the array's answer; characters, CR LF as one, columns, a click's nearest boundary.
   - The history: random edits of every kind, the text after each step kept, then undone to the start and done again
     to the end, each step's text compared; the runs a person expects as one step; the saved text unmodified again;
     a change larger than the history forgets it.
   - Editing: line ends kept and made the file's, paste and copy, auto-indent in each language, the closing brace,
     Tab and Shift+Tab on blocks, Backspace in an indentation, comments on and off, words, Home, Up and Down over
     tabs, go to line, brackets that strings and comments do not count.
   - Reading a file: its line ends, its indentation, its language by name and by #!, a NUL refused.
   - The tokenizers, a line at a time against what each byte should be, states over lines (comments, strings,
     here-documents, fences, define), and random bytes in every language; then the colouring's states after random
     edits against the states worked out from the top with nothing kept, with budgets and idle steps between.
   - Find and replace: either case, whole words, regular expressions with groups, round the ends either way, never
     an empty match, Replace all as one step that undo takes back.
   - Find in files over a file system in memory: what it finds and in what order, filters, a binary file, a long
     line, CR LF, /dev never read, every limit, and never more than one folder and one file open.
   - The view: cells of tabs and UTF-8, the selection, the brackets, a click's position, following the caret.

   `test_wcode speed` times a 16 MB file instead (docs/superpowers/plans/2026-11-05-desktop-apps-code.md has the
   numbers). Built and run by `make unittest`; tests/host/wcode/mac-sanitize.sh runs it under ASan and UBSan on the
   Mac, and again with the index's marks every 3 lines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include "text.h"
#include "undo.h"
#include "syntax.h"
#include "doc.h"
#include "find.h"
#include "grep.h"
#include "view.h"
#include "app.h"

/* libsurf's regex.c, built for the host, allocates under these names */
void *surf_malloc(size_t n) { return malloc(n); }
void surf_free(void *p) { free(p); }
void *surf_calloc(size_t n, size_t k) { return calloc(n, k); }
void *surf_realloc(void *p, size_t n) { return realloc(p, n); }

/* 11/09/26: the history's and the tabs' allocations (the Makefile builds undo.c and app.c with WC_UNDO_REALLOC and
   WC_APP_MALLOC naming these), each failing on request: the how-manieth call from now, -1 for none */
static int realloc_fails_at = -1, malloc_fails_at = -1;
void *wc_test_realloc(void *p, size_t n);
void *wc_test_malloc(size_t n);
void *wc_test_realloc(void *p, size_t n) {
    if(realloc_fails_at >= 0 && !realloc_fails_at--) return NULL;
    return realloc(p, n);
}
void *wc_test_malloc(size_t n) {
    if(malloc_fails_at >= 0 && !malloc_fails_at--) return NULL;
    return malloc(n);
}

int wc_lex_tables_sorted(void);

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { if(++failed <= 40) printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

static unsigned long long seed = 88172645463325252ULL;
static unsigned rnd(unsigned n) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return n ? (unsigned)(seed % n) : 0;
}

/**** a document from a string, its text as a string ****/

static struct wc_pool pool = { 0, WC_POOL_MAX };

static void doc_from(struct wc_doc *d, const char *path, const char *s, unsigned n) {
    char *p;
    if(wc_doc_init(d, &pool) < 0) abort();
    p = wc_text_load_begin(&d->t, n);
    if(!p) abort();
    memcpy(p, s, n);
    wc_text_load_end(&d->t, n);
    if(wc_doc_loaded(d, path) < 0) abort();
}

static void doc_str(struct wc_doc *d, const char *path, const char *s) {
    doc_from(d, path, s, (unsigned)strlen(s));
}

static char *text_of(struct wc_text *t) {
    char *b = malloc(t->len + 1);
    wc_text_get(t, 0, b, t->len);
    b[t->len] = 0;
    return b;
}

static int text_is(struct wc_doc *d, const char *want) {
    char *b = text_of(&d->t);
    int ok = strlen(want) == d->t.len && !memcmp(b, want, d->t.len);
    if(!ok && failed < 40) printf("  text: [%s]\n  want: [%s]\n", b, want);
    free(b);
    return ok;
}

/* the text with the caret as | (and the anchor as ^ when they differ) */
static int shows(struct wc_doc *d, const char *want) {
    char *b = text_of(&d->t), *o = malloc(d->t.len + 3);
    unsigned k, n = 0;
    int ok;
    for(k = 0; k <= d->t.len; k++) {
        if(k == d->anchor && d->anchor != d->caret) o[n++] = '^';
        if(k == d->caret) o[n++] = '|';
        if(k < d->t.len) o[n++] = b[k];
    }
    o[n] = 0;
    ok = !strcmp(o, want);
    if(!ok && failed < 40) printf("  shows: [%s]\n  want:  [%s]\n", o, want);
    free(b);
    free(o);
    return ok;
}

/* a document whose text is s with | as the caret (and ^ as the anchor) */
static void doc_at(struct wc_doc *d, const char *path, const char *s) {
    char *b = calloc(strlen(s) + 1, 1);
    unsigned n = 0, caret = 0, anchor = (unsigned)-1;
    const char *p;
    for(p = s; *p; p++) {
        if(*p == '|') caret = n;
        else if(*p == '^') anchor = n;
        else b[n++] = *p;
    }
    doc_from(d, path, b, n);
    wc_doc_select(d, anchor == (unsigned)-1 ? caret : anchor, caret);
    free(b);
}

/**** UTF-8 ****/

static void utf8(void) {
    static const struct { const char *s; int len; unsigned cp; } t[] = {
        { "A", 1, 'A' }, { "\xC3\xA9", 2, 0xE9 }, { "\xE2\x89\x8B", 3, 0x224B }, { "\xF0\x9F\x98\x80", 4, 0x1F600 },
        { "\xC0\x80", 1, WC_BAD | 0xC0 }, { "\xC1\xBF", 1, WC_BAD | 0xC1 }, { "\xE0\x9F\xBF", 1, WC_BAD | 0xE0 },
        { "\xED\xA0\x80", 1, WC_BAD | 0xED }, { "\xF0\x8F\xBF\xBF", 1, WC_BAD | 0xF0 },
        { "\xF4\x90\x80\x80", 1, WC_BAD | 0xF4 }, { "\xF5\x80\x80\x80", 1, WC_BAD | 0xF5 }, { "\x80", 1, WC_BAD | 0x80 },
        { "\xC3", 1, WC_BAD | 0xC3 }, { "\xE2\x89", 1, WC_BAD | 0xE2 }, { "\xC3\x41", 1, WC_BAD | 0xC3 },
        { "\xF4\x8F\xBF\xBF", 4, 0x10FFFF }, { "\xEF\xBF\xBF", 3, 0xFFFF }, { "\xED\x9F\xBF", 3, 0xD7FF },
    };
    unsigned i, cp, k;
    char b[4];
    for(i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        CHECK(wc_utf8_len((const unsigned char *)t[i].s, (unsigned)strlen(t[i].s)) == t[i].len);
        CHECK(wc_utf8_decode((const unsigned char *)t[i].s, (unsigned)strlen(t[i].s), &cp) == (unsigned)t[i].len);
        CHECK(cp == t[i].cp);
    }
    for(cp = 0; cp < 0x110000; cp += cp < 0x3000 ? 1 : 97) {
        int n = wc_utf8_put(cp, b);
        if(cp >= 0xD800 && cp < 0xE000) {
            CHECK(n == 0);
            continue;
        }
        CHECK(n >= 1 && wc_utf8_decode((const unsigned char *)b, (unsigned)n, &k) == (unsigned)n && k == cp);
    }
    CHECK(wc_utf8_put(0x110000, b) == 0);
}

/**** the text against a plain array ****/

static char *ref;
static unsigned rlen, rcap;

static void ref_set(const char *s, unsigned n) {
    if(n + 1 > rcap) {
        rcap = n + 1024;
        ref = realloc(ref, rcap);
    }
    memcpy(ref, s, n);
    rlen = n;
}

static void ref_insert(unsigned pos, const char *s, unsigned n) {
    if(rlen + n + 1 > rcap) {
        rcap = (rlen + n) * 2 + 64;
        ref = realloc(ref, rcap);
    }
    memmove(ref + pos + n, ref + pos, rlen - pos);
    memcpy(ref + pos, s, n);
    rlen += n;
}

static void ref_delete(unsigned pos, unsigned n) {
    memmove(ref + pos, ref + pos + n, rlen - pos - n);
    rlen -= n;
}

static unsigned ref_line_start(unsigned line) {
    unsigned k, at = 0;
    for(k = 0; k < rlen && at < line; k++)
        if(ref[k] == '\n') at++;
    return k;
}

static unsigned ref_line_of(unsigned pos) {
    unsigned k, at = 0;
    for(k = 0; k < pos; k++)
        if(ref[k] == '\n') at++;
    return at;
}

static unsigned ref_nl(void) {
    return ref_line_of(rlen);
}

static const char *const pieces[] = {
    "a", "bc", "def", "\n", "\r\n", "\r", "\t", "\xC3\xA9", "\xE2\x89\x8B", "\xF0\x9F\x98\x80", "\xFF", "\x80", "{",
    "}", "/*", "*/", "\"", "#", "  ", "\n\n\n", "x\ny\nz\n", "\xE2\x89", "(", ")", "'", "\\", "//", "$", "<<",
};

static unsigned random_text(char *b, unsigned max) {
    unsigned n = 0, k;
    const char *p;
    while(n < max) {
        p = pieces[rnd(sizeof(pieces) / sizeof(pieces[0]))];
        k = (unsigned)strlen(p);
        if(n + k > max) break;
        memcpy(b + n, p, k);
        n += k;
        if(!rnd(4)) break;
    }
    return n;
}

static void check_index(struct wc_text *t) {
    unsigned i;
    int ok = t->nm >= 1 && t->m[0].pos == 0 && t->m[0].line == 0;
    for(i = 1; i < t->nm && ok; i++)
        ok = t->m[i].pos > t->m[i - 1].pos && t->m[i].line > t->m[i - 1].line && t->m[i].pos <= t->len &&
             wc_text_byte(t, t->m[i].pos - 1) == '\n' && ref_line_start(t->m[i].line) == t->m[i].pos;
    CHECK(ok);
}

static void check_text(struct wc_text *t) {
    char *b = text_of(t);
    unsigned k, line, pos;
    CHECK(t->len == rlen && !memcmp(b, ref, rlen));
    CHECK(t->nl == ref_nl());
    free(b);
    check_index(t);
    for(k = 0; k < 4; k++) {
        line = rnd(t->nl + 2);
        CHECK(wc_text_line_start(t, line) == ref_line_start(line > t->nl ? t->nl : line));
        pos = rnd(t->len + 1);
        CHECK(wc_text_line_of(t, pos) == ref_line_of(pos));
        CHECK(wc_text_line_begin(t, pos) == ref_line_start(ref_line_of(pos)));
    }
    check_index(t);
}

static void text_fuzz(unsigned rounds) {
    struct wc_text t;
    char b[64];
    unsigned r, op, pos, n, k;
    for(r = 0; r < rounds; r++) {
        if(wc_text_init(&t, &pool) < 0) abort();
        rlen = 0;
        for(op = 0; op < 400; op++) {
            pos = rnd(t.len + 1);
            if(rnd(3)) {
                n = random_text(b, sizeof(b));
                if(!rnd(20))                    /* many lines at once, for the marks */
                    for(k = 0; k < 300; k++) {
                        CHECK(wc_text_insert(&t, pos, "q\n", 2) == 0);
                        ref_insert(pos, "q\n", 2);
                    }
                CHECK(wc_text_insert(&t, pos, b, n) == 0);
                ref_insert(pos, b, n);
            } else {
                n = rnd(t.len - pos + 1);
                if(!rnd(10)) n = t.len - pos;
                wc_text_delete(&t, pos, n);
                ref_delete(pos, n);
            }
            if(!rnd(5)) {                       /* the gap elsewhere */
                pos = rnd(t.len + 1);
                n = rnd(t.len - pos + 1);
                CHECK(!memcmp(wc_text_span(&t, pos, n), ref + pos, n));
            }
            if(!rnd(3)) check_text(&t);
        }
        check_text(&t);
        wc_text_free(&t);
    }
    CHECK(pool.used == 0);
}

/* lookups straight after a file is read, from the end back, and a file of empty lines */
static void text_index(void) {
    struct wc_text t;
    char *p;
    unsigned k, n = 100000;
    if(wc_text_init(&t, &pool) < 0) abort();
    p = wc_text_load_begin(&t, n);
    for(k = 0; k < n; k++) p[k] = k % 3 == 2 ? '\n' : 'x';
    wc_text_load_end(&t, n);
    ref_set(p, n);
    CHECK(t.nl == n / 3);
    CHECK(wc_text_line_start(&t, t.nl) == ref_line_start(t.nl));
    for(k = t.nl; k > 997; k -= 997) CHECK(wc_text_line_start(&t, k) == 3 * k);
    CHECK(wc_text_line_of(&t, n) == n / 3);
    check_index(&t);
    CHECK(t.nm > 2 && t.nm < n / 3 / (WC_MARK_EVERY > 8 ? 64 : 1) + 8);
    wc_text_free(&t);
    if(wc_text_init(&t, &pool) < 0) abort();
    p = wc_text_load_begin(&t, n);
    memset(p, '\n', n);
    wc_text_load_end(&t, n);
    CHECK(t.nl == n && wc_text_line_start(&t, 77777) == 77777 && wc_text_line_of(&t, 5) == 5);
    CHECK(wc_text_line_start(&t, n + 5) == n);
    wc_text_free(&t);
    /* too large, whatever room the gap has; and the pool */
    if(wc_text_init(&t, &pool) < 0) abort();
    p = wc_text_load_begin(&t, WC_TEXT_MAX - 10);
    memset(p, 'x', WC_TEXT_MAX - 10);
    wc_text_load_end(&t, WC_TEXT_MAX - 10);
    CHECK(t.ge - t.gs > 20 && wc_text_insert(&t, 5, "0123456789", 10) == 0);
    CHECK(wc_text_insert(&t, 5, "x", 1) == -1 && errno == EFBIG && t.len == WC_TEXT_MAX);
    wc_text_free(&t);
    {                                           /* Replace all that would pass it: none of it */
        struct wc_doc d;
        struct wc_find f;
        if(wc_doc_init(&d, &pool) < 0) abort();
        p = wc_text_load_begin(&d.t, WC_TEXT_MAX - 100);
        for(k = 0; k < WC_TEXT_MAX - 100; k++) p[k] = k % 50 == 49 ? '\n' : 'a';
        wc_text_load_end(&d.t, WC_TEXT_MAX - 100);
        wc_doc_loaded(&d, "big.txt");
        wc_find_init(&f);
        strcpy(f.what, "aaaaaaaaaa");
        strcpy(f.with, "aaaaaaaaaab");
        CHECK(wc_find_replace_all(&f, &d) < 0 && strstr(d.msg, "16 MB") && d.t.len == WC_TEXT_MAX - 100);
        CHECK(!wc_doc_modified(&d) && wc_undo_can(&d.u, 0) == 0);
        wc_find_free(&f);
        wc_doc_free(&d);
    }
    if(wc_text_init(&t, &pool) < 0) abort();
    CHECK(!wc_text_load_begin(&t, WC_TEXT_MAX + 1) && errno == EFBIG);
    CHECK(wc_text_load_begin(&t, 10) != NULL);
    wc_text_load_end(&t, 0);
    {
        struct wc_pool small = { 0, 200 << 10 };
        struct wc_text u;
        static char big[300 << 10];
        if(wc_text_init(&u, &small) < 0) abort();
        CHECK(wc_text_insert(&u, 0, big, 150 << 10) == 0);
        CHECK(wc_text_insert(&u, 0, big, 100 << 10) == -1 && errno == ENOMEM && u.len == 150 << 10);
        CHECK(small.used <= small.limit);
        wc_text_delete(&u, 0, u.len);
        wc_text_free(&u);
        CHECK(small.used == 0);
    }
    wc_text_free(&t);
}

static void characters(void) {
    struct wc_doc d;
    struct wc_line l;
    unsigned cp;
    /* a, CR LF, é, a lone CR, a tab, a bad byte, ≋, LF */
    doc_str(&d, "c.txt", "a\r\n\xC3\xA9\rx\t\xFFy\xE2\x89\x8Bz\n");
    CHECK(wc_text_next(&d.t, 0) == 1 && wc_text_next(&d.t, 1) == 3 && wc_text_next(&d.t, 3) == 5);
    CHECK(wc_text_prev(&d.t, 3) == 1 && wc_text_prev(&d.t, 5) == 3 && wc_text_prev(&d.t, 6) == 5);
    CHECK(wc_text_snap(&d.t, 2) == 1 && wc_text_snap(&d.t, 4) == 3 && wc_text_snap(&d.t, 12) == 10);
    CHECK(wc_text_snap(&d.t, 11) == 10 && wc_text_snap(&d.t, 13) == 13 && wc_text_snap(&d.t, 9) == 9);
    CHECK(wc_text_char(&d.t, 1, &cp) == 2 && cp == '\n');
    CHECK(wc_text_char(&d.t, 3, &cp) == 2 && cp == 0xE9);
    CHECK(wc_text_char(&d.t, 5, &cp) == 1 && cp == '\r');
    CHECK(wc_text_char(&d.t, 8, &cp) == 1 && cp == (WC_BAD | 0xFF));
    CHECK(wc_text_char(&d.t, 10, &cp) == 3 && cp == 0x224B);
    wc_text_line(&d.t, 0, &l);
    CHECK(l.start == 0 && l.end == 1 && l.eol == 2);
    wc_text_line(&d.t, 3, &l);
    CHECK(l.end == 14 && l.eol == 1);
    /* é ♪ x, a tab to 8, the bad byte, y, ≋, z */
    CHECK(wc_text_col(&d.t, 3, 7, 8) == 3 && wc_text_col(&d.t, 3, 8, 8) == 8 && wc_text_col(&d.t, 3, 14, 8) == 12);
    CHECK(wc_text_at_col(&d.t, &l, 0, 8) == 3 && wc_text_at_col(&d.t, &l, 1, 8) == 5);
    CHECK(wc_text_at_col(&d.t, &l, 4, 8) == 7 && wc_text_at_col(&d.t, &l, 5, 8) == 7);    /* the tab's nearer side */
    CHECK(wc_text_at_col(&d.t, &l, 6, 8) == 8 && wc_text_at_col(&d.t, &l, 99, 8) == 14);
    wc_doc_free(&d);
}

/**** the history ****/

struct snap { char *s; unsigned n; };

static void history_fuzz(unsigned rounds) {
    static const char *const langs[] = { "a.c", "a.py", "Makefile", "a.sh", "a.md", "a.json", "a.txt", "a.asm" };
    struct wc_doc d;
    struct snap *snaps;
    char b[64], clip[4096];
    unsigned r, op, n, i, steps, k, ns;
    for(r = 0; r < rounds; r++) {
        n = random_text(b, sizeof(b));
        doc_from(&d, langs[r % 8], b, n);
        if(r & 1) d.crlf = 1;
        snaps = calloc(1200, sizeof(struct snap));
        snaps[0].s = text_of(&d.t);
        snaps[0].n = d.t.len;
        ns = 1;
        for(op = 0; op < 300; op++) {
            unsigned before = wc_undo_can(&d.u, 0);
            switch(rnd(16)) {
            case 0: case 1: n = random_text(b, 8); wc_doc_type(&d, b, n); break;
            case 2: wc_doc_enter(&d); break;
            case 3: wc_doc_backspace(&d, rnd(3) == 0); break;
            case 4: wc_doc_delete(&d, rnd(3) == 0); break;
            case 5: n = random_text(b, sizeof(b)); wc_doc_paste(&d, b, n); break;
            case 6: wc_doc_tab(&d, rnd(2)); break;
            case 7: wc_doc_comment(&d); break;
            case 8:
                k = rnd(12);
                wc_doc_move(&d, (int)k, (int)rnd(2), 5);
                break;
            case 9:                             /* (one rnd() at a time: compilers order arguments differently) */
                k = rnd(d.t.len + 1);
                wc_doc_select(&d, k, rnd(d.t.len + 1));
                break;
            case 10: wc_doc_type(&d, "}", 1); break;
            case 11:
                k = rnd(d.t.len + 1);
                n = random_text(b, 16);
                i = k + rnd(d.t.len - k + 1);
                wc_doc_replace(&d, k, i, b, n, (int)rnd(2));
                break;
            case 12: if(wc_doc_copy(&d, clip, sizeof(clip))) wc_doc_erase(&d); break;
            case 13: wc_doc_type(&d, " ", 1); break;
            case 14: wc_doc_move(&d, WC_LEFT, 0, 5); break;
            default: wc_doc_type(&d, "x", 1); break;
            }
            steps = wc_undo_can(&d.u, 0);
            if(steps == before + 1) {           /* a new step */
                snaps[ns].s = text_of(&d.t);
                snaps[ns++].n = d.t.len;
            } else if(steps == before) {        /* the same step, run on (or nothing) */
                free(snaps[ns - 1].s);
                snaps[ns - 1].s = text_of(&d.t);
                snaps[ns - 1].n = d.t.len;
            } else CHECK(0);
            CHECK(d.caret <= d.t.len && d.anchor <= d.t.len && wc_text_snap(&d.t, d.caret) == d.caret);
        }
        CHECK(ns == wc_undo_can(&d.u, 0) + 1);
        for(i = ns - 1; i > 0; i--) {           /* back to the start, a step at a time */
            CHECK(wc_doc_undo(&d, 0) == 0);
            CHECK(d.t.len == snaps[i - 1].n && text_is(&d, snaps[i - 1].s));
        }
        CHECK(!wc_doc_modified(&d));
        CHECK(wc_undo_can(&d.u, 0) == 0);
        for(i = 1; i < ns; i++) {               /* and on to the end again */
            CHECK(wc_doc_undo(&d, 1) == 0);
            CHECK(text_is(&d, snaps[i].s));
        }
        CHECK(ns == 1 || wc_doc_modified(&d));
        for(i = 0; i < ns; i++) free(snaps[i].s);
        free(snaps);
        wc_doc_free(&d);
    }
    CHECK(pool.used == 0);
}

static void history_steps(void) {
    struct wc_doc d;
    static char big[WC_UNDO_BYTES + 100];
    doc_at(&d, "a.txt", "|");
    wc_doc_type(&d, "h", 1);
    wc_doc_type(&d, "i", 1);
    wc_doc_type(&d, " ", 1);
    wc_doc_type(&d, "there", 5);
    CHECK(wc_undo_can(&d.u, 0) == 1);           /* typing in a row: one step */
    wc_doc_enter(&d);
    wc_doc_type(&d, "x", 1);
    CHECK(wc_undo_can(&d.u, 0) == 3);           /* Enter, then the next line's typing */
    wc_doc_backspace(&d, 0);
    wc_doc_backspace(&d, 0);
    wc_doc_backspace(&d, 0);
    CHECK(wc_undo_can(&d.u, 0) == 4 && shows(&d, "hi ther|"));
    wc_doc_move(&d, WC_LEFT, 0, 1);
    wc_doc_type(&d, "y", 1);
    CHECK(wc_undo_can(&d.u, 0) == 5 && shows(&d, "hi they|r"));
    wc_doc_undo(&d, 0);
    CHECK(shows(&d, "hi the|r"));
    wc_doc_undo(&d, 0);
    CHECK(shows(&d, "hi there\nx|"));           /* the Backspaces as one, the caret where it was */
    wc_doc_saved(&d);
    CHECK(!wc_doc_modified(&d));
    wc_doc_type(&d, "z", 1);
    CHECK(wc_doc_modified(&d) && wc_undo_can(&d.u, 1) == 0);   /* a change: what was undone is gone */
    wc_doc_undo(&d, 0);
    CHECK(!wc_doc_modified(&d));                /* back to the saved text */
    wc_doc_undo(&d, 0);
    CHECK(wc_doc_modified(&d) && shows(&d, "hi there\n|"));
    wc_doc_undo(&d, 1);
    CHECK(!wc_doc_modified(&d) && shows(&d, "hi there\nx|"));
    /* the saved text, then more typing at once: a step of its own, so undo comes back to the saved text */
    wc_doc_type(&d, "a", 1);
    wc_doc_saved(&d);
    wc_doc_type(&d, "b", 1);
    wc_doc_undo(&d, 0);
    CHECK(!wc_doc_modified(&d) && shows(&d, "hi there\nxa|"));
    /* Delete in a row: one step */
    wc_doc_move(&d, WC_DOC_START, 0, 1);
    wc_doc_delete(&d, 0);
    wc_doc_delete(&d, 0);
    wc_doc_delete(&d, 0);
    CHECK(shows(&d, "|there\nxa"));
    wc_doc_undo(&d, 0);
    CHECK(shows(&d, "|hi there\nxa"));
    /* a change too large to keep: the history goes, and the text is right */
    memset(big, 'q', sizeof(big));
    CHECK(wc_doc_paste(&d, big, sizeof(big)) == 0);
    CHECK(d.t.len == sizeof(big) + 11 && wc_undo_can(&d.u, 0) == 0 && strstr(d.msg, "undo"));
    CHECK(wc_doc_modified(&d));
    wc_doc_free(&d);
}

/**** editing ****/

static void line_ends(void) {
    struct wc_doc d;
    char clip[64];
    doc_at(&d, "w.txt", "ab|c\r\ndef\r\n");
    CHECK(d.eols == WC_EOL_CRLF && d.crlf);
    wc_doc_enter(&d);
    CHECK(shows(&d, "ab\r\n|c\r\ndef\r\n"));
    wc_doc_paste(&d, "1\n2\r\n3", 6);
    CHECK(shows(&d, "ab\r\n1\r\n2\r\n3|c\r\ndef\r\n"));
    wc_doc_move(&d, WC_END, 0, 1);
    wc_doc_move(&d, WC_RIGHT, 0, 1);
    CHECK(shows(&d, "ab\r\n1\r\n2\r\n3c\r\n|def\r\n"));
    wc_doc_move(&d, WC_LEFT, 0, 1);
    wc_doc_delete(&d, 0);                       /* the line end, both its bytes */
    CHECK(shows(&d, "ab\r\n1\r\n2\r\n3c|def\r\n"));
    wc_doc_select(&d, 0, 9);
    CHECK(wc_doc_copy(&d, clip, sizeof(clip)) == 6 && !strcmp(clip, "ab\n1\n2"));
    CHECK(wc_doc_copy(&d, clip, 4) == 0 && strstr(d.msg, "clipboard"));
    wc_doc_free(&d);
    doc_at(&d, "u.txt", "a\nb|\r\nc\rd\n");
    CHECK(d.eols == WC_EOL_MIXED && !d.crlf);
    wc_doc_enter(&d);
    CHECK(shows(&d, "a\nb\n|\r\nc\rd\n"));      /* the file's kind: most are LF; the others stay */
    wc_doc_paste(&d, "x\r\ny", 4);
    CHECK(shows(&d, "a\nb\nx\ny|\r\nc\rd\n"));
    wc_doc_free(&d);
    doc_str(&d, "n.txt", "no line end");
    CHECK(d.eols == WC_EOL_NONE && !d.crlf);
    wc_doc_free(&d);
}

static void indenting(void) {
    struct wc_doc d;
    doc_at(&d, "a.c", "int f(void) {|}\n");
    wc_doc_enter(&d);
    CHECK(shows(&d, "int f(void) {\n    |\n}\n"));
    wc_doc_type(&d, "x;", 2);
    wc_doc_enter(&d);
    CHECK(shows(&d, "int f(void) {\n    x;\n    |\n}\n"));
    wc_doc_enter(&d);                           /* a line of nothing but indentation loses it */
    CHECK(shows(&d, "int f(void) {\n    x;\n\n    |\n}\n"));
    wc_doc_type(&d, "if(a) {", 7);
    wc_doc_enter(&d);
    wc_doc_type(&d, "y; // {", 7);
    wc_doc_enter(&d);
    CHECK(shows(&d, "int f(void) {\n    x;\n\n    if(a) {\n        y; // {\n        |\n}\n"));   /* a brace in a comment opens nothing */
    wc_doc_type(&d, "}", 1);
    CHECK(shows(&d, "int f(void) {\n    x;\n\n    if(a) {\n        y; // {\n    }|\n}\n"));
    wc_doc_undo(&d, 0);
    CHECK(shows(&d, "int f(void) {\n    x;\n\n    if(a) {\n        y; // {\n        |\n}\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.c", "s = \"{\";|\n");
    wc_doc_enter(&d);
    CHECK(shows(&d, "s = \"{\";\n|\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.py", "def f(x):|\n");
    wc_doc_enter(&d);
    CHECK(shows(&d, "def f(x):\n    |\n"));
    wc_doc_free(&d);
    doc_at(&d, "Makefile", "all: a b|\n");
    wc_doc_enter(&d);
    CHECK(shows(&d, "all: a b\n\t|\n"));
    wc_doc_type(&d, "cc -o $@ $^", 11);
    wc_doc_enter(&d);
    CHECK(shows(&d, "all: a b\n\tcc -o $@ $^\n\t|\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.sh", "if true; then|\n");
    wc_doc_enter(&d);
    CHECK(shows(&d, "if true; then\n    |\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.sh", "echo then|\n");
    wc_doc_enter(&d);
    CHECK(shows(&d, "echo then\n|\n"));         /* an argument, not the word */
    wc_doc_free(&d);
    doc_at(&d, "a.json", "{\"a\": [|]}");
    wc_doc_enter(&d);
    CHECK(shows(&d, "{\"a\": [\n    |\n]}"));
    wc_doc_free(&d);
    doc_at(&d, "notes.md", "  - item|");
    wc_doc_enter(&d);
    CHECK(shows(&d, "  - item\n  |"));
    wc_doc_free(&d);
    /* indented with tabs: a file that is, gets them */
    doc_at(&d, "t.c", "x {\n\ty;\n\tz {\n\t\tw;|\n\t}\n}\n");
    CHECK(d.indent_tab);
    wc_doc_tab(&d, 0);
    CHECK(shows(&d, "x {\n\ty;\n\tz {\n\t\tw;\t|\n\t}\n}\n"));
    wc_doc_free(&d);
    doc_str(&d, "two.c", "a\n  b\n    c\n  d\ne\n  f\n");
    CHECK(!d.indent_tab && d.indent == 2);
    wc_doc_free(&d);
    doc_str(&d, "eight.py", "a\n        b\n                c\n");
    CHECK(d.indent == 8);
    wc_doc_free(&d);
    doc_str(&d, "flat.c", "a\nb\n");
    CHECK(!d.indent_tab && d.indent == 4);
    wc_doc_free(&d);
}

static void tabs_and_blocks(void) {
    struct wc_doc d;
    doc_at(&d, "a.c", "ab|c\n");
    wc_doc_tab(&d, 0);
    CHECK(shows(&d, "ab  |c\n"));               /* spaces to the next step of four */
    wc_doc_free(&d);
    doc_at(&d, "a.c", "^one\ntwo\n\nthree|\nfour\n");
    wc_doc_tab(&d, 0);
    CHECK(shows(&d, "^    one\n    two\n\n    three|\nfour\n"));   /* the empty line stays empty */
    CHECK(wc_undo_can(&d.u, 0) == 1);
    wc_doc_tab(&d, 0);
    wc_doc_tab(&d, 1);
    CHECK(shows(&d, "^    one\n    two\n\n    three|\nfour\n"));
    wc_doc_tab(&d, 1);
    wc_doc_tab(&d, 1);
    CHECK(shows(&d, "^one\ntwo\n\nthree|\nfour\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.c", "one\n^two\nthree\n|four\n");   /* a selection that ends at a line's start leaves that line */
    wc_doc_tab(&d, 0);
    CHECK(shows(&d, "one\n^    two\n    three|\nfour\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.c", "a\n    b\n        y|\n");
    wc_doc_backspace(&d, 0);                    /* after text: one character */
    CHECK(shows(&d, "a\n    b\n        |\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.c", "a\n    b\n        |y\n");
    wc_doc_backspace(&d, 0);                    /* in an indentation of spaces: a step back */
    CHECK(shows(&d, "a\n    b\n    |y\n"));
    wc_doc_backspace(&d, 0);
    CHECK(shows(&d, "a\n    b\n|y\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.c", "x\n        |y\n");     /* a file whose step is 8 */
    CHECK(d.indent == 8);
    wc_doc_backspace(&d, 0);
    CHECK(shows(&d, "x\n|y\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.c", "x\n      |y\n");
    wc_doc_backspace(&d, 0);
    CHECK(shows(&d, "x\n    |y\n"));
    wc_doc_free(&d);
}

static void comments(void) {
    struct wc_doc d;
    doc_at(&d, "a.c", "^    a();\n\n      b();|\n");
    wc_doc_comment(&d);
    CHECK(text_is(&d, "    // a();\n\n    //   b();\n"));
    wc_doc_comment(&d);
    CHECK(text_is(&d, "    a();\n\n      b();\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.py", "x = 1|\n");
    wc_doc_comment(&d);
    CHECK(text_is(&d, "# x = 1\n"));
    wc_doc_comment(&d);
    CHECK(text_is(&d, "x = 1\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.md", "x|\n");
    CHECK(wc_doc_comment(&d) < 0 && strstr(d.msg, "Markdown"));
    wc_doc_free(&d);
}

static void moving(void) {
    struct wc_doc d;
    doc_at(&d, "a.c", "|foo_bar(baz, 12);  \n  x\n");
    wc_doc_move(&d, WC_WORD_RIGHT, 0, 1);
    CHECK(shows(&d, "foo_bar|(baz, 12);  \n  x\n"));
    wc_doc_move(&d, WC_WORD_RIGHT, 0, 1);
    CHECK(shows(&d, "foo_bar(|baz, 12);  \n  x\n"));
    wc_doc_move(&d, WC_WORD_RIGHT, 0, 1);
    wc_doc_move(&d, WC_WORD_RIGHT, 0, 1);
    wc_doc_move(&d, WC_WORD_RIGHT, 0, 1);
    CHECK(shows(&d, "foo_bar(baz, 12|);  \n  x\n"));
    wc_doc_move(&d, WC_WORD_RIGHT, 0, 1);
    wc_doc_move(&d, WC_WORD_RIGHT, 0, 1);
    CHECK(shows(&d, "foo_bar(baz, 12);  |\n  x\n"));
    wc_doc_move(&d, WC_WORD_RIGHT, 0, 1);
    CHECK(shows(&d, "foo_bar(baz, 12);  \n|  x\n"));
    wc_doc_move(&d, WC_WORD_LEFT, 0, 1);
    CHECK(shows(&d, "foo_bar(baz, 12);  |\n  x\n"));
    wc_doc_move(&d, WC_WORD_LEFT, 1, 1);
    CHECK(shows(&d, "foo_bar(baz, 12|);  ^\n  x\n"));
    wc_doc_backspace(&d, 1);
    CHECK(shows(&d, "foo_bar(baz, 12|\n  x\n"));
    wc_doc_backspace(&d, 1);
    CHECK(shows(&d, "foo_bar(baz, |\n  x\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.c", "x\n    in|dented\n");
    wc_doc_move(&d, WC_HOME, 0, 1);
    CHECK(shows(&d, "x\n    |indented\n"));
    wc_doc_move(&d, WC_HOME, 0, 1);
    CHECK(shows(&d, "x\n|    indented\n"));
    wc_doc_move(&d, WC_HOME, 0, 1);
    CHECK(shows(&d, "x\n    |indented\n"));
    wc_doc_move(&d, WC_END, 1, 1);
    CHECK(shows(&d, "x\n    ^indented|\n"));
    wc_doc_free(&d);
    /* Up and Down keep their column over tabs and short lines */
    doc_at(&d, "a.c", "abcdefghij|\n\tx\nab\nabcdefghijkl\n");
    wc_doc_move(&d, WC_DOWN, 0, 1);
    CHECK(shows(&d, "abcdefghij\n\tx|\nab\nabcdefghijkl\n"));
    wc_doc_move(&d, WC_DOWN, 0, 1);
    CHECK(shows(&d, "abcdefghij\n\tx\nab|\nabcdefghijkl\n"));
    wc_doc_move(&d, WC_DOWN, 0, 1);
    CHECK(shows(&d, "abcdefghij\n\tx\nab\nabcdefghij|kl\n"));
    wc_doc_move(&d, WC_PGUP, 0, 3);
    CHECK(shows(&d, "abcdefghij\n\tx|\nab\nabcdefghijkl\n"));
    wc_doc_move(&d, WC_UP, 0, 1);
    wc_doc_move(&d, WC_UP, 0, 1);
    CHECK(shows(&d, "|abcdefghij\n\tx\nab\nabcdefghijkl\n"));
    wc_doc_move(&d, WC_DOC_END, 1, 1);
    CHECK(shows(&d, "^abcdefghij\n\tx\nab\nabcdefghijkl\n|"));
    wc_doc_move(&d, WC_LEFT, 0, 1);             /* Left on a selection: to its start */
    CHECK(shows(&d, "|abcdefghij\n\tx\nab\nabcdefghijkl\n"));
    CHECK(wc_doc_goto(&d, 3, 2) == 0 && shows(&d, "abcdefghij\n\tx\na|b\nabcdefghijkl\n"));
    wc_doc_goto(&d, 2, 5);
    CHECK(shows(&d, "abcdefghij\n|\tx\nab\nabcdefghijkl\n"));   /* the nearer side of the tab */
    wc_doc_goto(&d, 99, 0);
    CHECK(shows(&d, "abcdefghij\n\tx\nab\nabcdefghijkl\n|") && strstr(d.msg, "5 lines"));
    wc_doc_select_word(&d, 13);
    CHECK(shows(&d, "abcdefghij\n\t^x|\nab\nabcdefghijkl\n"));
    wc_doc_select_line(&d, 2, 0);
    CHECK(shows(&d, "abcdefghij\n\tx\n^ab\n|abcdefghijkl\n"));
    wc_doc_select_line(&d, 0, 1);
    CHECK(shows(&d, "|abcdefghij\n\tx\nab\n^abcdefghijkl\n"));
    wc_doc_free(&d);
}

static void brackets(void) {
    struct wc_doc d;
    unsigned at, other;
    doc_at(&d, "a.c", "f(a, \")\", g(b) /* ) */, '(')|;\n");
    CHECK(wc_syn_match(&d.t, d.lang, d.caret, &at, &other, 100, 1 << 20) && at == d.caret - 1 && other == 1);
    CHECK(wc_doc_bracket(&d, 0) && d.caret == 1);
    CHECK(wc_doc_bracket(&d, 0) && d.caret == 28);
    wc_doc_free(&d);
    doc_at(&d, "a.c", "int x[3] = {\n  1, /* } */\n  \"}\",\n}|;\n");
    CHECK(wc_doc_bracket(&d, 1) && shows(&d, "int x[3] = |{\n  1, /* } */\n  \"}\",\n}^;\n"));
    wc_doc_free(&d);
    doc_at(&d, "a.c", "(|(()\n");
    CHECK(!wc_doc_bracket(&d, 0) && strstr(d.msg, "No matching"));
    wc_doc_free(&d);
    doc_at(&d, "a.c", "a|b\n");
    CHECK(!wc_doc_bracket(&d, 0) && strstr(d.msg, "No bracket"));
    wc_doc_free(&d);
    {                                           /* across many lines, back and forth */
        static char big[40000];
        unsigned k, n = 0;
        big[n++] = '{';
        for(k = 0; k < 2000; k++) n += (unsigned)snprintf(big + n, sizeof(big) - n, "x(%u); /* { */\n", k);
        big[n++] = '}';
        doc_from(&d, "b.c", big, n);
        CHECK(wc_syn_match(&d.t, d.lang, 0, &at, &other, 100000, 1 << 24) && at == 0 && other == n - 1);
        CHECK(wc_syn_match(&d.t, d.lang, n, &at, &other, 100000, 1 << 24) && at == n - 1 && other == 0);
        CHECK(!wc_syn_match(&d.t, d.lang, n, &at, &other, 10, 1 << 24) && at == n - 1);    /* beyond the limit */
        wc_doc_free(&d);
    }
}

static void reading(void) {
    struct wc_doc d;
    CHECK(wc_lang_guess("/usb1p1/src/kernel/task.c", NULL, 0) == WC_LANG_C);
    CHECK(wc_lang_guess("x.H", NULL, 0) == WC_LANG_C);
    CHECK(wc_lang_guess("traps.asm", NULL, 0) == WC_LANG_NASM && wc_lang_guess("boot.S", NULL, 0) == WC_LANG_GAS);
    CHECK(wc_lang_guess("Makefile", NULL, 0) == WC_LANG_MAKE && wc_lang_guess("a/GNUmakefile", NULL, 0) == WC_LANG_MAKE);
    CHECK(wc_lang_guess("rules.mk", NULL, 0) == WC_LANG_MAKE && wc_lang_guess("README.md", NULL, 0) == WC_LANG_MD);
    CHECK(wc_lang_guess("a.json", NULL, 0) == WC_LANG_JSON && wc_lang_guess("x.py", NULL, 0) == WC_LANG_PY);
    CHECK(wc_lang_guess("surfos-config/surfshrc", NULL, 0) == WC_LANG_SH);
    CHECK(wc_lang_guess("run", "#!/bin/sh", 9) == WC_LANG_SH);
    CHECK(wc_lang_guess("tool", "#!/usr/bin/env python3 -u", 25) == WC_LANG_PY);
    CHECK(wc_lang_guess("x", "#!/initrd/bin/surfsh", 20) == WC_LANG_SH);
    CHECK(wc_lang_guess("notes", "hello", 5) == WC_LANG_TEXT && wc_lang_guess(".c", NULL, 0) == WC_LANG_TEXT);
    CHECK(wc_lang_guess("x", "#!", 2) == WC_LANG_TEXT);
    if(wc_doc_init(&d, &pool) < 0) abort();
    memcpy(wc_text_load_begin(&d.t, 5), "ab\0cd", 5);
    wc_text_load_end(&d.t, 5);
    CHECK(wc_doc_loaded(&d, "bin") == WC_NOT_TEXT);
    wc_doc_free(&d);
    doc_str(&d, "/x/hello.c", "int main(void)\n{\n    return 0;\n}\n");
    CHECK(d.lang == WC_LANG_C && d.eols == WC_EOL_LF && !wc_doc_modified(&d) && !strcmp(d.path, "/x/hello.c"));
    {
        char title[64];
        wc_doc_title(&d, title, sizeof(title));
        CHECK(!strcmp(title, "hello.c"));
        wc_doc_free(&d);
        if(wc_doc_init(&d, &pool) < 0) abort();
        d.untitled = 3;
        wc_doc_title(&d, title, sizeof(title));
        CHECK(!strcmp(title, "Untitled 3"));
    }
    wc_doc_free(&d);
}

/**** the tokenizers ****/

static const char codes[] = ".csnktpvlheouyb";     /* a letter a class, in the order of WC_C_* */

/* line's classes, a letter a byte as codes[], and the state after it */
static void lex_is(int lang, unsigned st, const char *line, const char *want, unsigned want_st) {
    unsigned char cls[256];
    char got[257];
    unsigned n = (unsigned)strlen(line), k, end;
    end = wc_lex(lang, line, n, st, cls, sizeof(cls));
    for(k = 0; k < n; k++) got[k] = codes[cls[k] < WC_CLASSES ? cls[k] : 0];
    got[n] = 0;
    CHECK(!strcmp(got, want) && end == want_st);
    if((strcmp(got, want) || end != want_st) && failed < 40)
        printf("  lang %d [%s]\n  got  [%s] %u\n  want [%s] %u\n", lang, line, got, end, want, want_st);
}

/* a here-document's state, as lex.c packs it: its word's length and hash (FNV-1a folded to 16 bits), <<- */
static unsigned heredoc(const char *w, int dash) {
    unsigned h = 2166136261u, k, n = (unsigned)strlen(w);
    for(k = 0; k < n; k++) h = (h ^ (unsigned char)w[k]) * 16777619u;
    return 4u | (dash ? 8u : 0) | n << 4 | ((h ^ (h >> 16)) & 0xFFFF) << 16;
}

static void tokenizers(void) {
    unsigned st;
    CHECK(wc_lex_tables_sorted());
    /* C */
    lex_is(WC_LANG_C, 0, "int x = 0x1F; // c", "ttt.....nnnn..cccc", 0);
    lex_is(WC_LANG_C, 0, "#include <stdio.h>", "pppppppp.sssssssss", 0);
    lex_is(WC_LANG_C, 0, "  # define X \"a\\\"b\" /* x", "..pppppppp...ssssss.cccc", 9);
    lex_is(WC_LANG_C, 1 | 8, "still */ y", "cccccccc..", 0);
    lex_is(WC_LANG_C, 0, "#define A(x) \\", "ppppppp.......", 8);
    lex_is(WC_LANG_C, 8, "  (x + 1) /* z */", ".......n..ccccccc", 0);
    lex_is(WC_LANG_C, 0, "s = \"abc\\", "....sssss", 2);
    lex_is(WC_LANG_C, 2, "def\" + L'x' + u8\"q\";", "ssss...ssss...sssss.", 0);
    lex_is(WC_LANG_C, 0, "return NULL, sizeof(struct s), 1.5e+3f;", "kkkkkk.nnnn..kkkkkk.kkkkkk.....nnnnnnn.", 0);
    lex_is(WC_LANG_C, 0, "x = 'a // b';", "....ssssssss.", 0);
    lex_is(WC_LANG_C, 0, "// a \\", "cccccc", 16);
    lex_is(WC_LANG_C, 16, "still", "ccccc", 0);
    lex_is(WC_LANG_C, 0, "u32 caf\xC3\xA9;", "ttt.......", 0);
    /* NASM */
    lex_is(WC_LANG_NASM, 0, "start: mov eax, [ebx+4] ; go", "llllll.kkk.ttt...ttt.n..cccc", 0);
    lex_is(WC_LANG_NASM, 0, "section .text", "ppppppp......", 0);
    lex_is(WC_LANG_NASM, 0, "msg db 'hi', 0x0A, 10", "lll.pp.ssss..nnnn..nn", 0);
    lex_is(WC_LANG_NASM, 0, "%define X 5", "ppppppp...n", 0);
    lex_is(WC_LANG_NASM, 0, "  rep movsd", "..kkk.kkkkk", 0);
    lex_is(WC_LANG_NASM, 0, "MOV AL, BYTE [x]", "kkk.tt..kkkk....", 0);
    /* GNU as */
    lex_is(WC_LANG_GAS, 0, "1:  movl $0x10, %eax  # c", "ll..kkkk.nnnnn..tttt..ccc", 0);
    lex_is(WC_LANG_GAS, 0, ".globl _start", "pppppp.......", 0);
    lex_is(WC_LANG_GAS, 0, "#include \"x.h\"", "pppppppp.sssss", 0);
    lex_is(WC_LANG_GAS, 0, "# a comment", "ccccccccccc", 0);
    lex_is(WC_LANG_GAS, 0, "lgdt (gdtr) /* x", "kkkk........cccc", 1);
    lex_is(WC_LANG_GAS, 1, "*/ ret", "cc.kkk", 0);
    lex_is(WC_LANG_GAS, 0, "x = 5; jmp 1f", "l...n..kkk.nn", 0);
    /* Python */
    lex_is(WC_LANG_PY, 0, "def f(self, x=1.5):  # c", "kkk.l.tttt....nnn....ccc", 0);
    lex_is(WC_LANG_PY, 0, "s = r'\\d' + f\"{x}\"", "....sssss...ssssss", 0);
    lex_is(WC_LANG_PY, 0, "x = \"\"\"doc", "....ssssss", 2);
    lex_is(WC_LANG_PY, 2, "more ' \" still", "ssssssssssssss", 2);
    lex_is(WC_LANG_PY, 2, "end\"\"\" + 'a\\", "ssssss...sss", 3);
    lex_is(WC_LANG_PY, 3, "b' if True else None", "ss.kk.kkkk.kkkk.kkkk", 0);
    lex_is(WC_LANG_PY, 0, "@app.route('/')", "pppppppppp.sss.", 0);
    /* shell */
    lex_is(WC_LANG_SH, 0, "if [ \"$x\" = 1 ]; then echo $HOME # c", "kk.t.svvs........kkkk.tttt.vvvvv.ccc", 0);
    lex_is(WC_LANG_SH, 0, "for f in *.c; do cc \"$f\"; done", "kkk...kk......kk....svvs..kkkk", 0);
    lex_is(WC_LANG_SH, 0, "CC=gcc make x=1", "vv.............", 0);
    st = 0;
    lex_is(WC_LANG_SH, 0, "cat <<EOF > out", "....sssss......", (st = heredoc("EOF", 0)));
    lex_is(WC_LANG_SH, st, "$x is text", "ssssssssss", st);
    lex_is(WC_LANG_SH, st, "EOF", "kkk", 0);
    st = heredoc("END", 1);
    lex_is(WC_LANG_SH, 0, "cat <<-'END'", "....ssssssss", st);
    lex_is(WC_LANG_SH, st, "\t\tEND", "kkkkk", 0);
    lex_is(WC_LANG_SH, 0, "echo 'a b", "tttt.ssss", 1);
    lex_is(WC_LANG_SH, 1, "c' x#y", "ss....", 0);
    lex_is(WC_LANG_SH, 0, "x=$(ls) ${y:-z} $1", "v.vv....vvvvvvv.vv", 0);
    /* Makefile */
    lex_is(WC_LANG_MAKE, 0, "CFLAGS += -O2 $(X) # opts", "vvvvvv........vvvv.cccccc", 0);
    lex_is(WC_LANG_MAKE, 0, "all: $(OBJ) x.o ; echo $@", "lll..vvvvvv............vv", 0);
    lex_is(WC_LANG_MAKE, 0, "\t$(CC) -c $< -o $@", ".vvvvv....vv....vv", 0);
    lex_is(WC_LANG_MAKE, 0, "\techo a \\", ".........", 2);
    lex_is(WC_LANG_MAKE, 2, "b $(x)", "..vvvv", 0);
    lex_is(WC_LANG_MAKE, 0, "SRC := $(wildcard *.c)", "vvv....vvkkkkkkkkvvvvv", 0);
    lex_is(WC_LANG_MAKE, 0, "ifeq ($(O),2)", "kkkk..vvvv...", 0);
    lex_is(WC_LANG_MAKE, 0, "define RULE", "kkkkkk.....", 8);
    lex_is(WC_LANG_MAKE, 8, "x: $(y)", "...vvvv", 8);
    lex_is(WC_LANG_MAKE, 8, "endef", "kkkkk", 0);
    lex_is(WC_LANG_MAKE, 0, "# a \\", "ccccc", 4);
    lex_is(WC_LANG_MAKE, 4, "b", "c", 0);
    /* Markdown */
    lex_is(WC_LANG_MD, 0, "## Title", "hhhhhhhh", 0);
    lex_is(WC_LANG_MD, 0, "- a *b* and `c d` [e](f)", "k...eee.....ooooo.uuusss", 0);
    lex_is(WC_LANG_MD, 0, "snake_case_name **x**", "................eeeee", 0);
    lex_is(WC_LANG_MD, 0, "```c", "oooo", 1 | 3 << 8);
    lex_is(WC_LANG_MD, 1 | 3 << 8, "int *p;", "ooooooo", 1 | 3 << 8);
    lex_is(WC_LANG_MD, 1 | 3 << 8, "```", "ooo", 0);
    lex_is(WC_LANG_MD, 0, "> 1. quote <!-- x", "p.kk.......cccccc", 4);
    lex_is(WC_LANG_MD, 4, "y --> z", "ccccc..", 0);
    lex_is(WC_LANG_MD, 0, "===", "hhh", 0);
    lex_is(WC_LANG_MD, 0, "* * *", "ppppp", 0);
    /* JSON */
    lex_is(WC_LANG_JSON, 0, "{\"a\": [1, -2.5e3, true, null], \"b\": \"x\"}", ".yyy...n..nnnnnn..kkkk..kkkk...yyy..sss.", 0);
    lex_is(WC_LANG_JSON, 0, "{x: 'y'} // c /*", ".b..bbb..ccccccc", 0);
    lex_is(WC_LANG_JSON, 0, "1 /* a", "n.cccc", 1);
    lex_is(WC_LANG_JSON, 1, "b */ 2", "cccc.n", 0);
    /* plain text has no colours */
    lex_is(WC_LANG_TEXT, 0, "int /* x", "........", 0);
}

static void lex_hostile(unsigned rounds) {
    static unsigned char cls[300];
    char b[300];
    unsigned r, k, n, lang, st, st2;
    for(r = 0; r < rounds; r++) {
        n = rnd(sizeof(b));
        for(k = 0; k < n; k++) b[k] = rnd(3) ? "/*\"'\\#$`<-{}()[]%;:=!*_~>\t \r@.0123456789abcxEOF"[rnd(48)] : (char)rnd(256);
        st = rnd(4) ? 0 : (unsigned)rnd(0xFFFFFFFFu);
        for(lang = 0; lang < WC_LANGS; lang++) {
            k = rnd(2) ? sizeof(cls) : rnd(n + 1);
            st2 = wc_lex((int)lang, b, n, st, cls, k);
            CHECK(wc_lex((int)lang, b, n, st, NULL, 0) == st2);    /* the same with no classes asked */
            for(k = 0; k < n && k < sizeof(cls); k++)
                if(cls[k] >= WC_CLASSES) break;
            CHECK(k == n || k == sizeof(cls) || cls[k] < WC_CLASSES);
        }
    }
}

/* the state at line, worked out from the top with nothing kept */
static unsigned scratch_state(struct wc_text *t, int lang, unsigned line) {
    struct wc_line l;
    unsigned pos = 0, k, st = 0;
    char *s;
    for(k = 0; k < line; k++) {
        wc_text_line(t, pos, &l);
        s = malloc(l.end - l.start + 1);
        wc_text_get(t, l.start, s, l.end - l.start);
        st = wc_lex(lang, s, l.end - l.start, st, NULL, 0);
        free(s);
        pos = l.end + l.eol;
    }
    return st;
}

static void states_fuzz(unsigned rounds) {
    static const char *const bits[] = {
        "/*", "*/", "\"", "'", "\\", "//", "#define x \\", "\n", "\n", "\n", "a", " b ", "'''", "\"\"\"", "<<EOF",
        "EOF", "\tEOF", "```", "~~~", "<!--", "-->", "define", "endef", "x:", "\t", "#", "$(", ")", "{", "}",
    };
    struct wc_text t;
    char b[64];
    unsigned r, op, n, k, pos, line, st;
    int lang, exact;
    for(r = 0; r < rounds; r++) {
        lang = 1 + (int)(r % (WC_LANGS - 1));
        if(wc_text_init(&t, &pool) < 0) abort();
        for(op = 0; op < 250; op++) {
            for(n = 0, k = rnd(6) + 1; k > 0 && n < 48; k--) {
                const char *p = bits[rnd(sizeof(bits) / sizeof(bits[0]))];
                memcpy(b + n, p, strlen(p));
                n += (unsigned)strlen(p);
            }
            pos = rnd(t.len + 1);
            if(rnd(4)) wc_text_insert(&t, pos, b, n);
            else wc_text_delete(&t, pos, rnd(40));
            switch(rnd(4)) {
            case 0:                             /* a state, exactly */
                line = rnd(t.nl + 1);
                CHECK(wc_syn_state(&t, lang, line, ~0UL, &exact) == scratch_state(&t, lang, line) && exact);
                break;
            case 1:                             /* within a small budget: right whenever it says so */
                line = rnd(t.nl + 1);
                st = wc_syn_state(&t, lang, line, rnd(200), &exact);
                if(exact) CHECK(st == scratch_state(&t, lang, line));
                break;
            case 2: wc_syn_idle(&t, lang, rnd(300)); break;
            default: wc_text_line_of(&t, rnd(t.len + 1)); break;    /* marks with no state */
            }
            if(!rnd(50)) wc_syn_reset(&t);
        }
        for(line = 0; line <= t.nl; line += 1 + rnd(5))
            CHECK(wc_syn_state(&t, lang, line, ~0UL, &exact) == scratch_state(&t, lang, line));
        while(wc_syn_idle(&t, lang, 1000));
        for(k = 0; k < t.nm; k++) CHECK(t.m[k].ok && t.m[k].state == scratch_state(&t, lang, t.m[k].line));
        wc_text_free(&t);
    }
}

/* typing inside a line leaves the states after it standing; opening a comment does not */
static unsigned good_marks(struct wc_doc *d) {
    unsigned k, n = 0;
    for(k = 0; k < d->t.nm; k++) n += d->t.m[k].ok;
    return n;
}

static void states_stand(void) {
    struct wc_doc d;
    static char big[200000];
    unsigned k, n = 0, marks;
    int exact;
    for(k = 0; k < 4000; k++) n += (unsigned)snprintf(big + n, sizeof(big) - n, "int v%u = %u;\n", k, k);
    doc_from(&d, "s.c", big, n);
    CHECK(wc_syn_state(&d.t, d.lang, 3999, ~0UL, &exact) == 0 && exact);
    marks = good_marks(&d);
    CHECK(marks == d.t.nm && marks > 20);
    wc_doc_goto(&d, 10, 1);
    wc_doc_type(&d, "x", 1);
    CHECK(wc_syn_state(&d.t, d.lang, 3999, 16u << 10, &exact) == 0 && exact);   /* from the mark before it */
    CHECK(good_marks(&d) == d.t.nm);
    wc_doc_type(&d, "/*", 2);
    CHECK(wc_syn_state(&d.t, d.lang, 3999, 16u << 10, &exact) == 0 && !exact);  /* stale: the best guess */
    CHECK(good_marks(&d) < d.t.nm);
    CHECK(wc_syn_state(&d.t, d.lang, 3999, ~0UL, &exact) == 1 && exact);    /* inside the comment */
    wc_doc_undo(&d, 0);
    CHECK(wc_syn_state(&d.t, d.lang, 3999, ~0UL, &exact) == 0 && exact);
    wc_doc_free(&d);
}

/**** find and replace ****/

static void finding(void) {
    struct wc_doc d;
    struct wc_find f;
    unsigned a, b;
    int w;
    wc_find_init(&f);
    doc_at(&d, "f.c", "|Foo foo food\nbar FOO\n");
    strcpy(f.what, "foo");
    CHECK(wc_find_doc(&f, &d, 0) == 1 && shows(&d, "Foo ^foo| food\nbar FOO\n"));
    CHECK(wc_find_doc(&f, &d, 0) == 1 && shows(&d, "Foo foo ^foo|d\nbar FOO\n"));
    CHECK(wc_find_doc(&f, &d, 0) == 1 && shows(&d, "Foo ^foo| food\nbar FOO\n") && strstr(d.msg, "from the top"));
    f.icase = 1;
    f.word = 1;
    CHECK(wc_find_doc(&f, &d, 0) == 1 && shows(&d, "Foo foo food\nbar ^FOO|\n"));
    CHECK(wc_find_doc(&f, &d, 0) == 1 && shows(&d, "^Foo| foo food\nbar FOO\n"));
    CHECK(wc_find_doc(&f, &d, 1) == 1 && shows(&d, "Foo foo food\nbar |FOO^\n") && strstr(d.msg, "bottom"));
    CHECK(wc_find_doc(&f, &d, 1) == 1 && shows(&d, "Foo |foo^ food\nbar FOO\n"));
    strcpy(f.what, "nothere");
    CHECK(wc_find_doc(&f, &d, 0) == 0 && strstr(d.msg, "Not found"));
    /* regular expressions: no empty matches (11/09/26: a pattern that matches an empty text is refused), groups in
       the replacement */
    f.regex = 1;
    f.icase = f.word = 0;
    strcpy(f.what, "o*");
    wc_doc_select(&d, 0, 0);
    CHECK(wc_find_doc(&f, &d, 0) < 0 && strstr(d.msg, "matches nothing at all") && shows(&d, "|Foo foo food\nbar FOO\n"));
    strcpy(f.what, "o+");
    CHECK(wc_find_doc(&f, &d, 0) == 1 && shows(&d, "F^oo| foo food\nbar FOO\n"));
    strcpy(f.what, "([a-z]+) ([A-Z]+)");
    CHECK(wc_find_doc(&f, &d, 0) == 1 && shows(&d, "Foo foo food\n^bar FOO|\n"));
    strcpy(f.with, "<\\2-\\1&\\n\\\\>");
    CHECK(wc_find_replace(&f, &d) == 1 && text_is(&d, "Foo foo food\n<FOO-barbar FOO\n\\>\n"));
    strcpy(f.what, "^f");
    wc_doc_select(&d, 0, 0);
    CHECK(wc_find_doc(&f, &d, 0) == 0);         /* ^ is only a line's start */
    strcpy(f.what, "(");
    CHECK(wc_find_doc(&f, &d, 0) < 0 && strstr(d.msg, "Not a pattern"));
    wc_doc_free(&d);
    /* Replace all: one step */
    doc_at(&d, "r.c", "|a.b a.b\nxa.by\r\n\na.b");
    f.regex = 0;
    strcpy(f.what, "a.b");
    strcpy(f.with, "[&]");
    CHECK(wc_find_replace_all(&f, &d) == 4 && text_is(&d, "[&] [&]\nx[&]y\r\n\n[&]") && strstr(d.msg, "Replaced 4"));
    CHECK(wc_undo_can(&d.u, 0) == 1);
    wc_doc_undo(&d, 0);
    CHECK(text_is(&d, "a.b a.b\nxa.by\r\n\na.b"));
    f.regex = 1;
    strcpy(f.what, "a(.)b");
    strcpy(f.with, "\\1\\n");
    CHECK(wc_find_replace_all(&f, &d) == 4 && text_is(&d, ".\n .\n\nx.\ny\r\n\n.\n"));
    strcpy(f.what, "zzz");
    CHECK(wc_find_replace_all(&f, &d) == 0 && strstr(d.msg, "Not found") && wc_undo_can(&d.u, 0) == 1);
    wc_doc_free(&d);
    /* a match across the gap, and the last match before a position */
    doc_str(&d, "g.c", "xxabcxxabc");
    wc_text_insert(&d.t, 4, "", 0);
    wc_text_span(&d.t, 0, 1);
    wc_doc_select(&d, 3, 3);
    wc_doc_type(&d, "", 0);
    f.regex = 0;
    strcpy(f.what, "abc");
    CHECK(wc_find_next(&f, &d.t, 3, 0, &a, &b, &w) == 1 && a == 7 && !w);
    CHECK(wc_find_next(&f, &d.t, 7, 1, &a, &b, &w) == 1 && a == 2 && !w);
    CHECK(wc_find_next(&f, &d.t, 2, 1, &a, &b, &w) == 1 && a == 7 && w);
    wc_doc_free(&d);
    wc_find_free(&f);
}

/**** find in files ****/

struct fnode { const char *path; const char *data; unsigned len; int dir; };
static struct fnode tree[64];
static unsigned ntree;
static int open_dirs, open_files, most_dirs, most_files, reads_fail;
static struct { int node, pos, used; } handles[8];

static void tree_add(const char *path, const char *data, unsigned len, int dir) {
    tree[ntree].path = path;
    tree[ntree].data = data;
    tree[ntree].len = len || !data ? len : (unsigned)strlen(data);
    tree[ntree++].dir = dir;
}

static int node_of(const char *path) {
    unsigned i;
    for(i = 0; i < ntree; i++)
        if(!strcmp(tree[i].path, path)) return (int)i;
    return -1;
}

static int handle(int node) {
    int h;
    for(h = 0; h < 8; h++)
        if(!handles[h].used) {
            handles[h].used = 1;
            handles[h].node = node;
            handles[h].pos = 0;
            return h;
        }
    return -1;
}

static const char *refused_dir;         /* 11/09/26: a folder listed that does not open */

static int f_opendir(void *ctx, const char *path) {
    int n = node_of(path);
    (void)ctx;
    if(n < 0 || !tree[n].dir || !strncmp(path, "/dev", 4) || (refused_dir && !strcmp(path, refused_dir))) return -1;
    if(++open_dirs > most_dirs) most_dirs = open_dirs;
    return handle(n);
}

static int f_readdir(void *ctx, int h, char *name, unsigned size, int *dir, unsigned *bytes) {
    const char *dp = tree[handles[h].node].path, *p;
    size_t dl = strlen(dp);
    (void)ctx;
    for(; handles[h].pos < (int)ntree; handles[h].pos++) {
        p = tree[handles[h].pos].path;
        if(dl == 1) dl = 0;                     /* the root's children: "/x" */
        if(strncmp(p, dp, dl) || p[dl] != '/' || !p[dl + 1] || strchr(p + dl + 1, '/')) continue;
        snprintf(name, size, "%s", p + dl + 1);
        *dir = tree[handles[h].pos].dir;
        *bytes = tree[handles[h].pos].len;
        handles[h].pos++;
        return 1;
    }
    return 0;
}

static void f_closedir(void *ctx, int h) {
    (void)ctx;
    handles[h].used = 0;
    open_dirs--;
}

static int f_open(void *ctx, const char *path) {
    int n = node_of(path);
    (void)ctx;
    if(n < 0 || tree[n].dir) return -1;
    if(!strncmp(path, "/dev", 4)) abort();      /* never */
    if(++open_files > most_files) most_files = open_files;
    return handle(n);
}

static int f_read(void *ctx, int h, char *buf, unsigned n) {
    struct fnode *f = &tree[handles[h].node];
    unsigned left = f->len - (unsigned)handles[h].pos;
    (void)ctx;
    if(reads_fail) return -1;
    if(n > left) n = left;
    if(n > 5000) n = 5000;                      /* short reads, as a file system may give */
    memcpy(buf, f->data + handles[h].pos, n);
    handles[h].pos += (int)n;
    return (int)n;
}

static void f_close(void *ctx, int h) {
    (void)ctx;
    handles[h].used = 0;
    open_files--;
}

static const struct wc_fsops fake = { NULL, f_opendir, f_readdir, f_closedir, f_open, f_read, f_close };

static void find_in_files(void) {
    static char longline[200000], bin[100], many[60000];
    struct wc_grep g;
    unsigned k, n;
    for(k = 0; k < sizeof(longline); k++) longline[k] = 'a' + (char)(k % 7);
    memcpy(longline + 100, "needle", 6);
    memcpy(longline + 150000, "needle", 6);
    longline[sizeof(longline) - 2] = '\n';
    memcpy(longline + sizeof(longline) - 1, "n", 1);
    memcpy(bin, "needle\0", 7);
    for(n = 0, k = 0; k < 3000; k++) n += (unsigned)snprintf(many + n, sizeof(many) - n, "needle %u\n", k);
    ntree = 0;
    tree_add("/", NULL, 0, 1);
    tree_add("/w", NULL, 0, 1);
    tree_add("/w/a.c", "int needle;\r\nx\n\tneedle(); // NEEDLE\n", 0, 0);
    tree_add("/w/sub", NULL, 0, 1);
    tree_add("/w/sub/b.h", "#define NEEDLES 1\nneedle", 0, 0);
    tree_add("/w/sub/deep", NULL, 0, 1);
    tree_add("/w/sub/deep/c.txt", "nothing here\n", 0, 0);
    tree_add("/w/b.bin", bin, sizeof(bin), 0);
    tree_add("/w/long.txt", longline, sizeof(longline), 0);
    tree_add("/w/z.c", "needle\n", 0, 0);
    tree_add("/w/sub2", NULL, 0, 1);
    tree_add("/w/sub2/d.c", "a needle\n", 0, 0);
    tree_add("/dev", NULL, 0, 1);
    tree_add("/dev/kmsg", "needle", 0, 0);
    tree_add("/many.log", many, n, 0);
    wc_grep_init(&g);
    strcpy(g.f.what, "needle");
    CHECK(wc_grep_start(&g, &fake, "/w") == 0);
    while(wc_grep_step(&g, 1000));
    CHECK(g.state == WC_GREP_DONE && !g.why[0] && most_dirs == 1 && most_files == 1 && !open_dirs && !open_files);
    CHECK(g.nhits == 6 && g.binary == 1);
    if(g.nhits != 6 || g.binary != 1) {
        printf("  grep: %u hits, %u binary, %u files, %u skipped, why [%s]\n", g.nhits, g.binary, g.files, g.skipped, g.why);
        for(k = 0; k < g.nhits; k++) printf("  %s:%u:%u [%s]\n", wc_grep_path(&g, &g.hits[k]), g.hits[k].line, g.hits[k].col, g.hits[k].text);
    }
    if(g.nhits == 6) {
        CHECK(!strcmp(wc_grep_path(&g, &g.hits[0]), "/w/a.c") && g.hits[0].line == 1 && g.hits[0].col == 4);
        CHECK(!strcmp(g.hits[0].text, "int needle;") && g.hits[0].a == 4 && g.hits[0].b == 10);
        CHECK(g.hits[1].line == 3 && !strcmp(g.hits[1].text, "\tneedle(); // NEEDLE"));
        CHECK(!strcmp(wc_grep_path(&g, &g.hits[2]), "/w/long.txt") && g.hits[2].line == 1 && g.hits[2].col == 100);
        CHECK(!strcmp(wc_grep_path(&g, &g.hits[3]), "/w/z.c"));
        CHECK(!strcmp(wc_grep_path(&g, &g.hits[4]), "/w/sub/b.h") && g.hits[4].line == 2);  /* not NEEDLES: the case */
        CHECK(!strcmp(g.hits[5].text, "a needle") && !strcmp(wc_grep_path(&g, &g.hits[5]), "/w/sub2/d.c"));
    }
    /* a filter, a whole word in either case, from / (never /dev) */
    strcpy(g.filter, "*.C, *.h");
    g.f.icase = g.f.word = 1;
    most_dirs = most_files = 0;
    CHECK(wc_grep_start(&g, &fake, "/") == 0);
    while(wc_grep_step(&g, 333));
    CHECK(g.nhits == 5 && most_dirs == 1 && most_files == 1);
    CHECK(wc_grep_start(&g, &fake, "/dev") < 0 && strstr(g.why, "/dev"));
    /* the matches listed, at most */
    g.filter[0] = 0;
    g.f.icase = g.f.word = 0;
    CHECK(wc_grep_start(&g, &fake, "/") == 0);
    while(wc_grep_step(&g, 100000));
    CHECK(g.nhits == WC_GREP_HITS && strstr(g.why, "first") && !open_dirs && !open_files);
    /* stopped, and a read that fails */
    CHECK(wc_grep_start(&g, &fake, "/w") == 0);
    wc_grep_step(&g, 10);
    wc_grep_stop(&g);
    CHECK(g.state == WC_GREP_DONE && !strcmp(g.why, "Stopped") && !open_dirs && !open_files);
    reads_fail = 1;
    CHECK(wc_grep_start(&g, &fake, "/w") == 0);
    while(wc_grep_step(&g, 1000));
    CHECK(g.nhits == 0 && g.skipped == 7 && !open_files);
    reads_fail = 0;
    strcpy(g.f.what, "[");
    g.f.regex = 1;
    CHECK(wc_grep_start(&g, &fake, "/w") < 0 && strstr(g.why, "Not a pattern"));
    wc_grep_free(&g);
    CHECK(wc_grep_name_ok("", "x") && wc_grep_name_ok("*.c", "A.C") && !wc_grep_name_ok("*.c", "a.h"));
    CHECK(wc_grep_name_ok("a*b*c", "aXXbYYc") && !wc_grep_name_ok("a*b*c", "aXXbYY") && wc_grep_name_ok("?.md", "x.md"));
    CHECK(wc_grep_name_ok("Make*  *.mk", "Makefile") && !wc_grep_name_ok("Make*  *.mk", "x.c"));
}

/**** the view ****/

static void viewing(void) {
    struct wc_doc d;
    struct wc_view v;
    struct wc_cell cells[4 * 10];
    unsigned lines, row, col;
    char st[160];
    wc_view_init(&v);
    doc_at(&d, "v.c", "a\tb(\xC3\xA9)\n^x\xFF|\n");
    wc_view_render(&d, &v, cells, 4, 10, &lines);
    CHECK(lines == 3);
    CHECK(cells[0].cp == 'a' && cells[1].cp == ' ' && cells[7].cp == ' ' && cells[8].cp == 'b' && cells[9].cp == '(');
    CHECK(cells[10].cp == 'x' && cells[10].bg == WC_BG_SEL && cells[11].cp == (WC_BAD | 0xFF) && cells[11].bg == WC_BG_SEL);
    CHECK(cells[12].bg == WC_BG_NONE && cells[20].cp == ' ' && cells[30].cp == 0);
    d.left = 9;
    wc_doc_select(&d, 5, 5);                    /* after the opening bracket: it and its partner */
    wc_view_render(&d, &v, cells, 4, 10, &lines);
    CHECK(cells[0].cp == '(' && cells[0].bg == WC_BG_BRACKET && cells[1].cp == 0xE9 && cells[2].bg == WC_BG_BRACKET);
    CHECK(wc_view_hit(&d, 0, 2) == 6 && wc_view_hit(&d, 1, 0) == 10 && wc_view_hit(&d, 3, 0) == d.t.len);
    d.left = 0;
    CHECK(wc_view_hit(&d, 0, 3) == 1 && wc_view_hit(&d, 0, 5) == 2 && wc_view_hit(&d, 0, 99) == 7);
    wc_doc_move(&d, WC_DOC_END, 0, 1);
    wc_view_follow(&d, 2, 10);
    CHECK(d.top == 1 && wc_view_caret(&d, 2, 10, &row, &col) && row == 1 && col == 0);
    wc_view_scroll(&d, -5, 0, 2);
    CHECK(d.top == 0 && !wc_view_caret(&d, 2, 10, &row, &col));
    wc_doc_select(&d, 0, 9);
    wc_view_status(&d, st, sizeof(st));
    CHECK(!strcmp(st, "Ln 2, Col 2  9 selected (2 lines)  C  LF  Spaces: 4"));
    CHECK(wc_view_digits(&d) == 1 && wc_view_width(&d, 3) == 12);
    wc_doc_free(&d);
}


/**** the application: tabs, keys, the guard, with files in memory ****/

struct mfile { char path[64]; char data[256]; };
static struct mfile disk[16];
static int ndisk, saves, save_fails, disk_changes;

static struct mfile *mfind(const char *path) {
    int i;
    for(i = 0; i < ndisk; i++)
        if(!strcmp(disk[i].path, path)) return &disk[i];
    return NULL;
}

static void mput(const char *path, const char *data) {
    struct mfile *f = mfind(path);
    if(!f) f = &disk[ndisk++];
    snprintf(f->path, sizeof(f->path), "%s", path);
    snprintf(f->data, sizeof(f->data), "%s", data);
}

static int m_read(struct wc_doc *d, const char *path) {
    struct mfile *f = mfind(path);
    unsigned n;
    char *p;
    if(!f) {
        snprintf(d->msg, sizeof(d->msg), "Cannot open %s", path);
        return -1;
    }
    n = (unsigned)strlen(f->data);
    p = wc_text_load_begin(&d->t, n);
    memcpy(p, f->data, n);
    wc_text_load_end(&d->t, n);
    return wc_doc_loaded(d, path) == 0 ? 0 : -1;
}

static int m_save(struct wc_doc *d, const char *to) {
    char *b;
    saves++;
    if(disk_changes && !to && !d->overwrite) {  /* 11/09/26: as fileio.c says a file another program saved since */
        disk_changes--;
        d->disk_changed = 1;
        snprintf(d->msg, sizeof(d->msg), "Not saved: it changed on the disk since it was read here");
        return -1;
    }
    d->overwrite = 0;
    if(save_fails) {
        save_fails--;
        snprintf(d->msg, sizeof(d->msg), "Not saved: I/O error");
        return -1;
    }
    b = text_of(&d->t);
    mput(to ? to : d->path, b);
    free(b);
    if(to) snprintf(d->path, sizeof(d->path), "%s", to);
    if(to && d->lang == WC_LANG_TEXT) wc_doc_set_lang(d, wc_lang_guess(to, NULL, 0));
    wc_doc_saved(d);
    snprintf(d->msg, sizeof(d->msg), "Saved");
    return 0;
}

static const struct wc_app_io mio = { m_read, m_save };
#define CTRL(c) ((c) & 0x1f)

static void keys(struct wc_app *a, const char *s) {
    for(; *s; s++) wc_app_key(a, (unsigned char)*s, 0);
}

static int app_text_is(struct wc_app *a, const char *want) {
    return text_is(wc_app_doc(a), want);
}

static void application(void) {
    struct wc_app a;
    char b[256];
    unsigned i;
    int r;
    ndisk = saves = save_fails = 0;
    mput("/m/x.py", "def f():\n    return 1\n");
    mput("/m/y.c", "int y;\n");
    CHECK(wc_app_init(&a, &mio, &fake) == 0 && a.n == 1);
    wc_app_size(&a, 10, 40);
    wc_app_title(&a, b, sizeof(b));
    CHECK(!strcmp(b, "Code Editor - Untitled 1"));
    keys(&a, "hi");
    wc_app_title(&a, b, sizeof(b));
    CHECK(app_text_is(&a, "hi") && !strcmp(b, "Code Editor - Untitled 1 *"));
    CHECK(wc_app_key(&a, CTRL('s'), WC_MOD_CTRL) == WC_DO_SAVE_AS);  /* never saved: its name first */
    CHECK(wc_app_save_as(&a, "/m/hi.c") & WC_DO_TITLE);
    wc_app_tab_label(&a, 0, b, sizeof(b));
    CHECK(!strcmp(b, "hi.c") && wc_app_doc(&a)->lang == WC_LANG_C && mfind("/m/hi.c"));
    CHECK(wc_app_key(&a, CTRL('n'), WC_MOD_CTRL) & WC_DO_TITLE && a.n == 2 && a.cur == 1);
    CHECK(wc_app_key(&a, CTRL('o'), WC_MOD_CTRL) == WC_DO_OPEN);
    wc_app_open(&a, "/m/x.py");                 /* in place of the untouched new file */
    CHECK(a.n == 2 && a.cur == 1 && wc_app_doc(&a)->lang == WC_LANG_PY);
    wc_app_open(&a, "/m/hi.c");                 /* open already: to its tab */
    CHECK(a.n == 2 && a.cur == 0);
    wc_app_open(&a, "/m/none.c");
    wc_app_status(&a, b, sizeof(b));
    CHECK(a.n == 2 && !strcmp(b, "Cannot open /m/none.c"));
    for(i = 0; i < 7; i++) {
        snprintf(b, sizeof(b), "/m/f%u.c", i);
        mput(b, "x\n");
        wc_app_open(&a, b);
    }
    wc_app_status(&a, b, sizeof(b));
    CHECK(a.n == WC_TABS && strstr(b, "close one first"));
    CHECK(wc_app_key(&a, CTRL('w'), WC_MOD_CTRL) & WC_DO_TITLE && a.n == WC_TABS - 1);     /* nothing unsaved: it goes */
    /* the guard on a tab */
    wc_app_cmd(&a, WC_CMD_TAB + 1);
    keys(&a, "#");
    CHECK(wc_app_key(&a, CTRL('w'), WC_MOD_CTRL) & WC_DO_ASK);
    CHECK(wc_app_asking(&a, b, sizeof(b)) == 1 && !strcmp(b, "Save the changes to x.py?"));
    wc_app_answer(&a, WC_ANSWER_CANCEL);
    CHECK(a.n == WC_TABS - 1 && a.asking < 0 && !wc_doc_safe(a.docs[1]));
    wc_app_key(&a, CTRL('w'), WC_MOD_CTRL);
    wc_app_answer(&a, WC_ANSWER_DISCARD);
    CHECK(a.n == WC_TABS - 2 && strcmp(a.docs[1]->path, "/m/x.py") && !strcmp(mfind("/m/x.py")->data, "def f():\n    return 1\n"));
    /* the guard on quitting: one file at a time; a Cancel loses nothing */
    wc_app_cmd(&a, WC_CMD_TAB + 0);
    keys(&a, "A");
    wc_app_cmd(&a, WC_CMD_TAB + 2);
    keys(&a, "B");
    CHECK(wc_app_unsafe(&a) == 2);
    CHECK(wc_app_key(&a, CTRL('q'), WC_MOD_CTRL) & WC_DO_ASK && a.asking == 0 && a.cur == 0);
    CHECK(wc_app_answer(&a, WC_ANSWER_DISCARD) & WC_DO_ASK && a.asking == 2 && a.cur == 2);
    CHECK(!(wc_app_answer(&a, WC_ANSWER_CANCEL) & WC_DO_QUIT) && a.asking < 0 && wc_app_unsafe(&a) == 2);
    wc_app_cmd(&a, WC_CMD_QUIT);
    CHECK(a.asking == 0);                       /* asked again, from the first */
    saves = 0;
    CHECK(wc_app_answer(&a, WC_ANSWER_SAVE) & WC_DO_ASK && saves == 1 && !strcmp(mfind("/m/hi.c")->data, "hiA"));
    save_fails = 1;
    r = wc_app_answer(&a, WC_ANSWER_SAVE);      /* a save that fails ends the quit, and says why */
    wc_app_status(&a, b, sizeof(b));
    CHECK(!(r & WC_DO_QUIT) && a.asking < 0 && !strcmp(b, "Not saved: I/O error"));
    wc_app_cmd(&a, WC_CMD_QUIT);
    CHECK(wc_app_answer(&a, WC_ANSWER_SAVE) == WC_DO_QUIT);
    /* a new file asks its name first; a cancelled chooser ends the quit */
    wc_app_cmd(&a, WC_CMD_NEW);
    keys(&a, "new");
    wc_app_cmd(&a, WC_CMD_QUIT);
    CHECK(wc_app_answer(&a, WC_ANSWER_SAVE) == WC_DO_SAVE_AS);
    CHECK(!(wc_app_save_as(&a, NULL) & WC_DO_QUIT) && a.asking < 0);
    wc_app_cmd(&a, WC_CMD_QUIT);
    wc_app_answer(&a, WC_ANSWER_SAVE);
    CHECK(wc_app_save_as(&a, "/m/new.txt") == WC_DO_QUIT && !strcmp(mfind("/m/new.txt")->data, "new"));
    wc_app_free(&a);

    /* keys */
    CHECK(wc_app_init(&a, &mio, &fake) == 0);
    wc_app_size(&a, 5, 20);
    keys(&a, "if(x) {\r");
    CHECK(app_text_is(&a, "if(x) {\n"));           /* plain text: no brace rules */
    wc_app_key(&a, 0xE2, 0);                    /* ≋ typed a byte at a time: one character */
    wc_app_key(&a, 0x89, 0);
    CHECK(app_text_is(&a, "if(x) {\n"));
    wc_app_key(&a, 0x8B, 0);
    CHECK(app_text_is(&a, "if(x) {\n\xE2\x89\x8B"));
    wc_app_key(&a, WC_KEY_LEFT, WC_MOD_SHIFT);
    CHECK(wc_app_key(&a, CTRL('c'), WC_MOD_CTRL) == WC_DO_COPY && wc_app_clip(&a, b, sizeof(b)) == 3 && !strcmp(b, "\xE2\x89\x8B"));
    wc_app_copied(&a, 1);
    CHECK(app_text_is(&a, "if(x) {\n\xE2\x89\x8B"));
    wc_app_key(&a, CTRL('x'), WC_MOD_CTRL);
    wc_app_copied(&a, 0);                       /* the clipboard would not take it: nothing is cut */
    wc_app_status(&a, b, sizeof(b));
    CHECK(app_text_is(&a, "if(x) {\n\xE2\x89\x8B") && strstr(b, "focus"));
    CHECK(wc_app_key(&a, CTRL('x'), 0) == 0);   /* a control byte without Ctrl's bit is no command */
    wc_app_key(&a, CTRL('x'), WC_MOD_CTRL);
    wc_app_copied(&a, 1);
    CHECK(app_text_is(&a, "if(x) {\n"));
    CHECK(wc_app_key(&a, CTRL('v'), WC_MOD_CTRL) == WC_DO_PASTE);
    wc_app_paste(&a, "a\nb", 3);
    CHECK(app_text_is(&a, "if(x) {\na\nb"));
    wc_app_key(&a, CTRL('z'), WC_MOD_CTRL);
    CHECK(app_text_is(&a, "if(x) {\n"));
    wc_app_key(&a, CTRL('z'), WC_MOD_CTRL | WC_MOD_SHIFT);
    CHECK(app_text_is(&a, "if(x) {\na\nb"));
    CHECK(wc_app_key(&a, CTRL('l'), WC_MOD_CTRL) == WC_DO_GOTO);
    wc_app_goto(&a, "1:4");
    CHECK(wc_app_doc(&a)->caret == 3);
    wc_app_goto(&a, "1:x");
    wc_app_status(&a, b, sizeof(b));
    CHECK(strstr(b, "Not a line number") && wc_app_doc(&a)->caret == 3);
    wc_app_key(&a, WC_KEY_RIGHT, WC_MOD_SHIFT);
    CHECK(wc_app_key(&a, CTRL('f'), WC_MOD_CTRL) & WC_DO_FIND && !strcmp(a.find.what, "x"));
    CHECK((wc_app_key(&a, WC_KEY_F(5), 0) & WC_DO_DRAW) && wc_app_doc(&a)->caret == 4);
    wc_app_cmd(&a, WC_CMD_LANG + WC_LANG_C);
    wc_app_key(&a, CTRL('b'), WC_MOD_CTRL);               /* to the brace's match: there is none */
    wc_app_key(&a, WC_KEY_HOME, WC_MOD_CTRL);
    wc_app_key(&a, 0x1F, WC_MOD_CTRL);          /* Ctrl+/ */
    CHECK(app_text_is(&a, "// if(x) {\na\nb"));
    wc_app_key(&a, '/', WC_MOD_CTRL);
    CHECK(app_text_is(&a, "if(x) {\na\nb"));
    wc_app_key(&a, WC_KEY_END, WC_MOD_CTRL);
    wc_app_key(&a, WC_KEY_BACKSPACE, WC_MOD_CTRL);  /* Ctrl+Backspace: the word */
    CHECK(app_text_is(&a, "if(x) {\na\n"));
    wc_app_key(&a, 'z', 8);                     /* a key held, as the keyboard repeats it (WIN_MOD_REPEAT): typed */
    wc_app_key(&a, 'z', 8 | 0x80);              /* a modifier bit nobody uses yet changes nothing */
    CHECK(app_text_is(&a, "if(x) {\na\nzz"));
    wc_app_key(&a, WC_KEY_BACKSPACE, WC_MOD_CTRL | 8);    /* Ctrl+Backspace repeated: the word, as without */
    CHECK(app_text_is(&a, "if(x) {\na\n"));
    CHECK(wc_app_key(&a, CTRL('l'), WC_MOD_CTRL | 8) == WC_DO_GOTO);  /* a command's keys held: each repeat is it */
    wc_app_key(&a, 0x1B, 0);
    wc_app_cmd(&a, WC_CMD_NEW);
    CHECK(a.cur == 1 && (wc_app_key(&a, WC_KEY_TAB, WC_MOD_CTRL) & WC_DO_TITLE) && a.cur == 0);
    CHECK(wc_app_key(&a, WC_KEY_PGDN, WC_MOD_CTRL) && a.cur == 1 && wc_app_key(&a, WC_KEY_PGUP, WC_MOD_CTRL) && a.cur == 0);
    /* the pointer: a click, a drag past the bottom, a double and a triple click, the line numbers */
    wc_app_free(&a);
    CHECK(wc_app_init(&a, &mio, &fake) == 0);
    mput("/m/long.c", "l0\nl1 word\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\n");
    wc_app_open(&a, "/m/long.c");
    wc_app_size(&a, 3, 10);
    wc_app_press(&a, 1, 1, 1, 0);
    CHECK(wc_app_doc(&a)->caret == 4);
    wc_app_drag(&a, 5, 0);                      /* below the area: the text scrolls a line */
    CHECK(wc_app_doc(&a)->top == 1 && wc_app_doc(&a)->anchor == 4 && wc_app_doc(&a)->caret == 14);
    wc_app_press(&a, 0, 4, 2, 0);
    CHECK(wc_app_doc(&a)->anchor == 6 && wc_app_doc(&a)->caret == 10);   /* "word" */
    wc_app_press(&a, 1, 0, 3, 0);
    CHECK(wc_app_doc(&a)->anchor == 11 && wc_app_doc(&a)->caret == 14);
    wc_app_press(&a, 0, -1, 1, 0);
    wc_app_drag(&a, 2, -1);
    CHECK(wc_app_doc(&a)->anchor == 3 && wc_app_doc(&a)->caret == 17);
    wc_app_wheel(&a, 3);
    CHECK(wc_app_doc(&a)->top == 4);
    wc_app_key(&a, WC_KEY_DOWN, WC_MOD_CTRL);
    CHECK(wc_app_doc(&a)->top == 5);
    /* find in files, and a result opened at its match */
    mput("/w/a.c", "int needle;\r\nx\n\tneedle(); // NEEDLE\n");
    strcpy(a.find.what, "needle");
    CHECK(wc_app_cmd(&a, WC_CMD_FIND_FILES) & WC_DO_FILES && !strcmp(a.grep.f.what, "needle"));
    CHECK(wc_app_files(&a, "/w") & WC_DO_FILES);
    for(i = 0; i < 100 && (wc_app_idle(&a) & WC_DO_IDLE); i++);
    CHECK(a.grep.state == WC_GREP_DONE && a.grep.nhits == 6);
    wc_app_hit(&a, 1);
    CHECK(!strcmp(wc_app_doc(&a)->path, "/w/a.c") && wc_app_doc(&a)->anchor == 16 && wc_app_doc(&a)->caret == 22);
    wc_app_hit(&a, 2);                          /* long.txt is not on this "disk": said, and nothing else */
    wc_app_status(&a, b, sizeof(b));
    CHECK(strstr(b, "Cannot open /w/long.txt") && !strcmp(wc_app_doc(&a)->path, "/w/a.c"));
    wc_app_free(&a);
    CHECK(pool.used == 0);
}

/**** a 16 MB file, timed ****/

static double now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void timed(const char *what, double t0, double limit) {
    double s = now() - t0;
    printf("  %-48s %8.3f s\n", what, s);
    CHECK(s < limit);
}

static void speed(void) {
    static struct wc_cell cells[60 * 120];
    struct wc_doc d;
    struct wc_view v;
    struct wc_find f;
    char *p;
    unsigned n = 0, lines, k;
    double t0;
    int exact;
    wc_view_init(&v);
    if(wc_doc_init(&d, &pool) < 0) abort();
    p = wc_text_load_begin(&d.t, WC_TEXT_MAX);
    t0 = now();
    while(n + 64 < WC_TEXT_MAX - (1u << 20)) n += (unsigned)snprintf(p + n, WC_TEXT_MAX - n, "    x%u = f(\"s\", %u); // d\n", n, n % 977);
    wc_text_load_end(&d.t, n);
    CHECK(wc_doc_loaded(&d, "big.c") == 0);
    printf("speed: %u bytes, %u lines\n", d.t.len, d.t.nl);
    timed("read in (line ends, indentation, language)", t0, 2);
    t0 = now();
    wc_doc_move(&d, WC_DOC_END, 0, 50);
    wc_view_follow(&d, 50, 120);
    timed("Ctrl+End: the last line's number", t0, 2);
    t0 = now();
    wc_view_render(&d, &v, cells, 50, 120, &lines);
    timed("a screenful there, within the budget", t0, 1);
    t0 = now();
    while(wc_syn_idle(&d.t, d.lang, 1u << 20));
    timed("the colouring's states, all of them, idle", t0, 5);
    t0 = now();
    wc_doc_move(&d, WC_DOC_START, 0, 50);
    for(k = 0; k < 200; k++) wc_doc_type(&d, "y", 1);
    wc_view_follow(&d, 50, 120);
    wc_view_render(&d, &v, cells, 50, 120, &lines);
    timed("200 keys at the top, a screenful", t0, 1);
    t0 = now();
    d.top = d.t.nl - 10;
    wc_view_render(&d, &v, cells, 50, 120, &lines);
    CHECK(v.exact);
    timed("then a screenful at the end (states stand)", t0, 1);
    t0 = now();
    wc_doc_type(&d, "/*", 2);
    wc_view_render(&d, &v, cells, 50, 120, &lines);
    timed("a comment opened at the top, a screenful at the end", t0, 1);
    t0 = now();
    wc_syn_state(&d.t, d.lang, d.t.nl, ~0UL, &exact);
    timed("its states to the end, all at once", t0, 5);
    wc_doc_undo(&d, 0);
    wc_find_init(&f);
    strcpy(f.what, "nowhere");
    t0 = now();
    wc_doc_select(&d, 0, 0);
    CHECK(wc_find_doc(&f, &d, 0) == 0);
    timed("find: no match in 16 MB", t0, 3);
    f.regex = 1;
    strcpy(f.what, "f\\(\"t\"");
    t0 = now();
    CHECK(wc_find_doc(&f, &d, 0) == 0);
    timed("find: a regular expression, no match in 16 MB", t0, 10);
    f.regex = 0;
    strcpy(f.what, "// d");
    strcpy(f.with, "// done, and a comment much longer");
    t0 = now();
    CHECK(wc_find_replace_all(&f, &d) < 0 && strstr(d.msg, "more than 16 MB") && wc_undo_can(&d.u, 0) == 0);
    timed("replace all, refused: the file would pass 16 MB", t0, 5);
    strcpy(f.with, "// D");
    t0 = now();
    k = (unsigned)wc_find_replace_all(&f, &d);
    CHECK(k == d.t.nl);
    timed("replace all, every line", t0, 5);
    printf("  (%u replaced, history %s)\n", k, wc_undo_can(&d.u, 0) ? "kept" : "forgotten");
    f.regex = 1;
    strcpy(f.what, "x(1[0-9]*1) =");
    strcpy(f.with, "y\\1 =");
    t0 = now();
    k = (unsigned)wc_find_replace_all(&f, &d);
    timed("replace all, a regular expression with a group", t0, 10);
    printf("  (%u replaced, history %s)\n", k, wc_undo_can(&d.u, 0) ? "kept" : "forgotten");
    t0 = now();
    wc_doc_undo(&d, 0);
    timed("undo", t0, 3);
    t0 = now();
    for(k = 0; k < 2000; k++) wc_doc_move(&d, WC_DOWN, 0, 50);
    for(k = 0; k < 100; k++) wc_doc_move(&d, WC_PGDN, 0, 50);
    timed("2000 Downs and 100 Page Downs", t0, 1);
    wc_find_free(&f);
    wc_doc_free(&d);
    /* one line of 16 MB */
    if(wc_doc_init(&d, &pool) < 0) abort();
    p = wc_text_load_begin(&d.t, WC_TEXT_MAX);
    for(k = 0; k < WC_TEXT_MAX; k++) p[k] = "{\"k\": [1, 2], \"v\": \"x\"}, "[k % 26];
    wc_text_load_end(&d.t, WC_TEXT_MAX);
    wc_doc_loaded(&d, "one.json");
    t0 = now();
    wc_doc_move(&d, WC_DOC_END, 0, 50);
    wc_view_follow(&d, 50, 120);
    wc_view_render(&d, &v, cells, 50, 120, &lines);
    timed("one 16 MB line: to its end, a screenful", t0, 3);
    t0 = now();
    for(k = 0; k < 20; k++) wc_doc_move(&d, WC_LEFT, 0, 50);
    wc_view_follow(&d, 50, 120);
    timed("20 Lefts at its end", t0, 3);
    wc_doc_free(&d);
}


/**** 11/09/26: the review's findings (docs/superpowers/plans/2026-11-05-desktop-apps-code.md, "The review") ****/

/* a line of unit over and over, n bytes */
static char *repeat(const char *unit, unsigned n) {
    char *b = malloc(n + 1);
    unsigned k, u = (unsigned)strlen(unit);
    for(k = 0; k < n; k++) b[k] = unit[k % u];
    b[n] = 0;
    return b;
}

static void review_changes(void) {
    struct wc_doc d;
    struct wc_find f;
    char *b, *big;
    /* C1: Replace all with no room for what it comes to: nothing changed, and said (it cut each line first) */
    b = repeat("aaaa\n", 5000);
    doc_str(&d, "a.txt", b);
    pool.limit = pool.used;
    wc_find_init(&f);
    strcpy(f.what, "a");
    strcpy(f.with, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    CHECK(wc_find_replace_all(&f, &d) == -1 && text_is(&d, b) && !wc_undo_can(&d.u, 0) &&
          !strcmp(d.msg, "Not replaced: there is not enough memory"));
    pool.limit = WC_POOL_MAX;
    wc_doc_free(&d);
    free(b);
    /* ... with room, larger than the history: done, and said that it cannot be undone */
    big = repeat("x", 4u << 20);
    doc_str(&d, "a.json", big);
    strcpy(f.what, "x");
    strcpy(f.with, "yy");
    CHECK(wc_find_replace_all(&f, &d) == (int)(4u << 20) && d.t.len == 8u << 20 && d.u.forgot &&
          strstr(d.msg, "Replaced 4194304 (on 1 line)") && strstr(d.msg, "too large a change to undo"));
    wc_doc_free(&d);
    free(big);
    /* M1: a paste and Replace over a selection with no room: the selection stays, and it is said */
    doc_str(&d, "b.txt", "keep SELECTED keep\n");
    pool.limit = pool.used;
    big = repeat("p", 100000);
    wc_doc_select(&d, 5, 13);
    CHECK(wc_doc_paste(&d, big, 100000) < 0 && text_is(&d, "keep SELECTED keep\n") && !wc_doc_modified(&d) &&
          !strcmp(d.msg, "Not changed: there is not enough memory"));
    CHECK(wc_doc_replace(&d, 5, 13, big, 100000, 0) < 0 && text_is(&d, "keep SELECTED keep\n"));
    pool.limit = WC_POOL_MAX;
    wc_doc_free(&d);
    /* ... and one that fits only once the selection is cut: done (the old order asked the room first) */
    b = repeat("q", 70000);
    doc_str(&d, "c.txt", b);
    pool.limit = pool.used;
    wc_doc_select(&d, 0, 70000);
    CHECK(wc_doc_paste(&d, big, 100000) == 0 && d.t.len == 100000 && wc_text_byte(&d.t, 0) == 'p');
    wc_doc_undo(&d, 0);
    CHECK(text_is(&d, b));
    pool.limit = WC_POOL_MAX;
    wc_doc_free(&d);
    free(b);
    free(big);
    /* ... at 16 MB: a paste over foo fits once foo is cut; Replace refused, and its reason stays said (the search
       after it said "Found again from the top") */
    big = repeat("a", WC_TEXT_MAX - 4);
    memcpy(big, "int foo;\n", 9);
    doc_str(&d, "big.c", big);
    free(big);
    wc_doc_select(&d, 4, 7);
    CHECK(wc_doc_paste(&d, "foobar", 6) == 0 && d.t.len == WC_TEXT_MAX - 1);
    wc_doc_undo(&d, 0);
    wc_find_init(&f);
    strcpy(f.what, "foo");
    strcpy(f.with, "foobarbaz");
    wc_doc_select(&d, 4, 7);
    CHECK(wc_find_replace(&f, &d) == 0 && strstr(d.msg, "16 MB") && wc_text_byte(&d.t, 4) == 'f' &&
          d.t.len == WC_TEXT_MAX - 4);
    wc_doc_free(&d);
    wc_find_free(&f);
    /* L6: the history's ops array refused after its bytes' block grew: forgotten, its count back to nothing */
    doc_str(&d, "u.txt", "");
    realloc_fails_at = 1;                       /* (the bytes' block first, then the ops') */
    wc_doc_type(&d, "a", 1);
    realloc_fails_at = -1;
    CHECK(text_is(&d, "a") && d.u.forgot && d.u.bytes == 0);
    wc_doc_type(&d, "b", 1);
    CHECK(d.u.bytes > 0 && d.u.bytes < 4096);
    wc_doc_free(&d);
    /* L3: Enter after a lone CR in an LF file: a CR LF, so the CR stays lone; Backspace takes only what Enter put */
    doc_at(&d, "r.c", "a\rb\r\rc\n\r|");
    wc_doc_enter(&d);
    CHECK(text_is(&d, "a\rb\r\rc\n\r\r\n"));
    wc_doc_backspace(&d, 0);
    CHECK(text_is(&d, "a\rb\r\rc\n\r"));
    wc_doc_select(&d, 2, 2);
    wc_doc_paste(&d, "\nz", 2);
    CHECK(text_is(&d, "a\r\r\nzb\r\rc\n\r"));
    wc_doc_free(&d);
}

static void review_lexing(void) {
    static const char *const units[] = { "[", "![", "*a ", "_a ", "`", "``a", "<a", "\"", "'", "/*", "*/", "#", "$(",
                                         "${", "<<E\n", "\\", "{", "(", "]", ")", "<!--", "'''", "\"\"\"", ";", "%" };
    static unsigned char cls[WC_LEX_CLASSES];
    char *b;
    unsigned u, lang, st, st1;
    double t0, worst = 0, t;
    /* C2: every tokenizer, hostile lines of 64 KB and of 1 MB: a line's first WC_LEX_MAX bytes are read, as if it ended
       there, so its state is theirs, and the time is a short line's */
    for(u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
        b = repeat(units[u], 1u << 20);
        for(lang = 0; lang < WC_LANGS; lang++) {
            t0 = now();
            st = wc_lex((int)lang, b, 1u << 20, 0, cls, sizeof(cls));
            st1 = wc_lex((int)lang, b, 64u << 10, 0, NULL, 0);
            t = now() - t0;
            if(t > worst) worst = t;
            CHECK(st == wc_lex((int)lang, b, WC_LEX_MAX, 0, NULL, 0) && st1 == st);
        }
        free(b);
    }
    CHECK(worst < 2.5);                         /* (microseconds on the Mac; the quadratic Markdown took 140 s) */
    if(worst >= 2.5) printf("  the slowest tokenizing of a hostile line: %.3f s\n", worst);
    /* the Markdown scans themselves are linear: lines of WC_LEX_MAX [, *a and backticks, 64 times each */
    t = 0;
    for(u = 0; u < 3; u++) {
        b = repeat(u == 0 ? "[" : u == 1 ? "*a " : "` `` ``` ", WC_LEX_MAX);
        t0 = now();
        for(lang = 0; lang < 64; lang++) wc_lex(WC_LANG_MD, b, WC_LEX_MAX, 0, cls, sizeof(cls));
        t += now() - t0;
        free(b);
    }
    CHECK(t < 2.5);
    /* the classes past the first WC_LEX_MAX bytes: plain */
    b = repeat("/* c */ ", 20000);
    wc_lex(WC_LANG_C, b, 20000, 0, cls, sizeof(cls));
    CHECK(cls[0] == WC_C_COMMENT && cls[WC_LEX_CLASSES - 8] == WC_C_COMMENT);
    free(b);
}

static void review_searching(void) {
    struct wc_doc d;
    struct wc_find f, g;
    struct wc_app a;
    char *b;
    unsigned a1, b1, a2, b2, k;
    int w1, w2, r, steps;
    double t0;
    /* C3: Find previous walks a line's matches once; a pattern that matches an empty text is refused */
    b = repeat("{\"key\":\"value\",", 1u << 20);
    doc_str(&d, "x.json", b);
    free(b);
    wc_find_init(&f);
    f.regex = 1;
    strcpy(f.what, "\".*\"");
    wc_doc_select(&d, d.t.len, d.t.len);
    t0 = now();
    CHECK(wc_find_doc(&f, &d, 1) == 1 && d.caret == 1 && d.anchor > d.t.len - 16 && wc_text_byte(&d.t, d.anchor - 1) == '"');
    CHECK(wc_find_doc(&f, &d, 0) == 1);
    strcpy(f.what, ".+");
    wc_doc_select(&d, d.t.len, d.t.len);
    CHECK(wc_find_doc(&f, &d, 1) == 1 && d.caret == 0);
    strcpy(f.what, "(a*b)?");
    CHECK(wc_find_doc(&f, &d, 0) < 0 && strstr(d.msg, "matches nothing at all"));
    CHECK(now() - t0 < 5.0);                    /* (0.02 s on the Mac; room for an emulated 32-bit build under load:
                                                   the searches the review timed would take hours on this line) */
    wc_doc_free(&d);
    /* whole words go on from the next place a word can start */
    doc_str(&d, "w.c", "xfoo foox foo_ foo\n");
    f.regex = 0;
    f.word = 1;
    strcpy(f.what, "foo");
    wc_doc_select(&d, 0, 0);
    CHECK(wc_find_doc(&f, &d, 0) == 1 && d.anchor == 15 && d.caret == 18);
    f.regex = 1;
    strcpy(f.what, "fo+");
    wc_doc_select(&d, 0, 0);
    CHECK(wc_find_doc(&f, &d, 0) == 1 && d.anchor == 15);
    f.word = 0;
    wc_doc_free(&d);
    /* L5: a back-reference that takes too long is said so, not "Not found" */
    b = repeat("a", 400);
    b[399] = 'b';
    doc_str(&d, "e.txt", b);
    free(b);
    strcpy(f.what, "(a*)(a*)(a*)\\1\\2\\3b");
    wc_doc_select(&d, 0, 0);
    CHECK(wc_find_doc(&f, &d, 0) < 0 && strstr(d.msg, "takes too long"));
    wc_doc_free(&d);
    /* in steps: a budget at a time, the same match as all at once, forward and back, round the ends */
    b = repeat("alpha beta gamma\n", 20000 * 17);
    doc_str(&d, "s.txt", b);
    free(b);
    wc_find_init(&g);
    for(k = 0; k < 8; k++) {
        f.regex = g.regex = k & 1;
        strcpy(f.what, k & 1 ? "gam+a" : "beta");
        strcpy(g.what, f.what);
        r = wc_find_next(&f, &d.t, k * 40000 + 7, (k >> 1) & 1, &a1, &b1, &w1);
        CHECK(r == 1 && wc_find_begin(&g, &d.t, k * 40000 + 7, (k >> 1) & 1) == 0);
        for(steps = 0; (r = wc_find_go(&g, &d.t, 997, &a2, &b2, &w2)) == 2; steps++);
        CHECK(r == 1 && a1 == a2 && b1 == b2 && w1 == w2);
    }
    strcpy(g.what, "nowhere");
    g.regex = 0;
    CHECK(wc_find_begin(&g, &d.t, 5, 0) == 0);
    for(steps = 0; (r = wc_find_go(&g, &d.t, 1u << 16, &a2, &b2, &w2)) == 2; steps++);
    CHECK(r == 0 && steps > 3);
    wc_find_free(&g);
    wc_find_free(&f);
    wc_doc_free(&d);
    /* the app: a search in steps between events, which a key stops; Replace all counting, then changing (input
       waits, and Esc takes it back), or too large to undo (it cannot stop) */
    CHECK(wc_app_init(&a, &mio, &fake) == 0);
    a.step = 16u << 10;                         /* (fine steps, so that each phase takes several) */
    b = repeat("needle in a haystack, with hay\n", 256u << 10);
    wc_doc_free(a.docs[0]);
    doc_str(a.docs[0], "/m/h.txt", b);
    strcpy(a.find.what, "nothing like it");
    CHECK((wc_app_cmd(&a, WC_CMD_FIND_NEXT) & WC_DO_IDLE) && a.job == WC_JOB_FIND && strstr(a.status, "Searching"));
    CHECK(wc_app_interrupt(&a, 1) >= 0 && !a.job && !strcmp(wc_app_doc(&a)->msg, "Stopped searching"));
    wc_app_cmd(&a, WC_CMD_FIND_NEXT);
    for(steps = 0; a.job && steps < 1000; steps++) wc_app_idle(&a);
    CHECK(!a.job && strstr(wc_app_doc(&a)->msg, "Not found"));
    strcpy(a.find.what, "hay");
    strcpy(a.find.with, "straw");
    CHECK((wc_app_cmd(&a, WC_CMD_REPLACE_ALL) & WC_DO_IDLE) && a.job == WC_JOB_ALL && !wc_app_busy(&a));
    CHECK(wc_app_interrupt(&a, 1) >= 0 && !a.job && text_is(wc_app_doc(&a), b) &&
          !strcmp(wc_app_doc(&a)->msg, "Stopped: nothing replaced"));
    wc_app_cmd(&a, WC_CMD_REPLACE_ALL);
    for(steps = 0; a.job && !wc_app_busy(&a) && steps < 1000; steps++) wc_app_idle(&a);
    CHECK(wc_app_busy(&a) && wc_app_interrupt(&a, 0) < 0 && wc_app_key(&a, 'z', 0) == (WC_DO_DRAW | WC_DO_IDLE) &&
          (wc_app_cmd(&a, WC_CMD_UNDO) & WC_DO_IDLE) && strstr(a.status, "Replacing"));
    for(steps = 0; wc_app_busy(&a) && a.find.changed < 1000 && steps < 1000; steps++) wc_app_idle(&a);
    CHECK(wc_app_busy(&a) && !wc_app_doc(&a)->u.forgot && wc_app_interrupt(&a, 1) >= 0 && !a.job &&
          text_is(wc_app_doc(&a), b) && !strcmp(wc_app_doc(&a)->msg, "Stopped: nothing replaced"));
    wc_app_cmd(&a, WC_CMD_REPLACE_ALL);
    for(steps = 0; a.job && steps < 1000; steps++) wc_app_idle(&a);
    {
        char head[36];
        wc_text_get(&wc_app_doc(&a)->t, 0, head, 35);
        head[35] = 0;
        CHECK(!a.job && strstr(wc_app_doc(&a)->msg, "Replaced") && !strcmp(head, "needle in a strawstack, with straw\n"));
    }
    wc_app_free(&a);
    free(b);
    b = repeat("hay\n", 4u << 20);             /* too large to undo: Esc cannot stop it once it is forgotten */
    CHECK(wc_app_init(&a, &mio, &fake) == 0);
    wc_doc_free(a.docs[0]);
    doc_str(a.docs[0], "/m/hay.txt", b);
    free(b);
    strcpy(a.find.what, "hay");
    strcpy(a.find.with, "straw");
    wc_app_cmd(&a, WC_CMD_REPLACE_ALL);
    for(steps = 0; a.job && !wc_app_doc(&a)->u.forgot && steps < 1000; steps++) wc_app_idle(&a);
    CHECK(a.job && wc_app_busy(&a) && wc_app_interrupt(&a, 1) < 0 && strstr(a.status, "cannot stop"));
    for(steps = 0; a.job && steps < 1000; steps++) wc_app_idle(&a);
    CHECK(!a.job && wc_app_doc(&a)->t.len == 6u << 20 && strstr(wc_app_doc(&a)->msg, "too large a change to undo"));
    wc_app_free(&a);
}

static void review_files(void) {
    struct wc_app a;
    struct wc_grep g;
    char notes[200];
    /* H1: one file under two spellings, or with . and .., is one tab; Save as onto another tab's file is refused */
    ndisk = 0;
    mput("/m/a.c", "int a;\n");
    mput("/m/b.c", "int b;\n");
    CHECK(wc_app_init(&a, &mio, &fake) == 0);
    strcpy(a.cwd, "/m");
    wc_app_open(&a, "a.c");
    wc_app_open(&a, "/m/A.C");
    wc_app_open(&a, "/m/./x/../a.c");
    CHECK(a.n == 1 && !strcmp(wc_app_doc(&a)->path, "/m/a.c"));
    wc_app_open(&a, "b.c");
    CHECK(a.n == 2 && a.cur == 1);
    wc_app_cmd(&a, WC_CMD_SAVE_AS);
    CHECK((wc_app_save_as(&a, "/m/A.c") & WC_DO_DRAW) && strstr(wc_app_doc(&a)->msg, "open in another tab") &&
          !strcmp(mfind("/m/a.c")->data, "int a;\n"));
    /* ... a save over a file that changed on the disk since asks first: save over it, Save as, or neither */
    keys(&a, "x");
    disk_changes = 1;
    CHECK(wc_app_cmd(&a, WC_CMD_SAVE) == (WC_DO_DISK | WC_DO_DRAW) && wc_app_doc(&a)->disk_changed);
    CHECK((wc_app_disk(&a, WC_DISK_CANCEL) & WC_DO_DRAW) && wc_doc_modified(wc_app_doc(&a)) &&
          strstr(wc_app_doc(&a)->msg, "stays as it is"));
    disk_changes = 1;
    wc_app_cmd(&a, WC_CMD_SAVE);
    CHECK(wc_app_disk(&a, WC_DISK_SAVE_AS) == WC_DO_SAVE_AS);
    disk_changes = 1;
    wc_app_cmd(&a, WC_CMD_SAVE);
    CHECK((wc_app_disk(&a, WC_DISK_OVER) & WC_DO_TITLE) && !wc_doc_modified(wc_app_doc(&a)) &&
          !strcmp(mfind("/m/b.c")->data, "xint b;\n"));
    /* ... and in the guard: its question waits for the disk's, then goes on */
    keys(&a, "y");
    disk_changes = 1;
    wc_app_cmd(&a, WC_CMD_QUIT);
    CHECK(a.asking == 1 && wc_app_answer(&a, WC_ANSWER_SAVE) == (WC_DO_DISK | WC_DO_DRAW) && a.asking == 1);
    CHECK(wc_app_disk(&a, WC_DISK_OVER) == WC_DO_QUIT && !strcmp(mfind("/m/b.c")->data, "xyint b;\n"));
    wc_app_free(&a);
    /* L8: with several files named, a refusal stays said when a later one opens */
    CHECK(wc_app_init(&a, &mio, &fake) == 0);
    wc_app_open(&a, "/m/missing.c");
    wc_app_open(&a, "/m/a.c");
    CHECK(strstr(a.status, "Cannot open /m/missing.c") && a.n == 1);
    /* L7: closing the last tab when there is no memory for the new file in its place: it stays */
    malloc_fails_at = 0;
    wc_app_cmd(&a, WC_CMD_CLOSE);
    malloc_fails_at = -1;
    CHECK(a.n == 1 && !strcmp(wc_app_doc(&a)->path, "/m/a.c") && strstr(a.status, "not enough memory"));
    wc_app_cmd(&a, WC_CMD_CLOSE);
    CHECK(a.n == 1 && !wc_app_doc(&a)->path[0]);
    wc_app_free(&a);
    /* H2, L2: find in files from a folder in its one form, never /dev; the folders it left out said, and why */
    ntree = 0;
    tree_add("/", NULL, 0, 1);
    tree_add("/dev", NULL, 0, 1);
    tree_add("/dev/tty", "needle", 0, 0);
    tree_add("/usb1p1", NULL, 0, 1);
    tree_add("/usb1p1/a.c", "a needle\n", 0, 0);
    tree_add("/locked", NULL, 0, 1);
    wc_grep_init(&g);
    strcpy(g.f.what, "needle");
    CHECK(wc_grep_start(&g, &fake, "/usb1p1/../dev") < 0 && strstr(g.why, "devices are never read"));
    CHECK(wc_grep_start(&g, &fake, ".") < 0 && strstr(g.why, "one from /"));
    CHECK(wc_grep_start(&g, &fake, "/.") == 0 && !strcmp(g.root, "/"));
    refused_dir = "/locked";
    while(wc_grep_step(&g, 1u << 20));
    refused_dir = NULL;
    CHECK(g.nhits == 1 && g.left_out == 1 && g.left_unreadable == 1);
    wc_grep_notes(&g, notes, sizeof(notes));
    CHECK(!strcmp(notes, "1 folder left out (1 unreadable)"));
    wc_grep_free(&g);
    CHECK(wc_app_init(&a, &mio, &fake) == 0);   /* the app takes a folder not from / from its own */
    strcpy(a.cwd, "/usb1p1");
    strcpy(a.grep.f.what, "needle");
    wc_app_files(&a, ".");
    while(wc_grep_step(&a.grep, 1u << 20));
    CHECK(!strcmp(a.grep.root, "/usb1p1") && a.grep.nhits == 1);
    wc_app_free(&a);
    ntree = 0;
}

static void review(void) {
    review_changes();
    review_lexing();
    review_searching();
    review_files();
}

int main(int argc, char **argv) {
    if(argc > 1 && !strcmp(argv[1], "speed")) {
        speed();
        printf("test_wcode speed: %d checks, %d failed\n", checks, failed);
        return failed ? 1 : 0;
    }
    utf8();
    text_fuzz(argc > 1 ? (unsigned)atoi(argv[1]) : 60);
    text_index();
    characters();
    history_steps();
    history_fuzz(argc > 1 ? (unsigned)atoi(argv[1]) : 60);
    line_ends();
    indenting();
    tabs_and_blocks();
    comments();
    moving();
    brackets();
    reading();
    tokenizers();
    lex_hostile(argc > 1 ? 50u * (unsigned)atoi(argv[1]) : 3000);
    states_fuzz(argc > 1 ? (unsigned)atoi(argv[1]) : 60);
    states_stand();
    finding();
    find_in_files();
    viewing();
    application();
    review();
    CHECK(pool.used == 0);
    printf("test_wcode: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
