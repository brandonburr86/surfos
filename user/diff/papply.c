/*
SurfOS user programs: applying a hunk
--------------------
File: papply.c  Date: 11/01/26
--------------------
See patch.h. A hunk is looked for where it says, moved by what the last hunk was moved; then nearer places first, on
both sides, from the end of the last hunk to the end of the file. If it fits nowhere exactly the same search
runs with up to PATCH_FUZZ lines of its context at each end not compared: window_at() says which lines, and
why a hunk made at an edge of the file stays there. The first hunk is also looked for the other way round, to see a patch
that went in before: at each fuzz where it says, both ways, before anywhere else. Hunks are applied in order, each only after
the one before it, so that the file comes out forward, once.
*/

#include "patch.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define COPY_BLOCK 32768

/**** the file's index ****/

static int grow(struct ptarget *t) {
    unsigned cap = t->cap ? t->cap + t->cap / 2 : 1024;
    unsigned *p;
    if(cap >= 0x3FFFFFFFu) return -1;
    if(!(p = realloc(t->start, ((size_t)cap + 1) * sizeof(*p)))) return -1;
    t->start = p;
    if(!(p = realloc(t->hash, (size_t)cap * sizeof(*p)))) return -1;
    t->hash = p;
    t->cap = cap;
    return 0;
}

int pt_load(struct ptarget *t, struct dsrc *src) {
    unsigned at = 0, h = DH_START;
    int c, any = 0, e;
    memset(t, 0, sizeof(*t));
    t->src = src;
    if(grow(t) < 0) {
        errno = ENOMEM;
        return -1;
    }
    t->start[0] = 0;
    if(!src) return 0;
    if(dr_start(&t->rd, src, 0, src->size) < 0) return -1;
    while((c = dr_getc(&t->rd)) >= 0) {
        at++;
        any = 1;
        h = DH_ADD(h, c);
        if(c != '\n') continue;
        if(t->nlines == t->cap && grow(t) < 0) {
            errno = ENOMEM;
            return -1;
        }
        t->hash[t->nlines++] = h;
        t->start[t->nlines] = at;
        h = DH_START;
        any = 0;
    }
    e = t->rd.failed;
    if(e) {
        errno = e;
        return -1;
    }
    if(any) {                                   /* a last line with no newline */
        if(t->nlines == t->cap && grow(t) < 0) {
            errno = ENOMEM;
            return -1;
        }
        t->hash[t->nlines++] = h;
        t->start[t->nlines] = at;
    }
    return 0;
}

void pt_free(struct ptarget *t) {
    if(t->rd.buf) dr_stop(&t->rd);
    free(t->start);
    free(t->hash);
    memset(t, 0, sizeof(*t));
}

/**** where a hunk goes ****/

/* do the old lines of the hunk from number skip on, count of them, stand at line p of the file? By hash first,
   which needs no reading, and only then byte by byte */
static int fits(struct papply *a, const struct phunk *h, const unsigned *old_lines, unsigned skip, unsigned count, unsigned p) {
    struct ptarget *t = a->t;
    unsigned i;
    for(i = 0; i < count; i++) {
        const struct pline *l = &h->lines[old_lines[skip + i]];
        if(t->hash[p + i] != l->hash || t->start[p + i + 1] - t->start[p + i] != l->len + !l->no_newline) return 0;
    }
    if(count && dr_tell(&t->rd) != t->start[p]) dr_seek(&t->rd, t->start[p]);
    for(i = 0; i < count; i++) {
        const struct pline *l = &h->lines[old_lines[skip + i]];
        const unsigned char *text = (const unsigned char *)h->text + l->at;
        unsigned k;
        for(k = 0; k < l->len; k++) {
            int c = dr_getc(&t->rd);
            if(c != text[k]) {
                if(t->rd.failed) a->failed = t->rd.failed;
                return 0;
            }
        }
        if(!l->no_newline && dr_getc(&t->rd) != '\n') {
            if(t->rd.failed) a->failed = t->rd.failed;
            return 0;
        }
    }
    return 1;
}

/* the place for the first of count lines from skip, at the distance nearest the guess; fixed: only guess will do */
static int search(struct papply *a, const struct phunk *h, const unsigned *old_lines, unsigned skip, unsigned count,
                  long long guess, int fixed, unsigned *where) {
    long long lo = a->done, hi = (long long)a->t->nlines - count, d;
    long long far;
    if(hi < lo) return 0;
    if(fixed) {
        if(guess < lo || guess > hi) return 0;
        if(fits(a, h, old_lines, skip, count, (unsigned)guess)) {
            *where = (unsigned)guess;
            return 1;
        }
        return 0;
    }
    if(guess < lo) guess = lo;                  /* it cannot be before the last one's end */
    if(guess > hi) guess = hi;
    far = guess - lo > hi - guess ? guess - lo : hi - guess;
    for(d = 0; d <= far; d++) {
        if(guess + d <= hi && fits(a, h, old_lines, skip, count, (unsigned)(guess + d))) {
            *where = (unsigned)(guess + d);
            return 1;
        }
        if(a->failed) return 0;
        if(d && guess - d >= lo && fits(a, h, old_lines, skip, count, (unsigned)(guess - d))) {
            *where = (unsigned)(guess - d);
            return 1;
        }
        if(a->failed) return 0;
    }
    return 0;
}

/* the hunk's old lines, as indices into its lines */
static unsigned *old_side(const struct phunk *h, unsigned *count) {
    unsigned *v = malloc(((size_t)h->nlines + 1) * sizeof(*v)), i, n = 0;
    if(!v) return NULL;
    for(i = 0; i < h->nlines; i++)
        if(h->lines[i].kind != '+') v[n++] = i;
    *count = n;
    return v;
}

/* what of a hunk's old lines is compared at one fuzz */
struct window {
    unsigned first, count;              /* those old lines */
    int at_top, at_bottom;              /* and it must stand at that edge of the file */
};

/* Fuzz is how many lines at an end of a hunk may differ, taken from the outside in, as those lines are the furthest from the change
   and the likeliest to have moved on. It applies to context only, and something must be left to compare.

   A diff shows the same context at both ends of a hunk, unless the file ran out. So an end with less context than the other was
   made at an edge of the file: it is closed. A closed end says where that edge is, and that holds at every fuzz: the hunk
   stands at the edge, and that end's lines are all compared. Only the other end loosens. For the start the hunk's line
   number must agree; the end of a file has no number to ask.

   0 when this fuzz has nothing to add to the one below it, or would leave nothing to compare. */
static int window_at(const struct phunk *h, unsigned n, unsigned fuzz, struct window *w) {
    unsigned top = fuzz < h->lead ? fuzz : h->lead, bottom = fuzz < h->trail ? fuzz : h->trail;
    w->at_top = h->lead < h->trail && h->old_start <= 1;
    w->at_bottom = h->trail < h->lead;
    if(w->at_top) top = 0;
    if(w->at_bottom) bottom = 0;
    if(fuzz && top < fuzz && bottom < fuzz) return 0;
    if(top + bottom >= n) return 0;
    w->first = top;
    w->count = n - top - bottom;
    return 1;
}

#define NO_MORE 2                       /* at_fuzz(): this fuzz adds nothing */

/* the hunk at one fuzz: 1 and *where, 0, -1 a read failed, or NO_MORE. Only where it says, or with near nearby too, the
   nearest first */
static int at_fuzz(struct papply *a, const struct phunk *h, const unsigned *old_lines, unsigned n, unsigned f, int near,
                   unsigned *where) {
    struct window w;
    int found;
    if(!window_at(h, n, f, &w)) return NO_MORE;
    if(w.at_top) found = search(a, h, old_lines, w.first, w.count, 0, 1, where);
    else if(w.at_bottom) found = search(a, h, old_lines, w.first, w.count, (long long)a->t->nlines - w.count, 1, where);
    else found = search(a, h, old_lines, w.first, w.count, (long long)h->old_start - 1 + a->offset + w.first, !near, where);
    return a->failed ? -1 : found;
}

int pa_locate(struct papply *a, struct phunk *h, unsigned *at, unsigned *fuzz, int *reversed) {
    unsigned n, back_n = 0, f, *old_lines, *back_lines = NULL, where = 0, step;
    long long guess = (long long)h->old_start - 1 + a->offset;
    int r = 0, more[2];
    if(reversed) *reversed = 0;
    if(!(old_lines = old_side(h, &n))) {
        a->failed = ENOMEM;
        return -1;
    }
    if(reversed) {
        ph_reverse(h);
        back_lines = old_side(h, &back_n);
        ph_reverse(h);
        if(!back_lines) {
            free(old_lines);
            a->failed = ENOMEM;
            return -1;
        }
    }
    if(!n) {
        /* Nothing to compare: it says where, and that is all. But the lines it adds, there already where they would go, say
           that it went in before. Only there: the same lines elsewhere may be any lines. In a file the patch makes they must
           be all there is */
        if(back_n) {
            ph_reverse(h);
            r = at_fuzz(a, h, back_lines, back_n, 0, 0, &where);
            ph_reverse(h);
            if(r < 0) goto done;
            if(r == 1 && (!a->made || a->t->nlines == back_n)) {
                *reversed = 1;
                r = 0;
                goto done;
            }
        }
        r = guess >= a->done && guess <= a->t->nlines;
        if(r) {
            *at = (unsigned)guess;
            *fuzz = 0;
        }
        goto done;
    }
    /* At each fuzz four looks: forward where it says, the other way round where it says, then each nearby. So a patch that
       went in before is seen at its own place, before a block like it further on can take it a second time */
    more[0] = 1;
    more[1] = back_n != 0;
    for(f = 0; f <= PATCH_FUZZ && (more[0] || more[1]); f++) {
        for(step = 0; step < 4; step++) {
            unsigned back = step & 1;
            if(!more[back]) continue;
            if(back) ph_reverse(h);
            r = at_fuzz(a, h, back ? back_lines : old_lines, back ? back_n : n, f, step >= 2, &where);
            if(back) ph_reverse(h);
            if(r == NO_MORE) {
                more[back] = 0;
                continue;
            }
            if(r < 0) goto done;
            if(r) {
                if(back) {
                    *reversed = 1;
                    r = 0;
                } else {
                    *at = where;
                    *fuzz = f;
                }
                goto done;
            }
        }
    }
    r = 0;
done:
    free(old_lines);
    free(back_lines);
    return r;
}

/**** writing ****/

static int put_bytes(struct papply *a, const void *p, unsigned n) {
    if(a->owe_newline && n) {
        a->owe_newline = 0;
        if(a->put(a->ctx, "\n", 1) < 0) return -1;
    }
    return n ? a->put(a->ctx, p, n) : 0;
}

/* lines from to to of the file, as they are */
static int copy_lines(struct papply *a, unsigned from, unsigned to) {
    static unsigned char *block;
    struct ptarget *t = a->t;
    unsigned off, end;
    if(from >= to) return 0;
    if(!block && !(block = malloc(COPY_BLOCK))) {
        a->failed = ENOMEM;
        return -1;
    }
    off = t->start[from];
    end = t->start[to];
    while(off < end) {
        unsigned want = end - off < COPY_BLOCK ? end - off : COPY_BLOCK;
        long n = dsrc_read(t->src, off, block, want);
        if(n <= 0) {
            a->failed = n < 0 ? errno : EIO;
            return -1;
        }
        if(put_bytes(a, block, (unsigned)n) < 0) return -1;
        off += (unsigned)n;
    }
    if(to == t->nlines) {
        /* a last line with no newline: anything written after it must start a line of its own */
        unsigned char c = 0;
        long n = dsrc_read_full(t->src, end - 1, &c, 1);
        if(n != 1) {
            a->failed = n < 0 ? errno : EIO;
            return -1;
        }
        a->owe_newline = c != '\n';
    }
    return 0;
}

int pa_apply(struct papply *a, const struct phunk *h, unsigned at, unsigned fuzz) {
    struct window w;
    unsigned i, seen = 0, n = 0;
    for(i = 0; i < h->nlines; i++) n += h->lines[i].kind != '+';
    w.first = 0;                                /* with nothing to compare, nothing was left out */
    w.count = n;
    if(n) window_at(h, n, fuzz, &w);
    if(copy_lines(a, a->done, at) < 0) return -1;
    a->done = at;
    for(i = 0; i < h->nlines; i++) {
        const struct pline *l = &h->lines[i];
        if(l->kind == '+') {
            if(put_bytes(a, h->text + l->at, l->len) < 0) return -1;
            if(l->no_newline) a->owe_newline = 1;
            else if(put_bytes(a, "\n", 1) < 0) return -1;
            continue;
        }
        seen++;
        if(seen <= w.first || seen > w.first + w.count) continue;   /* not compared: the file's own line stands, and goes with the rest */
        if(l->kind == ' ' && copy_lines(a, a->done, a->done + 1) < 0) return -1;
        a->done++;
    }
    a->last_offset = (long long)at - w.first - ((long long)h->old_start - 1);
    a->offset = a->last_offset;
    a->last_line = (long long)at - w.first + 1 + a->added;
    a->added += (long long)h->new_count - h->old_count;
    return 0;
}

int pa_finish(struct papply *a) {
    if(copy_lines(a, a->done, a->t->nlines) < 0) return -1;
    a->done = a->t->nlines;
    a->owe_newline = 0;                         /* what was owed is what the patch said: the file ends with no newline */
    return 0;
}

void pa_start(struct papply *a, struct ptarget *t, pa_put put, void *ctx) {
    memset(a, 0, sizeof(*a));
    a->t = t;
    a->put = put;
    a->ctx = ctx;
}

/**** a rejected hunk ****/

static int put_text(pa_put put, void *ctx, const char *s) {
    return put(ctx, s, (unsigned)strlen(s));
}

int pr_write(const struct phunk *h, pa_put put, void *ctx) {
    char head[80];
    unsigned i;
    snprintf(head, sizeof(head), "@@ -%u,%u +%u,%u @@\n", h->old_count ? h->old_start : h->old_start - 1, h->old_count,
             h->new_count ? h->new_start : h->new_start - 1, h->new_count);
    if(put_text(put, ctx, head) < 0) return -1;
    for(i = 0; i < h->nlines; i++) {
        const struct pline *l = &h->lines[i];
        char kind = l->kind;
        if(put(ctx, &kind, 1) < 0) return -1;
        if(l->len && put(ctx, h->text + l->at, l->len) < 0) return -1;
        if(put(ctx, "\n", 1) < 0) return -1;
        if(l->no_newline && put_text(put, ctx, PATCH_NO_NEWLINE) < 0) return -1;
    }
    return 0;
}
