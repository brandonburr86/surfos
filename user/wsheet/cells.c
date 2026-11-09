/*
SurfOS user program: wsheet
--------------------
File: cells.c   Date: 11/05/26
--------------------
The sheet's cells: where they are kept, what a cell's content is and what typing makes of it,
and the sheet as main.c reads it (sh_value(), sh_input(), sh_display() and the rest of sheet.h's
reading half).

Rows are a sorted array, each with a sorted array of its cells, so a cell is two binary searches
away and a range is walked row by row, left to right, without looking at the cells outside it. A
row whose last cell went stays, empty, until the change that emptied it ends (drop_empty_rows()),
so that a change can be taken back halfway without asking for memory (edit.c).

Memory: every allocation of the model goes through mem_alloc(), which counts it against
sh_mem_max, so that a file or a paste too big for SurfOS is refused with a message rather than
taking every page the program may have; the tests make any one allocation fail with sh_mem_fail.
A formula also keeps back, while it lives, what a recalculation needs for it (recalc_per_formula),
so that recalc() always has its arrays within the budget (11/09/26, the review's L7). And every
allocation but a reader's stops short of the budget by what a reader needs besides the sheet it
builds (read_headroom), so that a sheet grown until an edit is refused, saved, opens again (11/09/26,
the second review).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "sheetpriv.h"

long sh_mem_max = SH_MEMORY, sh_mem_used, sh_mem_blocks, sh_mem_fail = -1, sh_steps_max = SH_STEPS;
long sh_file_max = SH_FILE_MAX;
int mem_readers;                        /* files.c's readers at work, and the recalculation after one */

/* what reading a file takes besides the sheet it makes: build_new()'s table of rows, each row's struct until
   build_finish() moves the rows into the sheet's array (which is no bigger than the saved sheet's), and files.c's
   reader and buffers (about 43 KB; 128 KB kept). The reviewer measured the reader's need: 2,094,055 bytes over a
   sheet of 65,536 rows on 64 bits, under the 2,228,224 kept there; 1,703,936 are kept on i386 */
const long read_headroom = (long)(SH_ROWS * (sizeof(struct row *) + sizeof(struct row))) + 131072L;

/* the budget left for an allocation: all of it for a reader, read_headroom short of it for everything else */
static long room(void) {
    return sh_mem_max - sh_mem_used - (mem_readers ? 0 : read_headroom);
}

static const char *const error_names[SH_ERRORS + 1] = { "", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!",
                                                        "#N/A", "#NULL!", "#CYCLE!", "#ERROR!", "#LIMIT!" };

union head {                            /* in front of every block: its size, and a double's alignment after it */
    size_t n;
    double d;
    long long l;
};

/**** memory ****/

void *mem_alloc(size_t n) {
    union head *h;
    if(sh_mem_fail >= 0 && sh_mem_fail-- == 0) return NULL;
    if(n > (size_t)sh_mem_max || (long)n > room()) return NULL;
    h = malloc(sizeof(*h) + n);
    if(!h) return NULL;
    h->n = n;
    sh_mem_used += (long)n;
    sh_mem_blocks++;
    return h + 1;
}

void *mem_zalloc(size_t n) {
    void *p = mem_alloc(n);
    if(p) memset(p, 0, n);
    return p;
}

void *mem_realloc(void *p, size_t n) {
    union head *h, *nh;
    size_t old;
    if(!p) return mem_alloc(n);
    h = (union head *)p - 1;
    old = h->n;
    if(sh_mem_fail >= 0 && sh_mem_fail-- == 0) return NULL;
    if(n > old && (n - old > (size_t)sh_mem_max || (long)(n - old) > room())) return NULL;
    nh = realloc(h, sizeof(*h) + n);
    if(!nh) return NULL;
    sh_mem_used += (long)n - (long)old;
    nh->n = n;
    return nh + 1;
}

void mem_free(void *p) {
    union head *h;
    if(!p) return;
    h = (union head *)p - 1;
    sh_mem_used -= (long)h->n;
    sh_mem_blocks--;
    free(h);
}

char *mem_dup(const char *s, int len) {
    char *d = mem_alloc((size_t)len + 1);
    if(!d) return NULL;
    if(len) memcpy(d, s, (size_t)len);
    d[len] = 0;
    return d;
}

void sheet_set_why(struct sheet *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->why, sizeof(s->why), fmt, ap);
    va_end(ap);
}

const char *sh_error_name(int e) {
    return e >= 1 && e <= SH_ERRORS ? error_names[e] : "#VALUE!";
}

/**** finding and walking ****/

int row_index(const struct sheet *s, int r, int *found) {
    int lo = 0, hi = s->nrows, mid;
    while(lo < hi) {
        mid = lo + (hi - lo) / 2;
        if(s->rows[mid].r < r) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < s->nrows && s->rows[lo].r == r;
    return lo;
}

int cell_index(const struct row *rw, int c, int *found) {
    int lo = 0, hi = rw->n, mid;
    while(lo < hi) {
        mid = lo + (hi - lo) / 2;
        if(rw->cells[mid].col < c) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < rw->n && rw->cells[lo].col == c;
    return lo;
}

struct cell *cell_at(const struct sheet *s, int r, int c) {
    int ri, ci, found;
    ri = row_index(s, r, &found);
    if(!found) return NULL;
    ci = cell_index(&s->rows[ri], c, &found);
    return found ? &s->rows[ri].cells[ci] : NULL;
}

void walk_start(const struct sheet *s, struct walk *w, int r1, int c1, int r2, int c2, int formulas) {
    int found;
    w->r1 = r1;
    w->c1 = c1;
    w->r2 = r2;
    w->c2 = c2;
    w->formulas = formulas;
    w->ri = row_index(s, r1, &found);
    w->ci = -1;
}

/* the range's next cell that holds something (with formulas set, the next formula), row by row; each row and cell
   looked at is a step */
struct cell *walk_next(const struct sheet *s, struct walk *w, int *row, long *steps) {
    const struct row *rw;
    struct cell *c;
    int found;
    for(;;) {
        if(w->ri >= s->nrows) return NULL;
        rw = &s->rows[w->ri];
        if(rw->r > w->r2) return NULL;
        if(w->ci < 0) {
            if(steps) ++*steps;
            if(!rw->n || (w->formulas && !rw->formulas)) {
                w->ri++;
                continue;
            }
            w->ci = cell_index(rw, w->c1, &found);
        }
        while(w->ci < rw->n && rw->cells[w->ci].col <= w->c2) {
            c = &rw->cells[w->ci++];
            if(steps) ++*steps;
            if(w->formulas && c->c.kind != K_FORMULA) continue;
            *row = rw->r;
            return c;
        }
        w->ri++;
        w->ci = -1;
    }
}

/**** contents ****/

int content_absent(const struct content *c) {
    return c->kind == K_NONE && c->fmt == SH_GENERAL;
}

/* a formula's block, its share of a recalculation kept back with it: NULL when the budget has no room */
static struct formula *formula_new(void) {
    struct formula *f;
    if(room() < recalc_per_formula) return NULL;
    f = mem_zalloc(sizeof(*f));
    if(f) sh_mem_used += recalc_per_formula;
    return f;
}

static void formula_free(struct formula *f) {
    if(!f) return;
    sh_mem_used -= recalc_per_formula;
    mem_free(f->src);
    mem_free(f->code);
    mem_free(f->vtext);
    mem_free(f);
}

void content_free(struct content *c) {
    mem_free(c->text);
    formula_free(c->f);
    memset(c, 0, sizeof(*c));
}

int content_formula(struct content *c, const char *src, int len, int fmt) {
    struct formula *f = formula_new();
    memset(c, 0, sizeof(*c));
    if(!f) return -1;
    f->src = mem_dup(src, len);
    f->len = len;
    if(!f->src || fx_compile(f) == -2) {
        formula_free(f);
        return -1;
    }
    c->kind = K_FORMULA;
    c->fmt = (unsigned char)fmt;
    c->f = f;
    return 0;
}

int content_copy(struct content *dst, const struct content *src) {
    *dst = *src;
    dst->text = NULL;
    dst->f = NULL;
    if(src->kind == K_TEXT) {
        dst->text = mem_dup(src->text, src->len);
        if(!dst->text) goto nomem;
    } else if(src->kind == K_FORMULA) {
        struct formula *f = formula_new();
        size_t bytes;
        if(!f) goto nomem;
        dst->f = f;
        f->len = src->f->len;
        f->src = mem_dup(src->f->src, src->f->len);
        if(!f->src) goto nomem;
        if(src->f->code) {                  /* the program and its strings are one block */
            bytes = (size_t)src->f->ncode * sizeof(struct ins) + strlen(FORMULA_POOL(src->f)) + 1;
            f->code = mem_alloc(bytes);
            if(!f->code) goto nomem;
            memcpy(f->code, src->f->code, bytes);
            f->ncode = src->f->ncode;
            f->depth = src->f->depth;
        }
    }
    return 0;
nomem:
    content_free(dst);
    return -1;
}

int content_equal(const struct content *a, const struct content *b) {
    if(a->kind != b->kind || a->fmt != b->fmt) return 0;
    switch(a->kind) {
    case K_NUMBER: return !memcmp(&a->num, &b->num, sizeof(a->num));
    case K_TEXT: return a->len == b->len && !memcmp(a->text, b->text, (size_t)a->len);
    case K_BOOL: case K_ERROR: return a->b == b->b;
    case K_FORMULA: return a->f->len == b->f->len && !memcmp(a->f->src, b->f->src, (size_t)a->f->len);
    }
    return 1;
}

static int word(const char *s, int len, const char *w) {
    return len == (int)strlen(w) && !strncasecmp(s, w, (size_t)len);
}

static int error_word(const char *s, int len) {
    int k;
    for(k = 1; k <= SH_ERRORS; k++) if(word(s, len, error_names[k])) return k;
    return 0;
}

static int make_text(struct content *c, const char *s, int len) {
    c->text = mem_dup(s, len);
    if(!c->text) return -1;
    c->kind = K_TEXT;
    c->len = len;
    return 0;
}

/* text typed: = a formula (its references and names put in capitals), ' text as it follows, a number (a % makes
   a General cell a percent one), TRUE or FALSE, an error's name, or else text. Nothing: no content */
int content_parse(struct content *c, const char *text, int len, int fmt) {
    static char norm[SH_INPUT_MAX + 1];
    double v;
    int pct, n;
    memset(c, 0, sizeof(*c));
    c->fmt = (unsigned char)fmt;
    if(len <= 0) return 0;
    if(text[0] == '=') {
        n = len - 1 <= SH_INPUT_MAX ? fx_rewrite(text + 1, len - 1, FX_NORMAL, 0, 0, norm, sizeof(norm)) : -1;
        if(n < 0 || n != len - 1) return content_formula(c, text + 1, len - 1, fmt);
        return content_formula(c, norm, n, fmt);
    }
    if(text[0] == '\'') return make_text(c, text + 1, len - 1);
    if(num_read(text, len, &v, &pct)) {
        c->kind = K_NUMBER;
        c->num = v;
        if(pct && SH_FMT_KIND(fmt) == SH_GENERAL) c->fmt = (unsigned char)SH_PERCENT(pct - 1);
        return 0;
    }
    if(word(text, len, "TRUE") || word(text, len, "FALSE")) {
        c->kind = K_BOOL;
        c->b = (unsigned char)word(text, len, "TRUE");
        return 0;
    }
    if((n = error_word(text, len))) {
        c->kind = K_ERROR;
        c->b = (unsigned char)n;
        return 0;
    }
    return make_text(c, text, len);
}

/* whether len bytes as typed fit a cell: SH_INPUT_MAX, and one more for the ' in front of a text of that many */
int input_fits(const char *t, int len) {
    return len <= SH_INPUT_MAX || (len == SH_INPUT_MAX + 1 && t[0] == '\'');
}

/* text that typing would take for something else, and that therefore shows with a ' in front */
int text_needs_quote(const char *t, int len) {
    double v;
    int pct;
    return !len || t[0] == '=' || t[0] == '\'' || num_read(t, len, &v, &pct) || word(t, len, "TRUE") ||
           word(t, len, "FALSE") || error_word(t, len);
}

static int copy_out(char *out, int size, const char *s, int n) {
    if(size > 0) {
        int k = n < size - 1 ? n : size - 1;
        memcpy(out, s, (size_t)k);
        out[k] = 0;
    }
    return n;
}

int content_input(const struct content *c, char *out, int size) {
    int n;
    switch(c->kind) {
    case K_NUMBER:
        if(SH_FMT_KIND(c->fmt) == SH_PERCENT(0)) return num_shortest_percent(c->num, out, size);
        return num_shortest(c->num, out, size);
    case K_TEXT:
        if(!text_needs_quote(c->text, c->len)) return copy_out(out, size, c->text, c->len);
        n = copy_out(out, size, "'", 1);
        if(size > 1) copy_out(out + 1, size - 1, c->text, c->len);
        return n + c->len;
    case K_BOOL:
        return copy_out(out, size, c->b ? "TRUE" : "FALSE", c->b ? 4 : 5);
    case K_ERROR:
        return copy_out(out, size, sh_error_name(c->b), (int)strlen(sh_error_name(c->b)));
    case K_FORMULA:
        n = copy_out(out, size, "=", 1);
        if(size > 1) copy_out(out + 1, size - 1, c->f->src, c->f->len);
        return n + c->f->len;
    }
    return copy_out(out, size, "", 0);
}

void cell_value(const struct sheet *s, const struct cell *c, struct sh_value *v) {
    (void)s;
    memset(v, 0, sizeof(*v));
    switch(c->c.kind) {
    case K_NUMBER:
        v->type = SH_NUMBER;
        v->num = c->c.num;
        break;
    case K_TEXT:
        v->type = SH_TEXT;
        v->text = c->c.text;
        v->len = c->c.len;
        break;
    case K_BOOL:
        v->type = SH_BOOL;
        v->b = c->c.b;
        break;
    case K_ERROR:
        v->type = SH_ERROR;
        v->b = c->c.b;
        break;
    case K_FORMULA:
        v->type = c->c.f->vtype;
        v->b = c->c.f->vb;
        v->num = c->c.f->vnum;
        v->text = c->c.f->vtext;
        v->len = c->c.f->vlen;
        if(v->type == SH_TEXT && !v->text) {
            v->text = "";
            v->len = 0;
        }
        break;
    default:
        v->type = SH_EMPTY;
        break;
    }
}

/**** changing cells ****/

static void count_formula(struct sheet *s, struct row *rw, const struct content *c, int d) {
    if(c->kind != K_FORMULA) return;
    rw->formulas += d;
    s->nformulas += d;
}

/* a formula that went out of a cell when a change was made: its value matters no more, and is not kept in the
   history (edit.c calls it once the change is whole, so that one taken back halfway keeps every value) */
void content_forget_value(struct content *c) {
    if(c->kind != K_FORMULA) return;
    mem_free(c->f->vtext);
    c->f->vtext = NULL;
    c->f->vlen = 0;
    c->f->vtype = SH_EMPTY;
}

int cell_swap(struct sheet *s, int r, int c, struct content *other) {
    struct content t;
    struct row *rw;
    struct cell *cl;
    int ri, ci, found_row, found;
    ri = row_index(s, r, &found_row);
    if(found_row) {
        rw = &s->rows[ri];
        ci = cell_index(rw, c, &found);
        if(found) {
            cl = &rw->cells[ci];
            count_formula(s, rw, &cl->c, -1);
            t = cl->c;
            cl->c = *other;
            *other = t;
            if(content_absent(&cl->c)) {        /* no cell any more; the row stays until the change ends */
                memmove(cl, cl + 1, (size_t)(rw->n - ci - 1) * sizeof(*cl));
                rw->n--;
                s->ncells--;
            } else count_formula(s, rw, &cl->c, 1);
            return 0;
        }
    }
    if(content_absent(other)) return 0;
    if(!found_row) {
        if(s->nrows == s->caprows) {
            int cap = s->caprows ? s->caprows * 2 : 16;
            struct row *nr = mem_realloc(s->rows, (size_t)cap * sizeof(*nr));
            if(!nr) return -1;
            s->rows = nr;
            s->caprows = cap;
        }
        memmove(&s->rows[ri + 1], &s->rows[ri], (size_t)(s->nrows - ri) * sizeof(struct row));
        memset(&s->rows[ri], 0, sizeof(struct row));
        s->rows[ri].r = r;
        s->nrows++;
    }
    rw = &s->rows[ri];
    ci = cell_index(rw, c, &found);
    if(rw->n == rw->cap) {
        int cap = rw->cap ? rw->cap * 2 : 4;
        struct cell *nc;
        if(cap > SH_COLS) cap = SH_COLS;
        nc = mem_realloc(rw->cells, (size_t)cap * sizeof(*nc));
        if(!nc) return -1;                      /* an empty row may be left: drop_empty_rows() takes it */
        rw->cells = nc;
        rw->cap = cap;
    }
    cl = &rw->cells[ci];
    memmove(cl + 1, cl, (size_t)(rw->n - ci) * sizeof(*cl));
    rw->n++;
    s->ncells++;
    cl->col = c;
    cl->c = *other;
    memset(other, 0, sizeof(*other));
    count_formula(s, rw, &cl->c, 1);
    return 0;
}

void drop_empty_rows(struct sheet *s) {
    int i, k = 0;
    for(i = 0; i < s->nrows; i++) {
        if(!s->rows[i].n) {
            mem_free(s->rows[i].cells);
            continue;
        }
        if(k != i) s->rows[k] = s->rows[i];
        k++;
    }
    s->nrows = k;
}

/* rows from at on move down by n; with n < 0, the -n rows from at, emptied before, go and those below move up */
void shift_rows(struct sheet *s, int at, int n) {
    int i;
    drop_empty_rows(s);
    for(i = 0; i < s->nrows; i++) {
        if(s->rows[i].r < at) continue;
        s->rows[i].r += n;
    }
}

/* the same for columns, with their widths: those that come in have the default width */
void shift_cols(struct sheet *s, int at, int n) {
    int i, j;
    for(i = 0; i < s->nrows; i++)
        for(j = 0; j < s->rows[i].n; j++)
            if(s->rows[i].cells[j].col >= at) s->rows[i].cells[j].col += n;
    if(n > 0) {
        memmove(&s->width[at + n], &s->width[at], (size_t)(SH_COLS - at - n) * sizeof(s->width[0]));
        for(i = at; i < at + n; i++) s->width[i] = SH_WIDTH_DEFAULT;
    } else {
        memmove(&s->width[at], &s->width[at - n], (size_t)(SH_COLS - at + n) * sizeof(s->width[0]));
        for(i = SH_COLS + n; i < SH_COLS; i++) s->width[i] = SH_WIDTH_DEFAULT;
    }
}

/**** a sheet read from a file: cells in any order, sorted at the end ****/

struct build {
    struct row **tab;                   /* by row number */
    int cells, rows;
};

struct build *build_new(void) {
    struct build *b = mem_zalloc(sizeof(*b));
    if(!b) return NULL;
    b->tab = mem_zalloc(SH_ROWS * sizeof(*b->tab));
    if(!b->tab) {
        mem_free(b);
        return NULL;
    }
    return b;
}

void build_free(struct build *b) {
    int r, i;
    if(!b) return;
    for(r = 0; r < SH_ROWS; r++) {
        if(!b->tab[r]) continue;
        for(i = 0; i < b->tab[r]->n; i++) content_free(&b->tab[r]->cells[i].c);
        mem_free(b->tab[r]->cells);
        mem_free(b->tab[r]);
    }
    mem_free(b->tab);
    mem_free(b);
}

/* ct goes into the build, or is freed: 0, or BUILD_NOMEM, BUILD_FULL (too many cells) or BUILD_TWICE (a row with
   more cells than columns: one of them is there twice) */
int build_add(struct build *b, int r, int c, struct content *ct) {
    struct row *rw;
    int e = BUILD_NOMEM;
    if(content_absent(ct)) return 0;
    if(b->cells >= SH_CELLS_MAX) {
        e = BUILD_FULL;
        goto fail;
    }
    rw = b->tab[r];
    if(!rw) {
        rw = b->tab[r] = mem_zalloc(sizeof(*rw));
        if(!rw) goto fail;
        rw->r = r;
        b->rows++;
    }
    if(rw->n == rw->cap) {
        int cap = rw->cap ? rw->cap * 2 : 4;
        struct cell *nc;
        if(rw->n >= SH_COLS) {
            e = BUILD_TWICE;
            goto fail;
        }
        if(cap > SH_COLS) cap = SH_COLS;
        nc = mem_realloc(rw->cells, (size_t)cap * sizeof(*nc));
        if(!nc) goto fail;
        rw->cells = nc;
        rw->cap = cap;
    }
    rw->cells[rw->n].col = c;
    rw->cells[rw->n].c = *ct;
    rw->n++;
    b->cells++;
    memset(ct, 0, sizeof(*ct));
    return 0;
fail:
    content_free(ct);
    return e;
}

static int by_col(const void *a, const void *b) {
    const struct cell *x = a, *y = b;
    return (x->col > y->col) - (x->col < y->col);
}

/* the build's cells into the empty sheet s: 0; or -1 with why (a cell given twice), the build kept for build_free() */
int build_finish(struct build *b, struct sheet *s, char *why, int size) {
    char name[16];
    int r, i, k = 0;
    for(r = 0; r < SH_ROWS; r++) {
        struct row *rw = b->tab[r];
        if(!rw) continue;
        qsort(rw->cells, (size_t)rw->n, sizeof(struct cell), by_col);
        for(i = 1; i < rw->n; i++) {
            if(rw->cells[i].col != rw->cells[i - 1].col) continue;
            sh_cellname(r, rw->cells[i].col, name);
            snprintf(why, (size_t)size, "%s is in the file twice", name);
            return -1;
        }
    }
    s->rows = mem_alloc((size_t)(b->rows ? b->rows : 1) * sizeof(struct row));
    if(!s->rows) {
        snprintf(why, (size_t)size, "Not enough memory for the sheet");
        return -1;
    }
    s->caprows = b->rows ? b->rows : 1;
    for(r = 0; r < SH_ROWS; r++) {
        struct row *rw = b->tab[r];
        if(!rw) continue;
        s->rows[k] = *rw;
        s->rows[k].formulas = 0;
        for(i = 0; i < rw->n; i++) if(rw->cells[i].c.kind == K_FORMULA) s->rows[k].formulas++;
        s->ncells += rw->n;
        s->nformulas += s->rows[k].formulas;
        k++;
        mem_free(rw);
        b->tab[r] = NULL;
    }
    s->nrows = k;
    return 0;
}

/**** the sheet ****/

struct sheet *sh_new(void) {
    struct sheet *s = mem_zalloc(sizeof(*s));
    int i;
    if(!s) return NULL;
    for(i = 0; i < SH_COLS; i++) s->width[i] = SH_WIDTH_DEFAULT;
    return s;
}

void sh_free(struct sheet *s) {
    int i, j;
    if(!s) return;
    for(i = 0; i < s->nrows; i++) {
        for(j = 0; j < s->rows[i].n; j++) content_free(&s->rows[i].cells[j].c);
        mem_free(s->rows[i].cells);
    }
    mem_free(s->rows);
    hist_free(s);
    mem_free(s);
}

void sh_value(const struct sheet *s, int row, int col, struct sh_value *v) {
    const struct cell *c = cell_at(s, row, col);
    if(c) cell_value(s, c, v);
    else {
        memset(v, 0, sizeof(*v));
        v->type = SH_EMPTY;
    }
}

int sh_format(const struct sheet *s, int row, int col) {
    const struct cell *c = cell_at(s, row, col);
    return c ? c->c.fmt : SH_GENERAL;
}

int sh_input(const struct sheet *s, int row, int col, char *out, int size) {
    const struct cell *c = cell_at(s, row, col);
    if(!c) return copy_out(out, size, "", 0);
    return content_input(&c->c, out, size);
}

int sh_is_formula(const struct sheet *s, int row, int col) {
    const struct cell *c = cell_at(s, row, col);
    return c && c->c.kind == K_FORMULA;
}

int sh_display(const struct sheet *s, int row, int col, int chars, char *out, int size, int *align) {
    struct sh_value v;
    int fmt = sh_format(s, row, col), n, i;
    sh_value(s, row, col, &v);
    *align = SH_LEFT;
    switch(v.type) {
    case SH_NUMBER:
        *align = SH_RIGHT;
        if(SH_FMT_KIND(fmt) == SH_GENERAL) return num_general(v.num, chars, out, size);
        n = num_fixed(v.num, SH_FMT_DECIMALS(fmt), SH_FMT_KIND(fmt) == SH_PERCENT(0), out, size);
        if(chars > 0 && n > chars) {            /* a fixed format does not fit: hashes, as wide as the cell */
            for(i = 0; i < chars && i < size - 1; i++) out[i] = '#';
            if(size > 0) out[i < size ? i : size - 1] = 0;
            return chars;
        }
        return n;
    case SH_TEXT:
        return copy_out(out, size, v.text, v.len);
    case SH_BOOL:
        *align = SH_CENTER;
        return copy_out(out, size, v.b ? "TRUE" : "FALSE", v.b ? 4 : 5);
    case SH_ERROR:
        *align = SH_CENTER;
        return copy_out(out, size, sh_error_name(v.b), (int)strlen(sh_error_name(v.b)));
    }
    return copy_out(out, size, "", 0);
}

int sh_problem(const struct sheet *s, int row, int col, char *out, int size) {
    const struct cell *c = cell_at(s, row, col);
    if(size > 0) out[0] = 0;
    if(!c || c->c.kind != K_FORMULA) return 0;
    return fx_check(c->c.f->src, c->c.f->len, out, size);
}

int sh_width(const struct sheet *s, int col) {
    return col >= 0 && col < SH_COLS ? s->width[col] : SH_WIDTH_DEFAULT;
}

void sh_extent(const struct sheet *s, int *rows, int *cols) {
    int i, j;
    *rows = *cols = 0;
    for(i = 0; i < s->nrows; i++)
        for(j = 0; j < s->rows[i].n; j++) {
            if(s->rows[i].cells[j].c.kind == K_NONE) continue;
            if(s->rows[i].r + 1 > *rows) *rows = s->rows[i].r + 1;
            if(s->rows[i].cells[j].col + 1 > *cols) *cols = s->rows[i].cells[j].col + 1;
        }
}

int sh_cells(const struct sheet *s) {
    return s->ncells;
}

int sh_formulas(const struct sheet *s) {
    return s->nformulas;
}

void sh_stats(const struct sheet *s, struct sh_range r, double *sum, int *numbers, int *values) {
    struct walk w;
    struct sh_value v;
    struct cell *c;
    double t, comp = 0, total = 0;
    int row;
    *numbers = *values = 0;
    walk_start(s, &w, r.r1, r.c1, r.r2, r.c2, 0);
    while((c = walk_next(s, &w, &row, NULL))) {
        cell_value(s, c, &v);
        if(v.type == SH_EMPTY) continue;
        ++*values;
        if(v.type != SH_NUMBER) continue;
        ++*numbers;
        t = total + v.num;                      /* Neumaier's sum, as SUM() */
        if((total < 0 ? -total : total) >= (v.num < 0 ? -v.num : v.num)) comp += (total - t) + v.num;
        else comp += (v.num - t) + total;
        total = t;
    }
    *sum = total + comp;
}

int sh_modified(const struct sheet *s) {
    return s->state != s->saved;
}

unsigned sh_state(const struct sheet *s) {
    return s->state;
}

void sh_saved(struct sheet *s) {
    s->saved = s->state;
}

int sh_limited(const struct sheet *s) {
    return s->limited;
}

const char *sh_why(const struct sheet *s) {
    return s->why;
}
