/*
SurfOS user program: bc
--------------------
File: vm.c      Date: 11/01/26
--------------------
The machine that runs what parse.c writes: a stack of numbers, the names, and the calls.

The names are dynamic, as POSIX has them: a call saves the values of its parameters and autos on
a stack of its own and gives the names new ones, so a function it calls sees them too, and the
return puts the old ones back. Arrays go to a function by value, so a call copies them; numbers
are shared by counting, so the copy is of pointers. The copies are all made before any name
changes: in f(b[], a[]) of define f(a[], b[]) each parameter is the other's old array.

The stacks grow on the heap with checks, and bc_exec() does not recurse, so a function may call
itself as deep as memory goes. When anything fails the run ends with one message and everything
is unwound: the names get their values back, and on a terminal the next line starts clean. A
function of the library also gets its caller's scale back then: it changes scale to work.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc.h"

#define LEAF_BITS 10
#define LEAF (1u << LEAF_BITS)
#define TOP(k) (b->stack[b->sp - 1 - (k)])

/**** messages and output ****/

void bc_say(struct bc *b, const char *what) {
    char line[512];                     /* a whole path and the longest message */
    b->host.flush(b->host.ctx);
    snprintf(line, sizeof(line), "%s: line %u: %s", b->name ? b->name : "bc", b->line, what);
    b->host.error(b->host.ctx, line);
}

static void say_in(struct bc *b, const struct func *f, const char *what) {
    char line[160];
    unsigned k = b->nframes;
    while(f->lib && k && b->frames[k - 1].caller->lib) f = b->frames[--k].caller;   /* the library's helpers are its own business */
    if(f == &b->main) {
        bc_say(b, what);
        return;
    }
    snprintf(line, sizeof(line), "%.32s: %s", b->sym[f->sym].name, what);
    bc_say(b, line);
}

/* everything bc prints goes out here, and the column is counted with it */
static void put_out(struct bc *b, int c) {
    b->col = c == '\n' ? 0 : b->col + 1;
    b->host.put(b->host.ctx, c);
}

static void put_text(struct bc *b, const char *s) {
    for(; *s; s++) put_out(b, (unsigned char)*s);
}

/* POSIX: a long number is split over lines of 70 characters with the newline, and all but the last line end in a
   backslash. So a line takes 68 and the backslash, or 69 when they end the number, and which it is depends on
   the character after: each waits here for the next. A number does not start in the 69th place, or past it where
   text has gone that far. All of this is what the Mac's bc prints. */
static void put_number_char(void *ctx, int c) {
    struct bc *b = ctx;
    if(b->col >= BC_LINE_CHARS) put_text(b, "\\\n");
    if(b->held) put_out(b, b->held);
    b->held = c;
}

/**** names ****/

static unsigned hash_name(const char *s, unsigned n) {
    unsigned h = 5381;
    while(n--) h = h * 33 + (unsigned char)*s++;
    return h % BC_HASH;
}

int bc_intern(struct bc *b, const char *name, unsigned len) {
    unsigned h = hash_name(name, len), i;
    struct sym *y;
    for(i = b->hash[h]; i; i = b->sym[i - 1].next)
        if(!strcmp(b->sym[i - 1].name, name)) return (int)i - 1;
    if(b->nsym == b->capsym) {
        unsigned cap = b->capsym ? b->capsym * 2 : 32;
        struct sym *v = cap < 0x01000000u ? realloc(b->sym, cap * sizeof(*v)) : NULL;
        if(!v) return -1;
        b->sym = v;
        b->capsym = cap;
    }
    y = &b->sym[b->nsym];
    memset(y, 0, sizeof(*y));
    if(!(y->name = malloc(len + 1))) return -1;
    memcpy(y->name, name, len + 1);
    y->next = b->hash[h];
    b->hash[h] = ++b->nsym;
    return (int)b->nsym - 1;
}

void bc_func_clear(struct func *f) {
    unsigned i;
    for(i = 0; i < f->nk; i++) {
        free(f->k[i].text);
        num_put(f->k[i].value);
    }
    for(i = 0; i < f->ntext; i++) free(f->text[i]);
    f->nk = f->ntext = f->ncode = 0;
}

void bc_func_free(struct func *f) {
    bc_func_clear(f);
    free(f->code);
    free(f->k);
    free(f->text);
    free(f->loc);
    free(f);
}

/**** arrays ****/

static void array_free(struct array *a) {
    unsigned i, k;
    if(!a) return;
    for(i = 0; i < a->ntop; i++) {
        if(!a->top[i]) continue;
        for(k = 0; k < LEAF; k++) num_put(a->top[i][k]);
        free(a->top[i]);
    }
    free(a->top);
    free(a);
}

static struct array *array_copy(const struct array *a) {
    struct array *r = calloc(1, sizeof(*r));
    unsigned i, k;
    if(!r) return NULL;
    if(!a || !a->ntop) return r;
    if(!(r->top = calloc(a->ntop, sizeof(*r->top)))) goto fail;
    r->ntop = a->ntop;
    for(i = 0; i < a->ntop; i++) {
        if(!a->top[i]) continue;
        if(!(r->top[i] = malloc(LEAF * sizeof(struct num *)))) goto fail;
        for(k = 0; k < LEAF; k++) {
            r->top[i][k] = a->top[i][k];
            if(r->top[i][k]) num_ref(r->top[i][k]);
        }
    }
    return r;
fail:
    array_free(r);
    return NULL;
}

/* the place of element i; NULL when it has none yet and create is 0, or memory ran out */
static struct num **array_slot(struct array *a, unsigned i, int create) {
    unsigned t = i >> LEAF_BITS;
    if(t >= a->ntop) {
        struct num ***v;
        if(!create) return NULL;
        v = realloc(a->top, (t + 1) * sizeof(*v));
        if(!v) return NULL;
        memset(v + a->ntop, 0, (t + 1 - a->ntop) * sizeof(*v));
        a->top = v;
        a->ntop = t + 1;
    }
    if(!a->top[t]) {
        if(!create) return NULL;
        a->top[t] = calloc(LEAF, sizeof(struct num *));
        if(!a->top[t]) return NULL;
    }
    return &a->top[t][i & (LEAF - 1)];
}

/**** the stacks ****/

/* v with room for need items: the same, a new block, or NULL with v as it was */
static void *grown(void *v, unsigned *cap, unsigned need, size_t size) {
    uint64_t n = *cap ? *cap : 64;
    void *p;
    while(n < need) n *= 2;
    if(n == *cap) return v;
    if(n * size > 0x7ff00000u || !(p = realloc(v, (size_t)(n * size)))) return NULL;
    *cap = (unsigned)n;
    return p;
}

/* takes the reference */
static int push(struct bc *b, struct num *n) {
    if(b->sp == b->capstack) {
        struct num **s = grown(b->stack, &b->capstack, b->sp + 1, sizeof(*b->stack));
        if(!s) {
            num_put(n);
            return -1;
        }
        b->stack = s;
    }
    b->stack[b->sp++] = n;
    return 0;
}

/* The names of g get their new values: the n values on the stack, array copies, nothing for
   the autos. All that can fail comes before the first name changes. */
static int enter(struct bc *b, struct func *g, const int *args, unsigned n) {
    union saved *s = grown(b->saved, &b->capsaved, b->nsaved + g->nloc, sizeof(*b->saved));
    struct frame *fr;
    unsigned i, values = 0, v;
    if(!s) return -1;
    b->saved = s;
    s += b->nsaved;
    if(!(fr = grown(b->frames, &b->capframes, b->nframes + 1, sizeof(*b->frames)))) return -1;
    b->frames = fr;
    for(i = 0; i < n; i++) {
        if(!args[i]) {
            values++;
            continue;
        }
        if(!(s[i].a = array_copy(b->sym[args[i] - 1].arr))) {
            while(i--) if(args[i]) array_free(s[i].a);
            return -1;
        }
    }
    v = b->sp - values;
    for(i = 0; i < g->nloc; i++) {
        struct sym *y = &b->sym[g->loc[i].sym];
        if(g->loc[i].array) {
            struct array *a = i < n ? s[i].a : NULL;
            s[i].a = y->arr;
            y->arr = a;
        } else {
            s[i].n = y->var;
            y->var = i < n ? b->stack[v++] : NULL;
        }
    }
    b->sp -= values;
    b->nsaved += g->nloc;
    return 0;
}

static void leave(struct bc *b, struct func *g) {
    unsigned i = g->nloc;
    while(i--) {
        struct sym *y = &b->sym[g->loc[i].sym];
        union saved *s = &b->saved[--b->nsaved];
        if(g->loc[i].array) {
            array_free(y->arr);
            y->arr = s->a;
        } else {
            num_put(y->var);
            y->var = s->n;
        }
    }
}

/* the value of a constant in the input base: a single digit is its own value in any base */
static struct num *constant(struct bc *b, struct konst *k, int lib) {
    unsigned base = lib ? 10 : b->ibase;
    if(!k->value || k->base != base) {
        struct num *v;
        if(k->len == 1 && k->text[0] >= 'A') v = num_uint((unsigned)(k->text[0] - 'A' + 10));
        else if(k->len == 1 && k->text[0] >= '0' && k->text[0] <= '9') v = num_uint((unsigned)(k->text[0] - '0'));
        else v = num_parse(k->text, k->len, base);
        if(!v) return NULL;
        num_put(k->value);
        k->value = v;
        k->base = base;
    }
    return num_ref(k->value);
}

/* the index on the stack as a number; -1 said */
static int array_index(struct bc *b, const struct func *f, const struct num *n, unsigned *i) {
    if(num_whole(n, i) < 0 || *i >= BC_DIM_MAX) {
        say_in(b, f, "array index too large");
        return -1;
    }
    if(n->neg && *i) {
        say_in(b, f, "negative array index");
        return -1;
    }
    return 0;
}

static int store_register(struct bc *b, const struct func *f, unsigned r) {
    static const char *const what[] = { "scale must be from 0 to 2147483647", "ibase must be from 2 to 16",
                                        "obase must be from 2 to 100000" };
    static const unsigned lo[] = { 0, 2, 2 }, hi[] = { NUM_MAX_SCALE, 16, NUM_MAX_BASE };
    const struct num *n = TOP(0);
    unsigned v;
    if(num_whole(n, &v) < 0 || (n->neg && v) || v < lo[r] || v > hi[r]) {
        say_in(b, f, what[r]);
        return -1;
    }
    if(r == REG_SCALE) b->scale = v;
    else if(r == REG_IBASE) b->ibase = v;
    else b->obase = v;
    return 0;
}

/* every call still open ends, and the stack empties: an error's way out */
static void unwind(struct bc *b, struct func *f) {
    while(b->nframes) {
        struct frame *fr = &b->frames[--b->nframes];
        leave(b, f);
        if(f->lib) b->scale = fr->scale;
        f = fr->caller;
    }
    while(b->sp) num_put(b->stack[--b->sp]);
}

/**** the machine ****/

int bc_exec(struct bc *b, struct func *f) {
    const int *code = f->code;
    unsigned pc = 0, i;
    struct num *r = NULL, **slot;
    int truth;

    for(;;) {
        switch(code[pc++]) {
        case OP_END:
            return 0;

        case OP_CONST:
            if(!(r = constant(b, &f->k[code[pc++]], f->lib))) goto num_trouble;
            if(push(b, r)) goto no_memory;
            break;

        case OP_ZERO:
            if(push(b, num_ref(b->zero))) goto no_memory;
            break;

        case OP_ONE:
            if(push(b, num_ref(b->one))) goto no_memory;
            break;

        case OP_LOADV:
            r = b->sym[code[pc++]].var;
            if(push(b, num_ref(r ? r : b->zero))) goto no_memory;
            break;

        case OP_STOREV: {
            struct sym *y = &b->sym[code[pc++]];
            num_put(y->var);
            y->var = num_ref(TOP(0));
            break;
        }

        case OP_LOADA: {
            struct array *a = b->sym[code[pc++]].arr;
            if(array_index(b, f, TOP(0), &i)) goto fail;
            slot = a ? array_slot(a, i, 0) : NULL;
            r = slot && *slot ? *slot : b->zero;
            num_put(TOP(0));
            TOP(0) = num_ref(r);
            break;
        }

        case OP_STOREA: {
            struct sym *y = &b->sym[code[pc++]];
            if(array_index(b, f, TOP(1), &i)) goto fail;
            if(!y->arr && !(y->arr = calloc(1, sizeof(*y->arr)))) goto no_memory;
            if(!(slot = array_slot(y->arr, i, 1))) goto no_memory;
            num_put(*slot);
            *slot = num_ref(TOP(0));
            num_put(TOP(1));                                    /* the index goes, the value stays */
            TOP(1) = TOP(0);
            b->sp--;
            break;
        }

        case OP_LOADR:
            i = code[pc++];
            r = num_uint(i == REG_SCALE ? b->scale : i == REG_IBASE ? b->ibase : b->obase);
            if(!r) goto num_trouble;
            if(push(b, r)) goto no_memory;
            break;

        case OP_STORER:
            /* a register takes the whole part, and that is the assignment's value too */
            i = code[pc++];
            if(store_register(b, f, i)) goto fail;
            r = num_uint(i == REG_SCALE ? b->scale : i == REG_IBASE ? b->ibase : b->obase);
            goto replace_top;

        case OP_ADD: r = num_add(TOP(1), TOP(0)); goto replace_two;
        case OP_SUB: r = num_sub(TOP(1), TOP(0)); goto replace_two;
        case OP_MUL: r = num_mul(TOP(1), TOP(0), b->scale); goto replace_two;
        case OP_DIV: r = num_div(TOP(1), TOP(0), b->scale); goto replace_two;
        case OP_MOD: r = num_mod(TOP(1), TOP(0), b->scale); goto replace_two;
        case OP_POW: r = num_pow(TOP(1), TOP(0), b->scale); goto replace_two;

        replace_two:
            if(!r) goto num_trouble;
            num_put(TOP(0));
            num_put(TOP(1));
            b->sp--;
            TOP(0) = r;
            break;

        case OP_NEG: r = num_neg(TOP(0)); goto replace_top;
        case OP_LENGTH: r = num_uint(num_length(TOP(0))); goto replace_top;
        case OP_SCALEOF: r = num_uint(TOP(0)->scale); goto replace_top;
        case OP_SQRT: r = num_sqrt(TOP(0), b->scale); goto replace_top;

        replace_top:
            if(!r) goto num_trouble;
            num_put(TOP(0));
            TOP(0) = r;
            break;

        case OP_NOT:
            truth = !TOP(0)->len;
            r = num_ref(truth ? b->one : b->zero);
            num_put(TOP(0));
            TOP(0) = r;
            break;

        case OP_EQ: truth = num_cmp(TOP(1), TOP(0)) == 0; goto compared;
        case OP_NE: truth = num_cmp(TOP(1), TOP(0)) != 0; goto compared;
        case OP_LT: truth = num_cmp(TOP(1), TOP(0)) < 0; goto compared;
        case OP_LE: truth = num_cmp(TOP(1), TOP(0)) <= 0; goto compared;
        case OP_GT: truth = num_cmp(TOP(1), TOP(0)) > 0; goto compared;
        case OP_GE: truth = num_cmp(TOP(1), TOP(0)) >= 0; goto compared;

        compared:
            r = num_ref(truth ? b->one : b->zero);
            goto replace_two;

        case OP_JMP:
            if(b->budget && !--b->budget) goto over_budget;
            pc = code[pc];
            break;

        case OP_JZ:
        case OP_JNZ:
            truth = TOP(0)->len != 0;
            num_put(TOP(0));
            b->sp--;
            if(b->budget && !--b->budget) goto over_budget;
            pc = truth == (code[pc - 1] == OP_JNZ) ? code[pc] : pc + 1;
            break;

        case OP_POP:
            num_put(TOP(0));
            b->sp--;
            break;

        case OP_DUP:
            if(push(b, num_ref(TOP(0)))) goto no_memory;
            break;

        case OP_TUCK:                                           /* [a b] to [b a b] */
            if(push(b, num_ref(TOP(0)))) goto no_memory;
            r = TOP(1);
            TOP(1) = TOP(2);
            TOP(2) = r;
            break;

        case OP_PRINT:
            b->held = 0;
            if(num_print(TOP(0), b->obase, put_number_char, b) < 0) goto num_trouble;
            put_out(b, b->held);
            put_out(b, '\n');
            num_put(TOP(0));
            b->sp--;
            break;

        case OP_STR:
            put_text(b, f->text[code[pc++]]);
            break;

        case OP_CALL: {
            unsigned id = (unsigned)code[pc], n = (unsigned)code[pc + 1];
            const int *args = code + pc + 2;
            struct func *g = b->sym[id].fn;
            char what[96];
            if(!g) {
                snprintf(what, sizeof(what), "function %.32s is not defined", b->sym[id].name);
                say_in(b, f, what);
                goto fail;
            }
            if(n != g->nparams) {
                snprintf(what, sizeof(what), "%.32s takes %u arguments, not %u", b->sym[id].name, g->nparams, n);
                say_in(b, f, what);
                goto fail;
            }
            for(i = 0; i < n; i++) {
                if(!args[i] == !g->loc[i].array) continue;
                snprintf(what, sizeof(what), "%.32s: argument %u should %s", b->sym[id].name, i + 1,
                         g->loc[i].array ? "be an array" : "not be an array");
                say_in(b, f, what);
                goto fail;
            }
            if(b->budget && !--b->budget) goto over_budget;
            if(enter(b, g, args, n)) goto no_memory;
            b->frames[b->nframes].caller = f;
            b->frames[b->nframes].pc = pc + 2 + n;
            b->frames[b->nframes++].scale = b->scale;
            f = g;
            code = g->code;
            pc = 0;
            break;
        }

        case OP_RET0:
            if(push(b, num_ref(b->zero))) goto no_memory;
            /* fall through */

        case OP_RET: {
            struct frame *fr = &b->frames[--b->nframes];
            struct func *g = f;
            leave(b, g);
            f = fr->caller;
            code = f->code;
            pc = fr->pc;
            if(g->lib) {
                /* a function of the library works at a scale of its own and gives the result at its caller's */
                b->scale = fr->scale;
                r = num_rescale(TOP(0), b->scale);
                goto replace_top;
            }
            break;
        }

        case OP_FAIL:
            say_in(b, f, f->text[code[pc]]);
            goto fail;
        }
    }

over_budget:
    b->budget = 1;                                              /* and it stays spent, for the next line too */
    say_in(b, f, "out of steps");
    goto fail;
no_memory:
    num_err = NUM_NOMEM;
num_trouble:
    say_in(b, f, num_strerror(num_err));
fail:
    unwind(b, f);
    return -1;
}

/**** starting and ending ****/

struct bc *bc_new(const struct bc_host *host) {
    struct bc *b = calloc(1, sizeof(*b));
    if(!b) return NULL;
    b->host = *host;
    b->ibase = b->obase = 10;
    b->zero = num_uint(0);
    b->one = num_uint(1);
    if(!b->zero || !b->one) {
        bc_free(b);
        return NULL;
    }
    return b;
}

void bc_free(struct bc *b) {
    unsigned i;
    if(!b) return;
    for(i = 0; i < b->nsym; i++) {
        num_put(b->sym[i].var);
        array_free(b->sym[i].arr);
        if(b->sym[i].fn) bc_func_free(b->sym[i].fn);
        free(b->sym[i].name);
    }
    bc_func_clear(&b->main);
    free(b->main.code);
    free(b->main.k);
    free(b->main.text);
    free(b->main.loc);
    free(b->sym);
    free(b->stack);
    free(b->frames);
    free(b->saved);
    num_put(b->zero);
    num_put(b->one);
    free(b);
}

void bc_set_budget(struct bc *b, unsigned long steps) {
    b->budget = steps;
}
