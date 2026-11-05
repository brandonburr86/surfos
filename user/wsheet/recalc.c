/*
SurfOS user program: wsheet
--------------------
File: recalc.c  Date: 11/05/26
--------------------
Recalculation: every formula again, each one after the formulas it refers to, which every change
ends with.

The formulas and what they refer to (a cell, or the formulas inside a range) are a directed
graph, and Tarjan's algorithm for its strongly connected components (R. Tarjan, "Depth-first
search and linear graph algorithms", SIAM J. Computing 1(2), 1972) gives the order: it finishes a
component only after every component it reaches, so a formula is worked out after those it uses.
A component of more than one formula, or one that refers to itself, is a cycle, and each of its
formulas shows #CYCLE!; a formula that only uses one gets that value like any other and passes it
on (or not: COUNTA counts it). The depth-first search keeps its own stack of frames, never C's,
so a chain of 65,536 formulas each using the one above costs no recursion.

The whole sheet is recalculated each time: a sheet of SurfOS's size takes milliseconds, and no
change can leave a value stale. Its work is bounded: every reference followed, row and cell looked
at in a range, and instruction run is a step, and past sh_steps_max it stops. The formulas it did
not finish then show #LIMIT!, and sh_limited() says so; so does memory running out.
*/

#include <string.h>
#include "sheetpriv.h"

#define M_ONSTACK 1
#define M_DONE 2
#define M_SELF 4

struct frame {
    struct formula *f;
    int ins;                            /* the instruction whose references are followed */
    int inrange;                        /* its range is being walked */
    struct walk w;
};

/* the next formula t's formula refers to, or NULL when there are no more */
static struct formula *next_used(const struct sheet *s, struct frame *t, long *steps) {
    const struct ins *in;
    struct cell *c;
    int row;
    for(;;) {
        if(t->inrange) {
            c = walk_next(s, &t->w, &row, steps);
            if(c) return c->c.f;
            t->inrange = 0;
            t->ins++;
            continue;
        }
        if(t->ins >= t->f->ncode) return NULL;
        in = &t->f->code[t->ins];
        if(in->op == I_RANGE) {
            walk_start(s, &t->w, in->r1, in->c1, in->r2, in->c2, 1);
            t->inrange = 1;
            continue;
        }
        t->ins++;
        if(in->op != I_REF) continue;
        ++*steps;
        c = cell_at(s, in->r1, in->c1);
        if(c && c->c.kind == K_FORMULA) return c->c.f;
    }
}

/* the depth-first search's stacks: its frames, and the formulas of components not yet finished */
struct search {
    struct frame *fr;
    struct formula **comp;
    int sp, top, counter;
};

static void push(struct search *x, struct formula *w) {
    w->index = w->low = x->counter++;
    w->mark |= M_ONSTACK;
    x->comp[x->top++] = w;
    x->fr[x->sp].f = w;
    x->fr[x->sp].ins = 0;
    x->fr[x->sp].inrange = 0;
    x->sp++;
}

static void every_formula_limit(struct sheet *s, int all) {
    int i, j;
    for(i = 0; i < s->nrows; i++)
        for(j = 0; j < s->rows[i].n; j++) {
            struct content *c = &s->rows[i].cells[j].c;
            if(c->kind == K_FORMULA && (all || !(c->f->mark & M_DONE))) ev_set_error(c->f, SH_LIMIT);
        }
}

void recalc(struct sheet *s) {
    struct search x;
    struct formula **all, *f, *w;
    int n = s->nformulas, i, j, k = 0, r;
    long steps = 0;
    s->limited = 0;
    if(!n) return;
    memset(&x, 0, sizeof(x));
    all = mem_alloc((size_t)n * sizeof(*all));
    x.comp = mem_alloc((size_t)n * sizeof(*x.comp));
    x.fr = mem_alloc((size_t)n * sizeof(*x.fr));
    if(!all || !x.comp || !x.fr) {
        every_formula_limit(s, 1);
        s->limited = 1;
        goto out;
    }
    for(i = 0; i < s->nrows; i++)
        for(j = 0; j < s->rows[i].n; j++) {
            struct content *c = &s->rows[i].cells[j].c;
            if(c->kind != K_FORMULA || k >= n) continue;
            all[k++] = c->f;
            c->f->index = -1;
            c->f->mark = 0;
        }
    for(i = 0; i < k && !s->limited; i++) {
        if(all[i]->index >= 0) continue;
        x.sp = 0;
        push(&x, all[i]);
        while(x.sp > 0) {
            struct frame *t = &x.fr[x.sp - 1];
            w = next_used(s, t, &steps);
            if(steps > sh_steps_max) {
                s->limited = 1;
                break;
            }
            if(w) {
                if(w == t->f) t->f->mark |= M_SELF;
                else if(w->index < 0) push(&x, w);
                else if((w->mark & M_ONSTACK) && w->index < t->f->low) t->f->low = w->index;
                continue;
            }
            f = t->f;                           /* every formula f uses is done or on the stack */
            x.sp--;
            if(x.sp > 0 && f->low < x.fr[x.sp - 1].f->low) x.fr[x.sp - 1].f->low = f->low;
            if(f->low != f->index) continue;
            for(j = x.top - 1; x.comp[j] != f; j--);        /* f roots a component: comp[j..top) */
            if(x.top - j > 1 || (f->mark & M_SELF)) {
                for(r = j; r < x.top; r++) {
                    ev_set_error(x.comp[r], SH_CYCLE);
                    x.comp[r]->mark = (unsigned char)((x.comp[r]->mark & ~M_ONSTACK) | M_DONE);
                }
            } else {
                r = ev_formula(s, f, &steps);
                if(r == -1) {
                    s->limited = 1;
                    break;
                }
                if(r == -2) s->limited = 1;     /* no memory for this one: #LIMIT!, and on with the rest */
                f->mark = (unsigned char)((f->mark & ~M_ONSTACK) | M_DONE);
            }
            x.top = j;
        }
    }
    if(s->limited) every_formula_limit(s, 0);
out:
    mem_free(all);
    mem_free(x.comp);
    mem_free(x.fr);
}
