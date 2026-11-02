/*
SurfOS user programs: printing an edit
--------------------
File: dout.c    Date: 11/01/26
--------------------
The three ways of writing what dfile.c found: normal, unified (-u) and context (-c). Everything needed
to lay a hunk out is in the flags; only then are the lines read, each file forward once.
A change is a run of lines of one file against a run of the other; a group is changes near enough
to share their context, and -B leaves out a group of only blank lines.
*/

#include "diff.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define NO_NEWLINE "\\ No newline at end of file\n"

struct change {
    unsigned a0, a1, b0, b1;            /* lines a0 to a1 of the first file against b0 to b1 of the second */
};

struct group {
    struct change first, last;
    unsigned count;
    int visible;
};

/* a reader that knows its line */
struct liner {
    struct dreader r;
    unsigned line;
};

/* the next change from lines i and j: 0 when there is none */
static int next_change(const struct dscript *s, unsigned *i, unsigned *j, struct change *c) {
    const unsigned char *fa = s->a.flag, *fb = s->b.flag;
    unsigned x = *i, y = *j;
    while(x < s->a.n && y < s->b.n && !(fa[x] & D_CHANGED) && !(fb[y] & D_CHANGED)) {
        x++;
        y++;
    }
    c->a0 = x;
    c->b0 = y;
    while(x < s->a.n && (fa[x] & D_CHANGED)) x++;
    while(y < s->b.n && (fb[y] & D_CHANGED)) y++;
    c->a1 = x;
    c->b1 = y;
    *i = x;
    *j = y;
    return x != c->a0 || y != c->b0;
}

/* -B: nothing but blank lines */
static int ignorable(const struct dscript *s, const struct dopts *o, const struct change *c) {
    unsigned i;
    if(!o->blanks) return 0;
    for(i = c->a0; i < c->a1; i++)
        if(!(s->a.flag[i] & D_BLANK)) return 0;
    for(i = c->b0; i < c->b1; i++)
        if(!(s->b.flag[i] & D_BLANK)) return 0;
    return 1;
}

/* the group that starts at lines i and j; normal diffs have one change to a group */
static int next_group(const struct dscript *s, const struct dopts *o, unsigned *i, unsigned *j, struct group *g) {
    struct change c;
    int last_ignored;
    if(!next_change(s, i, j, &c)) return 0;
    g->first = g->last = c;
    g->count = 1;
    last_ignored = ignorable(s, o, &c);
    g->visible = !last_ignored;
    if(o->style == 'n') return 1;
    for(;;) {
        unsigned ni = *i, nj = *j;
        unsigned long long gap, reach;
        int ignored;
        if(!next_change(s, &ni, &nj, &c)) return 1;
        ignored = ignorable(s, o, &c);
        gap = c.a0 - g->last.a1;
        /* two changes share a hunk when what each shows around itself meets; a blank change shows nothing of
           its own, only what the other shows around itself reaches it */
        reach = ignored ? (unsigned long long)o->context : last_ignored ? (unsigned long long)o->context + 1
                                                                       : 2 * (unsigned long long)o->context + 1;
        if(gap >= reach) return 1;
        *i = ni;
        *j = nj;
        g->last = c;
        g->count++;
        last_ignored = ignored;
        if(!ignored) g->visible = 1;
    }
}

int diff_visible(const struct dscript *s, const struct dopts *o) {
    unsigned i = 0, j = 0;
    struct group g;
    while(next_group(s, o, &i, &j, &g))
        if(g.visible) return 1;
    return 0;
}

/**** the lines ****/

static void to_line(struct liner *l, unsigned line) {
    while(l->line < line) {
        if(dr_skip_line(&l->r) < 0) return;
        l->line++;
    }
}

/* the next count lines, each after prefix */
static void put_lines(struct liner *l, const char *prefix, unsigned count, FILE *out) {
    for(; count; count--) {
        int r;
        fputs(prefix, out);
        r = dr_copy_line(&l->r, out);
        putc('\n', out);
        if(r == 0) fputs(NO_NEWLINE, out);
        if(r < 0) return;
        l->line++;
    }
}

/**** normal ****/

static void put_range(unsigned from, unsigned to, FILE *out) {
    if(to > from + 1) fprintf(out, "%u,%u", from + 1, to);
    else fprintf(out, "%u", to);
}

static void normal_change(struct dscript *s, struct liner *la, struct liner *lb, const struct change *c, FILE *out) {
    unsigned fa = s->a.first, fb = s->b.first;
    if(c->a0 == c->a1) {
        fprintf(out, "%u", fa + c->a0);
        putc('a', out);
        put_range(fb + c->b0, fb + c->b1, out);
    } else if(c->b0 == c->b1) {
        put_range(fa + c->a0, fa + c->a1, out);
        putc('d', out);
        fprintf(out, "%u", fb + c->b0);
    } else {
        put_range(fa + c->a0, fa + c->a1, out);
        putc('c', out);
        put_range(fb + c->b0, fb + c->b1, out);
    }
    putc('\n', out);
    to_line(la, c->a0);
    put_lines(la, "< ", c->a1 - c->a0, out);
    if(c->a0 != c->a1 && c->b0 != c->b1) fputs("---\n", out);
    to_line(lb, c->b0);
    put_lines(lb, "> ", c->b1 - c->b0, out);
}

/**** with context ****/

/* the lines a group shows, in each file */
struct span {
    unsigned a0, a1, b0, b1;
};

static void group_span(const struct dscript *s, const struct dopts *o, const struct group *g, struct span *v) {
    unsigned before = o->context < g->first.a0 ? o->context : g->first.a0;
    unsigned after = s->a.n - g->last.a1;
    if(before > g->first.b0) before = g->first.b0;
    if(after > o->context) after = o->context;
    v->a0 = g->first.a0 - before;
    v->b0 = g->first.b0 - before;
    v->a1 = g->last.a1 + after;
    v->b1 = g->last.b1 + after;
}

static void unified_range(unsigned from, unsigned to, FILE *out) {
    if(to - from == 1) fprintf(out, "%u", to);
    else fprintf(out, "%u,%u", to > from ? from + 1 : from, to - from);
}

static void unified_group(struct dscript *s, const struct dopts *o, const struct group *g, unsigned i, unsigned j,
                          struct liner *la, struct liner *lb, FILE *out) {
    struct span v;
    struct change c;
    unsigned k, at;
    group_span(s, o, g, &v);
    fputs("@@ -", out);
    unified_range(s->a.first + v.a0, s->a.first + v.a1, out);
    fputs(" +", out);
    unified_range(s->b.first + v.b0, s->b.first + v.b1, out);
    fputs(" @@\n", out);
    to_line(la, v.a0);
    to_line(lb, v.b0);
    at = v.a0;
    for(k = 0; k < g->count; k++) {
        next_change(s, &i, &j, &c);
        put_lines(la, " ", c.a0 - at, out);
        to_line(lb, c.b0);
        put_lines(la, "-", c.a1 - c.a0, out);
        put_lines(lb, "+", c.b1 - c.b0, out);
        at = c.a1;
    }
    put_lines(la, " ", v.a1 - at, out);
}

static void context_range(unsigned from, unsigned to, FILE *out) {
    if(to > from + 1) fprintf(out, "%u,%u", from + 1, to);
    else fprintf(out, "%u", to);
}

/* one file's half of a hunk; side 0 or 1 says which, and with it which runs are the file's own */
static void context_side(struct dscript *s, const struct group *g, int side, unsigned from, unsigned to, unsigned i,
                         unsigned j, struct liner *l, FILE *out) {
    unsigned k, at = from;
    to_line(l, from);
    for(k = 0; k < g->count; k++) {
        struct change c;
        unsigned c0, c1, other;
        next_change(s, &i, &j, &c);
        c0 = side ? c.b0 : c.a0;
        c1 = side ? c.b1 : c.a1;
        other = side ? c.a1 - c.a0 : c.b1 - c.b0;
        put_lines(l, "  ", c0 - at, out);
        put_lines(l, other ? "! " : side ? "+ " : "- ", c1 - c0, out);
        at = c1;
    }
    put_lines(l, "  ", to - at, out);
}

static void context_group(struct dscript *s, const struct dopts *o, const struct group *g, unsigned i, unsigned j,
                          struct liner *la, struct liner *lb, FILE *out) {
    struct span v;
    struct change c;
    unsigned k, ii = i, jj = j;
    int deletes = 0, inserts = 0;
    group_span(s, o, g, &v);
    for(k = 0; k < g->count; k++) {
        next_change(s, &ii, &jj, &c);
        if(c.a0 != c.a1) deletes = 1;
        if(c.b0 != c.b1) inserts = 1;
    }
    fputs("***************\n*** ", out);
    context_range(s->a.first + v.a0, s->a.first + v.a1, out);
    fputs(" ****\n", out);
    if(deletes) context_side(s, g, 0, v.a0, v.a1, i, j, la, out);
    fputs("--- ", out);
    context_range(s->b.first + v.b0, s->b.first + v.b1, out);
    fputs(" ----\n", out);
    if(inserts) context_side(s, g, 1, v.b0, v.b1, i, j, lb, out);
}

int diff_print(struct dscript *s, const struct dopts *o, const char *label1, const char *label2, FILE *out) {
    struct liner la, lb;
    struct group g;
    unsigned i = 0, j = 0;
    int e;
    if(dr_start(&la.r, s->a.src, s->a.start, s->a.end) < 0) return DIFF_FAILED;
    if(dr_start(&lb.r, s->b.src, s->b.start, s->b.end) < 0) {
        dr_stop(&la.r);
        return DIFF_FAILED;
    }
    la.line = lb.line = 0;
    if(o->style == 'u') fprintf(out, "--- %s\n+++ %s\n", label1, label2);
    else if(o->style == 'c') fprintf(out, "*** %s\n--- %s\n", label1, label2);
    for(;;) {
        unsigned gi = i, gj = j;
        if(!next_group(s, o, &i, &j, &g)) break;
        if(!g.visible) continue;
        if(o->style == 'n') normal_change(s, &la, &lb, &g.first, out);
        else if(o->style == 'u') unified_group(s, o, &g, gi, gj, &la, &lb, out);
        else context_group(s, o, &g, gi, gj, &la, &lb, out);
    }
    s->failed_side = la.r.failed ? 0 : 1;
    e = la.r.failed ? la.r.failed : lb.r.failed;
    dr_stop(&la.r);
    dr_stop(&lb.r);
    if(e) {
        errno = e;
        return DIFF_FAILED;
    }
    return 0;
}
