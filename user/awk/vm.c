/*
SurfOS user program: awk
--------------------
File: vm.c      Date: 11/01/26
--------------------
The machine that runs what parse.c writes: a stack of values, the frames of the calls on the heap,
and one loop, which does not recurse, so a function may call itself as deep as memory goes.

A function's locals are its slots on the stack, from A.fp. A parameter is untyped until it is used:
an array goes by reference, a scalar by value, and a name that is neither yet goes as a V_REF to
where it lives, so that if the function makes an array of it the caller has that array. Read as
a scalar, such a parameter is unset, whatever has become of the name since: that is by value.

An lvalue is a kind and a number, with what the kind needs on the stack below (place_*), so that
assignment, ++, getline and sub share one way of reading and writing.
*/

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include "awk.h"

#define TOP(k) (A.stack[A.sp - 1 - (unsigned)(k)])

struct awk A;

void *xalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if(!p) fatal("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if(!q) fatal("out of memory");
    return q;
}

unsigned line_now(unsigned *source) {
    unsigned lo = 0, hi = A.nmarks;
    if(!hi) return 0;
    while(hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if(A.marks[mid].pc <= A.pc) lo = mid;
        else hi = mid;
    }
    *source = A.marks[lo].source;
    return A.marks[lo].line;
}

static void say(const char *fmt, va_list ap) {
    unsigned source = 0, line = line_now(&source);
    fflush(stdout);
    fputs("awk: ", stderr);
    if(A.running && line) {
        if(A.sources && A.sources[source]) fprintf(stderr, "%s: ", A.sources[source]);
        fprintf(stderr, "line %u: ", line);
    }
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void say_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    say(fmt, ap);
    va_end(ap);
}

/* what a program's end does: what was sent to files arrives, and a command is waited for */
void stop(void) {
    streams_end();
    exit(2);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    say(fmt, ap);
    va_end(ap);
    stop();
}

static void grow_stack(void) {
    uint64_t cap = A.capstack ? (uint64_t)A.capstack * 2 : 256;
    if(cap * sizeof(*A.stack) >= 0x7ff00000u) fatal("out of memory");
    A.stack = xrealloc(A.stack, (size_t)cap * sizeof(*A.stack));
    A.capstack = (unsigned)cap;
}

static inline struct value *slot(void) {
    if(A.sp == A.capstack) grow_stack();
    return &A.stack[A.sp++];
}

static void push_num(double d) {
    struct value *v = slot();
    v->kind = V_NUM;
    v->u.num = d;
}

static void push_str(struct str *s) {
    struct value *v = slot();
    v->kind = V_STR;
    v->u.s = s;
}

static void pop(void) {
    val_put(&TOP(0));
    A.sp--;
}

/* the array a name holds, made if it holds nothing yet */
static struct array *array_of(struct value *v) {
    while(v->kind == V_REF) v = v->u.ref >= 0 ? &A.stack[v->u.ref] : &A.global[~v->u.ref];
    if(v->kind == V_UNSET) {
        v->u.a = array_new();
        v->kind = V_ARRAY;
    }
    if(v->kind != V_ARRAY) fatal("a scalar used as an array");
    return v->u.a;
}

static void check_scalar(const struct value *v) {
    while(v->kind == V_REF) v = v->u.ref >= 0 ? &A.stack[v->u.ref] : &A.global[~v->u.ref];
    if(v->kind == V_ARRAY) fatal("an array used as a scalar");
}

/**** places ****/

/* what the place holds: top is the last of its prefix on the stack */
static void place_load(int kind, int n, unsigned top, struct value *to) {
    switch(kind) {
    case PL_VAR:
        check_scalar(&A.global[n]);
        val_copy(to, &A.global[n]);
        break;
    case PL_LOCAL:
        check_scalar(&A.stack[A.fp + (unsigned)n]);
        if(A.stack[A.fp + (unsigned)n].kind == V_REF) to->kind = V_UNSET;
        else val_copy(to, &A.stack[A.fp + (unsigned)n]);
        break;
    case PL_NF:
        to->kind = V_NUM;
        to->u.num = nf_get();
        break;
    case PL_ARRAY: {
        struct str *key = to_str(&A.stack[top]);
        val_copy(to, array_elem(A.stack[top - 1].u.a, key, 1));
        str_put(key);
        break;
    }
    case PL_FIELD:
        field_load(to_num(&A.stack[top]), to);
        break;
    case PL_RECORD:
        to->kind = V_INPUT;
        to->u.s = str_ref(record_get());
        break;
    }
}

static void place_store(int kind, int n, unsigned top, const struct value *v) {
    struct value t, *to;
    switch(kind) {
    case PL_VAR:
        check_scalar(&A.global[n]);
        global_set(n, v);
        break;
    case PL_LOCAL:
        to = &A.stack[A.fp + (unsigned)n];
        check_scalar(to);
        val_copy(&t, v);
        val_put(to);
        *to = t;
        break;
    case PL_NF:
        nf_set(to_num(v));
        break;
    case PL_ARRAY: {
        struct str *key = to_str(&A.stack[top]);
        to = array_elem(A.stack[top - 1].u.a, key, 1);
        str_put(key);
        val_copy(&t, v);
        val_put(to);
        *to = t;
        break;
    }
    case PL_FIELD:
        field_store(to_num(&A.stack[top]), v);
        break;
    case PL_RECORD:
        record_set(to_str(v));
        break;
    }
}

static unsigned prefix_of(int kind) {
    return kind == PL_ARRAY ? 2 : kind == PL_FIELD ? 1 : 0;
}

/* the prefix goes from under the value on top */
static void drop_prefix(unsigned n) {
    struct value v = TOP(0);
    A.sp--;
    while(n--) pop();
    A.stack[A.sp++] = v;
}

/**** arithmetic ****/

static double power(double x, double y) {
    if(y == floor(y) && fabs(y) < 1024) {       /* exact where a double can be, and pow() is slow */
        double r = 1, b = x;
        int n = (int)fabs(y);
        while(n) {
            if(n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        return y < 0 ? 1 / r : r;
    }
    return pow(x, y);
}

static double arithmetic(int op, double a, double b) {
    switch(op) {
    case OP_ADD: return a + b;
    case OP_SUB: return a - b;
    case OP_MUL: return a * b;
    case OP_DIV:
        if(b == 0) fatal("division by zero");
        return a / b;
    case OP_MOD:
        if(b == 0) fatal("division by zero in %%");
        return fmod(a, b);
    }
    return power(a, b);
}

static void concat(int n, const struct str *between) {
    struct str **parts = xalloc((size_t)n * sizeof(*parts)), *r;
    uint64_t total = 0;
    unsigned at = 0;
    int i;
    for(i = 0; i < n; i++) {
        parts[i] = to_str(&TOP(n - 1 - i));
        total += parts[i]->len;
    }
    if(between) total += (uint64_t)between->len * (unsigned)(n - 1);
    if(total >= 0x7ff00000u) fatal("out of memory");
    r = str_alloc((unsigned)total);
    for(i = 0; i < n; i++) {
        if(between && i) {
            memcpy(r->s + at, between->s, between->len);
            at += between->len;
        }
        memcpy(r->s + at, parts[i]->s, parts[i]->len);
        at += parts[i]->len;
        str_put(parts[i]);
    }
    free(parts);
    for(i = 0; i < n; i++) pop();
    push_str(r);
}

/**** output ****/

static void put(FILE *f, const char *s, unsigned n) {
    if(n && fwrite(s, 1, n, f) != n) fatal("write error: %s", strerror(errno));
}

static FILE *destination(int how) {
    FILE *f = stdout;
    if(how != TO_STDOUT) {
        struct str *name = to_str(&TOP(0));
        f = output_to(how, name);
        str_put(name);
        pop();
    }
    return f;
}

static void do_print(int n, int how) {
    FILE *f = destination(how);
    struct str *ors = to_str(&A.global[S_ORS]);
    int i;
    if(!n) {
        struct str *rec = record_get();
        put(f, rec->s, rec->len);
    } else {
        struct str *ofs = n > 1 ? to_str(&A.global[S_OFS]) : NULL;
        for(i = 0; i < n; i++) {
            struct str *s = to_output(&TOP(n - 1 - i));
            if(i) put(f, ofs->s, ofs->len);
            put(f, s->s, s->len);
            str_put(s);
        }
        str_put(ofs);
    }
    put(f, ors->s, ors->len);
    str_put(ors);
    if(f == stderr) fflush(f);
    for(i = 0; i < n; i++) pop();
}

static void do_printf(int n, int how) {
    FILE *f = destination(how);
    static struct buf out;
    struct str *fmt = to_str(&TOP(n - 1));
    int i;
    out.len = 0;
    format_text(&out, fmt->s, fmt->len, &A.stack[A.sp - (unsigned)n + 1], n - 1);
    str_put(fmt);
    put(f, out.p, out.len);
    if(f == stderr) fflush(f);
    for(i = 0; i < n; i++) pop();
}

/**** getline ****/

static void bump(int id) {
    double d = to_num(&A.global[id]) + 1;
    val_put(&A.global[id]);
    A.global[id].kind = V_NUM;
    A.global[id].u.num = d;
}

static void do_getline(int source, unsigned prefix) {
    struct str *rec = NULL, *name;
    struct value *v;
    int rc;
    if(source == GL_MAIN) {
        rc = main_record(&rec);
        if(rc > 0) {
            bump(S_NR);
            bump(S_FNR);
        }
    } else {
        unsigned at = source == GL_CMD ? prefix : 0;
        unsigned i;
        name = to_str(&TOP(at));
        rc = getline_from(source == GL_CMD, name, &rec);
        str_put(name);
        val_put(&TOP(at));
        for(i = at; i > 0; i--) TOP(i) = TOP(i - 1);    /* the name goes from under the prefix */
        A.sp--;
    }
    push_num(rc);
    v = slot();
    if(rc > 0) {
        v->kind = V_INPUT;
        v->u.s = rec;
    } else {
        v->kind = V_UNSET;
    }
}

/**** the loop ****/

static int run_loop(unsigned pc) {
    const int *code = A.code;
    struct value t;
    double a, b;
    int c;
    for(;;) {
        A.pc = pc;
        switch(code[pc++]) {
        case OP_HALT:
            return R_DONE;

        case OP_NUM:
            push_num(A.knum[code[pc++]]);
            break;

        case OP_ONE:
            push_num(1);
            break;

        case OP_STR:
            push_str(str_ref(A.kstr[code[pc++]]));
            break;

        case OP_RE: {
            struct str *rec = record_get();
            regoff_t so, eo;
            push_num(regex_find(A.kre[code[pc++]], rec->s, 0, rec->len, &so, &eo));
            break;
        }

        case OP_RELIT: {
            struct value *v = slot();
            v->kind = V_REGEX;
            v->u.re = A.kre[code[pc++]];
            break;
        }

        case OP_LOADV:
            place_load(PL_VAR, code[pc++], 0, &t);
            *slot() = t;
            break;

        case OP_LOADL:
            place_load(PL_LOCAL, code[pc++], 0, &t);
            *slot() = t;
            break;

        case OP_LOADNF:
            push_num(nf_get());
            break;

        case OP_ARGV: {
            int n = code[pc++];
            if(A.global[n].kind == V_UNSET) {
                t.kind = V_REF;
                t.u.ref = ~n;
            } else {
                val_copy(&t, &A.global[n]);
            }
            *slot() = t;
            break;
        }

        case OP_ARGL: {
            unsigned at = A.fp + (unsigned)code[pc++];
            const struct value *v = &A.stack[at];
            if(v->kind == V_UNSET) {
                t.kind = V_REF;
                t.u.ref = (int)at;
            } else if(v->kind == V_REF) {
                const struct value *w = v;
                while(w->kind == V_REF) w = w->u.ref >= 0 ? &A.stack[w->u.ref] : &A.global[~w->u.ref];
                if(w->kind == V_ARRAY) val_copy(&t, w);
                else t = *v;
            } else {
                val_copy(&t, v);
            }
            *slot() = t;
            break;
        }

        case OP_ARRV:
        case OP_ARRL: {
            struct array *arr = code[pc - 1] == OP_ARRV ? array_of(&A.global[code[pc]]) : array_of(&A.stack[A.fp + (unsigned)code[pc]]);
            struct value *v;
            pc++;
            array_ref(arr);
            v = slot();
            v->kind = V_ARRAY;
            v->u.a = arr;
            break;
        }

        case OP_AGET:
            place_load(PL_ARRAY, 0, A.sp - 1, &t);
            pop();
            pop();
            *slot() = t;
            break;

        case OP_FIELD:
            place_load(PL_FIELD, 0, A.sp - 1, &t);
            pop();
            *slot() = t;
            break;

        case OP_AIN: {
            struct str *key = to_str(&TOP(1));
            int in = array_elem(TOP(0).u.a, key, 0) != NULL;
            str_put(key);
            pop();
            pop();
            push_num(in);
            break;
        }

        case OP_ADEL: {
            struct str *key = to_str(&TOP(0));
            array_del(TOP(1).u.a, key);
            str_put(key);
            pop();
            pop();
            break;
        }

        case OP_ACLEAR:
            array_clear(TOP(0).u.a);
            pop();
            break;

        case OP_STORE: {
            int kind = code[pc], n = code[pc + 1];
            pc += 2;
            place_store(kind, n, A.sp - 2, &TOP(0));
            drop_prefix(prefix_of(kind));
            break;
        }

        case OP_UPDATE: {
            int kind = code[pc], n = code[pc + 1], op = code[pc + 2];
            unsigned top = A.sp - 2;
            pc += 3;
            b = to_num(&TOP(0));
            place_load(kind, n, top, &t);
            a = to_num(&t);
            val_put(&t);
            t.kind = V_NUM;
            t.u.num = arithmetic(op, a, b);
            place_store(kind, n, top, &t);
            pop();
            for(c = (int)prefix_of(kind); c > 0; c--) pop();
            *slot() = t;
            break;
        }

        case OP_STEP: {
            int kind = code[pc], n = code[pc + 1], flags = code[pc + 2];
            unsigned top = A.sp - 1;
            pc += 3;
            place_load(kind, n, top, &t);
            a = to_num(&t);
            val_put(&t);
            t.kind = V_NUM;
            t.u.num = flags & STEP_DOWN ? a - 1 : a + 1;
            place_store(kind, n, top, &t);
            if(!(flags & STEP_POST)) a = t.u.num;
            for(c = (int)prefix_of(kind); c > 0; c--) pop();
            push_num(a);
            break;
        }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW:
            a = arithmetic(code[pc - 1], to_num(&TOP(1)), to_num(&TOP(0)));
            goto number;

        number:
            pop();
            pop();
            push_num(a);
            break;

        case OP_NEG: a = -to_num(&TOP(0)); goto number1;
        case OP_PLUS: a = to_num(&TOP(0)); goto number1;
        case OP_NOT: a = !to_bool(&TOP(0)); goto number1;

        number1:
            pop();
            push_num(a);
            break;

        case OP_LT: c = val_cmp(&TOP(1), &TOP(0)); a = c == -1; goto number;
        case OP_LE: c = val_cmp(&TOP(1), &TOP(0)); a = c == -1 || c == 0; goto number;
        case OP_EQ: c = val_cmp(&TOP(1), &TOP(0)); a = c == 0; goto number;
        case OP_NE: c = val_cmp(&TOP(1), &TOP(0)); a = c != 0; goto number;
        case OP_GT: c = val_cmp(&TOP(1), &TOP(0)); a = c == 1; goto number;
        case OP_GE: c = val_cmp(&TOP(1), &TOP(0)); a = c == 1 || c == 0; goto number;

        case OP_CONCAT:
            concat(code[pc++], NULL);
            break;

        case OP_JOIN: {
            struct str *sep = to_str(&A.global[S_SUBSEP]);
            concat(code[pc++], sep);
            str_put(sep);
            break;
        }

        case OP_MATCH: {
            const struct regex *re = regex_of(&TOP(0));
            struct str *s = to_str(&TOP(1));
            regoff_t so, eo;
            a = regex_find(re, s->s, 0, s->len, &so, &eo);
            str_put(s);
            goto number;
        }

        case OP_JMP:
            pc = (unsigned)code[pc];
            break;

        case OP_JZ:
        case OP_JNZ:
            c = to_bool(&TOP(0));
            pop();
            pc = c == (code[pc - 1] == OP_JNZ) ? (unsigned)code[pc] : pc + 1;
            break;

        case OP_POP:
            pop();
            break;

        case OP_PRINT:
            do_print(code[pc], code[pc + 1]);
            pc += 2;
            break;

        case OP_PRINTF:
            do_printf(code[pc], code[pc + 1]);
            pc += 2;
            break;

        case OP_CALL: {
            const struct func *g = &A.fn[code[pc]];
            unsigned n = (unsigned)code[pc + 1];
            struct frame *fr;
            pc += 2;
            for(; n < g->nparams; n++) slot()->kind = V_UNSET;
            if(A.nframes == A.capframes) {
                uint64_t cap = A.capframes ? (uint64_t)A.capframes * 2 : 64;
                if(cap * sizeof(*A.frames) >= 0x7ff00000u) fatal("out of memory");
                A.frames = xrealloc(A.frames, (size_t)cap * sizeof(*A.frames));
                A.capframes = (unsigned)cap;
            }
            fr = &A.frames[A.nframes++];
            fr->ret = pc;
            fr->fp = A.fp;
            A.fp = A.sp - g->nparams;
            pc = g->entry;
            break;
        }

        case OP_RET0:
            slot()->kind = V_UNSET;
            /* fall through */

        case OP_RET: {
            struct frame *fr = &A.frames[--A.nframes];
            t = TOP(0);
            A.sp--;
            while(A.sp > A.fp) pop();
            *slot() = t;
            A.fp = fr->fp;
            pc = fr->ret;
            break;
        }

        case OP_BUILTIN: {
            int which = code[pc], n = code[pc + 1];
            pc += 2;
            if(A.sp == A.capstack) grow_stack();
            builtin_call(which, n);
            break;
        }

        case OP_SUBST: {
            int kind = code[pc], n = code[pc + 1], global = code[pc + 2], count;
            unsigned size = prefix_of(kind), top = A.sp - 1;
            struct str *text, *repl = to_str(&TOP(size)), *result = NULL;
            pc += 3;
            place_load(kind, n, top, &t);
            text = to_str(&t);
            val_put(&t);
            count = substitute(&TOP(size + 1), repl, text, global, &result);
            str_put(text);
            str_put(repl);
            if(count) {
                t.kind = V_STR;
                t.u.s = result;
                place_store(kind, n, top, &t);
                val_put(&t);
            }
            for(c = (int)size + 2; c > 0; c--) pop();
            push_num(count);
            break;
        }

        case OP_GETLINE: {
            int source = code[pc], prefix = code[pc + 1];
            pc += 2;
            do_getline(source, (unsigned)prefix);
            break;
        }

        case OP_GL: {
            int kind = code[pc], n = code[pc + 1];
            unsigned size = prefix_of(kind);
            pc += 2;
            a = to_num(&TOP(1));
            if(a > 0) {
                if(kind == PL_RECORD) {
                    record_set(str_ref(TOP(0).u.s));
                } else {
                    place_store(kind, n, A.sp - 3, &TOP(0));
                }
            }
            pop();
            pop();
            for(c = (int)size; c > 0; c--) pop();
            push_num(a);
            break;
        }

        case OP_NEXT:
        case OP_NEXTFILE:
            if(A.in_begin_end) fatal("next or nextfile in BEGIN or END");
            c = code[pc - 1] == OP_NEXT ? R_NEXT : R_NEXTFILE;
            goto leave;

        case OP_EXIT:
            a = fmod(to_num(&TOP(0)), 256);
            A.exit_status = isnan(a) ? 0 : (int)a;
            pop();
            /* fall through */

        case OP_EXIT0:
            c = R_EXIT;
            goto leave;

        leave:
            while(A.sp) pop();
            A.nframes = 0;
            A.fp = 0;
            return c;

        case OP_ITER: {
            struct iter *it = iter_new(TOP(0).u.a);
            pop();
            t.kind = V_ITER;
            t.u.it = it;
            *slot() = t;
            break;
        }

        case OP_ITERNEXT: {
            struct str *key = iter_next(TOP(0).u.it);
            int kind = code[pc], n = code[pc + 1];
            if(!key) {
                pc = (unsigned)code[pc + 2];
                break;
            }
            pc += 3;
            t.kind = V_STR;
            t.u.s = key;
            place_store(kind, n, 0, &t);
            break;
        }

        case OP_RANGE:
            pc = code[pc] >= 0 && A.range_open[code[pc]] ? (unsigned)code[pc + 1] : pc + 2;
            break;

        case OP_RANGESET:
            A.range_open[code[pc]] = (unsigned char)code[pc + 1];
            pc += 2;
            break;
        }
    }
}

/* A message says the program's line only while the program runs: a file that cannot be read is no line's. */
int run_code(unsigned pc) {
    int rc;
    A.running = 1;
    rc = run_loop(pc);
    A.running = 0;
    return rc;
}

void run_program(void) {
    struct str *rec;
    int rc;
    A.in_begin_end = 1;
    rc = run_code(A.begin_pc);
    A.in_begin_end = 0;
    if(rc != R_EXIT && (A.have_main || A.have_end)) {
        while(main_record(&rec)) {
            record_set(rec);
            bump(S_NR);
            bump(S_FNR);
            rc = run_code(A.main_pc);
            if(rc == R_EXIT) break;
            if(rc == R_NEXTFILE) skip_file();
        }
    }
    A.in_begin_end = 1;
    run_code(A.end_pc);
}
