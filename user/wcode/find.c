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

11/09/26 (the review, C1 and C3). One regexec() reads from where it starts to the line's end at
most, but the searches called it again and again on a line: after an empty match a character on,
for Find previous from each match's start on, and for whole words from each rejected match's
start on, so a long line cost its length squared. A pattern that matches an empty text is now
refused; Find previous walks a line's matches once, each search on from the last one's end; whole
words go on from the next place a word can start. And every search is charged the text each call
may read, so that with a budget it stops where it is and goes on from there at the next call:
the window answers meanwhile and Esc stops it (struct wc_find's job, all and their fields).
Replace all counts first what it comes to: the file's size at its largest along the way and its
longest new line; it then has the text's room promised (wc_doc_room()) and the line's buffer made,
so that once a line is cut nothing can fail before its new text is in its place.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "find.h"

#define GROUPS 10

void wc_find_init(struct wc_find *f) {
    memset(f, 0, sizeof(*f));
}

void wc_find_free(struct wc_find *f) {
    if(f->compiled) regfree(&f->re);
    f->compiled = 0;
    free(f->ob);
    f->ob = NULL;
    f->on = f->ocap = 0;
    f->job = f->all = 0;
}

static int word_byte(int c) {
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}

static int fold(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* the pattern compiled, when it is a regular expression: 0, or -1 with f->err */
int wc_find_prepare(struct wc_find *f) {
    regmatch_t pm[1];
    int r;
    f->err[0] = 0;
    if(!f->what[0]) {
        snprintf(f->err, sizeof(f->err), "Nothing to find");
        return -1;
    }
    if(!f->regex) return 0;
    if(f->compiled && !strcmp(f->cwhat, f->what) && f->cicase == f->icase) return 0;
    if(f->compiled) regfree(&f->re);
    f->compiled = 0;
    r = regcomp(&f->re, f->what, REG_EXTENDED | (f->icase ? REG_ICASE : 0));
    if(r) {
        char e[64];
        regerror(r, &f->re, e, sizeof(e));
        snprintf(f->err, sizeof(f->err), "Not a pattern: %s", e);
        return -1;
    }
    /* 11/09/26: one that matches an empty text would match between every two characters, to be stepped over one at a
       time; it finds nothing a match can be, so it is refused (x+ for x*) */
    if(!regexec(&f->re, "", 1, pm, 0)) {
        regfree(&f->re);
        snprintf(f->err, sizeof(f->err), "Not searched: the pattern also matches nothing at all (x* where x+ is meant)");
        return -1;
    }
    f->compiled = 1;
    snprintf(f->cwhat, sizeof(f->cwhat), "%s", f->what);
    f->cicase = f->icase;
    return 0;
}

/* the first match in the line s[0..n) that starts at or after from: 1 with pm[0] (and its groups, a regular
   expression's); 0 for none; -1 when the work came to f->limit first (f->resume: where in the line to go on); -2 when
   the pattern took too long (a back-reference: REG_ESTEPS). Each regexec() is charged what it may read: from where it
   starts to the line's end */
static int match_in(struct wc_find *f, const char *s, unsigned n, unsigned from, regmatch_t *pm) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned m = (unsigned)strlen(f->what), k, j, a, b;
    int first, r;
    while(from <= n) {
        if(f->limit && f->work >= f->limit) {
            f->resume = from;
            return -1;
        }
        if(f->regex) {
            pm[0].rm_so = (regoff_t)from;
            pm[0].rm_eo = (regoff_t)n;
            r = regexec(&f->re, s, GROUPS, pm, REG_STARTEND);
            f->work += n - from + 1;
            if(r == REG_ESTEPS) return -2;
            if(r) return 0;
            a = (unsigned)pm[0].rm_so;
            b = (unsigned)pm[0].rm_eo;
            if(a == b) {                        /* nothing, at a place only (\<): on, a character further */
                from = a + (a < n ? (unsigned)wc_utf8_len(u + a, n - a) : 1);
                continue;
            }
        } else {
            if(m > n || from > n - m) {
                f->work += n - from + 1;
                return 0;
            }
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
            f->work += (k + m <= n ? k + m : n) - from + 1;
            if(k + m > n) return 0;
            a = k;
            b = k + m;
            pm[0].rm_so = (regoff_t)a;
            pm[0].rm_eo = (regoff_t)b;
        }
        if(!f->word || ((a == 0 || !word_byte(u[a - 1])) && (b >= n || !word_byte(u[b])))) return 1;
        /* not a whole word: a match from inside the word it is in would be none either, so on from the next place a
           word can start (after a byte that is no word's) */
        for(from = a + 1; from < n && word_byte(u[from - 1]); from++);
    }
    return 0;
}

int wc_find_in(struct wc_find *f, const char *s, unsigned n, unsigned from, unsigned *a, unsigned *b) {
    regmatch_t pm[GROUPS];
    int r = match_in(f, s, n, from, pm);
    if(r != 1) return r;
    *a = (unsigned)pm[0].rm_so;
    *b = (unsigned)pm[0].rm_eo;
    return 1;
}

/**** a search of the text, in steps ****/

int wc_find_begin(struct wc_find *f, struct wc_text *t, unsigned from, int backward) {
    f->job = 0;
    if(wc_find_prepare(f) < 0) return -1;
    if(from > t->len) from = t->len;
    f->job = 1;
    f->back = backward;
    f->phase = f->have = 0;
    f->start = from;
    f->first = f->ls = wc_text_line_begin(t, from);
    f->at = backward ? 0 : from - f->ls;
    return 0;
}

void wc_find_stop(struct wc_find *f) {
    f->job = 0;
}

/* the line the search is in: 1 found (pm), 0 none, -1 the budget spent (f->at kept), -2 too long. Backward, its
   matches are walked once from its start, each search on from the last one's end, the last one before the limit
   kept: the start in the line it began in, its end elsewhere */
static int job_line(struct wc_find *f, struct wc_text *t, regmatch_t *pm, struct wc_line *l) {
    unsigned n, lim;
    const char *s;
    int r;
    wc_text_line(t, f->ls, l);
    n = l->end - l->start;
    s = wc_text_span(t, l->start, n);
    if(!f->back) {
        if(f->at > n) return 0;
        r = match_in(f, s, n, f->at, pm);
        if(r == -1) f->at = f->resume;
        return r;
    }
    lim = !f->phase && f->ls == f->first ? f->start - l->start : n + 1;
    while(f->at < lim && f->at <= n) {
        r = match_in(f, s, n, f->at, pm);
        if(r == -1) {
            f->at = f->resume;
            return -1;
        }
        if(r == -2) return -2;
        if(!r || (unsigned)pm[0].rm_so >= lim) break;
        f->have = 1;
        f->ma = (unsigned)pm[0].rm_so;
        f->mb = (unsigned)pm[0].rm_eo;
        f->at = f->mb;                          /* (a match is never empty) */
    }
    if(!f->have) return 0;
    pm[0].rm_so = (regoff_t)f->ma;
    pm[0].rm_eo = (regoff_t)f->mb;
    return 1;
}

/* on to the next line to search, round the end once: 0 when it is back where it began */
static int next_line(struct wc_find *f, struct wc_text *t, const struct wc_line *l) {
    f->at = 0;
    f->have = 0;
    if(!f->back) {
        if(f->phase && f->ls >= f->first) return 0;
        if(l->eol) f->ls = l->end + l->eol;
        else if(f->phase) return 0;
        else {                                  /* from the top, to the line it began in */
            f->phase = 1;
            f->ls = 0;
        }
        return 1;
    }
    if(f->phase && f->ls <= f->first) return 0;
    if(f->ls) f->ls = wc_text_line_begin(t, f->ls - 1);
    else if(f->phase) return 0;
    else {                                      /* from the bottom, to the line it began in */
        f->phase = 1;
        f->ls = wc_text_line_begin(t, t->len);
    }
    return 1;
}

int wc_find_go(struct wc_find *f, struct wc_text *t, unsigned long budget, unsigned *a, unsigned *b, int *wrapped) {
    regmatch_t pm[GROUPS];
    struct wc_line l;
    int r;
    if(!f->job) return 0;
    f->limit = budget ? f->work + budget : 0;
    for(;;) {
        r = job_line(f, t, pm, &l);
        if(r == -1) {
            f->limit = 0;
            return 2;
        }
        if(r == -2) {
            snprintf(f->err, sizeof(f->err), "Stopped: the pattern takes too long on this text (a back-reference)");
            break;
        }
        if(r == 1) {
            *a = l.start + (unsigned)pm[0].rm_so;
            *b = l.start + (unsigned)pm[0].rm_eo;
            *wrapped = f->phase;
            break;
        }
        if(!next_line(f, t, &l)) break;
    }
    f->job = 0;
    f->limit = 0;
    return r == -2 ? -1 : r;
}

int wc_find_next(struct wc_find *f, struct wc_text *t, unsigned from, int backward, unsigned *a, unsigned *b,
                 int *wrapped) {
    *wrapped = 0;
    if(wc_find_begin(f, t, from, backward) < 0) return -1;
    return wc_find_go(f, t, 0, a, b, wrapped);
}

int wc_find_doc_begin(struct wc_find *f, struct wc_doc *d, int backward) {
    unsigned from, to;
    wc_doc_sel(d, &from, &to);
    if(wc_find_begin(f, &d->t, backward ? from : to, backward) < 0) {
        snprintf(d->msg, sizeof(d->msg), "%s", f->err);
        return -1;
    }
    return 0;
}

int wc_find_doc_go(struct wc_find *f, struct wc_doc *d, unsigned long budget) {
    unsigned a = 0, b = 0;
    int wrapped = 0, back = f->back, r = wc_find_go(f, &d->t, budget, &a, &b, &wrapped);
    if(r == 2) return 2;
    if(r < 0) snprintf(d->msg, sizeof(d->msg), "%s", f->err);
    else if(!r) snprintf(d->msg, sizeof(d->msg), "Not found: %.120s", f->what);
    else {
        wc_doc_select(d, back ? b : a, back ? a : b);
        if(wrapped) snprintf(d->msg, sizeof(d->msg), back ? "Found again from the bottom" : "Found again from the top");
    }
    return r;
}

int wc_find_doc(struct wc_find *f, struct wc_doc *d, int backward) {
    if(wc_find_doc_begin(f, d, backward) < 0) return -1;
    return wc_find_doc_go(f, d, 0);
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
            o->failed = 2;
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

int wc_find_replace_sel(struct wc_find *f, struct wc_doc *d) {
    struct wc_line l;
    regmatch_t pm[GROUPS];
    struct out o = { NULL, 0, 0, 0 };
    unsigned from, to, ls;
    int done = 0, r;
    if(wc_find_prepare(f) < 0) {
        snprintf(d->msg, sizeof(d->msg), "%s", f->err);
        return -1;
    }
    if(!wc_doc_sel(d, &from, &to)) return 0;
    ls = wc_text_line_begin(&d->t, from);
    wc_text_line(&d->t, ls, &l);
    if(to > l.end) return 0;
    f->limit = f->work + WC_FIND_STEP;          /* (is the selection a match: a question of one line, bounded) */
    r = match_in(f, wc_text_span(&d->t, l.start, l.end - l.start), l.end - l.start, from - ls, pm);
    f->limit = 0;
    if(r != 1 || (unsigned)pm[0].rm_so != from - ls || (unsigned)pm[0].rm_eo != to - ls) return 0;
    replacement(f, wc_text_span(&d->t, l.start, l.end - l.start), pm, d->crlf, &o);
    if(o.failed) snprintf(d->msg, sizeof(d->msg), o.failed == 1 ? "Not replaced: the replacement is too long" :
                          "Not replaced: there is not enough memory");
    else if(!wc_doc_replace(d, from, to, o.b ? o.b : "", o.n, 0)) done = 1;
    free(o.b);
    return done ? 1 : -2;                       /* (a match not replaced: d->msg says why, and the search stops there) */
}

int wc_find_replace(struct wc_find *f, struct wc_doc *d) {
    int done = wc_find_replace_sel(f, d);
    if(done < 0) return done == -2 ? 0 : -1;
    if(wc_find_doc(f, d, 0) < 0) return -1;
    return done;
}

/* Replace all, in two runs over the lines: counting (all 1), then changing (all 2) */

int wc_find_all_begin(struct wc_find *f, struct wc_doc *d) {
    f->all = 0;
    f->job = 0;
    if(wc_find_prepare(f) < 0) {
        snprintf(d->msg, sizeof(d->msg), "%s", f->err);
        return -1;
    }
    f->all = 1;
    f->ls = f->at = f->mb = 0;
    f->have = 0;
    f->count = f->changed = f->maxline = f->linelen = f->lines = 0;
    f->grow = f->maxgrow = 0;
    f->began = 0;
    free(f->ob);
    f->ob = NULL;
    f->on = f->ocap = 0;
    return 0;
}

int wc_find_all_changing(const struct wc_find *f) {
    return f->all == 2 && f->began;
}

static int all_end(struct wc_find *f, int r) {
    f->all = 0;
    f->limit = 0;
    free(f->ob);
    f->ob = NULL;
    f->on = f->ocap = 0;
    return r;
}

int wc_find_all_stop(struct wc_find *f, struct wc_doc *d) {
    if(!f->all) return 1;
    if(f->all == 2 && f->began) {
        if(d->u.forgot) return 0;               /* nothing to take it back with: it goes on to its end */
        wc_doc_end(d);
        wc_text_reserve(&d->t, d->t.len);       /* (no shrinking: the way back needs no more room than it had) */
        wc_doc_undo(d, 0);
        wc_text_release(&d->t);
    } else if(f->all == 2) wc_text_release(&d->t);
    snprintf(d->msg, sizeof(d->msg), "Stopped: nothing replaced");
    all_end(f, 0);
    return 1;
}

/* the first run: what each line comes to, the file's largest size along the way and its longest new line */
static int all_count(struct wc_find *f, struct wc_doc *d) {
    struct wc_text *t = &d->t;
    struct wc_line l;
    regmatch_t pm[GROUPS];
    struct out o = { NULL, 0, 0, 0 };
    const char *s;
    unsigned n;
    int r;
    for(;;) {
        wc_text_line(t, f->ls, &l);
        n = l.end - l.start;
        s = wc_text_span(t, l.start, n);
        for(;;) {
            r = match_in(f, s, n, f->at, pm);
            if(r == -1) {
                f->at = f->resume;
                free(o.b);
                return 2;
            }
            if(r == -2) {
                free(o.b);
                snprintf(d->msg, sizeof(d->msg), "Not replaced: the pattern takes too long on this text (a back-reference)");
                return -1;
            }
            if(!r) break;
            o.n = 0;
            replacement(f, s, pm, d->crlf, &o);
            if(o.failed == 2) {
                free(o.b);
                snprintf(d->msg, sizeof(d->msg), "Not replaced: there is not enough memory");
                return -1;
            }
            f->linelen += (unsigned)pm[0].rm_so - f->mb + o.n;
            f->grow += (long)o.n - (long)(pm[0].rm_eo - pm[0].rm_so);
            if(f->grow > f->maxgrow) f->maxgrow = f->grow;
            f->count++;
            f->have = 1;
            f->at = f->mb = (unsigned)pm[0].rm_eo;
            if(o.failed || (unsigned long)f->linelen + (n - f->mb) > WC_TEXT_MAX ||
               (long)t->len + f->maxgrow > (long)WC_TEXT_MAX) {
                free(o.b);
                snprintf(d->msg, sizeof(d->msg), "Not replaced: the file would have more than %u MB", WC_TEXT_MAX >> 20);
                return -1;
            }
        }
        if(f->have) {                           /* a line with a match: its new length */
            f->linelen += n - f->mb;
            if(f->linelen > f->maxline) f->maxline = f->linelen;
            f->lines++;
        }
        f->linelen = 0;
        f->at = f->mb = 0;
        f->have = 0;
        if(!l.eol) break;
        f->ls = l.end + l.eol;
    }
    free(o.b);
    return f->count ? 1 : 0;
}

/* the second run: each line with a match built anew and put in place of the old, which nothing can fail now */
static int all_change(struct wc_find *f, struct wc_doc *d) {
    struct wc_text *t = &d->t;
    struct wc_line l;
    regmatch_t pm[GROUPS];
    struct out o;
    const char *s;
    unsigned n;
    int r;
    o.b = f->ob;
    o.n = f->on;
    o.cap = f->ocap;
    o.failed = 0;
    for(;;) {
        wc_text_line(t, f->ls, &l);
        n = l.end - l.start;
        s = wc_text_span(t, l.start, n);
        for(;;) {
            r = match_in(f, s, n, f->at, pm);
            if(r == -1) {
                f->at = f->resume;
                f->on = o.n;
                return 2;
            }
            if(r != 1) break;                   /* (the count ran these: none gives up now) */
            add(&o, s + f->mb, (unsigned)pm[0].rm_so - f->mb);
            replacement(f, s, pm, d->crlf, &o);
            f->have = 1;
            f->at = f->mb = (unsigned)pm[0].rm_eo;
        }
        if(f->have) {                           /* this line had a match: its new text in place of the old */
            add(&o, s + f->mb, n - f->mb);
            if(!f->began) {
                wc_text_drop_marks(t, l.start);
                wc_doc_begin(d, WC_K_OTHER);
                f->began = 1;
            }
            wc_doc_cut(d, l.start, n);
            if(o.failed || wc_doc_put(d, l.start, o.b, o.n) < 0) return -1;    /* (room and buffer were made first) */
            f->changed++;
            n = o.n;
        }
        o.n = 0;
        f->on = 0;
        f->at = f->mb = 0;
        f->have = 0;
        if(!l.eol) break;
        f->ls = l.start + n + l.eol;
    }
    return 1;
}

int wc_find_all_go(struct wc_find *f, struct wc_doc *d, unsigned long budget) {
    unsigned count;
    int r;
    if(!f->all) return 0;
    f->limit = budget ? f->work + budget : 0;
    if(f->all == 1) {
        r = all_count(f, d);
        if(r == 2) {
            f->limit = 0;
            return 2;
        }
        if(r < 0) return all_end(f, -1);
        if(!r) {
            snprintf(d->msg, sizeof(d->msg), "Not found: %.120s", f->what);
            return all_end(f, 0);
        }
        /* all of it or none: the room the text needs at its largest, and the buffer of the longest line, first */
        f->ob = malloc(f->maxline ? f->maxline : 1);
        if(!f->ob) {
            snprintf(d->msg, sizeof(d->msg), "Not replaced: there is not enough memory");
            return all_end(f, -1);
        }
        f->ocap = f->maxline ? f->maxline : 1;
        f->on = 0;
        if(wc_doc_room(d, (unsigned long)d->t.len + (unsigned long)(f->maxgrow > 0 ? f->maxgrow : 0)) < 0) {
            snprintf(d->msg, sizeof(d->msg), errno == EFBIG ? "Not replaced: the file would have more than %u MB" :
                     "Not replaced: there is not enough memory", WC_TEXT_MAX >> 20);
            return all_end(f, -1);
        }
        f->all = 2;
        f->ls = f->at = f->mb = 0;
        f->have = 0;
        f->began = 0;
    }
    r = all_change(f, d);
    if(r == 2) {
        f->limit = 0;
        return 2;
    }
    if(f->began) wc_doc_end(d);
    else wc_text_release(&d->t);
    count = f->count;
    if(r < 0) snprintf(d->msg, sizeof(d->msg), "Stopped by an error after %u lines: Ctrl+Z takes them back", f->changed);
    else snprintf(d->msg, sizeof(d->msg), "Replaced %u (on %u line%s)%s", count, f->changed, f->changed == 1 ? "" : "s",
                  d->u.forgot ? ": too large a change to undo" : "");
    return all_end(f, r < 0 ? -1 : (int)count);
}

int wc_find_replace_all(struct wc_find *f, struct wc_doc *d) {
    if(wc_find_all_begin(f, d) < 0) return -1;
    return wc_find_all_go(f, d, 0);
}
