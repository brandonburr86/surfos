/*
SurfOS user program: wcode
--------------------
File: find.c    Date: 11/05/26
--------------------
Find and replace (find.h). A search goes a line at a time: each line is taken in one piece from
the text (wc_text_span(), which moves the gap out of the one line that may straddle it) and
searched there, so a match can never take a line end, and a regular expression is never given
more than one line, whatever the file's size. libsurf's regexec() is bounded in time by the
pattern's states a character (regex.h), and REG_STARTEND lets it start inside the line while ^ and
\< still see what came before.

Replace all builds each line that has a match anew and puts it in place of the old one, so the
history holds a line's old and new text for each line changed, not an op a match, and the index's
marks after the first change are dropped first (they would move at every line) and made again by
the next lookup.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "find.h"

#define GROUPS 10

void wc_find_init(struct wc_find *f) {
    memset(f, 0, sizeof(*f));
}

void wc_find_free(struct wc_find *f) {
    if(f->compiled) regfree(&f->re);
    f->compiled = 0;
}

static int word_byte(int c) {
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}

static int fold(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* the pattern compiled, when it is a regular expression: 0, or -1 with f->err */
int wc_find_prepare(struct wc_find *f) {
    int r;
    f->err[0] = 0;
    if(!f->what[0]) {
        snprintf(f->err, sizeof(f->err), "Nothing to find");
        return -1;
    }
    if(!f->regex) return 0;
    if(f->compiled && !strcmp(f->cwhat, f->what) && f->cicase == f->icase) return 0;
    wc_find_free(f);
    r = regcomp(&f->re, f->what, REG_EXTENDED | (f->icase ? REG_ICASE : 0));
    if(r) {
        char e[64];
        regerror(r, &f->re, e, sizeof(e));
        snprintf(f->err, sizeof(f->err), "Not a pattern: %s", e);
        return -1;
    }
    f->compiled = 1;
    snprintf(f->cwhat, sizeof(f->cwhat), "%s", f->what);
    f->cicase = f->icase;
    return 0;
}

/* the first match in the line s[0..n) that starts at or after from: 1 with pm[0] (and its groups, a regular
   expression's), 0 for none */
static int match_in(struct wc_find *f, const char *s, unsigned n, unsigned from, regmatch_t *pm) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned m = (unsigned)strlen(f->what), k, j, a, b;
    int first;
    while(from <= n) {
        if(f->regex) {
            pm[0].rm_so = (regoff_t)from;
            pm[0].rm_eo = (regoff_t)n;
            if(regexec(&f->re, s, GROUPS, pm, REG_STARTEND)) return 0;
            a = (unsigned)pm[0].rm_so;
            b = (unsigned)pm[0].rm_eo;
            if(a == b) {                        /* matching nothing finds nothing: on, a character further */
                from = a + (a < n ? (unsigned)wc_utf8_len(u + a, n - a) : 1);
                continue;
            }
        } else {
            if(m > n) return 0;
            first = f->icase ? fold((unsigned char)f->what[0]) : (unsigned char)f->what[0];
            for(k = from; k + m <= n; k++) {
                if(f->icase ? fold(u[k]) != first : u[k] != first) {
                    if(!f->icase) {             /* to the next byte that can start a match */
                        const unsigned char *p = memchr(u + k, first, n - m + 1 - k);
                        if(!p) {
                            k = n;
                            break;
                        }
                        k = (unsigned)(p - u) - 1;
                    }
                    continue;
                }
                for(j = 1; j < m && (f->icase ? fold(u[k + j]) == fold((unsigned char)f->what[j]) :
                                     u[k + j] == (unsigned char)f->what[j]); j++);
                if(j == m) break;
            }
            if(k + m > n) return 0;
            a = k;
            b = k + m;
            pm[0].rm_so = (regoff_t)a;
            pm[0].rm_eo = (regoff_t)b;
        }
        if(!f->word || ((a == 0 || !word_byte(u[a - 1])) && (b >= n || !word_byte(u[b])))) return 1;
        from = a + (unsigned)wc_utf8_len(u + a, n - a);
    }
    return 0;
}

int wc_find_in(struct wc_find *f, const char *s, unsigned n, unsigned from, unsigned *a, unsigned *b) {
    regmatch_t pm[GROUPS];
    if(!match_in(f, s, n, from, pm)) return 0;
    *a = (unsigned)pm[0].rm_so;
    *b = (unsigned)pm[0].rm_eo;
    return 1;
}

/* the last match in the line that starts before limit */
static int last_in(struct wc_find *f, const char *s, unsigned n, unsigned limit, regmatch_t *pm) {
    regmatch_t g[GROUPS];
    unsigned from = 0;
    int found = 0;
    while(from < limit && from <= n && match_in(f, s, n, from, g) && (unsigned)g[0].rm_so < limit) {
        memcpy(pm, g, sizeof(g));
        found = 1;
        from = (unsigned)g[0].rm_so + (unsigned)wc_utf8_len((const unsigned char *)s + g[0].rm_so, n - (unsigned)g[0].rm_so);
    }
    return found;
}

/* the line that starts at ls: a match in it, from from on (whole: anywhere), or the last before from (backward) */
static int search_line(struct wc_find *f, struct wc_text *t, unsigned ls, unsigned from, int backward, int whole,
                       regmatch_t *pm, struct wc_line *l) {
    unsigned n, at;
    const char *s;
    wc_text_line(t, ls, l);
    n = l->end - l->start;
    s = wc_text_span(t, l->start, n);
    at = whole ? (backward ? n + 1 : 0) : from - ls;
    if(!backward) return at <= n && match_in(f, s, n, at, pm);
    return at && last_in(f, s, n, at, pm);
}

int wc_find_next(struct wc_find *f, struct wc_text *t, unsigned from, int backward, unsigned *a, unsigned *b,
                 int *wrapped) {
    struct wc_line l;
    regmatch_t pm[GROUPS];
    unsigned first, ls;
    *wrapped = 0;
    if(wc_find_prepare(f) < 0) return -1;
    if(from > t->len) from = t->len;
    first = wc_text_line_begin(t, from);
    /* the lines a position at a time, never by number: a search through 16 MB looks each line up once */
    if(!backward) {
        for(ls = first;; ls = l.end + l.eol) {
            if(search_line(f, t, ls, from, 0, ls != first, pm, &l)) goto found;
            if(!l.eol) break;
        }
        for(ls = 0;; ls = l.end + l.eol) {      /* from the top, to the line it started on */
            if(search_line(f, t, ls, from, 0, 1, pm, &l)) {
                *wrapped = 1;
                goto found;
            }
            if(ls >= first || !l.eol) break;
        }
    } else {
        for(ls = first;; ls = wc_text_line_begin(t, ls - 1)) {
            if(search_line(f, t, ls, from, 1, ls != first, pm, &l)) goto found;
            if(!ls) break;
        }
        for(ls = wc_text_line_begin(t, t->len);; ls = wc_text_line_begin(t, ls - 1)) {     /* from the bottom */
            if(search_line(f, t, ls, from, 1, 1, pm, &l)) {
                *wrapped = 1;
                goto found;
            }
            if(ls <= first) break;
        }
    }
    return 0;
found:
    *a = l.start + (unsigned)pm[0].rm_so;
    *b = l.start + (unsigned)pm[0].rm_eo;
    return 1;
}

int wc_find_doc(struct wc_find *f, struct wc_doc *d, int backward) {
    unsigned from, to, a, b;
    int wrapped, r;
    wc_doc_sel(d, &from, &to);
    r = wc_find_next(f, &d->t, backward ? from : to, backward, &a, &b, &wrapped);
    if(r < 0) snprintf(d->msg, sizeof(d->msg), "%s", f->err);
    else if(!r) snprintf(d->msg, sizeof(d->msg), "Not found: %.120s", f->what);
    else {
        wc_doc_select(d, backward ? b : a, backward ? a : b);
        if(wrapped) snprintf(d->msg, sizeof(d->msg), backward ? "Found again from the bottom" : "Found again from the top");
    }
    return r;
}

/**** replacing ****/

struct out { char *b; unsigned n, cap; int failed; };

static void add(struct out *o, const char *s, unsigned n) {
    unsigned cap;
    char *b;
    if(o->failed || !n) return;
    if((unsigned long)o->n + n > WC_TEXT_MAX) {
        o->failed = 1;
        return;
    }
    if(o->n + n > o->cap) {
        for(cap = o->cap ? o->cap : 256; cap < o->n + n; cap *= 2);
        b = realloc(o->b, cap);
        if(!b) {
            o->failed = 1;
            return;
        }
        o->b = b;
        o->cap = cap;
    }
    memcpy(o->b + o->n, s, n);
    o->n += n;
}

/* what goes in place of the match pm[0] in the line s */
static void replacement(struct wc_find *f, const char *s, const regmatch_t *pm, int crlf, struct out *o) {
    const char *w = f->with;
    int g;
    if(!f->regex) {
        add(o, w, (unsigned)strlen(w));
        return;
    }
    for(; *w; w++) {
        if(*w == '&') g = 0;
        else if(*w == '\\' && w[1] >= '0' && w[1] <= '9') g = *++w - '0';
        else {
            if(*w == '\\' && w[1]) {
                w++;
                if(*w == 'n') {
                    add(o, crlf ? "\r\n" : "\n", crlf ? 2 : 1);
                    continue;
                }
                if(*w == 't') {
                    add(o, "\t", 1);
                    continue;
                }
            }
            add(o, w, 1);
            continue;
        }
        if(g < GROUPS && pm[g].rm_so >= 0) add(o, s + pm[g].rm_so, (unsigned)(pm[g].rm_eo - pm[g].rm_so));
    }
}

int wc_find_replace(struct wc_find *f, struct wc_doc *d) {
    struct wc_line l;
    regmatch_t pm[GROUPS];
    struct out o = { NULL, 0, 0, 0 };
    unsigned from, to, ls;
    int done = 0;
    if(wc_find_prepare(f) < 0) {
        snprintf(d->msg, sizeof(d->msg), "%s", f->err);
        return -1;
    }
    if(wc_doc_sel(d, &from, &to)) {             /* the selection, when it is a match */
        ls = wc_text_line_begin(&d->t, from);
        wc_text_line(&d->t, ls, &l);
        if(to <= l.end && match_in(f, wc_text_span(&d->t, l.start, l.end - l.start), l.end - l.start, from - ls, pm) &&
           (unsigned)pm[0].rm_so == from - ls && (unsigned)pm[0].rm_eo == to - ls) {
            replacement(f, wc_text_span(&d->t, l.start, l.end - l.start), pm, d->crlf, &o);
            if(o.failed) snprintf(d->msg, sizeof(d->msg), "Not replaced: the replacement is too long");
            else if(!wc_doc_replace(d, from, to, o.b ? o.b : "", o.n, 0)) done = 1;
            free(o.b);
            if(!done) return 0;
        }
    }
    if(wc_find_doc(f, d, 0) < 0) return -1;
    return done;
}

int wc_find_replace_all(struct wc_find *f, struct wc_doc *d) {
    struct wc_text *t = &d->t;
    struct wc_line l;
    regmatch_t pm[GROUPS];
    struct out o;
    const char *s;
    unsigned ls = 0, n, at, count = 0, changed = 0;
    long grow;
    int began = 0;
    if(wc_find_prepare(f) < 0) {
        snprintf(d->msg, sizeof(d->msg), "%s", f->err);
        return -1;
    }
    memset(&o, 0, sizeof(o));
    /* first what it comes to: all of it or none, never a file left half replaced at the size limit */
    for(ls = 0, grow = 0;; ls = l.end + l.eol) {
        wc_text_line(t, ls, &l);
        n = l.end - l.start;
        s = wc_text_span(t, l.start, n);
        for(at = 0; at <= n && match_in(f, s, n, at, pm); at = (unsigned)pm[0].rm_eo) {
            o.n = 0;
            replacement(f, s, pm, d->crlf, &o);
            grow += (long)o.n - (long)(pm[0].rm_eo - pm[0].rm_so);
            if(o.failed || (long)t->len + grow > (long)WC_TEXT_MAX) {
                free(o.b);
                snprintf(d->msg, sizeof(d->msg), "Not replaced: the file would have more than %u MB", WC_TEXT_MAX >> 20);
                return -1;
            }
        }
        if(!l.eol) break;
    }
    ls = 0;
    for(;;) {
        wc_text_line(t, ls, &l);
        n = l.end - l.start;
        s = wc_text_span(t, l.start, n);
        o.n = 0;
        for(at = 0; at <= n && match_in(f, s, n, at, pm); at = (unsigned)pm[0].rm_eo) {
            add(&o, s + at, (unsigned)pm[0].rm_so - at);
            replacement(f, s, pm, d->crlf, &o);
            count++;
        }
        if(o.failed) break;
        if(at) {                                /* this line had a match: its new text in place of the old */
            add(&o, s + at, n - at);
            if(o.failed) break;
            if(!began) {
                wc_text_drop_marks(t, l.start);
                wc_doc_begin(d, WC_K_OTHER);
                began = 1;
            }
            wc_doc_cut(d, l.start, n);
            if(wc_doc_put(d, l.start, o.b ? o.b : "", o.n) < 0) {
                o.failed = 2;
                break;
            }
            changed++;
            n = o.n;
        }
        if(!l.eol) break;
        ls = l.start + n + l.eol;
    }
    if(began) wc_doc_end(d);
    free(o.b);
    if(o.failed == 1) snprintf(d->msg, sizeof(d->msg), "Stopped: a line would be too long (%u replaced)", count);
    else if(!count && !o.failed) snprintf(d->msg, sizeof(d->msg), "Not found: %.120s", f->what);
    else if(!o.failed && !d->u.forgot)
        snprintf(d->msg, sizeof(d->msg), "Replaced %u (on %u line%s)", count, changed, changed == 1 ? "" : "s");
    return (int)count;
}
