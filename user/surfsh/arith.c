/*
SurfOS shell: surfsh
--------------------
File: arith.c   Date: 10/24/26
--------------------
$(( ... )): C's integer arithmetic on 32-bit numbers, as POSIX's shells have it. expand.c
expands the text first (parameters, $( ), the quotes go); then: numbers (decimal, 0x... hex,
0... octal), names of variables (their value, read as a number; unset or empty is 0), ( ),
the unary + - ! ~, * / %, + -, << >>, < <= > >=, == !=, &, ^, |, &&, ||, ?:, and the
assignments = *= /= %= += -= <<= >>= &= ^= |= to a name, with ++ and -- before or after a
name. The side of && || ?: that is not taken is read but not worked out (no assignment,
no error for a division by 0 there). Numbers wrap around as 32-bit ones do; dividing by 0
is an error, and so is the smallest number divided by -1 (the processor would stop).
*/

#include "surfsh.h"

struct ar {
    const char *s;                  /* what is left to read */
    int skip;                       /* the side not taken: nothing is changed or refused there */
    int depth;
    char *err;
    int errsize;
    int failed;
};

static long ar_expr(struct ar *a);

static long fail(struct ar *a, const char *what, const char *at) {
    if(!a->failed) {
        int n = 0;
        while(at && at[n] && n < 20) n++;
        if(at && *at) snprintf(a->err, (size_t)a->errsize, "%s at \"%.*s\"", what, n, at);
        else snprintf(a->err, (size_t)a->errsize, "%s", what);
        a->failed = 1;
    }
    return 0;
}

static void blanks(struct ar *a) {
    while(*a->s == ' ' || *a->s == '\t' || *a->s == '\n') a->s++;
}

/* the operator op is next (and not the start of a longer one in notlonger); taken */
static int next_is(struct ar *a, const char *op, const char *notlonger) {
    size_t n = strlen(op);
    blanks(a);
    if(strncmp(a->s, op, n)) return 0;
    if(notlonger && a->s[n] && strchr(notlonger, a->s[n])) return 0;
    a->s += n;
    return 1;
}

static int name_len(const char *s) {
    int n = 0;
    if(!(isalpha((unsigned char)*s) || *s == '_')) return 0;
    while(isalnum((unsigned char)s[n]) || s[n] == '_') n++;
    return n;
}

/* a number written in the text or held by a variable: 0, -1 when it is none */
static int number(const char *s, long *v, const char **end) {
    unsigned long x = 0;
    int base = 10, any = 0, neg = 0;
    while(*s == ' ' || *s == '\t') s++;
    if(*s == '-' || *s == '+') neg = *s++ == '-';
    if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if(s[0] == '0' && s[1]) {
        base = 8;
    }
    for(;; s++) {
        int d;
        if(isdigit((unsigned char)*s)) d = *s - '0';
        else if(isalpha((unsigned char)*s)) d = tolower((unsigned char)*s) - 'a' + 10;
        else break;
        if(d >= base) return -1;
        x = x * (unsigned long)base + (unsigned long)d;
        any = 1;
    }
    if(!any) return -1;
    *v = neg ? (long)(0 - x) : (long)x;
    if(end) *end = s;
    return 0;
}

static long value_of(struct ar *a, const char *name) {
    const char *v = var_get(name), *end;
    long x;
    if(!v || !*v || a->skip) return 0;
    if(number(v, &x, &end) < 0 || *end) {
        char why[WORD_LIMIT + 40];
        snprintf(why, sizeof(why), "%s is not a number (%.40s)", name, v);
        return fail(a, why, NULL);
    }
    return x;
}

static void store(struct ar *a, const char *name, long x) {
    char buf[WORD_LIMIT + 20];
    if(a->skip || a->failed) return;
    if(var_flags(name) & V_READONLY) {
        snprintf(buf, sizeof(buf), "%s is read-only", name);
        fail(a, buf, NULL);
        return;
    }
    snprintf(buf, sizeof(buf), "%ld", x);
    var_set(name, buf, 0);
}

/* the operator of an assignment, if one comes next (not ==): its operator character, '=' for =, else 0 */
static int assign_op(struct ar *a, char op[4]) {
    static const char *const ops[] = { "<<=", ">>=", "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "=" };
    unsigned i;
    blanks(a);
    for(i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        size_t n = strlen(ops[i]);
        if(!strncmp(a->s, ops[i], n) && !(n == 1 && a->s[1] == '=')) {
            strcpy(op, ops[i]);
            a->s += n;
            return 1;
        }
    }
    return 0;
}

static long apply(struct ar *a, long x, const char *op, long y, const char *at) {
    unsigned long ux = (unsigned long)x, uy = (unsigned long)y;
    if(!strcmp(op, "*")) return (long)(ux * uy);
    if(!strcmp(op, "+")) return (long)(ux + uy);
    if(!strcmp(op, "-")) return (long)(ux - uy);
    if(!strcmp(op, "/") || !strcmp(op, "%")) {
        if(y == 0) return a->skip ? 0 : fail(a, "division by 0", at);
        if(x == LONG_MIN && y == -1) return a->skip ? 0 : (op[0] == '%' ? 0 : fail(a, "the result does not fit", at));
        return op[0] == '/' ? x / y : x % y;
    }
    if(!strcmp(op, "<<")) return (long)(ux << (uy & 31));
    if(!strcmp(op, ">>")) return x >> (uy & 31);
    if(!strcmp(op, "&")) return x & y;
    if(!strcmp(op, "^")) return x ^ y;
    if(!strcmp(op, "|")) return x | y;
    return 0;
}

static long primary(struct ar *a) {
    const char *at;
    char name[WORD_LIMIT + 1];
    long x;
    int n;
    blanks(a);
    at = a->s;
    if(++a->depth > 64) return fail(a, "too deep", at);
    if(next_is(a, "(", NULL)) {
        x = ar_expr(a);
        if(!next_is(a, ")", NULL)) x = fail(a, "a ( is not closed", a->s);
    } else if(next_is(a, "++", NULL) || next_is(a, "--", NULL)) {    /* ++name, --name */
        int inc = at[0] == '+';
        blanks(a);
        n = name_len(a->s);
        if(!n || n > WORD_LIMIT) x = fail(a, inc ? "++ needs a name" : "-- needs a name", a->s);
        else {
            memcpy(name, a->s, (size_t)n);
            name[n] = 0;
            a->s += n;
            x = value_of(a, name) + (inc ? 1 : -1);
            store(a, name, x);
        }
    } else if(next_is(a, "+", NULL)) {
        x = primary(a);
    } else if(next_is(a, "-", NULL)) {
        x = (long)(0 - (unsigned long)primary(a));
    } else if(next_is(a, "!", "=")) {
        x = !primary(a);
    } else if(next_is(a, "~", NULL)) {
        x = ~primary(a);
    } else if(isdigit((unsigned char)*a->s)) {
        const char *end;
        if(number(a->s, &x, &end) < 0) x = fail(a, "not a number", at);
        else if(isalnum((unsigned char)*end) || *end == '_') x = fail(a, "not a number", at);
        else a->s = end;
    } else if((n = name_len(a->s)) > 0) {
        char op[4];
        if(n > WORD_LIMIT) {
            a->depth--;
            return fail(a, "a name too long", at);
        }
        memcpy(name, a->s, (size_t)n);
        name[n] = 0;
        a->s += n;
        if(next_is(a, "++", NULL) || next_is(a, "--", NULL)) {        /* name++, name-- */
            x = value_of(a, name);
            store(a, name, x + (a->s[-1] == '+' ? 1 : -1));
        } else if(assign_op(a, op)) {
            long y = ar_expr(a);                                        /* to the right: a = b = 1 */
            if(op[0] == '=') x = y;
            else {
                op[strlen(op) - 1] = 0;
                x = apply(a, value_of(a, name), op, y, at);
            }
            store(a, name, x);
        } else {
            x = value_of(a, name);
        }
    } else {
        x = fail(a, *a->s ? "not understood" : "a number is missing", at);
    }
    a->depth--;
    return x;
}

/* the binary operators, by level: each level's operators, and what must not follow them */
static const struct level {
    const char *ops[4];
    const char *notlonger[4];
} levels[] = {
    { { "*", "/", "%", NULL }, { "=", "=", "=", NULL } },
    { { "+", "-", NULL }, { "=+", "=-", NULL } },
    { { "<<", ">>", NULL }, { "=", "=", NULL } },
    { { "<=", ">=", "<", ">" }, { NULL, NULL, "<=", ">=" } },
    { { "==", "!=", NULL }, { NULL, NULL, NULL } },
    { { "&", NULL }, { "&=", NULL } },
    { { "^", NULL }, { "=", NULL } },
    { { "|", NULL }, { "|=", NULL } },
};

static long binary(struct ar *a, int level) {
    long x, y;
    if(level < 0) return primary(a);
    x = binary(a, level - 1);
    for(;;) {
        const struct level *l = &levels[level];
        const char *at;
        int i;
        blanks(a);
        at = a->s;
        for(i = 0; i < 4 && l->ops[i]; i++) if(next_is(a, l->ops[i], l->notlonger[i])) break;
        if(i == 4 || !l->ops[i] || a->failed) return x;
        y = binary(a, level - 1);
        if(level == 3) x = !strcmp(l->ops[i], "<=") ? x <= y : !strcmp(l->ops[i], ">=") ? x >= y :
                           !strcmp(l->ops[i], "<") ? x < y : x > y;
        else if(level == 4) x = l->ops[i][0] == '=' ? x == y : x != y;
        else x = apply(a, x, l->ops[i], y, at);
    }
}

static long logical_and(struct ar *a) {
    long x = binary(a, 7);
    while(next_is(a, "&&", NULL)) {
        int skip = a->skip;
        if(!x) a->skip = 1;
        x = binary(a, 7) && x;
        a->skip = skip;
    }
    return x;
}

static long logical_or(struct ar *a) {
    long x = logical_and(a);
    while(next_is(a, "||", NULL)) {
        int skip = a->skip;
        if(x) a->skip = 1;
        x = logical_and(a) || x;
        a->skip = skip;
    }
    return x;
}

/* the whole expression: ?: (to the right), down to the numbers */
static long ar_expr(struct ar *a) {
    long c, x, y;
    int skip = a->skip;
    if(++a->depth > 64) return fail(a, "too deep", a->s);
    c = logical_or(a);
    if(!next_is(a, "?", NULL)) {
        a->depth--;
        return c;
    }
    if(!c) a->skip = 1;
    x = ar_expr(a);
    a->skip = skip;
    if(!next_is(a, ":", NULL)) return fail(a, "a ? without its :", a->s);
    if(c) a->skip = 1;
    y = ar_expr(a);
    a->skip = skip;
    a->depth--;
    return c ? x : y;
}

int arith(const char *text, long *result, char *err, int errsize) {
    struct ar a;
    memset(&a, 0, sizeof(a));
    a.s = text;
    a.err = err;
    a.errsize = errsize;
    blanks(&a);
    if(!*a.s) {                                     /* $(( )) is 0 */
        *result = 0;
        return 0;
    }
    *result = ar_expr(&a);
    blanks(&a);
    if(!a.failed && *a.s) fail(&a, "not understood", a.s);
    return a.failed ? -1 : 0;
}
