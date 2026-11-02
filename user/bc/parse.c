/*
SurfOS user program: bc
--------------------
File: parse.c   Date: 11/01/26
--------------------
Reads bc's language a character at a time and writes code for vm.c as it goes, in one pass.

The language is POSIX bc's, with what every other bc takes and a script from elsewhere is likely
to use: names longer than a letter, else, relations anywhere an expression may stand, && || and
!, # comments, continue, an empty part in for, return without brackets. The precedence is GNU's,
chosen there so that POSIX programs keep their meaning; the odd results are POSIX's: -2^2 is 4,
2*x=3 assigns first, and x=1<2 compares after assigning. An assignment is therefore taken up
where its name is met, in primary(), as yacc's grammar takes it.

quit is obeyed when it is read, even in if(0) quit or a function's body, as POSIX says: the
statements of the line before it are run, and what it stands in is dropped.

The one thing the library may say that a person cannot: names that start with _ and
_fail "text", which stops with that message. They are its own, out of a program's reach.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc.h"

enum {
    T_EOF = 256, T_NUM, T_STR, T_NAME,
    T_EQ, T_NE, T_LE, T_GE, T_AND, T_OR, T_INC, T_DEC,
    T_ADDEQ, T_SUBEQ, T_MULEQ, T_DIVEQ, T_MODEQ, T_POWEQ,
    T_AUTO, T_BREAK, T_CONTINUE, T_DEFINE, T_ELSE, T_FOR, T_IBASE, T_IF, T_LENGTH, T_OBASE,
    T_QUIT, T_RETURN, T_SCALE, T_SQRT, T_WHILE,
    T_FAIL
};

static const struct { const char *word; int token; } keywords[] = {
    { "auto", T_AUTO }, { "break", T_BREAK }, { "continue", T_CONTINUE }, { "define", T_DEFINE },
    { "else", T_ELSE }, { "for", T_FOR }, { "ibase", T_IBASE }, { "if", T_IF },
    { "length", T_LENGTH }, { "obase", T_OBASE }, { "quit", T_QUIT }, { "return", T_RETURN },
    { "scale", T_SCALE }, { "sqrt", T_SQRT }, { "while", T_WHILE },
};

/* what a parsing function that gives -1 has found */
enum { P_SYNTAX = 1, P_NOMEM, P_QUIT };

/* what an expression turned out to be: a statement prints what is not an assignment */
enum { E_VALUE, E_ASSIGN, E_ARRAY };

struct loop {
    struct loop *up;
    int breaks;                         /* the jumps to the end: a chain through their operands */
    int next;                           /* where continue goes */
};

struct parser {
    struct bc *b;
    int (*get)(void *);
    void *in;
    int ahead;                          /* a character read and not used; -2, none */
    int newline;                        /* the last one read ended a line, which counts when the next is read */
    int tok;
    char *text;                         /* of a number, a string or a name */
    unsigned ntext, captext;
    struct func *fn;                    /* the code being written */
    struct loop *loop;
    int *args;                          /* the calls being read: a stack of OP_CALL's operands */
    unsigned nargs, capargs;
    unsigned depth;
    int why;                            /* P_... */
    int lib;
    int in_function;
    int interactive;
    unsigned done;                      /* of the item's code, what is whole statements */
    int array_sym;                      /* the a of an a[] just read */
};

/* the first complaint about an input is the one worth reading */
static int complain(struct parser *p, const char *what) {
    if(!p->why) {
        p->why = P_SYNTAX;
        bc_say(p->b, what);
    }
    return -1;
}

static int no_memory(struct parser *p) {
    if(!p->why) {
        p->why = P_NOMEM;
        bc_say(p->b, num_strerror(NUM_NOMEM));
    }
    return -1;
}

static int unexpected(struct parser *p) {
    static const char *const ops[] = { "==", "!=", "<=", ">=", "&&", "||", "++", "--",
                                       "+=", "-=", "*=", "/=", "%=", "^=" };
    char what[64];
    int t = p->tok;
    if(t == T_EOF) return complain(p, "unexpected end of input");
    if(t == '\n') return complain(p, "unexpected end of line");
    if(t == T_NUM) return complain(p, "unexpected number");
    if(t == T_STR) return complain(p, "unexpected string");
    if(t >= T_EQ && t <= T_POWEQ) snprintf(what, sizeof(what), "syntax error near '%s'", ops[t - T_EQ]);
    else if(t >= 256) snprintf(what, sizeof(what), "syntax error near '%.32s'", p->text);
    else snprintf(what, sizeof(what), "syntax error near '%c'", t);
    return complain(p, what);
}

/**** characters and tokens ****/

static int getch(struct parser *p) {
    int c = p->ahead;
    if(c == -2) c = p->get(p->in);
    p->ahead = -2;
    if(p->newline) {
        p->b->line++;
        p->newline = 0;
    }
    if(c == '\n') p->newline = 1;
    return c < 0 ? -1 : c;
}

static void ungetch(struct parser *p, int c) {
    p->ahead = c;
    if(c == '\n') p->newline = 0;
}

static int text_add(struct parser *p, int c) {
    if(p->ntext + 2 > p->captext) {
        unsigned cap = p->captext ? p->captext * 2 : 64;
        char *t = cap > p->captext ? realloc(p->text, cap) : NULL;
        if(!t) return no_memory(p);
        p->text = t;
        p->captext = cap;
    }
    p->text[p->ntext++] = (char)c;
    p->text[p->ntext] = 0;
    return 0;
}

static int is_digit(int c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

static int lex_number(struct parser *p, int c) {
    int dot = 0, digits = 0;
    for(;;) {
        if(c == '.') {
            if(dot) break;
            dot = 1;
        } else if(c == '\\') {          /* a number goes on across a backslash and a newline */
            if(getch(p) != '\n') return complain(p, "a backslash outside a string or a number");
            c = getch(p);
            continue;
        } else if(is_digit(c)) {
            digits = 1;
        } else {
            break;
        }
        if(text_add(p, c)) return -1;
        c = getch(p);
    }
    if(c >= 0) ungetch(p, c);
    if(!digits) return complain(p, "a point without a digit");
    p->tok = T_NUM;
    return 0;
}

static int lex_string(struct parser *p) {
    for(;;) {
        int c = getch(p);
        if(c < 0) return complain(p, "string without its closing \"");
        if(!c) return complain(p, "a NUL byte in a string");
        if(c == '"') break;
        if(text_add(p, c)) return -1;
    }
    p->tok = T_STR;
    return 0;
}

static int lex_name(struct parser *p, int c) {
    unsigned i;
    while((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
        if(text_add(p, c)) return -1;
        c = getch(p);
    }
    if(c >= 0) ungetch(p, c);
    p->tok = T_NAME;
    for(i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
        if(!strcmp(p->text, keywords[i].word)) p->tok = keywords[i].token;
    if(p->lib && !strcmp(p->text, "_fail")) p->tok = T_FAIL;
    return 0;
}

static int skip_comment(struct parser *p) {
    int c = 0, last;
    do {
        last = c;
        c = getch(p);
        if(c < 0) return complain(p, "comment without its closing */");
    } while(last != '*' || c != '/');
    return 0;
}

/* the next character is second: then the token is two, otherwise one */
static int two(struct parser *p, int second, int both, int one) {
    int c = getch(p);
    if(c == second) return both;
    if(c >= 0) ungetch(p, c);
    return one;
}

static int next(struct parser *p) {
    int c;
    p->ntext = 0;
    if(p->text) p->text[0] = 0;
    for(;;) {
        c = getch(p);
        if(c == ' ' || c == '\t' || c == '\r') continue;
        if(c == '\\') {                 /* a backslash and a newline are only a space */
            int d = getch(p);
            if(d == '\n') continue;
            if(d >= 0) ungetch(p, d);
            return complain(p, "a backslash outside a string or a number");
        }
        if(c == '#') {
            do c = getch(p); while(c >= 0 && c != '\n');
        }
        if(c == '/') {
            int d = getch(p);
            if(d == '*') {
                if(skip_comment(p)) return -1;
                continue;
            }
            if(d >= 0) ungetch(p, d);
        }
        break;
    }
    if(c < 0) {
        p->tok = T_EOF;
        return 0;
    }
    if(is_digit(c)) return lex_number(p, c);
    if(c == '.') {
        int d = getch(p);
        if(d >= 0) ungetch(p, d);
        if(is_digit(d) || d == '\\') return lex_number(p, c);
        p->tok = '.';
        return unexpected(p);
    }
    if(c == '"') return lex_string(p);
    if((c >= 'a' && c <= 'z') || (c == '_' && p->lib)) return lex_name(p, c);
    switch(c) {
    case '+':
        c = getch(p);
        p->tok = c == '+' ? T_INC : c == '=' ? T_ADDEQ : '+';
        if(p->tok == '+' && c >= 0) ungetch(p, c);
        return 0;
    case '-':
        c = getch(p);
        p->tok = c == '-' ? T_DEC : c == '=' ? T_SUBEQ : '-';
        if(p->tok == '-' && c >= 0) ungetch(p, c);
        return 0;
    case '*': p->tok = two(p, '=', T_MULEQ, '*'); return 0;
    case '/': p->tok = two(p, '=', T_DIVEQ, '/'); return 0;
    case '%': p->tok = two(p, '=', T_MODEQ, '%'); return 0;
    case '^': p->tok = two(p, '=', T_POWEQ, '^'); return 0;
    case '=': p->tok = two(p, '=', T_EQ, '='); return 0;
    case '<': p->tok = two(p, '=', T_LE, '<'); return 0;
    case '>': p->tok = two(p, '=', T_GE, '>'); return 0;
    case '!': p->tok = two(p, '=', T_NE, '!'); return 0;
    case '&':
        if(two(p, '&', T_AND, '&') == T_AND) {
            p->tok = T_AND;
            return 0;
        }
        break;
    case '|':
        if(two(p, '|', T_OR, '|') == T_OR) {
            p->tok = T_OR;
            return 0;
        }
        break;
    case '\n': case '(': case ')': case '[': case ']': case '{': case '}': case ',': case ';':
        p->tok = c;
        return 0;
    }
    {
        char what[48];
        if(c >= 32 && c < 127) snprintf(what, sizeof(what), "unexpected character '%c'", c);
        else snprintf(what, sizeof(what), "unexpected character 0x%02x", (unsigned)c);
        return complain(p, what);
    }
}

/**** writing code ****/

static int emit(struct parser *p, int word) {
    struct func *f = p->fn;
    if(f->ncode == f->capcode) {
        unsigned cap = f->capcode ? f->capcode * 2 : 32;
        int *c = cap > f->capcode && cap < 0x20000000u ? realloc(f->code, cap * sizeof(int)) : NULL;
        if(!c) return no_memory(p);
        f->code = c;
        f->capcode = cap;
    }
    f->code[f->ncode++] = word;
    return 0;
}

static int emit2(struct parser *p, int op, int operand) {
    return emit(p, op) || emit(p, operand);
}

/* a jump whose target comes later: where to write it */
static int emit_jump(struct parser *p, int op, int *hole) {
    *hole = (int)p->fn->ncode + 1;
    return emit2(p, op, 0);
}

static int here(struct parser *p) {
    return (int)p->fn->ncode;
}

static void patch(struct parser *p, int hole) {
    p->fn->code[hole] = here(p);
}

/* A break is a jump whose target is not yet known: its operand links it to the one before. */
static int emit_break(struct parser *p, int op) {
    int hole;
    if(emit_jump(p, op, &hole)) return -1;
    p->fn->code[hole] = p->loop->breaks;
    p->loop->breaks = hole;
    return 0;
}

static void patch_breaks(struct parser *p, struct loop *l) {
    int at = l->breaks;
    while(at >= 0) {
        int up = p->fn->code[at];
        p->fn->code[at] = here(p);
        at = up;
    }
}

static int add_text(struct parser *p) {
    struct func *f = p->fn;
    char *t;
    if(f->ntext == f->captext) {
        unsigned cap = f->captext ? f->captext * 2 : 8;
        char **v = cap < 0x10000000u ? realloc(f->text, cap * sizeof(char *)) : NULL;
        if(!v) {
            no_memory(p);
            return -1;
        }
        f->text = v;
        f->captext = cap;
    }
    t = malloc(p->ntext + 1);
    if(!t) {
        no_memory(p);
        return -1;
    }
    if(p->ntext) memcpy(t, p->text, p->ntext);
    t[p->ntext] = 0;
    f->text[f->ntext] = t;
    return (int)f->ntext++;
}

static int add_constant(struct parser *p) {
    struct func *f = p->fn;
    struct konst *k;
    if(f->nk == f->capk) {
        unsigned cap = f->capk ? f->capk * 2 : 8;
        struct konst *v = cap < 0x08000000u ? realloc(f->k, cap * sizeof(*v)) : NULL;
        if(!v) {
            no_memory(p);
            return -1;
        }
        f->k = v;
        f->capk = cap;
    }
    k = &f->k[f->nk];
    if(!(k->text = malloc(p->ntext + 1))) {
        no_memory(p);
        return -1;
    }
    memcpy(k->text, p->text, p->ntext + 1);
    k->len = p->ntext;
    k->base = 0;
    k->value = NULL;
    return (int)f->nk++;
}

/**** expressions ****/

static int binary(struct parser *p, int min, int *kind);

static int expect(struct parser *p, int tok) {
    if(p->tok != tok) return unexpected(p);
    return next(p);
}

static int deeper(struct parser *p) {
    if(++p->depth > BC_DEPTH_MAX) return complain(p, "nested too deeply");
    return 0;
}

/* the expression that is not an array's name */
static int expression(struct parser *p, int min, int *kind) {
    int k = E_VALUE;
    if(binary(p, min, &k)) return -1;
    if(k == E_ARRAY) return complain(p, "an array's name where a value should be");
    if(kind) *kind = k;
    return 0;
}

enum { PREC_OR = 1, PREC_AND, PREC_REL, PREC_ADD, PREC_MUL, PREC_POW };

static int expr(struct parser *p) {
    return expression(p, PREC_OR, NULL);
}

/* a place to read or write: a variable, a register, or an array element whose index is on the stack */
struct place {
    int load, store, operand;
};

static int place_load(struct parser *p, const struct place *w) {
    return emit2(p, w->load, w->operand);
}

static int place_store(struct parser *p, const struct place *w) {
    return emit2(p, w->store, w->operand);
}

static int compound_op(int tok) {
    switch(tok) {
    case T_ADDEQ: return OP_ADD;
    case T_SUBEQ: return OP_SUB;
    case T_MULEQ: return OP_MUL;
    case T_DIVEQ: return OP_DIV;
    case T_MODEQ: return OP_MOD;
    case T_POWEQ: return OP_POW;
    }
    return -1;
}

/* A named expression has been read, and what stands after it says what it is for. */
static int after_name(struct parser *p, const struct place *w, int *kind) {
    int op = compound_op(p->tok), indexed = w->load == OP_LOADA;
    if(p->tok == '=') {
        if(next(p) || expression(p, PREC_ADD, NULL)) return -1;
        *kind = E_ASSIGN;
        return place_store(p, w);
    }
    if(op >= 0) {
        if(next(p)) return -1;
        if(indexed && emit(p, OP_DUP)) return -1;
        if(place_load(p, w) || expression(p, PREC_ADD, NULL) || emit(p, op)) return -1;
        *kind = E_ASSIGN;
        return place_store(p, w);
    }
    if(p->tok == T_INC || p->tok == T_DEC) {    /* the value from before, and the new one stored */
        op = p->tok == T_INC ? OP_ADD : OP_SUB;
        if(next(p)) return -1;
        if(indexed && emit(p, OP_DUP)) return -1;
        if(place_load(p, w) || emit(p, indexed ? OP_TUCK : OP_DUP) || emit(p, OP_ONE) || emit(p, op) ||
           place_store(p, w) || emit(p, OP_POP))
            return -1;
        *kind = E_VALUE;
        return 0;
    }
    *kind = E_VALUE;
    return place_load(p, w);
}

/* What follows a name that is no call: a variable, an element (its index gets written), or
   with empty brackets an array's name alone, which only a call takes. */
static int named(struct parser *p, int sym, struct place *w, int *kind) {
    w->operand = sym;
    w->load = OP_LOADV;
    w->store = OP_STOREV;
    if(p->tok != '[') return 0;
    if(next(p)) return -1;
    if(p->tok == ']') {
        p->array_sym = sym;
        *kind = E_ARRAY;
        return next(p);
    }
    if(expr(p)) return -1;
    w->load = OP_LOADA;
    w->store = OP_STOREA;
    return expect(p, ']');
}

static void register_place(struct place *w, int tok) {
    w->load = OP_LOADR;
    w->store = OP_STORER;
    w->operand = tok == T_SCALE ? REG_SCALE : tok == T_IBASE ? REG_IBASE : REG_OBASE;
}

static int call(struct parser *p, int sym) {
    unsigned base = p->nargs, n = 0, i;
    if(next(p)) return -1;
    while(p->tok != ')') {
        int k;
        if(n && expect(p, ',')) return -1;
        if(binary(p, PREC_OR, &k)) return -1;
        if(p->nargs == p->capargs) {
            unsigned cap = p->capargs ? p->capargs * 2 : 16;
            int *v = cap < 0x10000000u ? realloc(p->args, cap * sizeof(int)) : NULL;
            if(!v) return no_memory(p);
            p->args = v;
            p->capargs = cap;
        }
        p->args[p->nargs++] = k == E_ARRAY ? p->array_sym + 1 : 0;
        n++;
        if(p->tok != ')' && p->tok != ',') return unexpected(p);
    }
    if(next(p)) return -1;
    if(emit2(p, OP_CALL, sym) || emit(p, (int)n)) return -1;
    for(i = 0; i < n; i++) if(emit(p, p->args[base + i])) return -1;
    p->nargs = base;
    return 0;
}

/* length(e), sqrt(e), scale(e): one argument */
static int builtin(struct parser *p, int op) {
    if(expect(p, '(') || expr(p) || expect(p, ')')) return -1;
    return emit(p, op);
}

/* ++ and -- in front of a named expression */
static int pre_step(struct parser *p, int op) {
    struct place w;
    int sym, kind = E_VALUE;
    if(next(p)) return -1;
    switch(p->tok) {
    case T_NAME:
        if((sym = bc_intern(p->b, p->text, p->ntext)) < 0) return no_memory(p);
        if(next(p) || named(p, sym, &w, &kind)) return -1;
        if(kind == E_ARRAY) return complain(p, "an array's name where a value should be");
        break;
    case T_SCALE: case T_IBASE: case T_OBASE:
        register_place(&w, p->tok);
        if(next(p)) return -1;
        break;
    default:
        return unexpected(p);
    }
    if(w.load == OP_LOADA && emit(p, OP_DUP)) return -1;
    return place_load(p, &w) || emit(p, OP_ONE) || emit(p, op) || place_store(p, &w) ? -1 : 0;
}

static int primary(struct parser *p, int *kind) {
    struct place w;
    int k, tok = p->tok;
    *kind = E_VALUE;
    switch(p->tok) {
    case T_NUM:
        k = add_constant(p);
        if(k < 0) return -1;
        return emit2(p, OP_CONST, k) || next(p) ? -1 : 0;
    case '(':
        if(next(p) || expr(p)) return -1;
        return expect(p, ')');
    case T_INC:
        return pre_step(p, OP_ADD);
    case T_DEC:
        return pre_step(p, OP_SUB);
    case T_LENGTH:
        if(next(p)) return -1;
        return builtin(p, OP_LENGTH);
    case T_SQRT:
        if(next(p)) return -1;
        return builtin(p, OP_SQRT);
    case T_SCALE:
        if(next(p)) return -1;
        if(p->tok == '(') return builtin(p, OP_SCALEOF);
        register_place(&w, tok);
        return after_name(p, &w, kind);
    case T_IBASE: case T_OBASE:
        register_place(&w, tok);
        if(next(p)) return -1;
        return after_name(p, &w, kind);
    case '!':                           /* lower than a relation, as in GNU's bc */
        if(next(p) || expression(p, PREC_REL, NULL)) return -1;
        return emit(p, OP_NOT);
    case T_NAME:
        if((k = bc_intern(p->b, p->text, p->ntext)) < 0) return no_memory(p);
        if(next(p)) return -1;
        if(p->tok == '(') return call(p, k);
        if(named(p, k, &w, kind)) return -1;
        return *kind == E_ARRAY ? 0 : after_name(p, &w, kind);
    }
    return unexpected(p);
}

/* minus and what it applies to: tighter than ^, as POSIX's table has it */
static int unary(struct parser *p, int *kind) {
    if(deeper(p)) return -1;
    if(p->tok == '-') {
        if(next(p) || unary(p, kind)) return -1;
        if(*kind == E_ARRAY) return complain(p, "an array's name where a value should be");
        *kind = E_VALUE;
        if(emit(p, OP_NEG)) return -1;
    } else if(primary(p, kind)) {
        return -1;
    }
    p->depth--;
    return 0;
}

/* the jumps of && and ||, which leave 1 or 0 and only run the right side when it counts */
static int logic(struct parser *p, int op, int prec) {
    int first, second, skip;
    int jump = op == T_AND ? OP_JZ : OP_JNZ;
    if(emit_jump(p, jump, &first)) return -1;
    if(next(p) || expression(p, prec + 1, NULL)) return -1;
    if(emit_jump(p, jump, &second)) return -1;
    if(emit(p, op == T_AND ? OP_ONE : OP_ZERO)) return -1;
    if(emit_jump(p, OP_JMP, &skip)) return -1;
    patch(p, first);
    patch(p, second);
    if(emit(p, op == T_AND ? OP_ZERO : OP_ONE)) return -1;
    patch(p, skip);
    return 0;
}

static int binary_op(int tok, int *prec) {
    switch(tok) {
    case '+': *prec = PREC_ADD; return OP_ADD;
    case '-': *prec = PREC_ADD; return OP_SUB;
    case '*': *prec = PREC_MUL; return OP_MUL;
    case '/': *prec = PREC_MUL; return OP_DIV;
    case '%': *prec = PREC_MUL; return OP_MOD;
    case '^': *prec = PREC_POW; return OP_POW;
    case T_EQ: *prec = PREC_REL; return OP_EQ;
    case T_NE: *prec = PREC_REL; return OP_NE;
    case '<': *prec = PREC_REL; return OP_LT;
    case T_LE: *prec = PREC_REL; return OP_LE;
    case '>': *prec = PREC_REL; return OP_GT;
    case T_GE: *prec = PREC_REL; return OP_GE;
    case T_AND: *prec = PREC_AND; return -1;
    case T_OR: *prec = PREC_OR; return -1;
    }
    *prec = 0;
    return -1;
}

/* the operators after a first operand, down to precedence min */
static int binary_rest(struct parser *p, int min, int *kind) {
    for(;;) {
        int prec, op = binary_op(p->tok, &prec), tok = p->tok;
        if(!prec || prec < min) return 0;
        *kind = E_VALUE;
        if(tok == T_AND || tok == T_OR) {
            if(logic(p, tok, prec)) return -1;
            continue;
        }
        if(deeper(p) || next(p)) return -1;
        if(expression(p, op == OP_POW ? prec : prec + 1, NULL)) return -1;  /* ^ goes right to left */
        p->depth--;
        if(emit(p, op)) return -1;
    }
}

static int binary(struct parser *p, int min, int *kind) {
    if(unary(p, kind)) return -1;
    if(*kind == E_ARRAY) return 0;      /* only call() takes it, and what follows must be , or ) */
    return binary_rest(p, min, kind);
}

/**** statements ****/

static int statement(struct parser *p);

static int skip_newlines(struct parser *p) {
    while(p->tok == '\n') if(next(p)) return -1;
    return 0;
}

/* the statements up to the } */
static int block(struct parser *p) {
    for(;;) {
        while(p->tok == '\n' || p->tok == ';') if(next(p)) return -1;
        if(p->tok == '}') return 0;
        if(statement(p)) return -1;
        if(p->tok != '\n' && p->tok != ';' && p->tok != '}') return unexpected(p);
    }
}

static int st_if(struct parser *p) {
    int to_else, to_end;
    if(next(p) || expect(p, '(') || expr(p) || expect(p, ')') || skip_newlines(p)) return -1;
    if(emit_jump(p, OP_JZ, &to_else) || statement(p)) return -1;
    if(p->tok != T_ELSE) {
        patch(p, to_else);
        return 0;
    }
    if(emit_jump(p, OP_JMP, &to_end)) return -1;
    patch(p, to_else);
    if(next(p) || skip_newlines(p) || statement(p)) return -1;
    patch(p, to_end);
    return 0;
}

static int st_while(struct parser *p) {
    struct loop l;
    l.up = p->loop;
    l.breaks = -1;
    l.next = here(p);
    p->loop = &l;
    if(next(p) || expect(p, '(') || expr(p) || expect(p, ')') || skip_newlines(p)) return -1;
    if(emit_break(p, OP_JZ) || statement(p) || emit2(p, OP_JMP, l.next)) return -1;
    patch_breaks(p, &l);
    p->loop = l.up;
    return 0;
}

/* for(a; b; c) body: the code keeps c ahead of the body, as this pass reads it first */
static int st_for(struct parser *p) {
    struct loop l;
    int test, to_body;
    l.up = p->loop;
    l.breaks = -1;
    p->loop = &l;
    if(next(p) || expect(p, '(')) return -1;
    if(p->tok != ';' && (expr(p) || emit(p, OP_POP))) return -1;
    if(expect(p, ';')) return -1;
    test = here(p);
    if(p->tok != ';' && (expr(p) || emit_break(p, OP_JZ))) return -1;
    if(expect(p, ';') || emit_jump(p, OP_JMP, &to_body)) return -1;
    l.next = here(p);
    if(p->tok != ')' && (expr(p) || emit(p, OP_POP))) return -1;
    if(emit2(p, OP_JMP, test) || expect(p, ')') || skip_newlines(p)) return -1;
    patch(p, to_body);
    if(statement(p) || emit2(p, OP_JMP, l.next)) return -1;
    patch_breaks(p, &l);
    p->loop = l.up;
    return 0;
}

/* return, return(), return(e), and as other bcs take it, return e: the bracket may begin a longer expression */
static int st_return(struct parser *p) {
    int kind = E_VALUE;
    if(!p->in_function) return complain(p, "return outside a function");
    if(next(p)) return -1;
    if(p->tok == '\n' || p->tok == ';' || p->tok == '}' || p->tok == T_EOF || p->tok == T_ELSE) return emit(p, OP_RET0);
    if(p->tok != '(') return expr(p) || emit(p, OP_RET) ? -1 : 0;
    if(next(p)) return -1;
    if(p->tok == ')') return next(p) || emit(p, OP_RET0) ? -1 : 0;
    if(expr(p) || expect(p, ')') || binary_rest(p, PREC_OR, &kind)) return -1;
    return emit(p, OP_RET);
}

static int statement(struct parser *p) {
    int kind, t, rc;
    if(deeper(p)) return -1;
    switch(p->tok) {
    case T_STR:
        t = add_text(p);
        rc = t < 0 || emit2(p, OP_STR, t) || next(p) ? -1 : 0;
        break;
    case T_BREAK:
        if(!p->loop) return complain(p, "break outside a loop");
        rc = emit_break(p, OP_JMP) || next(p) ? -1 : 0;
        break;
    case T_CONTINUE:
        if(!p->loop) return complain(p, "continue outside a loop");
        rc = emit2(p, OP_JMP, p->loop->next) || next(p) ? -1 : 0;
        break;
    case T_QUIT:
        p->why = P_QUIT;
        return -1;
    case T_RETURN:
        rc = st_return(p);
        break;
    case T_IF:
        rc = st_if(p);
        break;
    case T_WHILE:
        rc = st_while(p);
        break;
    case T_FOR:
        rc = st_for(p);
        break;
    case '{':
        rc = next(p) || block(p) || next(p) ? -1 : 0;
        break;
    case T_FAIL:
        if(next(p)) return -1;
        if(p->tok != T_STR) return unexpected(p);
        t = add_text(p);
        rc = t < 0 || emit2(p, OP_FAIL, t) || next(p) ? -1 : 0;
        break;
    default:
        rc = expression(p, PREC_OR, &kind) || emit(p, kind == E_ASSIGN ? OP_POP : OP_PRINT) ? -1 : 0;
        break;
    }
    p->depth--;
    return rc;
}

/**** functions ****/

/* the names of a define or of an auto, each with [ ] if it is an array */
static int local_list(struct parser *p, struct func *f) {
    for(;;) {
        int sym, array = 0;
        unsigned i;
        if(p->tok != T_NAME) return unexpected(p);
        if((sym = bc_intern(p->b, p->text, p->ntext)) < 0) return no_memory(p);
        if(next(p)) return -1;
        if(p->tok == '[') {
            array = 1;
            if(next(p) || expect(p, ']')) return -1;
        }
        for(i = 0; i < f->nloc; i++)
            if(f->loc[i].sym == (unsigned)sym && f->loc[i].array == array) return complain(p, "a name twice in one function");
        if(f->nloc == f->caploc) {
            unsigned cap = f->caploc ? f->caploc * 2 : 8;
            struct local *v = cap < 0x08000000u ? realloc(f->loc, cap * sizeof(*v)) : NULL;
            if(!v) return no_memory(p);
            f->loc = v;
            f->caploc = cap;
        }
        f->loc[f->nloc].sym = (unsigned)sym;
        f->loc[f->nloc++].array = array;
        if(p->tok != ',') return 0;
        if(next(p)) return -1;
    }
}

static int define(struct parser *p) {
    struct func *f = calloc(1, sizeof(*f));
    struct func *outer = p->fn;
    int sym;
    if(!f) return no_memory(p);
    if(next(p)) goto fail;
    if(p->tok != T_NAME) {
        unexpected(p);
        goto fail;
    }
    if((sym = bc_intern(p->b, p->text, p->ntext)) < 0) {
        no_memory(p);
        goto fail;
    }
    f->sym = (unsigned)sym;
    f->lib = p->lib;
    if(next(p) || expect(p, '(')) goto fail;
    if(p->tok != ')' && local_list(p, f)) goto fail;
    f->nparams = f->nloc;
    if(expect(p, ')') || skip_newlines(p) || expect(p, '{')) goto fail;
    while(p->tok == '\n' || p->tok == ';') if(next(p)) goto fail;
    while(p->tok == T_AUTO) {
        if(next(p) || local_list(p, f)) goto fail;
        if(p->tok != ';' && p->tok != '\n') {
            unexpected(p);
            goto fail;
        }
        if(next(p)) goto fail;
        while(p->tok == '\n' || p->tok == ';') if(next(p)) goto fail;
    }
    p->fn = f;
    p->in_function = 1;
    if(block(p) || emit(p, OP_RET0)) goto fail;
    p->fn = outer;
    p->in_function = 0;
    if(p->b->sym[sym].fn) bc_func_free(p->b->sym[sym].fn);
    p->b->sym[sym].fn = f;
    return next(p);
fail:
    p->fn = outer;
    p->in_function = 0;
    bc_func_free(f);
    return -1;
}

/**** items ****/

/* the rest of the line goes with a mistake in it, so that the next line starts clean */
static void skip_line(struct parser *p) {
    int c;
    if(p->tok == '\n' || p->tok == T_EOF) return;
    do c = getch(p); while(c >= 0 && c != '\n');
    p->tok = c < 0 ? T_EOF : '\n';
}

/* one item into b->main: 0, or -1 with p->why; at the end of the input p->tok is T_EOF */
static int read_item(struct parser *p) {
    struct func *f = &p->b->main;
    bc_func_clear(f);
    p->fn = f;
    p->done = 0;
    p->loop = NULL;
    p->depth = 0;
    p->nargs = 0;
    p->why = 0;
    if(next(p)) return -1;
    for(;;) {
        if(p->tok == T_EOF) return 0;
        if(p->tok == '\n') return 0;
        if(p->tok == ';') {
            if(next(p)) return -1;
            continue;
        }
        if(p->tok == T_DEFINE) {
            if(f->ncode) return unexpected(p);
            if(define(p)) return -1;
            continue;
        }
        if(statement(p)) return -1;
        p->done = f->ncode;
        if(p->tok != ';' && p->tok != '\n' && p->tok != T_EOF) return unexpected(p);
    }
}

/* what the item holds is run: its code ends here, at the last whole statement when quit cut it off */
static int run_item(struct parser *p) {
    struct func *f = &p->b->main;
    int rc;
    if(!p->done) return 0;
    f->ncode = p->done;
    p->fn = f;
    p->why = 0;
    if(emit(p, OP_END)) return -1;
    rc = bc_exec(p->b, f);
    if(p->interactive) p->b->host.flush(p->b->host.ctx);        /* POSIX: without delay; a script needs no write a line */
    return rc;
}

static int run_parser(struct parser *p) {
    int interactive = p->interactive;
    for(;;) {
        if(!read_item(p)) {
            if(run_item(p) < 0 && !interactive) return BC_TROUBLE;
            if(p->tok == T_EOF) return BC_END;
            continue;
        }
        if(p->why == P_QUIT) return run_item(p) < 0 && !interactive ? BC_TROUBLE : BC_QUIT;
        if(!interactive) return BC_TROUBLE;
        skip_line(p);
        if(p->tok == T_EOF) return BC_END;
    }
}

static int run_text(struct bc *b, int (*get)(void *), void *in, const char *name, int interactive, int lib) {
    struct parser p;
    int rc;
    memset(&p, 0, sizeof(p));
    p.b = b;
    p.get = get;
    p.in = in;
    p.ahead = -2;
    p.lib = lib;
    p.interactive = interactive;
    b->name = name;
    b->line = 1;
    rc = run_parser(&p);
    bc_func_clear(&b->main);
    free(p.text);
    free(p.args);
    return rc;
}

int bc_run(struct bc *b, int (*get)(void *), void *in, const char *name, int interactive) {
    return run_text(b, get, in, name, interactive, 0);
}

static int get_text(void *in) {
    const char **s = in;
    return **s ? (unsigned char)*(*s)++ : -1;
}

int bc_library(struct bc *b) {
    const char *s = bc_library_text;
    b->scale = 20;
    return run_text(b, get_text, &s, "library", 0, 1);
}
