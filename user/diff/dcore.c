/*
SurfOS user programs: which lines of two sequences changed
--------------------
File: dcore.c   Date: 11/01/26
--------------------
See dcore.h. x counts through a box's lines of a, y through its lines of b, and a diagonal k is the points with x - y = k. An
edit moves a point to the next diagonal; equal lines carry it along its own for nothing.

Section 4b of the paper runs the basic algorithm from one corner, and the same algorithm on the reversed sequences from the other.
So here there is one search, wave_edit(), and two waves, each with coordinates of its own in which it starts at (0, 0): the
one from the far corner just reads its lines backwards. A wave records, for each diagonal, the furthest x it has reached; a
point that would leave the box is not a point at all. The paper's overlap test is then one line: on the box's diagonal k the
other wave's diagonal is (w - h) - k, and they have met when their two x together reach w.

When the cap stops a search, nothing says which point the shortest edit goes through, and the cut is a guess. Two rules make
it, chosen by measuring the edits they give on files of several shapes (tests/host/cli/measure_dcore.c; the account of this
track has the tables), and a fallback for when the waves hold nothing to go by:

  - Where the waves reached common diagonals, take those on which each wave's point is well on its way, with four fifths or more
    of the progress of that wave's furthest point, and cut at both points of the one where the waves stopped nearest each
    other. Each corner then holds a part whose edit is known to be within the cap, and between them is the part still unknown.
    Without this rule a file against its sorted copy came out at twice the shortest edit, as its shortest edit leaves the
    course below far behind. The larger the part asked for, the less often the rule cuts and the better drifting files do;
    from seven eighths up the sorted file of 100,000 lines was no longer exact. Four fifths leaves room on both sides.
  - Otherwise, a point that has kept to its course: the straight line between the box's corners. Inside rewritten text nothing
    matches, every point has come about as far, and how far says nothing. A wave that goes by that alone wanders, and passes the
    lines it should have matched for good. So a point is valued at four times its progress, less how far it has strayed from
    that line at the distance it has come. Each wave's best is taken, both when they leave a box between them, or else the
    better of the two.
  - Before both, when neither wave met a single line of the other file, cut_at_far_match(): a block moved further than the cap
    leaves the waves with nothing to choose by, and the rules above then cut a cap's worth of lines away at a time (a block of
    4,001 lines moved down 40,000 gave 9 times the shortest edit). Such a block shows at the box's edges instead.

Why this keeps the time: what a wave does sliding along its diagonals is at most its diagonals times the furthest it got, so a
cut must take a fair part of that out of the box. The first rule's points have four fifths of their waves' progress, and the
parts at the corners are within the cap. In the second a point's stray is at most twice its progress, so its value is between
two and four times that, and the point taken has at least half the progress of its own wave's furthest, and of the other's when
it is taken alone. The fallback's cut is further than the cap from its corner, or a wave would have met that line, and its scan
is one pass over the box. So every cut pays for its search, and the total is proportional to the cap times the lines.

SEARCH_CAP: 4000, measured from 1000 to 8000. At 4000 every shape was within half a point of the best that any cap up to 6000
gave: moved blocks within 2% of the shortest, drifting files within 13%, the sorted file 19%. A larger cap costs time in
proportion, and only 8000, at twice the time, did better, by less than a point; a smaller one loses on moved blocks and drifting
files.
*/

#include "dcore.h"
#include <limits.h>
#include <stdlib.h>

#define SEARCH_CAP 4000
#define SMALLEST_CAP 64                 /* short of memory, the search goes on no smaller than this */
#define MOST_LINES 0x20000000u          /* the arithmetic of a point's value stays within 64 bits */
#define WELL_ON 4                       /* the first rule: a point is well on its way with 4/5 of its wave's best progress */
#define WELL_ON_OF 5
#define WAITING 64                      /* parts of boxes: the smallest of each cut comes next, so sizes fall by a factor at each */
#define NOT_MET INT_MAX

struct wave {
    int *reach;                         /* by diagonal: the furthest x, in this wave's coordinates; -1 where it has no point */
    const unsigned *a, *b;              /* the box's first lines as this wave sees them */
    int way;                            /* +1 or -1: how it goes through them */
    int low, high;                      /* the diagonals of its last edit */
    int before_low, before_high;        /* and of the one before */
    int zero;                           /* reach[zero] is diagonal 0 */
    int matched;                        /* it has met a line the other file has */
};

struct area {
    int x0, x1, y0, y1;
};

struct cut {
    int points;                         /* 1 or 2; 0: nothing to go by */
    int x[2], y[2];                     /* in order along the box */
};

struct search {
    const unsigned *a, *b;
    int *forward, *backward;
    int cap;
};

#define REACH(v, k) ((v)->reach[(k) + (v)->zero])

/* The wave's next edit: each diagonal it can reach with one more goes to its furthest point, and on along the lines that agree.
   w and h are the box's size. other: the wave from the far corner, when this edit is the one that can meet it; then the
   result is the diagonal they met on and *met the x there. NOT_MET otherwise. */
static int wave_edit(struct wave *v, const struct wave *other, int d, int w, int h, int *met) {
    int low = d > h ? -h + ((d + h) & 1) : -d, high = d > w ? w - ((d + w) & 1) : d, k;
    for(k = low; k <= high; k += 2) {
        int x = -1, y, from;
        if(k - 1 >= v->low && k - 1 <= v->high && (from = REACH(v, k - 1)) >= 0 && from < w) x = from + 1;
        if(k + 1 >= v->low && k + 1 <= v->high && (from = REACH(v, k + 1)) >= 0 && from - (k + 1) < h && from > x) x = from;
        REACH(v, k) = x;
        if(x < 0) continue;
        y = x - k;
        while(x < w && y < h && v->a[x * v->way] == v->b[y * v->way]) {
            x++;
            y++;
        }
        if(x > REACH(v, k)) v->matched = 1;
        REACH(v, k) = x;
        if(other) {
            int theirs = (w - h) - k;
            if(theirs >= other->low && theirs <= other->high && REACH(other, theirs) >= 0 && x + REACH(other, theirs) >= w) {
                *met = x;
                return k;
            }
        }
    }
    v->before_low = v->low;
    v->before_high = v->high;
    v->low = low;
    v->high = high;
    return NOT_MET;
}

/* a wave's point on diagonal k, in the coordinates of the whole */
static void real_point(const struct wave *v, const struct area *q, int k, int *px, int *py) {
    int x = REACH(v, k);
    *px = v->way > 0 ? q->x0 + x : q->x1 - x;
    *py = v->way > 0 ? q->y0 + x - k : q->y1 - (x - k);
}

/* how far a wave got: the most progress of its points on diagonals low to high */
static long long furthest_gone(const struct wave *v, int low, int high) {
    long long top = 0;
    int k;
    for(k = low; k <= high; k += 2)
        if(REACH(v, k) >= 0 && 2LL * REACH(v, k) - k > top) top = 2LL * REACH(v, k) - k;
    return top;
}

/* the first rule: of the common diagonals on which each wave's point is well on its way, the one where they stopped nearest each
   other. When the box's sides differ by an odd number the waves' last edits fall on diagonals of different parity, and r's edit
   before its last is the one to use */
static int cut_where_near(const struct wave *f, const struct wave *r, const struct area *q, struct cut *c) {
    int w = q->x1 - q->x0, h = q->y1 - q->y0, odd = (w - h) & 1, k, at = 0, gap = INT_MAX;
    int low = odd ? r->before_low : r->low, high = odd ? r->before_high : r->high;
    long long f_top = furthest_gone(f, f->low, f->high), r_top = furthest_gone(r, low, high);
    for(k = f->low; k <= f->high; k += 2) {
        int theirs = (w - h) - k, x = REACH(f, k), there;
        if(x < 0 || theirs < low || theirs > high || (there = REACH(r, theirs)) < 0) continue;
        if(WELL_ON_OF * (2LL * x - k) < WELL_ON * f_top || WELL_ON_OF * (2LL * there - theirs) < WELL_ON * r_top) continue;
        if(w - x - there < gap) {
            gap = w - x - there;
            at = k;
        }
    }
    if(gap == INT_MAX) return 0;
    real_point(f, q, at, &c->x[0], &c->y[0]);
    c->x[1] = q->x1 - REACH(r, (w - h) - at);
    c->y[1] = q->y1 - (REACH(r, (w - h) - at) - ((w - h) - at));
    c->points = 2;
    return 1;
}

/* what a point is worth, in 1/(w + h) of a line: four times how far it has come, less how far it has strayed from the course. The
   course at distance gone is on diagonal gone * (w - h) / (w + h) */
static long long point_value(int x, int k, int w, int h) {
    long long gone = 2LL * x - k, size = (long long)w + h, stray = k * size - gone * (w - h);
    return 4 * gone * size - (stray < 0 ? -stray : stray);
}

/* the wave's best point: its diagonal, and 1; or 0 when it has none */
static int best_on_course(const struct wave *v, int w, int h, int *at, long long *value) {
    int k, any = 0;
    for(k = v->low; k <= v->high; k += 2) {
        long long worth;
        if(REACH(v, k) < 0) continue;
        worth = point_value(REACH(v, k), k, w, h);
        if(!any || worth > *value) {
            *value = worth;
            *at = k;
            any = 1;
        }
    }
    return any;
}

/* the second rule: each wave's best point, both when they leave a box between them, or else the better */
static void cut_on_course(const struct wave *f, const struct wave *r, const struct area *q, struct cut *c) {
    int w = q->x1 - q->x0, h = q->y1 - q->y0, kf = 0, kr = 0, fx = 0, fy = 0, rx = 0, ry = 0;
    long long vf = 0, vr = 0;
    int have_f = best_on_course(f, w, h, &kf, &vf), have_r = best_on_course(r, w, h, &kr, &vr);
    c->points = 0;
    if(have_f) real_point(f, q, kf, &fx, &fy);
    if(have_r) real_point(r, q, kr, &rx, &ry);
    if(have_f && have_r && fx <= rx && fy <= ry) {
        c->x[0] = fx;
        c->y[0] = fy;
        c->x[1] = rx;
        c->y[1] = ry;
        c->points = 2;
    } else if(have_f && (!have_r || vf >= vr)) {
        c->x[0] = fx;
        c->y[0] = fy;
        c->points = 1;
    } else if(have_r) {
        c->x[0] = rx;
        c->y[0] = ry;
        c->points = 1;
    }
}

/* how many lines agree from (x, y) on, inside the box: forwards, or with way -1 backwards, from the lines before it */
static int run_from(const struct search *s, const struct area *q, int x, int y, int way) {
    int n = 0;
    if(way > 0)
        while(x + n < q->x1 && y + n < q->y1 && s->a[x + n] == s->b[y + n]) n++;
    else
        while(x - n > q->x0 && y - n > q->y0 && s->a[x - n - 1] == s->b[y - n - 1]) n++;
    return n;
}

/* The fallback when neither wave met a single line the other file has: what the files share starts further than the cap
   from both corners, and the waves hold nothing to choose by. That is what a large block moved looks like: the box's first
   line of one file is further down the other, past the block, or its last line further up. Each of those four is looked
   for, its nearest place only, and taken when the lines that agree from there are at least as many as the edits it takes to
   get there; of those, the one nearest its corner. The cut is there, and the part before it is the edits. 0: none */
static int cut_at_far_match(const struct search *s, const struct area *q, struct cut *c) {
    int w = q->x1 - q->x0, h = q->y1 - q->y0, best = INT_MAX, i;
    for(i = 1; i < w && i < best; i++)          /* b's first line further down a */
        if(s->a[q->x0 + i] == s->b[q->y0]) {
            if(run_from(s, q, q->x0 + i, q->y0, 1) >= i) {
                best = i;
                c->x[0] = q->x0 + i;
                c->y[0] = q->y0;
            }
            break;
        }
    for(i = 1; i < h && i < best; i++)          /* a's first line further down b */
        if(s->b[q->y0 + i] == s->a[q->x0]) {
            if(run_from(s, q, q->x0, q->y0 + i, 1) >= i) {
                best = i;
                c->x[0] = q->x0;
                c->y[0] = q->y0 + i;
            }
            break;
        }
    for(i = 1; i < w && i < best; i++)          /* b's last line further up a */
        if(s->a[q->x1 - 1 - i] == s->b[q->y1 - 1]) {
            if(run_from(s, q, q->x1 - i, q->y1, -1) >= i) {
                best = i;
                c->x[0] = q->x1 - i;
                c->y[0] = q->y1;
            }
            break;
        }
    for(i = 1; i < h && i < best; i++)          /* a's last line further up b */
        if(s->b[q->y1 - 1 - i] == s->a[q->x1 - 1]) {
            if(run_from(s, q, q->x1, q->y1 - i, -1) >= i) {
                best = i;
                c->x[0] = q->x1;
                c->y[0] = q->y1 - i;
            }
            break;
        }
    if(best == INT_MAX) return 0;
    c->points = 1;
    return 1;
}

/* where to cut the box, nothing being equal at either of its ends */
static void find_cut(const struct search *s, const struct area *q, struct cut *c) {
    struct wave f, r;
    int w = q->x1 - q->x0, h = q->y1 - q->y0, odd = (w - h) & 1, d, k, x;
    f.reach = s->forward;
    r.reach = s->backward;
    f.zero = r.zero = s->cap + 1;
    f.a = s->a + q->x0;
    f.b = s->b + q->y0;
    f.way = 1;
    r.a = s->a + q->x1 - 1;
    r.b = s->b + q->y1 - 1;
    r.way = -1;
    f.low = f.high = f.before_low = f.before_high = f.matched = 0;
    r.low = r.high = r.before_low = r.before_high = r.matched = 0;
    REACH(&f, 0) = REACH(&r, 0) = 0;
    c->points = 1;
    for(d = 1; d <= s->cap; d++) {
        /* a forward path of d edits can meet a reverse one of d - 1 only when the sides differ by an odd number, and one
           of d only when they differ by an even number: the paper's two tests */
        k = wave_edit(&f, odd ? &r : NULL, d, w, h, &x);
        if(k != NOT_MET) {
            c->x[0] = q->x0 + x;
            c->y[0] = q->y0 + x - k;
            return;
        }
        k = wave_edit(&r, odd ? NULL : &f, d, w, h, &x);
        if(k != NOT_MET) {
            c->x[0] = q->x1 - x;
            c->y[0] = q->y1 - (x - k);
            return;
        }
    }
    if(!f.matched && !r.matched && cut_at_far_match(s, q, c)) return;
    if(!cut_where_near(&f, &r, q, c)) cut_on_course(&f, &r, q, c);
}

static void mark(unsigned char *m, int from, int to) {
    for(; from < to; from++) m[from] = 1;
}

static void solve(const struct search *s, unsigned char *ma, unsigned char *mb, int n, int m) {
    struct area waiting[WAITING], q = { 0, n, 0, m };
    int top = 0;
    for(;;) {
        while(q.x0 < q.x1 && q.y0 < q.y1 && s->a[q.x0] == s->b[q.y0]) {
            q.x0++;
            q.y0++;
        }
        while(q.x0 < q.x1 && q.y0 < q.y1 && s->a[q.x1 - 1] == s->b[q.y1 - 1]) {
            q.x1--;
            q.y1--;
        }
        if(q.x0 < q.x1 && q.y0 < q.y1) {
            struct area part[3];
            struct cut c;
            int parts = 0, i, sx = q.x0, sy = q.y0, smallest = 0;
            find_cut(s, &q, &c);
            for(i = 0; i <= c.points; i++) {    /* the parts that hold anything */
                int ex = i < c.points ? c.x[i] : q.x1, ey = i < c.points ? c.y[i] : q.y1;
                if(ex > sx || ey > sy) {
                    part[parts].x0 = sx;
                    part[parts].x1 = ex;
                    part[parts].y0 = sy;
                    part[parts++].y1 = ey;
                }
                sx = ex;
                sy = ey;
            }
            /* one part is no cut, which should not be; and the stack cannot fill, by the sizes. Either way this is a correct edit, and it ends */
            if(parts > 1 && top + parts - 1 <= WAITING) {
                for(i = 1; i < parts; i++) {
                    if(part[i].x1 - part[i].x0 + part[i].y1 - part[i].y0 < part[smallest].x1 - part[smallest].x0 + part[smallest].y1 - part[smallest].y0)
                        smallest = i;
                }
                for(i = 0; i < parts; i++)
                    if(i != smallest) waiting[top++] = part[i];
                q = part[smallest];
                continue;
            }
        }
        mark(ma, q.x0, q.x1);
        mark(mb, q.y0, q.y1);
        if(!top) return;
        q = waiting[--top];
    }
}

/* An edit that only deletes or only inserts can often stand in several places: a a a with one a gone. Each is moved
   as far down as it will go, where it may join the next, so that repeated lines give the same hunks every time. An edit
   that does both stays where it is: moving one half would make two of it. Moving keeps the edit correct: the line that
   leaves the change equals the one that joins it, and takes over its partner. */
static void slide(const unsigned *a, unsigned n, unsigned char *ma, const unsigned *b, unsigned m, unsigned char *mb) {
    unsigned i = 0, j = 0;
    for(;;) {
        unsigned i0, j0;
        while(i < n && j < m && !ma[i] && !mb[j]) {
            i++;
            j++;
        }
        i0 = i;
        j0 = j;
        while(i < n && ma[i]) i++;
        while(j < m && mb[j]) j++;
        if(i == i0 && j == j0) return;
        if(j == j0) {
            while(i < n && j < m && !ma[i] && !mb[j] && a[i0] == a[i]) {
                ma[i0++] = 0;
                ma[i++] = 1;
                j++;
                while(i < n && ma[i]) i++;
            }
        } else if(i == i0) {
            while(i < n && j < m && !ma[i] && !mb[j] && b[j0] == b[j]) {
                mb[j0++] = 0;
                mb[j++] = 1;
                i++;
                while(j < m && mb[j]) j++;
            }
        }
    }
}

int dc_compare_limit(const unsigned *a, unsigned n, const unsigned *b, unsigned m, unsigned char *ma, unsigned char *mb,
                     unsigned limit) {
    struct search s;
    size_t cells;
    if(n > MOST_LINES || m > MOST_LINES) return -1;
    if(limit > n + m + 1) limit = n + m + 1;
    for(;; limit /= 2) {                        /* short of memory, a smaller search still gives a correct edit */
        cells = 2 * (size_t)limit + 3;
        s.forward = malloc(cells * sizeof(int));
        s.backward = malloc(cells * sizeof(int));
        if(s.forward && s.backward) break;
        free(s.forward);
        free(s.backward);
        if(limit < SMALLEST_CAP) return -1;
    }
    s.a = a;
    s.b = b;
    s.cap = (int)limit;
    solve(&s, ma, mb, (int)n, (int)m);
    free(s.forward);
    free(s.backward);
    slide(a, n, ma, b, m, mb);
    return 0;
}

int dc_compare(const unsigned *a, unsigned n, const unsigned *b, unsigned m, unsigned char *ma, unsigned char *mb) {
    return dc_compare_limit(a, n, b, m, ma, mb, SEARCH_CAP);
}
