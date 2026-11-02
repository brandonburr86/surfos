/* Host-side tests of diff and patch's logic (11/01/26), user/diff/ as it is, in one process: the O(ND) search against
   a table of longest common subsequences; for generated pairs of files diff then patch gives the second file back,
   forward and with -R, in all three forms and at several amounts of context, with and without last newlines and
   around the block size; what -i -b -w -B compare; where a hunk goes when the file has moved on (offset, fuzz, the
   ends of the file, in order); and damaged patches, which must fail cleanly: flipped bytes, cut ends, numbers that
   lie. Built twice by `make unittest`: as it is, and with dfile.c's hashes cut to 2 bits (-DD_HASH_MASK=3,
   test_diff_weak), where nearly every pair of lines collides and only the last check keeps the edit correct, so
   there the edit is checked as correct and not as shortest. tests/test_cli_diff.py runs the programs, beside real
   tools. tests/host/cli/mac-sanitize.sh runs this under the sanitizers, on the Mac. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/diff/dcore.h"
#include "../../user/diff/diff.h"
#include "../../user/diff/patch.h"

static int checks, failures;
static unsigned long long seed = 88172645463325252ULL;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        if(failures < 30) printf("FAIL: %s\n", what);
    }
}

static unsigned rnd(unsigned n) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return n ? (unsigned)((seed >> 11) % n) : 0;
}

struct mem {
    char *p;
    size_t n, cap;
};

static void mem_add(struct mem *m, const void *p, size_t n) {
    if(m->n + n + 1 > m->cap) {
        m->cap = (m->n + n + 1) * 2;
        m->p = realloc(m->p, m->cap);
        if(!m->p) abort();
    }
    if(n) memcpy(m->p + m->n, p, n);
    m->n += n;
}

static int mem_put(void *ctx, const void *p, unsigned n) {
    mem_add(ctx, p, n);
    return 0;
}

/**** the search ****/

static unsigned lcs(const unsigned *a, unsigned n, const unsigned *b, unsigned m) {
    unsigned *prev = calloc(m + 1, sizeof(*prev)), *row = calloc(m + 1, sizeof(*row)), i, j, r;
    for(i = 1; i <= n; i++) {
        for(j = 1; j <= m; j++)
            row[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 : row[j - 1] > prev[j] ? row[j - 1] : prev[j];
        memcpy(prev, row, (m + 1) * sizeof(*row));
    }
    r = prev[m];
    free(prev);
    free(row);
    return r;
}

/* the lines left unmarked are the same sequence in both; their number */
static int common_of(const unsigned *a, unsigned n, const unsigned char *ma, const unsigned *b, unsigned m,
                     const unsigned char *mb, unsigned *kept) {
    unsigned i = 0, j = 0, k = 0;
    for(;;) {
        while(i < n && ma[i]) i++;
        while(j < m && mb[j]) j++;
        if(i == n || j == m) break;
        if(a[i] != b[j]) return 0;
        i++;
        j++;
        k++;
    }
    while(i < n && ma[i]) i++;
    while(j < m && mb[j]) j++;
    *kept = k;
    return i == n && j == m;
}

static void test_search(void) {
    static unsigned a[400], b[400];
    static unsigned char ma[400], mb[400];
    unsigned r;
    for(r = 0; r < 4000; r++) {
        unsigned n = rnd(120), m = rnd(120), k = 1 + rnd(8), i, kept = 0, best;
        int shortest = 1;
        for(i = 0; i < n; i++) a[i] = rnd(k);
        if(r % 3) {                             /* the second an edit of the first: long common runs */
            unsigned j = 0;
            for(i = 0; i < n && j < 300; i++) {
                if(!rnd(6)) continue;
                if(!rnd(6)) b[j++] = rnd(k);
                b[j++] = a[i];
            }
            m = j;
        } else for(i = 0; i < m; i++) b[i] = rnd(k);
        memset(ma, 0, sizeof(ma));
        memset(mb, 0, sizeof(mb));
        check(dc_compare(a, n, b, m, ma, mb) == 0, "dc_compare has its memory");
        check(common_of(a, n, ma, b, m, mb, &kept), "what is left unmarked is common to both");
        best = lcs(a, n, b, m);
        if(kept != best) shortest = 0;
        check(shortest, "the edit is the shortest");
        /* a cap of a few edits: correct whatever it costs in length, and it ends */
        memset(ma, 0, sizeof(ma));
        memset(mb, 0, sizeof(mb));
        check(dc_compare_limit(a, n, b, m, ma, mb, 1 + rnd(4)) == 0, "dc_compare_limit has its memory");
        check(common_of(a, n, ma, b, m, mb, &kept), "with a small cap too");
        check(kept <= best, "and no better than the best");
    }
}

/* large and nothing in common but scattered lines: the cap is what makes this end, and the stack stays fixed */
static void test_search_large(void) {
    unsigned n = 200000, m = 200000, i, kept = 0;
    unsigned *a = malloc(n * sizeof(*a)), *b = malloc(m * sizeof(*b));
    unsigned char *ma = calloc(n, 1), *mb = calloc(m, 1);
    for(i = 0; i < n; i++) a[i] = rnd(1000);
    for(i = 0; i < m; i++) b[i] = rnd(1000);
    check(dc_compare_limit(a, n, b, m, ma, mb, 64) == 0 && common_of(a, n, ma, b, m, mb, &kept), "a large pair under a small cap");
    memset(ma, 0, n);
    memset(mb, 0, m);
    for(i = 0; i < n; i++) b[i] = i;            /* the worst for the depth of the boxes: nothing equal at all */
    for(i = 0; i < n; i++) a[i] = n + i;
    check(dc_compare_limit(a, n, b, m, ma, mb, 2) == 0 && common_of(a, n, ma, b, m, mb, &kept) && !kept, "nothing in common");
    free(a);
    free(b);
    free(ma);
    free(mb);
}

/* A long file with many stretches rewritten, each losing half its lines, so that the two files drift further apart as they go,
   well past what the capped search can see from either corner. Almost nothing matches inside a stretch, so a search that only
   goes by how far it got has nothing to steer by, and leaves the true alignment for good: cutting at the furthest point, it
   kept 415 of the 33,928 lines here at a cap of 512, an edit 2.7 times the shortest. The cap must cost little, not most of the
   lines left alone. */
static void test_search_drift(void) {
    unsigned n = 60000, i, m = 0, in_stretches = 0, kept = 0, cap;
    unsigned *a = malloc(n * sizeof(*a)), *b = malloc(n * sizeof(*b));
    unsigned char *ma = malloc(n), *mb = malloc(n);
    int rewriting = 0;
    for(i = 0; i < n; i++) a[i] = i;
    for(i = 0; i < n; i++) {
        if(!rnd(1500)) rewriting = !rewriting;
        in_stretches += rewriting;
        if(!rewriting) b[m++] = a[i];
        else if(rnd(2)) b[m++] = n + i;         /* a line of its own: nothing to match in the stretch */
    }
    for(cap = 512; cap <= 2048; cap *= 2) {
        memset(ma, 0, n);
        memset(mb, 0, n);
        check(dc_compare_limit(a, n, b, m, ma, mb, cap) == 0 && common_of(a, n, ma, b, m, mb, &kept), "a drifting pair under a cap");
        check(kept * 5 >= (n - in_stretches) * 4, "the cap loses less than a fifth of what the true alignment keeps");
        if(kept * 5 < (n - in_stretches) * 4) printf("  cap %u kept %u of %u\n", cap, kept, n - in_stretches);
    }
    free(a);
    free(b);
    free(ma);
    free(mb);
}

/* repeated lines: an insertion ends as far down as it will go */
static void test_slide(void) {
    unsigned a[] = { 1, 2, 2, 3 }, b[] = { 1, 2, 2, 2, 3 };
    unsigned char ma[4] = { 0 }, mb[5] = { 0 };
    check(!dc_compare(a, 4, b, 5, ma, mb) && mb[3] && !mb[1] && !mb[2], "one more of a repeated line: the last of them");
}

/**** files in memory ****/

static const char *const line_table[] = {
    "", "", "a", "b", "c", "d", "  x", "x  ", "\ty", "y", "same", "same", "same", "#include <stdio.h>", "}", "{",
    "--- not a header", "+++ nor this", "@@ -1 +1 @@", "*** 1,2 ****", "\\ not a marker", "< left", "> right", "---",
    "h\xC3\xA9llo", "a line with several words in it", "1a2", "5,7d4",
};
#define LINES (sizeof(line_table) / sizeof(line_table[0]))

static void gen_file(struct mem *m, unsigned lines, unsigned kinds) {
    unsigned i;
    m->n = 0;
    for(i = 0; i < lines; i++) {
        const char *s = line_table[rnd(kinds)];
        mem_add(m, s, strlen(s));
        mem_add(m, "\n", 1);
    }
}

/* b: a with random lines dropped, added and changed */
static void gen_edit(const struct mem *a, struct mem *b, unsigned kinds, unsigned rate) {
    size_t i = 0;
    b->n = 0;
    while(i < a->n) {
        const char *nl = memchr(a->p + i, '\n', a->n - i);
        size_t len = nl ? (size_t)(nl - (a->p + i)) + 1 : a->n - i;
        unsigned r = rnd(rate);
        if(r != 0) mem_add(b, a->p + i, len);
        if(r <= 1) {
            const char *s = line_table[rnd(kinds)];
            mem_add(b, s, strlen(s));
            mem_add(b, "\n", 1);
        }
        i += len;
    }
}

static void drop_last_newline(struct mem *m) {
    if(m->n && m->p[m->n - 1] == '\n') m->n--;
}

/* the diff of a and b as text: its status */
static int make_diff(const struct mem *a, const struct mem *b, const struct dopts *o, struct mem *out) {
    struct dsrc sa, sb;
    struct dscript s;
    char *text = NULL;
    size_t len = 0;
    FILE *f;
    int r;
    dsrc_memory(&sa, a->p ? a->p : "", (unsigned)a->n);
    dsrc_memory(&sb, b->p ? b->p : "", (unsigned)b->n);
    out->n = 0;
    r = diff_compare(&sa, &sb, o, &s);
    if(r == DIFF_DIFFER && !o->brief) {
        f = open_memstream(&text, &len);
        if(diff_print(&s, o, "old", "new", f)) r = DIFF_FAILED;
        fclose(f);
        mem_add(out, text, len);
        free(text);
    }
    diff_free(&s);
    return r;
}

/* the patch applied to file; *failed hunks did not go in, *moved went in but not where they said. -1: it would not parse */
static int apply(const struct mem *patch, const char *file, size_t size, int reverse, struct mem *out, unsigned *failed,
                 unsigned *moved) {
    struct dsrc ps, ts;
    struct pinput in;
    struct psection sec;
    struct phunk h;
    struct ptarget t;
    struct papply a;
    int r, rc = 0;
    memset(&h, 0, sizeof(h));
    *failed = *moved = 0;
    out->n = 0;
    dsrc_memory(&ps, patch->p ? patch->p : "", (unsigned)patch->n);
    dsrc_memory(&ts, file, (unsigned)size);
    if(pi_start(&in, &ps) < 0 || pt_load(&t, &ts) < 0) abort();
    pa_start(&a, &t, mem_put, out);
    while((r = pp_next_section(&in, &sec)) > 0) {
        while((r = pp_next_hunk(&in, &sec, &h)) > 0) {
            unsigned at, fuzz;
            int found;
            if(reverse) ph_reverse(&h);
            found = pa_locate(&a, &h, &at, &fuzz);
            if(found <= 0) {
                ++*failed;
                continue;
            }
            if(pa_apply(&a, &h, at, fuzz) < 0) abort();
            if(fuzz || a.last_offset) ++*moved;
        }
        if(r < 0) break;
    }
    if(r < 0) rc = -1;
    if(pa_finish(&a) < 0) abort();
    pt_free(&t);
    ph_free(&h);
    pi_stop(&in);
    return rc;
}

#ifndef D_HASH_MASK
static unsigned line_hashes(const struct mem *m, unsigned **out) {
    size_t i = 0;
    unsigned n = 0, *v = malloc((m->n + 2) * sizeof(*v));
    while(i < m->n) {
        unsigned h = DH_START;
        for(; i < m->n && m->p[i] != '\n'; i++) h = DH_ADD(h, m->p[i]);
        if(i < m->n) {
            h = DH_ADD(h, '\n');
            i++;
        }
        v[n++] = h;
    }
    *out = v;
    return n;
}

static int count_lines(const char *p, size_t n, char c, const char *not_this) {
    size_t i = 0;
    int k = 0;
    while(i < n) {
        const char *nl = memchr(p + i, '\n', n - i);
        size_t len = nl ? (size_t)(nl - (p + i)) : n - i;
        if(len && p[i] == c && (!not_this || len < strlen(not_this) || memcmp(p + i, not_this, strlen(not_this)))) k++;
        i += len + 1;
    }
    return k;
}
#endif

static void round_trip(struct mem *a, struct mem *b, int style, unsigned context) {
    struct dopts o;
    struct mem text = { 0 }, back = { 0 };
    unsigned failed, moved;
    int r, same = a->n == b->n && (!a->n || !memcmp(a->p, b->p, a->n));
    memset(&o, 0, sizeof(o));
    o.style = style;
    o.context = context;
    r = make_diff(a, b, &o, &text);
    check(r == (same ? DIFF_SAME : DIFF_DIFFER), "the status says whether they differ");
    if(r == DIFF_DIFFER) {
        check(apply(&text, a->p ? a->p : "", a->n, 0, &back, &failed, &moved) == 0, "a diff's text parses");
        check(!failed && !moved, "each hunk goes where it says");
        check(back.n == b->n && !memcmp(back.p ? back.p : "", b->p ? b->p : "", b->n), "diff then patch gives the second file");
        if(failures == 1 && !(back.n == b->n)) printf("%.*s", (int)text.n, text.p);
        check(apply(&text, b->p ? b->p : "", b->n, 1, &back, &failed, &moved) == 0, "and -R parses");
        check(!failed && !moved, "and with -R each hunk goes where it says");
        check(back.n == a->n && !memcmp(back.p ? back.p : "", a->p ? a->p : "", a->n), "and with -R the first");
#ifndef D_HASH_MASK
        if(style == 'u') {                      /* the shortest edit, counted from the text */
            unsigned *ha, *hb, na = line_hashes(a, &ha), nb = line_hashes(b, &hb), best = lcs(ha, na, hb, nb);
            int minus = count_lines(text.p, text.n, '-', "--- old"), plus = count_lines(text.p, text.n, '+', "+++ new");
            check((unsigned)minus == na - best && (unsigned)plus == nb - best, "as short as it can be");
            free(ha);
            free(hb);
        }
#endif
    }
    free(text.p);
    free(back.p);
}

static void test_round_trips(unsigned rounds) {
    static const int styles[3] = { 'n', 'u', 'c' };
    struct mem a = { 0 }, b = { 0 };
    unsigned r;
    for(r = 0; r < rounds; r++) {
        unsigned kinds = r % 4 == 0 ? 4 : r % 4 == 1 ? 8 : (unsigned)LINES;
        gen_file(&a, rnd(60), kinds);
        gen_edit(&a, &b, kinds, 2 + rnd(9));
        if(r % 5 == 1) drop_last_newline(&a);
        if(r % 7 == 2) drop_last_newline(&b);
        if(r % 11 == 3) b.n = 0;
        if(r % 13 == 4) a.n = 0;
        round_trip(&a, &b, styles[r % 3], r % 5);
    }
    free(a.p);
    free(b.p);
}

/* the block size runs through the search for what is common, the hashing and the output: lines across it */
static void test_blocks(void) {
    struct mem a = { 0 }, b = { 0 };
    unsigned size;
    for(size = DR_BLOCK - 3; size <= DR_BLOCK + 3; size++) {
        unsigned at, step;
        for(step = 0; step < 6; step++) {
            a.n = 0;
            while(a.n < size) {
                unsigned len = 1 + rnd(step ? 90 : 7), i;
                char c = (char)('a' + rnd(26));
                for(i = 0; i < len; i++) mem_add(&a, &c, 1);
                mem_add(&a, "\n", 1);
            }
            a.n = size;
            b.n = 0;
            mem_add(&b, a.p, a.n);
            at = step % 2 ? rnd(size) : (size > 4 ? size - 1 - rnd(4) : 0);
            b.p[at] = b.p[at] == '\n' ? '\n' : 'Z';
            if(step == 3) mem_add(&b, "tail\n", 5);
            if(step == 4) {
                memmove(b.p + 20, b.p, b.n - 20);
                memcpy(b.p, "a new first line\n\n\n\n", 20);
            }
            round_trip(&a, &b, step % 2 ? 'u' : 'c', step);
            round_trip(&a, &b, 'n', 0);
        }
    }
    /* far from the ends: many lines of each */
    a.n = 0;
    for(size = 0; size < 200000; size++) {
        char line[32];
        int n = snprintf(line, sizeof(line), "line %u\n", size);
        mem_add(&a, line, (size_t)n);
    }
    b.n = 0;
    mem_add(&b, a.p, a.n);
    memcpy(b.p + a.n / 2, "CHANGED", 7);
    round_trip(&a, &b, 'u', 3);
    round_trip(&a, &b, 'c', 100000);            /* more context than the start's scan keeps: it starts from line 1 */
    free(a.p);
    free(b.p);
}

/**** what the options compare ****/

static void mem_set(struct mem *m, const char *s) {
    m->n = 0;
    mem_add(m, s, strlen(s));
}

/* differ or not, with these options */
static int differs(const char *x, const char *y, int icase, int space, int blanks, int brief) {
    struct mem a = { 0 }, b = { 0 }, text = { 0 };
    struct dopts o;
    int r;
    memset(&o, 0, sizeof(o));
    o.style = 'n';
    o.icase = icase;
    o.space = space;
    o.blanks = blanks;
    o.brief = brief;
    mem_set(&a, x);
    mem_set(&b, y);
    r = make_diff(&a, &b, &o, &text);
    free(a.p);
    free(b.p);
    free(text.p);
    return r;
}

static void test_options(void) {
    int brief;
    for(brief = 0; brief < 2; brief++) {
        check(differs("a\nB\n", "a\nb\n", 0, 0, 0, brief) == DIFF_DIFFER, "case counts");
        check(differs("a\nB\n", "a\nb\n", 1, 0, 0, brief) == DIFF_SAME, "-i: it does not");
        check(differs("a b\n", "a   b\n", 0, 0, 0, brief) == DIFF_DIFFER, "space counts");
        check(differs("a b\n", "a \t b\n", 0, 'b', 0, brief) == DIFF_SAME, "-b: its amount does not");
        check(differs("ab\n", "a b\n", 0, 'b', 0, brief) == DIFF_DIFFER, "-b: its being there does");
        check(differs("a\n", "a  \n", 0, 'b', 0, brief) == DIFF_SAME, "-b: nor at a line's end");
        check(differs("a b\n", " ab \n", 0, 'w', 0, brief) == DIFF_SAME, "-w: nowhere");
        check(differs("a\n\nb\n", "a\nb\n", 0, 0, 0, brief) == DIFF_DIFFER, "a blank line counts");
        check(differs("a\n\nb\n", "a\nb\n", 0, 0, 1, brief) == DIFF_SAME, "-B: it does not");
        check(differs("a\n\nb\n", "a\nc\n", 0, 0, 1, brief) == DIFF_DIFFER, "-B: other changes still do");
        check(differs("a\n   \nb\n", "a\nb\n", 0, 0, 1, brief) == DIFF_DIFFER, "-B: spaces are not blank by themselves");
        check(differs("a\n   \nb\n", "a\nb\n", 0, 'b', 1, brief) == DIFF_SAME, "-B -b: they are then");
        check(differs("a", "a\n", 0, 0, 0, brief) == DIFF_DIFFER, "a missing last newline counts");
        check(differs("a", "a\n", 1, 0, 0, brief) == DIFF_DIFFER, "-i: and still");
        check(differs("a", "a\n", 0, 'b', 0, brief) == DIFF_SAME, "-b: it does not");
        check(differs("a", "a\n", 0, 'w', 0, brief) == DIFF_SAME, "-w: it does not");
    }
    {
        struct mem a = { 0 }, b = { 0 }, text = { 0 };
        struct dopts o;
        memset(&o, 0, sizeof(o));
        o.style = 'n';
        mem_add(&a, "x\0y\n", 4);
        mem_add(&b, "x\0z\n", 4);
        check(make_diff(&a, &b, &o, &text) == DIFF_BINARY, "a NUL makes a file binary");
        check(make_diff(&a, &a, &o, &text) == DIFF_SAME, "the same binary file is the same");
        free(a.p);
        free(b.p);
        free(text.p);
    }
}

/**** where hunks go ****/

#ifndef D_HASH_MASK
/* patch made from x to y, applied to z: is the result want, with so many failed and moved? */
static void moved_case(const char *x, const char *y, const char *z, const char *want, unsigned failed, unsigned moved,
                       const char *what) {
    struct mem a = { 0 }, b = { 0 }, text = { 0 }, back = { 0 };
    struct dopts o;
    unsigned f, m;
    memset(&o, 0, sizeof(o));
    o.style = 'u';
    o.context = 3;
    mem_set(&a, x);
    mem_set(&b, y);
    make_diff(&a, &b, &o, &text);
    apply(&text, z, strlen(z), 0, &back, &f, &m);
    mem_add(&back, "", 1);
    check(f == failed && m == moved && !strcmp(back.p, want), what);
    if(!(f == failed && m == moved && !strcmp(back.p, want)))
        printf("  failed %u moved %u, got:\n%s\n  from the patch:\n%.*s", f, m, back.p, (int)text.n, text.p);
    free(a.p);
    free(b.p);
    free(text.p);
    free(back.p);
}

static void test_moved(void) {
    static const char x[] = "1\n2\n3\n4\n5\n6\n7\n8\n9\n", y[] = "1\n2\n3\n4\nFIVE\n6\n7\n8\n9\n";
    moved_case(x, y, x, y, 0, 0, "where it says");
    moved_case(x, y, "a\nb\n1\n2\n3\n4\n5\n6\n7\n8\n9\n", "a\nb\n1\n2\n3\n4\nFIVE\n6\n7\n8\n9\n", 0, 1, "two lines down");
    moved_case(x, y, "3\n4\n5\n6\n7\n8\n9\n", "3\n4\nFIVE\n6\n7\n8\n9\n", 0, 1, "two lines up");
    moved_case(x, y, "1\n2\nX\n4\n5\n6\nY\n8\n9\n", "1\n2\nX\n4\nFIVE\n6\nY\n8\n9\n", 0, 1, "with fuzz: the outer context changed");
    moved_case(x, y, "1\n2\n3\nX\n5\nY\n7\n8\n9\n", "1\n2\n3\nX\n5\nY\n7\n8\n9\n", 1, 0, "too much changed: nothing goes in");
    moved_case(x, y, "1\n2\n3\n4\n6\n7\n8\n9\n", "1\n2\n3\n4\n6\n7\n8\n9\n", 1, 0, "the line it changes is gone");
    /* at the ends of the file: less context on one side means it must stand there */
    moved_case("1\n2\n3\n4\n5\n", "ONE\n2\n3\n4\n5\n", "0\n1\n2\n3\n4\n5\n", "0\n1\n2\n3\n4\n5\n", 1, 0,
               "a hunk at the start of the file will not go further in");
    moved_case("1\n2\n3\n4\n5\n", "1\n2\n3\n4\nFIVE\n", "1\n2\n3\n4\n5\n6\n", "1\n2\n3\n4\n5\n6\n", 1, 0,
               "a hunk at the end of the file will not go further up");
    /* with some context at the short end the edge still holds, at every fuzz */
    moved_case("1\n2\n3\n4\n5\n6\n7\n8\n", "1\n2\nTHREE\n4\n5\n6\n7\n8\n", "0\n1\n2\n3\n4\n5\n6\n7\n8\n", "0\n1\n2\n3\n4\n5\n6\n7\n8\n", 1, 0,
               "two lines of context at the start, three after: made at the start, and it stays there");
    moved_case("1\n2\n3\n4\n5\n6\n7\n8\n", "1\n2\n3\n4\n5\nSIX\n7\n8\n", "1\n2\n3\n4\n5\n6\n7\n8\n9\n", "1\n2\n3\n4\n5\n6\n7\n8\n9\n", 1, 0,
               "two lines of context at the end, three before: made at the end, and it stays there");
    /* and fuzz still works at the other end of such a hunk */
    moved_case("1\n2\n3\n4\n5\n6\n7\n8\n", "1\n2\nTHREE\n4\n5\n6\n7\n8\n", "1\n2\n3\n4\n5\nX\n7\n8\n", "1\n2\nTHREE\n4\n5\nX\n7\n8\n", 0, 1,
               "the open end of a hunk at the start may differ");
    moved_case("1\n2\n3\n4\n5\n6\n7\n8\n", "1\n2\n3\n4\n5\nSIX\n7\n8\n", "1\n2\n3\nX\n5\n6\n7\n8\n", "1\n2\n3\nX\n5\nSIX\n7\n8\n", 0, 1,
               "the open end of a hunk at the end may differ");
    /* the short end of such a hunk has to match: it says where the file begins */
    moved_case("1\n2\n3\n4\n5\n6\n7\n8\n", "1\n2\nTHREE\n4\n5\n6\n7\n8\n", "X\n2\n3\n4\n5\n6\n7\n8\n", "X\n2\n3\n4\n5\n6\n7\n8\n", 1, 0,
               "the short end is not loosened by fuzz");
    /* two hunks: the second follows the first's offset, and may not go back over it */
    moved_case("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\n", "a\nb\nc\nD\ne\nf\ng\nh\ni\nj\nk\nl\nm\nN\no\np\n",
               "x\ny\na\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\n", "x\ny\na\nb\nc\nD\ne\nf\ng\nh\ni\nj\nk\nl\nm\nN\no\np\n",
               0, 2, "both hunks two lines down");
    /* the same with the first change in the file's second line: that hunk was made at the start, which has moved on */
    moved_case("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\n", "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nN\no\np\n",
               "x\ny\na\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\n", "x\ny\na\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nN\no\np\n",
               1, 1, "a hunk at the start of a file that has grown at the start is refused, and the next still goes in");
    /* a repeated block: nearest first */
    moved_case("h\nh\nh\nx\nt\nt\nt\n", "h\nh\nh\nY\nt\nt\nt\n", "h\nh\nh\nx\nt\nt\nt\n0\nh\nh\nh\nx\nt\nt\nt\n",
               "h\nh\nh\nY\nt\nt\nt\n0\nh\nh\nh\nx\nt\nt\nt\n", 0, 0, "the nearest of two places");
    /* a file that lacks its last newline where the hunk has one */
    moved_case("1\n2\n3\n", "1\nTWO\n3\n", "1\n2\n3", "1\nTWO\n3", 0, 1,
               "the hunk's last line has a newline and the file's has not: by fuzz, and the file's stays as it is");
}
#endif

/**** a patch made by hand ****/

/* the patch applied to file gives want */
static void hand_case(const char *patch, const char *file, const char *want, const char *what) {
    struct mem p = { 0 }, back = { 0 };
    unsigned f, m;
    int r;
    mem_set(&p, patch);
    r = apply(&p, file, strlen(file), 0, &back, &f, &m);
    mem_add(&back, "", 1);
    check(r == 0 && !f && !strcmp(back.p, want), what);
    if(!(r == 0 && !f && !strcmp(back.p, want))) printf("  r %d failed %u, got [%s]\n", r, f, back.p);
    free(p.p);
    free(back.p);
}

static int parses(const char *patch, const char *file) {
    struct mem p = { 0 }, back = { 0 };
    unsigned f, m;
    int r;
    mem_set(&p, patch);
    r = apply(&p, file, strlen(file), 0, &back, &f, &m);
    free(p.p);
    free(back.p);
    return r;
}

static void test_by_hand(void) {
    hand_case("mail text\n--- a/x\t2026\n+++ b/x\t2026\n@@ -1,2 +1,2 @@ a function\n a\n-b\n+B\n-- \nsignature\n", "a\nb\n", "a\nB\n",
              "text around a section is passed over");
    hand_case("@@ -1 +1 @@\n-a\n+b\n", "a\n", "b\n", "no headers, and counts of 1 left out");
    hand_case("--- x\n+++ y\n@@ -1,3 +1,3 @@\n a\n\n-b\n+B\n", "a\n\nb\n", "a\n\nB\n", "an empty line with its space gone");
    hand_case("--- x\n+++ y\n@@ -1,2 +1,1 @@\n a\n--- x\n", "a\n-- x\n", "a\n", "a deleted line that looks like a header");
    hand_case("--- x\n+++ y\n@@ -2,0 +3 @@\n+new\n", "a\nb\nc\n", "a\nb\nnew\nc\n", "no context: after line 2");
    hand_case("--- x\n+++ y\n@@ -0,0 +1 @@\n+new\n", "a\n", "new\na\n", "no context: before line 1");
    hand_case("1c1\n< a\n---\n> b\n", "a\n", "b\n", "normal: a change");
    hand_case("1a2\n> b\n", "a\n", "a\nb\n", "normal: an addition");
    hand_case("0a1\n> b\n", "a\n", "b\na\n", "normal: an addition before line 1");
    hand_case("2d1\n< b\n", "a\nb\n", "a\n", "normal: a deletion");
    hand_case("1,2c1\n< a\n< b\n---\n> c\n", "a\nb\n", "c\n", "normal: lines for a line");
    hand_case("*** x\n--- y\n***************\n*** 1,2 ****\n  a\n! b\n--- 1,2 ----\n  a\n! B\n", "a\nb\n", "a\nB\n", "context");
    hand_case("*** x\n--- y\n***************\n*** 1,2 ****\n--- 1,3 ----\n  a\n+ n\n  b\n", "a\nb\n", "a\nn\nb\n",
              "context: the old side left out");
    hand_case("*** x\n--- y\n***************\n*** 1,3 ****\n  a\n- n\n  b\n--- 1,2 ----\n", "a\nn\nb\n", "a\nb\n",
              "context: the new side left out");
    hand_case("*** x\n--- y\n***************\n*** 1,3 ****\n  a\n- n\n  b\n--- 1,2 ----\n\nnot the hunk\n", "a\nn\nb\n", "a\nb\n",
              "context: an empty line after a side that was left out is not part of it");
    hand_case("*** x\n--- y\n***************\n*** 1 ****\n--- 2 ----\n+ b\n", "a\n", "a\nb\n", "context, no lines of context: an addition");
    hand_case("--- x\n+++ y\n@@ -1 +1 @@\n-a\n\\ No newline at end of file\n+a\n", "a", "a\n", "a newline added at the end");
    hand_case("--- x\n+++ y\n@@ -1 +1 @@\n-a\n+a\n\\ No newline at end of file\n", "a\n", "a", "a newline taken off the end");
    hand_case("--- x\n+++ y\n@@ -1,2 +1,2 @@\n a\n-b\n\\ Kein Zeilenumbruch am Dateiende.\n+c\n", "a\nb", "a\nc\n", "the marker in another language");
    hand_case("--- x\n+++ y\n@@ -1 +1 @@\n-a\n+b\n\\ No newline at end of file\n", "a\nc\n", "b\nc\n",
              "a line said to end the file, with more after it: it gets its newline");
    hand_case("--- x\n+++ y\n@@ -2,0 +3 @@\n+b\n", "x\na", "x\na\nb\n", "a line added after a last line with no newline");
    check(parses("garbage only\n", "a\n") == 0, "garbage is no error here: patch.c counts the sections");
    check(parses("--- x\n+++ y\n@@ -1,2 +1,2 @@\n a\n", "a\n") < 0, "a patch that ends inside a hunk");
    check(parses("--- x\n+++ y\n@@ -1,2 +1,2 @@\n a\n?what\n", "a\n") < 0, "a line that is no part of a hunk");
    check(parses("--- x\n+++ y\n@@ -1 +1 @@\n a\n trailing text\n", "a\n") == 0, "a hunk ends at its counts: what follows is text");
    check(parses("--- x\n+++ y\n@@ -x +y @@\n-a\n", "a\n") < 0, "a header that is none");
    check(parses("--- x\n+++ y\n@@ -0,1 +1 @@\n-a\n+a\n", "a\n") < 0, "a line 0");
    check(parses("--- x\n+++ y\n@@ -1,1000000000 +1,1000000000 @@\n-a\n", "a\n") < 0, "a count that lies");
    check(parses("--- x\n+++ y\n@@ -4294967295,2 +1 @@\n-a\n+b\n", "a\n") < 0, "a hunk past the largest file");
    check(parses("--- x\n+++ y\n@@ -99999999999999999999 +1 @@\n-a\n+b\n", "a\n") < 0, "a number too big for 32 bits");
    check(parses("*** x\n--- y\n***************\n*** 5,3 ****\n", "a\n") < 0, "a range backwards");
    check(parses("*** x\n--- y\n***************\n*** 1,2 ****\n  a\n! b\n--- 1,2 ----\n  z\n! B\n", "a\nb\n") < 0, "two sides that differ in context");
    check(parses("*** x\n--- y\n***************\n*** 1 ****\n! a\n--- 1 ----\n", "a\n") < 0, "a ! on one side only");
    check(parses("--- x\n+++ y\n@@ -1 +1 @@\n\\ No newline at end of file\n-a\n+b\n", "a\n") < 0, "a marker before any line");
    check(parses("3,1d2\n< a\n", "a\n") < 0, "a command backwards");
    check(parses("1,2a3\n> a\n", "a\n") < 0, "an addition after two lines");
}

/**** damage ****/

static void test_damage(unsigned rounds) {
    struct mem a = { 0 }, b = { 0 }, text = { 0 }, back = { 0 };
    static const int styles[3] = { 'n', 'u', 'c' };
    static const char *const junk[] = { "4294967295", "0", ",", "@@", "***", "---", "\\", "\n", "\t", "999999999 " };
    unsigned r, parsed = 0, refused = 0;
    for(r = 0; r < rounds; r++) {
        struct dopts o;
        unsigned k, failed, moved, cuts = 1 + rnd(6);
        memset(&o, 0, sizeof(o));
        o.style = styles[r % 3];
        o.context = r % 4;
        gen_file(&a, 3 + rnd(30), LINES);
        gen_edit(&a, &b, LINES, 4);
        if(r % 5 == 0) drop_last_newline(&b);
        if(make_diff(&a, &b, &o, &text) != DIFF_DIFFER) continue;
        for(k = 0; k < cuts && text.n; k++) {
            size_t at = rnd((unsigned)text.n);
            switch(rnd(5)) {
            case 0:
                text.p[at] = (char)rnd(256);
                break;
            case 1:
                text.p[at] ^= (char)(1u << rnd(8));
                break;
            case 2:
                text.n = at;
                break;
            case 3: {
                const char *s = junk[rnd(sizeof(junk) / sizeof(junk[0]))];
                size_t n = strlen(s);
                if(at + n < text.n) memcpy(text.p + at, s, n);
                break;
            }
            default: {
                const char *nl = memchr(text.p + at, '\n', text.n - at);     /* a whole line gone */
                if(nl) {
                    size_t n = (size_t)(nl - (text.p + at)) + 1;
                    memmove(text.p + at, nl + 1, text.n - at - n);
                    text.n -= n;
                }
            }
            }
        }
        if(apply(&text, a.p, a.n, (int)(r & 1), &back, &failed, &moved) < 0) refused++;
        else parsed++;
    }
    check(parsed && refused, "damage: some still parse, some are refused, and none does harm");
    free(a.p);
    free(b.p);
    free(text.p);
    free(back.p);
}

/* a line far longer than any buffer, through the parser and the search */
static void test_long_lines(void) {
    struct mem a = { 0 }, b = { 0 }, text = { 0 };
    unsigned i;
    for(i = 0; i < 300000; i++) mem_add(&a, "x", 1);
    mem_add(&a, "\nmiddle\n", 8);
    mem_add(&b, a.p, a.n);
    b.p[b.n - 2] = 'E';
    round_trip(&a, &b, 'u', 1);
    round_trip(&a, &b, 'n', 0);
    round_trip(&a, &b, 'c', 2);
    free(a.p);
    free(b.p);
    free(text.p);
}

int main(int argc, char **argv) {
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1500;
    test_search();
    test_search_large();
    test_search_drift();
    test_slide();
    test_round_trips(rounds);
    test_blocks();
    test_options();
#ifndef D_HASH_MASK
    test_moved();                               /* these want the shortest edit, which colliding hashes may not give */
#endif
    test_by_hand();
    test_damage(rounds);
    test_long_lines();
    printf("test_diff%s: %d checks, %d failures\n",
#ifdef D_HASH_MASK
           " (hashes of 2 bits)",
#else
           "",
#endif
           checks, failures);
    return failures ? 1 : 0;
}
