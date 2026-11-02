/*
SurfOS user programs: comparing two files
--------------------
File: dfile.c   Date: 11/01/26
--------------------
See diff.h. -i and -b and -w change only what two lines are compared as: norm_next() gives a line's characters as
the options see them, and the hash and the last check both go through it, so they cannot disagree.
*/

#include "diff.h"
#include "dcore.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define RING_MAX 65536                  /* more context than this: nothing at the start is passed over */
#define INCOMPLETE 0x9E3779B9u          /* a last line without a newline is not the same line as one with it */

#define IS_SPACE(c) ((c) == ' ' || ((c) >= 9 && (c) <= 13 && (c) != '\n'))
#define IS_UPPER(c) ((c) >= 'A' && (c) <= 'Z')

/**** the options' view of a line ****/

struct nstate {
    int held;                           /* -b: the character after a space, to give next */
    int pending;                        /* -b: white space not given yet, and not given at the line's end */
    int any;                            /* the line has bytes */
    int incomplete;                     /* it ended without a newline */
};

static void norm_start(struct nstate *st) {
    st->held = -1;
    st->pending = st->any = st->incomplete = 0;
}

/* the line's next character as the options see it, -1 at its end */
static int norm_next(struct dreader *r, const struct dopts *o, struct nstate *st) {
    int c = st->held;
    if(c >= 0) st->held = -1;
    else for(;;) {
        c = dr_getc(r);
        if(c < 0) {
            st->incomplete = st->any;
            return -1;
        }
        if(c == '\n') {
            st->any = 1;
            return -1;
        }
        st->any = 1;
        if(o->space && IS_SPACE(c)) {
            if(o->space == 'b') st->pending = 1;
            continue;
        }
        if(st->pending) {
            st->pending = 0;
            st->held = c;
            return ' ';
        }
        break;
    }
    return o->icase && IS_UPPER(c) ? c + 32 : c;
}

/**** what the two files start and end with ****/

/* where the first difference is. p: the offset of it; line: the offset of its line's start; 1 they are the same,
   0 they are not, -1 a read failed (s->failed_side) */
static int common_start(struct dscript *s, const struct dopts *o, unsigned char *ba, unsigned char *bb, unsigned *line_at,
                        int *binary) {
    struct dside *a = &s->a, *b = &s->b;
    unsigned keep = o->style == 'n' ? 0 : o->context, off = 0, line = 0, line_start = 0, *ring = NULL;
    int first_block = 1;
    if(keep < RING_MAX) {
        ring = malloc(((size_t)keep + 1) * sizeof(*ring));
        if(!ring) {
            s->failed_side = -1;
            errno = ENOMEM;
            return -1;
        }
        ring[0] = 0;
    }
    for(;;) {
        long na = dsrc_read_full(a->src, off, ba, DR_BLOCK), nb;
        unsigned common, k = 0;
        const unsigned char *p, *nl, *end;
        if(na < 0) {
            s->failed_side = 0;
            free(ring);
            return -1;
        }
        if((nb = dsrc_read_full(b->src, off, bb, DR_BLOCK)) < 0) {
            s->failed_side = 1;
            free(ring);
            return -1;
        }
        if(first_block) {
            *binary = (na && memchr(ba, 0, (size_t)na)) || (nb && memchr(bb, 0, (size_t)nb));
            first_block = 0;
        }
        common = (unsigned)(na < nb ? na : nb);
        if(memcmp(ba, bb, common))
            while(ba[k] == bb[k]) k++;
        else k = common;
        for(p = ba, end = ba + k; p < end && (nl = memchr(p, '\n', (size_t)(end - p))); p = nl + 1) {
            line++;
            line_start = off + (unsigned)(nl - ba) + 1;
            if(ring) ring[line % (keep + 1)] = line_start;
        }
        if(k < common || na != nb) {
            *line_at = line_start;
            a->first = b->first = 0;
            a->start = b->start = 0;
            if(ring && line >= keep) {
                a->first = b->first = line - keep;
                a->start = b->start = ring[(line - keep) % (keep + 1)];
            }
            free(ring);
            return 0;
        }
        if(!na) {
            free(ring);
            return 1;
        }
        off += common;
    }
}

/* a read that must give n bytes */
static int read_exact(struct dscript *s, int side, unsigned off, void *buf, unsigned n) {
    long r = dsrc_read_full(side ? s->b.src : s->a.src, off, buf, n);
    if(r == (long)n) return 0;
    if(r >= 0) errno = EIO;                     /* it got shorter */
    s->failed_side = side;
    return -1;
}

/* the bytes both files end with, from the end, and no further in than most: how many, or -1 */
static long same_tail(struct dscript *s, unsigned char *ba, unsigned char *bb, unsigned most) {
    unsigned sa = s->a.src->size, sb = s->b.src->size, tail = 0;
    while(tail < most) {
        unsigned n = most - tail < DR_BLOCK ? most - tail : DR_BLOCK, i;
        if(read_exact(s, 0, sa - tail - n, ba, n) < 0 || read_exact(s, 1, sb - tail - n, bb, n) < 0) return -1;
        for(i = n; i && ba[i - 1] == bb[i - 1]; i--);
        tail += n - i;
        if(i) break;
    }
    return (long)tail;
}

/* The files' common end, as far as lines allow: a->end and b->end are set before it. from is where the line of the first
   difference starts: nothing before that can be in it. Context for the last change stays in, as s->tail lines. */
static int common_end(struct dscript *s, const struct dopts *o, unsigned char *ba, unsigned char *bb, unsigned from) {
    struct dside *a = &s->a, *b = &s->b;
    unsigned sa = a->src->size, sb = b->src->size, pa, pb, lines = 0, keep = o->style == 'n' ? 0 : o->context, tail;
    unsigned char before = '\n';
    struct dreader r;
    long same;
    int c, at_line_start;
    a->end = sa;
    b->end = sb;
    if(sa > 0x7FFFFFFFu || sb > 0x7FFFFFFFu) return 0;  /* backwards is a seek a block, and lseek stops at 2 GB */
    same = same_tail(s, ba, bb, (sa < sb ? sa : sb) - from);
    if(same <= 0) return same < 0 ? -1 : 0;
    tail = (unsigned)same;
    pa = sa - tail;
    pb = sb - tail;
    /* the same bytes may begin in the middle of a line: only a whole line, in both files, can be passed over */
    if(pa > from && read_exact(s, 0, pa - 1, &before, 1) < 0) return -1;
    at_line_start = before == '\n';
    before = '\n';
    if(pb > from && read_exact(s, 1, pb - 1, &before, 1) < 0) return -1;
    at_line_start = at_line_start && before == '\n';
    if(dr_start(&r, a->src, pa, sa) < 0) return -1;
    c = '\n';
    if(!at_line_start) while((c = dr_getc(&r)) >= 0 && c != '\n');
    if(c >= 0) {
        while(lines < keep && dr_skip_line(&r) >= 0) lines++;
        a->end = dr_tell(&r);
        b->end = pb + (a->end - pa);
        s->tail = a->end < sa ? lines : 0;
    }
    c = r.failed;
    dr_stop(&r);
    if(c) {
        errno = c;
        s->failed_side = 0;
        return -1;
    }
    return 0;
}

/**** the lines between ****/

/* room for a line more: by half again, so that the last growth needs no more than it must */
static int grow(struct dside *d) {
    unsigned cap = d->cap ? d->cap + d->cap / 2 : 1024;
    unsigned *h;
    unsigned char *f;
    if(cap >= 0x3FFFFFFFu) return -1;
    if(!(h = realloc(d->hash, (size_t)cap * sizeof(*h)))) return -1;
    d->hash = h;
    if(!(f = realloc(d->flag, cap))) return -1;
    d->flag = f;
    d->cap = cap;
    return 0;
}

static int index_lines(struct dside *d, const struct dopts *o) {
    struct dreader r;
    int e;
    if(dr_start(&r, d->src, d->start, d->end) < 0) return -1;
    for(;;) {
        struct nstate st;
        unsigned h = DH_START;
        int c, blank = 1;
        norm_start(&st);
        while((c = norm_next(&r, o, &st)) >= 0) {
            h = DH_ADD(h, c);
            blank = 0;
        }
        if(!st.any) break;
        if(st.incomplete && !o->space) h = (h ^ INCOMPLETE) * 16777619u;
#ifdef D_HASH_MASK                      /* tests/host: lines made to collide, for the last check to catch */
        h &= D_HASH_MASK;
#endif
        if(d->n == d->cap && grow(d) < 0) {
            dr_stop(&r);
            errno = ENOMEM;
            return -1;
        }
        d->hash[d->n] = h;
        d->flag[d->n++] = blank ? D_BLANK : 0;
    }
    e = r.failed;
    dr_stop(&r);
    if(e) {
        errno = e;
        return -1;
    }
    return 0;
}

/* lines whose hash is not in the other file are changed for certain, and leave only the others to compare: which
   with two files of nothing in common is all the work there is. A table of bits for each hash, so that the
   hashes need no sorting and it takes little; a hash that shares its place just stays in. No memory: all stay in. */
static void drop_unmatched(struct dside *a, struct dside *b, unsigned *na, unsigned *nb) {
    size_t cells = 1024;
    unsigned char *seen;
    unsigned i, k;
    *na = a->n;
    *nb = b->n;
    while(cells < 2 * ((size_t)a->n + b->n) && cells < 0x10000000u) cells *= 2;
    seen = calloc(cells, 1);
    if(!seen) return;
#define PLACE(h) (((h) ^ ((h) >> 15)) & (unsigned)(cells - 1))
    for(i = 0; i < a->n; i++) seen[PLACE(a->hash[i])] |= 1;
    for(i = 0; i < b->n; i++) seen[PLACE(b->hash[i])] |= 2;
    for(i = k = 0; i < a->n; i++) {
        if(seen[PLACE(a->hash[i])] & 2) a->hash[k++] = a->hash[i];
        else a->flag[i] |= D_CHANGED;
    }
    *na = k;
    for(i = k = 0; i < b->n; i++) {
        if(seen[PLACE(b->hash[i])] & 1) b->hash[k++] = b->hash[i];
        else b->flag[i] |= D_CHANGED;
    }
    *nb = k;
    free(seen);
}

/* the marks of the lines left, back on the lines */
static void take_marks(struct dside *d, const unsigned char *marks) {
    unsigned i, k = 0;
    for(i = 0; i < d->n; i++) {
        if(d->flag[i] & D_CHANGED) continue;
        if(marks[k++]) d->flag[i] |= D_CHANGED;
    }
}

static int find_changes(struct dscript *s) {
    unsigned char *ma, *mb;
    unsigned na, nb;
    int r;
    drop_unmatched(&s->a, &s->b, &na, &nb);
    ma = calloc((size_t)na + 1, 1);
    mb = calloc((size_t)nb + 1, 1);
    /* The context kept after the last change is the same in both and stays out of it: left in, a change could slide
       down over it (dcore.c), and a hunk in the middle of a file would look like one at its end. Those lines
       exist in both files, so they are among the last that drop_unmatched() left. */
    if(!ma || !mb) r = -1;
    else if(s->tail > na || s->tail > nb) r = -1;
    else r = dc_compare(s->a.hash, na - s->tail, s->b.hash, nb - s->tail, ma, mb);
    if(!r) {
        take_marks(&s->a, ma);
        take_marks(&s->b, mb);
    }
    free(ma);
    free(mb);
    if(r) errno = ENOMEM;
    free(s->a.hash);                    /* they have done their work */
    free(s->b.hash);
    s->a.hash = s->b.hash = NULL;
    return r;
}

/**** the last check: lines called equal, byte by byte ****/

static int same_line(struct dreader *ra, struct dreader *rb, const struct dopts *o) {
    struct nstate sa, sb;
    int ca, cb;
    norm_start(&sa);
    norm_start(&sb);
    do {
        ca = norm_next(ra, o, &sa);
        cb = norm_next(rb, o, &sb);
    } while(ca == cb && ca >= 0);
    if(ca == cb) return o->space || sa.incomplete == sb.incomplete;
    while(ca >= 0) ca = norm_next(ra, o, &sa);
    while(cb >= 0) cb = norm_next(rb, o, &sb);
    return 0;
}

static int check_pairs(struct dscript *s, const struct dopts *o) {
    struct dside *a = &s->a, *b = &s->b;
    struct dreader ra, rb;
    unsigned i = 0, j = 0;
    int e;
    if(dr_start(&ra, a->src, a->start, a->end) < 0) return -1;
    if(dr_start(&rb, b->src, b->start, b->end) < 0) {
        dr_stop(&ra);
        return -1;
    }
    while(i < a->n && j < b->n) {
        if(a->flag[i] & D_CHANGED) {
            dr_skip_line(&ra);
            i++;
        } else if(b->flag[j] & D_CHANGED) {
            dr_skip_line(&rb);
            j++;
        } else {
            if(!same_line(&ra, &rb, o)) {
                a->flag[i] |= D_CHANGED;
                b->flag[j] |= D_CHANGED;
            }
            i++;
            j++;
        }
        if(ra.failed || rb.failed) break;
    }
    s->failed_side = ra.failed ? 0 : 1;
    e = ra.failed ? ra.failed : rb.failed;
    dr_stop(&ra);
    dr_stop(&rb);
    if(e) {
        errno = e;
        return -1;
    }
    return 0;
}

/**** all together ****/

int diff_compare(struct dsrc *a, struct dsrc *b, const struct dopts *o, struct dscript *s) {
    unsigned char *ba, *bb;
    unsigned line_at = 0;
    int binary = 0, r;
    memset(s, 0, sizeof(*s));
    s->a.src = a;
    s->b.src = b;
    s->failed_side = -1;
    ba = malloc(DR_BLOCK);
    bb = malloc(DR_BLOCK);
    if(!ba || !bb) {
        free(ba);
        free(bb);
        errno = ENOMEM;
        return DIFF_FAILED;
    }
    r = common_start(s, o, ba, bb, &line_at, &binary);
    if(!r && !binary && !(o->brief && !o->icase && !o->space && !o->blanks)) r = common_end(s, o, ba, bb, line_at) ? -1 : 2;
    free(ba);
    free(bb);
    if(r < 0) return DIFF_FAILED;
    if(r == 1) return DIFF_SAME;
    if(binary) return DIFF_BINARY;
    if(!r) return DIFF_DIFFER;                  /* -q: that byte is enough */
    s->failed_side = 0;
    if(index_lines(&s->a, o) < 0) return DIFF_FAILED;
    s->failed_side = 1;
    if(index_lines(&s->b, o) < 0) return DIFF_FAILED;
    s->failed_side = -1;
    if(find_changes(s) < 0) return DIFF_FAILED;
    if(check_pairs(s, o) < 0) return DIFF_FAILED;
    return diff_visible(s, o) ? DIFF_DIFFER : DIFF_SAME;
}

void diff_free(struct dscript *s) {
    free(s->a.hash);
    free(s->b.hash);
    free(s->a.flag);
    free(s->b.flag);
    memset(s, 0, sizeof(*s));
}
